// tests/test_voice_cfg.c —— 语音配置纯逻辑的主机测试。
#include "voice_cfg.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_mode(void)
{
    // 自动:中转优先,其次云端
    assert(!voice_use_direct(VOICE_MODE_AUTO, true, true));
    assert(voice_use_direct(VOICE_MODE_AUTO, false, true));
    assert(!voice_use_direct(VOICE_MODE_AUTO, false, false));
    // 固定模式不受中转状态影响
    assert(voice_use_direct(VOICE_MODE_DIRECT, true, true));
    assert(!voice_use_direct(VOICE_MODE_RELAY, false, true));

    assert(!voice_ready(VOICE_MODE_AUTO, true, true, false));
    assert(voice_ready(VOICE_MODE_AUTO, false, true, true));
    assert(!voice_ready(VOICE_MODE_AUTO, false, false, true));
    assert(!voice_ready(VOICE_MODE_DIRECT, true, false, true));
    assert(!voice_ready(VOICE_MODE_RELAY, false, true, true));
    assert(voice_ready(VOICE_MODE_RELAY, true, false, true));
}

static void test_resolve(void)
{
    const char *base, *key;
    // 语音没单独配:沿用对话
    voice_resolve("https://api.siliconflow.cn/v1", "sk-a", "", "", &base, &key);
    assert(!strcmp(base, "https://api.siliconflow.cn/v1") && !strcmp(key, "sk-a"));
    // DeepSeek 对话 + 硅基流动语音:各用各的密钥
    voice_resolve("https://api.deepseek.com/v1", "sk-ds", "https://api.siliconflow.cn/v1", "sk-sf", &base, &key);
    assert(!strcmp(base, "https://api.siliconflow.cn/v1") && !strcmp(key, "sk-sf"));
    // 语音换了别家但没填密钥:不能把对话密钥发过去
    voice_resolve("https://api.deepseek.com/v1", "sk-ds", "https://api.siliconflow.cn/v1", "", &base, &key);
    assert(!strcmp(key, ""));
    // 同一家(只差末尾斜杠)可以沿用
    voice_resolve("https://x.com/v1", "sk-x", "https://x.com/v1/", "", &base, &key);
    assert(!strcmp(key, "sk-x"));
}

static void test_errors(void)
{
    char buf[64];
    voice_error_text(buf, sizeof(buf), "识别", 401);
    assert(!strcmp(buf, "识别:密钥无效"));
    voice_error_text(buf, sizeof(buf), "对话", 402);
    assert(!strcmp(buf, "对话:余额不足"));
    voice_error_text(buf, sizeof(buf), "朗读", 503);
    assert(!strcmp(buf, "朗读:服务商出错"));
    voice_error_text(buf, sizeof(buf), "对话", 400);
    assert(!strcmp(buf, "对话:请求被拒"));
    voice_error_text(buf, sizeof(buf), "识别", VOICE_ERR_NET);
    assert(!strcmp(buf, "识别:网络不通"));
    // 放不下时按字符截断
    char small[12];
    voice_error_text(small, sizeof(small), "识别", 401);
    assert(!strcmp(small, "识别:密"));
}

static void test_utf8(void)
{
    char buf[8];
    // "你好世界" 每字 3 字节:8 字节缓冲最多放 7 字节 → 只能放两个完整的字
    assert(utf8_copy(buf, sizeof(buf), "你好世界") == 6);
    assert(!strcmp(buf, "你好"));
    assert(utf8_copy(buf, sizeof(buf), "abc") == 3 && !strcmp(buf, "abc"));
    assert(utf8_copy(buf, sizeof(buf), "abcdefg") == 7 && !strcmp(buf, "abcdefg"));
    assert(utf8_copy(buf, sizeof(buf), "abcde你") == 5 && !strcmp(buf, "abcde"));
    assert(utf8_copy(buf, sizeof(buf), "abcd你") == 7 && !strcmp(buf, "abcd你"));
    // 4 字节字符(表情)
    assert(utf8_copy(buf, 6, "ab\xF0\x9F\x98\x80") == 2);
    assert(utf8_copy(buf, 1, "abc") == 0 && buf[0] == '\0');
    // 截断后的每个结果都是合法 UTF-8:不会以续字节或不完整的首字节结尾
    const char *s = "语音:密钥无效 ok 测试";
    for (size_t cap = 1; cap < strlen(s) + 2; cap++) {
        char out[64];
        const size_t n = utf8_copy(out, cap, s);
        assert(n < cap && strncmp(out, s, n) == 0);
        assert(n == strlen(s) || ((unsigned char)s[n] & 0xC0) != 0x80);
    }
}

static void test_resample(void)
{
    resample24_t st = {{0}, 0};
    int16_t in[300], out[220];
    for (int i = 0; i < 300; i++) in[i] = 1000;
    // 常量信号保持不变,长度变为 2/3
    size_t n = resample24_to16(&st, in, 300, out);
    assert(n == 200);
    for (size_t i = 0; i < n; i++) assert(out[i] == 1000);
    // 分块送入与一次送入结果相同(流式状态正确)
    for (int i = 0; i < 300; i++) in[i] = (int16_t)(i * 37 % 2000 - 1000);
    int16_t whole[220], parts[220];
    resample24_t a = {{0}, 0}, b = {{0}, 0};
    const size_t nw = resample24_to16(&a, in, 300, whole);
    size_t np = 0;
    const size_t cuts[] = {1, 2, 7, 50, 51, 120, 300};
    size_t from = 0;
    for (size_t c = 0; c < sizeof(cuts) / sizeof(cuts[0]); c++) {
        np += resample24_to16(&b, in + from, cuts[c] - from, parts + np);
        from = cuts[c];
    }
    assert(nw == np && memcmp(whole, parts, nw * sizeof(int16_t)) == 0);
    // 极值不溢出
    for (int i = 0; i < 3; i++) in[i] = 32767;
    resample24_t c = {{0}, 0};
    assert(resample24_to16(&c, in, 3, out) == 2 && out[0] == 32767 && out[1] == 32767);
}

int main(void)
{
    test_mode();
    test_resolve();
    test_errors();
    test_utf8();
    test_resample();
    puts("voice cfg tests: PASS");
    return 0;
}
