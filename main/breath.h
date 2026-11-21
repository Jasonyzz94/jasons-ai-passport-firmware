// main/breath.h —— 呼吸练习节奏计算(纯逻辑,无 ESP-IDF / LVGL 依赖,有主机测试)。
#pragma once

#include <stdint.h>

typedef enum { BREATH_IN = 0, BREATH_HOLD_IN, BREATH_OUT, BREATH_HOLD_OUT } breath_step_t;

typedef struct {
    const char *name;      // 显示名
    const char *desc;      // 一句话说明
    uint16_t secs[4];      // 吸气 / 吸后屏息 / 呼气 / 呼后屏息(秒,0 = 跳过)
} breath_pattern_t;

#define BREATH_PATTERN_COUNT 4
extern const breath_pattern_t BREATH_PATTERNS[BREATH_PATTERN_COUNT];

typedef struct {
    breath_step_t step;
    int step_left_s;       // 当前步骤还剩几秒(向上取整,用来显示倒数)
    int progress;          // 当前步骤进度 0..1000
    int scale;             // 圆的大小 0..1000(吸气变大、呼气变小、屏息保持)
    int round;             // 第几轮(从 1 开始)
} breath_frame_t;

int breath_cycle_ms(int pattern);
breath_frame_t breath_at(int pattern, int64_t elapsed_ms);
const char *breath_step_name(breath_step_t step);
