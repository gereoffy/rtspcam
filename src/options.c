/* Command line and --config JSON handling, compatible with rtspcam.py's argparse setup:
 * the JSON keys are the option names with '_' (e.g. "mv_min"), the file sets the
 * defaults and explicit command-line flags win; --ignore on the command line is
 * appended to the zones from the file. */
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "options.h"

enum { T_STR, T_FLOAT, T_INT, T_TRUE, T_FALSE, T_APPEND, T_FIX };

typedef struct opt_def {
    const char *flag;       /* long flag */
    const char *shortflag;  /* or NULL */
    const char *dest;       /* JSON key */
    int type;
    size_t offset;
    const char *help;
} opt_def;

#define OFF(f) offsetof(rc_opts, f)

static const opt_def opts[] = {
    { "--name", "-n", "name", T_STR, OFF(name), "camera name (used in paths), required" },
    { "--config", NULL, "config", T_STR, OFF(config), "JSON file with tuned heuristics (saved by tune.py)" },
    { "--out", "-o", "out", T_STR, OFF(out), "output root directory (recordings)" },
    { "--timeout", NULL, "timeout", T_FLOAT, OFF(timeout), "network timeout in s (10)" },
    { "--always", NULL, "always", T_TRUE, OFF(always), "record continuously (segments only)" },
    { "--mv-min", NULL, "mv_min", T_FLOAT, OFF(mv_min), "min vector length in pixels (1.0)" },
    { "--min-blocks", NULL, "min_blocks", T_FLOAT, OFF(min_blocks), "min moving 16x16 blocks for a motion frame (4)" },
    { "--min-cluster", NULL, "min_cluster", T_INT, OFF(min_cluster), "if >0: largest connected group of moving cells instead of --min-blocks (0)" },
    { "--global-limit", NULL, "global_limit", T_FLOAT, OFF(global_limit), "ignore frames where more than this fraction moves (0.6)" },
    { "--window", NULL, "window", T_INT, OFF(window), "hysteresis window in P frames (15)" },
    { "--trigger-frames", NULL, "trigger_frames", T_INT, OFF(trigger_frames), "motion frames needed within the window (4)" },
    { "--max-ref-dist", NULL, "max_ref_dist", T_INT, OFF(max_ref_dist), "analyse only frames whose reference is at most N frames back, 0 = all (1)" },
    { "--skip-after-key", NULL, "skip_after_key", T_INT, OFF(skip_after_key), "do not analyse the first N pictures after a key frame (0)" },
    { "--ignore", NULL, "ignore", T_APPEND, OFF(ignore), "X0,Y0,X1,Y1 ignore zone as frame fractions, repeatable" },
    { "--pre-roll", NULL, "pre_roll", T_FLOAT, OFF(pre_roll), "seconds kept before the trigger (5)" },
    { "--post-roll", NULL, "post_roll", T_FLOAT, OFF(post_roll), "seconds kept after the last motion (8)" },
    { "--max-segment", NULL, "max_segment", T_FLOAT, OFF(max_segment), "split long events (600 s)" },
    { "--max-tail", NULL, "max_tail", T_FLOAT, OFF(max_tail), "max wait for a key frame to close a file (5 s)" },
    { "--fix", NULL, "fix", T_FIX, OFF(fix), "inline|remux|off: browser-friendly SPS (inline)" },
    { "--no-map", NULL, "map", T_FALSE, OFF(map), "do not write the .mvmap sidecar" },
    { "--live", NULL, "live", T_STR, OFF(live), "DIR: live stream for viewers on the socket DIR/<name>.sock" },
    { "--vectors", NULL, "vectors", T_TRUE, OFF(vectors), "also write <file>.mvvec: mean motion vector per 16x16 cell" },
    { "--cheap", NULL, "cheap", T_TRUE, OFF(cheap), "accepted for compatibility (no effect)" },
    { "--debug", NULL, "debug", T_TRUE, OFF(debug), "per-frame scores in the log" },
    { "--viewonly", NULL, "viewonly", T_TRUE, OFF(viewonly), "no recordings: detection and --live only (alarm/event state still shown)" },
    { "--min-free", NULL, "min_free", T_FLOAT, OFF(min_free), "MB of free disk space needed to start a recording; below it the recorder switches to view-only until restarted (100, 0: off)" },
    { "--force", NULL, "force", T_TRUE, OFF(force), "file input: overwrite an existing .mvmap" },
};
#define N_OPTS (int)(sizeof(opts) / sizeof(opts[0]))

#define FIELD(o, d, T) ((T *)((char *)(o) + (d)->offset))

void rc_usage(const char *prog)
{
    int i;
    fprintf(stderr, "usage: %s rtsp://[user:password@]host/... -n NAME [options]   record a camera\n"
                    "       %s FILE.mp4 [FILE.mp4 ...] [options]                   write FILE.mvmap for recordings\n\n",
            prog, prog);
    for (i = 0; i < N_OPTS; i++)
        fprintf(stderr, "  %s%s%-16s %s\n", opts[i].shortflag ? opts[i].shortflag : "",
                opts[i].shortflag ? ", " : "    ", opts[i].flag, opts[i].help);
}

static void set_defaults(rc_opts *o)
{
    memset(o, 0, sizeof(*o));
    o->out = strdup("recordings");
    o->timeout = 10;
    o->mv_min = 1.0;
    o->min_blocks = 4;
    o->min_cluster = 0;
    o->global_limit = 0.6;
    o->window = 15;
    o->trigger_frames = 4;
    o->max_ref_dist = 1;
    o->pre_roll = 5;
    o->post_roll = 8;
    o->max_segment = 600;
    o->max_tail = 5;
    o->fix = FIX_INLINE;
    o->map = 1;
    o->min_free = 100;
}

void rc_opts_free(rc_opts *o)
{
    int i;
    free(o->url);
    for (i = 0; i < o->n_inputs; i++)
        free(o->inputs[i]);
    free(o->inputs);
    free(o->name);
    free(o->config);
    free(o->out);
    free(o->live);
    for (i = 0; i < o->n_ignore; i++)
        free(o->ignore[i]);
    free(o->ignore);
    memset(o, 0, sizeof(*o));
}

static int append_ignore(rc_opts *o, const char *s)
{
    char **n = realloc(o->ignore, sizeof(char *) * (o->n_ignore + 1));
    if (!n)
        return -1;
    o->ignore = n;
    o->ignore[o->n_ignore++] = strdup(s);
    return 0;
}

static int parse_float(const char *s, double *v)
{
    char *end;
    errno = 0;
    *v = strtod(s, &end);
    while (*end == ' ')
        end++;
    return end == s || *end || errno ? -1 : 0;
}

static int parse_int(const char *s, int *v)
{
    char *end;
    long l;
    errno = 0;
    l = strtol(s, &end, 10);
    while (*end == ' ')
        end++;
    if (end == s || *end || errno || l < -2147483647L || l > 2147483647L)
        return -1;
    *v = (int)l;
    return 0;
}

/* apply a string value from the command line */
static int set_from_string(rc_opts *o, const opt_def *d, const char *val, char *err, int errlen)
{
    switch (d->type) {
    case T_STR:
        free(*FIELD(o, d, char *));
        *FIELD(o, d, char *) = strdup(val);
        return 0;
    case T_FLOAT:
        if (parse_float(val, FIELD(o, d, double)) < 0)
            break;
        return 0;
    case T_INT:
        if (parse_int(val, FIELD(o, d, int)) < 0)
            break;
        return 0;
    case T_APPEND:
        return append_ignore(o, val);
    case T_FIX:
        if (!strcmp(val, "inline"))
            o->fix = FIX_INLINE;
        else if (!strcmp(val, "remux"))
            o->fix = FIX_REMUX;
        else if (!strcmp(val, "off"))
            o->fix = FIX_OFF;
        else
            break;
        return 0;
    }
    snprintf(err, errlen, "argument %s: invalid value: '%s'", d->flag, val);
    return -1;
}

/* ---------------------------------------------------------------- minimal JSON reader */

typedef struct JP {
    const char *s;
    char *err;
    int errlen;
} JP;

static void ws(JP *p)
{
    while (*p->s == ' ' || *p->s == '\t' || *p->s == '\n' || *p->s == '\r')
        p->s++;
}

static int jfail(JP *p, const char *what)
{
    snprintf(p->err, p->errlen, "config: %s near '%.20s'", what, p->s);
    return -1;
}

/* parse a JSON string into a malloc'ed buffer (only \uXXXX below 0x80 is decoded) */
static char *jstring(JP *p)
{
    size_t cap = 32, n = 0;
    char *b;
    if (*p->s != '"')
        return NULL;
    p->s++;
    b = malloc(cap);
    while (b && *p->s && *p->s != '"') {
        char c = *p->s++;
        if (c == '\\') {
            c = *p->s++;
            switch (c) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                unsigned v = 0;
                if (sscanf(p->s, "%4x", &v) != 1 || v >= 0x80) {
                    free(b);
                    return NULL;
                }
                p->s += 4;
                c = (char)v;
                break;
            }
            case '"': case '\\': case '/': break;
            default:
                free(b);
                return NULL;
            }
        }
        if (n + 2 > cap) {
            char *nb = realloc(b, cap *= 2);
            if (!nb) {
                free(b);
                return NULL;
            }
            b = nb;
        }
        b[n++] = c;
    }
    if (!b || *p->s != '"') {
        free(b);
        return NULL;
    }
    p->s++;
    b[n] = 0;
    return b;
}

static const opt_def *find_dest(const char *key)
{
    int i;
    for (i = 0; i < N_OPTS; i++)
        if (!strcmp(opts[i].dest, key))
            return &opts[i];
    return NULL;
}

static int json_value(JP *p, rc_opts *o, const char *key)
{
    const opt_def *d = find_dest(key);
    ws(p);
    if (!strcmp(key, "url")) {           /* positional argument, allowed like in argparse */
        char *s = jstring(p);
        if (!s)
            return jfail(p, "url must be a string");
        free(o->url);
        o->url = s;
        return 0;
    }
    if (!d) {
        snprintf(p->err, p->errlen, "unknown key in config: %s", key);
        return -1;
    }
    if (*p->s == '"') {
        char *s = jstring(p);
        int r;
        if (!s)
            return jfail(p, "bad string");
        if (d->type == T_APPEND) {
            free(s);
            return jfail(p, "ignore must be a list");
        }
        r = set_from_string(o, d, s, p->err, p->errlen);
        free(s);
        return r;
    }
    if (*p->s == '[') {
        if (d->type != T_APPEND)
            return jfail(p, "unexpected list");
        while (o->n_ignore)
            free(o->ignore[--o->n_ignore]);
        p->s++;
        ws(p);
        if (*p->s == ']') {
            p->s++;
            return 0;
        }
        for (;;) {
            char *s;
            ws(p);
            s = jstring(p);
            if (!s)
                return jfail(p, "ignore zones must be strings");
            append_ignore(o, s);
            free(s);
            ws(p);
            if (*p->s == ',') {
                p->s++;
                continue;
            }
            if (*p->s == ']') {
                p->s++;
                return 0;
            }
            return jfail(p, "expected , or ]");
        }
    }
    if (!strncmp(p->s, "true", 4) || !strncmp(p->s, "false", 5)) {
        int v = p->s[0] == 't';
        p->s += v ? 4 : 5;
        if (d->type == T_TRUE || d->type == T_FALSE)
            *FIELD(o, d, int) = v;
        else if (d->type == T_INT)
            *FIELD(o, d, int) = v;
        else if (d->type == T_FLOAT)
            *FIELD(o, d, double) = v;
        else
            return jfail(p, "unexpected boolean");
        return 0;
    }
    if (!strncmp(p->s, "null", 4)) {
        p->s += 4;
        if (d->type == T_APPEND) {
            while (o->n_ignore)
                free(o->ignore[--o->n_ignore]);
            return 0;
        }
        if (d->type == T_STR) {
            free(*FIELD(o, d, char *));
            *FIELD(o, d, char *) = NULL;
            return 0;
        }
        return jfail(p, "unexpected null");
    }
    {
        char *end;
        double v = strtod(p->s, &end);
        if (end == p->s)
            return jfail(p, "bad value");
        p->s = end;
        if (d->type == T_FLOAT)
            *FIELD(o, d, double) = v;
        else if (d->type == T_INT || d->type == T_TRUE || d->type == T_FALSE)
            *FIELD(o, d, int) = (int)v;
        else
            return jfail(p, "unexpected number");
        return 0;
    }
}

static int load_config(rc_opts *o, const char *path, char *err, int errlen)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    JP p;
    int ret = -1;

    if (!f) {
        snprintf(err, errlen, "cannot open config %s", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(n + 1);
    if (!buf || fread(buf, 1, n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        snprintf(err, errlen, "cannot read config %s", path);
        return -1;
    }
    fclose(f);
    buf[n] = 0;

    p.s = buf;
    p.err = err;
    p.errlen = errlen;
    ws(&p);
    if (*p.s != '{') {
        jfail(&p, "expected an object");
        goto end;
    }
    p.s++;
    ws(&p);
    if (*p.s == '}') {
        ret = 0;
        goto end;
    }
    for (;;) {
        char *key;
        ws(&p);
        key = jstring(&p);
        if (!key) {
            jfail(&p, "expected a key");
            goto end;
        }
        ws(&p);
        if (*p.s != ':') {
            free(key);
            jfail(&p, "expected :");
            goto end;
        }
        p.s++;
        if (json_value(&p, o, key) < 0) {
            free(key);
            goto end;
        }
        free(key);
        ws(&p);
        if (*p.s == ',') {
            p.s++;
            continue;
        }
        if (*p.s == '}') {
            ret = 0;
            break;
        }
        jfail(&p, "expected , or }");
        goto end;
    }
end:
    free(buf);
    return ret;
}

/* ---------------------------------------------------------------- argv */

static const opt_def *find_flag(const char *arg, const char **inline_val)
{
    int i;
    *inline_val = NULL;
    for (i = 0; i < N_OPTS; i++) {
        size_t l = strlen(opts[i].flag);
        if (!strncmp(arg, opts[i].flag, l) && (arg[l] == 0 || arg[l] == '=')) {
            if (arg[l] == '=')
                *inline_val = arg + l + 1;
            return &opts[i];
        }
        if (opts[i].shortflag && !strncmp(arg, opts[i].shortflag, 2)) {
            if (arg[2])
                *inline_val = arg + 2;
            return &opts[i];
        }
    }
    return NULL;
}

int rc_parse_args(rc_opts *o, int argc, char **argv, char *err, int errlen)
{
    int i, pass;

    set_defaults(o);
    /* pass 0: only --config (its values become the defaults), pass 1: everything */
    for (pass = 0; pass < 2; pass++) {
        if (pass == 1 && o->config && load_config(o, o->config, err, errlen) < 0)
            return -1;
        for (i = 1; i < argc; i++) {
            const char *arg = argv[i], *val;
            const opt_def *d;
            if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
                if (pass == 1) {
                    rc_usage(argv[0]);
                    return 1;
                }
                continue;
            }
            if (arg[0] != '-' || !arg[1]) {
                if (pass == 1) {
                    char **n = realloc(o->inputs, sizeof(char *) * (o->n_inputs + 1));
                    if (!n)
                        return -1;
                    o->inputs = n;
                    o->inputs[o->n_inputs++] = strdup(arg);
                    if (o->n_inputs == 1) {
                        free(o->url);
                        o->url = strdup(arg);
                    }
                }
                continue;
            }
            d = find_flag(arg, &val);
            if (!d) {
                snprintf(err, errlen, "unrecognized argument: %s", arg);
                return -1;
            }
            if (d->type == T_TRUE || d->type == T_FALSE) {
                if (val) {
                    snprintf(err, errlen, "argument %s: ignored explicit argument", d->flag);
                    return -1;
                }
                if (pass == 1)
                    *FIELD(o, d, int) = d->type == T_TRUE;
                continue;
            }
            if (!val) {
                if (i + 1 >= argc) {
                    snprintf(err, errlen, "argument %s: expected one argument", d->flag);
                    return -1;
                }
                val = argv[++i];
            }
            if (pass == 0) {
                if (d->type == T_STR && !strcmp(d->dest, "config"))
                    if (set_from_string(o, d, val, err, errlen) < 0)
                        return -1;
                continue;
            }
            if (set_from_string(o, d, val, err, errlen) < 0)
                return -1;
        }
    }
    if (!o->url) {
        snprintf(err, errlen, "the following arguments are required: url");
        return -1;
    }
    if (!strncasecmp(o->url, "rtsp://", 7)) {
        if (o->n_inputs > 1) {
            snprintf(err, errlen, "one camera URL per process");
            return -1;
        }
        if (!o->name) {
            snprintf(err, errlen, "the following arguments are required: -n/--name");
            return -1;
        }
    } else if (!o->name) {
        o->name = strdup("file");              /* file analysis: only goes into the map header */
    }
    if (o->window < 1)
        o->window = 1;
    if (o->max_ref_dist < 0)
        o->max_ref_dist = 0;
    if (o->skip_after_key < 0)
        o->skip_after_key = 0;
    return 0;
}
