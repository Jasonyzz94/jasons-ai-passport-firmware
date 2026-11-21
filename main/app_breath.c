// main/app_breath.c —— 呼吸冥想:跟着圆圈吸气、屏息、呼气。节奏计算在 breath.c(有主机测试)。
#include "apps.h"
#include "breath.h"
#include "icons.h"
#include "settings.h"
#include "svc.h"
#include "sys.h"
#include "ui.h"

#include <stdio.h>

#define C_BREATH 0x5EEAD4
#define RING_D 184
#define DOT_MIN 64

typedef enum { ST_IDLE = 0, ST_RUN, ST_PAUSE, ST_DONE } state_t;

static const int k_minutes[] = {1, 3, 5, 10};
#define MIN_COUNT ((int)(sizeof(k_minutes) / sizeof(k_minutes[0])))

static int s_pattern, s_dur = 1;
static state_t s_state;
static int64_t s_start, s_paused_at, s_paused_total;
static int s_last_step = -1;
static lv_obj_t *s_title, *s_desc, *s_ring, *s_glow, *s_dot, *s_step, *s_secs, *s_foot;

static int64_t elapsed(int64_t now)
{
    if (s_state == ST_IDLE) return 0;
    const int64_t end = s_state == ST_PAUSE ? s_paused_at : now;
    return end - s_start - s_paused_total;
}

static void set_circle(int scale)
{
    const int d = DOT_MIN + (RING_D - 32 - DOT_MIN) * scale / 1000;   // 光晕最大正好贴着外圈
    lv_obj_set_size(s_dot, d, d);
    lv_obj_align(s_dot, LV_ALIGN_CENTER, 0, 0);
    const int g = d + 18;
    lv_obj_set_size(s_glow, g, g);
    lv_obj_align(s_glow, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(s_glow, (lv_opa_t)(30 + scale * 50 / 1000), 0);
}

static void hints(void)
{
    switch (s_state) {
    case ST_IDLE: shell_hints("▲▼:节奏|OK:开始|长▼:时长"); break;
    case ST_RUN: shell_hints("OK:暂停|长▲:结束"); break;
    case ST_PAUSE: shell_hints("OK:继续|长▲:结束"); break;
    default: shell_hints("OK:再来一次|长▲:返回"); break;
    }
}

static void render_header(void)
{
    lv_label_set_text(s_title, BREATH_PATTERNS[s_pattern].name);
    lv_label_set_text(s_desc, BREATH_PATTERNS[s_pattern].desc);
}

static void render(int64_t now)
{
    const int total_ms = k_minutes[s_dur] * 60000;
    char foot[64];
    if (s_state == ST_IDLE) {
        set_circle(350);
        lv_label_set_text(s_step, "准备");
        lv_label_set_text_fmt(s_secs, "%d", k_minutes[s_dur]);
        snprintf(foot, sizeof(foot), "%d 分钟 · 按确定开始", k_minutes[s_dur]);
    } else if (s_state == ST_DONE) {
        set_circle(500);
        lv_label_set_text(s_step, "完成");
        lv_label_set_text(s_secs, "✓");
        snprintf(foot, sizeof(foot), "%d 分钟,做得很好", k_minutes[s_dur]);
    } else {
        const int64_t e = elapsed(now);
        const breath_frame_t f = breath_at(s_pattern, e);
        set_circle(f.scale);
        lv_label_set_text(s_step, s_state == ST_PAUSE ? "已暂停" : breath_step_name(f.step));
        lv_label_set_text_fmt(s_secs, "%d", f.step_left_s);
        const int left = (int)((total_ms - e + 999) / 1000);
        snprintf(foot, sizeof(foot), "第 %d 轮 · 还剩 %d:%02d", f.round, left / 60, left % 60);
        if (s_state == ST_RUN && (int)f.step != s_last_step) {
            if (s_last_step >= 0 && g_settings.sound_feedback) audio_beep(BEEP_CLICK);
            s_last_step = (int)f.step;
        }
    }
    lv_label_set_text(s_foot, foot);
}

static void create(lv_obj_t *root)
{
    s_state = ST_IDLE;
    s_title = ui_label_w(root, &ui_font_20b, C_TEXT, UI_W, LV_TEXT_ALIGN_CENTER, "");
    lv_obj_set_pos(s_title, 0, 4);
    s_desc = ui_label_w(root, &ui_font_14, C_TEXT3, UI_W, LV_TEXT_ALIGN_CENTER, "");
    lv_obj_set_pos(s_desc, 0, 32);

    lv_obj_t *stage = ui_box(root, (UI_W - RING_D) / 2, 56, RING_D, RING_D);
    s_ring = ui_card(stage, 0, 0, RING_D, RING_D, C_BG, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(s_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ring, 2, 0);
    lv_obj_set_style_border_color(s_ring, ui_c(C_LINE), 0);
    s_glow = ui_card(stage, 0, 0, 80, 80, C_BREATH, LV_RADIUS_CIRCLE);
    s_dot = ui_card(stage, 0, 0, 64, 64, ui_mix(C_BREATH, C_BG, 160), LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_width(s_dot, 2, 0);
    lv_obj_set_style_border_color(s_dot, ui_c(C_BREATH), 0);
    s_step = ui_label(stage, &ui_font_20b, C_TEXT, "");
    lv_obj_align(s_step, LV_ALIGN_CENTER, 0, -18);
    s_secs = ui_label(stage, ui_font_36b, C_BREATH, "");
    lv_obj_align(s_secs, LV_ALIGN_CENTER, 0, 18);

    s_foot = ui_label_w(root, &ui_font_14, C_TEXT2, UI_W, LV_TEXT_ALIGN_CENTER, "");
    lv_obj_set_pos(s_foot, 0, 242);
    render_header();
    hints();
    render(sys_now_ms());
}

static void stop_session(void)
{
    s_state = ST_IDLE;
    shell_keep_awake(false);
}

static void destroy(void)
{
    stop_session();
    s_title = NULL;
}

static bool key(sys_key_t k, sys_key_ev_t ev)
{
    const int64_t now = sys_now_ms();
    if (k == SYS_KEY_UP && ev == SYS_EV_LONG && (s_state == ST_RUN || s_state == ST_PAUSE)) {
        stop_session();
        hints();
        render(now);
        return true;
    }
    if (k == SYS_KEY_DOWN && ev == SYS_EV_LONG && s_state == ST_IDLE) {
        s_dur = (s_dur + 1) % MIN_COUNT;
        render(now);
        return true;
    }
    if (ev != SYS_EV_CLICK) return false;
    if (k == SYS_KEY_OK) {
        if (s_state == ST_IDLE || s_state == ST_DONE) {
            s_state = ST_RUN;
            s_start = now;
            s_paused_total = 0;
            s_last_step = -1;
            shell_keep_awake(true);
            audio_beep(BEEP_START);
        } else if (s_state == ST_RUN) {
            s_state = ST_PAUSE;
            s_paused_at = now;
        } else {
            s_paused_total += now - s_paused_at;
            s_state = ST_RUN;
        }
        hints();
        render(now);
        return true;
    }
    if (s_state == ST_IDLE || s_state == ST_DONE) {
        s_pattern = (s_pattern + (k == SYS_KEY_DOWN ? 1 : -1) + BREATH_PATTERN_COUNT) % BREATH_PATTERN_COUNT;
        s_state = ST_IDLE;
        render_header();
        hints();
        render(now);
    }
    return true;
}

static void tick(int64_t now)
{
    if (s_state != ST_RUN) return;
    if (elapsed(now) >= (int64_t)k_minutes[s_dur] * 60000) {
        s_state = ST_DONE;
        shell_keep_awake(false);
        audio_beep(BEEP_BREAK_DONE);
        hints();
    }
    render(now);
}

const app_t APP_BREATH = {
    .name = "呼吸冥想",
    .desc = "跟着圆圈慢慢呼吸",
    .icon = &IC_HEART,
    .color = C_BREATH,
    .create = create,
    .destroy = destroy,
    .key = key,
    .tick = tick,
};
