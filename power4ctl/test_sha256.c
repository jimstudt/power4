#include "sha256.h"

#include <stdio.h>
#include <string.h>

static void to_hex(const uint8_t *digest, size_t length, char *hex)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < length; ++i) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 0x0f];
    }
    hex[length * 2] = '\0';
}

int main(void)
{
    static const char sha_expected[] =
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad";
    static const char hmac_expected[] =
        "b0344c61d8db38535ca8afceaf0bf12b"
        "881dc200c9833da726e9376c2e32cff7";
    uint8_t digest[32];
    char hex[65];
    Sha256Ctx ctx;
    uint8_t key[20];

    sha256_init(&ctx);
    sha256_update(&ctx, "abc", 3);
    sha256_final(&ctx, digest);
    to_hex(digest, sizeof(digest), hex);
    if (strcmp(hex, sha_expected) != 0) {
        fprintf(stderr, "SHA-256 vector failed: %s\n", hex);
        return 1;
    }

    memset(key, 0x0b, sizeof(key));
    hmac_sha256(key, sizeof(key), "Hi There", 8, digest);
    to_hex(digest, sizeof(digest), hex);
    if (strcmp(hex, hmac_expected) != 0) {
        fprintf(stderr, "HMAC-SHA256 vector failed: %s\n", hex);
        return 1;
    }

    puts("SHA-256 and HMAC-SHA256 vectors: ok");
    return 0;
}
