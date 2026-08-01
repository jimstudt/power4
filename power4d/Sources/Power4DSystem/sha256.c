#include "sha256.h"

#include <string.h>

#define ROTR32(value, bits) (((value) >> (bits)) | ((value) << (32 - (bits))))

static const uint32_t k_round_constants[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

static uint32_t read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) |
           ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) |
           (uint32_t)data[3];
}

static void sha256_compress(Sha256Ctx *ctx, const uint8_t block[64])
{
    uint32_t words[64];
    uint32_t a, b, c, d, e, f, g, h;
    size_t i;

    for (i = 0; i < 16; ++i) {
        words[i] = read_be32(block + (i * 4));
    }
    for (i = 16; i < 64; ++i) {
        const uint32_t s0 =
            ROTR32(words[i - 15], 7) ^ ROTR32(words[i - 15], 18) ^ (words[i - 15] >> 3);
        const uint32_t s1 =
            ROTR32(words[i - 2], 17) ^ ROTR32(words[i - 2], 19) ^ (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < 64; ++i) {
        const uint32_t sum1 = ROTR32(e, 6) ^ ROTR32(e, 11) ^ ROTR32(e, 25);
        const uint32_t choose = (e & f) ^ ((~e) & g);
        const uint32_t temp1 = h + sum1 + choose + k_round_constants[i] + words[i];
        const uint32_t sum0 = ROTR32(a, 2) ^ ROTR32(a, 13) ^ ROTR32(a, 22);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = sum0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void sha256_init(Sha256Ctx *ctx)
{
    static const uint32_t initial[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };

    memcpy(ctx->state, initial, sizeof(initial));
    ctx->bits = 0;
    ctx->buffer_used = 0;
}

void sha256_update(Sha256Ctx *ctx, const void *data, size_t length)
{
    const uint8_t *cursor = (const uint8_t *)data;

    ctx->bits += (uint64_t)length * 8;
    while (length > 0) {
        size_t take = sizeof(ctx->buffer) - ctx->buffer_used;
        if (take > length) {
            take = length;
        }
        memcpy(ctx->buffer + ctx->buffer_used, cursor, take);
        ctx->buffer_used += take;
        cursor += take;
        length -= take;
        if (ctx->buffer_used == sizeof(ctx->buffer)) {
            sha256_compress(ctx, ctx->buffer);
            ctx->buffer_used = 0;
        }
    }
}

void sha256_final(Sha256Ctx *ctx, uint8_t digest[32])
{
    const uint64_t bits = ctx->bits;
    size_t i;

    ctx->buffer[ctx->buffer_used++] = 0x80;
    if (ctx->buffer_used > 56) {
        while (ctx->buffer_used < sizeof(ctx->buffer)) {
            ctx->buffer[ctx->buffer_used++] = 0;
        }
        sha256_compress(ctx, ctx->buffer);
        ctx->buffer_used = 0;
    }
    while (ctx->buffer_used < 56) {
        ctx->buffer[ctx->buffer_used++] = 0;
    }
    for (i = 0; i < 8; ++i) {
        ctx->buffer[63 - i] = (uint8_t)(bits >> (i * 8));
    }
    sha256_compress(ctx, ctx->buffer);

    for (i = 0; i < 8; ++i) {
        digest[i * 4] = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)ctx->state[i];
    }
}

void hmac_sha256(const void *key,
                 size_t key_length,
                 const void *data,
                 size_t data_length,
                 uint8_t digest[32])
{
    uint8_t normalized_key[64] = {0};
    uint8_t inner_pad[64];
    uint8_t outer_pad[64];
    uint8_t inner_digest[32];
    Sha256Ctx ctx;
    size_t i;

    if (key_length > sizeof(normalized_key)) {
        sha256_init(&ctx);
        sha256_update(&ctx, key, key_length);
        sha256_final(&ctx, normalized_key);
    } else if (key_length > 0) {
        memcpy(normalized_key, key, key_length);
    }

    for (i = 0; i < sizeof(normalized_key); ++i) {
        inner_pad[i] = (uint8_t)(normalized_key[i] ^ 0x36U);
        outer_pad[i] = (uint8_t)(normalized_key[i] ^ 0x5cU);
    }

    sha256_init(&ctx);
    sha256_update(&ctx, inner_pad, sizeof(inner_pad));
    sha256_update(&ctx, data, data_length);
    sha256_final(&ctx, inner_digest);

    sha256_init(&ctx);
    sha256_update(&ctx, outer_pad, sizeof(outer_pad));
    sha256_update(&ctx, inner_digest, sizeof(inner_digest));
    sha256_final(&ctx, digest);

    memset(normalized_key, 0, sizeof(normalized_key));
    memset(inner_pad, 0, sizeof(inner_pad));
    memset(outer_pad, 0, sizeof(outer_pad));
    memset(inner_digest, 0, sizeof(inner_digest));
    memset(&ctx, 0, sizeof(ctx));
}
