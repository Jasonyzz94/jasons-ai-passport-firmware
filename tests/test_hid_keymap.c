// tests/test_hid_keymap.c —— ASCII → HID 键码映射测试(快捷指令要原样键入)。
#include "hid_keymap.h"

#include <assert.h>
#include <stdio.h>

static void expect(char c, uint8_t code, uint8_t mod)
{
    uint8_t k = 0, m = 0;
    assert(hid_ascii_to_key(c, &k, &m));
    if (k != code || m != mod) {
        printf("'%c': got 0x%02X/%u want 0x%02X/%u\n", c, k, m, code, mod);
        assert(0);
    }
}

int main(void)
{
    expect('a', 0x04, 0);
    expect('z', 0x1D, 0);
    expect('A', 0x04, HID_MOD_LSHIFT);
    expect('1', 0x1E, 0);
    expect('9', 0x26, 0);
    expect('0', 0x27, 0);
    expect(' ', 0x2C, 0);
    expect('/', 0x38, 0);
    expect('?', 0x38, HID_MOD_LSHIFT);
    expect(',', 0x36, 0);
    expect('!', 0x1E, HID_MOD_LSHIFT);
    expect(')', 0x27, HID_MOD_LSHIFT);
    expect('(', 0x26, HID_MOD_LSHIFT);
    expect('_', 0x2D, HID_MOD_LSHIFT);
    expect('"', 0x34, HID_MOD_LSHIFT);
    expect('\'', 0x34, 0);
    expect('\n', 0x28, 0);
    // 快捷指令里出现的每个字符都必须能键入
    const char *phrases[] = {"continue", "run the tests and fix any failures", "looks good, commit it",
                             "/clear", "/compact", "undo your last change", "explain what you just changed"};
    for (unsigned i = 0; i < sizeof(phrases) / sizeof(phrases[0]); i++) {
        for (const char *p = phrases[i]; *p; p++) {
            uint8_t k, m;
            assert(hid_ascii_to_key(*p, &k, &m));
        }
    }
    uint8_t k, m;
    assert(!hid_ascii_to_key('\0', &k, &m));
    assert(!hid_ascii_to_key((char)0xE4, &k, &m));   // 中文 UTF-8 字节不可键入
    assert(!hid_ascii_to_key('\x01', &k, &m));
    puts("hid keymap tests: PASS");
    return 0;
}
