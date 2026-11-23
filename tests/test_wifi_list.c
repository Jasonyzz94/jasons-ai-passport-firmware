// tests/test_wifi_list.c —— 已保存无线网络列表的主机测试。
#include "wifi_list.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_promote(void)
{
    wifi_list_t l;
    wifi_list_init(&l);
    assert(wifi_list_valid(&l) && l.count == 0);
    assert(!wifi_list_promote(&l, "", "x", NULL));   // 空名字不收
    assert(l.count == 0);
    wifi_list_promote(&l, "home", "p1", NULL);
    wifi_list_promote(&l, "office", "p2", NULL);
    assert(l.count == 2 && !strcmp(l.n[0].ssid, "office") && !strcmp(l.n[1].ssid, "home"));
    // 已有的:更新密码并移到最前,不重复
    wifi_list_promote(&l, "home", "p1new", NULL);
    assert(l.count == 2 && !strcmp(l.n[0].ssid, "home") && !strcmp(l.n[0].pass, "p1new"));
    assert(!strcmp(l.n[1].ssid, "office"));
    // 满了挤掉最久没用的
    wifi_list_promote(&l, "a", "", NULL);
    wifi_list_promote(&l, "b", "", NULL);
    wifi_list_promote(&l, "c", "", NULL);
    assert(l.count == WIFI_SAVED_MAX);
    char ev[33];
    assert(wifi_list_promote(&l, "d", "pd", ev));
    assert(!strcmp(ev, "office") && l.count == WIFI_SAVED_MAX && wifi_list_find(&l, "office") < 0);
    assert(!strcmp(l.n[0].ssid, "d"));
    // 满了但更新已有的:不挤
    assert(!wifi_list_promote(&l, "home", "p1", ev) && ev[0] == '\0');
    assert(wifi_list_valid(&l));
}

static void test_remove(void)
{
    wifi_list_t l;
    wifi_list_init(&l);
    wifi_list_promote(&l, "x", "", NULL);
    wifi_list_promote(&l, "y", "", NULL);
    wifi_list_promote(&l, "z", "", NULL);   // z y x
    assert(wifi_list_remove(&l, "y"));
    assert(l.count == 2 && !strcmp(l.n[0].ssid, "z") && !strcmp(l.n[1].ssid, "x"));
    assert(!wifi_list_remove(&l, "nope"));
    assert(l.n[2].ssid[0] == '\0');
}

static void test_valid(void)
{
    wifi_list_t l;
    wifi_list_init(&l);
    wifi_list_promote(&l, "ok", "", NULL);
    l.version = 99;
    assert(!wifi_list_valid(&l));
    l.version = WIFI_LIST_VERSION;
    l.count = WIFI_SAVED_MAX + 1;
    assert(!wifi_list_valid(&l));
    l.count = 1;
    memset(l.n[0].ssid, 'A', sizeof(l.n[0].ssid));   // 没有结尾 0
    assert(!wifi_list_valid(&l));
}

static void test_next(void)
{
    wifi_list_t l;
    wifi_list_init(&l);
    wifi_list_promote(&l, "c", "", NULL);
    wifi_list_promote(&l, "b", "", NULL);
    wifi_list_promote(&l, "a", "", NULL);   // a b c(a 最近连上)
    // 没有扫描结果:按列表顺序,试过的跳过
    assert(wifi_list_next(&l, 0, NULL, 0) == 0);
    assert(wifi_list_next(&l, 1u << 0, NULL, 0) == 1);
    assert(wifi_list_next(&l, (1u << 0) | (1u << 1), NULL, 0) == 2);
    assert(wifi_list_next(&l, 7, NULL, 0) == -1);
    // 只看得到 c:先试 c
    const char *vis[] = {"other", "c"};
    assert(wifi_list_next(&l, 0, vis, 2) == 2);
    // 曾经的 bug:先连了最近的 a(看不到),下一个必须是看得到的 c,而不是 b
    assert(wifi_list_next(&l, 1u << 0, vis, 2) == 2);
    assert(wifi_list_next(&l, (1u << 0) | (1u << 2), vis, 2) == 1);
    assert(wifi_list_next(&l, 7, vis, 2) == -1);
    // 每个网络一轮里都会被试到
    uint32_t tried = 1u << 0;
    int n = 1;
    for (int k; (k = wifi_list_next(&l, tried, vis, 2)) >= 0; n++) tried |= 1u << k;
    assert(n == 3 && tried == 7);
    wifi_list_t empty;
    wifi_list_init(&empty);
    assert(wifi_list_next(&empty, 0, NULL, 0) == -1);
}

int main(void)
{
    test_promote();
    test_remove();
    test_valid();
    test_next();
    puts("wifi list tests: PASS");
    return 0;
}
