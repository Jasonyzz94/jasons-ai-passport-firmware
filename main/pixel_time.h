// main/pixel_time.h —— 像素风大号时间渲染:5x7 点阵字模按整数倍放大,写入 LVGL canvas。
#pragma once

#include "lvgl.h"

// 画布尺寸(像素):5 个字符(mm:ss),每字 5+1 列、7 行,放大 PIXEL_TIME_SCALE 倍。
#define PIXEL_TIME_SCALE 6
#define PIXEL_TIME_WIDTH  (5 * 6 * PIXEL_TIME_SCALE)
#define PIXEL_TIME_HEIGHT (7 * PIXEL_TIME_SCALE)

// text 仅支持 '0'..'9' 与 ':';其他字符按空白处理。buf 至少
// PIXEL_TIME_WIDTH * PIXEL_TIME_HEIGHT * 2 字节(RGB565)。
void pixel_time_render(lv_obj_t *canvas, const char *text, lv_color_t fg, lv_color_t bg);
