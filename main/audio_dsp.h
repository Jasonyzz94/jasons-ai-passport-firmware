// main/audio_dsp.h —— 纯算法:FFT、频段、分贝、音高检测、音名换算、IMA ADPCM 编解码。
// 不依赖 ESP-IDF / LVGL,在主机上有单元测试(tests/test_audio_dsp.c)。
#pragma once

#include <stddef.h>
#include <stdint.h>

#define DSP_FFT_N 512

// 原地复数 FFT(n 为 2 的幂)。re/im 长度 n。
void dsp_fft(float *re, float *im, int n);

// 把 n 点实信号(已加窗)的幅度谱映射成 bands 个对数频段(0..1)。
void dsp_bands(const int16_t *pcm, int n, int sample_rate, float *bands, int nbands);

// 均方根电平换算的 dBFS(满幅正弦 ≈ -3 dBFS),以及显示用近似声压值(dBFS + 偏移)。
float dsp_dbfs(const int16_t *pcm, int n);
#define DSP_SPL_OFFSET 100.0f

// 自相关(NSDF / McLeod)音高检测,返回 Hz,0 表示无明确音高;clarity 0..1。
float dsp_pitch(const int16_t *pcm, int n, int sample_rate, float *clarity);

typedef struct {
    int midi;         // MIDI 音符号(A4 = 69)
    float cents;      // 相对最近音的偏差,-50..+50
    char name[4];     // 例 "A"、"C#"
    int octave;
} dsp_note_t;
void dsp_note_from_hz(float hz, float a4, dsp_note_t *out);

// ---------------------------------------------------------------------------
// IMA ADPCM:每块 = 4 字节头(int16 预测值 LE、uint8 步长索引、uint8 保留)+ 样本对。
// 每块独立可解,丢包只损失这一块(ESP-NOW 对讲需要)。
// ---------------------------------------------------------------------------
#define ADPCM_BLOCK_SAMPLES 256
#define ADPCM_BLOCK_BYTES (4 + ADPCM_BLOCK_SAMPLES / 2)

typedef struct {
    int16_t predictor;
    int8_t index;
} adpcm_state_t;

// 编码一块(samples 必须为偶数,<= ADPCM_BLOCK_SAMPLES),返回写入字节数。
size_t adpcm_encode_block(adpcm_state_t *st, const int16_t *pcm, int samples, uint8_t *out);
// 解码一块,返回样本数。
int adpcm_decode_block(const uint8_t *in, size_t len, int16_t *pcm, int max_samples);
