/* One camera session: motion analysis and the recording state machine of
 * rtspcam.py's run_session() (pre-roll, post-roll, hysteresis, segmenting at key frames).
 * The video output is pluggable (rc_video_ops); the .mvmap sidecar is written here. */
#ifndef RC_SESSION_H
#define RC_SESSION_H

#include <stdint.h>

#include "motion.h"
#include "options.h"

typedef struct rc_packet {
    const uint8_t *data;
    int size;
    int64_t dts, pts;       /* in the stream time base */
    int key;
    double ts;              /* dts in seconds */
} rc_packet;

typedef struct rc_stream_info {
    const uint8_t *extradata;   /* avcC or Annex B parameter sets, may be NULL */
    int extradata_size;
    int tb_num, tb_den;
    int nal_length_size;        /* 0 = Annex B packets */
    int width, height;          /* from the SPS, 0 if unknown */
} rc_stream_info;

typedef struct rc_video_ops {
    /* open <path> (directories exist); return a handle or NULL */
    void *(*open)(void *opaque, const char *path, const rc_stream_info *si);
    int (*write)(void *h, const rc_packet *p);
    int (*close)(void *h);
} rc_video_ops;

typedef struct rc_session rc_session;

/* a video writer that writes nothing (file analysis: only the .mvmap is wanted) */
extern const rc_video_ops rcs_null_ops;

/* wall0: wall clock time of the session start; <= 0: file names come from the wall clock at the
 * time a recording starts (immune to the camera's clock drift and timestamp jumps). A given
 * wall0 (tests) names the files from wall0 + stream time instead, so that the names are
 * reproducible. */
rc_session *rcs_create(const rc_opts *a, double wall0, const rc_video_ops *ops, void *opaque);
int rcs_set_stream(rc_session *s, const uint8_t *extradata, int size, int tb_num, int tb_den,
                   int nal_length_size);
/* one access unit; data must stay valid only during the call. broken: the picture is incomplete
 * (lost packets): it is stored but not analysed (its vectors would be garbage). */
int rcs_packet(rc_session *s, const uint8_t *data, int size, int64_t dts, int64_t pts, int key, int broken);
/* File analysis: write the whole stream as one .mvmap to `path` (no file names from the clock)
 * and analyse from the first packet on (no waiting for a key frame), so the map times are
 * the file's times. Use with --always and an infinite --max-segment. */
int rcs_set_map_path(rc_session *s, const char *path);
/* Live output: every packet also goes, as one fragmented-MP4 fragment, to the live socket
 * (live.h; owned by the caller, it may outlive the session). fix_refs as for the MP4 writer. */
struct live_srv;
void rcs_set_live(rc_session *s, struct live_srv *live, int fix_refs);
/* closes an open recording and frees everything */
void rcs_free(rc_session *s);

int mkdir_p(const char *dir);

#endif
