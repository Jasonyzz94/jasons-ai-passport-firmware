// main/app_ai.c —— AI 助手:按住 OK 说话,松开发送;Mac mini 负责识别 → 大模型 → 合成语音。
//
// 设备只做录音(IMA ADPCM 流式上传)、显示文字和播放回答语音;API key 全在服务器上。
// 按键:按住 OK 说话(短按 = 重播上一条)· ▲▼ 翻看对话 · 长▼ 清空 · 长▲ 返回
#include "apps.h"
#include "icons.h"
#include "voice_cfg.h"
#include "settings.h"
#include "svc.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>

#define MAX_TURNS 3
#define Q_LEN 160
#define A_LEN 640
#define SHORT_PRESS_MS 350

typedef struct {
    char q[Q_LEN];
    char a[A_LEN];
} turn_t;

static turn_t s_turns[MAX_TURNS];   // 跨进出保留,最新在末尾
static int s_turn_count;

static lv_obj_t *s_root;
static lv_obj_t *s_chat;
static lv_obj_t *s_dock;
static lv_obj_t *s_dock_label;
static lv_obj_t *s_meter[7];
static lv_obj_t *s_srv;
static ai_state_t s_last_state = (ai_state_t)-1;
static int64_t s_press_ms = -1;
static bool s_talking;

static lv_obj_t *bubble(lv_obj_t *parent, const char *text, bool mine)
{
    lv_obj_t *wrap = ui_box(parent, 0, 0, UI_W - 2 * UI_PAD, LV_SIZE_CONTENT);
    lv_obj_t *b = ui_label(wrap, &ui_font_16, mine ? C_BG : C_TEXT, text);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_width(b, UI_W - 2 * UI_PAD - 28, 0);
    lv_obj_set_width(b, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(b, ui_c(mine ? C_AI : C_SURFACE2), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_pad_hor(b, 12, 0);
    lv_obj_set_style_pad_ver(b, 8, 0);
    lv_obj_set_style_text_line_space(b, 3, 0);
    lv_obj_align(b, mine ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, 0, 0);
    return wrap;
}

static void render_chat(void)
{
    lv_obj_clean(s_chat);
    if (!s_turn_count) {
        lv_obj_t *box = ui_box(s_chat, 0, 0, UI_W - 2 * UI_PAD, 150);
        lv_obj_t *icon = ui_pix(box, &ICON_AI, 4);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 6);
        lv_obj_t *t = ui_label(box, &ui_font_16, C_TEXT2, "有什么想问的?");
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 80);
        lv_obj_t *d = ui_label(box, &ui_font_14, C_TEXT3, "天气、翻译、百科、算数都可以");
        lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 104);
        return;
    }
    lv_obj_t *last = NULL;
    for (int i = 0; i < s_turn_count; i++) {
        bubble(s_chat, s_turns[i].q, true);
        last = bubble(s_chat, s_turns[i].a[0] ? s_turns[i].a : "…", false);
    }
    lv_obj_update_layout(s_chat);
    // 新回答从开头读起:滚到最后一条回答的顶部。
    if (last) lv_obj_scroll_to_y(s_chat, lv_obj_get_y(last) - 8, LV_ANIM_OFF);
}

static void push_turn(const char *q, const char *a)
{
    if (s_turn_count == MAX_TURNS) {
        memmove(&s_turns[0], &s_turns[1], sizeof(turn_t) * (MAX_TURNS - 1));
        s_turn_count--;
    }
    snprintf(s_turns[s_turn_count].q, Q_LEN, "%s", q);
    snprintf(s_turns[s_turn_count].a, A_LEN, "%s", a);
    s_turn_count++;
}

static void dock_set(const char *text, uint32_t bg, uint32_t fg, bool meter)
{
    lv_label_set_text(s_dock_label, text);
    lv_obj_set_style_bg_color(s_dock, ui_c(bg), 0);
    lv_obj_set_style_text_color(s_dock_label, ui_c(fg), 0);
    for (int i = 0; i < 7; i++) {
        if (meter) lv_obj_clear_flag(s_meter[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_meter[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_align(s_dock_label, meter ? LV_ALIGN_RIGHT_MID : LV_ALIGN_CENTER, meter ? -16 : 0, 0);
}

static void refresh_dock(ai_state_t st)
{
    const bool ready = voice_ready(g_settings.ai_mode, relay_state() == SRV_OK, g_settings.api_key[0] != '\0',
                                   net_state() == NET_CONNECTED);
    switch (st) {
    case AI_CONNECTING:
        dock_set("连接中,听到提示音再说", ui_mix(C_AI, C_SURFACE, 70), C_TEXT, false);
        shell_hints("▲:取消");
        break;
    case AI_LISTENING:
        dock_set("松开发送", C_AI, C_BG, true);
        shell_hints("松开OK:发送|▲:取消");
        break;
    case AI_THINKING:
        dock_set("思考中…", ui_mix(C_AI, C_SURFACE, 70), C_TEXT, false);
        shell_hints("OK:取消");
        break;
    case AI_SPEAKING:
        dock_set("正在回答  ■ 停止", ui_mix(C_AI, C_SURFACE, 70), C_TEXT, false);
        shell_hints("OK:停止|▲▼:翻看");
        break;
    case AI_ERROR:
        dock_set(ai_error(), ui_mix(C_ERR, C_SURFACE, 60), C_TEXT, false);
        shell_hints("按住OK:重试|长▲:返回");
        break;
    default:
        if (ready) {
            dock_set("按住确定键说话", C_SURFACE2, C_TEXT, false);
            shell_hints("按住OK:说话|▲▼:翻看|长▲:返回");
        } else {
            dock_set(net_state() != NET_CONNECTED ? "正在联网…"
                     : g_settings.ai_mode == VOICE_MODE_RELAY ? "正在找 Mac 中转…"
                     : "设置›无线网络›高级设置 填密钥",
                     C_SURFACE2, C_TEXT3, false);
            shell_hints("OK:重新查找|长▲:返回");
        }
        break;
    }
}

static void create(lv_obj_t *root)
{
    s_root = root;
    s_last_state = (ai_state_t)-1;
    s_talking = false;
    s_press_ms = -1;

    lv_obj_t *t = ui_label(root, &ui_font_20b, C_TEXT, "语音助手");
    lv_obj_set_pos(t, UI_PAD + 4, 4);
    s_srv = ui_label(root, &ui_font_14, C_TEXT3, "");
    lv_obj_align(s_srv, LV_ALIGN_TOP_RIGHT, -UI_PAD - 4, 10);

    s_chat = ui_box(root, UI_PAD, 36, UI_W - 2 * UI_PAD, 162);
    lv_obj_add_flag(s_chat, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_chat, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(s_chat, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_chat, 8, 0);

    s_dock = ui_card(root, UI_PAD, 206, UI_W - 2 * UI_PAD, 46, C_SURFACE2, 23);
    s_dock_label = ui_label(s_dock, &ui_font_16, C_TEXT, "");
    for (int i = 0; i < 7; i++) {
        s_meter[i] = ui_card(s_dock, 18 + i * 9, 19, 5, 8, C_BG, 3);
    }
    render_chat();
    refresh_dock(AI_IDLE);
    net_acquire();
}

static void destroy(void)
{
    ai_release();   // 取消进行中的对话并释放后台任务
    shell_keep_awake(false);
    net_release();
    s_root = NULL;
}

static bool key(sys_key_t k, sys_key_ev_t ev)
{
    const ai_state_t st = ai_state();
    if (k == SYS_KEY_OK) {
        if (ev == SYS_EV_PRESS && ai_cancelling()) {
            shell_toast("正在结束上一条,稍等");
            return true;
        }
        if (ev == SYS_EV_PRESS && (st == AI_IDLE || st == AI_ERROR)) {
            if (!voice_ready(g_settings.ai_mode, relay_state() == SRV_OK, g_settings.api_key[0] != '\0',
                             net_state() == NET_CONNECTED)) {
                relay_discover();
                return true;
            }
            s_press_ms = sys_now_ms();
            if (ai_talk_begin()) {   // 提示音在真正开始录音时响(见 tick)
                s_talking = true;
                shell_keep_awake(true);
            }
            return true;
        }
        if (ev == SYS_EV_RELEASE && s_talking) {
            s_talking = false;
            if (sys_now_ms() - s_press_ms < SHORT_PRESS_MS) {
                ai_cancel();                     // 太短:当作"重播上一条"
                if (s_turn_count) ai_replay();
            } else {
                ai_talk_end();
            }
            return true;
        }
        if (ev == SYS_EV_CLICK && (st == AI_THINKING || st == AI_SPEAKING)) {
            ai_cancel();
            return true;
        }
        return true;   // OK 的其余事件(LONG 等)在此应用里都属于"按住说话"
    }
    if (k == SYS_KEY_UP && s_talking && ev == SYS_EV_CLICK) {
        s_talking = false;
        ai_cancel();
        shell_toast("已取消");
        return true;
    }
    if (ev == SYS_EV_CLICK) {
        lv_obj_scroll_by_bounded(s_chat, 0, k == SYS_KEY_DOWN ? -60 : 60, LV_ANIM_ON);
        return true;
    }
    if (k == SYS_KEY_DOWN && ev == SYS_EV_LONG) {
        s_turn_count = 0;
        ai_clear_history();
        render_chat();
        shell_toast("对话已清空");
        return true;
    }
    return false;
}

static void tick(int64_t now)
{
    (void)now;
    const ai_state_t st = ai_state();
    static srv_state_t last_srv = (srv_state_t)-1;
    const srv_state_t srv = relay_state();
    if (st != s_last_state || srv != last_srv) {
        if (st == AI_LISTENING && s_last_state != AI_LISTENING) audio_beep(BEEP_START);
        if (s_last_state == AI_LISTENING && st != AI_LISTENING) shell_keep_awake(st != AI_IDLE);
        if (st == AI_IDLE || st == AI_ERROR) shell_keep_awake(false);
        refresh_dock(st);
        const bool net_ok = net_state() == NET_CONNECTED;
        const bool direct = voice_use_direct(g_settings.ai_mode, srv == SRV_OK, g_settings.api_key[0] != '\0');
        lv_label_set_text(s_srv, !net_ok ? "" : direct ? (g_settings.api_key[0] ? "云端直连" : "")
                                 : srv == SRV_OK ? "Mac 中转" : "");
        s_last_state = st;
        last_srv = srv;
    }
    if (st == AI_LISTENING) {
        const float lvl = audio_input_level();
        static const float shape[7] = {0.45f, 0.7f, 0.9f, 1.0f, 0.9f, 0.7f, 0.45f};
        const int jitter = (int)(now / 80) % 3;
        for (int i = 0; i < 7; i++) {
            int h = 4 + (int)(26 * lvl * shape[i]) + ((i + jitter) % 3 == 0 ? 2 : 0);
            if (h > 30) h = 30;
            lv_obj_set_height(s_meter[i], h);
            lv_obj_set_y(s_meter[i], 23 - h / 2);
        }
    }
    static char q[Q_LEN], a[A_LEN];
    if (ai_take_result(q, sizeof(q), a, sizeof(a))) {
        push_turn(q, a);
        render_chat();
    }
}

const app_t APP_AI = {
    .name = "语音助手",
    .desc = "按住说话,有问必答",
    .icon = &ICON_AI,
    .color = C_AI,
    .create = create,
    .destroy = destroy,
    .key = key,
    .tick = tick,
};
