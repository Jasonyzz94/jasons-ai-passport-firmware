// tests/test_breath.c —— 呼吸节奏的主机测试。
#include "breath.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    // 4-7-8:一轮 19 秒
    assert(breath_cycle_ms(1) == 19000);
    breath_frame_t f = breath_at(1, 0);
    assert(f.step == BREATH_IN && f.scale == 0 && f.step_left_s == 4 && f.round == 1);
    f = breath_at(1, 2000);
    assert(f.step == BREATH_IN && f.scale == 500 && f.step_left_s == 2);
    f = breath_at(1, 4000);
    assert(f.step == BREATH_HOLD_IN && f.scale == 1000 && f.step_left_s == 7);
    f = breath_at(1, 11000);
    assert(f.step == BREATH_OUT && f.scale == 1000 && f.step_left_s == 8);
    f = breath_at(1, 18999);
    assert(f.step == BREATH_OUT && f.scale <= 1 && f.step_left_s == 1);
    f = breath_at(1, 19000);
    assert(f.step == BREATH_IN && f.round == 2);
    // 箱式:呼气后屏息
    f = breath_at(2, 13000);
    assert(f.step == BREATH_HOLD_OUT && f.scale == 0 && f.step_left_s == 3);
    // 平静:没有屏息
    f = breath_at(0, 5000);
    assert(f.step == BREATH_OUT);
    // 缩放单调:吸气阶段只增不减
    int last = -1;
    for (int t = 0; t < 5000; t += 50) {
        f = breath_at(0, t);
        assert(f.scale >= last);
        last = f.scale;
    }
    // 越界参数不崩
    f = breath_at(99, -5);
    assert(f.step == BREATH_IN && f.round == 1);
    puts("breath tests: PASS");
    return 0;
}
