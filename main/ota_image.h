/**
 * @file  ota_image.h
 * @brief 固件镜像源（阶段 5 只需要"能读字节 + 元信息"这一层抽象）
 *
 * 抽成回调，是为了以后接真实固件文件（阶段 7：从 storage 分区读 `.fwp`）时
 * 升级引擎一行都不用改 —— 只要换一个 read 实现。
 *
 * 当前提供：
 *   - `ota_image_make_synthetic()` 合成镜像：内容由偏移确定性生成，
 *     不占 Flash、不占内存，随时可造 64 KB ~ 2 MB 的测试数据，
 *     用来在真设备端固件写出来之前把整条 OTA 会话跑通。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "dev_table.h"      /* DEV_MODEL_LEN */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ota_image ota_image_t;

struct ota_image {
    char     name[24];                  /* 仅用于日志，如 "synthetic-256K" */
    char     model[DEV_MODEL_LEN + 1];  /* 目标型号，须与设备自报的 MODEL 逐字节相等 */
    uint16_t hw_min;
    uint16_t hw_max;
    uint32_t fw_ver;                    /* 见 OTA_FW_VER() */
    uint32_t size;                      /* 镜像字节数 */
    uint32_t crc32;                     /* 整包 CRC32（= zlib.crc32） */
    uint8_t  sha256[32];                /* 整包 SHA-256 */
    uint32_t seed;                      /* 合成镜像的种子 */
    /* 读 offset 起 len 字节到 buf；返回 ESP_OK 表示成功 */
    esp_err_t (*read)(ota_image_t *img, uint32_t offset, void *buf, uint32_t len);
};

/**
 * @brief 生成合成镜像并算好 CRC32 与 SHA-256。
 * @param size  镜像字节数，建议 64*1024 ~ 2*1024*1024
 * @note  同 seed 同 size 的镜像内容完全一致，方便两次升级结果可比对。
 */
esp_err_t ota_image_make_synthetic(ota_image_t *img,
                                   const char *model,
                                   uint16_t hw_min, uint16_t hw_max,
                                   uint32_t fw_ver,
                                   uint32_t size,
                                   uint32_t seed);

/** @brief 打印镜像元信息（一行） */
void ota_image_log(const ota_image_t *img);

#ifdef __cplusplus
}
#endif
