/*
 * mvparse - H.264 motion vector extraction without picture decoding.
 *
 * The CABAC engine, the macroblock syntax and the motion vector prediction are
 * taken from FFmpeg's H.264 decoder (libavcodec/cabac*.h, h264_cabac.c,
 * h264_mvpred.h, release/8.0) and stripped down to what is needed to find the
 * vectors: residual coefficients are entropy-decoded (CABAC cannot skip them)
 * but their values are dropped; no dequantisation, IDCT, prediction, motion
 * compensation, deblocking or reference picture management happens.
 *
 * Derived from FFmpeg, Copyright (c) 2003 Michael Niedermayer <michaelni@gmx.at>
 * and the FFmpeg developers. Licensed under the GNU Lesser General Public
 * License version 2.1 or later, like FFmpeg.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mvparse.h"
#include "h264_cabac_tables.h"

#define av_always_inline inline __attribute__((always_inline))
#define av_noinline __attribute__((noinline))
#define FFMIN(a, b) ((a) > (b) ? (b) : (a))

/* ---------------------------------------------------------------- mb types */

#define MB_TYPE_INTRA4x4   (1 <<  0)
#define MB_TYPE_INTRA16x16 (1 <<  1)
#define MB_TYPE_INTRA_PCM  (1 <<  2)
#define MB_TYPE_16x16      (1 <<  3)
#define MB_TYPE_16x8       (1 <<  4)
#define MB_TYPE_8x16       (1 <<  5)
#define MB_TYPE_8x8        (1 <<  6)
#define MB_TYPE_DIRECT2    (1 <<  8)
#define MB_TYPE_REF0       (1 <<  9)
#define MB_TYPE_P0L0       (1 << 12)
#define MB_TYPE_P1L0       (1 << 13)
#define MB_TYPE_L0         (MB_TYPE_P0L0 | MB_TYPE_P1L0)
#define MB_TYPE_SKIP       (1 << 17)
#define MB_TYPE_8x8DCT     0x01000000

#define IS_INTRA4x4(a)   ((a) & MB_TYPE_INTRA4x4)
#define IS_INTRA16x16(a) ((a) & MB_TYPE_INTRA16x16)
#define IS_INTRA_PCM(a)  ((a) & MB_TYPE_INTRA_PCM)
#define IS_INTRA(a)      ((a) & 7)
#define IS_INTER(a)      ((a) & (MB_TYPE_16x16 | MB_TYPE_16x8 | MB_TYPE_8x16 | MB_TYPE_8x8))
#define IS_SKIP(a)       ((a) & MB_TYPE_SKIP)
#define IS_16X16(a)      ((a) & MB_TYPE_16x16)
#define IS_16X8(a)       ((a) & MB_TYPE_16x8)
#define IS_8X16(a)       ((a) & MB_TYPE_8x16)
#define IS_8X8(a)        ((a) & MB_TYPE_8x8)
#define IS_8x8DCT(a)     ((a) & MB_TYPE_8x8DCT)
#define IS_SUB_8X8(a)    ((a) & MB_TYPE_16x16) /* note reused */
#define IS_SUB_8X4(a)    ((a) & MB_TYPE_16x8)
#define IS_SUB_4X8(a)    ((a) & MB_TYPE_8x16)
#define IS_DIR(a, part)  ((a) & (MB_TYPE_P0L0 << (part)))
#define USES_LIST0(a)    ((a) & MB_TYPE_L0)

#define LIST_NOT_USED      -1
#define PART_NOT_AVAILABLE -2

#define LUMA_DC_BLOCK_INDEX   48
#define CHROMA_DC_BLOCK_INDEX 49

static const uint8_t scan8[16 * 3 + 3] = {
    4 +  1 * 8, 5 +  1 * 8, 4 +  2 * 8, 5 +  2 * 8,
    6 +  1 * 8, 7 +  1 * 8, 6 +  2 * 8, 7 +  2 * 8,
    4 +  3 * 8, 5 +  3 * 8, 4 +  4 * 8, 5 +  4 * 8,
    6 +  3 * 8, 7 +  3 * 8, 6 +  4 * 8, 7 +  4 * 8,
    4 +  6 * 8, 5 +  6 * 8, 4 +  7 * 8, 5 +  7 * 8,
    6 +  6 * 8, 7 +  6 * 8, 6 +  7 * 8, 7 +  7 * 8,
    4 +  8 * 8, 5 +  8 * 8, 4 +  9 * 8, 5 +  9 * 8,
    6 +  8 * 8, 7 +  8 * 8, 6 +  9 * 8, 7 +  9 * 8,
    4 + 11 * 8, 5 + 11 * 8, 4 + 12 * 8, 5 + 12 * 8,
    6 + 11 * 8, 7 + 11 * 8, 6 + 12 * 8, 7 + 12 * 8,
    4 + 13 * 8, 5 + 13 * 8, 4 + 14 * 8, 5 + 14 * 8,
    6 + 13 * 8, 7 + 13 * 8, 6 + 14 * 8, 7 + 14 * 8,
    0 +  0 * 8, 0 +  5 * 8, 0 + 10 * 8
};

typedef struct { uint32_t type; int8_t partition_count; } PMbInfo;
typedef struct { uint32_t type; int8_t cbp; } IMbInfo;

static const PMbInfo p_mb_type_info[5] = {
    { MB_TYPE_16x16 | MB_TYPE_P0L0,                               1 },
    { MB_TYPE_16x8  | MB_TYPE_P0L0 | MB_TYPE_P1L0,                2 },
    { MB_TYPE_8x16  | MB_TYPE_P0L0 | MB_TYPE_P1L0,                2 },
    { MB_TYPE_8x8   | MB_TYPE_P0L0 | MB_TYPE_P1L0,                4 },
    { MB_TYPE_8x8   | MB_TYPE_P0L0 | MB_TYPE_P1L0 | MB_TYPE_REF0, 4 },
};

static const PMbInfo p_sub_mb_type_info[4] = {
    { MB_TYPE_16x16 | MB_TYPE_P0L0, 1 },
    { MB_TYPE_16x8  | MB_TYPE_P0L0, 2 },
    { MB_TYPE_8x16  | MB_TYPE_P0L0, 2 },
    { MB_TYPE_8x8   | MB_TYPE_P0L0, 4 },
};

/* ff_h264_i_mb_type_info without the prediction modes (not needed for parsing) */
static av_always_inline IMbInfo i_mb_type_info(int t)
{
    IMbInfo r;
    if (t == 0) {
        r.type = MB_TYPE_INTRA4x4; r.cbp = -1;
    } else if (t == 25) {
        r.type = MB_TYPE_INTRA_PCM; r.cbp = -1;
    } else {
        t--;
        r.type = MB_TYPE_INTRA16x16;
        r.cbp = (t >= 12 ? 15 : 0) + 16 * ((t % 12) / 4);
    }
    return r;
}

/* ---------------------------------------------------------------- CABAC engine */

#define CABAC_BITS 16
#define CABAC_MASK ((1 << CABAC_BITS) - 1)
#define PADDING 64          /* zero bytes after every unescaped NAL unit */
#define OVERREAD 32         /* how far the engine may run past the end before it stalls */

typedef struct CABACContext {
    int low;
    int range;
    const uint8_t *bytestream_start;
    const uint8_t *bytestream;
    const uint8_t *bytestream_end;
} CABACContext;

static const uint8_t *const ff_h264_norm_shift = ff_h264_cabac_tables + H264_NORM_SHIFT_OFFSET;
static const uint8_t *const ff_h264_lps_range = ff_h264_cabac_tables + H264_LPS_RANGE_OFFSET;
static const uint8_t *const ff_h264_mlps_state = ff_h264_cabac_tables + H264_MLPS_STATE_OFFSET;
static const uint8_t *const ff_h264_last_coeff_flag_offset_8x8 = ff_h264_cabac_tables + H264_LAST_COEFF_FLAG_OFFSET_8x8_OFFSET;

static int init_cabac_decoder(CABACContext *c, const uint8_t *buf, int buf_size)
{
    c->bytestream_start =
    c->bytestream = buf;
    c->bytestream_end = buf + buf_size;

    c->low =  (*c->bytestream++) << 18;
    c->low += (*c->bytestream++) << 10;
    if (((uintptr_t)c->bytestream & 1) == 0) {
        c->low += (1 << 9);
    } else {
        c->low += ((*c->bytestream++) << 2) + 2;
    }
    c->range = 0x1FE;
    if ((c->range << (CABAC_BITS + 1)) < c->low)
        return -1;
    return 0;
}

static av_always_inline void refill(CABACContext *c)
{
    c->low += (c->bytestream[0] << 9) + (c->bytestream[1] << 1);
    c->low -= CABAC_MASK;
    if (c->bytestream < c->bytestream_end + OVERREAD)
        c->bytestream += CABAC_BITS / 8;
}

static av_always_inline void refill2(CABACContext *c)
{
    int i;
    unsigned x;
    i = __builtin_ctz(c->low) - CABAC_BITS;
    x = -CABAC_MASK;
    x += (c->bytestream[0] << 9) + (c->bytestream[1] << 1);
    c->low += x << i;
    if (c->bytestream < c->bytestream_end + OVERREAD)
        c->bytestream += CABAC_BITS / 8;
}

static av_always_inline void renorm_cabac_decoder_once(CABACContext *c)
{
    int shift = (uint32_t)(c->range - 0x100) >> 31;
    c->range <<= shift;
    c->low   <<= shift;
    if (!(c->low & CABAC_MASK))
        refill(c);
}

static av_always_inline int get_cabac_inline(CABACContext *c, uint8_t *const state)
{
    int s = *state;
    int RangeLPS = ff_h264_lps_range[2 * (c->range & 0xC0) + s];
    int bit, lps_mask;

    c->range -= RangeLPS;
    lps_mask = ((c->range << (CABAC_BITS + 1)) - c->low) >> 31;

    c->low -= (c->range << (CABAC_BITS + 1)) & lps_mask;
    c->range += (RangeLPS - c->range) & lps_mask;

    s ^= lps_mask;
    *state = (ff_h264_mlps_state + 128)[s];
    bit = s & 1;

    lps_mask = ff_h264_norm_shift[c->range];
    c->range <<= lps_mask;
    c->low   <<= lps_mask;
    if (!(c->low & CABAC_MASK))
        refill2(c);
    return bit;
}

static av_noinline int get_cabac_noinline(CABACContext *c, uint8_t *const state)
{
    return get_cabac_inline(c, state);
}

#define get_cabac get_cabac_inline

static av_always_inline int get_cabac_bypass(CABACContext *c)
{
    int range;
    c->low += c->low;

    if (!(c->low & CABAC_MASK))
        refill(c);

    range = c->range << (CABAC_BITS + 1);
    if (c->low < range) {
        return 0;
    } else {
        c->low -= range;
        return 1;
    }
}

static av_always_inline int get_cabac_bypass_sign(CABACContext *c, int val)
{
    int range, mask;
    c->low += c->low;

    if (!(c->low & CABAC_MASK))
        refill(c);

    range = c->range << (CABAC_BITS + 1);
    c->low -= range;
    mask = c->low >> 31;
    range &= mask;
    c->low += range;
    return (val ^ mask) - mask;
}

static int get_cabac_terminate(CABACContext *c)
{
    c->range -= 2;
    if (c->low < c->range << (CABAC_BITS + 1)) {
        renorm_cabac_decoder_once(c);
        return 0;
    } else {
        return c->bytestream - c->bytestream_start;
    }
}

/* ---------------------------------------------------------------- bit reader */

typedef struct BitReader {
    const uint8_t *buf;
    int size_bits;
    int pos;
} BitReader;

static void br_init(BitReader *r, const uint8_t *buf, int size)
{
    r->buf = buf;
    r->size_bits = size * 8;
    r->pos = 0;
}

static unsigned br_bit(BitReader *r)
{
    unsigned v = 0;
    if (r->pos < r->size_bits)
        v = (r->buf[r->pos >> 3] >> (7 - (r->pos & 7))) & 1;
    r->pos++;
    return v;
}

static unsigned br_bits(BitReader *r, int n)
{
    unsigned v = 0;
    while (n-- > 0)
        v = (v << 1) | br_bit(r);
    return v;
}

static unsigned br_ue(BitReader *r)
{
    int zeros = 0;
    while (!br_bit(r)) {
        if (++zeros > 31) {
            r->pos = r->size_bits + 1;   /* mark as overrun */
            return 0;
        }
    }
    return ((1u << zeros) - 1) + br_bits(r, zeros);
}

static int br_se(BitReader *r)
{
    unsigned k = br_ue(r);
    return (k & 1) ? (int)((k + 1) >> 1) : -(int)(k >> 1);
}

static int br_overrun(const BitReader *r)
{
    return r->pos > r->size_bits;
}

/* ---------------------------------------------------------------- parameter sets */

typedef struct SPS {
    int valid;
    int profile_idc;
    int chroma_format_idc;
    int bit_depth_luma;
    int log2_max_frame_num;
    int poc_type;
    int log2_max_poc_lsb;
    int delta_pic_order_always_zero_flag;
    int frame_mbs_only_flag;
    int mb_aff;
    int direct_8x8_inference_flag;
    int max_num_ref_frames;
    int mb_width, mb_height;
    int width, height;           /* cropped */
} SPS;

typedef struct PPS {
    int valid;
    int sps_id;
    int cabac;
    int pic_order_present;
    int slice_group_count;
    int ref_count[2];
    int weighted_pred;
    int weighted_bipred_idc;
    int init_qp;
    int deblocking_filter_parameters_present;
    int redundant_pic_cnt_present;
    int transform_8x8_mode;
} PPS;

static void skip_scaling_list(BitReader *r, int size)
{
    int last = 8, next = 8, j;
    for (j = 0; j < size; j++) {
        if (next != 0)
            next = (last + br_se(r) + 256) & 255;
        last = next == 0 ? last : next;
    }
}

static int parse_sps(mvp_ctx *c, BitReader *r);
static int parse_pps(mvp_ctx *c, BitReader *r, int rbsp_bits);

/* ---------------------------------------------------------------- context */

#define MAX_DPB 33
#define MAX_SLICES 64

typedef struct RefPic {
    int frame_num;
    int poc;
    int long_idx;           /* LongTermFrameIdx, < 0 for short-term references */
} RefPic;

struct mvp_ctx {
    SPS sps[32];
    PPS pps[256];

    /* picture-level tables, laid out like FFmpeg's (mb_stride = mb_width + 1) */
    int mb_width, mb_height, mb_stride, b_stride;
    void *mem;
    uint32_t *mb_type;
    uint16_t *slice_table;
    uint16_t *cbp_table;
    uint8_t *chroma_pred_mode_table;
    uint8_t (*non_zero_count)[48];
    int8_t *ref_index;           /* 4 per mb_xy */
    uint8_t (*mvd_table)[2];     /* 8 per mb_xy: bottom row + right column */
    int16_t (*motion_val)[2];    /* one per 4x4 block, b_stride per row */
    int *mb2b_xy;

    uint8_t *rbsp;
    int rbsp_cap;
    mvp_mv *mvs;
    int mvs_cap;

    /* slice state */
    const SPS *cur_sps;
    const PPS *cur_pps;
    CABACContext cabac;
    uint8_t cabac_state[1024];
    int slice_num;
    int ref_count;
    int last_qscale_diff;
    int prev_mb_skipped;
    int mb_x, mb_y, mb_xy;
    int decode_chroma;

    /* neighbours of the current macroblock */
    int topleft_mb_xy, top_mb_xy, topright_mb_xy, left_mb_xy[2];
    uint32_t topleft_type, top_type, topright_type, left_type[2];
    const uint8_t *left_block;
    int topleft_partition;
    int left_cbp, top_cbp;
    int neighbor_transform_size;
    uint32_t sub_mb_type[4];

    /* reference picture tracking (only to know how far each picture's references are) */
    RefPic dpb[MAX_DPB];
    int n_dpb;
    int dpb_valid;          /* reference state known (since the last IDR) */
    int max_lt_idx;         /* MaxLongTermFrameIdx, -1 = no long-term frame indices */
    int prev_ref_poc_msb, prev_ref_poc_lsb;        /* POC type 0 */
    int prev_frame_num, prev_frame_num_offset;     /* POC type 2 */
    int last_poc, have_last_poc;
    int poc_step;           /* POC units per frame, learnt from the stream (0 = not yet known) */
    int max_ref_dist;       /* > 0: do not parse P slices whose references are all farther */
    int16_t slice_dist[MAX_SLICES][32];            /* frames to the picture of each ref_idx, 0 = unknown */

    int8_t ref_cache[5 * 8];
    int16_t mv_cache[5 * 8][2];
    uint8_t mvd_cache[5 * 8][2];
    uint8_t non_zero_count_cache[15 * 8];
};

#define LTOP 0
#define LBOT 1
#define LEFT(i) (i)

mvp_ctx *mvp_create(void)
{
    mvp_ctx *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->ref_cache[scan8[5]  + 1] =
    c->ref_cache[scan8[7]  + 1] =
    c->ref_cache[scan8[13] + 1] = PART_NOT_AVAILABLE;
    return c;
}

void mvp_free(mvp_ctx *c)
{
    if (!c)
        return;
    free(c->mem);
    free(c->rbsp);
    free(c->mvs);
    free(c);
}

/* (Re)allocate the picture tables for a new frame size. Every table gets a guard
 * area of mb_stride + 1 entries in front, so the top/left neighbours of the first
 * row and column can be indexed. */
static int alloc_tables(mvp_ctx *c, int mb_width, int mb_height)
{
    int stride = mb_width + 1;
    int guard = stride + 1;
    size_t n = (size_t)guard + stride * (mb_height + 1);
    size_t b_n = (size_t)mb_width * 4 * mb_height * 4;
    size_t off = 0, sz;
    uint8_t *m;

#define ALIGN(x) (((x) + 15) & ~(size_t)15)
    sz = ALIGN(n * 4) + ALIGN(n * 2) + ALIGN(n * 2) + ALIGN(n) + ALIGN(n * 48) +
         ALIGN(n * 4) + ALIGN(n * 16) + ALIGN(b_n * 4) + ALIGN(n * sizeof(int));
    free(c->mem);
    c->mem = m = calloc(1, sz);
    if (!m) {
        c->mb_width = c->mb_height = 0;
        return -1;
    }
    c->mb_type = (uint32_t *)(m + off) + guard;                    off += ALIGN(n * 4);
    c->slice_table = (uint16_t *)(m + off) + guard;                off += ALIGN(n * 2);
    c->cbp_table = (uint16_t *)(m + off) + guard;                  off += ALIGN(n * 2);
    c->chroma_pred_mode_table = (uint8_t *)(m + off) + guard;      off += ALIGN(n);
    c->non_zero_count = (uint8_t (*)[48])(m + off) + guard;        off += ALIGN(n * 48);
    c->ref_index = (int8_t *)(m + off) + 4 * guard;                off += ALIGN(n * 4);
    c->mvd_table = (uint8_t (*)[2])(m + off) + 8 * guard;          off += ALIGN(n * 16);
    c->motion_val = (int16_t (*)[2])(m + off);                     off += ALIGN(b_n * 4);
    c->mb2b_xy = (int *)(m + off) + guard;
#undef ALIGN
    c->mb_width = mb_width;
    c->mb_height = mb_height;
    c->mb_stride = stride;
    c->b_stride = mb_width * 4;
    for (int y = 0; y < mb_height; y++)
        for (int x = 0; x < mb_width; x++)
            c->mb2b_xy[x + y * stride] = 4 * x + 4 * y * c->b_stride;
    return 0;
}

/* ---------------------------------------------------------------- neighbours (FFmpeg h264_mvpred.h, no MBAFF) */

static const uint8_t left_block_options[16] = {
    0, 1, 2, 3, 7, 10, 8, 11, 3 + 0 * 4, 3 + 1 * 4, 3 + 2 * 4, 3 + 3 * 4, 1 + 4 * 4, 1 + 8 * 4, 1 + 5 * 4, 1 + 9 * 4
};

static av_always_inline int mid_pred(int a, int b, int c)
{
    if (a > b) {
        if (c > b) {
            if (c > a) b = a;
            else       b = c;
        }
    } else {
        if (b > c) {
            if (c > a) b = c;
            else       b = a;
        }
    }
    return b;
}

static void fill_decode_neighbors(mvp_ctx *c)
{
    const int mb_xy = c->mb_xy;
    int topleft_xy, top_xy, topright_xy, left_xy;

    c->topleft_partition = -1;
    top_xy = mb_xy - c->mb_stride;
    topleft_xy = top_xy - 1;
    topright_xy = top_xy + 1;
    left_xy = mb_xy - 1;
    c->left_block = left_block_options;

    c->topleft_mb_xy = topleft_xy;
    c->top_mb_xy = top_xy;
    c->topright_mb_xy = topright_xy;
    c->left_mb_xy[LTOP] = c->left_mb_xy[LBOT] = left_xy;

    c->topleft_type = c->mb_type[topleft_xy];
    c->top_type = c->mb_type[top_xy];
    c->topright_type = c->mb_type[topright_xy];
    c->left_type[LTOP] = c->left_type[LBOT] = c->mb_type[left_xy];

    if (c->slice_table[topleft_xy] != c->slice_num) {
        c->topleft_type = 0;
        if (c->slice_table[top_xy] != c->slice_num)
            c->top_type = 0;
        if (c->slice_table[left_xy] != c->slice_num)
            c->left_type[LTOP] = c->left_type[LBOT] = 0;
    }
    if (c->slice_table[topright_xy] != c->slice_num)
        c->topright_type = 0;
}

static av_always_inline void fill8(uint8_t *p, int w, int h, int v)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            p[x + 8 * y] = v;
}

static av_always_inline void fill_ref(int8_t *p, int w, int h, int v)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            p[x + 8 * y] = v;
}

static av_always_inline void fill_mv(int16_t (*p)[2], int w, int h, int mx, int my)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            p[x + 8 * y][0] = mx;
            p[x + 8 * y][1] = my;
        }
}

static av_always_inline void fill_mvd(uint8_t (*p)[2], int w, int h, int mx, int my)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            p[x + 8 * y][0] = mx;
            p[x + 8 * y][1] = my;
        }
}

static void fill_decode_caches(mvp_ctx *c, uint32_t mb_type)
{
    const int top_xy = c->top_mb_xy, topleft_xy = c->topleft_mb_xy, topright_xy = c->topright_mb_xy;
    const int left_xy[2] = { c->left_mb_xy[LTOP], c->left_mb_xy[LBOT] };
    const uint32_t top_type = c->top_type, topleft_type = c->topleft_type, topright_type = c->topright_type;
    const uint32_t left_type[2] = { c->left_type[LTOP], c->left_type[LBOT] };
    const uint8_t *left_block = c->left_block;
    uint8_t *nnz_cache = c->non_zero_count_cache;
    const uint8_t *nnz;
    int i;

    /* (the intra prediction availability and intra4x4 mode caches are not needed for parsing) */

    if (top_type) {
        nnz = c->non_zero_count[top_xy];
        memcpy(&nnz_cache[4 + 8 * 0], &nnz[4 * 3], 4);
        memcpy(&nnz_cache[4 + 8 * 5], &nnz[4 * 5], 4);
        memcpy(&nnz_cache[4 + 8 * 10], &nnz[4 * 9], 4);
    } else {
        int top_empty = !IS_INTRA(mb_type) ? 0 : 0x40;
        memset(&nnz_cache[4 + 8 * 0], top_empty, 4);
        memset(&nnz_cache[4 + 8 * 5], top_empty, 4);
        memset(&nnz_cache[4 + 8 * 10], top_empty, 4);
    }

    for (i = 0; i < 2; i++) {
        if (left_type[LEFT(i)]) {
            nnz = c->non_zero_count[left_xy[LEFT(i)]];
            nnz_cache[3 + 8 * 1 + 2 * 8 * i] = nnz[left_block[8 + 0 + 2 * i]];
            nnz_cache[3 + 8 * 2 + 2 * 8 * i] = nnz[left_block[8 + 1 + 2 * i]];
            nnz_cache[3 + 8 *  6 + 8 * i] = nnz[left_block[8 + 4 + 2 * i]];
            nnz_cache[3 + 8 * 11 + 8 * i] = nnz[left_block[8 + 5 + 2 * i]];
        } else {
            nnz_cache[3 + 8 *  1 + 2 * 8 * i] =
            nnz_cache[3 + 8 *  2 + 2 * 8 * i] =
            nnz_cache[3 + 8 *  6 + 2 * 8 * i] =
            nnz_cache[3 + 8 *  7 + 2 * 8 * i] =
            nnz_cache[3 + 8 * 11 + 2 * 8 * i] =
            nnz_cache[3 + 8 * 12 + 2 * 8 * i] = !IS_INTRA(mb_type) ? 0 : 64;
        }
    }

    if (top_type)
        c->top_cbp = c->cbp_table[top_xy];
    else
        c->top_cbp = IS_INTRA(mb_type) ? 0x7CF : 0x00F;
    if (left_type[LTOP]) {
        c->left_cbp =   (c->cbp_table[left_xy[LTOP]] & 0x7F0) |
                       ((c->cbp_table[left_xy[LTOP]] >> (left_block[0] & (~1))) & 2) |
                      (((c->cbp_table[left_xy[LBOT]] >> (left_block[2] & (~1))) & 2) << 2);
    } else {
        c->left_cbp = IS_INTRA(mb_type) ? 0x7CF : 0x00F;
    }

    if (IS_INTER(mb_type)) {
        const int b_stride = c->b_stride;
        int8_t *ref_cache = &c->ref_cache[scan8[0]];
        const int8_t *ref = c->ref_index;
        int16_t (*mv_cache)[2] = &c->mv_cache[scan8[0]];
        int16_t (*mv)[2] = c->motion_val;
        uint8_t (*mvd_cache)[2] = &c->mvd_cache[scan8[0]];
        uint8_t (*mvd)[2] = c->mvd_table;

        if (USES_LIST0(top_type)) {
            const int b_xy = c->mb2b_xy[top_xy] + 3 * b_stride;
            memcpy(mv_cache[0 - 1 * 8], mv[b_xy + 0], 16);
            ref_cache[0 - 1 * 8] =
            ref_cache[1 - 1 * 8] = ref[4 * top_xy + 2];
            ref_cache[2 - 1 * 8] =
            ref_cache[3 - 1 * 8] = ref[4 * top_xy + 3];
        } else {
            memset(mv_cache[0 - 1 * 8], 0, 16);
            memset(&ref_cache[0 - 1 * 8], top_type ? LIST_NOT_USED : PART_NOT_AVAILABLE, 4);
        }

        if (mb_type & (MB_TYPE_16x8 | MB_TYPE_8x8)) {
            for (i = 0; i < 2; i++) {
                int cache_idx = -1 + i * 2 * 8;
                if (USES_LIST0(left_type[LEFT(i)])) {
                    const int b_xy  = c->mb2b_xy[left_xy[LEFT(i)]] + 3;
                    const int b8_xy = 4 * left_xy[LEFT(i)] + 1;
                    memcpy(mv_cache[cache_idx], mv[b_xy + b_stride * left_block[0 + i * 2]], 4);
                    memcpy(mv_cache[cache_idx + 8], mv[b_xy + b_stride * left_block[1 + i * 2]], 4);
                    ref_cache[cache_idx]     = ref[b8_xy + (left_block[0 + i * 2] & ~1)];
                    ref_cache[cache_idx + 8] = ref[b8_xy + (left_block[1 + i * 2] & ~1)];
                } else {
                    memset(mv_cache[cache_idx], 0, 4);
                    memset(mv_cache[cache_idx + 8], 0, 4);
                    ref_cache[cache_idx]     =
                    ref_cache[cache_idx + 8] = left_type[LEFT(i)] ? LIST_NOT_USED : PART_NOT_AVAILABLE;
                }
            }
        } else {
            if (USES_LIST0(left_type[LTOP])) {
                const int b_xy  = c->mb2b_xy[left_xy[LTOP]] + 3;
                const int b8_xy = 4 * left_xy[LTOP] + 1;
                memcpy(mv_cache[-1], mv[b_xy + b_stride * left_block[0]], 4);
                ref_cache[-1] = ref[b8_xy + (left_block[0] & ~1)];
            } else {
                memset(mv_cache[-1], 0, 4);
                ref_cache[-1] = left_type[LTOP] ? LIST_NOT_USED : PART_NOT_AVAILABLE;
            }
        }

        if (USES_LIST0(topright_type)) {
            const int b_xy = c->mb2b_xy[topright_xy] + 3 * b_stride;
            memcpy(mv_cache[4 - 1 * 8], mv[b_xy], 4);
            ref_cache[4 - 1 * 8] = ref[4 * topright_xy + 2];
        } else {
            memset(mv_cache[4 - 1 * 8], 0, 4);
            ref_cache[4 - 1 * 8] = topright_type ? LIST_NOT_USED : PART_NOT_AVAILABLE;
        }
        if (ref_cache[2 - 1 * 8] < 0 || ref_cache[4 - 1 * 8] < 0) {
            if (USES_LIST0(topleft_type)) {
                const int b_xy  = c->mb2b_xy[topleft_xy] + 3 + b_stride +
                                  (c->topleft_partition & 2 * b_stride);
                const int b8_xy = 4 * topleft_xy + 1 + (c->topleft_partition & 2);
                memcpy(mv_cache[-1 - 1 * 8], mv[b_xy], 4);
                ref_cache[-1 - 1 * 8] = ref[b8_xy];
            } else {
                memset(mv_cache[-1 - 1 * 8], 0, 4);
                ref_cache[-1 - 1 * 8] = topleft_type ? LIST_NOT_USED : PART_NOT_AVAILABLE;
            }
        }

        ref_cache[2 + 8 * 0] =
        ref_cache[2 + 8 * 2] = PART_NOT_AVAILABLE;
        memset(mv_cache[2 + 8 * 0], 0, 4);
        memset(mv_cache[2 + 8 * 2], 0, 4);

        if (USES_LIST0(top_type)) {
            memcpy(mvd_cache[0 - 1 * 8], mvd[8 * top_xy + 0], 8);
        } else {
            memset(mvd_cache[0 - 1 * 8], 0, 8);
        }
        if (USES_LIST0(left_type[LTOP])) {
            const int b_xy = 8 * left_xy[LTOP] + 6;
            memcpy(mvd_cache[-1 + 0 * 8], mvd[b_xy - left_block[0]], 2);
            memcpy(mvd_cache[-1 + 1 * 8], mvd[b_xy - left_block[1]], 2);
        } else {
            memset(mvd_cache[-1 + 0 * 8], 0, 2);
            memset(mvd_cache[-1 + 1 * 8], 0, 2);
        }
        if (USES_LIST0(left_type[LBOT])) {
            const int b_xy = 8 * left_xy[LBOT] + 6;
            memcpy(mvd_cache[-1 + 2 * 8], mvd[b_xy - left_block[2]], 2);
            memcpy(mvd_cache[-1 + 3 * 8], mvd[b_xy - left_block[3]], 2);
        } else {
            memset(mvd_cache[-1 + 2 * 8], 0, 2);
            memset(mvd_cache[-1 + 3 * 8], 0, 2);
        }
        memset(mvd_cache[2 + 8 * 0], 0, 2);
        memset(mvd_cache[2 + 8 * 2], 0, 2);
    }

    c->neighbor_transform_size = !!IS_8x8DCT(top_type) + !!IS_8x8DCT(left_type[LTOP]);
}

static av_always_inline int fetch_diagonal_mv(mvp_ctx *c, const int16_t **C, int i, int part_width)
{
    const int topright_ref = c->ref_cache[i - 8 + part_width];

    if (topright_ref != PART_NOT_AVAILABLE) {
        *C = c->mv_cache[i - 8 + part_width];
        return topright_ref;
    } else {
        *C = c->mv_cache[i - 8 - 1];
        return c->ref_cache[i - 8 - 1];
    }
}

static av_always_inline void pred_motion(mvp_ctx *c, int n, int part_width, int ref, int *mx, int *my)
{
    const int index8 = scan8[n];
    const int top_ref = c->ref_cache[index8 - 8];
    const int left_ref = c->ref_cache[index8 - 1];
    const int16_t *const A = c->mv_cache[index8 - 1];
    const int16_t *const B = c->mv_cache[index8 - 8];
    const int16_t *C;
    int diagonal_ref, match_count;

    diagonal_ref = fetch_diagonal_mv(c, &C, index8, part_width);
    match_count = (diagonal_ref == ref) + (top_ref == ref) + (left_ref == ref);
    if (match_count > 1) {
        *mx = mid_pred(A[0], B[0], C[0]);
        *my = mid_pred(A[1], B[1], C[1]);
    } else if (match_count == 1) {
        if (left_ref == ref) {
            *mx = A[0];
            *my = A[1];
        } else if (top_ref == ref) {
            *mx = B[0];
            *my = B[1];
        } else {
            *mx = C[0];
            *my = C[1];
        }
    } else {
        if (top_ref == PART_NOT_AVAILABLE &&
            diagonal_ref == PART_NOT_AVAILABLE &&
            left_ref != PART_NOT_AVAILABLE) {
            *mx = A[0];
            *my = A[1];
        } else {
            *mx = mid_pred(A[0], B[0], C[0]);
            *my = mid_pred(A[1], B[1], C[1]);
        }
    }
}

static av_always_inline void pred_16x8_motion(mvp_ctx *c, int n, int ref, int *mx, int *my)
{
    if (n == 0) {
        const int top_ref = c->ref_cache[scan8[0] - 8];
        const int16_t *const B = c->mv_cache[scan8[0] - 8];
        if (top_ref == ref) {
            *mx = B[0];
            *my = B[1];
            return;
        }
    } else {
        const int left_ref = c->ref_cache[scan8[8] - 1];
        const int16_t *const A = c->mv_cache[scan8[8] - 1];
        if (left_ref == ref) {
            *mx = A[0];
            *my = A[1];
            return;
        }
    }
    pred_motion(c, n, 4, ref, mx, my);
}

static av_always_inline void pred_8x16_motion(mvp_ctx *c, int n, int ref, int *mx, int *my)
{
    if (n == 0) {
        const int left_ref = c->ref_cache[scan8[0] - 1];
        const int16_t *const A = c->mv_cache[scan8[0] - 1];
        if (left_ref == ref) {
            *mx = A[0];
            *my = A[1];
            return;
        }
    } else {
        const int16_t *C;
        int diagonal_ref = fetch_diagonal_mv(c, &C, scan8[4], 2);
        if (diagonal_ref == ref) {
            *mx = C[0];
            *my = C[1];
            return;
        }
    }
    pred_motion(c, n, 2, ref, mx, my);
}

static void pred_pskip_motion(mvp_ctx *c)
{
    static const int16_t zeromv[2] = { 0 };
    const int8_t *ref = c->ref_index;
    int16_t (*mv)[2] = c->motion_val;
    int top_ref, left_ref, diagonal_ref, match_count, mx, my;
    const int16_t *A, *B, *C;
    const int b_stride = c->b_stride;

    fill_ref(&c->ref_cache[scan8[0]], 4, 4, 0);

    if (USES_LIST0(c->left_type[LTOP])) {
        left_ref = ref[4 * c->left_mb_xy[LTOP] + 1 + (c->left_block[0] & ~1)];
        A = mv[c->mb2b_xy[c->left_mb_xy[LTOP]] + 3 + b_stride * c->left_block[0]];
        if (!left_ref && !A[0] && !A[1])
            goto zeromv;
    } else if (c->left_type[LTOP]) {
        left_ref = LIST_NOT_USED;
        A = zeromv;
    } else {
        goto zeromv;
    }

    if (USES_LIST0(c->top_type)) {
        top_ref = ref[4 * c->top_mb_xy + 2];
        B = mv[c->mb2b_xy[c->top_mb_xy] + 3 * b_stride];
        if (!top_ref && !B[0] && !B[1])
            goto zeromv;
    } else if (c->top_type) {
        top_ref = LIST_NOT_USED;
        B = zeromv;
    } else {
        goto zeromv;
    }

    if (USES_LIST0(c->topright_type)) {
        diagonal_ref = ref[4 * c->topright_mb_xy + 2];
        C = mv[c->mb2b_xy[c->topright_mb_xy] + 3 * b_stride];
    } else if (c->topright_type) {
        diagonal_ref = LIST_NOT_USED;
        C = zeromv;
    } else {
        if (USES_LIST0(c->topleft_type)) {
            diagonal_ref = ref[4 * c->topleft_mb_xy + 1 + (c->topleft_partition & 2)];
            C = mv[c->mb2b_xy[c->topleft_mb_xy] + 3 + b_stride + (c->topleft_partition & 2 * b_stride)];
        } else if (c->topleft_type) {
            diagonal_ref = LIST_NOT_USED;
            C = zeromv;
        } else {
            diagonal_ref = PART_NOT_AVAILABLE;
            C = zeromv;
        }
    }

    match_count = !diagonal_ref + !top_ref + !left_ref;
    if (match_count > 1) {
        mx = mid_pred(A[0], B[0], C[0]);
        my = mid_pred(A[1], B[1], C[1]);
    } else if (match_count == 1) {
        if (!left_ref) {
            mx = A[0];
            my = A[1];
        } else if (!top_ref) {
            mx = B[0];
            my = B[1];
        } else {
            mx = C[0];
            my = C[1];
        }
    } else {
        mx = mid_pred(A[0], B[0], C[0]);
        my = mid_pred(A[1], B[1], C[1]);
    }

    fill_mv(&c->mv_cache[scan8[0]], 4, 4, mx, my);
    return;

zeromv:
    fill_mv(&c->mv_cache[scan8[0]], 4, 4, 0, 0);
}

static void write_back_motion(mvp_ctx *c, uint32_t mb_type)
{
    const int b_stride = c->b_stride;
    const int b_xy = 4 * c->mb_x + 4 * c->mb_y * b_stride;
    const int b8_xy = 4 * c->mb_xy;
    int y;

    if (USES_LIST0(mb_type)) {
        int16_t (*mv_dst)[2] = &c->motion_val[b_xy];
        int16_t (*mv_src)[2] = &c->mv_cache[scan8[0]];
        uint8_t (*mvd_dst)[2] = &c->mvd_table[8 * c->mb_xy];
        uint8_t (*mvd_src)[2] = &c->mvd_cache[scan8[0]];
        int8_t *ref_index = &c->ref_index[b8_xy];

        for (y = 0; y < 4; y++)
            memcpy(mv_dst + y * b_stride, mv_src + 8 * y, 16);
        if (IS_SKIP(mb_type)) {
            memset(mvd_dst, 0, 16);
        } else {
            memcpy(mvd_dst, mvd_src + 8 * 3, 8);
            memcpy(mvd_dst + 3 + 3, mvd_src + 3 + 8 * 0, 2);
            memcpy(mvd_dst + 3 + 2, mvd_src + 3 + 8 * 1, 2);
            memcpy(mvd_dst + 3 + 1, mvd_src + 3 + 8 * 2, 2);
        }
        ref_index[0 + 0 * 2] = c->ref_cache[scan8[0]];
        ref_index[1 + 0 * 2] = c->ref_cache[scan8[4]];
        ref_index[0 + 1 * 2] = c->ref_cache[scan8[8]];
        ref_index[1 + 1 * 2] = c->ref_cache[scan8[12]];
    } else {
        memset(&c->ref_index[b8_xy], LIST_NOT_USED, 4);
    }
}

static void decode_mb_skip(mvp_ctx *c)
{
    const int mb_xy = c->mb_xy;
    const uint32_t mb_type = MB_TYPE_16x16 | MB_TYPE_P0L0 | MB_TYPE_P1L0 | MB_TYPE_SKIP;

    memset(c->non_zero_count[mb_xy], 0, 48);
    fill_decode_neighbors(c);
    pred_pskip_motion(c);
    write_back_motion(c, mb_type);
    c->mb_type[mb_xy] = mb_type;
    c->slice_table[mb_xy] = c->slice_num;
    c->prev_mb_skipped = 1;
}

/* ---------------------------------------------------------------- syntax elements (FFmpeg h264_cabac.c) */

static int decode_cabac_intra_mb_type(mvp_ctx *c, int ctx_base, int intra_slice)
{
    uint8_t *state = &c->cabac_state[ctx_base];
    int mb_type;

    if (intra_slice) {
        int ctx = 0;
        if (c->left_type[LTOP] & (MB_TYPE_INTRA16x16 | MB_TYPE_INTRA_PCM))
            ctx++;
        if (c->top_type & (MB_TYPE_INTRA16x16 | MB_TYPE_INTRA_PCM))
            ctx++;
        if (get_cabac_noinline(&c->cabac, &state[ctx]) == 0)
            return 0;   /* I4x4 */
        state += 2;
    } else {
        if (get_cabac_noinline(&c->cabac, state) == 0)
            return 0;   /* I4x4 */
    }

    if (get_cabac_terminate(&c->cabac))
        return 25;  /* PCM */

    mb_type = 1; /* I16x16 */
    mb_type += 12 * get_cabac_noinline(&c->cabac, &state[1]); /* cbp_luma != 0 */
    if (get_cabac_noinline(&c->cabac, &state[2])) /* cbp_chroma */
        mb_type += 4 + 4 * get_cabac_noinline(&c->cabac, &state[2 + intra_slice]);
    mb_type += 2 * get_cabac_noinline(&c->cabac, &state[3 + intra_slice]);
    mb_type += 1 * get_cabac_noinline(&c->cabac, &state[3 + 2 * intra_slice]);
    return mb_type;
}

static int decode_cabac_mb_skip(mvp_ctx *c)
{
    const int mba_xy = c->mb_xy - 1;
    const int mbb_xy = c->mb_xy - c->mb_stride;
    int ctx = 0;

    if (c->slice_table[mba_xy] == c->slice_num && !IS_SKIP(c->mb_type[mba_xy]))
        ctx++;
    if (c->slice_table[mbb_xy] == c->slice_num && !IS_SKIP(c->mb_type[mbb_xy]))
        ctx++;
    return get_cabac_noinline(&c->cabac, &c->cabac_state[11 + ctx]);
}

/* the decoded mode itself is irrelevant: only the number of bins matters */
static void skip_cabac_mb_intra4x4_pred_mode(mvp_ctx *c)
{
    if (get_cabac(&c->cabac, &c->cabac_state[68]))
        return;
    get_cabac(&c->cabac, &c->cabac_state[69]);
    get_cabac(&c->cabac, &c->cabac_state[69]);
    get_cabac(&c->cabac, &c->cabac_state[69]);
}

static int decode_cabac_mb_chroma_pre_mode(mvp_ctx *c)
{
    const int mba_xy = c->left_mb_xy[0];
    const int mbb_xy = c->top_mb_xy;
    int ctx = 0;

    if (c->left_type[LTOP] && c->chroma_pred_mode_table[mba_xy] != 0)
        ctx++;
    if (c->top_type && c->chroma_pred_mode_table[mbb_xy] != 0)
        ctx++;

    if (get_cabac_noinline(&c->cabac, &c->cabac_state[64 + ctx]) == 0)
        return 0;
    if (get_cabac_noinline(&c->cabac, &c->cabac_state[64 + 3]) == 0)
        return 1;
    if (get_cabac_noinline(&c->cabac, &c->cabac_state[64 + 3]) == 0)
        return 2;
    else
        return 3;
}

static int decode_cabac_mb_cbp_luma(mvp_ctx *c)
{
    int cbp_b, cbp_a, ctx, cbp = 0;

    cbp_a = c->left_cbp;
    cbp_b = c->top_cbp;

    ctx = !(cbp_a & 0x02) + 2 * !(cbp_b & 0x04);
    cbp += get_cabac_noinline(&c->cabac, &c->cabac_state[73 + ctx]);
    ctx = !(cbp   & 0x01) + 2 * !(cbp_b & 0x08);
    cbp += get_cabac_noinline(&c->cabac, &c->cabac_state[73 + ctx]) << 1;
    ctx = !(cbp_a & 0x08) + 2 * !(cbp   & 0x01);
    cbp += get_cabac_noinline(&c->cabac, &c->cabac_state[73 + ctx]) << 2;
    ctx = !(cbp   & 0x04) + 2 * !(cbp   & 0x02);
    cbp += get_cabac_noinline(&c->cabac, &c->cabac_state[73 + ctx]) << 3;
    return cbp;
}

static int decode_cabac_mb_cbp_chroma(mvp_ctx *c)
{
    int ctx;
    int cbp_a, cbp_b;

    cbp_a = (c->left_cbp >> 4) & 0x03;
    cbp_b = (c->top_cbp  >> 4) & 0x03;

    ctx = 0;
    if (cbp_a > 0) ctx++;
    if (cbp_b > 0) ctx += 2;
    if (get_cabac_noinline(&c->cabac, &c->cabac_state[77 + ctx]) == 0)
        return 0;

    ctx = 4;
    if (cbp_a == 2) ctx++;
    if (cbp_b == 2) ctx += 2;
    return 1 + get_cabac_noinline(&c->cabac, &c->cabac_state[77 + ctx]);
}

static int decode_cabac_p_mb_sub_type(mvp_ctx *c)
{
    if (get_cabac(&c->cabac, &c->cabac_state[21]))
        return 0;   /* 8x8 */
    if (!get_cabac(&c->cabac, &c->cabac_state[22]))
        return 1;   /* 8x4 */
    if (get_cabac(&c->cabac, &c->cabac_state[23]))
        return 2;   /* 4x8 */
    return 3;       /* 4x4 */
}

static int decode_cabac_mb_ref(mvp_ctx *c, int n)
{
    int refa = c->ref_cache[scan8[n] - 1];
    int refb = c->ref_cache[scan8[n] - 8];
    int ref = 0;
    int ctx = 0;

    if (refa > 0)
        ctx++;
    if (refb > 0)
        ctx += 2;

    while (get_cabac(&c->cabac, &c->cabac_state[54 + ctx])) {
        ref++;
        ctx = (ctx >> 2) + 4;
        if (ref >= 32)
            return -1;
    }
    return ref;
}

#define INT_BIT 32
#define MVD_ERROR (-0x7FFFFFFF - 1)

static int decode_cabac_mb_mvd(mvp_ctx *c, int ctxbase, int amvd, int *mvda)
{
    int mvd;

    if (!get_cabac(&c->cabac, &c->cabac_state[ctxbase + ((amvd - 3) >> (INT_BIT - 1)) + ((amvd - 33) >> (INT_BIT - 1)) + 2])) {
        *mvda = 0;
        return 0;
    }

    mvd = 1;
    ctxbase += 3;
    while (mvd < 9 && get_cabac(&c->cabac, &c->cabac_state[ctxbase])) {
        if (mvd < 4)
            ctxbase++;
        mvd++;
    }

    if (mvd >= 9) {
        int k = 3;
        while (get_cabac_bypass(&c->cabac)) {
            mvd += 1 << k;
            k++;
            if (k > 24)
                return MVD_ERROR;
        }
        while (k--)
            mvd += get_cabac_bypass(&c->cabac) << k;
        *mvda = mvd < 70 ? mvd : 70;
    } else
        *mvda = mvd;
    return get_cabac_bypass_sign(&c->cabac, -mvd);
}

#define DECODE_CABAC_MB_MVD(c, n)                                       \
{                                                                       \
    int amvd0 = c->mvd_cache[scan8[n] - 1][0] +                         \
                c->mvd_cache[scan8[n] - 8][0];                          \
    int amvd1 = c->mvd_cache[scan8[n] - 1][1] +                         \
                c->mvd_cache[scan8[n] - 8][1];                          \
                                                                        \
    int mxd = decode_cabac_mb_mvd(c, 40, amvd0, &mpx);                  \
    int myd = decode_cabac_mb_mvd(c, 47, amvd1, &mpy);                  \
    if (mxd == MVD_ERROR || myd == MVD_ERROR)                           \
        return -1;                                                      \
    mx += mxd;                                                          \
    my += myd;                                                          \
}

/* ---------------------------------------------------------------- residual (counted, values dropped) */

static av_always_inline int get_cabac_cbf_ctx(mvp_ctx *c, int cat, int idx, int is_dc)
{
    int nza, nzb;
    int ctx = 0;
    static const uint16_t base_ctx[14] = { 85, 89, 93, 97, 101, 1012, 460, 464, 468, 1016, 472, 476, 480, 1020 };

    if (is_dc) {
        if (cat == 3) {
            idx -= CHROMA_DC_BLOCK_INDEX;
            nza = (c->left_cbp >> (6 + idx)) & 0x01;
            nzb = (c->top_cbp  >> (6 + idx)) & 0x01;
        } else {
            idx -= LUMA_DC_BLOCK_INDEX;
            nza = c->left_cbp & (0x100 << idx);
            nzb = c->top_cbp  & (0x100 << idx);
        }
    } else {
        nza = c->non_zero_count_cache[scan8[idx] - 1];
        nzb = c->non_zero_count_cache[scan8[idx] - 8];
    }

    if (nza > 0)
        ctx++;
    if (nzb > 0)
        ctx += 2;

    return base_ctx[cat] + ctx;
}

static av_always_inline void decode_cabac_residual_internal(mvp_ctx *c, int cat, int n,
                                                            int max_coeff, int is_dc)
{
    static const int significant_coeff_flag_offset[14] = {
        105+0, 105+15, 105+29, 105+44, 105+47, 402, 484+0, 484+15, 484+29, 660, 528+0, 528+15, 528+29, 718
    };
    static const int last_coeff_flag_offset[14] = {
        166+0, 166+15, 166+29, 166+44, 166+47, 417, 572+0, 572+15, 572+29, 690, 616+0, 616+15, 616+29, 748
    };
    static const int coeff_abs_level_m1_offset[14] = {
        227+0, 227+10, 227+20, 227+30, 227+39, 426, 952+0, 952+10, 952+20, 708, 982+0, 982+10, 982+20, 766
    };
    static const uint8_t significant_coeff_flag_offset_8x8[63] = {
        0, 1, 2, 3, 4, 5, 5, 4, 4, 3, 3, 4, 4, 4, 5, 5,
        4, 4, 4, 4, 3, 3, 6, 7, 7, 7, 8, 9,10, 9, 8, 7,
        7, 6,11,12,13,11, 6, 7, 8, 9,14,10, 9, 8, 6,11,
       12,13,11, 6, 9,14,10, 9,11,12,13,11,14,10,12
    };
    static const uint8_t coeff_abs_level1_ctx[8] = { 1, 2, 3, 4, 0, 0, 0, 0 };
    static const uint8_t coeff_abs_levelgt1_ctx[8] = { 5, 5, 5, 5, 6, 7, 8, 9 };
    static const uint8_t coeff_abs_level_transition[2][8] = {
        { 1, 2, 3, 3, 4, 5, 6, 7 },
        { 4, 4, 4, 4, 5, 6, 7, 7 }
    };

    int last;
    int coeff_count = 0;
    int node_ctx = 0;
    uint8_t *significant_coeff_ctx_base;
    uint8_t *last_coeff_ctx_base;
    uint8_t *abs_level_m1_ctx_base;
    CABACContext cc = c->cabac;
#define CC &cc

    significant_coeff_ctx_base = c->cabac_state + significant_coeff_flag_offset[cat];
    last_coeff_ctx_base = c->cabac_state + last_coeff_flag_offset[cat];
    abs_level_m1_ctx_base = c->cabac_state + coeff_abs_level_m1_offset[cat];

#define DECODE_SIGNIFICANCE(coefs, sig_off, last_off)               \
        for (last = 0; last < coefs; last++) {                      \
            uint8_t *sig_ctx = significant_coeff_ctx_base + sig_off; \
            if (get_cabac(CC, sig_ctx)) {                           \
                uint8_t *last_ctx = last_coeff_ctx_base + last_off; \
                coeff_count++;                                      \
                if (get_cabac(CC, last_ctx)) {                      \
                    last = max_coeff;                               \
                    break;                                          \
                }                                                   \
            }                                                       \
        }                                                           \
        if (last == max_coeff - 1)                                  \
            coeff_count++;

    if (!is_dc && max_coeff == 64) {
        const uint8_t *sig_off = significant_coeff_flag_offset_8x8;
        DECODE_SIGNIFICANCE(63, sig_off[last], ff_h264_last_coeff_flag_offset_8x8[last]);
    } else {
        DECODE_SIGNIFICANCE(max_coeff - 1, last, last);
    }

    if (is_dc) {
        if (cat == 3)
            c->cbp_table[c->mb_xy] |= 0x40 << (n - CHROMA_DC_BLOCK_INDEX);
        else
            c->cbp_table[c->mb_xy] |= 0x100 << (n - LUMA_DC_BLOCK_INDEX);
        c->non_zero_count_cache[scan8[n]] = coeff_count;
    } else {
        if (max_coeff == 64)
            fill8(&c->non_zero_count_cache[scan8[n]], 2, 2, coeff_count);
        else
            c->non_zero_count_cache[scan8[n]] = coeff_count;
    }

    do {
        uint8_t *ctx = coeff_abs_level1_ctx[node_ctx] + abs_level_m1_ctx_base;

        if (get_cabac(CC, ctx) == 0) {
            node_ctx = coeff_abs_level_transition[0][node_ctx];
            get_cabac_bypass(CC);                        /* sign */
        } else {
            unsigned coeff_abs = 2;
            ctx = coeff_abs_levelgt1_ctx[node_ctx] + abs_level_m1_ctx_base;
            node_ctx = coeff_abs_level_transition[1][node_ctx];

            while (coeff_abs < 15 && get_cabac(CC, ctx))
                coeff_abs++;

            if (coeff_abs >= 15) {
                int j = 0;
                while (get_cabac_bypass(CC) && j < 16 + 7)
                    j++;
                while (j--)
                    get_cabac_bypass(CC);
            }
            get_cabac_bypass(CC);                        /* sign */
        }
    } while (--coeff_count);
#undef CC
#undef DECODE_SIGNIFICANCE
    c->cabac = cc;
}

static av_noinline void decode_cabac_residual_dc_internal(mvp_ctx *c, int cat, int n, int max_coeff)
{
    decode_cabac_residual_internal(c, cat, n, max_coeff, 1);
}

static av_noinline void decode_cabac_residual_nondc_internal(mvp_ctx *c, int cat, int n, int max_coeff)
{
    decode_cabac_residual_internal(c, cat, n, max_coeff, 0);
}

static av_always_inline void decode_cabac_residual_dc(mvp_ctx *c, int cat, int n, int max_coeff)
{
    if (get_cabac(&c->cabac, &c->cabac_state[get_cabac_cbf_ctx(c, cat, n, 1)]) == 0) {
        c->non_zero_count_cache[scan8[n]] = 0;
        return;
    }
    decode_cabac_residual_dc_internal(c, cat, n, max_coeff);
}

static av_always_inline void decode_cabac_residual_nondc(mvp_ctx *c, int cat, int n, int max_coeff)
{
    /* 8x8 luma blocks (cat 5) have no coded_block_flag outside 4:4:4 */
    if (cat != 5 && get_cabac(&c->cabac, &c->cabac_state[get_cabac_cbf_ctx(c, cat, n, 0)]) == 0) {
        c->non_zero_count_cache[scan8[n]] = 0;
        return;
    }
    decode_cabac_residual_nondc_internal(c, cat, n, max_coeff);
}

static av_always_inline void decode_cabac_luma_residual(mvp_ctx *c, uint32_t mb_type, int cbp)
{
    int i8x8, i4x4;
    if (IS_INTRA16x16(mb_type)) {
        decode_cabac_residual_dc(c, 0, LUMA_DC_BLOCK_INDEX, 16);
        if (cbp & 15) {
            for (i4x4 = 0; i4x4 < 16; i4x4++)
                decode_cabac_residual_nondc(c, 1, i4x4, 15);
        } else {
            fill8(&c->non_zero_count_cache[scan8[0]], 4, 4, 0);
        }
    } else {
        for (i8x8 = 0; i8x8 < 4; i8x8++) {
            if (cbp & (1 << i8x8)) {
                if (IS_8x8DCT(mb_type)) {
                    decode_cabac_residual_nondc(c, 5, 4 * i8x8, 64);
                } else {
                    for (i4x4 = 0; i4x4 < 4; i4x4++)
                        decode_cabac_residual_nondc(c, 2, 4 * i8x8 + i4x4, 16);
                }
            } else {
                fill8(&c->non_zero_count_cache[scan8[4 * i8x8]], 2, 2, 0);
            }
        }
    }
}

static void write_back_non_zero_count(mvp_ctx *c)
{
    uint8_t *nnz = c->non_zero_count[c->mb_xy];
    const uint8_t *nnz_cache = c->non_zero_count_cache;

    memcpy(&nnz[ 0], &nnz_cache[4 + 8 *  1], 4);
    memcpy(&nnz[ 4], &nnz_cache[4 + 8 *  2], 4);
    memcpy(&nnz[ 8], &nnz_cache[4 + 8 *  3], 4);
    memcpy(&nnz[12], &nnz_cache[4 + 8 *  4], 4);
    memcpy(&nnz[16], &nnz_cache[4 + 8 *  6], 4);
    memcpy(&nnz[20], &nnz_cache[4 + 8 *  7], 4);
    memcpy(&nnz[32], &nnz_cache[4 + 8 * 11], 4);
    memcpy(&nnz[36], &nnz_cache[4 + 8 * 12], 4);
}

static av_always_inline int get_dct8x8_allowed(mvp_ctx *c)
{
    const uint32_t m = MB_TYPE_16x8 | MB_TYPE_8x16 | MB_TYPE_8x8 |
                       (c->cur_sps->direct_8x8_inference_flag ? 0 : MB_TYPE_DIRECT2);
    return !((c->sub_mb_type[0] | c->sub_mb_type[1] | c->sub_mb_type[2] | c->sub_mb_type[3]) & m);
}

/* ---------------------------------------------------------------- macroblock (FFmpeg ff_h264_decode_mb_cabac) */

static int decode_mb_cabac(mvp_ctx *c, int slice_type_p)
{
    const int mb_xy = c->mb_xy = c->mb_x + c->mb_y * c->mb_stride;
    uint32_t mb_type;
    int partition_count, cbp = 0;
    int dct8x8_allowed = c->cur_pps->transform_8x8_mode;

    if (slice_type_p) {
        if (decode_cabac_mb_skip(c)) {
            decode_mb_skip(c);
            c->cbp_table[mb_xy] = 0;
            c->chroma_pred_mode_table[mb_xy] = 0;
            c->last_qscale_diff = 0;
            return 0;
        }
    }

    c->prev_mb_skipped = 0;
    fill_decode_neighbors(c);

    if (slice_type_p) {
        if (get_cabac_noinline(&c->cabac, &c->cabac_state[14]) == 0) {
            int t;
            if (get_cabac_noinline(&c->cabac, &c->cabac_state[15]) == 0)
                t = 3 * get_cabac_noinline(&c->cabac, &c->cabac_state[16]);    /* P_L0_D16x16, P_8x8 */
            else
                t = 2 - get_cabac_noinline(&c->cabac, &c->cabac_state[17]);    /* P_L0_D8x16, P_L0_D16x8 */
            partition_count = p_mb_type_info[t].partition_count;
            mb_type = p_mb_type_info[t].type;
        } else {
            IMbInfo info = i_mb_type_info(decode_cabac_intra_mb_type(c, 17, 0));
            partition_count = 0;
            cbp = info.cbp;
            mb_type = info.type;
        }
    } else {
        IMbInfo info = i_mb_type_info(decode_cabac_intra_mb_type(c, 3, 1));
        partition_count = 0;
        cbp = info.cbp;
        mb_type = info.type;
    }

    c->slice_table[mb_xy] = c->slice_num;

    if (IS_INTRA_PCM(mb_type)) {
        const int mb_size = (c->decode_chroma ? 384 : 256) * c->cur_sps->bit_depth_luma >> 3;
        const uint8_t *ptr;

        ptr = c->cabac.bytestream;
        if (c->cabac.low & 0x1)
            ptr--;
        if (c->cabac.low & 0x1FF)
            ptr--;
        if ((int)(c->cabac.bytestream_end - ptr) < mb_size)
            return -1;
        ptr += mb_size;
        if (init_cabac_decoder(&c->cabac, ptr, c->cabac.bytestream_end - ptr) < 0)
            return -1;

        c->cbp_table[mb_xy] = 0xf7ef;
        c->chroma_pred_mode_table[mb_xy] = 0;
        memset(c->non_zero_count[mb_xy], 16, 48);
        c->mb_type[mb_xy] = mb_type;
        c->last_qscale_diff = 0;
        return 0;
    }

    fill_decode_caches(c, mb_type);

    if (IS_INTRA(mb_type)) {
        int i;
        if (IS_INTRA4x4(mb_type)) {
            if (dct8x8_allowed && get_cabac_noinline(&c->cabac, &c->cabac_state[399 + c->neighbor_transform_size])) {
                mb_type |= MB_TYPE_8x8DCT;
                for (i = 0; i < 16; i += 4)
                    skip_cabac_mb_intra4x4_pred_mode(c);
            } else {
                for (i = 0; i < 16; i++)
                    skip_cabac_mb_intra4x4_pred_mode(c);
            }
        }
        if (c->decode_chroma)
            c->chroma_pred_mode_table[mb_xy] = decode_cabac_mb_chroma_pre_mode(c);
    } else if (partition_count == 4) {
        int i, j, sub_partition_count[4], ref[4];

        for (i = 0; i < 4; i++) {
            int t = decode_cabac_p_mb_sub_type(c);
            sub_partition_count[i] = p_sub_mb_type_info[t].partition_count;
            c->sub_mb_type[i] = p_sub_mb_type_info[t].type;
        }

        for (i = 0; i < 4; i++) {
            if (IS_DIR(c->sub_mb_type[i], 0)) {
                unsigned rc = c->ref_count;
                if (rc > 1) {
                    ref[i] = decode_cabac_mb_ref(c, 4 * i);
                    if ((unsigned)ref[i] >= rc)
                        return -1;
                } else
                    ref[i] = 0;
            } else {
                ref[i] = -1;
            }
            c->ref_cache[scan8[4 * i] + 1] =
            c->ref_cache[scan8[4 * i] + 8] = c->ref_cache[scan8[4 * i] + 9] = ref[i];
        }

        if (dct8x8_allowed)
            dct8x8_allowed = get_dct8x8_allowed(c);

        for (i = 0; i < 4; i++) {
            c->ref_cache[scan8[4 * i]] = c->ref_cache[scan8[4 * i] + 1];
            if (IS_DIR(c->sub_mb_type[i], 0)) {
                const uint32_t sub_mb_type = c->sub_mb_type[i];
                const int block_width = (sub_mb_type & (MB_TYPE_16x16 | MB_TYPE_16x8)) ? 2 : 1;
                for (j = 0; j < sub_partition_count[i]; j++) {
                    int mpx, mpy;
                    int mx, my;
                    const int index = 4 * i + block_width * j;
                    int16_t (*mv_cache)[2] = &c->mv_cache[scan8[index]];
                    uint8_t (*mvd_cache)[2] = &c->mvd_cache[scan8[index]];
                    pred_motion(c, index, block_width, c->ref_cache[scan8[index]], &mx, &my);
                    DECODE_CABAC_MB_MVD(c, index)

                    if (IS_SUB_8X8(sub_mb_type)) {
                        mv_cache[1][0] =
                        mv_cache[8][0] = mv_cache[9][0] = mx;
                        mv_cache[1][1] =
                        mv_cache[8][1] = mv_cache[9][1] = my;

                        mvd_cache[1][0] =
                        mvd_cache[8][0] = mvd_cache[9][0] = mpx;
                        mvd_cache[1][1] =
                        mvd_cache[8][1] = mvd_cache[9][1] = mpy;
                    } else if (IS_SUB_8X4(sub_mb_type)) {
                        mv_cache[1][0] = mx;
                        mv_cache[1][1] = my;

                        mvd_cache[1][0] = mpx;
                        mvd_cache[1][1] = mpy;
                    } else if (IS_SUB_4X8(sub_mb_type)) {
                        mv_cache[8][0] = mx;
                        mv_cache[8][1] = my;

                        mvd_cache[8][0] = mpx;
                        mvd_cache[8][1] = mpy;
                    }
                    mv_cache[0][0] = mx;
                    mv_cache[0][1] = my;

                    mvd_cache[0][0] = mpx;
                    mvd_cache[0][1] = mpy;
                }
            } else {
                fill_mv(&c->mv_cache[scan8[4 * i]], 2, 2, 0, 0);
                fill_mvd(&c->mvd_cache[scan8[4 * i]], 2, 2, 0, 0);
            }
        }
    } else {
        int i;
        if (IS_16X16(mb_type)) {
            int ref, mx, my, mpx, mpy;
            unsigned rc = c->ref_count;
            if (rc > 1) {
                ref = decode_cabac_mb_ref(c, 0);
                if ((unsigned)ref >= rc)
                    return -1;
            } else
                ref = 0;
            fill_ref(&c->ref_cache[scan8[0]], 4, 4, ref);
            pred_motion(c, 0, 4, c->ref_cache[scan8[0]], &mx, &my);
            DECODE_CABAC_MB_MVD(c, 0)
            fill_mvd(&c->mvd_cache[scan8[0]], 4, 4, mpx, mpy);
            fill_mv(&c->mv_cache[scan8[0]], 4, 4, mx, my);
        } else if (IS_16X8(mb_type)) {
            for (i = 0; i < 2; i++) {
                int ref;
                unsigned rc = c->ref_count;
                if (rc > 1) {
                    ref = decode_cabac_mb_ref(c, 8 * i);
                    if ((unsigned)ref >= rc)
                        return -1;
                } else
                    ref = 0;
                fill_ref(&c->ref_cache[scan8[0] + 16 * i], 4, 2, ref);
            }
            for (i = 0; i < 2; i++) {
                int mx, my, mpx, mpy;
                pred_16x8_motion(c, 8 * i, c->ref_cache[scan8[0] + 16 * i], &mx, &my);
                DECODE_CABAC_MB_MVD(c, 8 * i)
                fill_mvd(&c->mvd_cache[scan8[0] + 16 * i], 4, 2, mpx, mpy);
                fill_mv(&c->mv_cache[scan8[0] + 16 * i], 4, 2, mx, my);
            }
        } else { /* 8x16 */
            for (i = 0; i < 2; i++) {
                int ref;
                unsigned rc = c->ref_count;
                if (rc > 1) {
                    ref = decode_cabac_mb_ref(c, 4 * i);
                    if ((unsigned)ref >= rc)
                        return -1;
                } else
                    ref = 0;
                fill_ref(&c->ref_cache[scan8[0] + 2 * i], 2, 4, ref);
            }
            for (i = 0; i < 2; i++) {
                int mx, my, mpx, mpy;
                pred_8x16_motion(c, i * 4, c->ref_cache[scan8[0] + 2 * i], &mx, &my);
                DECODE_CABAC_MB_MVD(c, 4 * i)
                fill_mvd(&c->mvd_cache[scan8[0] + 2 * i], 2, 4, mpx, mpy);
                fill_mv(&c->mv_cache[scan8[0] + 2 * i], 2, 4, mx, my);
            }
        }
    }

    if (IS_INTER(mb_type)) {
        c->chroma_pred_mode_table[mb_xy] = 0;
        write_back_motion(c, mb_type);
    }

    if (!IS_INTRA16x16(mb_type)) {
        cbp = decode_cabac_mb_cbp_luma(c);
        if (c->decode_chroma)
            cbp |= decode_cabac_mb_cbp_chroma(c) << 4;
    } else if (!c->decode_chroma && cbp > 15) {
        return -1;
    }

    c->cbp_table[mb_xy] = cbp;

    if (dct8x8_allowed && (cbp & 15) && !IS_INTRA(mb_type))
        mb_type |= MB_TYPE_8x8DCT * get_cabac_noinline(&c->cabac, &c->cabac_state[399 + c->neighbor_transform_size]);

    c->mb_type[mb_xy] = mb_type;

    if (cbp || IS_INTRA16x16(mb_type)) {
        /* mb_qp_delta: only whether it was zero matters for the next context */
        if (get_cabac_noinline(&c->cabac, &c->cabac_state[60 + (c->last_qscale_diff != 0)])) {
            int val = 1;
            int ctx = 2;
            const int max_qp = 51 + 6 * (c->cur_sps->bit_depth_luma - 8);

            while (get_cabac_noinline(&c->cabac, &c->cabac_state[60 + ctx])) {
                ctx = 3;
                val++;
                if (val > 2 * max_qp)
                    return -1;
            }
            c->last_qscale_diff = val;
        } else
            c->last_qscale_diff = 0;

        decode_cabac_luma_residual(c, mb_type, cbp);
        if (c->decode_chroma) {
            if (cbp & 0x30) {
                int ch;
                for (ch = 0; ch < 2; ch++)
                    decode_cabac_residual_dc(c, 3, CHROMA_DC_BLOCK_INDEX + ch, 4);
            }
            if (cbp & 0x20) {
                int ch, i;
                for (ch = 0; ch < 2; ch++)
                    for (i = 0; i < 4; i++)
                        decode_cabac_residual_nondc(c, 4, 16 + 16 * ch + i, 15);
            } else {
                fill8(&c->non_zero_count_cache[scan8[16]], 4, 4, 0);
                fill8(&c->non_zero_count_cache[scan8[32]], 4, 4, 0);
            }
        }
    } else {
        fill8(&c->non_zero_count_cache[scan8[ 0]], 4, 4, 0);
        fill8(&c->non_zero_count_cache[scan8[16]], 4, 4, 0);
        fill8(&c->non_zero_count_cache[scan8[32]], 4, 4, 0);
        c->last_qscale_diff = 0;
    }

    write_back_non_zero_count(c);
    return 0;
}

/* ---------------------------------------------------------------- parameter set parsing */

static int is_high_profile(int p)
{
    return p == 100 || p == 110 || p == 122 || p == 244 || p == 44 || p == 83 ||
           p == 86 || p == 118 || p == 128 || p == 138 || p == 139 || p == 134 || p == 135;
}

static int parse_sps(mvp_ctx *c, BitReader *r)
{
    SPS s;
    unsigned id;
    int crop_l = 0, crop_r = 0, crop_t = 0, crop_b = 0, i;

    memset(&s, 0, sizeof(s));
    s.profile_idc = br_bits(r, 8);
    br_bits(r, 16);                          /* constraint flags, level_idc */
    id = br_ue(r);
    if (id >= 32)
        return -1;
    s.chroma_format_idc = 1;
    s.bit_depth_luma = 8;
    if (is_high_profile(s.profile_idc)) {
        s.chroma_format_idc = br_ue(r);
        if (s.chroma_format_idc > 3)
            return -1;
        if (s.chroma_format_idc == 3)
            br_bit(r);                       /* separate_colour_plane_flag */
        s.bit_depth_luma = br_ue(r) + 8;
        br_ue(r);                            /* bit_depth_chroma */
        br_bit(r);                           /* qpprime_y_zero_transform_bypass_flag */
        if (br_bit(r)) {                     /* seq_scaling_matrix_present_flag */
            for (i = 0; i < (s.chroma_format_idc != 3 ? 8 : 12); i++)
                if (br_bit(r))
                    skip_scaling_list(r, i < 6 ? 16 : 64);
        }
    }
    s.log2_max_frame_num = br_ue(r) + 4;
    s.poc_type = br_ue(r);
    if (s.poc_type == 0) {
        s.log2_max_poc_lsb = br_ue(r) + 4;
    } else if (s.poc_type == 1) {
        unsigned n;
        s.delta_pic_order_always_zero_flag = br_bit(r);
        br_se(r);
        br_se(r);
        n = br_ue(r);
        if (n > 255)
            return -1;
        while (n--)
            br_se(r);
    }
    s.max_num_ref_frames = br_ue(r);
    br_bit(r);                               /* gaps_in_frame_num_allowed_flag */
    s.mb_width = br_ue(r) + 1;
    s.mb_height = br_ue(r) + 1;
    s.frame_mbs_only_flag = br_bit(r);
    if (!s.frame_mbs_only_flag) {
        s.mb_aff = br_bit(r);
        s.mb_height *= 2;
    }
    s.direct_8x8_inference_flag = br_bit(r);
    if (br_bit(r)) {
        crop_l = br_ue(r);
        crop_r = br_ue(r);
        crop_t = br_ue(r);
        crop_b = br_ue(r);
    }
    if (br_overrun(r) || s.mb_width > 1024 || s.mb_height > 1024 || s.log2_max_frame_num > 16 ||
        s.log2_max_poc_lsb > 16)
        return -1;
    {
        int cx = s.chroma_format_idc == 1 || s.chroma_format_idc == 2 ? 2 : 1;
        int cy = (s.chroma_format_idc == 1 ? 2 : 1) * (2 - s.frame_mbs_only_flag);
        s.width = 16 * s.mb_width - cx * (crop_l + crop_r);
        s.height = 16 * s.mb_height - cy * (crop_t + crop_b);
        if (s.width <= 0 || s.height <= 0) {
            s.width = 16 * s.mb_width;
            s.height = 16 * s.mb_height;
        }
    }
    s.valid = 1;
    c->sps[id] = s;
    return 0;
}

static int parse_pps(mvp_ctx *c, BitReader *r, int rbsp_bits)
{
    PPS p;
    unsigned id;

    memset(&p, 0, sizeof(p));
    id = br_ue(r);
    if (id >= 256)
        return -1;
    p.sps_id = br_ue(r);
    if (p.sps_id >= 32)
        return -1;
    p.cabac = br_bit(r);
    p.pic_order_present = br_bit(r);
    p.slice_group_count = br_ue(r) + 1;
    if (p.slice_group_count > 1) {           /* FMO: not supported, keep the PPS as a marker */
        p.valid = 1;
        c->pps[id] = p;
        return 0;
    }
    p.ref_count[0] = br_ue(r) + 1;
    p.ref_count[1] = br_ue(r) + 1;
    if (p.ref_count[0] > 32 || p.ref_count[1] > 32)
        return -1;
    p.weighted_pred = br_bit(r);
    p.weighted_bipred_idc = br_bits(r, 2);
    p.init_qp = 26 + br_se(r);
    br_se(r);                                /* pic_init_qs */
    br_se(r);                                /* chroma_qp_index_offset */
    p.deblocking_filter_parameters_present = br_bit(r);
    br_bit(r);                               /* constrained_intra_pred */
    p.redundant_pic_cnt_present = br_bit(r);
    if (r->pos < rbsp_bits) {                /* more_rbsp_data() */
        const SPS *sps = &c->sps[p.sps_id];
        p.transform_8x8_mode = br_bit(r);
        if (br_bit(r)) {                     /* pic_scaling_matrix_present_flag */
            int n = 6 + ((sps->valid && sps->chroma_format_idc == 3) ? 6 : 2) * p.transform_8x8_mode, i;
            for (i = 0; i < n; i++)
                if (br_bit(r))
                    skip_scaling_list(r, i < 6 ? 16 : 64);
        }
        br_se(r);                            /* second_chroma_qp_index_offset */
    }
    if (br_overrun(r))
        return -1;
    p.valid = 1;
    c->pps[id] = p;
    return 0;
}

/* ---------------------------------------------------------------- NAL units and slices */

/* Remove emulation prevention bytes; returns the RBSP size (without trailing
 * zero bytes) or -1. The buffer is followed by PADDING zero bytes. */
static int unescape(mvp_ctx *c, const uint8_t *src, int len)
{
    int i, o = 0, zeros = 0;
    if (len + PADDING > c->rbsp_cap) {
        int cap = (len + PADDING) * 3 / 2;
        uint8_t *b = realloc(c->rbsp, cap);
        if (!b)
            return -1;
        c->rbsp = b;
        c->rbsp_cap = cap;
    }
    for (i = 0; i < len; i++) {
        uint8_t x = src[i];
        if (zeros >= 2 && x == 3) {
            zeros = 0;
            continue;
        }
        c->rbsp[o++] = x;
        zeros = x == 0 ? zeros + 1 : 0;
    }
    while (o > 0 && c->rbsp[o - 1] == 0)
        o--;
    memset(c->rbsp + o, 0, PADDING);
    return o;
}

/* number of bits before the rbsp_stop_one_bit */
static int rbsp_payload_bits(const uint8_t *b, int size)
{
    int last;
    if (size <= 0)
        return 0;
    last = b[size - 1];
    return size * 8 - 1 - __builtin_ctz(last);
}

typedef struct PicState {
    int type;          /* MVP_* of the picture so far */
    int key;
    int errors;
    int slices;
    const SPS *sps;

    /* from the first slice header, for reference marking after the picture */
    int have_hdr;
    int idr, nal_ref_idc, frame_num;
    int poc_known, poc, poc_msb, poc_lsb, frame_num_offset;
    int lt_flag;       /* IDR: long_term_reference_flag */
    int adaptive;      /* adaptive_ref_pic_marking_mode_flag */
    int n_mmco;
    int mmco[66][3];   /* op, arg1, arg2 */

    int parsed_p, skipped_p;   /* P slices parsed / skipped for a far reference */
    int vcl_bytes;     /* RBSP bytes of all slices of the picture */
    int skip_dist;     /* nearest reference distance of the skipped slices */
    const char *reason;    /* MVP_UNSUPPORTED: what the stream uses that is not supported */
    int no_ps;             /* a slice refers to an SPS/PPS that was not seen yet */
} PicState;

static void start_picture(mvp_ctx *c, PicState *ps, const SPS *sps)
{
    if (c->mb_width != sps->mb_width || c->mb_height != sps->mb_height)
        if (alloc_tables(c, sps->mb_width, sps->mb_height) < 0)
            return;
    memset(c->slice_table - (c->mb_stride + 1), 0xFF,
           sizeof(uint16_t) * ((size_t)c->mb_stride + 1 + c->mb_stride * (c->mb_height + 1)));
    ps->sps = sps;
    c->slice_num = 0;
}

/* ---------------------------------------------------------------- reference pictures
 * H.264 8.2.1 (POC), 8.2.4 (list initialisation and modification for P slices) and
 * 8.2.5 (marking), for frames only. Nothing is decoded: the DPB holds just frame_num
 * and POC, which is enough to know how many frames back each reference is. */

/* 8.2.1: POC of the current picture (frames: TopFieldOrderCnt) */
static void compute_poc(mvp_ctx *c, PicState *ps, const SPS *sps, int lsb)
{
    ps->poc_known = 1;
    if (sps->poc_type == 0) {
        int max = 1 << sps->log2_max_poc_lsb, pm = 0, pl = 0, msb;
        if (!ps->idr) {
            pm = c->prev_ref_poc_msb;
            pl = c->prev_ref_poc_lsb;
        }
        if (lsb < pl && pl - lsb >= max / 2)
            msb = pm + max;
        else if (lsb > pl && lsb - pl > max / 2)
            msb = pm - max;
        else
            msb = pm;
        ps->poc_msb = msb;
        ps->poc_lsb = lsb;
        ps->poc = msb + lsb;
    } else if (sps->poc_type == 2) {
        int max_fn = 1 << sps->log2_max_frame_num, off;
        if (ps->idr)
            off = 0;
        else if (c->prev_frame_num > ps->frame_num)
            off = c->prev_frame_num_offset + max_fn;
        else
            off = c->prev_frame_num_offset;
        ps->frame_num_offset = off;
        ps->poc = ps->idr ? 0 : 2 * (off + ps->frame_num) - (ps->nal_ref_idc == 0);
    } else {
        ps->poc_known = 0;          /* type 1 is not used by any camera we know */
    }
    /* POC units per frame: the smallest step between consecutive pictures */
    if (ps->poc_known && c->have_last_poc && !ps->idr && ps->poc > c->last_poc &&
        (c->poc_step == 0 || ps->poc - c->last_poc < c->poc_step))
        c->poc_step = ps->poc - c->last_poc;
}

static int frame_num_wrap(const RefPic *p, int cur_fn, int max_fn)
{
    return p->frame_num > cur_fn ? p->frame_num - max_fn : p->frame_num;
}

static void dpb_remove(mvp_ctx *c, int i)
{
    memmove(&c->dpb[i], &c->dpb[i + 1], sizeof(c->dpb[0]) * (c->n_dpb - i - 1));
    c->n_dpb--;
}

static int find_short(mvp_ctx *c, int pic_num, int cur_fn, int max_fn)
{
    int i;
    for (i = 0; i < c->n_dpb; i++)
        if (c->dpb[i].long_idx < 0 && frame_num_wrap(&c->dpb[i], cur_fn, max_fn) == pic_num)
            return i;
    return -1;
}

static int find_long(mvp_ctx *c, int idx)
{
    int i;
    for (i = 0; i < c->n_dpb; i++)
        if (c->dpb[i].long_idx == idx)
            return i;
    return -1;
}

/* 8.2.5.3: drop the oldest short-term reference if the DPB is full */
static void sliding_window(mvp_ctx *c, const SPS *sps, int cur_fn)
{
    int max_fn = 1 << sps->log2_max_frame_num, limit = sps->max_num_ref_frames > 0 ? sps->max_num_ref_frames : 1;
    while (c->n_dpb >= limit || c->n_dpb >= MAX_DPB - 1) {
        int i, oldest = -1;
        for (i = 0; i < c->n_dpb; i++)
            if (c->dpb[i].long_idx < 0 &&
                (oldest < 0 || frame_num_wrap(&c->dpb[i], cur_fn, max_fn) <
                               frame_num_wrap(&c->dpb[oldest], cur_fn, max_fn)))
                oldest = i;
        if (oldest < 0)
            break;
        dpb_remove(c, oldest);
    }
}

/* 8.2.5: reference marking after the picture; also advances the POC state */
static void finish_picture(mvp_ctx *c, PicState *ps)
{
    const SPS *sps = ps->sps;
    int max_fn = 1 << sps->log2_max_frame_num, cur_fn = ps->frame_num, mmco5 = 0, cur_lt = -1, k, i;

    if (ps->idr) {
        c->n_dpb = 0;
        c->dpb_valid = ps->poc_known;
        c->max_lt_idx = ps->lt_flag ? 0 : -1;
        cur_lt = ps->lt_flag ? 0 : -1;
    } else if (ps->nal_ref_idc) {
        if (ps->adaptive) {
            for (k = 0; k < ps->n_mmco; k++) {
                int op = ps->mmco[k][0], a1 = ps->mmco[k][1], a2 = ps->mmco[k][2];
                switch (op) {
                case 1:                         /* short-term -> unused */
                    if ((i = find_short(c, cur_fn - (a1 + 1), cur_fn, max_fn)) >= 0)
                        dpb_remove(c, i);
                    break;
                case 2:                         /* long-term -> unused */
                    if ((i = find_long(c, a1)) >= 0)
                        dpb_remove(c, i);
                    break;
                case 3:                         /* short-term -> long-term a2 */
                    if ((i = find_short(c, cur_fn - (a1 + 1), cur_fn, max_fn)) >= 0) {
                        int j = find_long(c, a2);
                        if (j >= 0 && j != i) {
                            dpb_remove(c, j);
                            if (j < i)
                                i--;
                        }
                        c->dpb[i].long_idx = a2;
                    }
                    break;
                case 4:                         /* max long-term index */
                    c->max_lt_idx = a1 - 1;
                    for (i = c->n_dpb - 1; i >= 0; i--)
                        if (c->dpb[i].long_idx > c->max_lt_idx)
                            dpb_remove(c, i);
                    break;
                case 5:                         /* everything -> unused */
                    c->n_dpb = 0;
                    c->max_lt_idx = -1;
                    mmco5 = 1;
                    break;
                case 6:                         /* current -> long-term a1 */
                    if ((i = find_long(c, a1)) >= 0)
                        dpb_remove(c, i);
                    cur_lt = a1;
                    break;
                }
            }
        }
        if (cur_lt < 0)
            sliding_window(c, sps, cur_fn);
    }
    if (ps->idr || ps->nal_ref_idc) {
        if (c->n_dpb < MAX_DPB) {
            c->dpb[c->n_dpb].frame_num = mmco5 ? 0 : cur_fn;
            c->dpb[c->n_dpb].poc = mmco5 ? 0 : ps->poc;
            c->dpb[c->n_dpb].long_idx = cur_lt;
            c->n_dpb++;
        }
        c->prev_ref_poc_msb = mmco5 ? 0 : ps->poc_msb;
        c->prev_ref_poc_lsb = mmco5 ? 0 : ps->poc_lsb;
    }
    if (!ps->poc_known)
        c->dpb_valid = 0;

    c->last_poc = mmco5 ? 0 : ps->poc;
    c->have_last_poc = ps->poc_known;
    c->prev_frame_num = mmco5 ? 0 : cur_fn;
    c->prev_frame_num_offset = mmco5 ? 0 : ps->frame_num_offset;
}

/* 8.2.4: RefPicList0 of a P slice -> distance in frames of every entry (0 = unknown) */
static void ref_distances(mvp_ctx *c, const PicState *ps, int ref_count, const int (*mods)[2], int n_mods,
                          int16_t *dist)
{
    const SPS *sps = ps->sps;
    int max_fn = 1 << sps->log2_max_frame_num, cur_fn = ps->frame_num;
    int list[34], n = 0, i, j, k, ref_idx = 0, pred = cur_fn;
    int order[MAX_DPB];

    for (i = 0; i < ref_count; i++)
        dist[i] = 0;
    if (!c->dpb_valid || !ps->poc_known || c->poc_step <= 0)
        return;

    /* initial list: short-term by descending PicNum, then long-term by ascending index */
    for (i = 0; i < c->n_dpb; i++)
        order[i] = i;
    for (i = 0; i < c->n_dpb; i++)
        for (j = i + 1; j < c->n_dpb; j++) {
            const RefPic *a = &c->dpb[order[i]], *b = &c->dpb[order[j]];
            int swap;
            if ((a->long_idx < 0) != (b->long_idx < 0))
                swap = a->long_idx >= 0;
            else if (a->long_idx < 0)
                swap = frame_num_wrap(a, cur_fn, max_fn) < frame_num_wrap(b, cur_fn, max_fn);
            else
                swap = a->long_idx > b->long_idx;
            if (swap) {
                int t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
        }
    for (i = 0; i < c->n_dpb && n < ref_count; i++)
        list[n++] = order[i];
    while (n < ref_count)
        list[n++] = -1;

    /* 8.2.4.3 modification */
    for (k = 0; k < n_mods && ref_idx < ref_count; k++) {
        int idc = mods[k][0], v = mods[k][1], target;
        if (idc == 0 || idc == 1) {
            int no_wrap = idc == 0 ? pred - (v + 1) : pred + (v + 1);
            if (no_wrap < 0)
                no_wrap += max_fn;
            else if (no_wrap >= max_fn)
                no_wrap -= max_fn;
            pred = no_wrap;
            target = find_short(c, no_wrap > cur_fn ? no_wrap - max_fn : no_wrap, cur_fn, max_fn);
        } else {
            target = find_long(c, v);
        }
        for (i = ref_count; i > ref_idx; i--)
            list[i] = list[i - 1];
        list[ref_idx++] = target;
        for (i = j = ref_idx; i <= ref_count; i++)
            if (list[i] != target || target < 0)
                list[j++] = list[i];
    }

    for (i = 0; i < ref_count; i++) {
        if (list[i] >= 0) {
            int d = ps->poc - c->dpb[list[i]].poc;
            if (d > 0)
                dist[i] = (d + c->poc_step / 2) / c->poc_step;
        }
    }
}

static int decode_slice(mvp_ctx *c, PicState *ps, const uint8_t *nal, int nal_size)
{
    BitReader r;
    int nal_ref_idc = (nal[0] >> 5) & 3;
    int nal_type = nal[0] & 0x1F;
    int idr = nal_type == 5;
    unsigned first_mb, slice_type, pps_id;
    const PPS *pps;
    const SPS *sps;
    int is_p, qp, cabac_init_idc = 0, i, size, ret = 0;
    int frame_num, poc_lsb = 0, n_mods = 0, mods[33][2], lt_flag = 0, adaptive = 0, n_mmco = 0;
    int mmco[66][3];

    size = unescape(c, nal + 1, nal_size - 1);
    if (size <= 0)
        return -1;
    ps->vcl_bytes += size;
    br_init(&r, c->rbsp, size);

    first_mb = br_ue(&r);
    slice_type = br_ue(&r);
    if (slice_type > 9)
        return -1;
    slice_type %= 5;
    pps_id = br_ue(&r);
    if (pps_id >= 256 || !c->pps[pps_id].valid || !c->sps[c->pps[pps_id].sps_id].valid) {
        if (ps->type == MVP_NONE)
            ps->type = MVP_UNSUPPORTED;
        ps->no_ps = 1;
        return -2;
    }
    pps = &c->pps[pps_id];
    sps = &c->sps[pps->sps_id];
    if (idr)
        ps->key = 1;

    if (slice_type == 1 || !pps->cabac || pps->slice_group_count > 1 || !sps->frame_mbs_only_flag ||
        (sps->chroma_format_idc != 0 && sps->chroma_format_idc != 1)) {
        ps->type = MVP_UNSUPPORTED;       /* B, CAVLC, FMO, interlace, 4:2:2/4:4:4 */
        ps->reason = slice_type == 1 ? "B slices" : !pps->cabac ? "CAVLC entropy coding (Baseline profile?)" :
                     pps->slice_group_count > 1 ? "FMO slice groups" : !sps->frame_mbs_only_flag ? "interlaced coding" :
                     "4:2:2/4:4:4 chroma";
        c->dpb_valid = 0;                 /* reference marking is not followed for these */
        return -2;
    }
    is_p = slice_type == 0 || slice_type == 3;  /* P, SP */
    if (ps->type == MVP_UNSUPPORTED)
        return -2;
    if (is_p)
        ps->type = MVP_P;
    else if (ps->type == MVP_NONE)
        ps->type = MVP_I;

    if (ps->slices++ == 0 || ps->sps != sps)
        start_picture(c, ps, sps);
    if (c->mb_width != sps->mb_width || c->mb_height != sps->mb_height)
        return -1;                            /* allocation failed */
    if (first_mb >= (unsigned)(sps->mb_width * sps->mb_height))
        return -1;

    frame_num = br_bits(&r, sps->log2_max_frame_num);
    if (idr)
        br_ue(&r);                            /* idr_pic_id */
    if (sps->poc_type == 0) {
        poc_lsb = br_bits(&r, sps->log2_max_poc_lsb);
        if (pps->pic_order_present)
            br_se(&r);
    } else if (sps->poc_type == 1 && !sps->delta_pic_order_always_zero_flag) {
        br_se(&r);
        if (pps->pic_order_present)
            br_se(&r);
    }
    if (pps->redundant_pic_cnt_present && br_ue(&r) > 0)
        return 0;                             /* redundant slice: ignored, like FFmpeg */

    c->ref_count = pps->ref_count[0];
    if (is_p) {
        if (br_bit(&r)) {                     /* num_ref_idx_active_override_flag */
            c->ref_count = br_ue(&r) + 1;
            if (c->ref_count > 32)
                return -1;
        }
        if (br_bit(&r)) {                     /* ref_pic_list_modification_flag_l0 */
            for (i = 0; i < 64; i++) {
                unsigned idc = br_ue(&r);
                if (idc == 3)
                    break;
                if (idc > 2 || br_overrun(&r))
                    return -1;
                if (n_mods < 33) {
                    mods[n_mods][0] = idc;
                    mods[n_mods][1] = br_ue(&r);  /* abs_diff_pic_num_minus1 / long_term_pic_num */
                    n_mods++;
                } else {
                    br_ue(&r);
                }
            }
        }
        if (pps->weighted_pred) {             /* pred_weight_table */
            br_ue(&r);                        /* luma_log2_weight_denom */
            if (sps->chroma_format_idc)
                br_ue(&r);
            for (i = 0; i < c->ref_count; i++) {
                if (br_bit(&r)) {
                    br_se(&r);
                    br_se(&r);
                }
                if (sps->chroma_format_idc && br_bit(&r)) {
                    br_se(&r); br_se(&r);
                    br_se(&r); br_se(&r);
                }
            }
        }
    }
    if (nal_ref_idc) {                        /* dec_ref_pic_marking */
        if (idr) {
            br_bit(&r);                       /* no_output_of_prior_pics_flag */
            lt_flag = br_bit(&r);
        } else if ((adaptive = br_bit(&r))) {
            for (i = 0; i < 66; i++) {
                unsigned op = br_ue(&r);
                if (op == 0)
                    break;
                if (op > 6 || br_overrun(&r))
                    return -1;
                mmco[n_mmco][0] = op;
                mmco[n_mmco][1] = mmco[n_mmco][2] = 0;
                if (op == 1 || op == 3)
                    mmco[n_mmco][1] = br_ue(&r);  /* difference_of_pic_nums_minus1 */
                if (op == 2)
                    mmco[n_mmco][1] = br_ue(&r);  /* long_term_pic_num */
                if (op == 3)
                    mmco[n_mmco][2] = br_ue(&r);  /* long_term_frame_idx */
                if (op == 6)
                    mmco[n_mmco][1] = br_ue(&r);  /* long_term_frame_idx */
                if (op == 4)
                    mmco[n_mmco][1] = br_ue(&r);  /* max_long_term_frame_idx_plus1 */
                n_mmco++;
            }
        }
    }
    if (br_overrun(&r))
        return -1;

    if (!ps->have_hdr) {                      /* the picture's reference information */
        ps->have_hdr = 1;
        ps->idr = idr;
        ps->nal_ref_idc = nal_ref_idc;
        ps->frame_num = frame_num;
        ps->lt_flag = lt_flag;
        ps->adaptive = adaptive;
        ps->n_mmco = n_mmco;
        memcpy(ps->mmco, mmco, sizeof(mmco[0]) * n_mmco);
        compute_poc(c, ps, sps, poc_lsb);
    }
    if (!is_p)
        return 0;                             /* intra slices carry no vectors */

    {
        int16_t dist[32];
        ref_distances(c, ps, c->ref_count, (const int (*)[2])mods, n_mods, dist);
        if (c->max_ref_dist > 0) {
            int nearest = 0;
            for (i = 0; i < c->ref_count; i++)
                if (dist[i] == 0 || dist[i] <= c->max_ref_dist)
                    break;
                else if (nearest == 0 || dist[i] < nearest)
                    nearest = dist[i];
            if (i == c->ref_count) {          /* every reference is too far: not worth parsing */
                ps->skipped_p++;
                if (ps->skip_dist == 0 || nearest < ps->skip_dist)
                    ps->skip_dist = nearest;
                return 0;
            }
        }
        c->slice_num++;
        memcpy(c->slice_dist[c->slice_num % MAX_SLICES], dist, sizeof(dist[0]) * c->ref_count);
    }
    ps->parsed_p++;

    cabac_init_idc = br_ue(&r);
    if (cabac_init_idc > 2)
        return -1;
    qp = pps->init_qp + br_se(&r);
    if (slice_type == 3) {                    /* SP */
        br_bit(&r);                           /* sp_for_switch_flag */
        br_se(&r);                            /* slice_qs_delta */
    }
    if (pps->deblocking_filter_parameters_present) {
        if (br_ue(&r) != 1) {
            br_se(&r);
            br_se(&r);
        }
    }
    if (br_overrun(&r))
        return -1;

    /* slice data */
    c->cur_sps = sps;
    c->cur_pps = pps;
    c->decode_chroma = sps->chroma_format_idc == 1;
    c->last_qscale_diff = 0;
    c->prev_mb_skipped = 0;
    c->mb_x = first_mb % sps->mb_width;
    c->mb_y = first_mb / sps->mb_width;
    {
        const int8_t (*tab)[2] = ff_cabac_context_init_PB[cabac_init_idc];
        int slice_qp = qp - 6 * (sps->bit_depth_luma - 8);
        slice_qp = slice_qp < 0 ? 0 : slice_qp > 51 ? 51 : slice_qp;
        for (i = 0; i < 1024; i++) {
            int pre = 2 * (((tab[i][0] * slice_qp) >> 4) + tab[i][1]) - 127;
            pre ^= pre >> 31;
            if (pre > 124)
                pre = 124 + (pre & 1);
            c->cabac_state[i] = pre;
        }
    }
    {
        int byte = (r.pos + 7) >> 3;
        if (byte >= size)
            return -1;
        if (init_cabac_decoder(&c->cabac, c->rbsp + byte, size - byte) < 0)
            return -1;
    }

    for (;;) {
        int eos;
        ret = decode_mb_cabac(c, 1);
        eos = get_cabac_terminate(&c->cabac);
        if (ret < 0 || c->cabac.bytestream > c->cabac.bytestream_end + 4)
            return -1;
        if (++c->mb_x >= sps->mb_width) {
            c->mb_x = 0;
            ++c->mb_y;
        }
        if (eos || c->mb_y >= sps->mb_height)
            break;
    }
    return 0;
}

static int handle_nal(mvp_ctx *c, PicState *ps, const uint8_t *nal, int size)
{
    int type, n;
    BitReader r;

    if (size < 1)
        return 0;
    type = nal[0] & 0x1F;
    switch (type) {
    case 1:
    case 5:
        if (size < 2)
            return 0;
        if (decode_slice(c, ps, nal, size) == -1)
            ps->errors++;
        return 0;
    case 7:
    case 8:
        n = unescape(c, nal + 1, size - 1);
        if (n <= 0)
            return -1;
        br_init(&r, c->rbsp, n);
        return type == 7 ? parse_sps(c, &r) : parse_pps(c, &r, rbsp_payload_bits(c->rbsp, n));
    default:
        return 0;
    }
}

/* calls handle_nal for every NAL unit of an Annex B or length-prefixed buffer */
static int for_each_nal(mvp_ctx *c, PicState *ps, const uint8_t *buf, int size, int nal_length_size)
{
    int pos = 0, ret = 0;
    if (nal_length_size > 0) {
        while (pos + nal_length_size <= size) {
            uint32_t len = 0;
            int i;
            for (i = 0; i < nal_length_size; i++)
                len = (len << 8) | buf[pos + i];
            pos += nal_length_size;
            if (len > (uint32_t)(size - pos))
                return -1;
            if (handle_nal(c, ps, buf + pos, len) < 0)
                ret = -1;
            pos += len;
        }
        return ret;
    }
    /* Annex B: find 00 00 01 start codes */
    {
        int start = -1, i = 0;
        while (i + 2 < size) {
            if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
                if (start >= 0) {
                    int end = i;
                    while (end > start && buf[end - 1] == 0)
                        end--;
                    if (handle_nal(c, ps, buf + start, end - start) < 0)
                        ret = -1;
                }
                i += 3;
                start = i;
            } else if (buf[i + 2] > 1) {
                i += 3;
            } else {
                i++;
            }
        }
        if (start >= 0 && start < size)
            if (handle_nal(c, ps, buf + start, size - start) < 0)
                ret = -1;
    }
    return ret;
}

int mvp_get_size(const mvp_ctx *c, int *width, int *height)
{
    int i;
    for (i = 0; i < 32; i++)
        if (c->sps[i].valid) {
            *width = c->sps[i].width;
            *height = c->sps[i].height;
            return 0;
        }
    return -1;
}

int mvp_set_extradata(mvp_ctx *c, const uint8_t *data, int size)
{
    PicState ps;
    memset(&ps, 0, sizeof(ps));
    if (size >= 7 && data[0] == 1) {         /* avcC */
        int pos = 6, n, i, k, ret = 0;
        for (k = 0; k < 2; k++) {
            if (pos >= size)
                return -1;
            n = k == 0 ? (data[5] & 0x1F) : data[pos - 1];
            for (i = 0; i < n; i++) {
                int len;
                if (pos + 2 > size)
                    return -1;
                len = (data[pos] << 8) | data[pos + 1];
                pos += 2;
                if (pos + len > size)
                    return -1;
                if (handle_nal(c, &ps, data + pos, len) < 0)
                    ret = -1;
                pos += len;
            }
            pos++;                            /* numOfPictureParameterSets */
        }
        return ret;
    }
    return for_each_nal(c, &ps, data, size, 0);
}

/* the block list FFmpeg exports for one picture (libavcodec/mpegutils.c, list 0) */
static int export_mvs(mvp_ctx *c)
{
    const int mv_stride = c->b_stride;
    int mb_x, mb_y, n = 0;
    size_t need = (size_t)c->mb_width * c->mb_height * 4;

    if (need > (size_t)c->mvs_cap) {
        mvp_mv *m = realloc(c->mvs, need * sizeof(*m));
        if (!m)
            return -1;
        c->mvs = m;
        c->mvs_cap = need;
    }
#define ADD(SX, SY, XY, W, H) do {                       \
        mvp_mv *v = &c->mvs[n++];                        \
        v->dst_x = SX; v->dst_y = SY;                    \
        v->mx = c->motion_val[XY][0];                    \
        v->my = c->motion_val[XY][1];                    \
        v->w = W; v->h = H;                              \
    } while (0)
    for (mb_y = 0; mb_y < c->mb_height; mb_y++) {
        for (mb_x = 0; mb_x < c->mb_width; mb_x++) {
            const int mb_xy = mb_x + mb_y * c->mb_stride;
            const uint32_t mb_type = c->mb_type[mb_xy];
            int i;
            if (c->slice_table[mb_xy] == 0xFFFF || !USES_LIST0(mb_type))
                continue;
            if (IS_8X8(mb_type)) {
                for (i = 0; i < 4; i++)
                    ADD(mb_x * 16 + 4 + 8 * (i & 1), mb_y * 16 + 4 + 8 * (i >> 1),
                        (mb_x * 4 + 2 * (i & 1)) + (mb_y * 4 + 2 * (i >> 1)) * mv_stride, 8, 8);
            } else if (IS_16X8(mb_type)) {
                for (i = 0; i < 2; i++)
                    ADD(mb_x * 16 + 8, mb_y * 16 + 4 + 8 * i,
                        mb_x * 4 + (mb_y * 4 + 2 * i) * mv_stride, 16, 8);
            } else if (IS_8X16(mb_type)) {
                for (i = 0; i < 2; i++)
                    ADD(mb_x * 16 + 4 + 8 * i, mb_y * 16 + 8,
                        mb_x * 4 + 2 * i + mb_y * 4 * mv_stride, 8, 16);
            } else {
                ADD(mb_x * 16 + 8, mb_y * 16 + 8, mb_x * 4 + mb_y * 4 * mv_stride, 16, 16);
            }
        }
    }
#undef ADD
    return n;
}

/* largest reference distance actually used by the inter macroblocks (0 = unknown) */
static int used_ref_dist(mvp_ctx *c)
{
    int mb_x, mb_y, k, best = 0, unknown = 0;
    for (mb_y = 0; mb_y < c->mb_height; mb_y++) {
        for (mb_x = 0; mb_x < c->mb_width; mb_x++) {
            const int mb_xy = mb_x + mb_y * c->mb_stride;
            const int16_t *dist;
            if (c->slice_table[mb_xy] == 0xFFFF || !USES_LIST0(c->mb_type[mb_xy]))
                continue;
            dist = c->slice_dist[c->slice_table[mb_xy] % MAX_SLICES];
            for (k = 0; k < 4; k++) {
                int ri = c->ref_index[4 * mb_xy + k];
                if (ri < 0 || ri >= 32)
                    continue;
                if (dist[ri] == 0)
                    unknown = 1;
                else if (dist[ri] > best)
                    best = dist[ri];
            }
        }
    }
    return unknown ? 0 : best;
}

int mvp_decode(mvp_ctx *c, const uint8_t *data, int size, int nal_length_size, mvp_frame *out)
{
    PicState ps;
    int ret;

    memset(&ps, 0, sizeof(ps));
    memset(out, 0, sizeof(*out));
    ret = for_each_nal(c, &ps, data, size, nal_length_size);

    out->type = ps.type;
    out->key = ps.key;
    out->errors = ps.errors;
    out->poc = ps.poc_known ? ps.poc : 0;
    out->vcl_bytes = ps.vcl_bytes;
    out->reason = ps.reason;
    out->no_ps = ps.no_ps && !ps.reason;
    if (ps.sps) {
        out->width = ps.sps->width;
        out->height = ps.sps->height;
        out->mb_width = ps.sps->mb_width;
        out->mb_height = ps.sps->mb_height;
    }
    if (ps.type == MVP_P && ps.parsed_p == 0 && ps.skipped_p > 0) {
        out->type = MVP_SKIPPED;
        out->ref_dist = ps.skip_dist;
    } else if (ps.type == MVP_P && c->mb_width == out->mb_width && c->mb_height == out->mb_height) {
        int n = export_mvs(c);
        if (n < 0)
            return -1;
        out->n_mv = n;
        out->mv = c->mvs;
        out->ref_dist = used_ref_dist(c);
    }
    if (ps.have_hdr && ps.sps)
        finish_picture(c, &ps);
    return ret < 0 && ps.slices == 0 ? -1 : 0;
}

void mvp_set_max_ref_dist(mvp_ctx *c, int frames)
{
    c->max_ref_dist = frames > 0 ? frames : 0;
}

int mvp_dump_refs(const mvp_ctx *c, char *buf, int size)
{
    int i, n = 0;
    if (size <= 0)
        return 0;
    buf[0] = 0;
    if (!c->dpb_valid)
        return snprintf(buf, size, "unknown");
    for (i = 0; i < c->n_dpb && n < size; i++)
        n += snprintf(buf + n, size - n, "%s%c%d:fn%d:poc%d", i ? " " : "",
                      c->dpb[i].long_idx < 0 ? 'S' : 'L', c->dpb[i].long_idx < 0 ? 0 : c->dpb[i].long_idx,
                      c->dpb[i].frame_num, c->dpb[i].poc);
    return n;
}
