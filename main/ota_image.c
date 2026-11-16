/**
 * @file  ota_image.c
 * @brief 合成镜像实现：字节由绝对偏移确定性生成，可随机访问、可复现
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

#include "ota_frame.h"
#include "ota_image.h"
#include "ota_proto.h"      /* ota_fw_ver_str */
#include "ota_sha256.h"

static const char *TAG = "ota_image";

/* 偏移 -> 字节。必须只依赖 offset，才能做到"随机访问 + 重传时可重算" */
static inline uint8_t synth_byte(uint32_t off, uint32_t seed)
{
    uint32_t x = off * 2654435761u + seed * 2246822519u + 0x9E3779B9u;
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    x *= 3266489917u;
    x ^= x >> 16;
    return (uint8_t)(x >> 24);
}

static void synth_fill(uint32_t offset, uint8_t *buf, uint32_t len, uint32_t seed)
{
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = synth_byte(offset + i, seed);
    }
}

static esp_err_t synth_read(ota_image_t *img, uint32_t offset, void *buf, uint32_t len)
{
    if (img == NULL || buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (offset > img->size || len > img->size - offset) {
        return ESP_ERR_INVALID_SIZE;
    }
    synth_fill(offset, (uint8_t *)buf, len, img->seed);
    return ESP_OK;
}

esp_err_t ota_image_make_synthetic(ota_image_t *img,
                                   const char *model,
                                   uint16_t hw_min, uint16_t hw_max,
                                   uint32_t fw_ver,
                                   uint32_t size,
                                   uint32_t seed)
{
    ESP_RETURN_ON_FALSE(img != NULL, ESP_ERR_INVALID_ARG, TAG, "img 为空");
    ESP_RETURN_ON_FALSE(model != NULL && model[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "model 为空");
    ESP_RETURN_ON_FALSE(size > 0, ESP_ERR_INVALID_ARG, TAG, "size 必须 > 0");

    memset(img, 0, sizeof(*img));
    strncpy(img->model, model, DEV_MODEL_LEN);
    img->model[DEV_MODEL_LEN] = '\0';
    img->hw_min  = hw_min;
    img->hw_max  = hw_max;
    img->fw_ver  = fw_ver;
    img->size    = size;
    img->seed    = seed;
    img->read    = synth_read;
    snprintf(img->name, sizeof(img->name), "synthetic-%uK", (unsigned)(size / 1024));

    /* 一遍扫完同时算 CRC32 与 SHA-256；1 KB 缓冲，栈开销固定 */
    uint8_t  chunk[1024];
    uint32_t crc = 0xFFFFFFFFu;
    ota_sha256_ctx_t sha;
    ota_sha256_init(&sha);

    for (uint32_t off = 0; off < size; ) {
        uint32_t n = (size - off > sizeof(chunk)) ? (uint32_t)sizeof(chunk) : (size - off);
        synth_fill(off, chunk, n, seed);
        crc = ota_crc32_update(crc, chunk, n);
        ota_sha256_update(&sha, chunk, n);
        off += n;
    }

    img->crc32 = ~crc;
    ota_sha256_final(&sha, img->sha256);

    ota_image_log(img);
    return ESP_OK;
}

void ota_image_log(const ota_image_t *img)
{
    if (img == NULL) {
        return;
    }
    ESP_LOGI(TAG, "镜像 '%s'  model='%s'  hw=%u.%u~%u.%u  fw=%s  size=%u KB  crc32=0x%08x",
             img->name, img->model,
             (unsigned)(img->hw_min >> 8), (unsigned)(img->hw_min & 0xFF),
             (unsigned)(img->hw_max >> 8), (unsigned)(img->hw_max & 0xFF),
             ota_fw_ver_str(img->fw_ver),
             (unsigned)(img->size / 1024), (unsigned)img->crc32);
    ESP_LOGI(TAG, "  sha256=%02x%02x%02x%02x%02x%02x%02x%02x...%02x%02x%02x%02x",
             img->sha256[0], img->sha256[1], img->sha256[2], img->sha256[3],
             img->sha256[4], img->sha256[5], img->sha256[6], img->sha256[7],
             img->sha256[28], img->sha256[29], img->sha256[30], img->sha256[31]);
}

/* ---------------- 真实文件源 ---------------- */

typedef struct {
    FILE *f;
} file_ctx_t;

static esp_err_t file_read(ota_image_t *img, uint32_t offset, void *buf, uint32_t len)
{
    if (img == NULL || buf == NULL || img->ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    file_ctx_t *c = (file_ctx_t *)img->ctx;
    if (c->f == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (offset > img->size || len > img->size - offset) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(c->f, (long)(FW_PKG_HDR_LEN + offset), SEEK_SET) != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 短读 = 卡被拔了 / 文件被截断。返回错误让会话正常失败，而不是读出一片垃圾。 */
    if (fread(buf, 1, len, c->f) != len) {
        ESP_LOGW(TAG, "读文件失败（偏移 %u，%u 字节）", (unsigned)offset, (unsigned)len);
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static void file_release(ota_image_t *img)
{
    if (img == NULL || img->ctx == NULL) {
        return;
    }
    file_ctx_t *c = (file_ctx_t *)img->ctx;
    if (c->f != NULL) {
        fclose(c->f);
        c->f = NULL;
    }
    free(c);
    img->ctx     = NULL;
    img->read    = NULL;
    img->release = NULL;
}

esp_err_t ota_image_from_file(ota_image_t *img, const fw_pkg_hdr_t *h, const char *path)
{
    ESP_RETURN_ON_FALSE(img != NULL && h != NULL && path != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "参数为空");

    memset(img, 0, sizeof(*img));

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "打不开 %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    file_ctx_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    c->f = f;

    strncpy(img->model, h->model, DEV_MODEL_LEN);
    img->model[DEV_MODEL_LEN] = '\0';
    img->hw_min  = h->hw_min;
    img->hw_max  = h->hw_max;
    img->fw_ver  = h->fw_ver;
    img->size    = h->payload_len;
    img->crc32   = h->payload_crc32;
    memcpy(img->sha256, h->sha256, sizeof(img->sha256));

    const char *base = strrchr(path, '/');
    snprintf(img->name, sizeof(img->name), "%s", (base != NULL) ? base + 1 : path);

    img->read    = file_read;
    img->ctx     = c;
    img->release = file_release;

    ESP_LOGI(TAG, "镜像来自文件 '%s'  fw=%s  %u KB",
             img->name, ota_fw_ver_str(img->fw_ver), (unsigned)(img->size / 1024u));
    return ESP_OK;
}
