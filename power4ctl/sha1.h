#ifndef SHA1_H
#define SHA1_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[5];
    uint64_t bits;
    uint8_t  buf[64];
    size_t   buf_used;
} Sha1Ctx;

void sha1_init(Sha1Ctx *ctx);
void sha1_update(Sha1Ctx *ctx, const void *data, size_t len);
void sha1_final(Sha1Ctx *ctx, uint8_t digest[20]);

/* Compute lowercase hex SHA-1 of data into hex[41]. */
void sha1_hex_of(const void *data, size_t len, char hex[41]);

#endif /* SHA1_H */
