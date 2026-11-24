// main/pet.c —— 见 pet.h。
#include "pet.h"

#include <string.h>

#define TICK_MIN 10
#define MAX_TICKS (3 * 24 * 60 / TICK_MIN)
#define DAY_MIN (24 * 60)

// 进入各阶段所需的经验,以及在上一阶段至少要待的分钟数。
static const int k_stage_xp[PET_STAGE_COUNT] = {0, 5, 40, 150, 400};
static const int32_t k_stage_min[PET_STAGE_COUNT] = {0, 0, 1 * DAY_MIN, 2 * DAY_MIN, 4 * DAY_MIN};
#define ELDER_PASS_MIN (3 * DAY_MIN)

static int32_t clamp(int32_t v) { return v < 0 ? 0 : (v > PET_MAX ? PET_MAX : v); }
static int32_t clamp_care(int32_t v) { return v < -PET_CARE_MAX ? -PET_CARE_MAX : (v > PET_CARE_MAX ? PET_CARE_MAX : v); }

void pet_init(pet_t *p, int32_t now_min, pet_species_t species)
{
    memset(p, 0, sizeof(*p));
    p->version = PET_VERSION;
    p->hunger = 700;
    p->mood = 700;
    p->clean = 1000;
    p->last_min = now_min;
    p->born_min = now_min;
    p->stage_min = now_min;
    p->species = species;
    p->stage = PET_EGG;
    p->generation = 1;
}

bool pet_valid(const pet_t *p)
{
    return p->version == PET_VERSION && p->hunger >= 0 && p->hunger <= PET_MAX && p->mood >= 0
        && p->mood <= PET_MAX && p->clean >= 0 && p->clean <= PET_MAX && p->xp >= 0 && p->species >= 0
        && p->species < PET_SPECIES_COUNT && p->stage >= 0 && p->stage < PET_STAGE_COUNT
        && p->care >= -PET_CARE_MAX && p->care <= PET_CARE_MAX && p->form >= 0 && p->form < PET_FORM_COUNT
        && p->generation >= 1;
}

static uint32_t dex_bit(int species, int form) { return 1u << (species * PET_FORM_COUNT + form); }

bool pet_migrate_v1(const pet_v1_t *old, int32_t now_min, pet_t *out)
{
    if (old->version != 1 || old->hunger < 0 || old->hunger > PET_MAX || old->mood < 0 || old->mood > PET_MAX
        || old->clean < 0 || old->clean > PET_MAX || old->xp < 0) {
        return false;
    }
    pet_init(out, now_min, PET_SP_DANGO);
    out->hunger = old->hunger;
    out->mood = old->mood;
    out->clean = old->clean;
    out->xp = old->xp;
    out->last_min = old->last_min;
    out->born_min = old->born_min;
    // 第 1 版只按经验分阶段(最高成年),迁移后从现在开始计这一阶段的时间。
    static const int v1_xp[] = {0, 5, 40, 150};
    for (int s = PET_ADULT; s >= PET_EGG; s--) {
        if (old->xp >= v1_xp[s]) {
            out->stage = s;
            break;
        }
    }
    if (out->stage == PET_ADULT) out->dex = dex_bit(PET_SP_DANGO, PET_FORM_NORMAL);
    return true;
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
    if (p->stage == PET_EGG) return;   // 蛋不会饿
    // 长老新陈代谢慢一些
    const int slow = p->stage == PET_ELDER ? 2 : 1;
    for (int32_t i = 0; i < ticks; i++) {
        p->hunger = clamp(p->hunger - 4 / slow);   // 约 1.7 天从饱到饿
        p->clean = clamp(p->clean - 3 / slow);
        int drop = 3;
        if (p->hunger < 300) drop += 3;
        if (p->clean < 300) drop += 2;
        p->mood = clamp(p->mood - drop);
        // 照顾评分:三项都不错时慢慢涨,有一项很差时掉得更快
        if (p->hunger >= 500 && p->mood >= 500 && p->clean >= 500) p->care = clamp_care(p->care + 1);
        else if (p->hunger < 200 || p->mood < 200 || p->clean < 200) p->care = clamp_care(p->care - 2);
    }
}

bool pet_evolve(pet_t *p, int32_t now_min)
{
    if (p->stage >= PET_ELDER) return false;
    const int next = p->stage + 1;
    if (p->xp < k_stage_xp[next] || now_min - p->stage_min < k_stage_min[next]) return false;
    p->stage = next;
    p->stage_min = now_min;
    if (next == PET_ADULT) {
        p->form = p->care >= PET_SHINY_CARE ? PET_FORM_SHINY : PET_FORM_NORMAL;
        p->dex |= dex_bit(p->species, p->form);
    }
    return true;
}

pet_result_t pet_feed(pet_t *p)
{
    if (p->hunger > 900) {
        p->mood = clamp(p->mood - 20);
        return PET_FULL;
    }
    p->hunger = clamp(p->hunger + 300);
    p->mood = clamp(p->mood + 30);
    p->care = clamp_care(p->care + 2);
    p->xp += 2;
    return PET_OK;
}

pet_result_t pet_play(pet_t *p)
{
    if (p->stage != PET_EGG && p->hunger < 150) return PET_TOO_HUNGRY;
    p->mood = clamp(p->mood + 250);
    p->hunger = clamp(p->hunger - 50);
    p->clean = clamp(p->clean - 40);
    p->care = clamp_care(p->care + 2);
    p->xp += 3;
    return PET_OK;
}

pet_result_t pet_wash(pet_t *p)
{
    if (p->clean > 900) return PET_ALREADY_CLEAN;
    p->clean = PET_MAX;
    p->mood = clamp(p->mood + 50);
    p->care = clamp_care(p->care + 2);
    p->xp += 1;
    return PET_OK;
}

void pet_gift(pet_t *p, pet_gift_t gift)
{
    switch (gift) {
    case PET_GIFT_POMODORO:   // 完成一个番茄:像吃了顿好的
        p->hunger = clamp(p->hunger + 200);
        p->mood = clamp(p->mood + 100);
        p->care = clamp_care(p->care + 5);
        p->xp += 5;
        break;
    case PET_GIFT_BREATH:     // 一起冥想:心情平静
        p->mood = clamp(p->mood + 150);
        p->care = clamp_care(p->care + 5);
        p->xp += 3;
        break;
    case PET_GIFT_CHAT:       // 和语音助手聊天:有人说话就开心
        p->mood = clamp(p->mood + 20);
        p->xp += 1;
        break;
    }
}

pet_stage_t pet_stage(const pet_t *p) { return (pet_stage_t)p->stage; }

int pet_level(const pet_t *p) { return 1 + p->xp / 10; }

bool pet_next_need(const pet_t *p, int32_t now_min, int *xp_left, int32_t *min_left)
{
    *xp_left = 0;
    *min_left = 0;
    if (p->stage >= PET_ELDER) return false;
    const int next = p->stage + 1;
    if (p->xp < k_stage_xp[next]) *xp_left = k_stage_xp[next] - p->xp;
    const int32_t waited = now_min - p->stage_min;
    if (waited < k_stage_min[next]) *min_left = k_stage_min[next] - waited;
    return true;
}

bool pet_can_pass_on(const pet_t *p, int32_t now_min)
{
    return p->stage == PET_ELDER && now_min - p->stage_min >= ELDER_PASS_MIN;
}

bool pet_dex_has(uint32_t dex, pet_species_t s, pet_form_t f) { return (dex & dex_bit(s, f)) != 0; }

int pet_dex_count(uint32_t dex)
{
    int n = 0;
    for (int i = 0; i < PET_SPECIES_COUNT * PET_FORM_COUNT; i++) n += (dex >> i) & 1u;
    return n;
}

pet_species_t pet_next_species(uint32_t dex, uint32_t rnd)
{
    int fresh[PET_SPECIES_COUNT], n = 0;
    for (int s = 0; s < PET_SPECIES_COUNT; s++) {
        bool seen = false;
        for (int f = 0; f < PET_FORM_COUNT; f++) seen |= pet_dex_has(dex, (pet_species_t)s, (pet_form_t)f);
        if (!seen) fresh[n++] = s;
    }
    if (n) return (pet_species_t)fresh[rnd % (uint32_t)n];
    return (pet_species_t)(rnd % PET_SPECIES_COUNT);
}

void pet_pass_on(pet_t *p, int32_t now_min, uint32_t rnd)
{
    const uint32_t dex = p->dex;
    const int32_t gen = p->generation;
    pet_init(p, now_min, pet_next_species(dex, rnd));
    p->dex = dex;
    p->generation = gen + 1;
}

pet_status_t pet_status(const pet_t *p, int hour)
{
    if (p->stage == PET_EGG) return PET_FINE;
    if (p->hunger < 300) return PET_HUNGRY;
    if (p->clean < 300) return PET_DIRTY;
    if (p->mood < 300) return PET_SAD;
    if (hour >= 23 || hour < 7) return PET_SLEEPY;
    if (p->hunger >= 600 && p->mood >= 600 && p->clean >= 600) return PET_HAPPY;
    return PET_FINE;
}

int pet_age_days(const pet_t *p, int32_t now_min)
{
    return now_min > p->born_min ? (int)((now_min - p->born_min) / DAY_MIN) : 0;
}

const char *pet_stage_name(pet_stage_t s)
{
    static const char *const names[PET_STAGE_COUNT] = {"蛋", "幼崽", "少年", "成年", "长老"};
    return names[s];
}

const char *pet_species_name(pet_species_t s)
{
    static const char *const names[PET_SPECIES_COUNT] = {"团子", "企鹅", "小恐龙", "兔兔"};
    return names[s];
}
