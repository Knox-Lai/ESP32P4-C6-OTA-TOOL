/**
 * @file  ota_frame.c
 * @brief 帧编码 / 解码 / CRC32
 */
#include <string.h>

#include "ota_frame.h"

uint32_t ota_crc32_update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) {
            /* 无表实现：分支换成位运算，代码量小、对 1 KB 量级足够快 */
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return crc;
}

uint32_t ota_crc32(const void *data, size_t len)
{
    return ~ota_crc32_update(0xFFFFFFFFu, data, len);
}

size_t ota_frame_encode(const ota_frame_t *f, uint8_t *out, size_t out_cap)
{
    if (f == NULL || out == NULL) {
        return 0;
    }
    if (f->len > OTA_MAX_PAYLOAD) {
        return 0;
    }

    size_t total = OTA_HDR_LEN + (size_t)f->len + OTA_CRC_LEN;
    if (out_cap < total) {
        return 0;
    }

    ota_put_u16(out + 0, OTA_FRAME_MAGIC);
    out[2] = f->ver;
    out[3] = f->cmd;
    ota_put_u16(out + 4, f->len);
    ota_put_u32(out + 6, f->seq);
    ota_put_u16(out + 10, f->flags);

    if (f->len > 0) {
        memcpy(out + OTA_HDR_LEN, f->payload, f->len);
    }

    uint32_t crc = ota_crc32(out, OTA_HDR_LEN + (size_t)f->len);
    ota_put_u32(out + OTA_HDR_LEN + f->len, crc);

    return total;
}

bool ota_frame_decode(const uint8_t *buf, size_t avail, ota_frame_t *f)
{
    if (buf == NULL || f == NULL) {
        return false;
    }
    if (avail < OTA_HDR_LEN + OTA_CRC_LEN) {
        return false;
    }
    if (ota_get_u16(buf) != OTA_FRAME_MAGIC) {
        return false;
    }

    uint16_t len = ota_get_u16(buf + 4);
    if (len > OTA_MAX_PAYLOAD) {
        return false;
    }
    if (avail < OTA_HDR_LEN + (size_t)len + OTA_CRC_LEN) {
        return false;
    }

    uint32_t crc_rx = ota_get_u32(buf + OTA_HDR_LEN + len);
    if (crc_rx != ota_crc32(buf, OTA_HDR_LEN + (size_t)len)) {
        return false;
    }

    f->ver   = buf[2];
    f->cmd   = buf[3];
    f->len   = len;
    f->seq   = ota_get_u32(buf + 6);
    f->flags = ota_get_u16(buf + 10);
    if (len > 0) {
        memcpy(f->payload, buf + OTA_HDR_LEN, len);
    }
    return true;
}
