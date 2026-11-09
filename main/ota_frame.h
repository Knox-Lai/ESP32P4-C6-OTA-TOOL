/**
 * @file  ota_frame.h
 * @brief 通用帧格式（UDP 发现与后续 TCP OTA 共用同一套头，避免两套协议）
 *
 *  0        1        2        3        4        5        6        7
 * +--------+--------+--------+--------+--------+--------+--------+--------+
 * |  MAGIC(2B,BE 0x4F54)   | VER(1B)| CMD(1B)|       LEN(2B,BE)        |
 * +--------+--------+--------+--------+--------+--------+--------+--------+
 * |                     SEQ(4B,BE)        |        FLAGS(2B,BE)        |
 * +--------+--------+--------+--------+--------+--------+--------+--------+
 * |              PAYLOAD (LEN 字节)          |      CRC32(4B, BE)       |
 * +----------------------------------------------------------------------+
 *
 * - 头固定 12 字节；CRC32 覆盖「头 + PAYLOAD」，不含自身。
 * - CRC32 与 zlib.crc32() 完全等价（poly 0xEDB88320，init/final 取反），
 *   所以 PC 侧模拟器直接用 binascii.crc32() 即可，不需要额外实现。
 * - 不用 packed struct 直接 memcpy：所有字段手工按大端打包，换编译器/换端序都不会错。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_FRAME_MAGIC      0x4F54u   /* 'O' 'T'，流内重同步用 */
#define OTA_PROTO_VER        0x01u
#define OTA_HDR_LEN          12u
#define OTA_CRC_LEN          4u

/* 单帧允许的最大固件数据量（由设备在 OTA_BEGIN_ACK 里协商，上限不变） */
#define OTA_MAX_CHUNK        1024u

/* 载荷上限：OTA_DATA 的载荷是「4 B offset 头 + N 字节数据」，N 最大 1024，
 * 所以上限必须是 1028 —— 若按 1024 算，chunk=1024 时最后一帧会超限，
 * 而设备端按 chunk 对齐检查 offset 又会判定 E_OFFSET。 */
#define OTA_MAX_PAYLOAD      (OTA_MAX_CHUNK + 4u)
#define OTA_FRAME_MAX        (OTA_HDR_LEN + OTA_MAX_PAYLOAD + OTA_CRC_LEN)

/* 命令字（与 PLAN.md §5.2 一致） */
enum {
    OTA_CMD_DISCOVER      = 0x01,  /* 工具 → 设备：广播探测 */
    OTA_CMD_DISCOVER_RSP  = 0x02,  /* 设备 → 工具：单播回身份 */
    OTA_CMD_ANNOUNCE      = 0x03,  /* 设备 → 工具：主动上报 */
    OTA_CMD_QUERY_INFO    = 0x21,  /* 工具 → 设备：单播查身份（兼心跳） */
    OTA_CMD_OTA_BEGIN     = 0x10,
    OTA_CMD_OTA_BEGIN_ACK = 0x11,
    OTA_CMD_OTA_DATA      = 0x12,
    OTA_CMD_OTA_DATA_ACK  = 0x13,
    OTA_CMD_OTA_END       = 0x14,
    OTA_CMD_OTA_RESULT    = 0x15,
    OTA_CMD_REBOOT        = 0x20,
};

/* FLAGS 位域（预留） */
#define OTA_FLAG_LAST_PACKET  (1u << 0)
#define OTA_FLAG_RETRANSMIT   (1u << 1)
#define OTA_FLAG_NEED_ACK     (1u << 2)

typedef struct {
    uint8_t  ver;
    uint8_t  cmd;
    uint16_t len;                        /* payload 实际长度 */
    uint32_t seq;
    uint16_t flags;
    uint8_t  payload[OTA_MAX_PAYLOAD];
} ota_frame_t;

/** @brief CRC32（= zlib.crc32，poly 0xEDB88320，初值与结果均取反） */
uint32_t ota_crc32(const void *data, size_t len);

/**
 * @brief 增量 CRC32（整包固件要分块算，不能一次读进内存）
 * @param crc  初值传 0xFFFFFFFF；后续传上次返回值
 * @return 中间状态，**最后必须取反**才是与 zlib.crc32 一致的结果
 */
uint32_t ota_crc32_update(uint32_t crc, const void *data, size_t len);

/**
 * @brief 编码一帧。
 * @return 写入 out 的总字节数；0 = 失败（len 超限 / out 空间不足 / 参数为空）
 */
size_t ota_frame_encode(const ota_frame_t *f, uint8_t *out, size_t out_cap);

/**
 * @brief 解码一帧（校验 MAGIC、长度、CRC32）。
 * @param avail  收到的字节数
 * @return true = 帧合法且完整
 */
bool ota_frame_decode(const uint8_t *buf, size_t avail, ota_frame_t *f);

/** @brief 4 字节大端读写（协议统一大端） */
static inline void ota_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static inline void ota_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v & 0xFF);
}

static inline uint16_t ota_get_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline uint32_t ota_get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

#ifdef __cplusplus
}
#endif
