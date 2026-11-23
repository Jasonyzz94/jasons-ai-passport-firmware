// tests/test_audio_dsp.c —— 音频纯算法主机测试:FFT、频段、分贝、音高、音名、ADPCM。
#include "audio_dsp.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static dsp_work_t s_work;   // 测试用:静态清零的工作区

#define SR 16000
#define PI 3.14159265358979323846

static void sine(int16_t *buf, int n, float hz, float amp)
{
    for (int i = 0; i < n; i++) buf[i] = (int16_t)(amp * 32767.0 * sin(2 * PI * hz * i / SR));
}

static void test_fft_peak(void)
{
    float re[64], im[64];
    for (int i = 0; i < 64; i++) {
        re[i] = (float)cos(2 * PI * 5 * i / 64);
        im[i] = 0;
    }
    dsp_fft(re, im, 64);
    int best = 0;
    for (int k = 1; k < 32; k++) if (re[k] * re[k] + im[k] * im[k] > re[best] * re[best] + im[best] * im[best]) best = k;
    assert(best == 5);
    assert(fabsf(re[5] - 32.0f) < 0.01f);   // N/2 幅度
}

static void test_dbfs(void)
{
    static int16_t buf[1024];
    sine(buf, 1024, 1000, 1.0f);
    const float db = dsp_dbfs(buf, 1024);
    assert(db > -3.3f && db < -2.7f);
    sine(buf, 1024, 1000, 0.1f);
    assert(fabsf(dsp_dbfs(buf, 1024) - (db - 20.0f)) < 0.3f);
    memset(buf, 0, sizeof(buf));
    assert(dsp_dbfs(buf, 1024) <= -119.0f);
}

static void test_bands_follow_tone(void)
{
    static int16_t buf[512];
    float low[20], high[20];
    sine(buf, 512, 200, 0.5f);
    dsp_bands(&s_work, buf, 512, SR, low, 20);
    sine(buf, 512, 4000, 0.5f);
    dsp_bands(&s_work, buf, 512, SR, high, 20);
    int lo_peak = 0, hi_peak = 0;
    for (int i = 0; i < 20; i++) {
        if (low[i] > low[lo_peak]) lo_peak = i;
        if (high[i] > high[hi_peak]) hi_peak = i;
    }
    assert(lo_peak < 8);
    assert(hi_peak > 12);
    assert(low[lo_peak] > 0.5f);
}

static void test_pitch(void)
{
    static int16_t buf[1024];
    const float tones[] = {82.41f, 110.0f, 196.0f, 440.0f, 523.25f, 880.0f};
    for (size_t i = 0; i < sizeof(tones) / sizeof(tones[0]); i++) {
        sine(buf, 1024, tones[i], 0.4f);
        float clarity = 0;
        const float hz = dsp_pitch(&s_work, buf, 1024, SR, &clarity);
        if (fabsf(hz - tones[i]) > tones[i] * 0.01f) {
            printf("pitch %.2f -> %.2f\n", tones[i], hz);
            assert(0);
        }
        assert(clarity > 0.9f);
    }
    // 带谐波的"乐器"音仍识别基频
    for (int i = 0; i < 1024; i++) {
        buf[i] = (int16_t)(8000 * sin(2 * PI * 146.83 * i / SR) + 5000 * sin(2 * PI * 293.66 * i / SR)
                           + 3000 * sin(2 * PI * 440.49 * i / SR));
    }
    const float hz = dsp_pitch(&s_work, buf, 1024, SR, NULL);
    assert(fabsf(hz - 146.83f) < 2.0f);
    // 静音无音高
    memset(buf, 0, sizeof(buf));
    assert(dsp_pitch(&s_work, buf, 1024, SR, NULL) == 0.0f);
}

static void test_note(void)
{
    dsp_note_t n;
    dsp_note_from_hz(440.0f, 440.0f, &n);
    assert(n.midi == 69 && !strcmp(n.name, "A") && n.octave == 4 && fabsf(n.cents) < 0.01f);
    dsp_note_from_hz(261.63f, 440.0f, &n);
    assert(!strcmp(n.name, "C") && n.octave == 4 && fabsf(n.cents) < 1.0f);
    dsp_note_from_hz(452.89f, 440.0f, &n);           // A4 + 50 音分附近
    assert(n.cents > 45.0f || n.cents < -45.0f);
    dsp_note_from_hz(446.0f, 440.0f, &n);
    assert(!strcmp(n.name, "A") && n.cents > 20.0f && n.cents < 26.0f);
    dsp_note_from_hz(82.41f, 440.0f, &n);
    assert(!strcmp(n.name, "E") && n.octave == 2);
    dsp_note_from_hz(277.18f, 440.0f, &n);
    assert(!strcmp(n.name, "C#") && n.octave == 4);
}

static void test_adpcm_roundtrip(void)
{
    static int16_t in[ADPCM_BLOCK_SAMPLES * 8], out[ADPCM_BLOCK_SAMPLES * 8];
    for (int i = 0; i < ADPCM_BLOCK_SAMPLES * 8; i++) {
        in[i] = (int16_t)(12000 * sin(2 * PI * 300 * i / SR) + 4000 * sin(2 * PI * 1700 * i / SR));
    }
    adpcm_state_t st = {0, 0};
    uint8_t block[ADPCM_BLOCK_BYTES];
    int total = 0;
    for (int b = 0; b < 8; b++) {
        const size_t len = adpcm_encode_block(&st, in + b * ADPCM_BLOCK_SAMPLES, ADPCM_BLOCK_SAMPLES, block);
        assert(len == ADPCM_BLOCK_BYTES);
        total += adpcm_decode_block(block, len, out + total, ADPCM_BLOCK_SAMPLES);
    }
    assert(total == ADPCM_BLOCK_SAMPLES * 8);
    // 跳过第一块(编码器从 0 起步需要收敛),其余 SNR > 20 dB
    double sig = 0, err = 0;
    for (int i = ADPCM_BLOCK_SAMPLES; i < total; i++) {
        sig += (double)in[i] * in[i];
        err += (double)(in[i] - out[i]) * (in[i] - out[i]);
    }
    const double snr = 10 * log10(sig / (err + 1));
    assert(snr > 20.0);
    // 每块独立:单独解第 5 块也正确(模拟丢包后恢复)
    adpcm_state_t st2 = {0, 0};
    for (int b = 0; b < 5; b++) adpcm_encode_block(&st2, in + b * ADPCM_BLOCK_SAMPLES, ADPCM_BLOCK_SAMPLES, block);
    int16_t solo[ADPCM_BLOCK_SAMPLES];
    adpcm_encode_block(&st2, in + 5 * ADPCM_BLOCK_SAMPLES, ADPCM_BLOCK_SAMPLES, block);
    assert(adpcm_decode_block(block, ADPCM_BLOCK_BYTES, solo, ADPCM_BLOCK_SAMPLES) == ADPCM_BLOCK_SAMPLES);
    assert(memcmp(solo, out + 5 * ADPCM_BLOCK_SAMPLES, sizeof(solo)) == 0);
    // 截断输入不越界
    assert(adpcm_decode_block(block, 3, solo, ADPCM_BLOCK_SAMPLES) == 0);
    assert(adpcm_decode_block(block, ADPCM_BLOCK_BYTES, solo, 10) == 10);
}

int main(void)
{
    test_fft_peak();
    test_dbfs();
    test_bands_follow_tone();
    test_pitch();
    test_note();
    test_adpcm_roundtrip();
    puts("audio dsp tests: PASS");
    return 0;
}
