// main/pixel_art.c —— 字符画像素精灵渲染、动画帧与主题。
#include "pixel_art.h"

static uint32_t color_of(const pixel_sprite_t *sprite, char key) {
    for (const pixel_color_t *c = sprite->colors; c && c->key; c++) {
        if (c->key == key) return c->rgb;
    }
    return 0;
}

static bool has_key(const pixel_sprite_t *sprite, char key) {
    for (const pixel_color_t *c = sprite->colors; c && c->key; c++) {
        if (c->key == key) return true;
    }
    return false;
}

void pixel_art_blit(lv_obj_t *canvas, const pixel_sprite_t *sprite,
                    int origin_x, int origin_y, int scale) {
    for (int y = 0; y < sprite->height; y++) {
        const char *row = sprite->rows[y];
        for (int x = 0; x < sprite->width; x++) {
            const char key = row[x];
            if (key == '.' || key == ' ' || !has_key(sprite, key)) continue;
            const lv_color_t color = lv_color_hex(color_of(sprite, key));
            for (int dy = 0; dy < scale; dy++) {
                for (int dx = 0; dx < scale; dx++) {
                    lv_canvas_set_px(canvas,
                                     origin_x + x * scale + dx,
                                     origin_y + y * scale + dy,
                                     color, LV_OPA_COVER);
                }
            }
        }
    }
}

void pixel_art_render(lv_obj_t *canvas, const pixel_sprite_t *sprite,
                      int scale, uint32_t bg_rgb) {
    lv_canvas_fill_bg(canvas, lv_color_hex(bg_rgb), LV_OPA_COVER);
    pixel_art_blit(canvas, sprite, 0, 0, scale);
    lv_obj_invalidate(canvas);
}

#define SPRITE(name, rows_, colors_) \
    const pixel_sprite_t name = { .width = 16, .height = 16, .rows = rows_, .colors = colors_ }

// ---------------------------------------------------------------------------
// 主题 0 果园:番茄(专注)/ 咖啡(休息)
// ---------------------------------------------------------------------------
static const pixel_color_t TOMATO_COLORS[] = {
    {'R', 0xE74C3C}, {'r', 0xC0392B}, {'G', 0x3FD68F}, {'g', 0x2AA06B},
    {'W', 0xFF8A75}, {0, 0},
};

static const char *TOMATO_A[16] = {
    "......GGGG......",
    ".....GgGGgG.....",
    "..GGGgGGGGgGGG..",
    "...gGGGGGGGGg...",
    "....RRRRRRRR....",
    "..RRRRRRRRRRRR..",
    ".RRWWRRRRRRRRRR.",
    ".RWWRRRRRRRRRRR.",
    "RWRRRRRRRRRRRRRR",
    "RRRRRRRRRRRRRRRR",
    "RRRRRRRRRRRRRRRR",
    ".RRRRRRRRRRRRRR.",
    ".RRRRRRRRRRRRRR.",
    "..RRRRRRRRRRRR..",
    "...RRRRRRRRRR...",
    ".....RRRRRR.....",
};

// 第二帧:叶子歪向另一侧,果身压扁一格,形成"呼吸"感。
static const char *TOMATO_B[16] = {
    "................",
    "......GGGG......",
    "....GGgGGgG.....",
    "...GGgGGGGgGG...",
    "....gGGGGGGg....",
    "..RRRRRRRRRRRR..",
    ".RRWWRRRRRRRRRR.",
    ".RWWRRRRRRRRRRR.",
    "RWRRRRRRRRRRRRRR",
    "RRRRRRRRRRRRRRRR",
    "RRRRRRRRRRRRRRRR",
    ".RRRRRRRRRRRRRR.",
    ".RRRRRRRRRRRRRR.",
    "..RRRRRRRRRRRR..",
    "...RRRRRRRRRR...",
    ".....RRRRRR.....",
};

SPRITE(SPR_TOMATO_A, TOMATO_A, TOMATO_COLORS);
SPRITE(SPR_TOMATO_B, TOMATO_B, TOMATO_COLORS);

static const pixel_color_t COFFEE_COLORS[] = {
    {'C', 0xFFB020}, {'D', 0xB97A0F}, {'S', 0x8A97A8}, {0, 0},
};

static const char *COFFEE_A[16] = {
    "................",
    "....S..S........",
    ".....S..S..S....",
    "....S..S..S.....",
    "..CCCCCCCCCC....",
    "..CCCCCCCCCC.CC.",
    "..CCCCCCCCCC..C.",
    "..CCCCCCCCCC..C.",
    "..CCCCCCCCCC.CC.",
    "..CCCCCCCCCC....",
    "...CCCCCCCC.....",
    "..DDDDDDDDDD....",
    ".DDDDDDDDDDDD...",
    ".DDDDDDDDDDDD...",
    "................",
    "................",
};

// 第二帧:蒸汽飘向另一侧。
static const char *COFFEE_B[16] = {
    "................",
    "......S..S......",
    "...S..S..S......",
    "....S..S..S.....",
    "..CCCCCCCCCC....",
    "..CCCCCCCCCC.CC.",
    "..CCCCCCCCCC..C.",
    "..CCCCCCCCCC..C.",
    "..CCCCCCCCCC.CC.",
    "..CCCCCCCCCC....",
    "...CCCCCCCC.....",
    "..DDDDDDDDDD....",
    ".DDDDDDDDDDDD...",
    ".DDDDDDDDDDDD...",
    "................",
    "................",
};

SPRITE(SPR_COFFEE_A, COFFEE_A, COFFEE_COLORS);
SPRITE(SPR_COFFEE_B, COFFEE_B, COFFEE_COLORS);

// ---------------------------------------------------------------------------
// 主题 1 猫咪:端坐(专注)/ 睡觉(休息)
// ---------------------------------------------------------------------------
static const pixel_color_t CAT_COLORS[] = {
    {'O', 0xF0A35E}, {'o', 0xC77E3D}, {'K', 0x1B2430},
    {'P', 0xE98A8A}, {'Z', 0x8A97A8}, {0, 0},
};

static const char *CAT_SIT_A[16] = {
    "..OO........OO..",
    "..OOO......OOO..",
    "..OOOOOOOOOOOO..",
    "..OOOOOOOOOOOO..",
    "..OOKOOOOOOKOO..",
    "..OOOOOOOOOOOO..",
    "..OOOOPPPPOOOO..",
    "..OOOOOOOOOOOO..",
    "...OOOOOOOOOO...",
    "...OOOOOOOOOO...",
    "...OOOOOOOOOO...",
    "...OOOOOOOOOO...",
    "...OOOOOOOOOO...",
    "...OOOOOOOOOO...",
    "...OOOOOOOOOO...",
    "................",
};

// 第二帧:闭眼 + 耳朵一高一低。
static const char *CAT_SIT_B[16] = {
    "..OO.........O..",
    "..OOO.......OO..",
    "..OOOOOOOOOOOO..",
    "..OOOOOOOOOOOO..",
    "..OooOOOOOOooO..",
    "..OOOOOOOOOOOO..",
    "..OOOOPPPPOOOO..",
    "..OOOOOOOOOOOO..",
    "...OOOOOOOOOO...",
    "...OOOOOOOOOO...",
    "...OOOOOOOOOO...",
    "...OOOOOOOOOO...",
    "...OOOOOOOOOO...",
    "...OOOOOOOOOO...",
    "...OOOOOOOOOO...",
    "................",
};

SPRITE(SPR_CAT_SIT_A, CAT_SIT_A, CAT_COLORS);
SPRITE(SPR_CAT_SIT_B, CAT_SIT_B, CAT_COLORS);

static const char *CAT_SLEEP_A[16] = {
    ".............Z..",
    "...........Z....",
    ".....Z..........",
    "..OOOOOOOOOO....",
    ".OOOOOOOOOOOOO..",
    ".OOOOooooOOOOO..",
    ".OOOOOOOOOOOOO..",
    ".OOOOOOOOOOOOOO.",
    ".OOOOOOOOOOOOOO.",
    ".OOOOOOOOOOOOOO.",
    "..OOOOOOOOOOOO..",
    "..oooooooooooo..",
    "................",
    "................",
    "................",
    "................",
};

// 第二帧:Z 的位置变化,模拟飘出。
static const char *CAT_SLEEP_B[16] = {
    "............Z...",
    "..............Z.",
    "......Z.........",
    "..OOOOOOOOOO....",
    ".OOOOOOOOOOOOO..",
    ".OOOOooooOOOOO..",
    ".OOOOOOOOOOOOO..",
    ".OOOOOOOOOOOOOO.",
    ".OOOOOOOOOOOOOO.",
    ".OOOOOOOOOOOOOO.",
    "..OOOOOOOOOOOO..",
    "..oooooooooooo..",
    "................",
    "................",
    "................",
    "................",
};

SPRITE(SPR_CAT_SLEEP_A, CAT_SLEEP_A, CAT_COLORS);
SPRITE(SPR_CAT_SLEEP_B, CAT_SLEEP_B, CAT_COLORS);

// ---------------------------------------------------------------------------
// 主题 2 太空:火箭(专注)/ 月亮与星星(休息)
// ---------------------------------------------------------------------------
static const pixel_color_t SPACE_COLORS[] = {
    {'W', 0xE6EDF3}, {'R', 0xE74C3C}, {'B', 0x6EC1FF},
    {'F', 0xFFB020}, {'f', 0xFF6B35}, {'M', 0xF5E6B8},
    {'S', 0xFFF3A0}, {0, 0},
};

static const char *ROCKET_A[16] = {
    ".......WW.......",
    "......WWWW......",
    "......WBBW......",
    "......WBBW......",
    "......WWWW......",
    "......WWWW......",
    ".....RWWWWR.....",
    ".....RWWWWR.....",
    "....RRWWWWRR....",
    "....RRWWWWRR....",
    "....RRRRRRRR....",
    ".......FF.......",
    "......FFFF......",
    "......fFFf......",
    ".......ff.......",
    "................",
};

// 第二帧:火苗变长并分叉。
static const char *ROCKET_B[16] = {
    ".......WW.......",
    "......WWWW......",
    "......WBBW......",
    "......WBBW......",
    "......WWWW......",
    "......WWWW......",
    ".....RWWWWR.....",
    ".....RWWWWR.....",
    "....RRWWWWRR....",
    "....RRWWWWRR....",
    "....RRRRRRRR....",
    ".......FF.......",
    "......FFFF......",
    ".....fFffFf.....",
    "......f..f......",
    "................",
};

SPRITE(SPR_ROCKET_A, ROCKET_A, SPACE_COLORS);
SPRITE(SPR_ROCKET_B, ROCKET_B, SPACE_COLORS);

static const char *MOON_A[16] = {
    ".....MMMM.......",
    "...MMMMMM.......",
    "..MMMM..........",
    "..MMM...........",
    "..MMM......S....",
    "..MMM...........",
    "..MMM...........",
    "..MMMM..........",
    "...MMMMMM.......",
    ".....MMMM.......",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
};

// 第二帧:星星闪烁换位。
static const char *MOON_B[16] = {
    ".....MMMM.......",
    "...MMMMMM.......",
    "..MMMM..........",
    "..MMM...........",
    "..MMM...........",
    "..MMM...........",
    "..MMM....S......",
    "..MMMM..........",
    "...MMMMMM.......",
    ".....MMMM.......",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
};

SPRITE(SPR_MOON_A, MOON_A, SPACE_COLORS);
SPRITE(SPR_MOON_B, MOON_B, SPACE_COLORS);

const pixel_theme_t PIXEL_THEMES[PIXEL_THEME_COUNT] = {
    { .name = "GARDEN", .focus = { &SPR_TOMATO_A, &SPR_TOMATO_B },
      .rest = { &SPR_COFFEE_A, &SPR_COFFEE_B } },
    { .name = "CATS", .focus = { &SPR_CAT_SIT_A, &SPR_CAT_SIT_B },
      .rest = { &SPR_CAT_SLEEP_A, &SPR_CAT_SLEEP_B } },
    { .name = "SPACE", .focus = { &SPR_ROCKET_A, &SPR_ROCKET_B },
      .rest = { &SPR_MOON_A, &SPR_MOON_B } },
};

// 8x8 小番茄(完成次数)
static const char *TOMATO8_ROWS[8] = {
    "...GG...",
    "..GgGG..",
    ".RRRRRR.",
    "RRRRRRRR",
    "RRRRRRRR",
    "RRRRRRRR",
    ".RRRRRR.",
    "..RRRR..",
};

const pixel_sprite_t PIXEL_TOMATO_8 = {
    .width = 8, .height = 8, .rows = TOMATO8_ROWS, .colors = TOMATO_COLORS,
};
