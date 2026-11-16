/**
 * @file  bsp_sdcard.h
 * @brief TF(microSD) 卡：后台挂载 + 容量查询
 *
 * 板级事实：SD 走 SDMMC 4 线，Slot 0 的 IO MUX 默认引脚
 * （CLK=43 / CMD=44 / D0..D3=39~42），IO 电源由片内 LDO ch4 供电。
 * 没有卡检测（CD）脚，所以"有没有卡"只能靠挂载成功与否判断，
 * 插拔也无法产生中断 —— 探测只发生在被显式请求时。
 *
 * 设计要点：
 *   - 挂载放在独立低优先级任务里做，**不阻塞启动**：没插卡时挂载会失败，
 *     试若干次才超时，放主流程里会拖慢开机几百毫秒。
 *   - **不做后台轮询**。开机探测一次，之后只在 bsp_sdcard_probe() /
 *     bsp_sdcard_mount_now() 被调用时再探。插卡后要有一次"用户动作"
 *     （shell 敲 sd、刷新固件库）才会被认出来。
 *   - 已挂载状态下探测 = stat + 刷容量；stat 失败即判定拔卡并卸载。
 *   - 容量查询走缓存，只在探测时刷新。FATFS 的 f_getfree 要遍历 FAT，
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

/** @brief 启动后台任务（立即返回），并异步探测一次；之后不再自动轮询 */
esp_err_t bsp_sdcard_init(void);

/**
 * @brief 异步请求探测一次（投给 sd 任务，立即返回）。
 * @note  UI / LVGL 任务里只能用这个：完整挂载会阻塞几百毫秒。
 */
void bsp_sdcard_probe(void);

/** @brief 当前是否已挂载成功 */
bool bsp_sdcard_mounted(void);

/**
 * @brief 读缓存容量（KB）。未挂载返回 false，两个出参不动。
 * @note  只读缓存，不碰文件系统，可在 LVGL 任务里随时调用。
 */
bool bsp_sdcard_usage(uint64_t *total_kb, uint64_t *free_kb);

/**
 * @brief 同步探测一次：没挂载就挂，已挂载就校验 + 刷容量。
 * @return ESP_OK 已挂载；ESP_ERR_NOT_FOUND 卡不在；
 *         其它为挂载失败原因（没插卡 / 非 FAT / 接触不良）。
 * @note  会阻塞几百毫秒，只能在 shell 或后台任务里调用。
 */
esp_err_t bsp_sdcard_mount_now(void);

/** @brief 卸载（串口命令用；下次探测会重新挂载） */
void bsp_sdcard_unmount(void);

#ifdef __cplusplus
}
#endif
