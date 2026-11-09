/**
 * @file  app_config.h
 * @brief 全局可调参数。改配置只改这里，其它 .c 不要出现魔数。
 *
 * 板级以太网参数来自开发板自带示例的实测配置
 * （ethernet / esp_brookesia_phone 两个工程一致）：
 *   PHY = IP101，addr = 1，MDC = GPIO31，MDIO = GPIO52，nRST = GPIO51
 */
#pragma once

/* ---------------- 板级以太网（IP101 / RMII） ---------------- */
#define APP_ETH_MDC_GPIO        31
#define APP_ETH_MDIO_GPIO       52
#define APP_PHY_RST_GPIO        51
#define APP_PHY_ADDR            1

/* ---------------- OTA 工具自身网络 ---------------- */
/* 网段：10.77.0.0/24
 *
 * 换网段只改这里的 A/B/C 和下面地址池的起止主机号 —— netif 静态 IP、
 * DHCP 地址池、DISCOVER 广播地址（.255）全部由这几个宏派生，别处没有硬编码。
 * 旧网段留档备查：A=192 B=168 C=4，池 100~150。
 */
#define APP_NET_A               10
#define APP_NET_B               77
#define APP_NET_C               0

/* 工具自身地址（PC 的"默认网关"与"DHCP 服务器"都应显示这个） */
#define APP_TOOL_HOST           1

/* DHCP 地址池：PC/设备从这里分配。数量上限受 DHCPS_MAX_LEASE(100) 限制
 * 当前 200~230 共 31 个地址，与工具地址 .1 隔开，便于一眼看出"这是分配来的" */
#define APP_POOL_START          200
#define APP_POOL_END            230

/* 租约时长（秒）。实际写入的是 秒 / CONFIG_LWIP_DHCPS_LEASE_UNIT */
#define APP_LEASE_SECONDS       3600

/* netif 接口名，避免与默认 ETH_DEF 冲突 */
#define APP_NETIF_KEY           "ETH_OTA"

/* 状态自检周期（毫秒）。只在状态变化时才打印一行，稳态完全静默，
 * 否则 5 s 一条会把串口命令行输入冲得没法看。 */
#define APP_MONITOR_PERIOD_MS   5000

/* 可用堆告警阈值：低于它在状态行里升级成 WARN，恢复后再报一次 */
#define APP_HEAP_WARN_BYTES     (100 * 1024)

/* ---------------- 设备发现（阶段 4） ---------------- */
/* 工具与设备共用的 UDP 端口（PC 侧模拟器 tools/ota_sim.py 也监听这个端口） */
#define APP_DISCOVERY_PORT          4010

/* 设备表容量。放在 RAM 里，单槽约 120 B，16 槽不到 2 KB */
#define APP_DEV_MAX                 16

/* 广播 DISCOVER 的兜底周期（只在静态 IP 设备 / 租约表丢失时才靠它） */
#define APP_DISCOVER_BROADCAST_MS   5000

/* 未识别设备的探测节奏：先快后慢。
 * 快档 1 s × 3 次（约 3 s）—— 覆盖“设备刚拿到租约、UDP 服务已就绪”这一最常见情况；
 * 慢档之后 10 s 一次 —— 真实设备可能 DHCP 之后还要几秒才起应用层，
 * 而普通 PC 永远不会应答，绝不能因为 3 s 没回音就认定它掉线。 */
#define APP_IDENTIFY_RETRY_MS       1000
#define APP_IDENTIFY_FAST_TRIES     3
#define APP_IDENTIFY_SLOW_MS        10000

/* 已识别设备的心跳间隔（连续 3 次无应答判离线，约 15 s） */
#define APP_POLL_INTERVAL_MS        5000

/* 轮询任务的唤醒粒度：决定 DHCP 事件到首次 QUERY_INFO 的延迟上限 */
#define APP_POLL_TASK_TICK_MS       500

/* 连续多少次无应答判离线 */
#define APP_DEV_OFFLINE_MISS        3

/* 判离线后的降级探测间隔：留一条"复活"通道，设备重新上电/恢复后能自己回到 ONLINE */
#define APP_OFFLINE_PROBE_MS        30000

/* ---------------- OTA 升级会话（阶段 5） ---------------- */
/* 升级走 TCP，发现走 UDP，两者独立：升级期间心跳照跑，
 * 所以能用"心跳还在不在"区分"设备在擦 Flash"和"设备掉线"。 */
#define APP_OTA_PORT                4011

/* 建连超时。设备就在直连网段上，正常 <10 ms；这里给 3 s 只为兜住"端口没开"。 */
#define APP_OTA_CONNECT_TIMEOUT_MS  3000

/* 等 BEGIN_ACK / DATA_ACK 的超时（对应协议 T_ACK） */
#define APP_OTA_T_ACK_MS            2000

/* 等 OTA_RESULT 的超时。设备要读完整个分区算 CRC32 + SHA-256，
 * 1 MB 在 4 MB Flash 上大约 1~3 s，留到 30 s 是给慢速 SPI 与校验算法兜底。 */
#define APP_OTA_T_RESULT_MS         30000

/* 单帧重传次数（协议 N_RETRY） */
#define APP_OTA_N_RETRY             3

/* 默认分片与窗口。chunk 以设备 BEGIN_ACK 报的为准，这里只是发 BEGIN 之前的值 */
#define APP_OTA_DEFAULT_CHUNK       1024
#define APP_OTA_DEFAULT_WINDOW      4

/* 工具侧允许的最大窗口。设备报得更大也按这个截断，避免一次塞爆 lwIP 发送缓冲。 */
#define APP_OTA_WINDOW              4

/* REBOOT 后等设备以新版本重新出现在设备表的上限，以及轮询粒度 */
#define APP_OTA_T_REBOOT_MS         60000
#define APP_OTA_VERIFY_PERIOD_MS    500

/* 会话任务栈：两帧缓冲（各约 1 KB）在栈上，必须给足 */
#define APP_OTA_TASK_STACK          8192

/* 合成测试镜像（阶段 5 用；阶段 7 会换成从 storage 分区读 .fwp） */
#define APP_OTA_SYNTH_SIZE_KB       256
#define APP_OTA_SYNTH_SEED          20261105u

/* 命令行任务栈（生成合成镜像时要 1 KB 缓冲 + 计算摘要） */
#define APP_SHELL_TASK_STACK        6144
