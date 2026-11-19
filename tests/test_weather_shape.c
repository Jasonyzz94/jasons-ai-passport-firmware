// tests/test_weather_shape.c —— Open-Meteo → 设备天气 JSON(直连天气,不经过服务器)。
#include "cJSON.h"
#include "weather_shape.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *k_om =
    "{\"current\":{\"time\":\"2026-11-19T09:45\",\"temperature_2m\":18.4,\"relative_humidity_2m\":62,"
    "\"weather_code\":2,\"is_day\":1},"
    "\"hourly\":{\"time\":[\"2026-11-19T09:00\",\"2026-11-19T10:00\",\"2026-11-19T11:00\","
    "\"2026-11-19T12:00\",\"2026-11-19T13:00\",\"2026-11-19T14:00\",\"2026-11-19T15:00\"],"
    "\"temperature_2m\":[17,18,19,20,21,21,20],\"weather_code\":[2,1,0,0,2,3,61]},"
    "\"daily\":{\"time\":[\"2026-11-19\",\"2026-11-20\",\"2026-11-21\",\"2026-11-22\"],"
    "\"weather_code\":[2,61,3,0],\"temperature_2m_max\":[21,17,16,19],\"temperature_2m_min\":[14,12,11,10]}}";

int main(void)
{
    char out[1536];
    assert(weather_shape(k_om, "上海", out, sizeof(out)));
    cJSON *j = cJSON_Parse(out);
    assert(j);
    const cJSON *now = cJSON_GetObjectItem(j, "now");
    assert(cJSON_GetObjectItem(now, "temp")->valueint == 18);
    assert(!strcmp(cJSON_GetObjectItem(now, "code")->valuestring, "partly"));
    assert(!strcmp(cJSON_GetObjectItem(now, "text")->valuestring, "多云"));
    const cJSON *hourly = cJSON_GetObjectItem(j, "hourly");
    assert(cJSON_GetArraySize(hourly) == 5);
    assert(!strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(hourly, 0), "h")->valuestring, "10时"));
    const cJSON *daily = cJSON_GetObjectItem(j, "daily");
    assert(cJSON_GetArraySize(daily) == 4);
    assert(!strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(daily, 0), "day")->valuestring, "今天"));
    assert(!strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(daily, 1), "day")->valuestring, "周五"));
    assert(!strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(daily, 1), "code")->valuestring, "rain"));
    assert(!strcmp(cJSON_GetObjectItem(j, "updated")->valuestring, "09:45"));
    cJSON_Delete(j);
    assert(!strcmp(weather_wmo_icon(0, false), "moon"));
    assert(!weather_shape("not json", NULL, out, sizeof(out)));
    assert(!weather_shape("{\"x\":1}", NULL, out, sizeof(out)));
    puts("weather shape tests: PASS");
    return 0;
}
