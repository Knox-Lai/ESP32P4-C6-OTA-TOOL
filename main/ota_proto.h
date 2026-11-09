/**
 * @file  ota_proto.h
 * @brief OTA 命令的载荷编解码（协议契约层，对应 docs/ota-protocol.md §5）
 *
 * 分三层，互不越界：
 *   ota_frame.*  帧：MAGIC / VER / CMD / LEN / SEQ / FLAGS / CRC32（UDP、TCP 共用）
 *   ota_proto.*  载荷：OTA_BEGIN(64B) / BEGIN_ACK(12B) / DATA(4+N) / DATA_ACK(5B) / RESULT(6B)
 *   ota_client.* 会话：TCP 连接、滑动窗口、超时重传、状态机
 *
 * 全部手工按大端读写，不用 packed struct —— 设备端换编译器、换端序都不会错。
 * 改这里的任何偏移都必须同步改 docs/ota-protocol.md，否则设备端会对不上。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dev_table.h"      /* DEV_MODEL_LEN */
#include "ota_frame.h"      /* OTA_MAX_CHUNK / 大端读写 */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 载荷长度（线上固定值） ---------------- */
#define OTA_BEGIN_PAYLOAD_LEN   64
#define OTA_BEGIN_ACK_LEN       12
#define OTA_DATA_HDR_LEN        4       /* offset 字段，后面跟 N 字节数据 */
#define OTA_DATA_ACK_LEN        5
#define OTA_RESULT_LEN          6

/* ---------------- 协商范围 ---------------- */
#define OTA_CHUNK_MIN           64
#define OTA_CHUNK_MAX           ((uint16_t)OTA_MAX_CHUNK)   /* 1024 */
#define OTA_WINDOW_MIN          1
#define OTA_WINDOW_MAX          8

/* ---------------- 错误码（与 §7 一一对应） ---------------- */
enum {
    OTA_ERR_OK        = 0x00,
    OTA_ERR_MODEL     = 0x01,   /* BEGIN：型号不符 */
    OTA_ERR_HW_VER    = 0x02,   /* BEGIN：硬件版本不支持 */
    OTA_ERR_NO_SPACE  = 0x03,   /* BEGIN：设备空间不足 */
    OTA_ERR_BUSY      = 0x04,   /* BEGIN：设备已有会话 */
    OTA_ERR_OFFSET    = 0x05,   /* DATA：偏移越界/不对齐 */
    OTA_ERR_CRC       = 0x06,   /* DATA/END：单帧或整包 CRC 错 */
    OTA_ERR_SHA256    = 0x07,   /* END：整包 SHA-256 不符 */
    OTA_ERR_FLASH     = 0x08,   /* DATA/END：擦写失败 */
    OTA_ERR_SESSION   = 0x09,   /* DATA/END：session 不匹配 */
    OTA_ERR_VERSION   = 0x0A,   /* BEGIN：同版本或降级，需确认 */
    OTA_ERR_TIMEOUT   = 0x0B,   /* 任意：设备侧空闲超时 */

    /* 0x80 以上是"工具侧本地错误"，不会出现在设备回的帧里，
     * 只为让 UI 能区分"设备拒绝"和"我们连不上/等不到"。 */
    OTA_ERR_LOCAL_CONNECT = 0x81,   /* 连不上设备 4011 */
    OTA_ERR_LOCAL_PROTO   = 0x82,   /* 连接中断 / 帧无法解析 */
    OTA_ERR_LOCAL_VERIFY  = 0x83,   /* 重启后版本未变 */
};

/** @brief 错误码转中文短语，日志与 UI 共用 */
const char *ota_err_str(uint8_t err);

/* ---------------- 版本号：4 字节，与身份载荷里的 fw_ver[4] 字节序一致 ---------------- */
#define OTA_FW_VER(maj, min, pat, bld) \
    ((((uint32_t)(maj) & 0xFFu) << 24) | (((uint32_t)(min) & 0xFFu) << 16) | \
     (((uint32_t)(pat) & 0xFFu) << 8)  |  ((uint32_t)(bld) & 0xFFu))

/** @brief "1.0.3.12" 风格打印，内部静态缓冲轮转 */
const char *ota_fw_ver_str(uint32_t ver);

/* ---------------- OTA_BEGIN（64 B） ---------------- */
typedef struct {
    uint32_t fw_size;                   /* 镜像字节数，不含 DATA 的 4 B 偏移头 */
    uint32_t fw_crc32;                  /* 整包 CRC32，END 后快速失败 */
    uint32_t fw_ver;                    /* 目标版本，见 OTA_FW_VER */
    uint16_t hw_min;
    uint16_t hw_max;
    char     model[DEV_MODEL_LEN + 1];  /* 内部以 0 结尾；线上固定 16 B，不足补 0 */
    uint8_t  sha256[32];
} ota_begin_t;

size_t ota_begin_encode(const ota_begin_t *b, uint8_t *out, size_t cap);
bool   ota_begin_decode(const uint8_t *in, size_t len, ota_begin_t *b);

/* ---------------- OTA_BEGIN_ACK（12 B） ---------------- */
typedef struct {
    uint8_t  status;                    /* 0 = OK */
    uint16_t chunk_size;                /* 64~1024，4 的倍数 */
    uint8_t  window;                    /* 1~8 */
    uint32_t session_id;                /* 仅日志对账用 */
    uint32_t resume_offset;             /* 0 = 从头；非 0 = 从该偏移续传 */
} ota_begin_ack_t;

size_t ota_begin_ack_encode(const ota_begin_ack_t *a, uint8_t *out, size_t cap);
bool   ota_begin_ack_decode(const uint8_t *in, size_t len, ota_begin_ack_t *a);

/* ---------------- OTA_DATA_ACK（5 B） ---------------- */
typedef struct {
    uint32_t next_offset;               /* 累计确认：此前的字节都已落盘 */
    uint8_t  status;
} ota_data_ack_t;

size_t ota_data_ack_encode(const ota_data_ack_t *a, uint8_t *out, size_t cap);
bool   ota_data_ack_decode(const uint8_t *in, size_t len, ota_data_ack_t *a);

/* ---------------- OTA_RESULT（6 B） ---------------- */
typedef struct {
    uint8_t  status;                    /* 0 = 成功（待重启） */
    uint8_t  err_code;
    uint32_t detail;                    /* 失败偏移 / 期望与实际 CRC 等，按 err_code 解释 */
} ota_result_t;

size_t ota_result_encode(const ota_result_t *r, uint8_t *out, size_t cap);
bool   ota_result_decode(const uint8_t *in, size_t len, ota_result_t *r);

#ifdef __cplusplus
}
#endif
