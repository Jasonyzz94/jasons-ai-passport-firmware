// main/ui.h —— 工具箱视觉系统:配色、字体、通用控件。
//
// 风格:深色玻璃质感卡片 + 每个工具一个强调色 + 像素图标点缀。
// 屏幕 240x320,四角 30px 圆角遮罩:贴边内容至少内缩 8px(顶/底行更多)。
// 布局分三段:状态栏 0..UI_BAR_H、内容区、底部按键提示 UI_HINT_H。
#pragma once

#include "lvgl.h"
#include "pixel_art.h"
#include <stdbool.h>

#define UI_W        240
#define UI_H        320
#define UI_BAR_H    30
#define UI_HINT_H   30
#define UI_CONTENT_Y UI_BAR_H
#define UI_CONTENT_H (UI_H - UI_BAR_H - UI_HINT_H)
#define UI_PAD      12

// 配色(RGB888)。
#define C_BG        0x0B0E14
#define C_SURFACE   0x151A23
#define C_SURFACE2  0x1E2531
#define C_LINE      0x2A3241
#define C_TEXT      0xF1F4F9
#define C_TEXT2     0xA3ADBD
#define C_TEXT3     0x667185
#define C_OK        0x34D399
#define C_WARN      0xFBBF24
#define C_ERR       0xF87171

// 每个工具的强调色。
#define C_TOMATO    0xFF6B57
#define C_BREAK     0x2DD4BF
#define C_CLOCK     0x60A5FA
#define C_REMOTE    0x8B7CF6
#define C_SOUND     0x34D399
#define C_HOME      0xFBBF24
#define C_BOARD     0x38BDF8
#define C_WIFI      0x4ADE80
#define C_LINK      0xF472B6
#define C_AI        0xA78BFA
#define C_TIMER     0xFACC15
#define C_DICE      0xFB923C
#define C_SETTINGS  0x94A3B8

// 字体:中文小字 14(提示/注释)、中文正文 16、中文粗体标题 20、粗体数字 36/64。均已挂好图标回退。
extern lv_font_t ui_font_14;
extern lv_font_t ui_font_16;
extern lv_font_t ui_font_20b;
extern const lv_font_t *ui_font_36b;
extern const lv_font_t *ui_font_64b;

void ui_init(void);

lv_color_t ui_c(uint32_t rgb);
// 把 rgb 与 bg 按 mix(0..255,255=全 rgb)混合,用于做暗色衬底。
uint32_t ui_mix(uint32_t rgb, uint32_t bg, uint8_t mix);

// 清掉默认样式的容器(无边框、无滚动、透明)。
lv_obj_t *ui_box(lv_obj_t *parent, int x, int y, int w, int h);
// 圆角卡片。
lv_obj_t *ui_card(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg, int radius);
lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text);
// 固定宽度、单行、超出显示省略号的标签。
lv_obj_t *ui_label_w(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                     int w, lv_text_align_t align, const char *text);
// 像素精灵图片(无抗锯齿整数放大,透明底)。
lv_obj_t *ui_pix(lv_obj_t *parent, const pixel_sprite_t *sprite, int scale);
void ui_pix_set(lv_obj_t *img, const pixel_sprite_t *sprite);
// 小圆角标签胶囊。
lv_obj_t *ui_pill(lv_obj_t *parent, const char *text, uint32_t fg, uint32_t bg);
// 横向进度条(轨道 + 填充),value 0..1000。
lv_obj_t *ui_bar(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color);
void ui_bar_set(lv_obj_t *bar, int value_permille);

// ---------------------------------------------------------------------------
// 列表:上下键移动焦点,自动滚动;行 = [图标] 标题 [值] [>]。
// ---------------------------------------------------------------------------
#define UI_LIST_MAX 24
typedef struct {
    lv_obj_t *cont;
    lv_obj_t *rows[UI_LIST_MAX];
    lv_obj_t *values[UI_LIST_MAX];
    lv_obj_t *titles[UI_LIST_MAX];
    lv_obj_t *icons[UI_LIST_MAX];
    int count;
    int focus;
    uint32_t accent;
    int row_h;
    int w;
} ui_list_t;

void ui_list_create(ui_list_t *list, lv_obj_t *parent, int x, int y, int w, int h,
                    uint32_t accent);
// 返回行号;icon 可为 NULL;value 可为 NULL(无值列);chevron 显示右箭头。
int ui_list_add(ui_list_t *list, const pixel_sprite_t *icon, const char *title,
                const char *value, bool chevron);
void ui_list_set_value(ui_list_t *list, int idx, const char *value);
void ui_list_set_value_color(ui_list_t *list, int idx, uint32_t color);
void ui_list_set_title(ui_list_t *list, int idx, const char *title);
void ui_list_set_icon(ui_list_t *list, int idx, const pixel_sprite_t *icon);
void ui_list_focus(ui_list_t *list, int idx, bool anim);
// 上下键移动(循环)。返回新焦点。
int ui_list_move(ui_list_t *list, int delta);
void ui_list_clear(ui_list_t *list);

// 空状态/错误提示:图标 + 标题 + 说明,居中放在 parent 里。
lv_obj_t *ui_empty(lv_obj_t *parent, const pixel_sprite_t *icon, const char *title,
                   const char *detail);
