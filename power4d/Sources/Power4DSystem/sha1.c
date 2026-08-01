/* SHA-1 (FIPS 180-4) */

#include "sha1.h"

#include <stdio.h>
#include <string.h>

#define ROL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

static void sha1_compress(Sha1Ctx *ctx, const uint8_t block[64])
{
    uint32_t w[80], a, b, c, d, e;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i*4+0] << 24) |
               ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] <<  8) |
               ((uint32_t)block[i*4+3]);
    }
    for (i = 16; i < 80; i++)
        w[i] = ROL32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    a = ctx->h[0]; b = ctx->h[1]; c = ctx->h[2];
    d = ctx->h[3]; e = ctx->h[4];

    for (i = 0; i < 80; i++) {
        uint32_t f, k, tmp;
        if      (i < 20) { f = (b & c) | (~b & d);           k = 0x5a827999u; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ed9eba1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdcu; }
        else             { f = b ^ c ^ d;                    k = 0xca62c1d6u; }
        tmp = ROL32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL32(b, 30); b = a; a = tmp;
    }

    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c;
    ctx->h[3] += d; ctx->h[4] += e;
}

void sha1_init(Sha1Ctx *ctx)
{
    ctx->h[0] = 0x67452301u;
    ctx->h[1] = 0xefcdab89u;
    ctx->h[2] = 0x98badcfeu;
    ctx->h[3] = 0x10325476u;
    ctx->h[4] = 0xc3d2e1f0u;
    ctx->bits = 0;
    ctx->buf_used = 0;
}

void sha1_update(Sha1Ctx *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    ctx->bits += (uint64_t)len * 8;
    while (len > 0) {
        size_t take = 64 - ctx->buf_used;
        if (take > len) take = len;
        memcpy(ctx->buf + ctx->buf_used, p, take);
        ctx->buf_used += take;
        p   += take;
        len -= take;
        if (ctx->buf_used == 64) {
            sha1_compress(ctx, ctx->buf);
            ctx->buf_used = 0;
        }
    }
}

void sha1_final(Sha1Ctx *ctx, uint8_t digest[20])
{
    uint64_t bits = ctx->bits;
    size_t i;

    ctx->buf[ctx->buf_used++] = 0x80;
    if (ctx->buf_used > 56) {
        while (ctx->buf_used < 64) ctx->buf[ctx->buf_used++] = 0;
        sha1_compress(ctx, ctx->buf);
        ctx->buf_used = 0;
    }
    while (ctx->buf_used < 56) ctx->buf[ctx->buf_used++] = 0;
    for (i = 0; i < 8; i++)
        ctx->buf[63 - i] = (uint8_t)(bits >> (i * 8));
    sha1_compress(ctx, ctx->buf);

    for (i = 0; i < 5; i++) {
        digest[i*4+0] = (ctx->h[i] >> 24) & 0xff;
        digest[i*4+1] = (ctx->h[i] >> 16) & 0xff;
        digest[i*4+2] = (ctx->h[i] >>  8) & 0xff;
        digest[i*4+3] =  ctx->h[i]        & 0xff;
    }
}

void sha1_hex_of(const void *data, size_t len, char hex[41])
{
    Sha1Ctx ctx;
    uint8_t digest[20];
    size_t i;

    sha1_init(&ctx);
    sha1_update(&ctx, data, len);
    sha1_final(&ctx, digest);
    for (i = 0; i < 20; i++)
        snprintf(hex + i*2, 3, "%02x", digest[i]);
    hex[40] = '\0';
}
