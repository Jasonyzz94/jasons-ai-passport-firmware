// tests/test_noise_gen.c —— 环境声合成:非静音、不溢出、可复现、频谱形状、渐入。
#include "noise_gen.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define N 32000

static int16_t buf[N];

static void gen(noise_type_t t, uint32_t seed)
{
    noise_state_t st;
    noise_init(&st, seed);
    st.gain_q8 = 256;                       // 跳过渐入,直接看稳态
    noise_fill(&st, t, buf, N, 256);
}

static double rms(void)
{
    double acc = 0;
    for (int i = 0; i < N; i++) acc += (double)buf[i] * buf[i];
    return acc / N;
}

// 高频比例:一阶差分能量 / 信号能量。白噪音约 2,越"低沉"越小。
static double hf_ratio(void)
{
    double d = 0, e = 0;
    for (int i = 1; i < N; i++) {
        const double x = buf[i] - buf[i - 1];
        d += x * x;
        e += (double)buf[i] * buf[i];
    }
    return d / (e + 1);
}

int main(void)
{
    double ratio[NOISE_COUNT];
    for (int t = NOISE_WHITE; t < NOISE_COUNT; t++) {
        gen((noise_type_t)t, 1234);
        assert(rms() > 1e5);                // 不是静音(均方 > 316^2)
        ratio[t] = hf_ratio();
        static int16_t copy[N];
        memcpy(copy, buf, sizeof(buf));
        gen((noise_type_t)t, 1234);
        assert(memcmp(copy, buf, sizeof(buf)) == 0);   // 同种子可复现
    }
    assert(ratio[NOISE_WHITE] > 1.5);
    assert(ratio[NOISE_PINK] < ratio[NOISE_WHITE]);
    assert(ratio[NOISE_BROWN] < ratio[NOISE_PINK]);
    assert(ratio[NOISE_FAN] < ratio[NOISE_WHITE]);

    // 海浪有明显涨落:前后半周期的能量差别大
    noise_state_t st;
    noise_init(&st, 7);
    st.gain_q8 = 256;
    static int16_t waves[16000 * 9];
    noise_fill(&st, NOISE_WAVES, waves, 16000 * 9, 256);
    double lo = 1e300, hi = 0;
    for (int s = 0; s < 9; s++) {
        double e = 0;
        for (int i = 0; i < 16000; i++) e += (double)waves[s * 16000 + i] * waves[s * 16000 + i];
        if (e < lo) lo = e;
        if (e > hi) hi = e;
    }
    assert(hi > lo * 4);

    // 渐入:从 0 增益开始,开头很安静
    noise_init(&st, 9);
    noise_fill(&st, NOISE_WHITE, buf, 64, 256);
    for (int i = 0; i < 64; i++) assert(buf[i] > -400 && buf[i] < 400);
    noise_fill(&st, NOISE_OFF, buf, 16, 0);
    for (int i = 0; i < 16; i++) assert(buf[i] == 0);
    assert(!strcmp(noise_name(NOISE_RAIN), "雨声"));
    puts("noise gen tests: PASS");
    return 0;
}
