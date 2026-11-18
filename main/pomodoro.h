// main/pomodoro.h —— 番茄钟纯逻辑:不依赖 ESP-IDF / LVGL,可在主机上单元测试。
#pragma once

#include <stdint.h>

#define POMO_MIN_MINUTES 1
#define POMO_MAX_MINUTES 60

typedef enum {
    POMO_PHASE_WORK = 0,
    POMO_PHASE_BREAK,
} pomo_phase_t;

typedef enum {
    POMO_STATE_READY = 0,   // 未开始,剩余时间=当前阶段全长,可调时长
    POMO_STATE_RUNNING,
    POMO_STATE_PAUSED,
} pomo_state_t;

typedef enum {
    POMO_EVENT_NONE = 0,
    POMO_EVENT_WORK_DONE,   // 专注结束:已完成数+1,自动进入休息计时
    POMO_EVENT_BREAK_DONE,  // 休息结束:回到专注 READY,等待手动开始
} pomo_event_t;

typedef struct {
    pomo_phase_t phase;
    pomo_state_t state;
    int work_min;
    int break_min;
    int done;               // 本次开机以来完成的专注段数
    int64_t deadline_ms;    // RUNNING:结束时刻(单调时钟)
    int64_t remaining_ms;   // READY/PAUSED:冻结的剩余时间
} pomo_t;

void pomo_init(pomo_t *p, int work_min, int break_min);
int pomo_clamp_minutes(int minutes);

// READY -> RUNNING,RUNNING -> PAUSED,PAUSED -> RUNNING。
void pomo_toggle(pomo_t *p, int64_t now_ms);
// 回到当前阶段的 READY,剩余时间恢复为全长,已完成数保留。
void pomo_reset(pomo_t *p);
// 仅 READY 有效:调整当前阶段时长(分钟),同时刷新剩余时间。
void pomo_adjust(pomo_t *p, int delta_min);
// 直接设置某个阶段的时长(1..60 分钟)。若该阶段正处于 READY,同步刷新剩余时间。
void pomo_set_minutes(pomo_t *p, pomo_phase_t phase, int minutes);
// 推进计时,到点时完成阶段切换并返回事件;其他情况返回 POMO_EVENT_NONE。
pomo_event_t pomo_tick(pomo_t *p, int64_t now_ms);

int64_t pomo_remaining_ms(const pomo_t *p, int64_t now_ms);
int64_t pomo_total_ms(const pomo_t *p);
const char *pomo_phase_name(pomo_phase_t phase);
