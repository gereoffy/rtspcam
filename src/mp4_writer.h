/* Fragmented MP4 writer for the recording session (see mp4_writer.c). */
#ifndef RC_MP4_WRITER_H
#define RC_MP4_WRITER_H

#include "session.h"

typedef struct mp4_options {
    int fix_refs;           /* > 0: rewrite max_num_ref_frames in every SPS (browser fix) */
} mp4_options;

/* use with rcs_create(..., &mp4_writer_ops, &options) */
extern const rc_video_ops mp4_writer_ops;

/* Live mode: the same muxer in memory, one moof+mdat fragment per picture (for browsers' MSE). */
typedef struct mp4_live mp4_live;
mp4_live *mp4_live_create(const rc_stream_info *si, int fix_refs);
/* Adds one access unit. The fragment of the PREVIOUS picture comes out (exact duration), so
 * the output is one picture behind. Returns 1 and the fragment (valid until the next call;
 * *key: it is a key frame) if one was made, 0 if not (first picture, no SPS/PPS yet), -1 on
 * error. *new_init is set when the init segment (mp4_live_init) was (re)made: receivers must
 * start over with it, before any further fragment. */
int mp4_live_frame(mp4_live *l, const rc_packet *p, const uint8_t **frag, size_t *len, int *new_init, int *key);
const uint8_t *mp4_live_init(mp4_live *l, size_t *len);
void mp4_live_free(mp4_live *l);

#endif
