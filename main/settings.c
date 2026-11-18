// main/settings.c —— 全局设置持久化。
#include "settings.h"

#include "sys.h"
#include <stdio.h>
#include <string.h>

settings_t g_settings;

static const int k_timeout_ms[SETTINGS_TIMEOUT_COUNT] = {15000, 30000, 60000, 120000, 300000, 0};
static const char *const k_timeout_names[SETTINGS_TIMEOUT_COUNT] = {
    "15 秒", "30 秒", "1 分钟", "2 分钟", "5 分钟", "永不",
};

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void defaults(void)
{
    memset(&g_settings, 0, sizeof(g_settings));
    g_settings.brightness = 80;
    g_settings.volume = 70;
    g_settings.screen_timeout = 2;
    g_settings.sound_feedback = 1;
    snprintf(g_settings.tz, sizeof(g_settings.tz), "CST-8");
    snprintf(g_settings.name, sizeof(g_settings.name), "Passport-%s", sys_short_id());
}

void settings_load(void)
{
    defaults();
    g_settings.brightness = clampi(sys_kv_get_i32("bright", g_settings.brightness), 10, 100);
    g_settings.volume = clampi(sys_kv_get_i32("volume", g_settings.volume), 0, 100);
    g_settings.screen_timeout = clampi(sys_kv_get_i32("timeout", g_settings.screen_timeout),
                                       0, SETTINGS_TIMEOUT_COUNT - 1);
    g_settings.sound_feedback = sys_kv_get_i32("click", 1) ? 1 : 0;
    sys_kv_get_str("ssid", g_settings.wifi_ssid, sizeof(g_settings.wifi_ssid));
    sys_kv_get_str("pass", g_settings.wifi_pass, sizeof(g_settings.wifi_pass));
    sys_kv_get_str("server", g_settings.server, sizeof(g_settings.server));
    sys_kv_get_str("token", g_settings.token, sizeof(g_settings.token));
    char buf[48];
    if (sys_kv_get_str("tz", buf, sizeof(buf)) && buf[0]) {
        snprintf(g_settings.tz, sizeof(g_settings.tz), "%s", buf);
    }
    char name[24];
    if (sys_kv_get_str("name", name, sizeof(name)) && name[0]) {
        snprintf(g_settings.name, sizeof(g_settings.name), "%s", name);
    }
}

void settings_save(void)
{
    sys_kv_set_i32("bright", g_settings.brightness);
    sys_kv_set_i32("volume", g_settings.volume);
    sys_kv_set_i32("timeout", g_settings.screen_timeout);
    sys_kv_set_i32("click", g_settings.sound_feedback);
    sys_kv_set_str("ssid", g_settings.wifi_ssid);
    sys_kv_set_str("pass", g_settings.wifi_pass);
    sys_kv_set_str("server", g_settings.server);
    sys_kv_set_str("token", g_settings.token);
    sys_kv_set_str("tz", g_settings.tz);
    sys_kv_set_str("name", g_settings.name);
}

void settings_factory_reset(void)
{
    sys_kv_erase_all();
    defaults();
}

int settings_timeout_ms(void) { return k_timeout_ms[g_settings.screen_timeout]; }

const char *settings_timeout_name(int idx)
{
    return k_timeout_names[clampi(idx, 0, SETTINGS_TIMEOUT_COUNT - 1)];
}

bool settings_has_wifi(void) { return g_settings.wifi_ssid[0] != '\0'; }
