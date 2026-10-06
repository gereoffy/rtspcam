/*
 * mvparse - extract H.264 motion vectors without decoding pictures.
 *
 * Only the entropy layer is decoded (CABAC, residual coefficients are parsed and
 * discarded); there is no reconstruction, no reference picture buffer and no
 * deblocking. The output is the same block list that libavcodec exports with
 * flags2=+export_mvs (AVMotionVector, motion_scale 4, list 0 only).
 *
 * Supported: progressive (frame_mbs_only) streams, 4:2:0 or 4:0:0, CABAC,
 * I/P/SP slices, any number of slices per picture. Pictures with B slices,
 * CAVLC, interlace or FMO are reported as MVP_UNSUPPORTED.
 *
 * Every picture is parsed independently (motion vectors are predicted from the
 * neighbours in the same picture only), so a broken picture never affects the next one.
 */
#ifndef MVPARSE_H
#define MVPARSE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* one block, same meaning as AVMotionVector: the block of w x h pixels centred at
 * (dst_x, dst_y) moves by (mx / 4, my / 4) pixels relative to the reference picture */
typedef struct mvp_mv {
    int16_t dst_x, dst_y;
    int16_t mx, my;
    uint8_t w, h;
} mvp_mv;

enum mvp_type {
    MVP_NONE        = 0,  /* no slice in the data (parameter sets, SEI, ...) */
    MVP_P           = 1,  /* inter picture: vectors are valid (possibly none at all) */
    MVP_I           = 2,  /* intra picture: no vectors */
    MVP_UNSUPPORTED = 3,  /* B slices, CAVLC, interlace, FMO, missing SPS/PPS */
    MVP_SKIPPED     = 4,  /* inter picture not parsed: its references are farther than
                             mvp_set_max_ref_dist() allows (see ref_dist) */
};

typedef struct mvp_frame {
    int type;             /* enum mvp_type */
    int key;              /* the picture contains an IDR slice */
    int errors;           /* number of slices that could not be parsed (vectors are partial) */
    int width, height;    /* cropped picture size from the SPS */
    int mb_width, mb_height;
    int n_mv;
    const mvp_mv *mv;     /* owned by the context, valid until the next mvp_decode() */
    int ref_dist;         /* P: how many frames back the farthest used reference is (vectors span
                             that many frame intervals); SKIPPED: the nearest one; 0 = unknown */
    int poc;              /* picture order count */
    int vcl_bytes;        /* coded size of the picture: RBSP bytes of its slice NAL units, without the NAL
                             header byte, start codes/length fields, parameter sets/SEI, emulation
                             prevention bytes and trailing zero padding */
    const char *reason;   /* MVP_UNSUPPORTED: what the stream uses that is not supported (static
                             string), else NULL */
    int no_ps;            /* MVP_UNSUPPORTED because the slices refer to an SPS/PPS not seen yet
                             (usually temporary: they may still come in-band) */
} mvp_frame;

typedef struct mvp_ctx mvp_ctx;

mvp_ctx *mvp_create(void);
void mvp_free(mvp_ctx *c);

/* Parameter sets from an avcC record (MP4 extradata) or an Annex B buffer. Returns 0 or <0. */
int mvp_set_extradata(mvp_ctx *c, const uint8_t *data, int size);

/* Do not parse inter pictures whose references are all more than `frames` frames back
 * (they are reported as MVP_SKIPPED). 0 (default): parse everything.
 * Some cameras use hierarchical P frames (reference distances 1,2,1,4,1,2,1,8): the far
 * ones code moving objects as intra blocks, so their vectors do not show the motion. */
void mvp_set_max_ref_dist(mvp_ctx *c, int frames);

/* Reference pictures after the last mvp_decode() (debugging): "S0:fn<frame_num>:poc<poc> L<idx>:..." */
int mvp_dump_refs(const mvp_ctx *c, char *buf, int size);

/* Picture size (cropped) from the first known SPS. Returns 0, or -1 if no SPS was seen. */
int mvp_get_size(const mvp_ctx *c, int *width, int *height);

/* Parse one access unit (= one picture). nal_length_size is 1..4 for length-prefixed
 * NAL units (MP4 samples) or 0 for Annex B start codes (RTP depacketizer output).
 * In-band SPS/PPS are picked up. Returns 0 on success (see out->type), <0 on bad input. */
int mvp_decode(mvp_ctx *c, const uint8_t *data, int size, int nal_length_size, mvp_frame *out);

#ifdef __cplusplus
}
#endif
#endif
