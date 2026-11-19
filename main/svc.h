// main/svc.h —— 后台服务接口。
//
// 固件实现在 svc_*.c(各自有 FreeRTOS 任务);桌面模拟器用 sim/fake_svc.c 给出假数据。
// 约定:这里的函数都可在 LVGL 任务里调用且不阻塞;结果通过轮询取得。
// 无线资源仲裁:Wi-Fi(含 ESP-NOW)与 BLE 不同时运行。BLE 启动时会等待 Wi-Fi
// 释放;Wi-Fi 被应用占用时 BLE 启动返回 false。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

// ===========================================================================
// 时间
// ===========================================================================
bool time_valid(void);
bool time_local(struct tm *out);     // 未同步时返回 false
void time_apply_tz(const char *tz);

// ===========================================================================
// Wi-Fi(STA 连接引用计数、扫描、配网门户)
// ===========================================================================
typedef enum {
    NET_OFF = 0,
    NET_NO_CONFIG,
    NET_CONNECTING,
    NET_CONNECTED,
    NET_FAILED,
} net_state_t;

void net_init(void);
void net_acquire(void);              // 引用计数 +1;有配置时自动连接
void net_release(void);              // 引用计数 -1;归零后关闭 Wi-Fi
net_state_t net_state(void);
bool net_radio_on(void);
const char *net_ip(void);
const char *net_ssid(void);
int net_rssi(void);
int net_channel(void);
void net_reconnect(void);            // 配置变化后重连

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    uint8_t secure;                  // 0 = 开放
    uint8_t bssid[6];
} net_ap_t;

#define NET_SCAN_MAX 24
typedef enum { SCAN_IDLE = 0, SCAN_RUNNING, SCAN_DONE, SCAN_FAILED } scan_state_t;
bool net_scan_start(void);           // 需要先 net_acquire
scan_state_t net_scan_state(void);
int net_scan_results(net_ap_t *out, int max);

typedef enum {
    PORTAL_OFF = 0,
    PORTAL_STARTING,
    PORTAL_WAITING,                  // 热点已开,等待手机连接
    PORTAL_CLIENT,                   // 手机已连上热点
    PORTAL_SAVED,                    // 已提交配置
    PORTAL_FAILED,
} portal_state_t;
bool net_portal_start(void);
void net_portal_stop(void);
portal_state_t net_portal_state(void);
const char *net_portal_ssid(void);   // 例 "Passport-A3F2"
// 手机在配置页提交的内容。空字段表示未填写(密钥类字段留空 = 不修改)。
typedef struct {
    bool valid;
    char ssid[33], pass[65], server[96];
    char api_base[80], api_key[96], llm_model[48], asr_model[48], tts_model[48], tts_voice[64];
    char city[24], lat[12], lon[12];
    char ha_url[64], ha_token[200], ha_entities[200];
} portal_form_t;
// 提交后返回 true 并拷出内容(只返回一次)。
bool net_portal_take(portal_form_t *out);

// ===========================================================================
// 中转服务器(Mac mini)
// ===========================================================================
typedef enum {
    SRV_UNKNOWN = 0,
    SRV_NO_NET,
    SRV_DISCOVERING,
    SRV_OK,
    SRV_UNREACHABLE,
} srv_state_t;

typedef enum { REQ_FREE = 0, REQ_BUSY, REQ_DONE, REQ_ERROR } req_state_t;

srv_state_t relay_state(void);
// 能发请求:服务器在线,或者没有服务器但已联网(此时天气/看板/家居由设备直连处理)。
bool relay_ready(void);
// 没有服务器、由设备直连在处理请求。
bool relay_local(void);
const char *relay_url(void);
void relay_discover(void);           // 局域网广播发现 + 配对取令牌
// 发起请求,返回句柄(>=0),失败 -1。body 为 NULL 时 GET,否则 POST JSON。
int relay_request(const char *path, const char *json_body);
// 轮询:完成后 *body 指向以 0 结尾的响应(句柄释放前有效)。
req_state_t relay_poll(int handle, const char **body, int *http_status);
void relay_free(int handle);

// ===========================================================================
// 音频:提示音、分析(频谱/分贝/音高)、节拍器、采集与播放(IMA ADPCM)
// ===========================================================================
typedef enum {
    BEEP_CLICK = 0,
    BEEP_OK,
    BEEP_ERR,
    BEEP_WORK_DONE,
    BEEP_BREAK_DONE,
    BEEP_MSG,
    BEEP_START,
    BEEP_STOP,
} beep_t;

void audio_svc_init(void);
bool audio_available(void);
void audio_beep(beep_t pattern);
void audio_apply_volume(void);

#define AUDIO_BANDS 20
typedef struct {
    float bands[AUDIO_BANDS];        // 0..1 对数频段能量
    float db;                        // 近似声压 dB(未校准,相对值)
    float peak_db;
    float pitch_hz;                  // 0 = 无明确音高
    float clarity;                   // 0..1 音高置信度
} audio_analysis_t;
bool audio_analyze_start(void);
void audio_analyze_pitch(bool on);   // 只有调音器需要:音高检测较费 CPU
void audio_analyze_stop(void);
bool audio_analysis_get(audio_analysis_t *out);

void audio_metronome_set(int bpm, int beats_per_bar);
void audio_metronome_run(bool on);
uint32_t audio_metronome_ticks(void);   // 已响的拍数(用于界面同步闪烁)

// 采集:编码后的 ADPCM 块回调(在音频任务里调用,不能碰 LVGL)。
typedef void (*audio_sink_t)(const uint8_t *block, size_t len, void *user);
bool audio_capture_start(audio_sink_t sink, void *user);
void audio_capture_stop(void);
float audio_input_level(void);          // 0..1,界面电平
// 播放:推入 ADPCM 块(任意任务),音频任务解码播放。
bool audio_play_push(const uint8_t *block, size_t len);
void audio_play_flush(void);            // 丢弃队列
bool audio_playing(void);

// ===========================================================================
// 蓝牙 HID(键盘 + 媒体键)
// ===========================================================================
typedef enum {
    BLE_OFF = 0,
    BLE_STARTING,
    BLE_ADVERTISING,
    BLE_CONNECTED,
    BLE_FAILED,
} ble_state_t;

bool ble_hid_start(void);
void ble_hid_stop(void);
ble_state_t ble_hid_state(void);
const char *ble_hid_peer(void);
void ble_hid_key(uint8_t keycode, uint8_t modifiers);   // 按下 + 抬起
void ble_hid_consumer(uint16_t usage);                  // 按下 + 抬起
// 逐字键入 ASCII 文本(非 ASCII 字符跳过;中文需要电脑输入法,HID 无法直接发送)。
void ble_hid_type(const char *ascii);
void ble_hid_forget(void);                              // 清除配对

// ===========================================================================
// 设备互联(ESP-NOW)
// ===========================================================================
typedef struct {
    uint8_t mac[6];
    char name[24];
    int8_t rssi;
    int64_t seen_ms;
} link_peer_t;

typedef enum {
    LINK_EV_NONE = 0,
    LINK_EV_PAIR_REQUEST,     // 有人请求配对(peer 有效)
    LINK_EV_PAIRED,
    LINK_EV_TEXT,             // text 有效
    LINK_EV_PING,
    LINK_EV_FOCUS,            // value = 分钟
    LINK_EV_VOICE_BEGIN,
    LINK_EV_VOICE_END,
} link_ev_type_t;

typedef struct {
    link_ev_type_t type;
    link_peer_t peer;
    char text[64];
    int value;
} link_event_t;

bool link_start(void);
void link_stop(void);
bool link_running(void);
int link_nearby(link_peer_t *out, int max);
bool link_partner(link_peer_t *out);       // 已配对设备
void link_pair(const uint8_t mac[6], bool accept);
void link_unpair(void);
bool link_poll(link_event_t *ev);
bool link_send_text(const char *text);
bool link_send_ping(void);
bool link_send_focus(int minutes);
bool link_talk_begin(void);
void link_talk_end(void);
bool link_partner_online(void);

// ===========================================================================
// AI 助手(经中转服务器)
// ===========================================================================
typedef enum {
    AI_IDLE = 0,
    AI_LISTENING,
    AI_THINKING,
    AI_SPEAKING,
    AI_ERROR,
} ai_state_t;

bool ai_talk_begin(void);
void ai_talk_end(void);
void ai_cancel(void);
ai_state_t ai_state(void);
const char *ai_error(void);
// 新一轮问答完成时返回 true 并拷出文本。
bool ai_take_result(char *question, size_t qlen, char *answer, size_t alen);
void ai_replay(void);
