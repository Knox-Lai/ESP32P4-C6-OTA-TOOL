/**
 * @file  fw_pkg.h
 * @brief `.fwp` 固件包：96 字节自描述包头 + 裸负载
 *
 * 为什么要在固件文件里放头，而不是靠文件名/服务器字段：
 *   TF 卡是离线介质，卡里散放几个 bin 时，"这是谁的固件、什么版本"只能靠猜。
 *   头里写死 型号 + hw 范围 + 版本 + 长度 + SHA-256，插卡即可自证，
 *   不依赖任何外部清单，也不会刷错型号。
 *
 * 布局（小端，负载从 offset 96 开始，32 字节对齐）：
 * ```
 * off  size  field
 *   0    4   magic        "FWP1"
 *   4    1   hdr_ver      = 1
 *   5    1   flags        bit0 保留（压缩），当前 0
 *   6    2   hdr_len      = 96
 *   8   16   model        ASCII 补 0，与设备自报的 identity.model 逐字节可比
 *  24    2   hw_min       0xMMmm
 *  26    2   hw_max
 *  28    4   fw_ver       (a<<24)|(b<<16)|(c<<8)|d，见 OTA_FW_VER()
 *  32    4   payload_len  纯负载字节数（不含头）
 *  36    4   payload_crc32
 *  40   32   sha256[32]   对纯负载算
 *  72    8   reserved     全 0
 *  80   16   build_time   "2026-11-12 10:30"，仅展示
 * --- payload ---
 * ```
 * 校验从严：magic / hdr_ver / hdr_len / 文件总长 任一不符即拒绝，不做兼容猜测。
 */
#pragma once

#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

#include "dev_table.h"      /* DEV_MODEL_LEN */

#ifdef __cplusplus
extern "C" {
#endif

#define FW_PKG_MAGIC        "FWP1"
#define FW_PKG_HDR_VER      1
#define FW_PKG_HDR_LEN      96

/* 单包上限：内部仓库是 8 MB 分区，留一半余量。
 * 也正好卡住"误把整片 Flash 镜像拖进来"的情况。 */
#define FW_PKG_MAX_BYTES    (4u * 1024u * 1024u)

typedef struct {
    char     model[DEV_MODEL_LEN + 1];
    uint16_t hw_min;
    uint16_t hw_max;
    uint32_t fw_ver;
    uint32_t payload_len;
    uint32_t payload_crc32;
    uint8_t  sha256[32];
    char     build_time[17];
    uint32_t file_size;                 /* 头 + 负载，parse 时实测 */
} fw_pkg_hdr_t;

/**
 * @brief 读并校验包头（不读负载）。
 * @param f  已打开的文件，函数内部会 fseek 到 0，退出时位置不保证
 * @note  失败返回 ESP_ERR_INVALID_ARG（非 .fwp）/ ESP_ERR_INVALID_SIZE（长度不符）/
 *        ESP_ERR_NOT_SUPPORTED（版本或标志不支持）
 */
esp_err_t fw_pkg_parse(FILE *f, fw_pkg_hdr_t *out);

/** @brief 流式重算负载 CRC32 + SHA-256 并与包头比对（导入前必做一次） */
esp_err_t fw_pkg_verify_payload(FILE *f, const fw_pkg_hdr_t *h);

/** @brief 一行摘要：型号 / 版本 / 大小 / SHA 前 8 字节 */
void fw_pkg_log(const fw_pkg_hdr_t *h, const char *path);

#ifdef __cplusplus
}
#endif
