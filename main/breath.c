// main/breath.c —— 见 breath.h。
#include "breath.h"

const breath_pattern_t BREATH_PATTERNS[BREATH_PATTERN_COUNT] = {
    {"平静呼吸", "吸 5 秒 · 呼 5 秒,让心率慢下来", {5, 0, 5, 0}},
    {"4-7-8 助眠", "吸 4 · 屏 7 · 呼 8,睡前做 4 轮", {4, 7, 8, 0}},
    {"箱式呼吸", "吸 屏 呼 屏各 4 秒,紧张时用", {4, 4, 4, 4}},
    {"提神呼吸", "吸 4 · 屏 2 · 呼 2,犯困时用", {4, 2, 2, 0}},
};

static int clamp_pattern(int p) { return p < 0 || p >= BREATH_PATTERN_COUNT ? 0 : p; }

int breath_cycle_ms(int pattern)
{
    const breath_pattern_t *bp = &BREATH_PATTERNS[clamp_pattern(pattern)];
    return (bp->secs[0] + bp->secs[1] + bp->secs[2] + bp->secs[3]) * 1000;
}

// 缓入缓出(smoothstep),0..1000 → 0..1000,呼吸圈的缩放更自然。
static int ease(int t)
{
    const int64_t x = t;
    return (int)((x * x * (3000 - 2 * x)) / 1000000);
}

breath_frame_t breath_at(int pattern, int64_t elapsed_ms)
{
    pattern = clamp_pattern(pattern);
    const breath_pattern_t *bp = &BREATH_PATTERNS[pattern];
    const int cycle = breath_cycle_ms(pattern);
    if (elapsed_ms < 0) elapsed_ms = 0;
    breath_frame_t f = {0};
    f.round = (int)(elapsed_ms / cycle) + 1;
    int64_t t = elapsed_ms % cycle;
    for (int i = 0; i < 4; i++) {
        const int len = bp->secs[i] * 1000;
        if (len == 0) continue;
        if (t < len) {
            f.step = (breath_step_t)i;
            f.progress = (int)(t * 1000 / len);
            f.step_left_s = (int)((len - t + 999) / 1000);
            break;
        }
        t -= len;
    }
    switch (f.step) {
    case BREATH_IN: f.scale = ease(f.progress); break;
    case BREATH_HOLD_IN: f.scale = 1000; break;
    case BREATH_OUT: f.scale = 1000 - ease(f.progress); break;
    default: f.scale = 0; break;
    }
    return f;
}

const char *breath_step_name(breath_step_t step)
{
    switch (step) {
    case BREATH_IN: return "吸气";
    case BREATH_HOLD_IN: return "屏住";
    case BREATH_OUT: return "呼气";
    default: return "停一下";
    }
}
