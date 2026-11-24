// main/apps.h —— 所有工具的注册表。启动器按 APPS[] 的顺序展示。
#pragma once

#include "pet.h"
#include "shell.h"

extern const app_t APP_LAUNCHER;
extern const app_t APP_POMODORO;
extern const app_t APP_NOISE;
extern const app_t APP_CLOCK;
extern const app_t APP_AI;
extern const app_t APP_REMOTE;
extern const app_t APP_SOUND;
extern const app_t APP_LINK;
extern const app_t APP_HOME;
extern const app_t APP_BOARD;
extern const app_t APP_WIFI;
extern const app_t APP_TIMER;
extern const app_t APP_DICE;
extern const app_t APP_SETTINGS;
extern const app_t APP_PET;
extern const app_t APP_BREATH;
// 给像素宠物送奖励(番茄完成、冥想完成、和语音助手聊天)。只在 LVGL 任务里调用,会写一次存储。
void pet_give(pet_gift_t gift);

extern const app_t *const APPS[];
extern const int APP_COUNT;

// 后台服务启动(开机时调用一次,注册 bg tick / 状态栏胶囊)。
void pomo_service_init(void);
void link_service_init(void);
void timer_service_init(void);
