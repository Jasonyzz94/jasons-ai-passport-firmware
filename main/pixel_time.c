// main/pixel_time.c —— 5x7 点阵数字/冒号,整数倍放大绘制,保持锐利像素边缘。
#include "pixel_time.h"
#include <string.h>

// 每字 7 行,每行低 5 位有效,bit4 为最左像素。
static const uint8_t GLYPHS[11][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},  // 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},  // 1
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},  // 2
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},  // 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},  // 4
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},  // 5
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},  // 6
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},  // 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},  // 8
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},  // 9
    {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00},  // :
};

static const uint8_t *glyph_for(char c) {
    if (c >= '0' && c <= '9') return GLYPHS[c - '0'];
    if (c == ':') return GLYPHS[10];
    return NULL;
}

void pixel_time_render(lv_obj_t *canvas, const char *text, lv_color_t fg, lv_color_t bg) {
    lv_canvas_fill_bg(canvas, bg, LV_OPA_COVER);

    const size_t len = strlen(text);
    for (size_t ci = 0; ci < len && ci < 5; ci++) {
        const uint8_t *glyph = glyph_for(text[ci]);
        if (!glyph) continue;
        const int base_x = (int)ci * 6 * PIXEL_TIME_SCALE;
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (!(glyph[row] & (1u << (4 - col)))) continue;
                const int x0 = base_x + col * PIXEL_TIME_SCALE;
                const int y0 = row * PIXEL_TIME_SCALE;
                for (int dy = 0; dy < PIXEL_TIME_SCALE; dy++) {
                    for (int dx = 0; dx < PIXEL_TIME_SCALE; dx++) {
                        lv_canvas_set_px(canvas, x0 + dx, y0 + dy, fg, LV_OPA_COVER);
                    }
                }
            }
        }
    }
    lv_obj_invalidate(canvas);
}
