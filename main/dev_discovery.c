/**
 * @file  dev_discovery.c
 * @brief 设备发现：UDP 套接字 + 身份识别 + 心跳轮询
 *
 * 三个入口：
 *   1) DHCP 租约事件（被动）  —— 设备刚拿到 IP，立刻单播 QUERY_INFO
 *   2) QUERY_INFO / DISCOVER  —— 工具主动探测，设备回 DISCOVER_RSP
 *   3) ANNOUNCE               —— 设备主动上报（支持静态 IP 设备）
 */
#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"

#include "app_config.h"
#include "dev_discovery.h"
#include "dev_table.h"
#include "net_eth.h"
#include "ota_frame.h"

static const char *TAG = "dev_disc";

/* 网络字节序 uint32 -> 四个十进制数，日志用 */
#define IP4_OCTETS(ip) \
    (int)((ip) & 0xFF), (int)(((ip) >> 8) & 0xFF), \
    (int)(((ip) >> 16) & 0xFF), (int)(((ip) >> 24) & 0xFF)

static int s_sock = -1;
static uint32_t s_seq = 0;

/* 累计收到的 UDP 包数。用处很直接：设备表里一直有“身份未知”的条目时，
 * 看这个计数就能分清是「对端没回」还是「包根本没进来」。 */
static uint32_t s_rx_frames = 0;

/* 发送缓冲是共享的：poll 任务、启动时的一次性广播、将来 UI 的"刷新"按钮都会走到这里，
 * 必须串行化，否则两个上下文同时写 s_tx_frame 会发出错帧。 */
static SemaphoreHandle_t s_tx_lock = NULL;

/* 复用静态缓冲避免 1 KB 栈占用 */
static ota_frame_t s_tx_frame;
static uint8_t     s_tx_buf[OTA_FRAME_MAX];

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/* ------------------------------------------------------------------ */
/* 发送                                                                */
/* ------------------------------------------------------------------ */

static void send_frame(uint32_t dst_ip_net, uint8_t cmd, const uint8_t *payload, uint16_t plen)
{
    if (s_sock < 0 || plen > OTA_MAX_PAYLOAD) {
        return;
    }

    if (s_tx_lock) {
        xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    }

    s_tx_frame.ver   = OTA_PROTO_VER;
    s_tx_frame.cmd   = cmd;
    s_tx_frame.len   = plen;
    s_tx_frame.seq   = ++s_seq;
    s_tx_frame.flags = 0;
    if (plen > 0 && payload != NULL) {
        memcpy(s_tx_frame.payload, payload, plen);
    }

    size_t total = ota_frame_encode(&s_tx_frame, s_tx_buf, sizeof(s_tx_buf));
    if (total == 0) {
        if (s_tx_lock) {
            xSemaphoreGive(s_tx_lock);
        }
        ESP_LOGE(TAG, "帧编码失败 cmd=0x%02x len=%u", (unsigned)cmd, (unsigned)plen);
        return;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(APP_DISCOVERY_PORT);
    dst.sin_addr.s_addr = dst_ip_net;

    int n = sendto(s_sock, s_tx_buf, total, 0, (struct sockaddr *)&dst, sizeof(dst));

    if (s_tx_lock) {
        xSemaphoreGive(s_tx_lock);
    }

    if (n < 0) {
        ESP_LOGW(TAG, "sendto %d.%d.%d.%d 失败 errno=%d", IP4_OCTETS(dst_ip_net), errno);
    }
}

/* ------------------------------------------------------------------ */
/* 轮询：由 dev_table_poll_pass 逐个槽回调                              */
/* ------------------------------------------------------------------ */

static uint32_t on_poll_due(const dev_entry_t *e, uint32_t now)
{
    (void)now;
    send_frame(e->ip, OTA_CMD_QUERY_INFO, NULL, 0);

    if (e->identified) {
        /* 已识别：在线走心跳，离线低频等复活 */
        return e->online ? APP_POLL_INTERVAL_MS : APP_OFFLINE_PROBE_MS;
    }

    /* 未识别：1 s 快探 3 次，之后 10 s 慢探。
     * 快档用于抓"设备刚拿到租约、UDP 服务已起来"的常见场景；
     * 慢档避免对 PC / 非 OTA 设备持续刷包（miss 在此前已累加到 ≥3）。 */
    return (e->miss < APP_IDENTIFY_FAST_TRIES) ? APP_IDENTIFY_RETRY_MS
                                               : APP_IDENTIFY_SLOW_MS;
}

/* ------------------------------------------------------------------ */
/* 收包处理                                                            */
/* ------------------------------------------------------------------ */

static const char *uid_str(const uint8_t uid[DEV_UID_LEN])
{
    static char buf[4][DEV_UID_LEN * 2 + 1];
    static int idx = 0;

    char *p = buf[idx];
    idx = (idx + 1) & 3;
    for (int i = 0; i < DEV_UID_LEN; i++) {
        static const char hex[] = "0123456789abcdef";
        p[i * 2]     = hex[uid[i] >> 4];
        p[i * 2 + 1] = hex[uid[i] & 0x0F];
    }
    p[DEV_UID_LEN * 2] = '\0';
    return p;
}

static void handle_identity(const ota_frame_t *f, uint32_t src_ip_net)
{
    dev_identity_t id;
    if (!dev_identity_parse(f->payload, f->len, &id)) {
        ESP_LOGW(TAG, "身份字段长度不足：%u < %u", (unsigned)f->len, (unsigned)DEV_IDENTITY_WIRE_LEN);
        return;
    }

    uint32_t now = now_ms();

    dev_entry_t *e = dev_table_find_ip(src_ip_net);
    if (e == NULL) {
        e = dev_table_find_uid(id.uid);            /* 设备换了 IP */
    }
    if (e == NULL) {
        e = dev_table_claim_ip(src_ip_net, "身份应答（不在 DHCP 租约表）");
    }
    if (e == NULL) {
        return;
    }

    /* 心跳 5 s 一次，稳态下不能每次都打。只有首次识别、换 IP、或身份字段
     * （型号 / 硬件 / 固件版本 / Flash / caps / UID）变化时才输出一行。
     * fw_ver 必须留在变化集合里 —— OTA 成功与否就是靠它体现的。 */
    dev_identity_t old_id = e->id;
    bool           was_identified = e->identified;
    uint32_t       old_ip = e->ip;

    dev_table_apply_identity(e, &id, now);

    bool changed = !was_identified
                || old_ip != src_ip_net
                || old_id.hw_ver != id.hw_ver
                || old_id.flash_size != id.flash_size
                || old_id.caps != id.caps
                || memcmp(old_id.uid, id.uid, DEV_UID_LEN) != 0
                || memcmp(old_id.fw_ver, id.fw_ver, sizeof(id.fw_ver)) != 0
                || strcmp(old_id.model, id.model) != 0;

    if (!changed) {
        ESP_LOGD(TAG, "心跳正常 %d.%d.%d.%d", IP4_OCTETS(src_ip_net));
        return;
    }

    ESP_LOGI(TAG, "%s %d.%d.%d.%d  model='%s' hw=%u.%u fw=%u.%u.%u.%u flash=%" PRIu32 "KB caps=0x%04x uid=%s",
             was_identified ? "设备信息更新" : "识别到设备",
             IP4_OCTETS(src_ip_net),
             id.model,
             (unsigned)(id.hw_ver >> 8), (unsigned)(id.hw_ver & 0xFF),
             id.fw_ver[0], id.fw_ver[1], id.fw_ver[2], id.fw_ver[3],
             id.flash_size / 1024u,
             id.caps,
             uid_str(id.uid));
}

static void rx_task(void *arg)
{
    static uint8_t  rx[1500];
    static ota_frame_t f;   /* 单任务使用，放静态区省栈 */

    ESP_LOGI(TAG, "收包任务启动，监听 UDP %d", APP_DISCOVERY_PORT);

    while (true) {
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);

        int n = recvfrom(s_sock, rx, sizeof(rx), 0, (struct sockaddr *)&src, &sl);
        if (n <= 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        s_rx_frames++;
        ESP_LOGD(TAG, "收到 %d 字节，来自 %d.%d.%d.%d", n, IP4_OCTETS(src.sin_addr.s_addr));

        if (!ota_frame_decode(rx, (size_t)n, &f)) {
            ESP_LOGW(TAG, "非法帧（%d 字节，MAGIC/CRC 不符），丢弃", n);
            continue;
        }
        if (f.ver != OTA_PROTO_VER) {
            ESP_LOGW(TAG, "协议版本不匹配：收到 %u，本机 %u", (unsigned)f.ver, (unsigned)OTA_PROTO_VER);
            continue;
        }

        switch (f.cmd) {
        case OTA_CMD_DISCOVER_RSP:
        case OTA_CMD_ANNOUNCE:
            handle_identity(&f, src.sin_addr.s_addr);
            break;

        case OTA_CMD_DISCOVER:
            /* 别的工具在扫描，本机是 Server 侧，不回 */
            ESP_LOGD(TAG, "收到来自 %d.%d.%d.%d 的 DISCOVER，忽略", IP4_OCTETS(src.sin_addr.s_addr));
            break;

        default:
            ESP_LOGD(TAG, "忽略命令 0x%02x（%u 字节）", f.cmd, f.len);
            break;
        }
    }
}

static void poll_task(void *arg)
{
    uint32_t last_bcast = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(APP_POLL_TASK_TICK_MS));

        uint32_t now = now_ms();

        /* 链路断开：网段整个不存在，设备表也已被上层清空。
         * 此时继续 sendto 只会每次失败刷一条 WARN，干脆空转到链路恢复；
         * 恢复后 last_bcast 早已过期，会立刻补发一次广播。 */
        if (!net_eth_link_up()) {
            continue;
        }

        /* 单播：识别 / 保活 / miss 计数 */
        dev_table_poll_pass(now, on_poll_due);

        /* 兜底广播：覆盖静态 IP 设备、以及工具重启后租约表为空的情况 */
        if (now - last_bcast >= APP_DISCOVER_BROADCAST_MS) {
            send_frame(ESP_IP4TOADDR(APP_NET_A, APP_NET_B, APP_NET_C, 255),
                       OTA_CMD_DISCOVER, NULL, 0);
            last_bcast = now;
            ESP_LOGD(TAG, "广播 DISCOVER -> %d.%d.%d.255", APP_NET_A, APP_NET_B, APP_NET_C);
        }
    }
}

/* ------------------------------------------------------------------ */
/* DHCP 租约事件：被动发现的主入口                                      */
/* ------------------------------------------------------------------ */

static void on_dhcp_lease(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_assigned_ip_to_client_t *e = (const ip_event_assigned_ip_to_client_t *)data;

    char host[sizeof(e->hostname) + 1];
    memcpy(host, e->hostname, sizeof(e->hostname));
    host[sizeof(e->hostname)] = '\0';

    dev_entry_t *entry = dev_table_upsert_lease(e->mac, e->ip.addr, host);
    if (entry == NULL) {
        return;
    }

    ESP_LOGI(TAG, "租约入表：%d.%d.%d.%d  %s  host='%s'，立即探测身份",
             IP4_OCTETS(e->ip.addr), dev_mac_str(e->mac), host);
    /* next_poll_ms 已被置 0，poll 任务下一拍（≤500 ms）就会发 QUERY_INFO */
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

esp_err_t dev_discovery_init(void)
{
    ESP_RETURN_ON_FALSE(s_sock < 0, ESP_ERR_INVALID_STATE, TAG, "重复初始化");

    ESP_RETURN_ON_ERROR(dev_table_init(), TAG, "设备表初始化失败");

    if (s_tx_lock == NULL) {
        s_tx_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_tx_lock != NULL, ESP_ERR_NO_MEM, TAG, "创建发送锁失败");
    }

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "创建 UDP 套接字失败 errno=%d", errno);
        return ESP_FAIL;
    }

    int opt = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(APP_DISCOVERY_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "绑定 UDP %d 失败 errno=%d", APP_DISCOVERY_PORT, errno);
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT,
                                                  on_dhcp_lease, NULL),
                        TAG, "注册租约事件失败");

    ESP_LOGI(TAG, "发现模块就绪：UDP %d，广播周期 %d ms，保活 %d ms",
             APP_DISCOVERY_PORT, APP_DISCOVER_BROADCAST_MS, APP_POLL_INTERVAL_MS);
    return ESP_OK;
}

esp_err_t dev_discovery_start(void)
{
    ESP_RETURN_ON_FALSE(s_sock >= 0, ESP_ERR_INVALID_STATE, TAG, "先调 dev_discovery_init");

    if (xTaskCreate(rx_task, "dev_rx", 4096, NULL, tskIDLE_PRIORITY + 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(poll_task, "dev_poll", 4096, NULL, tskIDLE_PRIORITY + 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    dev_discovery_broadcast_now();
    return ESP_OK;
}

void dev_discovery_broadcast_now(void)
{
    /* 链路没起来时广播必然发不出去（sendto 只会返回 ENETUNREACH 并留一条 WARN），
     * 启动阶段没插网线就是这种情况。静默跳过，等链路 UP 后由上层补发。 */
    if (!net_eth_link_up()) {
        ESP_LOGD(TAG, "链路未 up，跳过本次 DISCOVER 广播");
        return;
    }

    send_frame(ESP_IP4TOADDR(APP_NET_A, APP_NET_B, APP_NET_C, 255),
               OTA_CMD_DISCOVER, NULL, 0);
    ESP_LOGI(TAG, "已广播 DISCOVER 到 %d.%d.%d.255:%d",
             APP_NET_A, APP_NET_B, APP_NET_C, APP_DISCOVERY_PORT);
}

/* 表内容签名：槽数 + 每台 IP / 在线 / 已识别 / 固件版本 / 型号。
 * 只用来判断"整张表要不要重打一遍"，不要求抗碰撞强度。 */
static uint32_t table_signature(const dev_entry_t *snap, int n)
{
    uint32_t h = 2166136261u ^ (uint32_t)n;

    for (int i = 0; i < n; i++) {
        const dev_entry_t *e = &snap[i];
        h = h * 16777619u ^ e->ip;
        h = h * 16777619u ^ (uint32_t)((e->online ? 1u : 0u) | (e->identified ? 2u : 0u));
        for (int k = 0; k < 4; k++) {
            h = h * 16777619u ^ e->id.fw_ver[k];
        }
        for (const char *p = e->id.model; *p != '\0'; p++) {
            h = h * 16777619u ^ (uint8_t)*p;
        }
    }
    return h;
}

void dev_discovery_log_table(void)
{
    static uint32_t last_sig = 0;
    static bool     have_sig = false;

    dev_entry_t snap[APP_DEV_MAX];
    int n = dev_table_snapshot(snap, APP_DEV_MAX);

    uint32_t sig = table_signature(snap, n);
    if (have_sig && sig == last_sig) {
        return;                     /* 表没变：一个字都不打 */
    }
    have_sig = true;
    last_sig = sig;

    if (n == 0) {
        ESP_LOGI(TAG, "设备表为空（等待 PC/设备接入；累计收包 %" PRIu32 "，若一直为 0 说明包没进来，"
                      "查 PC 防火墙是否放行 UDP %d）",
                 s_rx_frames, APP_DISCOVERY_PORT);
        return;
    }

    ESP_LOGI(TAG, "设备表：%d 台（累计收包 %" PRIu32 "）", n, s_rx_frames);
    for (int i = 0; i < n; i++) {
        const dev_entry_t *e = &snap[i];
        if (e->identified) {
            ESP_LOGI(TAG, "  [%d] %d.%d.%d.%d %-17s %-10s %s  model='%s' hw=%u.%u fw=%u.%u.%u.%u %" PRIu32 "KB",
                     i,
                     IP4_OCTETS(e->ip),
                     dev_mac_str(e->mac),
                     e->hostname[0] ? e->hostname : "-",
                     e->online ? "ONLINE" : "OFFLINE",
                     e->id.model,
                     (unsigned)(e->id.hw_ver >> 8), (unsigned)(e->id.hw_ver & 0xFF),
                     e->id.fw_ver[0], e->id.fw_ver[1], e->id.fw_ver[2], e->id.fw_ver[3],
                     e->id.flash_size / 1024u);
        } else {
            ESP_LOGI(TAG, "  [%d] %d.%d.%d.%d %-17s %-10s %s  仅 DHCP 租约，未响应发现协议",
                     i,
                     IP4_OCTETS(e->ip),
                     dev_mac_str(e->mac),
                     e->hostname[0] ? e->hostname : "-",
                     e->online ? "待识别" : "无应答");
        }
    }
}
