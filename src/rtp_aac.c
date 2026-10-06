/* RTP AAC depacketizer, RFC 3640 (see rtp_aac.h) */
#include <stdlib.h>
#include <string.h>

#include "bytebuf.h"
#include "rtp_aac.h"

struct rtp_aac {
    int size_len, index_len, index_delta_len, frame_len;
    rtp_aac_cb cb;
    void *opaque;
    int have_ts;
    uint32_t last_ts32;
    int64_t ts64;
    /* an AU spread over several packets */
    bytebuf frag;
    size_t frag_size;       /* its full size, 0 = none pending */
    uint32_t frag_ts32;
};

rtp_aac *rtp_aac_create(int size_length, int index_length, int index_delta_length, int frame_len,
                        rtp_aac_cb cb, void *opaque)
{
    rtp_aac *d;
    if (size_length <= 0 || size_length > 16 || index_length < 0 || index_length > 16 ||
        index_delta_length < 0 || index_delta_length > 16)
        return NULL;
    d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->size_len = size_length;
    d->index_len = index_length;
    d->index_delta_len = index_delta_length;
    d->frame_len = frame_len > 0 ? frame_len : 1024;
    d->cb = cb;
    d->opaque = opaque;
    return d;
}

static unsigned get_bits(const uint8_t *p, size_t bitpos, int n)
{
    unsigned v = 0;
    while (n-- > 0) {
        v = (v << 1) | ((p[bitpos >> 3] >> (7 - (bitpos & 7))) & 1);
        bitpos++;
    }
    return v;
}

static int64_t unwrap(rtp_aac *d, uint32_t ts32)
{
    if (!d->have_ts) {
        d->have_ts = 1;
        d->ts64 = ts32;
    } else {
        d->ts64 += (int32_t)(ts32 - d->last_ts32);
    }
    d->last_ts32 = ts32;
    return d->ts64;
}

int rtp_aac_push(rtp_aac *d, const uint8_t *pkt, int len)
{
    const uint8_t *pl;
    int plen, cc, hdr_bits, hdr_bytes, n_au, i, r;
    uint32_t ts32;
    size_t bit, off;
    unsigned sizes[64];

    if (len < 12 || (pkt[0] >> 6) != 2)
        return 0;
    cc = pkt[0] & 0x0F;
    ts32 = ((uint32_t)pkt[4] << 24) | (pkt[5] << 16) | (pkt[6] << 8) | pkt[7];
    pl = pkt + 12 + 4 * cc;
    plen = len - 12 - 4 * cc;
    if (plen < 0)
        return 0;
    if (pkt[0] & 0x10) {                        /* header extension */
        int ext;
        if (plen < 4)
            return 0;
        ext = 4 + 4 * ((pl[2] << 8) | pl[3]);
        if (ext > plen)
            return 0;
        pl += ext;
        plen -= ext;
    }
    if ((pkt[0] & 0x20) && plen > 0) {          /* padding */
        int pad = pl[plen - 1];
        if (pad > plen)
            return 0;
        plen -= pad;
    }
    if (plen < 2)
        return 0;

    hdr_bits = (pl[0] << 8) | pl[1];
    hdr_bytes = (hdr_bits + 7) / 8;
    if (2 + hdr_bytes > plen)
        return 0;
    /* AU headers: size, then index (first) or index delta (the others) */
    n_au = 0;
    for (bit = 0; bit + d->size_len <= (size_t)hdr_bits && n_au < 64;) {
        sizes[n_au++] = get_bits(pl + 2, bit, d->size_len);
        bit += d->size_len + (n_au == 1 ? d->index_len : d->index_delta_len);
    }
    off = 2 + hdr_bytes;

    /* the continuation of a fragmented AU: same timestamp, one AU header with the full size */
    if (d->frag_size) {
        if (n_au == 1 && ts32 == d->frag_ts32 && sizes[0] == d->frag_size) {
            bb_put(&d->frag, pl + off, plen - off);
            if (d->frag.err)
                d->frag_size = 0;
            else if (d->frag.len >= d->frag_size) {
                d->frag_size = 0;
                return d->cb(d->opaque, d->frag.d, (int)d->frag.len, unwrap(d, ts32));
            }
            return 0;
        }
        d->frag_size = 0;                       /* the rest was lost: drop it */
    }
    if (n_au == 1 && off + sizes[0] > (size_t)plen) {   /* the first part of a fragmented AU */
        d->frag.len = 0;
        d->frag.err = 0;
        bb_put(&d->frag, pl + off, plen - off);
        d->frag_size = d->frag.err ? 0 : sizes[0];
        d->frag_ts32 = ts32;
        return 0;
    }
    for (i = 0; i < n_au; i++) {
        if (off + sizes[i] > (size_t)plen)
            break;                              /* damaged packet */
        if (sizes[i]) {
            r = d->cb(d->opaque, pl + off, (int)sizes[i], unwrap(d, ts32 + (uint32_t)(i * d->frame_len)));
            if (r < 0)
                return r;
        }
        off += sizes[i];
    }
    return 0;
}

void rtp_aac_free(rtp_aac *d)
{
    if (!d)
        return;
    bb_free(&d->frag);
    free(d);
}
