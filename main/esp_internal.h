// main/esp_internal.h —— 固件服务之间共享的内部接口(不给应用层用)。
#pragma once

#include "esp_netif.h"
#include <stdbool.h>

esp_netif_t *net_sta_netif(void);
// 中转服务器的设备令牌(配对后获得,可能为空串)。
const char *relay_token(void);
void relay_start(void);
bool relay_task_running(void);
// 本次联网后是否已完成过一次服务器发现(无论成败)。
bool relay_discovery_attempted(void);
// 当前空闲内存是否够做一次 HTTPS 握手(不够时跳过请求,避免界面因内存耗尽卡死)。
bool local_tls_ok(void);
// 联网功能报告一次访问外网(非局域网地址)的结果:连续失败会触发外网探测,
// 确实不通且附近有别的已保存网络时换网络(esp_net.c)。
void net_report_internet(bool ok);
#define TLS_MIN_FREE (48 * 1024)    // 握手前至少要这么多空闲
#define TLS_MIN_BLOCK (18 * 1024)   // 且最大连续块不小于这个
// 最近一次 local_tls_ok 看到的空闲 / 最大块(KB),以及是否因界面在用应急储备而拒绝。
void local_tls_last(unsigned *free_k, unsigned *largest_k, bool *ui_tight);
// 蓝牙是否正在启动或关闭(过渡中)。Wi-Fi 启动前等它结束。
bool ble_radio_busy(void);
// 蓝牙启动 / 运行期间让出界面的应急内存储备(esp_lv_mem.c)。
void lv_mem_reserve_suspend(bool on);

// 发现协议(与 server/passport_server/discovery.py 保持一致)
#define RELAY_DISCOVERY_PORT 8788
#define RELAY_DISCOVERY_MAGIC "PASSPORT_DISCOVER 1"
#define RELAY_BEACON_PORT 8789          // 服务器每 3 秒向这个端口广播自己的地址
