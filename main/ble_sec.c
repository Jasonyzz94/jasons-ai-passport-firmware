// main/ble_sec.c —— 蓝牙键盘加密 / 配对决策(见 ble_sec.h)。
#include "ble_sec.h"

#include <string.h>

ble_sec_action_t ble_sec_on_connect(ble_sec_t *s, bool link_encrypted)
{
    memset(s, 0, sizeof(*s));
    s->connected = true;
    s->encrypted = link_encrypted;
    if (link_encrypted) return BLE_SEC_NONE;
    s->initiated = true;
    return BLE_SEC_INITIATE;
}

void ble_sec_on_initiate_result(ble_sec_t *s, bool ok)
{
    // 失败时也不再重复发起:同一连接里反复请求只会让对端更混乱。
    (void)ok;
    s->initiated = true;
}

ble_sec_action_t ble_sec_on_enc_change(ble_sec_t *s, bool success, bool key_missing)
{
    if (!s->connected) return BLE_SEC_NONE;
    if (success) {
        s->encrypted = true;
        return BLE_SEC_NONE;
    }
    s->encrypted = false;
    if (key_missing && !s->retried) {
        s->retried = true;
        s->initiated = true;
        return BLE_SEC_FORGET_RETRY;
    }
    return BLE_SEC_DISCONNECT;
}

void ble_sec_on_disconnect(ble_sec_t *s)
{
    memset(s, 0, sizeof(*s));
}

bool ble_sec_ready(const ble_sec_t *s)
{
    return s->connected && s->encrypted;
}
