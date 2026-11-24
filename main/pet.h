// main/pet.h —— 像素宠物的养成规则(纯逻辑,无 ESP-IDF / LVGL 依赖,有主机测试)。
//
// 三项状态都是 0..1000:饱腹、心情、清洁。每过 10 分钟按固定量下降(离线期间最多补算 3 天)。
// 照顾它(喂食 / 玩耍 / 清洁)获得经验。成长要经验和时间都够:
//   蛋 →(经验 5)幼崽 →(经验 40 且满 1 天)少年 →(经验 150 且满 2 天)成年
//      →(经验 400 且满 4 天)长老 →(满 3 天)可以送它去旅行,留下一颗新蛋(下一代)。
// 一个完整周期至少约 10 天。物种在下蛋时决定(优先图鉴里还没有的);成年时按这段时间的
// 照顾评分决定形态:照顾得好是"闪亮"形态。见过的成年形态记在图鉴里,跨代保留。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PET_VERSION 2
#define PET_MAX 1000

typedef enum { PET_SP_DANGO = 0, PET_SP_PENGUIN, PET_SP_DINO, PET_SP_BUNNY, PET_SPECIES_COUNT } pet_species_t;
typedef enum { PET_EGG = 0, PET_BABY, PET_TEEN, PET_ADULT, PET_ELDER, PET_STAGE_COUNT } pet_stage_t;
typedef enum { PET_FORM_NORMAL = 0, PET_FORM_SHINY, PET_FORM_COUNT } pet_form_t;
typedef enum { PET_HAPPY = 0, PET_FINE, PET_HUNGRY, PET_DIRTY, PET_SAD, PET_SLEEPY } pet_status_t;
typedef enum { PET_OK = 0, PET_FULL, PET_TOO_HUNGRY, PET_ALREADY_CLEAN } pet_result_t;
// 其他功能送给宠物的奖励
typedef enum { PET_GIFT_POMODORO = 0, PET_GIFT_BREATH, PET_GIFT_CHAT } pet_gift_t;

#define PET_CARE_MAX 1000
#define PET_SHINY_CARE 300   // 成年时照顾评分达到这个值就是闪亮形态

typedef struct {
    int32_t version;
    int32_t hunger;      // 饱腹
    int32_t mood;        // 心情
    int32_t clean;       // 清洁
    int32_t xp;          // 经验(这一代)
    int32_t last_min;    // 上次结算的时间(分钟,Unix 时间 / 60)
    int32_t born_min;    // 这一代出生(下蛋)的时间
    int32_t species;     // pet_species_t
    int32_t stage;       // pet_stage_t,只增不减
    int32_t stage_min;   // 进入当前阶段的时间
    int32_t care;        // 照顾评分 -PET_CARE_MAX..PET_CARE_MAX
    int32_t form;        // pet_form_t,成年时决定
    int32_t generation;  // 第几代,从 1 开始
    uint32_t dex;        // 图鉴:bit(species * PET_FORM_COUNT + form) = 见过这个成年形态
} pet_t;

// 第 1 版存档(只有前 7 个字段),用于升级迁移。
typedef struct {
    int32_t version, hunger, mood, clean, xp, last_min, born_min;
} pet_v1_t;

void pet_init(pet_t *p, int32_t now_min, pet_species_t species);
// 载入的数据无效(版本不对 / 数值越界)时返回 false,调用方应重新 pet_init。
bool pet_valid(const pet_t *p);
// 把第 1 版存档升级成当前版本(物种为团子,按经验折算阶段)。旧存档无效时返回 false。
bool pet_migrate_v1(const pet_v1_t *old, int32_t now_min, pet_t *out);
// 结算从上次到现在的状态变化(含照顾评分)。
void pet_tick(pet_t *p, int32_t now_min);
// 经验和时间都够就进化一级。返回 true 表示阶段变了(界面应庆祝一下)。
bool pet_evolve(pet_t *p, int32_t now_min);

pet_result_t pet_feed(pet_t *p);
pet_result_t pet_play(pet_t *p);
pet_result_t pet_wash(pet_t *p);
// 其他功能完成时的奖励(番茄 / 冥想 / 和语音助手聊天)。
void pet_gift(pet_t *p, pet_gift_t gift);

pet_stage_t pet_stage(const pet_t *p);
int pet_level(const pet_t *p);
// hour 为本地小时(0..23),夜里会犯困。
pet_status_t pet_status(const pet_t *p, int hour);
int pet_age_days(const pet_t *p, int32_t now_min);
// 进化到下一阶段还差的经验和分钟数(都不差时为 0);已是长老返回 false。
bool pet_next_need(const pet_t *p, int32_t now_min, int *xp_left, int32_t *min_left);
// 长老满 3 天,可以送它去旅行了。
bool pet_can_pass_on(const pet_t *p, int32_t now_min);
// 送它去旅行:留下一颗新蛋,代数 +1,图鉴保留。rnd 是随机数(决定下一个物种)。
void pet_pass_on(pet_t *p, int32_t now_min, uint32_t rnd);
// 下一颗蛋的物种:优先图鉴里一个形态都还没有的物种。
pet_species_t pet_next_species(uint32_t dex, uint32_t rnd);
bool pet_dex_has(uint32_t dex, pet_species_t s, pet_form_t f);
int pet_dex_count(uint32_t dex);

const char *pet_stage_name(pet_stage_t s);
const char *pet_species_name(pet_species_t s);
