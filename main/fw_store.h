/**
 * @file  fw_store.h
 * @brief 固件仓库：TF 卡 + 内部 Flash 双介质，对上层屏蔽路径
 *
 * 两个介质、同一个 API：
 *   TF 卡   `/sdcard/fw/` 里的 `.fwp`   —— 用户拷进来，随插随用（由 bsp_sdcard 挂载）
 *   内部    `/storage/fw/` 里的 `.fwp`  —— 8 MB `storage` 分区，FAT + 磨损均衡
 *
 * 为什么内部用 FAT 而不是 SPIFFS：
 *   SPIFFS 无目录、`CONFIG_SPIFFS_OBJ_NAME_LEN=32`（路径一长就截断）、大文件 GC 抖动大；
 *   FAT 与 TF 卡共用同一套 POSIX 接口，本模块只有一份路径逻辑，
 *   并且能靠 `.part` → `rename()` 实现"写完整才可见"的原子入库。
 *
 * 并发约定：
 *   - 扫描与缓存更新受互斥量保护，任务里可以随时 `fw_store_snapshot()`；
 *   - `fw_store_open()` 拿到的是独立 FILE 句柄，与扫描互不影响，
 *     但**一次只允许一个升级会话**（ota_client 的既有约束）；
 *   - 本模块不做长时间持锁（导入/校验都在锁外做，只在最后更新缓存）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "dev_model.h"
#include "dev_table.h"      /* DEV_MODEL_LEN */
#include "ota_image.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FW_STORE_INT_MOUNT  "/storage"
#define FW_STORE_INT_DIR    "/storage/fw"
#define FW_STORE_SD_DIR     "/sdcard/fw"
#define FW_STORE_MAX        24          /* 缓存条目上限，超出只列前 N 个 */
#define FW_STORE_PATH_LEN   72

typedef enum {
    FW_SRC_SD = 0,      /* TF 卡 */
    FW_SRC_FLASH,       /* 内部 storage 分区 */
    FW_SRC_COUNT,
} fw_src_t;

typedef struct {
    fw_src_t   src;
    char       path[FW_STORE_PATH_LEN];
    char       raw_model[DEV_MODEL_LEN + 1];
    fw_model_t model_id;
    uint16_t   hw_min;
    uint16_t   hw_max;
    uint32_t   fw_ver;
    uint32_t   size;                    /* 负载字节数（不含 96 B 头） */
    uint8_t    sha256[32];
} fw_item_t;

/**
 * @brief 挂内部仓库（首次挂载会格式化 8 MB 分区）+ 起后台扫描任务。
 * @note  不阻塞启动：挂载失败（分区异常）只打日志，TF 卡侧仍然可用。
 */
esp_err_t fw_store_init(void);

/** @brief 请求下一次后台周期重新扫描（导入/删除/SD 插拔后调用） */
void fw_store_request_scan(void);

/** @brief 立即扫描一次并刷新缓存（会做文件 IO，别在 LVGL 任务里调） */
int fw_store_scan(void);

/**
 * @brief 拷贝缓存快照，按型号分组、版本降序。
 * @return 条目数
 */
int fw_store_snapshot(fw_item_t *out, int max);

/** @brief 把某个条目绑成 ota_image（打开独立句柄；会话结束时由 ota_client 调 release） */
esp_err_t fw_store_open(const fw_item_t *it, ota_image_t *img);

/**
 * @brief 把第 idx 条（必须来自 TF 卡）复制进内部仓库。
 * @param msg/cap  失败原因写入这里，供串口/UI 直接显示
 * @note  先写 `.part`，校验通过才 rename，中途断电不留半包。
 */
esp_err_t fw_store_import(int idx, char *msg, size_t cap);

/** @brief 删除第 idx 条（只允许删内部仓库的，TF 卡是用户的，不碰） */
esp_err_t fw_store_remove(int idx);

/** @brief 某介质的剩余空间（字节）。介质未挂载返回 false */
bool fw_store_space(fw_src_t src, uint64_t *free_b);

/** @brief 内部仓库是否挂载成功 */
bool fw_store_int_mounted(void);

/**
 * @brief 该型号版本最高的条目。
 * @return 指向缓存内部的指针（下一次扫描后失效）；没有则 NULL
 */
const fw_item_t *fw_store_best(fw_model_t m);

/** @brief "SD" / "FLASH" */
const char *fw_src_str(fw_src_t s);

#ifdef __cplusplus
}
#endif
