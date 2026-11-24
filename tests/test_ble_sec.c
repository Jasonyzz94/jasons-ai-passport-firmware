// tests/test_ble_sec.c —— 蓝牙键盘加密 / 配对决策测试(只验证决策,不代表真机配对通过)。
#include "ble_sec.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    ble_sec_t s;

    // 新连接未加密:外设主动发起一次安全请求;加密成功后才可发送按键。
    assert(ble_sec_on_connect(&s, false) == BLE_SEC_INITIATE);
    assert(!ble_sec_ready(&s));
    ble_sec_on_initiate_result(&s, true);
    assert(ble_sec_on_enc_change(&s, true, false) == BLE_SEC_NONE);
    assert(ble_sec_ready(&s));

    // 对端用旧绑定直接加密:不再发起。
    assert(ble_sec_on_connect(&s, true) == BLE_SEC_NONE);
    assert(ble_sec_ready(&s));

    // 对端删了配对、本机还留着旧绑定:删旧绑定重试一次,再失败就断开。
    assert(ble_sec_on_connect(&s, false) == BLE_SEC_INITIATE);
    assert(ble_sec_on_enc_change(&s, false, true) == BLE_SEC_FORGET_RETRY);
    assert(!ble_sec_ready(&s));
    assert(ble_sec_on_enc_change(&s, false, true) == BLE_SEC_DISCONNECT);

    // 其他配对失败:直接断开,回到广播。
    assert(ble_sec_on_connect(&s, false) == BLE_SEC_INITIATE);
    assert(ble_sec_on_enc_change(&s, false, false) == BLE_SEC_DISCONNECT);

    // 发起失败不断开,对端访问加密特征时仍会配对。
    assert(ble_sec_on_connect(&s, false) == BLE_SEC_INITIATE);
    ble_sec_on_initiate_result(&s, false);
    assert(ble_sec_on_enc_change(&s, true, false) == BLE_SEC_NONE);
    assert(ble_sec_ready(&s));

    // 加密后的链路重新加密失败(例如对端换了密钥):同样按规则处理。
    assert(ble_sec_on_enc_change(&s, false, false) == BLE_SEC_DISCONNECT);
    assert(!ble_sec_ready(&s));

    // 断开后清空;断开后迟到的加密事件不触发动作。
    ble_sec_on_disconnect(&s);
    assert(!ble_sec_ready(&s));
    assert(ble_sec_on_enc_change(&s, false, true) == BLE_SEC_NONE);
    assert(ble_sec_on_enc_change(&s, true, false) == BLE_SEC_NONE);
    assert(!ble_sec_ready(&s));

    // 每次新连接都重新允许一次删旧绑定重试。
    assert(ble_sec_on_connect(&s, false) == BLE_SEC_INITIATE);
    assert(ble_sec_on_enc_change(&s, false, true) == BLE_SEC_FORGET_RETRY);

    printf("ble_sec: ok\n");
    return 0;
}
