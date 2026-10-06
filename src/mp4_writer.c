/* Fragmented MP4 writer (stream copy), laid out like FFmpeg's
 * movflags=frag_keyframe+empty_moov+default_base_moof: ftyp, an empty moov, one
 * moof+mdat fragment per GOP and an mfra index at the end. The file is written as
 * <path>.part and renamed on close; after a crash everything up to the last
 * complete fragment is playable.
 *
 * Input packets are Annex B or length-prefixed access units; samples are stored with
 * 4-byte NAL lengths. With fix_refs > 0 every SPS (in avcC and in-band) gets
 * max_num_ref_frames = fix_refs (see h264_sps.c). Timestamps start at 0.
 *
 * Optional audio (si->audio_rate > 0): raw AAC frames as a second track (mp4a/esds), in
 * the same fragments as the video (a second traf; its data follows the video's in the mdat),
 * on the same time line (0 = the first picture). Never in live mode. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bytebuf.h"
#include "h264_sps.h"
#include "log.h"
#include "mp4_writer.h"

typedef struct sample {
    size_t off, size;       /* in the fragment's mdat buffer */
    int64_t dts;
    int key;
} sample;

typedef struct asample {
    size_t off, size;       /* in the fragment's audio data buffer */
    int64_t dts;            /* 1/audio_rate s */
} asample;

typedef struct frag_index {
    int64_t time;
    uint64_t moof_off;
} frag_index;

typedef struct mp4w {
    char *path, *part;
    FILE *f;
    rc_stream_info si;
    int fix_refs;
    uint32_t timescale;
    int header_done;
    uint8_t *sps, *pps;     /* parameter sets for avcC (without start codes) */
    int sps_len, pps_len;

    int64_t base_dts;
    int have_base;
    uint64_t file_off;
    uint32_t seq;
    bytebuf mdat;
    sample *smp;
    int n_smp, cap_smp;
    int64_t last_duration;
    frag_index *idx;
    int n_idx, cap_idx;
    int failed;

    /* audio track (a_rate 0: none) */
    int a_rate, a_channels;
    uint8_t a_cfg[64];
    int a_cfg_len;
    int64_t a_base;         /* the first picture's time in 1/a_rate s */
    bytebuf amdat;
    asample *asmp;
    int n_asmp, cap_asmp;
    int64_t a_last_dts, a_last_duration;
    int have_a_last;

    /* live mode (mp4_live_*): no file; the init segment and one fragment per picture go to memory */
    int live;
    bytebuf init;           /* ftyp + moov */
    bytebuf out;            /* the fragment(s) produced by the last mp4_live_frame() */
    uint8_t *hdr_sps;       /* the SPS the init segment was made from */
    int hdr_sps_len;
    int64_t prev_dts;
    int have_prev;
} mp4w;

/* ---------------------------------------------------------------- NAL helpers */

typedef void (*nal_cb)(void *opaque, const uint8_t *nal, int len);

static void each_nal(const uint8_t *d, int size, int nal_length_size, nal_cb cb, void *opaque)
{
    int pos = 0;
    if (nal_length_size > 0) {
        while (pos + nal_length_size <= size) {
            uint32_t len = 0;
            int i;
            for (i = 0; i < nal_length_size; i++)
                len = (len << 8) | d[pos + i];
            pos += nal_length_size;
            if (len > (uint32_t)(size - pos))
                break;
            if (len)
                cb(opaque, d + pos, len);
            pos += len;
        }
        return;
    }
    {
        int start = -1, i = 0;
        while (i + 2 < size) {
            if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
                if (start >= 0) {
                    int end = i;
                    while (end > start && d[end - 1] == 0)
                        end--;
                    if (end > start)
                        cb(opaque, d + start, end - start);
                }
                i += 3;
                start = i;
            } else if (d[i + 2] > 1) {
                i += 3;
            } else {
                i++;
            }
        }
        if (start >= 0 && start < size)
            cb(opaque, d + start, size - start);
    }
}

static void keep_ps(mp4w *w, const uint8_t *nal, int len)
{
    int t = nal[0] & 0x1F;
    uint8_t **dst = t == 7 ? &w->sps : &w->pps;
    int *dlen = t == 7 ? &w->sps_len : &w->pps_len;
    uint8_t *n = malloc(len);
    if (!n)
        return;
    memcpy(n, nal, len);
    free(*dst);
    *dst = n;
    *dlen = len;
}

static void ps_from_cb(void *opaque, const uint8_t *nal, int len)
{
    mp4w *w = opaque;
    int t = nal[0] & 0x1F;
    if (t == 7 || t == 8)
        keep_ps(w, nal, len);
}

/* extradata may be avcC or Annex B */
static void ps_from_extradata(mp4w *w, const uint8_t *ex, int size)
{
    if (size >= 7 && ex[0] == 1) {
        int pos = 6, k, i, n;
        for (k = 0; k < 2; k++) {
            if (pos > size - 1 && k == 1)
                return;
            n = k == 0 ? (ex[5] & 0x1F) : ex[pos++];
            for (i = 0; i < n; i++) {
                int len;
                if (pos + 2 > size)
                    return;
                len = (ex[pos] << 8) | ex[pos + 1];
                pos += 2;
                if (pos + len > size)
                    return;
                if (len)
                    ps_from_cb(w, ex + pos, len);
                pos += len;
            }
        }
        return;
    }
    each_nal(ex, size, 0, ps_from_cb, w);
}

typedef struct conv_ctx {
    mp4w *w;
    bytebuf *out;
} conv_ctx;

/* one NAL unit -> 4-byte length + NAL (SPS patched) */
static void conv_cb(void *opaque, const uint8_t *nal, int len)
{
    conv_ctx *c = opaque;
    bytebuf *o = c->out;
    size_t lenpos = o->len;
    int t = nal[0] & 0x1F;
    if (t == 7 || t == 8)
        keep_ps(c->w, nal, len);
    bb_u32(o, 0);
    if (t == 7 && c->w->fix_refs > 0)
        sps_patch(o, nal, len, c->w->fix_refs);
    else
        bb_put(o, nal, len);
    bb_set_u32(o, lenpos, (uint32_t)(o->len - lenpos - 4));
}

/* ---------------------------------------------------------------- boxes */

static void put_matrix(bytebuf *b)
{
    static const uint32_t m[9] = { 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000 };
    int i;
    for (i = 0; i < 9; i++)
        bb_u32(b, m[i]);
}

static void put_avcc(mp4w *w, bytebuf *b)
{
    size_t box = bb_box(b, "avcC");
    bytebuf sps = { 0 };
    if (w->fix_refs > 0)
        sps_patch(&sps, w->sps, w->sps_len, w->fix_refs);
    else
        bb_put(&sps, w->sps, w->sps_len);
    bb_u8(b, 1);
    bb_u8(b, sps.d[1]);             /* profile */
    bb_u8(b, sps.d[2]);             /* constraint flags */
    bb_u8(b, sps.d[3]);             /* level */
    bb_u8(b, 0xFF);                 /* 4-byte NAL lengths */
    bb_u8(b, 0xE1);                 /* one SPS */
    bb_u16(b, sps.len);
    bb_put(b, sps.d, sps.len);
    bb_u8(b, 1);                    /* one PPS */
    bb_u16(b, w->pps_len);
    bb_put(b, w->pps, w->pps_len);
    if (sps.d[1] == 100 || sps.d[1] == 110 || sps.d[1] == 122 || sps.d[1] == 144) {
        bb_u8(b, 0xFC | 1);         /* chroma_format_idc 4:2:0 */
        bb_u8(b, 0xF8 | 0);         /* bit_depth_luma - 8 */
        bb_u8(b, 0xF8 | 0);         /* bit_depth_chroma - 8 */
        bb_u8(b, 0);                /* no SPS extensions */
    }
    bb_box_end(b, box);
    bb_free(&sps);
}

/* MPEG-4 descriptor header with a 4-byte size field, as FFmpeg writes it */
static void put_descr(bytebuf *b, int tag, unsigned size)
{
    bb_u8(b, tag);
    bb_u8(b, 0x80 | ((size >> 21) & 0x7F));
    bb_u8(b, 0x80 | ((size >> 14) & 0x7F));
    bb_u8(b, 0x80 | ((size >> 7) & 0x7F));
    bb_u8(b, size & 0x7F);
}

/* the audio trak: AAC (mp4a + esds with the AudioSpecificConfig), track id 2 */
static void put_audio_trak(mp4w *w, bytebuf *b)
{
    size_t trak, mdia, minf, dinf, dref, stbl, stsd, mp4a, box;
    unsigned dsi = (unsigned)w->a_cfg_len, dcd = 13 + 5 + dsi, es = 3 + 5 + dcd + 5 + 1;

    trak = bb_box(b, "trak");
    box = bb_fullbox(b, "tkhd", 0, 3);
    bb_u32(b, 0);
    bb_u32(b, 0);
    bb_u32(b, 2);                   /* track id */
    bb_u32(b, 0);
    bb_u32(b, 0);                   /* duration */
    bb_zero(b, 8);
    bb_u16(b, 0);                   /* layer */
    bb_u16(b, 1);                   /* alternate group */
    bb_u16(b, 0x100);               /* volume */
    bb_u16(b, 0);
    put_matrix(b);
    bb_u32(b, 0);                   /* width, height */
    bb_u32(b, 0);
    bb_box_end(b, box);

    mdia = bb_box(b, "mdia");
    box = bb_fullbox(b, "mdhd", 0, 0);
    bb_u32(b, 0);
    bb_u32(b, 0);
    bb_u32(b, (uint32_t)w->a_rate);
    bb_u32(b, 0);
    bb_u16(b, 0x55C4);              /* "und" */
    bb_u16(b, 0);
    bb_box_end(b, box);
    box = bb_fullbox(b, "hdlr", 0, 0);
    bb_u32(b, 0);
    bb_put(b, "soun", 4);
    bb_zero(b, 12);
    bb_put(b, "SoundHandler", 13);
    bb_box_end(b, box);

    minf = bb_box(b, "minf");
    box = bb_fullbox(b, "smhd", 0, 0);
    bb_u16(b, 0);                   /* balance */
    bb_u16(b, 0);
    bb_box_end(b, box);
    dinf = bb_box(b, "dinf");
    dref = bb_fullbox(b, "dref", 0, 0);
    bb_u32(b, 1);
    box = bb_fullbox(b, "url ", 0, 1);
    bb_box_end(b, box);
    bb_box_end(b, dref);
    bb_box_end(b, dinf);

    stbl = bb_box(b, "stbl");
    stsd = bb_fullbox(b, "stsd", 0, 0);
    bb_u32(b, 1);
    mp4a = bb_box(b, "mp4a");
    bb_zero(b, 6);
    bb_u16(b, 1);                   /* data reference index */
    bb_zero(b, 8);                  /* version, revision, vendor */
    bb_u16(b, w->a_channels);
    bb_u16(b, 16);                  /* sample size */
    bb_u16(b, 0);
    bb_u16(b, 0);
    bb_u32(b, (uint32_t)(w->a_rate <= 0xFFFF ? w->a_rate : 0) << 16);
    box = bb_fullbox(b, "esds", 0, 0);
    put_descr(b, 0x03, es);         /* ES_Descriptor */
    bb_u16(b, 2);                   /* ES_ID */
    bb_u8(b, 0);
    put_descr(b, 0x04, dcd);        /* DecoderConfigDescriptor */
    bb_u8(b, 0x40);                 /* MPEG-4 audio */
    bb_u8(b, 0x15);                 /* audio stream */
    bb_u24(b, 0);                   /* buffer size */
    bb_u32(b, 0);                   /* max bitrate */
    bb_u32(b, 0);                   /* average bitrate */
    put_descr(b, 0x05, dsi);        /* DecoderSpecificInfo: AudioSpecificConfig */
    bb_put(b, w->a_cfg, dsi);
    put_descr(b, 0x06, 1);          /* SLConfigDescriptor */
    bb_u8(b, 0x02);
    bb_box_end(b, box);
    bb_box_end(b, mp4a);
    bb_box_end(b, stsd);
    box = bb_fullbox(b, "stts", 0, 0);
    bb_u32(b, 0);
    bb_box_end(b, box);
    box = bb_fullbox(b, "stsc", 0, 0);
    bb_u32(b, 0);
    bb_box_end(b, box);
    box = bb_fullbox(b, "stsz", 0, 0);
    bb_u32(b, 0);
    bb_u32(b, 0);
    bb_box_end(b, box);
    box = bb_fullbox(b, "stco", 0, 0);
    bb_u32(b, 0);
    bb_box_end(b, box);
    bb_box_end(b, stbl);
    bb_box_end(b, minf);
    bb_box_end(b, mdia);
    bb_box_end(b, trak);
}

static int write_header(mp4w *w)
{
    bytebuf b = { 0 };
    size_t moov, trak, mdia, minf, dinf, dref, stbl, stsd, avc1, mvex, box;
    int width = w->si.width, height = w->si.height;
    static const uint8_t compressor[32] = { 0 };
    sps_info inf;

    /* the size of the stream as the SPS says it (the caller's value is the initial one and the
     * camera may have been reconfigured since) */
    if (w->sps && sps_params(w->sps, w->sps_len, &inf) == 0 && inf.width > 0 && inf.height > 0) {
        width = inf.width;
        height = inf.height;
    }

    box = bb_box(&b, "ftyp");
    bb_put(&b, "iso5", 4);
    bb_u32(&b, 512);
    bb_put(&b, "iso5iso6mp41", 12);
    bb_box_end(&b, box);

    moov = bb_box(&b, "moov");
    box = bb_fullbox(&b, "mvhd", 0, 0);
    bb_u32(&b, 0);                  /* creation time */
    bb_u32(&b, 0);                  /* modification time */
    bb_u32(&b, 1000);               /* timescale */
    bb_u32(&b, 0);                  /* duration: unknown (fragmented) */
    bb_u32(&b, 0x10000);            /* rate */
    bb_u16(&b, 0x100);              /* volume */
    bb_zero(&b, 10);
    put_matrix(&b);
    bb_zero(&b, 24);
    bb_u32(&b, w->a_rate ? 3 : 2);  /* next track id */
    bb_box_end(&b, box);

    trak = bb_box(&b, "trak");
    box = bb_fullbox(&b, "tkhd", 0, 3);
    bb_u32(&b, 0);
    bb_u32(&b, 0);
    bb_u32(&b, 1);                  /* track id */
    bb_u32(&b, 0);
    bb_u32(&b, 0);                  /* duration */
    bb_zero(&b, 8);
    bb_u16(&b, 0);                  /* layer */
    bb_u16(&b, 0);                  /* alternate group */
    bb_u16(&b, 0);                  /* volume */
    bb_u16(&b, 0);
    put_matrix(&b);
    bb_u32(&b, (uint32_t)width << 16);
    bb_u32(&b, (uint32_t)height << 16);
    bb_box_end(&b, box);

    mdia = bb_box(&b, "mdia");
    box = bb_fullbox(&b, "mdhd", 0, 0);
    bb_u32(&b, 0);
    bb_u32(&b, 0);
    bb_u32(&b, w->timescale);
    bb_u32(&b, 0);
    bb_u16(&b, 0x55C4);             /* "und" */
    bb_u16(&b, 0);
    bb_box_end(&b, box);
    box = bb_fullbox(&b, "hdlr", 0, 0);
    bb_u32(&b, 0);
    bb_put(&b, "vide", 4);
    bb_zero(&b, 12);
    bb_put(&b, "VideoHandler", 13);
    bb_box_end(&b, box);

    minf = bb_box(&b, "minf");
    box = bb_fullbox(&b, "vmhd", 0, 1);
    bb_zero(&b, 8);
    bb_box_end(&b, box);
    dinf = bb_box(&b, "dinf");
    dref = bb_fullbox(&b, "dref", 0, 0);
    bb_u32(&b, 1);
    box = bb_fullbox(&b, "url ", 0, 1);
    bb_box_end(&b, box);
    bb_box_end(&b, dref);
    bb_box_end(&b, dinf);

    stbl = bb_box(&b, "stbl");
    stsd = bb_fullbox(&b, "stsd", 0, 0);
    bb_u32(&b, 1);
    avc1 = bb_box(&b, "avc1");
    bb_zero(&b, 6);
    bb_u16(&b, 1);                  /* data reference index */
    bb_zero(&b, 16);
    bb_u16(&b, width);
    bb_u16(&b, height);
    bb_u32(&b, 0x480000);
    bb_u32(&b, 0x480000);
    bb_u32(&b, 0);
    bb_u16(&b, 1);                  /* frame count */
    bb_put(&b, compressor, 32);
    bb_u16(&b, 0x18);
    bb_u16(&b, 0xFFFF);
    put_avcc(w, &b);
    bb_box_end(&b, avc1);
    bb_box_end(&b, stsd);
    box = bb_fullbox(&b, "stts", 0, 0);
    bb_u32(&b, 0);
    bb_box_end(&b, box);
    box = bb_fullbox(&b, "stsc", 0, 0);
    bb_u32(&b, 0);
    bb_box_end(&b, box);
    box = bb_fullbox(&b, "stsz", 0, 0);
    bb_u32(&b, 0);
    bb_u32(&b, 0);
    bb_box_end(&b, box);
    box = bb_fullbox(&b, "stco", 0, 0);
    bb_u32(&b, 0);
    bb_box_end(&b, box);
    bb_box_end(&b, stbl);
    bb_box_end(&b, minf);
    bb_box_end(&b, mdia);
    bb_box_end(&b, trak);
    if (w->a_rate)
        put_audio_trak(w, &b);

    mvex = bb_box(&b, "mvex");
    box = bb_fullbox(&b, "trex", 0, 0);
    bb_u32(&b, 1);                  /* track id */
    bb_u32(&b, 1);                  /* sample description index */
    bb_u32(&b, 0);
    bb_u32(&b, 0);
    bb_u32(&b, 0);
    bb_box_end(&b, box);
    if (w->a_rate) {
        box = bb_fullbox(&b, "trex", 0, 0);
        bb_u32(&b, 2);
        bb_u32(&b, 1);
        bb_u32(&b, 1024);           /* default duration: one AAC frame */
        bb_u32(&b, 0);
        bb_u32(&b, 0x02000000);     /* every audio frame is a sync sample */
        bb_box_end(&b, box);
    }
    bb_box_end(&b, mvex);
    bb_box_end(&b, moov);

    if (b.err) {
        bb_free(&b);
        return -1;
    }
    if (w->live) {
        uint8_t *s = malloc(w->sps_len);
        bb_free(&w->init);
        w->init = b;                            /* hand the buffer over */
        if (s)
            memcpy(s, w->sps, w->sps_len);
        free(w->hdr_sps);
        w->hdr_sps = s;
        w->hdr_sps_len = s ? w->sps_len : 0;
        w->header_done = 1;
        return 0;
    }
    if (fwrite(b.d, 1, b.len, w->f) != b.len || fflush(w->f)) {
        bb_free(&b);
        return -1;
    }
    w->file_off += b.len;
    bb_free(&b);
    w->header_done = 1;
    return 0;
}

/* write the buffered GOP as one moof + mdat; next_dts gives the last sample's duration */
static int flush_fragment(mp4w *w, int64_t next_dts, int have_next)
{
    bytebuf b = { 0 };
    size_t moof, traf, box, trun_data_off, atrun_data_off = 0;
    int i;

    if (w->n_smp == 0)
        return 0;
    moof = bb_box(&b, "moof");
    box = bb_fullbox(&b, "mfhd", 0, 0);
    bb_u32(&b, ++w->seq);
    bb_box_end(&b, box);
    traf = bb_box(&b, "traf");
    box = bb_fullbox(&b, "tfhd", 0, 0x020000);  /* default-base-is-moof */
    bb_u32(&b, 1);
    bb_box_end(&b, box);
    box = bb_fullbox(&b, "tfdt", 1, 0);
    bb_u64(&b, (uint64_t)w->smp[0].dts);
    bb_box_end(&b, box);
    /* data offset, sample duration, size and flags present */
    box = bb_fullbox(&b, "trun", 0, 0x000701);
    bb_u32(&b, w->n_smp);
    trun_data_off = b.len;
    bb_u32(&b, 0);
    for (i = 0; i < w->n_smp; i++) {
        int64_t dur;
        if (i + 1 < w->n_smp)
            dur = w->smp[i + 1].dts - w->smp[i].dts;
        else if (have_next)
            dur = next_dts - w->smp[i].dts;
        else
            dur = w->last_duration;
        if (dur <= 0 || dur > 0x7FFFFFFF)
            dur = w->last_duration;
        w->last_duration = dur;
        bb_u32(&b, (uint32_t)dur);
        bb_u32(&b, (uint32_t)w->smp[i].size);
        bb_u32(&b, w->smp[i].key ? 0x02000000 : 0x01010000);
    }
    bb_box_end(&b, box);
    bb_box_end(&b, traf);
    if (w->n_asmp) {                /* the audio that arrived during this GOP */
        traf = bb_box(&b, "traf");
        box = bb_fullbox(&b, "tfhd", 0, 0x020000);
        bb_u32(&b, 2);
        bb_box_end(&b, box);
        box = bb_fullbox(&b, "tfdt", 1, 0);
        bb_u64(&b, (uint64_t)w->asmp[0].dts);
        bb_box_end(&b, box);
        box = bb_fullbox(&b, "trun", 0, 0x000301);     /* data offset, sample duration and size */
        bb_u32(&b, w->n_asmp);
        atrun_data_off = b.len;
        bb_u32(&b, 0);
        for (i = 0; i < w->n_asmp; i++) {
            /* up to the next frame: after lost packets the frame is followed by silence and
             * the audio stays in sync; the last one gets a frame's length (the next
             * fragment's tfdt places what follows) */
            int64_t dur = i + 1 < w->n_asmp ? w->asmp[i + 1].dts - w->asmp[i].dts : w->a_last_duration;
            if (dur <= 0 || dur > 0x7FFFFFFF)
                dur = w->a_last_duration;
            bb_u32(&b, (uint32_t)dur);
            bb_u32(&b, (uint32_t)w->asmp[i].size);
        }
        bb_box_end(&b, box);
        bb_box_end(&b, traf);
    }
    bb_box_end(&b, moof);
    bb_set_u32(&b, trun_data_off, (uint32_t)(b.len - moof + 8));   /* first sample after the mdat header */
    if (w->n_asmp)
        bb_set_u32(&b, atrun_data_off, (uint32_t)(b.len - moof + 8 + w->mdat.len));
    bb_u32(&b, (uint32_t)(w->mdat.len + w->amdat.len + 8));
    bb_put(&b, "mdat", 4);

    if (w->n_idx == w->cap_idx) {
        int cap = w->cap_idx ? 2 * w->cap_idx : 64;
        frag_index *n = realloc(w->idx, sizeof(*n) * cap);
        if (n) {
            w->idx = n;
            w->cap_idx = cap;
        }
    }
    if (w->live) {
        bb_put(&w->out, b.d, b.len);
        bb_put(&w->out, w->mdat.d, w->mdat.len);
        bb_free(&b);
        w->mdat.len = 0;
        w->n_smp = 0;
        return w->out.err ? -1 : 0;
    }
    if (b.err || w->amdat.err || fwrite(b.d, 1, b.len, w->f) != b.len ||
        fwrite(w->mdat.d, 1, w->mdat.len, w->f) != w->mdat.len ||
        fwrite(w->amdat.d, 1, w->amdat.len, w->f) != w->amdat.len || fflush(w->f)) {
        bb_free(&b);
        return -1;
    }
    if (w->n_idx < w->cap_idx) {
        w->idx[w->n_idx].time = w->smp[0].dts;
        w->idx[w->n_idx].moof_off = w->file_off;
        w->n_idx++;
    }
    w->file_off += b.len + w->mdat.len + w->amdat.len;
    bb_free(&b);
    w->mdat.len = 0;
    w->n_smp = 0;
    w->amdat.len = 0;
    w->n_asmp = 0;
    return 0;
}

static int write_mfra(mp4w *w)
{
    bytebuf b = { 0 };
    size_t mfra, box;
    int i;
    mfra = bb_box(&b, "mfra");
    box = bb_fullbox(&b, "tfra", 1, 0);
    bb_u32(&b, 1);                  /* track id */
    bb_u32(&b, 0);                  /* 1-byte traf/trun/sample numbers */
    bb_u32(&b, w->n_idx);
    for (i = 0; i < w->n_idx; i++) {
        bb_u64(&b, (uint64_t)w->idx[i].time);
        bb_u64(&b, w->idx[i].moof_off);
        bb_u8(&b, 1);
        bb_u8(&b, 1);
        bb_u8(&b, 1);
    }
    bb_box_end(&b, box);
    box = bb_fullbox(&b, "mfro", 0, 0);
    bb_u32(&b, (uint32_t)(b.len + 4));
    bb_box_end(&b, box);
    bb_box_end(&b, mfra);
    i = b.err || fwrite(b.d, 1, b.len, w->f) != b.len ? -1 : 0;
    bb_free(&b);
    return i;
}

/* ---------------------------------------------------------------- rc_video_ops */

static void *mp4_open(void *opaque, const char *path, const rc_stream_info *si)
{
    const mp4_options *o = opaque;
    mp4w *w = calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    w->path = strdup(path);
    w->part = malloc(strlen(path) + 6);
    if (!w->path || !w->part)
        goto fail;
    sprintf(w->part, "%s.part", path);
    w->si = *si;
    w->si.extradata = NULL;
    w->fix_refs = o ? o->fix_refs : 0;
    w->timescale = si->tb_num == 1 && si->tb_den > 0 ? (uint32_t)si->tb_den : 90000;
    w->last_duration = w->timescale / 25;
    if (si->extradata && si->extradata_size > 0)
        ps_from_extradata(w, si->extradata, si->extradata_size);
    if (si->audio_rate > 0 && si->audio_config && si->audio_config_size >= 2 &&
        si->audio_config_size <= (int)sizeof(w->a_cfg)) {
        int ch = (si->audio_config[1] >> 3) & 0x0F;    /* channelConfiguration of the AudioSpecificConfig */
        w->a_rate = si->audio_rate;
        w->a_channels = ch >= 1 && ch <= 7 ? ch : (si->audio_channels > 0 ? si->audio_channels : 1);
        memcpy(w->a_cfg, si->audio_config, si->audio_config_size);
        w->a_cfg_len = si->audio_config_size;
        w->a_last_duration = 1024;
    }
    w->si.audio_config = NULL;
    w->f = fopen(w->part, "wb");
    if (!w->f) {
        log_error("cannot create %s", w->part);
        goto fail;
    }
    return w;
fail:
    free(w->path);
    free(w->part);
    free(w);
    return NULL;
}

static int mp4_write(void *h, const rc_packet *p)
{
    mp4w *w = h;
    conv_ctx c;
    size_t start;
    int64_t dts;

    if (w->failed)
        return -1;
    if (!w->have_base) {
        w->have_base = 1;
        w->base_dts = p->dts;
        if (w->a_rate)
            w->a_base = (int64_t)llround((double)p->dts * w->a_rate / w->timescale);
    }
    dts = p->dts - w->base_dts;
    if (p->key && w->n_smp && flush_fragment(w, dts, 1) < 0)
        goto fail;

    start = w->mdat.len;
    c.w = w;
    c.out = &w->mdat;
    each_nal(p->data, p->size, w->si.nal_length_size, conv_cb, &c);
    if (w->mdat.err)
        goto fail;
    if (!w->header_done) {
        if (!w->sps || !w->pps) {               /* cannot describe the stream yet */
            w->mdat.len = start;
            return 0;
        }
        if (write_header(w) < 0)
            goto fail;
    }
    if (w->n_smp == w->cap_smp) {
        int cap = w->cap_smp ? 2 * w->cap_smp : 256;
        sample *n = realloc(w->smp, sizeof(*n) * cap);
        if (!n)
            goto fail;
        w->smp = n;
        w->cap_smp = cap;
    }
    w->smp[w->n_smp].off = start;
    w->smp[w->n_smp].size = w->mdat.len - start;
    w->smp[w->n_smp].dts = dts;
    w->smp[w->n_smp].key = p->key;
    w->n_smp++;
    return 0;
fail:
    log_error("write error in %s", w->part);
    w->failed = 1;
    return -1;
}

/* one raw AAC frame; ts in 1/a_rate s on the video's time line. Frames before the first
 * picture (or before the header could be written) and out-of-order ones are dropped. */
static int mp4_write_audio(void *h, const uint8_t *data, int size, int64_t ts)
{
    mp4w *w = h;
    int64_t dts;
    if (w->failed)
        return -1;
    if (!w->a_rate || !w->have_base || !w->header_done || size <= 0)
        return 0;
    dts = ts - w->a_base;
    if (dts < 0 || (w->have_a_last && dts <= w->a_last_dts))
        return 0;
    if (w->n_asmp == w->cap_asmp) {
        int cap = w->cap_asmp ? 2 * w->cap_asmp : 256;
        asample *n = realloc(w->asmp, sizeof(*n) * cap);
        if (!n)
            goto fail;
        w->asmp = n;
        w->cap_asmp = cap;
    }
    w->asmp[w->n_asmp].off = w->amdat.len;
    w->asmp[w->n_asmp].size = size;
    w->asmp[w->n_asmp].dts = dts;
    bb_put(&w->amdat, data, size);
    if (w->amdat.err)
        goto fail;
    w->n_asmp++;
    w->have_a_last = 1;
    w->a_last_dts = dts;
    return 0;
fail:
    log_error("write error in %s", w->part);
    w->failed = 1;
    return -1;
}

/* returns 0 if a playable file was produced (it may be shorter than intended after a write
 * error), -1 if nothing usable could be written (the .part is removed) */
static int mp4_close(void *h)
{
    mp4w *w = h;
    int ret = 0;
    if (!w)
        return 0;
    if (!w->failed && w->header_done) {
        if (flush_fragment(w, 0, 0) < 0 || write_mfra(w) < 0)
            w->failed = 1;
    }
    if (fclose(w->f))
        w->failed = 1;
    if (!w->header_done || w->n_idx == 0) {
        remove(w->part);                        /* no complete fragment: nothing playable */
        ret = -1;
    } else if (rename(w->part, w->path)) {
        log_error("cannot rename %s", w->part);
        ret = -1;
    }
    free(w->path);
    free(w->part);
    free(w->sps);
    free(w->pps);
    bb_free(&w->mdat);
    bb_free(&w->amdat);
    free(w->smp);
    free(w->asmp);
    free(w->idx);
    free(w);
    return ret;
}

const rc_video_ops mp4_writer_ops = { mp4_open, mp4_write, mp4_close, mp4_write_audio };

/* ---------------------------------------------------------------- live mode */

mp4_live *mp4_live_create(const rc_stream_info *si, int fix_refs)
{
    mp4w *w = calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    w->live = 1;
    w->part = strdup("the live stream");       /* for messages */
    w->si = *si;
    w->si.extradata = NULL;
    w->fix_refs = fix_refs;
    w->timescale = si->tb_num == 1 && si->tb_den > 0 ? (uint32_t)si->tb_den : 90000;
    w->last_duration = w->timescale / 15;
    if (si->extradata && si->extradata_size > 0)
        ps_from_extradata(w, si->extradata, si->extradata_size);
    return (mp4_live *)w;
}

static int live_same_config(const uint8_t *a, int al, const uint8_t *b, int bl)
{
    sps_info x, y;
    if (sps_params(a, al, &x) < 0 || sps_params(b, bl, &y) < 0)
        return 0;
    return x.profile == y.profile && x.level == y.level && x.chroma == y.chroma &&
           x.bit_depth == y.bit_depth && x.width == y.width && x.height == y.height;
}

/* keep only the newest sample: move it to the front of the mdat buffer */
static void live_keep_last(mp4w *w)
{
    sample last = w->smp[w->n_smp - 1];
    size_t n = w->mdat.len - last.off;
    memmove(w->mdat.d, w->mdat.d + last.off, n);
    w->mdat.len = n;
    last.off = 0;
    w->smp[0] = last;
    w->n_smp = 1;
}

/* One picture is always held back until the next one arrives, so that every fragment gets its
 * exact duration: with a guessed duration, irregular camera timestamps leave small gaps in the
 * browser's buffer, and MSE playback stops at a gap. */
int mp4_live_frame(mp4_live *l, const rc_packet *p, const uint8_t **frag, size_t *len, int *new_init, int *key)
{
    mp4w *w = (mp4w *)l;
    int had_header = w->header_done, held_key = w->n_smp ? w->smp[0].key : 0;

    w->out.len = 0;
    w->out.err = 0;
    *new_init = 0;
    *frag = NULL;
    *len = 0;
    *key = 0;
    w->failed = 0;
    /* a key frame makes mp4_write emit the held picture itself (with the exact duration) */
    if (mp4_write(w, p) < 0)
        return -1;
    /* A new SPS: only a change of what the decoder configuration depends on (size, profile,
     * level, chroma format) needs a new init segment. Other changes (some cameras vary their
     * SPS slightly) are fine: the SPS also travels in-band with every key frame. */
    if (w->header_done && w->hdr_sps && w->sps &&
        (w->sps_len != w->hdr_sps_len || memcmp(w->sps, w->hdr_sps, w->sps_len)) &&
        !live_same_config(w->sps, w->sps_len, w->hdr_sps, w->hdr_sps_len)) {
        if (w->n_smp > 1)
            live_keep_last(w);              /* the held picture belongs to the old stream: dropped */
        w->out.len = 0;
        w->header_done = 0;
        if (write_header(w) < 0)
            return -1;
        *new_init = 1;
        return 0;
    }
    if (!had_header && w->header_done)
        *new_init = 1;
    if (w->n_smp == 2) {                    /* emit the held picture, hold the new one */
        sample next = w->smp[1];
        size_t n = w->mdat.len - next.off;
        uint8_t *tmp = malloc(n ? n : 1);
        if (!tmp)
            return -1;
        memcpy(tmp, w->mdat.d + next.off, n);
        held_key = w->smp[0].key;
        w->mdat.len = next.off;
        w->n_smp = 1;
        if (flush_fragment(w, next.dts, 1) < 0) {
            free(tmp);
            return -1;
        }
        bb_put(&w->mdat, tmp, n);
        free(tmp);
        next.off = 0;
        w->smp[0] = next;
        w->n_smp = 1;
    }
    if (!w->out.len)
        return 0;
    *frag = w->out.d;
    *len = w->out.len;
    *key = held_key;
    return 1;
}

const uint8_t *mp4_live_init(mp4_live *l, size_t *len)
{
    mp4w *w = (mp4w *)l;
    *len = w->header_done ? w->init.len : 0;
    return w->header_done ? w->init.d : NULL;
}

void mp4_live_free(mp4_live *l)
{
    mp4w *w = (mp4w *)l;
    if (!w)
        return;
    free(w->part);
    free(w->sps);
    free(w->pps);
    free(w->hdr_sps);
    bb_free(&w->mdat);
    bb_free(&w->init);
    bb_free(&w->out);
    free(w->smp);
    free(w->idx);
    free(w);
}
