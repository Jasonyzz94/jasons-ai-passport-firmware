// main/pomodoro.h —— 番茄钟纯逻辑:不依赖 ESP-IDF / LVGL,可在主机上单元测试。
//
// 规则(经典番茄工作法):
//   专注 → 短休息 → 专注 → … 每完成 long_every 个专注,下一次休息为长休息。
//   专注结束:完成数 +1,休息自动开始计时。
//   休息结束:回到专注;auto_next 为真时自动开始,否则等待手动开始。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define POMO_MIN_MINUTES 1
#define POMO_MAX_MINUTES 90

typedef enum {
    POMO_PHASE_WORK = 0,
    POMO_PHASE_BREAK,
    POMO_PHASE_LONG,
} pomo_phase_t;

typedef enum {
    POMO_STATE_READY = 0,   // 未开始,剩余时间 = 当前阶段全长
    POMO_STATE_RUNNING,
    POMO_STATE_PAUSED,
} pomo_state_t;

typedef enum {
    POMO_EVENT_NONE = 0,
    POMO_EVENT_WORK_DONE,   // 专注结束,已进入休息并开始计时
    POMO_EVENT_BREAK_DONE,  // 休息结束,已回到专注
} pomo_event_t;

typedef struct {
    pomo_phase_t phase;
    pomo_state_t state;
    int work_min;
    int break_min;
    int long_min;
    int long_every;         // 每几个专注一次长休息
    bool auto_next;         // 休息结束后自动开始下一个专注
    int done;               // 累计完成的专注数(由调用方决定是否按天清零)
    int cycle;              // 本轮(距上次长休息)已完成的专注数
    int64_t deadline_ms;    // RUNNING:结束时刻(单调时钟)
    int64_t remaining_ms;   // READY/PAUSED:冻结的剩余时间
} pomo_t;

void pomo_init(pomo_t *p, int work_min, int break_min);
int pomo_clamp_minutes(int minutes);
// READY -> RUNNING,RUNNING -> PAUSED,PAUSED -> RUNNING。
void pomo_toggle(pomo_t *p, int64_t now_ms);
// 当前阶段回到 READY,剩余时间恢复为全长。
void pomo_reset(pomo_t *p);
// 整轮重来:回到专注 READY,本轮计数清零(累计完成数保留)。
void pomo_restart_cycle(pomo_t *p);
// 跳过当前阶段(不计入完成数),进入下一阶段的 READY。
void pomo_skip(pomo_t *p);
// 仅 READY 有效:调整当前阶段时长(分钟)。
void pomo_adjust(pomo_t *p, int delta_min);
void pomo_set_minutes(pomo_t *p, pomo_phase_t phase, int minutes);
int pomo_minutes(const pomo_t *p, pomo_phase_t phase);
pomo_event_t pomo_tick(pomo_t *p, int64_t now_ms);
int64_t pomo_remaining_ms(const pomo_t *p, int64_t now_ms);
int64_t pomo_total_ms(const pomo_t *p);
// 下一次休息是否为长休息(用于界面预告)。
bool pomo_next_break_is_long(const pomo_t *p);
const char *pomo_phase_name(pomo_phase_t phase);

// ---------------------------------------------------------------------------
// 最近 7 天统计(环形,按"本地日序号" = 自 1970-01-01 起的天数)。
// ---------------------------------------------------------------------------
#define POMO_STATS_DAYS 7
typedef struct {
    int32_t day[POMO_STATS_DAYS];
    uint16_t count[POMO_STATS_DAYS];
    uint16_t minutes[POMO_STATS_DAYS];
} pomo_stats_t;

void pomo_stats_add(pomo_stats_t *s, int32_t day, int minutes);
// out_count/out_minutes[0] 是 6 天前,[6] 是今天;缺失的天为 0。
void pomo_stats_week(const pomo_stats_t *s, int32_t today, int out_count[POMO_STATS_DAYS],
                     int out_minutes[POMO_STATS_DAYS]);
int pomo_stats_today(const pomo_stats_t *s, int32_t today);
// 由年月日算日序号(公历,与时区无关的纯计算)。
int32_t pomo_day_number(int year, int month, int mday);
