idf_component_register(
    SRCS "main.c"
         "pomodoro.c"
         "pixel_time.c"
         "pixel_art.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_timer nvs_flash
)
