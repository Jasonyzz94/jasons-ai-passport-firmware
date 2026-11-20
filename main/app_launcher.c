// main/app_launcher.c —— 启动器:竖向轮播,中间大卡片 + 上下预览,右侧位置刻度。
#include "apps.h"
#include "icons.h"
#include "settings.h"
#include "svc.h"
#include "ui.h"

#include <stdio.h>

const app_t *const APPS[] = {
    &APP_POMODORO, &APP_NOISE, &APP_CLOCK, &APP_AI, &APP_REMOTE, &APP_SOUND, &APP_LINK,
    &APP_HOME, &APP_BOARD, &APP_WIFI, &APP_TIMER, &APP_DICE, &APP_SETTINGS,
};
const int APP_COUNT = (int)(sizeof(APPS) / sizeof(APPS[0]));

#define CARD_X 22
#define CARD_Y 44
#define CARD_W 188
#define CARD_H 172


static void anim_y(void *obj, int32_t v) { lv_obj_set_y(obj, v); }
static int s_index;            // 跨进出保留
static lv_obj_t *s_card;
static lv_obj_t *s_icon;
static lv_obj_t *s_name;
static lv_obj_t *s_desc;
static lv_obj_t *s_prev_icon;
static lv_obj_t *s_prev_name;
static lv_obj_t *s_next_icon;
static lv_obj_t *s_next_name;
static lv_obj_t *s_dots[16];

static const app_t *at(int i) { return APPS[(i + APP_COUNT) % APP_COUNT]; }

static void peek_set(lv_obj_t *icon, lv_obj_t *name, const app_t *app)
{
    ui_pix_set(icon, app->icon);
    lv_label_set_text(name, app->name);
}

static void refresh(int dir)
{
    const app_t *app = at(s_index);
    const uint32_t c = app->color;
    lv_obj_set_style_bg_color(s_card, ui_c(ui_mix(c, C_SURFACE, 44)), 0);
    lv_obj_set_style_bg_grad_color(s_card, ui_c(C_SURFACE), 0);
    lv_obj_set_style_border_color(s_card, ui_c(ui_mix(c, C_SURFACE, 120)), 0);
    ui_pix_set(s_icon, app->icon);
    lv_label_set_text(s_name, app->name);
    lv_label_set_text(s_desc, app->desc);

    peek_set(s_prev_icon, s_prev_name, at(s_index - 1));
    peek_set(s_next_icon, s_next_name, at(s_index + 1));

    for (int i = 0; i < APP_COUNT; i++) {
        const bool on = (i == s_index);
        lv_obj_set_size(s_dots[i], 4, on ? 14 : 4);
        lv_obj_set_style_bg_color(s_dots[i], ui_c(on ? c : C_LINE), 0);
    }

    if (dir) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_card);
        lv_anim_set_values(&a, CARD_Y + dir * 22, CARD_Y);
        lv_anim_set_duration(&a, 170);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a, anim_y);
        lv_anim_start(&a);
        lv_obj_set_style_opa(s_card, LV_OPA_50, 0);
        lv_obj_fade_in(s_card, 170, 0);
    }
}

static void build_peek(lv_obj_t *root, int y, lv_obj_t **icon, lv_obj_t **name)
{
    lv_obj_t *row = ui_box(root, 0, y, UI_W, 28);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_style_opa(row, LV_OPA_60, 0);
    *icon = ui_pix(row, &ICON_GEAR, 1);
    *name = ui_label(row, &ui_font_16, C_TEXT3, "");
}

static void create(lv_obj_t *root)
{
    build_peek(root, 8, &s_prev_icon, &s_prev_name);

    s_card = ui_card(root, CARD_X, CARD_Y, CARD_W, CARD_H, C_SURFACE, 24);
    lv_obj_set_style_bg_grad_dir(s_card, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(s_card, 1, 0);

    s_icon = ui_pix(s_card, &ICON_GEAR, 5);
    lv_obj_align(s_icon, LV_ALIGN_TOP_MID, 0, 16);
    s_name = ui_label_w(s_card, &ui_font_20b, C_TEXT, CARD_W - 16, LV_TEXT_ALIGN_CENTER, "");
    lv_obj_align(s_name, LV_ALIGN_TOP_MID, 0, 106);
    s_desc = ui_label_w(s_card, &ui_font_16, C_TEXT2, CARD_W - 20, LV_TEXT_ALIGN_CENTER, "");
    lv_obj_align(s_desc, LV_ALIGN_TOP_MID, 0, 136);

    build_peek(root, CARD_Y + CARD_H + 10, &s_next_icon, &s_next_name);

    // 右侧位置刻度
    lv_obj_t *rail = ui_box(root, UI_W - 16, CARD_Y + 6, 6, CARD_H - 12);
    lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(rail, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(rail, 5, 0);
    for (int i = 0; i < APP_COUNT && i < 16; i++) {
        s_dots[i] = ui_card(rail, 0, 0, 4, 4, C_LINE, 2);
    }

    refresh(0);
    shell_hints("▲▼:选择|OK:打开");
}

static void destroy(void)
{
    s_card = NULL;
}

static bool key(sys_key_t k, sys_key_ev_t ev)
{
    if (ev == SYS_EV_CLICK && (k == SYS_KEY_UP || k == SYS_KEY_DOWN)) {
        const int dir = (k == SYS_KEY_DOWN) ? 1 : -1;
        s_index = (s_index + dir + APP_COUNT) % APP_COUNT;
        refresh(dir);
        return true;
    }
    if (k == SYS_KEY_OK && ev == SYS_EV_CLICK) {
        if (g_settings.sound_feedback) audio_beep(BEEP_CLICK);
        shell_open(at(s_index));
        return true;
    }
    if (k == SYS_KEY_OK && ev == SYS_EV_LONG) {
        shell_screen_off();
        return true;
    }
    return false;
}

const app_t APP_LAUNCHER = {
    .name = "工具箱",
    .desc = "",
    .icon = &ICON_GEAR,
    .color = C_SETTINGS,
    .create = create,
    .destroy = destroy,
    .key = key,
};
