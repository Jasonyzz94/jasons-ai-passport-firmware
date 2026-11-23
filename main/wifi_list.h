// main/wifi_list.h —— 已保存的无线网络列表(纯逻辑,无 ESP-IDF 依赖,有主机测试)。
//
// 最多存 WIFI_SAVED_MAX 个,n[0] 是最近一次连上的。连上某个网络就把它移到最前;
// 列表满时加入新网络会挤掉最久没用的(末尾那个)。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WIFI_SAVED_MAX 5
#define WIFI_LIST_VERSION 1

typedef struct {
    char ssid[33];
    char pass[65];   // 空 = 开放网络
} wifi_cred_t;

typedef struct {
    int32_t version;
    int32_t count;
    wifi_cred_t n[WIFI_SAVED_MAX];
} wifi_list_t;

void wifi_list_init(wifi_list_t *l);
// 从存储读出的数据是否可用(版本、数量、字符串结尾都检查)。
bool wifi_list_valid(const wifi_list_t *l);
// 返回下标,没有返回 -1。
int wifi_list_find(const wifi_list_t *l, const char *ssid);
// 加入或更新密码,并移到最前。返回 true 表示因为列表满挤掉了一个(evicted 可为 NULL)。
bool wifi_list_promote(wifi_list_t *l, const char *ssid, const char *pass, char evicted[33]);
bool wifi_list_remove(wifi_list_t *l, const char *ssid);
// 这一轮下一个该试的下标;tried 是这一轮已经试过的下标位图(bit i = n[i]),都试过了返回 -1。
// visible 是最近扫描到的网络名(nvis 为 0 表示没有扫描结果,这时按列表顺序):
// 先试扫描里能看到的,看不到的排在后面;同一组里按列表顺序(最近连上的在前)。
int wifi_list_next(const wifi_list_t *l, uint32_t tried, const char *const *visible, int nvis);
