// main/esp_local.c —— 没有中转服务器时,由设备自己处理应用的请求(在 relay 任务里运行)。
//
//   /api/weather      按 IP 自动定位(或用配置的经纬度)→ Open-Meteo(HTTPS)→ 与服务器同格式
//   /api/board        本机生成卡片:今天、距离元旦、今日专注、本机状态
//   /api/home         直连局域网 Home Assistant(配网页"高级设置"里填地址、令牌、实体)
//   /api/home/toggle
// 一次只做一个 HTTPS 请求;响应缓冲按需分配、用完释放,尽量少占常驻内存。
#include "esp_internal.h"
#include "settings.h"
#include "svc.h"
#include "sys.h"
#include "ics_parse.h"
#include "weather_shape.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "local";

int pomo_today_done(void);

#define FETCH_MAX 6144

static char s_geo_city[24];
static char s_geo_lat[12];
static char s_geo_lon[12];

typedef struct {
    char *buf;
    int len;
    int cap;
} sink_t;

static esp_err_t on_data(esp_http_client_event_t *e)
{
    sink_t *s = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && s) {
        const int n = e->data_len < s->cap - 1 - s->len ? e->data_len : s->cap - 1 - s->len;
        if (n > 0) {
            memcpy(s->buf + s->len, e->data, (size_t)n);
            s->len += n;
            s->buf[s->len] = '\0';
        }
    }
    return ESP_OK;
}

// 通用 HTTP(S) 请求;auth 可为 NULL。返回状态码,失败 -1。
// HTTPS 握手要临时占用二三十 KB 内存;界面(LVGL)和它共用同一个堆,内存被挤光时界面会卡死。
// 所以内存不够时宁可这次不取,也不冒险。
bool lv_mem_reserve_ok(void);

bool local_tls_ok(void)
{
    if (!lv_mem_reserve_ok()) {   // 界面已经在用应急内存:先别联网抢内存
        ESP_LOGW(TAG, "界面内存紧张,跳过加密请求");
        return false;
    }
    const size_t free_now = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (free_now >= 48 * 1024 && largest >= 18 * 1024) return true;
    ESP_LOGW(TAG, "内存不足,跳过加密请求(空闲 %u,最大块 %u)", (unsigned)free_now, (unsigned)largest);
    return false;
}

int local_http(const char *url, const char *auth, const char *body, char *out, int cap, int timeout_ms)
{
    if (out && cap) out[0] = '\0';
    if (strncmp(url, "https", 5) == 0 && !local_tls_ok()) return -1;
    sink_t sink = {.buf = out, .len = 0, .cap = cap};
    if (out && cap) out[0] = '\0';
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = timeout_ms,
        .event_handler = on_data,
        .user_data = out ? &sink : NULL,
        .method = body ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = strncmp(url, "https", 5) == 0 ? esp_crt_bundle_attach : NULL,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    if (auth) esp_http_client_set_header(c, "Authorization", auth);
    if (body) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
        esp_http_client_set_post_field(c, body, (int)strlen(body));
    }
    const esp_err_t err = esp_http_client_perform(c);
    const int status = err == ESP_OK ? esp_http_client_get_status_code(c) : -1;
    esp_http_client_cleanup(c);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%.40s 失败: %s(空闲内存 %u)", url, esp_err_to_name(err),
                 (unsigned)sys_free_heap());
    }
    return status;
}

// 天气数据不敏感:先走明文 HTTP(不需要 TLS,省内存也快);不通时再试 HTTPS。
static int local_http_plain_first(const char *http_url, char *out, int cap, int timeout_ms)
{
    const int st = local_http(http_url, NULL, NULL, out, cap, timeout_ms);
    if (st == 200 || strncmp(http_url, "http://", 7) != 0) return st;
    char url[420];
    snprintf(url, sizeof(url), "https://%s", http_url + 7);
    return local_http(url, NULL, NULL, out, cap, timeout_ms);
}

// ---------------------------------------------------------------------------
// 天气
// ---------------------------------------------------------------------------
static bool geolocate(char *buf, int cap)
{
    if (s_geo_lat[0]) return true;
    // ip-api 是 HTTP(不用 TLS,省内存);lang=zh-CN 直接给中文城市名。
    if (local_http("http://ip-api.com/json/?lang=zh-CN&fields=status,city,lat,lon", NULL, NULL,
                   buf, cap, 6000) != 200) {
        return false;
    }
    cJSON *j = cJSON_Parse(buf);
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(j, "status");
    const cJSON *city = cJSON_GetObjectItemCaseSensitive(j, "city");
    const cJSON *lat = cJSON_GetObjectItemCaseSensitive(j, "lat");
    const cJSON *lon = cJSON_GetObjectItemCaseSensitive(j, "lon");
    const bool ok = cJSON_IsString(st) && !strcmp(st->valuestring, "success") && cJSON_IsNumber(lat)
                 && cJSON_IsNumber(lon);
    if (ok) {
        snprintf(s_geo_city, sizeof(s_geo_city), "%s", cJSON_IsString(city) ? city->valuestring : "");
        snprintf(s_geo_lat, sizeof(s_geo_lat), "%.3f", lat->valuedouble);
        snprintf(s_geo_lon, sizeof(s_geo_lon), "%.3f", lon->valuedouble);
        ESP_LOGI(TAG, "自动定位:%s", s_geo_city);
    }
    cJSON_Delete(j);
    return ok;
}

// 只填了城市名(没有经纬度):用 Open-Meteo 地名搜索查经纬度,结果按城市名缓存。
static char s_geo_for_city[24];

static void url_encode(char *out, size_t len, const char *in)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < len; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-') {
            out[o++] = (char)*p;
        } else {
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 15];
        }
    }
    out[o] = '\0';
}

static bool geocode_city(char *buf, int cap)
{
    if (s_geo_lat[0] && !strcmp(s_geo_for_city, g_settings.city)) return true;
    char q[80], url[200];
    url_encode(q, sizeof(q), g_settings.city);
    snprintf(url, sizeof(url),
             "http://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=zh&format=json", q);
    if (local_http_plain_first(url, buf, cap, 8000) != 200) return false;
    cJSON *j = cJSON_Parse(buf);
    const cJSON *r = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j, "results"), 0);
    const cJSON *lat = cJSON_GetObjectItemCaseSensitive(r, "latitude");
    const cJSON *lon = cJSON_GetObjectItemCaseSensitive(r, "longitude");
    const bool ok = cJSON_IsNumber(lat) && cJSON_IsNumber(lon);
    if (ok) {
        snprintf(s_geo_lat, sizeof(s_geo_lat), "%.3f", lat->valuedouble);
        snprintf(s_geo_lon, sizeof(s_geo_lon), "%.3f", lon->valuedouble);
        snprintf(s_geo_city, sizeof(s_geo_city), "%s", g_settings.city);
        snprintf(s_geo_for_city, sizeof(s_geo_for_city), "%s", g_settings.city);
        ESP_LOGI(TAG, "城市 %s → %s,%s", g_settings.city, s_geo_lat, s_geo_lon);
    } else {
        ESP_LOGW(TAG, "找不到城市 %s,改用网络定位", g_settings.city);
    }
    cJSON_Delete(j);
    return ok;
}

static int weather(char *out, int cap)
{
    char *buf = malloc(FETCH_MAX);
    if (!buf) return 503;
    // 位置优先级:手填经纬度 > 城市名查经纬度 > 按网络 IP 自动定位
    const bool manual = g_settings.lat[0] && g_settings.lon[0];
    bool located = manual;
    if (!located && g_settings.city[0]) located = geocode_city(buf, FETCH_MAX);
    if (!located && strcmp(s_geo_for_city, "") != 0) {
        s_geo_lat[0] = '\0';   // 城市查不到时清掉旧结果,重新按 IP 定位
        s_geo_for_city[0] = '\0';
    }
    if (!located && !geolocate(buf, FETCH_MAX)) {
        free(buf);
        return 502;
    }
    char url[400];
    snprintf(url, sizeof(url),
             "http://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s&timezone=auto"
             "&forecast_days=4&forecast_hours=7"
             "&current=temperature_2m,relative_humidity_2m,weather_code,is_day"
             "&hourly=temperature_2m,weather_code"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min",
             manual ? g_settings.lat : s_geo_lat, manual ? g_settings.lon : s_geo_lon);
    const int st = local_http_plain_first(url, buf, FETCH_MAX, 10000);
    const char *city = g_settings.city[0] ? g_settings.city : s_geo_city;
    const bool ok = st == 200 && weather_shape(buf, city, out, (size_t)cap);
    free(buf);
    return ok ? 200 : 502;
}

// ---------------------------------------------------------------------------
// 看板(本机卡片)
// ---------------------------------------------------------------------------
// 汇率(open.er-api.com,免密钥):1 美元 / 1 欧元 兑人民币,缓存 10 分钟。
static char s_fx_card[200];
static int64_t s_fx_at = -600000;

static void fetch_fx(void)
{
    if (sys_now_ms() - s_fx_at < 600000) return;
    s_fx_at = sys_now_ms();
    char *buf = malloc(FETCH_MAX);
    if (!buf) return;
    if (local_http_plain_first("http://open.er-api.com/v6/latest/USD", buf, FETCH_MAX, 8000) == 200) {
        cJSON *j = cJSON_Parse(buf);
        const cJSON *rates = cJSON_GetObjectItemCaseSensitive(j, "rates");
        const cJSON *cny = cJSON_GetObjectItemCaseSensitive(rates, "CNY");
        const cJSON *eur = cJSON_GetObjectItemCaseSensitive(rates, "EUR");
        if (cJSON_IsNumber(cny) && cJSON_IsNumber(eur) && eur->valuedouble > 0) {
            snprintf(s_fx_card, sizeof(s_fx_card),
                     "{\"title\":\"美元汇率\",\"value\":\"%.2f\",\"unit\":\"人民币\","
                     "\"sub\":\"1 欧元 = %.2f 人民币\",\"color\":\"4ADE80\",\"icon\":\"coin\"},",
                     cny->valuedouble, cny->valuedouble / eur->valuedouble);
        }
        cJSON_Delete(j);
    }
    free(buf);
}

// 日历:流式读取订阅文件(不整个放进内存),找接下来 7 天最近的 3 个日程。缓存 10 分钟。
static char s_cal_card[320];
static int64_t s_cal_at = -600000;

static int tz_offset_minutes(void)
{
    const time_t t = time(NULL);
    struct tm lt, gt;
    localtime_r(&t, &lt);
    gmtime_r(&t, &gt);
    return (int)(ics_minutes(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min)
               - ics_minutes(gt.tm_year + 1900, gt.tm_mon + 1, gt.tm_mday, gt.tm_hour, gt.tm_min));
}

static void fetch_calendar(void)
{
    if (!g_settings.ics_url[0] || sys_now_ms() - s_cal_at < 600000) return;
    struct tm tm;
    if (!time_local(&tm)) return;
    s_cal_at = sys_now_ms();
    char url[210];
    if (!strncmp(g_settings.ics_url, "webcal://", 9)) snprintf(url, sizeof(url), "https://%s", g_settings.ics_url + 9);
    else snprintf(url, sizeof(url), "%s", g_settings.ics_url);

    if (strncmp(url, "https", 5) == 0 && !local_tls_ok()) {
        s_cal_at = sys_now_ms() - 540000;   // 1 分钟后再试
        return;
    }
    static ics_state_t st;
    const int64_t now = ics_minutes(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    ics_init(&st, now, tz_offset_minutes(), 7);
    esp_http_client_config_t cfg = {
        .url = url, .timeout_ms = 10000, .buffer_size = 1536, .buffer_size_tx = 1024,
        .crt_bundle_attach = strncmp(url, "https", 5) == 0 ? esp_crt_bundle_attach : NULL,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return;
    bool ok = false;
    if (esp_http_client_open(c, 0) == ESP_OK && esp_http_client_fetch_headers(c) >= 0
        && esp_http_client_get_status_code(c) == 200) {
        char chunk[512], line[256];
        int ll = 0, r;
        while ((r = esp_http_client_read(c, chunk, sizeof(chunk))) > 0) {
            for (int i = 0; i < r; i++) {
                const char ch = chunk[i];
                if (ch == '\n') {
                    if (ll && line[ll - 1] == '\r') ll--;
                    line[ll] = '\0';
                    ics_line(&st, line);
                    ll = 0;
                } else if (ll < (int)sizeof(line) - 1) {
                    line[ll++] = ch;
                }
            }
        }
        ok = true;
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (!ok) {
        snprintf(s_cal_card, sizeof(s_cal_card),
                 "{\"title\":\"日程\",\"value\":\"--\",\"unit\":\"\",\"sub\":\"日历读取失败,检查订阅地址\","
                 "\"color\":\"FBBF24\",\"icon\":\"check\"},");
        return;
    }
    if (!st.count) {
        snprintf(s_cal_card, sizeof(s_cal_card),
                 "{\"title\":\"日程\",\"value\":\"0\",\"unit\":\"项\",\"sub\":\"接下来 7 天没有安排\","
                 "\"color\":\"FBBF24\",\"icon\":\"check\"},");
        return;
    }
    const ics_event_t *e = &st.events[0];
    const int64_t day_diff = e->start / 1440 - now / 1440;
    char when[24], unit[16], sub[120];
    if (e->all_day) snprintf(when, sizeof(when), "全天");
    else snprintf(when, sizeof(when), "%02d:%02d", (int)(e->start % 1440 / 60), (int)(e->start % 60));
    snprintf(unit, sizeof(unit), "%s", day_diff == 0 ? "今天" : day_diff == 1 ? "明天" : "之后");
    if (st.count > 1) snprintf(sub, sizeof(sub), "%s · 之后还有 %d 项", e->summary, st.count - 1);
    else snprintf(sub, sizeof(sub), "%s", e->summary);
    // 标题来自日历,可能含引号:做最基本的转义
    char esc[160];
    size_t o = 0;
    for (const char *p = sub; *p && o + 2 < sizeof(esc); p++) {
        if (*p == '"' || *p == '\\') esc[o++] = '\\';
        esc[o++] = *p;
    }
    esc[o] = '\0';
    snprintf(s_cal_card, sizeof(s_cal_card),
             "{\"title\":\"下一个日程\",\"value\":\"%s\",\"unit\":\"%s\",\"sub\":\"%s\","
             "\"color\":\"FBBF24\",\"icon\":\"check\"},", when, unit, esc);
}

static int board(char *out, int cap)
{
    static const char *const wd[7] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
    fetch_fx();
    fetch_calendar();
    struct tm tm;
    const bool dated = time_local(&tm);
    char today[160] = "", newyear[160] = "";
    if (dated) {
        snprintf(today, sizeof(today),
                 "{\"title\":\"今天\",\"value\":\"%d\",\"unit\":\"日\",\"sub\":\"%d月 · %s · 今年第 %d 天\","
                 "\"color\":\"60A5FA\",\"icon\":\"check\"},",
                 tm.tm_mday, tm.tm_mon + 1, wd[tm.tm_wday], tm.tm_yday + 1);
        const int year = tm.tm_year + 1900;
        const int year_days = (year % 4 == 0 && (year % 100 || year % 400 == 0)) ? 366 : 365;
        snprintf(newyear, sizeof(newyear),
                 "{\"title\":\"距离元旦\",\"value\":\"%d\",\"unit\":\"天\",\"sub\":\"%d 年已过去 %d%%\","
                 "\"color\":\"FBBF24\",\"icon\":\"star\"},",
                 year_days - tm.tm_yday, year, tm.tm_yday * 100 / year_days);
    }
    snprintf(out, (size_t)cap,
             "{\"updated\":\"%02d:%02d\",\"cards\":[%s%s%s%s"
             "{\"title\":\"今日专注\",\"value\":\"%d\",\"unit\":\"个番茄\",\"sub\":\"打开番茄钟开始下一个\","
             "\"color\":\"FF6B57\",\"icon\":\"heart\"}]}",
             dated ? tm.tm_hour : 0, dated ? tm.tm_min : 0, today, s_cal_card, s_fx_card, newyear,
             pomo_today_done());
    return 200;
}

// ---------------------------------------------------------------------------
// Home Assistant 直连
// ---------------------------------------------------------------------------
static bool ha_configured(void)
{
    return g_settings.ha_url[0] && g_settings.ha_token[0] && g_settings.ha_entities[0];
}

static void ha_auth(char *auth, size_t len) { snprintf(auth, len, "Bearer %s", g_settings.ha_token); }

static int home_list(char *out, int cap)
{
    if (!ha_configured()) {
        snprintf(out, (size_t)cap, "{\"source\":\"未配置\",\"items\":[]}");
        return 200;
    }
    char *buf = malloc(FETCH_MAX);
    if (!buf) return 503;
    char auth[220];
    ha_auth(auth, sizeof(auth));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "source", "家庭助手");
    cJSON *items = cJSON_AddArrayToObject(root, "items");
    char ents[200];
    snprintf(ents, sizeof(ents), "%s", g_settings.ha_entities);
    char *save = NULL;
    for (char *e = strtok_r(ents, ", ", &save); e; e = strtok_r(NULL, ", ", &save)) {
        char url[160];
        snprintf(url, sizeof(url), "%s/api/states/%s", g_settings.ha_url, e);
        if (local_http(url, auth, NULL, buf, FETCH_MAX, 5000) != 200) continue;
        cJSON *st = cJSON_Parse(buf);
        if (!st) continue;
        const cJSON *attrs = cJSON_GetObjectItemCaseSensitive(st, "attributes");
        const cJSON *fname = cJSON_GetObjectItemCaseSensitive(attrs, "friendly_name");
        const cJSON *state = cJSON_GetObjectItemCaseSensitive(st, "state");
        const cJSON *unit = cJSON_GetObjectItemCaseSensitive(attrs, "unit_of_measurement");
        const char *raw = cJSON_IsString(state) ? state->valuestring : "";
        const bool is_light = !strncmp(e, "light.", 6);
        const bool is_switch = !strncmp(e, "switch.", 7) || !strncmp(e, "fan.", 4)
                            || !strncmp(e, "input_boolean.", 14);
        const bool is_scene = !strncmp(e, "scene.", 6) || !strncmp(e, "script.", 7);
        char shown[24];
        if (is_light || is_switch) snprintf(shown, sizeof(shown), "%s", !strcmp(raw, "on") ? "开" : "关");
        else if (is_scene) snprintf(shown, sizeof(shown), "执行");
        else snprintf(shown, sizeof(shown), "%s%s", raw, cJSON_IsString(unit) ? unit->valuestring : "");
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "id", e);
        cJSON_AddStringToObject(it, "name", cJSON_IsString(fname) ? fname->valuestring : e);
        cJSON_AddStringToObject(it, "type", is_light ? "light" : is_switch ? "switch" : is_scene ? "scene" : "sensor");
        cJSON_AddBoolToObject(it, "on", !strcmp(raw, "on"));
        cJSON_AddStringToObject(it, "state", shown);
        cJSON_AddItemToArray(items, it);
        cJSON_Delete(st);
    }
    free(buf);
    const bool ok = cJSON_PrintPreallocated(root, out, cap, false);
    cJSON_Delete(root);
    return ok ? 200 : 500;
}

static int home_toggle(const char *body)
{
    if (!ha_configured() || !body) return 404;
    cJSON *j = cJSON_Parse(body);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(j, "id");
    int st = 400;
    if (cJSON_IsString(id) && strstr(g_settings.ha_entities, id->valuestring)) {
        const char *e = id->valuestring;
        char domain[24];
        snprintf(domain, sizeof(domain), "%.*s", (int)(strchr(e, '.') ? strchr(e, '.') - e : 0), e);
        const bool run = !strcmp(domain, "scene") || !strcmp(domain, "script");
        char url[160], payload[96], auth[220];
        snprintf(url, sizeof(url), "%s/api/services/%s/%s", g_settings.ha_url, domain, run ? "turn_on" : "toggle");
        snprintf(payload, sizeof(payload), "{\"entity_id\":\"%s\"}", e);
        ha_auth(auth, sizeof(auth));
        st = local_http(url, auth, payload, NULL, 0, 6000) == 200 ? 200 : 502;
    }
    cJSON_Delete(j);
    return st;
}

// ---------------------------------------------------------------------------
int local_request(const char *path, const char *body, char *out, int cap)
{
    if (!strcmp(path, "/api/weather")) return weather(out, cap);
    if (!strcmp(path, "/api/board")) return board(out, cap);
    if (!strcmp(path, "/api/home")) return home_list(out, cap);
    if (!strcmp(path, "/api/home/toggle")) return home_toggle(body);
    return 404;
}
