// main/hid_keymap.h —— ASCII 字符 → USB HID 键码(美式键盘布局),纯逻辑,主机测试覆盖。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define HID_MOD_LSHIFT 0x02

// 成功返回 true 并给出键码与修饰键;不能键入的字符(非 ASCII、控制字符)返回 false。
bool hid_ascii_to_key(char c, uint8_t *keycode, uint8_t *modifiers);
