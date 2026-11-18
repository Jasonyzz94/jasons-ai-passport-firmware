// main/pixel_art.h —— 字符画像素精灵、双帧动画与主题轮换。
#pragma once

#include "lvgl.h"
#include <stdint.h>

typedef struct {
    char key;          // 字符画里的像素符号
    uint32_t rgb;      // 对应颜色
} pixel_color_t;

typedef struct {
    int width;
    int height;
    const char *const *rows;    // height 行,每行 width 个字符
    const pixel_color_t *colors; // 以 key == '\0' 结尾
} pixel_sprite_t;

// 一套主题:专注 / 休息各两帧,400ms 交替形成动画。
typedef struct {
    const char *name;
    const pixel_sprite_t *focus[2];
    const pixel_sprite_t *rest[2];
} pixel_theme_t;

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
