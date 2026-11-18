// main/hid_keymap.c —— ASCII → HID 键码(美式布局)。
#include "hid_keymap.h"

#include <string.h>

// 不按 Shift 时的符号键与按 Shift 时的符号键,按同一顺序对应同一键码。
static const char k_plain[] = "-=[]\\;'`,./";
static const char k_shift[] = "_+{}|:\"~<>?";
static const uint8_t k_codes[] = {0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38};
static const char k_shift_digits[] = ")!@#$%^&*(";   // Shift + 0..9

bool hid_ascii_to_key(char c, uint8_t *keycode, uint8_t *modifiers)
{
    *modifiers = 0;
    if (c >= 'a' && c <= 'z') {
        *keycode = (uint8_t)(0x04 + (c - 'a'));
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        *keycode = (uint8_t)(0x04 + (c - 'A'));
        *modifiers = HID_MOD_LSHIFT;
        return true;
    }
    if (c >= '1' && c <= '9') {
        *keycode = (uint8_t)(0x1E + (c - '1'));
        return true;
    }
    if (c == '0') {
        *keycode = 0x27;
        return true;
    }
    switch (c) {
    case ' ': *keycode = 0x2C; return true;
    case '\n': *keycode = 0x28; return true;
    case '\t': *keycode = 0x2B; return true;
    default: break;
    }
    const char *p;
    if (c && (p = strchr(k_plain, c)) != NULL) {
        *keycode = k_codes[p - k_plain];
        return true;
    }
    if (c && (p = strchr(k_shift, c)) != NULL) {
        *keycode = k_codes[p - k_shift];
        *modifiers = HID_MOD_LSHIFT;
        return true;
    }
    if (c && (p = strchr(k_shift_digits, c)) != NULL) {
        const int d = (int)(p - k_shift_digits);
        *keycode = d == 0 ? 0x27 : (uint8_t)(0x1E + d - 1);
        *modifiers = HID_MOD_LSHIFT;
        return true;
    }
    return false;
}
