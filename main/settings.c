// main/settings.c —— 全局设置持久化。
#include "settings.h"

#include "svc.h"
#include "sys.h"
#include "voice_cfg.h"
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
    // 默认云服务:硅基流动(国内可直连,一个密钥同时提供识别 / 对话 / 合成)。
    snprintf(g_settings.api_base, sizeof(g_settings.api_base), "https://api.siliconflow.cn/v1");
    snprintf(g_settings.asr_model, sizeof(g_settings.asr_model), "FunAudioLLM/SenseVoiceSmall");
    snprintf(g_settings.llm_model, sizeof(g_settings.llm_model), "deepseek-ai/DeepSeek-V3");
    snprintf(g_settings.tts_model, sizeof(g_settings.tts_model), "FunAudioLLM/CosyVoice2-0.5B");
    snprintf(g_settings.tts_voice, sizeof(g_settings.tts_voice), "FunAudioLLM/CosyVoice2-0.5B:anna");
    g_settings.ai_mode = VOICE_MODE_AUTO;
    g_settings.tts_rate = VOICE_TTS_16K;
}

static int valid_rate(int r) { return r == VOICE_TTS_OFF || r == VOICE_TTS_24K ? r : VOICE_TTS_16K; }

// 读字符串设置:存储里是空串时保留默认值(避免旧版本写入的空值覆盖默认)。
static void load_str(const char *key, char *dst, size_t len)
{
    char buf[200];
    if (sys_kv_get_str(key, buf, sizeof(buf)) && buf[0]) snprintf(dst, len, "%s", buf);
}

void settings_load(void)
{
    defaults();
    g_settings.brightness = clampi(sys_kv_get_i32("bright", g_settings.brightness), 10, 100);
    g_settings.volume = clampi(sys_kv_get_i32("volume", g_settings.volume), 0, 100);
    g_settings.screen_timeout = clampi(sys_kv_get_i32("timeout", g_settings.screen_timeout),
                                       0, SETTINGS_TIMEOUT_COUNT - 1);
    g_settings.sound_feedback = sys_kv_get_i32("click", 1) ? 1 : 0;
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
    load_str("city", g_settings.city, sizeof(g_settings.city));
    load_str("lat", g_settings.lat, sizeof(g_settings.lat));
    load_str("lon", g_settings.lon, sizeof(g_settings.lon));
    load_str("api_base", g_settings.api_base, sizeof(g_settings.api_base));
    sys_kv_get_str("api_key", g_settings.api_key, sizeof(g_settings.api_key));
    sys_kv_get_str("voice_base", g_settings.voice_base, sizeof(g_settings.voice_base));
    sys_kv_get_str("voice_key", g_settings.voice_key, sizeof(g_settings.voice_key));
    g_settings.ai_mode = clampi(sys_kv_get_i32("ai_mode", VOICE_MODE_AUTO), VOICE_MODE_AUTO, VOICE_MODE_DIRECT);
    g_settings.tts_rate = valid_rate(sys_kv_get_i32("tts_rate", VOICE_TTS_16K));
    load_str("asr_model", g_settings.asr_model, sizeof(g_settings.asr_model));
    load_str("llm_model", g_settings.llm_model, sizeof(g_settings.llm_model));
    load_str("tts_model", g_settings.tts_model, sizeof(g_settings.tts_model));
    load_str("tts_voice", g_settings.tts_voice, sizeof(g_settings.tts_voice));
    sys_kv_get_str("ha_url", g_settings.ha_url, sizeof(g_settings.ha_url));
    sys_kv_get_str("ha_token", g_settings.ha_token, sizeof(g_settings.ha_token));
    sys_kv_get_str("ha_ents", g_settings.ha_entities, sizeof(g_settings.ha_entities));
    sys_kv_get_str("ics", g_settings.ics_url, sizeof(g_settings.ics_url));
}

void settings_save(void)
{
    sys_kv_set_i32("bright", g_settings.brightness);
    sys_kv_set_i32("volume", g_settings.volume);
    sys_kv_set_i32("timeout", g_settings.screen_timeout);
    sys_kv_set_i32("click", g_settings.sound_feedback);
    sys_kv_set_str("server", g_settings.server);
    sys_kv_set_str("token", g_settings.token);
    sys_kv_set_str("tz", g_settings.tz);
    sys_kv_set_str("name", g_settings.name);
    sys_kv_set_str("city", g_settings.city);
    sys_kv_set_str("lat", g_settings.lat);
    sys_kv_set_str("lon", g_settings.lon);
    sys_kv_set_str("api_base", g_settings.api_base);
    sys_kv_set_str("api_key", g_settings.api_key);
    sys_kv_set_str("voice_base", g_settings.voice_base);
    sys_kv_set_str("voice_key", g_settings.voice_key);
    sys_kv_set_i32("ai_mode", g_settings.ai_mode);
    sys_kv_set_i32("tts_rate", g_settings.tts_rate);
    sys_kv_set_str("asr_model", g_settings.asr_model);
    sys_kv_set_str("llm_model", g_settings.llm_model);
    sys_kv_set_str("tts_model", g_settings.tts_model);
    sys_kv_set_str("tts_voice", g_settings.tts_voice);
    sys_kv_set_str("ha_url", g_settings.ha_url);
    sys_kv_set_str("ha_token", g_settings.ha_token);
    sys_kv_set_str("ha_ents", g_settings.ha_entities);
    sys_kv_set_str("ics", g_settings.ics_url);
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

bool settings_has_wifi(void) { return net_saved_count() > 0; }
