// main/pixel_art.h —— 字符画像素精灵、双帧动画与主题轮换。
#pragma once

#include "lvgl.h"
#include "pixel_types.h"
#include <stdint.h>

// 在 canvas 上按 scale 整数倍绘制精灵,bg_rgb 为背景色。
void pixel_art_render(lv_obj_t *canvas, const pixel_sprite_t *sprite,
                      int scale, uint32_t bg_rgb);
// 在已有画布上继续绘制(用于一张画布摆多个精灵),不清背景。
void pixel_art_blit(lv_obj_t *canvas, const pixel_sprite_t *sprite,
                    int origin_x, int origin_y, int scale);

extern const pixel_sprite_t PIXEL_TOMATO_8; // 8x8 小番茄(完成次数)

#define PIXEL_THEME_COUNT 3
extern const pixel_theme_t PIXEL_THEMES[PIXEL_THEME_COUNT];

#define PIXEL_ICON_SCALE 3
#define PIXEL_ICON_SIZE  (16 * PIXEL_ICON_SCALE)
#define PIXEL_MINI_SCALE 2
#define PIXEL_MINI_SIZE  (8 * PIXEL_MINI_SCALE)
