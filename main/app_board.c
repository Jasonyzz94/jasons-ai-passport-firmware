// main/app_board.c —— 信息看板:服务器汇总的卡片(GitHub、汇率、日程、Mac 状态…),一屏一张。
// 卡片内容完全由 Mac mini 上的 passport-server 配置,设备只负责展示。
// 按键:▲▼ 翻卡 · OK 刷新 · 长▲ 返回
#include "apps.h"
#include "icons.h"
#include "svc.h"
#include "ui.h"

#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CARDS 10
#define REFRESH_MS (5 * 60 * 1000)

typedef struct {
    char title[32];
    char value[16];
    char unit[16];
    char sub[64];
    char icon[12];
    uint32_t color;
} card_t;


static void anim_y(void *obj, int32_t v) { lv_obj_set_y(obj, v); }
static card_t s_cards[MAX_CARDS];
static int s_count;
static int s_page;
static char s_updated[8];
static int64_t s_fetched_ms = -REFRESH_MS;

static lv_obj_t *s_root;
static lv_obj_t *s_view;
static lv_obj_t *s_dots;
static int s_req = -1;

static const pixel_sprite_t *icon_named(const char *n)
{
    if (!strcmp(n, "bell")) return &IC_BELL;
    if (!strcmp(n, "coin")) return &IC_COIN;
    if (!strcmp(n, "check")) return &IC_CHECK;
    if (!strcmp(n, "heart")) return &IC_HEART;
    if (!strcmp(n, "chat")) return &IC_CHAT;
    if (!strcmp(n, "music")) return &IC_MUSIC;
    if (!strcmp(n, "weather")) return &WX_PARTLY;
    if (!strcmp(n, "home")) return &ICON_HOME;
    return &IC_STAR;
}

static void parse(const char *body)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) return;
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "cards");
    s_count = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (s_count >= MAX_CARDS) break;
        card_t *c = &s_cards[s_count++];
        memset(c, 0, sizeof(*c));
        const cJSON *v;
#define STR(field, key) \
        v = cJSON_GetObjectItemCaseSensitive(it, key); \
        if (cJSON_IsString(v)) snprintf(c->field, sizeof(c->field), "%s", v->valuestring)
        STR(title, "title");
        STR(value, "value");
        STR(unit, "unit");
        STR(sub, "sub");
        STR(icon, "icon");
#undef STR
        v = cJSON_GetObjectItemCaseSensitive(it, "color");
        c->color = cJSON_IsString(v) ? (uint32_t)strtoul(v->valuestring, NULL, 16) : C_BOARD;
    }
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(root, "updated");
    snprintf(s_updated, sizeof(s_updated), "%s", cJSON_IsString(u) ? u->valuestring : "");
    cJSON_Delete(root);
}

static void render(int dir)
{
    lv_obj_clean(s_view);
    lv_obj_clean(s_dots);
    if (s_count == 0) {
        if (relay_state() != SRV_OK) {
            ui_empty(s_view, &IC_CLOUD_OFF, "找不到中转服务器", "看板内容由家里的服务器提供");
        } else {
            ui_empty(s_view, &ICON_BOARD, s_req >= 0 ? "正在加载…" : "还没有卡片",
                     s_req >= 0 ? NULL : "在服务器配置文件里添加卡片");
        }
        return;
    }
    if (s_page >= s_count) s_page = 0;
    const card_t *c = &s_cards[s_page];

    lv_obj_t *card = ui_card(s_view, 0, 0, UI_W - 2 * UI_PAD, 206, C_SURFACE, 22);
    lv_obj_set_style_bg_color(card, ui_c(ui_mix(c->color, C_SURFACE, 46)), 0);
    lv_obj_set_style_bg_grad_color(card, ui_c(C_SURFACE), 0);
    lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, ui_c(ui_mix(c->color, C_SURFACE, 110)), 0);

    lv_obj_t *icon = ui_pix(card, icon_named(c->icon), 2);
    lv_obj_set_pos(icon, 16, 16);
    lv_obj_t *t = ui_label_w(card, &ui_font_16, C_TEXT2, 150, LV_TEXT_ALIGN_LEFT, c->title);
    lv_obj_set_pos(t, 58, 22);

    lv_obj_t *val = ui_label(card, ui_font_64b, C_TEXT, c->value);
    if (lv_obj_get_style_text_font(val, 0) == ui_font_64b && strlen(c->value) > 5) {
        lv_obj_set_style_text_font(val, ui_font_36b, 0);
    }
    lv_obj_set_pos(val, 16, 62);
    lv_obj_t *unit = ui_label(card, &ui_font_16, c->color, c->unit);
    lv_obj_align_to(unit, val, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -12);

    lv_obj_t *sub = ui_label(card, &ui_font_16, C_TEXT, c->sub);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(sub, UI_W - 2 * UI_PAD - 32);
    lv_obj_set_pos(sub, 16, 150);

    if (s_updated[0]) {
        lv_obj_t *u = ui_label(s_view, &ui_font_14, C_TEXT3, "");
        lv_label_set_text_fmt(u, "更新于 %s", s_updated);
        lv_obj_align(u, LV_ALIGN_BOTTOM_RIGHT, -4, 0);
    }
    for (int i = 0; i < s_count; i++) {
        ui_card(s_dots, 0, 0, i == s_page ? 14 : 5, 5, i == s_page ? c->color : C_LINE, 3);
    }
    if (dir) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, card);
        lv_anim_set_values(&a, dir * 20, 0);
        lv_anim_set_duration(&a, 160);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a, anim_y);
        lv_anim_start(&a);
    }
}

static void request(void)
{
    if (s_req < 0 && relay_state() == SRV_OK) s_req = relay_request("/api/board", NULL);
}

static void create(lv_obj_t *root)
{
    s_root = root;
    s_req = -1;
    s_view = ui_box(root, UI_PAD, 6, UI_W - 2 * UI_PAD, 230);
    s_dots = ui_box(root, 0, 238, UI_W, 12);
    lv_obj_set_flex_flow(s_dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_dots, 5, 0);
    shell_hints("▲▼:翻页|OK:刷新|长▲:返回");
    net_acquire();
    if (sys_now_ms() - s_fetched_ms >= REFRESH_MS) request();
    render(0);
}

static void destroy(void)
{
    if (s_req >= 0) relay_free(s_req);
    s_req = -1;
    net_release();
    s_root = NULL;
}

static bool key(sys_key_t k, sys_key_ev_t ev)
{
    if (ev != SYS_EV_CLICK) return false;
    if (k == SYS_KEY_OK) {
        if (relay_state() != SRV_OK) relay_discover();
        request();
        shell_toast("正在刷新");
        return true;
    }
    if (s_count) {
        const int dir = k == SYS_KEY_DOWN ? 1 : -1;
        s_page = (s_page + dir + s_count) % s_count;
        render(dir);
    }
    return true;
}

static void tick(int64_t now)
{
    if (s_req < 0) {
        static srv_state_t last;
        const srv_state_t st = relay_state();
        if (st != last) {
            last = st;
            if (!s_count) render(0);
        }
        if (now - s_fetched_ms >= REFRESH_MS) request();
        return;
    }
    const char *body;
    int status;
    const req_state_t st = relay_poll(s_req, &body, &status);
    if (st == REQ_BUSY) return;
    if (st == REQ_DONE && status == 200) {
        parse(body);
        s_fetched_ms = now;
    } else {
        s_fetched_ms = now - REFRESH_MS + 60000;
        if (s_count) shell_toast("刷新失败");
    }
    relay_free(s_req);
    s_req = -1;
    render(0);
}

const app_t APP_BOARD = {
    .name = "信息看板",
    .desc = "通知 · 汇率 · 日程",
    .icon = &ICON_BOARD,
    .color = C_BOARD,
    .create = create,
    .destroy = destroy,
    .key = key,
    .tick = tick,
};
