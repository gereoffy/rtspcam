/* RTP H.264 depacketizer, see rtp_h264.h. */
#include <stdlib.h>
#include <string.h>

#include "bytebuf.h"
#include "log.h"
#include "rtp_h264.h"

struct rtp_h264 {
    int pt;
    rtp_au_cb cb;
    void *opaque;
    bytebuf au;
    int have_au;            /* au holds data for cur_ts */
    uint32_t cur_ts;
    int broken;
    int in_fu;              /* inside a FU-A whose start was received */
    int have_seq;
    uint16_t last_seq;
    int have_ts;
    uint32_t last_ts32;
    int64_t ts64;
    int warned_mode2;
    long seq_gaps;
};

long rtp_h264_seq_gaps(const rtp_h264 *d)
{
    return d ? d->seq_gaps : 0;
}

rtp_h264 *rtp_h264_create(int payload_type, rtp_au_cb cb, void *opaque)
{
    rtp_h264 *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->pt = payload_type;
    d->cb = cb;
    d->opaque = opaque;
    return d;
}

void rtp_h264_free(rtp_h264 *d)
{
    if (!d)
        return;
    bb_free(&d->au);
    free(d);
}

/* NAL units of the assembled data, found from the start codes (some cameras pack several NAL
 * units, with their start codes, into one FU-A whose header names only the first one) */
typedef struct nal_pos {
    size_t start;           /* offset of the start code */
    int type;
    int first_mb_zero;      /* VCL NAL: first_mb_in_slice == 0, i.e. it begins a new picture */
} nal_pos;

static int find_nals(const bytebuf *au, nal_pos *out, int max)
{
    size_t i = 0;
    int n = 0;
    while (i + 3 < au->len && n < max) {
        if (au->d[i] == 0 && au->d[i + 1] == 0 && au->d[i + 2] == 1) {
            size_t sc = i > 0 && au->d[i - 1] == 0 ? i - 1 : i;
            out[n].start = sc;
            out[n].type = au->d[i + 3] & 0x1F;
            out[n].first_mb_zero = i + 4 < au->len && (au->d[i + 4] & 0x80);   /* ue(v) == 0 is a single 1 bit */
            n++;
            i += 3;
        } else {
            i++;
        }
    }
    return n;
}

static int send_picture(rtp_h264 *d, const uint8_t *p, size_t len, int key)
{
    int32_t delta = (int32_t)(d->cur_ts - d->last_ts32);
    if (!d->have_ts) {
        d->have_ts = 1;
        d->ts64 = d->cur_ts;
    } else if (delta > 0) {
        d->ts64 += delta;
    } else {
        d->ts64 += 1;           /* same (or older) timestamp: keep the pictures strictly increasing */
    }
    d->last_ts32 = d->cur_ts;
    return d->cb(d->opaque, p, (int)len, d->ts64, key, d->broken || d->au.err);
}

/* Hands over the access unit(s): normally one picture, but a camera that falls behind may send
 * several pictures with one timestamp; they are split at the slices that start a new picture
 * (together with the parameter sets/SEI in front of them). Data without any slice (some cameras
 * send SPS/PPS with the marker bit set or with their own timestamp) is kept for the next picture. */
static int emit(rtp_h264 *d)
{
    nal_pos nals[256];
    int n, i, r = 0, start = 0, have_vcl = 0, key = 0, cut_from = -1;
    if (!d->have_au || !d->au.len)
        return 0;
    n = find_nals(&d->au, nals, 256);
    for (i = 0; i < n; i++)
        if (nals[i].type >= 1 && nals[i].type <= 5)
            break;
    if (i == n) {
        if (d->au.len > (1 << 20)) {            /* no slice for a long time: drop the junk */
            d->au.len = 0;
            d->have_au = 0;
        }
        d->in_fu = 0;
        return 0;
    }
    for (i = 0; i < n && r >= 0; i++) {
        int t = nals[i].type, vcl = t >= 1 && t <= 5;
        if (!vcl) {
            if (have_vcl && cut_from < 0 && (t == 6 || t == 7 || t == 8 || t == 9))
                cut_from = i;                   /* may begin the next picture */
            continue;
        }
        if (have_vcl && nals[i].first_mb_zero) {
            int cut = cut_from >= 0 ? cut_from : i;
            r = send_picture(d, d->au.d + nals[start].start, nals[cut].start - nals[start].start, key);
            start = cut;
            key = 0;
        }
        cut_from = -1;
        have_vcl = 1;
        if (t == 5)
            key = 1;
    }
    if (r >= 0)
        r = send_picture(d, d->au.d + nals[start].start, d->au.len - nals[start].start, key);
    d->au.len = 0;
    d->au.err = 0;
    d->have_au = 0;
    d->broken = 0;
    d->in_fu = 0;
    return r;
}

static void add_nal(rtp_h264 *d, const uint8_t *nal, int len)
{
    static const uint8_t sc[4] = { 0, 0, 0, 1 };
    if (len <= 0)
        return;
    bb_put(&d->au, sc, 4);
    bb_put(&d->au, nal, len);
}

int rtp_h264_push(rtp_h264 *d, const uint8_t *p, int len)
{
    int cc, x, marker, pt, off, r = 0, type;
    uint16_t seq;
    uint32_t ts;

    if (len < 12 || (p[0] >> 6) != 2)
        return 0;
    if (p[0] & 0x20) {                          /* padding */
        int pad = p[len - 1];
        if (pad >= len - 12)
            return 0;
        len -= pad;
    }
    cc = p[0] & 0x0F;
    x = (p[0] >> 4) & 1;
    marker = p[1] >> 7;
    pt = p[1] & 0x7F;
    seq = (p[2] << 8) | p[3];
    ts = ((uint32_t)p[4] << 24) | (p[5] << 16) | (p[6] << 8) | p[7];
    if (pt != d->pt)
        return 0;
    off = 12 + 4 * cc;
    if (x) {
        if (off + 4 > len)
            return 0;
        off += 4 + 4 * ((p[off + 2] << 8) | p[off + 3]);
    }
    if (off >= len)
        return 0;

    if (d->have_au && ts != d->cur_ts) {
        if ((r = emit(d)) < 0)
            return r;
        d->in_fu = 0;                           /* a FU-A cannot continue across pictures */
    }
    if (d->have_seq && seq != (uint16_t)(d->last_seq + 1)) {
        d->seq_gaps++;
        d->broken = 1;                          /* lost packets */
        d->in_fu = 0;
    }
    d->have_seq = 1;
    d->last_seq = seq;
    d->cur_ts = ts;
    d->have_au = 1;

    p += off;
    len -= off;
    type = p[0] & 0x1F;
    if (type >= 1 && type <= 23) {
        add_nal(d, p, len);
    } else if (type == 24) {                    /* STAP-A */
        int i = 1;
        while (i + 2 <= len) {
            int n = (p[i] << 8) | p[i + 1];
            i += 2;
            if (i + n > len) {
                d->broken = 1;
                break;
            }
            add_nal(d, p + i, n);
            i += n;
        }
    } else if (type == 28) {                    /* FU-A */
        if (len < 2) {
            d->broken = 1;
        } else {
            int start = p[1] >> 7, end = (p[1] >> 6) & 1;
            if (start) {
                uint8_t hdr = (p[0] & 0xE0) | (p[1] & 0x1F);
                add_nal(d, &hdr, 1);
                bb_put(&d->au, p + 2, len - 2);
                d->in_fu = !end;
            } else if (d->in_fu) {
                bb_put(&d->au, p + 2, len - 2);
                if (end)
                    d->in_fu = 0;
            } else {
                d->broken = 1;                  /* fragment without its start */
            }
        }
    } else {
        if (!d->warned_mode2) {
            log_warn("RTP H.264 packet type %d (STAP-B/MTAP/FU-B) is not supported", type);
            d->warned_mode2 = 1;
        }
        d->broken = 1;
    }
    if (marker)
        r = emit(d);
    return r;
}
