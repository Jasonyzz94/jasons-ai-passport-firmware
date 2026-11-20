// main/app_settings.c —— 设置:Wi-Fi 配网(手机扫码)、服务器、亮度、音量、息屏、按键音、关于、重启、恢复出厂。
#include "apps.h"
#include "icons.h"
#include "noise_gen.h"
#include "voice_cfg.h"
#include "settings.h"
#include "svc.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>

typedef enum { PAGE_LIST = 0, PAGE_PORTAL, PAGE_ABOUT, PAGE_BG } page_t;
enum {
    ROW_WIFI = 0, ROW_SERVER, ROW_BRIGHT, ROW_VOLUME, ROW_TIMEOUT, ROW_CLICK,
    ROW_BG, ROW_ABOUT, ROW_RESTART, ROW_RESET, ROW_COUNT,
};

static lv_obj_t *s_root;
static lv_obj_t *s_page;
static page_t s_kind;
static ui_list_t s_list;
static int s_focus;
static bool s_editing;
static bool s_dirty;

// 配网页
static lv_obj_t *s_portal_status;
static lv_obj_t *s_qr;
static lv_obj_t *s_step;
static portal_state_t s_last_portal = (portal_state_t)-1;

// 关于页
static lv_obj_t *s_about;

static void build(page_t page);

static const char *srv_text(void)
{
    switch (relay_state()) {
    case SRV_OK: return "已连接";
    case SRV_DISCOVERING: return "查找中…";
    case SRV_UNREACHABLE: return net_state() == NET_CONNECTED ? "未找到 · 直连" : "未找到";
    case SRV_NO_NET: return "未联网";
    default: return "未连接";
    }
}

static void list_values(void)
{
    char buf[40];
    if (settings_has_wifi()) {
        snprintf(buf, sizeof(buf), "%s", g_settings.wifi_ssid);
        ui_list_set_value(&s_list, ROW_WIFI, buf);
    } else {
        ui_list_set_value(&s_list, ROW_WIFI, "未设置");
    }
    ui_list_set_value(&s_list, ROW_SERVER, srv_text());
    ui_list_set_value_color(&s_list, ROW_SERVER, relay_state() == SRV_OK ? C_OK : C_TEXT3);
    snprintf(buf, sizeof(buf), "%d%%", g_settings.brightness);
    ui_list_set_value(&s_list, ROW_BRIGHT, buf);
    snprintf(buf, sizeof(buf), "%d%%", g_settings.volume);
    ui_list_set_value(&s_list, ROW_VOLUME, buf);
    ui_list_set_value(&s_list, ROW_TIMEOUT, settings_timeout_name(g_settings.screen_timeout));
    ui_list_set_value(&s_list, ROW_CLICK, g_settings.sound_feedback ? "开" : "关");
    for (int i = ROW_BRIGHT; i <= ROW_TIMEOUT; i++) {
        ui_list_set_value_color(&s_list, i, s_editing && s_list.focus == i ? C_TOMATO : C_TEXT3);
    }
}

static void list_hints(void)
{
    shell_hints(s_editing ? "▲▼:调整|OK:完成" : "▲▼:选择|OK:打开|长▲:返回");
}

static void build_list(lv_obj_t *p)
{
    lv_obj_t *t = ui_label(p, &ui_font_20b, C_TEXT, "设置");
    lv_obj_set_pos(t, UI_PAD + 4, 4);
    ui_list_create(&s_list, p, UI_PAD, 36, UI_W - 2 * UI_PAD, UI_CONTENT_H - 40, C_SETTINGS);
    ui_list_add(&s_list, &ICON_WIFI, "无线网络", "", true);
    ui_list_add(&s_list, &ICON_BOARD, "中转服务器", "", false);
    ui_list_add(&s_list, &IC_STAR, "亮度", "", false);
    ui_list_add(&s_list, &IC_MUSIC, "音量", "", false);
    ui_list_add(&s_list, &ICON_TIMER, "自动息屏", "", false);
    ui_list_add(&s_list, &IC_BELL, "按键音", "", false);
    ui_list_add(&s_list, &IC_SENSOR, "后台运行", NULL, true);
    ui_list_add(&s_list, &IC_CHECK, "关于本机", NULL, true);
    ui_list_add(&s_list, &ICON_GEAR, "重启", NULL, false);
    ui_list_add(&s_list, &IC_CLOUD_OFF, "恢复出厂设置", NULL, false);
    s_editing = false;
    ui_list_focus(&s_list, s_focus, false);
    list_values();
    list_hints();
}

// ---------------------------------------------------------------------------
static void portal_render(void)
{
    const portal_state_t st = net_portal_state();
    if (st == s_last_portal) return;
    s_last_portal = st;
    char qr[96];
    switch (st) {
    case PORTAL_CLIENT:
        snprintf(qr, sizeof(qr), "http://192.168.4.1");
        lv_label_set_text(s_step, "② 扫码打开配置页");
        lv_label_set_text(s_portal_status, "手机已连上,填写家里的无线网络");
        break;
    case PORTAL_SAVED:
        snprintf(qr, sizeof(qr), "http://192.168.4.1");
        lv_label_set_text(s_step, "已保存");
        lv_label_set_text_fmt(s_portal_status, "正在连接 %s …", g_settings.wifi_ssid);
        break;
    case PORTAL_FAILED:
        snprintf(qr, sizeof(qr), "-");
        lv_label_set_text(s_step, "热点启动失败");
        lv_label_set_text(s_portal_status, "长▲ 返回后重试");
        break;
    default:
        snprintf(qr, sizeof(qr), "WIFI:T:nopass;S:%s;;", net_portal_ssid());
        lv_label_set_text(s_step, "① 手机扫码连接热点");
        lv_label_set_text_fmt(s_portal_status, "或手动连接 %s", net_portal_ssid());
        break;
    }
#if LV_USE_QRCODE
    lv_qrcode_update(s_qr, qr, strlen(qr));
#endif
}

static void build_portal(lv_obj_t *p)
{
    s_step = ui_label_w(p, &ui_font_20b, C_TEXT, UI_W, LV_TEXT_ALIGN_CENTER, "");
    lv_obj_set_pos(s_step, 0, 4);
    lv_obj_t *frame = ui_card(p, (UI_W - 156) / 2, 34, 156, 156, 0xFFFFFF, 14);
#if LV_USE_QRCODE
    s_qr = lv_qrcode_create(frame);
    lv_qrcode_set_size(s_qr, 136);
    lv_qrcode_set_dark_color(s_qr, ui_c(0x0B0E14));
    lv_qrcode_set_light_color(s_qr, ui_c(0xFFFFFF));
    lv_obj_center(s_qr);
#else
    s_qr = NULL;
#endif
    s_portal_status = ui_label_w(p, &ui_font_16, C_TEXT2, UI_W - 2 * UI_PAD, LV_TEXT_ALIGN_CENTER, "");
    lv_obj_set_pos(s_portal_status, UI_PAD, 198);
    lv_obj_t *tip = ui_label_w(p, &ui_font_14, C_TEXT3, UI_W - 2 * UI_PAD, LV_TEXT_ALIGN_CENTER,
                               "网页里还有高级设置:云端密钥、天气、家居");
    lv_obj_set_pos(tip, UI_PAD, 226);
    s_last_portal = (portal_state_t)-1;
    shell_hints("长▲:取消返回");
    shell_keep_awake(true);
    if (!net_portal_start()) shell_toast("热点启动失败");
    portal_render();
}

// ---------------------------------------------------------------------------
static void about_render(void)
{
    uint8_t mac[6];
    sys_mac(mac);
    char buf[520];
    const int64_t up = sys_now_ms() / 1000;
    snprintf(buf, sizeof(buf),
             "设备名  %s\n固件    %s 版\n硬件码  %02X%02X%02X%02X%02X%02X\n"
             "地址    %s\n服务器  %s\n电池    %d%% · %d 毫伏\n内存    空闲 %u 千字节\n"
             "运行    %d 小时 %d 分\n发现    %s",
             g_settings.name, SYS_FW_VERSION, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             net_state() == NET_CONNECTED ? net_ip() : "未联网",
             relay_state() == SRV_OK ? (strstr(relay_url(), "://") ? strstr(relay_url(), "://") + 3
                                                                   : relay_url()) : srv_text(),
             sys_battery_soc(), sys_battery_mv(), (unsigned)(sys_free_heap() / 1024),
             (int)(up / 3600), (int)(up / 60 % 60), relay_diag());
    lv_label_set_text(s_about, buf);
}

static void build_about(lv_obj_t *p)
{
    lv_obj_t *head = ui_box(p, UI_PAD, 4, UI_W - 2 * UI_PAD, 40);
    lv_obj_t *icon = ui_pix(head, &ICON_GEAR, 2);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_t *t = ui_label(head, &ui_font_20b, C_TEXT, "随身工具箱");
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 44, 0);
    lv_obj_t *card = ui_card(p, UI_PAD, 46, UI_W - 2 * UI_PAD, 210, C_SURFACE, 16);
    s_about = ui_label(card, &ui_font_14, C_TEXT2, "");
    lv_label_set_long_mode(s_about, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_about, UI_W - 2 * UI_PAD - 24);
    lv_obj_set_style_text_line_space(s_about, 3, 0);
    lv_obj_set_pos(s_about, 14, 12);
    about_render();
    shell_hints("长▲:返回");
}

// ---------------------------------------------------------------------------
// 后台运行:哪些服务开着、内存还剩多少。没在用的服务会自己释放内存。
// ---------------------------------------------------------------------------
static lv_obj_t *s_bg_rows[8];
static lv_obj_t *s_bg_mem;
static const char *const k_bg_names[] = {"无线网络", "蓝牙", "双机互联", "白噪音", "语音助手", "服务器通信"};
#define BG_ROWS ((int)(sizeof(k_bg_names) / sizeof(k_bg_names[0])))

static void bg_render(void)
{
    static const char *const ble_names[] = {"关闭", "启动中", "等待配对", "已连接", "出错"};
    const ble_state_t bs = ble_hid_state();
    const int noise = audio_noise_current();
    const char *val[BG_ROWS] = {
        !net_radio_on() ? "关闭" : net_state() == NET_CONNECTED ? "已连接" : "连接中",
        ble_names[bs <= BLE_FAILED ? bs : 0],
        link_running() ? "在线" : "关闭",
        noise ? noise_name((noise_type_t)noise) : "关闭",
        ai_active() ? "已加载" : "未加载",
        relay_task_running() ? "运行中" : "休眠",
    };
    const bool on[BG_ROWS] = {net_radio_on(), bs != BLE_OFF, link_running(), noise != 0, ai_active(),
                              relay_task_running()};
    for (int i = 0; i < BG_ROWS; i++) {
        lv_label_set_text(s_bg_rows[i], val[i]);
        lv_obj_set_style_text_color(s_bg_rows[i], ui_c(on[i] ? C_OK : C_TEXT3), 0);
    }
    const unsigned free_kb = (unsigned)(sys_free_heap() / 1024);
    lv_label_set_text_fmt(s_bg_mem, "空闲内存 %u KB · 最大连续 %u KB", free_kb,
                          (unsigned)(sys_largest_block() / 1024));
    lv_obj_set_style_text_color(s_bg_mem, ui_c(free_kb < 30 ? C_WARN : C_TEXT2), 0);
}

static void build_bg(lv_obj_t *p)
{
    lv_obj_t *t = ui_label(p, &ui_font_20b, C_TEXT, "后台运行");
    lv_obj_set_pos(t, UI_PAD + 4, 4);
    lv_obj_t *card = ui_card(p, UI_PAD, 40, UI_W - 2 * UI_PAD, BG_ROWS * 26 + 14, C_SURFACE, 16);
    for (int i = 0; i < BG_ROWS; i++) {
        lv_obj_t *k = ui_label(card, &ui_font_16, C_TEXT2, k_bg_names[i]);
        lv_obj_set_pos(k, 14, 8 + i * 26);
        s_bg_rows[i] = ui_label(card, &ui_font_16, C_TEXT3, "");
        lv_obj_align(s_bg_rows[i], LV_ALIGN_TOP_RIGHT, -14, 8 + i * 26);
    }
    s_bg_mem = ui_label_w(p, &ui_font_14, C_TEXT2, UI_W, LV_TEXT_ALIGN_CENTER, "");
    lv_obj_set_pos(s_bg_mem, 0, 40 + BG_ROWS * 26 + 24);
    lv_obj_t *tip = ui_label_w(p, &ui_font_14, C_TEXT3, UI_W - 2 * UI_PAD, LV_TEXT_ALIGN_CENTER,
                               "离开功能后会自动关闭并释放内存");
    lv_obj_set_pos(tip, UI_PAD, 40 + BG_ROWS * 26 + 46);
    bg_render();
    shell_hints("长▲:返回");
}

// ---------------------------------------------------------------------------
static void build(page_t page)
{
    if (s_page) lv_obj_delete(s_page);
    s_page = ui_box(s_root, 0, 0, UI_W, UI_CONTENT_H);
    s_kind = page;
    switch (page) {
    case PAGE_PORTAL: build_portal(s_page); break;
    case PAGE_ABOUT: build_about(s_page); break;
    case PAGE_BG: build_bg(s_page); break;
    default: build_list(s_page); break;
    }
}

static void create(lv_obj_t *root)
{
    s_root = root;
    s_page = NULL;
    s_dirty = false;
    build(PAGE_LIST);
}

static void leave_portal(void)
{
    net_portal_stop();
    shell_keep_awake(false);
}

static void destroy(void)
{
    if (s_kind == PAGE_PORTAL) leave_portal();
    if (s_dirty) settings_save();
    s_dirty = false;
    s_root = NULL;
    s_page = NULL;
    s_kind = PAGE_LIST;
}

static void reset_answer(bool yes)
{
    if (!yes) return;
    settings_factory_reset();
    sys_restart();
}

static void restart_answer(bool yes)
{
    if (yes) {
        settings_save();
        sys_restart();
    }
}

static void adjust(int delta)
{
    switch (s_list.focus) {
    case ROW_BRIGHT:
        g_settings.brightness += delta * 10;
        if (g_settings.brightness < 10) g_settings.brightness = 10;
        if (g_settings.brightness > 100) g_settings.brightness = 100;
        shell_apply_brightness();
        break;
    case ROW_VOLUME:
        g_settings.volume += delta * 10;
        if (g_settings.volume < 0) g_settings.volume = 0;
        if (g_settings.volume > 100) g_settings.volume = 100;
        audio_apply_volume();
        audio_beep(BEEP_CLICK);
        break;
    case ROW_TIMEOUT:
        g_settings.screen_timeout = (g_settings.screen_timeout + delta + SETTINGS_TIMEOUT_COUNT)
                                  % SETTINGS_TIMEOUT_COUNT;
        break;
    default:
        break;
    }
    s_dirty = true;
    list_values();
}

static bool key(sys_key_t k, sys_key_ev_t ev)
{
    if (s_kind != PAGE_LIST) {
        if (k == SYS_KEY_UP && ev == SYS_EV_LONG) {
            if (s_kind == PAGE_PORTAL) leave_portal();
            build(PAGE_LIST);
            return true;
        }
        return ev == SYS_EV_CLICK;
    }
    if (ev != SYS_EV_CLICK) return false;
    if (s_editing) {
        if (k == SYS_KEY_OK) {
            s_editing = false;
            settings_save();
            s_dirty = false;
            list_values();
            list_hints();
        } else {
            adjust(k == SYS_KEY_UP ? 1 : -1);
        }
        return true;
    }
    if (k != SYS_KEY_OK) {
        s_focus = ui_list_move(&s_list, k == SYS_KEY_DOWN ? 1 : -1);
        return true;
    }
    s_focus = s_list.focus;
    switch (s_list.focus) {
    case ROW_WIFI: build(PAGE_PORTAL); break;
    case ROW_SERVER:
        relay_discover();
        shell_toast("正在局域网查找服务器");
        break;
    case ROW_BRIGHT:
    case ROW_VOLUME:
    case ROW_TIMEOUT:
        s_editing = true;
        list_values();
        list_hints();
        break;
    case ROW_CLICK:
        g_settings.sound_feedback = !g_settings.sound_feedback;
        settings_save();
        list_values();
        break;
    case ROW_BG: build(PAGE_BG); break;
    case ROW_ABOUT: build(PAGE_ABOUT); break;
    case ROW_RESTART: shell_confirm("重启设备?", NULL, restart_answer); break;
    default:
        shell_confirm("恢复出厂设置?", "网络、配对、统计等全部清除并重启", reset_answer);
        break;
    }
    return true;
}

static void tick(int64_t now)
{
    static int64_t last;
    if (s_kind == PAGE_PORTAL) {
        static portal_form_t f;
        if (net_portal_take(&f)) {
#define SET(dst, src) snprintf(dst, sizeof(dst), "%s", src)
            // 名称没变且密码留空 = 沿用已保存的网络,不重新连接。
            const bool same_ssid = !strcmp(f.ssid, g_settings.wifi_ssid);
            const bool wifi_changed = f.ssid[0] != '\0' && (!same_ssid || f.pass[0] != '\0');
            if (wifi_changed) {
                SET(g_settings.wifi_ssid, f.ssid);
                SET(g_settings.wifi_pass, f.pass);
            }
            // 普通字段:提交什么就存什么(清空也生效);密钥类字段留空表示不修改。
            SET(g_settings.server, f.server);
            // 语音助手。密钥绑定接口:换了接口地址却没填新密钥,就清掉旧密钥,不发给别家。
            if (f.clear_keys) {
                g_settings.api_key[0] = '\0';
                g_settings.voice_key[0] = '\0';
            }
            if (f.ai_mode >= VOICE_MODE_AUTO && f.ai_mode <= VOICE_MODE_DIRECT) g_settings.ai_mode = f.ai_mode;
            if (f.tts_rate == VOICE_TTS_OFF || f.tts_rate == VOICE_TTS_16K || f.tts_rate == VOICE_TTS_24K) {
                g_settings.tts_rate = f.tts_rate;
            }
            if (f.api_base[0] && strcmp(f.api_base, g_settings.api_base) != 0) {
                SET(g_settings.api_base, f.api_base);
                if (!f.api_key[0]) g_settings.api_key[0] = '\0';
            }
            if (f.api_key[0]) SET(g_settings.api_key, f.api_key);
            if (strcmp(f.voice_base, g_settings.voice_base) != 0) {   // 语音接口可以清空(= 同对话)
                SET(g_settings.voice_base, f.voice_base);
                if (!f.voice_key[0]) g_settings.voice_key[0] = '\0';
            }
            if (f.voice_key[0]) SET(g_settings.voice_key, f.voice_key);
            if (f.llm_model[0]) SET(g_settings.llm_model, f.llm_model);
            if (f.asr_model[0]) SET(g_settings.asr_model, f.asr_model);
            if (f.tts_model[0]) SET(g_settings.tts_model, f.tts_model);
            if (f.tts_voice[0]) SET(g_settings.tts_voice, f.tts_voice);
            SET(g_settings.city, f.city);
            SET(g_settings.lat, f.lat);
            SET(g_settings.lon, f.lon);
            SET(g_settings.ha_url, f.ha_url);
            if (f.ha_token[0]) SET(g_settings.ha_token, f.ha_token);
            SET(g_settings.ha_entities, f.ha_entities);
            SET(g_settings.ics_url, f.ics_url);
#undef SET
            settings_save();
            if (wifi_changed) {
                net_reconnect();
            } else {
                leave_portal();
                shell_toast("高级设置已保存");
                build(PAGE_LIST);
                return;
            }
        }
        portal_render();
        // 连上后多留 6 秒热点,让手机网页能看到"连接成功";密码错误时提示并允许重新提交。
        static int64_t s_connected_at;
        static bool s_fail_shown;
        if (net_portal_state() == PORTAL_SAVED && net_state() == NET_CONNECTED) {
            if (!s_connected_at) {
                s_connected_at = now;
                audio_beep(BEEP_OK);
                lv_label_set_text(s_step, "连接成功");
                lv_label_set_text_fmt(s_portal_status, "地址 %s,马上返回", net_ip());
            } else if (now - s_connected_at > 6000) {
                s_connected_at = 0;
                leave_portal();
                shell_toast("无线网络已连接");
                build(PAGE_LIST);
            }
        } else if (net_portal_state() == PORTAL_SAVED && net_state() == NET_FAILED) {
            if (!s_fail_shown) {
                s_fail_shown = true;
                audio_beep(BEEP_ERR);
                lv_label_set_text(s_step, "连接失败");
                lv_label_set_text_fmt(s_portal_status, "连不上 %s,请在手机上改密码后重新保存",
                                      g_settings.wifi_ssid);
            }
        } else {
            s_fail_shown = false;
        }
    } else if (now - last > 1000) {
        last = now;
        if (s_kind == PAGE_ABOUT) about_render();
        else if (s_kind == PAGE_BG) bg_render();
        else if (!s_editing) list_values();
    }
}

const app_t APP_SETTINGS = {
    .name = "设置",
    .desc = "网络 · 亮度 · 关于",
    .icon = &ICON_GEAR,
    .color = C_SETTINGS,
    .create = create,
    .destroy = destroy,
    .key = key,
    .tick = tick,
};
