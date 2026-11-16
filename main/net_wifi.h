/**
 * @file  net_wifi.h
 * @brief WiFi STA：扫描、连接、自动重连、凭据持久化
 *
 * ESP32-P4 自身没有射频，WiFi 由板载 ESP32-C6 通过 SDIO 提供
 * （esp_hosted + esp_wifi_remote，两个组件在 idf_component.yml 里按 target
 * 限定为 esp32p4）。上层看到的仍是标准 `esp_wifi_*` 接口 —— remote 组件
 * 把调用转成 RPC 发给 C6，本模块不需要为"远程"写任何特殊分支。
 *
 * 与以太网的分工：
 *   以太网  固定 10.77.0.1/24，跑 DHCP Server，是**升级通道**（设备在这条线上）
 *   WiFi    DHCP Client，连现场路由器，是**出网通道**（查服务器 / 拉固件 / 对时）
 * 两者互不替代：网线拔了照样能用 WiFi，WiFi 没配也不影响升级。
 *
 * 状态查询全部无锁只读（字段用 8 位原子写），可以每 400 ms 被 UI 轮询；
 * 需要感知"状态变了"就注册 net_wifi_set_cb()，回调里只置标志、不碰 LVGL。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SSID 上限 32 字节 + 结束符；密码上限 64 字节 + 结束符 */
#define NET_WIFI_SSID_MAX   33
#define NET_WIFI_PASS_MAX   65

/* 一次扫描最多保留多少个网络（去重后，按信号降序） */
#define NET_WIFI_SCAN_MAX   20

/* 最多记住多少个连过的网络（SSID + 密码）。满了淘汰最早的一条。
 * 6 组足够覆盖"家里 / 公司 / 手机热点 / 现场几个路由器"，
 * NVS 里每条是 (<33) + (<65) 字节，总量不到 600 B。 */
#define NET_WIFI_SAVED_MAX  6

typedef enum {
    NET_WIFI_OFF = 0,       /* 用户关掉了 WiFi 开关 */
    NET_WIFI_IDLE,          /* 已开启但没有连接（没配过，或刚断开） */
    NET_WIFI_CONNECTING,    /* 正在关联 / 等 IP */
    NET_WIFI_CONNECTED,     /* 拿到 IP */
    NET_WIFI_FAILED,        /* 上一次连接失败，正在退避重试 */
    NET_WIFI_UNAVAILABLE,   /* 起不来：C6 没响应、hosted 未启用、内存不足 */
    NET_WIFI_STATE_MAX,
} net_wifi_state_t;

/**
 * 失败原因（给界面用的人话分类，不是 reason 原值）。
 *
 * 为什么不在界面直接翻译 wifi_err_reason_t：那套码位有 60 多个，
 * 现场最常见的只有"密码错 / 找不到 AP / 被 AP 拒 / 超时"四类，
 * 把 60 个码位逐个写文案既没人看得懂，也白占字库。
 * 只在 net_wifi.c 里做一次归类，界面按枚举取文案。
 * 原值（含 disc_hint 的中文解释）仍然打在串口日志里，排障时看日志。
 */
typedef enum {
    NET_WIFI_ERR_NONE = 0,   /* 没有失败，或还没失败过 */
    NET_WIFI_ERR_PASSWORD,   /* 4/15/202/204：认证或四次握手失败，基本是密码错 */
    NET_WIFI_ERR_NO_AP,      /* 200/201：AP 消失或根本没扫到 */
    NET_WIFI_ERR_REJECTED,   /* 203/205：关联被 AP 拒绝（MAC 过滤、满员、band steering） */
    NET_WIFI_ERR_TIMEOUT,    /* 驱动一个事件都没回，本地超时判失败 */
    NET_WIFI_ERR_LINK_LOST,  /* 连上之后链路探活连续失败 */
    NET_WIFI_ERR_OTHER,      /* 其余 reason，界面退回通用"连接失败" */
} net_wifi_err_t;

typedef struct {
    char     ssid[NET_WIFI_SSID_MAX];
    int8_t   rssi;          /* dBm，负数，-127 表示无效 */
    uint8_t  authmode;      /* wifi_auth_mode_t，0 = 开放网络 */
    bool     saved;         /* 本机存过它的密码 */
} net_wifi_ap_t;

/** @brief 读 NVS 里的开关与凭据，需要时在后台起 WiFi 任务。
 *         不阻塞：esp_wifi_init 在 P4 上要走一次 SDIO 握手，最坏几秒，
 *         不能在 app_main 的主路径里等。 */
esp_err_t net_wifi_init(void);

/** @brief 开关 WiFi。关闭会断开连接并停掉射频；开启会尝试连上次的 SSID。 */
esp_err_t net_wifi_set_enabled(bool on);

bool net_wifi_enabled(void);

net_wifi_state_t net_wifi_state(void);

/** @brief 状态的中文短语（日志/串口用，不用于界面） */
const char *net_wifi_state_str(void);

/** @brief 当前连接（或正在连）的 SSID；没有返回空串 */
const char *net_wifi_ssid(void);

/** @brief 当前连接的 IP 字符串；没有返回 "0.0.0.0" */
const char *net_wifi_ip(void);

/**
 * @brief 当前（最后一次请求的）密码明文
 *
 * 只用于给"重新输入密码"的输入框做预填 —— 该输入框是掩码显示的，
 * 预填只是省掉用户重敲一遍。**不要打日志、不要传给别的模块。**
 * 没设过密码时返回空串。
 */
const char *net_wifi_pass(void);

/** @brief 最近一次测得的信号强度，-127 = 无效 */
int8_t net_wifi_rssi(void);

/** @brief 开始一次扫描（异步，约 2~4 s）。已连接时也能扫，不会断线。 */
esp_err_t net_wifi_scan_start(void);

bool net_wifi_scanning(void);

/** @brief 拷贝扫描结果，返回条数（最多 max 条）。
 *         排序规则：已保存的在前，同组内按信号从强到弱。 */
int net_wifi_scan_get(net_wifi_ap_t *out, int max);

/** @brief 上次扫描结果的条数（UI 判重用，不拷贝数据） */
int net_wifi_scan_count(void);

/**
 * @brief 用**已保存的密码**连接。这个 SSID 没存过时返回 ESP_ERR_NOT_FOUND，
 *        调用方应改用 net_wifi_connect() 让用户输密码。
 * @note  连过的网络都会记住密码（最多 NET_WIFI_SAVED_MAX 组），不只是最后一个。
 */
esp_err_t net_wifi_connect_saved(const char *ssid);

/** @brief 这个 SSID 有没有存过密码（列表标记 / 分组用） */
bool net_wifi_is_saved(const char *ssid);

/** @brief 已保存的网络条数 */
int net_wifi_saved_count(void);

/** @brief 第 idx 个已保存网络的 SSID；越界返回 NULL */
const char *net_wifi_saved_ssid(int idx);

/**
 * @brief 进入 FAILED 之后过了多少秒（用于"失败→正在重试"的界面收敛）。
 * @return 非 FAILED 状态返回 -1
 */
int32_t net_wifi_fail_age_s(void);

/**
 * @brief 最近一次失败的原因分类（界面文案用）。
 * @return 非 FAILED 状态返回 NET_WIFI_ERR_NONE
 * @note  和 net_wifi_fail_age_s() 一样是 8 位原子读，可以每 400 ms 直接问。
 */
net_wifi_err_t net_wifi_fail_reason(void);

/**
 * @brief 是否还会自动重连（本轮重连预算没用完）
 *
 * 一轮最多重连 RETRY_MAX_TIMES 次（见 net_wifi.c）。用完之后状态停在
 * FAILED 但不再自动重试，界面必须区分这两种情况 —— 否则会一直显示
 * "正在重试…"，那正是用户抱怨的那个永远转不完的状态。
 *
 * @return true  = 还会再试（显示"正在重试…"）
 *         false = 已停止重试（只留失败原因，等用户点选网络 / 拨开关）
 */
bool net_wifi_retry_pending(void);

/**
 * @brief 连接指定 SSID。
 * @param pass  密码，开放网络传 "" 或 NULL
 * @param save  true = **连上之后**写进"已知网络"表并作为下次上电自动重连的目标
 *              （连不上就不落盘：输错一次密码不该顶掉原来能用的网络）
 * @note  真正的关联是异步的，返回 ESP_OK 只代表"已下发"；结果看 net_wifi_state()。
 */
esp_err_t net_wifi_connect(const char *ssid, const char *pass, bool save);

/** @brief 主动断开（保留凭据）。之后不会自动重连，直到再次 connect 或重启。 */
esp_err_t net_wifi_disconnect(void);

/** @brief 清除保存的 SSID/密码并断开 */
esp_err_t net_wifi_forget(void);

/**
 * @brief 只忘记**某一个**网络（从已知网络里删掉，连同它的密码）。
 * @return ESP_ERR_NOT_FOUND = 这个 SSID 本来就没存过
 * @note  删的若是当前连接 / 开机自动连的目标，会一并断开并清掉配置，
 *        否则驱动侧还留着凭据，"忘记"完照样连得回去。
 *        内部要写 NVS，别在 LVGL 任务里直接调（用一次性任务包一层）。
 */
esp_err_t net_wifi_forget_one(const char *ssid);

/** @brief 状态变化回调。在 WiFi/IP 事件任务上下文被调用，只置标志，别碰 LVGL。 */
typedef void (*net_wifi_cb_t)(void);

void net_wifi_set_cb(net_wifi_cb_t cb);

#ifdef __cplusplus
}
#endif
