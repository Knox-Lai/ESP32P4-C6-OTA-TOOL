/**
 * @file  dev_discovery.h
 * @brief 设备发现（阶段 4）：DHCP 被动发现 + UDP 主动识别 + 心跳保活
 *
 * 为什么不是"纯广播发现"：
 *   本工具自己就是 DHCP Server，`IP_EVENT_ASSIGNED_IP_TO_CLIENT` 已经把
 *   「谁接入了、MAC 是多少、分配了什么 IP、主机名叫什么」直接送到手上。
 *   所以主路径是「租约事件 → 立刻单播 QUERY_INFO 补全身份」，广播只做兜底，
 *   覆盖两类情况：设备用静态 IP（不在租约表里）、工具重启后租约表丢失。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 建表、注册 DHCP 租约事件、创建并绑定 UDP 套接字（不启动任务） */
esp_err_t dev_discovery_init(void);

/** @brief 启动收包任务与轮询任务，并立即广播一次 DISCOVER */
esp_err_t dev_discovery_start(void);

/** @brief 手动触发一次广播（UI 上"刷新"按钮用；可中途插入） */
void dev_discovery_broadcast_now(void);

/**
 * @brief 打印设备表。内部做节流：有变化时立刻打，否则最多 30 s 一次。
 */
void dev_discovery_log_table(void);

#ifdef __cplusplus
}
#endif
