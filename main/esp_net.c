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
// 配网页:附近网络列表(按信号排序、同名合并,点一下填入)+ 高级设置(云端密钥、位置、家居)。
static const char k_page_head[] =
    "<!doctype html><html lang=zh><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'><title>工具箱设置</title>"
    "<style>body{font-family:-apple-system,system-ui,sans-serif;background:#0b0e14;color:#f1f4f9;"
    "margin:0;padding:20px}h1{font-size:22px;margin:4px 0 12px}h2{font-size:16px;color:#a3adbd;margin:22px 0 4px}"
    "label{display:block;margin:12px 0 6px;color:#a3adbd;font-size:14px}"
    "input,select{width:100%;box-sizing:border-box;padding:12px;border-radius:10px;border:1px solid #2a3241;"
    "background:#151a23;color:#f1f4f9;font-size:16px}"
    ".ap{display:flex;justify-content:space-between;padding:12px 14px;margin:6px 0;border-radius:10px;"
    "background:#151a23;border:1px solid #2a3241}.ap:active,.ap.on{border-color:#ff6b57}.s{color:#667185}"
    "button{margin-top:22px;width:100%;padding:14px;border:0;border-radius:12px;background:#ff6b57;"
    "color:#0b0e14;font-size:17px;font-weight:600}.ghost{background:#1e2531;color:#f1f4f9;margin-top:8px}"
    "details{margin-top:18px}summary{color:#ff6b57;font-size:16px;padding:8px 0}"
    "p{color:#667185;font-size:13px;line-height:1.5}</style></head><body>"
    "<h1>工具箱设置</h1><form method=post action=/save id=f><h2>无线网络(点选或手动输入)</h2>";

static const char k_page_mid[] =
    "<a href=/rescan><button type=button class=ghost onclick=\"location='/rescan'\">重新扫描</button></a>"
    "<label>网络名称(隐藏网络可手动输入;只改高级设置就留空)</label><input name=ssid id=ssid>"
    "<label>密码</label><input name=pass type=password placeholder='留空表示无密码或不修改'>"
    "<details><summary>高级设置(不用家里的服务器也能用)</summary>"
    "<h2>语音助手 · 云端接口(兼容 OpenAI)</h2>"
    "<p>没有家里的服务器时,设备直接调用云端完成 识别 → 对话 → 朗读。默认是硅基流动"
    "(siliconflow.cn,注册后创建密钥),也可换成其他兼容接口。</p>";

static const char k_page_tail[] =
    "<h2>天气位置</h2><p>不填则按网络位置自动定位。</p>"
    "<h2>家庭助手(Home Assistant)</h2><p>长期访问令牌在 HA 个人资料页底部创建;实体用逗号分隔。</p>"
    "%s</details>"
    "<button>保存</button></form><p>保存后设备会自动连接,看设备屏幕上的结果。</p>"
    "<script>document.querySelectorAll('.ap').forEach(e=>e.onclick=()=>{"
    "document.querySelectorAll('.ap').forEach(x=>x.classList.remove('on'));e.classList.add('on');"
    "document.getElementById('ssid').value=e.dataset.s;document.querySelector('[name=pass]').focus()})</script>"
    "</body></html>";

// 门户提交结果(httpd 任务写,LVGL 任务取)
static portal_form_t s_form;

static void html_escape(char *out, size_t len, const char *in)
{
    size_t o = 0;
    for (; *in && o + 6 < len; in++) {
        const char *rep = *in == '<' ? "&lt;" : *in == '>' ? "&gt;" : *in == '&' ? "&amp;"
                        : *in == '\'' ? "&#39;" : *in == '"' ? "&quot;" : NULL;
        if (rep) {
            memcpy(out + o, rep, strlen(rep));
            o += strlen(rep);
        } else {
            out[o++] = *in;
        }
    }
    out[o] = '\0';
}

static void field(httpd_req_t *req, const char *label, const char *name, const char *value, bool secret)
{
    char esc[220], buf[420];
    html_escape(esc, sizeof(esc), secret ? "" : value);
    snprintf(buf, sizeof(buf), "<label>%s</label><input name=%s value=\"%s\"%s>", label, name, esc,
             secret ? (value[0] ? " type=password placeholder='已保存,留空不变'" : " type=password") : "");
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send_chunk(req, k_page_head, HTTPD_RESP_USE_STRLEN);

    // 附近网络:按信号排序,同名只留最强的
    static net_ap_t aps[NET_SCAN_MAX];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int n = s_ap_count;
    memcpy(aps, s_aps, sizeof(net_ap_t) * (size_t)n);
    xSemaphoreGive(s_lock);
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (aps[j].rssi > aps[i].rssi) {
                net_ap_t t = aps[i];
                aps[i] = aps[j];
                aps[j] = t;
            }
        }
    }
    int shown = 0;
    for (int i = 0; i < n; i++) {
        if (!aps[i].ssid[0]) continue;
        bool dup = false;
        for (int j = 0; j < i; j++) dup |= !strcmp(aps[j].ssid, aps[i].ssid);
        if (dup) continue;
        char esc[100], row[300];
        html_escape(esc, sizeof(esc), aps[i].ssid);
        const int bars = aps[i].rssi >= -55 ? 4 : aps[i].rssi >= -67 ? 3 : aps[i].rssi >= -78 ? 2 : 1;
        // 信号格:用实心/空心圆点表示
        snprintf(row, sizeof(row), "<div class=ap data-s=\"%s\"><span>%s</span><span class=s>%s%s</span></div>",
                 esc, esc, aps[i].secure ? "" : "开放 · ",
                 bars == 4 ? "●●●●" : bars == 3 ? "●●●○" : bars == 2 ? "●●○○" : "●○○○");
        httpd_resp_send_chunk(req, row, HTTPD_RESP_USE_STRLEN);
        shown++;
    }
    if (!shown) {
        httpd_resp_send_chunk(req, s_scan == SCAN_RUNNING ? "<p>正在扫描,稍后点 重新扫描。</p>"
                                                          : "<p>没有扫描到网络,点 重新扫描 或手动输入。</p>",
                              HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_chunk(req, k_page_mid, HTTPD_RESP_USE_STRLEN);
    field(req, "接口地址", "api_base", g_settings.api_base, false);
    field(req, "API 密钥", "api_key", g_settings.api_key, true);
    field(req, "对话模型", "llm_model", g_settings.llm_model, false);
    field(req, "识别模型", "asr_model", g_settings.asr_model, false);
    field(req, "朗读模型", "tts_model", g_settings.tts_model, false);
    field(req, "朗读声音", "tts_voice", g_settings.tts_voice, false);
    field(req, "家里的中转服务器(可选,留空自动查找)", "server", g_settings.server, false);
    // 天气位置与家居字段插在 tail 的 %s 位置
    // 只在打开网页时临时分配,平时不占常驻内存。
    const size_t tf_len = 1600, tail_len = sizeof(k_page_tail) + 1600;
    char *tail_fields = malloc(tf_len);
    char *tail = malloc(tail_len);
    if (!tail_fields || !tail) {
        free(tail_fields);
        free(tail);
        return httpd_resp_send_chunk(req, NULL, 0);
    }
    tail_fields[0] = '\0';
    char esc[220];
    const struct { const char *label, *name, *value; bool secret; } extra[] = {
        {"城市名(显示用)", "city", g_settings.city, false},
        {"纬度", "lat", g_settings.lat, false},
        {"经度", "lon", g_settings.lon, false},
        {"地址,例 http://192.168.1.5:8123", "ha_url", g_settings.ha_url, false},
        {"长期访问令牌", "ha_token", g_settings.ha_token, true},
        {"实体,例 light.living_room, switch.fan", "ha_ents", g_settings.ha_entities, false},
    };
    for (size_t i = 0; i < sizeof(extra) / sizeof(extra[0]); i++) {
        html_escape(esc, sizeof(esc), extra[i].secret ? "" : extra[i].value);
        char one[420];
        snprintf(one, sizeof(one), "<label>%s</label><input name=%s value=\"%s\"%s>", extra[i].label,
                 extra[i].name, esc,
                 extra[i].secret ? (extra[i].value[0] ? " type=password placeholder='已保存,留空不变'"
                                                       : " type=password") : "");
        strncat(tail_fields, one, tf_len - strlen(tail_fields) - 1);
    }
    snprintf(tail, tail_len, k_page_tail, tail_fields);
    httpd_resp_send_chunk(req, tail, HTTPD_RESP_USE_STRLEN);
    free(tail_fields);
    free(tail);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t page_rescan(httpd_req_t *req)
{
    s_scan = SCAN_RUNNING;
    esp_wifi_scan_start(NULL, true);   // 阻塞约 2 秒,在 httpd 任务里,不影响界面
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
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
    enum { BODY_MAX = 1800 };
    char *body = malloc(BODY_MAX);
    if (!body) return ESP_FAIL;
    const int n = req->content_len < BODY_MAX - 1 ? req->content_len : BODY_MAX - 1;
    int got = 0;
    while (got < n) {
        const int r = httpd_req_recv(req, body + got, n - got);
        if (r <= 0) {
            free(body);
            return ESP_FAIL;
        }
        got += r;
    }
    body[got] = '\0';
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_form, 0, sizeof(s_form));
    form_field(body, "ssid", s_form.ssid, sizeof(s_form.ssid));
    form_field(body, "pass", s_form.pass, sizeof(s_form.pass));
    form_field(body, "server", s_form.server, sizeof(s_form.server));
    form_field(body, "api_base", s_form.api_base, sizeof(s_form.api_base));
    form_field(body, "api_key", s_form.api_key, sizeof(s_form.api_key));
    form_field(body, "llm_model", s_form.llm_model, sizeof(s_form.llm_model));
    form_field(body, "asr_model", s_form.asr_model, sizeof(s_form.asr_model));
    form_field(body, "tts_model", s_form.tts_model, sizeof(s_form.tts_model));
    form_field(body, "tts_voice", s_form.tts_voice, sizeof(s_form.tts_voice));
    form_field(body, "city", s_form.city, sizeof(s_form.city));
    form_field(body, "lat", s_form.lat, sizeof(s_form.lat));
    form_field(body, "lon", s_form.lon, sizeof(s_form.lon));
    form_field(body, "ha_url", s_form.ha_url, sizeof(s_form.ha_url));
    form_field(body, "ha_token", s_form.ha_token, sizeof(s_form.ha_token));
    form_field(body, "ha_ents", s_form.ha_entities, sizeof(s_form.ha_entities));
    s_form.valid = true;
    xSemaphoreGive(s_lock);
    free(body);
    s_portal = PORTAL_SAVED;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req,
        "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
        "<body style='font-family:system-ui;background:#0b0e14;color:#f1f4f9;padding:24px'>"
        "<h2>已保存</h2><p>请看设备屏幕上的结果。需要再改可以返回上一页。</p></body>");
}

bool net_portal_take(portal_form_t *out)
{
    if (!s_form.valid) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_form;
    s_form.valid = false;
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
    cfg.max_uri_handlers = 4;
    cfg.stack_size = 8192;
    if (httpd_start(&s_httpd, &cfg) == ESP_OK) {
        const httpd_uri_t get = {.uri = "/", .method = HTTP_GET, .handler = page_get};
        const httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = page_save};
        httpd_register_uri_handler(s_httpd, &get);
        httpd_register_uri_handler(s_httpd, &save);
        const httpd_uri_t rescan = {.uri = "/rescan", .method = HTTP_GET, .handler = page_rescan};
        httpd_register_uri_handler(s_httpd, &rescan);
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
// 蓝牙启动前调用:放弃开机对时,没有应用占用时关掉 Wi-Fi 腾出内存。
void net_yield_radio(void)
{
    if (s_boot_sync) {
        s_boot_sync = false;
        post(CMD_UPDATE);
    }
}

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
