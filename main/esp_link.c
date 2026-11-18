// main/esp_link.c —— 设备互联:ESP-NOW(Wi-Fi 射频点对点,不经过路由器)。
//
// 信道:两台设备必须在同一信道。连着路由器时用路由器的信道,没连时固定用 1 信道。
//      所以要么两台都连同一个家里的 Wi-Fi,要么两台都不配网——混用会互相收不到。
// 配对:附近的设备每秒广播 HELLO;一方发 PAIR_REQ,另一方确认后回 PAIR_ACK,双方
//      把对方 MAC 存入 NVS。只接受已配对设备发来的消息(配对类报文除外)。
// 对讲:按住时把麦克风编码成 ADPCM,每块一个包(约 62 包/秒,8 KB/s)。
#include "audio_dsp.h"
#include "link_proto.h"
#include "settings.h"
#include "svc.h"
#include "sys.h"

#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "link";

#define NEARBY_MAX 8
#define ONLINE_MS 4000
#define FALLBACK_CHANNEL 1

typedef struct {
    uint8_t mac[6];
    int8_t rssi;
    size_t len;
    uint8_t data[LINK_PACKET_MAX];
} rx_t;

typedef struct {
    uint8_t mac[6];
    char name[24];
    bool valid;
} partner_t;

static const uint8_t k_bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static QueueHandle_t s_rx;
static QueueHandle_t s_events;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static volatile bool s_running;
static volatile bool s_stop;
static bool s_now_inited;
static partner_t s_partner;
static bool s_partner_loaded;
static link_peer_t s_nearby[NEARBY_MAX];
static int s_nearby_n;
static int64_t s_partner_seen;
static int8_t s_partner_rssi;
static uint8_t s_pending_req[6];
static bool s_have_pending;
static uint8_t s_requested[6];
static bool s_have_requested;
static uint16_t s_seq;
static volatile bool s_talking;
static int64_t s_voice_rx_ms;

// ---------------------------------------------------------------------------
static void partner_load(void)
{
    if (s_partner_loaded) return;
    s_partner_loaded = true;
    if (!sys_kv_get_blob("l_peer", &s_partner, sizeof(s_partner))) memset(&s_partner, 0, sizeof(s_partner));
}

static void partner_save(const uint8_t mac[6], const char *name)
{
    memcpy(s_partner.mac, mac, 6);
    snprintf(s_partner.name, sizeof(s_partner.name), "%s", name);
    s_partner.valid = true;
    sys_kv_set_blob("l_peer", &s_partner, sizeof(s_partner));
}

static void emit(link_ev_type_t type, const uint8_t mac[6], const char *name, const char *text, int value)
{
    link_event_t ev = {0};
    ev.type = type;
    if (mac) memcpy(ev.peer.mac, mac, 6);
    snprintf(ev.peer.name, sizeof(ev.peer.name), "%s", name ? name : s_partner.name);
    if (text) snprintf(ev.text, sizeof(ev.text), "%s", text);
    ev.value = value;
    if (s_events) xQueueSend(s_events, &ev, 0);
}

static bool send_to(const uint8_t mac[6], link_type_t type, const void *payload, size_t len)
{
    uint8_t buf[LINK_PACKET_MAX];
    const size_t n = link_pack(type, s_seq++, payload, len, buf);
    if (!n || !s_now_inited) return false;
    if (!esp_now_is_peer_exist(mac)) {
        esp_now_peer_info_t p = {0};
        memcpy(p.peer_addr, mac, 6);
        p.ifidx = WIFI_IF_STA;
        p.channel = 0;   // 跟随当前信道
        esp_now_add_peer(&p);
    }
    return esp_now_send(mac, buf, n) == ESP_OK;
}

static bool send_partner(link_type_t type, const void *payload, size_t len)
{
    if (!s_partner.valid || !s_running) return false;
    return send_to(s_partner.mac, type, payload, len);
}

// ---------------------------------------------------------------------------
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!s_rx || len <= 0 || len > LINK_PACKET_MAX) return;
    rx_t r;
    memcpy(r.mac, info->src_addr, 6);
    r.rssi = info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : 0;
    r.len = (size_t)len;
    memcpy(r.data, data, (size_t)len);
    xQueueSend(s_rx, &r, 0);   // Wi-Fi 任务里不阻塞;满了就丢
}

static void nearby_update(const uint8_t mac[6], const char *name, int8_t rssi)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int slot = -1, oldest = 0;
    for (int i = 0; i < s_nearby_n; i++) {
        if (!memcmp(s_nearby[i].mac, mac, 6)) slot = i;
        if (s_nearby[i].seen_ms < s_nearby[oldest].seen_ms) oldest = i;
    }
    if (slot < 0) slot = s_nearby_n < NEARBY_MAX ? s_nearby_n++ : oldest;
    memcpy(s_nearby[slot].mac, mac, 6);
    snprintf(s_nearby[slot].name, sizeof(s_nearby[slot].name), "%s", name);
    s_nearby[slot].rssi = rssi;
    s_nearby[slot].seen_ms = sys_now_ms();
    xSemaphoreGive(s_lock);
}

static void handle(const rx_t *r)
{
    link_msg_t m;
    if (!link_parse(r->data, r->len, &m)) return;
    const bool from_partner = s_partner.valid && !memcmp(r->mac, s_partner.mac, 6);
    const char *name = (const char *)m.payload;

    switch (m.type) {
    case LINK_HELLO:
        nearby_update(r->mac, name, r->rssi);
        if (from_partner) {
            s_partner_seen = sys_now_ms();
            s_partner_rssi = r->rssi;
        }
        return;
    case LINK_PAIR_REQ:
        if (s_have_requested && !memcmp(s_requested, r->mac, 6)) {
            // 双方同时请求:直接视为同意
            partner_save(r->mac, name);
            send_to(r->mac, LINK_PAIR_ACK, g_settings.name, strlen(g_settings.name));
            emit(LINK_EV_PAIRED, r->mac, name, NULL, 0);
            s_have_requested = false;
            return;
        }
        memcpy(s_pending_req, r->mac, 6);
        s_have_pending = true;
        emit(LINK_EV_PAIR_REQUEST, r->mac, name, NULL, 0);
        return;
    case LINK_PAIR_ACK:
        if (s_have_requested && !memcmp(s_requested, r->mac, 6)) {
            partner_save(r->mac, name);
            s_have_requested = false;
            emit(LINK_EV_PAIRED, r->mac, name, NULL, 0);
        }
        return;
    default:
        break;
    }
    if (!from_partner) return;   // 其余报文只接受已配对设备
    s_partner_seen = sys_now_ms();
    switch (m.type) {
    case LINK_TEXT: emit(LINK_EV_TEXT, r->mac, NULL, name, 0); break;
    case LINK_PING: emit(LINK_EV_PING, r->mac, NULL, NULL, 0); break;
    case LINK_FOCUS: emit(LINK_EV_FOCUS, r->mac, NULL, NULL, m.payload[0]); break;
    case LINK_VOICE:
        if (!s_voice_rx_ms) emit(LINK_EV_VOICE_BEGIN, r->mac, NULL, NULL, 0);
        s_voice_rx_ms = sys_now_ms();
        if (!s_talking) audio_play_push(m.payload, m.len);
        break;
    case LINK_VOICE_END:
        if (s_voice_rx_ms) emit(LINK_EV_VOICE_END, r->mac, NULL, NULL, 0);
        s_voice_rx_ms = 0;
        break;
    case LINK_UNPAIR:
        memset(&s_partner, 0, sizeof(s_partner));
        sys_kv_set_blob("l_peer", &s_partner, sizeof(s_partner));
        break;
    default: break;
    }
}

static bool now_up(void)
{
    if (s_now_inited) return true;
    if (!net_radio_on()) return false;
    if (net_state() != NET_CONNECTED && net_state() != NET_CONNECTING) {
        esp_wifi_set_channel(FALLBACK_CHANNEL, WIFI_SECOND_CHAN_NONE);
    }
    if (esp_now_init() != ESP_OK) return false;
    esp_now_register_recv_cb(on_recv);
    esp_now_peer_info_t p = {0};
    memcpy(p.peer_addr, k_bcast, 6);
    p.ifidx = WIFI_IF_STA;
    esp_now_add_peer(&p);
    s_now_inited = true;
    ESP_LOGI(TAG, "ESP-NOW 已启动,信道 %d", net_channel());
    return true;
}

static void link_task(void *arg)
{
    (void)arg;
    int64_t next_hello = 0;
    while (!s_stop) {
        rx_t r;
        if (xQueueReceive(s_rx, &r, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (s_now_inited) handle(&r);
            continue;
        }
        if (!now_up()) continue;
        const int64_t now = sys_now_ms();
        if (now >= next_hello) {
            next_hello = now + 1000;
            send_to(k_bcast, LINK_HELLO, g_settings.name,
                    link_utf8_clip(g_settings.name, LINK_NAME_MAX));
        }
        if (s_voice_rx_ms && now - s_voice_rx_ms > 600) {   // 对方断续/松手但 END 丢了
            s_voice_rx_ms = 0;
            emit(LINK_EV_VOICE_END, s_partner.mac, NULL, NULL, 0);
        }
    }
    if (s_now_inited) {
        esp_now_unregister_recv_cb();
        esp_now_deinit();
        s_now_inited = false;
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
bool link_start(void)
{
    partner_load();
    if (s_running) return true;
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        s_rx = xQueueCreate(24, sizeof(rx_t));
        s_events = xQueueCreate(8, sizeof(link_event_t));
    }
    net_acquire();
    s_stop = false;
    s_running = true;
    s_nearby_n = 0;
    if (xTaskCreate(link_task, "link", 4096, NULL, 5, &s_task) != pdPASS) {
        s_running = false;
        net_release();
        return false;
    }
    return true;
}

void link_stop(void)
{
    if (!s_running) return;
    link_talk_end();
    s_stop = true;
    for (int i = 0; i < 40 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(10));
    s_running = false;
    net_release();
}

bool link_running(void) { return s_running; }

int link_nearby(link_peer_t *out, int max)
{
    if (!s_lock) return 0;
    const int64_t now = sys_now_ms();
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_nearby_n && n < max; i++) {
        if (now - s_nearby[i].seen_ms > 5000) continue;
        if (s_partner.valid && !memcmp(s_nearby[i].mac, s_partner.mac, 6)) continue;
        out[n++] = s_nearby[i];
    }
    xSemaphoreGive(s_lock);
    return n;
}

bool link_partner(link_peer_t *out)
{
    partner_load();
    if (!s_partner.valid) return false;
    memset(out, 0, sizeof(*out));
    memcpy(out->mac, s_partner.mac, 6);
    snprintf(out->name, sizeof(out->name), "%s", s_partner.name);
    out->rssi = s_partner_rssi;
    out->seen_ms = s_partner_seen;
    return true;
}

bool link_partner_online(void)
{
    return s_partner.valid && s_running && sys_now_ms() - s_partner_seen < ONLINE_MS;
}

void link_pair(const uint8_t mac[6], bool accept)
{
    if (!accept) {
        s_have_pending = false;
        return;
    }
    if (s_have_pending && !memcmp(s_pending_req, mac, 6)) {
        // 同意对方的请求:从附近列表找名字
        char name[24] = "Passport";
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < s_nearby_n; i++) {
            if (!memcmp(s_nearby[i].mac, mac, 6)) snprintf(name, sizeof(name), "%s", s_nearby[i].name);
        }
        xSemaphoreGive(s_lock);
        partner_save(mac, name);
        send_to(mac, LINK_PAIR_ACK, g_settings.name, link_utf8_clip(g_settings.name, LINK_NAME_MAX));
        s_have_pending = false;
        emit(LINK_EV_PAIRED, mac, name, NULL, 0);
        return;
    }
    memcpy(s_requested, mac, 6);
    s_have_requested = true;
    send_to(mac, LINK_PAIR_REQ, g_settings.name, link_utf8_clip(g_settings.name, LINK_NAME_MAX));
}

void link_unpair(void)
{
    send_partner(LINK_UNPAIR, NULL, 0);
    memset(&s_partner, 0, sizeof(s_partner));
    sys_kv_set_blob("l_peer", &s_partner, sizeof(s_partner));
}

bool link_poll(link_event_t *ev)
{
    return s_events && xQueueReceive(s_events, ev, 0) == pdTRUE;
}

bool link_send_text(const char *text)
{
    return send_partner(LINK_TEXT, text, link_utf8_clip(text, LINK_TEXT_MAX));
}

bool link_send_ping(void) { return send_partner(LINK_PING, NULL, 0); }

bool link_send_focus(int minutes)
{
    const uint8_t m = (uint8_t)(minutes < 1 ? 1 : (minutes > 255 ? 255 : minutes));
    return send_partner(LINK_FOCUS, &m, 1);
}

static void voice_sink(const uint8_t *block, size_t len, void *user)
{
    (void)user;
    send_partner(LINK_VOICE, block, len);
}

bool link_talk_begin(void)
{
    if (!link_partner_online()) return false;
    s_talking = true;
    audio_play_flush();
    if (!audio_capture_start(voice_sink, NULL)) {
        s_talking = false;
        return false;
    }
    return true;
}

void link_talk_end(void)
{
    if (!s_talking) return;
    audio_capture_stop();
    s_talking = false;
    for (int i = 0; i < 3; i++) send_partner(LINK_VOICE_END, NULL, 0);   // 多发几次防丢
}
