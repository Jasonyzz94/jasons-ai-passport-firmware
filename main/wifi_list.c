// main/wifi_list.c —— 见 wifi_list.h。
#include "wifi_list.h"

#include <stdio.h>
#include <string.h>

void wifi_list_init(wifi_list_t *l)
{
    memset(l, 0, sizeof(*l));
    l->version = WIFI_LIST_VERSION;
}

static bool terminated(const char *s, size_t cap) { return memchr(s, '\0', cap) != NULL; }

bool wifi_list_valid(const wifi_list_t *l)
{
    if (l->version != WIFI_LIST_VERSION || l->count < 0 || l->count > WIFI_SAVED_MAX) return false;
    for (int i = 0; i < l->count; i++) {
        if (!terminated(l->n[i].ssid, sizeof(l->n[i].ssid)) || !terminated(l->n[i].pass, sizeof(l->n[i].pass))
            || !l->n[i].ssid[0]) {
            return false;
        }
    }
    return true;
}

int wifi_list_find(const wifi_list_t *l, const char *ssid)
{
    for (int i = 0; i < l->count; i++) {
        if (!strcmp(l->n[i].ssid, ssid)) return i;
    }
    return -1;
}

bool wifi_list_promote(wifi_list_t *l, const char *ssid, const char *pass, char evicted[33])
{
    if (evicted) evicted[0] = '\0';
    if (!ssid[0]) return false;
    wifi_cred_t c;
    memset(&c, 0, sizeof(c));
    snprintf(c.ssid, sizeof(c.ssid), "%s", ssid);
    snprintf(c.pass, sizeof(c.pass), "%s", pass);
    int at = wifi_list_find(l, c.ssid);
    bool dropped = false;
    if (at < 0) {
        if (l->count == WIFI_SAVED_MAX) {
            if (evicted) snprintf(evicted, 33, "%s", l->n[WIFI_SAVED_MAX - 1].ssid);
            dropped = true;
            l->count--;
        }
        at = l->count++;
    }
    memmove(&l->n[1], &l->n[0], sizeof(wifi_cred_t) * (size_t)at);
    l->n[0] = c;
    return dropped;
}

bool wifi_list_remove(wifi_list_t *l, const char *ssid)
{
    const int at = wifi_list_find(l, ssid);
    if (at < 0) return false;
    memmove(&l->n[at], &l->n[at + 1], sizeof(wifi_cred_t) * (size_t)(l->count - at - 1));
    l->count--;
    memset(&l->n[l->count], 0, sizeof(wifi_cred_t));
    return true;
}

static bool seen(const char *ssid, const char *const *visible, int nvis)
{
    for (int i = 0; i < nvis; i++) {
        if (visible[i] && !strcmp(visible[i], ssid)) return true;
    }
    return false;
}

// 以前按"当前这个的下一个"取:开机总是先连 n[0],它排在"看得到的"后面时,
// 排在它前面的网络(恰恰是看得到的那个)整轮都不会被试到。现在记下这一轮试过哪些。
int wifi_list_next(const wifi_list_t *l, uint32_t tried, const char *const *visible, int nvis)
{
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < l->count; i++) {
            if (tried & (1u << i)) continue;
            const bool v = nvis == 0 || seen(l->n[i].ssid, visible, nvis);
            if (v == (pass == 0)) return i;
        }
    }
    return -1;
}
