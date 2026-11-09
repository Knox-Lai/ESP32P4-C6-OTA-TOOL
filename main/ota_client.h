/**
 * @file  ota_client.h
 * @brief 升级会话引擎（工具侧，TCP 客户端）—— 对应 ota-protocol.md §6 状态机
 *
 * 分工：设备 listen(4011)，工具 connect。工具因此要自己维护全部重试逻辑：
 *   connect → OTA_BEGIN → 等 BEGIN_ACK → 滑窗发 OTA_DATA → 等累计 ACK
 *   → OTA_END → 等 RESULT → REBOOT → 断连 → 等设备重新上线并核对版本
 *
 * 成功判据是"重启后设备自报版本 == 目标版本"，不是 RESULT(OK)：
 * RESULT 只说明写完了，跳转失败 / 看门狗复位 / 回滚都会让它变成假成功。
 *
 * 并发：v1 只允许一条会话（设备端 RAM 与 Flash 写带宽都是瓶颈，
 * 多设备并发放在 UI 层排队实现，不改本模块的接口）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "dev_table.h"
#include "ota_image.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_CONNECT,      /* 建 TCP 连接 */
    OTA_STATE_BEGIN,        /* 发 OTA_BEGIN 等 BEGIN_ACK */
    OTA_STATE_STREAM,       /* 滑窗发 OTA_DATA */
    OTA_STATE_END,          /* 发 OTA_END 等 OTA_RESULT */
    OTA_STATE_REBOOT,       /* 发 REBOOT 并断开 */
    OTA_STATE_VERIFY,       /* 等设备重新上线并核对版本 */
    OTA_STATE_DONE,
    OTA_STATE_FAILED,
} ota_state_t;

typedef struct {
    ota_state_t state;
    uint32_t    total;          /* 镜像总字节 */
    uint32_t    acked;          /* 设备已确认落盘的字节 */
    uint8_t     err_code;       /* OTA_ERR_*，0 = 无 */
    uint32_t    detail;         /* 失败上下文（偏移 / CRC 等） */
    uint32_t    elapsed_ms;
    char        dev[18];        /* MAC 字符串 */
    char        model[DEV_MODEL_LEN + 1];
    char        msg[80];        /* 一句话结论，UI 直接显示 */
} ota_status_t;

/** @brief 初始化（建互斥量）。app_main 早期调用一次。 */
esp_err_t ota_client_init(void);

/** @brief 当前是否有会话在跑 */
bool ota_client_busy(void);

/**
 * @brief 对一台设备发起升级（异步：内部起任务，立即返回）。
 * @param ip_net  设备 IP，网络字节序（来自设备表）
 * @param uid     设备 UID，用于重启后按 UID 重新认人（可为全 0，则按 IP 认）
 * @param mac     设备 MAC，仅用于日志
 * @param img     镜像（结构体会被整体拷贝，调用方不必保持有效）
 * @return ESP_ERR_INVALID_STATE = 已有会话在跑；ESP_ERR_NO_MEM = 起任务失败
 */
esp_err_t ota_client_start(uint32_t ip_net,
                           const uint8_t uid[DEV_UID_LEN],
                           const uint8_t mac[6],
                           const ota_image_t *img);

/** @brief 拷贝一份当前会话状态（无会话时 state = IDLE） */
void ota_client_get_status(ota_status_t *out);

/** @brief 状态枚举转字符串，日志与 UI 共用 */
const char *ota_state_str(ota_state_t s);

#ifdef __cplusplus
}
#endif
