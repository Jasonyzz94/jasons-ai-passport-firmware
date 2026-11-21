// main/pet.c —— 见 pet.h。
#include "pet.h"

#include <string.h>

#define TICK_MIN 10
#define MAX_TICKS (3 * 24 * 60 / TICK_MIN)

static const int k_stage_xp[] = {0, 5, 40, 150};   // 进入各阶段所需经验

static int32_t clamp(int32_t v) { return v < 0 ? 0 : (v > PET_MAX ? PET_MAX : v); }

void pet_init(pet_t *p, int32_t now_min)
{
    memset(p, 0, sizeof(*p));
    p->version = PET_VERSION;
    p->hunger = 700;
    p->mood = 700;
    p->clean = 1000;
    p->last_min = now_min;
    p->born_min = now_min;
}

bool pet_valid(const pet_t *p)
{
    return p->version == PET_VERSION && p->hunger >= 0 && p->hunger <= PET_MAX && p->mood >= 0
        && p->mood <= PET_MAX && p->clean >= 0 && p->clean <= PET_MAX && p->xp >= 0;
}

void pet_tick(pet_t *p, int32_t now_min)
{
    if (now_min < p->last_min) {   // 时钟被往回调:只重置基准,不倒扣
        p->last_min = now_min;
        return;
    }
    int32_t ticks = (now_min - p->last_min) / TICK_MIN;
    if (ticks <= 0) return;
    p->last_min += ticks * TICK_MIN;
    if (ticks > MAX_TICKS) ticks = MAX_TICKS;
    if (pet_stage(p) == PET_EGG) return;   // 蛋不会饿
    for (int32_t i = 0; i < ticks; i++) {
        p->hunger = clamp(p->hunger - 4);   // 约 1.7 天从饱到饿
        p->clean = clamp(p->clean - 3);
        int drop = 3;
        if (p->hunger < 300) drop += 3;
        if (p->clean < 300) drop += 2;
        p->mood = clamp(p->mood - drop);
    }
}

pet_result_t pet_feed(pet_t *p)
{
    if (p->hunger > 900) {
        p->mood = clamp(p->mood - 20);
        return PET_FULL;
    }
    p->hunger = clamp(p->hunger + 300);
    p->mood = clamp(p->mood + 30);
    p->xp += 2;
    return PET_OK;
}

pet_result_t pet_play(pet_t *p)
{
    if (pet_stage(p) != PET_EGG && p->hunger < 150) return PET_TOO_HUNGRY;
    p->mood = clamp(p->mood + 250);
    p->hunger = clamp(p->hunger - 50);
    p->clean = clamp(p->clean - 40);
    p->xp += 3;
    return PET_OK;
}

pet_result_t pet_wash(pet_t *p)
{
    if (p->clean > 900) return PET_ALREADY_CLEAN;
    p->clean = PET_MAX;
    p->mood = clamp(p->mood + 50);
    p->xp += 1;
    return PET_OK;
}

void pet_reward(pet_t *p, int count)
{
    if (count <= 0) return;
    p->hunger = clamp(p->hunger + 200 * count);
    p->mood = clamp(p->mood + 100 * count);
    p->xp += 5 * count;
}

pet_stage_t pet_stage(const pet_t *p)
{
    for (int s = PET_ADULT; s > PET_EGG; s--) {
        if (p->xp >= k_stage_xp[s]) return (pet_stage_t)s;
    }
    return PET_EGG;
}

int pet_level(const pet_t *p) { return 1 + p->xp / 10; }

int pet_xp_to_next(const pet_t *p)
{
    const pet_stage_t s = pet_stage(p);
    return s == PET_ADULT ? 0 : k_stage_xp[s + 1] - p->xp;
}

pet_status_t pet_status(const pet_t *p, int hour)
{
    if (pet_stage(p) == PET_EGG) return PET_FINE;
    if (p->hunger < 300) return PET_HUNGRY;
    if (p->clean < 300) return PET_DIRTY;
    if (p->mood < 300) return PET_SAD;
    if (hour >= 23 || hour < 7) return PET_SLEEPY;
    if (p->hunger >= 600 && p->mood >= 600 && p->clean >= 600) return PET_HAPPY;
    return PET_FINE;
}

int pet_age_days(const pet_t *p, int32_t now_min)
{
    return now_min > p->born_min ? (int)((now_min - p->born_min) / (24 * 60)) : 0;
}

const char *pet_stage_name(pet_stage_t s)
{
    static const char *const names[] = {"蛋", "幼崽", "少年", "成年"};
    return names[s];
}
