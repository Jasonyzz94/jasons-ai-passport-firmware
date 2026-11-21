// sim/sim_main.c —— 桌面模拟器:用真实的 shell/应用/字体渲染每个界面并导出 PPM 截图。
//
// 用法:sim/run.sh [输出目录]。截图包含与真机相同的 30px 圆角遮罩。
// 只用于视觉验收;不模拟硬件时序、内存与功耗。
#include "apps.h"
#include "bsp_display_rounding.h"
#include "settings.h"
#include "sim.h"
#include "ui.h"

#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 240
#define H 320

static uint16_t s_fb[W * H];
static uint8_t s_draw_buf[W * H * 2];
static const char *s_out = "build/sim";

static uint32_t tick_cb(void) { return (uint32_t)g_sim_now_ms; }

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    const uint16_t *src = (const uint16_t *)px;
    const int w = lv_area_get_width(area);
    for (int y = area->y1; y <= area->y2; y++) {
        memcpy(&s_fb[y * W + area->x1], &src[(y - area->y1) * w], (size_t)w * 2);
    }
    lv_display_flush_ready(disp);
}

static void skip_ms(int ms);
static void run_ms(int ms)
{
    if (ms > 5000) {
        skip_ms(ms);
        return;
    }
    for (int t = 0; t < ms; t += 5) {
        g_sim_now_ms += 5;
        lv_timer_handler();
    }
}

// 快进:大步推进时间(每 500ms 跑一次定时器),用于跳过长时间计时。
static void skip_ms(int ms)
{
    for (int t = 0; t < ms; t += 500) {
        g_sim_now_ms += 500;
        lv_timer_handler();
    }
}

static void shot(const char *name)
{
    run_ms(400);
    lv_refr_now(NULL);
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.ppm", s_out, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    const float bl = g_sim_backlight / 100.0f;
    for (int y = 0; y < H; y++) {
        int32_t x1, x2;
        const bool vis = bsp_display_rounded_row_span(y, W, H, 30 /* BSP_LVGL_SCREEN_RADIUS */, &x1, &x2);
        for (int x = 0; x < W; x++) {
            uint8_t rgb[3] = {0, 0, 0};
            if (vis && x >= x1 && x <= x2) {
                const uint16_t c = s_fb[y * W + x];
                rgb[0] = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31 * bl);
                rgb[1] = (uint8_t)(((c >> 5) & 0x3F) * 255 / 63 * bl);
                rgb[2] = (uint8_t)((c & 0x1F) * 255 / 31 * bl);
            }
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    printf("shot %s\n", name);
}

static void key(sys_key_t k, sys_key_ev_t e)
{
    if (e == SYS_EV_CLICK) sim_key(k, SYS_EV_PRESS);
    sim_key(k, e);
    if (e == SYS_EV_CLICK || e == SYS_EV_LONG) sim_key(k, SYS_EV_RELEASE);
    run_ms(60);
}

static void open_app(const app_t *app)
{
    shell_open(app);
    run_ms(300);
}

void sim_scenario(void (*shot)(const char *), void (*key)(sys_key_t, sys_key_ev_t),
                  void (*run_ms)(int), void (*open_app)(const app_t *));

int main(int argc, char **argv)
{
    if (argc > 1) s_out = argv[1];
    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, s_draw_buf, NULL, sizeof(s_draw_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    ui_init();
    settings_load();
    pomo_service_init();
    link_service_init();
    timer_service_init();
    shell_start();
    run_ms(500);

    sim_scenario(shot, key, run_ms, open_app);
    return 0;
}
