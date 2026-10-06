/* Recording state machine, a port of run_session() / Segment in rtspcam.py. */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>

#include "live.h"
#include "log.h"
#include "mp4_writer.h"
#include "mvmap_writer.h"
#include "mvparse.h"
#include "session.h"

#define NOKEY_WARN_S 30.0       /* warn when no key frame arrived for this long at the start */
#define ERR_STREAK_WARN 50      /* warn after this many consecutive unparseable pictures */
#define NOPS_WARN_S 10.0        /* warn when the slices had no SPS/PPS for this long */

/* a packet waiting in the pre-roll buffer, with its map record */
typedef struct buf_pkt {
    rc_packet p;            /* p.data points to an owned copy */
    int has_rec;
    mt_record rec;          /* rec.cells owned */
    uint8_t *vec;           /* motion field (gw*gh bytes, owned) or NULL */
    int vec_n;
    int vcl_bytes;
} buf_pkt;

typedef struct segment {
    char *path;
    void *video;
    mvmap_writer *map;
    mvmap_writer *vec;
    int vec_n;              /* grid size the .mvvec header announces */
    int have_t0;
    double t0, first_ts, last_ts;
    int failed;             /* a write failed (logged once) */
} segment;

struct rc_session {
    const rc_opts *a;
    const rc_video_ops *ops;
    void *opaque;
    rc_stream_info si;
    uint8_t *extradata;

    mvp_ctx *mvp;
    mt_det det;

    double wall0, ts0, last_ts, last_motion_ts, seg_start_ts;
    int fixed_wall;         /* wall0 was given: file names from wall0 + stream time */
    int since_key;          /* pictures since the last key frame (--skip-after-key) */
    int have_ts0, synced, moving_now;
    int viewonly;           /* --viewonly, or switched to it after a disk failure: no files */
    /* stream diagnostics, each warned once */
    double unsync_t0;       /* stream time of the first packet while waiting for a key frame */
    int have_unsync_t0, warned_nokey, warned_unsupported, warned_errors, err_streak;
    double nops_t0;         /* stream time since when the slices refer to a missing SPS/PPS */
    int have_nops_t0, warned_nops;

    buf_pkt *buf;
    int buf_n, buf_cap;
    segment *writer;
    char *map_path;         /* file analysis: fixed .mvmap path */

    struct live_srv *live;  /* live output (not owned) */
    mp4_live *live_mux;
    int live_fix;
    int live_alarm, live_rec;       /* last status sent */
    double live_status_ts;

    /* --vectors: motion field of the current picture */
    uint8_t *vec;
    double *vec_best;
    int vec_cap;
};

void rcs_set_live(rc_session *s, struct live_srv *live, int fix_refs)
{
    s->live = live;
    s->live_fix = fix_refs;
}

/* one packet to the live viewers (they get the previous picture, see mp4_live_frame) */
static void live_packet(rc_session *s, const rc_packet *p)
{
    const uint8_t *frag;
    size_t len;
    int new_init, key, r;
    if (!s->live_mux)
        s->live_mux = mp4_live_create(&s->si, s->live_fix);
    if (s->live_mux) {
        r = mp4_live_frame(s->live_mux, p, &frag, &len, &new_init, &key);
        if (new_init) {
            size_t il;
            const uint8_t *init = mp4_live_init(s->live_mux, &il);
            live_send_init(s->live, init, il);
        }
        if (r == 1)
            live_send_frame(s->live, frag, len, key);
    }
    live_poll(s->live);
}

int rcs_set_map_path(rc_session *s, const char *path)
{
    free(s->map_path);
    s->map_path = strdup(path);
    s->synced = 1;
    return s->map_path ? 0 : -1;
}

int mkdir_p(const char *dir)
{
    char tmp[4096];
    size_t i, n = strlen(dir);
    if (n == 0 || n >= sizeof(tmp))
        return -1;
    memcpy(tmp, dir, n + 1);
    for (i = 1; i <= n; i++) {
        if (tmp[i] == '/' || tmp[i] == 0) {
            char c = tmp[i];
            tmp[i] = 0;
            if (mkdir(tmp, 0777) < 0 && errno != EEXIST)
                return -1;
            tmp[i] = c;
        }
    }
    return 0;
}

/* the "video writer" of view-only mode */
static void *null_open(void *opaque, const char *path, const rc_stream_info *si)
{
    (void)opaque; (void)path; (void)si;
    return (void *)1;
}

static int null_write(void *h, const rc_packet *p)
{
    (void)h; (void)p;
    return 0;
}

static int null_close(void *h)
{
    (void)h;
    return 0;
}

const rc_video_ops rcs_null_ops = { null_open, null_write, null_close };
#define null_ops rcs_null_ops

/* Disk full, unwritable directory, write error: from now on the session behaves as with
 * --viewonly (analysis, alarms and --live go on, nothing is written). Recording resumes with a
 * restart of the program, after the space was freed. */
static void go_viewonly(rc_session *s, const char *why)
{
    if (s->viewonly)
        return;
    s->viewonly = 1;
    log_error("%s: switching to view-only mode (no recordings, no .mvmap/.mvvec); free space and restart the "
              "recorder", why);
}

/* MB free on the output disk (the root directory is created if needed), or -1 if unknown */
static double free_mb(const char *dir)
{
    struct statvfs vs;
    if (statvfs(dir, &vs) < 0 && (mkdir_p(dir) < 0 || statvfs(dir, &vs) < 0))
        return -1;
    return (double)vs.f_bavail * vs.f_frsize / (1024.0 * 1024.0);
}

static double now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

rc_session *rcs_create(const rc_opts *a, double wall0, const rc_video_ops *ops, void *opaque)
{
    rc_session *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->a = a;
    s->ops = ops;
    s->opaque = opaque;
    s->wall0 = wall0 > 0 ? wall0 : now();
    s->fixed_wall = wall0 > 0;
    s->viewonly = a->viewonly;
    s->last_motion_ts = -1e9;
    s->si.tb_num = 1;
    s->si.tb_den = 90000;
    s->mvp = mvp_create();
    if (s->mvp)
        mvp_set_max_ref_dist(s->mvp, a->max_ref_dist);
    if (!s->mvp || mt_init(&s->det, a, 1280, 720) < 0) {
        rcs_free(s);
        return NULL;
    }
    return s;
}

int rcs_set_stream(rc_session *s, const uint8_t *extradata, int size, int tb_num, int tb_den,
                   int nal_length_size)
{
    int w, h;
    free(s->extradata);
    s->extradata = NULL;
    s->si.extradata = NULL;
    s->si.extradata_size = 0;
    if (extradata && size > 0) {
        s->extradata = malloc(size);
        if (!s->extradata)
            return -1;
        memcpy(s->extradata, extradata, size);
        s->si.extradata = s->extradata;
        s->si.extradata_size = size;
        mvp_set_extradata(s->mvp, extradata, size);
    }
    s->si.tb_num = tb_num;
    s->si.tb_den = tb_den;
    s->si.nal_length_size = nal_length_size;
    if (mvp_get_size(s->mvp, &w, &h) == 0 && (w != s->det.w || h != s->det.h)) {
        mt_free(&s->det);
        if (mt_init(&s->det, s->a, w, h) < 0)
            return -1;
    }
    s->si.width = s->det.w;
    s->si.height = s->det.h;
    log_info("stream: %dx%d", s->det.w, s->det.h);
    return 0;
}

/* <out>/<name>/<YYYY-MM-DD>/<name>_<HHMMSS>.mp4 for the given wall clock time; if that name is
 * taken (a recording from the repeated hour at the end of DST, a restarted clock), _1, _2, ... */
static char *out_path(const rc_opts *a, double wall)
{
    long long us = llround(wall * 1e6);
    time_t t = (time_t)(us >= 0 ? us / 1000000 : -((-us + 999999) / 1000000));
    struct tm tm;
    struct stat st;
    char day[16], hms[16], part[4096];
    size_t n;
    char *p;
    int k;

    localtime_r(&t, &tm);
    strftime(day, sizeof(day), "%Y-%m-%d", &tm);
    strftime(hms, sizeof(hms), "%H%M%S", &tm);
    n = strlen(a->out) + 2 * strlen(a->name) + 64;
    p = malloc(n);
    if (!p)
        return NULL;
    for (k = 0; k < 100; k++) {
        if (k == 0)
            snprintf(p, n, "%s/%s/%s/%s_%s.mp4", a->out, a->name, day, a->name, hms);
        else
            snprintf(p, n, "%s/%s/%s/%s_%s_%d.mp4", a->out, a->name, day, a->name, hms, k);
        snprintf(part, sizeof(part), "%s.part", p);
        if (a->viewonly || (stat(p, &st) < 0 && stat(part, &st) < 0))
            break;
    }
    return p;
}

static char *map_header(rc_session *s)
{
    const rc_opts *a = s->a;
    size_t cap = 1024 + 64 * (size_t)a->n_ignore + 8 * strlen(a->name), n;
    char *h = malloc(cap), num[9][40], name[512];
    int i;
    if (!h)
        return NULL;
    json_put_string(name, sizeof(name), a->name);
    json_put_double(num[0], sizeof(num[0]), a->mv_min);
    json_put_double(num[1], sizeof(num[1]), a->min_blocks);
    json_put_double(num[2], sizeof(num[2]), a->global_limit);
    json_put_double(num[3], sizeof(num[3]), a->pre_roll);
    json_put_double(num[4], sizeof(num[4]), a->post_roll);
    n = snprintf(h, cap, "{\"camera\": %s, \"w\": %d, \"h\": %d, \"gw\": %d, \"gh\": %d, \"params\": "
                 "{\"mv_min\": %s, \"min_cluster\": %d, \"min_blocks\": %s, \"global_limit\": %s, "
                 "\"window\": %d, \"trigger_frames\": %d, \"max_ref_dist\": %d, \"skip_after_key\": %d, \"ignore\": ",
                 name, s->det.w, s->det.h, s->det.gw, s->det.gh, num[0], a->min_cluster, num[1], num[2],
                 a->window, a->trigger_frames, a->max_ref_dist, a->skip_after_key);
    if (!a->n_ignore) {
        n += snprintf(h + n, cap - n, "null");
    } else {
        n += snprintf(h + n, cap - n, "[");
        for (i = 0; i < a->n_ignore; i++) {
            char z[256];
            json_put_string(z, sizeof(z), a->ignore[i]);
            n += snprintf(h + n, cap - n, "%s%s", i ? ", " : "", z);
        }
        n += snprintf(h + n, cap - n, "]");
    }
    snprintf(h + n, cap - n, ", \"pre_roll\": %s, \"post_roll\": %s}}", num[3], num[4]);
    return h;
}

static segment *segment_open(rc_session *s, const char *path)
{
    segment *g = calloc(1, sizeof(*g));
    char *dir = strdup(path), *slash;
    if (!g || !dir)
        goto fail;
    g->path = strdup(path);
    slash = strrchr(dir, '/');
    if (slash && !s->viewonly) {
        *slash = 0;
        if (mkdir_p(dir) < 0) {
            log_error("cannot create directory %s", dir);
            goto fail;
        }
    }
    g->video = (s->viewonly ? &null_ops : s->ops)->open(s->opaque, path, &s->si);
    if (!g->video)
        goto fail;
    if (s->a->map && !s->viewonly) {
        char *hdr = map_header(s);
        size_t n = strlen(path);
        char *mp = malloc(n + 8);
        if (hdr && mp && s->map_path) {
            g->map = mvmap_open(s->map_path, hdr);
            if (!g->map)
                log_warn("cannot write %s", s->map_path);
        } else if (hdr && mp) {
            const char *dot = strrchr(path, '.');
            size_t base = dot && dot > strrchr(path, '/') ? (size_t)(dot - path) : n;
            memcpy(mp, path, base);
            strcpy(mp + base, ".mvmap");
            g->map = mvmap_open(mp, hdr);
            if (!g->map)
                log_warn("cannot write %s", mp);
        }
        free(hdr);
        free(mp);
    }
    if (s->a->vectors && !s->viewonly) {
        char vp[4096], hdr[1024], name[512];
        const char *mp = s->map_path ? s->map_path : path;
        const char *dot = strrchr(mp, '.'), *sl = strrchr(mp, '/');
        size_t base = dot && (!sl || dot > sl) ? (size_t)(dot - mp) : strlen(mp);
        if (base + 7 < sizeof(vp)) {
            memcpy(vp, mp, base);
            strcpy(vp + base, ".mvvec");
            json_put_string(name, sizeof(name), s->a->name);
            snprintf(hdr, sizeof(hdr), "{\"camera\": %s, \"w\": %d, \"h\": %d, \"gw\": %d, \"gh\": %d}",
                     name, s->det.w, s->det.h, s->det.gw, s->det.gh);
            g->vec = mvvec_open(vp, hdr);
            g->vec_n = s->det.gw * s->det.gh;
            if (!g->vec)
                log_warn("cannot write %s", vp);
        }
    }
    free(dir);
    return g;
fail:
    if (g)
        free(g->path);
    free(g);
    free(dir);
    return NULL;
}

/* returns -1 if the video or a sidecar could not be written (disk full?) */
static int segment_write(rc_session *s, segment *g, const rc_packet *p, const mt_record *rec, const uint8_t *vec,
                         int vec_n, int vcl_bytes)
{
    int r = 0;
    if (!g->have_t0) {
        g->have_t0 = 1;
        g->t0 = p->ts;
        g->first_ts = p->ts;
    }
    g->last_ts = p->ts;
    if ((s->viewonly ? &null_ops : s->ops)->write(g->video, p) < 0)
        r = -1;
    if (g->map && rec && mvmap_write(g->map, (long)nearbyint((p->ts - g->t0) * 1000), rec) < 0)
        r = -1;
    if (g->vec &&   /* a grid of another size (stream resolution changed) is left out */
        mvvec_write(g->vec, (long)nearbyint((p->ts - g->t0) * 1000), (uint32_t)p->size, (uint32_t)vcl_bytes,
                    vec_n == g->vec_n ? vec : NULL, g->vec_n) < 0)
        r = -1;
    if (r < 0)
        g->failed = 1;          /* the writer has logged it; finish() closes the recording */
    return r;
}

/* <video path with the extension replaced by ext>, malloc'ed */
static char *sidecar_path(const char *path, const char *ext)
{
    const char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
    size_t base = dot && (!slash || dot > slash) ? (size_t)(dot - path) : strlen(path);
    char *p = malloc(base + strlen(ext) + 1);
    if (p) {
        memcpy(p, path, base);
        strcpy(p + base, ext);
    }
    return p;
}

/* returns -1 if the video or a sidecar could not be written or closed. Without a playable
 * video file the sidecars are removed too. */
static int segment_close(rc_session *s, segment *g)
{
    int r = g->failed ? -1 : 0, had_map = g->map != NULL, had_vec = g->vec != NULL;
    if (g->map && mvmap_close(g->map) < 0)
        r = -1;
    if (g->vec && mvmap_close(g->vec) < 0)
        r = -1;
    if ((s->viewonly ? &null_ops : s->ops)->close(g->video) < 0) {
        r = -1;
        if (!s->map_path) {
            char *mp = had_map ? sidecar_path(g->path, ".mvmap") : NULL;
            char *vp = had_vec ? sidecar_path(g->path, ".mvvec") : NULL;
            if (mp)
                remove(mp);
            if (vp)
                remove(vp);
            free(mp);
            free(vp);
        }
    }
    free(g->path);
    free(g);
    return r;
}

static void buf_free_pkt(buf_pkt *b)
{
    free((void *)b->p.data);
    free(b->rec.cells);
    free(b->vec);
}

static void buf_drop_front(rc_session *s, int n)
{
    int i;
    for (i = 0; i < n; i++)
        buf_free_pkt(&s->buf[i]);
    memmove(s->buf, s->buf + n, sizeof(*s->buf) * (s->buf_n - n));
    s->buf_n -= n;
}

static int buf_append(rc_session *s, const rc_packet *p, const mt_record *rec, const uint8_t *vec, int vec_n,
                      int vcl_bytes)
{
    buf_pkt *b;
    if (s->buf_n == s->buf_cap) {
        int cap = s->buf_cap ? s->buf_cap * 2 : 256;
        buf_pkt *nb = realloc(s->buf, sizeof(*nb) * cap);
        if (!nb)
            return -1;
        s->buf = nb;
        s->buf_cap = cap;
    }
    b = &s->buf[s->buf_n];
    memset(b, 0, sizeof(*b));
    b->p = *p;
    b->p.data = malloc(p->size ? p->size : 1);
    if (!b->p.data)
        return -1;
    memcpy((void *)b->p.data, p->data, p->size);
    if (rec) {
        b->has_rec = 1;
        b->rec = *rec;
        b->rec.cells = NULL;
        if (rec->n_cells) {
            b->rec.cells = malloc(sizeof(*rec->cells) * rec->n_cells);
            if (!b->rec.cells) {
                free((void *)b->p.data);
                return -1;
            }
            memcpy(b->rec.cells, rec->cells, sizeof(*rec->cells) * rec->n_cells);
        }
    }
    if (vec) {
        b->vec = malloc(vec_n);
        if (b->vec)
            memcpy(b->vec, vec, vec_n);
    }
    b->vec_n = b->vec ? vec_n : 0;
    b->vcl_bytes = vcl_bytes;
    s->buf_n++;
    return 0;
}

static void start(rc_session *s, double ts)
{
    double first_ts = s->buf[0].p.ts;
    /* the wall clock time of the first buffered picture: now minus the buffered stream time
     * (a camera's clock drifts and its timestamps may jump; the session start + stream time
     * would drift with it) */
    double wall = s->fixed_wall ? s->wall0 + first_ts - s->ts0 : now() - (ts - first_ts);
    char *path;
    segment *g;
    int i;

    /* A file must begin with a key frame. After a cut at a non-key frame (--max-tail, a write
     * error) the buffer may begin mid-GOP: those pictures are dropped; if no key frame is
     * buffered yet, the recording starts with the next one (motion is kept in the buffer). */
    if (!s->map_path) {
        for (i = 0; i < s->buf_n && !s->buf[i].p.key; i++)
            ;
        if (i == s->buf_n)
            return;
        if (i > 0)
            buf_drop_front(s, i);
        first_ts = s->buf[0].p.ts;
        wall = s->fixed_wall ? s->wall0 + first_ts - s->ts0 : now() - (ts - first_ts);
    }
    if (!s->viewonly && !s->map_path && s->a->min_free > 0) {
        double mb = free_mb(s->a->out);
        if (mb >= 0 && mb < s->a->min_free) {
            char why[256];
            snprintf(why, sizeof(why), "only %.0f MB free in %s (--min-free %.0f)", mb, s->a->out, s->a->min_free);
            go_viewonly(s, why);
        }
    }
    path = s->map_path ? strdup(s->map_path) : out_path(s->a, wall);
    g = path ? segment_open(s, path) : NULL;
    if (!g && !s->viewonly) {
        char why[4600];
        snprintf(why, sizeof(why), "cannot start recording %s", path ? path : "");
        go_viewonly(s, why);
        g = path ? segment_open(s, path) : NULL;      /* as a view-only "recording" */
    }
    if (!g) {
        log_error("cannot start recording %s", path ? path : "");
        free(path);
        return;                 /* the buffer is kept (trimmed to the pre-roll); the next packet retries */
    }
    for (i = 0; i < s->buf_n; i++)
        segment_write(s, g, &s->buf[i].p, s->buf[i].has_rec ? &s->buf[i].rec : NULL, s->buf[i].vec,
                      s->buf[i].vec_n, s->buf[i].vcl_bytes);
    buf_drop_front(s, s->buf_n);
    s->writer = g;
    s->seg_start_ts = first_ts;
    if (s->viewonly)
        log_info("EVENT start (view only, not recorded; pre-roll %.1fs, stream t=%.1fs)", ts - first_ts, ts - s->ts0);
    else if (!s->map_path)
        log_info("REC start %s (pre-roll %.1fs, stream t=%.1fs)", path, ts - first_ts, ts - s->ts0);
    free(path);
}

static void finish(rc_session *s)
{
    segment *g = s->writer;
    char *path;
    double len;
    if (!g)
        return;
    s->writer = NULL;
    path = strdup(g->path);
    len = g->last_ts - g->first_ts;
    if (segment_close(s, g) < 0 && !s->viewonly) {
        char why[4600];
        snprintf(why, sizeof(why), "cannot write %s", path ? path : "");
        go_viewonly(s, why);
    }
    if (s->viewonly)
        log_info("EVENT stop  (view only, %.1fs, stream t=%.1fs)", len, s->last_ts - s->ts0);
    else if (!s->map_path)
        log_info("REC stop  %s (%.1fs, stream t=%.1fs)", path ? path : "", len, s->last_ts - s->ts0);
    free(path);
}

/* Not recording, a key frame has just been buffered: drop what cannot be part of a recording.
 * After a cut at a non-key frame (--max-tail) the buffer begins mid-GOP: a file cannot start
 * with those pictures. Then drop the GOPs that are fully older than the pre-roll window. */
static void trim_buffer(rc_session *s, double ts)
{
    int i;
    for (i = 0; i < s->buf_n && !s->buf[i].p.key; i++)
        ;
    if (i > 0 && i < s->buf_n)
        buf_drop_front(s, i);
    for (;;) {
        int k1 = -1, seen = 0;
        for (i = 0; i < s->buf_n; i++)
            if (s->buf[i].p.key && ++seen == 2) {
                k1 = i;
                break;
            }
        if (k1 < 0 || !(ts - s->buf[k1].p.ts >= s->a->pre_roll))
            break;
        buf_drop_front(s, k1);
    }
}

/* --vectors: longest vector per cell of an analysed picture (session-owned buffer) */
static const uint8_t *vec_grid(rc_session *s, const mvp_frame *fr, int *n)
{
    int cells = s->det.gw * s->det.gh;
    if (cells > s->vec_cap) {
        uint8_t *v = realloc(s->vec, cells);
        double *b;
        if (v)
            s->vec = v;
        b = realloc(s->vec_best, sizeof(double) * cells);
        if (b)
            s->vec_best = b;
        if (!v || !b)
            return NULL;
        s->vec_cap = cells;
    }
    mvvec_grid(fr->mv, fr->n_mv, s->det.gw, s->det.gh, s->vec_best, s->vec);
    *n = cells;
    return s->vec;
}

int rcs_packet(rc_session *s, const uint8_t *data, int size, int64_t dts, int64_t pts, int key, int broken)
{
    const rc_opts *a = s->a;
    rc_packet p;
    mvp_frame fr;
    mt_record rec, *recp = NULL;
    const uint8_t *vec = NULL;
    int vec_n = 0, skip;
    double ts;
    int want;

    if (size <= 0)
        return 0;
    ts = (double)(dts * s->si.tb_num) / s->si.tb_den;
    if (!s->synced) {               /* decoding mid-GOP yields garbage vectors */
        if (!key) {
            if (!s->have_unsync_t0) {
                s->have_unsync_t0 = 1;
                s->unsync_t0 = ts;
            } else if (!s->warned_nokey && ts - s->unsync_t0 >= NOKEY_WARN_S) {
                s->warned_nokey = 1;
                log_warn("no key frame (IDR) in the first %.0f s of the stream: nothing is analysed or recorded "
                         "until one arrives (does the camera send I frames?)", NOKEY_WARN_S);
            }
            return 0;
        }
        s->synced = 1;
    }
    s->last_ts = ts;
    if (!s->have_ts0) {
        s->have_ts0 = 1;
        s->ts0 = ts;
    }

    /* --- analysis --- */
    skip = mvp_decode(s->mvp, data, size, s->si.nal_length_size, &fr) == 0 && fr.type != MVP_NONE;
    if (fr.type == MVP_UNSUPPORTED && fr.no_ps) {
        /* usually temporary: the parameter sets may come in-band later (with a key frame) */
        if (!s->have_nops_t0) {
            s->have_nops_t0 = 1;
            s->nops_t0 = ts;
        } else if (!s->warned_nops && ts - s->nops_t0 >= NOPS_WARN_S) {
            s->warned_nops = 1;
            log_warn("no SPS/PPS for %.0f s (none in the SDP, none in the stream so far): the pictures cannot be "
                     "analysed until the camera sends them", NOPS_WARN_S);
        }
    } else if (fr.type != MVP_NONE && s->have_nops_t0) {
        if (s->warned_nops)
            log_info("SPS/PPS arrived, the pictures are analysed from now on");
        s->have_nops_t0 = s->warned_nops = 0;
    }
    if (fr.type == MVP_UNSUPPORTED && !fr.no_ps && !s->warned_unsupported) {
        s->warned_unsupported = 1;
        log_warn("the stream uses %s: the motion vectors cannot be read, there is no motion detection "
                 "(recording only with --always)", fr.reason ? fr.reason : "an unsupported coding");
    }
    if (fr.type == MVP_P && !broken) {
        s->err_streak = fr.errors ? s->err_streak + 1 : 0;
        if (s->err_streak == ERR_STREAK_WARN && !s->warned_errors) {
            s->warned_errors = 1;
            log_warn("the last %d pictures could not be parsed (damaged or unusual stream?): no motion detection "
                     "while this lasts", ERR_STREAK_WARN);
        }
    }
    if (skip) {
        s->since_key = fr.key ? 0 : s->since_key + 1;
        /* hierarchical P frames: the reference is several frames back, moving objects are
         * intra-coded there and the vectors do not show the motion; low-bitrate cameras
         * repaint the picture right after a key frame (--skip-after-key): leave them out */
        skip = fr.type == MVP_SKIPPED || (fr.type == MVP_P && a->max_ref_dist > 0 && fr.ref_dist > a->max_ref_dist) ||
               (!fr.key && fr.type == MVP_P && s->since_key <= a->skip_after_key);
    }
    if (skip) {
        if (a->map) {
            rec.cluster = 0;
            rec.blocks = 0;
            rec.flags = F_SKIP | (s->moving_now ? F_TRIG : 0);
            rec.n_cells = 0;
            rec.cells = NULL;
            recp = &rec;
        }
    } else if (!broken && (fr.type == MVP_P || fr.type == MVP_I) && !fr.errors) {
        int m = mt_update(&s->det, &fr);
        if (a->vectors && m >= 0)          /* analysed picture (an all-intra one gets a zero grid) */
            vec = vec_grid(s, &fr, &vec_n);
        if (m >= 0) {
            s->moving_now = mt_triggered(&s->det);
            if (m && (s->moving_now || s->writer))
                s->last_motion_ts = ts;
        }
        if (a->map) {
            mt_record_get(&s->det, m, s->moving_now, &rec);
            recp = &rec;
        }
    }
    if (s->det.w != s->si.width || s->det.h != s->si.height) {   /* the camera was reconfigured */
        s->si.width = s->det.w;
        s->si.height = s->det.h;
        log_info("stream: %dx%d", s->det.w, s->det.h);
    }
    if (a->map && !recp) {          /* no usable picture: keep one record per packet */
        rec.cluster = 0;
        rec.blocks = 0;
        rec.flags = F_NODATA | (s->moving_now ? F_TRIG : 0);
        rec.n_cells = 0;
        rec.cells = NULL;
        recp = &rec;
    }
    want = a->always || s->moving_now || ts - s->last_motion_ts < a->post_roll;

    p.data = data;
    p.size = size;
    p.dts = dts;
    p.pts = pts;
    p.key = key;
    p.ts = ts;
    if (s->live)
        live_packet(s, &p);
    if (s->live && (s->moving_now != s->live_alarm || (s->writer != NULL) != s->live_rec ||
                    ts - s->live_status_ts >= 2 || ts < s->live_status_ts)) {
        /* alarm/recording state for the viewers (the recording state is the one before this
         * packet: a start or stop shows up with the next one) */
        s->live_alarm = s->moving_now;
        s->live_rec = s->writer != NULL;
        s->live_status_ts = ts;
        live_send_status(s->live, s->live_alarm, s->live_rec, 1);
    }

    /* --- recording state machine --- */
    if (!s->writer) {
        if (buf_append(s, &p, recp, vec, vec_n, fr.vcl_bytes) < 0)
            return -1;
        if (want)
            start(s, ts);
        if (!s->writer && key)      /* not recording (or the start failed): bounded buffer */
            trim_buffer(s, ts);
    } else {
        int rotate = key && ts - s->seg_start_ts >= a->max_segment;
        if ((!want || rotate) && key) {
            finish(s);
            if (buf_append(s, &p, recp, vec, vec_n, fr.vcl_bytes) < 0)
                return -1;
            if (want)
                start(s, ts);
            if (!s->writer)
                trim_buffer(s, ts);
        } else if (!want && ts - s->last_motion_ts >= a->post_roll + a->max_tail) {
            finish(s);              /* no key frame arrived in time; cut anyway */
            if (buf_append(s, &p, recp, vec, vec_n, fr.vcl_bytes) < 0)
                return -1;
        } else if (segment_write(s, s->writer, &p, recp, vec, vec_n, fr.vcl_bytes) < 0) {
            /* disk full or similar: close what could be written; finish() switches to view-only */
            finish(s);
            if (buf_append(s, &p, recp, vec, vec_n, fr.vcl_bytes) < 0)
                return -1;
        }
    }
    return 0;
}

void rcs_free(rc_session *s)
{
    int i;
    if (!s)
        return;
    finish(s);
    for (i = 0; i < s->buf_n; i++)
        buf_free_pkt(&s->buf[i]);
    free(s->buf);
    mt_free(&s->det);
    mvp_free(s->mvp);
    free(s->extradata);
    free(s->map_path);
    mp4_live_free(s->live_mux);
    free(s->vec);
    free(s->vec_best);
    free(s);
}
