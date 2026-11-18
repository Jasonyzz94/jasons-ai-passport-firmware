// main/ui.c —— 工具箱视觉系统实现。
#include "ui.h"

#include <stdlib.h>
#include <string.h>

LV_FONT_DECLARE(toolbox_cn_14);
LV_FONT_DECLARE(toolbox_cn_16);
LV_FONT_DECLARE(toolbox_cn_20b);
LV_FONT_DECLARE(toolbox_num_36b);
LV_FONT_DECLARE(toolbox_num_64b);

lv_font_t ui_font_14;
lv_font_t ui_font_16;
lv_font_t ui_font_20b;
const lv_font_t *ui_font_36b = &toolbox_num_36b;
const lv_font_t *ui_font_64b = &toolbox_num_64b;

void ui_init(void)
{
    // 生成字库只含文字;LV_SYMBOL_* 图标由 Montserrat 回退提供。
    ui_font_14 = toolbox_cn_14;
    ui_font_14.fallback = &lv_font_montserrat_14;
    ui_font_16 = toolbox_cn_16;
    ui_font_16.fallback = &lv_font_montserrat_14;
    ui_font_20b = toolbox_cn_20b;
    ui_font_20b.fallback = &lv_font_montserrat_20;
}

lv_color_t ui_c(uint32_t rgb) { return lv_color_hex(rgb); }

uint32_t ui_mix(uint32_t rgb, uint32_t bg, uint8_t mix)
{
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        const uint32_t a = (rgb >> shift) & 0xFF;
        const uint32_t b = (bg >> shift) & 0xFF;
        out |= ((a * mix + b * (255u - mix)) / 255u) << shift;
    }
    return out;
}

lv_obj_t *ui_box(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t *ui_card(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg, int radius)
{
    lv_obj_t *o = ui_box(parent, x, y, w, h);
    lv_obj_set_style_bg_color(o, ui_c(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_clip_corner(o, false, 0);
    return o;
}

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, ui_c(color), 0);
    lv_label_set_text(l, text ? text : "");
    return l;
}

lv_obj_t *ui_label_w(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                     int w, lv_text_align_t align, const char *text)
{
    lv_obj_t *l = ui_label(parent, font, color, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, w);
    lv_obj_set_height(l, lv_font_get_line_height(font));   // 固定一行,超出显示省略号
    lv_obj_set_style_text_align(l, align, 0);
    return l;
}

// ---------------------------------------------------------------------------
// 像素精灵 → RGB565A8 小图(每个图片对象独占 3*w*h 字节,随对象删除释放)。
// ---------------------------------------------------------------------------
typedef struct {
    lv_image_dsc_t dsc;
    uint8_t data[];
} pix_img_t;

static uint32_t sprite_color(const pixel_sprite_t *s, char key, bool *found)
{
    for (const pixel_color_t *c = s->colors; c && c->key; c++) {
        if (c->key == key) {
            *found = true;
            return c->rgb;
        }
    }
    *found = false;
    return 0;
}

static void pix_fill(pix_img_t *img, const pixel_sprite_t *s)
{
    const int n = s->width * s->height;
    uint16_t *rgb = (uint16_t *)img->data;
    uint8_t *alpha = img->data + n * 2;
    for (int y = 0; y < s->height; y++) {
        for (int x = 0; x < s->width; x++) {
            bool found = false;
            const char key = s->rows[y][x];
            const uint32_t c = (key == '.' || key == ' ') ? 0 : sprite_color(s, key, &found);
            const int i = y * s->width + x;
            rgb[i] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
            alpha[i] = found ? 0xFF : 0x00;
        }
    }
}

static pix_img_t *pix_alloc(const pixel_sprite_t *s)
{
    const int n = s->width * s->height;
    pix_img_t *img = malloc(sizeof(pix_img_t) + (size_t)n * 3);
    if (!img) return NULL;
    memset(&img->dsc, 0, sizeof(img->dsc));
    img->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    img->dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
    img->dsc.header.w = s->width;
    img->dsc.header.h = s->height;
    img->dsc.header.stride = s->width * 2;
    img->dsc.data_size = (uint32_t)n * 3;
    img->dsc.data = img->data;
    pix_fill(img, s);
    return img;
}

static void pix_delete_cb(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

lv_obj_t *ui_pix(lv_obj_t *parent, const pixel_sprite_t *sprite, int scale)
{
    lv_obj_t *o = lv_image_create(parent);
    pix_img_t *img = pix_alloc(sprite);
    if (!img) return o;
    lv_image_set_src(o, &img->dsc);
    lv_obj_add_event_cb(o, pix_delete_cb, LV_EVENT_DELETE, img);
    lv_obj_set_user_data(o, img);
    lv_image_set_antialias(o, false);
    // 默认枢轴在图片中心,图片在对象里居中:放大后正好铺满对象。
    lv_image_set_scale(o, (uint32_t)(256 * scale));
    lv_obj_set_size(o, sprite->width * scale, sprite->height * scale);
    return o;
}

void ui_pix_set(lv_obj_t *o, const pixel_sprite_t *sprite)
{
    pix_img_t *img = lv_obj_get_user_data(o);
    if (!img || img->dsc.header.w != sprite->width || img->dsc.header.h != sprite->height) return;
    pix_fill(img, sprite);
    lv_image_cache_drop(&img->dsc);
    lv_obj_invalidate(o);
}

lv_obj_t *ui_pill(lv_obj_t *parent, const char *text, uint32_t fg, uint32_t bg)
{
    lv_obj_t *l = ui_label(parent, &ui_font_16, fg, text);
    lv_obj_set_style_bg_color(l, ui_c(bg), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(l, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(l, 10, 0);
    lv_obj_set_style_pad_ver(l, 2, 0);
    return l;
}

lv_obj_t *ui_bar(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *track = ui_card(parent, x, y, w, h, C_SURFACE2, h / 2);
    lv_obj_t *fill = ui_card(track, 0, 0, 0, h, color, h / 2);
    lv_obj_set_user_data(track, fill);
    return track;
}

void ui_bar_set(lv_obj_t *bar, int value)
{
    lv_obj_t *fill = lv_obj_get_user_data(bar);
    if (value < 0) value = 0;
    if (value > 1000) value = 1000;
    lv_obj_update_layout(bar);
    const int w = lv_obj_get_width(bar);
    const int h = lv_obj_get_height(bar);
    int fw = w * value / 1000;
    if (fw > 0 && fw < h) fw = h;   // 保持圆头
    lv_obj_set_width(fill, fw);
}

// ---------------------------------------------------------------------------
// 列表
// ---------------------------------------------------------------------------
#define LIST_ROW_H 46
#define LIST_GAP   6

static void list_style_row(ui_list_t *list, int idx, bool focused)
{
    lv_obj_t *row = list->rows[idx];
    lv_obj_set_style_bg_color(row, ui_c(focused ? ui_mix(list->accent, C_SURFACE, 46)
                                                : C_SURFACE), 0);
    lv_obj_set_style_border_color(row, ui_c(focused ? list->accent : C_SURFACE), 0);
    lv_obj_set_style_text_color(list->titles[idx], ui_c(focused ? C_TEXT : C_TEXT2), 0);
}

void ui_list_create(ui_list_t *list, lv_obj_t *parent, int x, int y, int w, int h,
                    uint32_t accent)
{
    memset(list, 0, sizeof(*list));
    list->accent = accent;
    list->row_h = LIST_ROW_H;
    list->w = w;
    list->cont = ui_box(parent, x, y, w, h);
    lv_obj_add_flag(list->cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(list->cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(list->cont, LV_DIR_VER);
    lv_obj_set_style_pad_ver(list->cont, 2, 0);
}

int ui_list_add(ui_list_t *list, const pixel_sprite_t *icon, const char *title,
                const char *value, bool chevron)
{
    if (list->count >= UI_LIST_MAX) return -1;
    const int idx = list->count++;
    const int w = list->w;
    lv_obj_t *row = ui_card(list->cont, 0, idx * (list->row_h + LIST_GAP),
                            w, list->row_h, C_SURFACE, 12);
    lv_obj_set_style_border_width(row, 2, 0);
    list->rows[idx] = row;

    int tx = 14;
    if (icon) {
        lv_obj_t *pix = ui_pix(row, icon, 2);
        lv_obj_align(pix, LV_ALIGN_LEFT_MID, 10, 0);
        list->icons[idx] = pix;
        tx = 50;
    }
    int right = chevron ? 26 : 12;
    lv_obj_t *t = ui_label_w(row, &ui_font_16, C_TEXT2, value ? (w - tx - right) / 2 + 30
                                                             : w - tx - right,
                             LV_TEXT_ALIGN_LEFT, title);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, tx, 0);
    list->titles[idx] = t;

    if (chevron) {
        lv_obj_t *c = ui_label(row, &lv_font_montserrat_14, C_TEXT3, LV_SYMBOL_RIGHT);
        lv_obj_align(c, LV_ALIGN_RIGHT_MID, -10, 0);
    }
    if (value) {
        lv_obj_t *v = ui_label_w(row, &ui_font_16, C_TEXT3, (w - tx - right) / 2 - 30 + 20,
                                 LV_TEXT_ALIGN_RIGHT, value);
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, -right, 0);
        list->values[idx] = v;
    }
    list_style_row(list, idx, idx == list->focus);
    return idx;
}

void ui_list_set_value(ui_list_t *list, int idx, const char *value)
{
    if (idx < 0 || idx >= list->count || !list->values[idx]) return;
    lv_label_set_text(list->values[idx], value);
}

void ui_list_set_value_color(ui_list_t *list, int idx, uint32_t color)
{
    if (idx < 0 || idx >= list->count || !list->values[idx]) return;
    lv_obj_set_style_text_color(list->values[idx], ui_c(color), 0);
}

void ui_list_set_title(ui_list_t *list, int idx, const char *title)
{
    if (idx < 0 || idx >= list->count) return;
    lv_label_set_text(list->titles[idx], title);
}

void ui_list_set_icon(ui_list_t *list, int idx, const pixel_sprite_t *icon)
{
    if (idx < 0 || idx >= list->count || !list->icons[idx]) return;
    ui_pix_set(list->icons[idx], icon);
}

void ui_list_focus(ui_list_t *list, int idx, bool anim)
{
    if (list->count == 0) return;
    if (idx < 0) idx = 0;
    if (idx >= list->count) idx = list->count - 1;
    if (list->focus < list->count) list_style_row(list, list->focus, false);
    list->focus = idx;
    list_style_row(list, idx, true);
    lv_obj_scroll_to_view(list->rows[idx], anim ? LV_ANIM_ON : LV_ANIM_OFF);
}

int ui_list_move(ui_list_t *list, int delta)
{
    if (list->count == 0) return 0;
    const int next = (list->focus + delta + list->count) % list->count;
    ui_list_focus(list, next, true);
    return next;
}

void ui_list_clear(ui_list_t *list)
{
    lv_obj_clean(list->cont);
    memset(list->rows, 0, sizeof(list->rows));
    memset(list->values, 0, sizeof(list->values));
    memset(list->titles, 0, sizeof(list->titles));
    memset(list->icons, 0, sizeof(list->icons));
    list->count = 0;
    list->focus = 0;
}

lv_obj_t *ui_empty(lv_obj_t *parent, const pixel_sprite_t *icon, const char *title,
                   const char *detail)
{
    lv_obj_update_layout(parent);
    lv_obj_t *box = ui_box(parent, 0, 0, lv_obj_get_width(parent), lv_obj_get_height(parent));
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 8, 0);
    if (icon) ui_pix(box, icon, 4);
    if (title) ui_label(box, &ui_font_20b, C_TEXT, title);
    if (detail) {
        lv_obj_t *d = ui_label(box, &ui_font_16, C_TEXT2, detail);
        lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(d, lv_obj_get_width(parent) - 24);
        lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
    }
    return box;
}
