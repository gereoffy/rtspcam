/* Writer for the .mvmap motion map sidecar (format: see mvmap.py). */
#ifndef RC_MVMAP_WRITER_H
#define RC_MVMAP_WRITER_H

#include <stdint.h>
#include <stdio.h>

#include "motion.h"
#include "mvparse.h"

typedef struct mvmap_writer mvmap_writer;

/* Creates <path>.part with the given JSON header (without the version/cell/time_unit
 * keys, which are added here, like MapWriter does). */
mvmap_writer *mvmap_open(const char *path, const char *header_json);
int mvmap_write(mvmap_writer *m, long t_ms, const mt_record *r);
/* Finishes the zlib stream and renames <path>.part to <path> (also for .mvvec writers). */
int mvmap_close(mvmap_writer *m);

/* .mvvec sidecar (motion field, see mvmap.py): same framing with magic "RCMVVEC2"; one record
 * per packet: u32 t_ms, u8 has_grid, u32 frame_bytes (the packet as stored), u32 vcl_bytes (slice
 * RBSP bytes, see mvp_frame), then gw*gh bytes if has_grid. */
mvmap_writer *mvvec_open(const char *path, const char *header_json);
int mvvec_write(mvmap_writer *m, long t_ms, uint32_t frame_bytes, uint32_t vcl_bytes, const uint8_t *grid, int n);

/* motion field of one picture: the longest vector of each 16x16 cell (length floored to whole
 * pixels, so that "length >= mv_min" gives exactly the detector's moving cells for an integer
 * --mv-min), quantised as described in the .mvvec header. best is a gw*gh scratch array. */
void mvvec_grid(const mvp_mv *mv, int n_mv, int gw, int gh, double *best, uint8_t *out);

/* JSON helpers for building the header */
void json_put_string(char *buf, size_t size, const char *s);
void json_put_double(char *buf, size_t size, double v);

#endif
