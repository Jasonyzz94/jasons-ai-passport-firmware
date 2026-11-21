// main/shell.h —— 工具箱外壳:应用生命周期、启动器、状态栏、按键提示、浮层。
//
// 线程模型:shell 与所有应用的回调都在 LVGL 任务里运行(由 lv_timer 驱动),
// 因此应用代码可以直接操作 LVGL 对象,不需要加锁;但回调里绝不能阻塞——
// 网络、音频、蓝牙等慢操作都交给 svc_* 后台服务,应用在 tick 里轮询结果。
//
// 全局按键约定(所有应用一致,底部提示栏会显示):
//   ▲/▼ 单击   移动 / 调整
//   OK  单击   主操作 / 确认
//   长按 ▲     返回上一级(应用内子页先自己处理,返回 false 才退回启动器)
//   长按 OK / 长按 ▼  应用自定义的次要操作
// 息屏时第一次按键只点亮屏幕;提醒浮层出现时任意键先关闭浮层。
#pragma once

#include "lvgl.h"
#include "pixel_types.h"
#include "sys.h"
#include <stdbool.h>

#define APP_FLAG_KEEP_AWAKE 0x1   // 前台时不自动息屏
#define APP_FLAG_KEYS_WHEN_OFF 0x2 // 息屏时按键照常送达应用(遥控器),屏幕保持熄灭

typedef struct app_s {
    const char *name;
    const char *desc;
    const pixel_sprite_t *icon;
    uint32_t color;
    // root 是内容区(240 x UI_CONTENT_H);应用在里面建界面。
    void (*create)(lv_obj_t *root);
    // 停掉自己的服务与动画、清空静态指针。root 由 shell 随后删除。
    void (*destroy)(void);
    // 返回 true 表示已处理;长按 ▲ 未处理时 shell 退回启动器。
    bool (*key)(sys_key_t key, sys_key_ev_t ev);
    // 约每 50ms 一次。
    void (*tick)(int64_t now_ms);
    uint32_t flags;
} app_t;

// 后台服务 tick(无论前台是哪个应用都会调用),例如番茄钟计时。
typedef void (*shell_bg_tick_t)(int64_t now_ms);
// 状态栏中间的"胶囊"(例如后台运行中的番茄钟),返回 false 表示不显示。
typedef bool (*shell_chip_fn_t)(char *text, size_t len, uint32_t *color);

void shell_start(void);          // LVGL 已初始化、持有锁时调用一次
void shell_open(const app_t *app);
void shell_back(void);           // 回到启动器
const app_t *shell_current(void);

void shell_add_bg_tick(shell_bg_tick_t fn);
void shell_set_chip(shell_chip_fn_t fn);

// 底部提示,格式 "键:说明|键:说明",例 "▲▼:选择|OK:打开|长▲:返回"。
void shell_hints(const char *spec);
void shell_toast(const char *text);

// 全屏提醒浮层(到点、来消息等),点亮屏幕,任意键关闭后调用 on_close。
typedef void (*shell_alert_cb_t)(void);
void shell_alert(const pixel_sprite_t *icon, uint32_t color, const char *title,
                 const char *detail, shell_alert_cb_t on_close);
bool shell_alert_active(void);

// 确认对话框:OK 确认,其他键取消。
typedef void (*shell_confirm_cb_t)(bool confirmed);
void shell_confirm(const char *title, const char *detail, shell_confirm_cb_t cb);

// 让屏幕保持常亮/恢复按设置息屏(例如录音、对讲进行中)。
void shell_keep_awake(bool on);
void shell_wake(void);
bool shell_screen_on(void);
void shell_screen_off(void);

// 设置变化后重新应用亮度。
void shell_apply_brightness(void);
