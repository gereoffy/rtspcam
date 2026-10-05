/* Lossless SPS rewrite for browser playback (port of h264fix.patch_sps): sets
 * max_num_ref_frames, everything else (including the slice data) stays untouched. */
#ifndef RC_H264_SPS_H
#define RC_H264_SPS_H

#include <stdint.h>

#include "bytebuf.h"

/* Appends the SPS NAL unit (with its header byte, escaped) to out, with max_num_ref_frames
 * replaced by refs. If the SPS cannot be parsed it is appended unchanged. Returns 1 if changed. */
int sps_patch(bytebuf *out, const uint8_t *nal, int len, int refs);

typedef struct sps_info {
    int profile, constraints, level, chroma, bit_depth;
    int width, height;          /* cropped (4:2:0 cropping units) */
} sps_info;

/* Reads the fields above from an SPS NAL unit (with its header byte). Returns 0 or -1. */
int sps_params(const uint8_t *nal, int len, sps_info *out);

#endif
