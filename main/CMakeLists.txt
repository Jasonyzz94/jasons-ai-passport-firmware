# 工具箱固件。界面代码(app_*.c、shell、ui)与模拟器共用;esp_*.c 只在固件里编译。
file(GLOB TOOLBOX_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/app_*.c"
    "${CMAKE_CURRENT_LIST_DIR}/esp_*.c")

idf_component_register(
    SRCS ${TOOLBOX_SRCS}
         "shell.c" "ui.c" "icons.c" "pixel_art.c" "settings.c"
         "pomodoro.c" "audio_dsp.c" "hid_keymap.c" "link_proto.c" "weather_shape.c" "noise_gen.c" "ics_parse.c" "pet.c" "breath.c" "app_pet.c" "app_breath.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_timer nvs_flash esp_wifi esp_netif esp_event esp_http_client
             esp_http_server json lwip bt esp_hid mbedtls
)

# 生成的中文/数字字库(tools/gen_fonts.py)
file(GLOB TOOLBOX_FONTS "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/*.c")
target_sources(${COMPONENT_LIB} PRIVATE ${TOOLBOX_FONTS})
