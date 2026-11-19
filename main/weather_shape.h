// main/weather_shape.h —— 把 Open-Meteo 的预报 JSON 转成设备天气页使用的精简 JSON
// (与中转服务器 /api/weather 的格式一致)。纯逻辑,固件与模拟器共用。
#pragma once

#include <stdbool.h>
#include <stddef.h>

// city 可为空。成功返回 true,out 以 0 结尾。
bool weather_shape(const char *openmeteo_json, const char *city, char *out, size_t cap);
// WMO 天气代码 → 设备图标代码("sun"/"partly"/…)与中文描述。
const char *weather_wmo_icon(int code, bool is_day);
const char *weather_wmo_text(int code);
