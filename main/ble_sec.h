// main/ble_sec.h —— 蓝牙键盘连接后的加密 / 配对决策,纯逻辑,主机测试覆盖。
//
// HID 键盘必须在加密、已绑定的链路上工作:只完成 GATT 发现、能收到通知,不代表已配对。
// macOS 会等外设提出加密要求,不能指望对端主动发起配对,所以连接建立后由外设发起一次安全请求;
// 失败时按结果决定:对端丢了旧密钥 → 删掉本地旧绑定重试一次;其他失败 → 断开,让对端重新连接,
// 避免系统设置一直停在"正在连接"。
#pragma once

#include <stdbool.h>

typedef enum {
    BLE_SEC_NONE = 0,       // 什么都不用做
    BLE_SEC_INITIATE,       // 发起安全请求(ble_gap_security_initiate)
    BLE_SEC_FORGET_RETRY,   // 删除该对端的旧绑定,再发起一次
    BLE_SEC_DISCONNECT,     // 配对失败:断开连接,回到广播
} ble_sec_action_t;

typedef struct {
    bool connected;
    bool encrypted;
    bool initiated;   // 本次连接已发起过安全请求(或对端已在配对中)
    bool retried;     // 本次连接已做过一次"删旧绑定重试"
} ble_sec_t;

// 新连接建立。link_encrypted:连接描述里链路是否已经加密(对端用旧绑定直接加密时为真)。
ble_sec_action_t ble_sec_on_connect(ble_sec_t *s, bool link_encrypted);

// 发起安全请求的返回值:ok 为真表示请求已发出或对端已在配对中(BLE_HS_EALREADY)。
// 发起失败不断开:HID 特征要求加密,对端访问时仍会被要求配对。
void ble_sec_on_initiate_result(ble_sec_t *s, bool ok);

// 加密状态变化(BLE_GAP_EVENT_ENC_CHANGE)。
// success:status == 0;key_missing:对端没有这把密钥(对端删了配对而本机还留着旧绑定)。
ble_sec_action_t ble_sec_on_enc_change(ble_sec_t *s, bool success, bool key_missing);

// 连接断开:清空本次连接的状态。
void ble_sec_on_disconnect(ble_sec_t *s);

// 链路是否已加密(可以发送按键)。
bool ble_sec_ready(const ble_sec_t *s);
