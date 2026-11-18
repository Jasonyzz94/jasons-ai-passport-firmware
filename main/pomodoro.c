// main/pomodoro.c —— 番茄钟纯逻辑实现。
#include "pomodoro.h"

#include <string.h>

static int64_t minutes_to_ms(int minutes) { return (int64_t)minutes * 60 * 1000; }

int pomo_clamp_minutes(int minutes)
{
    if (minutes < POMO_MIN_MINUTES) return POMO_MIN_MINUTES;
    if (minutes > POMO_MAX_MINUTES) return POMO_MAX_MINUTES;
    return minutes;
}

int pomo_minutes(const pomo_t *p, pomo_phase_t phase)
{
    switch (phase) {
    case POMO_PHASE_WORK: return p->work_min;
    case POMO_PHASE_BREAK: return p->break_min;
    default: return p->long_min;
    }
}

static int64_t phase_total_ms(const pomo_t *p) { return minutes_to_ms(pomo_minutes(p, p->phase)); }

void pomo_init(pomo_t *p, int work_min, int break_min)
{
    memset(p, 0, sizeof(*p));
    p->work_min = pomo_clamp_minutes(work_min);
    p->break_min = pomo_clamp_minutes(break_min);
    p->long_min = 15;
    p->long_every = 4;
    p->phase = POMO_PHASE_WORK;
    p->state = POMO_STATE_READY;
    p->remaining_ms = phase_total_ms(p);
}

void pomo_toggle(pomo_t *p, int64_t now_ms)
{
    if (p->state == POMO_STATE_RUNNING) {
        const int64_t left = p->deadline_ms - now_ms;
        p->remaining_ms = left > 0 ? left : 0;
        p->state = POMO_STATE_PAUSED;
    } else {
        if (p->remaining_ms <= 0) p->remaining_ms = phase_total_ms(p);
        p->deadline_ms = now_ms + p->remaining_ms;
        p->state = POMO_STATE_RUNNING;
    }
}

void pomo_reset(pomo_t *p)
{
    p->state = POMO_STATE_READY;
    p->remaining_ms = phase_total_ms(p);
    p->deadline_ms = 0;
}

void pomo_restart_cycle(pomo_t *p)
{
    p->phase = POMO_PHASE_WORK;
    p->cycle = 0;
    pomo_reset(p);
}

bool pomo_next_break_is_long(const pomo_t *p)
{
    return p->long_every > 0 && (p->cycle + 1) % p->long_every == 0;
}

void pomo_skip(pomo_t *p)
{
    if (p->phase == POMO_PHASE_WORK) {
        p->phase = pomo_next_break_is_long(p) ? POMO_PHASE_LONG : POMO_PHASE_BREAK;
    } else {
        if (p->phase == POMO_PHASE_LONG) p->cycle = 0;
        p->phase = POMO_PHASE_WORK;
    }
    pomo_reset(p);
}

void pomo_adjust(pomo_t *p, int delta_min)
{
    if (p->state != POMO_STATE_READY) return;
    pomo_set_minutes(p, p->phase, pomo_minutes(p, p->phase) + delta_min);
}

void pomo_set_minutes(pomo_t *p, pomo_phase_t phase, int minutes)
{
    const int clamped = pomo_clamp_minutes(minutes);
    switch (phase) {
    case POMO_PHASE_WORK: p->work_min = clamped; break;
    case POMO_PHASE_BREAK: p->break_min = clamped; break;
    default: p->long_min = clamped; break;
    }
    if (p->phase == phase && p->state == POMO_STATE_READY) p->remaining_ms = phase_total_ms(p);
}

pomo_event_t pomo_tick(pomo_t *p, int64_t now_ms)
{
    if (p->state != POMO_STATE_RUNNING || now_ms < p->deadline_ms) return POMO_EVENT_NONE;

    // 以截止时刻为新阶段起点,避免 tick 延迟累积误差。
    const int64_t end = p->deadline_ms;
    if (p->phase == POMO_PHASE_WORK) {
        p->done++;
        const bool is_long = pomo_next_break_is_long(p);
        p->cycle++;
        p->phase = is_long ? POMO_PHASE_LONG : POMO_PHASE_BREAK;
        p->remaining_ms = phase_total_ms(p);
        p->deadline_ms = end + p->remaining_ms;
        p->state = POMO_STATE_RUNNING;
        return POMO_EVENT_WORK_DONE;
    }
    if (p->phase == POMO_PHASE_LONG) p->cycle = 0;
    p->phase = POMO_PHASE_WORK;
    p->remaining_ms = phase_total_ms(p);
    if (p->auto_next) {
        p->deadline_ms = end + p->remaining_ms;
        p->state = POMO_STATE_RUNNING;
    } else {
        p->deadline_ms = 0;
        p->state = POMO_STATE_READY;
    }
    return POMO_EVENT_BREAK_DONE;
}

int64_t pomo_remaining_ms(const pomo_t *p, int64_t now_ms)
{
    if (p->state != POMO_STATE_RUNNING) return p->remaining_ms;
    const int64_t left = p->deadline_ms - now_ms;
    return left > 0 ? left : 0;
}

int64_t pomo_total_ms(const pomo_t *p) { return phase_total_ms(p); }

const char *pomo_phase_name(pomo_phase_t phase)
{
    switch (phase) {
    case POMO_PHASE_WORK: return "专注";
    case POMO_PHASE_BREAK: return "短休息";
    default: return "长休息";
    }
}

// ---------------------------------------------------------------------------
// 统计
// ---------------------------------------------------------------------------
static int stats_slot(const pomo_stats_t *s, int32_t day)
{
    for (int i = 0; i < POMO_STATS_DAYS; i++) {
        if (s->day[i] == day && (s->count[i] || s->minutes[i])) return i;
    }
    return -1;
}

void pomo_stats_add(pomo_stats_t *s, int32_t day, int minutes)
{
    int slot = stats_slot(s, day);
    if (slot < 0) {
        // 选最旧(或空)的槽位覆盖。
        slot = 0;
        for (int i = 1; i < POMO_STATS_DAYS; i++) {
            const bool empty_i = !s->count[i] && !s->minutes[i];
            const bool empty_s = !s->count[slot] && !s->minutes[slot];
            if (empty_i && !empty_s) slot = i;
            else if (empty_i == empty_s && s->day[i] < s->day[slot]) slot = i;
        }
        s->day[slot] = day;
        s->count[slot] = 0;
        s->minutes[slot] = 0;
    }
    if (s->count[slot] < UINT16_MAX) s->count[slot]++;
    const int m = s->minutes[slot] + (minutes > 0 ? minutes : 0);
    s->minutes[slot] = (uint16_t)(m > UINT16_MAX ? UINT16_MAX : m);
}

void pomo_stats_week(const pomo_stats_t *s, int32_t today, int out_count[POMO_STATS_DAYS],
                     int out_minutes[POMO_STATS_DAYS])
{
    for (int i = 0; i < POMO_STATS_DAYS; i++) {
        const int slot = stats_slot(s, today - (POMO_STATS_DAYS - 1 - i));
        out_count[i] = slot >= 0 ? s->count[slot] : 0;
        if (out_minutes) out_minutes[i] = slot >= 0 ? s->minutes[slot] : 0;
    }
}

int pomo_stats_today(const pomo_stats_t *s, int32_t today)
{
    const int slot = stats_slot(s, today);
    return slot >= 0 ? s->count[slot] : 0;
}

int32_t pomo_day_number(int year, int month, int mday)
{
    // Howard Hinnant 的 days_from_civil。
    year -= month <= 2;
    const int32_t era = (year >= 0 ? year : year - 399) / 400;
    const int32_t yoe = year - era * 400;
    const int32_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + mday - 1;
    const int32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
