// main/esp_ai.c —— 语音助手客户端。
//
// 1. 按住:麦克风 → ADPCM 块 → 流缓冲 → HTTP 分块上传到 POST /api/assistant/talk
//    (边说边传,松手时服务器已经收到大部分音频)。
// 2. 松手:发结束块,等待服务器跑完 识别 → 大模型 → 语音合成,返回
//    {"question","answer","audio"}。
// 3. 朗读:GET /api/assistant/audio/<id>,服务器按同样的 ADPCM 块格式流式返回,边收边播。
//
// 没有中转时走云端直连(见下半部分);用哪条路由设置里的"使用方式"决定(voice_cfg.h)。
#include "audio_dsp.h"
#include "voice_cfg.h"
#include "esp_internal.h"
#include "svc.h"
#include "sys.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "settings.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ai";

#define STREAM_BYTES 8192
#define MAX_TALK_MS 30000

static volatile ai_state_t s_state = AI_IDLE;
static volatile bool s_end;
static volatile bool s_cancel;
static volatile bool s_replay;
static StreamBufferHandle_t s_stream;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_lock;
static char s_error[64];
static bool s_last_direct; // 上一条回答来自云端直连(重播时用)
static char s_q[160], s_a[640];
static bool s_result_ready;
static char s_audio_id[40];

ai_state_t ai_state(void) { return s_state; }
const char *ai_error(void) { return s_error; }

static void fail(const char *msg)
{
    utf8_copy(s_error, sizeof(s_error), msg);
    s_state = AI_ERROR;
    ESP_LOGW(TAG, "%s", msg);
}

static void fail_stage(const char *stage, int status)
{
    char msg[64];
    if (status == VOICE_ERR_MEM) {
        // 内存不足时带上加密连接前看到的数字,方便判断是哪里占了内存。
        unsigned free_k, largest_k;
        bool ui_tight;
        local_tls_last(&free_k, &largest_k, &ui_tight);
        if (ui_tight) snprintf(msg, sizeof(msg), "%s:界面内存紧张", stage);
        else snprintf(msg, sizeof(msg), "%s:内存不足 空闲%uK 最大%uK", stage, free_k, largest_k);
    } else {
        voice_error_text(msg, sizeof(msg), stage, status);
    }
    fail(msg);
}

// 只在麦克风停下后调用(audio_capture_stop 会等回调走完)。
static void drop_stream(void)
{
    StreamBufferHandle_t st = s_stream;
    s_stream = NULL;
    if (st) vStreamBufferDelete(st);
}

static void sink(const uint8_t *block, size_t len, void *user)
{
    (void)user;
    if (s_stream) xStreamBufferSend(s_stream, block, len, 0);   // 满了就丢,不阻塞音频任务
}

static esp_http_client_handle_t open_client(const char *path, esp_http_client_method_t method, int timeout)
{
    char url[160];
    snprintf(url, sizeof(url), "%s%s", relay_url(), path);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = timeout,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NULL;
    char auth[64];
    snprintf(auth, sizeof(auth), "Bearer %s", relay_token());
    esp_http_client_set_header(c, "Authorization", auth);
    esp_http_client_set_header(c, "X-Device", sys_short_id());
    return c;
}

static bool write_chunk(esp_http_client_handle_t c, const uint8_t *data, size_t len)
{
    char head[12];
    const int hl = snprintf(head, sizeof(head), "%x\r\n", (unsigned)len);
    return esp_http_client_write(c, head, hl) == hl
        && (len == 0 || esp_http_client_write(c, (const char *)data, (int)len) == (int)len)
        && esp_http_client_write(c, "\r\n", 2) == 2;
}

static void parse_reply(const char *body)
{
    cJSON *j = cJSON_Parse(body);
    const cJSON *q = cJSON_GetObjectItemCaseSensitive(j, "question");
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(j, "answer");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(j, "audio");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    utf8_copy(s_q, sizeof(s_q), cJSON_IsString(q) ? q->valuestring : "(没听清)");
    utf8_copy(s_a, sizeof(s_a), cJSON_IsString(a) ? a->valuestring : "");
    snprintf(s_audio_id, sizeof(s_audio_id), "%s", cJSON_IsString(id) ? id->valuestring : "");
    s_result_ready = true;
    xSemaphoreGive(s_lock);
    cJSON_Delete(j);
}

static void speak(void)
{
    if (!s_audio_id[0] || !audio_available()) {
        s_state = AI_IDLE;
        return;
    }
    s_state = AI_SPEAKING;
    char path[80];
    snprintf(path, sizeof(path), "/api/assistant/audio/%s", s_audio_id);
    esp_http_client_handle_t c = open_client(path, HTTP_METHOD_GET, 15000);
    if (!c || esp_http_client_open(c, 0) != ESP_OK) {
        if (c) esp_http_client_cleanup(c);
        fail_stage("朗读", VOICE_ERR_NET);
        return;
    }
    esp_http_client_fetch_headers(c);
    uint8_t block[ADPCM_BLOCK_BYTES];
    int fill = 0;
    while (!s_cancel) {
        const int r = esp_http_client_read(c, (char *)block + fill, ADPCM_BLOCK_BYTES - fill);
        if (r <= 0) break;
        fill += r;
        if (fill == ADPCM_BLOCK_BYTES) {
            audio_play_push(block, ADPCM_BLOCK_BYTES);
            fill = 0;
        }
    }
    if (fill > 4 && !s_cancel) audio_play_push(block, (size_t)fill);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    while (!s_cancel && audio_playing()) vTaskDelay(pdMS_TO_TICKS(50));
    if (s_cancel) audio_play_flush();
    s_state = AI_IDLE;
}

static void talk(void)
{
    esp_http_client_handle_t c = open_client("/api/assistant/talk", HTTP_METHOD_POST, 60000);
    if (!c) {
        audio_capture_stop();
        fail("内存不足");
        return;
    }
    esp_http_client_set_header(c, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(c, "X-Audio-Format", "ima-adpcm;rate=16000;block=256");
    if (esp_http_client_open(c, -1) != ESP_OK) {
        audio_capture_stop();
        esp_http_client_cleanup(c);
        fail_stage("中转", VOICE_ERR_NET);
        return;
    }
    const int64_t started = sys_now_ms();
    uint8_t buf[ADPCM_BLOCK_BYTES * 2];
    bool ok = true;
    for (;;) {
        const size_t n = xStreamBufferReceive(s_stream, buf, sizeof(buf), pdMS_TO_TICKS(40));
        if (n && !write_chunk(c, buf, n)) {
            ok = false;
            break;
        }
        if (s_cancel) break;
        if (sys_now_ms() - started > MAX_TALK_MS) s_end = true;   // 最长 30 秒
        if (s_end && !n && xStreamBufferIsEmpty(s_stream)) break;
    }
    audio_capture_stop();
    if (s_cancel || !ok) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        if (s_cancel) s_state = AI_IDLE;
        else fail_stage("中转", VOICE_ERR_UPLOAD);
        return;
    }
    s_state = AI_THINKING;
    write_chunk(c, NULL, 0);   // 结束块
    esp_http_client_fetch_headers(c);
    const int status = esp_http_client_get_status_code(c);
    static char resp[1536];
    int got = 0, r;
    while (got < (int)sizeof(resp) - 1 &&
           (r = esp_http_client_read(c, resp + got, (int)sizeof(resp) - 1 - got)) > 0) {
        got += r;
    }
    resp[got] = '\0';
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (s_cancel) {
        s_state = AI_IDLE;
        return;
    }
    if (status != 200) {
        // 中转会在 {"error": "..."} 里说明是哪一步出错(识别 / 对话 / 朗读)
        cJSON *j = cJSON_Parse(resp);
        const cJSON *e = cJSON_GetObjectItemCaseSensitive(j, "error");
        if (cJSON_IsString(e) && e->valuestring[0]) fail(e->valuestring);
        else if (status == 401) fail("中转未授权本设备");
        else if (status == 422) fail("没听清,再说一次");
        else fail_stage("中转", status > 0 ? status : VOICE_ERR_NET);
        cJSON_Delete(j);
        return;
    }
    s_last_direct = false;
    parse_reply(resp);
    speak();
    // 中转的朗读失败时文字照常显示,另外提示是哪一步出的问题
    cJSON *j = cJSON_Parse(resp);
    const cJSON *te = cJSON_GetObjectItemCaseSensitive(j, "tts_error");
    if (cJSON_IsString(te) && te->valuestring[0] && !s_cancel) fail(te->valuestring);
    cJSON_Delete(j);
}

// ===========================================================================
// 直连模式:没有中转服务器时,设备自己调用兼容 OpenAI 接口的云服务
//   识别  POST {语音接口}/audio/transcriptions   multipart(WAV 边录边传,分块上传)
//   对话  POST {对话接口}/chat/completions
//   合成  POST {语音接口}/audio/speech            返回 16 / 24 kHz PCM,边收边播
// 对话和语音可以是两家(例:DeepSeek 对话 + 硅基流动语音),见 voice_resolve()。
// ===========================================================================
#define BOUNDARY "----passport7c3boundary"
#define DIRECT_SYSTEM "你是装在随身小设备里的中文语音助手,回答会被朗读并显示在小屏幕上。" \
    "用简体中文口语回答,先给结论,通常一到三句话、不超过 120 字,不要用 Markdown、列表或表情。"
#define CHAT_RESP_MAX 6144

static char s_hist_q[2][160], s_hist_a[2][320];
static int s_hist_n;

static void voice_endpoint(const char **base, const char **key)
{
    voice_resolve(g_settings.api_base, g_settings.api_key, g_settings.voice_base, g_settings.voice_key,
                  base, key);
}

// 失败时 *err 给出原因(VOICE_ERR_MEM / VOICE_ERR_NET)。
static esp_http_client_handle_t open_direct(const char *base, const char *key, const char *path,
                                            int timeout, int *err)
{
    char url[160];
    snprintf(url, sizeof(url), "%s%s", base, path);
    if (!voice_key_transport_ok(url, key)) {
        *err = VOICE_ERR_INSECURE;
        return NULL;
    }
    *err = VOICE_ERR_MEM;
    if (strncmp(url, "https", 5) == 0 && !local_tls_ok()) return NULL;
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = timeout,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = strncmp(url, "https", 5) == 0 ? esp_crt_bundle_attach : NULL,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NULL;
    char auth[sizeof(g_settings.api_key) + 8];
    snprintf(auth, sizeof(auth), "Bearer %s", key);
    esp_http_client_set_header(c, "Authorization", auth);
    *err = VOICE_ERR_NET;
    return c;
}

static int read_body(esp_http_client_handle_t c, char *out, int cap)
{
    int got = 0, r;
    while (got < cap - 1 && (r = esp_http_client_read(c, out + got, cap - 1 - got)) > 0) got += r;
    out[got] = '\0';
    return got;
}

static void wav_header(uint8_t h[44])
{
    // 长度未知(边录边传):RIFF/data 长度写成最大值,解码器会读到流结束。
    static const uint8_t t[44] = {'R','I','F','F',0xFF,0xFF,0xFF,0x7F,'W','A','V','E','f','m','t',' ',
        16,0,0,0,1,0,1,0,0x80,0x3E,0,0,0,0x7D,0,0,2,0,16,0,'d','a','t','a',0xFF,0xFF,0xFF,0x7F};
    memcpy(h, t, 44);
}

#define ASR_CANCELLED 0
#define ASR_EARLY 1   // 连接好之前就松开了

// 识别。先建好 HTTPS 连接、写好表单头,再开麦:C3 上握手要一两秒,
// 而录音缓冲只有约 1 秒,边握手边录会把开头的话丢掉。返回 200 或错误码。
static int direct_asr(char *text, size_t len)
{
    const char *base, *key;
    voice_endpoint(&base, &key);
    int err;
    esp_http_client_handle_t c = open_direct(base, key, "/audio/transcriptions", 30000, &err);
    if (!c) return err;
    esp_http_client_set_header(c, "Content-Type", "multipart/form-data; boundary=" BOUNDARY);
    if (esp_http_client_open(c, -1) != ESP_OK) {
        esp_http_client_cleanup(c);
        return VOICE_ERR_NET;
    }
    char head[320];
    const int hl = snprintf(head, sizeof(head),
        "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n%s\r\n"
        "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"voice.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n", g_settings.asr_model);
    uint8_t wav[44];
    wav_header(wav);
    bool ok = write_chunk(c, (const uint8_t *)head, (size_t)hl) && write_chunk(c, wav, sizeof(wav));
    if (!ok || s_cancel || s_end) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return !ok ? VOICE_ERR_NET : s_cancel ? ASR_CANCELLED : ASR_EARLY;
    }
    // 录音缓冲(8KB)等握手完成再建:握手时内存峰值最高,不和它抢。
    if (!s_stream) s_stream = xStreamBufferCreate(STREAM_BYTES, 1);
    if (s_stream) xStreamBufferReset(s_stream);
    if (!s_stream || !audio_capture_start(sink, NULL)) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return VOICE_ERR_MEM;
    }
    s_state = AI_LISTENING;   // 界面此时响提示音:可以说话了
    const int64_t started = sys_now_ms();
    uint8_t blk[ADPCM_BLOCK_BYTES];
    size_t blk_fill = 0;   // 流缓冲是字节流:攒满一整块再解码
    static int16_t pcm[ADPCM_BLOCK_SAMPLES];
    while (ok) {
        const size_t n = xStreamBufferReceive(s_stream, blk + blk_fill, sizeof(blk) - blk_fill,
                                              pdMS_TO_TICKS(40));
        blk_fill += n;
        if (blk_fill == sizeof(blk)) {
            const int samples = adpcm_decode_block(blk, blk_fill, pcm, ADPCM_BLOCK_SAMPLES);
            ok = write_chunk(c, (const uint8_t *)pcm, (size_t)samples * 2);
            blk_fill = 0;
        }
        if (s_cancel) break;
        if (sys_now_ms() - started > MAX_TALK_MS) s_end = true;
        if (s_end && !n && xStreamBufferIsEmpty(s_stream)) break;
    }
    audio_capture_stop();
    drop_stream();   // 对话和朗读还要各握手一次:把录音缓冲还回去
    static const char tail[] = "\r\n--" BOUNDARY "--\r\n";
    if (ok && !s_cancel) ok = write_chunk(c, (const uint8_t *)tail, sizeof(tail) - 1) && write_chunk(c, NULL, 0);
    int status = s_cancel ? ASR_CANCELLED : VOICE_ERR_UPLOAD;
    static char resp[768];
    resp[0] = '\0';
    if (ok && !s_cancel) {
        s_state = AI_THINKING;
        if (esp_http_client_fetch_headers(c) < 0) {
            status = VOICE_ERR_NET;
        } else {
            status = esp_http_client_get_status_code(c);
            read_body(c, resp, sizeof(resp));
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (status != 200) {
        if (status != ASR_CANCELLED) ESP_LOGW(TAG, "识别失败 %d: %.160s", status, resp);
        return status;
    }
    cJSON *j = cJSON_Parse(resp);
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(j, "text");
    utf8_copy(text, len, cJSON_IsString(t) ? t->valuestring : "");
    if (!j) ESP_LOGW(TAG, "识别结果看不懂: %.160s", resp);
    cJSON_Delete(j);
    return text[0] ? 200 : (j ? VOICE_ERR_EMPTY : VOICE_ERR_PARSE);
}

static cJSON *add_msg(cJSON *msgs, const char *role, const char *content)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", role);
    cJSON_AddStringToObject(m, "content", content);
    cJSON_AddItemToArray(msgs, m);
    return m;
}

// 对话。返回 200 或错误码。
static int direct_chat(const char *question, char *answer, size_t len)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", g_settings.llm_model);
    cJSON_AddNumberToObject(req, "max_tokens", 400);
    cJSON_AddBoolToObject(req, "stream", false);
    cJSON *msgs = cJSON_AddArrayToObject(req, "messages");
    add_msg(msgs, "system", DIRECT_SYSTEM);
    for (int i = 0; i < s_hist_n; i++) {   // 带上最近两轮,支持追问
        add_msg(msgs, "user", s_hist_q[i]);
        add_msg(msgs, "assistant", s_hist_a[i]);
    }
    add_msg(msgs, "user", question);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) return VOICE_ERR_MEM;

    int err;
    esp_http_client_handle_t c = open_direct(g_settings.api_base, g_settings.api_key, "/chat/completions",
                                             45000, &err);
    int status = err;
    char *resp = NULL;
    int got = 0;
    if (c) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
        const int bl = (int)strlen(body);
        status = VOICE_ERR_NET;
        if (esp_http_client_open(c, bl) == ESP_OK && esp_http_client_write(c, body, bl) == bl
            && esp_http_client_fetch_headers(c) >= 0) {
            status = esp_http_client_get_status_code(c);
            resp = malloc(CHAT_RESP_MAX);
            if (!resp) status = VOICE_ERR_MEM;
            else got = read_body(c, resp, CHAT_RESP_MAX);
        }
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    free(body);
    // 先关掉加密连接再解析:握手和收发缓冲已经还回去,解析 JSON 不和它们抢内存。
    if (resp) {
        if (status == 200) {
            // 读满说明被截断了(例如推理模型带了很长的思考内容)
            cJSON *j = got < CHAT_RESP_MAX - 1 ? cJSON_Parse(resp) : NULL;
            const cJSON *ch = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j, "choices"), 0);
            const cJSON *msg = cJSON_GetObjectItemCaseSensitive(ch, "message");
            const cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, "content");
            if (cJSON_IsString(content) && content->valuestring[0]) utf8_copy(answer, len, content->valuestring);
            else status = VOICE_ERR_PARSE;
            cJSON_Delete(j);
        }
        if (status != 200) ESP_LOGW(TAG, "对话失败 %d: %.200s", status, resp);
        free(resp);
    }
    if (status == 200) {
        if (s_hist_n == 2) {
            memcpy(s_hist_q[0], s_hist_q[1], sizeof(s_hist_q[0]));
            memcpy(s_hist_a[0], s_hist_a[1], sizeof(s_hist_a[0]));
            s_hist_n = 1;
        }
        // 按字符截断:半个汉字会让下一轮请求变成非法 UTF-8 而被拒
        utf8_copy(s_hist_q[s_hist_n], sizeof(s_hist_q[0]), question);
        utf8_copy(s_hist_a[s_hist_n], sizeof(s_hist_a[0]), answer);
        s_hist_n++;
    }
    return status;
}

// 朗读用:把 PCM 样本攒成整块,编码后送去播放。
typedef struct {
    int16_t acc[ADPCM_BLOCK_SAMPLES];
    int n;
    adpcm_state_t enc;
} play_acc_t;

static void play_samples(play_acc_t *p, const int16_t *pcm, size_t n)
{
    uint8_t blk[ADPCM_BLOCK_BYTES];
    for (size_t i = 0; i < n; i++) {
        p->acc[p->n++] = pcm[i];
        if (p->n == ADPCM_BLOCK_SAMPLES) {
            audio_play_push(blk, adpcm_encode_block(&p->enc, p->acc, ADPCM_BLOCK_SAMPLES, blk));
            p->n = 0;
        }
    }
}

static void direct_speak(const char *text)
{
    if (g_settings.tts_rate == VOICE_TTS_OFF || !audio_available() || !g_settings.tts_model[0] || !text[0]
        || s_cancel) {
        s_state = AI_IDLE;
        return;
    }
    const char *base, *key;
    voice_endpoint(&base, &key);
    if (!key[0]) {
        fail("朗读:没填语音密钥");
        return;
    }
    const bool rate24 = g_settings.tts_rate == VOICE_TTS_24K;
    s_state = AI_SPEAKING;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", g_settings.tts_model);
    cJSON_AddStringToObject(req, "voice", g_settings.tts_voice);
    cJSON_AddStringToObject(req, "input", text);
    cJSON_AddStringToObject(req, "response_format", "pcm");
    // OpenAI 的 pcm 固定 24 kHz,不认 sample_rate;硅基流动等支持指定 16 kHz。
    if (!rate24) cJSON_AddNumberToObject(req, "sample_rate", 16000);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    int err = VOICE_ERR_MEM;
    esp_http_client_handle_t c = body ? open_direct(base, key, "/audio/speech", 30000, &err) : NULL;
    int status = err;
    if (c) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
        const int bl = (int)strlen(body);
        status = VOICE_ERR_NET;
        if (esp_http_client_open(c, bl) == ESP_OK && esp_http_client_write(c, body, bl) == bl
            && esp_http_client_fetch_headers(c) >= 0) {
            status = esp_http_client_get_status_code(c);
            // 约 1.8KB,只在朗读时占用
            struct {
                play_acc_t acc;
                resample24_t rst;
                int16_t raw[384], rs[260];
            } *w = status == 200 ? calloc(1, sizeof(*w)) : NULL;
            if (status == 200 && !w) status = VOICE_ERR_MEM;
            if (w) {
                int fill = 0;   // raw 里的字节数(可能有半个样本留到下一次)
                while (!s_cancel) {
                    const int r = esp_http_client_read(c, (char *)w->raw + fill, (int)sizeof(w->raw) - fill);
                    if (r <= 0) break;
                    fill += r;
                    const size_t samples = (size_t)fill / 2;
                    if (rate24) play_samples(&w->acc, w->rs, resample24_to16(&w->rst, w->raw, samples, w->rs));
                    else play_samples(&w->acc, w->raw, samples);
                    if (fill & 1) ((uint8_t *)w->raw)[0] = ((uint8_t *)w->raw)[fill - 1];
                    fill &= 1;
                }
                if (w->acc.n >= 2 && !s_cancel) {
                    uint8_t blk[ADPCM_BLOCK_BYTES];
                    audio_play_push(blk, adpcm_encode_block(&w->acc.enc, w->acc.acc, w->acc.n, blk));
                }
                free(w);
            } else if (status != 200 && status != VOICE_ERR_MEM) {
                char resp[200];
                read_body(c, resp, sizeof(resp));
                ESP_LOGW(TAG, "合成失败 %d: %s", status, resp);
            }
        }
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    free(body);
    while (!s_cancel && audio_playing()) vTaskDelay(pdMS_TO_TICKS(50));
    if (s_cancel) audio_play_flush();
    if (status != 200 && !s_cancel) {
        fail_stage("朗读", status);   // 文字回复已经显示,只提示朗读这一步
        return;
    }
    s_state = AI_IDLE;
}

static void direct_talk(void)
{
    char question[160];
    const int st = direct_asr(question, sizeof(question));
    if (s_cancel || st == ASR_CANCELLED) {
        s_state = AI_IDLE;
        return;
    }
    if (st == ASR_EARLY) {
        fail("听到提示音后再说话");
        return;
    }
    if (st != 200) {
        fail_stage("识别", st);
        return;
    }
    s_state = AI_THINKING;
    static char answer[640];
    const int cst = direct_chat(question, answer, sizeof(answer));
    if (s_cancel) {   // 思考时按了取消:不显示也不朗读
        s_state = AI_IDLE;
        return;
    }
    if (cst != 200) {
        fail_stage("对话", cst);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    utf8_copy(s_q, sizeof(s_q), question);
    utf8_copy(s_a, sizeof(s_a), answer);
    s_audio_id[0] = '\0';
    s_last_direct = true;
    s_result_ready = true;
    xSemaphoreGive(s_lock);
    direct_speak(answer);
}

static volatile bool s_quit;   // 离开语音助手:任务空闲后自行退出并释放约 14KB

static void ai_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_quit) {
            audio_capture_stop();
            drop_stream();
            ESP_LOGI(TAG, "语音助手已释放(空闲 %u)", (unsigned)sys_free_heap());
            s_task = NULL;
            vTaskDelete(NULL);
        }
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_quit) continue;
        if (s_replay) {
            s_replay = false;
            s_cancel = false;
            if (s_audio_id[0]) speak();
            else if (s_last_direct) direct_speak(s_a);
            continue;
        }
        if (s_state == AI_CONNECTING) direct_talk();
        else if (s_state == AI_LISTENING) talk();
    }
}

bool ai_talk_begin(void)
{
    const bool relay_ok = relay_state() == SRV_OK;
    const bool has_key = g_settings.api_key[0] != '\0';
    if (net_state() != NET_CONNECTED) {
        fail("还没联网");
        return false;
    }
    if (!voice_ready(g_settings.ai_mode, relay_ok, has_key, true)) {
        fail(g_settings.ai_mode == VOICE_MODE_RELAY ? "没找到 Mac 中转" : "没填对话密钥");
        return false;
    }
    const bool direct = voice_use_direct(g_settings.ai_mode, relay_ok, has_key);
    if (direct) {
        const char *base, *key;
        voice_endpoint(&base, &key);
        if (!key[0]) {
            fail("识别:没填语音密钥");
            return false;
        }
    }
    if (!audio_available()) {
        fail("音频不可用");
        return false;
    }
    // 上一次离开时任务还在退出:稍等它释放完
    for (int i = 0; i < 25 && s_quit && s_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
    if (s_quit && s_task) {
        fail("正在释放,请再试一次");
        return false;
    }
    s_quit = false;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (s_state != AI_IDLE && s_state != AI_ERROR) return false;
    if (!s_task && xTaskCreate(ai_task, "ai", 6144, NULL, 5, &s_task) != pdPASS) {
        s_task = NULL;
        fail("内存不足");
        return false;
    }
    // 中转:松手前就在边录边传,缓冲现在就要。直连:识别接口连好后才建(见 direct_asr)。
    if (!direct) {
        if (!s_stream) s_stream = xStreamBufferCreate(STREAM_BYTES, 1);
        if (!s_stream) {
            fail("内存不足");
            return false;
        }
        xStreamBufferReset(s_stream);
    }
    audio_play_flush();
    s_end = false;
    s_cancel = false;
    s_error[0] = '\0';
    if (direct) {
        s_state = AI_CONNECTING;   // 任务连好识别接口后自己开麦
    } else {
        s_state = AI_LISTENING;
        if (!audio_capture_start(sink, NULL)) {
            fail("麦克风不可用");
            return false;
        }
    }
    xTaskNotifyGive(s_task);
    return true;
}

void ai_talk_end(void) { s_end = true; }

void ai_release(void)
{
    if (!s_task) return;
    ai_cancel();
    s_quit = true;
    xTaskNotifyGive(s_task);
}

bool ai_active(void) { return s_task != NULL; }

bool ai_cancelling(void) { return s_cancel && s_state != AI_IDLE && s_state != AI_ERROR; }

void ai_clear_history(void)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_hist_n = 0;   // 只在对话成功后由任务改写;这里清掉后,下一轮不再带旧上下文
    s_a[0] = '\0';
    s_audio_id[0] = '\0';
    s_last_direct = false;
    if (s_lock) xSemaphoreGive(s_lock);
}

void ai_cancel(void)
{
    s_cancel = true;
    s_end = true;
    audio_play_flush();
    if (s_state == AI_ERROR) s_state = AI_IDLE;
}

bool ai_take_result(char *question, size_t qlen, char *answer, size_t alen)
{
    if (!s_result_ready || !s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(question, qlen, "%s", s_q);
    snprintf(answer, alen, "%s", s_a);
    s_result_ready = false;
    xSemaphoreGive(s_lock);
    return true;
}

void ai_replay(void)
{
    if (!s_task || (!s_audio_id[0] && !s_a[0])) return;
    // 等上一轮任务因取消而退回空闲
    s_replay = true;
    xTaskNotifyGive(s_task);
}
