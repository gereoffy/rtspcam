/* H.264 CABAC tables from FFmpeg (LGPL-2.1-or-later), see h264_cabac_tables.c */
#ifndef H264_CABAC_TABLES_H
#define H264_CABAC_TABLES_H
#include <stdint.h>

#define H264_NORM_SHIFT_OFFSET 0
#define H264_LPS_RANGE_OFFSET 512
#define H264_MLPS_STATE_OFFSET 1024
#define H264_LAST_COEFF_FLAG_OFFSET_8x8_OFFSET 1280

extern const uint8_t ff_h264_cabac_tables[512 + 4*2*64 + 4*64 + 63];
extern const int8_t ff_cabac_context_init_I[1024][2];
extern const int8_t ff_cabac_context_init_PB[3][1024][2];
#endif
