// main/esp_sys.c —— sys.h 的固件实现:按键队列、背光、电量、NVS 键值、随机数、MAC、内存。
#include "sys.h"

#include "key_guard.h"

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "esp_sleep.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "sys";
#define NVS_NS "toolbox"

static QueueHandle_t s_keys;
static volatile bool s_input_ready;

// 深睡按键唤醒后,唤醒键还按着:松手前吞掉它的动作(见 key_guard.h)。
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)
static key_guard_t s_key_guard;
static bool s_battery_ok;
static char s_short_id[8];

typedef struct {
    uint8_t key;
    uint8_t ev;
} key_msg_t;

// 按键回调运行在 esp_timer 任务:只入队,不碰 LVGL。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    // 先消费守卫再判断就绪:松手事件必须能解除唤醒键屏蔽。
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) return;
    if (!s_input_ready || !s_keys) return;
    const key_msg_t m = {.key = (uint8_t)btn, .ev = (uint8_t)ev};
    (void)xQueueSend(s_keys, &m, 0);
}

// BSP 的枚举顺序与 sys.h 一致(UP/DOWN/OK;PRESS/CLICK/DOUBLE/LONG/RELEASE)。
_Static_assert((int)BSP_BTN_UP == (int)SYS_KEY_UP && (int)BSP_BTN_OK == (int)SYS_KEY_OK, "key map");
_Static_assert((int)BSP_BTN_LONG == (int)SYS_EV_LONG && (int)BSP_BTN_RELEASE == (int)SYS_EV_RELEASE,
               "event map");

esp_err_t sys_esp_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 分区格式无法读取时只能重建;正常升级不会走到这里。
        ESP_LOGW(TAG, "NVS 需要重建: %s", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(err));

    if (esp_sleep_get_wakeup_causes() & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
    }
    s_battery_ok = bsp_battery_init() == ESP_OK;
    s_keys = xQueueCreate(16, sizeof(key_msg_t));
    if (!s_keys) return ESP_ERR_NO_MEM;
    const esp_err_t btn_err = bsp_button_init(on_key, NULL);
    if (btn_err == ESP_OK && s_key_guard.armed) {
        // 唤醒键若在按键驱动启动前已松手,不会产生 release 事件,按真实电压同步撤防。
        key_guard_sync_released(&s_key_guard, bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN);
    }
    s_input_ready = true;
    return btn_err;
}

bool sys_key_any_held(void)
{
    int v[5];
    for (int i = 0; i < 5; i++) {
        v[i] = bsp_button_read_mv();
        if (v[i] < 0) return false;
    }
    for (int i = 1; i < 5; i++) {   // 插入排序取中位数
        const int x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > x) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = x;
    }
    return v[2] < BSP_BTN_MV_RELEASED_MIN;
}

bool sys_key_pop(sys_key_t *key, sys_key_ev_t *ev)
{
    key_msg_t m;
    if (!s_keys || xQueueReceive(s_keys, &m, 0) != pdTRUE) return false;
    *key = (sys_key_t)m.key;
    *ev = (sys_key_ev_t)m.ev;
    return true;
}

int64_t sys_now_ms(void) { return esp_timer_get_time() / 1000; }
void sys_backlight(uint8_t percent) { bsp_display_backlight(percent); }
int sys_battery_soc(void) { return s_battery_ok ? bsp_battery_soc() : -1; }
int sys_battery_mv(void) { return s_battery_ok ? bsp_battery_mv() : -1; }

static bool kv_open(nvs_open_mode_t mode, nvs_handle_t *h)
{
    return nvs_open(NVS_NS, mode, h) == ESP_OK;
}

int32_t sys_kv_get_i32(const char *key, int32_t def)
{
    nvs_handle_t h;
    int32_t v = def;
    if (kv_open(NVS_READONLY, &h)) {
        if (nvs_get_i32(h, key, &v) != ESP_OK) v = def;
        nvs_close(h);
    }
    return v;
}

void sys_kv_set_i32(const char *key, int32_t value)
{
    nvs_handle_t h;
    if (!kv_open(NVS_READWRITE, &h)) return;
    int32_t old;
    if (nvs_get_i32(h, key, &old) != ESP_OK || old != value) {   // 避免无谓的闪存写
        nvs_set_i32(h, key, value);
        nvs_commit(h);
    }
    nvs_close(h);
}

bool sys_kv_get_str(const char *key, char *buf, size_t len)
{
    nvs_handle_t h;
    if (len) buf[0] = '\0';
    if (!kv_open(NVS_READONLY, &h)) return false;
    size_t n = len;
    const bool ok = nvs_get_str(h, key, buf, &n) == ESP_OK;
    if (!ok && len) buf[0] = '\0';
    nvs_close(h);
    return ok;
}

void sys_kv_set_str(const char *key, const char *value)
{
    nvs_handle_t h;
    if (!kv_open(NVS_READWRITE, &h)) return;
    char old[128];
    size_t n = sizeof(old);
    if (nvs_get_str(h, key, old, &n) != ESP_OK || strcmp(old, value) != 0) {
        nvs_set_str(h, key, value);
        nvs_commit(h);
    }
    nvs_close(h);
}

bool sys_kv_get_blob(const char *key, void *buf, size_t len)
{
    nvs_handle_t h;
    if (!kv_open(NVS_READONLY, &h)) return false;
    size_t n = len;
    const bool ok = nvs_get_blob(h, key, buf, &n) == ESP_OK && n == len;
    nvs_close(h);
    return ok;
}

void sys_kv_set_blob(const char *key, const void *buf, size_t len)
{
    nvs_handle_t h;
    if (!kv_open(NVS_READWRITE, &h)) return;
    nvs_set_blob(h, key, buf, len);
    nvs_commit(h);
    nvs_close(h);
}

void sys_kv_erase_all(void)
{
    nvs_handle_t h;
    if (!kv_open(NVS_READWRITE, &h)) return;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
}

uint32_t sys_random(void) { return esp_random(); }

void sys_mac(uint8_t mac[6]) { esp_read_mac(mac, ESP_MAC_WIFI_STA); }

const char *sys_short_id(void)
{
    if (!s_short_id[0]) {
        uint8_t mac[6];
        sys_mac(mac);
        snprintf(s_short_id, sizeof(s_short_id), "%02X%02X", mac[4], mac[5]);
    }
    return s_short_id;
}

uint32_t sys_free_heap(void) { return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }
uint32_t sys_largest_block(void)
{
    return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
}

void sys_restart(void) { esp_restart(); }

void sys_log_memory(const char *where)
{
    ESP_LOGI(TAG, "内存[%s] 空闲 %u,最大块 %u,历史最低 %u", where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}
