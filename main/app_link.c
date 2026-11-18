// main/app_link.c —— 设备互联:两台 Passport 通过 ESP-NOW(Wi-Fi 射频点对点,不需要路由器)。
//
// 未配对:列出附近的设备,OK 发起配对,对方确认后记住彼此。
// 已配对:长按 OK 对讲(松开结束)· ▲▼ 选择动作 · OK 执行(快捷消息 / 一起专注 / 呼叫 / 后台在线)
// 离开应用时若"后台在线"开着,链路保持运行,来消息会在任何界面弹出提醒(更耗电)。
#include "apps.h"
#include "icons.h"
#include "settings.h"
#include "svc.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>

void pomo_remote_start(int minutes);
bool pomo_running_focus(int *minutes);

#define LOG_MAX 4

typedef struct {
    char text[64];
    bool mine;
} log_t;

static log_t s_log[LOG_MAX];
static int s_log_count;
static bool s_background;           // 离开应用后保持在线

static const char *const k_quick[] = {
    "收到", "好的", "马上来", "吃饭啦", "在忙,稍后回", "到家了", "加油!", "想你了",
};
#define QUICK_COUNT ((int)(sizeof(k_quick) / sizeof(k_quick[0])))

typedef enum { PAGE_MAIN = 0, PAGE_NEARBY, PAGE_QUICK, PAGE_MORE } page_t;
enum { ACT_MSG = 0, ACT_FOCUS, ACT_PING, ACT_MORE, ACT_COUNT };
static const char *const k_act_names[ACT_COUNT] = {"消息", "一起专注", "呼叫", "更多"};

static lv_obj_t *s_root;
static lv_obj_t *s_page;
static page_t s_kind;
static ui_list_t s_list;
static link_peer_t s_nearby[8];
static int s_nearby_count;
static int64_t s_nearby_at;

static lv_obj_t *s_partner_name;
static lv_obj_t *s_partner_state;
static lv_obj_t *s_partner_dot;
static lv_obj_t *s_log_box;
static lv_obj_t *s_acts[ACT_COUNT];
static lv_obj_t *s_talk;
static int s_act;
static bool s_talking;
static uint8_t s_pending_mac[6];

static void build(page_t page);

static void log_push(const char *text, bool mine)
{
    if (s_log_count == LOG_MAX) {
        memmove(&s_log[0], &s_log[1], sizeof(log_t) * (LOG_MAX - 1));
        s_log_count--;
    }
    snprintf(s_log[s_log_count].text, sizeof(s_log[0].text), "%s", text);
    s_log[s_log_count].mine = mine;
    s_log_count++;
}

// ---------------------------------------------------------------------------
// 后台:不在本应用时处理来件
// ---------------------------------------------------------------------------
static int s_focus_offer;

static void focus_answer(bool yes)
{
    if (yes) {
        pomo_remote_start(s_focus_offer);
        shell_open(&APP_POMODORO);
    }
}

static void handle_event_global(const link_event_t *ev)
{
    char buf[96];
    switch (ev->type) {
    case LINK_EV_TEXT:
        log_push(ev->text, false);
        snprintf(buf, sizeof(buf), "%s", ev->text);
        shell_alert(&IC_CHAT, C_LINK, ev->peer.name, buf, NULL);
        audio_beep(BEEP_MSG);
        break;
    case LINK_EV_PING:
        snprintf(buf, sizeof(buf), "%s 在呼叫你", ev->peer.name);
        shell_alert(&IC_BELL, C_LINK, "叮咚!", buf, NULL);
        audio_beep(BEEP_MSG);
        break;
    case LINK_EV_FOCUS:
        s_focus_offer = ev->value;
        snprintf(buf, sizeof(buf), "%s 开始了 %d 分钟专注,一起吗?", ev->peer.name, ev->value);
        audio_beep(BEEP_MSG);
        shell_confirm("一起专注", buf, focus_answer);
        break;
    case LINK_EV_VOICE_BEGIN:
        shell_toast("对方正在说话…");
        break;
    default:
        break;
    }
}

static void bg_tick(int64_t now)
{
    (void)now;
    if (shell_current() == &APP_LINK || !link_running()) return;
    link_event_t ev;
    while (link_poll(&ev)) handle_event_global(&ev);
}

void link_service_init(void)
{
    s_background = sys_kv_get_i32("l_bg", 0) != 0;
    shell_add_bg_tick(bg_tick);
    // 开机时若已配对且开启了后台在线,直接上线。
    link_peer_t p;
    if (s_background && link_partner(&p)) link_start();
}

// ---------------------------------------------------------------------------
// 主页(已配对)
// ---------------------------------------------------------------------------
static void render_log(void)
{
    lv_obj_clean(s_log_box);
    if (!s_log_count) {
        lv_obj_t *e = ui_label(s_log_box, &ui_font_14, C_TEXT3, "长按确定键对讲,或发一条快捷消息");
        lv_obj_align(e, LV_ALIGN_CENTER, 0, 0);
        return;
    }
    for (int i = 0; i < s_log_count; i++) {
        lv_obj_t *wrap = ui_box(s_log_box, 0, 0, UI_W - 2 * UI_PAD, LV_SIZE_CONTENT);
        lv_obj_t *b = ui_pill(wrap, s_log[i].text, s_log[i].mine ? C_BG : C_TEXT,
                              s_log[i].mine ? C_LINK : C_SURFACE2);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_set_style_pad_ver(b, 4, 0);
        lv_obj_align(b, s_log[i].mine ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, 0, 0);
    }
    lv_obj_update_layout(s_log_box);
    lv_obj_scroll_to_y(s_log_box, LV_COORD_MAX, LV_ANIM_OFF);
}

static void render_acts(void)
{
    for (int i = 0; i < ACT_COUNT; i++) {
        const bool on = i == s_act;
        lv_obj_set_style_bg_color(s_acts[i], ui_c(on ? C_LINK : C_SURFACE), 0);
        lv_obj_set_style_text_color(s_acts[i], ui_c(on ? C_BG : C_TEXT2), 0);
    }
}

static void render_partner(void)
{
    link_peer_t p;
    if (!link_partner(&p)) return;
    lv_label_set_text(s_partner_name, p.name);
    const bool online = link_partner_online();
    lv_label_set_text_fmt(s_partner_state, online ? "在线 · 信号 %d" : "离线 · 等待对方打开互联",
                          p.rssi);
    lv_obj_set_style_bg_color(s_partner_dot, ui_c(online ? C_OK : C_TEXT3), 0);
}

static void build_main(lv_obj_t *p)
{
    lv_obj_t *card = ui_card(p, UI_PAD, 4, UI_W - 2 * UI_PAD, 56, C_SURFACE, 16);
    lv_obj_t *icon = ui_pix(card, &ICON_LINK, 2);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 10, 0);
    s_partner_name = ui_label_w(card, &ui_font_16, C_TEXT, 140, LV_TEXT_ALIGN_LEFT, "");
    lv_obj_set_pos(s_partner_name, 52, 8);
    s_partner_dot = ui_card(card, 52, 36, 7, 7, C_TEXT3, 4);
    s_partner_state = ui_label_w(card, &ui_font_14, C_TEXT3, 140, LV_TEXT_ALIGN_LEFT, "");
    lv_obj_set_pos(s_partner_state, 64, 30);

    s_log_box = ui_box(p, UI_PAD, 66, UI_W - 2 * UI_PAD, 104);
    lv_obj_add_flag(s_log_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_log_box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(s_log_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_log_box, 6, 0);

    s_talk = ui_card(p, UI_PAD, 176, UI_W - 2 * UI_PAD, 36, C_SURFACE2, 18);
    lv_obj_t *tl = ui_label(s_talk, &ui_font_16, C_TEXT, "长按确定键对讲");
    lv_obj_center(tl);

    lv_obj_t *row = ui_box(p, 6, 220, UI_W - 12, 32);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < ACT_COUNT; i++) {
        s_acts[i] = ui_pill(row, k_act_names[i], C_TEXT2, C_SURFACE);
        lv_obj_set_style_pad_hor(s_acts[i], 10, 0);
        lv_obj_set_style_pad_ver(s_acts[i], 4, 0);
    }
    render_partner();
    render_log();
    render_acts();
    shell_hints("▲▼:选择|OK:执行|长OK:对讲");
}

static void talk_ui(bool on)
{
    lv_obj_set_style_bg_color(s_talk, ui_c(on ? C_LINK : C_SURFACE2), 0);
    lv_obj_t *tl = lv_obj_get_child(s_talk, 0);
    lv_label_set_text(tl, on ? "正在对讲 · 松开结束" : "长按确定键对讲");
    lv_obj_set_style_text_color(tl, ui_c(on ? C_BG : C_TEXT), 0);
}

static void unpair_answer(bool yes)
{
    if (!yes) return;
    link_unpair();
    s_log_count = 0;
    build(PAGE_NEARBY);
}

static void do_action(void)
{
    switch (s_act) {
    case ACT_MSG: build(PAGE_QUICK); break;
    case ACT_FOCUS: {
        int minutes = 25;
        pomo_running_focus(&minutes);
        if (link_send_focus(minutes)) {
            log_push("邀请一起专注", true);
            render_log();
            if (!pomo_running_focus(NULL)) pomo_remote_start(minutes);
            shell_toast("已开始专注,对方会收到邀请");
        }
        break;
    }
    case ACT_PING:
        if (link_send_ping()) shell_toast("已呼叫");
        break;
    default:
        build(PAGE_MORE);
        break;
    }
}

// ---------------------------------------------------------------------------
// 更多:后台在线、解除配对
// ---------------------------------------------------------------------------
static void build_more(lv_obj_t *p)
{
    lv_obj_t *t = ui_label(p, &ui_font_20b, C_TEXT, "互联设置");
    lv_obj_set_pos(t, UI_PAD + 4, 4);
    ui_list_create(&s_list, p, UI_PAD, 36, UI_W - 2 * UI_PAD, UI_CONTENT_H - 40, C_LINK);
    ui_list_add(&s_list, &IC_BELL, "离开后保持在线", s_background ? "开" : "关", false);
    ui_list_add(&s_list, &IC_CLOUD_OFF, "解除配对", NULL, false);
    ui_list_focus(&s_list, 0, false);
    lv_obj_t *n = ui_label_w(p, &ui_font_14, C_TEXT3, UI_W - 2 * UI_PAD, LV_TEXT_ALIGN_CENTER,
                             "保持在线时来消息会在任何界面提醒,但更耗电");
    lv_obj_align(n, LV_ALIGN_BOTTOM_MID, 0, -4);
    shell_hints("▲▼:选择|OK:确定|长▲:返回");
}

static void more_ok(void)
{
    if (s_list.focus == 0) {
        s_background = !s_background;
        sys_kv_set_i32("l_bg", s_background);
        ui_list_set_value(&s_list, 0, s_background ? "开" : "关");
    } else {
        shell_confirm("解除配对?", "之后需要重新配对才能互联", unpair_answer);
    }
}

// ---------------------------------------------------------------------------
// 附近设备(未配对)
// ---------------------------------------------------------------------------
static void render_nearby(void)
{
    const int focus = s_list.count ? s_list.focus : 0;
    ui_list_clear(&s_list);
    for (int i = 0; i < s_nearby_count; i++) {
        char v[16];
        snprintf(v, sizeof(v), "信号 %d", s_nearby[i].rssi);
        ui_list_add(&s_list, &ICON_LINK, s_nearby[i].name, v, false);
    }
    if (s_nearby_count) ui_list_focus(&s_list, focus < s_nearby_count ? focus : 0, false);
}

static void build_nearby(lv_obj_t *p)
{
    lv_obj_t *t = ui_label(p, &ui_font_20b, C_TEXT, "附近的设备");
    lv_obj_set_pos(t, UI_PAD + 4, 4);
    lv_obj_t *me = ui_label_w(p, &ui_font_14, C_TEXT3, UI_W - 2 * UI_PAD, LV_TEXT_ALIGN_LEFT, "");
    lv_label_set_text_fmt(me, "本机 %s", g_settings.name);
    lv_obj_set_pos(me, UI_PAD + 4, 32);
    ui_list_create(&s_list, p, UI_PAD, 56, UI_W - 2 * UI_PAD, UI_CONTENT_H - 60, C_LINK);
    s_nearby_count = link_nearby(s_nearby, 8);
    s_nearby_at = sys_now_ms();
    render_nearby();
    shell_hints("▲▼:选择|OK:请求配对|长▲:返回");
}

// ---------------------------------------------------------------------------
static void build_quick(lv_obj_t *p)
{
    lv_obj_t *t = ui_label(p, &ui_font_20b, C_TEXT, "快捷消息");
    lv_obj_set_pos(t, UI_PAD + 4, 4);
    ui_list_create(&s_list, p, UI_PAD, 36, UI_W - 2 * UI_PAD, UI_CONTENT_H - 40, C_LINK);
    for (int i = 0; i < QUICK_COUNT; i++) ui_list_add(&s_list, NULL, k_quick[i], NULL, false);
    ui_list_focus(&s_list, 0, false);
    shell_hints("▲▼:选择|OK:发送|长▲:返回");
}

static void build(page_t page)
{
    if (s_page) lv_obj_delete(s_page);
    s_page = ui_box(s_root, 0, 0, UI_W, UI_CONTENT_H);
    s_kind = page;
    memset(&s_list, 0, sizeof(s_list));
    switch (page) {
    case PAGE_MAIN: build_main(s_page); break;
    case PAGE_NEARBY: build_nearby(s_page); break;
    case PAGE_MORE: build_more(s_page); break;
    default: build_quick(s_page); break;
    }
}

static void create(lv_obj_t *root)
{
    s_root = root;
    s_page = NULL;
    s_talking = false;
    if (!link_running() && !link_start()) shell_toast("无线启动失败");
    link_peer_t p;
    build(link_partner(&p) ? PAGE_MAIN : PAGE_NEARBY);
}

static void destroy(void)
{
    if (s_talking) link_talk_end();
    s_talking = false;
    shell_keep_awake(false);
    link_peer_t p;
    if (!(s_background && link_partner(&p))) link_stop();
    s_root = NULL;
    s_page = NULL;
}

static void pair_answer(bool yes) { link_pair(s_pending_mac, yes); }

static bool key(sys_key_t k, sys_key_ev_t ev)
{
    if (s_kind == PAGE_MAIN) {
        if (k == SYS_KEY_OK && ev == SYS_EV_LONG && !s_talking) {
            if (link_talk_begin()) {
                s_talking = true;
                shell_keep_awake(true);
                talk_ui(true);
                audio_beep(BEEP_START);
            } else {
                shell_toast("对方不在线");
            }
            return true;
        }
        if (k == SYS_KEY_OK && ev == SYS_EV_RELEASE && s_talking) {
            s_talking = false;
            link_talk_end();
            shell_keep_awake(false);
            talk_ui(false);
            return true;
        }
        if (ev != SYS_EV_CLICK) return k == SYS_KEY_OK;
        if (k == SYS_KEY_OK) do_action();
        else {
            s_act = (s_act + (k == SYS_KEY_DOWN ? 1 : -1) + ACT_COUNT) % ACT_COUNT;
            render_acts();
        }
        return true;
    }
    if (s_kind == PAGE_MORE) {
        if (k == SYS_KEY_UP && ev == SYS_EV_LONG) {
            build(PAGE_MAIN);
            return true;
        }
        if (ev != SYS_EV_CLICK) return true;
        if (k == SYS_KEY_OK) more_ok();
        else ui_list_move(&s_list, k == SYS_KEY_DOWN ? 1 : -1);
        return true;
    }
    if (s_kind == PAGE_QUICK) {
        if (k == SYS_KEY_UP && ev == SYS_EV_LONG) {
            build(PAGE_MAIN);
            return true;
        }
        if (ev != SYS_EV_CLICK) return true;
        if (k == SYS_KEY_OK) {
            const char *msg = k_quick[s_list.focus];
            if (link_send_text(msg)) {
                log_push(msg, true);
                shell_toast("已发送");
            } else {
                shell_toast("发送失败,对方不在线?");
            }
            build(PAGE_MAIN);
        } else {
            ui_list_move(&s_list, k == SYS_KEY_DOWN ? 1 : -1);
        }
        return true;
    }
    // 附近设备
    if (ev != SYS_EV_CLICK) return false;
    if (k == SYS_KEY_OK) {
        if (s_nearby_count) {
            link_pair(s_nearby[s_list.focus].mac, true);
            shell_toast("已发出配对请求,请在对方设备上确认");
        }
    } else if (s_list.count) {
        ui_list_move(&s_list, k == SYS_KEY_DOWN ? 1 : -1);
    }
    return true;
}

static void tick(int64_t now)
{
    link_event_t ev;
    while (link_poll(&ev)) {
        switch (ev.type) {
        case LINK_EV_PAIR_REQUEST: {
            memcpy(s_pending_mac, ev.peer.mac, 6);
            char buf[64];
            snprintf(buf, sizeof(buf), "%s 想与你配对", ev.peer.name);
            audio_beep(BEEP_MSG);
            shell_confirm("配对请求", buf, pair_answer);
            break;
        }
        case LINK_EV_PAIRED:
            audio_beep(BEEP_OK);
            shell_toast("配对成功");
            build(PAGE_MAIN);
            break;
        case LINK_EV_TEXT:
            log_push(ev.text, false);
            audio_beep(BEEP_MSG);
            if (s_kind == PAGE_MAIN) render_log();
            break;
        case LINK_EV_VOICE_BEGIN:
            if (s_kind == PAGE_MAIN && !s_talking) {
                lv_obj_set_style_bg_color(s_talk, ui_c(ui_mix(C_LINK, C_SURFACE, 90)), 0);
                lv_label_set_text(lv_obj_get_child(s_talk, 0), "对方正在说话…");
            }
            break;
        case LINK_EV_VOICE_END:
            if (s_kind == PAGE_MAIN && !s_talking) talk_ui(false);
            break;
        default:
            handle_event_global(&ev);
            break;
        }
    }
    if (s_kind == PAGE_NEARBY && now - s_nearby_at > 1500) {
        const int n = link_nearby(s_nearby, 8);
        s_nearby_at = now;
        if (n != s_nearby_count || n) {
            s_nearby_count = n;
            render_nearby();
        }
        if (!n && !lv_obj_get_child_count(s_list.cont)) {
            lv_obj_t *e = ui_label(s_list.cont, &ui_font_16, C_TEXT3, "正在搜索…");
            lv_obj_align(e, LV_ALIGN_TOP_MID, 0, 40);
        }
    }
    if (s_kind == PAGE_MAIN) {
        static int64_t last;
        if (now - last > 1000) {
            last = now;
            render_partner();
        }
    }
}

const app_t APP_LINK = {
    .name = "设备互联",
    .desc = "对讲 · 消息 · 一起专注",
    .icon = &ICON_LINK,
    .color = C_LINK,
    .create = create,
    .destroy = destroy,
    .key = key,
    .tick = tick,
};
