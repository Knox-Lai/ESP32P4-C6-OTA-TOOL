/**
 * @file  ota_sha256.c
 * @brief FIPS 180-4 SHA-256 实现（无表、无动态分配，栈占用约 300 B）
 */
#include <string.h>

#include "ota_sha256.h"

static const uint32_t K[64] = {
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u, 0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u,
    0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u, 0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u,
    0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu, 0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
    0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u, 0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u,
    0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u, 0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
    0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u, 0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
    0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u, 0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
    0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u, 0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u,
};

#define ROTR(x, n)  (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x)    (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define BSIG1(x)    (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SSIG0(x)    (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SSIG1(x)    (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static void sha256_block(ota_sha256_ctx_t *ctx, const uint8_t *p)
{
    uint32_t w[64];

    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        w[i] = SSIG1(w[i - 2]) + w[i - 7] + SSIG0(w[i - 15]) + w[i - 16];
    }

    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3];
    uint32_t e = ctx->h[4], f = ctx->h[5], g = ctx->h[6], hh = ctx->h[7];

    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + BSIG1(e) + CH(e, f, g) + K[i] + w[i];
        uint32_t t2 = BSIG0(a) + MAJ(a, b, c);
        hh = g;
        g  = f;
        f  = e;
        e  = d + t1;
        d  = c;
        c  = b;
        b  = a;
        a  = t1 + t2;
    }

    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
    ctx->h[4] += e;
    ctx->h[5] += f;
    ctx->h[6] += g;
    ctx->h[7] += hh;
}

void ota_sha256_init(ota_sha256_ctx_t *ctx)
{
    ctx->h[0] = 0x6A09E667u;
    ctx->h[1] = 0xBB67AE85u;
    ctx->h[2] = 0x3C6EF372u;
    ctx->h[3] = 0xA54FF53Au;
    ctx->h[4] = 0x510E527Fu;
    ctx->h[5] = 0x9B05688Cu;
    ctx->h[6] = 0x1F83D9ABu;
    ctx->h[7] = 0x5BE0CD19u;
    ctx->total  = 0;
    ctx->buflen = 0;
}

void ota_sha256_update(ota_sha256_ctx_t *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    ctx->total += len;

    if (ctx->buflen > 0) {
        size_t need = 64 - ctx->buflen;
        size_t take = (len < need) ? len : need;
        memcpy(ctx->buf + ctx->buflen, p, take);
        ctx->buflen += take;
        p   += take;
        len -= take;
        if (ctx->buflen < 64) {
            return;
        }
        sha256_block(ctx, ctx->buf);
        ctx->buflen = 0;
    }

    while (len >= 64) {
        sha256_block(ctx, p);
        p   += 64;
        len -= 64;
    }

    if (len > 0) {
        memcpy(ctx->buf, p, len);
        ctx->buflen = len;
    }
}

void ota_sha256_final(ota_sha256_ctx_t *ctx, uint8_t out[32])
{
    uint64_t bits = ctx->total * 8u;

    /* 0x80 + 补零，最后 8 字节放大端比特数 */
    uint8_t pad[128];
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    size_t padlen = (ctx->buflen < 56) ? (56 - ctx->buflen) : (120 - ctx->buflen);

    /* update 会累加 total，但 final 之后 total 不再使用，先修正回原值 */
    uint64_t saved = ctx->total;
    ota_sha256_update(ctx, pad, padlen);
    ctx->total = saved;

    uint8_t lenbuf[8];
    for (int i = 0; i < 8; i++) {
        lenbuf[i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    ota_sha256_update(ctx, lenbuf, 8);
    ctx->total = saved;

    for (int i = 0; i < 8; i++) {
        out[i * 4 + 0] = (uint8_t)(ctx->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->h[i]);
    }
}

void ota_sha256(const void *data, size_t len, uint8_t out[32])
{
    ota_sha256_ctx_t ctx;
    ota_sha256_init(&ctx);
    ota_sha256_update(&ctx, data, len);
    ota_sha256_final(&ctx, out);
}
