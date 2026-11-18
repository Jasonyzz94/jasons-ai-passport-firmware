// main/app_timer.c —— 计时器:秒表(带计次)+ 倒计时(后台运行,到点全屏提醒)。
// 按键:▲ 切换 秒表/倒计时
//   秒表  :OK 开始/停止 · ▼ 计次(运行中)/ 清零(停止时)
//   倒计时:OK 开始/暂停 · ▼ 选择预设时长(未开始时)/ 重置(暂停时)
#include "apps.h"
#include "icons.h"
#include "svc.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>

#define LAPS 3
static const int k_presets[] = {1, 3, 5, 10, 15, 20, 30, 45, 60};
#define PRESET_COUNT ((int)(sizeof(k_presets) / sizeof(k_presets[0])))

// 状态跨进出保留(倒计时在后台继续)
static bool s_countdown_mode;
static bool s_sw_running;
static int64_t s_sw_start, s_sw_accum;
static int64_t s_laps[LAPS];
static int s_lap_count;
static int s_preset = 2;
static bool s_cd_running, s_cd_paused;
static int64_t s_cd_deadline, s_cd_left;

static lv_obj_t *s_root;
static lv_obj_t *s_tab_sw, *s_tab_cd;
static lv_obj_t *s_big, *s_small;
static lv_obj_t *s_ring;
static lv_obj_t *s_info;

static int64_t sw_elapsed(int64_t now) { return s_sw_accum + (s_sw_running ? now - s_sw_start : 0); }
static int64_t cd_total(void) { return (int64_t)k_presets[s_preset] * 60000; }
static int64_t cd_left(int64_t now)
{
    if (!s_cd_running) return s_cd_paused ? s_cd_left : cd_total();
    const int64_t l = s_cd_deadline - now;
    return l > 0 ? l : 0;
}

static void bg_tick(int64_t now)
{
    if (s_cd_running && now >= s_cd_deadline) {
        s_cd_running = false;
        s_cd_paused = false;
        char buf[32];
        snprintf(buf, sizeof(buf), "%d 分钟倒计时结束", k_presets[s_preset]);
        shell_alert(&ICON_TIMER, C_TIMER, "时间到!", buf, NULL);
        audio_beep(BEEP_WORK_DONE);
    }
}

static void render_tabs(void)
{
    lv_obj_set_style_bg_color(s_tab_sw, ui_c(!s_countdown_mode ? C_TIMER : C_SURFACE), 0);
    lv_obj_set_style_text_color(s_tab_sw, ui_c(!s_countdown_mode ? C_BG : C_TEXT2), 0);
    lv_obj_set_style_bg_color(s_tab_cd, ui_c(s_countdown_mode ? C_TIMER : C_SURFACE), 0);
    lv_obj_set_style_text_color(s_tab_cd, ui_c(s_countdown_mode ? C_BG : C_TEXT2), 0);
    if (s_countdown_mode) {
        lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_HIDDEN);
        shell_hints(s_cd_running ? "OK:暂停|▲:秒表|长▲:返回"
                    : (s_cd_paused ? "OK:继续|▼:重置|▲:秒表" : "OK:开始|▼:时长|▲:秒表"));
    } else {
        lv_obj_add_flag(s_ring, LV_OBJ_FLAG_HIDDEN);
        shell_hints(s_sw_running ? "OK:停止|▼:计次|▲:倒计时"
                                 : "OK:开始|▼:清零|▲:倒计时");
    }
}

static void fmt_ms(char *buf, size_t len, int64_t ms)
{
    const int64_t s = ms / 1000;
    if (s >= 3600) snprintf(buf, len, "%d:%02d", (int)(s / 3600), (int)(s / 60 % 60));
    else snprintf(buf, len, "%02d:%02d", (int)(s / 60), (int)(s % 60));
}

static void render(int64_t now)
{
    char buf[24];
    if (s_countdown_mode) {
        const int64_t left = cd_left(now);
        fmt_ms(buf, sizeof(buf), left + 999);
        lv_label_set_text(s_big, buf);
        lv_label_set_text(s_small, "");
        lv_arc_set_value(s_ring, (int)((cd_total() - left) * 1000 / cd_total()));
        lv_label_set_text_fmt(s_info, s_cd_running || s_cd_paused ? "共 %d 分钟" : "▼ 换时长",
                              k_presets[s_preset]);
    } else {
        const int64_t e = sw_elapsed(now);
        fmt_ms(buf, sizeof(buf), e);
        lv_label_set_text(s_big, buf);
        lv_label_set_text_fmt(s_small, ".%d", (int)(e / 100 % 10));
        char laps[96] = "";
        for (int i = 0; i < s_lap_count; i++) {
            char one[32], t[12];
            fmt_ms(t, sizeof(t), s_laps[i]);
            snprintf(one, sizeof(one), "%s计次 %d   %s.%d", i ? "\n" : "", i + 1, t,
                     (int)(s_laps[i] / 100 % 10));
            strncat(laps, one, sizeof(laps) - strlen(laps) - 1);
        }
        lv_label_set_text(s_info, s_lap_count ? laps : "▼ 记录计次");
    }
    lv_obj_align(s_big, LV_ALIGN_TOP_MID, s_countdown_mode ? 0 : -14, s_countdown_mode ? 104 : 70);
    lv_obj_align_to(s_small, s_big, LV_ALIGN_OUT_RIGHT_BOTTOM, 0, -8);
    lv_obj_align(s_info, LV_ALIGN_TOP_MID, 0, s_countdown_mode ? 186 : 160);
}

static void create(lv_obj_t *root)
{
    s_root = root;
    lv_obj_t *tabs = ui_box(root, 0, 6, UI_W, 30);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tabs, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tabs, 10, 0);
    s_tab_sw = ui_pill(tabs, "秒表", C_TEXT2, C_SURFACE);
    s_tab_cd = ui_pill(tabs, "倒计时", C_TEXT2, C_SURFACE);
    lv_obj_set_style_pad_ver(s_tab_sw, 4, 0);
    lv_obj_set_style_pad_ver(s_tab_cd, 4, 0);

    s_ring = lv_arc_create(root);
    lv_obj_set_size(s_ring, 204, 204);
    lv_obj_align(s_ring, LV_ALIGN_TOP_MID, 0, 44);
    lv_arc_set_rotation(s_ring, 270);
    lv_arc_set_bg_angles(s_ring, 0, 360);
    lv_arc_set_range(s_ring, 0, 1000);
    lv_obj_remove_style(s_ring, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_ring, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_ring, ui_c(ui_mix(C_TIMER, C_BG, 40)), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_ring, ui_c(C_TIMER), LV_PART_INDICATOR);

    s_big = ui_label(root, ui_font_64b, C_TEXT, "");
    s_small = ui_label(root, ui_font_36b, C_TIMER, "");
    s_info = ui_label(root, &ui_font_14, C_TEXT2, "");
    lv_obj_set_style_text_align(s_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_info, UI_W - 2 * UI_PAD);
    lv_obj_align(s_info, LV_ALIGN_TOP_MID, 0, 160);

    render_tabs();
    render(sys_now_ms());
}

static void destroy(void) { s_root = NULL; }

static bool key(sys_key_t k, sys_key_ev_t ev)
{
    if (ev != SYS_EV_CLICK) return false;
    const int64_t now = sys_now_ms();
    if (k == SYS_KEY_UP) {
        s_countdown_mode = !s_countdown_mode;
        render_tabs();
    } else if (!s_countdown_mode) {
        if (k == SYS_KEY_OK) {
            if (s_sw_running) s_sw_accum += now - s_sw_start;
            else s_sw_start = now;
            s_sw_running = !s_sw_running;
            audio_beep(s_sw_running ? BEEP_START : BEEP_STOP);
        } else if (s_sw_running) {
            if (s_lap_count == LAPS) {
                memmove(&s_laps[0], &s_laps[1], sizeof(int64_t) * (LAPS - 1));
                s_lap_count--;
            }
            s_laps[s_lap_count++] = sw_elapsed(now);
            audio_beep(BEEP_CLICK);
        } else {
            s_sw_accum = 0;
            s_lap_count = 0;
        }
        render_tabs();
    } else {
        if (k == SYS_KEY_OK) {
            if (s_cd_running) {
                s_cd_left = cd_left(now);
                s_cd_running = false;
                s_cd_paused = true;
            } else {
                s_cd_deadline = now + (s_cd_paused ? s_cd_left : cd_total());
                s_cd_running = true;
                s_cd_paused = false;
            }
            audio_beep(s_cd_running ? BEEP_START : BEEP_STOP);
        } else if (s_cd_paused) {
            s_cd_paused = false;
        } else if (!s_cd_running) {
            s_preset = (s_preset + 1) % PRESET_COUNT;
        }
        render_tabs();
    }
    render(now);
    return true;
}

static void tick(int64_t now) { render(now); }

// 由 apps 初始化时注册后台 tick(倒计时离开应用也要到点提醒)。
void timer_service_init(void)
{
    shell_add_bg_tick(bg_tick);
}

const app_t APP_TIMER = {
    .name = "计时器",
    .desc = "秒表 · 倒计时",
    .icon = &ICON_TIMER,
    .color = C_TIMER,
    .create = create,
    .destroy = destroy,
    .key = key,
    .tick = tick,
};
