/**
 * @file  fw_pkg.c
 * @brief `.fwp` 包头解析与负载校验
 *
 * 全部手工按小端读写，不用 packed struct：文件在 PC 上由 fwp_pack.py 生成，
 * 换个编译器/对齐策略也不该让偏移量改变。头固定 96 B，一次 fread 拿完。
 */
#include "fw_pkg.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

#include "ota_frame.h"      /* ota_crc32_update，与 zlib.crc32 等价 */
#include "ota_proto.h"      /* ota_fw_ver_str */
#include "ota_sha256.h"

static const char *TAG = "fw_pkg";

#define VERIFY_CHUNK  4096u

static inline uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

esp_err_t fw_pkg_parse(FILE *f, fw_pkg_hdr_t *out)
{
    ESP_RETURN_ON_FALSE(f != NULL && out != NULL, ESP_ERR_INVALID_ARG, TAG, "参数为空");

    memset(out, 0, sizeof(*out));

    if (fseek(f, 0, SEEK_END) != 0) {
        return ESP_FAIL;
    }
    long sz = ftell(f);
    if (sz < (long)FW_PKG_HDR_LEN) {
        ESP_LOGW(TAG, "文件只有 %ld 字节，连包头都不够", sz);
        return ESP_ERR_INVALID_SIZE;
    }
    rewind(f);

    uint8_t b[FW_PKG_HDR_LEN];
    if (fread(b, 1, sizeof(b), f) != sizeof(b)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (memcmp(b, FW_PKG_MAGIC, 4) != 0) {
        ESP_LOGW(TAG, "magic 不是 FWP1（%02x%02x%02x%02x）", b[0], b[1], b[2], b[3]);
        return ESP_ERR_INVALID_ARG;
    }
    if (b[4] != FW_PKG_HDR_VER) {
        ESP_LOGW(TAG, "包头版本 %u 不支持（当前 %u）", (unsigned)b[4], (unsigned)FW_PKG_HDR_VER);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (rd_u16(b + 6) != FW_PKG_HDR_LEN) {
        ESP_LOGW(TAG, "hdr_len=%u，本工具只认 %u", (unsigned)rd_u16(b + 6), (unsigned)FW_PKG_HDR_LEN);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* model 固定 16 B，可能没有终止符；强制补 0 后当 C 串用 */
    memcpy(out->model, b + 8, DEV_MODEL_LEN);
    out->model[DEV_MODEL_LEN] = '\0';
    if (out->model[0] == '\0') {
        ESP_LOGW(TAG, "包头 model 为空");
        return ESP_ERR_INVALID_ARG;
    }

    out->hw_min        = rd_u16(b + 24);
    out->hw_max        = rd_u16(b + 26);
    out->fw_ver        = rd_u32(b + 28);
    out->payload_len   = rd_u32(b + 32);
    out->payload_crc32 = rd_u32(b + 36);
    memcpy(out->sha256, b + 40, sizeof(out->sha256));

    memcpy(out->build_time, b + 80, sizeof(out->build_time) - 1);
    out->build_time[sizeof(out->build_time) - 1] = '\0';

    if (out->payload_len == 0) {
        ESP_LOGW(TAG, "payload_len=0");
        return ESP_ERR_INVALID_SIZE;
    }
    if (out->payload_len > FW_PKG_MAX_BYTES) {
        ESP_LOGW(TAG, "payload %u KB 超过上限 %u KB",
                 (unsigned)(out->payload_len / 1024u), (unsigned)(FW_PKG_MAX_BYTES / 1024u));
        return ESP_ERR_INVALID_SIZE;
    }
    if ((uint32_t)sz != FW_PKG_HDR_LEN + out->payload_len) {
        ESP_LOGW(TAG, "文件长 %ld，但头里写 96+%u = %u（文件被截断或尾部有垃圾）",
                 sz, (unsigned)out->payload_len,
                 (unsigned)(FW_PKG_HDR_LEN + out->payload_len));
        return ESP_ERR_INVALID_SIZE;
    }

    out->file_size = (uint32_t)sz;
    return ESP_OK;
}

esp_err_t fw_pkg_verify_payload(FILE *f, const fw_pkg_hdr_t *h)
{
    ESP_RETURN_ON_FALSE(f != NULL && h != NULL, ESP_ERR_INVALID_ARG, TAG, "参数为空");

    uint8_t *buf = malloc(VERIFY_CHUNK);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (fseek(f, FW_PKG_HDR_LEN, SEEK_SET) != 0) {
        free(buf);
        return ESP_FAIL;
    }

    uint32_t crc = 0xFFFFFFFFu;
    ota_sha256_ctx_t sha;
    ota_sha256_init(&sha);

    uint32_t left = h->payload_len;
    while (left > 0) {
        uint32_t n = (left > VERIFY_CHUNK) ? VERIFY_CHUNK : left;
        if (fread(buf, 1, n, f) != n) {
            ESP_LOGW(TAG, "读取负载失败（偏移 %u）",
                     (unsigned)(h->payload_len - left));
            free(buf);
            return ESP_ERR_INVALID_SIZE;
        }
        crc = ota_crc32_update(crc, buf, n);
        ota_sha256_update(&sha, buf, n);
        left -= n;
    }
    free(buf);

    if ((~crc) != h->payload_crc32) {
        ESP_LOGW(TAG, "CRC32 不符：实测 0x%08x，头里 0x%08x",
                 (unsigned)(~crc), (unsigned)h->payload_crc32);
        return ESP_ERR_INVALID_CRC;
    }

    uint8_t digest[32];
    ota_sha256_final(&sha, digest);
    if (memcmp(digest, h->sha256, sizeof(digest)) != 0) {
        ESP_LOGW(TAG, "SHA-256 不符：实测 %02x%02x%02x%02x…，头里 %02x%02x%02x%02x…",
                 digest[0], digest[1], digest[2], digest[3],
                 h->sha256[0], h->sha256[1], h->sha256[2], h->sha256[3]);
        return ESP_ERR_INVALID_CRC;
    }

    return ESP_OK;
}

void fw_pkg_log(const fw_pkg_hdr_t *h, const char *path)
{
    if (h == NULL) {
        return;
    }
    ESP_LOGI(TAG, "'%s'  model='%s'  hw=%u.%u~%u.%u  fw=%s  %u KB  sha=%02x%02x%02x%02x…",
             (path != NULL) ? path : "-", h->model,
             (unsigned)(h->hw_min >> 8), (unsigned)(h->hw_min & 0xFF),
             (unsigned)(h->hw_max >> 8), (unsigned)(h->hw_max & 0xFF),
             ota_fw_ver_str(h->fw_ver),
             (unsigned)(h->payload_len / 1024u),
             h->sha256[0], h->sha256[1], h->sha256[2], h->sha256[3]);
}
