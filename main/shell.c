// main/shell.c —— 工具箱外壳实现:状态栏、启动器、按键分发、息屏、浮层。
#include "shell.h"

#include "apps.h"
#include "icons.h"
#include "settings.h"
#include "svc.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>

#define TICK_MS          50
#define DIM_BEFORE_MS    6000      // 息屏前先调暗的时长
#define DIM_PERCENT      12
#define TOAST_MS         1800
#define MAX_BG_TICKS     6

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------
static lv_obj_t *s_screen;
static lv_obj_t *s_content;
static lv_obj_t *s_hint;
static lv_obj_t *s_time;
static lv_obj_t *s_chip;
static lv_obj_t *s_icons;
static lv_obj_t *s_batt_box;
static lv_obj_t *s_batt_fill;
static lv_obj_t *s_batt_text;
static lv_obj_t *s_toast;
static lv_obj_t *s_overlay;
static int64_t s_toast_until;

static const app_t *s_app;
static shell_bg_tick_t s_bg[MAX_BG_TICKS];
static int s_bg_count;
static shell_chip_fn_t s_chip_fn;

static bool s_screen_on = true;
static bool s_dimmed;
static bool s_swallow_until_release;
static bool s_keep_awake;
static int64_t s_last_input;

typedef enum { OVL_NONE = 0, OVL_ALERT, OVL_CONFIRM } overlay_kind_t;
static overlay_kind_t s_overlay_kind;
static shell_alert_cb_t s_alert_cb;
static shell_confirm_cb_t s_confirm_cb;

// ---------------------------------------------------------------------------
// 状态栏
// ---------------------------------------------------------------------------
static void bar_build(void)
{
    lv_obj_t *bar = ui_box(s_screen, 0, 0, UI_W, UI_BAR_H);

    s_time = ui_label(bar, &ui_font_16, C_TEXT, "");
    lv_obj_align(s_time, LV_ALIGN_LEFT_MID, 24, 2);

    s_chip = ui_pill(bar, "", C_BG, C_TOMATO);
    lv_obj_set_style_text_font(s_chip, &ui_font_14, 0);
    lv_obj_set_style_pad_hor(s_chip, 7, 0);
    lv_obj_set_style_pad_ver(s_chip, 1, 0);
    lv_obj_align(s_chip, LV_ALIGN_LEFT_MID, 72, 2);
    lv_obj_add_flag(s_chip, LV_OBJ_FLAG_HIDDEN);

    s_batt_box = ui_box(bar, 0, 0, 22, 11);
    lv_obj_set_style_border_width(s_batt_box, 1, 0);
    lv_obj_set_style_border_color(s_batt_box, ui_c(C_TEXT2), 0);
    lv_obj_set_style_radius(s_batt_box, 3, 0);
    lv_obj_align(s_batt_box, LV_ALIGN_RIGHT_MID, -26, 2);
    s_batt_fill = ui_card(s_batt_box, 2, 2, 10, 5, C_TEXT, 1);
    lv_obj_t *nub = ui_card(bar, 0, 0, 2, 5, C_TEXT2, 1);
    lv_obj_align_to(nub, s_batt_box, LV_ALIGN_OUT_RIGHT_MID, 1, 0);

    s_batt_text = ui_label(bar, &ui_font_16, C_TEXT2, "");
    lv_obj_align_to(s_batt_text, s_batt_box, LV_ALIGN_OUT_LEFT_MID, -5, 0);

    s_icons = ui_label(bar, &lv_font_montserrat_14, C_TEXT2, "");
    lv_obj_align_to(s_icons, s_batt_text, LV_ALIGN_OUT_LEFT_MID, -6, 0);
}

static void bar_refresh(int64_t now)
{
    static int64_t last_batt = -100000;
    static int last_min = -1;

    struct tm tm;
    if (time_local(&tm)) {
        if (tm.tm_min != last_min) {
            lv_label_set_text_fmt(s_time, "%02d:%02d", tm.tm_hour, tm.tm_min);
            last_min = tm.tm_min;
        }
    } else if (last_min != -2) {
        lv_label_set_text(s_time, "工具箱");
        last_min = -2;
    }

    if (now - last_batt > 20000) {
        last_batt = now;
        const int soc = sys_battery_soc();
        if (soc < 0) {
            lv_label_set_text(s_batt_text, "");
            lv_obj_set_width(s_batt_fill, 0);
        } else {
            lv_label_set_text_fmt(s_batt_text, "%d%%", soc);
            lv_obj_set_width(s_batt_fill, soc >= 100 ? 16 : (soc * 16 + 50) / 100);
            lv_obj_set_style_bg_color(s_batt_fill,
                                      ui_c(soc <= 15 ? C_ERR : (soc <= 30 ? C_WARN : C_TEXT)), 0);
        }
        lv_obj_align_to(s_batt_text, s_batt_box, LV_ALIGN_OUT_LEFT_MID, -5, 0);
    }

    // 无线状态图标
    char icons[24] = "";
    const net_state_t ns = net_state();
    if (ns == NET_CONNECTED) strcat(icons, LV_SYMBOL_WIFI);
    else if (ns == NET_CONNECTING) strcat(icons, LV_SYMBOL_REFRESH);
    const ble_state_t bs = ble_hid_state();
    if (bs == BLE_CONNECTED || bs == BLE_ADVERTISING) {
        if (icons[0]) strcat(icons, " ");
        strcat(icons, LV_SYMBOL_BLUETOOTH);
    }
    if (strcmp(lv_label_get_text(s_icons), icons) != 0) lv_label_set_text(s_icons, icons);
    lv_obj_set_style_text_color(s_icons, ui_c(bs == BLE_CONNECTED ? C_REMOTE : C_TEXT2), 0);
    if (lv_obj_has_flag(s_batt_text, LV_OBJ_FLAG_HIDDEN) || !lv_label_get_text(s_batt_text)[0]) {
        lv_obj_align_to(s_icons, s_batt_box, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    } else {
        lv_obj_align_to(s_icons, s_batt_text, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    }

    // 中间胶囊(后台番茄钟等)
    char chip[24];
    uint32_t color = C_TOMATO;
    if (s_chip_fn && s_chip_fn(chip, sizeof(chip), &color)) {
        if (strcmp(lv_label_get_text(s_chip), chip) != 0) lv_label_set_text(s_chip, chip);
        lv_obj_set_style_bg_color(s_chip, ui_c(color), 0);
        lv_obj_clear_flag(s_chip, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_batt_text, LV_OBJ_FLAG_HIDDEN);   // 腾出空间,电池图标仍显示电量
    } else {
        lv_obj_add_flag(s_chip, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_batt_text, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------------------------------------------------------------------------
// 底部按键提示
// ---------------------------------------------------------------------------
void shell_hints(const char *spec)
{
    if (!s_hint) return;
    lv_obj_clean(s_hint);
    if (!spec || !spec[0]) return;

    char buf[96];
    snprintf(buf, sizeof(buf), "%s", spec);
    char *save = NULL;
    for (char *item = strtok_r(buf, "|", &save); item; item = strtok_r(NULL, "|", &save)) {
        char *colon = strchr(item, ':');
        if (!colon) continue;
        *colon = '\0';
        lv_obj_t *grp = ui_box(s_hint, 0, 0, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(grp, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(grp, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(grp, 3, 0);
        lv_obj_t *key = ui_label(grp, &ui_font_14, C_TEXT, item);
        lv_obj_set_style_bg_color(key, ui_c(C_SURFACE2), 0);
        lv_obj_set_style_bg_opa(key, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(key, 5, 0);
        lv_obj_set_style_pad_hor(key, 3, 0);
        ui_label(grp, &ui_font_14, C_TEXT3, colon + 1);
    }
}

static void hint_build(void)
{
    s_hint = ui_box(s_screen, 0, UI_H - UI_HINT_H, UI_W, UI_HINT_H - 6);
    lv_obj_set_flex_flow(s_hint, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_hint, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_hint, 9, 0);
}

// ---------------------------------------------------------------------------
// 浮层:toast / 提醒 / 确认
// ---------------------------------------------------------------------------
void shell_toast(const char *text)
{
    if (!s_toast) {
        s_toast = ui_pill(lv_layer_top(), "", C_TEXT, C_SURFACE2);
        lv_obj_set_style_border_width(s_toast, 1, 0);
        lv_obj_set_style_border_color(s_toast, ui_c(C_LINE), 0);
        lv_obj_set_style_pad_ver(s_toast, 6, 0);
        lv_obj_set_style_pad_hor(s_toast, 14, 0);
    }
    lv_label_set_text(s_toast, text);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -UI_HINT_H - 8);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    s_toast_until = sys_now_ms() + TOAST_MS;
}

static void overlay_close(void)
{
    if (s_overlay) {
        lv_obj_delete(s_overlay);
        s_overlay = NULL;
    }
    s_overlay_kind = OVL_NONE;
}

// 动画回调签名是 (var, value);样式设置函数多一个 selector 参数,不能直接当回调用。
static void anim_border_opa(void *obj, int32_t v)
{
    lv_obj_set_style_border_opa(obj, (lv_opa_t)v, 0);
}

static void anim_y(void *obj, int32_t v) { lv_obj_set_y(obj, v); }

static lv_obj_t *overlay_new(uint32_t tint)
{
    overlay_close();
    s_overlay = ui_card(lv_layer_top(), 0, 0, UI_W, UI_H, ui_mix(tint, C_BG, 34), 0);
    return s_overlay;
}

void shell_alert(const pixel_sprite_t *icon, uint32_t color, const char *title,
                 const char *detail, shell_alert_cb_t on_close)
{
    lv_obj_t *o = overlay_new(color);
    s_overlay_kind = OVL_ALERT;
    s_alert_cb = on_close;

    lv_obj_t *ring = ui_card(o, 0, 0, 132, 132, ui_mix(color, C_BG, 70), 66);
    lv_obj_align(ring, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_set_style_border_width(ring, 3, 0);
    lv_obj_set_style_border_color(ring, ui_c(color), 0);
    if (icon) {
        lv_obj_t *pix = ui_pix(ring, icon, 5);
        lv_obj_center(pix);
    }
    // 呼吸动画:光环透明度往复
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ring);
    lv_anim_set_values(&a, LV_OPA_40, LV_OPA_COVER);
    lv_anim_set_duration(&a, 700);
    lv_anim_set_playback_duration(&a, 700);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, anim_border_opa);
    lv_anim_start(&a);

    lv_obj_t *t = ui_label_w(o, &ui_font_20b, C_TEXT, UI_W - 32, LV_TEXT_ALIGN_CENTER, title);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 196);
    if (detail) {
        lv_obj_t *d = ui_label(o, &ui_font_16, C_TEXT2, detail);
        lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(d, UI_W - 40);
        lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 228);
    }
    lv_obj_t *hint = ui_label(o, &ui_font_16, C_TEXT3, "按任意键继续");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -14);
    shell_wake();
}

bool shell_alert_active(void) { return s_overlay_kind != OVL_NONE; }

void shell_confirm(const char *title, const char *detail, shell_confirm_cb_t cb)
{
    lv_obj_t *o = overlay_new(C_BG);
    lv_obj_set_style_bg_opa(o, LV_OPA_80, 0);
    s_overlay_kind = OVL_CONFIRM;
    s_confirm_cb = cb;

    lv_obj_t *card = ui_card(o, 16, 0, UI_W - 32, 168, C_SURFACE, 18);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, ui_c(C_LINE), 0);
    lv_obj_center(card);
    lv_obj_t *t = ui_label_w(card, &ui_font_20b, C_TEXT, UI_W - 64, LV_TEXT_ALIGN_CENTER, title);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 18);
    if (detail) {
        lv_obj_t *d = ui_label(card, &ui_font_16, C_TEXT2, detail);
        lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(d, UI_W - 64);
        lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 52);
    }
    lv_obj_t *ok = ui_pill(card, "OK 确认", C_BG, C_TOMATO);
    lv_obj_set_style_pad_ver(ok, 6, 0);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_LEFT, 16, -16);
    lv_obj_t *no = ui_pill(card, "▲▼ 取消", C_TEXT, C_SURFACE2);
    lv_obj_set_style_pad_ver(no, 6, 0);
    lv_obj_align(no, LV_ALIGN_BOTTOM_RIGHT, -16, -16);
    shell_wake();
}

static bool overlay_key(sys_key_t key, sys_key_ev_t ev)
{
    if (s_overlay_kind == OVL_NONE) return false;
    if (ev != SYS_EV_CLICK && ev != SYS_EV_LONG) return true;
    if (s_overlay_kind == OVL_ALERT) {
        shell_alert_cb_t cb = s_alert_cb;
        s_alert_cb = NULL;
        overlay_close();
        if (cb) cb();
    } else {
        shell_confirm_cb_t cb = s_confirm_cb;
        s_confirm_cb = NULL;
        overlay_close();
        if (cb) cb(key == SYS_KEY_OK && ev == SYS_EV_CLICK);
    }
    return true;
}

// ---------------------------------------------------------------------------
// 屏幕与亮度
// ---------------------------------------------------------------------------
void shell_apply_brightness(void)
{
    if (s_screen_on && !s_dimmed) sys_backlight((uint8_t)g_settings.brightness);
}

void shell_wake(void)
{
    s_last_input = sys_now_ms();
    if (!s_screen_on || s_dimmed) {
        s_screen_on = true;
        s_dimmed = false;
        lv_obj_invalidate(lv_screen_active());
        sys_backlight((uint8_t)g_settings.brightness);
    }
}

void shell_screen_off(void)
{
    s_screen_on = false;
    s_dimmed = false;
    sys_backlight(0);
}

bool shell_screen_on(void) { return s_screen_on; }
void shell_keep_awake(bool on) { s_keep_awake = on; if (on) shell_wake(); }

static void screen_timeout_tick(int64_t now)
{
    const int timeout = settings_timeout_ms();
    if (!s_screen_on || timeout == 0 || s_keep_awake || s_overlay_kind == OVL_ALERT) return;
    if (s_app && (s_app->flags & APP_FLAG_KEEP_AWAKE)) return;
    const int64_t idle = now - s_last_input;
    if (idle >= timeout) {
        shell_screen_off();
    } else if (!s_dimmed && idle >= timeout - DIM_BEFORE_MS && timeout > DIM_BEFORE_MS * 2) {
        s_dimmed = true;
        sys_backlight(DIM_PERCENT);
    }
}

// ---------------------------------------------------------------------------
// 应用切换
// ---------------------------------------------------------------------------
const app_t *shell_current(void) { return s_app; }

static void content_enter_anim(int dir)
{
    lv_obj_set_style_opa(s_content, LV_OPA_TRANSP, 0);
    lv_obj_fade_in(s_content, 160, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_content);
    lv_anim_set_values(&a, UI_CONTENT_Y + dir * 18, UI_CONTENT_Y);
    lv_anim_set_duration(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, anim_y);
    lv_anim_start(&a);
}

static void switch_to(const app_t *app, int dir)
{
    if (s_app && s_app->destroy) s_app->destroy();
    lv_anim_delete(s_content, NULL);
    lv_obj_clean(s_content);
    lv_obj_set_y(s_content, UI_CONTENT_Y);
    shell_hints("");
    s_keep_awake = false;
    s_app = app;
    if (app->create) app->create(s_content);
    content_enter_anim(dir);
}

void shell_open(const app_t *app) { switch_to(app, 1); }
void shell_back(void) { switch_to(&APP_LAUNCHER, -1); }

void shell_add_bg_tick(shell_bg_tick_t fn)
{
    if (s_bg_count < MAX_BG_TICKS) s_bg[s_bg_count++] = fn;
}

void shell_set_chip(shell_chip_fn_t fn) { s_chip_fn = fn; }

// ---------------------------------------------------------------------------
// 主循环(lv_timer,运行于 LVGL 任务)
// ---------------------------------------------------------------------------
static void handle_key(sys_key_t key, sys_key_ev_t ev)
{
    const int64_t now = sys_now_ms();
    if (s_swallow_until_release) {
        if (ev == SYS_EV_RELEASE) s_swallow_until_release = false;
        s_last_input = now;
        return;
    }
    if ((!s_screen_on || s_dimmed) && s_app && (s_app->flags & APP_FLAG_KEYS_WHEN_OFF)
        && s_overlay_kind == OVL_NONE) {
        s_last_input = now;
        // 遥控器:不点亮屏幕,直接送达;未处理的长按 ▲ 用来点亮屏幕。
        const bool handled = s_app->key && s_app->key(key, ev);
        if (!handled && key == SYS_KEY_UP && ev == SYS_EV_LONG) shell_wake();
        return;
    }
    if (!s_screen_on || s_dimmed) {
        const bool was_off = !s_screen_on;
        shell_wake();
        // 从息屏唤醒:吞掉这次按键直到松手,避免误触发功能。调暗时同样只点亮。
        if (ev != SYS_EV_RELEASE) s_swallow_until_release = true;
        (void)was_off;
        return;
    }
    s_last_input = now;
    if (overlay_key(key, ev)) return;

    bool handled = s_app && s_app->key && s_app->key(key, ev);
    if (!handled && key == SYS_KEY_UP && ev == SYS_EV_LONG && s_app != &APP_LAUNCHER) {
        if (g_settings.sound_feedback) audio_beep(BEEP_CLICK);
        shell_back();
    }
}

static void shell_timer_cb(lv_timer_t *t)
{
    (void)t;
    sys_key_t key;
    sys_key_ev_t ev;
    while (sys_key_pop(&key, &ev)) handle_key(key, ev);

    const int64_t now = sys_now_ms();
    for (int i = 0; i < s_bg_count; i++) s_bg[i](now);
    if (s_app && s_app->tick) s_app->tick(now);
    if (s_toast && s_toast_until && now > s_toast_until) {
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        s_toast_until = 0;
    }
    screen_timeout_tick(now);
    if (s_screen_on) bar_refresh(now);
}

void shell_start(void)
{
    s_screen = lv_screen_active();
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_style_bg_color(s_screen, ui_c(C_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    s_content = ui_box(s_screen, 0, UI_CONTENT_Y, UI_W, UI_CONTENT_H);
    bar_build();
    hint_build();

    s_last_input = sys_now_ms();
    s_app = NULL;
    switch_to(&APP_LAUNCHER, 0);
    bar_refresh(sys_now_ms());
    lv_timer_create(shell_timer_cb, TICK_MS, NULL);
}
