// sim/sim.h —— 桌面模拟器控制接口:场景脚本用它设置假数据、按键、推进时间。
#pragma once

#include "svc.h"
#include "sys.h"

extern int64_t g_sim_now_ms;
extern int g_sim_backlight;
extern int g_sim_soc;

typedef struct {
    bool time_ok;
    struct tm tm;
    net_state_t net;
    srv_state_t srv;
    bool relay_fail;
    scan_state_t scan;
    portal_state_t portal;
    ble_state_t ble;
    bool link_on;
    bool link_paired;
    int link_nearby;
    ai_state_t ai;
    bool ai_has_result;
    float level;
    float pitch;
    bool audio_ok;
} sim_fake_t;

extern sim_fake_t g_fake;

void sim_key(sys_key_t k, sys_key_ev_t e);
void sim_fake_link_event(link_ev_type_t type, const char *text, int value);
