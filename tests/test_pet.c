// tests/test_pet.c —— 像素宠物规则的主机测试。
#include "pet.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define DAY (24 * 60)

static void test_care_basics(void)
{
    pet_t p;
    pet_init(&p, 1000, PET_SP_DANGO);
    assert(pet_valid(&p) && pet_stage(&p) == PET_EGG && pet_level(&p) == 1 && p.generation == 1);
    // 蛋不会饿
    pet_tick(&p, 1000 + 600);
    assert(p.hunger == 700 && p.last_min == 1600);
    // 照顾几次孵化(孵化不需要等时间)
    assert(pet_wash(&p) == PET_ALREADY_CLEAN);
    assert(pet_play(&p) == PET_OK);   // xp 3
    assert(!pet_evolve(&p, 1600));
    assert(pet_feed(&p) == PET_OK);   // xp 5
    assert(pet_evolve(&p, 1600) && pet_stage(&p) == PET_BABY);
    assert(pet_feed(&p) == PET_FULL);
    // 10 小时后:60 个结算周期
    const int h0 = p.hunger, c0 = p.clean;
    pet_tick(&p, 1600 + 600 + 5);     // 不满 10 分钟的零头留到下次
    assert(p.hunger == h0 - 240 && p.clean == c0 - 180 && p.last_min == 2200);
    // 离线很久最多补算 3 天,并且不会低于 0;照顾评分变差
    pet_tick(&p, 2200 + 30 * DAY);
    assert(p.hunger == 0 && p.clean == 0 && p.mood == 0 && p.care < 0);
    assert(pet_status(&p, 12) == PET_HUNGRY);
    assert(pet_play(&p) == PET_TOO_HUNGRY);
    pet_feed(&p);
    pet_feed(&p);
    assert(pet_status(&p, 12) == PET_DIRTY);
    pet_wash(&p);
    assert(pet_status(&p, 12) == PET_SAD);
    pet_play(&p);
    pet_play(&p);
    assert(pet_status(&p, 2) == PET_SLEEPY);
    // 时钟回拨:不倒扣
    const pet_t before = p;
    pet_tick(&p, 100);
    assert(p.hunger == before.hunger && p.last_min == 100);
    // 坏数据
    p.version = 99;
    assert(!pet_valid(&p));
}

// 一个完整的成长周期:经验和时间都够才进化;照顾得好成年时是闪亮形态,记进图鉴。
static void test_lifecycle(void)
{
    pet_t p;
    int32_t t = 0;
    pet_init(&p, t, PET_SP_PENGUIN);
    for (int i = 0; i < 2; i++) pet_play(&p);
    assert(pet_evolve(&p, t) && p.stage == PET_BABY);
    // 经验够了但不满 1 天:不进化,并告诉界面还差多久
    for (int i = 0; i < 20; i++) pet_gift(&p, PET_GIFT_BREATH);
    int xp_left;
    int32_t min_left;
    assert(!pet_evolve(&p, t + DAY - 1));
    assert(pet_next_need(&p, t + DAY - 1, &xp_left, &min_left) && xp_left == 0 && min_left == 1);
    assert(pet_evolve(&p, t + DAY) && p.stage == PET_TEEN);
    // 少年 → 成年:经验 150 且满 2 天;照顾评分高 → 闪亮
    p.care = PET_SHINY_CARE;
    for (int i = 0; i < 30; i++) pet_gift(&p, PET_GIFT_POMODORO);
    assert(!pet_evolve(&p, t + DAY + 2 * DAY - 1));
    assert(pet_evolve(&p, t + 3 * DAY) && p.stage == PET_ADULT && p.form == PET_FORM_SHINY);
    assert(pet_dex_has(p.dex, PET_SP_PENGUIN, PET_FORM_SHINY) && pet_dex_count(p.dex) == 1);
    // 成年 → 长老:经验 400 且满 4 天
    for (int i = 0; i < 60; i++) pet_gift(&p, PET_GIFT_POMODORO);
    assert(pet_evolve(&p, t + 7 * DAY) && p.stage == PET_ELDER);
    assert(!pet_evolve(&p, t + 100 * DAY));
    assert(!pet_next_need(&p, t + 7 * DAY, &xp_left, &min_left));
    // 长老满 3 天可以送去旅行:新蛋、下一代、图鉴保留,优先没见过的物种
    assert(!pet_can_pass_on(&p, t + 10 * DAY - 1));
    assert(pet_can_pass_on(&p, t + 10 * DAY));
    pet_pass_on(&p, t + 10 * DAY, 7);
    assert(p.stage == PET_EGG && p.generation == 2 && p.xp == 0 && p.species != PET_SP_PENGUIN);
    assert(pet_dex_has(p.dex, PET_SP_PENGUIN, PET_FORM_SHINY) && pet_valid(&p));
}

static void test_normal_form_and_species_choice(void)
{
    pet_t p;
    pet_init(&p, 0, PET_SP_DINO);
    p.stage = PET_TEEN;
    p.xp = 150;
    p.care = PET_SHINY_CARE - 1;
    assert(pet_evolve(&p, 2 * DAY) && p.form == PET_FORM_NORMAL);
    assert(pet_dex_has(p.dex, PET_SP_DINO, PET_FORM_NORMAL) && !pet_dex_has(p.dex, PET_SP_DINO, PET_FORM_SHINY));
    // 下一个物种只在没见过的里面挑
    for (uint32_t r = 0; r < 20; r++) assert(pet_next_species(p.dex, r) != PET_SP_DINO);
    // 全都见过:四种都可能
    const uint32_t all = 0x55;   // 每个物种的普通形态
    int seen[PET_SPECIES_COUNT] = {0};
    for (uint32_t r = 0; r < 8; r++) seen[pet_next_species(all, r)] = 1;
    assert(seen[0] && seen[1] && seen[2] && seen[3]);
}

static void test_migrate_v1(void)
{
    const pet_v1_t old = {1, 500, 600, 700, 160, 100, 50};   // 旧版成年(经验 >= 150)
    pet_t p;
    assert(pet_migrate_v1(&old, 1000, &p) && pet_valid(&p));
    assert(p.species == PET_SP_DANGO && p.stage == PET_ADULT && p.hunger == 500 && p.xp == 160);
    assert(p.stage_min == 1000 && p.generation == 1 && pet_dex_has(p.dex, PET_SP_DANGO, PET_FORM_NORMAL));
    const pet_v1_t egg = {1, 700, 700, 1000, 2, 100, 100};
    assert(pet_migrate_v1(&egg, 1000, &p) && p.stage == PET_EGG && p.dex == 0);
    const pet_v1_t bad = {1, 2000, 0, 0, 0, 0, 0};
    assert(!pet_migrate_v1(&bad, 1000, &p));
}

int main(void)
{
    test_care_basics();
    test_lifecycle();
    test_normal_form_and_species_choice();
    test_migrate_v1();
    assert(!strcmp(pet_stage_name(PET_ELDER), "长老") && !strcmp(pet_species_name(PET_SP_DINO), "小恐龙"));
    puts("pet tests: PASS");
    return 0;
}
