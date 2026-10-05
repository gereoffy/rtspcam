/* Entry points for c/test_session.py (ctypes): a session with the same command line
 * as rtspcam.py whose "video writer" only logs the packets of every recording to
 * <path>.pkts ("dts key size" per line), so the segmenting can be compared with Python. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "session.h"

typedef struct test_session {
    rc_opts opts;
    rc_session *s;
} test_session;

static void *log_open(void *opaque, const char *path, const rc_stream_info *si)
{
    char *p = malloc(strlen(path) + 6);
    FILE *f;
    if (!p)
        return NULL;
    sprintf(p, "%s.pkts", path);
    f = fopen(p, "w");
    free(p);
    return f;
}

static int log_write(void *h, const rc_packet *p)
{
    return fprintf((FILE *)h, "%lld %d %d\n", (long long)p->dts, p->key, p->size) < 0 ? -1 : 0;
}

static int log_close(void *h)
{
    return fclose((FILE *)h);
}

static const rc_video_ops log_ops = { log_open, log_write, log_close };

test_session *rct_create(int argc, char **argv, double wall0)
{
    char err[256];
    test_session *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    if (rc_parse_args(&t->opts, argc, argv, err, sizeof(err)) != 0) {
        fprintf(stderr, "%s\n", err);
        free(t);
        return NULL;
    }
    log_setup(t->opts.name, t->opts.debug);
    t->s = rcs_create(&t->opts, wall0, &log_ops, NULL);
    if (!t->s) {
        rc_opts_free(&t->opts);
        free(t);
        return NULL;
    }
    return t;
}

int rct_set_stream(test_session *t, const uint8_t *ex, int size, int tb_num, int tb_den, int nal_length_size)
{
    return rcs_set_stream(t->s, ex, size, tb_num, tb_den, nal_length_size);
}

int rct_packet(test_session *t, const uint8_t *data, int size, long long dts, long long pts, int key)
{
    return rcs_packet(t->s, data, size, dts, pts, key);
}

void rct_quiet(int q)
{
    log_quiet = q;
}

void rct_free(test_session *t)
{
    if (!t)
        return;
    rcs_free(t->s);
    rc_opts_free(&t->opts);
    free(t);
}
