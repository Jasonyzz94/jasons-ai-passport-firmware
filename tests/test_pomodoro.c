// tests/test_pomodoro.c —— 番茄钟纯逻辑主机测试。
#include "pomodoro.h"
#include <assert.h>
#include <stdio.h>

#define MS_MIN(min) ((int64_t)(min) * 60 * 1000)

static void test_init_and_clamp(void) {
    pomo_t p;
    pomo_init(&p, 25, 5);
    assert(p.state == POMO_STATE_READY && p.phase == POMO_PHASE_WORK);
    assert(pomo_remaining_ms(&p, 0) == MS_MIN(25));
    assert(pomo_total_ms(&p) == MS_MIN(25));
    assert(pomo_clamp_minutes(0) == POMO_MIN_MINUTES);
    assert(pomo_clamp_minutes(61) == POMO_MAX_MINUTES);
    assert(pomo_clamp_minutes(30) == 30);
}

static void test_toggle_and_pause_freezes(void) {
    pomo_t p;
    pomo_init(&p, 25, 5);
    pomo_toggle(&p, 1000);
    assert(p.state == POMO_STATE_RUNNING);
    assert(pomo_remaining_ms(&p, 1000) == MS_MIN(25));
    assert(pomo_remaining_ms(&p, 11000) == MS_MIN(25) - 10000);

    pomo_toggle(&p, 11000);  // 暂停
    assert(p.state == POMO_STATE_PAUSED);
    assert(pomo_remaining_ms(&p, 99000) == MS_MIN(25) - 10000);

    pomo_toggle(&p, 100000);  // 继续
    assert(p.state == POMO_STATE_RUNNING);
    assert(pomo_remaining_ms(&p, 100000) == MS_MIN(25) - 10000);
    assert(pomo_remaining_ms(&p, 101000) == MS_MIN(25) - 11000);
}

static void test_adjust_only_in_ready(void) {
    pomo_t p;
    pomo_init(&p, 25, 5);
    pomo_adjust(&p, 1);
    assert(p.work_min == 26 && pomo_remaining_ms(&p, 0) == MS_MIN(26));
    pomo_adjust(&p, -30);
    assert(p.work_min == POMO_MIN_MINUTES);
    pomo_adjust(&p, 100);
    assert(p.work_min == POMO_MAX_MINUTES);

    pomo_toggle(&p, 0);       // 运行中不可调
    pomo_adjust(&p, -1);
    assert(p.work_min == POMO_MAX_MINUTES);
}

static void test_work_done_starts_break(void) {
    pomo_t p;
    pomo_init(&p, 1, 1);
    pomo_toggle(&p, 0);
    assert(pomo_tick(&p, MS_MIN(1) - 1) == POMO_EVENT_NONE);
    assert(pomo_tick(&p, MS_MIN(1)) == POMO_EVENT_WORK_DONE);
    assert(p.done == 1 && p.phase == POMO_PHASE_BREAK);
    assert(p.state == POMO_STATE_RUNNING);              // 自动开始休息
    assert(pomo_remaining_ms(&p, MS_MIN(1)) == MS_MIN(1));
}

static void test_break_done_returns_ready(void) {
    pomo_t p;
    pomo_init(&p, 1, 1);
    pomo_toggle(&p, 0);
    assert(pomo_tick(&p, MS_MIN(1)) == POMO_EVENT_WORK_DONE);
    assert(pomo_tick(&p, MS_MIN(2)) == POMO_EVENT_BREAK_DONE);
    assert(p.phase == POMO_PHASE_WORK && p.state == POMO_STATE_READY);
    assert(pomo_remaining_ms(&p, MS_MIN(2)) == MS_MIN(1));
    assert(p.done == 1);
}

static void test_reset_keeps_progress(void) {
    pomo_t p;
    pomo_init(&p, 2, 1);
    pomo_toggle(&p, 0);
    pomo_tick(&p, MS_MIN(2));      // 完成一次专注
    pomo_reset(&p);
    assert(p.state == POMO_STATE_READY && p.phase == POMO_PHASE_BREAK);
    assert(pomo_remaining_ms(&p, 0) == MS_MIN(1));
    assert(p.done == 1);
}

static void test_set_minutes(void) {
    pomo_t p;
    pomo_init(&p, 25, 5);
    pomo_set_minutes(&p, POMO_PHASE_BREAK, 10);
    assert(p.break_min == 10);
    assert(pomo_remaining_ms(&p, 0) == MS_MIN(25));   // 专注 READY 不受休息设置影响
    pomo_set_minutes(&p, POMO_PHASE_WORK, 30);
    assert(pomo_remaining_ms(&p, 0) == MS_MIN(30));   // 当前 READY 阶段立即生效
    pomo_set_minutes(&p, POMO_PHASE_WORK, 999);
    assert(p.work_min == POMO_MAX_MINUTES);

    pomo_toggle(&p, 0);                                // RUNNING 下设置不影响剩余时间
    const int64_t left = pomo_remaining_ms(&p, 0);
    pomo_set_minutes(&p, POMO_PHASE_WORK, 10);
    assert(pomo_remaining_ms(&p, 0) == left);
    assert(p.work_min == 10);
}

int main(void) {
    test_init_and_clamp();
    test_toggle_and_pause_freezes();
    test_adjust_only_in_ready();
    test_work_done_starts_break();
    test_break_done_returns_ready();
    test_reset_keeps_progress();
    test_set_minutes();
    puts("pomodoro tests: PASS");
    return 0;
}
