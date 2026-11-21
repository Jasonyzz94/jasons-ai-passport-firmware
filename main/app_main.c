// main/app_main.c —— 固件入口:初始化 BSP 与服务,然后把界面交给 shell(运行在 LVGL 任务里)。
#include "apps.h"
#include "settings.h"
#include "svc.h"
#include "sys.h"
#include "ui.h"
#include "esp_internal.h"

#include "bsp_display.h"
#include "bsp_i2c.h"
#include "esp_log.h"

static const char *TAG = "toolbox";

esp_err_t sys_esp_init(void);

void app_main(void)
{
    ESP_LOGI(TAG, "工具箱 v%s 启动", SYS_FW_VERSION);
    bsp_i2c_init();
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示初始化失败,无法继续");
        return;
    }
    bsp_display_backlight(0);   // 首帧画好后再亮,避免花屏

    if (sys_esp_init() != ESP_OK) ESP_LOGE(TAG, "按键初始化失败,界面可显示但无法操作");
    settings_load();
    time_apply_tz(g_settings.tz);
    audio_svc_init();
    net_init();
    relay_start();

    if (!bsp_lvgl_lock(2000)) {
        ESP_LOGE(TAG, "LVGL 锁获取失败");
        return;
    }
    ui_init();
    pomo_service_init();
    link_service_init();
    timer_service_init();
    shell_start();
    bsp_lvgl_unlock();

    sys_backlight((uint8_t)g_settings.brightness);
    ESP_LOGI(TAG, "就绪:空闲内存 %u,最大块 %u", (unsigned)sys_free_heap(),
             (unsigned)sys_largest_block());
}
