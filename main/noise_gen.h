// main/noise_gen.h —— 环境声合成(白 / 粉红 / 棕噪音、雨声、海浪、风扇)。
// 全整数运算(ESP32-C3 没有浮点单元),纯逻辑,主机测试覆盖。16 kHz 单声道。
#pragma once

#include <stdint.h>

typedef enum {
    NOISE_OFF = 0,
    NOISE_WHITE,
    NOISE_PINK,
    NOISE_BROWN,
    NOISE_RAIN,
    NOISE_WAVES,
    NOISE_FAN,
    NOISE_COUNT,
} noise_type_t;

typedef struct {
    uint32_t rng;
    uint32_t counter;          // 粉红噪音的 Voss-McCartney 计数器
    int32_t rows[8];
    int32_t pink_sum;
    int32_t brown;
    int32_t lp;                // 一阶低通(风扇、雨声底噪)
    int32_t drop;              // 当前雨滴的幅度
    uint32_t phase;            // 慢速包络相位(海浪 / 风扇转动)
    int32_t gain_q8;           // 渐入渐出,0..256
} noise_state_t;

void noise_init(noise_state_t *st, uint32_t seed);
// 生成 n 个样本。target_q8 为目标增益(0..256),内部平滑过渡避免爆音。
void noise_fill(noise_state_t *st, noise_type_t type, int16_t *out, int n, int32_t target_q8);
const char *noise_name(noise_type_t type);
