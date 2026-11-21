// tests/test_pet.c —— 像素宠物规则的主机测试。
#include "pet.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    pet_t p;
    pet_init(&p, 1000);
    assert(pet_valid(&p) && pet_stage(&p) == PET_EGG && pet_level(&p) == 1);
    // 蛋不会饿
    pet_tick(&p, 1000 + 600);
    assert(p.hunger == 700 && p.last_min == 1600);
    // 照顾几次孵化
    assert(pet_wash(&p) == PET_ALREADY_CLEAN);
    assert(pet_play(&p) == PET_OK);   // xp 3
    assert(pet_feed(&p) == PET_OK);   // xp 5 → 幼崽
    assert(pet_stage(&p) == PET_BABY);
    assert(pet_feed(&p) == PET_FULL);
    // 10 小时后:60 个结算周期
    const int h0 = p.hunger, c0 = p.clean;
    pet_tick(&p, 1600 + 600 + 5);     // 不满 10 分钟的零头留到下次
    assert(p.hunger == h0 - 240 && p.clean == c0 - 180 && p.last_min == 2200);
    // 离线很久最多补算 3 天,并且不会低于 0
    pet_tick(&p, 2200 + 30 * 24 * 60);
    assert(p.hunger == 0 && p.clean == 0 && p.mood == 0);
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
    // 奖励与成长
    pet_reward(&p, 30);
    assert(pet_stage(&p) == PET_ADULT && pet_xp_to_next(&p) == 0 && p.hunger == PET_MAX);
    assert(pet_age_days(&p, p.born_min + 3 * 24 * 60 + 5) == 3);
    // 坏数据
    p.version = 99;
    assert(!pet_valid(&p));
    puts("pet tests: PASS");
    return 0;
}
