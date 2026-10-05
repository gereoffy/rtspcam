/* MD5 (RFC 1321), for RTSP Digest authentication. */
#ifndef RC_MD5_H
#define RC_MD5_H

#include <stddef.h>
#include <stdint.h>

typedef struct md5_ctx {
    uint32_t h[4];
    uint64_t len;
    uint8_t buf[64];
    size_t n;
} md5_ctx;

void md5_init(md5_ctx *c);
void md5_update(md5_ctx *c, const void *data, size_t len);
void md5_final(md5_ctx *c, uint8_t out[16]);
/* lowercase hex digest of a NUL terminated string */
void md5_hex(const char *s, char out[33]);

#endif
