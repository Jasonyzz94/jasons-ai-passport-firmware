// main/esp_ble.c —— 蓝牙 HID 服务:键盘(报告 1)+ 媒体键(报告 2),NimBLE + esp_hid。
//
// 配对方式:Just Works + 绑定(设备无输入能力),电脑/手机在蓝牙列表里点一下即可;
// 绑定信息存 NVS(CONFIG_BT_NIMBLE_NVS_PERSIST),下次自动回连。
// 发送动作在独立的 hid 任务里排队执行(每次按键需要按下 → 间隔 → 抬起)。
#include "hid_keymap.h"
#include "settings.h"
#include "svc.h"

#include "esp_hidd.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ble";

void ble_store_config_init(void);

#define RPT_KEYBOARD 1
#define RPT_CONSUMER 2

static const uint8_t k_report_map[] = {
    // 键盘
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, RPT_KEYBOARD,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,
    // 媒体键(16 位用法)
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, RPT_CONSUMER,
    0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xC0,
};

typedef enum { ACT_KEY = 0, ACT_CONSUMER, ACT_TEXT } act_type_t;
typedef struct {
    act_type_t type;
    uint8_t key, mod;
    uint16_t usage;
    char *text;
} act_t;

static esp_hidd_dev_t *s_dev;
static QueueHandle_t s_act;
static TaskHandle_t s_worker;
static volatile ble_state_t s_state = BLE_OFF;
static volatile bool s_running;
static struct ble_hs_adv_fields s_fields;
static char s_name[24];

ble_state_t ble_hid_state(void) { return s_state; }
const char *ble_hid_peer(void) { return "你的设备"; }

// ---------------------------------------------------------------------------
static int gap_event(struct ble_gap_event *event, void *arg);

static void adv_start(void)
{
    if (!s_running) return;
    struct ble_gap_adv_params p = {0};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = BLE_GAP_ADV_ITVL_MS(30);
    p.itvl_max = BLE_GAP_ADV_ITVL_MS(50);
    if (ble_gap_adv_set_fields(&s_fields) == 0 &&
        ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &p, gap_event, NULL) == 0) {
        s_state = BLE_ADVERTISING;
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    struct ble_gap_conn_desc desc;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) adv_start();
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_state = BLE_ADVERTISING;
        adv_start();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        adv_start();
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // 对方删掉了旧配对再来:丢弃旧绑定,接受新的。
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    default:
        return 0;
    }
}

static void hidd_event(void *handler_args, esp_event_base_t base, int32_t id, void *data)
{
    (void)handler_args;
    (void)base;
    (void)data;
    switch ((esp_hidd_event_t)id) {
    case ESP_HIDD_START_EVENT: adv_start(); break;
    case ESP_HIDD_CONNECT_EVENT: s_state = BLE_CONNECTED; ESP_LOGI(TAG, "已连接"); break;
    case ESP_HIDD_DISCONNECT_EVENT: s_state = BLE_ADVERTISING; adv_start(); break;
    default: break;
    }
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    adv_start();
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ---------------------------------------------------------------------------
// 发送
// ---------------------------------------------------------------------------
static void send_key(uint8_t key, uint8_t mod)
{
    uint8_t r[8] = {mod, 0, key, 0, 0, 0, 0, 0};
    esp_hidd_dev_input_set(s_dev, 0, RPT_KEYBOARD, r, sizeof(r));
    vTaskDelay(pdMS_TO_TICKS(12));
    memset(r, 0, sizeof(r));
    esp_hidd_dev_input_set(s_dev, 0, RPT_KEYBOARD, r, sizeof(r));
    vTaskDelay(pdMS_TO_TICKS(12));
}

static void worker(void *arg)
{
    (void)arg;
    act_t a;
    for (;;) {
        if (xQueueReceive(s_act, &a, portMAX_DELAY) != pdTRUE) continue;
        if (s_state != BLE_CONNECTED || !s_dev) {
            free(a.text);
            continue;
        }
        switch (a.type) {
        case ACT_KEY: send_key(a.key, a.mod); break;
        case ACT_CONSUMER: {
            uint8_t r[2] = {(uint8_t)(a.usage & 0xFF), (uint8_t)(a.usage >> 8)};
            esp_hidd_dev_input_set(s_dev, 0, RPT_CONSUMER, r, 2);
            vTaskDelay(pdMS_TO_TICKS(20));
            r[0] = r[1] = 0;
            esp_hidd_dev_input_set(s_dev, 0, RPT_CONSUMER, r, 2);
            break;
        }
        default:
            for (const char *p = a.text; p && *p; p++) {
                uint8_t k, m;
                if (hid_ascii_to_key(*p, &k, &m)) send_key(k, m);
            }
            free(a.text);
            break;
        }
    }
}

static void enqueue(act_t *a)
{
    if (!s_act || xQueueSend(s_act, a, 0) != pdTRUE) free(a->text);
}

void ble_hid_key(uint8_t keycode, uint8_t modifiers)
{
    act_t a = {.type = ACT_KEY, .key = keycode, .mod = modifiers};
    enqueue(&a);
}

void ble_hid_consumer(uint16_t usage)
{
    act_t a = {.type = ACT_CONSUMER, .usage = usage};
    enqueue(&a);
}

void ble_hid_type(const char *ascii)
{
    act_t a = {.type = ACT_TEXT, .text = strdup(ascii)};
    if (a.text) enqueue(&a);
}

// ---------------------------------------------------------------------------
bool ble_hid_start(void)
{
    if (s_running) return true;
    s_state = BLE_STARTING;
    if (!s_act) {
        s_act = xQueueCreate(16, sizeof(act_t));
        xTaskCreate(worker, "hid", 3072, NULL, 5, &s_worker);
    }
    if (nimble_port_init() != ESP_OK) {
        s_state = BLE_FAILED;
        return false;
    }
    snprintf(s_name, sizeof(s_name), "%s", g_settings.name);
    ble_svc_gap_device_name_set(s_name);
    ble_svc_gap_device_appearance_set(ESP_HID_APPEARANCE_KEYBOARD);

    memset(&s_fields, 0, sizeof(s_fields));
    static const ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
    s_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    s_fields.appearance = ESP_HID_APPEARANCE_KEYBOARD;
    s_fields.appearance_is_present = 1;
    s_fields.name = (uint8_t *)s_name;
    s_fields.name_len = (uint8_t)strlen(s_name);
    s_fields.name_is_complete = 1;
    s_fields.uuids16 = &hid_uuid;
    s_fields.num_uuids16 = 1;
    s_fields.uuids16_is_complete = 1;

    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;   // Just Works
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    static esp_hid_raw_report_map_t maps[] = {{.data = k_report_map, .len = sizeof(k_report_map)}};
    esp_hid_device_config_t cfg = {
        .vendor_id = 0x303A,
        .product_id = 0x8100,
        .version = 0x0100,
        .device_name = s_name,
        .manufacturer_name = "Passport",
        .serial_number = "0001",
        .report_maps = maps,
        .report_maps_len = 1,
    };
    if (esp_hidd_dev_init(&cfg, ESP_HID_TRANSPORT_BLE, hidd_event, &s_dev) != ESP_OK) {
        nimble_port_deinit();
        s_state = BLE_FAILED;
        return false;
    }
    ble_store_config_init();
    s_running = true;
    nimble_port_freertos_init(host_task);
    return true;
}

void ble_hid_stop(void)
{
    if (!s_running) return;
    s_running = false;
    ble_gap_adv_stop();
    if (s_dev) {
        esp_hidd_dev_deinit(s_dev);
        s_dev = NULL;
    }
    if (nimble_port_stop() == 0) nimble_port_deinit();
    s_state = BLE_OFF;
}

void ble_hid_forget(void)
{
    ble_store_clear();
}
