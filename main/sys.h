// main/sys.h —— 平台抽象层:固件(sys_esp.c)与桌面模拟器(sim/)各自实现。
//
// 应用与 shell 只通过这里接触硬件/系统,这样同一份界面代码既能上板,
// 也能在电脑上渲染截图做视觉验收。所有函数都可在 LVGL 任务上下文调用且不阻塞
// (sys_kv_* 写 NVS 约数毫秒,只在状态真正改变时调用)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SYS_FW_VERSION "1.0.0"

typedef enum {
    SYS_KEY_UP = 0,
    SYS_KEY_DOWN,
    SYS_KEY_OK,
} sys_key_t;

typedef enum {
    SYS_EV_PRESS = 0,
    SYS_EV_CLICK,
    SYS_EV_DOUBLE,
    SYS_EV_LONG,
    SYS_EV_RELEASE,
} sys_key_ev_t;

// 按键事件队列:按键回调(其他任务)入队,shell 在 LVGL 任务里出队。
bool sys_key_pop(sys_key_t *key, sys_key_ev_t *ev);

// 单调毫秒时钟(开机起算)。
int64_t sys_now_ms(void);

// 背光 0..100。
void sys_backlight(uint8_t percent);

// 电量百分比,-1 表示不可用;充电状态硬件未提供,不显示。
int sys_battery_soc(void);
int sys_battery_mv(void);

// 键值存储(固件为 NVS 命名空间 "toolbox")。get 失败返回默认值。
int32_t sys_kv_get_i32(const char *key, int32_t def);
void sys_kv_set_i32(const char *key, int32_t value);
// 读字符串到 buf(含结尾 0);不存在时写空串并返回 false。
bool sys_kv_get_str(const char *key, char *buf, size_t len);
void sys_kv_set_str(const char *key, const char *value);
bool sys_kv_get_blob(const char *key, void *buf, size_t len);
void sys_kv_set_blob(const char *key, const void *buf, size_t len);
// 清除整个应用命名空间(恢复出厂)。
void sys_kv_erase_all(void);

// 32 位随机数(固件用硬件 RNG)。
uint32_t sys_random(void);

// 设备 6 字节 MAC(Wi-Fi STA),以及由其派生的短 ID,如 "A3F2"。
void sys_mac(uint8_t mac[6]);
const char *sys_short_id(void);

// 内存诊断:空闲堆与最大连续块(字节)。
uint32_t sys_free_heap(void);
uint32_t sys_largest_block(void);

void sys_restart(void);
