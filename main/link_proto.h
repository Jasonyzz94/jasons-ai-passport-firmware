// main/link_proto.h —— 设备互联(ESP-NOW)报文格式。纯逻辑,主机测试覆盖。
//
// 每个报文:'P' 'L' 版本 类型 | 序号(uint16 LE) | 负载
//   HELLO     负载 = 设备名(UTF-8,≤23 字节)          每秒广播一次
//   PAIR_REQ  负载 = 设备名                             请求配对
//   PAIR_ACK  负载 = 设备名                             同意配对
//   TEXT      负载 = UTF-8 文本(≤60 字节)
//   PING      无负载
//   FOCUS     负载 = 1 字节分钟数
//   VOICE     负载 = 一个 IMA ADPCM 块(audio_dsp.h)
//   VOICE_END 无负载
//   UNPAIR    无负载
// 长度不符、魔数/版本不对、文本截断在多字节字符中间的报文一律丢弃。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LINK_VERSION 1
#define LINK_HDR 6
#define LINK_NAME_MAX 23
#define LINK_TEXT_MAX 60
#define LINK_PAYLOAD_MAX 200
#define LINK_PACKET_MAX (LINK_HDR + LINK_PAYLOAD_MAX)

typedef enum {
    LINK_HELLO = 1,
    LINK_PAIR_REQ,
    LINK_PAIR_ACK,
    LINK_TEXT,
    LINK_PING,
    LINK_FOCUS,
    LINK_VOICE,
    LINK_VOICE_END,
    LINK_UNPAIR,
} link_type_t;

typedef struct {
    link_type_t type;
    uint16_t seq;
    uint8_t payload[LINK_PAYLOAD_MAX];
    size_t len;
} link_msg_t;

// 打包,返回报文长度;负载不合法时返回 0。
size_t link_pack(link_type_t type, uint16_t seq, const void *payload, size_t len,
                 uint8_t out[LINK_PACKET_MAX]);
// 解析并校验;合法返回 true。文本类负载在 msg->payload 里以 0 结尾。
bool link_parse(const uint8_t *buf, size_t len, link_msg_t *msg);
// 文本按 UTF-8 字符边界截断到 max 字节以内,返回新长度。
size_t link_utf8_clip(const char *s, size_t max);
