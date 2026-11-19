// main/esp_internal.h —— 固件服务之间共享的内部接口(不给应用层用)。
#pragma once

#include "esp_netif.h"
#include <stdbool.h>

esp_netif_t *net_sta_netif(void);
// 中转服务器的设备令牌(配对后获得,可能为空串)。
const char *relay_token(void);
void relay_start(void);
// 本次联网后是否已完成过一次服务器发现(无论成败)。
bool relay_discovery_attempted(void);

// 发现协议(与 server/passport_server/discovery.py 保持一致)
#define RELAY_DISCOVERY_PORT 8788
#define RELAY_DISCOVERY_MAGIC "PASSPORT_DISCOVER 1"
#define RELAY_BEACON_PORT 8789          // 服务器每 3 秒向这个端口广播自己的地址
