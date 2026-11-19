// main/esp_ai.c —— 语音助手客户端。
//
// 1. 按住:麦克风 → ADPCM 块 → 流缓冲 → HTTP 分块上传到 POST /api/assistant/talk
//    (边说边传,松手时服务器已经收到大部分音频)。
// 2. 松手:发结束块,等待服务器跑完 识别 → 大模型 → 语音合成,返回
//    {"question","answer","audio"}。
// 3. 朗读:GET /api/assistant/audio/<id>,服务器按同样的 ADPCM 块格式流式返回,边收边播。
#include "audio_dsp.h"
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
static char s_error[48];
static char s_q[160], s_a[640];
static bool s_result_ready;
static char s_audio_id[40];

ai_state_t ai_state(void) { return s_state; }
const char *ai_error(void) { return s_error; }

static void fail(const char *msg)
{
    snprintf(s_error, sizeof(s_error), "%s", msg);
    s_state = AI_ERROR;
    ESP_LOGW(TAG, "%s", msg);
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
    snprintf(s_q, sizeof(s_q), "%s", cJSON_IsString(q) ? q->valuestring : "(没听清)");
    snprintf(s_a, sizeof(s_a), "%s", cJSON_IsString(a) ? a->valuestring : "");
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
        fail("朗读失败");
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
        fail("连不上服务器");
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
        else fail("上传中断");
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
        fail(status == 401 ? "服务器未授权本设备" : status == 422 ? "没听清,再说一次" : "服务器出错了");
        return;
    }
    parse_reply(resp);
    speak();
}

// ===========================================================================
// 直连模式:没有中转服务器时,设备自己调用兼容 OpenAI 接口的云服务
//   识别  POST {base}/audio/transcriptions   multipart(WAV 边录边传,分块上传)
//   对话  POST {base}/chat/completions
//   合成  POST {base}/audio/speech            返回 16 kHz PCM,边收边播
// ===========================================================================
#define BOUNDARY "----passport7c3boundary"
#define DIRECT_SYSTEM "你是装在随身小设备里的中文语音助手,回答会被朗读并显示在小屏幕上。" \
    "用简体中文口语回答,先给结论,通常一到三句话、不超过 120 字,不要用 Markdown、列表或表情。"

static char s_hist_q[2][160], s_hist_a[2][320];
static int s_hist_n;

static bool direct_mode(void) { return relay_state() != SRV_OK && g_settings.api_key[0]; }

static esp_http_client_handle_t open_direct(const char *path, int timeout)
{
    char url[160];
    snprintf(url, sizeof(url), "%s%s", g_settings.api_base, path);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = timeout,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NULL;
    char auth[120];
    snprintf(auth, sizeof(auth), "Bearer %s", g_settings.api_key);
    esp_http_client_set_header(c, "Authorization", auth);
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

// 识别:把录音流(ADPCM 块)解码成 PCM,包成 WAV 分块上传。
static bool direct_asr(char *text, size_t len)
{
    esp_http_client_handle_t c = open_direct("/audio/transcriptions", 30000);
    if (!c) return false;
    esp_http_client_set_header(c, "Content-Type", "multipart/form-data; boundary=" BOUNDARY);
    if (esp_http_client_open(c, -1) != ESP_OK) {
        esp_http_client_cleanup(c);
        return false;
    }
    char head[320];
    const int hl = snprintf(head, sizeof(head),
        "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n%s\r\n"
        "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"voice.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n", g_settings.asr_model);
    uint8_t wav[44];
    wav_header(wav);
    bool ok = write_chunk(c, (const uint8_t *)head, (size_t)hl) && write_chunk(c, wav, sizeof(wav));
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
    static const char tail[] = "\r\n--" BOUNDARY "--\r\n";
    if (ok && !s_cancel) ok = write_chunk(c, (const uint8_t *)tail, sizeof(tail) - 1) && write_chunk(c, NULL, 0);
    if (ok && !s_cancel) s_state = AI_THINKING;
    int status = -1;
    static char resp[768];
    if (ok && !s_cancel) {
        esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        read_body(c, resp, sizeof(resp));
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (status != 200) {
        if (status > 0) ESP_LOGW(TAG, "识别失败 %d: %.120s", status, resp);
        return false;
    }
    cJSON *j = cJSON_Parse(resp);
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(j, "text");
    snprintf(text, len, "%s", cJSON_IsString(t) ? t->valuestring : "");
    cJSON_Delete(j);
    return text[0] != '\0';
}

static bool direct_chat(const char *question, char *answer, size_t len)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", g_settings.llm_model);
    cJSON_AddNumberToObject(req, "max_tokens", 400);
    cJSON *msgs = cJSON_AddArrayToObject(req, "messages");
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "system");
    cJSON_AddStringToObject(m, "content", DIRECT_SYSTEM);
    cJSON_AddItemToArray(msgs, m);
    for (int i = 0; i < s_hist_n; i++) {   // 带上最近两轮,支持追问
        m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", "user");
        cJSON_AddStringToObject(m, "content", s_hist_q[i]);
        cJSON_AddItemToArray(msgs, m);
        m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", "assistant");
        cJSON_AddStringToObject(m, "content", s_hist_a[i]);
        cJSON_AddItemToArray(msgs, m);
    }
    m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON_AddStringToObject(m, "content", question);
    cJSON_AddItemToArray(msgs, m);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) return false;

    esp_http_client_handle_t c = open_direct("/chat/completions", 60000);
    bool ok = false;
    if (c) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
        const int bl = (int)strlen(body);
        if (esp_http_client_open(c, bl) == ESP_OK && esp_http_client_write(c, body, bl) == bl) {
            esp_http_client_fetch_headers(c);
            const int status = esp_http_client_get_status_code(c);
            char *resp = malloc(4096);
            if (resp) {
                read_body(c, resp, 4096);
                cJSON *j = status == 200 ? cJSON_Parse(resp) : NULL;
                const cJSON *ch = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j, "choices"), 0);
                const cJSON *msg = cJSON_GetObjectItemCaseSensitive(ch, "message");
                const cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, "content");
                if (cJSON_IsString(content)) {
                    snprintf(answer, len, "%s", content->valuestring);
                    ok = true;
                } else {
                    ESP_LOGW(TAG, "对话失败 %d: %.160s", status, resp);
                }
                cJSON_Delete(j);
                free(resp);
            }
        }
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    free(body);
    if (ok) {
        if (s_hist_n == 2) {
            memcpy(s_hist_q[0], s_hist_q[1], sizeof(s_hist_q[0]));
            memcpy(s_hist_a[0], s_hist_a[1], sizeof(s_hist_a[0]));
            s_hist_n = 1;
        }
        snprintf(s_hist_q[s_hist_n], sizeof(s_hist_q[0]), "%s", question);
        snprintf(s_hist_a[s_hist_n], sizeof(s_hist_a[0]), "%s", answer);
        s_hist_n++;
    }
    return ok;
}

static void direct_speak(const char *text)
{
    if (!audio_available() || !g_settings.tts_model[0] || !text[0]) {
        s_state = AI_IDLE;
        return;
    }
    s_state = AI_SPEAKING;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", g_settings.tts_model);
    cJSON_AddStringToObject(req, "voice", g_settings.tts_voice);
    cJSON_AddStringToObject(req, "input", text);
    cJSON_AddStringToObject(req, "response_format", "pcm");
    cJSON_AddNumberToObject(req, "sample_rate", 16000);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    esp_http_client_handle_t c = body ? open_direct("/audio/speech", 30000) : NULL;
    if (c) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
        const int bl = (int)strlen(body);
        if (esp_http_client_open(c, bl) == ESP_OK && esp_http_client_write(c, body, bl) == bl) {
            esp_http_client_fetch_headers(c);
            if (esp_http_client_get_status_code(c) == 200) {
                static int16_t pcm[ADPCM_BLOCK_SAMPLES];
                uint8_t blk[ADPCM_BLOCK_BYTES];
                adpcm_state_t enc = {0, 0};
                int fill = 0;   // 字节
                while (!s_cancel) {
                    const int r = esp_http_client_read(c, (char *)pcm + fill, (int)sizeof(pcm) - fill);
                    if (r <= 0) break;
                    fill += r;
                    if (fill == (int)sizeof(pcm)) {
                        audio_play_push(blk, adpcm_encode_block(&enc, pcm, ADPCM_BLOCK_SAMPLES, blk));
                        fill = 0;
                    }
                }
                if (fill >= 4 && !s_cancel) audio_play_push(blk, adpcm_encode_block(&enc, pcm, fill / 2, blk));
            } else {
                ESP_LOGW(TAG, "合成失败 %d", esp_http_client_get_status_code(c));
            }
        }
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    free(body);
    while (!s_cancel && audio_playing()) vTaskDelay(pdMS_TO_TICKS(50));
    if (s_cancel) audio_play_flush();
    s_state = AI_IDLE;
}

static void direct_talk(void)
{
    char question[160];
    if (!direct_asr(question, sizeof(question))) {
        if (s_cancel) s_state = AI_IDLE;
        else fail("没听清,再说一次");
        return;
    }
    if (s_cancel) {
        s_state = AI_IDLE;
        return;
    }
    s_state = AI_THINKING;
    static char answer[640];
    if (!direct_chat(question, answer, sizeof(answer))) {
        fail("云端对话失败,检查密钥");
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_q, sizeof(s_q), "%s", question);
    snprintf(s_a, sizeof(s_a), "%s", answer);
    s_audio_id[0] = '\0';
    s_result_ready = true;
    xSemaphoreGive(s_lock);
    direct_speak(answer);
}

static void ai_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_replay) {
            s_replay = false;
            s_cancel = false;
            if (s_audio_id[0]) speak();
            else direct_speak(s_a);
            continue;
        }
        if (s_state == AI_LISTENING) {
            if (direct_mode()) direct_talk();
            else talk();
        }
    }
}

bool ai_talk_begin(void)
{
    if (relay_state() != SRV_OK && !g_settings.api_key[0]) {
        fail("没有服务器,也没填云端密钥");
        return false;
    }
    if (net_state() != NET_CONNECTED) {
        fail("还没联网");
        return false;
    }
    if (!audio_available()) {
        fail("音频不可用");
        return false;
    }
    if (!s_task) {
        s_lock = xSemaphoreCreateMutex();
        s_stream = xStreamBufferCreate(STREAM_BYTES, 1);
        if (!s_stream || xTaskCreate(ai_task, "ai", 6144, NULL, 5, &s_task) != pdPASS) {
            fail("内存不足");
            return false;
        }
    }
    if (s_state != AI_IDLE && s_state != AI_ERROR) return false;
    xStreamBufferReset(s_stream);
    audio_play_flush();
    s_end = false;
    s_cancel = false;
    s_error[0] = '\0';
    s_state = AI_LISTENING;
    if (!audio_capture_start(sink, NULL)) {
        fail("麦克风不可用");
        return false;
    }
    xTaskNotifyGive(s_task);
    return true;
}

void ai_talk_end(void) { s_end = true; }

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
