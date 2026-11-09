/**
 * @file  net_dhcps.c
 * @brief DHCP Server：地址池、租约、启动自检
 *
 * 关于"ESP-IDF 的 DHCP Server 只能跑在 SoftAP 上"是个常见误解：
 * esp_netif 层的 DHCP Server 与 WiFi 无关，只取决于三件事：
 *   1) netif 创建时 flags 含 ESP_NETIF_DHCP_SERVER（否则 dhcps 句柄为 NULL）；
 *   2) CONFIG_LWIP_DHCPS=y；
 *   3) 启动时 lwIP netif 已 up。
 * 详见 PLAN.md §6.1。
 */
#include <inttypes.h>
#include <string.h>

#include "net_dhcps.h"
#include "app_config.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "sdkconfig.h"

#include "dhcpserver/dhcpserver.h"

static const char *TAG = "net_dhcps";

static volatile uint32_t s_lease_count = 0;
static bool s_handler_registered = false;

#ifndef CONFIG_LWIP_DHCPS_LEASE_UNIT
#define CONFIG_LWIP_DHCPS_LEASE_UNIT 60
#endif

const char *net_dhcps_status_str(esp_netif_dhcp_status_t st)
{
    switch (st) {
    case ESP_NETIF_DHCP_INIT:    return "INIT";
    case ESP_NETIF_DHCP_STARTED: return "STARTED";
    case ESP_NETIF_DHCP_STOPPED: return "STOPPED";
    default:                     return "UNKNOWN";
    }
}

uint32_t net_dhcps_lease_count(void)
{
    return s_lease_count;
}

/* 有新客户端拿到地址时触发，这是服务端最硬的证据 */
static void on_client_lease(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ip_event_assigned_ip_to_client_t *e = (ip_event_assigned_ip_to_client_t *)event_data;

    s_lease_count++;
    ESP_LOGI(TAG, "新租约 #%" PRIu32 " -> %d.%d.%d.%d  client %02x:%02x:%02x:%02x:%02x:%02x  host='%s'",
             s_lease_count,
             esp_ip4_addr1(&e->ip), esp_ip4_addr2(&e->ip),
             esp_ip4_addr3(&e->ip), esp_ip4_addr4(&e->ip),
             e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
             e->hostname);
}

esp_err_t net_dhcps_configure(esp_netif_t *netif)
{
    ESP_RETURN_ON_FALSE(netif, ESP_ERR_INVALID_ARG, TAG, "netif 为空");

    esp_netif_dhcp_status_t st = ESP_NETIF_DHCP_INIT;
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_get_status(netif, &st), TAG, "读状态失败");
    ESP_RETURN_ON_FALSE(st != ESP_NETIF_DHCP_STARTED, ESP_ERR_INVALID_STATE, TAG,
                        "DHCP Server 已启动，地址池必须在启动前配置");

    /* ---- 地址池 ---- */
    dhcps_lease_t lease = {
        .enable   = true,
        .start_ip = { .addr = ESP_IP4TOADDR(APP_NET_A, APP_NET_B, APP_NET_C, APP_POOL_START) },
        .end_ip   = { .addr = ESP_IP4TOADDR(APP_NET_A, APP_NET_B, APP_NET_C, APP_POOL_END) },
    };
    esp_err_t err = esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET,
                                           ESP_NETIF_REQUESTED_IP_ADDRESS,
                                           &lease, sizeof(lease));
    if (err != ESP_OK) {
        /* 池配置非法时 lwIP 会自动退回"由服务器 IP 推导"的默认池，不致命 */
        ESP_LOGW(TAG, "地址池配置被拒(%s)，将使用自动推导池（服务器 IP/24 -> .2~.101）",
                 esp_err_to_name(err));
    }

    /* ---- 租约时长 ---- */
    uint32_t lease_ticks = APP_LEASE_SECONDS / CONFIG_LWIP_DHCPS_LEASE_UNIT;
    if (lease_ticks == 0) {
        lease_ticks = 1;
    }
    err = esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_IP_ADDRESS_LEASE_TIME,
                                 &lease_ticks, sizeof(lease_ticks));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "租约时长设置失败: %s", esp_err_to_name(err));
    }

    /* ---- 新租约事件 ---- */
    if (!s_handler_registered) {
        ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT,
                                                       IP_EVENT_ASSIGNED_IP_TO_CLIENT,
                                                       on_client_lease, NULL),
                            TAG, "注册租约事件失败");
        s_handler_registered = true;
    }

    ESP_LOGI(TAG, "地址池 %d.%d.%d.%d ~ %d.%d.%d.%d，租约 %u s（单位 %d s × %" PRIu32 "）",
             APP_NET_A, APP_NET_B, APP_NET_C, APP_POOL_START,
             APP_NET_A, APP_NET_B, APP_NET_C, APP_POOL_END,
             (unsigned)APP_LEASE_SECONDS, CONFIG_LWIP_DHCPS_LEASE_UNIT,
             lease_ticks);
    return ESP_OK;
}

esp_err_t net_dhcps_ensure_started(esp_netif_t *netif)
{
    ESP_RETURN_ON_FALSE(netif, ESP_ERR_INVALID_ARG, TAG, "netif 为空");

    esp_netif_dhcp_status_t st = ESP_NETIF_DHCP_INIT;
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_get_status(netif, &st), TAG, "读状态失败");
    if (st == ESP_NETIF_DHCP_STARTED) {
        return ESP_OK;
    }

    esp_err_t err = esp_netif_dhcps_start(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGE(TAG, "dhcps_start 失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 回读状态：start 返回 OK 不代表真的起来了 */
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_get_status(netif, &st), TAG, "读状态失败");
    if (st != ESP_NETIF_DHCP_STARTED) {
        ESP_LOGW(TAG, "已调用 start，但状态为 %s —— netif 还没 up，稍后重试",
                 net_dhcps_status_str(st));
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "DHCP Server 已启动（状态 = STARTED）");
    return ESP_OK;
}

esp_netif_dhcp_status_t net_dhcps_status(esp_netif_t *netif)
{
    esp_netif_dhcp_status_t st = ESP_NETIF_DHCP_INIT;
    if (netif == NULL || esp_netif_dhcps_get_status(netif, &st) != ESP_OK) {
        return ESP_NETIF_DHCP_INIT;
    }
    return st;
}
