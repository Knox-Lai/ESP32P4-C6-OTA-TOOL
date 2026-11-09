/**
 * @file  ota_sha256.h
 * @brief 纯软件 SHA-256（整包校验用，见 ota-protocol.md §5.1）
 *
 * 为什么不用 mbedtls：
 *   IDF 6.x 内置的是 mbedtls 4.1，`mbedtls/sha256.h` 已随 PSA 迁移删除，
 *   只剩 `psa/crypto.h` 一套；为了一个"算一次整包摘要"的需求去引入 PSA 初始化，
 *   既增加 MINIMAL_BUILD 的依赖面，也让代码在不同 IDF 版本上更容易编不过。
 *   这里只有 100 行，且与 `hashlib.sha256()` 逐字节一致（已用主机侧测试对拍）。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t h[8];
    uint64_t total;         /* 已喂入的字节数 */
    uint8_t  buf[64];
    size_t   buflen;
} ota_sha256_ctx_t;

void ota_sha256_init(ota_sha256_ctx_t *ctx);
void ota_sha256_update(ota_sha256_ctx_t *ctx, const void *data, size_t len);
void ota_sha256_final(ota_sha256_ctx_t *ctx, uint8_t out[32]);

/** @brief 一次性算完（内部 64 B 分块，不额外占大缓冲） */
void ota_sha256(const void *data, size_t len, uint8_t out[32]);

#ifdef __cplusplus
}
#endif
