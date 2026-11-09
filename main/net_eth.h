/**
 * @file  net_eth.h
 * @brief 以太网底层：EMAC + IP101 PHY + esp_netif（DHCP Server 模式）
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "esp_eth.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建 PHY/MAC 驱动、netif（带 ESP_NETIF_DHCP_SERVER 标志）并挂载 glue。
 *        本函数不启动 EMAC，需要在 DHCP 池配置完成后再调 net_eth_start()。
 */
esp_err_t net_eth_init(void);

/** @brief 启动以太网驱动（会触发 ETHERNET_EVENT_START，netif 随之 up） */
esp_err_t net_eth_start(void);

/** @brief 取 netif 句柄 */
esp_netif_t *net_eth_netif(void);

/** @brief 取驱动句柄（读 PHY 寄存器、取链路状态时用） */
esp_eth_handle_t net_eth_handle(void);

/** @brief 读 PHY ID（寄存器 0x02/0x03），用于确认贴片型号是 IP101 */
esp_err_t net_eth_read_phy_id(uint32_t *phy_id);

/** @brief 当前链路是否 up（由 ETH 事件维护） */
bool net_eth_link_up(void);

/** @brief 读当前速率/双工，仅链路 up 时有效 */
esp_err_t net_eth_get_link(eth_speed_t *speed, eth_duplex_t *duplex);

/**
 * @brief 链路状态变化回调：插上 = true，拔掉 / EMAC 停止 = false。
 * @note  在 ETH 事件任务上下文被调用，回调里不要做阻塞操作（清表、置标志可以）。
 *        本模块刻意不直接依赖 dev_table：谁关心链路谁自己注册。
 */
typedef void (*net_eth_link_cb_t)(bool up);

/** @brief 注册链路变化回调（只保留一个，传 NULL 取消） */
void net_eth_set_link_cb(net_eth_link_cb_t cb);

#ifdef __cplusplus
}
#endif
