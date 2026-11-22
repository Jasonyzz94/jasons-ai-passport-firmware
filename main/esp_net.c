// main/esp_net.c —— Wi-Fi 服务:STA 引用计数连接、扫描、SoftAP 配网门户、SNTP 对时。
//
// 所有会阻塞或耗时的 esp_wifi_* 调用都在 net 任务里执行;LVGL 任务只发命令、读状态。
// 开机若已配网,会先连一次 Wi-Fi 对时(最多 60 秒),对时成功且没有应用占用就关掉射频省电。
// 最多保存 5 个网络(wifi_list.h):当前网络连不上时自动换下一个已保存的;
// 手机上提交的新网络先试连,拿到 IP 才保存,失败就回到原来的网络。
#include "esp_internal.h"
#include "settings.h"
#include "svc.h"
#include "sys.h"
#include "voice_cfg.h"
#include "wifi_list.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
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
    CMD_GOT_IP,           // 连上了:更新已保存列表的顺序
    CMD_TRY,              // 试连手机上提交的新网络
    CMD_FORGET,           // 从已保存列表删除 s_forget
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

// 已保存的网络与试连状态,都受 s_lock 保护;只在 net 任务里改列表和写存储。
static wifi_list_t s_saved;
static int s_cur = -1;                 // 正在用的已保存网络下标
static bool s_trial_on;                // 正在试连 s_trial(还没保存)
static wifi_cred_t s_trial;
static volatile net_try_t s_try = NET_TRY_NONE;
static char s_try_ssid[33];
static char s_forget[33];


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
        case WIFI_EVENT_STA_DISCONNECTED: {
            s_ip[0] = '\0';
            const wifi_event_sta_disconnected_t *d = data;
            // 密码错 / 找不到这个网络:多试也没用,算两次,早点换下一个。
            const bool hopeless = d && (d->reason == WIFI_REASON_AUTH_FAIL || d->reason == WIFI_REASON_NO_AP_FOUND
                                        || d->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
                                        || d->reason == WIFI_REASON_HANDSHAKE_TIMEOUT);
            if (s_state == NET_CONNECTED || s_state == NET_CONNECTING) {
                s_retries += hopeless && s_state == NET_CONNECTING ? 2 : 1;
                s_state = s_retries > RETRY_FAST ? NET_FAILED : NET_CONNECTING;
                post(CMD_RECONNECT);
            }
            break;
        }
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
        post(CMD_GOT_IP);
        sntp_begin();
        relay_discover();
    }
}

// ---------------------------------------------------------------------------
// 已保存的网络
// ---------------------------------------------------------------------------
static void saved_persist(void)   // 只在 net 任务里调用(写存储约数毫秒)
{
    static wifi_list_t copy;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    copy = s_saved;
    xSemaphoreGive(s_lock);
    sys_kv_set_blob("wifis", &copy, sizeof(copy));
    // 旧版只认 ssid / pass:同步最近的那个,刷回旧固件也还能联网。
    sys_kv_set_str("ssid", copy.count ? copy.n[0].ssid : "");
    sys_kv_set_str("pass", copy.count ? copy.n[0].pass : "");
}

static void saved_load(void)
{
    if (sys_kv_get_blob("wifis", &s_saved, sizeof(s_saved)) && wifi_list_valid(&s_saved)) return;
    wifi_list_init(&s_saved);
    char ssid[33] = "", pass[65] = "";
    sys_kv_get_str("ssid", ssid, sizeof(ssid));
    sys_kv_get_str("pass", pass, sizeof(pass));
    if (ssid[0]) {   // 从旧版的单个网络迁移
        wifi_list_promote(&s_saved, ssid, pass, NULL);
        sys_kv_set_blob("wifis", &s_saved, sizeof(s_saved));
        ESP_LOGI(TAG, "已迁移旧版保存的网络 %s", ssid);
    }
}

int net_saved_count(void) { return (int)s_saved.count; }

bool net_saved_get(int i, char *ssid, size_t len)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool ok = i >= 0 && i < s_saved.count;
    if (ok) snprintf(ssid, len, "%s", s_saved.n[i].ssid);
    xSemaphoreGive(s_lock);
    return ok;
}

net_try_t net_try_state(void) { return s_try; }
const char *net_try_ssid(void) { return s_try_ssid; }

// 连上了:试连的网络这时才保存;换到的备用网络排到最前,下次先连它。
static void on_got_ip(void)
{
    bool dirty = false;
    char evicted[33] = "";
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_trial_on) {
        wifi_list_promote(&s_saved, s_trial.ssid, s_trial.pass, evicted);
        s_trial_on = false;
        s_try = NET_TRY_OK;
        dirty = true;
    } else if (s_cur > 0) {
        const wifi_cred_t c = s_saved.n[s_cur];
        wifi_list_promote(&s_saved, c.ssid, c.pass, NULL);
        dirty = true;
    }
    s_cur = 0;
    xSemaphoreGive(s_lock);
    if (evicted[0]) ESP_LOGI(TAG, "已保存的网络满了,移除最久没用的 %s", evicted);
    if (dirty) saved_persist();
}

// 当前网络连不上。返回 true 表示换了一个网络并已开始连接。
static void apply(void);
static bool fallback(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_trial_on) {   // 试连失败:不保存,回到原来的网络
        s_trial_on = false;
        s_try = NET_TRY_FAILED;
        s_cur = -1;
        const bool any = s_saved.count > 0;
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "试连 %s 失败,回到已保存的网络", s_try_ssid);
        if (!any) return false;
        s_retries = 0;
        s_state = NET_OFF;
        apply();
        return true;
    }
    const char *vis[NET_SCAN_MAX];
    for (int i = 0; i < s_ap_count; i++) vis[i] = s_aps[i].ssid;
    const int next = wifi_list_next(&s_saved, s_cur, vis, s_ap_count);
    s_cur = next;   // -1:这一轮都试过了,30 秒后从头再来
    xSemaphoreGive(s_lock);
    if (next < 0) return false;
    ESP_LOGI(TAG, "换下一个已保存的网络");
    s_retries = 0;
    s_state = NET_OFF;
    apply();
    return true;
}

// ---------------------------------------------------------------------------
// 射频启停
// ---------------------------------------------------------------------------
static int s_start_fails;   // Wi-Fi 启动连续失败次数(内存不足等),net_task 会定时重试

static void wifi_start(wifi_mode_t mode)
{
    if (!s_wifi_started) {
        // 蓝牙正在启动或关闭(例如刚从遥控器退出):等它结束再开 Wi-Fi,最多 3 秒;
        // 还没结束就这次先不开,稍后重试,不和蓝牙同时抢内存。
        for (int i = 0; i < 30 && ble_radio_busy(); i++) vTaskDelay(pdMS_TO_TICKS(100));
        if (ble_radio_busy()) {
            s_start_fails++;
            ESP_LOGW(TAG, "蓝牙仍在切换,稍后再开 Wi-Fi");
            s_state = NET_FAILED;
            return;
        }
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        const esp_err_t ierr = esp_wifi_init(&cfg);
        if (ierr != ESP_OK) {
            s_start_fails++;
            ESP_LOGW(TAG, "Wi-Fi 启动失败 %s(第 %d 次,空闲 %u,蓝牙%s)", esp_err_to_name(ierr),
                     s_start_fails, (unsigned)sys_free_heap(), ble_radio_busy() ? "占用中" : "已关");
            s_state = NET_FAILED;
            return;
        }
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        esp_wifi_set_mode(mode);
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        const esp_err_t serr = esp_wifi_start();
        if (serr != ESP_OK) {
            esp_wifi_deinit();
            s_start_fails++;
            ESP_LOGW(TAG, "Wi-Fi 启动失败 %s(第 %d 次,空闲 %u)", esp_err_to_name(serr), s_start_fails,
                     (unsigned)sys_free_heap());
            s_state = NET_FAILED;
            return;
        }
        s_wifi_started = true;
        s_start_fails = 0;
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
    wifi_cred_t c;
    bool have = true;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_trial_on) {
        c = s_trial;
    } else if (s_saved.count) {
        if (s_cur < 0 || s_cur >= s_saved.count) s_cur = 0;
        c = s_saved.n[s_cur];
    } else {
        have = false;
    }
    xSemaphoreGive(s_lock);
    if (!have) {
        s_state = NET_NO_CONFIG;
        return;
    }
    wifi_config_t wc = {0};
    // ssid/password 字段不要求结尾 0:按字段长度拷贝。
    strncpy((char *)wc.sta.ssid, c.ssid, sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, c.pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = c.pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    snprintf(s_ssid, sizeof(s_ssid), "%s", c.ssid);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    s_state = NET_CONNECTING;
    esp_wifi_connect();
}

static bool wanted(void) { return s_refs > 0 || s_boot_sync || s_portal_on || s_trial_on; }

static void apply(void)
{
    if (!wanted()) {
        wifi_stop();
        s_start_fails = 0;
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
    ".card h2{font-size:16px;margin:0 0 8px}.card h3{font-size:14px;margin:14px 0 4px}.row{display:flex;justify-content:space-between;padding:4px 0;"
    "font-size:14px}.k{color:#a3adbd}.ok{color:#34d399}.warn{color:#fbbf24}"
    "label{display:block;margin:10px 0 6px;color:#a3adbd;font-size:13px}"
    "input,select{width:100%;box-sizing:border-box;padding:12px;border-radius:10px;border:1px solid #2a3241;"
    "background:#0b0e14;color:#f1f4f9;font-size:16px}.pw{display:flex;gap:8px}.pw input{flex:1}"
    ".pw button,.ghost{width:auto;margin:0;padding:0 14px;background:#1e2531;color:#f1f4f9;border:0;"
    "border-radius:10px;font-size:14px}.ap{display:flex;justify-content:space-between;padding:11px 12px;"
    "margin:6px 0;border-radius:10px;background:#0b0e14;border:1px solid #2a3241}.ap.on{border-color:#ff6b57}"
    ".s{color:#667185}details summary{color:#a3adbd;font-size:14px;padding:6px 0}"
    ".sv{display:flex;justify-content:space-between;align-items:center;padding:8px 12px;margin:6px 0;"
    "border-radius:10px;background:#0b0e14;border:1px solid #2a3241}.sv .ghost{padding:8px 12px}"
    ".chk{display:flex;gap:8px;align-items:center}.chk input{width:auto;margin:0}"
    ".go.dirty::after{content:' · 有改动未保存';font-weight:400}"
    ".cur{color:#34d399;font-size:13px}.go:disabled{opacity:.5}.msg{display:none;padding:12px;border-radius:12px;"
    "margin:10px 0 0;font-size:15px}.done{background:#1e2531;color:#f1f4f9}"
    ".go{margin-top:6px;width:100%;padding:15px;border:0;border-radius:12px;background:#ff6b57;color:#0b0e14;"
    "font-size:17px;font-weight:600}p{color:#667185;font-size:13px;line-height:1.5;margin:6px 0}</style></head><body>";

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
    "const $=q=>document.querySelector(q),A=q=>document.querySelectorAll(q);"
    "const wait=ms=>new Promise(r=>setTimeout(r,ms));"
    "const st=async()=>{try{return await(await fetch('/status',{cache:'no-store'})).json()}catch(_){return null}};"
    "const say=(f,t,c)=>{const m=f.querySelector('.msg');m.style.display='block';m.style.background=c;m.textContent=t};"
    "A('.ap').forEach(e=>e.onclick=()=>{A('.ap').forEach(x=>x.classList.remove('on'));e.classList.add('on');"
    "$('#ssid').value=e.dataset.s;$('#pass').value='';"
    "$('#pass').placeholder=e.dataset.k?'已保存,留空沿用原密码':'';$('#pass').focus()});"
    "A('.eye').forEach(b=>b.onclick=()=>{const i=b.previousElementSibling;"
    "i.type=i.type=='password'?'text':'password';b.textContent=i.type=='password'?'显示':'隐藏'});"
    "const cs=$('#cities').dataset.c.split('|').map(x=>x.split(','));const sel=$('#cities');"
    "cs.forEach(c=>{const o=document.createElement('option');o.value=c.join(',');o.textContent=c[0];sel.add(o)});"
    "sel.onchange=()=>{const v=sel.value;if(v=='auto'){$('[name=city]').value='';$('[name=lat]').value='';"
    "$('[name=lon]').value=''}else if(v=='typed'){$('[name=lat]').value='';$('[name=lon]').value='';"
    "$('[name=city]').focus()}else if(v!='keep'){const c=v.split(',');$('[name=city]').value=c[0];$('[name=lat]').value=c[1];"
    "$('[name=lon]').value=c[2]}};"
    "$('[name=city]').oninput=()=>{$('[name=lat]').value='';$('[name=lon]').value='';sel.value='typed'};"
    "const P={dssf:['https://api.deepseek.com/v1','deepseek-chat','https://api.siliconflow.cn/v1',"
    "'FunAudioLLM/SenseVoiceSmall','FunAudioLLM/CosyVoice2-0.5B','FunAudioLLM/CosyVoice2-0.5B:anna',16000,"
    "'对话密钥填 DeepSeek 的,语音密钥填硅基流动的。'],"
    "sf:['https://api.siliconflow.cn/v1','deepseek-ai/DeepSeek-V3','','FunAudioLLM/SenseVoiceSmall',"
    "'FunAudioLLM/CosyVoice2-0.5B','FunAudioLLM/CosyVoice2-0.5B:anna',16000,'只填对话密钥,语音沿用它。'],"
    "oa:['https://api.openai.com/v1','gpt-4o-mini','','whisper-1','gpt-4o-mini-tts','alloy',24000,"
    "'只填对话密钥,语音沿用它。']};"
    "const F=['api_base','llm_model','voice_base','asr_model','tts_model','tts_voice','tts_rate'];"
    "$('#prov').onchange=()=>{const p=P[$('#prov').value];"
    "if(!p){$('#provtip').textContent=$('#prov').value=='custom'?'按服务商文档填写;服务商没有识别或朗读接口时,语音接口另填一家。':'';return}"
    "F.forEach((k,i)=>{const e=$('[name='+k+']');e.value=p[i];e.dispatchEvent(new Event('input',{bubbles:true}))});"
    "$('#provtip').textContent=p[7]+'填好后分别点两张卡片的保存。'};"
    // 改了没保存的卡片在按钮上提示
    "A('form.sec').forEach(f=>{const b=f.querySelector('.go');"
    "f.addEventListener('input',()=>b.classList.add('dirty'));f.addEventListener('change',()=>b.classList.add('dirty'))});"
    // 每个分区单独保存;无线网络先试连,连上才算保存
    "A('form.sec').forEach(f=>f.onsubmit=async e=>{e.preventDefault();const b=f.querySelector('.go');"
    "const wifi=f.dataset.sec=='wifi';if(wifi&&!$('#ssid').value){say(f,'先选一个网络或输入网络名称','#7f1d1d');return}"
    "b.disabled=true;say(f,wifi?'正在发给设备…':'正在保存…','#1e2531');"
    "const fd=new URLSearchParams(new FormData(f));fd.set('sec',f.dataset.sec);let r;"
    "try{r=await(await fetch('/save',{method:'POST',body:fd})).json()}"
    "catch(_){say(f,'没送到设备,请重试','#7f1d1d');b.disabled=false;return}"
    "if(wifi){say(f,'正在连接 '+r.ssid+' …手机可能会短暂断开,稍等','#1e2531');"
    "for(let i=0;i<60;i++){await wait(1500);const s=await st();if(!s)continue;"
    "if(s.try=='ok'){say(f,'已连接 '+s.ssid+',已保存。','#065f46');setTimeout(()=>location.reload(),1800);return}"
    "if(s.try=='failed'){say(f,'连不上 '+r.ssid+',没有保存。请检查密码'+(s.saved?',设备已回到原来的网络。':'。'),'#7f1d1d');"
    "b.disabled=false;return}}"
    "say(f,'还没有结果,请看设备屏幕。','#1e2531');b.disabled=false;return}"
    "for(let i=0;i<25;i++){await wait(400);const s=await st();"
    "if(s&&s.applied>=r.seq){say(f,'已保存。可以继续改其他项,或点最下面的完成配置。','#065f46');"
    "b.disabled=false;b.classList.remove('dirty');f.querySelectorAll('[type=password]').forEach(x=>{if(x.value){"
    "x.value='';x.placeholder='已保存,留空不变'}});return}}"
    "say(f,'设备没有确认保存,请看设备屏幕后重试。','#7f1d1d');b.disabled=false});"
    "A('.forget').forEach(b=>b.onclick=async()=>{if(!confirm('忘记 '+b.dataset.s+'?以后不再自动连接它。'))return;"
    "b.disabled=true;try{await fetch('/forget',{method:'POST',body:new URLSearchParams({ssid:b.dataset.s})})}catch(_){}"
    "await wait(800);location.reload()});"
    "$('#done').onclick=async()=>{try{await fetch('/done',{method:'POST'})}catch(_){}"
    "document.body.innerHTML='<h2 style=\"padding:24px\">配置完成,设备热点已关闭。可以关掉这个页面了。</h2>'};"
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

static volatile uint32_t s_form_seq, s_applied_seq;
static volatile bool s_portal_done;

// JSON 字符串转义(网页用 textContent 显示,不需要 HTML 转义)。
static void json_escape(char *out, size_t len, const char *in)
{
    size_t o = 0;
    for (; *in && o + 3 < len; in++) {
        const unsigned char ch = (unsigned char)*in;
        if (ch == '"' || ch == '\\') {
            out[o++] = '\\';
            out[o++] = (char)ch;
        } else if (ch >= 0x20) {
            out[o++] = (char)ch;
        }
    }
    out[o] = '\0';
}

static void form_open(httpd_req_t *req, const char *sec, const char *title, const char *intro)
{
    char buf[420];
    snprintf(buf, sizeof(buf), "<form class=\"card sec\" data-sec=%s><h2>%s</h2>%s%s%s", sec, title,
             intro ? "<p>" : "", intro ? intro : "", intro ? "</p>" : "");
    chunk(req, buf);
}

static void form_close(httpd_req_t *req, const char *button)
{
    char buf[200];
    snprintf(buf, sizeof(buf), "<button class=go>%s</button><div class=msg></div></form>", button);
    chunk(req, buf);
}

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    chunk(req, k_css);
    char buf[260];
    snprintf(buf, sizeof(buf), "<h1>工具箱设置</h1><div class=sub>%s · 固件 %s · 每一项单独保存</div>",  // glyph-check: ignore
             g_settings.name, SYS_FW_VERSION);
    chunk(req, buf);

    // 已保存的网络(拷出来再输出,不在发网页时占着锁)
    static wifi_list_t saved;
    static net_ap_t aps[NET_SCAN_MAX];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    saved = s_saved;
    const int n = s_ap_count;
    memcpy(aps, s_aps, sizeof(net_ap_t) * (size_t)n);
    xSemaphoreGive(s_lock);
    const bool connected = s_wifi_started && s_state == NET_CONNECTED;

    // 当前状态
    chunk(req, "<div class=card><h2>当前状态</h2>");  // glyph-check: ignore
    {   // status_row 自己会转义
        if (connected) {
            snprintf(buf, sizeof(buf), "%s · %s", s_ssid, s_ip);
            status_row(req, "无线网络", buf, "ok");  // glyph-check: ignore
        } else if (s_wifi_started && s_state == NET_CONNECTING && s_ssid[0]) {
            snprintf(buf, sizeof(buf), "正在连接 %s", s_ssid);  // glyph-check: ignore
            status_row(req, "无线网络", buf, "warn");  // glyph-check: ignore
        } else {
            status_row(req, "无线网络", saved.count ? "没有连上" : "还没有保存网络", "warn");  // glyph-check: ignore
        }
    }
    snprintf(buf, sizeof(buf), "%s%s", g_settings.city[0] ? g_settings.city : "按网络自动定位",  // glyph-check: ignore
             g_settings.lat[0] ? "(经纬度已填)" : "");  // glyph-check: ignore
    status_row(req, "天气位置", buf, "s");  // glyph-check: ignore
    {
        static const char *const modes[] = {"自动", "只用 Mac 中转", "只用云端直连"};  // glyph-check: ignore
        const char *base, *key;
        voice_resolve(g_settings.api_base, g_settings.api_key, g_settings.voice_base, g_settings.voice_key,
                      &base, &key);
        snprintf(buf, sizeof(buf), "%s · 对话密钥%s · 语音%s", modes[g_settings.ai_mode],  // glyph-check: ignore
                 g_settings.api_key[0] ? "已保存" : "未设置",  // glyph-check: ignore
                 !g_settings.voice_base[0] ? "同对话" : key[0] ? "密钥已保存" : "缺密钥");  // glyph-check: ignore
        status_row(req, "语音助手", buf, g_settings.api_key[0] && key[0] ? "ok" : "warn");  // glyph-check: ignore
    }
    chunk(req, "</div>");

    // 无线网络:已保存的 + 附近的
    form_open(req, "wifi", "无线网络",  // glyph-check: ignore
              "最多保存 5 个网络(家里、公司、手机热点都可以)。当前网络连不上时,设备会自动换一个已保存的。"  // glyph-check: ignore
              "新网络会先试连,连上才保存;连不上就回到原来的网络。");  // glyph-check: ignore
    snprintf(buf, sizeof(buf), "<h3>已保存(%d/%d)</h3>", (int)saved.count, WIFI_SAVED_MAX);  // glyph-check: ignore
    chunk(req, buf);
    if (!saved.count) chunk(req, "<p>还没有。从下面选一个网络连接。</p>");  // glyph-check: ignore
    for (int i = 0; i < saved.count; i++) {
        char esc[100], row[520];
        html_escape(esc, sizeof(esc), saved.n[i].ssid);
        const bool cur = connected && !strcmp(saved.n[i].ssid, s_ssid);
        snprintf(row, sizeof(row),
                 "<div class=sv><span>%s %s<span class=s>%s</span></span>"
                 "<button type=button class=\"ghost forget\" data-s=\"%s\">忘记</button></div>",  // glyph-check: ignore
                 esc, cur ? "<span class=cur>● 当前连接</span>" : "",  // glyph-check: ignore
                 saved.n[i].pass[0] ? "" : " 开放网络", esc);  // glyph-check: ignore
        chunk(req, row);
    }
    chunk(req, "<h3>附近的网络</h3>");  // glyph-check: ignore
    for (int i = 0; i < n; i++) {   // 按信号排序
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
        char esc[100], row[460];
        html_escape(esc, sizeof(esc), aps[i].ssid);
        const int bars = aps[i].rssi >= -55 ? 4 : aps[i].rssi >= -67 ? 3 : aps[i].rssi >= -78 ? 2 : 1;
        const bool known = wifi_list_find(&saved, aps[i].ssid) >= 0;
        snprintf(row, sizeof(row), "<div class=ap data-s=\"%s\"%s><span>%s%s</span><span class=s>%s%s</span></div>",
                 esc, known ? " data-k=1" : "", esc, known ? " <b style='color:#4ade80'>✓ 已保存</b>" : "",  // glyph-check: ignore
                 aps[i].secure ? "" : "开放 · ",  // glyph-check: ignore
                 bars == 4 ? "●●●●" : bars == 3 ? "●●●○" : bars == 2 ? "●●○○" : "●○○○");  // glyph-check: ignore
        chunk(req, row);
        shown++;
    }
    if (!shown) chunk(req, s_scan == SCAN_RUNNING ? "<p>正在扫描…</p>" : "<p>没有扫描到网络。</p>");  // glyph-check: ignore
    chunk(req, "<p><a href=/rescan style='color:#ff6b57'>重新扫描</a></p>"  // glyph-check: ignore
               "<label>网络名称(点上面的列表,或手动输入隐藏网络)</label>"  // glyph-check: ignore
               "<input name=ssid id=ssid autocomplete=off>"
               "<label>密码(开放网络留空)</label><div class=pw><input name=pass id=pass type=password autocomplete=off>"  // glyph-check: ignore
               "<button type=button class=eye>显示</button></div>");  // glyph-check: ignore
    form_close(req, "连接并保存");  // glyph-check: ignore

    // 天气位置
    form_open(req, "weather", "天气位置", "选城市最准;区县可以直接输入名字,设备联网后自动查经纬度。");  // glyph-check: ignore
    chunk(req, "<select id=cities data-c=\"");
    chunk(req, k_cities);
    chunk(req, "\"><option value=keep>保持当前设置</option><option value=auto>按网络自动定位</option>"  // glyph-check: ignore
              "<option value=typed>手动输入城市 / 区县</option></select>");  // glyph-check: ignore
    field(req, "城市 / 区县名", "city", g_settings.city, false, "例:浦东、海淀、余杭");  // glyph-check: ignore
    chunk(req, "<details><summary>精确经纬度(可选)</summary>");  // glyph-check: ignore
    field(req, "纬度", "lat", g_settings.lat, false, "例:31.23");  // glyph-check: ignore
    field(req, "经度", "lon", g_settings.lon, false, "例:121.47");  // glyph-check: ignore
    chunk(req, "</details>");
    form_close(req, "保存天气位置");  // glyph-check: ignore

    // 语音助手:使用方式、服务商预设、对话、识别与朗读,各自保存
    chunk(req, "<h1 style='font-size:18px;margin:22px 0 0'>语音助手</h1><p>语音助手要三步:识别(语音转文字)、"  // glyph-check: ignore
               "对话、朗读。DeepSeek 这类只提供对话的服务,需要另配一家做识别和朗读,或者交给家里的 Mac 中转在本地完成。</p>");  // glyph-check: ignore
    form_open(req, "mode", "使用方式", NULL);  // glyph-check: ignore
    {
        char sel[700];
        const int m = g_settings.ai_mode;
        snprintf(sel, sizeof(sel),
                 "<select name=ai_mode>"
                 "<option value=0%s>自动:找得到 Mac 中转就用中转,否则云端直连</option>"  // glyph-check: ignore
                 "<option value=1%s>只用 Mac 中转(识别和朗读在 Mac 本地)</option>"  // glyph-check: ignore
                 "<option value=2%s>只用云端直连</option></select>",  // glyph-check: ignore
                 m == 0 ? " selected" : "", m == 1 ? " selected" : "", m == 2 ? " selected" : "");
        chunk(req, sel);
    }
    form_close(req, "保存使用方式");  // glyph-check: ignore
    chunk(req, "<div class=card><h2>服务商预设(可选)</h2><p>选一个会把下面两张卡片的地址和模型填好,"  // glyph-check: ignore
               "再分别填密钥、分别保存。</p><select id=prov><option value=keep>不使用预设</option>"  // glyph-check: ignore
               "<option value=dssf>DeepSeek 对话 + 硅基流动语音(两个密钥)</option>"  // glyph-check: ignore
               "<option value=sf>硅基流动全包(一个密钥)</option>"  // glyph-check: ignore
               "<option value=oa>OpenAI 全包(一个密钥)</option>"  // glyph-check: ignore
               "<option value=custom>自定义兼容 OpenAI 的接口</option></select><p id=provtip></p></div>");  // glyph-check: ignore

    form_open(req, "chat", "对话", "密钥留空表示不修改。换了接口地址却没填新密钥时,旧密钥会自动清除,不会发给别家。");  // glyph-check: ignore
    field(req, "对话密钥", "api_key", g_settings.api_key, true, "sk-...");  // glyph-check: ignore
    field(req, "对话接口地址", "api_base", g_settings.api_base, false, "https://api.deepseek.com/v1");  // glyph-check: ignore
    field(req, "对话模型", "llm_model", g_settings.llm_model, false, "deepseek-chat");  // glyph-check: ignore
    chunk(req, "<label class=chk><input type=checkbox name=clr value=1>清除已保存的对话密钥</label>");  // glyph-check: ignore
    form_close(req, "保存对话设置");  // glyph-check: ignore

    form_open(req, "speech", "识别与朗读", "接口留空表示和对话用同一家、同一个密钥。");  // glyph-check: ignore
    field(req, "语音密钥", "voice_key", g_settings.voice_key, true, "留空 = 同对话密钥");  // glyph-check: ignore
    field(req, "语音接口地址", "voice_base", g_settings.voice_base, false, "留空 = 同对话接口");  // glyph-check: ignore
    field(req, "识别模型", "asr_model", g_settings.asr_model, false, NULL);  // glyph-check: ignore
    field(req, "朗读模型", "tts_model", g_settings.tts_model, false, NULL);  // glyph-check: ignore
    field(req, "朗读声音", "tts_voice", g_settings.tts_voice, false, NULL);  // glyph-check: ignore
    {
        char sel[420];
        const int r = g_settings.tts_rate;
        snprintf(sel, sizeof(sel),
                 "<label>朗读音频格式</label><select name=tts_rate>"  // glyph-check: ignore
                 "<option value=16000%s>16 kHz PCM(硅基流动等)</option>"  // glyph-check: ignore
                 "<option value=24000%s>24 kHz PCM(OpenAI)</option>"  // glyph-check: ignore
                 "<option value=0%s>不朗读,只显示文字</option></select>",  // glyph-check: ignore
                 r == VOICE_TTS_16K ? " selected" : "", r == VOICE_TTS_24K ? " selected" : "",
                 r == VOICE_TTS_OFF ? " selected" : "");
        chunk(req, sel);
    }
    chunk(req, "<label class=chk><input type=checkbox name=clr value=1>清除已保存的语音密钥</label>"  // glyph-check: ignore
               "<p>密钥保存在设备里,请勿把设备借给不信任的人。</p>");  // glyph-check: ignore
    form_close(req, "保存识别与朗读");  // glyph-check: ignore

    // 其他:各自保存
    chunk(req, "<h1 style='font-size:18px;margin:22px 0 0'>更多(可选)</h1>");  // glyph-check: ignore
    form_open(req, "ha", "家庭助手", "长期访问令牌在 Home Assistant 的个人资料页底部创建;实体用逗号分隔。");  // glyph-check: ignore
    field(req, "地址", "ha_url", g_settings.ha_url, false, "http://192.168.1.5:8123");  // glyph-check: ignore
    field(req, "长期访问令牌", "ha_token", g_settings.ha_token, true, NULL);  // glyph-check: ignore
    field(req, "实体", "ha_ents", g_settings.ha_entities, false, "light.living_room, switch.fan");  // glyph-check: ignore
    form_close(req, "保存家庭助手");  // glyph-check: ignore
    form_open(req, "cal", "日历(信息看板显示日程)", "在 iCloud / Google / Outlook / 飞书 / 钉钉等日历里找"  // glyph-check: ignore
              "\"订阅链接\"或\"公开 iCal 地址\",粘贴到这里。");  // glyph-check: ignore
    field(req, "日历订阅地址(.ics 或 webcal://)", "ics", g_settings.ics_url, false, "https://...ics");  // glyph-check: ignore
    form_close(req, "保存日历");  // glyph-check: ignore
    form_open(req, "server", "家里的中转服务器", "留空会自动在局域网查找;找不到时其他功能照常直连。");  // glyph-check: ignore
    field(req, "服务器地址", "server", g_settings.server, false, "http://192.168.1.20:8787");  // glyph-check: ignore
    form_close(req, "保存服务器地址");  // glyph-check: ignore

    chunk(req, "<button id=done class=\"go done\" type=button>完成配置,关闭设备热点</button>");  // glyph-check: ignore
    chunk(req, k_script);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t page_status(httpd_req_t *req)
{
    static const char *const names[] = {"off", "no_config", "connecting", "connected", "failed"};
    static const char *const tries[] = {"none", "running", "ok", "failed"};
    char esc[80], buf[260];
    json_escape(esc, sizeof(esc), s_ssid);
    snprintf(buf, sizeof(buf), "{\"net\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"try\":\"%s\",\"saved\":%d,\"applied\":%u}",
             names[s_wifi_started ? s_state : NET_OFF], esc, s_ip, tries[s_try], (int)s_saved.count,
             (unsigned)s_applied_seq);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
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

// 读整个表单(上限 3KB;超出就拒绝,免得截断后悄悄丢字段)。成功返回需要 free 的缓冲。
static char *read_body(httpd_req_t *req)
{
    enum { BODY_MAX = 3072 };
    if (req->content_len >= BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "form too large");
        return NULL;
    }
    char *body = malloc(BODY_MAX);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return NULL;
    }
    const int n = req->content_len;
    int got = 0;
    while (got < n) {
        const int r = httpd_req_recv(req, body + got, n - got);
        if (r <= 0) {
            free(body);
            return NULL;
        }
        got += r;
    }
    body[got] = '\0';
    return body;
}

// 试连手机上提交的网络:已保存的网络密码留空 = 沿用原密码。
static void start_try(const char *ssid, const char *pass)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int at = wifi_list_find(&s_saved, ssid);
    memset(&s_trial, 0, sizeof(s_trial));
    snprintf(s_trial.ssid, sizeof(s_trial.ssid), "%s", ssid);
    snprintf(s_trial.pass, sizeof(s_trial.pass), "%s", !pass[0] && at >= 0 ? s_saved.n[at].pass : pass);
    snprintf(s_try_ssid, sizeof(s_try_ssid), "%s", ssid);
    s_trial_on = true;
    s_try = NET_TRY_RUNNING;
    xSemaphoreGive(s_lock);
    post(CMD_TRY);
}

static esp_err_t page_save(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) return ESP_FAIL;
    char sec[12], buf[120];
    form_field(body, "sec", sec, sizeof(sec));
    httpd_resp_set_type(req, "application/json");
    if (!strcmp(sec, "wifi")) {
        char ssid[33], pass[65], esc[80];
        form_field(body, "ssid", ssid, sizeof(ssid));
        form_field(body, "pass", pass, sizeof(pass));
        free(body);
        if (!ssid[0]) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
            return ESP_FAIL;
        }
        start_try(ssid, pass);
        s_portal = PORTAL_SAVED;
        json_escape(esc, sizeof(esc), ssid);
        snprintf(buf, sizeof(buf), "{\"ssid\":\"%s\"}", esc);
        return httpd_resp_sendstr(req, buf);
    }
    static const struct { const char *name; portal_sec_t sec; } k_secs[] = {
        {"weather", PORTAL_SEC_WEATHER}, {"mode", PORTAL_SEC_MODE}, {"chat", PORTAL_SEC_CHAT},
        {"speech", PORTAL_SEC_SPEECH},   {"ha", PORTAL_SEC_HA},     {"cal", PORTAL_SEC_CAL},
        {"server", PORTAL_SEC_SERVER},
    };
    portal_sec_t section = 0;
    for (size_t k = 0; k < sizeof(k_secs) / sizeof(k_secs[0]); k++) {
        if (!strcmp(sec, k_secs[k].name)) section = k_secs[k].sec;
    }
    if (!section) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown section");
        return ESP_FAIL;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_form, 0, sizeof(s_form));
    s_form.section = section;
    s_form.seq = ++s_form_seq;
    form_field(body, "server", s_form.server, sizeof(s_form.server));
    char num[8];
    form_field(body, "ai_mode", num, sizeof(num));
    s_form.ai_mode = num[0] ? atoi(num) : -1;
    form_field(body, "tts_rate", num, sizeof(num));
    s_form.tts_rate = num[0] ? atoi(num) : -1;
    form_field(body, "clr", num, sizeof(num));   // 本卡片的"清除密钥"
    s_form.clear_keys = num[0] == '1';
    form_field(body, "api_base", s_form.api_base, sizeof(s_form.api_base));
    form_field(body, "api_key", s_form.api_key, sizeof(s_form.api_key));
    form_field(body, "voice_base", s_form.voice_base, sizeof(s_form.voice_base));
    form_field(body, "voice_key", s_form.voice_key, sizeof(s_form.voice_key));
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
    form_field(body, "ics", s_form.ics_url, sizeof(s_form.ics_url));
    s_form.valid = true;
    const uint32_t seq = s_form.seq;
    xSemaphoreGive(s_lock);
    free(body);
    // 设备界面应用并写入存储后才 ack;网页等到 applied >= seq 才显示"已保存"。
    snprintf(buf, sizeof(buf), "{\"seq\":%u}", (unsigned)seq);
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t page_forget(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) return ESP_FAIL;
    char ssid[33];
    form_field(body, "ssid", ssid, sizeof(ssid));
    free(body);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_forget, sizeof(s_forget), "%s", ssid);
    xSemaphoreGive(s_lock);
    post(CMD_FORGET);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{}");
}

static esp_err_t page_done(httpd_req_t *req)
{
    s_portal_done = true;   // 设备界面看到后关闭热点
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{}");
}

void net_portal_ack(uint32_t seq) { s_applied_seq = seq; }

bool net_portal_done(void)
{
    const bool d = s_portal_done;
    s_portal_done = false;
    return d;
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

static char s_portal_pass[9];

const char *net_portal_pass(void) { return s_portal_pass; }

static void portal_start(void)
{
    s_portal_on = true;
    s_portal = PORTAL_STARTING;
    s_portal_done = false;
    s_try = NET_TRY_NONE;
    snprintf(s_portal_ssid, sizeof(s_portal_ssid), "Passport-%s", sys_short_id());
    // 每次开热点换一个 8 位数字密码(设备屏幕上显示,二维码里也带着):
    // 以前是开放热点,附近任何人都能连上来改设置。
    snprintf(s_portal_pass, sizeof(s_portal_pass), "%08u", (unsigned)(esp_random() % 100000000u));
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
    strncpy((char *)ap.ap.password, s_portal_pass, sizeof(ap.ap.password));
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    if (esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK) {
        s_portal = PORTAL_FAILED;
        return;
    }
    esp_wifi_scan_start(NULL, false);   // 给表单提供附近网络列表
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.stack_size = 8192;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_httpd, &cfg) == ESP_OK) {
        const httpd_uri_t uris[] = {
            {.uri = "/", .method = HTTP_GET, .handler = page_get},
            {.uri = "/save", .method = HTTP_POST, .handler = page_save},
            {.uri = "/status", .method = HTTP_GET, .handler = page_status},
            {.uri = "/rescan", .method = HTTP_GET, .handler = page_rescan},
            {.uri = "/forget", .method = HTTP_POST, .handler = page_forget},
            {.uri = "/done", .method = HTTP_POST, .handler = page_done},
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
                        // 连不上:先换下一个已保存的网络;都不行就 30 秒后从头再试。
                        if (!fallback()) retry_at = sys_now_ms() + 30000;
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
            case CMD_GOT_IP: on_got_ip(); break;
            case CMD_TRY:
                retry_at = 0;
                s_retries = 0;
                if (s_wifi_started) {
                    s_state = NET_OFF;
                    esp_wifi_disconnect();
                }
                apply();
                break;
            case CMD_FORGET: {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                const bool removed = wifi_list_remove(&s_saved, s_forget);
                s_cur = wifi_list_find(&s_saved, s_ssid);   // 下标变了:重新对上当前网络
                xSemaphoreGive(s_lock);
                if (removed) {
                    ESP_LOGI(TAG, "已忘记 %s", s_forget);
                    saved_persist();
                }
                break;
            }
            case CMD_PORTAL_START: portal_start(); break;
            case CMD_PORTAL_STOP: portal_stop(); apply(); break;
            }
        }
        const int64_t now = sys_now_ms();
        // 以前 Wi-Fi 启动失败一次就不再尝试,界面会一直显示"没有网络"。现在每 2 秒重试。
        static int64_t start_retry_at;
        if (wanted() && !s_wifi_started && s_start_fails > 0 && now >= start_retry_at) {
            start_retry_at = now + 2000;
            apply();
        }
        if (retry_at && now >= retry_at) {
            retry_at = 0;
            s_retries = 0;
            if (s_wifi_started) {
                s_state = NET_OFF;
                apply();
            }
        }
        // 开机对时:时间对好 且 第一次服务器发现已经做完 才关 Wi-Fi
        // (以前时间一对好就关,会把正在进行的发现打断)。最多 60 秒。
        if (s_boot_sync && ((time_valid() && relay_discovery_attempted()) || now >= boot_sync_until)) {
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
    saved_load();
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
    if (!s_wifi_started) {
        if (!settings_has_wifi()) return NET_NO_CONFIG;
        // 有应用要用网但射频还没起来(等蓝牙释放 / 重试中):显示"正在连接",多次失败才算失败。
        if (wanted()) return s_start_fails >= 8 ? NET_FAILED : NET_CONNECTING;
        return NET_OFF;
    }
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
