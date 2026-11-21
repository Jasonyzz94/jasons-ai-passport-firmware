// main/pixel_types.h —— 字符画像素精灵的数据类型(不依赖 LVGL,便于主机测试)。
#pragma once

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

