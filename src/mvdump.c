/*
 * mvdump - run mvparse over a raw H.264 Annex B file (one access unit at a time).
 *
 *   mvdump [-v] [-q] [-r] [-m N] file.h264
 *
 * Default output: one line per picture "frame <n> type <t> key <k> err <e> mvs <count>".
 * -v also prints every vector as "  w h dst_x dst_y mx my".
 * -q prints only the totals and the CPU time (for benchmarking).
 * -r adds the POC, the reference distance and the reference pictures after each picture.
 * -m N skips inter pictures whose references are all more than N frames back.
 *
 * Make an Annex B file from an MP4 with:
 *   ffmpeg -i in.mp4 -c copy -bsf:v h264_mp4toannexb -f h264 out.h264
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mvparse.h"

static int nal_start(const uint8_t *b, int size, int i)
{
    return i + 2 < size && b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1;
}

/* Does the NAL unit at b[i] (after the start code) begin a new access unit,
 * given whether the current one already holds a slice? */
static int starts_au(const uint8_t *nal, int len, int have_vcl)
{
    int type = nal[0] & 0x1F;
    if (type == 9)
        return 1;
    if (!have_vcl)
        return 0;
    if (type == 6 || type == 7 || type == 8)
        return 1;
    if ((type == 1 || type == 5) && len > 1 && (nal[1] & 0x80))   /* first_mb_in_slice == 0 */
        return 1;
    return 0;
}

typedef struct Stats { long frames, pframes, errors, unsupported, skipped, mvs; } Stats;
static int show_refs;

static void feed(mvp_ctx *ctx, const uint8_t *au, int len, int verbose, int quiet, Stats *st)
{
    mvp_frame fr;
    int i;
    mvp_decode(ctx, au, len, 0, &fr);
    if (fr.type == MVP_NONE)
        return;
    st->frames++;
    st->pframes += fr.type == MVP_P;
    st->unsupported += fr.type == MVP_UNSUPPORTED;
    st->skipped += fr.type == MVP_SKIPPED;
    st->errors += fr.errors != 0;
    st->mvs += fr.n_mv;
    if (quiet)
        return;
    printf("frame %ld type %d key %d err %d mvs %d", st->frames - 1, fr.type, fr.key, fr.errors, fr.n_mv);
    if (show_refs) {
        char refs[512];
        mvp_dump_refs(ctx, refs, sizeof(refs));
        printf(" poc %d dist %d refs_after %s", fr.poc, fr.ref_dist, refs);
    }
    printf("\n");
    if (verbose)
        for (i = 0; i < fr.n_mv; i++)
            printf("  %d %d %d %d %d %d\n", fr.mv[i].w, fr.mv[i].h,
                   fr.mv[i].dst_x, fr.mv[i].dst_y, fr.mv[i].mx, fr.mv[i].my);
}

int main(int argc, char **argv)
{
    int verbose = 0, quiet = 0, i, max_ref_dist = 0;
    const char *path = NULL;
    FILE *f;
    uint8_t *buf;
    long size;
    mvp_ctx *ctx;
    Stats st = { 0 };
    clock_t t0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v"))
            verbose = 1;
        else if (!strcmp(argv[i], "-q"))
            quiet = 1;
        else if (!strcmp(argv[i], "-r"))
            show_refs = 1;
        else if (!strcmp(argv[i], "-m") && i + 1 < argc)
            max_ref_dist = atoi(argv[++i]);
        else
            path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: %s [-v] [-q] [-r] [-m N] file.h264\n", argv[0]);
        return 2;
    }
    f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(size + 3);
    if (!buf || fread(buf, 1, size, f) != (size_t)size) {
        fprintf(stderr, "read error\n");
        return 1;
    }
    fclose(f);
    memset(buf + size, 0, 3);

    ctx = mvp_create();
    mvp_set_max_ref_dist(ctx, max_ref_dist);
    t0 = clock();

    /* split into access units (at AUD, parameter sets/SEI after a slice, or a slice
     * with first_mb_in_slice == 0) and feed them one by one */
    {
        int au_start = -1, have_vcl = 0, pos = 0, next;
        while (pos < size && !nal_start(buf, size, pos))
            pos++;
        while (pos < size) {
            const uint8_t *nal = buf + pos + 3;
            int t = nal[0] & 0x1F;
            next = pos + 3;
            while (next < size && !nal_start(buf, size, next))
                next++;
            if (au_start >= 0 && starts_au(nal, next - pos - 3, have_vcl)) {
                feed(ctx, buf + au_start, pos - au_start, verbose, quiet, &st);
                au_start = -1;
                have_vcl = 0;
            }
            if (au_start < 0)
                au_start = pos;
            if (t == 1 || t == 5)
                have_vcl = 1;
            pos = next;
        }
        if (au_start >= 0)
            feed(ctx, buf + au_start, size - au_start, verbose, quiet, &st);
    }

    fprintf(stderr, "%ld pictures (%ld P, %ld skipped, %ld unsupported, %ld with errors), %ld vectors, %.3f s CPU\n",
            st.frames, st.pframes, st.skipped, st.unsupported, st.errors, st.mvs, (double)(clock() - t0) / CLOCKS_PER_SEC);
    mvp_free(ctx);
    free(buf);
    return 0;
}
