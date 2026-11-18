// main/esp_net.c —— Wi-Fi 服务:STA 引用计数连接、扫描、SoftAP 配网门户、SNTP 对时。
//
// 所有会阻塞或耗时的 esp_wifi_* 调用都在 net 任务里执行;LVGL 任务只发命令、读状态。
// 开机若已配网,会先连一次 Wi-Fi 对时(最多 60 秒),对时成功且没有应用占用就关掉射频省电。
#include "settings.h"
#include "svc.h"
#include "sys.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static const char *TAG = "net";

typedef enum {
    CMD_UPDATE = 0,       // 引用计数变化
    CMD_RECONNECT,
    CMD_SCAN,
    CMD_PORTAL_START,
    CMD_PORTAL_STOP,
} net_cmd_t;

#define BOOT_SYNC_MS 60000
#define RETRY_FAST 5

static QueueHandle_t s_cmd;
static SemaphoreHandle_t s_lock;
static esp_netif_t *s_sta;
static esp_netif_t *s_ap;
static httpd_handle_t s_httpd;

static volatile int s_refs;
static volatile bool s_boot_sync;
static volatile bool s_wifi_started;
static volatile bool s_portal_on;
static volatile net_state_t s_state = NET_OFF;
static volatile scan_state_t s_scan = SCAN_IDLE;
static volatile portal_state_t s_portal = PORTAL_OFF;
static volatile int s_retries;
static bool s_sntp_started;

static char s_ip[16];
static char s_ssid[33];
static char s_portal_ssid[33];
static net_ap_t s_aps[NET_SCAN_MAX];
static int s_ap_count;

// 门户提交结果(httpd 任务写,LVGL 任务取)
static bool s_portal_pending;
static char s_p_ssid[33], s_p_pass[65], s_p_server[96], s_p_tz[48];

static void post(net_cmd_t c)
{
    if (s_cmd) xQueueSend(s_cmd, &c, 0);
}

// ---------------------------------------------------------------------------
// 时间
// ---------------------------------------------------------------------------
bool time_valid(void)
{
    time_t now = time(NULL);
    return now > 1735689600;   // 2025-01-01 之后才算对过时
}

bool time_local(struct tm *out)
{
    if (!time_valid()) return false;
    time_t now = time(NULL);
    localtime_r(&now, out);
    return true;
}

void time_apply_tz(const char *tz)
{
    setenv("TZ", tz && tz[0] ? tz : "CST-8", 1);
    tzset();
}

static void sntp_begin(void)
{
    if (s_sntp_started) {
        esp_netif_sntp_start();
        return;
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        3, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "cn.pool.ntp.org", "pool.ntp.org"));
    if (esp_netif_sntp_init(&cfg) == ESP_OK) s_sntp_started = true;
}

// ---------------------------------------------------------------------------
// 事件
// ---------------------------------------------------------------------------
static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_DISCONNECTED:
            s_ip[0] = '\0';
            if (s_state == NET_CONNECTED || s_state == NET_CONNECTING) {
                s_state = ++s_retries > RETRY_FAST ? NET_FAILED : NET_CONNECTING;
                post(CMD_RECONNECT);
            }
            break;
        case WIFI_EVENT_SCAN_DONE: {
            uint16_t n = NET_SCAN_MAX;
            static wifi_ap_record_t recs[NET_SCAN_MAX];
            if (esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_ap_count = n;
                for (int i = 0; i < n; i++) {
                    snprintf(s_aps[i].ssid, sizeof(s_aps[i].ssid), "%s", (const char *)recs[i].ssid);
                    s_aps[i].rssi = recs[i].rssi;
                    s_aps[i].channel = recs[i].primary;
                    s_aps[i].secure = recs[i].authmode != WIFI_AUTH_OPEN;
                    memcpy(s_aps[i].bssid, recs[i].bssid, 6);
                }
                xSemaphoreGive(s_lock);
                s_scan = SCAN_DONE;
            } else {
                s_scan = SCAN_FAILED;
            }
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED:
            if (s_portal == PORTAL_WAITING) s_portal = PORTAL_CLIENT;
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_retries = 0;
        s_state = NET_CONNECTED;
        ESP_LOGI(TAG, "已连接 %s,IP %s", s_ssid, s_ip);
        sntp_begin();
        relay_discover();
    }
}

// ---------------------------------------------------------------------------
// 射频启停
// ---------------------------------------------------------------------------
static void wifi_start(wifi_mode_t mode)
{
    if (!s_wifi_started) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        if (esp_wifi_init(&cfg) != ESP_OK) {
            s_state = NET_FAILED;
            return;
        }
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        esp_wifi_set_mode(mode);
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        if (esp_wifi_start() != ESP_OK) {
            esp_wifi_deinit();
            s_state = NET_FAILED;
            return;
        }
        s_wifi_started = true;
    } else {
        esp_wifi_set_mode(mode);
    }
}

static void wifi_stop(void)
{
    if (!s_wifi_started) return;
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    s_wifi_started = false;
    s_state = NET_OFF;
    s_ip[0] = '\0';
    s_scan = SCAN_IDLE;
}

static void sta_connect(void)
{
    if (!settings_has_wifi()) {
        s_state = NET_NO_CONFIG;
        return;
    }
    wifi_config_t wc = {0};
    // ssid/password 字段不要求结尾 0:按字段长度拷贝。
    strncpy((char *)wc.sta.ssid, g_settings.wifi_ssid, sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, g_settings.wifi_pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = g_settings.wifi_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    snprintf(s_ssid, sizeof(s_ssid), "%s", g_settings.wifi_ssid);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    s_state = NET_CONNECTING;
    esp_wifi_connect();
}

static bool wanted(void) { return s_refs > 0 || s_boot_sync || s_portal_on; }

static void apply(void)
{
    if (!wanted()) {
        wifi_stop();
        return;
    }
    wifi_start(s_portal_on ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (!s_wifi_started) return;
    if (s_state == NET_OFF || s_state == NET_NO_CONFIG) sta_connect();
}

// ---------------------------------------------------------------------------
// 配网门户(SoftAP + 一个网页表单)
// ---------------------------------------------------------------------------
static const char k_page_head[] =
    "<!doctype html><html lang=zh><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'><title>工具箱配网</title>"
    "<style>body{font-family:-apple-system,system-ui,sans-serif;background:#0b0e14;color:#f1f4f9;"
    "margin:0;padding:24px}h1{font-size:22px}label{display:block;margin:16px 0 6px;color:#a3adbd}"
    "input,select{width:100%;box-sizing:border-box;padding:12px;border-radius:10px;border:1px solid #2a3241;"
    "background:#151a23;color:#f1f4f9;font-size:16px}button{margin-top:24px;width:100%;padding:14px;"
    "border:0;border-radius:12px;background:#ff6b57;color:#0b0e14;font-size:17px;font-weight:600}"
    "p{color:#667185;font-size:13px}</style></head><body><h1>连接家里的无线网络</h1>"
    "<form method=post action=/save><label>无线网络名称</label><input name=ssid list=aps required>"
    "<datalist id=aps>";
static const char k_page_tail[] =
    "</datalist><label>密码</label><input name=pass type=password>"
    "<label>中转服务器地址(可选)</label><input name=server placeholder='留空则自动在局域网查找'>"
    "<label>时区</label><select name=tz><option value='CST-8'>北京时间</option>"
    "<option value='JST-9'>东京</option><option value='SGT-8'>新加坡</option>"
    "<option value='GMT0BST,M3.5.0/1,M10.5.0'>伦敦</option>"
    "<option value='PST8PDT,M3.2.0,M11.1.0'>美国西部</option>"
    "<option value='EST5EDT,M3.2.0,M11.1.0'>美国东部</option></select>"
    "<button>保存并连接</button></form><p>提交后设备会自动连接,热点随之关闭。</p></body></html>";

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send_chunk(req, k_page_head, HTTPD_RESP_USE_STRLEN);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_ap_count; i++) {
        if (!s_aps[i].ssid[0]) continue;
        char opt[64];
        // SSID 里的引号会破坏属性,直接跳过这种少见的名字。
        if (strchr(s_aps[i].ssid, '\'') || strchr(s_aps[i].ssid, '<')) continue;
        snprintf(opt, sizeof(opt), "<option value='%s'>", s_aps[i].ssid);
        httpd_resp_send_chunk(req, opt, HTTPD_RESP_USE_STRLEN);
    }
    xSemaphoreGive(s_lock);
    httpd_resp_send_chunk(req, k_page_tail, HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(req, NULL, 0);
}

// application/x-www-form-urlencoded 解码
static void url_decode(char *dst, size_t len, const char *src, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < len; i++) {
        if (src[i] == '+') {
            dst[o++] = ' ';
        } else if (src[i] == '%' && i + 2 < n) {
            char hex[3] = {src[i + 1], src[i + 2], 0};
            dst[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            dst[o++] = src[i];
        }
    }
    dst[o] = '\0';
}

static void form_field(const char *body, const char *key, char *out, size_t len)
{
    out[0] = '\0';
    const size_t kl = strlen(key);
    for (const char *p = body; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1;
            const char *end = strchr(v, '&');
            url_decode(out, len, v, end ? (size_t)(end - v) : strlen(v));
            return;
        }
    }
}

static esp_err_t page_save(httpd_req_t *req)
{
    char body[512];
    const int n = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int got = 0;
    while (got < n) {
        const int r = httpd_req_recv(req, body + got, n - got);
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    body[got] = '\0';
    xSemaphoreTake(s_lock, portMAX_DELAY);
    form_field(body, "ssid", s_p_ssid, sizeof(s_p_ssid));
    form_field(body, "pass", s_p_pass, sizeof(s_p_pass));
    form_field(body, "server", s_p_server, sizeof(s_p_server));
    form_field(body, "tz", s_p_tz, sizeof(s_p_tz));
    s_portal_pending = s_p_ssid[0] != '\0';
    xSemaphoreGive(s_lock);
    if (s_portal_pending) s_portal = PORTAL_SAVED;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req,
        "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
        "<body style='font-family:system-ui;background:#0b0e14;color:#f1f4f9;padding:24px'>"
        "<h2>已保存</h2><p>请看设备屏幕上的连接结果。</p></body>");
}

bool net_portal_take(char ssid[33], char pass[65], char server[96], char tz[48])
{
    if (!s_portal_pending) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(ssid, s_p_ssid, 33);
    memcpy(pass, s_p_pass, 65);
    memcpy(server, s_p_server, 96);
    memcpy(tz, s_p_tz, 48);
    s_portal_pending = false;
    xSemaphoreGive(s_lock);
    return true;
}

static void portal_start(void)
{
    s_portal_on = true;
    s_portal = PORTAL_STARTING;
    snprintf(s_portal_ssid, sizeof(s_portal_ssid), "Passport-%s", sys_short_id());
    wifi_start(WIFI_MODE_APSTA);
    if (!s_wifi_started) {
        s_portal = PORTAL_FAILED;
        return;
    }
    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, s_portal_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = (uint8_t)strlen(s_portal_ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    if (esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK) {
        s_portal = PORTAL_FAILED;
        return;
    }
    esp_wifi_scan_start(NULL, false);   // 给表单提供附近网络列表
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 4;
    if (httpd_start(&s_httpd, &cfg) == ESP_OK) {
        const httpd_uri_t get = {.uri = "/", .method = HTTP_GET, .handler = page_get};
        const httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = page_save};
        httpd_register_uri_handler(s_httpd, &get);
        httpd_register_uri_handler(s_httpd, &save);
        s_portal = PORTAL_WAITING;
        ESP_LOGI(TAG, "配网热点 %s 已开启", s_portal_ssid);
    } else {
        s_portal = PORTAL_FAILED;
    }
}

static void portal_stop(void)
{
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    s_portal_on = false;
    s_portal = PORTAL_OFF;
    if (s_wifi_started) esp_wifi_set_mode(WIFI_MODE_STA);
}

// ---------------------------------------------------------------------------
// net 任务
// ---------------------------------------------------------------------------
static void net_task(void *arg)
{
    (void)arg;
    int64_t boot_sync_until = s_boot_sync ? sys_now_ms() + BOOT_SYNC_MS : 0;
    int64_t retry_at = 0;
    if (s_boot_sync) apply();
    for (;;) {
        net_cmd_t cmd;
        if (xQueueReceive(s_cmd, &cmd, pdMS_TO_TICKS(1000)) == pdTRUE) {
            switch (cmd) {
            case CMD_UPDATE: apply(); break;
            case CMD_RECONNECT:
                if (s_wifi_started) {
                    if (s_state == NET_FAILED) {
                        retry_at = sys_now_ms() + 30000;   // 连不上:30 秒后再试
                    } else {
                        esp_wifi_disconnect();
                        vTaskDelay(pdMS_TO_TICKS(s_retries ? 1000 * s_retries : 100));
                        if (s_state != NET_NO_CONFIG) s_state = NET_OFF;
                        apply();
                    }
                } else {
                    s_retries = 0;
                    apply();
                }
                break;
            case CMD_SCAN:
                if (s_wifi_started) {
                    // 正在连接时扫描会被拒绝:先停下连接尝试再扫。
                    if (s_state == NET_CONNECTING) esp_wifi_disconnect();
                    if (esp_wifi_scan_start(NULL, false) != ESP_OK) s_scan = SCAN_FAILED;
                } else {
                    s_scan = SCAN_FAILED;
                }
                break;
            case CMD_PORTAL_START: portal_start(); break;
            case CMD_PORTAL_STOP: portal_stop(); apply(); break;
            }
        }
        const int64_t now = sys_now_ms();
        if (retry_at && now >= retry_at) {
            retry_at = 0;
            s_retries = 0;
            if (s_wifi_started) {
                s_state = NET_OFF;
                apply();
            }
        }
        if (s_boot_sync && (time_valid() || now >= boot_sync_until)) {
            s_boot_sync = false;
            ESP_LOGI(TAG, "开机对时%s", time_valid() ? "完成" : "超时");
            apply();
        }
    }
}

void net_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_cmd = xQueueCreate(8, sizeof(net_cmd_t));
    esp_netif_init();
    esp_event_loop_create_default();
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL, NULL);
    s_boot_sync = settings_has_wifi();
    s_state = settings_has_wifi() ? NET_OFF : NET_NO_CONFIG;
    xTaskCreate(net_task, "net", 4096, NULL, 5, NULL);
}

// ---------------------------------------------------------------------------
// 对外接口(LVGL 任务调用)
// ---------------------------------------------------------------------------
void net_acquire(void)
{
    s_refs++;
    post(CMD_UPDATE);
}

void net_release(void)
{
    if (s_refs > 0) s_refs--;
    post(CMD_UPDATE);
}

net_state_t net_state(void)
{
    if (!s_wifi_started) return settings_has_wifi() ? NET_OFF : NET_NO_CONFIG;
    return s_state;
}

bool net_radio_on(void) { return s_wifi_started; }
const char *net_ip(void) { return s_ip[0] ? s_ip : "-"; }
const char *net_ssid(void) { return s_ssid; }

int net_rssi(void)
{
    wifi_ap_record_t ap;
    return s_state == NET_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

int net_channel(void)
{
    uint8_t ch = 0;
    wifi_second_chan_t sec;
    if (s_wifi_started && esp_wifi_get_channel(&ch, &sec) == ESP_OK) return ch;
    return 0;
}

void net_reconnect(void)
{
    s_retries = 0;
    if (s_wifi_started) {
        s_state = NET_OFF;
        esp_wifi_disconnect();
    }
    post(CMD_UPDATE);
}

bool net_scan_start(void)
{
    if (s_scan == SCAN_RUNNING) return true;
    s_scan = SCAN_RUNNING;
    post(CMD_SCAN);
    return true;
}

scan_state_t net_scan_state(void) { return s_scan; }

int net_scan_results(net_ap_t *out, int max)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int n = s_ap_count < max ? s_ap_count : max;
    memcpy(out, s_aps, sizeof(net_ap_t) * (size_t)n);
    xSemaphoreGive(s_lock);
    if (s_scan == SCAN_DONE) s_scan = SCAN_IDLE;
    return n;
}

bool net_portal_start(void)
{
    s_portal = PORTAL_STARTING;
    post(CMD_PORTAL_START);
    return true;
}

void net_portal_stop(void) { post(CMD_PORTAL_STOP); }
portal_state_t net_portal_state(void) { return s_portal; }

const char *net_portal_ssid(void)
{
    if (!s_portal_ssid[0]) snprintf(s_portal_ssid, sizeof(s_portal_ssid), "Passport-%s", sys_short_id());
    return s_portal_ssid;
}

// 供 esp_link.c / esp_relay.c 使用
esp_netif_t *net_sta_netif(void) { return s_sta; }
