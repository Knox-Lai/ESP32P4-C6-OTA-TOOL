/**
 * @file  ota_image.h
 * @brief 固件镜像源（阶段 5 只需要"能读字节 + 元信息"这一层抽象）
 *
 * 抽成回调，是为了"合成镜像"和"真实 .fwp 文件"共用同一套升级引擎 ——
 * ota_client 只调 `read()`，不知道字节从哪来。
 *
 * 当前提供：
 *   - `ota_image_make_synthetic()` 合成镜像：内容由偏移确定性生成，
 *     不占 Flash、不占内存，随时可造 64 KB ~ 2 MB 的测试数据，
 *     用来在真设备端固件写出来之前把整条 OTA 会话跑通。
 *   - `ota_image_from_file()` 真实固件：打开 `.fwp`，跳过 96 B 包头读负载。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "dev_table.h"      /* DEV_MODEL_LEN */
#include "fw_pkg.h"         /* fw_pkg_hdr_t */

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
    /* 文件源专用：句柄等私有数据 + 回收钩子（合成镜像两者都是 NULL）。
     * ota_client 在会话结束时调一次 release，所以升级引擎不需要知道源类型。 */
    void     *ctx;
    void     (*release)(ota_image_t *img);
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

/**
 * @brief 打开真实固件文件（`.fwp`），把包头字段填进 img，负载由 read 回调按需读。
 * @param h     已由 fw_pkg_parse() 校验过的包头
 * @param path  文件绝对路径（VFS 路径，如 "/sdcard/demo.fwp"）
 * @note  成功时必须由调用方（ota_client 在会话结束）调 `img->release(img)` 关句柄。
 *        文件被拔掉/截断时 read 返回 ESP_ERR_INVALID_SIZE，不会崩。
 */
esp_err_t ota_image_from_file(ota_image_t *img, const fw_pkg_hdr_t *h, const char *path);

/** @brief 打印镜像元信息（一行） */
void ota_image_log(const ota_image_t *img);

#ifdef __cplusplus
}
#endif
