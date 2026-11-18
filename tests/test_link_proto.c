// tests/test_link_proto.c —— 设备互联报文测试:往返、边界、畸形报文、UTF-8 截断。
#include "audio_dsp.h"
#include "link_proto.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    uint8_t buf[LINK_PACKET_MAX];
    link_msg_t m;

    // 文本往返(中文)
    const char *hi = "晚上一起吃饭吗?";
    size_t n = link_pack(LINK_TEXT, 513, hi, strlen(hi), buf);
    assert(n == LINK_HDR + strlen(hi));
    assert(link_parse(buf, n, &m));
    assert(m.type == LINK_TEXT && m.seq == 513 && !strcmp((char *)m.payload, hi));

    // 无负载
    n = link_pack(LINK_PING, 1, NULL, 0, buf);
    assert(n == LINK_HDR && link_parse(buf, n, &m) && m.type == LINK_PING && m.len == 0);

    // 专注分钟数必须正好 1 字节
    uint8_t mins = 25;
    n = link_pack(LINK_FOCUS, 2, &mins, 1, buf);
    assert(link_parse(buf, n, &m) && m.payload[0] == 25);
    assert(link_pack(LINK_FOCUS, 2, "ab", 2, buf) == 0);

    // 语音块
    uint8_t blk[ADPCM_BLOCK_BYTES];
    memset(blk, 0x5A, sizeof(blk));
    n = link_pack(LINK_VOICE, 7, blk, sizeof(blk), buf);
    assert(n == LINK_HDR + ADPCM_BLOCK_BYTES && n <= 250);   // ESP-NOW v1 单包上限
    assert(link_parse(buf, n, &m) && m.len == ADPCM_BLOCK_BYTES);

    // 过长 / 畸形
    char longtxt[100];
    memset(longtxt, 'a', sizeof(longtxt));
    assert(link_pack(LINK_TEXT, 0, longtxt, 61, buf) == 0);
    assert(link_pack(LINK_HELLO, 0, longtxt, 24, buf) == 0);
    n = link_pack(LINK_TEXT, 0, "ok", 2, buf);
    buf[0] = 'X';
    assert(!link_parse(buf, n, &m));
    buf[0] = 'P';
    buf[2] = 9;                                   // 版本不对
    assert(!link_parse(buf, n, &m));
    buf[2] = LINK_VERSION;
    buf[3] = 99;                                  // 未知类型
    assert(!link_parse(buf, n, &m));
    assert(!link_parse(buf, 3, &m));              // 太短
    // 截断在多字节字符中间的文本被拒绝
    const char *zh = "你好";
    n = link_pack(LINK_TEXT, 0, zh, strlen(zh), buf);
    assert(!link_parse(buf, n - 1, &m));
    assert(link_pack(LINK_TEXT, 0, zh, strlen(zh) - 1, buf) == 0);

    // UTF-8 安全截断
    assert(link_utf8_clip("abc", 10) == 3);
    assert(link_utf8_clip("你好世界", 7) == 6);   // 每字 3 字节,不能切开第 3 个字
    assert(link_utf8_clip("你好世界", 6) == 6);
    assert(link_utf8_clip("a你", 2) == 1);
    puts("link proto tests: PASS");
    return 0;
}
