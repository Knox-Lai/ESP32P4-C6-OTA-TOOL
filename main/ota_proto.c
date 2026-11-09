/**
 * @file  ota_proto.c
 * @brief 载荷编解码实现。字段偏移严格按 docs/ota-protocol.md §5。
 */
#include <stdio.h>
#include <string.h>

#include "ota_frame.h"      /* ota_put_u16 / ota_get_u32 / OTA_MAX_PAYLOAD */
#include "ota_proto.h"

const char *ota_err_str(uint8_t err)
{
    switch (err) {
    case OTA_ERR_OK:       return "OK";
    case OTA_ERR_MODEL:    return "型号不符";
    case OTA_ERR_HW_VER:   return "硬件版本不支持";
    case OTA_ERR_NO_SPACE: return "设备空间不足";
    case OTA_ERR_BUSY:     return "设备忙（已有会话）";
    case OTA_ERR_OFFSET:   return "偏移越界/不对齐";
    case OTA_ERR_CRC:      return "CRC 校验失败";
    case OTA_ERR_SHA256:   return "SHA-256 不符";
    case OTA_ERR_FLASH:    return "Flash 擦写失败";
    case OTA_ERR_SESSION:  return "会话失配（设备可能重启过）";
    case OTA_ERR_VERSION:  return "版本相同或回退，需确认";
    case OTA_ERR_TIMEOUT:  return "设备侧空闲超时";
    case OTA_ERR_LOCAL_CONNECT: return "连不上设备（4011 未监听？）";
    case OTA_ERR_LOCAL_PROTO:   return "连接中断或帧无法解析";
    case OTA_ERR_LOCAL_VERIFY:  return "重启后版本未变（跳转失败/回滚）";
    default:               return "未知错误";
    }
}

const char *ota_fw_ver_str(uint32_t ver)
{
    static char buf[4][20];
    static int  idx = 0;
    char *p = buf[idx = (idx + 1) & 3];
    snprintf(p, sizeof(buf[0]), "%u.%u.%u.%u",
             (unsigned)((ver >> 24) & 0xFF), (unsigned)((ver >> 16) & 0xFF),
             (unsigned)((ver >> 8) & 0xFF),  (unsigned)(ver & 0xFF));
    return p;
}

/* ---------------- OTA_BEGIN ---------------- */

size_t ota_begin_encode(const ota_begin_t *b, uint8_t *out, size_t cap)
{
    if (b == NULL || out == NULL || cap < OTA_BEGIN_PAYLOAD_LEN) {
        return 0;
    }
    memset(out, 0, OTA_BEGIN_PAYLOAD_LEN);

    ota_put_u32(out + 0,  b->fw_size);
    ota_put_u32(out + 4,  b->fw_crc32);
    ota_put_u32(out + 8,  b->fw_ver);
    ota_put_u16(out + 12, b->hw_min);
    ota_put_u16(out + 14, b->hw_max);

    /* model 固定 16 B；来源是 char[]，用 strnlen 保证不越界读 */
    size_t mlen = strnlen(b->model, DEV_MODEL_LEN);
    memcpy(out + 16, b->model, mlen);

    memcpy(out + 32, b->sha256, 32);
    return OTA_BEGIN_PAYLOAD_LEN;
}

bool ota_begin_decode(const uint8_t *in, size_t len, ota_begin_t *b)
{
    if (in == NULL || b == NULL || len < OTA_BEGIN_PAYLOAD_LEN) {
        return false;
    }
    b->fw_size  = ota_get_u32(in + 0);
    b->fw_crc32 = ota_get_u32(in + 4);
    b->fw_ver   = ota_get_u32(in + 8);
    b->hw_min   = ota_get_u16(in + 12);
    b->hw_max   = ota_get_u16(in + 14);

    memcpy(b->model, in + 16, DEV_MODEL_LEN);
    b->model[DEV_MODEL_LEN] = '\0';

    memcpy(b->sha256, in + 32, 32);
    return true;
}

/* ---------------- OTA_BEGIN_ACK ---------------- */

size_t ota_begin_ack_encode(const ota_begin_ack_t *a, uint8_t *out, size_t cap)
{
    if (a == NULL || out == NULL || cap < OTA_BEGIN_ACK_LEN) {
        return 0;
    }
    out[0] = a->status;
    ota_put_u16(out + 1, a->chunk_size);
    out[3] = a->window;
    ota_put_u32(out + 4, a->session_id);
    ota_put_u32(out + 8, a->resume_offset);
    return OTA_BEGIN_ACK_LEN;
}

bool ota_begin_ack_decode(const uint8_t *in, size_t len, ota_begin_ack_t *a)
{
    if (in == NULL || a == NULL || len < OTA_BEGIN_ACK_LEN) {
        return false;
    }
    a->status        = in[0];
    a->chunk_size    = ota_get_u16(in + 1);
    a->window        = in[3];
    a->session_id    = ota_get_u32(in + 4);
    a->resume_offset = ota_get_u32(in + 8);
    return true;
}

/* ---------------- OTA_DATA_ACK ---------------- */

size_t ota_data_ack_encode(const ota_data_ack_t *a, uint8_t *out, size_t cap)
{
    if (a == NULL || out == NULL || cap < OTA_DATA_ACK_LEN) {
        return 0;
    }
    ota_put_u32(out + 0, a->next_offset);
    out[4] = a->status;
    return OTA_DATA_ACK_LEN;
}

bool ota_data_ack_decode(const uint8_t *in, size_t len, ota_data_ack_t *a)
{
    if (in == NULL || a == NULL || len < OTA_DATA_ACK_LEN) {
        return false;
    }
    a->next_offset = ota_get_u32(in + 0);
    a->status      = in[4];
    return true;
}

/* ---------------- OTA_RESULT ---------------- */

size_t ota_result_encode(const ota_result_t *r, uint8_t *out, size_t cap)
{
    if (r == NULL || out == NULL || cap < OTA_RESULT_LEN) {
        return 0;
    }
    out[0] = r->status;
    out[1] = r->err_code;
    ota_put_u32(out + 2, r->detail);
    return OTA_RESULT_LEN;
}

bool ota_result_decode(const uint8_t *in, size_t len, ota_result_t *r)
{
    if (in == NULL || r == NULL || len < OTA_RESULT_LEN) {
        return false;
    }
    r->status   = in[0];
    r->err_code = in[1];
    r->detail   = ota_get_u32(in + 2);
    return true;
}
