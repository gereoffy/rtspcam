/* Minimal MP4/MOV reader (ISO/IEC 14496-12): the sample index of the first H.264 video track. */
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp4_reader.h"

typedef struct span {
    const uint8_t *p;
    uint64_t n;
} span;

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static uint64_t be64(const uint8_t *p)
{
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

/* next box of a buffer: type and body; returns 0 at the end or on a broken box */
static int next_box(span *s, char type[5], span *body)
{
    uint64_t size, hdr = 8;
    if (s->n < 8)
        return 0;
    size = be32(s->p);
    memcpy(type, s->p + 4, 4);
    type[4] = 0;
    if (size == 1) {
        if (s->n < 16)
            return 0;
        size = be64(s->p + 8);
        hdr = 16;
    } else if (size == 0) {
        size = s->n;
    }
    if (size < hdr || size > s->n)
        return 0;
    body->p = s->p + hdr;
    body->n = size - hdr;
    s->p += size;
    s->n -= size;
    return 1;
}

static int find_box(span s, const char *type, span *body)
{
    char t[5];
    span b;
    while (next_box(&s, t, &b))
        if (!memcmp(t, type, 4)) {
            *body = b;
            return 1;
        }
    return 0;
}

/* path like "mdia/minf/stbl" */
static int find_path(span s, const char *path, span *body)
{
    char t[5];
    while (*path) {
        memcpy(t, path, 4);
        t[4] = 0;
        if (!find_box(s, t, &s))
            return 0;
        path += 4;
        if (*path == '/')
            path++;
    }
    *body = s;
    return 1;
}

typedef struct track {
    uint32_t id;
    uint32_t def_duration, def_size, def_flags;    /* trex */
    int64_t next_dts;
} track;

static int add_sample(mp4_file *m, int *cap, uint64_t off, uint32_t size, int64_t dts, int64_t pts, int key)
{
    if (m->n_samples == *cap) {
        int nc = *cap ? 2 * *cap : 1024;
        mp4_sample *n = realloc(m->samples, sizeof(*n) * nc);
        if (!n)
            return -1;
        m->samples = n;
        *cap = nc;
    }
    m->samples[m->n_samples].offset = off;
    m->samples[m->n_samples].size = size;
    m->samples[m->n_samples].dts = dts;
    m->samples[m->n_samples].pts = pts;
    m->samples[m->n_samples].key = key;
    m->n_samples++;
    return 0;
}

/* stsd entry avc1/avc3: size and avcC */
static int parse_stsd(mp4_file *m, span stsd)
{
    span s, b, e;
    char t[5];
    if (stsd.n < 8)
        return -1;
    s.p = stsd.p + 8;                           /* version/flags, entry count */
    s.n = stsd.n - 8;
    if (!next_box(&s, t, &e))
        return -1;
    if (strcmp(t, "avc1") && strcmp(t, "avc3"))
        return -1;
    if (e.n < 78)
        return -1;
    m->width = (e.p[24] << 8) | e.p[25];
    m->height = (e.p[26] << 8) | e.p[27];
    s.p = e.p + 78;
    s.n = e.n - 78;
    if (find_box(s, "avcC", &b) && b.n >= 7) {
        m->avcc = malloc(b.n);
        if (!m->avcc)
            return -1;
        memcpy(m->avcc, b.p, b.n);
        m->avcc_size = (int)b.n;
        m->nal_length_size = (b.p[4] & 3) + 1;
    }
    return 0;
}

/* classic sample tables */
static int parse_stbl(mp4_file *m, int *cap, span stbl, track *tr)
{
    span stsz, stco, stsc, stts, ctts, stss;
    int has_ctts, has_stss, co64 = 0;
    uint32_t n_samples, fixed_size, n_chunks, n_stsc, n_stts, n_ctts = 0, n_stss = 0;
    uint32_t chunk, k, sample = 0, stsc_i = 0, stts_i = 0, stts_left, ctts_i = 0, ctts_left = 0, stss_i = 0;
    int64_t dts = 0;
    int ctts_signed = 0;

    if (!find_box(stbl, "stsz", &stsz) || stsz.n < 12)
        return 0;                               /* no samples in moov (fragmented file) */
    fixed_size = be32(stsz.p + 4);
    n_samples = be32(stsz.p + 8);
    if (n_samples == 0)
        return 0;
    if (!fixed_size && stsz.n < 12 + 4ULL * n_samples)
        return -1;
    if (!find_box(stbl, "stco", &stco)) {
        if (!find_box(stbl, "co64", &stco))
            return -1;
        co64 = 1;
    }
    if (!find_box(stbl, "stsc", &stsc) || !find_box(stbl, "stts", &stts) || stco.n < 8 || stsc.n < 8 || stts.n < 8)
        return -1;
    n_chunks = be32(stco.p + 4);
    n_stsc = be32(stsc.p + 4);
    n_stts = be32(stts.p + 4);
    if (stco.n < 8 + (uint64_t)n_chunks * (co64 ? 8 : 4) || stsc.n < 8 + 12ULL * n_stsc || stts.n < 8 + 8ULL * n_stts ||
        n_stsc == 0 || n_stts == 0)
        return -1;
    has_ctts = find_box(stbl, "ctts", &ctts) && ctts.n >= 8;
    if (has_ctts) {
        ctts_signed = ctts.p[0] == 1;
        n_ctts = be32(ctts.p + 4);
        if (ctts.n < 8 + 8ULL * n_ctts)
            has_ctts = 0;
    }
    has_stss = find_box(stbl, "stss", &stss) && stss.n >= 8;
    if (has_stss) {
        n_stss = be32(stss.p + 4);
        if (stss.n < 8 + 4ULL * n_stss)
            has_stss = 0;
    }
    stts_left = be32(stts.p + 8);

    for (chunk = 1; chunk <= n_chunks && sample < n_samples; chunk++) {
        uint64_t off = co64 ? be64(stco.p + 8 + 8ULL * (chunk - 1)) : be32(stco.p + 8 + 4ULL * (chunk - 1));
        uint32_t per;
        while (stsc_i + 1 < n_stsc && be32(stsc.p + 8 + 12ULL * (stsc_i + 1)) <= chunk)
            stsc_i++;
        per = be32(stsc.p + 8 + 12ULL * stsc_i + 4);
        for (k = 0; k < per && sample < n_samples; k++, sample++) {
            uint32_t size = fixed_size ? fixed_size : be32(stsz.p + 12 + 4ULL * sample);
            int64_t cto = 0;
            int key = 1;
            while (stts_left == 0 && stts_i + 1 < n_stts) {
                stts_i++;
                stts_left = be32(stts.p + 8 + 8ULL * stts_i);
            }
            if (has_ctts) {
                while (ctts_left == 0 && ctts_i < n_ctts) {
                    ctts_left = be32(ctts.p + 8 + 8ULL * ctts_i);
                    ctts_i++;
                }
                if (ctts_left) {
                    uint32_t v = be32(ctts.p + 8 + 8ULL * (ctts_i - 1) + 4);
                    cto = ctts_signed ? (int32_t)v : (int64_t)v;
                    ctts_left--;
                }
            }
            if (has_stss) {
                while (stss_i < n_stss && be32(stss.p + 8 + 4ULL * stss_i) < sample + 1)
                    stss_i++;
                key = stss_i < n_stss && be32(stss.p + 8 + 4ULL * stss_i) == sample + 1;
            }
            if (add_sample(m, cap, off, size, dts, dts + cto, key) < 0)
                return -1;
            off += size;
            dts += be32(stts.p + 8 + 8ULL * stts_i + 4);
            if (stts_left)
                stts_left--;
        }
    }
    tr->next_dts = dts;
    return 0;
}

static int parse_moov(mp4_file *m, int *cap, span moov, track *tr)
{
    span s = moov, trak, b;
    char t[5];
    while (next_box(&s, t, &trak)) {
        span hdlr, mdhd, stsd, stbl, tkhd;
        if (strcmp(t, "trak"))
            continue;
        if (!find_path(trak, "mdia/hdlr", &hdlr) || hdlr.n < 12 || memcmp(hdlr.p + 8, "vide", 4))
            continue;
        if (!find_path(trak, "mdia/minf/stbl/stsd", &stsd) || parse_stsd(m, stsd) < 0)
            continue;
        if (!find_path(trak, "mdia/mdhd", &mdhd) || mdhd.n < 24)
            return -1;
        m->timescale = mdhd.p[0] == 1 ? be32(mdhd.p + 20) : be32(mdhd.p + 12);
        if (find_box(trak, "tkhd", &tkhd) && tkhd.n >= 20)
            tr->id = tkhd.p[0] == 1 ? be32(tkhd.p + 20) : be32(tkhd.p + 12);
        if (find_path(trak, "mdia/minf/stbl", &stbl) && parse_stbl(m, cap, stbl, tr) < 0)
            return -1;
        /* fragment defaults */
        if (find_box(moov, "mvex", &b)) {
            span ms = b, tx;
            while (next_box(&ms, t, &tx))
                if (!strcmp(t, "trex") && tx.n >= 24 && be32(tx.p + 4) == tr->id) {
                    tr->def_duration = be32(tx.p + 12);
                    tr->def_size = be32(tx.p + 16);
                    tr->def_flags = be32(tx.p + 20);
                }
        }
        return 1;
    }
    return 0;
}

static int parse_moof(mp4_file *m, int *cap, span moof, uint64_t moof_off, track *tr)
{
    span s = moof, traf;
    char t[5];
    while (next_box(&s, t, &traf)) {
        span ts = traf, b, tfhd;
        uint32_t flags, def_dur = tr->def_duration, def_size = tr->def_size, def_flags = tr->def_flags;
        uint64_t base = moof_off, next_off;
        if (strcmp(t, "traf") || !find_box(traf, "tfhd", &tfhd) || tfhd.n < 8)
            continue;
        if (be32(tfhd.p + 4) != tr->id)
            continue;
        flags = be32(tfhd.p) & 0xFFFFFF;
        {
            const uint8_t *p = tfhd.p + 8, *end = tfhd.p + tfhd.n;
            if ((flags & 0x1) && p + 8 <= end) { base = be64(p); p += 8; }
            if ((flags & 0x2) && p + 4 <= end) p += 4;
            if ((flags & 0x8) && p + 4 <= end) { def_dur = be32(p); p += 4; }
            if ((flags & 0x10) && p + 4 <= end) { def_size = be32(p); p += 4; }
            if ((flags & 0x20) && p + 4 <= end) { def_flags = be32(p); p += 4; }
        }
        if (find_box(traf, "tfdt", &b) && b.n >= 8)
            tr->next_dts = b.p[0] == 1 && b.n >= 12 ? (int64_t)be64(b.p + 4) : (int64_t)be32(b.p + 4);
        next_off = base;
        while (next_box(&ts, t, &b)) {
            uint32_t tf, n, i;
            const uint8_t *p, *end;
            uint32_t first_flags = 0;
            int have_first = 0;
            uint64_t off;
            if (strcmp(t, "trun") || b.n < 8)
                continue;
            tf = be32(b.p) & 0xFFFFFF;
            n = be32(b.p + 4);
            p = b.p + 8;
            end = b.p + b.n;
            off = next_off;
            if ((tf & 0x1) && p + 4 <= end) { off = base + (int32_t)be32(p); p += 4; }
            if ((tf & 0x4) && p + 4 <= end) { first_flags = be32(p); have_first = 1; p += 4; }
            for (i = 0; i < n; i++) {
                uint32_t dur = def_dur, size = def_size, sf = def_flags;
                int64_t cto = 0;
                if (tf & 0x100) { if (p + 4 > end) return -1; dur = be32(p); p += 4; }
                if (tf & 0x200) { if (p + 4 > end) return -1; size = be32(p); p += 4; }
                if (tf & 0x400) { if (p + 4 > end) return -1; sf = be32(p); p += 4; }
                if (tf & 0x800) { if (p + 4 > end) return -1; cto = (int32_t)be32(p); p += 4; }
                if (i == 0 && have_first)
                    sf = first_flags;
                if (add_sample(m, cap, off, size, tr->next_dts, tr->next_dts + cto, !((sf >> 16) & 1)) < 0)
                    return -1;
                off += size;
                tr->next_dts += dur;
            }
            next_off = off;
        }
    }
    return 0;
}

static uint8_t *read_at(FILE *f, uint64_t off, uint64_t n)
{
    uint8_t *b;
    if (n > (1ULL << 30))
        return NULL;
    b = malloc(n ? n : 1);
    if (!b)
        return NULL;
    if (fseeko(f, (off_t)off, SEEK_SET) || fread(b, 1, n, f) != n) {
        free(b);
        return NULL;
    }
    return b;
}

int mp4_open_index(const char *path, mp4_file *m, char *err, int errlen)
{
    FILE *f = fopen(path, "rb");
    uint64_t pos = 0, fsize;
    track tr;
    int cap = 0, have_track = 0, ret = -1;

    memset(m, 0, sizeof(*m));
    memset(&tr, 0, sizeof(tr));
    m->nal_length_size = 4;
    if (!f) {
        snprintf(err, errlen, "cannot open %s", path);
        return -1;
    }
    fseeko(f, 0, SEEK_END);
    fsize = (uint64_t)ftello(f);

    while (pos + 8 <= fsize) {
        uint8_t h[16];
        uint64_t size, hdr = 8;
        if (fseeko(f, (off_t)pos, SEEK_SET) || fread(h, 1, 16 > fsize - pos ? fsize - pos : 16, f) < 8)
            break;
        size = be32(h);
        if (size == 1) {
            size = be64(h + 8);
            hdr = 16;
        } else if (size == 0) {
            size = fsize - pos;
        }
        if (size < hdr)
            break;
        if (pos + size > fsize)
            size = fsize - pos;                 /* truncated (crash): use what is there */
        if (!memcmp(h + 4, "moov", 4) || (!memcmp(h + 4, "moof", 4) && have_track)) {
            uint8_t *b = read_at(f, pos + hdr, size - hdr);
            span s;
            int r;
            if (!b) {
                snprintf(err, errlen, "%s: cannot read a %.4s box", path, h + 4);
                goto end;
            }
            s.p = b;
            s.n = size - hdr;
            if (!memcmp(h + 4, "moov", 4)) {
                r = parse_moov(m, &cap, s, &tr);
                if (r == 1)
                    have_track = 1;
            } else {
                r = parse_moof(m, &cap, s, pos, &tr);
            }
            free(b);
            if (r < 0) {
                snprintf(err, errlen, "%s: broken %.4s box", path, h + 4);
                goto end;
            }
        }
        pos += size;
    }
    if (!have_track) {
        snprintf(err, errlen, "%s: no H.264 video track", path);
        goto end;
    }
    /* drop samples that point outside a truncated file */
    while (m->n_samples && m->samples[m->n_samples - 1].offset + m->samples[m->n_samples - 1].size > fsize)
        m->n_samples--;
    if (!m->n_samples) {
        snprintf(err, errlen, "%s: no samples", path);
        goto end;
    }
    if (!m->timescale)
        m->timescale = 90000;
    ret = 0;
end:
    fclose(f);
    if (ret < 0)
        mp4_free_index(m);
    return ret;
}

void mp4_free_index(mp4_file *m)
{
    free(m->avcc);
    free(m->samples);
    memset(m, 0, sizeof(*m));
}
