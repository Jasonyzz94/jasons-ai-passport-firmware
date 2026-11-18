// main/main.c —— 番茄时钟:清晰排版 + Montserrat 文字 + 像素图案点缀。
//
// 主屏:OK 单击 开始/暂停/继续;OK 长按 重置当前阶段;
//      上/下单击 READY 时微调当前阶段分钟;上键长按 进入设置(非 RUNNING)。
// 设置:上/下单击 调整选中项分钟;OK 单击 在专注/休息之间切换;
//      OK 长按 保存并返回主屏。
// 行为:专注结束完成数+1、响 3 声并自动进入休息;休息结束响 2 声回到专注 READY。
// 时长与完成次数存 NVS(命名空间 pomodoro),重启保留。
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_pins.h"      // BSP_BTN_MV_RELEASED_MIN
#include "pomodoro.h"
#include "pixel_time.h"
#include "pixel_art.h"
#include "key_guard.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "pomodoro";

#define INPUT_QUEUE_DEPTH 8
#define UI_TICK_MS 200
#define BATTERY_REFRESH_MS 30000
#define NVS_NAMESPACE "pomodoro"
#define DISPLAY_MODE_ALWAYS_ON 4

// 息屏档位:30s/60s/120s/5min,第 5 档为常亮(不自动息屏)。
static const int32_t k_screen_off_ms[] = {30000, 60000, 120000, 300000};
static const char *const k_display_names[] = {"30 s", "60 s", "120 s", "5 min", "Always on"};
static int s_display_mode = 1;

#define C_BG      0x12161F
#define C_PANEL   0x1A2230
#define C_BORDER  0x2E3A4E
#define C_SELECT  0x24334A
#define C_TEXT    0xE6EDF3
#define C_MUTED   0x8A97A8
#define C_FOCUS   0x3FD68F
#define C_BREAK   0xFFB020

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

typedef enum {
    MODE_MAIN = 0,
    MODE_SETUP,
    MODE_HELP,
} app_mode_t;

static pomo_t s_pomo;
static app_mode_t s_mode;
static int s_setup_field;                 // 0=专注 1=休息
static QueueHandle_t s_input_queue;
static QueueHandle_t s_beep_queue;
static bool s_audio_ok;
static bool s_battery_ok;
static volatile bool s_input_ready;
static int64_t s_last_activity_ms;
static bool s_screen_on = true;

// 按键唤醒时那颗键在启动期间仍按着,先挡住它的动作,避免误触发(同基线守卫)。
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)
static key_guard_t s_key_guard;

static lv_obj_t *s_main_scr;
static lv_obj_t *s_setup_scr;
static lv_obj_t *s_help_scr;
static lv_obj_t *s_phase_label;
static lv_obj_t *s_time_canvas;
static lv_obj_t *s_bar;
static lv_obj_t *s_state_label;
static lv_obj_t *s_done_label;
static lv_obj_t *s_battery_label;
static lv_obj_t *s_setup_rows[5];
static lv_obj_t *s_setup_values[5];
static lv_obj_t *s_icon_canvas;
static lv_obj_t *s_sessions_canvas;
static uint8_t s_icon_buf[PIXEL_ICON_SIZE * PIXEL_ICON_SIZE * 2];
static uint8_t s_time_buf[PIXEL_TIME_WIDTH * PIXEL_TIME_HEIGHT * 2];
static int s_theme;                       // 当前主题索引,随完成次数轮换

#define SESSIONS_SLOTS 4
#define SESSIONS_W     (SESSIONS_SLOTS * (PIXEL_MINI_SIZE + 4) - 4)
#define SESSIONS_H     PIXEL_MINI_SIZE
static uint8_t s_sessions_buf[SESSIONS_W * SESSIONS_H * 2];

#define ICON_BASE_Y 86

// ---------------------------------------------------------------------------
// 持久化:时长与完成次数。
// ---------------------------------------------------------------------------
static void settings_save(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "work", (int32_t)s_pomo.work_min);
    nvs_set_i32(h, "brk", (int32_t)s_pomo.break_min);
    nvs_set_i32(h, "done", (int32_t)s_pomo.done);
    nvs_set_i32(h, "theme", (int32_t)s_theme);
    nvs_set_i32(h, "disp", (int32_t)s_display_mode);
    nvs_commit(h);
    nvs_close(h);
}

static void settings_load(int32_t *work_min, int32_t *break_min, int32_t *done) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    nvs_get_i32(h, "work", work_min);
    nvs_get_i32(h, "brk", break_min);
    nvs_get_i32(h, "done", done);
    nvs_close(h);
}

static int display_mode_load(void) {
    nvs_handle_t h;
    int32_t mode = 1;  // 默认 60 秒自动息屏
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, "disp", &mode);
        nvs_close(h);
    }
    if (mode < 0 || mode > DISPLAY_MODE_ALWAYS_ON) mode = 1;
    return (int)mode;
}

static int theme_load(void) {
    nvs_handle_t h;
    int32_t theme = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, "theme", &theme);
        nvs_close(h);
    }
    if (theme < 0 || theme >= PIXEL_THEME_COUNT) theme = 0;
    return (int)theme;
}

// 取当前主题、当前阶段的指定动画帧。调用方须已持有 LVGL 锁。
static void render_icon(int frame) {
    const pixel_theme_t *theme = &PIXEL_THEMES[s_theme];
    const pixel_sprite_t *sprite = s_pomo.phase == POMO_PHASE_WORK
                                 ? theme->focus[frame] : theme->rest[frame];
    pixel_art_render(s_icon_canvas, sprite, PIXEL_ICON_SCALE, C_BG);
}

static void next_theme(void) {
    s_theme = (s_theme + 1) % PIXEL_THEME_COUNT;
    settings_save();
}

// ---------------------------------------------------------------------------
// 提示音:独立任务,避免阻塞 UI 与按键。
// ---------------------------------------------------------------------------
#define BEEP_SAMPLE_RATE 16000
#define BEEP_HZ 1000
#define BEEP_ON_MS 180
#define BEEP_OFF_MS 120
#define BEEP_CHUNK 512

static void beep_play(int count) {
    if (bsp_audio_set_format(BEEP_SAMPLE_RATE, 16, 1) != ESP_OK) return;
    bsp_audio_set_volume(70);
    int16_t buf[BEEP_CHUNK];
    const int period = BEEP_SAMPLE_RATE / BEEP_HZ;
    for (int b = 0; b < count; b++) {
        int total = BEEP_SAMPLE_RATE * BEEP_ON_MS / 1000;
        int phase = 0;
        while (total > 0) {
            int n = total < BEEP_CHUNK ? total : BEEP_CHUNK;
            for (int i = 0; i < n; i++) {
                buf[i] = (phase < period / 2) ? 6000 : -6000;
                if (++phase >= period) phase = 0;
            }
            bsp_audio_write(buf, (size_t)n * sizeof(int16_t));
            total -= n;
        }
        if (b + 1 < count) vTaskDelay(pdMS_TO_TICKS(BEEP_OFF_MS));
    }
}

static void beep_task(void *arg) {
    (void)arg;
    int pattern;
    for (;;) {
        if (xQueueReceive(s_beep_queue, &pattern, portMAX_DELAY) == pdTRUE) {
            if (s_audio_ok) beep_play(pattern);
        }
    }
}

// ---------------------------------------------------------------------------
// UI 构件:统一 Montserrat 文字,禁止换行以避免重叠;像素图案只用于图标。
// ---------------------------------------------------------------------------
static lv_obj_t *text_at(lv_obj_t *parent, const lv_font_t *font,
                         int x, int y, int w, lv_text_align_t align,
                         uint32_t color, const char *text) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(label, w);
    lv_obj_set_pos(label, x, y);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *panel_at(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg) {
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, lv_color_hex(bg), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(C_BORDER), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

static lv_obj_t *screen_new(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_radius(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

static void ui_build_main(void) {
    s_main_scr = screen_new();

    text_at(s_main_scr, &lv_font_montserrat_20, 18, 16, 150, LV_TEXT_ALIGN_LEFT,
            C_TEXT, "Pomodoro");
    s_battery_label = text_at(s_main_scr, &lv_font_montserrat_14, 168, 20, 56,
                              LV_TEXT_ALIGN_RIGHT, C_MUTED, "--");

    s_phase_label = text_at(s_main_scr, &lv_font_montserrat_20, 20, 54, 200,
                            LV_TEXT_ALIGN_CENTER, C_FOCUS, "FOCUS");

    s_icon_canvas = lv_canvas_create(s_main_scr);
    lv_canvas_set_buffer(s_icon_canvas, s_icon_buf,
                         PIXEL_ICON_SIZE, PIXEL_ICON_SIZE, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_icon_canvas, (240 - PIXEL_ICON_SIZE) / 2, ICON_BASE_Y);

    s_time_canvas = lv_canvas_create(s_main_scr);
    lv_canvas_set_buffer(s_time_canvas, s_time_buf,
                         PIXEL_TIME_WIDTH, PIXEL_TIME_HEIGHT, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_time_canvas, (240 - PIXEL_TIME_WIDTH) / 2, 148);

    s_bar = lv_bar_create(s_main_scr);
    lv_obj_set_size(s_bar, 190, 12);
    lv_obj_set_pos(s_bar, 25, 212);
    lv_bar_set_range(s_bar, 0, 1000);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(C_PANEL), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 6, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(C_FOCUS), LV_PART_INDICATOR);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);

    s_state_label = text_at(s_main_scr, &lv_font_montserrat_14, 20, 236, 200,
                            LV_TEXT_ALIGN_CENTER, C_MUTED, "READY");

    s_sessions_canvas = lv_canvas_create(s_main_scr);
    lv_canvas_set_buffer(s_sessions_canvas, s_sessions_buf,
                         SESSIONS_W, SESSIONS_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_sessions_canvas, 46, 262);
    s_done_label = text_at(s_main_scr, &lv_font_montserrat_14, 130, 262, 90,
                           LV_TEXT_ALIGN_LEFT, C_TEXT, "x 0");

    text_at(s_main_scr, &lv_font_montserrat_14, 10, 292, 220, LV_TEXT_ALIGN_CENTER,
            C_MUTED, "OK: RUN   HOLD UP: SETUP");
}

static void ui_build_setup(void) {
    s_setup_scr = screen_new();
    text_at(s_setup_scr, &lv_font_montserrat_20, 20, 14, 200, LV_TEXT_ALIGN_CENTER,
            C_TEXT, "Setup");

    // 五项单行卡片:左名称右取值,上下键选行、OK 改值,充分利用整屏高度。
    static const char *const names[5] = {"Focus", "Break", "Display", "Help", "Reset"};
    for (int i = 0; i < 5; i++) {
        const int y = 44 + i * 50;
        s_setup_rows[i] = panel_at(s_setup_scr, 20, y, 200, 48, C_PANEL);
        text_at(s_setup_rows[i], &lv_font_montserrat_14, 12, 17, 70, LV_TEXT_ALIGN_LEFT,
                C_MUTED, names[i]);
        s_setup_values[i] = text_at(s_setup_rows[i], &lv_font_montserrat_20, 80, 14, 108,
                                    LV_TEXT_ALIGN_RIGHT, C_TEXT, "");
    }
    text_at(s_setup_scr, &lv_font_montserrat_14, 10, 300, 220, LV_TEXT_ALIGN_CENTER,
            C_MUTED, "HOLD OK: save + exit");
}

static void ui_build_help(void) {
    s_help_scr = screen_new();
    text_at(s_help_scr, &lv_font_montserrat_20, 20, 20, 200, LV_TEXT_ALIGN_CENTER,
            C_TEXT, "Controls");
    static const char *const lines[] = {
        "OK: start / pause",
        "HOLD OK: screen off",
        "UP/DOWN: min (READY)",
        "HOLD UP: setup",
        "HOLD DOWN: theme",
        "Sleep: any key wakes",
        "Setup: UP/DOWN select",
        "Setup: OK change value",
    };
    for (int i = 0; i < 8; i++) {
        text_at(s_help_scr, &lv_font_montserrat_14, 16, 58 + i * 28, 208,
                LV_TEXT_ALIGN_LEFT, C_TEXT, lines[i]);
    }
    text_at(s_help_scr, &lv_font_montserrat_14, 10, 292, 220, LV_TEXT_ALIGN_CENTER,
            C_MUTED, "OK: back");
}

static void ui_setup_refresh(void) {
    const bool duration_editable = (s_pomo.state != POMO_STATE_RUNNING);
    for (int i = 0; i < 5; i++) {
        const bool selected = (i == s_setup_field);
        lv_obj_set_style_bg_color(s_setup_rows[i],
                                  lv_color_hex(selected ? C_SELECT : C_PANEL), 0);
        lv_obj_set_style_border_color(s_setup_rows[i],
                                      lv_color_hex(selected ? C_FOCUS : C_BORDER), 0);
    }
    for (int i = 0; i < 2; i++) {
        lv_label_set_text_fmt(s_setup_values[i], "%d min",
                              i == 0 ? s_pomo.work_min : s_pomo.break_min);
        // 运行中时长只读,置灰提示当前段不受影响。
        lv_obj_set_style_text_color(s_setup_values[i],
                                    lv_color_hex(duration_editable ? C_TEXT : C_MUTED), 0);
    }
    lv_label_set_text(s_setup_values[2], k_display_names[s_display_mode]);
    lv_label_set_text(s_setup_values[3], "view");
    lv_label_set_text(s_setup_values[4], "go");
}

static void ui_build(void) {
    ui_build_main();
    ui_build_setup();
    ui_build_help();
    lv_screen_load(s_main_scr);
}

// ---------------------------------------------------------------------------
// 主屏刷新
// ---------------------------------------------------------------------------
static const char *state_text(pomo_state_t state) {
    switch (state) {
        case POMO_STATE_RUNNING: return "RUNNING";
        case POMO_STATE_PAUSED: return "PAUSED";
        default: return "READY";
    }
}

static void ui_refresh(int64_t now_ms, bool force) {
    static int64_t last_sec = -1;
    static int last_state = -1;
    static int last_phase = -1;
    static int last_done = -1;
    static int last_soc = -2;
    static int last_bounce = -1;
    static int64_t last_battery_ms = -BATTERY_REFRESH_MS;

    // 双帧动画 + 每 400ms 上下跳 2 像素;非运行状态回到第 0 帧原位。
    const int frame = (int)((now_ms / 400) % 2);
    const bool animate = (s_pomo.state == POMO_STATE_RUNNING);
    if ((animate && frame != last_bounce) || (!animate && last_bounce != 0)) {
        if (bsp_lvgl_lock(50)) {
            render_icon(animate ? frame : 0);
            lv_obj_set_y(s_icon_canvas, animate ? ICON_BASE_Y - 2 * frame : ICON_BASE_Y);
            bsp_lvgl_unlock();
        }
        last_bounce = animate ? frame : 0;
    }

    const int64_t remaining = pomo_remaining_ms(&s_pomo, now_ms);
    const int64_t sec = (remaining + 999) / 1000;
    int soc = last_soc;
    if (force || now_ms - last_battery_ms >= BATTERY_REFRESH_MS) {
        soc = s_battery_ok ? bsp_battery_soc() : -1;
        last_battery_ms = now_ms;
    }

    const bool changed = force || sec != last_sec
                      || (int)s_pomo.state != last_state
                      || (int)s_pomo.phase != last_phase
                      || s_pomo.done != last_done || soc != last_soc;
    if (!changed) return;

    if (bsp_lvgl_lock(200)) {
        if (sec != last_sec || force) {
            char text[8];
            snprintf(text, sizeof(text), "%02d:%02d", (int)(sec / 60), (int)(sec % 60));
            if (s_pomo.state == POMO_STATE_RUNNING && (sec & 1)) text[2] = ' ';
            pixel_time_render(s_time_canvas, text,
                              lv_color_hex(s_pomo.phase == POMO_PHASE_WORK ? C_FOCUS : C_BREAK),
                              lv_color_hex(C_BG));
            const int64_t total = pomo_total_ms(&s_pomo);
            const int64_t elapsed = total - remaining;
            lv_bar_set_value(s_bar, total > 0 ? (int)(elapsed * 1000 / total) : 0,
                             LV_ANIM_OFF);
        }
        if ((int)s_pomo.phase != last_phase || force) {
            const uint32_t color = s_pomo.phase == POMO_PHASE_WORK ? C_FOCUS : C_BREAK;
            lv_label_set_text(s_phase_label, pomo_phase_name(s_pomo.phase));
            lv_obj_set_style_text_color(s_phase_label, lv_color_hex(color), 0);
            lv_obj_set_style_bg_color(s_bar, lv_color_hex(color), LV_PART_INDICATOR);
            render_icon(0);
            last_bounce = 0;
        }
        if ((int)s_pomo.state != last_state || (int)s_pomo.phase != last_phase || force) {
            lv_label_set_text(s_state_label, state_text(s_pomo.state));
        }
        if (s_pomo.done != last_done || force) {
            lv_label_set_text_fmt(s_done_label, "x %d", s_pomo.done);
            lv_canvas_fill_bg(s_sessions_canvas, lv_color_hex(C_BG), LV_OPA_COVER);
            int slots = s_pomo.done < SESSIONS_SLOTS ? s_pomo.done : SESSIONS_SLOTS;
            for (int i = 0; i < slots; i++) {
                pixel_art_blit(s_sessions_canvas, &PIXEL_TOMATO_8,
                               i * (PIXEL_MINI_SIZE + 4), 0, PIXEL_MINI_SCALE);
            }
            lv_obj_invalidate(s_sessions_canvas);
        }
        if (soc != last_soc || force) {
            if (soc >= 0) lv_label_set_text_fmt(s_battery_label, "%d%%", soc);
            else lv_label_set_text(s_battery_label, "--");
        }
        bsp_lvgl_unlock();
    }

    last_sec = sec;
    last_state = s_pomo.state;
    last_phase = s_pomo.phase;
    last_done = s_pomo.done;
    last_soc = soc;
}

// ---------------------------------------------------------------------------
// 输入:回调只入队,逻辑与 UI 都在 app 任务里串行处理。
// ---------------------------------------------------------------------------
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    // 先消费守卫再判断就绪:松手事件必须能解除唤醒键屏蔽。
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        return;
    }
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

static void enter_setup(void) {
    s_mode = MODE_SETUP;
    s_setup_field = 0;
    if (bsp_lvgl_lock(200)) {
        ui_setup_refresh();
        lv_screen_load(s_setup_scr);
        bsp_lvgl_unlock();
    }
}

static void exit_setup(void) {
    settings_save();
    s_mode = MODE_MAIN;
    if (bsp_lvgl_lock(200)) {
        lv_screen_load(s_main_scr);
        bsp_lvgl_unlock();
    }
}

static void handle_setup_input(const input_event_t *input) {
    if (input->btn == BSP_BTN_OK && input->event == BSP_BTN_LONG) {
        exit_setup();
        return;
    }
    if (input->event != BSP_BTN_CLICK) return;

    if (input->btn == BSP_BTN_UP || input->btn == BSP_BTN_DOWN) {
        // 上下键只移动选中行,循环五选一。
        const int delta = input->btn == BSP_BTN_DOWN ? 1 : -1;
        s_setup_field = (s_setup_field + delta + 5) % 5;
    } else if (input->btn == BSP_BTN_OK) {
        // OK 修改选中项:时长 +1 分钟循环,Display 循环档位,Help/Reset 直接执行。
        switch (s_setup_field) {
            case 0: case 1: {
                if (s_pomo.state == POMO_STATE_RUNNING) return;  // 运行中时长只读
                const pomo_phase_t phase = s_setup_field == 0 ? POMO_PHASE_WORK : POMO_PHASE_BREAK;
                const int current = s_setup_field == 0 ? s_pomo.work_min : s_pomo.break_min;
                pomo_set_minutes(&s_pomo, phase, current >= 60 ? 1 : current + 1);
                break;
            }
            case 2:
                s_display_mode = (s_display_mode + 1) % 5;
                break;
            case 3:
                s_mode = MODE_HELP;
                if (bsp_lvgl_lock(200)) {
                    lv_screen_load(s_help_scr);
                    bsp_lvgl_unlock();
                }
                return;
            default:
                pomo_reset(&s_pomo);
                exit_setup();
                return;
        }
    } else {
        return;
    }
    if (bsp_lvgl_lock(200)) {
        ui_setup_refresh();
        bsp_lvgl_unlock();
    }
}

static void handle_help_input(const input_event_t *input) {
    if (input->btn != BSP_BTN_OK) return;
    if (input->event != BSP_BTN_CLICK && input->event != BSP_BTN_LONG) return;
    s_mode = MODE_SETUP;
    if (bsp_lvgl_lock(200)) {
        ui_setup_refresh();
        lv_screen_load(s_setup_scr);
        bsp_lvgl_unlock();
    }
}

static void handle_main_input(const input_event_t *input, int64_t now_ms) {
    if (input->btn == BSP_BTN_OK && input->event == BSP_BTN_LONG) {
        bsp_display_backlight(0);           // 手动息屏,计时继续
        s_screen_on = false;
        return;
    }
    if (input->btn == BSP_BTN_UP && input->event == BSP_BTN_LONG) {
        enter_setup();                      // RUNNING 也可进设置(时长行只读)
        return;
    }
    if (input->btn == BSP_BTN_DOWN && input->event == BSP_BTN_LONG) {
        next_theme();
        if (bsp_lvgl_lock(200)) {
            render_icon(0);
            bsp_lvgl_unlock();
        }
        return;
    }
    if (input->event != BSP_BTN_CLICK) return;
    if (input->btn == BSP_BTN_OK) {
        pomo_toggle(&s_pomo, now_ms);
    } else if (input->btn == BSP_BTN_UP || input->btn == BSP_BTN_DOWN) {
        if (s_pomo.state != POMO_STATE_READY) return;
        pomo_adjust(&s_pomo, input->btn == BSP_BTN_UP ? 1 : -1);
        settings_save();
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Pomodoro 启动");
    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    if (wake_causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
        ESP_LOGI(TAG, "按键唤醒:先屏蔽按键动作,直到那颗键松手");
    }
    bsp_i2c_init();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,无法运行");
        return;
    }
    bsp_display_backlight(100);

    s_audio_ok = (bsp_audio_init() == ESP_OK);
    s_battery_ok = (bsp_battery_init() == ESP_OK);
    if (!s_audio_ok) ESP_LOGW(TAG, "音频不可用,阶段结束只改屏幕不发声");

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要擦除后重建: %s", esp_err_to_name(nvs_err));
        nvs_flash_erase();
        nvs_flash_init();
    }

    int32_t work_min = 25, break_min = 5, done = 0;
    settings_load(&work_min, &break_min, &done);
    pomo_init(&s_pomo, (int)work_min, (int)break_min);
    s_pomo.done = done < 0 ? 0 : (int)done;
    s_theme = theme_load();
    s_display_mode = display_mode_load();

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    s_beep_queue = xQueueCreate(2, sizeof(int));
    if (!s_input_queue || !s_beep_queue
        || xTaskCreate(beep_task, "pomo_beep", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "任务/队列创建失败");
        return;
    }
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败");
        return;
    }
    if (s_key_guard.armed) {
        // 唤醒键若在按键驱动启动前已松手,不会产生 release 事件,按真实电压同步撤防。
        const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
    }
    s_input_ready = true;

    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "LVGL 锁获取失败");
        return;
    }
    ui_build();
    bsp_lvgl_unlock();

    ui_refresh(esp_timer_get_time() / 1000, true);
    s_last_activity_ms = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG, "就绪:work=%d break=%d done=%d audio=%d battery=%d",
             s_pomo.work_min, s_pomo.break_min, s_pomo.done, s_audio_ok, s_battery_ok);

    for (;;) {
        input_event_t input;
        const bool got = xQueueReceive(s_input_queue, &input,
                                       pdMS_TO_TICKS(UI_TICK_MS)) == pdTRUE;
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (got) {
            if (!s_screen_on) {
                // 松手事件不唤醒:否则长按息屏后,松开 OK 的 RELEASE 会立刻点亮屏幕。
                if (input.event == BSP_BTN_RELEASE) continue;
                // 息屏时第一次按键只点亮屏幕,不触发功能,避免误操作。
                bsp_display_backlight(100);
                s_screen_on = true;
                s_last_activity_ms = now_ms;
                ui_refresh(now_ms, true);
                continue;
            }
            s_last_activity_ms = now_ms;
            if (s_mode == MODE_SETUP) handle_setup_input(&input);
            else if (s_mode == MODE_HELP) handle_help_input(&input);
            else handle_main_input(&input, now_ms);
        }

        if (s_display_mode != DISPLAY_MODE_ALWAYS_ON && s_screen_on
            && now_ms - s_last_activity_ms >= k_screen_off_ms[s_display_mode]) {
            bsp_display_backlight(0);
            s_screen_on = false;
        }

        const pomo_event_t event = pomo_tick(&s_pomo, now_ms);
        if (event != POMO_EVENT_NONE) {
            if (event == POMO_EVENT_WORK_DONE) next_theme();  // 每完成一个番茄换主题
            settings_save();
            const int pattern = event == POMO_EVENT_WORK_DONE ? 3 : 2;
            (void)xQueueSend(s_beep_queue, &pattern, 0);
            ESP_LOGI(TAG, "%s,done=%d",
                     event == POMO_EVENT_WORK_DONE ? "专注结束,进入休息" : "休息结束,回到专注",
                     s_pomo.done);
        }
        if (s_mode == MODE_MAIN && s_screen_on) {
            ui_refresh(now_ms, got || event != POMO_EVENT_NONE);
        }
    }
}
