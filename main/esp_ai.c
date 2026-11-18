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
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include <stdio.h>
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

static void ai_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_replay) {
            s_replay = false;
            s_cancel = false;
            speak();
            continue;
        }
        if (s_state == AI_LISTENING) talk();
    }
}

bool ai_talk_begin(void)
{
    if (relay_state() != SRV_OK) {
        fail("找不到服务器");
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
    if (!s_task || !s_audio_id[0]) return;
    // 等上一轮任务因取消而退回空闲
    s_replay = true;
    xTaskNotifyGive(s_task);
}
