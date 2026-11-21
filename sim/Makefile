# sim/Makefile —— 桌面模拟器构建。只编译与硬件无关的界面代码 + LVGL + 假服务。
ROOT := $(abspath ..)
OBJ := $(ROOT)/build/sim-obj
LVGL := $(ROOT)/managed_components/lvgl__lvgl
IDF_JSON := $(firstword $(wildcard $(IDF_PATH)/components/json/cJSON) $(wildcard $(ROOT)/sim/cjson))

# 固件专用文件(esp_*.c、app_main.c)与基线 demo 不参与模拟器构建。
APP_SRCS := $(filter-out $(ROOT)/main/esp_% $(ROOT)/main/app_main.c $(ROOT)/main/demo_% \
                $(ROOT)/main/ui_pixel% $(ROOT)/main/main.c, $(wildcard $(ROOT)/main/*.c))
SRCS := $(APP_SRCS) $(wildcard $(ROOT)/sim/*.c) $(wildcard $(ROOT)/assets/fonts/*.c) \
        $(ROOT)/components/bsp/src/bsp_display_rounding.c \
        $(shell find $(LVGL)/src -name '*.c')

CFLAGS := -O1 -g -std=gnu11 -w -DLV_CONF_INCLUDE_SIMPLE -DTOOLBOX_SIM \
          -I$(ROOT)/sim -I$(ROOT)/main -I$(LVGL) -I$(ROOT)/components/bsp/src \
          -I$(ROOT)/components/bsp/include -I$(IDF_JSON)

OBJS := $(patsubst $(ROOT)/%.c,$(OBJ)/%.o,$(SRCS)) $(OBJ)/ext/cJSON.o

$(OBJ)/toolbox-sim: $(OBJS)
	@$(CC) -o $@ $^ -lm

$(OBJ)/%.o: $(ROOT)/%.c $(ROOT)/sim/lv_conf.h
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -MMD -c $< -o $@

$(OBJ)/ext/cJSON.o: $(IDF_JSON)/cJSON.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

-include $(OBJS:.o=.d)
