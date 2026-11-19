// main/app_pomodoro.c —— 番茄钟:后台计时服务 + 环形进度主屏 + 快捷菜单 / 设置 / 统计。
//
// 计时在 shell 的后台 tick 里跑,退出到启动器或其他工具也不停;状态栏中间显示剩余时间,
// 到点时任何界面都会弹出全屏提醒并响铃(未确认时每 30 秒再提醒一次)。
//
// 主屏按键:OK 开始/暂停 · READY 时 ▲▼ ±1 分钟 · 长按 OK 快捷菜单 · 长按 ▼ 息屏 · 长按 ▲ 返回
#include "apps.h"
#include "icons.h"
#include "pixel_art.h"
#include "pomodoro.h"
#include "settings.h"
#include "svc.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>

#define THEME_NAMES_COUNT PIXEL_THEME_COUNT
static const char *const k_theme_names[THEME_NAMES_COUNT] = {"果园", "猫咪", "太空"};
static const char *const k_weekdays[7] = {"日", "一", "二", "三", "四", "五", "六"};

// ===========================================================================
// 服务(后台)
// ===========================================================================
static pomo_t s_pomo;
static pomo_stats_t s_stats;
static int s_theme;
static int32_t s_today = -1;        // 当前日序号(对时后有效)
static bool s_alert_pending;
static int64_t s_alert_next_ms;

static void svc_save(void)
{
    sys_kv_set_i32("p_work", s_pomo.work_min);
    sys_kv_set_i32("p_brk", s_pomo.break_min);
    sys_kv_set_i32("p_long", s_pomo.long_min);
    sys_kv_set_i32("p_every", s_pomo.long_every);
    sys_kv_set_i32("p_auto", s_pomo.auto_next);
    sys_kv_set_i32("p_theme", s_theme);
    sys_kv_set_i32("p_done", s_pomo.done);
    sys_kv_set_i32("p_day", s_today);
    sys_kv_set_blob("p_stats", &s_stats, sizeof(s_stats));
}

static int32_t today_number(void)
{
    struct tm tm;
    if (!time_local(&tm)) return -1;
    return pomo_day_number(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

static const pixel_sprite_t *phase_sprite(int frame)
{
    const pixel_theme_t *t = &PIXEL_THEMES[s_theme];
    return s_pomo.phase == POMO_PHASE_WORK ? t->focus[frame & 1] : t->rest[frame & 1];
}

static uint32_t phase_color(void)
{
    return s_pomo.phase == POMO_PHASE_WORK ? C_TOMATO : C_BREAK;
}

static void alert_closed(void) { s_alert_pending = false; }

static void raise_alert(pomo_event_t ev)
{
    char detail[64];
    if (ev == POMO_EVENT_WORK_DONE) {
        snprintf(detail, sizeof(detail), "今天第 %d 个番茄\n%s %d 分钟已开始", s_pomo.done,
                 pomo_phase_name(s_pomo.phase), pomo_minutes(&s_pomo, s_pomo.phase));
        shell_alert(PIXEL_THEMES[s_theme].focus[0], C_TOMATO, "专注完成！", detail, alert_closed);
        audio_beep(BEEP_WORK_DONE);
    } else {
        snprintf(detail, sizeof(detail), s_pomo.auto_next ? "下一个专注已自动开始"
                                                          : "准备好了就按确定键开始专注");
        shell_alert(PIXEL_THEMES[s_theme].rest[0], C_BREAK, "休息结束", detail, alert_closed);
        audio_beep(BEEP_BREAK_DONE);
    }
    s_alert_pending = true;
    s_alert_next_ms = sys_now_ms() + 30000;
}

static void svc_tick(int64_t now)
{
    // 跨天:累计完成数清零。
    const int32_t day = today_number();
    if (day >= 0 && day != s_today) {
        if (s_today >= 0) s_pomo.done = 0;
        s_today = day;
        svc_save();
    }

    const pomo_event_t ev = pomo_tick(&s_pomo, now);
    if (ev != POMO_EVENT_NONE) {
        if (ev == POMO_EVENT_WORK_DONE && s_today >= 0) {
            pomo_stats_add(&s_stats, s_today, s_pomo.work_min);
        }
        if (ev == POMO_EVENT_WORK_DONE) s_theme = (s_theme + 1) % PIXEL_THEME_COUNT;
        svc_save();
        raise_alert(ev);
    }
    // 未确认的提醒每 30 秒再响一次(最多持续到下次确认)。
    if (s_alert_pending && shell_alert_active() && now >= s_alert_next_ms) {
        audio_beep(BEEP_MSG);
        s_alert_next_ms = now + 30000;
    }
}

static bool svc_chip(char *text, size_t len, uint32_t *color)
{
    if (shell_current() == &APP_POMODORO || s_pomo.state == POMO_STATE_READY) return false;
    const int64_t sec = (pomo_remaining_ms(&s_pomo, sys_now_ms()) + 999) / 1000;
    const bool paused = s_pomo.state == POMO_STATE_PAUSED;
    snprintf(text, len, "%02d:%02d", (int)(sec / 60), (int)(sec % 60));
    *color = paused ? C_TEXT3 : phase_color();
    return true;
}

void pomo_service_init(void)
{
    pomo_init(&s_pomo, sys_kv_get_i32("p_work", 25), sys_kv_get_i32("p_brk", 5));
    pomo_set_minutes(&s_pomo, POMO_PHASE_LONG, sys_kv_get_i32("p_long", 15));
    s_pomo.long_every = sys_kv_get_i32("p_every", 4);
    if (s_pomo.long_every < 2 || s_pomo.long_every > 8) s_pomo.long_every = 4;
    s_pomo.auto_next = sys_kv_get_i32("p_auto", 0) != 0;
    s_pomo.done = sys_kv_get_i32("p_done", 0);
    s_today = sys_kv_get_i32("p_day", -1);
    s_theme = sys_kv_get_i32("p_theme", 0);
    if (s_theme < 0 || s_theme >= PIXEL_THEME_COUNT) s_theme = 0;
    if (!sys_kv_get_blob("p_stats", &s_stats, sizeof(s_stats))) memset(&s_stats, 0, sizeof(s_stats));
    shell_add_bg_tick(svc_tick);
    shell_set_chip(svc_chip);
}

// 供"设备互联"同步专注:对方开始专注时调用。
void pomo_remote_start(int minutes)
{
    if (s_pomo.state == POMO_STATE_RUNNING) return;
    s_pomo.phase = POMO_PHASE_WORK;
    pomo_set_minutes(&s_pomo, POMO_PHASE_WORK, minutes);
    pomo_reset(&s_pomo);
    pomo_toggle(&s_pomo, sys_now_ms());
}

int pomo_today_done(void) { return s_pomo.done; }

bool pomo_running_focus(int *minutes)
{
    if (s_pomo.state != POMO_STATE_RUNNING || s_pomo.phase != POMO_PHASE_WORK) return false;
    if (minutes) *minutes = s_pomo.work_min;
    return true;
}

// ===========================================================================
// 界面
// ===========================================================================
typedef enum { PAGE_MAIN = 0, PAGE_MENU, PAGE_SETTINGS, PAGE_STATS } page_t;

static lv_obj_t *s_root;
static lv_obj_t *s_page;
static page_t s_page_kind;

// 主屏控件
static lv_obj_t *s_arc;
static lv_obj_t *s_sprite;
static lv_obj_t *s_time;
static lv_obj_t *s_phase;
static lv_obj_t *s_cycle[8];
static lv_obj_t *s_today_lbl;
static int64_t s_last_sec = -1;
static int s_last_state = -1;
static int s_last_phase = -1;
static int s_last_frame = -1;
static int s_last_done = -1;

// 列表页
static ui_list_t s_list;
static bool s_editing;

static void build_page(page_t page);

static void main_hints(void)
{
    if (s_pomo.state == POMO_STATE_READY) shell_hints("▲▼:时长|OK:开始|长OK:菜单");
    else if (s_pomo.state == POMO_STATE_RUNNING) shell_hints("OK:暂停|长OK:菜单|长▼:息屏");
    else shell_hints("OK:继续|长OK:菜单|长▲:返回");
}

static void main_refresh(bool force)
{
    const int64_t now = sys_now_ms();
    const int64_t remaining = pomo_remaining_ms(&s_pomo, now);
    const int64_t sec = (remaining + 999) / 1000;
    const uint32_t color = phase_color();
    const bool running = s_pomo.state == POMO_STATE_RUNNING;

    if (force || (int)s_pomo.phase != s_last_phase || (int)s_pomo.state != s_last_state) {
        const bool paused = s_pomo.state == POMO_STATE_PAUSED;
        lv_obj_set_style_arc_color(s_arc, ui_c(paused ? C_TEXT3 : color), LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(s_arc, ui_c(ui_mix(color, C_BG, 40)), LV_PART_MAIN);
        lv_obj_set_style_text_color(s_time, ui_c(paused ? C_TEXT2 : C_TEXT), 0);
        char buf[40];
        if (paused) snprintf(buf, sizeof(buf), "已暂停");
        else if (s_pomo.state == POMO_STATE_READY && s_pomo.phase == POMO_PHASE_WORK)
            snprintf(buf, sizeof(buf), "准备专注");
        else snprintf(buf, sizeof(buf), "%s中", pomo_phase_name(s_pomo.phase));
        lv_label_set_text(s_phase, buf);
        lv_obj_set_style_text_color(s_phase, ui_c(paused ? C_TEXT3 : color), 0);
        lv_obj_set_style_opa(s_sprite, paused ? LV_OPA_50 : LV_OPA_COVER, 0);
        main_hints();
        s_last_frame = -1;
    }

    // 运行时精灵每 500ms 换帧并轻跳。
    const int frame = running ? (int)((now / 500) & 1) : 0;
    if (frame != s_last_frame) {
        ui_pix_set(s_sprite, phase_sprite(frame));
        lv_obj_align(s_sprite, LV_ALIGN_TOP_MID, 0, 34 - (running ? frame * 3 : 0));
        s_last_frame = frame;
    }

    if (force || sec != s_last_sec) {
        lv_label_set_text_fmt(s_time, "%02d:%02d", (int)(sec / 60), (int)(sec % 60));
        const int64_t total = pomo_total_ms(&s_pomo);
        lv_arc_set_value(s_arc, total > 0 ? (int)((total - remaining) * 1000 / total) : 0);
    }

    if (force || s_pomo.done != s_last_done || (int)s_pomo.phase != s_last_phase) {
        const int every = s_pomo.long_every;
        for (int i = 0; i < 8; i++) {
            if (!s_cycle[i]) continue;
            if (i >= every) {
                lv_obj_add_flag(s_cycle[i], LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            lv_obj_clear_flag(s_cycle[i], LV_OBJ_FLAG_HIDDEN);
            const bool filled = i < s_pomo.cycle;
            const bool current = i == s_pomo.cycle && s_pomo.phase == POMO_PHASE_WORK;
            lv_obj_set_style_bg_color(s_cycle[i], ui_c(filled ? C_TOMATO
                                                       : (current ? ui_mix(C_TOMATO, C_BG, 90)
                                                                  : C_SURFACE2)), 0);
            lv_obj_set_width(s_cycle[i], current ? 22 : 12);
        }
        lv_label_set_text_fmt(s_today_lbl, "今日 %d", s_pomo.done);
    }

    s_last_sec = sec;
    s_last_state = s_pomo.state;
    s_last_phase = s_pomo.phase;
    s_last_done = s_pomo.done;
}

static void build_main(lv_obj_t *p)
{
    s_arc = lv_arc_create(p);
    lv_obj_set_size(s_arc, 204, 204);
    lv_obj_align(s_arc, LV_ALIGN_TOP_MID, 0, 6);
    lv_arc_set_rotation(s_arc, 270);
    lv_arc_set_bg_angles(s_arc, 0, 360);
    lv_arc_set_range(s_arc, 0, 1000);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc, true, LV_PART_INDICATOR);

    s_sprite = ui_pix(s_arc, phase_sprite(0), 3);
    lv_obj_align(s_sprite, LV_ALIGN_TOP_MID, 0, 34);
    s_time = ui_label(s_arc, ui_font_64b, C_TEXT, "25:00");
    lv_obj_align(s_time, LV_ALIGN_TOP_MID, 0, 84);
    s_phase = ui_label(s_arc, &ui_font_16, C_TOMATO, "");
    lv_obj_align(s_phase, LV_ALIGN_TOP_MID, 0, 152);

    // 本轮进度:长休息前的番茄格
    lv_obj_t *row = ui_box(p, 0, 222, UI_W, 20);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    memset(s_cycle, 0, sizeof(s_cycle));
    for (int i = 0; i < 8; i++) s_cycle[i] = ui_card(row, 0, 0, 12, 8, C_SURFACE2, 4);
    lv_obj_t *gap = ui_box(row, 0, 0, 8, 8);
    (void)gap;
    s_today_lbl = ui_label(row, &ui_font_16, C_TEXT2, "");

    s_last_sec = -1;
    main_refresh(true);
}

// ---------------------------------------------------------------------------
// 快捷菜单
// ---------------------------------------------------------------------------
enum { MENU_SKIP = 0, MENU_RESTART, MENU_THEME, MENU_STATS, MENU_SETTINGS, MENU_SCREEN };

static void build_menu(lv_obj_t *p)
{
    lv_obj_t *t = ui_label(p, &ui_font_20b, C_TEXT, "番茄钟");
    lv_obj_set_pos(t, UI_PAD + 4, 4);
    ui_list_create(&s_list, p, UI_PAD, 36, UI_W - 2 * UI_PAD, UI_CONTENT_H - 40, C_TOMATO);
    char skip[24];
    snprintf(skip, sizeof(skip), "跳过%s", pomo_phase_name(s_pomo.phase));
    ui_list_add(&s_list, &IC_STAR, skip, NULL, false);
    ui_list_add(&s_list, &IC_CHECK, "重新开始本轮", NULL, false);
    ui_list_add(&s_list, PIXEL_THEMES[s_theme].focus[0], "主题", k_theme_names[s_theme], false);
    ui_list_add(&s_list, &ICON_BOARD, "本周统计", NULL, true);
    ui_list_add(&s_list, &ICON_GEAR, "时长与规则", NULL, true);
    ui_list_add(&s_list, &IC_BELL, "息屏(计时继续)", NULL, false);
    ui_list_focus(&s_list, 0, false);
    shell_hints("▲▼:选择|OK:确定|长▲:返回");
}

static void menu_ok(void)
{
    switch (s_list.focus) {
    case MENU_SKIP:
        pomo_skip(&s_pomo);
        svc_save();
        shell_toast("已跳过");
        build_page(PAGE_MAIN);
        break;
    case MENU_RESTART:
        pomo_restart_cycle(&s_pomo);
        svc_save();
        shell_toast("新一轮开始");
        build_page(PAGE_MAIN);
        break;
    case MENU_THEME:
        s_theme = (s_theme + 1) % PIXEL_THEME_COUNT;
        svc_save();
        ui_list_set_value(&s_list, MENU_THEME, k_theme_names[s_theme]);
        ui_list_set_icon(&s_list, MENU_THEME, PIXEL_THEMES[s_theme].focus[0]);
        break;
    case MENU_STATS: build_page(PAGE_STATS); break;
    case MENU_SETTINGS: build_page(PAGE_SETTINGS); break;
    default:
        build_page(PAGE_MAIN);
        shell_screen_off();
        break;
    }
}

// ---------------------------------------------------------------------------
// 设置:OK 进入编辑(▲▼ 调整),再按 OK 完成;改动即时保存。
// ---------------------------------------------------------------------------
enum { SET_WORK = 0, SET_BREAK, SET_LONG, SET_EVERY, SET_AUTO, SET_COUNT };

static void settings_values(void)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%d 分钟", s_pomo.work_min);
    ui_list_set_value(&s_list, SET_WORK, buf);
    snprintf(buf, sizeof(buf), "%d 分钟", s_pomo.break_min);
    ui_list_set_value(&s_list, SET_BREAK, buf);
    snprintf(buf, sizeof(buf), "%d 分钟", s_pomo.long_min);
    ui_list_set_value(&s_list, SET_LONG, buf);
    snprintf(buf, sizeof(buf), "每 %d 个", s_pomo.long_every);
    ui_list_set_value(&s_list, SET_EVERY, buf);
    ui_list_set_value(&s_list, SET_AUTO, s_pomo.auto_next ? "开" : "关");
    for (int i = 0; i < SET_COUNT; i++) {
        ui_list_set_value_color(&s_list, i, (s_editing && i == s_list.focus) ? C_TOMATO : C_TEXT3);
    }
}

static void settings_hints(void)
{
    if (s_editing) shell_hints("▲▼:调整|OK:完成");
    else shell_hints("▲▼:选择|OK:修改|长▲:返回");
}

static void build_settings(lv_obj_t *p)
{
    lv_obj_t *t = ui_label(p, &ui_font_20b, C_TEXT, "时长与规则");
    lv_obj_set_pos(t, UI_PAD + 4, 4);
    ui_list_create(&s_list, p, UI_PAD, 36, UI_W - 2 * UI_PAD, UI_CONTENT_H - 40, C_TOMATO);
    ui_list_add(&s_list, NULL, "专注", "", false);
    ui_list_add(&s_list, NULL, "短休息", "", false);
    ui_list_add(&s_list, NULL, "长休息", "", false);
    ui_list_add(&s_list, NULL, "长休息间隔", "", false);
    ui_list_add(&s_list, NULL, "自动开始专注", "", false);
    s_editing = false;
    ui_list_focus(&s_list, 0, false);
    settings_values();
    settings_hints();
}

static void settings_adjust(int delta)
{
    const bool running = s_pomo.state != POMO_STATE_READY;
    switch (s_list.focus) {
    case SET_WORK: pomo_set_minutes(&s_pomo, POMO_PHASE_WORK, s_pomo.work_min + delta); break;
    case SET_BREAK: pomo_set_minutes(&s_pomo, POMO_PHASE_BREAK, s_pomo.break_min + delta); break;
    case SET_LONG: pomo_set_minutes(&s_pomo, POMO_PHASE_LONG, s_pomo.long_min + delta); break;
    case SET_EVERY:
        s_pomo.long_every += delta;
        if (s_pomo.long_every < 2) s_pomo.long_every = 2;
        if (s_pomo.long_every > 8) s_pomo.long_every = 8;
        break;
    default: s_pomo.auto_next = !s_pomo.auto_next; break;
    }
    (void)running;
    svc_save();
    settings_values();
}

// ---------------------------------------------------------------------------
// 统计
// ---------------------------------------------------------------------------
static void build_stats(lv_obj_t *p)
{
    lv_obj_t *t = ui_label(p, &ui_font_20b, C_TEXT, "本周专注");
    lv_obj_set_pos(t, UI_PAD + 4, 4);

    int counts[7] = {0}, mins[7] = {0};
    const bool dated = s_today >= 0 && time_valid();
    if (dated) pomo_stats_week(&s_stats, s_today, counts, mins);
    else counts[6] = s_pomo.done;

    int total = 0, total_min = 0, maxc = 1;
    for (int i = 0; i < 7; i++) {
        total += counts[i];
        total_min += mins[i];
        if (counts[i] > maxc) maxc = counts[i];
    }

    // 汇总卡片
    lv_obj_t *card = ui_card(p, UI_PAD, 36, UI_W - 2 * UI_PAD, 64, C_SURFACE, 14);
    lv_obj_t *n = ui_label(card, ui_font_36b, C_TOMATO, "");
    lv_label_set_text_fmt(n, "%d", total);
    lv_obj_align(n, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_t *l1 = ui_label(card, &ui_font_16, C_TEXT2, "个番茄");
    lv_obj_align_to(l1, n, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -6);
    lv_obj_t *l2 = ui_label(card, &ui_font_16, C_TEXT2, "");
    lv_label_set_text_fmt(l2, "%d 小时 %d 分", total_min / 60, total_min % 60);
    lv_obj_align(l2, LV_ALIGN_RIGHT_MID, -14, 0);

    // 柱状图
    const int chart_y = 116, chart_h = 96, bar_w = 18;
    const int gap = (UI_W - 2 * UI_PAD - 7 * bar_w) / 6;
    struct tm tm;
    const bool have_tm = time_local(&tm);
    for (int i = 0; i < 7; i++) {
        const int x = UI_PAD + i * (bar_w + gap);
        const int h = counts[i] ? 8 + (chart_h - 8) * counts[i] / maxc : 4;
        const bool today = i == 6;
        ui_card(p, x, chart_y + chart_h - h, bar_w, h,
                today ? C_TOMATO : (counts[i] ? ui_mix(C_TOMATO, C_BG, 120) : C_SURFACE2), 5);
        if (counts[i]) {
            lv_obj_t *c = ui_label(p, &ui_font_16, today ? C_TEXT : C_TEXT3, "");
            lv_label_set_text_fmt(c, "%d", counts[i]);
            lv_obj_set_width(c, bar_w + 10);
            lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(c, x - 5, chart_y + chart_h - h - 20);
        }
        const char *wd = have_tm ? k_weekdays[(tm.tm_wday + 1 + i) % 7] : (today ? "今" : "");
        lv_obj_t *d = ui_label(p, &ui_font_16, today ? C_TOMATO : C_TEXT3, wd);
        lv_obj_set_width(d, bar_w + 10);
        lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(d, x - 5, chart_y + chart_h + 6);
    }
    if (!dated) {
        lv_obj_t *hint = ui_label(p, &ui_font_16, C_TEXT3, "联网对时后按天记录");
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
    }
    shell_hints("长▲:返回");
}

// ---------------------------------------------------------------------------
static void build_page(page_t page)
{
    if (s_page) lv_obj_delete(s_page);
    s_page = ui_box(s_root, 0, 0, UI_W, UI_CONTENT_H);
    s_page_kind = page;
    s_arc = NULL;
    switch (page) {
    case PAGE_MAIN: build_main(s_page); break;
    case PAGE_MENU: build_menu(s_page); break;
    case PAGE_SETTINGS: build_settings(s_page); break;
    default: build_stats(s_page); break;
    }
    lv_obj_set_style_opa(s_page, LV_OPA_TRANSP, 0);
    lv_obj_fade_in(s_page, 140, 0);
}

static void create(lv_obj_t *root)
{
    s_root = root;
    s_page = NULL;
    build_page(PAGE_MAIN);
}

static void destroy(void)
{
    s_root = NULL;
    s_page = NULL;
    s_arc = NULL;
}

static void maybe_sync_partner(void)
{
    // 正在与另一台设备互联时,开始专注会邀请对方一起专注。
    int minutes;
    if (link_running() && link_partner_online() && pomo_running_focus(&minutes)) {
        link_send_focus(minutes);
    }
}

static bool key(sys_key_t k, sys_key_ev_t ev)
{
    if (ev != SYS_EV_CLICK && ev != SYS_EV_LONG) return false;
    const bool click = ev == SYS_EV_CLICK;

    switch (s_page_kind) {
    case PAGE_MAIN:
        if (k == SYS_KEY_OK && click) {
            const bool was_ready = s_pomo.state == POMO_STATE_READY;
            pomo_toggle(&s_pomo, sys_now_ms());
            audio_beep(s_pomo.state == POMO_STATE_RUNNING ? BEEP_START : BEEP_STOP);
            if (was_ready) maybe_sync_partner();
            main_refresh(true);
            return true;
        }
        if (k == SYS_KEY_OK) {
            build_page(PAGE_MENU);
            return true;
        }
        if (k == SYS_KEY_DOWN && !click) {
            shell_screen_off();
            return true;
        }
        if (click && s_pomo.state == POMO_STATE_READY) {
            pomo_adjust(&s_pomo, k == SYS_KEY_UP ? 1 : -1);
            svc_save();
            main_refresh(true);
            return true;
        }
        return false;

    case PAGE_MENU:
        if (k == SYS_KEY_UP && !click) {
            build_page(PAGE_MAIN);
            return true;
        }
        if (click && k != SYS_KEY_OK) ui_list_move(&s_list, k == SYS_KEY_DOWN ? 1 : -1);
        else if (click) menu_ok();
        return true;

    case PAGE_SETTINGS:
        if (k == SYS_KEY_UP && !click) {
            build_page(PAGE_MENU);
            return true;
        }
        if (!click) return true;
        if (k == SYS_KEY_OK) {
            if (s_list.focus == SET_AUTO) {
                settings_adjust(0);
            } else {
                s_editing = !s_editing;
                settings_values();
                settings_hints();
            }
        } else if (s_editing) {
            settings_adjust(k == SYS_KEY_UP ? 1 : -1);
        } else {
            ui_list_move(&s_list, k == SYS_KEY_DOWN ? 1 : -1);
        }
        return true;

    default:
        if (k == SYS_KEY_UP && !click) {
            build_page(PAGE_MENU);
            return true;
        }
        return true;
    }
}

static void tick(int64_t now)
{
    (void)now;
    if (s_page_kind == PAGE_MAIN && s_arc) main_refresh(false);
}

const app_t APP_POMODORO = {
    .name = "番茄钟",
    .desc = "专注 · 休息 · 统计",
    .icon = &ICON_TOMATO,
    .color = C_TOMATO,
    .create = create,
    .destroy = destroy,
    .key = key,
    .tick = tick,
};
