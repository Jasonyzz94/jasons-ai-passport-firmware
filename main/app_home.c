// main/app_home.c —— 智能家居:经中转服务器控制 Home Assistant(灯、开关、场景、传感器)。
// 设备上不存 HA 令牌;服务器的 config.toml 决定哪些实体出现在这里。
// 按键:▲▼ 选择 · OK 开/关/执行 · 长▲ 返回
#include "apps.h"
#include "icons.h"
#include "svc.h"
#include "ui.h"

#include "cJSON.h"
#include <stdio.h>
#include <string.h>

#define MAX_ITEMS 16
#define POLL_MS 10000

typedef struct {
    char id[48];
    char name[24];
    char type[10];
    char state[16];
    bool on;
} item_t;

static item_t s_items[MAX_ITEMS];
static int s_count;
static ui_list_t s_list;
static lv_obj_t *s_root;
static lv_obj_t *s_body;
static lv_obj_t *s_source;
static int s_req = -1;
static int s_toggle_req = -1;
static int64_t s_polled_ms;
static bool s_built;

static const pixel_sprite_t *type_icon(const char *t)
{
    if (!strcmp(t, "light")) return &IC_BULB;
    if (!strcmp(t, "switch")) return &IC_PLUG;
    if (!strcmp(t, "scene") || !strcmp(t, "script")) return &IC_SCENE;
    return &IC_SENSOR;
}

static bool toggleable(const item_t *it)
{
    return !strcmp(it->type, "light") || !strcmp(it->type, "switch") || !strcmp(it->type, "scene")
        || !strcmp(it->type, "script");
}

static uint32_t state_color(const item_t *it)
{
    if (!strcmp(it->type, "sensor")) return C_TEXT;
    if (!strcmp(it->type, "scene") || !strcmp(it->type, "script")) return C_HOME;
    return it->on ? C_OK : C_TEXT3;
}

static void parse_item(const cJSON *o, item_t *it)
{
    const cJSON *v;
#define STR(field, key) \
    v = cJSON_GetObjectItemCaseSensitive(o, key); \
    if (cJSON_IsString(v)) snprintf(it->field, sizeof(it->field), "%s", v->valuestring)
    STR(id, "id");
    STR(name, "name");
    STR(type, "type");
    STR(state, "state");
#undef STR
    it->on = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "on"));
}

static void build_list(void)
{
    const int focus = s_built ? s_list.focus : 0;
    lv_obj_clean(s_body);
    if (!s_count) {
        if (relay_state() != SRV_OK) {
            ui_empty(s_body, &IC_CLOUD_OFF, "找不到中转服务器", "智能家居经 Mac mini 转发");
        } else {
            ui_empty(s_body, &ICON_HOME, s_req >= 0 ? "正在加载…" : "没有设备",
                     s_req >= 0 ? NULL : "在服务器配置 Home Assistant");
        }
        s_built = false;
        return;
    }
    ui_list_create(&s_list, s_body, 0, 0, UI_W - 2 * UI_PAD, UI_CONTENT_H - 40, C_HOME);
    for (int i = 0; i < s_count; i++) {
        ui_list_add(&s_list, type_icon(s_items[i].type), s_items[i].name, s_items[i].state, false);
        ui_list_set_value_color(&s_list, i, state_color(&s_items[i]));
    }
    ui_list_focus(&s_list, focus < s_count ? focus : 0, false);
    s_built = true;
}

static void update_rows(void)
{
    if (!s_built || s_list.count != s_count) {
        build_list();
        return;
    }
    for (int i = 0; i < s_count; i++) {
        ui_list_set_value(&s_list, i, s_items[i].state);
        ui_list_set_value_color(&s_list, i, state_color(&s_items[i]));
    }
}

static void parse_list(const char *body)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) return;
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "items");
    int n = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (n >= MAX_ITEMS) break;
        memset(&s_items[n], 0, sizeof(item_t));
        parse_item(it, &s_items[n++]);
    }
    s_count = n;
    const cJSON *src = cJSON_GetObjectItemCaseSensitive(root, "source");
    if (s_source && cJSON_IsString(src)) lv_label_set_text(s_source, src->valuestring);
    cJSON_Delete(root);
    update_rows();
}

static void request_list(void)
{
    if (s_req < 0 && relay_state() == SRV_OK) s_req = relay_request("/api/home", NULL);
}

static void create(lv_obj_t *root)
{
    s_root = root;
    s_req = s_toggle_req = -1;
    s_built = false;
    lv_obj_t *t = ui_label(root, &ui_font_20b, C_TEXT, "智能家居");
    lv_obj_set_pos(t, UI_PAD + 4, 4);
    s_source = ui_label(root, &ui_font_14, C_TEXT3, "");
    lv_obj_align(s_source, LV_ALIGN_TOP_RIGHT, -UI_PAD - 4, 10);
    s_body = ui_box(root, UI_PAD, 36, UI_W - 2 * UI_PAD, UI_CONTENT_H - 40);
    shell_hints("▲▼:选择|OK:切换|长▲:返回");
    net_acquire();
    build_list();
    request_list();
}

static void destroy(void)
{
    if (s_req >= 0) relay_free(s_req);
    if (s_toggle_req >= 0) relay_free(s_toggle_req);
    s_req = s_toggle_req = -1;
    net_release();
    s_root = NULL;
    s_built = false;
}

static bool key(sys_key_t k, sys_key_ev_t ev)
{
    if (ev != SYS_EV_CLICK) return false;
    if (!s_built) {
        if (k == SYS_KEY_OK) {
            if (relay_state() != SRV_OK) relay_discover();
            request_list();
        }
        return true;
    }
    if (k != SYS_KEY_OK) {
        ui_list_move(&s_list, k == SYS_KEY_DOWN ? 1 : -1);
        return true;
    }
    item_t *it = &s_items[s_list.focus];
    if (!toggleable(it)) {
        shell_toast("传感器只读");
        return true;
    }
    if (s_toggle_req >= 0) return true;
    char body[96];
    snprintf(body, sizeof(body), "{\"id\":\"%s\"}", it->id);
    s_toggle_req = relay_request("/api/home/toggle", body);
    if (s_toggle_req < 0) {
        shell_toast("服务器忙,请重试");
        return true;
    }
    // 乐观更新:立即翻转显示,服务器回包后再校正。
    if (strcmp(it->type, "scene") && strcmp(it->type, "script")) {
        it->on = !it->on;
        snprintf(it->state, sizeof(it->state), "%s", it->on ? "开" : "关");
    } else {
        snprintf(it->state, sizeof(it->state), "已执行");
    }
    audio_beep(BEEP_CLICK);
    update_rows();
    return true;
}

static void tick(int64_t now)
{
    const char *body;
    int status;
    if (s_toggle_req >= 0) {
        const req_state_t st = relay_poll(s_toggle_req, &body, &status);
        if (st == REQ_DONE || st == REQ_ERROR) {
            if (st == REQ_ERROR || status != 200) shell_toast("操作失败");
            relay_free(s_toggle_req);
            s_toggle_req = -1;
            s_polled_ms = now - POLL_MS + 800;   // 稍后拉一次真实状态
        }
    }
    if (s_req >= 0) {
        const req_state_t st = relay_poll(s_req, &body, &status);
        if (st == REQ_DONE || st == REQ_ERROR) {
            if (st == REQ_DONE && status == 200) parse_list(body);
            relay_free(s_req);
            s_req = -1;
            s_polled_ms = now;
            if (!s_built) build_list();
        }
    } else if (now - s_polled_ms >= POLL_MS && s_toggle_req < 0) {
        request_list();
        if (s_req < 0 && !s_built) {
            static srv_state_t last;
            if (relay_state() != last) {
                last = relay_state();
                build_list();
            }
        }
    }
}

const app_t APP_HOME = {
    .name = "智能家居",
    .desc = "灯光 · 开关 · 场景",
    .icon = &ICON_HOME,
    .color = C_HOME,
    .create = create,
    .destroy = destroy,
    .key = key,
    .tick = tick,
};
