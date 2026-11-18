// main/link_proto.c —— 设备互联报文打包 / 解析。
#include "link_proto.h"

#include "audio_dsp.h"
#include <string.h>

size_t link_utf8_clip(const char *s, size_t max)
{
    size_t n = strlen(s);
    if (n <= max) return n;
    n = max;
    // 回退到字符开头(10xxxxxx 是续字节)
    while (n > 0 && ((uint8_t)s[n] & 0xC0) == 0x80) n--;
    return n;
}

static bool utf8_valid(const uint8_t *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        const uint8_t c = s[i];
        size_t extra;
        if (c == 0) return false;
        if (c < 0x80) extra = 0;
        else if ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        else return false;
        if (i + extra >= n) return false;   // 截断的多字节字符
        for (size_t k = 1; k <= extra; k++) {
            if ((s[i + k] & 0xC0) != 0x80) return false;
        }
        i += extra + 1;
    }
    return true;
}

static size_t max_payload(link_type_t t)
{
    switch (t) {
    case LINK_HELLO:
    case LINK_PAIR_REQ:
    case LINK_PAIR_ACK: return LINK_NAME_MAX;
    case LINK_TEXT: return LINK_TEXT_MAX;
    case LINK_FOCUS: return 1;
    case LINK_VOICE: return ADPCM_BLOCK_BYTES;
    default: return 0;
    }
}

static bool is_text(link_type_t t)
{
    return t == LINK_HELLO || t == LINK_PAIR_REQ || t == LINK_PAIR_ACK || t == LINK_TEXT;
}

size_t link_pack(link_type_t type, uint16_t seq, const void *payload, size_t len,
                 uint8_t out[LINK_PACKET_MAX])
{
    if (len > max_payload(type)) return 0;
    if (type == LINK_FOCUS && len != 1) return 0;
    if (is_text(type) && len && !utf8_valid(payload, len)) return 0;
    out[0] = 'P';
    out[1] = 'L';
    out[2] = LINK_VERSION;
    out[3] = (uint8_t)type;
    out[4] = (uint8_t)(seq & 0xFF);
    out[5] = (uint8_t)(seq >> 8);
    if (len) memcpy(out + LINK_HDR, payload, len);
    return LINK_HDR + len;
}

bool link_parse(const uint8_t *buf, size_t len, link_msg_t *msg)
{
    if (len < LINK_HDR || buf[0] != 'P' || buf[1] != 'L' || buf[2] != LINK_VERSION) return false;
    const link_type_t t = (link_type_t)buf[3];
    if (t < LINK_HELLO || t > LINK_UNPAIR) return false;
    const size_t n = len - LINK_HDR;
    if (n > max_payload(t)) return false;
    if (t == LINK_FOCUS && n != 1) return false;
    if (t == LINK_VOICE && n < 4) return false;
    if (is_text(t) && n && !utf8_valid(buf + LINK_HDR, n)) return false;
    msg->type = t;
    msg->seq = (uint16_t)(buf[4] | (buf[5] << 8));
    msg->len = n;
    memcpy(msg->payload, buf + LINK_HDR, n);
    if (n < LINK_PAYLOAD_MAX) msg->payload[n] = 0;
    return true;
}
