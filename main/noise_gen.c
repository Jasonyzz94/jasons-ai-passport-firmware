// main/noise_gen.c —— 环境声合成实现(纯整数)。
#include "noise_gen.h"

static inline uint32_t xorshift(noise_state_t *st)
{
    uint32_t x = st->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    st->rng = x;
    return x;
}

// 均匀分布的 ±4096 白噪声
static inline int32_t white(noise_state_t *st) { return (int32_t)(xorshift(st) >> 19) - 4096; }

static inline int16_t clamp16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }

// 0..256 的平滑周期包络(由相位的高 16 位做三角波再平方,接近余弦形状)
static inline int32_t swell(uint32_t phase)
{
    int32_t tri = (int32_t)((phase >> 15) & 0x1FFFF);       // 0..131071
    if (tri > 65535) tri = 131071 - tri;                     // 0..65535..0
    const int32_t t = tri >> 8;                              // 0..255
    return (t * t) >> 8;                                     // 0..254,两端更平缓
}

void noise_init(noise_state_t *st, uint32_t seed)
{
    for (unsigned i = 0; i < sizeof(*st); i++) ((uint8_t *)st)[i] = 0;
    st->rng = seed ? seed : 0x9E3779B9u;
}

static int32_t pink(noise_state_t *st)
{
    // Voss-McCartney:每个样本只换一行,行号 = 计数器最低位 1 的位置
    st->counter++;
    uint32_t c = st->counter;
    int k = 0;
    while (!(c & 1) && k < 7) {
        c >>= 1;
        k++;
    }
    const int32_t v = white(st);
    st->pink_sum += v - st->rows[k];
    st->rows[k] = v;
    return (st->pink_sum + white(st)) / 3;                   // 约 ±12000
}

static int32_t brown(noise_state_t *st)
{
    st->brown += white(st) >> 1;
    st->brown -= st->brown >> 6;                             // 漏积分,防止漂移
    if (st->brown > 24000) st->brown = 24000;
    if (st->brown < -24000) st->brown = -24000;
    return st->brown;
}

void noise_fill(noise_state_t *st, noise_type_t type, int16_t *out, int n, int32_t target_q8)
{
    for (int i = 0; i < n; i++) {
        // 增益每 16 个样本向目标靠近 1,约 0.25 秒完成渐入渐出
        if ((i & 15) == 0 && st->gain_q8 != target_q8) st->gain_q8 += st->gain_q8 < target_q8 ? 1 : -1;
        int32_t s;
        switch (type) {
        case NOISE_WHITE: s = white(st) * 2; break;
        case NOISE_PINK: s = pink(st); break;
        case NOISE_BROWN: s = brown(st); break;
        case NOISE_RAIN: {
            // 柔和的粉红底噪 + 随机雨滴(快速衰减的高频脉冲)
            const int32_t bed = pink(st) >> 1;
            if ((xorshift(st) & 0x3FF) < 3) st->drop = (int32_t)(xorshift(st) >> 18) + 6000;
            const int32_t d = (white(st) * st->drop) >> 12;
            st->drop -= st->drop >> 7;
            s = bed + d;
            break;
        }
        case NOISE_WAVES: {
            // 棕噪音 × 约 9 秒一次的涨落
            st->phase += 29826;                              // 2^32 / (29826 * 16000) ≈ 9 秒一个周期
            const int32_t env = 40 + ((swell(st->phase) * 216) >> 8);
            s = (brown(st) * env) >> 8;
            break;
        }
        case NOISE_FAN: {
            // 低通白噪声 + 轻微的转动起伏(约 0.5 秒一次)
            st->lp += (white(st) * 2 - st->lp) >> 3;
            st->phase += 536871;                             // 约 0.5 秒一个周期
            const int32_t env = 200 + (swell(st->phase) >> 3);
            s = (st->lp * 3 * env) >> 8;
            break;
        }
        default: s = 0; break;
        }
        out[i] = clamp16((s * st->gain_q8) >> 8);
    }
}

const char *noise_name(noise_type_t type)
{
    static const char *const names[NOISE_COUNT] = {"关闭", "白噪音", "粉红噪音", "棕噪音", "雨声", "海浪", "风扇"};
    return type < NOISE_COUNT ? names[type] : "";
}
