// main/esp_audio.c —— 音频服务:一个任务独占 ES8311(16 kHz / 16 bit / 单声道),按需做
//   · 提示音(正弦 + 包络,不刺耳)
//   · 实时分析(频谱 20 段、近似分贝、音高)
//   · 节拍器(重拍高音)
//   · 采集:编码成 IMA ADPCM 块回调给 AI / 对讲
//   · 播放:接收 ADPCM 块(AI 回答、对讲)解码播放
// I2S TX 配置了 auto_clear,只读麦克风不写时扬声器输出静音。
#include "audio_dsp.h"
#include "noise_gen.h"
#include "settings.h"
#include "svc.h"
#include "sys.h"

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

static const char *TAG = "audio";

#define SR 16000
#define CHUNK 256                 // 16 ms
#define ANALYZE_N 1024
#define PLAY_DEPTH 40             // 40 块 ≈ 640 ms 缓冲

typedef enum { CMD_BEEP = 0, CMD_VOLUME } acmd_type_t;
typedef struct {
    acmd_type_t type;
    int arg;
} acmd_t;

typedef struct {
    uint16_t len;
    uint8_t data[ADPCM_BLOCK_BYTES];
} ablock_t;

static bool s_ok;
static QueueHandle_t s_cmd;
static QueueHandle_t s_play;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;

static volatile bool s_analyze;
static volatile bool s_want_pitch;
static volatile bool s_capture;
static audio_sink_t s_sink;
static void *s_sink_user;
static volatile float s_level;
static audio_analysis_t s_result;
static volatile bool s_result_fresh;

static volatile bool s_metro_on;
static volatile int s_bpm = 96;
static volatile int s_beats = 4;
static volatile uint32_t s_metro_ticks;
static volatile bool s_playing;
static volatile int s_noise = NOISE_OFF;     // 想要的环境声
static int s_noise_playing = NOISE_OFF;      // 正在播放(渐出完成前保持旧类型)
static noise_state_t s_noise_st;

bool audio_available(void) { return s_ok; }

// ---------------------------------------------------------------------------
// 合成
// ---------------------------------------------------------------------------
static void tone(float hz, int ms, float amp)
{
    static int16_t buf[CHUNK];
    const int total = SR * ms / 1000;
    const int attack = SR * 4 / 1000, release = SR * 25 / 1000;
    float phase = 0;
    const float step = 2.0f * (float)M_PI * hz / SR;
    for (int done = 0; done < total;) {
        const int n = total - done < CHUNK ? total - done : CHUNK;
        for (int i = 0; i < n; i++) {
            const int t = done + i;
            float env = 1.0f;
            if (t < attack) env = (float)t / attack;
            else if (t > total - release) env = (float)(total - t) / release;
            // 加一点二次谐波,小喇叭上更清楚
            buf[i] = (int16_t)(amp * 26000.0f * env * (0.8f * sinf(phase) + 0.2f * sinf(2 * phase)));
            phase += step;
            if (phase > 2 * (float)M_PI) phase -= 2 * (float)M_PI;
        }
        bsp_audio_write(buf, (size_t)n * 2);
        done += n;
    }
}

static void silence(int ms)
{
    static const int16_t zero[CHUNK];
    for (int left = SR * ms / 1000; left > 0; left -= CHUNK) {
        bsp_audio_write(zero, (size_t)(left < CHUNK ? left : CHUNK) * 2);
    }
}

static void play_beep(beep_t b)
{
    switch (b) {
    case BEEP_CLICK: tone(1800, 12, 0.25f); break;
    case BEEP_OK: tone(1320, 60, 0.5f); tone(1760, 90, 0.5f); break;
    case BEEP_ERR: tone(330, 140, 0.6f); silence(40); tone(262, 200, 0.6f); break;
    case BEEP_START: tone(988, 60, 0.5f); tone(1319, 80, 0.5f); break;
    case BEEP_STOP: tone(1319, 60, 0.5f); tone(988, 80, 0.5f); break;
    case BEEP_MSG: tone(1568, 80, 0.55f); silence(50); tone(2093, 120, 0.55f); break;
    case BEEP_WORK_DONE:   // 上行琶音 ×2:完成了!
        for (int r = 0; r < 2; r++) {
            tone(1047, 110, 0.7f); tone(1319, 110, 0.7f); tone(1568, 110, 0.7f); tone(2093, 220, 0.7f);
            silence(180);
        }
        break;
    case BEEP_BREAK_DONE:  // 下行:该回来干活了
        tone(1568, 140, 0.7f); tone(1319, 140, 0.7f); tone(1047, 260, 0.7f);
        break;
    }
}

// ---------------------------------------------------------------------------
// 分析
// ---------------------------------------------------------------------------
static int16_t s_win[ANALYZE_N];
static int s_win_fill;
static float s_peak_db;

static void analyze_push(const int16_t *pcm, int n)
{
    memmove(s_win, s_win + n, (ANALYZE_N - n) * sizeof(int16_t));
    memcpy(s_win + ANALYZE_N - n, pcm, (size_t)n * sizeof(int16_t));
    if (s_win_fill < ANALYZE_N) s_win_fill += n;
    if (s_win_fill < ANALYZE_N) return;
    // 没有浮点单元:频谱约 15 次/秒;音高只在调音器页需要时算(约 10 次/秒)。
    static int skip;
    ++skip;
    const bool do_pitch = s_want_pitch && skip % 6 == 0;
    if (skip % 4 && !do_pitch) return;

    audio_analysis_t r;
    dsp_bands(s_win + ANALYZE_N - DSP_FFT_N, DSP_FFT_N, SR, r.bands, AUDIO_BANDS);
    const float dbfs = dsp_dbfs(s_win + ANALYZE_N - CHUNK * 2, CHUNK * 2);
    r.db = dbfs + DSP_SPL_OFFSET;
    if (r.db < 20) r.db = 20;
    if (r.db > s_peak_db) s_peak_db = r.db;
    r.peak_db = s_peak_db;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    r.pitch_hz = s_result.pitch_hz;
    r.clarity = s_result.clarity;
    xSemaphoreGive(s_lock);
    if (do_pitch) r.pitch_hz = dsp_pitch(s_win, ANALYZE_N, SR, &r.clarity);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_result = r;
    s_result_fresh = true;
    xSemaphoreGive(s_lock);
}

// ---------------------------------------------------------------------------
static void audio_task(void *arg)
{
    (void)arg;
    static int16_t pcm[CHUNK];
    static int16_t enc_buf[ADPCM_BLOCK_SAMPLES];
    adpcm_state_t enc = {0, 0};
    int enc_fill = 0;
    int64_t next_beat = 0;
    uint8_t block[ADPCM_BLOCK_BYTES];

    for (;;) {
        acmd_t c;
        const bool busy = s_analyze || s_capture || s_metro_on || uxQueueMessagesWaiting(s_play)
                       || s_noise != NOISE_OFF || s_noise_playing != NOISE_OFF;
        if (xQueueReceive(s_cmd, &c, busy ? 0 : pdMS_TO_TICKS(50)) == pdTRUE) {
            if (c.type == CMD_VOLUME) bsp_audio_set_volume((uint8_t)c.arg);
            else if (!s_capture) play_beep((beep_t)c.arg);   // 录音时不出提示音,免得录进去
            continue;
        }

        // 播放(AI 回答 / 对讲)
        ablock_t ab;
        if (!s_capture && xQueueReceive(s_play, &ab, 0) == pdTRUE) {
            s_playing = true;
            const int n = adpcm_decode_block(ab.data, ab.len, enc_buf, ADPCM_BLOCK_SAMPLES);
            bsp_audio_write(enc_buf, (size_t)n * 2);
            continue;
        }
        s_playing = false;

        // 环境声:没有录音时持续合成;切换或关闭时先渐出再换,避免爆音
        if (!s_capture && (s_noise != NOISE_OFF || s_noise_playing != NOISE_OFF)) {
            static int16_t nbuf[CHUNK];
            if (s_noise_playing == NOISE_OFF) {
                s_noise_playing = s_noise;
                s_noise_st.gain_q8 = 0;
            }
            const bool switching = s_noise != s_noise_playing;
            noise_fill(&s_noise_st, (noise_type_t)s_noise_playing, nbuf, CHUNK, switching ? 0 : 150);
            bsp_audio_write(nbuf, sizeof(nbuf));
            if (switching && s_noise_st.gain_q8 == 0) s_noise_playing = s_noise;   // 渐出完成
            if (!s_analyze && !s_metro_on) continue;
        }

        // 节拍器
        if (s_metro_on) {
            const int64_t now = sys_now_ms();
            if (!next_beat || next_beat < now - 1000) next_beat = now;
            if (now >= next_beat) {
                const bool accent = s_metro_ticks % (uint32_t)s_beats == 0;
                s_metro_ticks++;
                tone(accent ? 2093 : 1568, 30, accent ? 0.8f : 0.55f);
                next_beat += 60000 / (s_bpm > 0 ? s_bpm : 60);
            }
            if (!s_analyze && !s_capture) {
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }
        } else {
            next_beat = 0;
        }

        // 麦克风
        if (s_analyze || s_capture) {
            if (bsp_audio_read(pcm, sizeof(pcm)) != ESP_OK) {
                static int64_t warned;
                if (sys_now_ms() - warned > 5000) {
                    warned = sys_now_ms();
                    ESP_LOGW(TAG, "麦克风读取失败");
                }
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            int32_t peak = 0;
            for (int i = 0; i < CHUNK; i++) {
                const int32_t v = pcm[i] < 0 ? -pcm[i] : pcm[i];
                if (v > peak) peak = v;
            }
            const float lvl = sqrtf((float)peak / 32768.0f);
            s_level = s_level * 0.6f + lvl * 0.4f;
            if (s_analyze) {
                analyze_push(pcm, CHUNK);
                taskYIELD();
            }
            if (s_capture && s_sink) {
                for (int i = 0; i < CHUNK; i++) {
                    enc_buf[enc_fill++] = pcm[i];
                    if (enc_fill == ADPCM_BLOCK_SAMPLES) {
                        const size_t len = adpcm_encode_block(&enc, enc_buf, ADPCM_BLOCK_SAMPLES, block);
                        s_sink(block, len, s_sink_user);
                        enc_fill = 0;
                    }
                }
            } else {
                enc_fill = 0;
            }
        }
    }
}

void audio_svc_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_cmd = xQueueCreate(6, sizeof(acmd_t));
    s_play = xQueueCreate(PLAY_DEPTH, sizeof(ablock_t));
    if (bsp_audio_init() != ESP_OK || bsp_audio_set_format(SR, 16, 1) != ESP_OK) {
        ESP_LOGW(TAG, "音频不可用:提示音、声音工具、语音功能将关闭");
        return;
    }
    bsp_audio_set_volume((uint8_t)g_settings.volume);
    s_ok = true;
    // 优先级必须低于 LVGL 任务(4):分析算得慢时只会少几帧,不会饿死界面和空闲任务
    // (之前是 6,配合软件浮点的音高检测会触发看门狗 = "死机")。
    xTaskCreate(audio_task, "audio", 6144, NULL, 3, &s_task);
}

void audio_beep(beep_t pattern)
{
    if (!s_ok) return;
    const acmd_t c = {CMD_BEEP, pattern};
    xQueueSend(s_cmd, &c, 0);
}

void audio_apply_volume(void)
{
    if (!s_ok) return;
    const acmd_t c = {CMD_VOLUME, g_settings.volume};
    xQueueSend(s_cmd, &c, 0);
}

void audio_analyze_pitch(bool on) { s_want_pitch = on; }

bool audio_analyze_start(void)
{
    s_peak_db = 0;
    s_win_fill = 0;
    s_analyze = s_ok;
    return s_ok;
}

void audio_analyze_stop(void)
{
    s_analyze = false;
    s_want_pitch = false;
}

bool audio_analysis_get(audio_analysis_t *out)
{
    if (!s_ok || !s_result_fresh) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_result;
    s_result_fresh = false;
    xSemaphoreGive(s_lock);
    return true;
}

void audio_noise(int type)
{
    if (!s_ok) return;
    if (type < NOISE_OFF || type >= NOISE_COUNT) type = NOISE_OFF;
    if (s_noise_st.rng == 0) noise_init(&s_noise_st, (uint32_t)sys_now_ms() | 1u);
    s_noise = type;
}

int audio_noise_current(void) { return s_noise; }

void audio_metronome_set(int bpm, int beats_per_bar)
{
    s_bpm = bpm;
    s_beats = beats_per_bar > 0 ? beats_per_bar : 4;
}

void audio_metronome_run(bool on)
{
    if (on) s_metro_ticks = 0;
    s_metro_on = on && s_ok;
}

uint32_t audio_metronome_ticks(void) { return s_metro_ticks; }

bool audio_capture_start(audio_sink_t sink, void *user)
{
    if (!s_ok) return false;
    s_sink = sink;
    s_sink_user = user;
    s_level = 0;
    s_capture = true;
    return true;
}

void audio_capture_stop(void)
{
    s_capture = false;
    vTaskDelay(pdMS_TO_TICKS(20));   // 让正在进行的一块编码回调走完
    s_sink = NULL;
}

float audio_input_level(void) { return s_level; }

bool audio_play_push(const uint8_t *data, size_t len)
{
    if (!s_ok || len > ADPCM_BLOCK_BYTES) return false;
    ablock_t b;
    b.len = (uint16_t)len;
    memcpy(b.data, data, len);
    return xQueueSend(s_play, &b, pdMS_TO_TICKS(200)) == pdTRUE;
}

void audio_play_flush(void)
{
    if (s_play) xQueueReset(s_play);
}

bool audio_playing(void) { return s_playing || (s_play && uxQueueMessagesWaiting(s_play)); }
