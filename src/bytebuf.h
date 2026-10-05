/* Growable byte buffer with big-endian writers and MP4 box helpers. */
#ifndef RC_BYTEBUF_H
#define RC_BYTEBUF_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct bytebuf {
    uint8_t *d;
    size_t len, cap;
    int err;
} bytebuf;

static inline int bb_reserve(bytebuf *b, size_t n)
{
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        uint8_t *nd;
        while (cap < b->len + n)
            cap *= 2;
        nd = realloc(b->d, cap);
        if (!nd) {
            b->err = 1;
            return -1;
        }
        b->d = nd;
        b->cap = cap;
    }
    return 0;
}

static inline void bb_put(bytebuf *b, const void *p, size_t n)
{
    if (bb_reserve(b, n) == 0) {
        memcpy(b->d + b->len, p, n);
        b->len += n;
    }
}

static inline void bb_zero(bytebuf *b, size_t n)
{
    if (bb_reserve(b, n) == 0) {
        memset(b->d + b->len, 0, n);
        b->len += n;
    }
}

static inline void bb_u8(bytebuf *b, unsigned v)
{
    uint8_t x = v;
    bb_put(b, &x, 1);
}

static inline void bb_u16(bytebuf *b, unsigned v)
{
    uint8_t x[2] = { v >> 8, v };
    bb_put(b, x, 2);
}

static inline void bb_u24(bytebuf *b, unsigned v)
{
    uint8_t x[3] = { v >> 16, v >> 8, v };
    bb_put(b, x, 3);
}

static inline void bb_u32(bytebuf *b, uint32_t v)
{
    uint8_t x[4] = { v >> 24, v >> 16, v >> 8, v };
    bb_put(b, x, 4);
}

static inline void bb_u64(bytebuf *b, uint64_t v)
{
    bb_u32(b, (uint32_t)(v >> 32));
    bb_u32(b, (uint32_t)v);
}

static inline void bb_set_u32(bytebuf *b, size_t off, uint32_t v)
{
    if (off + 4 <= b->len) {
        b->d[off] = v >> 24;
        b->d[off + 1] = v >> 16;
        b->d[off + 2] = v >> 8;
        b->d[off + 3] = v;
    }
}

/* start a box: returns the offset of its size field, patched by bb_box_end() */
static inline size_t bb_box(bytebuf *b, const char *type)
{
    size_t off = b->len;
    bb_u32(b, 0);
    bb_put(b, type, 4);
    return off;
}

static inline size_t bb_fullbox(bytebuf *b, const char *type, int version, unsigned flags)
{
    size_t off = bb_box(b, type);
    bb_u8(b, version);
    bb_u24(b, flags);
    return off;
}

static inline void bb_box_end(bytebuf *b, size_t off)
{
    bb_set_u32(b, off, (uint32_t)(b->len - off));
}

static inline void bb_free(bytebuf *b)
{
    free(b->d);
    memset(b, 0, sizeof(*b));
}

#endif
