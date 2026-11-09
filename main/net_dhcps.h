/**
 * @file  net_dhcps.h
 * @brief DHCP Server 配置与启动自检
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 状态枚举转字符串，便于日志阅读 */
const char *net_dhcps_status_str(esp_netif_dhcp_status_t st);

/**
 * @brief 配置地址池与租约时长，并注册"新租约"事件。
 *        必须在 DHCP Server 启动之前调用（启动后 option SET 会被拒绝）。
 */
esp_err_t net_dhcps_configure(esp_netif_t *netif);

/**
 * @brief 幂等启动：已启动直接返回成功；未启动则启动并**回读状态确认**。
 *        注意 esp_netif_dhcps_start() 在 netif 未 up 时会返回 ESP_OK 但没启动。
 */
esp_err_t net_dhcps_ensure_started(esp_netif_t *netif);

/** @brief 读当前 DHCP Server 状态 */
esp_netif_dhcp_status_t net_dhcps_status(esp_netif_t *netif);

/** @brief 累计分配过的租约数（用于状态栏显示） */
uint32_t net_dhcps_lease_count(void);

#ifdef __cplusplus
}
#endif
