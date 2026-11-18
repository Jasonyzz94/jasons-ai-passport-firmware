// main/app_wifi.c —— Wi-Fi 分析:信道占用图 + 推荐信道 + 热点列表(信号格)。
// 不需要服务器;只打开射频做被动扫描。
// 按键:▲▼ 浏览列表 · OK 重新扫描 · 长▲ 返回
#include "apps.h"
#include "icons.h"
#include "svc.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>

#define CHANNELS 13
#define ROW_H 34
#define LIST_Y 128

static lv_obj_t *s_root;
static lv_obj_t *s_bars[CHANNELS];
static lv_obj_t *s_counts[CHANNELS];
static lv_obj_t *s_best;
static lv_obj_t *s_list;
static lv_obj_t *s_rows[NET_SCAN_MAX];
static net_ap_t s_aps[NET_SCAN_MAX];
static int s_count;
static int s_focus;
static bool s_scanning;
static int64_t s_scan_at;

static uint32_t rssi_color(int rssi)
{
    if (rssi >= -55) return C_WIFI;
    if (rssi >= -67) return 0xA3E635;
    if (rssi >= -78) return C_WARN;
    return C_ERR;
}

static int rssi_level(int rssi)
{
    if (rssi >= -55) return 4;
    if (rssi >= -67) return 3;
    if (rssi >= -78) return 2;
    return 1;
}

static void focus_row(int idx)
{
    for (int i = 0; i < s_count; i++) {
        lv_obj_set_style_bg_color(s_rows[i], ui_c(i == idx ? ui_mix(C_WIFI, C_SURFACE, 40)
                                                         : C_SURFACE), 0);
        lv_obj_set_style_border_color(s_rows[i], ui_c(i == idx ? C_WIFI : C_SURFACE), 0);
    }
    if (idx >= 0 && idx < s_count) lv_obj_scroll_to_view(s_rows[idx], LV_ANIM_ON);
}

static void render(void)
{
    // 信道占用:每个热点按信号强度加权,并向相邻 ±2 信道扩散(2.4G 信道重叠)。
    int load[CHANNELS] = {0};
    int count[CHANNELS] = {0};
    for (int i = 0; i < s_count; i++) {
        const int ch = s_aps[i].channel;
        if (ch < 1 || ch > CHANNELS) continue;
        count[ch - 1]++;
        const int w = 100 + s_aps[i].rssi;   // -40 → 60,-90 → 10
        for (int d = -2; d <= 2; d++) {
            const int c = ch - 1 + d;
            if (c >= 0 && c < CHANNELS) load[c] += (w > 0 ? w : 1) * (d == 0 ? 3 : (d == 1 || d == -1 ? 2 : 1));
        }
    }
    int maxl = 1;
    for (int c = 0; c < CHANNELS; c++) if (load[c] > maxl) maxl = load[c];
    const int mine = net_state() == NET_CONNECTED ? net_channel() : 0;
    for (int c = 0; c < CHANNELS; c++) {
        const int h = 4 + 56 * load[c] / maxl;
        lv_obj_set_height(s_bars[c], h);
        lv_obj_set_y(s_bars[c], 64 - h);
        uint32_t color = c + 1 == mine ? C_WIFI : (load[c] * 3 > maxl * 2 ? C_ERR
                                                 : load[c] * 3 > maxl ? C_WARN : C_TEXT3);
        lv_obj_set_style_bg_color(s_bars[c], ui_c(color), 0);
        if ((c % 2 == 0) || c + 1 == mine) lv_label_set_text_fmt(s_counts[c], "%d", c + 1);
        else lv_label_set_text(s_counts[c], "");
        lv_obj_set_style_text_color(s_counts[c], ui_c(c + 1 == mine ? C_WIFI : C_TEXT3), 0);
    }
    // 推荐:1/6/11 三个不重叠信道里最空的那个
    const int cand[3] = {1, 6, 11};
    int best = 1;
    for (int i = 1; i < 3; i++) if (load[cand[i] - 1] < load[best - 1]) best = cand[i];
    lv_label_set_text_fmt(s_best, "%d 个热点 · 推荐信道 %d", s_count, best);

    lv_obj_clean(s_list);
    for (int i = 0; i < s_count; i++) {
        lv_obj_t *row = ui_card(s_list, 0, i * (ROW_H + 4), UI_W - 2 * UI_PAD, ROW_H, C_SURFACE, 10);
        lv_obj_set_style_border_width(row, 1, 0);
        s_rows[i] = row;
        // 信号格
        const int lvl = rssi_level(s_aps[i].rssi);
        for (int b = 0; b < 4; b++) {
            lv_obj_t *bar = ui_card(row, 10 + b * 5, 22 - (b + 1) * 4, 3, (b + 1) * 4,
                                    b < lvl ? rssi_color(s_aps[i].rssi) : C_LINE, 1);
            (void)bar;
        }
        lv_obj_t *name = ui_label_w(row, &ui_font_16, C_TEXT, 110, LV_TEXT_ALIGN_LEFT,
                                    s_aps[i].ssid[0] ? s_aps[i].ssid : "(隐藏网络)");
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 36, 0);
        char meta[24];
        snprintf(meta, sizeof(meta), "%s%d · %d", s_aps[i].secure ? "" : LV_SYMBOL_WARNING " ",
                 s_aps[i].channel, s_aps[i].rssi);
        lv_obj_t *m = ui_label(row, &ui_font_14, C_TEXT3, meta);
        lv_obj_align(m, LV_ALIGN_RIGHT_MID, -10, 0);
    }
    if (s_count == 0) {
        lv_obj_t *e = ui_label(s_list, &ui_font_16, C_TEXT3, s_scanning ? "正在扫描…" : "没有找到热点");
        lv_obj_align(e, LV_ALIGN_TOP_MID, 0, 30);
    }
    if (s_focus >= s_count) s_focus = s_count ? s_count - 1 : 0;
    focus_row(s_focus);
}

static void start_scan(void)
{
    if (net_scan_start()) {
        s_scanning = true;
        s_scan_at = sys_now_ms();
        lv_label_set_text(s_best, "正在扫描…");
    }
}

static void create(lv_obj_t *root)
{
    s_root = root;
    s_count = 0;
    s_focus = 0;
    s_scanning = false;
    memset(s_rows, 0, sizeof(s_rows));

    lv_obj_t *chart = ui_card(root, UI_PAD, 4, UI_W - 2 * UI_PAD, 92, C_SURFACE, 14);
    const int bw = 10, gap = (UI_W - 2 * UI_PAD - 20 - CHANNELS * bw) / (CHANNELS - 1);
    for (int c = 0; c < CHANNELS; c++) {
        const int x = 10 + c * (bw + gap);
        s_bars[c] = ui_card(chart, x, 60, bw, 4, C_TEXT3, 3);
        s_counts[c] = ui_label(chart, &ui_font_14, C_TEXT3, "");
        lv_obj_set_width(s_counts[c], bw + 12);
        lv_obj_set_style_text_align(s_counts[c], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_counts[c], x - 6, 68);
    }
    s_best = ui_label_w(root, &ui_font_14, C_TEXT2, UI_W - 2 * UI_PAD, LV_TEXT_ALIGN_CENTER, "");
    lv_obj_set_pos(s_best, UI_PAD, 102);

    s_list = ui_box(root, UI_PAD, LIST_Y, UI_W - 2 * UI_PAD, UI_CONTENT_H - LIST_Y);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    shell_hints("▲▼:浏览|OK:重新扫描|长▲:返回");
    net_acquire();
    s_count = 0;
    render();
    start_scan();
}

static void destroy(void)
{
    net_release();
    s_root = NULL;
}

static bool key(sys_key_t k, sys_key_ev_t ev)
{
    if (ev != SYS_EV_CLICK) return false;
    if (k == SYS_KEY_OK) {
        start_scan();
    } else if (s_count) {
        s_focus = (s_focus + (k == SYS_KEY_DOWN ? 1 : -1) + s_count) % s_count;
        focus_row(s_focus);
    }
    return true;
}

static void tick(int64_t now)
{
    if (!s_scanning) {
        // 射频刚开时扫描可能被拒,隔 2 秒重试一次;之后每 20 秒自动刷新。
        if (now - s_scan_at > 20000) start_scan();
        return;
    }
    const scan_state_t st = net_scan_state();
    if (st == SCAN_DONE) {
        s_scanning = false;
        s_count = net_scan_results(s_aps, NET_SCAN_MAX);
        render();
    } else if (st == SCAN_FAILED || now - s_scan_at > 8000) {
        s_scanning = false;
        s_scan_at = now - 18000;
        lv_label_set_text(s_best, "扫描失败,稍后重试");
    }
}

const app_t APP_WIFI = {
    .name = "Wi-Fi 分析",
    .desc = "信道占用 · 信号强度",
    .icon = &ICON_WIFI,
    .color = C_WIFI,
    .create = create,
    .destroy = destroy,
    .key = key,
    .tick = tick,
};
