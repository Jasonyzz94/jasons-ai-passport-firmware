// main/pomodoro.c —— 番茄钟纯逻辑实现。
#include "pomodoro.h"

static int64_t minutes_to_ms(int minutes) {
    return (int64_t)minutes * 60 * 1000;
}

int pomo_clamp_minutes(int minutes) {
    if (minutes < POMO_MIN_MINUTES) return POMO_MIN_MINUTES;
    if (minutes > POMO_MAX_MINUTES) return POMO_MAX_MINUTES;
    return minutes;
}

static int64_t phase_total_ms(const pomo_t *p) {
    return minutes_to_ms(p->phase == POMO_PHASE_WORK ? p->work_min : p->break_min);
}

void pomo_init(pomo_t *p, int work_min, int break_min) {
    p->work_min = pomo_clamp_minutes(work_min);
    p->break_min = pomo_clamp_minutes(break_min);
    p->phase = POMO_PHASE_WORK;
    p->state = POMO_STATE_READY;
    p->done = 0;
    p->deadline_ms = 0;
    p->remaining_ms = phase_total_ms(p);
}

void pomo_toggle(pomo_t *p, int64_t now_ms) {
    if (p->state == POMO_STATE_RUNNING) {
        int64_t left = p->deadline_ms - now_ms;
        p->remaining_ms = left > 0 ? left : 0;
        p->state = POMO_STATE_PAUSED;
    } else {
        if (p->remaining_ms <= 0) p->remaining_ms = phase_total_ms(p);
        p->deadline_ms = now_ms + p->remaining_ms;
        p->state = POMO_STATE_RUNNING;
    }
}

void pomo_reset(pomo_t *p) {
    p->state = POMO_STATE_READY;
    p->remaining_ms = phase_total_ms(p);
    p->deadline_ms = 0;
}

void pomo_adjust(pomo_t *p, int delta_min) {
    if (p->state != POMO_STATE_READY) return;
    if (p->phase == POMO_PHASE_WORK) {
        p->work_min = pomo_clamp_minutes(p->work_min + delta_min);
    } else {
        p->break_min = pomo_clamp_minutes(p->break_min + delta_min);
    }
    p->remaining_ms = phase_total_ms(p);
}

void pomo_set_minutes(pomo_t *p, pomo_phase_t phase, int minutes) {
    const int clamped = pomo_clamp_minutes(minutes);
    if (phase == POMO_PHASE_WORK) p->work_min = clamped;
    else p->break_min = clamped;
    if (p->phase == phase && p->state == POMO_STATE_READY) {
        p->remaining_ms = phase_total_ms(p);
    }
}

pomo_event_t pomo_tick(pomo_t *p, int64_t now_ms) {
    if (p->state != POMO_STATE_RUNNING || now_ms < p->deadline_ms) {
        return POMO_EVENT_NONE;
    }
    if (p->phase == POMO_PHASE_WORK) {
        p->done++;
        p->phase = POMO_PHASE_BREAK;
        p->remaining_ms = phase_total_ms(p);
        p->deadline_ms = now_ms + p->remaining_ms;
        p->state = POMO_STATE_RUNNING;
        return POMO_EVENT_WORK_DONE;
    }
    p->phase = POMO_PHASE_WORK;
    p->state = POMO_STATE_READY;
    p->remaining_ms = phase_total_ms(p);
    p->deadline_ms = 0;
    return POMO_EVENT_BREAK_DONE;
}

int64_t pomo_remaining_ms(const pomo_t *p, int64_t now_ms) {
    if (p->state != POMO_STATE_RUNNING) return p->remaining_ms;
    int64_t left = p->deadline_ms - now_ms;
    return left > 0 ? left : 0;
}

int64_t pomo_total_ms(const pomo_t *p) {
    return phase_total_ms(p);
}

const char *pomo_phase_name(pomo_phase_t phase) {
    return phase == POMO_PHASE_WORK ? "FOCUS" : "BREAK";
}
