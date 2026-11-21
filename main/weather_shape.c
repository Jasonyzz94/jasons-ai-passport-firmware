// main/weather_shape.c —— Open-Meteo → 设备天气 JSON。
#include "weather_shape.h"

#include "cJSON.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    int code;
    const char *icon;
    const char *text;
} wmo_t;

static const wmo_t k_wmo[] = {
    {0, "sun", "晴"}, {1, "partly", "大部晴朗"}, {2, "partly", "多云"}, {3, "cloud", "阴"},
    {45, "fog", "雾"}, {48, "fog", "雾凇"}, {51, "rain", "毛毛雨"}, {53, "rain", "毛毛雨"},
    {55, "rain", "毛毛雨"}, {56, "rain", "冻毛毛雨"}, {57, "rain", "冻毛毛雨"}, {61, "rain", "小雨"},
    {63, "rain", "中雨"}, {65, "rain", "大雨"}, {66, "rain", "冻雨"}, {67, "rain", "冻雨"},
    {71, "snow", "小雪"}, {73, "snow", "中雪"}, {75, "snow", "大雪"}, {77, "snow", "雪粒"},
    {80, "rain", "阵雨"}, {81, "rain", "阵雨"}, {82, "rain", "强阵雨"}, {85, "snow", "阵雪"},
    {86, "snow", "强阵雪"}, {95, "storm", "雷阵雨"}, {96, "storm", "雷阵雨伴冰雹"}, {99, "storm", "强雷暴"},
};

static const wmo_t *find(int code)
{
    for (size_t i = 0; i < sizeof(k_wmo) / sizeof(k_wmo[0]); i++) {
        if (k_wmo[i].code == code) return &k_wmo[i];
    }
    return NULL;
}

const char *weather_wmo_icon(int code, bool is_day)
{
    const wmo_t *w = find(code);
    const char *icon = w ? w->icon : "cloud";
    return (!is_day && !strcmp(icon, "sun")) ? "moon" : icon;
}

const char *weather_wmo_text(int code)
{
    const wmo_t *w = find(code);
    return w ? w->text : "多云";
}

static double num(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
}

static int atoi_hour(const char *iso) { return (iso[11] - '0') * 10 + (iso[12] - '0'); }

static int rnd(double v) { return (int)(v >= 0 ? v + 0.5 : v - 0.5); }

static double at(const cJSON *arr, int i)
{
    const cJSON *v = cJSON_GetArrayItem(arr, i);
    return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
}

// "2026-11-19" → 星期几(Zeller,纯计算,不依赖时区库)
static const char *weekday_cn(const char *ymd)
{
    static const char *const names[7] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    int y = 0, m = 0, d = 0;
    if (sscanf(ymd, "%d-%d-%d", &y, &m, &d) != 3) return "";
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return names[(y + y / 4 - y / 100 + y / 400 + t[(m - 1) % 12] + d) % 7];
}

bool weather_shape(const char *json, const char *city, char *out, size_t cap)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    const cJSON *cur = cJSON_GetObjectItemCaseSensitive(root, "current");
    const cJSON *hourly = cJSON_GetObjectItemCaseSensitive(root, "hourly");
    const cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
    if (!cur || !daily) {
        cJSON_Delete(root);
        return false;
    }
    const int code = (int)num(cur, "weather_code");
    const bool day = num(cur, "is_day") != 0.0;

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "city", city ? city : "");
    cJSON *now = cJSON_AddObjectToObject(o, "now");
    cJSON_AddNumberToObject(now, "temp", rnd(num(cur, "temperature_2m")));
    cJSON_AddNumberToObject(now, "humidity", rnd(num(cur, "relative_humidity_2m")));
    cJSON_AddStringToObject(now, "code", weather_wmo_icon(code, day));
    cJSON_AddStringToObject(now, "text", weather_wmo_text(code));
    cJSON_AddStringToObject(now, "aqi_text", "--");

    const cJSON *dtime = cJSON_GetObjectItemCaseSensitive(daily, "time");
    const cJSON *dcode = cJSON_GetObjectItemCaseSensitive(daily, "weather_code");
    const cJSON *dmax = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max");
    const cJSON *dmin = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min");
    cJSON *today = cJSON_AddObjectToObject(o, "today");
    cJSON_AddNumberToObject(today, "hi", rnd(at(dmax, 0)));
    cJSON_AddNumberToObject(today, "lo", rnd(at(dmin, 0)));

    // 逐小时:从当前小时之后取 5 个
    cJSON *harr = cJSON_AddArrayToObject(o, "hourly");
    const cJSON *htime = cJSON_GetObjectItemCaseSensitive(hourly, "time");
    const cJSON *htemp = cJSON_GetObjectItemCaseSensitive(hourly, "temperature_2m");
    const cJSON *hcode = cJSON_GetObjectItemCaseSensitive(hourly, "weather_code");
    const cJSON *ctime = cJSON_GetObjectItemCaseSensitive(cur, "time");
    const char *now_t = cJSON_IsString(ctime) ? ctime->valuestring : "";
    int start = 0;
    const int hn = cJSON_GetArraySize(htime);
    for (int i = 0; i < hn; i++) {
        const cJSON *t = cJSON_GetArrayItem(htime, i);
        if (cJSON_IsString(t) && strncmp(t->valuestring, now_t, 13) > 0) {
            start = i;
            break;
        }
    }
    for (int i = start; i < hn && i < start + 5; i++) {
        const cJSON *t = cJSON_GetArrayItem(htime, i);
        const int hour = cJSON_IsString(t) && strlen(t->valuestring) >= 13 ? atoi_hour(t->valuestring) : 0;
        char label[12];
        snprintf(label, sizeof(label), "%d时", hour);
        cJSON *h = cJSON_CreateObject();
        cJSON_AddStringToObject(h, "h", label);
        cJSON_AddNumberToObject(h, "t", rnd(at(htemp, i)));
        cJSON_AddStringToObject(h, "code", weather_wmo_icon((int)at(hcode, i), hour >= 6 && hour < 18));
        cJSON_AddItemToArray(harr, h);
    }

    cJSON *darr = cJSON_AddArrayToObject(o, "daily");
    const int dn = cJSON_GetArraySize(dtime);
    for (int i = 0; i < dn && i < 4; i++) {
        const cJSON *t = cJSON_GetArrayItem(dtime, i);
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "day", i == 0 ? "今天" : (cJSON_IsString(t) ? weekday_cn(t->valuestring) : ""));
        cJSON_AddStringToObject(d, "code", weather_wmo_icon((int)at(dcode, i), true));
        cJSON_AddNumberToObject(d, "hi", rnd(at(dmax, i)));
        cJSON_AddNumberToObject(d, "lo", rnd(at(dmin, i)));
        cJSON_AddItemToArray(darr, d);
    }
    if (strlen(now_t) >= 16) {
        char upd[8];
        snprintf(upd, sizeof(upd), "%.5s", now_t + 11);
        cJSON_AddStringToObject(o, "updated", upd);
    }
    const bool ok = cJSON_PrintPreallocated(o, out, (int)cap, false);
    cJSON_Delete(o);
    cJSON_Delete(root);
    return ok;
}
