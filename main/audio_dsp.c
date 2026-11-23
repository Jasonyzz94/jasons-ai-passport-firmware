// main/audio_dsp.c —— 纯算法实现。
#include "audio_dsp.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void dsp_fft(float *re, float *im, int n)
{
    // 位反转
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const float ang = -2.0f * (float)M_PI / (float)len;
        const float wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < len / 2; k++) {
                const int a = i + k, b = i + k + len / 2;
                const float tr = re[b] * cr - im[b] * ci;
                const float ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
                const float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

void dsp_bands(dsp_work_t *w, const int16_t *pcm, int n, int sample_rate, float *bands, int nbands)
{
    float *re = w->re, *im = w->im, *window = w->window;
    if (n > DSP_FFT_N) n = DSP_FFT_N;
    if (w->window_n != n) {   // Hann 窗只算一次(没有 FPU,cosf 很贵)
        for (int i = 0; i < n; i++) window[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (float)(n - 1));
        w->window_n = n;
    }
    for (int i = 0; i < n; i++) {
        re[i] = pcm[i] / 32768.0f * window[i];
        im[i] = 0.0f;
    }
    for (int i = n; i < DSP_FFT_N; i++) re[i] = im[i] = 0.0f;
    dsp_fft(re, im, DSP_FFT_N);

    // 对数频段:60 Hz .. 奈奎斯特的 90%
    const float fmin = 60.0f, fmax = sample_rate * 0.45f;
    const float bin_hz = (float)sample_rate / DSP_FFT_N;
    for (int b = 0; b < nbands; b++) {
        const float f0 = fmin * powf(fmax / fmin, (float)b / nbands);
        const float f1 = fmin * powf(fmax / fmin, (float)(b + 1) / nbands);
        int k0 = (int)(f0 / bin_hz), k1 = (int)(f1 / bin_hz);
        if (k1 <= k0) k1 = k0 + 1;
        if (k1 > DSP_FFT_N / 2) k1 = DSP_FFT_N / 2;
        float sum = 0.0f;
        for (int k = k0; k < k1; k++) sum += re[k] * re[k] + im[k] * im[k];
        const float mag = sqrtf(sum / (float)(k1 - k0));
        // 映射:-70 dB..-10 dB → 0..1
        const float db = 20.0f * log10f(mag + 1e-9f);
        float v = (db + 70.0f) / 60.0f;
        bands[b] = v < 0 ? 0 : (v > 1 ? 1 : v);
    }
}

float dsp_dbfs(const int16_t *pcm, int n)
{
    if (n <= 0) return -120.0f;
    double acc = 0.0;
    for (int i = 0; i < n; i++) acc += (double)pcm[i] * pcm[i];
    const double rms = sqrt(acc / n) / 32768.0;
    return rms < 1e-6 ? -120.0f : (float)(20.0 * log10(rms));
}

float dsp_pitch(dsp_work_t *w, const int16_t *pcm, int n, int sample_rate, float *clarity)
{
    // ESP32-C3 没有浮点单元:整个相关运算用整数做。先 2:1 降采样到 sample_rate/2,
    // 再做 McLeod NSDF:nsdf(t) = 2*r(t) / m(t),取第一个超过 0.85*最大值的峰。
    // 16 kHz、1024 点时约 155 个延迟 × 512 点 ≈ 8 万次整数乘加(原来是 50 万次软件双精度)。
    enum { MAXD = DSP_PITCH_MAXD, MAXLAG = DSP_PITCH_MAXLAG };
    int16_t *d = w->d;
    float *nsdf = w->nsdf;
    if (clarity) *clarity = 0.0f;
    int m = n / 2;
    if (m > MAXD) m = MAXD;
    const int rate = sample_rate / 2;
    int64_t energy = 0;
    for (int i = 0; i < m; i++) {
        d[i] = (int16_t)(((int32_t)pcm[2 * i] + pcm[2 * i + 1]) >> 2);   // 平均并 /2 防溢出
        energy += (int32_t)d[i] * d[i];
    }
    // 太安静(约 -50 dBFS 以下)不算音高
    if (energy < (int64_t)m * 30 * 30) return 0.0f;

    const int min_lag = rate / 1200;
    int max_lag = rate / 50;
    if (max_lag > m / 2) max_lag = m / 2;
    if (max_lag > MAXLAG) max_lag = MAXLAG;

    // m(t) 可以递推:m(t+1) = m(t) - d[t]^2 - d[m-1-t]^2
    int64_t mt = 2 * energy;
    float best = 0.0f;
    for (int t = 0; t < max_lag; t++) {
        int64_t r = 0;
        const int16_t *a0 = d, *a1 = d + t;
        for (int i = 0; i < m - t; i++) r += (int32_t)a0[i] * a1[i];
        nsdf[t] = mt > 0 ? (float)(2 * r) / (float)mt : 0.0f;
        if (t >= min_lag && nsdf[t] > best) best = nsdf[t];
        mt -= (int32_t)d[t] * d[t] + (int32_t)d[m - 1 - t] * d[m - 1 - t];
    }
    if (best < 0.5f) return 0.0f;
    const float thresh = 0.85f * best;
    for (int t = min_lag < 1 ? 1 : min_lag; t < max_lag - 1; t++) {
        if (nsdf[t] >= thresh && nsdf[t] >= nsdf[t - 1] && nsdf[t] >= nsdf[t + 1]) {
            const float a = nsdf[t - 1], b = nsdf[t], c = nsdf[t + 1];
            const float denom = a - 2 * b + c;
            const float shift = denom != 0.0f ? 0.5f * (a - c) / denom : 0.0f;
            if (clarity) *clarity = b;
            return (float)rate / ((float)t + shift);
        }
    }
    return 0.0f;
}

void dsp_note_from_hz(float hz, float a4, dsp_note_t *out)
{
    static const char *const names[12] = {"C", "C#", "D", "D#", "E", "F",
                                          "F#", "G", "G#", "A", "A#", "B"};
    memset(out, 0, sizeof(*out));
    if (hz <= 0.0f) return;
    const float midi_f = 69.0f + 12.0f * log2f(hz / a4);
    const int midi = (int)lroundf(midi_f);
    out->midi = midi;
    out->cents = (midi_f - (float)midi) * 100.0f;
    const int pc = ((midi % 12) + 12) % 12;
    strcpy(out->name, names[pc]);
    out->octave = midi / 12 - 1;
}

// ---------------------------------------------------------------------------
// IMA ADPCM
// ---------------------------------------------------------------------------
static const int16_t k_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};
static const int8_t k_index_adj[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static int16_t clamp16(int v) { return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }

static uint8_t encode_sample(adpcm_state_t *st, int16_t sample)
{
    const int step = k_step[st->index];
    int diff = sample - st->predictor;
    uint8_t code = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    int delta = step >> 3;
    if (diff >= step) { code |= 4; diff -= step; delta += step; }
    if (diff >= step >> 1) { code |= 2; diff -= step >> 1; delta += step >> 1; }
    if (diff >= step >> 2) { code |= 1; delta += step >> 2; }
    st->predictor = clamp16(st->predictor + ((code & 8) ? -delta : delta));
    int idx = st->index + k_index_adj[code];
    st->index = (int8_t)(idx < 0 ? 0 : (idx > 88 ? 88 : idx));
    return code;
}

static int16_t decode_sample(adpcm_state_t *st, uint8_t code)
{
    const int step = k_step[st->index];
    int delta = step >> 3;
    if (code & 4) delta += step;
    if (code & 2) delta += step >> 1;
    if (code & 1) delta += step >> 2;
    st->predictor = clamp16(st->predictor + ((code & 8) ? -delta : delta));
    int idx = st->index + k_index_adj[code & 15];
    st->index = (int8_t)(idx < 0 ? 0 : (idx > 88 ? 88 : idx));
    return st->predictor;
}

size_t adpcm_encode_block(adpcm_state_t *st, const int16_t *pcm, int samples, uint8_t *out)
{
    if (samples > ADPCM_BLOCK_SAMPLES) samples = ADPCM_BLOCK_SAMPLES;
    samples &= ~1;
    out[0] = (uint8_t)(st->predictor & 0xFF);
    out[1] = (uint8_t)((uint16_t)st->predictor >> 8);
    out[2] = (uint8_t)st->index;
    out[3] = 0;
    for (int i = 0; i < samples; i += 2) {
        const uint8_t lo = encode_sample(st, pcm[i]);
        const uint8_t hi = encode_sample(st, pcm[i + 1]);
        out[4 + i / 2] = (uint8_t)(lo | (hi << 4));
    }
    return 4 + (size_t)samples / 2;
}

int adpcm_decode_block(const uint8_t *in, size_t len, int16_t *pcm, int max_samples)
{
    if (len < 4) return 0;
    adpcm_state_t st = {
        .predictor = (int16_t)(in[0] | (in[1] << 8)),
        .index = (int8_t)(in[2] > 88 ? 88 : in[2]),
    };
    int n = 0;
    for (size_t i = 4; i < len && n + 1 < max_samples + 1; i++) {
        if (n < max_samples) pcm[n++] = decode_sample(&st, in[i] & 0x0F);
        if (n < max_samples) pcm[n++] = decode_sample(&st, in[i] >> 4);
    }
    return n;
}
