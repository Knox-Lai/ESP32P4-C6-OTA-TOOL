/**
 * @file  net_eth.c
 * @brief 以太网底层：EMAC(RMII) + IP101 PHY + esp_netif 装配
 *
 * 关键设计（对应 PLAN.md §6.1）：
 *   1. netif 必须用自定义 inherent config，flags 里带 ESP_NETIF_DHCP_SERVER，
 *      否则 esp_netif_new() 不会创建 dhcps 句柄，后续所有 dhcps API 都失败。
 *      默认的 ESP_NETIF_INHERENT_DEFAULT_ETH() 是 DHCP_CLIENT，不能直接用。
 *   2. 静态 IP 从 base.ip_info 注入。不要事后再调 esp_netif_set_ip_info()，
 *      因为该函数在 DHCP_SERVER 模式下要求 dhcps 处于 STOPPED，否则返回
 *      ESP_ERR_ESP_NETIF_DHCP_NOT_STOPPED。
 *   3. 带 AUTOUP 标志后，EMAC 启动（ETHERNET_EVENT_START）时 netif 立即 up，
 *      esp_netif_action_start() 会自动把 DHCP Server 拉起来 —— 不用等网线插上。
 */
#include <string.h>

#include "net_eth.h"
#include "net_dhcps.h"
#include "app_config.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_phy_ip101.h"

static const char *TAG = "net_eth";

static esp_netif_t *s_netif = NULL;
static esp_eth_handle_t s_eth = NULL;
static esp_eth_netif_glue_handle_t s_glue = NULL;
static volatile bool s_link_up = false;
static net_eth_link_cb_t s_link_cb = NULL;   /* 指针宽度读写，单次赋值天然原子 */

/* 工具自身静态地址：PC 侧 ipconfig 应显示"DHCP 服务器 = 10.77.0.1"
 * 地址全部由 app_config.h 派生，换网段不用改这个文件 */
static const esp_netif_ip_info_t s_static_ip = {
    .ip      = { .addr = ESP_IP4TOADDR(APP_NET_A, APP_NET_B, APP_NET_C, APP_TOOL_HOST) },
    .gw      = { .addr = ESP_IP4TOADDR(APP_NET_A, APP_NET_B, APP_NET_C, APP_TOOL_HOST) },
    .netmask = { .addr = ESP_IP4TOADDR(255, 255, 255, 0) },
};

static const char *speed_str(eth_speed_t s)
{
    switch (s) {
    case ETH_SPEED_10M:   return "10M";
    case ETH_SPEED_100M:  return "100M";
    default:              return "?";
    }
}

static const char *duplex_str(eth_duplex_t d)
{
    return (d == ETH_DUPLEX_HALF) ? "half" : "full";
}

/* ---------------- 事件 ---------------- */

/* 只在状态"真的变了"时通知上层：拔网线往往先 DISCONNECTED 再 STOP，
 * 不判重复会把同一个断链事件上报两次。 */
static void notify_link(bool up)
{
    bool was = s_link_up;
    s_link_up = up;
    if (was == up) {
        return;
    }

    net_eth_link_cb_t cb = s_link_cb;
    if (cb != NULL) {
        cb(up);
    }
}

static void eth_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    switch (event_id) {
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "EMAC 已启动");
        break;

    case ETHERNET_EVENT_STOP:
        notify_link(false);
        ESP_LOGW(TAG, "EMAC 已停止");
        break;

    case ETHERNET_EVENT_CONNECTED: {
        notify_link(true);
        eth_speed_t speed = ETH_SPEED_10M;
        eth_duplex_t duplex = ETH_DUPLEX_HALF;
        net_eth_get_link(&speed, &duplex);
        ESP_LOGI(TAG, "链路 UP：%s %s", speed_str(speed), duplex_str(duplex));
        /* 链路到位后确认 DHCP Server 是否真的在跑（防止 start 被静默吞掉） */
        net_dhcps_ensure_started(s_netif);
        break;
    }

    case ETHERNET_EVENT_DISCONNECTED:
        notify_link(false);
        ESP_LOGW(TAG, "链路 DOWN（网线被拔或对端掉电）");
        break;

    default:
        break;
    }
}

/* ---------------- 对外接口 ---------------- */

esp_netif_t *net_eth_netif(void)
{
    return s_netif;
}

esp_eth_handle_t net_eth_handle(void)
{
    return s_eth;
}

bool net_eth_link_up(void)
{
    return s_link_up;
}

void net_eth_set_link_cb(net_eth_link_cb_t cb)
{
    s_link_cb = cb;
}

esp_err_t net_eth_get_link(eth_speed_t *speed, eth_duplex_t *duplex)
{
    ESP_RETURN_ON_FALSE(s_eth, ESP_ERR_INVALID_STATE, TAG, "驱动未初始化");

    esp_err_t err = esp_eth_ioctl(s_eth, ETH_CMD_G_SPEED, speed);
    if (err != ESP_OK) {
        return err;
    }
    return esp_eth_ioctl(s_eth, ETH_CMD_G_DUPLEX_MODE, duplex);
}

esp_err_t net_eth_read_phy_id(uint32_t *phy_id)
{
    ESP_RETURN_ON_FALSE(s_eth && phy_id, ESP_ERR_INVALID_STATE, TAG, "驱动未初始化");

    uint32_t id1 = 0, id2 = 0;
    esp_eth_phy_reg_rw_data_t rw = { .reg_addr = 0x02, .reg_value_p = &id1 };
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(s_eth, ETH_CMD_READ_PHY_REG, &rw), TAG,
                        "读 PHY 寄存器 0x02 失败");

    rw.reg_addr = 0x03;
    rw.reg_value_p = &id2;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(s_eth, ETH_CMD_READ_PHY_REG, &rw), TAG,
                        "读 PHY 寄存器 0x03 失败");

    *phy_id = ((id1 & 0xFFFF) << 16) | (id2 & 0xFFFF);

    /* IP101 的 PHYID1（寄存器 0x02）= 0x0243 */
    if ((id1 & 0xFFFF) == 0x0243) {
        ESP_LOGI(TAG, "PHY ID = 0x%08X（IP101，PHYID1=0x%04X）", (unsigned)*phy_id,
                 (unsigned)(id1 & 0xFFFF));
    } else {
        ESP_LOGW(TAG, "PHY ID = 0x%08X，PHYID1=0x%04X 与 IP101(0x0243) 不符，检查 PHY addr/引脚",
                 (unsigned)*phy_id, (unsigned)(id1 & 0xFFFF));
    }
    return ESP_OK;
}

esp_err_t net_eth_init(void)
{
    ESP_RETURN_ON_FALSE(s_netif == NULL, ESP_ERR_INVALID_STATE, TAG, "已初始化");

    /* ---- 1. MAC + PHY ---- */
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = APP_PHY_ADDR;
    phy_cfg.reset_gpio_num = APP_PHY_RST_GPIO;

    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.smi_gpio.mdc_num = APP_ETH_MDC_GPIO;
    emac_cfg.smi_gpio.mdio_num = APP_ETH_MDIO_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    ESP_RETURN_ON_FALSE(mac, ESP_FAIL, TAG, "MAC 创建失败");

    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_cfg);
    if (phy == NULL) {
        mac->del(mac);
        ESP_LOGE(TAG, "PHY 创建失败");
        return ESP_FAIL;
    }

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t err = esp_eth_driver_install(&eth_cfg, &s_eth);
    if (err != ESP_OK) {
        mac->del(mac);
        phy->del(phy);
        ESP_LOGE(TAG, "Ethernet 驱动安装失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ---- 2. netif：重点在 flags ---- */
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base.flags = (esp_netif_flags_t)(ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP);
    base.ip_info = &s_static_ip;   /* 静态地址由此注入，勿再调 set_ip_info */
    base.get_ip_event = 0;         /* 自己是服务器，不关心 GOT_IP */
    base.lost_ip_event = 0;
    base.if_key = APP_NETIF_KEY;
    base.if_desc = "ota_eth";

    esp_netif_config_t netif_cfg = {
        .base = &base,
        .driver = NULL,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };

    s_netif = esp_netif_new(&netif_cfg);
    ESP_RETURN_ON_FALSE(s_netif, ESP_FAIL, TAG, "netif 创建失败");

    s_glue = esp_eth_new_netif_glue(s_eth);
    if (s_glue == NULL) {
        ESP_LOGE(TAG, "netif glue 创建失败");
        esp_netif_destroy(s_netif);
        s_netif = NULL;
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(esp_netif_attach(s_netif, s_glue), TAG, "netif attach 失败");

    /* ---- 3. 事件：必须在 esp_eth_start() 之前注册 ---- */
    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                                   eth_event_handler, NULL),
                        TAG, "注册 ETH 事件失败");

    /* ---- 4. 打印 MAC，便于和 PC 的 arp -a 对照 ---- */
    uint8_t mac_addr[6] = {0};
    if (esp_eth_ioctl(s_eth, ETH_CMD_G_MAC_ADDR, mac_addr) == ESP_OK) {
        ESP_LOGI(TAG, "P4 ETH MAC = %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2],
                 mac_addr[3], mac_addr[4], mac_addr[5]);
    }

    ESP_LOGI(TAG, "netif 就绪：%s，静态地址 " IPSTR "/" IPSTR, APP_NETIF_KEY,
             IP2STR(&s_static_ip.ip), IP2STR(&s_static_ip.netmask));
    return ESP_OK;
}

esp_err_t net_eth_start(void)
{
    ESP_RETURN_ON_FALSE(s_eth, ESP_ERR_INVALID_STATE, TAG, "驱动未初始化");
    return esp_eth_start(s_eth);
}
