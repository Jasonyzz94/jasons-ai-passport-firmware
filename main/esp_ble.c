// main/esp_ble.c —— 蓝牙 HID 服务:键盘(报告 1)+ 媒体键(报告 2),NimBLE + esp_hid。
//
// 配对方式:Just Works + 绑定(设备无输入能力),电脑/手机在蓝牙列表里点一下即可;
// 绑定信息存 NVS(CONFIG_BT_NIMBLE_NVS_PERSIST),下次自动回连。
// HID 特征要求加密(CONFIG_BT_NIMBLE_SM_LVL=2),连接建立后外设主动发起配对(见 ble_sec.h):
// macOS 不会自己发起,只连 GATT 不加密时系统设置会一直停在"正在连接"。
// 发送动作在独立的 hid 任务里排队执行(每次按键需要按下 → 间隔 → 抬起)。
#include "ble_sec.h"
#include "hid_keymap.h"
#include "sys.h"
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
#include "services/dis/ble_svc_dis.h"
#include "services/gap/ble_svc_gap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ble";

void ble_store_config_init(void);
void net_yield_radio(void);
bool net_radio_on(void);
void lv_mem_reserve_suspend(bool on);

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

// 带 Apple Fn 的完整报告表:在上面两段之后追加 Apple 私有 "Top Case" 页的 Fn 键(报告 3,1 位)。
// Mac 能否识别第三方设备的 Fn 取决于系统版本;若这张表让 HID 初始化失败,会自动退回上面的基础表。
#define RPT_FN 3
static const uint8_t k_report_map_fn[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, RPT_KEYBOARD,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, RPT_CONSUMER,
    0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xC0,
    0x06, 0xFF, 0x00, 0x09, 0x01, 0xA1, 0x01, 0x85, RPT_FN,          // Usage Page 0x00FF(Apple Top Case)
    0x09, 0x03, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,   // Usage 0x03 = Fn
    0x75, 0x07, 0x95, 0x01, 0x81, 0x03,                               // 补齐 7 位
    0xC0,
};
static bool s_has_fn;
static uint8_t s_held_mod;      // 正在按住的修饰键(语音键)

typedef enum { ACT_KEY = 0, ACT_CONSUMER, ACT_TEXT, ACT_START, ACT_STOP, ACT_HOLD } act_type_t;
typedef struct {
    act_type_t type;
    uint8_t key, mod;
    uint16_t usage;
    char *text;
} act_t;

static esp_hidd_dev_t *s_dev;
static QueueHandle_t s_act;
static TaskHandle_t volatile s_worker;
static portMUX_TYPE s_worker_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile ble_state_t s_state = BLE_OFF;
static volatile bool s_running;
static volatile bool s_stop_pending;   // 已请求关闭、hid 任务还没处理完

// 蓝牙是否正在启动或关闭(过渡中)。Wi-Fi 启动前会等它结束,避免两边同时抢内存。
// 蓝牙正常运行时不算:Wi-Fi 与蓝牙可以同时工作。
bool ble_radio_busy(void) { return s_stop_pending || s_state == BLE_STARTING; }
static struct ble_hs_adv_fields s_fields;
static char s_name[24];
static ble_sec_t s_sec;   // 当前连接的加密状态(只在 NimBLE 主机任务里改)

static char s_error[48];            // 最近一次失败原因(界面显示)
static volatile int64_t s_start_ms;  // 进入 BLE_STARTING 的时间,用于启动超时
#define START_TIMEOUT_MS 10000

static void fail(const char *why)
{
    snprintf(s_error, sizeof(s_error), "%s", why);
    ESP_LOGE(TAG, "蓝牙失败:%s(空闲 %u,最大块 %u)", why, (unsigned)sys_free_heap(),
             (unsigned)sys_largest_block());
    s_state = BLE_FAILED;
}

ble_state_t ble_hid_state(void) { return s_state; }
const char *ble_hid_error(void) { return s_error; }
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
    if (ble_gap_adv_active()) {
        s_state = BLE_ADVERTISING;
        return;
    }
    int rc = ble_gap_adv_set_fields(&s_fields);
    if (rc == 0) rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc == 0) {
        s_state = BLE_ADVERTISING;
        ESP_LOGI(TAG, "开始广播(空闲 %u)", (unsigned)sys_free_heap());
    } else if (s_state != BLE_CONNECTED) {
        char why[48];
        snprintf(why, sizeof(why), rc == BLE_HS_ENOMEM ? "广播失败:内存不足" : "广播失败 %d", rc);
        fail(why);
    }
}

// 发起一次安全请求。对端已在配对中(BLE_HS_EALREADY)也算成功。
static void sec_initiate(uint16_t conn)
{
    const int rc = ble_gap_security_initiate(conn);
    const bool ok = rc == 0 || rc == BLE_HS_EALREADY;
    if (ok) ESP_LOGI(TAG, "发起配对(连接 %u)", conn);
    else ESP_LOGW(TAG, "发起配对失败 %d,等待对端访问加密特征时再配对", rc);
    ble_sec_on_initiate_result(&s_sec, ok);
}

static void sec_apply(ble_sec_action_t act, uint16_t conn)
{
    struct ble_gap_conn_desc desc;
    switch (act) {
    case BLE_SEC_INITIATE: sec_initiate(conn); break;
    case BLE_SEC_FORGET_RETRY:
        // 对端删了配对、本机还留着旧绑定:丢掉旧绑定重新配对一次。
        ESP_LOGW(TAG, "对端没有旧密钥,删除本地旧绑定后重新配对");
        if (ble_gap_conn_find(conn, &desc) == 0) ble_store_util_delete_peer(&desc.peer_id_addr);
        sec_initiate(conn);
        break;
    case BLE_SEC_DISCONNECT:
        ESP_LOGW(TAG, "配对失败,断开连接后重新广播");
        ble_gap_terminate(conn, BLE_ERR_AUTH_FAIL);
        break;
    default: break;
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    struct ble_gap_conn_desc desc;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status != 0) {
            adv_start();
            return 0;
        }
        const uint16_t conn = event->connect.conn_handle;
        const bool enc = ble_gap_conn_find(conn, &desc) == 0 && desc.sec_state.encrypted;
        sec_apply(ble_sec_on_connect(&s_sec, enc), conn);
        if (enc) s_state = BLE_CONNECTED;
        return 0;
    }
    case BLE_GAP_EVENT_ENC_CHANGE: {
        const int st = event->enc_change.status;
        const uint16_t conn = event->enc_change.conn_handle;
        const bool key_missing = st == BLE_HS_ERR_HCI_BASE + BLE_ERR_PINKEY_MISSING;
        bool bonded = false;
        if (st == 0 && ble_gap_conn_find(conn, &desc) == 0) bonded = desc.sec_state.bonded;
        if (st == 0) ESP_LOGI(TAG, "链路已加密%s", bonded ? ",已绑定" : "");
        else ESP_LOGW(TAG, "加密失败 %d", st);
        sec_apply(ble_sec_on_enc_change(&s_sec, st == 0, key_missing), conn);
        if (ble_sec_ready(&s_sec)) s_state = BLE_CONNECTED;
        return 0;
    }
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "连接断开 0x%x", event->disconnect.reason);
        ble_sec_on_disconnect(&s_sec);
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
    case ESP_HIDD_CONNECT_EVENT:
        // GATT 连上不等于配对完成:链路加密后才算"已连接"(见 gap_event 的 ENC_CHANGE)。
        if (ble_sec_ready(&s_sec)) s_state = BLE_CONNECTED;
        ESP_LOGI(TAG, "HID 已连接%s", ble_sec_ready(&s_sec) ? "" : ",等待配对加密");
        break;
    case ESP_HIDD_DISCONNECT_EVENT: s_state = BLE_ADVERTISING; adv_start(); break;
    default: break;
    }
}

static void on_sync(void)
{
    const int rc = ble_hs_util_ensure_addr(0);
    ESP_LOGI(TAG, "协议栈已同步(地址 %d,空闲 %u)", rc, (unsigned)sys_free_heap());
    if (rc != 0) {
        fail("蓝牙地址不可用");
        return;
    }
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
    // 按住语音键时其他按键也带上它(例如按住 ⌘ 时单击 = ⌘+键)
    uint8_t r[8] = {(uint8_t)(mod | s_held_mod), 0, key, 0, 0, 0, 0, 0};
    esp_hidd_dev_input_set(s_dev, 0, RPT_KEYBOARD, r, sizeof(r));
    vTaskDelay(pdMS_TO_TICKS(12));
    memset(r, 0, sizeof(r));
    r[0] = s_held_mod;
    esp_hidd_dev_input_set(s_dev, 0, RPT_KEYBOARD, r, sizeof(r));
    vTaskDelay(pdMS_TO_TICKS(12));
}

static bool do_start(void);
static void do_stop(void);

static void worker(void *arg)
{
    (void)arg;
    act_t a;
    for (;;) {
        if (xQueueReceive(s_act, &a, pdMS_TO_TICKS(1000)) != pdTRUE) {
            // 启动超时:协议栈一直没同步 / 没开始广播,不能让界面永远停在"正在启动"。
            if (s_running && s_state == BLE_STARTING && sys_now_ms() - s_start_ms > START_TIMEOUT_MS) {
                fail("启动超时,协议栈未就绪");
            }
            continue;
        }
        if (a.type == ACT_START) {
            do_start();
            continue;
        }
        if (a.type == ACT_STOP) {
            do_stop();
            s_stop_pending = false;
            // 蓝牙关了且没有新请求:hid 任务自己退出,还回 4KB 栈;下次进遥控器再建。
            bool quit = false;
            taskENTER_CRITICAL(&s_worker_mux);
            if (uxQueueMessagesWaiting(s_act) == 0) {
                s_worker = NULL;
                quit = true;
            }
            taskEXIT_CRITICAL(&s_worker_mux);
            if (quit) vTaskDelete(NULL);
            continue;
        }
        if (s_state != BLE_CONNECTED || !s_dev) {
            free(a.text);
            continue;
        }
        switch (a.type) {
        case ACT_KEY: send_key(a.key, a.mod); break;
        case ACT_HOLD: {
            // mod = 修饰键位(0 表示 Fn),key = 1 按下 / 0 松开
            if (a.mod) {
                s_held_mod = a.key ? a.mod : 0;
                uint8_t r[8] = {s_held_mod, 0, 0, 0, 0, 0, 0, 0};
                esp_hidd_dev_input_set(s_dev, 0, RPT_KEYBOARD, r, sizeof(r));
            } else if (s_has_fn) {
                uint8_t fn = a.key ? 1 : 0;
                esp_hidd_dev_input_set(s_dev, 0, RPT_FN, &fn, 1);
            }
            break;
        }
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
static bool do_start(void)
{
    if (s_running) return true;
    s_state = BLE_STARTING;
    s_start_ms = sys_now_ms();
    s_error[0] = '\0';
    // 没人用 Wi-Fi 时先让它关掉(开机对时),最多等 2 秒,给蓝牙腾内存。
    net_yield_radio();
    for (int i = 0; i < 20 && net_radio_on(); i++) vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "蓝牙启动前:空闲 %u,最大块 %u,Wi-Fi %s", (unsigned)sys_free_heap(),
             (unsigned)sys_largest_block(), net_radio_on() ? "开" : "关");
    lv_mem_reserve_suspend(true);   // 把界面的应急储备让给蓝牙初始化
    const esp_err_t perr = nimble_port_init();
    if (perr != ESP_OK) {
        lv_mem_reserve_suspend(false);
        fail(perr == ESP_ERR_NO_MEM ? "初始化失败:内存不足" : "协议栈初始化失败");
        return false;
    }
    snprintf(s_name, sizeof(s_name), "%s", g_settings.name);
    memset(&s_sec, 0, sizeof(s_sec));

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

    // 先试带 Fn 的报告表,失败再退回基础表(都在 hid 任务里,不卡界面)。
    static esp_hid_raw_report_map_t maps[1];
    maps[0].data = k_report_map_fn;
    maps[0].len = sizeof(k_report_map_fn);
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
    esp_err_t herr = esp_hidd_dev_init(&cfg, ESP_HID_TRANSPORT_BLE, hidd_event, &s_dev);
    s_has_fn = herr == ESP_OK;
    if (herr != ESP_OK) {
        ESP_LOGW(TAG, "带 Fn 的报告表初始化失败(%s),改用基础报告表", esp_err_to_name(herr));
        maps[0].data = k_report_map;
        maps[0].len = sizeof(k_report_map);
        s_dev = NULL;
        herr = esp_hidd_dev_init(&cfg, ESP_HID_TRANSPORT_BLE, hidd_event, &s_dev);
    }
    if (herr != ESP_OK) {
        s_dev = NULL;
        nimble_port_deinit();
        lv_mem_reserve_suspend(false);
        fail(herr == ESP_ERR_NO_MEM ? "键盘服务失败:内存不足" : "键盘服务初始化失败");
        return false;
    }
    // esp_hidd_dev_init 内部会调 ble_svc_gap_init(),把设备名重置成默认的 "nimble":
    // 必须在它之后、开始广播之前设置,否则 Mac 读到的名字和广播里的不一致。
    ble_svc_gap_device_name_set(s_name);
    ble_svc_gap_device_appearance_set(ESP_HID_APPEARANCE_KEYBOARD);
    ble_svc_dis_model_number_set("Passport");   // 默认是协议栈名 "NimBLE"
    ble_store_config_init();
    s_running = true;
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "蓝牙已初始化,等待协议栈同步:空闲 %u,最大块 %u", (unsigned)sys_free_heap(),
             (unsigned)sys_largest_block());
    return true;
}

static void do_stop(void)
{
    s_held_mod = 0;
    if (!s_running) return;
    s_running = false;
    ble_gap_adv_stop();
    if (s_dev) {
        esp_hidd_dev_deinit(s_dev);
        s_dev = NULL;
    }
    const int rc = nimble_port_stop();
    if (rc == 0) nimble_port_deinit();
    else ESP_LOGE(TAG, "蓝牙停止失败 %d,内存未释放", rc);
    s_state = BLE_OFF;
    lv_mem_reserve_suspend(false);
    ESP_LOGI(TAG, "蓝牙已关闭:空闲 %u", (unsigned)sys_free_heap());
}

// 启停都交给 hid 任务异步执行:界面线程不等待 NimBLE 初始化 / 关闭。
bool ble_hid_start(void)
{
    if (!s_act) s_act = xQueueCreate(16, sizeof(act_t));
    if (!s_act) {
        fail("内存不足");
        return false;
    }
    if (s_running && !s_stop_pending) return true;
    s_state = BLE_STARTING;
    s_start_ms = sys_now_ms();
    act_t a = {.type = ACT_START};
    if (xQueueSend(s_act, &a, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    // 先入队再看任务在不在:任务只会在队列为空时退出,所以这里不会漏掉启动请求。
    taskENTER_CRITICAL(&s_worker_mux);
    const bool need = s_worker == NULL;
    taskEXIT_CRITICAL(&s_worker_mux);
    // 句柄在任务开始运行前就写好,不会和任务自己清空句柄冲突。
    if (need && xTaskCreate(worker, "hid", 4096, NULL, 4, (TaskHandle_t *)&s_worker) != pdPASS) {
        s_worker = NULL;
        fail("内存不足");
        return false;
    }
    return true;
}

void ble_hid_stop(void)
{
    // 没有 hid 任务就没有在运行的蓝牙;这时入队的停止请求没人处理,会让 Wi-Fi 一直等。
    if (!s_act || !s_worker) return;
    act_t a = {.type = ACT_STOP};
    s_stop_pending = true;
    if (xQueueSend(s_act, &a, pdMS_TO_TICKS(100)) != pdTRUE) s_stop_pending = false;
}

// 按住 / 松开一个修饰键(语音键):modifier 为 HID 修饰位,0 表示 Apple Fn。
void ble_hid_hold(uint8_t modifier, bool down)
{
    act_t a = {.type = ACT_HOLD, .mod = modifier, .key = down ? 1 : 0};
    enqueue(&a);
}

bool ble_hid_has_fn(void) { return s_has_fn; }

void ble_hid_forget(void)
{
    ble_store_clear();
}
