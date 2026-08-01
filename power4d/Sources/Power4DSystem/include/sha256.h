#ifndef POWER4D_SHA256_H
#define POWER4D_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t bits;
    uint8_t buffer[64];
    size_t buffer_used;
} Sha256Ctx;

void sha256_init(Sha256Ctx *ctx);
void sha256_update(Sha256Ctx *ctx, const void *data, size_t length);
void sha256_final(Sha256Ctx *ctx, uint8_t digest[32]);
void hmac_sha256(const void *key,
                 size_t key_length,
                 const void *data,
                 size_t data_length,
                 uint8_t digest[32]);

#endif
