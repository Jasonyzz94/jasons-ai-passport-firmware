// main/app_remote.c —— 蓝牙遥控:设备伪装成蓝牙键盘,三种模式。
//   翻页笔:▲ 上一页 · ▼ 下一页 · OK 放映(F5) · 长▼ 黑屏(B)
//   媒体  :▲ 音量+ · ▼ 音量- · OK 播放/暂停 · 长▼ 下一首
//   拍照  :OK / ▲ / ▼ 快门(音量+,手机相机通用)
// 长OK 切换模式,长▲ 返回(断开蓝牙)。无需装 App,在手机/电脑蓝牙里配对即可。
#include "apps.h"
#include "icons.h"
#include "settings.h"
#include "svc.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>

// HID 键码(Keyboard/Keypad page 0x07)与媒体用法(Consumer page 0x0C)
#define KEY_RIGHT 0x4F
#define KEY_LEFT  0x50
#define KEY_F5    0x3E
#define KEY_B     0x05
#define CC_VOL_UP   0x00E9
#define CC_VOL_DOWN 0x00EA
#define CC_PLAY     0x00CD
#define CC_NEXT     0x00B5

typedef enum { MODE_SLIDES = 0, MODE_MEDIA, MODE_CAMERA, MODE_COUNT } mode_t_;

typedef struct {
    const char *name;
    const pixel_sprite_t *icon;
    const char *up, *ok, *down, *down_long;
} mode_info_t;

static const mode_info_t k_modes[MODE_COUNT] = {
    {"翻页笔", &IC_SLIDES, "上一页", "开始放映", "下一页", "黑屏"},
    {"媒体", &IC_MUSIC, "音量 +", "播放 / 暂停", "音量 -", "下一首"},
    {"拍照", &IC_CAMERA, "快门", "快门", "快门", NULL},
};

static int s_mode;
static lv_obj_t *s_root;
static lv_obj_t *s_status;
static lv_obj_t *s_status_dot;
static lv_obj_t *s_tabs[MODE_COUNT];
static lv_obj_t *s_keys[3];      // 0=▲ 1=OK 2=▼
static lv_obj_t *s_key_text[3];
static lv_obj_t *s_extra;
static int64_t s_flash_until[3];
static ble_state_t s_last = (ble_state_t)-1;

static void render_mode(void)
{
    const mode_info_t *m = &k_modes[s_mode];
    for (int i = 0; i < MODE_COUNT; i++) {
        const bool on = i == s_mode;
        lv_obj_set_style_bg_color(s_tabs[i], ui_c(on ? C_REMOTE : C_SURFACE), 0);
        lv_obj_set_style_text_color(s_tabs[i], ui_c(on ? C_BG : C_TEXT2), 0);
    }
    lv_label_set_text(s_key_text[0], m->up);
    lv_label_set_text(s_key_text[1], m->ok);
    lv_label_set_text(s_key_text[2], m->down);
    if (m->down_long) {
        lv_label_set_text_fmt(s_extra, "长按 ▼:%s", m->down_long);
    } else {
        lv_label_set_text(s_extra, "手机相机里任意键都是快门");
    }
}

static lv_obj_t *key_card(lv_obj_t *root, int y, int h, const char *glyph, int idx)
{
    lv_obj_t *c = ui_card(root, UI_PAD + 18, y, UI_W - 2 * UI_PAD - 36, h, C_SURFACE, h / 2);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, ui_c(C_LINE), 0);
    lv_obj_t *g = ui_label(c, &ui_font_16, C_REMOTE, glyph);
    lv_obj_align(g, LV_ALIGN_LEFT_MID, 16, 0);
    s_key_text[idx] = ui_label(c, idx == 1 ? &ui_font_20b : &ui_font_16, C_TEXT, "");
    lv_obj_align(s_key_text[idx], LV_ALIGN_CENTER, 8, 0);
    return c;
}

static void create(lv_obj_t *root)
{
    s_root = root;
    s_last = (ble_state_t)-1;

    lv_obj_t *head = ui_card(root, UI_PAD, 4, UI_W - 2 * UI_PAD, 50, C_SURFACE, 14);
    s_status_dot = ui_card(head, 14, 21, 8, 8, C_WARN, 4);
    s_status = ui_label_w(head, &ui_font_14, C_TEXT, UI_W - 2 * UI_PAD - 40, LV_TEXT_ALIGN_LEFT, "");
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_status, LV_ALIGN_LEFT_MID, 30, 0);

    lv_obj_t *tabs = ui_box(root, UI_PAD, 62, UI_W - 2 * UI_PAD, 28);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tabs, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < MODE_COUNT; i++) {
        s_tabs[i] = ui_pill(tabs, k_modes[i].name, C_TEXT2, C_SURFACE);
        lv_obj_set_style_pad_ver(s_tabs[i], 3, 0);
    }

    s_keys[0] = key_card(root, 100, 38, "▲", 0);
    s_keys[1] = key_card(root, 144, 50, "OK", 1);
    s_keys[2] = key_card(root, 200, 38, "▼", 2);
    s_extra = ui_label_w(root, &ui_font_14, C_TEXT3, UI_W - 2 * UI_PAD, LV_TEXT_ALIGN_CENTER, "");
    lv_obj_set_pos(s_extra, UI_PAD, 244);

    render_mode();
    shell_hints("长OK:换模式|长▲:断开返回");
    if (!ble_hid_start()) shell_toast("蓝牙启动失败");
}

static void destroy(void)
{
    ble_hid_stop();
    s_root = NULL;
}

static void flash(int idx)
{
    s_flash_until[idx] = sys_now_ms() + 160;
    lv_obj_set_style_bg_color(s_keys[idx], ui_c(C_REMOTE), 0);
    lv_obj_set_style_text_color(s_key_text[idx], ui_c(C_BG), 0);
}

static bool key(sys_key_t k, sys_key_ev_t ev)
{
    if (k == SYS_KEY_OK && ev == SYS_EV_LONG) {
        s_mode = (s_mode + 1) % MODE_COUNT;
        render_mode();
        shell_toast(k_modes[s_mode].name);
        return true;
    }
    if (ev != SYS_EV_CLICK && !(k == SYS_KEY_DOWN && ev == SYS_EV_LONG)) return false;
    const int idx = k == SYS_KEY_UP ? 0 : (k == SYS_KEY_OK ? 1 : 2);
    if (ble_hid_state() != BLE_CONNECTED) {
        shell_toast("还没连接,先在设备上配对");
        return true;
    }
    const bool lng = ev == SYS_EV_LONG;
    switch (s_mode) {
    case MODE_SLIDES:
        if (lng) ble_hid_key(KEY_B, 0);
        else ble_hid_key(idx == 0 ? KEY_LEFT : idx == 1 ? KEY_F5 : KEY_RIGHT, 0);
        break;
    case MODE_MEDIA:
        if (lng) ble_hid_consumer(CC_NEXT);
        else ble_hid_consumer(idx == 0 ? CC_VOL_UP : idx == 1 ? CC_PLAY : CC_VOL_DOWN);
        break;
    default:
        ble_hid_consumer(CC_VOL_UP);
        break;
    }
    flash(idx);
    return true;
}

static void tick(int64_t now)
{
    for (int i = 0; i < 3; i++) {
        if (s_flash_until[i] && now > s_flash_until[i]) {
            s_flash_until[i] = 0;
            lv_obj_set_style_bg_color(s_keys[i], ui_c(C_SURFACE), 0);
            lv_obj_set_style_text_color(s_key_text[i], ui_c(C_TEXT), 0);
        }
    }
    const ble_state_t st = ble_hid_state();
    if (st == s_last) return;
    s_last = st;
    char buf[80];
    uint32_t dot = C_WARN;
    switch (st) {
    case BLE_CONNECTED:
        snprintf(buf, sizeof(buf), "已连接 %s", ble_hid_peer());
        dot = C_OK;
        audio_beep(BEEP_OK);
        break;
    case BLE_ADVERTISING:
        snprintf(buf, sizeof(buf), "等待配对:在手机/电脑蓝牙里选 %s", g_settings.name);
        break;
    case BLE_FAILED:
        snprintf(buf, sizeof(buf), "蓝牙出错,返回后重试");
        dot = C_ERR;
        break;
    default:
        snprintf(buf, sizeof(buf), "正在启动蓝牙…");
        break;
    }
    lv_label_set_text(s_status, buf);
    lv_obj_set_style_bg_color(s_status_dot, ui_c(dot), 0);
}

const app_t APP_REMOTE = {
    .name = "蓝牙遥控",
    .desc = "翻页笔 · 媒体 · 拍照",
    .icon = &ICON_REMOTE,
    .color = C_REMOTE,
    .create = create,
    .destroy = destroy,
    .key = key,
    .tick = tick,
    .flags = APP_FLAG_KEYS_WHEN_OFF,
};
