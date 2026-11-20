// main/esp_lv_mem.c —— LVGL 内存分配(CONFIG_LV_USE_CUSTOM_MALLOC)。
//
// 界面和 Wi-Fi / 蓝牙 / HTTPS 共用一个堆。以前内存被网络挤光时,LVGL 申请失败会在断言里
// 死循环,表现就是"打开天气 / 信息看板时卡死"。这里留一块 16KB 的应急储备:
// 界面申请失败时先释放储备再试一次,保证界面能继续画;联网代码看到储备被动用就不再发起
// HTTPS 请求(local_tls_ok)。内存回升后自动补回储备。
#include "lvgl.h"
#include "src/stdlib/lv_mem.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdbool.h>
#include <stdlib.h>

#define RESERVE_BYTES (16 * 1024)
#define REFILL_FREE_BYTES (64 * 1024)   // 空闲超过这么多时补回储备

static const char *TAG = "lvmem";
static void *s_reserve;
static volatile bool s_used_reserve;

static void refill(void)
{
    if (s_reserve || heap_caps_get_free_size(MALLOC_CAP_8BIT) < REFILL_FREE_BYTES) return;
    s_reserve = malloc(RESERVE_BYTES);
    if (s_reserve && s_used_reserve) {
        s_used_reserve = false;
        ESP_LOGI(TAG, "内存已回升,应急储备已补回");
    }
}

static bool release_reserve(size_t want)
{
    if (!s_reserve) return false;
    free(s_reserve);
    s_reserve = NULL;
    s_used_reserve = true;
    ESP_LOGW(TAG, "界面申请 %u 字节失败,动用应急储备(空闲 %u)", (unsigned)want,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    return true;
}

bool lv_mem_reserve_ok(void) { return s_reserve != NULL; }

void lv_mem_init(void) { s_reserve = malloc(RESERVE_BYTES); }
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    (void)mem;
    (void)bytes;
    return NULL;
}
void lv_mem_remove_pool(lv_mem_pool_t pool) { (void)pool; }

void *lv_malloc_core(size_t size)
{
    void *p = malloc(size);
    if (!p && release_reserve(size)) p = malloc(size);
    return p;
}

void *lv_realloc_core(void *p, size_t new_size)
{
    void *n = realloc(p, new_size);
    if (!n && new_size && release_reserve(new_size)) n = realloc(p, new_size);
    return n;
}

void lv_free_core(void *p)
{
    free(p);
    if (!s_reserve) refill();
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_8BIT);
    mon->total_size = info.total_free_bytes + info.total_allocated_bytes;
    mon->free_size = info.total_free_bytes;
    mon->free_biggest_size = info.largest_free_block;
    mon->free_cnt = info.free_blocks;
    mon->used_cnt = info.allocated_blocks;
    mon->max_used = info.total_free_bytes + info.total_allocated_bytes - info.minimum_free_bytes;
    mon->used_pct = mon->total_size ? (uint8_t)(100 - 100 * info.total_free_bytes / mon->total_size) : 0;
    mon->frag_pct = info.total_free_bytes
                        ? (uint8_t)(100 - 100 * info.largest_free_block / info.total_free_bytes) : 0;
}

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
