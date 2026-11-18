// main/app_sound.c —— 声音工具:频谱与分贝计、乐器调音器、节拍器。全部离线,在设备上实时计算。
// 进入后先选工具(列表),工具页里 长▲ 回到列表。
#include "apps.h"
#include "audio_dsp.h"
#include "icons.h"
#include "svc.h"
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef enum { PAGE_MENU = 0, PAGE_SPECTRUM, PAGE_TUNER, PAGE_METRO } page_t;

static lv_obj_t *s_root;
static lv_obj_t *s_page;
static page_t s_kind;
static ui_list_t s_list;

// 频谱
static lv_obj_t *s_bars[AUDIO_BANDS];
static lv_obj_t *s_caps[AUDIO_BANDS];
static float s_cap_v[AUDIO_BANDS];
static float s_smooth[AUDIO_BANDS];
static lv_obj_t *s_db;
static lv_obj_t *s_db_desc;
static lv_obj_t *s_db_unit;
static lv_obj_t *s_peak;
static float s_peak_db;
static float s_db_smooth;

// 调音器
static lv_obj_t *s_note;
static lv_obj_t *s_oct;
static lv_obj_t *s_hz;
static lv_obj_t *s_needle;
static lv_obj_t *s_cents;
static lv_obj_t *s_tune_hint;
static float s_cents_smooth;
static int64_t s_last_pitch_ms;

// 节拍器
static int s_bpm = 96;
static int s_beats = 4;
static bool s_metro_on;
static lv_obj_t *s_bpm_lbl;
static lv_obj_t *s_tempo_lbl;
static lv_obj_t *s_beat_dots[8];
static uint32_t s_last_tick = UINT32_MAX;

#define BAR_AREA_Y 96
#define BAR_AREA_H 132

static void build(page_t page);

// ---------------------------------------------------------------------------
static void build_menu(lv_obj_t *p)
{
    lv_obj_t *t = ui_label(p, &ui_font_20b, C_TEXT, "声音工具");
    lv_obj_set_pos(t, UI_PAD + 4, 4);
    ui_list_create(&s_list, p, UI_PAD, 36, UI_W - 2 * UI_PAD, UI_CONTENT_H - 40, C_SOUND);
    ui_list_add(&s_list, &IC_WAVE, "频谱 · 分贝计", NULL, true);
    ui_list_add(&s_list, &IC_TUNER, "调音器", NULL, true);
    ui_list_add(&s_list, &IC_METRO, "节拍器", NULL, true);
    ui_list_focus(&s_list, s_kind == PAGE_MENU ? 0 : (int)s_kind - 1, false);
    if (!audio_available()) {
        lv_obj_t *w = ui_label(p, &ui_font_14, C_ERR, "音频芯片不可用");
        lv_obj_align(w, LV_ALIGN_BOTTOM_MID, 0, -6);
    }
    shell_hints("▲▼:选择|OK:打开|长▲:返回");
}

// ---------------------------------------------------------------------------
static const char *loudness_desc(float db)
{
    if (db < 35) return "非常安静";
    if (db < 50) return "安静 · 适合睡眠";
    if (db < 62) return "正常交谈";
    if (db < 75) return "嘈杂";
    if (db < 85) return "很吵 · 注意休息";
    return "过响 · 保护听力";
}

static uint32_t band_color(float v)
{
    if (v > 0.82f) return C_ERR;
    if (v > 0.62f) return C_WARN;
    return C_SOUND;
}

static void build_spectrum(lv_obj_t *p)
{
    lv_obj_t *card = ui_card(p, UI_PAD, 4, UI_W - 2 * UI_PAD, 84, C_SURFACE, 16);
    s_db = ui_label(card, ui_font_64b, C_TEXT, "--");
    lv_obj_align(s_db, LV_ALIGN_LEFT_MID, 12, 2);
    s_db_unit = ui_label(card, &ui_font_14, C_SOUND, "分贝");
    lv_obj_align_to(s_db_unit, s_db, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -12);
    s_db_desc = ui_label_w(card, &ui_font_14, C_TEXT2, 76, LV_TEXT_ALIGN_RIGHT, "");
    lv_obj_align(s_db_desc, LV_ALIGN_TOP_RIGHT, -12, 18);
    s_peak = ui_label_w(card, &ui_font_14, C_TEXT3, 76, LV_TEXT_ALIGN_RIGHT, "");
    lv_obj_align(s_peak, LV_ALIGN_TOP_RIGHT, -12, 44);

    const int area_w = UI_W - 2 * UI_PAD;
    const int bw = 7, gap = (area_w - AUDIO_BANDS * bw) / (AUDIO_BANDS - 1);
    const int x0 = UI_PAD + (area_w - (AUDIO_BANDS * bw + (AUDIO_BANDS - 1) * gap)) / 2;
    for (int i = 0; i < AUDIO_BANDS; i++) {
        s_bars[i] = ui_card(p, x0 + i * (bw + gap), BAR_AREA_Y + BAR_AREA_H - 4, bw, 4, C_SOUND, 2);
        s_caps[i] = ui_card(p, x0 + i * (bw + gap), BAR_AREA_Y + BAR_AREA_H - 8, bw, 2, C_TEXT2, 1);
        s_cap_v[i] = 0;
        s_smooth[i] = 0;
    }
    lv_obj_t *lo = ui_label(p, &ui_font_14, C_TEXT3, "低音");
    lv_obj_set_pos(lo, UI_PAD, BAR_AREA_Y + BAR_AREA_H + 4);
    lv_obj_t *hi = ui_label(p, &ui_font_14, C_TEXT3, "高音");
    lv_obj_align(hi, LV_ALIGN_TOP_RIGHT, -UI_PAD, BAR_AREA_Y + BAR_AREA_H + 4);
    s_peak_db = 0;
    s_db_smooth = 0;
    shell_hints("OK:清除峰值|长▲:返回");
    audio_analyze_start();
}

static void tick_spectrum(void)
{
    audio_analysis_t a;
    if (!audio_analysis_get(&a)) return;
    for (int i = 0; i < AUDIO_BANDS; i++) {
        // 上升快、下落慢,看起来更"弹"
        const float v = a.bands[i];
        s_smooth[i] = v > s_smooth[i] ? v : s_smooth[i] * 0.82f + v * 0.18f;
        const int h = 4 + (int)(s_smooth[i] * (BAR_AREA_H - 8));
        lv_obj_set_height(s_bars[i], h);
        lv_obj_set_y(s_bars[i], BAR_AREA_Y + BAR_AREA_H - h);
        lv_obj_set_style_bg_color(s_bars[i], ui_c(band_color(s_smooth[i])), 0);
        s_cap_v[i] = s_smooth[i] > s_cap_v[i] ? s_smooth[i] : s_cap_v[i] - 0.012f;
        if (s_cap_v[i] < 0) s_cap_v[i] = 0;
        lv_obj_set_y(s_caps[i], BAR_AREA_Y + BAR_AREA_H - 8 - (int)(s_cap_v[i] * (BAR_AREA_H - 8)));
    }
    s_db_smooth = s_db_smooth * 0.7f + a.db * 0.3f;
    if (a.peak_db > s_peak_db) s_peak_db = a.peak_db;
    lv_label_set_text_fmt(s_db, "%d", (int)lroundf(s_db_smooth));
    lv_label_set_text(s_db_desc, loudness_desc(s_db_smooth));
    lv_label_set_text_fmt(s_peak, "最高 %d", (int)lroundf(s_peak_db));
    lv_obj_align_to(s_db_unit, s_db, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -12);
}

// ---------------------------------------------------------------------------
#define DIAL_X 22
#define DIAL_W (UI_W - 2 * DIAL_X)
#define DIAL_Y 168

static void build_tuner(lv_obj_t *p)
{
    s_note = ui_label(p, ui_font_64b, C_TEXT, "-");
    lv_obj_align(s_note, LV_ALIGN_TOP_MID, -8, 14);
    s_oct = ui_label(p, ui_font_36b, C_TEXT3, "");
    lv_obj_align_to(s_oct, s_note, LV_ALIGN_OUT_RIGHT_BOTTOM, 2, -6);
    s_hz = ui_label(p, &ui_font_16, C_TEXT2, "弹一个音");
    lv_obj_align(s_hz, LV_ALIGN_TOP_MID, 0, 92);
    s_cents = ui_label(p, &ui_font_14, C_TEXT3, "");
    lv_obj_align(s_cents, LV_ALIGN_TOP_MID, 0, 120);

    // 刻度盘:-50 .. +50 音分
    for (int c = -50; c <= 50; c += 10) {
        const int x = DIAL_X + (c + 50) * DIAL_W / 100;
        const int h = c == 0 ? 26 : (c % 50 == 0 ? 18 : 12);
        ui_card(p, x - 1, DIAL_Y + 26 - h, 2, h, c == 0 ? C_SOUND : C_LINE, 1);
    }
    lv_obj_t *flat = ui_label(p, &ui_font_14, C_TEXT3, "偏低");
    lv_obj_set_pos(flat, DIAL_X - 4, DIAL_Y + 34);
    lv_obj_t *sharp = ui_label(p, &ui_font_14, C_TEXT3, "偏高");
    lv_obj_align(sharp, LV_ALIGN_TOP_RIGHT, -DIAL_X + 4, DIAL_Y + 34);
    s_needle = ui_card(p, UI_W / 2 - 3, DIAL_Y - 10, 6, 40, C_TEXT3, 3);
    s_tune_hint = ui_label(p, &ui_font_14, C_TEXT3, "标准音 A4 = 440 赫兹");
    lv_obj_align(s_tune_hint, LV_ALIGN_TOP_MID, 0, DIAL_Y + 56);
    s_cents_smooth = 0;
    s_last_pitch_ms = 0;
    shell_hints("长▲:返回");
    audio_analyze_start();
}

static void tick_tuner(int64_t now)
{
    audio_analysis_t a;
    if (!audio_analysis_get(&a)) return;
    if (a.pitch_hz <= 0 || a.clarity < 0.8f) {
        if (now - s_last_pitch_ms > 1500) {
            lv_obj_set_style_bg_color(s_needle, ui_c(C_TEXT3), 0);
            lv_obj_set_style_text_color(s_note, ui_c(C_TEXT3), 0);
        }
        return;
    }
    s_last_pitch_ms = now;
    dsp_note_t n;
    dsp_note_from_hz(a.pitch_hz, 440.0f, &n);
    s_cents_smooth = s_cents_smooth * 0.6f + n.cents * 0.4f;
    const bool in_tune = fabsf(s_cents_smooth) < 5.0f;
    const uint32_t color = in_tune ? C_SOUND : (fabsf(s_cents_smooth) < 20 ? C_WARN : C_ERR);
    lv_label_set_text(s_note, n.name);
    lv_obj_set_style_text_color(s_note, ui_c(in_tune ? C_SOUND : C_TEXT), 0);
    lv_label_set_text_fmt(s_oct, "%d", n.octave);
    lv_obj_align(s_note, LV_ALIGN_TOP_MID, -8, 14);
    lv_obj_align_to(s_oct, s_note, LV_ALIGN_OUT_RIGHT_BOTTOM, 2, -6);
    lv_label_set_text_fmt(s_hz, "%d.%d 赫兹", (int)a.pitch_hz, (int)(a.pitch_hz * 10) % 10);
    lv_label_set_text_fmt(s_cents, in_tune ? "准了" : "%+d 音分", (int)lroundf(s_cents_smooth));
    lv_obj_set_style_text_color(s_cents, ui_c(color), 0);
    const int x = DIAL_X + (int)((s_cents_smooth + 50) * DIAL_W / 100) - 3;
    lv_obj_set_x(s_needle, x < DIAL_X - 3 ? DIAL_X - 3 : (x > DIAL_X + DIAL_W - 3 ? DIAL_X + DIAL_W - 3 : x));
    lv_obj_set_style_bg_color(s_needle, ui_c(color), 0);
}

// ---------------------------------------------------------------------------
static const char *tempo_name(int bpm)
{
    if (bpm < 60) return "广板 · 很慢";
    if (bpm < 76) return "柔板 · 慢";
    if (bpm < 108) return "行板 · 稍慢";
    if (bpm < 120) return "中板 · 中速";
    if (bpm < 168) return "快板 · 快";
    return "急板 · 很快";
}

static void metro_render(void)
{
    lv_label_set_text_fmt(s_bpm_lbl, "%d", s_bpm);
    lv_label_set_text(s_tempo_lbl, tempo_name(s_bpm));
    for (int i = 0; i < 8; i++) {
        if (i < s_beats) lv_obj_clear_flag(s_beat_dots[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_beat_dots[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_beat_dots[i], ui_c(C_SURFACE2), 0);
    }
    shell_hints(s_metro_on ? "OK:停止|▲▼:速度|长▼:拍号" : "OK:开始|▲▼:速度|长▼:拍号");
}

static void build_metro(lv_obj_t *p)
{
    lv_obj_t *card = ui_card(p, UI_PAD, 6, UI_W - 2 * UI_PAD, 150, C_SURFACE, 20);
    s_bpm_lbl = ui_label(card, ui_font_64b, C_TEXT, "");
    lv_obj_align(s_bpm_lbl, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_t *u = ui_label(card, &ui_font_14, C_TEXT3, "每分钟拍数");
    lv_obj_align(u, LV_ALIGN_TOP_MID, 0, 92);
    s_tempo_lbl = ui_label(card, &ui_font_16, C_SOUND, "");
    lv_obj_align(s_tempo_lbl, LV_ALIGN_TOP_MID, 0, 114);

    lv_obj_t *row = ui_box(p, 0, 176, UI_W, 40);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    for (int i = 0; i < 8; i++) s_beat_dots[i] = ui_card(row, 0, 0, i == 0 ? 30 : 22, i == 0 ? 30 : 22,
                                                        C_SURFACE2, 15);
    lv_obj_t *sig = ui_label(p, &ui_font_14, C_TEXT3, "");
    lv_label_set_text_fmt(sig, "%d/4 拍", s_beats);
    lv_obj_align(sig, LV_ALIGN_TOP_MID, 0, 226);
    s_last_tick = UINT32_MAX;
    audio_metronome_set(s_bpm, s_beats);
    metro_render();
}

static void tick_metro(void)
{
    if (!s_metro_on) return;
    const uint32_t t = audio_metronome_ticks();
    if (t == s_last_tick) return;
    s_last_tick = t;
    const int beat = (int)((t + s_beats - 1) % s_beats);
    for (int i = 0; i < s_beats; i++) {
        lv_obj_set_style_bg_color(s_beat_dots[i], ui_c(i == beat ? (i == 0 ? C_WARN : C_SOUND)
                                                              : C_SURFACE2), 0);
    }
}

// ---------------------------------------------------------------------------
static void leave_tool(void)
{
    audio_analyze_stop();
    if (s_metro_on) {
        s_metro_on = false;
        audio_metronome_run(false);
    }
}

static void build(page_t page)
{
    if (s_page) lv_obj_delete(s_page);
    s_page = ui_box(s_root, 0, 0, UI_W, UI_CONTENT_H);
    const page_t prev = s_kind;
    s_kind = page;
    switch (page) {
    case PAGE_SPECTRUM: build_spectrum(s_page); break;
    case PAGE_TUNER: build_tuner(s_page); break;
    case PAGE_METRO: build_metro(s_page); break;
    default:
        s_kind = prev;               // 让菜单记住上次的工具
        build_menu(s_page);
        s_kind = PAGE_MENU;
        break;
    }
    lv_obj_set_style_opa(s_page, LV_OPA_TRANSP, 0);
    lv_obj_fade_in(s_page, 140, 0);
}

static void create(lv_obj_t *root)
{
    s_root = root;
    s_page = NULL;
    s_kind = PAGE_MENU;
    build(PAGE_MENU);
}

static void destroy(void)
{
    leave_tool();
    s_root = NULL;
    s_page = NULL;
}

static bool key(sys_key_t k, sys_key_ev_t ev)
{
    if (s_kind == PAGE_MENU) {
        if (ev != SYS_EV_CLICK) return false;
        if (k == SYS_KEY_OK) build((page_t)(s_list.focus + 1));
        else ui_list_move(&s_list, k == SYS_KEY_DOWN ? 1 : -1);
        return true;
    }
    if (k == SYS_KEY_UP && ev == SYS_EV_LONG) {
        leave_tool();
        build(PAGE_MENU);
        return true;
    }
    if (s_kind == PAGE_SPECTRUM && k == SYS_KEY_OK && ev == SYS_EV_CLICK) {
        s_peak_db = 0;
        for (int i = 0; i < AUDIO_BANDS; i++) s_cap_v[i] = 0;
        return true;
    }
    if (s_kind == PAGE_METRO) {
        if (k == SYS_KEY_OK && ev == SYS_EV_CLICK) {
            s_metro_on = !s_metro_on;
            audio_metronome_set(s_bpm, s_beats);
            audio_metronome_run(s_metro_on);
            s_last_tick = UINT32_MAX;
            metro_render();
        } else if (ev == SYS_EV_CLICK) {
            s_bpm += k == SYS_KEY_UP ? 2 : -2;
            if (s_bpm < 30) s_bpm = 30;
            if (s_bpm > 240) s_bpm = 240;
            audio_metronome_set(s_bpm, s_beats);
            metro_render();
        } else if (k == SYS_KEY_DOWN && ev == SYS_EV_LONG) {
            static const int sigs[] = {2, 3, 4, 6};
            int i = 0;
            while (i < 4 && sigs[i] != s_beats) i++;
            s_beats = sigs[(i + 1) % 4];
            audio_metronome_set(s_bpm, s_beats);
            build(PAGE_METRO);
        }
        return true;
    }
    return ev == SYS_EV_CLICK;
}

static void tick(int64_t now)
{
    switch (s_kind) {
    case PAGE_SPECTRUM: tick_spectrum(); break;
    case PAGE_TUNER: tick_tuner(now); break;
    case PAGE_METRO: tick_metro(); break;
    default: break;
    }
}

const app_t APP_SOUND = {
    .name = "声音工具",
    .desc = "频谱 · 调音 · 节拍",
    .icon = &ICON_SOUND,
    .color = C_SOUND,
    .create = create,
    .destroy = destroy,
    .key = key,
    .tick = tick,
    .flags = APP_FLAG_KEEP_AWAKE,
};
