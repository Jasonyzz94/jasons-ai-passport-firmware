// main/pet.h —— 像素宠物的养成规则(纯逻辑,无 ESP-IDF / LVGL 依赖,有主机测试)。
//
// 三项状态都是 0..1000:饱腹、心情、清洁。每过 10 分钟按固定量下降(离线期间最多补算 3 天)。
// 照顾它(喂食 / 玩耍 / 清洁)获得经验,经验决定成长阶段:蛋 → 幼崽 → 少年 → 成年。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PET_VERSION 1
#define PET_MAX 1000

typedef struct {
    int32_t version;
    int32_t hunger;      // 饱腹
    int32_t mood;        // 心情
    int32_t clean;       // 清洁
    int32_t xp;          // 经验
    int32_t last_min;    // 上次结算的时间(分钟,Unix 时间 / 60)
    int32_t born_min;    // 出生时间(分钟)
} pet_t;

typedef enum { PET_EGG = 0, PET_BABY, PET_TEEN, PET_ADULT } pet_stage_t;
typedef enum { PET_HAPPY = 0, PET_FINE, PET_HUNGRY, PET_DIRTY, PET_SAD, PET_SLEEPY } pet_status_t;
typedef enum { PET_OK = 0, PET_FULL, PET_TOO_HUNGRY, PET_ALREADY_CLEAN } pet_result_t;

void pet_init(pet_t *p, int32_t now_min);
// 载入的数据无效(版本不对 / 数值越界)时返回 false,调用方应重新 pet_init。
bool pet_valid(const pet_t *p);
// 结算从上次到现在的状态变化。
void pet_tick(pet_t *p, int32_t now_min);

pet_result_t pet_feed(pet_t *p);
pet_result_t pet_play(pet_t *p);
pet_result_t pet_wash(pet_t *p);
// 预留:完成番茄等奖励(目前没有接入其他功能)。
void pet_reward(pet_t *p, int count);

pet_stage_t pet_stage(const pet_t *p);
int pet_level(const pet_t *p);
// hour 为本地小时(0..23),夜里会犯困。
pet_status_t pet_status(const pet_t *p, int hour);
int pet_age_days(const pet_t *p, int32_t now_min);
// 距下一阶段还差多少经验;已成年返回 0。
int pet_xp_to_next(const pet_t *p);
const char *pet_stage_name(pet_stage_t s);
