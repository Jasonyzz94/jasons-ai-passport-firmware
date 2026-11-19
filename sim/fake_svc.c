// sim/fake_svc.c —— svc.h 的假实现,只为桌面截图提供逼真数据。
#include "sim.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

sim_fake_t g_fake = {
    .time_ok = true,
    .tm = { .tm_year = 126, .tm_mon = 10, .tm_mday = 19, .tm_hour = 9, .tm_min = 41,
            .tm_sec = 27, .tm_wday = 4 },
    .net = NET_CONNECTED,
    .srv = SRV_OK,
    .scan = SCAN_DONE,
    .portal = PORTAL_WAITING,
    .ble = BLE_CONNECTED,
    .link_on = true,
    .link_paired = true,
    .link_nearby = 2,
    .ai = AI_IDLE,
    .level = 0.4f,
    .pitch = 440.0f,
    .audio_ok = true,
};

// ---- 时间 ----
bool time_valid(void) { return g_fake.time_ok; }
bool time_local(struct tm *out)
{
    if (!g_fake.time_ok) return false;
    *out = g_fake.tm;
    const int64_t s = g_sim_now_ms / 1000;
    out->tm_sec = (int)((g_fake.tm.tm_sec + s) % 60);
    return true;
}
void time_apply_tz(const char *tz) { (void)tz; }

// ---- Wi-Fi ----
void net_init(void) {}
void net_acquire(void) {}
void net_release(void) {}
net_state_t net_state(void) { return g_fake.net; }
bool net_radio_on(void) { return g_fake.net != NET_OFF; }
const char *net_ip(void) { return "192.168.31.86"; }
const char *net_ssid(void) { return "Jason-Home-5G"; }
int net_rssi(void) { return -52; }
int net_channel(void) { return 6; }
void net_reconnect(void) {}

static const net_ap_t k_aps[] = {
    {"Jason-Home", -41, 6, 1, {0}}, {"ChinaNet-7Kq2", -58, 1, 1, {0}},
    {"MiShare-2F", -63, 11, 1, {0}}, {"Starbucks", -70, 6, 0, {0}},
    {"TP-LINK_9A1C", -74, 1, 1, {0}}, {"Office-Guest", -79, 11, 0, {0}},
    {"HUAWEI-B5E2", -81, 3, 1, {0}}, {"DIRECT-HP", -86, 6, 1, {0}},
};
bool net_scan_start(void) { return true; }
scan_state_t net_scan_state(void) { return g_fake.scan; }
int net_scan_results(net_ap_t *out, int max)
{
    int n = (int)(sizeof(k_aps) / sizeof(k_aps[0]));
    if (n > max) n = max;
    memcpy(out, k_aps, sizeof(net_ap_t) * (size_t)n);
    return n;
}
bool net_portal_start(void) { return true; }
void net_portal_stop(void) {}
portal_state_t net_portal_state(void) { return g_fake.portal; }
const char *net_portal_ssid(void) { return "Passport-A3F2"; }
bool net_portal_take(portal_form_t *f) { (void)f; return false; }

// ---- 中转服务器 ----
srv_state_t relay_state(void) { return g_fake.srv; }
bool relay_ready(void) { return g_fake.net == NET_CONNECTED; }
bool relay_local(void) { return g_fake.srv != SRV_OK; }
const char *relay_url(void) { return "http://192.168.31.20:8787"; }
void relay_discover(void) {}

static const char *k_weather =
    "{\"city\":\"上海\",\"now\":{\"temp\":18,\"code\":\"partly\",\"text\":\"多云\","
    "\"humidity\":62,\"wind\":\"东北风 2级\",\"feels\":17,\"aqi\":42,\"aqi_text\":\"优\"},"
    "\"today\":{\"hi\":21,\"lo\":14},"
    "\"hourly\":[{\"h\":\"10时\",\"t\":18,\"code\":\"partly\"},{\"h\":\"11时\",\"t\":19,\"code\":\"sun\"},"
    "{\"h\":\"12时\",\"t\":20,\"code\":\"sun\"},{\"h\":\"13时\",\"t\":21,\"code\":\"partly\"},"
    "{\"h\":\"14时\",\"t\":21,\"code\":\"cloud\"}],"
    "\"daily\":[{\"day\":\"今天\",\"code\":\"partly\",\"hi\":21,\"lo\":14},"
    "{\"day\":\"周五\",\"code\":\"rain\",\"hi\":17,\"lo\":12},"
    "{\"day\":\"周六\",\"code\":\"cloud\",\"hi\":16,\"lo\":11},"
    "{\"day\":\"周日\",\"code\":\"sun\",\"hi\":19,\"lo\":10}],\"updated\":\"09:30\"}";

static const char *k_board =
    "{\"updated\":\"09:40\",\"cards\":["
    "{\"title\":\"GitHub 通知\",\"value\":\"3\",\"unit\":\"条未读\",\"sub\":\"PR #12 待你审阅\",\"color\":\"38BDF8\",\"icon\":\"bell\"},"
    "{\"title\":\"美元汇率\",\"value\":\"7.12\",\"unit\":\"CNY\",\"sub\":\"较昨日 -0.03\",\"color\":\"4ADE80\",\"icon\":\"coin\"},"
    "{\"title\":\"今日日程\",\"value\":\"2\",\"unit\":\"项\",\"sub\":\"14:00 产品评审\",\"color\":\"FBBF24\",\"icon\":\"check\"},"
    "{\"title\":\"Mac mini\",\"value\":\"23%\",\"unit\":\"CPU\",\"sub\":\"内存 11.2/16 GB\",\"color\":\"A78BFA\",\"icon\":\"star\"}]}";

static const char *k_home =
    "{\"source\":\"Home Assistant\",\"items\":["
    "{\"id\":\"light.living\",\"name\":\"客厅灯\",\"type\":\"light\",\"on\":true,\"state\":\"开\"},"
    "{\"id\":\"light.bed\",\"name\":\"卧室灯\",\"type\":\"light\",\"on\":false,\"state\":\"关\"},"
    "{\"id\":\"switch.fan\",\"name\":\"风扇插座\",\"type\":\"switch\",\"on\":true,\"state\":\"开\"},"
    "{\"id\":\"scene.home\",\"name\":\"回家模式\",\"type\":\"scene\",\"state\":\"执行\"},"
    "{\"id\":\"sensor.temp\",\"name\":\"室内温度\",\"type\":\"sensor\",\"state\":\"23.5℃\"}]}";

static int s_handle_kind[4];
int relay_request(const char *path, const char *json_body)
{
    (void)json_body;
    for (int i = 0; i < 4; i++) {
        if (!s_handle_kind[i]) {
            s_handle_kind[i] = strstr(path, "weather") ? 1 : strstr(path, "board") ? 2
                             : strstr(path, "home") ? 3 : 4;
            return i;
        }
    }
    return -1;
}
req_state_t relay_poll(int h, const char **body, int *status)
{
    if (h < 0 || h >= 4 || !s_handle_kind[h]) return REQ_FREE;
    if (g_fake.relay_fail) {
        *status = 0;
        *body = "";
        return REQ_ERROR;
    }
    *status = 200;
    switch (s_handle_kind[h]) {
    case 1: *body = k_weather; break;
    case 2: *body = k_board; break;
    case 3: *body = k_home; break;
    default: *body = "{\"ok\":true}"; break;
    }
    return REQ_DONE;
}
void relay_free(int h)
{
    if (h >= 0 && h < 4) s_handle_kind[h] = 0;
}

// ---- 音频 ----
void audio_svc_init(void) {}
bool audio_available(void) { return g_fake.audio_ok; }
void audio_beep(beep_t p) { (void)p; }
void audio_apply_volume(void) {}
bool audio_analyze_start(void) { return true; }
void audio_analyze_stop(void) {}
void audio_analyze_pitch(bool on) { (void)on; }
bool audio_analysis_get(audio_analysis_t *out)
{
    for (int i = 0; i < AUDIO_BANDS; i++) {
        const float x = (float)i / AUDIO_BANDS;
        out->bands[i] = 0.25f + 0.6f * expf(-8.0f * (x - 0.25f) * (x - 0.25f))
                      + 0.15f * sinf(i * 1.7f) * sinf(i * 0.9f);
        if (out->bands[i] < 0.03f) out->bands[i] = 0.03f;
        if (out->bands[i] > 1) out->bands[i] = 1;
    }
    out->db = 58.4f;
    out->peak_db = 71.2f;
    out->pitch_hz = g_fake.pitch;
    out->clarity = 0.92f;
    return true;
}
static int s_bpm = 96;
void audio_metronome_set(int bpm, int beats) { s_bpm = bpm; (void)beats; }
void audio_metronome_run(bool on) { (void)on; }
uint32_t audio_metronome_ticks(void) { return (uint32_t)(g_sim_now_ms * s_bpm / 60000); }
bool audio_capture_start(audio_sink_t sink, void *user) { (void)sink; (void)user; return true; }
void audio_capture_stop(void) {}
float audio_input_level(void) { return g_fake.level; }
bool audio_play_push(const uint8_t *b, size_t l) { (void)b; (void)l; return true; }
void audio_play_flush(void) {}
bool audio_playing(void) { return false; }

// ---- BLE ----
bool ble_hid_start(void) { return true; }
void ble_hid_stop(void) {}
ble_state_t ble_hid_state(void) { return g_fake.ble; }
const char *ble_hid_peer(void) { return "Jason 的 MacBook"; }
void ble_hid_key(uint8_t k, uint8_t m) { (void)k; (void)m; }
void ble_hid_consumer(uint16_t u) { (void)u; }
void ble_hid_type(const char *t) { (void)t; }
void ble_hid_hold(uint8_t m, bool d) { (void)m; (void)d; }
bool ble_hid_has_fn(void) { return true; }
void ble_hid_forget(void) {}

// ---- 互联 ----
static link_event_t s_ev;
static bool s_ev_pending;
void sim_fake_link_event(link_ev_type_t type, const char *text, int value)
{
    memset(&s_ev, 0, sizeof(s_ev));
    s_ev.type = type;
    snprintf(s_ev.peer.name, sizeof(s_ev.peer.name), "Passport-7C10");
    if (text) snprintf(s_ev.text, sizeof(s_ev.text), "%s", text);
    s_ev.value = value;
    s_ev_pending = true;
}
bool link_start(void) { return true; }
void link_stop(void) {}
bool link_running(void) { return g_fake.link_on; }
int link_nearby(link_peer_t *out, int max)
{
    const char *names[] = {"Passport-7C10", "Passport-19B4"};
    int n = g_fake.link_nearby < max ? g_fake.link_nearby : max;
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        snprintf(out[i].name, sizeof(out[i].name), "%s", names[i % 2]);
        out[i].rssi = (int8_t)(-48 - i * 17);
        out[i].mac[5] = (uint8_t)i;
    }
    return n;
}
bool link_partner(link_peer_t *out)
{
    if (!g_fake.link_paired) return false;
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "Passport-7C10");
    out->rssi = -48;
    return true;
}
void link_pair(const uint8_t mac[6], bool accept) { (void)mac; (void)accept; }
void link_unpair(void) {}
bool link_poll(link_event_t *ev)
{
    if (!s_ev_pending) return false;
    *ev = s_ev;
    s_ev_pending = false;
    return true;
}
bool link_send_text(const char *t) { (void)t; return true; }
bool link_send_ping(void) { return true; }
bool link_send_focus(int m) { (void)m; return true; }
bool link_talk_begin(void) { return true; }
void link_talk_end(void) {}
bool link_partner_online(void) { return g_fake.link_paired; }

// ---- AI ----
bool ai_talk_begin(void) { g_fake.ai = AI_LISTENING; return true; }
void ai_talk_end(void) { g_fake.ai = AI_THINKING; }
void ai_cancel(void) { g_fake.ai = AI_IDLE; }
ai_state_t ai_state(void) { return g_fake.ai; }
const char *ai_error(void) { return "服务器无响应"; }
bool ai_take_result(char *q, size_t ql, char *a, size_t al)
{
    if (!g_fake.ai_has_result) return false;
    g_fake.ai_has_result = false;
    snprintf(q, ql, "明天上海需要带伞吗？");
    snprintf(a, al, "需要。明天上海有小雨，气温 12 到 17 度，傍晚雨势稍大，"
                    "出门记得带伞，再加一件薄外套。");
    return true;
}
void ai_replay(void) {}
