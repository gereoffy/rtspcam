/* Motion detector: per-frame score from motion vectors, hysteresis, motion map record.
 * Same rules as MotionDetector in rtspcam.py. */
#ifndef RC_MOTION_H
#define RC_MOTION_H

#include <stdint.h>

#include "mvparse.h"
#include "options.h"

/* .mvmap record flags (mvmap.py) */
#define F_KEY    1
#define F_MOVING 2
#define F_TRIG   4
#define F_NODATA 8
#define F_SKIP   16      /* not analysed: the picture's reference is too far back (--max-ref-dist) */

typedef struct mt_cell {
    uint16_t cell;          /* gy * gw + gx */
    uint8_t level;          /* 1 moving, 2 largest group below the threshold, 3 group at/above the threshold */
} mt_cell;

typedef struct mt_record {
    int cluster;
    double blocks;
    int flags;
    int n_cells;
    mt_cell *cells;         /* owned by the detector until the next frame */
} mt_record;

typedef struct mt_det {
    const rc_opts *a;
    int w, h, gw, gh;
    double total_mb;
    double (*zones)[4];     /* in pixels */
    int n_zones;

    uint8_t *hist;          /* ring buffer of the last `window` decisions */
    int hist_n, hist_pos, hist_sum;

    /* score of the last frame fed to mt_update */
    int last_valid;         /* 0: key frame (no score) */
    double blocks, frac;
    int cluster;

    /* clustering scratch */
    int32_t *label;         /* per grid cell: -1 = not moving, else group index */
    int *cells;             /* moving cells in discovery order */
    int n_cells;
    int *stack;
    int *group_size;
    int n_groups, best;
    mt_cell *rec_cells;
} mt_det;

int mt_init(mt_det *d, const rc_opts *a, int width, int height);
void mt_free(mt_det *d);
/* the options' --ignore zones changed: convert them again (the detector's state is kept);
 * on a bad zone the old ones stay */
int mt_reload_zones(mt_det *d);

/* Feed a parsed picture. Returns -1 for key frames (unknown: the caller keeps its state),
 * else 1 for a motion frame and 0 otherwise. Adopts a new frame size (resets the window). */
int mt_update(mt_det *d, const mvp_frame *f);
int mt_triggered(const mt_det *d);
/* map record for the frame just fed to mt_update(); `moving` is its return value */
void mt_record_get(mt_det *d, int moving, int trig, mt_record *r);

#endif
