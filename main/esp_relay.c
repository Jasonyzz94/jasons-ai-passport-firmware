// main/esp_relay.c —— 中转服务器客户端:局域网自动发现、配对取令牌、排队执行 HTTP 请求。
//
// 发现:向 255.255.255.255:8788 广播 "PASSPORT_DISCOVER 1 <设备ID>",服务器回 JSON
//      {"url":"http://192.168.x.x:8787","name":"..."}。配置了固定地址则跳过广播。
// 配对:POST /api/pair {"device","name"} → {"token"},令牌存 NVS,之后每个请求带
//      Authorization: Bearer <token>。服务器在局域网默认自动批准(可在服务器配置里关)。
#include "esp_internal.h"
#include "settings.h"
#include "svc.h"
#include "sys.h"

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "relay";

#define SLOTS 4
#define RESP_MAX 6144
#define PATH_MAX_LEN 64

typedef struct {
    volatile req_state_t state;
    char path[PATH_MAX_LEN];
    char *body;
    char *resp;
    int resp_len;
    int status;
    volatile bool abandoned;   // 应用已放弃:任务做完后自己回收
} slot_t;

static slot_t s_slots[SLOTS];
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static volatile srv_state_t s_state = SRV_UNKNOWN;
static volatile bool s_discover;
static char s_url[96];
static char s_diag[96] = "还没联网";      // 最近一次发现的结果(给设置页显示)
static volatile bool s_attempted;
static char s_token[48];

const char *relay_token(void) { return s_token; }
srv_state_t relay_state(void)
{
    if (net_state() != NET_CONNECTED) return s_state == SRV_OK ? SRV_NO_NET : s_state;
    return s_state;
}
const char *relay_url(void) { return s_url; }
bool relay_ready(void) { return net_state() == NET_CONNECTED; }
bool relay_local(void) { return net_state() == NET_CONNECTED && s_state != SRV_OK; }

int local_request(const char *path, const char *body, char *out, int cap);

bool relay_discovery_attempted(void) { return s_attempted; }
const char *relay_diag(void) { return s_diag; }

void relay_discover(void)
{
    s_attempted = false;
    s_discover = true;
    if (s_task) xTaskNotifyGive(s_task);
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------
typedef struct {
    char *buf;
    int len;
    int cap;
} sink_t;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    sink_t *s = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && s) {
        const int n = e->data_len < s->cap - 1 - s->len ? e->data_len : s->cap - 1 - s->len;
        if (n > 0) {
            memcpy(s->buf + s->len, e->data, (size_t)n);
            s->len += n;
            s->buf[s->len] = '\0';
        }
    }
    return ESP_OK;
}

static int http(const char *base, const char *path, const char *body, char *out, int cap, int timeout_ms)
{
    char url[192];
    snprintf(url, sizeof(url), "%s%s", base, path);
    sink_t sink = {.buf = out, .len = 0, .cap = cap};
    if (out && cap) out[0] = '\0';
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = timeout_ms,
        .event_handler = on_http,
        .user_data = out ? &sink : NULL,
        .method = body ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    char auth[64];
    if (s_token[0]) {
        snprintf(auth, sizeof(auth), "Bearer %s", s_token);
        esp_http_client_set_header(c, "Authorization", auth);
    }
    esp_http_client_set_header(c, "X-Device", sys_short_id());
    if (body) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
        esp_http_client_set_post_field(c, body, (int)strlen(body));
    }
    const esp_err_t err = esp_http_client_perform(c);
    const int status = err == ESP_OK ? esp_http_client_get_status_code(c) : -1;
    esp_http_client_cleanup(c);
    if (err != ESP_OK) ESP_LOGW(TAG, "%s 失败: %s", path, esp_err_to_name(err));
    return status;
}

// ---------------------------------------------------------------------------
// 发现与配对
// ---------------------------------------------------------------------------
static bool broadcast_discover(char *url, size_t len)
{
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return false;
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    // 绑定固定端口:既能收到服务器对询问的回复,也能收到服务器每 3 秒的主动广播
    // (有些 VPN / 路由器只拦一个方向)。
    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = htons(RELAY_BEACON_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    bind(sock, (struct sockaddr *)&local, sizeof(local));
    struct timeval tv = {.tv_sec = 0, .tv_usec = 500000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // 两种广播地址都发:受限广播 255.255.255.255 与本网段广播(如 192.168.31.255)。
    uint32_t targets[2] = {htonl(INADDR_BROADCAST), 0};
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(net_sta_netif(), &ip) == ESP_OK && ip.ip.addr) {
        targets[1] = ip.ip.addr | ~ip.netmask.addr;
    }
    char msg[48];
    snprintf(msg, sizeof(msg), "%s %s", RELAY_DISCOVERY_MAGIC, sys_short_id());
    bool found = false;
    for (int attempt = 0; attempt < 8 && !found; attempt++) {
        for (int t = 0; t < 2; t++) {
            if (!targets[t]) continue;
            struct sockaddr_in to = {
                .sin_family = AF_INET,
                .sin_port = htons(RELAY_DISCOVERY_PORT),
                .sin_addr.s_addr = targets[t],
            };
            sendto(sock, msg, strlen(msg), 0, (struct sockaddr *)&to, sizeof(to));
        }
        char buf[256];
        const int n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) continue;
        buf[n] = '\0';
        cJSON *j = cJSON_Parse(buf);
        const cJSON *u = cJSON_GetObjectItemCaseSensitive(j, "url");
        if (cJSON_IsString(u) && !strncmp(u->valuestring, "http", 4)) {
            snprintf(url, len, "%s", u->valuestring);
            found = true;
        }
        cJSON_Delete(j);
    }
    close(sock);
    if (!found) {
        ESP_LOGW(TAG, "发现失败:本机 " IPSTR ",已向 255.255.255.255 与 " IPSTR " 广播 8 次,"
                 "也没收到服务器广播。检查 VPN/防火墙/路由器的客户端隔离,或在配网页填写服务器地址",
                 IP2STR(&ip.ip), IP2STR((esp_ip4_addr_t *)&targets[1]));
    }
    return found;
}

static bool pair(void)
{
    char body[96], resp[256];
    snprintf(body, sizeof(body), "{\"device\":\"%s\",\"name\":\"%s\"}", sys_short_id(), g_settings.name);
    if (http(s_url, "/api/pair", body, resp, sizeof(resp), 5000) != 200) return false;
    cJSON *j = cJSON_Parse(resp);
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(j, "token");
    const bool ok = cJSON_IsString(t) && strlen(t->valuestring) < sizeof(s_token);
    if (ok) {
        snprintf(s_token, sizeof(s_token), "%s", t->valuestring);
        sys_kv_set_str("token", s_token);
        ESP_LOGI(TAG, "已与服务器配对");
    }
    cJSON_Delete(j);
    return ok;
}

static void do_discover(void)
{
    s_state = SRV_DISCOVERING;
    if (g_settings.server[0]) {
        snprintf(s_url, sizeof(s_url), "%s", g_settings.server);
    } else if (!broadcast_discover(s_url, sizeof(s_url))) {
        s_state = SRV_UNREACHABLE;
        snprintf(s_diag, sizeof(s_diag), "广播无回应(VPN/防火墙/路由器隔离?)");
        ESP_LOGW(TAG, "局域网里没有找到服务器");
        s_attempted = true;
        return;
    }
    // 去掉末尾斜杠
    const size_t l = strlen(s_url);
    if (l && s_url[l - 1] == '/') s_url[l - 1] = '\0';

    int st = http(s_url, "/api/hello", NULL, NULL, 0, 4000);
    if (st == 401 || (st == 200 && !s_token[0])) {
        if (pair()) st = http(s_url, "/api/hello", NULL, NULL, 0, 4000);
    }
    s_state = st == 200 ? SRV_OK : SRV_UNREACHABLE;
    if (st == 200) snprintf(s_diag, sizeof(s_diag), "已连接");
    else if (st < 0) snprintf(s_diag, sizeof(s_diag), "连不上 8787 端口(防火墙?)");
    else snprintf(s_diag, sizeof(s_diag), "服务器返回 %d(配对被拒?)", st);
    s_attempted = true;
    ESP_LOGI(TAG, "服务器 %s: %s", s_url, st == 200 ? "已连接" : "不可用");
}

// ---------------------------------------------------------------------------
static void relay_task(void *arg)
{
    (void)arg;
    int64_t next_auto = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        if (net_state() != NET_CONNECTED) continue;
        const int64_t now = sys_now_ms();
        if (s_discover || (s_state != SRV_OK && s_state != SRV_DISCOVERING && now >= next_auto)) {
            s_discover = false;
            do_discover();
            next_auto = now + 60000;
        }
        for (int i = 0; i < SLOTS; i++) {
            slot_t *s = &s_slots[i];
            if (s->abandoned && s->state != REQ_BUSY && s->state != REQ_FREE) {
                // 放弃标记与完成同时发生的竞态:在这里补回收。
                free(s->body);
                free(s->resp);
                s->body = s->resp = NULL;
                s->abandoned = false;
                s->state = REQ_FREE;
                continue;
            }
            if (s->state != REQ_BUSY) continue;
            if (s_state != SRV_OK) {
                // 没有服务器:天气 / 看板 / 家居由设备自己直连处理。
                s->status = local_request(s->path, s->body, s->resp, RESP_MAX);
            } else {
                s->status = http(s_url, s->path, s->body, s->resp, RESP_MAX, 8000);
                if (s->status < 0) {
                    s_state = SRV_UNREACHABLE;   // 连接失败:这次改走直连,稍后自动重新发现
                    s->status = local_request(s->path, s->body, s->resp, RESP_MAX);
                } else if (s->status >= 500 && strcmp(s->path, "/api/weather") == 0) {
                    // 服务器在,但它自己取天气失败(例如 Mac 的代理拦了天气接口):设备改为直连。
                    ESP_LOGW(TAG, "服务器天气失败 %d,改为设备直连", s->status);
                    s->status = local_request(s->path, s->body, s->resp, RESP_MAX);
                }
            }
            s->state = s->status > 0 ? REQ_DONE : REQ_ERROR;
            if (s->abandoned) {
                free(s->body);
                free(s->resp);
                s->body = s->resp = NULL;
                s->abandoned = false;
                s->state = REQ_FREE;
            }
        }
    }
}

void relay_start(void);

int relay_request(const char *path, const char *json_body)
{
    relay_start();
    for (int i = 0; i < SLOTS; i++) {
        slot_t *s = &s_slots[i];
        if (s->state != REQ_FREE) continue;
        s->resp = malloc(RESP_MAX);
        if (!s->resp) return -1;
        s->body = json_body ? strdup(json_body) : NULL;
        snprintf(s->path, sizeof(s->path), "%s", path);
        s->status = 0;
        s->abandoned = false;
        s->state = REQ_BUSY;
        xTaskNotifyGive(s_task);
        return i;
    }
    return -1;
}

req_state_t relay_poll(int h, const char **body, int *http_status)
{
    if (h < 0 || h >= SLOTS) return REQ_FREE;
    slot_t *s = &s_slots[h];
    const req_state_t st = s->state;
    if (st == REQ_DONE || st == REQ_ERROR) {
        *body = s->resp ? s->resp : "";
        *http_status = s->status;
    }
    return st;
}

void relay_free(int h)
{
    if (h < 0 || h >= SLOTS) return;
    slot_t *s = &s_slots[h];
    if (s->state == REQ_BUSY) {
        // 不能在界面线程里等网络:交给任务做完后回收。
        s->abandoned = true;
        return;
    }
    free(s->body);
    free(s->resp);
    s->body = NULL;
    s->resp = NULL;
    s->state = REQ_FREE;
}

// 开机时启动后台任务(以便 Wi-Fi 一连上就开始发现)。
void relay_start(void)
{
    if (s_lock) return;
    s_lock = xSemaphoreCreateMutex();
    sys_kv_get_str("token", s_token, sizeof(s_token));
    xTaskCreate(relay_task, "relay", 8192, NULL, 4, &s_task);   // HTTPS 握手需要较大的栈
}
