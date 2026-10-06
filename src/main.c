/* rtspcam - lightweight single-camera RTSP recorder with motion-vector triggering (C version).
 *
 * Given MP4/MOV files instead of an rtsp:// URL it only writes their .mvmap motion maps
 * (same settings, same detector), e.g. for recordings of other systems.
 *
 * One process per camera. The video is never decoded or re-encoded: RTP packets are
 * depacketized and stored (stream copy) in fragmented MP4 files; the motion vectors
 * are read straight from the H.264 entropy layer (mvparse) and drive the same state
 * machine as rtspcam.py (pre-roll, post-roll, hysteresis, .mvmap sidecar). */
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "live.h"
#include "log.h"
#include "mp4_reader.h"
#include "mp4_writer.h"
#include "options.h"
#include "rtp_h264.h"
#include "rtsp.h"
#include "session.h"

#define FIX_REFS 4          /* max_num_ref_frames written into the SPS (see h264_sps.c) */

static volatile int stop_requested;

static void on_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static double mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* After a crash: give leftover .part files of an earlier run their final name (the
 * fragmented MP4 is playable up to its last complete fragment). */
static void recover_parts(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d)
        return;
    while ((e = readdir(d))) {
        char p[4096];
        struct stat st;
        size_t n = strlen(e->d_name);
        if (e->d_name[0] == '.')
            continue;
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        if (stat(p, &st) < 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 4)
                recover_parts(p, depth + 1);
        } else if ((n > 9 && !strcmp(e->d_name + n - 9, ".mp4.part")) ||
                   (n > 11 && !strcmp(e->d_name + n - 11, ".mvmap.part")) ||
                   (n > 11 && !strcmp(e->d_name + n - 11, ".mvvec.part"))) {
            char final[4096];
            struct stat st2;
            snprintf(final, sizeof(final), "%s", p);
            final[strlen(final) - 5] = 0;
            if (stat(final, &st2) == 0)
                continue;
            log_info("recovering %s", p);
            if (rename(p, final) < 0)
                log_warn("cannot rename %s", p);
        }
    }
    closedir(d);
}

/* ---------------------------------------------------------------- file input: .mvmap only */

/* Writes <file>.mvmap (next to the file, extension replaced) with the given settings. */
static int analyse_file(const rc_opts *a, const char *path)
{
    rc_opts fo = *a;
    mp4_file m;
    rc_session *s;
    char err[512], *map, *vec;
    int want_map, want_vec;
    const char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
    size_t base = dot && (!slash || dot > slash) ? (size_t)(dot - path) : strlen(path);
    struct stat st;
    uint8_t *buf = NULL;
    size_t cap = 0;
    FILE *f;
    int i, ret = -1;
    clock_t c0 = clock();

    map = malloc(base + 8);
    vec = malloc(base + 8);
    if (!map || !vec) {
        free(map);
        free(vec);
        return -1;
    }
    memcpy(map, path, base);
    strcpy(map + base, ".mvmap");
    memcpy(vec, path, base);
    strcpy(vec + base, ".mvvec");
    /* write what is missing; existing files only with --force */
    want_map = a->force || stat(map, &st) != 0;
    want_vec = a->vectors && (a->force || stat(vec, &st) != 0);
    if (!want_map && !want_vec) {
        if (a->vectors)
            log_warn("%s and %s exist, skipped (use --force to overwrite)", map, vec);
        else
            log_warn("%s exists, skipped (use --force to overwrite)", map);
        free(map);
        free(vec);
        return 0;
    }
    if (mp4_open_index(path, &m, err, sizeof(err)) < 0) {
        log_error("%s", err);
        free(map);
        return -1;
    }
    f = fopen(path, "rb");
    fo.always = 1;                  /* one map for the whole file; the triggers are still computed */
    fo.max_segment = 1e300;
    fo.map = want_map;
    fo.vectors = want_vec;
    s = f ? rcs_create(&fo, 0, &rcs_null_ops, NULL) : NULL;
    if (!s || rcs_set_map_path(s, map) < 0)
        goto end;
    rcs_set_stream(s, m.avcc, m.avcc_size, 1, (int)m.timescale, m.nal_length_size);
    for (i = 0; i < m.n_samples && !stop_requested; i++) {
        const mp4_sample *p = &m.samples[i];
        if (p->size > cap) {
            uint8_t *nb = realloc(buf, p->size);
            if (!nb)
                goto end;
            buf = nb;
            cap = p->size;
        }
        if (fseeko(f, (off_t)p->offset, SEEK_SET) || fread(buf, 1, p->size, f) != p->size) {
            log_warn("%s: read error at sample %d", path, i);
            break;
        }
        rcs_packet(s, buf, (int)p->size, p->dts, p->pts, p->key, 0);
    }
    rcs_free(s);                    /* writes the map */
    s = NULL;
    log_info("%s: %d pictures, %.1f s -> %s%s%s (%.2f s CPU)", path, i,
             m.n_samples ? (double)(m.samples[m.n_samples - 1].dts - m.samples[0].dts) / m.timescale : 0.0,
             want_map ? map : "", want_map && want_vec ? ", " : "", want_vec ? vec : "",
             (double)(clock() - c0) / CLOCKS_PER_SEC);
    ret = 0;
end:
    rcs_free(s);
    if (f)
        fclose(f);
    free(buf);
    free(map);
    free(vec);
    mp4_free_index(&m);
    return ret;
}

typedef struct run_ctx {
    rc_session *s;
    long rtp, aus, keys, broken;
    int64_t ts_first, ts_last;
} run_ctx;

static int on_au(void *opaque, const uint8_t *au, int size, int64_t ts, int key, int broken)
{
    run_ctx *r = opaque;
    if (r->aus == 0)
        r->ts_first = ts;
    r->ts_last = ts;
    r->aus++;
    r->keys += key;
    if (log_debug_enabled && (r->aus <= 30 || r->aus % 25 == 0)) {
        char types[256];
        int i = 0, n = 0;
        while (i + 3 < size && n < (int)sizeof(types) - 8) {   /* NAL types after the start codes */
            if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
                n += snprintf(types + n, sizeof(types) - n, "%d ", au[i + 3] & 0x1F);
                i += 3;
            } else {
                i++;
            }
        }
        types[n] = 0;
        log_debug("picture %ld: ts %lld, %d bytes, key %d, broken %d, NAL types %s", r->aus, (long long)ts, size,
                  key, broken, types);
    }
    if (broken) {
        r->broken++;
        log_debug("incomplete access unit (lost RTP packets) at ts %lld", (long long)ts);
    }
    return rcs_packet(r->s, au, size, ts, ts, key, broken);
}

/* one connection; returns when the stream drops or a stop is requested */
static live_srv *live;          /* --live: kept over reconnects */

static void run_session(const rc_opts *a, const mp4_options *mo)
{
    rtsp_client *c;
    rtp_h264 *dp = NULL;
    run_ctx r = { 0 };
    const uint8_t *sprop;
    int sprop_len;
    double t_start = mono();

    {
        char shown[1024];
        rtsp_redact(a->url, shown, sizeof(shown));
        log_info("connecting to %s", shown);
    }
    c = rtsp_open(a->url, a->timeout, &stop_requested);
    if (!c)
        return;
    r.s = rcs_create(a, 0, &mp4_writer_ops, (void *)mo);     /* --viewonly: the session writes nothing */
    if (!r.s)
        goto end;
    if (live)
        rcs_set_live(r.s, live, mo->fix_refs);
    sprop = rtsp_sprop(c, &sprop_len);
    if (!sprop_len)
        log_info("no sprop-parameter-sets in the SDP: waiting for in-band SPS/PPS");
    rcs_set_stream(r.s, sprop, sprop_len, 1, rtsp_clock_rate(c), 0);
    dp = rtp_h264_create(rtsp_payload_type(c), on_au, &r);
    if (!dp)
        goto end;
    for (;;) {
        const uint8_t *pkt;
        int n = rtsp_read_rtp(c, &pkt, &stop_requested);
        if (n <= 0)
            break;
        r.rtp++;
        if (live)
            live_poll(live);            /* keep the viewers' data flowing between pictures */
        if (rtp_h264_push(dp, pkt, n) < 0) {
            log_error("processing error");
            break;
        }
    }
end:
    if (c)
        log_info("session: %ld RTP packets (%ld sequence gaps), %ld pictures (%ld key, %ld incomplete), "
                 "stream time %.1f s in %.1f s", r.rtp, rtp_h264_seq_gaps(dp), r.aus, r.keys, r.broken,
                 r.aus ? (double)(r.ts_last - r.ts_first) / rtsp_clock_rate(c) : 0.0, mono() - t_start);
    rtp_h264_free(dp);
    rcs_free(r.s);                      /* closes an open recording */
    rtsp_close(c);
}

int main(int argc, char **argv)
{
    rc_opts a;
    mp4_options mo;
    char err[512], root[4096];
    int backoff = 1, r;
    struct sigaction sa;

    r = rc_parse_args(&a, argc, argv, err, sizeof(err));
    if (r == 1)
        return 0;
    if (r < 0) {
        rc_usage(argv[0]);
        fprintf(stderr, "%s: error: %s\n", argv[0], err);
        return 2;
    }
    if (a.fix == FIX_REMUX) {
        fprintf(stderr, "%s: error: --fix remux is not supported, use inline or off\n", argv[0]);
        return 2;
    }
    log_setup(a.name, a.debug);
    mo.fix_refs = a.fix == FIX_INLINE ? FIX_REFS : 0;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (strncasecmp(a.url, "rtsp://", 7)) {    /* files: generate the motion maps, no recording */
        int i, failed = 0;
        for (i = 0; i < a.n_inputs && !stop_requested; i++)
            failed += analyse_file(&a, a.inputs[i]) < 0;
        rc_opts_free(&a);
        return failed ? 1 : 0;
    }

    if (a.viewonly) {                           /* everything as usual, but nothing is written */
        a.map = 0;
        a.vectors = 0;
        if (!a.live)
            log_warn("--viewonly without --live: the camera is only analysed (alarms in the log)");
    } else {
        snprintf(root, sizeof(root), "%s/%s", a.out, a.name);
        recover_parts(root, 0);
    }
    if (a.live) {
        char sp[4096];
        mkdir_p(a.live);
        snprintf(sp, sizeof(sp), "%s/%s.sock", a.live, a.name);
        live = live_open(sp);
    }

    while (!stop_requested) {
        double started = mono(), until;
        run_session(&a, &mo);
        if (stop_requested)
            break;
        backoff = mono() - started > 60 ? 1 : (backoff * 2 > 60 ? 60 : backoff * 2);
        log_info("reconnecting in %ds", backoff);
        until = mono() + backoff;
        while (!stop_requested && mono() < until) {
            if (live)
                live_poll(live);        /* viewers may connect or leave while the camera is away */
            usleep(100000);
        }
    }
    log_info("stopped");
    live_close(live);
    rc_opts_free(&a);
    return 0;
}
