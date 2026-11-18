// main/app_dice.c —— 随机:骰子(1~3 颗)、抛硬币、随机数 1-100、今天吃什么。
// 按键:▲▼ 切换玩法 · OK 摇一下(带滚动动画)· 长▼ 骰子数量 · 长▲ 返回
#include "apps.h"
#include "icons.h"
#include "svc.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>

typedef enum { MODE_DICE = 0, MODE_COIN, MODE_NUMBER, MODE_FOOD, MODE_COUNT } mode_t_;
static const char *const k_mode_names[MODE_COUNT] = {"骰子", "硬币", "随机数", "吃什么"};
static const char *const k_foods[] = {
    "火锅", "麻辣烫", "牛肉面", "寿司", "饺子", "汉堡", "烤肉", "螺蛳粉",
    "黄焖鸡", "沙拉", "披萨", "煲仔饭", "烧鹅饭", "酸菜鱼", "炸鸡", "米线",
};
#define FOOD_COUNT ((int)(sizeof(k_foods) / sizeof(k_foods[0])))

static int s_mode;
static int s_dice = 2;
static int s_values[3] = {3, 5, 1};
static int s_result;          // 硬币 0/1、随机数、食物下标
static int64_t s_roll_until;
static int64_t s_last_frame;

static lv_obj_t *s_root;
static lv_obj_t *s_stage;
static lv_obj_t *s_tabs[MODE_COUNT];
static lv_obj_t *s_caption;

static int rnd(int n) { return (int)(sys_random() % (uint32_t)n); }

// 骰子点位(3x3 网格编号 0..8)
static const uint16_t k_pips[7] = {
    0, 0x010, 0x101, 0x111, 0x145, 0x155, 0x16D,
};

static void draw_die(lv_obj_t *parent, int x, int y, int size, int value, bool rolling)
{
    lv_obj_t *d = ui_card(parent, x, y, size, size, rolling ? 0xE5E7EB : 0xFFFFFF, size / 5);
    lv_obj_set_style_shadow_width(d, 12, 0);
    lv_obj_set_style_shadow_color(d, ui_c(C_DICE), 0);
    lv_obj_set_style_shadow_opa(d, LV_OPA_40, 0);
    const int cell = size / 4, r = size / 7;
    for (int i = 0; i < 9; i++) {
        if (!(k_pips[value] & (1 << i))) continue;
        const int cx = cell * (1 + i % 3), cy = cell * (1 + i / 3);
        ui_card(d, cx - r / 2, cy - r / 2, r, r, value == 1 ? 0xEF4444 : 0x1F2937, r / 2);
    }
}

static void render(bool rolling)
{
    lv_obj_clean(s_stage);
    char buf[48];
    switch (s_mode) {
    case MODE_DICE: {
        const int size = s_dice == 1 ? 110 : (s_dice == 2 ? 84 : 62);
        const int gap = 14;
        const int total = s_dice * size + (s_dice - 1) * gap;
        int sum = 0;
        for (int i = 0; i < s_dice; i++) {
            draw_die(s_stage, (UI_W - total) / 2 + i * (size + gap), (150 - size) / 2 + (rolling && i % 2 ? 6 : 0),
                     size, s_values[i], rolling);
            sum += s_values[i];
        }
        if (rolling) snprintf(buf, sizeof(buf), "…");
        else if (s_dice > 1) snprintf(buf, sizeof(buf), "合计 %d 点", sum);
        else snprintf(buf, sizeof(buf), "%d 点", sum);
        break;
    }
    case MODE_COIN: {
        lv_obj_t *c = ui_card(s_stage, (UI_W - 120) / 2, 15, 120, 120, s_result ? 0xFBBF24 : 0xD1D5DB, 60);
        lv_obj_set_style_border_width(c, 6, 0);
        lv_obj_set_style_border_color(c, ui_c(s_result ? 0xD97706 : 0x9CA3AF), 0);
        lv_obj_t *t = ui_label(c, &ui_font_20b, 0x3F2D0A, rolling ? "?" : (s_result ? "正" : "反"));
        lv_obj_center(t);
        snprintf(buf, sizeof(buf), "%s", rolling ? "…" : (s_result ? "正面朝上" : "反面朝上"));
        break;
    }
    case MODE_NUMBER: {
        lv_obj_t *n = ui_label(s_stage, ui_font_64b, rolling ? C_TEXT3 : C_TEXT, "");
        lv_label_set_text_fmt(n, "%d", s_result);
        lv_obj_align(n, LV_ALIGN_CENTER, 0, 0);
        snprintf(buf, sizeof(buf), "1 ~ 100");
        break;
    }
    default: {
        lv_obj_t *card = ui_card(s_stage, UI_PAD + 10, 20, UI_W - 2 * UI_PAD - 20, 110,
                                 ui_mix(C_DICE, C_SURFACE, 50), 20);
        lv_obj_t *n = ui_label(card, &ui_font_20b, C_TEXT, k_foods[s_result]);
        lv_obj_center(n);
        snprintf(buf, sizeof(buf), "%s", rolling ? "让我想想…" : "就决定是你了!");
        break;
    }
    }
    lv_label_set_text(s_caption, buf);
}

static void render_tabs(void)
{
    for (int i = 0; i < MODE_COUNT; i++) {
        const bool on = i == s_mode;
        lv_obj_set_style_bg_color(s_tabs[i], ui_c(on ? C_DICE : C_SURFACE), 0);
        lv_obj_set_style_text_color(s_tabs[i], ui_c(on ? C_BG : C_TEXT2), 0);
    }
    shell_hints(s_mode == MODE_DICE ? "▲▼:玩法|OK:摇一摇|长▼:骰子数" : "▲▼:玩法|OK:摇一摇|长▲:返回");
}

static void randomize(void)
{
    for (int i = 0; i < 3; i++) s_values[i] = 1 + rnd(6);
    switch (s_mode) {
    case MODE_COIN: s_result = rnd(2); break;
    case MODE_NUMBER: s_result = 1 + rnd(100); break;
    case MODE_FOOD: s_result = rnd(FOOD_COUNT); break;
    default: break;
    }
}

static void create(lv_obj_t *root)
{
    s_root = root;
    s_roll_until = 0;
    lv_obj_t *tabs = ui_box(root, 0, 6, UI_W, 30);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tabs, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tabs, 6, 0);
    for (int i = 0; i < MODE_COUNT; i++) {
        s_tabs[i] = ui_pill(tabs, k_mode_names[i], C_TEXT2, C_SURFACE);
        lv_obj_set_style_text_font(s_tabs[i], &ui_font_14, 0);
        lv_obj_set_style_pad_ver(s_tabs[i], 4, 0);
    }
    s_stage = ui_box(root, 0, 52, UI_W, 150);
    s_caption = ui_label_w(root, &ui_font_20b, C_DICE, UI_W, LV_TEXT_ALIGN_CENTER, "");
    lv_obj_set_pos(s_caption, 0, 214);
    if (s_mode == MODE_NUMBER && !s_result) s_result = 42;
    render_tabs();
    render(false);
}

static void destroy(void) { s_root = NULL; }

static bool key(sys_key_t k, sys_key_ev_t ev)
{
    if (k == SYS_KEY_DOWN && ev == SYS_EV_LONG && s_mode == MODE_DICE) {
        s_dice = s_dice % 3 + 1;
        render(false);
        return true;
    }
    if (ev != SYS_EV_CLICK) return false;
    if (k == SYS_KEY_OK) {
        s_roll_until = sys_now_ms() + 700;
        s_last_frame = 0;
        return true;
    }
    s_mode = (s_mode + (k == SYS_KEY_DOWN ? 1 : -1) + MODE_COUNT) % MODE_COUNT;
    if (s_mode == MODE_NUMBER && !s_result) s_result = 42;
    if (s_mode == MODE_FOOD || s_mode == MODE_COIN) randomize();
    render_tabs();
    render(false);
    return true;
}

static void tick(int64_t now)
{
    if (!s_roll_until) return;
    if (now >= s_roll_until) {
        s_roll_until = 0;
        randomize();
        render(false);
        audio_beep(BEEP_OK);
        return;
    }
    if (now - s_last_frame >= 70) {
        s_last_frame = now;
        randomize();
        render(true);
        audio_beep(BEEP_CLICK);
    }
}

const app_t APP_DICE = {
    .name = "随机",
    .desc = "骰子 · 硬币 · 吃什么",
    .icon = &ICON_DICE,
    .color = C_DICE,
    .create = create,
    .destroy = destroy,
    .key = key,
    .tick = tick,
};
