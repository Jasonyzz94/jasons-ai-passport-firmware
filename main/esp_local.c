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
#include "weather_shape.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
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
int local_http(const char *url, const char *auth, const char *body, char *out, int cap, int timeout_ms)
{
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

static int weather(char *out, int cap)
{
    char *buf = malloc(FETCH_MAX);
    if (!buf) return 503;
    const bool manual = g_settings.lat[0] && g_settings.lon[0];
    if (!manual && !geolocate(buf, FETCH_MAX)) {
        free(buf);
        return 502;
    }
    char url[400];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s&timezone=auto"
             "&forecast_days=4&forecast_hours=7"
             "&current=temperature_2m,relative_humidity_2m,weather_code,is_day"
             "&hourly=temperature_2m,weather_code"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min",
             manual ? g_settings.lat : s_geo_lat, manual ? g_settings.lon : s_geo_lon);
    const int st = local_http(url, NULL, NULL, buf, FETCH_MAX, 10000);
    const char *city = g_settings.city[0] ? g_settings.city : s_geo_city;
    const bool ok = st == 200 && weather_shape(buf, city, out, (size_t)cap);
    free(buf);
    return ok ? 200 : 502;
}

// ---------------------------------------------------------------------------
// 看板(本机卡片)
// ---------------------------------------------------------------------------
static int board(char *out, int cap)
{
    static const char *const wd[7] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
    struct tm tm;
    const bool dated = time_local(&tm);
    char today[160] = "", newyear[160] = "";
    if (dated) {
        snprintf(today, sizeof(today),
                 "{\"title\":\"今天\",\"value\":\"%d\",\"unit\":\"日\",\"sub\":\"%d月 · %s · 今年第 %d 天\","
                 "\"color\":\"60A5FA\",\"icon\":\"check\"},",
                 tm.tm_mday, tm.tm_mon + 1, wd[tm.tm_wday], tm.tm_yday + 1);
        const int year_days = ((tm.tm_year + 1900) % 4 == 0 && ((tm.tm_year + 1900) % 100 || (tm.tm_year + 1900) % 400 == 0)) ? 366 : 365;
        snprintf(newyear, sizeof(newyear),
                 "{\"title\":\"距离元旦\",\"value\":\"%d\",\"unit\":\"天\",\"sub\":\"%d 年已过去 %d%%\","
                 "\"color\":\"FBBF24\",\"icon\":\"star\"},",
                 year_days - tm.tm_yday, tm.tm_year + 1900, tm.tm_yday * 100 / year_days);
    }
    const int64_t up = sys_now_ms() / 60000;
    snprintf(out, (size_t)cap,
             "{\"updated\":\"%02d:%02d\",\"cards\":[%s%s"
             "{\"title\":\"今日专注\",\"value\":\"%d\",\"unit\":\"个番茄\",\"sub\":\"打开番茄钟开始下一个\","
             "\"color\":\"FF6B57\",\"icon\":\"heart\"},"
             "{\"title\":\"本机\",\"value\":\"%d\",\"unit\":\"%s\",\"sub\":\"空闲内存 %u KB · 未连接家里的服务器\","
             "\"color\":\"A78BFA\",\"icon\":\"home\"}]}",
             dated ? tm.tm_hour : 0, dated ? tm.tm_min : 0, today, newyear, pomo_today_done(),
             (int)(up >= 60 ? up / 60 : up), up >= 60 ? "小时运行" : "分钟运行",
             (unsigned)(sys_free_heap() / 1024));
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
