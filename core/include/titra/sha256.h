/* SHA-256 (FIPS 180-4), for checking the model's weights against their published digest at boot. */
#ifndef TITRA_SHA256_H
#define TITRA_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t h[8];
    uint64_t total;
    unsigned char block[64];
    size_t fill;
} titra_sha256_t;

void titra_sha256_init(titra_sha256_t *s);
void titra_sha256_update(titra_sha256_t *s, const void *data, size_t n);
/* Writes 64 lower-case hex digits and a NUL into hex. */
void titra_sha256_final_hex(titra_sha256_t *s, char hex[65]);

#ifdef __cplusplus
}
#endif

#endif
