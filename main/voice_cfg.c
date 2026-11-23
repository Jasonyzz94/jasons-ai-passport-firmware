// main/voice_cfg.c —— 见 voice_cfg.h。
#include "voice_cfg.h"

#include <stdio.h>
#include <string.h>

bool voice_use_direct(int mode, bool relay_ok, bool has_key)
{
    if (mode == VOICE_MODE_DIRECT) return true;
    if (mode == VOICE_MODE_RELAY) return false;
    return !relay_ok && has_key;
}

bool voice_ready(int mode, bool relay_ok, bool has_key, bool net_ok)
{
    if (!net_ok) return false;
    if (mode == VOICE_MODE_DIRECT) return has_key;
    if (mode == VOICE_MODE_RELAY) return relay_ok;
    return relay_ok || has_key;
}

// 比较两个接口地址:忽略末尾的 '/'。
static bool same_base(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    while (la && a[la - 1] == '/') la--;
    while (lb && b[lb - 1] == '/') lb--;
    return la == lb && strncmp(a, b, la) == 0;
}

void voice_resolve(const char *api_base, const char *api_key, const char *voice_base,
                   const char *voice_key, const char **base, const char **key)
{
    const bool own_base = voice_base[0] != '\0';
    *base = own_base ? voice_base : api_base;
    if (voice_key[0]) *key = voice_key;
    else if (!own_base || same_base(voice_base, api_base)) *key = api_key;
    else *key = "";
}

const char *voice_reason(int status)
{
    switch (status) {
    case VOICE_ERR_NET: return "网络不通";
    case VOICE_ERR_MEM: return "内存不足";
    case VOICE_ERR_PARSE: return "回复异常";
    case VOICE_ERR_EMPTY: return "没听清";
    case VOICE_ERR_UPLOAD: return "上传中断";
    case VOICE_ERR_INSECURE: return "公网接口须用https";
    case VOICE_ERR_CUT: return "音频中途断了";
    case 401:
    case 403: return "密钥无效";
    case 402: return "余额不足";
    case 404: return "接口或模型不存在";
    case 408:
    case 504: return "超时";
    case 413: return "录音太长";
    case 429: return "太频繁或额度用完";
    default: break;
    }
    if (status >= 500) return "服务商出错";
    if (status >= 400) return "请求被拒";
    return "失败";
}

void voice_error_text(char *out, size_t cap, const char *stage, int status)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "%s:%s", stage, voice_reason(status));
    utf8_copy(out, cap, buf);
}

static uint8_t *put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    return p + 2;
}
static uint8_t *put32(uint8_t *p, uint32_t v) { return put16(put16(p, v & 0xFFFF), v >> 16); }
static uint8_t *put_tag(uint8_t *p, const char *t)
{
    memcpy(p, t, 4);
    return p + 4;
}

void voice_ima_wav_header(uint8_t h[VOICE_WAV_HEADER_BYTES], uint32_t blocks)
{
    enum { BLOCK_BYTES = 132, SAMPLES_PER_BLOCK = 257 };   // WAV 里块头的预测值也算一个样本
    const uint32_t data = blocks * BLOCK_BYTES;
    uint8_t *p = h;
    p = put_tag(p, "RIFF");
    p = put32(p, VOICE_WAV_HEADER_BYTES - 8 + data);
    p = put_tag(p, "WAVE");
    p = put_tag(p, "fmt ");
    p = put32(p, 20);
    p = put16(p, 0x11);                                   // IMA ADPCM
    p = put16(p, 1);                                      // 单声道
    p = put32(p, 16000);
    p = put32(p, 16000 * BLOCK_BYTES / SAMPLES_PER_BLOCK); // 平均字节率
    p = put16(p, BLOCK_BYTES);
    p = put16(p, 4);                                      // 每样本 4 位
    p = put16(p, 2);                                      // 扩展字段长度
    p = put16(p, SAMPLES_PER_BLOCK);
    p = put_tag(p, "fact");
    p = put32(p, 4);
    p = put32(p, blocks * SAMPLES_PER_BLOCK);
    p = put_tag(p, "data");
    put32(p, data);
}

static bool prefix(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

bool voice_url_is_lan(const char *url)
{
    const char *h = prefix(url, "https://") ? url + 8 : prefix(url, "http://") ? url + 7 : NULL;
    if (!h) return false;
    char host[64];
    const size_t n = strcspn(h, ":/?#");
    if (n == 0 || n >= sizeof(host)) return false;
    memcpy(host, h, n);
    host[n] = '\0';
    for (char *c = host; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
    if (!strcmp(host, "localhost")) return true;
    if (n > 6 && !strcmp(host + n - 6, ".local")) return true;   // mDNS 局域网主机名
    unsigned a, b, c, d;
    char tail;
    if (sscanf(host, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
        return false;
    return a == 10 || a == 127 || (a == 192 && b == 168) || (a == 172 && b >= 16 && b <= 31)
        || (a == 169 && b == 254);
}

bool voice_key_transport_ok(const char *url, const char *key)
{
    if (!key[0] || prefix(url, "https://")) return true;
    return prefix(url, "http://") && voice_url_is_lan(url);
}

size_t utf8_copy(char *dst, size_t cap, const char *src)
{
    if (!cap) return 0;
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        // 退回到一个字符的开头(跳过 10xxxxxx 续字节),再丢掉这个不完整的字符。
        size_t start = n;
        while (start > 0 && ((unsigned char)src[start] & 0xC0) == 0x80) start--;
        const unsigned char lead = (unsigned char)src[start];
        const size_t need = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3
                          : (lead >> 3) == 0x1E ? 4 : 1;
        if (start + need > n) n = start;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
    return n;
}

size_t voice_trim_answer(char *dst, size_t cap, const char *src)
{
    static const char ell[] = "…";   // 3 字节
    if (strlen(src) < cap) return utf8_copy(dst, cap, src);
    // 留出省略号的位置,先按字符截
    size_t n = utf8_copy(dst, cap - (sizeof(ell) - 1), src);
    // 往回找句末标点(。!?;及换行),只在后半段找,免得砍掉太多
    static const char *const ends[] = {"。", "！", "？", "；", "!", "?", ";", "\n"};
    size_t best = 0;
    for (size_t i = 0; i < sizeof(ends) / sizeof(ends[0]); i++) {
        const size_t el = strlen(ends[i]);
        for (size_t k = n; k >= el && k > n / 2; k--) {
            if (!memcmp(dst + k - el, ends[i], el)) {
                if (k > best) best = k;
                break;
            }
        }
    }
    if (best) {
        n = best;   // 停在完整的一句话后面
    }
    memcpy(dst + n, ell, sizeof(ell));   // 带上结尾的 0
    return n + sizeof(ell) - 1;
}

size_t resample24_to16(resample24_t *st, const int16_t *in, size_t n, int16_t *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (st->n < 2) {
            st->carry[st->n++] = in[i];
            continue;
        }
        // 凑满 3 个:a b c → (2a+b)/3, (b+2c)/3(位置 0 和 1.5,顺带平滑)
        const int a = st->carry[0], b = st->carry[1], c = in[i];
        out[o++] = (int16_t)((2 * a + b) / 3);
        out[o++] = (int16_t)((b + 2 * c) / 3);
        st->n = 0;
    }
    return o;
}
