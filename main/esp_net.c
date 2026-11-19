// main/esp_net.c —— Wi-Fi 服务:STA 引用计数连接、扫描、SoftAP 配网门户、SNTP 对时。
//
// 所有会阻塞或耗时的 esp_wifi_* 调用都在 net 任务里执行;LVGL 任务只发命令、读状态。
// 开机若已配网,会先连一次 Wi-Fi 对时(最多 60 秒),对时成功且没有应用占用就关掉射频省电。
#include "settings.h"
#include "svc.h"
#include "sys.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static const char *TAG = "net";

typedef enum {
    CMD_UPDATE = 0,       // 引用计数变化
    CMD_RECONNECT,
    CMD_SCAN,
    CMD_PORTAL_START,
    CMD_PORTAL_STOP,
} net_cmd_t;

#define BOOT_SYNC_MS 60000
#define RETRY_FAST 5

static QueueHandle_t s_cmd;
static SemaphoreHandle_t s_lock;
static esp_netif_t *s_sta;
static esp_netif_t *s_ap;
static httpd_handle_t s_httpd;

static volatile int s_refs;
static volatile bool s_boot_sync;
static volatile bool s_wifi_started;
static volatile bool s_portal_on;
static volatile net_state_t s_state = NET_OFF;
static volatile scan_state_t s_scan = SCAN_IDLE;
static volatile portal_state_t s_portal = PORTAL_OFF;
static volatile int s_retries;
static bool s_sntp_started;

static char s_ip[16];
static char s_ssid[33];
static char s_portal_ssid[33];
static net_ap_t s_aps[NET_SCAN_MAX];
static int s_ap_count;


static void post(net_cmd_t c)
{
    if (s_cmd) xQueueSend(s_cmd, &c, 0);
}

// ---------------------------------------------------------------------------
// 时间
// ---------------------------------------------------------------------------
bool time_valid(void)
{
    time_t now = time(NULL);
    return now > 1735689600;   // 2025-01-01 之后才算对过时
}

bool time_local(struct tm *out)
{
    if (!time_valid()) return false;
    time_t now = time(NULL);
    localtime_r(&now, out);
    return true;
}

void time_apply_tz(const char *tz)
{
    setenv("TZ", tz && tz[0] ? tz : "CST-8", 1);
    tzset();
}

static void sntp_begin(void)
{
    if (s_sntp_started) {
        esp_netif_sntp_start();
        return;
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        3, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "cn.pool.ntp.org", "pool.ntp.org"));
    if (esp_netif_sntp_init(&cfg) == ESP_OK) s_sntp_started = true;
}

// ---------------------------------------------------------------------------
// 事件
// ---------------------------------------------------------------------------
static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_DISCONNECTED:
            s_ip[0] = '\0';
            if (s_state == NET_CONNECTED || s_state == NET_CONNECTING) {
                s_state = ++s_retries > RETRY_FAST ? NET_FAILED : NET_CONNECTING;
                post(CMD_RECONNECT);
            }
            break;
        case WIFI_EVENT_SCAN_DONE: {
            uint16_t n = NET_SCAN_MAX;
            static wifi_ap_record_t recs[NET_SCAN_MAX];
            if (esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_ap_count = n;
                for (int i = 0; i < n; i++) {
                    snprintf(s_aps[i].ssid, sizeof(s_aps[i].ssid), "%s", (const char *)recs[i].ssid);
                    s_aps[i].rssi = recs[i].rssi;
                    s_aps[i].channel = recs[i].primary;
                    s_aps[i].secure = recs[i].authmode != WIFI_AUTH_OPEN;
                    memcpy(s_aps[i].bssid, recs[i].bssid, 6);
                }
                xSemaphoreGive(s_lock);
                s_scan = SCAN_DONE;
            } else {
                s_scan = SCAN_FAILED;
            }
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED:
            if (s_portal == PORTAL_WAITING) s_portal = PORTAL_CLIENT;
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_retries = 0;
        s_state = NET_CONNECTED;
        ESP_LOGI(TAG, "已连接 %s,IP %s", s_ssid, s_ip);
        sntp_begin();
        relay_discover();
    }
}

// ---------------------------------------------------------------------------
// 射频启停
// ---------------------------------------------------------------------------
static void wifi_start(wifi_mode_t mode)
{
    if (!s_wifi_started) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        if (esp_wifi_init(&cfg) != ESP_OK) {
            s_state = NET_FAILED;
            return;
        }
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        esp_wifi_set_mode(mode);
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        if (esp_wifi_start() != ESP_OK) {
            esp_wifi_deinit();
            s_state = NET_FAILED;
            return;
        }
        s_wifi_started = true;
    } else {
        esp_wifi_set_mode(mode);
    }
}

static void wifi_stop(void)
{
    if (!s_wifi_started) return;
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    s_wifi_started = false;
    s_state = NET_OFF;
    s_ip[0] = '\0';
    s_scan = SCAN_IDLE;
}

static void sta_connect(void)
{
    if (!settings_has_wifi()) {
        s_state = NET_NO_CONFIG;
        return;
    }
    wifi_config_t wc = {0};
    // ssid/password 字段不要求结尾 0:按字段长度拷贝。
    strncpy((char *)wc.sta.ssid, g_settings.wifi_ssid, sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, g_settings.wifi_pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = g_settings.wifi_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    snprintf(s_ssid, sizeof(s_ssid), "%s", g_settings.wifi_ssid);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    s_state = NET_CONNECTING;
    esp_wifi_connect();
}

static bool wanted(void) { return s_refs > 0 || s_boot_sync || s_portal_on; }

static void apply(void)
{
    if (!wanted()) {
        wifi_stop();
        return;
    }
    wifi_start(s_portal_on ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (!s_wifi_started) return;
    if (s_state == NET_OFF || s_state == NET_NO_CONFIG) sta_connect();
}

// ---------------------------------------------------------------------------
// 配网门户(SoftAP + 一个网页表单)
// ---------------------------------------------------------------------------
// ===========================================================================
// 配置网页(手机连上设备热点后自动弹出)。所有文字只在手机上显示。 glyph-check: ignore
// ===========================================================================
static const char k_css[] =   // glyph-check: ignore
    "<!doctype html><html lang=zh><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'><title>工具箱设置</title><style>"
    "body{font-family:-apple-system,system-ui,sans-serif;background:#0b0e14;color:#f1f4f9;margin:0;padding:16px}"
    "h1{font-size:22px;margin:6px 0 2px}.sub{color:#667185;font-size:13px;margin-bottom:12px}"
    ".card{background:#151a23;border:1px solid #2a3241;border-radius:16px;padding:14px;margin:12px 0}"
    ".card h2{font-size:16px;margin:0 0 8px}.row{display:flex;justify-content:space-between;padding:4px 0;"
    "font-size:14px}.k{color:#a3adbd}.ok{color:#34d399}.warn{color:#fbbf24}"
    "label{display:block;margin:10px 0 6px;color:#a3adbd;font-size:13px}"
    "input,select{width:100%;box-sizing:border-box;padding:12px;border-radius:10px;border:1px solid #2a3241;"
    "background:#0b0e14;color:#f1f4f9;font-size:16px}.pw{display:flex;gap:8px}.pw input{flex:1}"
    ".pw button,.ghost{width:auto;margin:0;padding:0 14px;background:#1e2531;color:#f1f4f9;border:0;"
    "border-radius:10px;font-size:14px}.ap{display:flex;justify-content:space-between;padding:11px 12px;"
    "margin:6px 0;border-radius:10px;background:#0b0e14;border:1px solid #2a3241}.ap.on{border-color:#ff6b57}"
    ".s{color:#667185}details summary{color:#a3adbd;font-size:14px;padding:6px 0}"
    ".go{margin-top:6px;width:100%;padding:15px;border:0;border-radius:12px;background:#ff6b57;color:#0b0e14;"
    "font-size:17px;font-weight:600}#msg{display:none;padding:12px;border-radius:12px;margin:12px 0;"
    "font-size:15px}p{color:#667185;font-size:13px;line-height:1.5;margin:6px 0}</style></head><body>";

// 常用城市经纬度(手机上直接选,不需要联网)
static const char k_cities[] =   // glyph-check: ignore
    "北京,39.90,116.41|上海,31.23,121.47|广州,23.13,113.26|深圳,22.54,114.06|杭州,30.27,120.16|"
    "南京,32.06,118.80|苏州,31.30,120.59|成都,30.57,104.07|重庆,29.56,106.55|武汉,30.59,114.31|"
    "西安,34.34,108.94|天津,39.13,117.20|长沙,28.23,112.94|郑州,34.75,113.63|青岛,36.07,120.38|"
    "厦门,24.48,118.09|福州,26.07,119.30|合肥,31.82,117.23|济南,36.65,117.12|宁波,29.87,121.54|"
    "无锡,31.49,120.31|东莞,23.02,113.75|佛山,23.02,113.12|珠海,22.27,113.58|沈阳,41.80,123.43|"
    "大连,38.91,121.61|哈尔滨,45.80,126.53|长春,43.82,125.32|昆明,25.04,102.71|贵阳,26.65,106.63|"
    "南宁,22.82,108.32|海口,20.04,110.32|三亚,18.25,109.51|南昌,28.68,115.86|太原,37.87,112.55|"
    "石家庄,38.04,114.51|兰州,36.06,103.83|乌鲁木齐,43.83,87.62|拉萨,29.65,91.17|呼和浩特,40.84,111.75|"
    "银川,38.49,106.23|西宁,36.62,101.78|香港,22.32,114.17|澳门,22.20,113.55|台北,25.03,121.57";

static const char k_script[] =   // glyph-check: ignore
    "<script>"
    "const $=q=>document.querySelector(q);"
    "document.querySelectorAll('.ap').forEach(e=>e.onclick=()=>{document.querySelectorAll('.ap')"
    ".forEach(x=>x.classList.remove('on'));e.classList.add('on');$('#ssid').value=e.dataset.s;$('#pass').focus()});"
    "document.querySelectorAll('.eye').forEach(b=>b.onclick=()=>{const i=b.previousElementSibling;"
    "i.type=i.type=='password'?'text':'password';b.textContent=i.type=='password'?'显示':'隐藏'});"
    "const cs=$('#cities').dataset.c.split('|').map(x=>x.split(','));const sel=$('#cities');"
    "cs.forEach(c=>{const o=document.createElement('option');o.value=c.join(',');o.textContent=c[0];sel.add(o)});"
    "sel.onchange=()=>{const v=sel.value;if(v=='auto'){$('[name=city]').value='';$('[name=lat]').value='';"
    "$('[name=lon]').value=''}else if(v=='typed'){$('[name=lat]').value='';$('[name=lon]').value='';"
    "$('[name=city]').focus()}else{const c=v.split(',');$('[name=city]').value=c[0];$('[name=lat]').value=c[1];"
    "$('[name=lon]').value=c[2]}};"
    "$('[name=city]').oninput=()=>{$('[name=lat]').value='';$('[name=lon]').value='';sel.value='typed'};"
    "$('#prov').onchange=()=>{if($('#prov').value=='sf'){$('[name=api_base]').value='https://api.siliconflow.cn/v1';"
    "$('[name=llm_model]').value='deepseek-ai/DeepSeek-V3';$('[name=asr_model]').value='FunAudioLLM/SenseVoiceSmall';"
    "$('[name=tts_model]').value='FunAudioLLM/CosyVoice2-0.5B';$('[name=tts_voice]').value='FunAudioLLM/CosyVoice2-0.5B:anna'}};"
    "const msg=(t,c)=>{const m=$('#msg');m.style.display='block';m.style.background=c;m.textContent=t;"
    "m.scrollIntoView({behavior:'smooth'})};"
    "$('#f').onsubmit=async e=>{e.preventDefault();msg('正在保存…','#1e2531');"
    "try{await fetch('/save',{method:'POST',body:new URLSearchParams(new FormData($('#f')))})}"
    "catch(_){msg('保存失败,请重试','#7f1d1d');return}"
    "if(!$('#ssid').value){msg('已保存。设备上可以返回了。','#065f46');return}"
    "msg('已保存,正在连接 '+$('#ssid').value+' …(手机可能会短暂断开,请看设备屏幕)','#1e2531');"
    "for(let i=0;i<40;i++){await new Promise(r=>setTimeout(r,1500));try{const s=await(await fetch('/status')).json();"
    "if(s.net=='connected'){msg('连接成功!设备地址 '+s.ip+'。可以关掉这个页面了。','#065f46');return}"
    "if(s.net=='failed'){msg('连不上 '+s.ssid+',请检查密码后重新保存。','#7f1d1d');return}}catch(_){}}"
    "msg('还没有结果,请看设备屏幕。','#1e2531')}"
    "</script></body></html>";

static portal_form_t s_form;

static void html_escape(char *out, size_t len, const char *in)
{
    size_t o = 0;
    for (; *in && o + 6 < len; in++) {
        const char *rep = *in == '<' ? "&lt;" : *in == '>' ? "&gt;" : *in == '&' ? "&amp;"
                        : *in == '\'' ? "&#39;" : *in == '"' ? "&quot;" : NULL;
        if (rep) {
            memcpy(out + o, rep, strlen(rep));
            o += strlen(rep);
        } else {
            out[o++] = *in;
        }
    }
    out[o] = '\0';
}

static void chunk(httpd_req_t *req, const char *text) { httpd_resp_send_chunk(req, text, HTTPD_RESP_USE_STRLEN); }

// 输入框:secret 字段不回显已保存的值,只提示"已保存,留空不变"。 glyph-check: ignore
static void field(httpd_req_t *req, const char *label, const char *name, const char *value,
                  bool secret, const char *placeholder)
{
    char esc[220], buf[600];
    html_escape(esc, sizeof(esc), secret ? "" : value);
    const char *ph = secret && value[0] ? "已保存,留空不变" : (placeholder ? placeholder : "");  // glyph-check: ignore
    if (secret) {
        snprintf(buf, sizeof(buf), "<label>%s</label><div class=pw><input name=%s type=password "  // glyph-check: ignore
                 "placeholder='%s' autocomplete=off><button type=button class=eye>显示</button></div>",  // glyph-check: ignore
                 label, name, ph);
    } else {
        snprintf(buf, sizeof(buf), "<label>%s</label><input name=%s value=\"%s\" placeholder='%s'>",
                 label, name, esc, ph);
    }
    chunk(req, buf);
}

static void status_row(httpd_req_t *req, const char *k, const char *v, const char *cls)
{
    char esc[100], buf[260];
    html_escape(esc, sizeof(esc), v);
    snprintf(buf, sizeof(buf), "<div class=row><span class=k>%s</span><span class=%s>%s</span></div>", k, cls, esc);
    chunk(req, buf);
}

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    chunk(req, k_css);
    char buf[200];
    snprintf(buf, sizeof(buf), "<h1>工具箱设置</h1><div class=sub>%s · 固件 %s</div>", g_settings.name, SYS_FW_VERSION);  // glyph-check: ignore
    chunk(req, buf);

    // 当前状态
    chunk(req, "<div class=card><h2>当前状态</h2>");  // glyph-check: ignore
    status_row(req, "无线网络", g_settings.wifi_ssid[0] ? g_settings.wifi_ssid : "未设置",  // glyph-check: ignore
               g_settings.wifi_ssid[0] ? "ok" : "warn");
    snprintf(buf, sizeof(buf), "%s%s", g_settings.city[0] ? g_settings.city : "按网络自动定位",  // glyph-check: ignore
             g_settings.lat[0] ? "(经纬度已填)" : "");  // glyph-check: ignore
    status_row(req, "天气位置", buf, "s");  // glyph-check: ignore
    status_row(req, "语音助手密钥", g_settings.api_key[0] ? "已保存" : "未设置",  // glyph-check: ignore
               g_settings.api_key[0] ? "ok" : "warn");
    status_row(req, "家庭助手", g_settings.ha_url[0] ? g_settings.ha_url : "未设置", "s");  // glyph-check: ignore
    chunk(req, "</div><div id=msg></div><form id=f method=post action=/save>");

    // 无线网络:附近网络(按信号排序,同名只留最强)
    chunk(req, "<div class=card><h2>无线网络</h2>");  // glyph-check: ignore
    static net_ap_t aps[NET_SCAN_MAX];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int n = s_ap_count;
    memcpy(aps, s_aps, sizeof(net_ap_t) * (size_t)n);
    xSemaphoreGive(s_lock);
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (aps[j].rssi > aps[i].rssi) {
                net_ap_t t = aps[i];
                aps[i] = aps[j];
                aps[j] = t;
            }
        }
    }
    int shown = 0;
    for (int i = 0; i < n && shown < 12; i++) {
        if (!aps[i].ssid[0]) continue;
        bool dup = false;
        for (int j = 0; j < i; j++) dup |= !strcmp(aps[j].ssid, aps[i].ssid);
        if (dup) continue;
        char esc[100], row[300];
        html_escape(esc, sizeof(esc), aps[i].ssid);
        const int bars = aps[i].rssi >= -55 ? 4 : aps[i].rssi >= -67 ? 3 : aps[i].rssi >= -78 ? 2 : 1;
        snprintf(row, sizeof(row), "<div class=ap data-s=\"%s\"><span>%s</span><span class=s>%s%s</span></div>",
                 esc, esc, aps[i].secure ? "" : "开放 · ",  // glyph-check: ignore
                 bars == 4 ? "●●●●" : bars == 3 ? "●●●○" : bars == 2 ? "●●○○" : "●○○○");  // glyph-check: ignore
        chunk(req, row);
        shown++;
    }
    if (!shown) chunk(req, s_scan == SCAN_RUNNING ? "<p>正在扫描…</p>" : "<p>没有扫描到网络。</p>");  // glyph-check: ignore
    chunk(req, "<p><a href=/rescan style='color:#ff6b57'>重新扫描</a> · 只改下面其他设置时,名称留空即可</p>"  // glyph-check: ignore
              "<label>网络名称(点上面的列表或手动输入隐藏网络)</label><input name=ssid id=ssid autocomplete=off>"  // glyph-check: ignore
              "<label>密码</label><div class=pw><input name=pass id=pass type=password autocomplete=off>"  // glyph-check: ignore
              "<button type=button class=eye>显示</button></div></div>");  // glyph-check: ignore

    // 天气位置
    chunk(req, "<div class=card><h2>天气位置</h2><p>选城市最准;区县可以直接输入名字,设备联网后自动查经纬度。</p>"  // glyph-check: ignore
              "<select id=cities data-c=\"");
    chunk(req, k_cities);
    chunk(req, "\"><option value=keep>保持当前设置</option><option value=auto>按网络自动定位</option>"  // glyph-check: ignore
              "<option value=typed>手动输入城市 / 区县</option></select>");  // glyph-check: ignore
    field(req, "城市 / 区县名", "city", g_settings.city, false, "例:浦东、海淀、余杭");  // glyph-check: ignore
    chunk(req, "<details><summary>精确经纬度(可选)</summary>");  // glyph-check: ignore
    field(req, "纬度", "lat", g_settings.lat, false, "例:31.23");  // glyph-check: ignore
    field(req, "经度", "lon", g_settings.lon, false, "例:121.47");  // glyph-check: ignore
    chunk(req, "</details></div>");

    // 语音助手
    chunk(req, "<div class=card><h2>语音助手(云端)</h2><p>没有家里的服务器时,设备直接调用云端完成 识别 → 对话 → 朗读。"  // glyph-check: ignore
              "硅基流动在国内可直连,注册后在控制台创建密钥即可。</p>"  // glyph-check: ignore
              "<select id=prov><option value=keep>保持当前接口</option><option value=sf>硅基流动(推荐)</option>"  // glyph-check: ignore
              "<option value=custom>其他兼容 OpenAI 的接口</option></select>");  // glyph-check: ignore
    field(req, "API 密钥", "api_key", g_settings.api_key, true, "sk-...");
    chunk(req, "<details><summary>接口与模型</summary>");  // glyph-check: ignore
    field(req, "接口地址", "api_base", g_settings.api_base, false, NULL);  // glyph-check: ignore
    field(req, "对话模型", "llm_model", g_settings.llm_model, false, NULL);  // glyph-check: ignore
    field(req, "识别模型", "asr_model", g_settings.asr_model, false, NULL);  // glyph-check: ignore
    field(req, "朗读模型", "tts_model", g_settings.tts_model, false, NULL);  // glyph-check: ignore
    field(req, "朗读声音", "tts_voice", g_settings.tts_voice, false, NULL);  // glyph-check: ignore
    chunk(req, "</details><p>注意:密钥保存在设备里,请勿把设备借给不信任的人。</p></div>");  // glyph-check: ignore

    // 家庭助手 + 服务器
    chunk(req, "<div class=card><h2>家庭助手(可选)</h2><p>长期访问令牌在 Home Assistant 的个人资料页底部创建;实体用逗号分隔。</p>");  // glyph-check: ignore
    field(req, "地址", "ha_url", g_settings.ha_url, false, "http://192.168.1.5:8123");  // glyph-check: ignore
    field(req, "长期访问令牌", "ha_token", g_settings.ha_token, true, NULL);  // glyph-check: ignore
    field(req, "实体", "ha_ents", g_settings.ha_entities, false, "light.living_room, switch.fan");  // glyph-check: ignore
    chunk(req, "</div><div class=card><h2>家里的中转服务器(可选)</h2><p>留空会自动在局域网查找;找不到时其他功能照常直连。</p>");  // glyph-check: ignore
    field(req, "服务器地址", "server", g_settings.server, false, "http://192.168.1.20:8787");  // glyph-check: ignore
    chunk(req, "</div><button class=go>保存</button></form>");  // glyph-check: ignore
    chunk(req, k_script);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t page_status(httpd_req_t *req)
{
    static const char *const names[] = {"off", "no_config", "connecting", "connected", "failed"};
    char esc[80], buf[200];
    html_escape(esc, sizeof(esc), s_ssid);
    snprintf(buf, sizeof(buf), "{\"net\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\"}",
             names[s_wifi_started ? s_state : NET_OFF], esc, s_ip);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t page_rescan(httpd_req_t *req)
{
    s_scan = SCAN_RUNNING;
    esp_wifi_scan_start(NULL, true);   // 阻塞约 2 秒,在 httpd 任务里,不影响界面
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

// 其他任何地址(手机系统的联网检测、随手输入的网址)都跳到配置页:手机连上热点会自动弹出。
static esp_err_t page_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

// 极简 DNS:热点里所有域名都解析到 192.168.4.1(强制门户)。
static volatile bool s_dns_run;

static void dns_task(void *arg)
{
    (void)arg;
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (sock >= 0) close(sock);
        vTaskDelete(NULL);
        return;
    }
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t pkt[256];
    while (s_dns_run) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        const int n = recvfrom(sock, pkt, sizeof(pkt) - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12 || (pkt[2] & 0x80)) continue;            // 太短或本身是回复
        // 找到问题段结尾(名字 + 类型 + 类)
        int q = 12;
        while (q < n && pkt[q]) q += pkt[q] + 1;
        q += 5;
        if (q > n) continue;
        pkt[2] = 0x81;                                     // 回复、递归可用
        pkt[3] = 0x80;
        pkt[6] = 0; pkt[7] = 1;                            // 1 个回答
        pkt[8] = pkt[9] = pkt[10] = pkt[11] = 0;
        static const uint8_t ans[16] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1};
        memcpy(pkt + q, ans, sizeof(ans));
        sendto(sock, pkt, (size_t)q + sizeof(ans), 0, (struct sockaddr *)&from, fl);
    }
    close(sock);
    vTaskDelete(NULL);
}

// application/x-www-form-urlencoded 解码
static void url_decode(char *dst, size_t len, const char *src, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < len; i++) {
        if (src[i] == '+') {
            dst[o++] = ' ';
        } else if (src[i] == '%' && i + 2 < n) {
            char hex[3] = {src[i + 1], src[i + 2], 0};
            dst[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            dst[o++] = src[i];
        }
    }
    dst[o] = '\0';
}

static void form_field(const char *body, const char *key, char *out, size_t len)
{
    out[0] = '\0';
    const size_t kl = strlen(key);
    for (const char *p = body; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1;
            const char *end = strchr(v, '&');
            url_decode(out, len, v, end ? (size_t)(end - v) : strlen(v));
            return;
        }
    }
}

static esp_err_t page_save(httpd_req_t *req)
{
    enum { BODY_MAX = 1800 };
    char *body = malloc(BODY_MAX);
    if (!body) return ESP_FAIL;
    const int n = req->content_len < BODY_MAX - 1 ? req->content_len : BODY_MAX - 1;
    int got = 0;
    while (got < n) {
        const int r = httpd_req_recv(req, body + got, n - got);
        if (r <= 0) {
            free(body);
            return ESP_FAIL;
        }
        got += r;
    }
    body[got] = '\0';
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_form, 0, sizeof(s_form));
    form_field(body, "ssid", s_form.ssid, sizeof(s_form.ssid));
    form_field(body, "pass", s_form.pass, sizeof(s_form.pass));
    form_field(body, "server", s_form.server, sizeof(s_form.server));
    form_field(body, "api_base", s_form.api_base, sizeof(s_form.api_base));
    form_field(body, "api_key", s_form.api_key, sizeof(s_form.api_key));
    form_field(body, "llm_model", s_form.llm_model, sizeof(s_form.llm_model));
    form_field(body, "asr_model", s_form.asr_model, sizeof(s_form.asr_model));
    form_field(body, "tts_model", s_form.tts_model, sizeof(s_form.tts_model));
    form_field(body, "tts_voice", s_form.tts_voice, sizeof(s_form.tts_voice));
    form_field(body, "city", s_form.city, sizeof(s_form.city));
    form_field(body, "lat", s_form.lat, sizeof(s_form.lat));
    form_field(body, "lon", s_form.lon, sizeof(s_form.lon));
    form_field(body, "ha_url", s_form.ha_url, sizeof(s_form.ha_url));
    form_field(body, "ha_token", s_form.ha_token, sizeof(s_form.ha_token));
    form_field(body, "ha_ents", s_form.ha_entities, sizeof(s_form.ha_entities));
    s_form.valid = true;
    xSemaphoreGive(s_lock);
    free(body);
    s_portal = PORTAL_SAVED;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req,
        "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
        "<body style='font-family:system-ui;background:#0b0e14;color:#f1f4f9;padding:24px'>"
        "<h2>已保存</h2><p>请看设备屏幕上的结果。需要再改可以返回上一页。</p></body>");
}

bool net_portal_take(portal_form_t *out)
{
    if (!s_form.valid) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_form;
    s_form.valid = false;
    xSemaphoreGive(s_lock);
    return true;
}

static void portal_start(void)
{
    s_portal_on = true;
    s_portal = PORTAL_STARTING;
    snprintf(s_portal_ssid, sizeof(s_portal_ssid), "Passport-%s", sys_short_id());
    wifi_start(WIFI_MODE_APSTA);
    if (!s_wifi_started) {
        s_portal = PORTAL_FAILED;
        return;
    }
    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, s_portal_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = (uint8_t)strlen(s_portal_ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    if (esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK) {
        s_portal = PORTAL_FAILED;
        return;
    }
    esp_wifi_scan_start(NULL, false);   // 给表单提供附近网络列表
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 6;
    cfg.stack_size = 8192;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_httpd, &cfg) == ESP_OK) {
        const httpd_uri_t uris[] = {
            {.uri = "/", .method = HTTP_GET, .handler = page_get},
            {.uri = "/save", .method = HTTP_POST, .handler = page_save},
            {.uri = "/status", .method = HTTP_GET, .handler = page_status},
            {.uri = "/rescan", .method = HTTP_GET, .handler = page_rescan},
            {.uri = "/*", .method = HTTP_GET, .handler = page_redirect},   // 必须最后注册
        };
        for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(s_httpd, &uris[i]);
        s_dns_run = true;
        xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL);
        s_portal = PORTAL_WAITING;
        ESP_LOGI(TAG, "配网热点 %s 已开启", s_portal_ssid);
    } else {
        s_portal = PORTAL_FAILED;
    }
}

static void portal_stop(void)
{
    s_dns_run = false;
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    s_portal_on = false;
    s_portal = PORTAL_OFF;
    if (s_wifi_started) esp_wifi_set_mode(WIFI_MODE_STA);
}

// ---------------------------------------------------------------------------
// net 任务
// ---------------------------------------------------------------------------
static void net_task(void *arg)
{
    (void)arg;
    int64_t boot_sync_until = s_boot_sync ? sys_now_ms() + BOOT_SYNC_MS : 0;
    int64_t retry_at = 0;
    if (s_boot_sync) apply();
    for (;;) {
        net_cmd_t cmd;
        if (xQueueReceive(s_cmd, &cmd, pdMS_TO_TICKS(1000)) == pdTRUE) {
            switch (cmd) {
            case CMD_UPDATE: apply(); break;
            case CMD_RECONNECT:
                if (s_wifi_started) {
                    if (s_state == NET_FAILED) {
                        retry_at = sys_now_ms() + 30000;   // 连不上:30 秒后再试
                    } else {
                        esp_wifi_disconnect();
                        vTaskDelay(pdMS_TO_TICKS(s_retries ? 1000 * s_retries : 100));
                        if (s_state != NET_NO_CONFIG) s_state = NET_OFF;
                        apply();
                    }
                } else {
                    s_retries = 0;
                    apply();
                }
                break;
            case CMD_SCAN:
                if (s_wifi_started) {
                    // 正在连接时扫描会被拒绝:先停下连接尝试再扫。
                    if (s_state == NET_CONNECTING) esp_wifi_disconnect();
                    if (esp_wifi_scan_start(NULL, false) != ESP_OK) s_scan = SCAN_FAILED;
                } else {
                    s_scan = SCAN_FAILED;
                }
                break;
            case CMD_PORTAL_START: portal_start(); break;
            case CMD_PORTAL_STOP: portal_stop(); apply(); break;
            }
        }
        const int64_t now = sys_now_ms();
        if (retry_at && now >= retry_at) {
            retry_at = 0;
            s_retries = 0;
            if (s_wifi_started) {
                s_state = NET_OFF;
                apply();
            }
        }
        if (s_boot_sync && (time_valid() || now >= boot_sync_until)) {
            s_boot_sync = false;
            ESP_LOGI(TAG, "开机对时%s", time_valid() ? "完成" : "超时");
            apply();
        }
    }
}

void net_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_cmd = xQueueCreate(8, sizeof(net_cmd_t));
    esp_netif_init();
    esp_event_loop_create_default();
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL, NULL);
    s_boot_sync = settings_has_wifi();
    s_state = settings_has_wifi() ? NET_OFF : NET_NO_CONFIG;
    xTaskCreate(net_task, "net", 4096, NULL, 5, NULL);
}

// ---------------------------------------------------------------------------
// 对外接口(LVGL 任务调用)
// ---------------------------------------------------------------------------
// 蓝牙启动前调用:放弃开机对时,没有应用占用时关掉 Wi-Fi 腾出内存。
void net_yield_radio(void)
{
    if (s_boot_sync) {
        s_boot_sync = false;
        post(CMD_UPDATE);
    }
}

void net_acquire(void)
{
    s_refs++;
    post(CMD_UPDATE);
}

void net_release(void)
{
    if (s_refs > 0) s_refs--;
    post(CMD_UPDATE);
}

net_state_t net_state(void)
{
    if (!s_wifi_started) return settings_has_wifi() ? NET_OFF : NET_NO_CONFIG;
    return s_state;
}

bool net_radio_on(void) { return s_wifi_started; }
const char *net_ip(void) { return s_ip[0] ? s_ip : "-"; }
const char *net_ssid(void) { return s_ssid; }

int net_rssi(void)
{
    wifi_ap_record_t ap;
    return s_state == NET_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

int net_channel(void)
{
    uint8_t ch = 0;
    wifi_second_chan_t sec;
    if (s_wifi_started && esp_wifi_get_channel(&ch, &sec) == ESP_OK) return ch;
    return 0;
}

void net_reconnect(void)
{
    s_retries = 0;
    if (s_wifi_started) {
        s_state = NET_OFF;
        esp_wifi_disconnect();
    }
    post(CMD_UPDATE);
}

bool net_scan_start(void)
{
    if (s_scan == SCAN_RUNNING) return true;
    s_scan = SCAN_RUNNING;
    post(CMD_SCAN);
    return true;
}

scan_state_t net_scan_state(void) { return s_scan; }

int net_scan_results(net_ap_t *out, int max)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int n = s_ap_count < max ? s_ap_count : max;
    memcpy(out, s_aps, sizeof(net_ap_t) * (size_t)n);
    xSemaphoreGive(s_lock);
    if (s_scan == SCAN_DONE) s_scan = SCAN_IDLE;
    return n;
}

bool net_portal_start(void)
{
    s_portal = PORTAL_STARTING;
    post(CMD_PORTAL_START);
    return true;
}

void net_portal_stop(void) { post(CMD_PORTAL_STOP); }
portal_state_t net_portal_state(void) { return s_portal; }

const char *net_portal_ssid(void)
{
    if (!s_portal_ssid[0]) snprintf(s_portal_ssid, sizeof(s_portal_ssid), "Passport-%s", sys_short_id());
    return s_portal_ssid;
}

// 供 esp_link.c / esp_relay.c 使用
esp_netif_t *net_sta_netif(void) { return s_sta; }
