// main/app_remote.c —— 蓝牙遥控:设备伪装成蓝牙键盘(无需装 App,在手机/电脑的蓝牙里配对即可)。
//
// 先选模式,再进入模式页;模式页里 长▲ 回到模式列表。
//   编程助手(Vibe Coding):给 Claude Code / Cursor 等 AI 编程工具当"一手遥控"
//       ▲ ↑  ▼ ↓(选选项/翻历史)· 确定 回车(同意/发送)· 长按确定 Esc(打断)
//       长▼ Shift+Tab(切换自动接受/计划模式)· 双击确定 打开快捷指令,一键键入常用指令
//   翻页笔:▲ 上一页 · ▼ 下一页 · 确定 放映(F5)· 长▼ 黑屏(B)
//   媒体  :▲ 音量+ · ▼ 音量- · 确定 播放/暂停 · 长▼ 下一首
//   拍照  :任意键 快门(音量+,手机相机通用)
// 息屏后按键照常发送(不会先点亮屏幕),长▲ 点亮屏幕。
#include "apps.h"
#include "icons.h"
#include "settings.h"
#include "svc.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>

// HID 键码(Keyboard/Keypad page 0x07)、修饰键与媒体用法(Consumer page 0x0C)
#define KEY_B      0x05
#define KEY_ENTER  0x28
#define KEY_ESC    0x29
#define KEY_TAB    0x2B
#define KEY_F5     0x3E
#define KEY_RIGHT  0x4F
#define KEY_LEFT   0x50
#define KEY_DOWN   0x51
#define KEY_UP     0x52
#define MOD_SHIFT  0x02
#define CC_VOL_UP   0x00E9
#define CC_VOL_DOWN 0x00EA
#define CC_PLAY     0x00CD
#define CC_NEXT     0x00B5

typedef enum { MODE_VIBE = 0, MODE_SLIDES, MODE_MEDIA, MODE_CAMERA, MODE_COUNT } mode_t_;

typedef struct {
    const char *name;
    const char *desc;
    const pixel_sprite_t *icon;
    const char *up, *ok, *down;
    const char *extra;
    const char *up2, *ok2, *down2;   // 键卡片第二行:长按 / 双击功能(为空则不显示)
} mode_info_t;

static const mode_info_t k_modes[MODE_COUNT] = {
    {"编程助手", "AI 编程一手遥控", &ICON_AI, "↑ 上一项", "回车 · 同意", "↓ 下一项", "",
     "双击 切换计划模式", "长按打断  双击指令", "长按语音  双击换系统"},
    {"翻页笔", "演示文稿翻页", &IC_SLIDES, "上一页", "开始放映", "下一页", "",
     NULL, NULL, "长按 黑屏"},
    {"媒体", "音乐与视频", &IC_MUSIC, "音量 +", "播放 / 暂停", "音量 -", "",
     NULL, NULL, "长按 下一首"},
    {"拍照", "手机相机快门", &IC_CAMERA, "快门", "快门", "快门", "相机里任意键都是快门",
     NULL, NULL, NULL},
};

// 快捷指令:列表显示中文说明,实际键入英文指令并回车(HID 键盘只能可靠地发送 ASCII)。
typedef struct {
    const char *label;
    const char *text;
} phrase_t;

static const phrase_t k_phrases[] = {
    {"继续", "continue"},
    {"同意", "yes"},
    {"跑测试并修复失败", "run the tests and fix any failures"},
    {"没问题,提交", "looks good, commit it"},
    {"解释刚才的改动", "explain what you just changed"},
    {"撤销上一步", "undo your last change"},
    {"清空上下文", "/clear"},
    {"压缩上下文", "/compact"},
};
#define PHRASE_COUNT ((int)(sizeof(k_phrases) / sizeof(k_phrases[0])))

typedef enum { PAGE_MODES = 0, PAGE_MODE, PAGE_PHRASES } page_t;

static int s_mode;
static page_t s_kind;
static lv_obj_t *s_root;
static lv_obj_t *s_page;
static ui_list_t s_list;
static lv_obj_t *s_status;
static lv_obj_t *s_status_dot;
static lv_obj_t *s_keys[3];      // 0=▲ 1=确定 2=▼
static lv_obj_t *s_key_text[3];
static lv_obj_t *s_phrase_preview;
static int64_t s_flash_until[3];
static ble_state_t s_last = (ble_state_t)-1;

static void build(page_t page);

// ---------------------------------------------------------------------------
static lv_obj_t *status_card(lv_obj_t *p)
{
    lv_obj_t *head = ui_card(p, UI_PAD, 4, UI_W - 2 * UI_PAD, 50, C_SURFACE, 14);
    s_status_dot = ui_card(head, 14, 21, 8, 8, C_WARN, 4);
    s_status = ui_label(head, &ui_font_14, C_TEXT, "");
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status, UI_W - 2 * UI_PAD - 40);
    lv_obj_align(s_status, LV_ALIGN_LEFT_MID, 30, 0);
    s_last = (ble_state_t)-1;
    return head;
}

static void build_modes(lv_obj_t *p)
{
    status_card(p);
    ui_list_create(&s_list, p, UI_PAD, 62, UI_W - 2 * UI_PAD, UI_CONTENT_H - 66, C_REMOTE);
    for (int i = 0; i < MODE_COUNT; i++) {
        ui_list_add(&s_list, k_modes[i].icon, k_modes[i].name, NULL, true);
    }
    ui_list_focus(&s_list, s_mode, false);
    shell_hints("▲▼:选择|OK:进入|长▲:返回");
}

// 键卡片:左侧键名,右侧两行 —— 单击功能(大)+ 长按/双击功能(小)。
static lv_obj_t *key_card(lv_obj_t *root, int y, int h, const char *glyph, int idx,
                          const char *main_text, const char *sub_text)
{
    lv_obj_t *c = ui_card(root, UI_PAD, y, UI_W - 2 * UI_PAD, h, C_SURFACE, 14);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, ui_c(C_LINE), 0);
    lv_obj_t *g = ui_label(c, &ui_font_16, C_REMOTE, glyph);
    lv_obj_align(g, LV_ALIGN_LEFT_MID, 12, 0);
    s_key_text[idx] = ui_label(c, idx == 1 ? &ui_font_20b : &ui_font_16, C_TEXT, main_text);
    if (sub_text && sub_text[0]) {
        lv_obj_align(s_key_text[idx], LV_ALIGN_TOP_LEFT, 56, idx == 1 ? 4 : 3);
        lv_obj_t *sub = ui_label_w(c, &ui_font_14, C_TEXT3, UI_W - 2 * UI_PAD - 66, LV_TEXT_ALIGN_LEFT,
                                   sub_text);
        lv_obj_align(sub, LV_ALIGN_BOTTOM_LEFT, 56, -4);
    } else {
        lv_obj_align(s_key_text[idx], LV_ALIGN_LEFT_MID, 56, 0);
    }
    return c;
}

static bool s_windows;   // 编程助手的"语音输入"发往 Windows(Win+H)还是 Mac(按两下右侧 ⌘)

static void build_mode(lv_obj_t *p)
{
    const mode_info_t *m = &k_modes[s_mode];
    status_card(p);
    lv_obj_t *title = ui_label_w(p, &ui_font_16, C_REMOTE, UI_W, LV_TEXT_ALIGN_CENTER, m->name);
    lv_obj_set_pos(title, 0, 62);
    s_keys[0] = key_card(p, 86, 44, "▲", 0, m->up, m->up2);
    s_keys[1] = key_card(p, 136, 52, "确定", 1, m->ok, m->ok2);
    s_keys[2] = key_card(p, 194, 44, "▼", 2, m->down, m->down2);
    lv_obj_t *extra = ui_label_w(p, &ui_font_14, C_TEXT3, UI_W - 2 * UI_PAD, LV_TEXT_ALIGN_CENTER,
                                 m->extra);
    if (s_mode == MODE_VIBE) {
        lv_label_set_text(extra, s_windows ? "语音输入:Windows(Win+H)"
                                           : "Mac 听写快捷键请设为 按两下右⌘");
    }
    lv_obj_set_pos(extra, UI_PAD, 244);
    memset(s_flash_until, 0, sizeof(s_flash_until));
    shell_hints("长▲:换模式");
}

static void build_phrases(lv_obj_t *p)
{
    lv_obj_t *t = ui_label(p, &ui_font_20b, C_TEXT, "快捷指令");
    lv_obj_set_pos(t, UI_PAD + 4, 4);
    ui_list_create(&s_list, p, UI_PAD, 36, UI_W - 2 * UI_PAD, 180, C_REMOTE);
    for (int i = 0; i < PHRASE_COUNT; i++) ui_list_add(&s_list, NULL, k_phrases[i].label, NULL, false);
    ui_list_focus(&s_list, 0, false);
    s_phrase_preview = ui_label_w(p, &ui_font_14, C_TEXT3, UI_W - 2 * UI_PAD, LV_TEXT_ALIGN_CENTER, "");
    lv_obj_set_pos(s_phrase_preview, UI_PAD, 226);
    lv_label_set_text_fmt(s_phrase_preview, "键入:%s", k_phrases[0].text);
    shell_hints("▲▼:选择|OK:发送|长▲:返回");
}

static void build(page_t page)
{
    if (s_page) lv_obj_delete(s_page);
    s_page = ui_box(s_root, 0, 0, UI_W, UI_CONTENT_H);
    s_kind = page;
    s_status = NULL;
    switch (page) {
    case PAGE_MODE: build_mode(s_page); break;
    case PAGE_PHRASES: build_phrases(s_page); break;
    default: build_modes(s_page); break;
    }
}

static void create(lv_obj_t *root)
{
    s_root = root;
    s_windows = sys_kv_get_i32("r_win", 0) != 0;
    s_page = NULL;
    build(PAGE_MODES);
    if (!ble_hid_start()) shell_toast("蓝牙启动失败");
}

static void destroy(void)
{
    ble_hid_stop();
    s_root = NULL;
    s_page = NULL;
}

static void flash(int idx)
{
    if (s_kind != PAGE_MODE) return;
    s_flash_until[idx] = sys_now_ms() + 160;
    lv_obj_set_style_bg_color(s_keys[idx], ui_c(C_REMOTE), 0);
    lv_obj_set_style_text_color(s_key_text[idx], ui_c(C_BG), 0);
}

static bool connected(void)
{
    if (ble_hid_state() == BLE_CONNECTED) return true;
    shell_toast("还没连接,先在电脑或手机上配对");
    return false;
}

static bool mode_key(sys_key_t k, sys_key_ev_t ev)
{
    const int idx = k == SYS_KEY_UP ? 0 : (k == SYS_KEY_OK ? 1 : 2);
    const bool lng = ev == SYS_EV_LONG;

    if (s_mode == MODE_VIBE) {
        // ▲ 单击 ↑ · 双击 Shift+Tab   确定 单击 回车 · 长按 Esc · 双击 快捷指令
        // ▼ 单击 ↓ · 长按 语音输入 · 双击 切换 Mac/Windows        (长按 ▲ = 返回)
        const bool dbl = ev == SYS_EV_DOUBLE;
        if (k == SYS_KEY_OK && dbl) {
            build(PAGE_PHRASES);
            return true;
        }
        if (k == SYS_KEY_DOWN && dbl) {
            s_windows = !s_windows;
            sys_kv_set_i32("r_win", s_windows);
            shell_toast(s_windows ? "语音输入:Windows" : "语音输入:Mac");
            if (shell_screen_on()) build(PAGE_MODE);
            return true;
        }
        if (ev != SYS_EV_CLICK && !dbl && !(lng && k != SYS_KEY_UP)) return false;
        if (!connected()) return true;
        if (k == SYS_KEY_UP) {
            if (dbl) ble_hid_key(KEY_TAB, MOD_SHIFT);
            else ble_hid_key(KEY_UP, 0);
        } else if (k == SYS_KEY_OK) {
            ble_hid_key(lng ? KEY_ESC : KEY_ENTER, 0);
        } else if (lng) {
            if (s_windows) {
                ble_hid_key(0x0B, 0x08);          // Win + H
            } else {
                ble_hid_key(0x00, 0x80);          // 右侧 ⌘ 按两下(Mac 听写快捷键)
                ble_hid_key(0x00, 0x80);
            }
            shell_toast("语音输入");
        } else {
            ble_hid_key(KEY_DOWN, 0);
        }
        if (dbl) shell_toast("已切换模式");
        else if (lng && k == SYS_KEY_OK) shell_toast("已打断");
        flash(idx);
        return true;
    }

    if (ev != SYS_EV_CLICK && !(k == SYS_KEY_DOWN && lng)) return false;
    if (!connected()) return true;
    switch (s_mode) {
    case MODE_SLIDES:
        if (lng) ble_hid_key(KEY_B, 0);
        else ble_hid_key(idx == 0 ? KEY_LEFT : idx == 1 ? KEY_F5 : KEY_RIGHT, 0);
        break;
    case MODE_MEDIA:
        if (lng) ble_hid_consumer(CC_NEXT);
        else ble_hid_consumer(idx == 0 ? CC_VOL_UP : idx == 1 ? CC_PLAY : CC_VOL_DOWN);
        break;
    default:
        ble_hid_consumer(CC_VOL_UP);
        break;
    }
    flash(idx);
    return true;
}

static bool key(sys_key_t k, sys_key_ev_t ev)
{
    const bool back = k == SYS_KEY_UP && ev == SYS_EV_LONG;
    switch (s_kind) {
    case PAGE_MODES:
        if (ev != SYS_EV_CLICK) return false;
        if (k == SYS_KEY_OK) {
            s_mode = s_list.focus;
            build(PAGE_MODE);
        } else {
            ui_list_move(&s_list, k == SYS_KEY_DOWN ? 1 : -1);
        }
        return true;
    case PAGE_PHRASES:
        if (back) {
            build(PAGE_MODE);
            return true;
        }
        if (ev != SYS_EV_CLICK) return true;
        if (k == SYS_KEY_OK) {
            if (connected()) {
                ble_hid_type(k_phrases[s_list.focus].text);
                ble_hid_key(KEY_ENTER, 0);
                shell_toast("已发送");
            }
            build(PAGE_MODE);
        } else {
            ui_list_move(&s_list, k == SYS_KEY_DOWN ? 1 : -1);
            lv_label_set_text_fmt(s_phrase_preview, "键入:%s", k_phrases[s_list.focus].text);
        }
        return true;
    default:
        if (back) {
            if (!shell_screen_on()) return false;   // 息屏时让 shell 点亮屏幕
            build(PAGE_MODES);
            return true;
        }
        return mode_key(k, ev);
    }
}

static void tick(int64_t now)
{
    if (s_kind == PAGE_MODE) {
        for (int i = 0; i < 3; i++) {
            if (s_flash_until[i] && now > s_flash_until[i]) {
                s_flash_until[i] = 0;
                lv_obj_set_style_bg_color(s_keys[i], ui_c(C_SURFACE), 0);
                lv_obj_set_style_text_color(s_key_text[i], ui_c(C_TEXT), 0);
            }
        }
    }
    if (!s_status) return;
    const ble_state_t st = ble_hid_state();
    if (st == s_last) return;
    if (st == BLE_CONNECTED && s_last != (ble_state_t)-1) audio_beep(BEEP_OK);
    s_last = st;
    char buf[80];
    uint32_t dot = C_WARN;
    switch (st) {
    case BLE_CONNECTED:
        snprintf(buf, sizeof(buf), "已连接 %s", ble_hid_peer());
        dot = C_OK;
        break;
    case BLE_ADVERTISING:
        snprintf(buf, sizeof(buf), "等待配对:在电脑/手机蓝牙里选 %s", g_settings.name);
        break;
    case BLE_FAILED:
        snprintf(buf, sizeof(buf), "蓝牙出错,返回后重试");
        dot = C_ERR;
        break;
    default:
        snprintf(buf, sizeof(buf), "正在启动蓝牙…");
        break;
    }
    lv_label_set_text(s_status, buf);
    lv_obj_set_style_bg_color(s_status_dot, ui_c(dot), 0);
}

const app_t APP_REMOTE = {
    .name = "蓝牙遥控",
    .desc = "编程助手 · 翻页 · 媒体",
    .icon = &ICON_REMOTE,
    .color = C_REMOTE,
    .create = create,
    .destroy = destroy,
    .key = key,
    .tick = tick,
    .flags = APP_FLAG_KEYS_WHEN_OFF,
};
