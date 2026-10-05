/* SPS max_num_ref_frames rewrite, a port of h264fix.py (_locate / patch_sps without the VUI part). */
#include <string.h>

#include "h264_sps.h"

typedef struct bits {
    const uint8_t *b;
    long n, p;              /* size and position in bits */
} bits;

static int get1(bits *r)
{
    int v;
    if (r->p >= r->n)
        return -1;
    v = (r->b[r->p >> 3] >> (7 - (r->p & 7))) & 1;
    r->p++;
    return v;
}

static long getn(bits *r, int n)
{
    long v = 0;
    while (n-- > 0) {
        int x = get1(r);
        if (x < 0)
            return -1;
        v = v * 2 + x;
    }
    return v;
}

static long ue(bits *r)
{
    int z = 0, x;
    while ((x = get1(r)) == 0)
        if (++z > 31)
            return -1;
    if (x < 0)
        return -1;
    return (1L << z) - 1 + getn(r, z);
}

static long se(bits *r)
{
    long k = ue(r);
    if (k < 0)
        return -100000;
    return k % 2 ? (k + 1) / 2 : -(k / 2);
}

static int high_profile(long p)
{
    return p == 100 || p == 110 || p == 122 || p == 244 || p == 44 || p == 83 || p == 86 ||
           p == 118 || p == 128 || p == 138 || p == 139 || p == 134 || p == 135;
}

/* bit writer */
typedef struct wbits {
    uint8_t *b;
    long p;
} wbits;

static void put1(wbits *w, int v)
{
    if (v)
        w->b[w->p >> 3] |= 0x80 >> (w->p & 7);
    w->p++;
}

static void put_ue(wbits *w, unsigned v)
{
    int n = 0, i;
    v++;
    while ((v >> n) > 1)
        n++;
    for (i = 0; i < n; i++)
        put1(w, 0);
    for (i = n; i >= 0; i--)
        put1(w, (v >> i) & 1);
}

int sps_patch(bytebuf *out, const uint8_t *nal, int len, int refs)
{
    uint8_t rbsp[1024], nb[1100];
    int rl = 0, i, zeros = 0, last;
    long pos, after, prof, k;
    bits r;
    wbits w;

    if (len < 4 || len > (int)sizeof(rbsp))
        goto copy;
    for (i = 1; i < len; i++) {             /* unescape */
        if (zeros >= 2 && nal[i] == 3) {
            zeros = 0;
            continue;
        }
        rbsp[rl++] = nal[i];
        zeros = nal[i] == 0 ? zeros + 1 : 0;
    }
    r.b = rbsp;
    r.n = rl * 8L;
    r.p = 0;
    prof = getn(&r, 8);
    getn(&r, 16);
    ue(&r);
    if (high_profile(prof)) {
        long cf = ue(&r);
        if (cf == 3)
            get1(&r);
        ue(&r);
        ue(&r);
        get1(&r);
        if (get1(&r) == 1) {                /* seq_scaling_matrix_present_flag */
            for (i = 0; i < (cf == 3 ? 12 : 8); i++) {
                if (get1(&r) == 1) {
                    long lastv = 8, next = 8;
                    for (k = 0; k < (i < 6 ? 16 : 64); k++) {
                        if (next != 0)
                            next = (lastv + se(&r) + 256) % 256;
                        lastv = next == 0 ? lastv : next;
                    }
                }
            }
        }
    }
    ue(&r);                                 /* log2_max_frame_num_minus4 */
    k = ue(&r);                             /* pic_order_cnt_type */
    if (k == 0)
        ue(&r);
    else if (k != 2)
        goto copy;                          /* poc type 1 is not needed for these cameras */
    pos = r.p;
    if (ue(&r) < 0 || r.p > r.n)
        goto copy;
    after = r.p;

    /* last set bit = rbsp_stop_one_bit */
    for (last = rl * 8 - 1; last >= 0; last--)
        if ((rbsp[last >> 3] >> (7 - (last & 7))) & 1)
            break;
    if (last < after)
        goto copy;

    memset(nb, 0, sizeof(nb));
    w.b = nb;
    w.p = 0;
    for (k = 0; k < pos; k++)
        put1(&w, (rbsp[k >> 3] >> (7 - (k & 7))) & 1);
    put_ue(&w, refs);
    for (k = after; k <= last; k++)
        put1(&w, (rbsp[k >> 3] >> (7 - (k & 7))) & 1);

    /* header byte, then the escaped RBSP */
    {
        int nbytes = (int)((w.p + 7) / 8);
        bb_u8(out, nal[0]);
        zeros = 0;
        for (i = 0; i < nbytes; i++) {
            if (zeros >= 2 && nb[i] <= 3) {
                bb_u8(out, 3);
                zeros = 0;
            }
            bb_u8(out, nb[i]);
            zeros = nb[i] == 0 ? zeros + 1 : 0;
        }
    }
    return 1;
copy:
    bb_put(out, nal, len);
    return 0;
}

/* the parameters an MP4/MSE decoder configuration depends on */
int sps_params(const uint8_t *nal, int len, sps_info *out)
{
    uint8_t rbsp[1024];
    int rl = 0, i, zeros = 0;
    long prof, k, cf = 1, mbw, mbh, fmo, crop[4] = { 0, 0, 0, 0 };
    bits r;

    memset(out, 0, sizeof(*out));
    if (len < 4 || len > (int)sizeof(rbsp))
        return -1;
    for (i = 1; i < len; i++) {
        if (zeros >= 2 && nal[i] == 3) {
            zeros = 0;
            continue;
        }
        rbsp[rl++] = nal[i];
        zeros = nal[i] == 0 ? zeros + 1 : 0;
    }
    r.b = rbsp;
    r.n = rl * 8L;
    r.p = 0;
    prof = getn(&r, 8);
    out->profile = (int)prof;
    out->constraints = (int)getn(&r, 8);
    out->level = (int)getn(&r, 8);
    ue(&r);
    if (high_profile(prof)) {
        cf = ue(&r);
        if (cf == 3)
            get1(&r);
        out->bit_depth = (int)ue(&r) + 8;
        ue(&r);
        get1(&r);
        if (get1(&r) == 1) {
            for (i = 0; i < (cf == 3 ? 12 : 8); i++) {
                if (get1(&r) == 1) {
                    long lastv = 8, next = 8;
                    for (k = 0; k < (i < 6 ? 16 : 64); k++) {
                        if (next != 0)
                            next = (lastv + se(&r) + 256) % 256;
                        lastv = next == 0 ? lastv : next;
                    }
                }
            }
        }
    } else {
        out->bit_depth = 8;
    }
    out->chroma = (int)cf;
    ue(&r);                                 /* log2_max_frame_num_minus4 */
    k = ue(&r);                             /* pic_order_cnt_type */
    if (k == 0) {
        ue(&r);
    } else if (k == 1) {
        long n;
        get1(&r);
        se(&r);
        se(&r);
        n = ue(&r);
        if (n < 0 || n > 255)
            return -1;
        while (n-- > 0)
            se(&r);
    }
    ue(&r);                                 /* max_num_ref_frames */
    get1(&r);
    mbw = ue(&r) + 1;
    mbh = ue(&r) + 1;
    fmo = get1(&r);                         /* frame_mbs_only_flag */
    if (fmo == 0)
        get1(&r);
    get1(&r);                               /* direct_8x8_inference_flag */
    if (get1(&r) == 1)
        for (i = 0; i < 4; i++)
            crop[i] = ue(&r);
    if (r.p > r.n || mbw <= 0 || mbh <= 0)
        return -1;
    out->width = (int)(16 * mbw - 2 * (crop[0] + crop[1]));
    out->height = (int)(16 * mbh * (2 - fmo) - 2 * (2 - fmo) * (crop[2] + crop[3]));
    return 0;
}
