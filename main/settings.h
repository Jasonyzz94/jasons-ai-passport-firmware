// main/settings.h —— 全局设置(存 NVS)。只在 LVGL 任务里读写。
#pragma once

#include <stdbool.h>

#define SETTINGS_TIMEOUT_COUNT 6

typedef struct {
    int brightness;          // 10..100
    int volume;              // 0..100
    int screen_timeout;      // 0..SETTINGS_TIMEOUT_COUNT-1
    int sound_feedback;      // 0/1 按键提示音
    char wifi_ssid[33];
    char wifi_pass[65];
    char server[96];         // 中转服务器,例 "http://192.168.1.20:8787";空 = 自动发现
    char token[48];          // 服务器下发的设备令牌
    char tz[48];             // POSIX TZ,例 "CST-8"
    char name[24];           // 设备名(互联/蓝牙广播用)
} settings_t;

extern settings_t g_settings;

void settings_load(void);
void settings_save(void);
// 恢复出厂:清空所有存储并回到默认值。
void settings_factory_reset(void);

int settings_timeout_ms(void);   // 0 表示永不息屏
const char *settings_timeout_name(int idx);
bool settings_has_wifi(void);
