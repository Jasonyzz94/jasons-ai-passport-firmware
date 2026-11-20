// sim/scenario.c —— 截图脚本:逐个界面/状态渲染。新增界面时在这里加一步。
#include "apps.h"
#include "settings.h"
#include "sim.h"

#include <string.h>

#define CLICK(k) key(k, SYS_EV_CLICK)
#define LONG(k)  key(k, SYS_EV_LONG)
#define UP   SYS_KEY_UP
#define DOWN SYS_KEY_DOWN
#define OK   SYS_KEY_OK

void sim_scenario(void (*shot)(const char *), void (*key)(sys_key_t, sys_key_ev_t),
                  void (*run_ms)(int), void (*open_app)(const app_t *))
{
    g_settings.screen_timeout = SETTINGS_TIMEOUT_COUNT - 1;   // 截图时不自动息屏

    shot("00_launcher");
    CLICK(DOWN);
    shot("01_launcher_clock");

    // ---- 番茄钟 ----
    open_app(&APP_POMODORO);
    shot("10_pomo_ready");
    CLICK(OK);
    run_ms(7 * 60 * 1000 + 300);
    shot("11_pomo_running");
    CLICK(OK);
    shot("12_pomo_paused");
    LONG(OK);
    shot("13_pomo_menu");
    CLICK(DOWN); CLICK(DOWN);
    CLICK(OK);
    shot("13a_pomo_noise");
    CLICK(DOWN); CLICK(OK);
    run_ms(1500);
    shot("13b_pomo_noise_playing");
    LONG(UP);
    CLICK(DOWN); CLICK(DOWN);
    CLICK(OK);
    shot("14_pomo_stats");
    LONG(UP);
    CLICK(DOWN); CLICK(DOWN); CLICK(DOWN); CLICK(DOWN); CLICK(DOWN);
    CLICK(OK);
    CLICK(OK);
    CLICK(UP);
    shot("15_pomo_settings_edit");
    CLICK(OK);
    LONG(UP);
    LONG(UP);
    CLICK(OK);                       // 继续计时
    run_ms(19 * 60 * 1000);
    shot("16_pomo_alert");
    CLICK(OK);
    LONG(UP);
    shot("17_launcher_with_chip");
    open_app(&APP_NOISE);
    shot("18_noise_app");
    LONG(UP);

    // ---- 时钟天气 ----
    open_app(&APP_CLOCK);
    shot("20_clock");
    CLICK(DOWN);
    shot("21_clock_hourly");
    LONG(OK);
    shot("22_clock_night");
    LONG(OK);
    g_fake.srv = SRV_UNREACHABLE;
    LONG(UP);

    // ---- 信息看板 / 家居(离线状态)----
    open_app(&APP_BOARD);
    shot("30_board_offline");
    LONG(UP);
    g_fake.srv = SRV_OK;
    open_app(&APP_BOARD);
    shot("31_board");
    CLICK(DOWN);
    shot("32_board_2");
    LONG(UP);
    open_app(&APP_HOME);
    shot("40_home");
    CLICK(DOWN);
    CLICK(OK);
    shot("41_home_toggled");
    LONG(UP);

    // ---- Wi-Fi 分析 ----
    open_app(&APP_WIFI);
    shot("50_wifi");
    LONG(UP);

    // ---- AI ----
    open_app(&APP_AI);
    shot("60_ai_empty");
    key(OK, SYS_EV_PRESS);
    run_ms(100);
    shot("61_ai_listening");
    run_ms(600);
    key(OK, SYS_EV_RELEASE);
    shot("62_ai_thinking");
    g_fake.ai = AI_IDLE;
    g_fake.ai_has_result = true;
    shot("63_ai_answer");
    LONG(UP);

    // ---- 遥控 ----
    open_app(&APP_REMOTE);
    shot("70_remote_modes");
    for (int i = 0; i < 5; i++) CLICK(DOWN);
    CLICK(OK);
    CLICK(DOWN);
    shot("70b_remote_pick_voice");
    LONG(UP);
    CLICK(DOWN);
    CLICK(OK);
    shot("71_remote_vibe");
    key(OK, SYS_EV_DOUBLE);
    CLICK(DOWN); CLICK(DOWN);
    shot("72_remote_phrases");
    LONG(UP);
    LONG(UP);
    CLICK(DOWN);
    CLICK(OK);
    shot("73_remote_slides");
    LONG(UP);
    LONG(UP);
    g_fake.ble = BLE_ADVERTISING;
    open_app(&APP_REMOTE);
    shot("74_remote_pairing");
    g_fake.ble = BLE_OFF;
    LONG(UP);

    // ---- 声音 ----
    open_app(&APP_SOUND);
    shot("80_sound_menu");
    CLICK(OK);
    shot("81_spectrum");
    LONG(UP);
    CLICK(DOWN);
    CLICK(OK);
    shot("82_tuner");
    g_fake.pitch = 446.0f;
    run_ms(300);
    shot("83_tuner_sharp");
    LONG(UP);
    CLICK(DOWN);
    CLICK(OK);
    CLICK(OK);
    run_ms(500);
    shot("84_metronome");
    LONG(UP);
    LONG(UP);

    // ---- 互联 ----
    open_app(&APP_LINK);
    sim_fake_link_event(LINK_EV_TEXT, "晚上一起吃饭吗?", 0);
    run_ms(200);
    shot("90_link_main");
    CLICK(OK);
    shot("91_link_quick");
    LONG(UP);
    LONG(UP);
    g_fake.link_paired = false;
    open_app(&APP_LINK);
    run_ms(1600);
    shot("92_link_nearby");
    sim_fake_link_event(LINK_EV_PAIR_REQUEST, NULL, 0);
    run_ms(200);
    shot("93_link_pair_req");
    CLICK(DOWN);
    LONG(UP);
    g_fake.link_paired = true;
    g_fake.link_on = true;
    g_fake.ble = BLE_CONNECTED;
    sim_fake_link_event(LINK_EV_FOCUS, NULL, 25);
    run_ms(200);
    shot("94_focus_invite");
    CLICK(UP);

    // ---- 计时器 / 随机 / 设置 ----
    open_app(&APP_TIMER);
    CLICK(OK);
    run_ms(83 * 1000 + 400);
    CLICK(DOWN);
    run_ms(12 * 1000);
    shot("A0_stopwatch");
    CLICK(UP);
    CLICK(OK);
    run_ms(65 * 1000);
    shot("A1_countdown");
    LONG(UP);
    open_app(&APP_DICE);
    shot("B0_dice");
    CLICK(OK);
    run_ms(200);
    shot("B1_dice_rolling");
    run_ms(800);
    CLICK(DOWN);
    shot("B2_coin");
    CLICK(DOWN);
    CLICK(DOWN);
    shot("B3_food");
    CLICK(OK); run_ms(1200);
    shot("B4_food_result");
    CLICK(DOWN); CLICK(DOWN); CLICK(OK); run_ms(1200);
    shot("B5_answer");
    CLICK(DOWN); CLICK(OK); run_ms(1200);
    shot("B6_fortune");
    CLICK(DOWN); CLICK(OK); run_ms(1200);
    shot("B7_rps");
    CLICK(DOWN); CLICK(OK); run_ms(1200);
    shot("B8_activity");
    LONG(UP);
    open_app(&APP_SETTINGS);
    shot("C0_settings");
    CLICK(DOWN); CLICK(DOWN);
    CLICK(OK);
    CLICK(DOWN);
    shot("C1_settings_edit");
    CLICK(OK);
    CLICK(UP); CLICK(UP);
    CLICK(OK);
    shot("C2_portal");
    LONG(UP);
    for (int i = 0; i < 6; i++) CLICK(DOWN);
    CLICK(OK);
    shot("C3_about");
    LONG(UP);
    CLICK(DOWN); CLICK(DOWN);
    CLICK(OK);
    shot("C4_confirm_reset");
    CLICK(UP);
}
