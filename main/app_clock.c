// main/app_clock.c —— 时钟天气:大号时间 + 日期 + 当前天气 + 4 天/逐小时预报。
//
// 天气由 Mac mini 中转服务器的 /api/weather 提供(服务器负责定位与数据源),
// 设备只解析一小段 JSON。打开时连 Wi-Fi,离开时释放;最近一次天气缓存在内存里,
// 重新打开立即显示。
// 按键:▲▼ 切换 每日/逐小时 · OK 刷新 · 长OK 床头钟(常亮低亮度) · 长▲ 返回
#include "apps.h"
#include "icons.h"
#include "settings.h"
#include "svc.h"
#include "ui.h"

#include "cJSON.h"
#include <stdio.h>
#include <string.h>

#define REFRESH_MS (15 * 60 * 1000)
#define CACHE_LEN 2048

static const char *const k_wday[7] = {"星期日", "星期一", "星期二", "星期三",
                                       "星期四", "星期五", "星期六"};

static char s_cache[CACHE_LEN];        // 最近一次天气 JSON(跨进出保留)
static int64_t s_cache_ms = -REFRESH_MS;

static lv_obj_t *s_root;
static lv_obj_t *s_time;
static lv_obj_t *s_date;
static lv_obj_t *s_card;
static lv_obj_t *s_status;
static int s_req = -1;
static bool s_hourly;
static bool s_night;
static int s_last_min = -1;

const pixel_sprite_t *weather_sprite(const char *code)
{
    if (!code) return &WX_CLOUD;
    if (!strcmp(code, "sun")) return &WX_SUN;
    if (!strcmp(code, "partly")) return &WX_PARTLY;
    if (!strcmp(code, "rain")) return &WX_RAIN;
    if (!strcmp(code, "snow")) return &WX_SNOW;
    if (!strcmp(code, "storm")) return &WX_STORM;
    if (!strcmp(code, "fog")) return &WX_FOG;
    if (!strcmp(code, "moon")) return &WX_MOON;
    return &WX_CLOUD;
}

static int jint(const cJSON *o, const char *k, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (int)(v->valuedouble + (v->valuedouble >= 0 ? 0.5 : -0.5)) : def;
}

static const char *jstr(const cJSON *o, const char *k, const char *def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : def;
}

static void card_message(const pixel_sprite_t *icon, const char *title, const char *detail)
{
    lv_obj_clean(s_card);
    ui_empty(s_card, icon, title, detail);
}

static void render_weather(void)
{
    cJSON *root = cJSON_Parse(s_cache);
    if (!root) {
        card_message(&IC_CLOUD_OFF, "天气数据异常", "稍后按确定键重试");
        return;
    }
    lv_obj_clean(s_card);
    const cJSON *now = cJSON_GetObjectItemCaseSensitive(root, "now");
    const cJSON *today = cJSON_GetObjectItemCaseSensitive(root, "today");

    lv_obj_t *icon = ui_pix(s_card, weather_sprite(jstr(now, "code", "cloud")), 3);
    lv_obj_set_pos(icon, 12, 10);

    char buf[48];
    snprintf(buf, sizeof(buf), "%d°", jint(now, "temp", 0));
    lv_obj_t *temp = ui_label(s_card, ui_font_36b, C_TEXT, buf);
    lv_obj_set_pos(temp, 68, 10);

    lv_obj_t *cond = ui_label_w(s_card, &ui_font_16, C_TEXT, 92, LV_TEXT_ALIGN_RIGHT,
                                jstr(now, "text", ""));
    lv_obj_align(cond, LV_ALIGN_TOP_RIGHT, -12, 10);
    snprintf(buf, sizeof(buf), "%d° / %d°", jint(today, "hi", 0), jint(today, "lo", 0));
    lv_obj_t *range = ui_label_w(s_card, &ui_font_14, C_TEXT2, 92, LV_TEXT_ALIGN_RIGHT, buf);
    lv_obj_align(range, LV_ALIGN_TOP_RIGHT, -12, 32);

    // 显示数据的更新时间,一眼看出天气是不是最新的
    snprintf(buf, sizeof(buf), "%s · 湿度 %d%% · %s 更新", jstr(root, "city", ""),
             jint(now, "humidity", 0), jstr(root, "updated", "--"));
    lv_obj_t *meta = ui_label_w(s_card, &ui_font_14, C_TEXT3, 192, LV_TEXT_ALIGN_LEFT, buf);
    lv_obj_set_pos(meta, 12, 62);

    // 预报行
    lv_obj_t *line = ui_card(s_card, 12, 84, 192, 1, C_LINE, 0);
    (void)line;
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, s_hourly ? "hourly" : "daily");
    const int n = cJSON_GetArraySize(arr) > (s_hourly ? 5 : 4) ? (s_hourly ? 5 : 4)
                                                              : cJSON_GetArraySize(arr);
    const int col_w = n ? 192 / n : 192;
    for (int i = 0; i < n; i++) {
        const cJSON *it = cJSON_GetArrayItem(arr, i);
        lv_obj_t *col = ui_box(s_card, 12 + i * col_w, 90, col_w, 60);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(col, 1, 0);
        ui_label(col, &ui_font_14, i == 0 && !s_hourly ? C_CLOCK : C_TEXT3,
                 jstr(it, s_hourly ? "h" : "day", ""));
        ui_pix(col, weather_sprite(jstr(it, "code", "cloud")), 1);
        if (s_hourly) snprintf(buf, sizeof(buf), "%d°", jint(it, "t", 0));
        else snprintf(buf, sizeof(buf), "%d/%d°", jint(it, "hi", 0), jint(it, "lo", 0));
        ui_label(col, &ui_font_14, C_TEXT2, buf);
    }
    cJSON_Delete(root);
}

static void update_status(void)
{
    if (s_cache[0]) {
        render_weather();
        return;
    }
    if (!settings_has_wifi()) {
        card_message(&IC_CLOUD_OFF, "还没连接无线网络", "在 设置 › 无线网络 里用手机配置");
    } else if (net_state() != NET_CONNECTED) {
        card_message(&WX_CLOUD, "正在联网…", NULL);
    } else if (!relay_ready()) {
        card_message(&IC_CLOUD_OFF, "暂时连不上网络", "稍后按确定键重试");
    } else {
        card_message(&WX_CLOUD, "正在获取天气…", NULL);
    }
}

static void request(void)
{
    if (s_req >= 0 || !relay_ready()) return;
    s_req = relay_request("/api/weather", NULL);
}

static void refresh_clock(bool force)
{
    struct tm tm;
    if (!time_local(&tm)) {
        if (force) {
            lv_label_set_text(s_time, "--:--");
            lv_label_set_text(s_date, "等待联网对时");
        }
        return;
    }
    if (force || tm.tm_min != s_last_min) {
        lv_label_set_text_fmt(s_time, "%02d:%02d", tm.tm_hour, tm.tm_min);
        lv_label_set_text_fmt(s_date, "%d月%d日  %s", tm.tm_mon + 1, tm.tm_mday, k_wday[tm.tm_wday]);
        s_last_min = tm.tm_min;
    }
}

static void apply_night(void)
{
    if (s_night) {
        lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(s_time, LV_ALIGN_CENTER, 0, -20);
        lv_obj_align(s_date, LV_ALIGN_CENTER, 0, 40);
        lv_obj_set_style_text_color(s_time, ui_c(0xC2410C), 0);
        lv_obj_set_style_text_color(s_date, ui_c(0x7C2D12), 0);
        sys_backlight(15);
        shell_keep_awake(true);
        shell_hints("长OK:退出床头钟");
    } else {
        lv_obj_clear_flag(s_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(s_time, LV_ALIGN_TOP_MID, 0, 2);
        lv_obj_align(s_date, LV_ALIGN_TOP_MID, 0, 74);
        lv_obj_set_style_text_color(s_time, ui_c(C_TEXT), 0);
        lv_obj_set_style_text_color(s_date, ui_c(C_TEXT2), 0);
        shell_keep_awake(false);
        shell_apply_brightness();
        shell_hints("▲▼:切换|OK:刷新|长OK:床头钟");
    }
}

static void create(lv_obj_t *root)
{
    s_root = root;
    s_req = -1;
    s_night = false;
    s_last_min = -1;

    s_time = ui_label(root, ui_font_64b, C_TEXT, "--:--");
    s_date = ui_label(root, &ui_font_16, C_TEXT2, "");

    s_card = ui_card(root, UI_PAD, 102, UI_W - 2 * UI_PAD, 152, C_SURFACE, 18);
    s_status = NULL;
    apply_night();
    refresh_clock(true);
    update_status();

    net_acquire();
    if (sys_now_ms() - s_cache_ms >= REFRESH_MS) request();
}

static void destroy(void)
{
    if (s_req >= 0) relay_free(s_req);
    s_req = -1;
    if (s_night) {
        shell_keep_awake(false);
        shell_apply_brightness();
    }
    net_release();
    s_root = NULL;
}

static bool key(sys_key_t k, sys_key_ev_t ev)
{
    if (s_night) {
        if (k == SYS_KEY_OK && ev == SYS_EV_LONG) {
            s_night = false;
            apply_night();
            refresh_clock(true);
        }
        return ev == SYS_EV_CLICK || ev == SYS_EV_LONG;   // 床头钟模式下屏蔽误触(长▲ 也不退出)
    }
    if (ev == SYS_EV_CLICK && (k == SYS_KEY_UP || k == SYS_KEY_DOWN)) {
        s_hourly = !s_hourly;
        if (s_cache[0]) render_weather();
        return true;
    }
    if (k == SYS_KEY_OK && ev == SYS_EV_CLICK) {
        s_cache_ms = sys_now_ms() - REFRESH_MS;   // 标记为过期:联网后会自动刷新
        if (!relay_ready()) {
            shell_toast("正在联网,连上后自动刷新");
        } else if (s_req >= 0) {
            shell_toast("正在获取,请稍候");
        } else {
            request();
            shell_toast("正在刷新天气");
        }
        return true;
    }
    if (k == SYS_KEY_OK && ev == SYS_EV_LONG) {
        s_night = true;
        apply_night();
        refresh_clock(true);
        return true;
    }
    return false;
}

static void tick(int64_t now)
{
    refresh_clock(false);
    if (s_req < 0) {
        static net_state_t last_net;
        static srv_state_t last_srv;
        const net_state_t ns = net_state();
        const srv_state_t ss = relay_state();
        if (ns != last_net || ss != last_srv) {
            last_net = ns;
            last_srv = ss;
            if (!s_night) update_status();
            if (relay_ready() && now - s_cache_ms >= REFRESH_MS) request();
        }
        if (relay_ready() && now - s_cache_ms >= REFRESH_MS) request();
        return;
    }
    const char *body;
    int status;
    const req_state_t st = relay_poll(s_req, &body, &status);
    if (st == REQ_DONE && status == 200) {
        snprintf(s_cache, sizeof(s_cache), "%s", body);
        s_cache_ms = now;
        relay_free(s_req);
        s_req = -1;
        if (!s_night) render_weather();
    } else if (st == REQ_DONE || st == REQ_ERROR) {
        relay_free(s_req);
        s_req = -1;
        s_cache_ms = now - REFRESH_MS + 60000;   // 1 分钟后重试
        if (!s_cache[0] && !s_night) card_message(&IC_CLOUD_OFF, "获取天气失败", "按确定键重试");
    }
}

const app_t APP_CLOCK = {
    .name = "时钟天气",
    .desc = "时间 · 天气 · 预报",
    .icon = &ICON_WEATHER,
    .color = C_CLOCK,
    .create = create,
    .destroy = destroy,
    .key = key,
    .tick = tick,
};
