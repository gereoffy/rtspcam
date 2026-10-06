/* Command line and --config JSON handling, compatible with rtspcam.py. */
#ifndef RC_OPTIONS_H
#define RC_OPTIONS_H

enum { FIX_OFF = 0, FIX_INLINE = 1, FIX_REMUX = 2 };

typedef struct rc_opts {
    char *url;              /* first positional argument */
    char **inputs;          /* all positional arguments (several files for map generation) */
    int n_inputs;
    int force;              /* file mode: overwrite an existing .mvmap */
    int viewonly;           /* analyse and serve --live, but write no files */
    double min_free;        /* MB that must be free on the output disk to start a recording (0: no check) */
    char *name;
    char *config;
    char *out;
    double timeout;
    int always;
    /* motion heuristics */
    double mv_min;
    double min_blocks;
    int min_cluster;
    double global_limit;
    int window;
    int trigger_frames;
    int max_ref_dist;
    int skip_after_key;     /* leave out the first N pictures after a key frame (low-bitrate cameras) */
    char **ignore;          /* "x0,y0,x1,y1" strings, as given */
    int n_ignore;
    double pre_roll;
    double post_roll;
    double max_segment;
    double max_tail;
    int fix;                /* FIX_* */
    int map;
    int vectors;            /* also write the .mvvec motion field sidecar */
    char *live;             /* directory of the live stream socket <live>/<name>.sock, or NULL */
    int cheap;              /* accepted for compatibility, nothing to skip here */
    int debug;
} rc_opts;

/* Parse argv (argv[0] is the program name). Returns 0, 1 if --help was printed,
 * or -1 with a message in err. */
int rc_parse_args(rc_opts *o, int argc, char **argv, char *err, int errlen);
void rc_opts_free(rc_opts *o);
void rc_usage(const char *prog);

#endif
