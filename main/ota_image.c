/**
 * @file  ota_image.c
 * @brief 合成镜像实现：字节由绝对偏移确定性生成，可随机访问、可复现
 */
#include <stdio.h>
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
