// main/app_dice.c —— 随机:9 种玩法。
//   骰子(1~3 颗)· 硬币 · 随机数 · 吃什么 · 喝什么 · 答案之书 · 今日运势 · 石头剪刀布 · 做点什么
// 按键:▲▼ 换玩法 · 确定 摇一下(带滚动动画)· 长▼ 骰子数量 · 长▲ 返回
// 今日运势按日期和设备固定:同一天怎么抽都是同一签,第二天才变。
#include "apps.h"
#include "icons.h"
#include "pomodoro.h"
#include "svc.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>

typedef enum {
    MODE_DICE = 0, MODE_COIN, MODE_NUMBER, MODE_FOOD, MODE_DRINK, MODE_ANSWER, MODE_FORTUNE,
    MODE_RPS, MODE_ACTIVITY, MODE_COUNT,
} mode_t_;

static const char *const k_mode_names[MODE_COUNT] = {
    "骰子", "硬币", "随机数", "吃什么", "喝什么", "答案之书", "今日运势", "石头剪刀布", "做点什么",
};

#define LIST(name, ...) static const char *const name[] = {__VA_ARGS__}
#define LEN(a) ((int)(sizeof(a) / sizeof(a[0])))
LIST(k_foods, "火锅", "麻辣烫", "牛肉面", "寿司", "饺子", "汉堡", "烤肉", "螺蛳粉", "黄焖鸡", "沙拉",
     "披萨", "煲仔饭", "烧鹅饭", "酸菜鱼", "炸鸡", "米线", "盖浇饭", "馄饨", "咖喱饭", "麻辣香锅");
LIST(k_drinks, "奶茶", "美式咖啡", "拿铁", "柠檬茶", "鲜榨果汁", "豆浆", "温白开", "气泡水", "酸梅汤",
     "绿茶", "椰子水", "热可可");
LIST(k_answers, "去做吧", "再等等", "当然可以", "最好不要", "换个角度想想", "现在就是时候", "问问身边的人",
     "答案在你心里", "大胆一点", "先睡一觉再说", "不必犹豫", "顺其自然", "值得一试", "放手吧");
LIST(k_activities, "散步二十分钟", "跑步", "看一部电影", "逛公园", "骑车兜风", "打一场球", "给自己做顿饭",
     "拼图", "去博物馆", "爬山", "睡个懒觉", "学一首歌", "整理房间", "给老朋友打电话");
LIST(k_fortunes, "大吉", "中吉", "小吉", "吉", "末吉", "小凶");
LIST(k_good, "写代码", "健身", "读书", "早睡", "学新东西", "整理房间", "联系老朋友", "喝水", "散步", "专注");
LIST(k_bad, "熬夜", "冲动消费", "拖延", "开长会", "喝冰水", "改需求", "刷手机", "吃夜宵", "生闷气", "久坐");
LIST(k_hands, "石头", "剪刀", "布");

static int s_mode;
static int s_dice = 2;
static int s_values[3] = {3, 5, 1};
static int s_result;            // 列表类玩法的下标 / 随机数 / 硬币
static int s_a, s_b;            // 运势的宜 / 忌,或猜拳双方
static int64_t s_roll_until;
static int64_t s_last_frame;

static lv_obj_t *s_root;
static lv_obj_t *s_stage;
static lv_obj_t *s_title;
static lv_obj_t *s_dots[MODE_COUNT];
static lv_obj_t *s_caption;

static int rnd(int n) { return (int)(sys_random() % (uint32_t)n); }

// 骰子点位(3x3 网格编号 0..8)
static const uint16_t k_pips[7] = {0, 0x010, 0x101, 0x111, 0x145, 0x155, 0x16D};

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

// 大字卡片(列表类玩法共用)
static lv_obj_t *word_card(const char *text, uint32_t tint)
{
    lv_obj_t *card = ui_card(s_stage, UI_PAD + 6, 14, UI_W - 2 * UI_PAD - 12, 120, ui_mix(tint, C_SURFACE, 50), 20);
    lv_obj_t *n = ui_label(card, &ui_font_20b, C_TEXT, text);
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(n, UI_W - 2 * UI_PAD - 40);
    lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(n);
    return card;
}

// 今日运势:同一天、同一台设备结果固定
static void fortune_of_today(void)
{
    struct tm tm;
    uint32_t seed = 0x2545F491u;
    if (time_local(&tm)) seed ^= (uint32_t)pomo_day_number(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday) * 2654435761u;
    for (const char *p = sys_short_id(); *p; p++) seed = seed * 31u + (uint8_t)*p;
    seed ^= seed >> 16;
    // 大吉、中吉更常见一些,小凶少见
    static const uint8_t weights[] = {3, 4, 4, 4, 2, 1};
    int total = 0;
    for (int i = 0; i < LEN(weights); i++) total += weights[i];
    int pick = (int)(seed % (uint32_t)total);
    s_result = 0;
    while (pick >= weights[s_result]) pick -= weights[s_result++];
    s_a = (int)((seed >> 8) % (uint32_t)LEN(k_good));
    s_b = (int)((seed >> 20) % (uint32_t)LEN(k_bad));
}

static void render(bool rolling)
{
    lv_obj_clean(s_stage);
    char buf[64] = "";
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
        else snprintf(buf, sizeof(buf), s_dice > 1 ? "合计 %d 点" : "%d 点", sum);
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
    case MODE_FOOD:
        word_card(k_foods[s_result % LEN(k_foods)], C_DICE);
        snprintf(buf, sizeof(buf), "%s", rolling ? "让我想想…" : "就决定是你了!");
        break;
    case MODE_DRINK:
        word_card(k_drinks[s_result % LEN(k_drinks)], C_BREAK);
        snprintf(buf, sizeof(buf), "%s", rolling ? "让我想想…" : "来一杯吧");
        break;
    case MODE_ANSWER:
        word_card(rolling ? "…" : k_answers[s_result % LEN(k_answers)], C_AI);
        snprintf(buf, sizeof(buf), "%s", rolling ? "翻书中…" : "心里想着问题,再按一次");
        break;
    case MODE_ACTIVITY:
        word_card(k_activities[s_result % LEN(k_activities)], C_SOUND);
        snprintf(buf, sizeof(buf), "%s", rolling ? "让我想想…" : "现在就去!");
        break;
    case MODE_FORTUNE: {
        lv_obj_t *card = ui_card(s_stage, UI_PAD + 6, 6, UI_W - 2 * UI_PAD - 12, 140,
                                 ui_mix(C_TOMATO, C_SURFACE, 50), 20);
        lv_obj_t *lvl = ui_label(card, &ui_font_20b, C_WARN, rolling ? "…" : k_fortunes[s_result]);
        lv_obj_align(lvl, LV_ALIGN_TOP_MID, 0, 16);
        if (!rolling) {
            char line[48];
            snprintf(line, sizeof(line), "宜 · %s", k_good[s_a]);
            lv_obj_t *g = ui_label(card, &ui_font_16, C_OK, line);
            lv_obj_align(g, LV_ALIGN_TOP_MID, 0, 62);
            snprintf(line, sizeof(line), "忌 · %s", k_bad[s_b]);
            lv_obj_t *b = ui_label(card, &ui_font_16, C_ERR, line);
            lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 92);
        }
        snprintf(buf, sizeof(buf), "%s", rolling ? "摇签中…" : "每天一签,明天再来");
        break;
    }
    default: {   // 石头剪刀布:甲乙两手
        for (int side = 0; side < 2; side++) {
            const int x = side ? UI_W / 2 + 6 : UI_PAD + 6;
            lv_obj_t *c = ui_card(s_stage, x, 20, UI_W / 2 - UI_PAD - 12, 110, C_SURFACE, 18);
            lv_obj_t *who = ui_label(c, &ui_font_14, C_TEXT3, side ? "乙" : "甲");
            lv_obj_align(who, LV_ALIGN_TOP_MID, 0, 10);
            lv_obj_t *h = ui_label(c, &ui_font_20b, C_TEXT, rolling ? "…" : k_hands[side ? s_b : s_a]);
            lv_obj_align(h, LV_ALIGN_CENTER, 0, 8);
        }
        if (rolling) snprintf(buf, sizeof(buf), "石头 剪刀 布!");
        else if (s_a == s_b) snprintf(buf, sizeof(buf), "平局,再来");
        else snprintf(buf, sizeof(buf), "%s 赢了", (s_a + 1) % 3 == s_b ? "甲" : "乙");
        break;
    }
    }
    lv_label_set_text(s_caption, buf);
}

static void render_header(void)
{
    lv_label_set_text_fmt(s_title, "‹  %s  ›", k_mode_names[s_mode]);
    for (int i = 0; i < MODE_COUNT; i++) {
        lv_obj_set_size(s_dots[i], i == s_mode ? 12 : 5, 5);
        lv_obj_set_style_bg_color(s_dots[i], ui_c(i == s_mode ? C_DICE : C_LINE), 0);
    }
    shell_hints(s_mode == MODE_DICE ? "▲▼:玩法|OK:摇一摇|长▼:骰子数" : "▲▼:玩法|OK:摇一摇|长▲:返回");
}

static void randomize(void)
{
    for (int i = 0; i < 3; i++) s_values[i] = 1 + rnd(6);
    switch (s_mode) {
    case MODE_COIN: s_result = rnd(2); break;
    case MODE_NUMBER: s_result = 1 + rnd(100); break;
    case MODE_FOOD: s_result = rnd(LEN(k_foods)); break;
    case MODE_DRINK: s_result = rnd(LEN(k_drinks)); break;
    case MODE_ANSWER: s_result = rnd(LEN(k_answers)); break;
    case MODE_ACTIVITY: s_result = rnd(LEN(k_activities)); break;
    case MODE_RPS: s_a = rnd(3); s_b = rnd(3); break;
    case MODE_FORTUNE: s_result = rnd(LEN(k_fortunes)); break;   // 动画用,结束时换成当天的签
    default: break;
    }
}

static void create(lv_obj_t *root)
{
    s_root = root;
    s_roll_until = 0;
    s_title = ui_label_w(root, &ui_font_20b, C_TEXT, UI_W, LV_TEXT_ALIGN_CENTER, "");
    lv_obj_set_pos(s_title, 0, 6);
    lv_obj_t *dots = ui_box(root, 0, 36, UI_W, 8);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dots, 4, 0);
    for (int i = 0; i < MODE_COUNT; i++) s_dots[i] = ui_card(dots, 0, 0, 5, 5, C_LINE, 3);
    s_stage = ui_box(root, 0, 52, UI_W, 150);
    s_caption = ui_label_w(root, &ui_font_20b, C_DICE, UI_W, LV_TEXT_ALIGN_CENTER, "");
    lv_obj_set_pos(s_caption, 0, 214);
    if (s_mode == MODE_NUMBER && !s_result) s_result = 42;
    if (s_mode == MODE_FORTUNE) fortune_of_today();
    render_header();
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
    s_roll_until = 0;
    randomize();
    if (s_mode == MODE_NUMBER && !s_result) s_result = 42;
    if (s_mode == MODE_FORTUNE) fortune_of_today();
    render_header();
    render(false);
    return true;
}

static void tick(int64_t now)
{
    if (!s_roll_until) return;
    if (now >= s_roll_until) {
        s_roll_until = 0;
        randomize();
        if (s_mode == MODE_FORTUNE) fortune_of_today();
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
    .desc = "骰子 · 运势 · 吃什么",
    .icon = &ICON_DICE,
    .color = C_DICE,
    .create = create,
    .destroy = destroy,
    .key = key,
    .tick = tick,
};
