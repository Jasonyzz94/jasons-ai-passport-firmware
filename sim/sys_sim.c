// sim/sys_sim.c —— 桌面模拟器的平台层:时间可控、内存键值、按键队列。
#include "sim.h"
#include "sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int64_t g_sim_now_ms = 1000;
int g_sim_backlight = 100;
int g_sim_soc = 76;

#define KQ 32
static struct { sys_key_t k; sys_key_ev_t e; } s_q[KQ];
static int s_qh, s_qt;

void sim_key(sys_key_t k, sys_key_ev_t e)
{
    s_q[s_qt % KQ].k = k;
    s_q[s_qt % KQ].e = e;
    s_qt++;
}

bool sys_key_pop(sys_key_t *key, sys_key_ev_t *ev)
{
    if (s_qh == s_qt) return false;
    *key = s_q[s_qh % KQ].k;
    *ev = s_q[s_qh % KQ].e;
    s_qh++;
    return true;
}

int64_t sys_now_ms(void) { return g_sim_now_ms; }
void sys_backlight(uint8_t p) { g_sim_backlight = p; }
int sys_battery_soc(void) { return g_sim_soc; }
int sys_battery_mv(void) { return 3950; }

#define KV_MAX 64
typedef struct {
    char key[16];
    int is_str;
    int32_t i;
    char s[128];
    size_t blen;
} kv_t;
static kv_t s_kv[KV_MAX];

static kv_t *kv_find(const char *key, int create)
{
    for (int i = 0; i < KV_MAX; i++) {
        if (s_kv[i].key[0] && strcmp(s_kv[i].key, key) == 0) return &s_kv[i];
    }
    if (!create) return NULL;
    for (int i = 0; i < KV_MAX; i++) {
        if (!s_kv[i].key[0]) {
            snprintf(s_kv[i].key, sizeof(s_kv[i].key), "%s", key);
            return &s_kv[i];
        }
    }
    return NULL;
}

int32_t sys_kv_get_i32(const char *key, int32_t def)
{
    kv_t *kv = kv_find(key, 0);
    return kv ? kv->i : def;
}
void sys_kv_set_i32(const char *key, int32_t v)
{
    kv_t *kv = kv_find(key, 1);
    if (kv) kv->i = v;
}
bool sys_kv_get_str(const char *key, char *buf, size_t len)
{
    kv_t *kv = kv_find(key, 0);
    if (!kv) {
        if (len) buf[0] = 0;
        return false;
    }
    snprintf(buf, len, "%s", kv->s);
    return true;
}
void sys_kv_set_str(const char *key, const char *v)
{
    kv_t *kv = kv_find(key, 1);
    if (kv) snprintf(kv->s, sizeof(kv->s), "%s", v);
}
bool sys_kv_get_blob(const char *key, void *buf, size_t len)
{
    kv_t *kv = kv_find(key, 0);
    if (!kv || kv->blen != len) return false;
    memcpy(buf, kv->s, len);
    return true;
}
void sys_kv_set_blob(const char *key, const void *buf, size_t len)
{
    kv_t *kv = kv_find(key, 1);
    if (kv && len <= sizeof(kv->s)) {
        memcpy(kv->s, buf, len);
        kv->blen = len;
    }
}
void sys_kv_erase_all(void) { memset(s_kv, 0, sizeof(s_kv)); }

uint32_t sys_random(void) { return (uint32_t)rand(); }
void sys_mac(uint8_t mac[6])
{
    const uint8_t m[6] = {0x34, 0x85, 0x18, 0x7A, 0xA3, 0xF2};
    memcpy(mac, m, 6);
}
const char *sys_short_id(void) { return "A3F2"; }
uint32_t sys_free_heap(void) { return 148 * 1024; }
uint32_t sys_largest_block(void) { return 92 * 1024; }
void sys_restart(void) { printf("sim: restart requested\n"); }
