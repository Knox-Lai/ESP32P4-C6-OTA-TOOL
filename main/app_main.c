/**
 * @file  app_main.c
 * @brief ESP32-P4 Ethernet OTA 工具 —— 第一阶段：DHCP Server 跑通
 *
 * 启动顺序（顺序错了 DHCP Server 起不来）：
 *   esp_netif_init → 事件循环 → net_eth_init（建 netif/driver/glue）
 *   → net_dhcps_configure（地址池必须在 start 之前配）
 *   → dev_discovery_init（租约事件必须在 dhcps 启动前注册，否则漏掉第一批租约）
 *   → net_eth_start（触发 netif up，自动拉起 DHCP Server）
 *   → 回读状态确认 STARTED → dev_discovery_start
 *   → ota_client_init / ota_shell_init（阶段 5）
 *   → bsp_display_init / ui_app_start（阶段 8，失败不致命）
 */
#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_system.h"
#include "esp_timer.h"      /* esp_timer_get_time：DHCP 自愈重试的限频时间戳 */
#include "nvs_flash.h"      /* 界面语言偏好持久化 */

#include "app_config.h"
#include "net_eth.h"
#include "net_dhcps.h"
#include "dev_discovery.h"
#include "dev_table.h"
#include "ota_client.h"
#include "ota_shell.h"
#include "bsp_display.h"
#include "sys_clock.h"
#include "ui_app.h"

static const char *TAG = "app";

/**
 * @brief 链路变化：拔网线就把设备表清空，插回来重播一次 DISCOVER。
 *
 * 为什么拔线要清表：链路一断，整条网段都不存在了，DHCP 租约也不会再续，
 * 表里的条目只剩"上次见过"的意义。UI 上继续挂着这些设备会让用户以为
 * 还能点升级。清空后 dev_table_count() 归零，列表、状态栏、详情页
 * 都会在下一拍 400 ms 刷新里自动变成空状态（detail_fill 已有 gone 分支）。
 *
 * 本函数在 ETH 事件任务上下文执行，所以：
 *   - 清表：内部只持一把互斥，够快，直接做；
 *   - 广播：sendto 有可能阻塞，绝不能在事件任务里发，只置标志，
 *     由 monitor_task 下一拍代发。
 */
static volatile bool s_need_broadcast = false;

static void on_link_change(bool up)
{
    if (up) {
        s_need_broadcast = true;
    } else {
        int n = dev_table_count();
        if (n > 0) {
            ESP_LOGW(TAG, "链路断开，清空设备表（%d 台）", n);
        }
        dev_table_clear();
    }
}

/* 周期自检：只在"状态变了"或"出异常"时打印。
 * 稳态（链路一直 UP、租约数不变）完全不输出 —— 5 s 一条会把串口命令行冲掉。
 * 想恢复逐拍观察时，把日志级别开到 DEBUG，未变化的那条走 ESP_LOGD。 */
static void monitor_task(void *arg)
{
    esp_netif_t *netif = (esp_netif_t *)arg;
    esp_netif_ip_info_t ip = {0};

    bool     have_last   = false;
    bool     last_up     = false;
    uint8_t  last_speed  = 0xFF;
    uint8_t  last_duplex = 0xFF;
    uint32_t last_ip     = 0;
    int      last_dhcps  = -1;
    uint32_t last_lease  = UINT32_MAX;
    int      last_dev    = -1;
    bool     heap_warned = false;
    uint32_t last_retry  = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(APP_MONITOR_PERIOD_MS));

        /* 链路刚恢复：补一次 DISCOVER，免得空等下一轮周期广播。
         * 放在这里而不是 ETH 事件回调里，是因为 sendto 可能阻塞。 */
        if (s_need_broadcast) {
            s_need_broadcast = false;
            dev_discovery_broadcast_now();
        }

        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

        bool up = net_eth_link_up();
        esp_netif_dhcp_status_t st = net_dhcps_status(netif);

        eth_speed_t  sp = ETH_SPEED_10M;
        eth_duplex_t dp = ETH_DUPLEX_HALF;
        bool detail = up && (net_eth_get_link(&sp, &dp) == ESP_OK);

        if (esp_netif_get_ip_info(netif, &ip) != ESP_OK) {
            memset(&ip, 0, sizeof(ip));
        }

        uint32_t lease = net_dhcps_lease_count();
        int      dev   = dev_table_count();
        uint32_t heap  = (uint32_t)esp_get_free_heap_size();

        /* 链路 down 时 sp/dp 无意义，用 0xFF 占位，避免"上次残留值"造成假变化 */
        uint8_t cur_speed  = detail ? (uint8_t)sp : 0xFF;
        uint8_t cur_duplex = detail ? (uint8_t)dp : 0xFF;

        char speed[16] = "--";
        char duplex[8] = "--";
        if (detail) {
            strcpy(speed, (sp == ETH_SPEED_10M) ? "10M" : "100M");
            strcpy(duplex, (dp == ETH_DUPLEX_HALF) ? "half" : "full");
        }

        bool changed = !have_last
                    || up != last_up
                    || cur_speed != last_speed
                    || cur_duplex != last_duplex
                    || ip.ip.addr != last_ip
                    || (int)st != last_dhcps
                    || lease != last_lease
                    || dev != last_dev;

        if (changed) {
            /* 链路断 / DHCP Server 掉了属于异常，用 WARN 让它在满屏里跳出来 */
            bool normal = up && (st == ESP_NETIF_DHCP_STARTED);
            ESP_LOG_LEVEL_LOCAL(normal ? ESP_LOG_INFO : ESP_LOG_WARN, TAG,
                          "link=%s %s/%s | ip=" IPSTR " | dhcps=%s | lease=%" PRIu32
                          " | dev=%d | heap=%" PRIu32,
                          up ? "UP" : "DOWN", speed, duplex,
                          IP2STR(&ip.ip),
                          net_dhcps_status_str(st),
                          lease, dev, heap);

            have_last   = true;
            last_up     = up;
            last_speed  = cur_speed;
            last_duplex = cur_duplex;
            last_ip     = ip.ip.addr;
            last_dhcps  = (int)st;
            last_lease  = lease;
            last_dev    = dev;
        } else {
            ESP_LOGD(TAG, "状态未变：link=%s dhcps=%s lease=%" PRIu32 " dev=%d heap=%" PRIu32,
                     up ? "UP" : "DOWN", net_dhcps_status_str(st), lease, dev, heap);
        }

        /* 堆只在"跌破阈值"和"恢复"各报一次，不参与上面的状态变化比较 */
        if (!heap_warned && heap < APP_HEAP_WARN_BYTES) {
            heap_warned = true;
            ESP_LOGW(TAG, "可用堆偏低：%" PRIu32 " B（阈值 %d）", heap, APP_HEAP_WARN_BYTES);
        } else if (heap_warned && heap > APP_HEAP_WARN_BYTES + 16384) {
            heap_warned = false;
            ESP_LOGI(TAG, "可用堆恢复：%" PRIu32 " B", heap);
        }

        /* 设备表：内部按签名比较，只有增删/上下线/版本变化才打明细 */
        dev_discovery_log_table();

        /* 自愈：链路已通但服务器没起来，最多 30 s 重试一次，避免每拍刷一条 WARN */
        if (up && st != ESP_NETIF_DHCP_STARTED && (now - last_retry) >= 30000u) {
            last_retry = now;
            ESP_LOGW(TAG, "DHCP Server 未运行，尝试重启");
            net_dhcps_ensure_started(netif);
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "===== ESP32-P4 Ethernet OTA 工具 / DHCP Server POC =====");
    ESP_LOGI(TAG, "PHY=IP101 addr=%d MDC=GPIO%d MDIO=GPIO%d nRST=GPIO%d",
             APP_PHY_ADDR, APP_ETH_MDC_GPIO, APP_ETH_MDIO_GPIO, APP_PHY_RST_GPIO);
    ESP_LOGI(TAG, "工具地址 %d.%d.%d.%d/24，地址池 %d.%d.%d.%d ~ %d.%d.%d.%d",
             APP_NET_A, APP_NET_B, APP_NET_C, APP_TOOL_HOST,
             APP_NET_A, APP_NET_B, APP_NET_C, APP_POOL_START,
             APP_NET_A, APP_NET_B, APP_NET_C, APP_POOL_END);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 0) NVS：界面语言偏好存在这里。
     *    首次烧录/换分区表后常见 NO_FREE_PAGES / NEW_VERSION_FOUND，擦一次重来。 */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重新格式化：%s", esp_err_to_name(nvs_err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    /* 0) 系统时钟：无 RTC / 无网络，用编译时间做初值，串口 time set 校正。
     *    必须在 UI 之前，状态栏一建出来就要有值。 */
    sys_clock_init();

    /* 1) 建 netif（带 ESP_NETIF_DHCP_SERVER 标志）与驱动 */
    ESP_ERROR_CHECK(net_eth_init());

    /* 2) 配地址池 —— 必须在启动之前 */
    ESP_ERROR_CHECK(net_dhcps_configure(net_eth_netif()));

    /* 3) 发现模块：建表 + 建 UDP 套接字 + 注册租约事件。
     *    必须在 net_eth_start 之前，否则第一批租约事件会漏掉。 */
    ESP_ERROR_CHECK(dev_discovery_init());

    /* 3.5) 链路变化 → 清空/重建设备表（拔网线后列表不再显示"幽灵设备"）。
     *      必须在 net_eth_start 之前注册，否则第一个 CONNECTED 事件收不到。 */
    net_eth_set_link_cb(on_link_change);

    /* 4) 启动 EMAC，netif 随之 up，DHCP Server 被自动拉起 */
    ESP_ERROR_CHECK(net_eth_start());

    /* 5) 自检：PHY 型号、DHCP Server 状态 */
    uint32_t phy_id = 0;
    net_eth_read_phy_id(&phy_id);
    net_dhcps_ensure_started(net_eth_netif());

    /* 6) 启动收包与轮询任务，并广播一次 DISCOVER */
    ESP_ERROR_CHECK(dev_discovery_start());

    /* 7) 升级会话引擎与串口命令行（阶段 5） */
    ESP_ERROR_CHECK(ota_client_init());
    ESP_ERROR_CHECK(ota_shell_init());

    /* 8) 显示与界面（阶段 8）。屏/触摸起不来不该拖死整个工具：
     *    串口命令仍然可用，只是没有界面。 */
    if (bsp_display_init() == ESP_OK) {
        if (ui_app_start() != ESP_OK) {
            ESP_LOGW(TAG, "界面启动失败，继续用串口命令");
        }
    } else {
        ESP_LOGW(TAG, "显示初始化失败（屏/触摸），继续用串口命令");
    }

    xTaskCreate(monitor_task, "eth_monitor", 4096, net_eth_netif(),
                tskIDLE_PRIORITY + 3, NULL);

    ESP_LOGI(TAG, "等待 PC 接入：插网线 → PC 网卡设为自动获取 IP");
    ESP_LOGI(TAG, "PC 侧验证：ipconfig /all 里 \"DHCP 服务器\" 应为 %d.%d.%d.%d",
             APP_NET_A, APP_NET_B, APP_NET_C, APP_TOOL_HOST);
    ESP_LOGI(TAG, "升级入口（串口输入）：dev 看设备 → ota 0 [KB] 发起升级 → status 看进度");
}
