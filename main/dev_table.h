/**
 * @file  dev_table.h
 * @brief 设备表：MAC / IP / 身份（UID、型号、硬件版本、固件版本、Flash、能力）
 *
 * 设计要点：
 * - 固定槽位数组（APP_DEV_MAX），不动态分配，杜绝长时间运行后的碎片问题；
 * - 三种来源都会落到同一张表里：
 *     ① DHCP 租约事件（已知 MAC + IP，身份未知）—— 被动发现
 *     ② QUERY_INFO / DISCOVER 的应答（补全身份）
 *     ③ ANNOUNCE（设备主动上报，静态 IP 设备也能进表）
 * - 内建互斥：DHCP 事件回调、UDP 收包任务、轮询任务、日志任务分属四个上下文。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEV_UID_LEN            16
#define DEV_MODEL_LEN          16
#define DEV_IDENTITY_WIRE_LEN  44   /* 线上固定 44 字节，见 dev_identity_serialize */

typedef struct {
    uint8_t  uid[DEV_UID_LEN];
    char     model[DEV_MODEL_LEN + 1];  /* 补 0 结尾，方便直接打印 */
    uint16_t hw_ver;                    /* 高字节主版本，低字节次版本：0x0102 = 1.2 */
    uint8_t  fw_ver[4];                 /* major.minor.patch.build */
    uint32_t flash_size;                /* 字节 */
    uint16_t caps;                      /* 能力位域，见 DEV_CAP_* */
} dev_identity_t;

/* caps 位域（设备端自报，工具侧按位判断，未知位一律忽略） */
#define DEV_CAP_RESUME          (1u << 0)  /* 支持断点续传 */
#define DEV_CAP_SHA256          (1u << 1)  /* 支持整包 SHA-256 校验 */
#define DEV_CAP_DUAL_BANK       (1u << 2)  /* 支持 A/B 双分区回滚 */

typedef struct {
    bool     used;
    bool     online;                    /* miss 计数派生，连续 2 次无应答判离线 */
    bool     identified;                /* id 字段是否有效 */
    bool     probe_notified;            /* 已提示过“有 DHCP 但不应答发现协议”，只提示一次 */
    uint8_t  mac[6];                    /* 全 0 表示未知（静态 IP 设备） */
    uint32_t ip;                        /* 网络字节序，可直接用于 sockaddr_in */
    char     hostname[33];              /* DHCP 上报的主机名，可能为空 */
    dev_identity_t id;
    uint32_t first_seen_ms;
    uint32_t last_seen_ms;              /* 最后一次收到应答的时刻 */
    uint32_t next_poll_ms;
    uint8_t  miss;                      /* 连续未应答次数，收到应答清零 */
} dev_entry_t;

/** @brief 建表并初始化互斥量（在 app_main 早期调用一次） */
esp_err_t dev_table_init(void);

/** @brief 清空整表 */
void dev_table_clear(void);

/** @brief 已用槽位数 */
int dev_table_count(void);

/* ---------- 查找（返回静态数组内指针，生命周期 = 程序运行期；字段值可能被其它任务改） ---------- */

dev_entry_t *dev_table_find_mac(const uint8_t mac[6]);
dev_entry_t *dev_table_find_ip(uint32_t ip_net);
dev_entry_t *dev_table_find_uid(const uint8_t uid[DEV_UID_LEN]);

/**
 * @brief DHCP 租约事件入口：按 MAC 匹配，没有则按 IP 匹配，再没有则占新槽。
 * @param hostname  可为 NULL
 * @note  命中已有槽时会更新 IP（设备换地址）与 hostname，不清空已识别的身份。
 */
dev_entry_t *dev_table_upsert_lease(const uint8_t mac[6], uint32_t ip_net, const char *hostname);

/** @brief 为「不在 DHCP 租约表里的设备」占一个槽（来源 IP，MAC 未知） */
dev_entry_t *dev_table_claim_ip(uint32_t ip_net, const char *why);

/** @brief 收到身份应答：写入 id、清 miss、置 online/identified，并刷新 last_seen */
void dev_table_apply_identity(dev_entry_t *e, const dev_identity_t *id, uint32_t now_ms);

/**
 * @brief 轮询一轮：对到期的槽调用 cb。
 * @param cb  返回下次轮询间隔（ms）；返回 0 表示本次不探测
 * @note  内部持锁，cb 里不要做阻塞操作（只做一次 sendto 是安全的）。
 */
typedef uint32_t (*dev_poll_cb_t)(const dev_entry_t *e, uint32_t now_ms);
void dev_table_poll_pass(uint32_t now_ms, dev_poll_cb_t cb);

/** @brief 按 miss 计数刷新 online 标志（poll_pass 结尾会自动调用一次，一般不必手动调） */
void dev_table_refresh_online(uint32_t now_ms);

/** @brief 拷贝一份快照，供打印/UI 使用（避免持锁渲染） */
int dev_table_snapshot(dev_entry_t *out, int max);

/* ---------- 身份字段的线上序列化（固定 44 字节，大端） ---------- */

size_t dev_identity_serialize(const dev_identity_t *id, uint8_t *out, size_t cap);
bool   dev_identity_parse(const uint8_t *buf, size_t len, dev_identity_t *id);

/** @brief 把 MAC 格式化成 "aa:bb:cc:dd:ee:ff"（4 个静态缓冲轮转，可连续用 4 次） */
const char *dev_mac_str(const uint8_t mac[6]);

#ifdef __cplusplus
}
#endif
