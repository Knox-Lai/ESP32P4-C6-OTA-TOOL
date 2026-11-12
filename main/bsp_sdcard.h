/**
 * @file  bsp_sdcard.h
 * @brief TF(microSD) 卡：后台挂载 + 容量查询
 *
 * 板级事实：SD 走 SDMMC 4 线，Slot 0 的 IO MUX 默认引脚
 * （CLK=43 / CMD=44 / D0..D3=39~42），IO 电源由片内 LDO ch4 供电。
 * 没有卡检测（CD）脚，所以"有没有卡"只能靠挂载成功与否判断。
 *
 * 设计要点：
 *   - 挂载放在独立低优先级任务里做，**不阻塞启动**：没插卡时挂载会失败，
 *     重试若干次才超时，放主流程里会拖慢开机几百毫秒。
 *   - 每 3 s 重试一次，插卡后无需重启即可识别。
 *   - 容量查询走缓存（6 s 刷新一次）。FATFS 的 f_getfree 要遍历 FAT，
 *     32 GB 卡上一次十几毫秒，绝不能放在 LVGL 任务里同步调用。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 挂载点（FATFS） */
#define BSP_SD_MOUNT_POINT  "/sdcard"

/** @brief 启动后台挂载任务（立即返回，挂载结果异步生效） */
esp_err_t bsp_sdcard_init(void);

/** @brief 当前是否已挂载成功 */
bool bsp_sdcard_mounted(void);

/**
 * @brief 读缓存容量（KB）。未挂载返回 false，两个出参不动。
 * @note  只读缓存，不碰文件系统，可在 LVGL 任务里随时调用。
 */
bool bsp_sdcard_usage(uint64_t *total_kb, uint64_t *free_kb);

/** @brief 立即尝试挂载一次（串口命令用）。已挂载时直接返回 ESP_OK。 */
esp_err_t bsp_sdcard_mount_now(void);

/** @brief 卸载（串口命令用；下次重试会重新挂载） */
void bsp_sdcard_unmount(void);

#ifdef __cplusplus
}
#endif
