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
     NULL, "长按打断  双击指令", NULL},
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
    {"切换计划/自动模式", NULL},    // 特殊项:发送 Shift+Tab
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

typedef enum { PAGE_MODES = 0, PAGE_MODE, PAGE_PHRASES, PAGE_PICK_DOWN, PAGE_PICK_VOICE } page_t;

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

// 语音键:▼ 可设为"语音键"(按住 ▼ = 电脑上按住所选的键,松开即松开)或"向下 ↓"。
typedef struct {
    const char *name;
    uint8_t modifier;   // 0 = Apple Fn
} voice_key_t;
static const voice_key_t k_voice_keys[] = {
    {"Fn", 0x00},
    {"左 Control", 0x01},
    {"右 Command", 0x80},
};
#define VOICE_KEY_COUNT ((int)(sizeof(k_voice_keys) / sizeof(k_voice_keys[0])))
static bool s_down_voice = true;
static int s_voice_key;
static bool s_holding;
enum { ROW_DOWN_KEY = MODE_COUNT, ROW_VOICE_KEY };

static void voice_release(void)
{
    if (s_holding) {
        ble_hid_hold(k_voice_keys[s_voice_key].modifier, false);
        s_holding = false;
    }
}


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
    ui_list_add(&s_list, NULL, "▼ 键用途", s_down_voice ? "语音键" : "向下 ↓", false);
    ui_list_add(&s_list, NULL, "语音键对应", k_voice_keys[s_voice_key].name, false);
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

static void build_mode(lv_obj_t *p)
{
    const mode_info_t *m = &k_modes[s_mode];
    status_card(p);
    lv_obj_t *title = ui_label_w(p, &ui_font_16, C_REMOTE, UI_W, LV_TEXT_ALIGN_CENTER, m->name);
    lv_obj_set_pos(title, 0, 62);
    const bool vibe = s_mode == MODE_VIBE;
    char down_main[32];
    snprintf(down_main, sizeof(down_main), "按住 = 按住 %s", k_voice_keys[s_voice_key].name);
    s_keys[0] = key_card(p, 86, 44, "▲", 0, m->up,
                         vibe ? (s_down_voice ? "双击 ↓ 下一项" : "双击 切换计划模式") : m->up2);
    s_keys[1] = key_card(p, 136, 52, "确定", 1, m->ok, m->ok2);
    s_keys[2] = key_card(p, 194, 44, "▼", 2, vibe && s_down_voice ? down_main : m->down,
                         vibe ? (s_down_voice ? "语音输入 · 松开即松开" : NULL) : m->down2);
    lv_obj_t *extra = ui_label_w(p, &ui_font_14, C_TEXT3, UI_W - 2 * UI_PAD, LV_TEXT_ALIGN_CENTER,
                                 m->extra);
    if (vibe) {
        lv_label_set_text(extra, s_down_voice && s_voice_key == 0 && !ble_hid_has_fn()
                                     ? "这台电脑不认 Fn,请在列表里改用 Command"
                                     : "▼ 的用途与语音键可在上一页切换");
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
    lv_label_set_text_fmt(s_phrase_preview, "键入:%s", k_phrases[0].text ? k_phrases[0].text : "Shift+Tab");
    shell_hints("▲▼:选择|OK:发送|长▲:返回");
}

// 选项页:选好后固定保存,以后一直用这个(▲▼ 选择,确定保存,长按 ▲ 不保存返回)。
static void build_pick(lv_obj_t *p, page_t page)
{
    const bool voice = page == PAGE_PICK_VOICE;
    lv_obj_t *t = ui_label(p, &ui_font_20b, C_TEXT, voice ? "语音键对应" : "▼ 键用途");
    lv_obj_set_pos(t, UI_PAD + 4, 4);
    ui_list_create(&s_list, p, UI_PAD, 36, UI_W - 2 * UI_PAD, 170, C_REMOTE);
    const int current = voice ? s_voice_key : (s_down_voice ? 0 : 1);
    const int count = voice ? VOICE_KEY_COUNT : 2;
    for (int i = 0; i < count; i++) {
        const char *name = voice ? k_voice_keys[i].name : (i == 0 ? "语音键(按住说话)" : "向下 ↓");
        ui_list_add(&s_list, i == current ? &IC_CHECK : NULL, name, NULL, false);
    }
    ui_list_focus(&s_list, current, false);
    lv_obj_t *note = ui_label(p, &ui_font_14, C_TEXT3,
                              voice ? "听写工具里的按住说话键设成同一个。\nFn 不生效就改选 Command"
                                    : "语音键:按住 ▼ = 电脑上按住所选的键");
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, UI_W - 2 * UI_PAD);
    lv_obj_set_pos(note, UI_PAD, 206);
    shell_hints("▲▼:选择|OK:保存|长▲:返回");
}

static void build(page_t page)
{
    voice_release();
    if (s_page) lv_obj_delete(s_page);
    s_page = ui_box(s_root, 0, 0, UI_W, UI_CONTENT_H);
    s_kind = page;
    s_status = NULL;
    switch (page) {
    case PAGE_MODE: build_mode(s_page); break;
    case PAGE_PHRASES: build_phrases(s_page); break;
    case PAGE_PICK_DOWN:
    case PAGE_PICK_VOICE: build_pick(s_page, page); break;
    default: build_modes(s_page); break;
    }
}

static void create(lv_obj_t *root)
{
    s_root = root;
    s_down_voice = sys_kv_get_i32("r_dv", 1) != 0;
    s_voice_key = sys_kv_get_i32("r_vk2", 0);
    if (s_voice_key < 0 || s_voice_key >= VOICE_KEY_COUNT) s_voice_key = 0;
    s_holding = false;
    s_page = NULL;
    build(PAGE_MODES);
    if (!ble_hid_start()) shell_toast("蓝牙启动失败");
}

static void destroy(void)
{
    voice_release();
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
        // ▲ 单击 ↑ · 双击 ↓(▼ 是语音键时)或 Shift+Tab · 长按 返回
        // 确定 单击 回车 · 长按 Esc · 双击 快捷指令
        // ▼ 语音键:按下即按下所选键,松开即松开(短按=短按,长按=长按);或普通 ↓
        const bool dbl = ev == SYS_EV_DOUBLE;
        if (k == SYS_KEY_DOWN && s_down_voice) {
            if (ev == SYS_EV_PRESS && !s_holding) {
                if (!connected()) return true;
                ble_hid_hold(k_voice_keys[s_voice_key].modifier, true);
                s_holding = true;
                flash(2);
                s_flash_until[2] = INT64_MAX;   // 按住期间一直高亮
            } else if (ev == SYS_EV_RELEASE && s_holding) {
                voice_release();
                s_flash_until[2] = sys_now_ms();
            }
            return true;   // ▼ 的单击/长按/双击都由按下-松开表达,不再另作处理
        }
        if (k == SYS_KEY_OK && dbl) {
            build(PAGE_PHRASES);
            return true;
        }
        if (ev != SYS_EV_CLICK && !dbl && !(lng && k == SYS_KEY_OK)) return false;
        if (!connected()) return true;
        if (k == SYS_KEY_UP) {
            if (dbl) ble_hid_key(s_down_voice ? KEY_DOWN : KEY_TAB, s_down_voice ? 0 : MOD_SHIFT);
            else ble_hid_key(KEY_UP, 0);
        } else if (k == SYS_KEY_OK) {
            ble_hid_key(lng ? KEY_ESC : KEY_ENTER, 0);
            if (lng) shell_toast("已打断");
        } else {
            ble_hid_key(KEY_DOWN, 0);
        }
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
        if (k == SYS_KEY_OK && s_list.focus == ROW_DOWN_KEY) {
            build(PAGE_PICK_DOWN);
        } else if (k == SYS_KEY_OK && s_list.focus == ROW_VOICE_KEY) {
            build(PAGE_PICK_VOICE);
        } else if (k == SYS_KEY_OK) {
            s_mode = s_list.focus;
            build(PAGE_MODE);
        } else {
            ui_list_move(&s_list, k == SYS_KEY_DOWN ? 1 : -1);
        }
        return true;
    case PAGE_PICK_DOWN:
    case PAGE_PICK_VOICE: {
        const bool voice = s_kind == PAGE_PICK_VOICE;
        const int row = voice ? ROW_VOICE_KEY : ROW_DOWN_KEY;
        if (back) {
            build(PAGE_MODES);
            ui_list_focus(&s_list, row, false);
            return true;
        }
        if (ev != SYS_EV_CLICK) return true;
        if (k != SYS_KEY_OK) {
            ui_list_move(&s_list, k == SYS_KEY_DOWN ? 1 : -1);
            return true;
        }
        if (voice) {
            s_voice_key = s_list.focus;
            sys_kv_set_i32("r_vk2", s_voice_key);
        } else {
            s_down_voice = s_list.focus == 0;
            sys_kv_set_i32("r_dv", s_down_voice);
        }
        shell_toast("已保存");
        build(PAGE_MODES);
        ui_list_focus(&s_list, row, false);
        return true;
    }
    case PAGE_PHRASES:
        if (back) {
            build(PAGE_MODE);
            return true;
        }
        if (ev != SYS_EV_CLICK) return true;
        if (k == SYS_KEY_OK) {
            if (connected()) {
                if (k_phrases[s_list.focus].text) {
                    ble_hid_type(k_phrases[s_list.focus].text);
                    ble_hid_key(KEY_ENTER, 0);
                } else {
                    ble_hid_key(KEY_TAB, MOD_SHIFT);
                }
                shell_toast("已发送");
            }
            build(PAGE_MODE);
        } else {
            ui_list_move(&s_list, k == SYS_KEY_DOWN ? 1 : -1);
            lv_label_set_text_fmt(s_phrase_preview, "键入:%s",
                                  k_phrases[s_list.focus].text ? k_phrases[s_list.focus].text : "Shift+Tab");
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
