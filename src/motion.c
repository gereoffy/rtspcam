/* Motion detector, a port of MotionDetector in rtspcam.py. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "motion.h"

static int parse_zones(mt_det *d, const rc_opts *a)
{
    int i;
    d->zones = calloc(a->n_ignore ? a->n_ignore : 1, sizeof(*d->zones));
    if (!d->zones)
        return -1;
    for (i = 0; i < a->n_ignore; i++) {
        double z[4];
        if (sscanf(a->ignore[i], "%lf,%lf,%lf,%lf", &z[0], &z[1], &z[2], &z[3]) != 4) {
            log_error("bad --ignore zone '%s' (expected x0,y0,x1,y1)", a->ignore[i]);
            return -1;
        }
        d->zones[i][0] = z[0] * d->w;
        d->zones[i][1] = z[1] * d->h;
        d->zones[i][2] = z[2] * d->w;
        d->zones[i][3] = z[3] * d->h;
    }
    d->n_zones = a->n_ignore;
    return 0;
}

int mt_init(mt_det *d, const rc_opts *a, int width, int height)
{
    int n;
    memset(d, 0, sizeof(*d));
    d->a = a;
    d->w = width;
    d->h = height;
    d->total_mb = (width / 16.0) * (height / 16.0);
    d->gw = (width + 15) / 16;
    d->gh = (height + 15) / 16;
    n = d->gw * d->gh;
    d->hist = calloc(a->window > 0 ? a->window : 1, 1);
    d->label = malloc(sizeof(*d->label) * n);
    d->cells = malloc(sizeof(*d->cells) * n);
    d->stack = malloc(sizeof(*d->stack) * n);
    d->group_size = malloc(sizeof(*d->group_size) * n);
    d->rec_cells = malloc(sizeof(*d->rec_cells) * n);
    if (!d->hist || !d->label || !d->cells || !d->stack || !d->group_size || !d->rec_cells)
        return -1;
    memset(d->label, 0xFF, sizeof(*d->label) * n);
    d->best = -1;
    return parse_zones(d, a);
}

int mt_reload_zones(mt_det *d)
{
    double (*old)[4] = d->zones;
    int n_old = d->n_zones;
    if (parse_zones(d, d->a) < 0) {
        free(d->zones);
        d->zones = old;
        d->n_zones = n_old;
        return -1;
    }
    free(old);
    return 0;
}

void mt_free(mt_det *d)
{
    free(d->zones);
    free(d->hist);
    free(d->label);
    free(d->cells);
    free(d->stack);
    free(d->group_size);
    free(d->rec_cells);
    memset(d, 0, sizeof(*d));
}

/* 8-connected groups of the moving cells; best = the first largest group */
static void cluster(mt_det *d)
{
    int i, gw = d->gw, gh = d->gh;
    d->n_groups = 0;
    d->best = -1;
    /* label: -2 = moving, not yet assigned */
    for (i = 0; i < d->n_cells; i++) {
        int start = d->cells[i], sp = 0, g, size = 0;
        if (d->label[start] != -2)
            continue;
        g = d->n_groups++;
        d->label[start] = g;
        d->stack[sp++] = start;
        while (sp) {
            int c = d->stack[--sp], y = c / gw, x = c % gw, dy, dx;
            size++;
            for (dy = -1; dy <= 1; dy++) {
                int yy = y + dy;
                if (yy < 0 || yy >= gh)
                    continue;
                for (dx = -1; dx <= 1; dx++) {
                    int xx = x + dx, q;
                    if (xx < 0 || xx >= gw)
                        continue;
                    q = yy * gw + xx;
                    if (d->label[q] == -2) {
                        d->label[q] = g;
                        d->stack[sp++] = q;
                    }
                }
            }
        }
        d->group_size[g] = size;
        if (d->best < 0 || size > d->group_size[d->best])
            d->best = g;
    }
    d->cluster = d->best >= 0 ? d->group_size[d->best] : 0;
}

static void score(mt_det *d, const mvp_frame *f)
{
    const rc_opts *a = d->a;
    long area = 0;
    int i, k;

    /* forget the cells of the previous frame */
    for (i = 0; i < d->n_cells; i++)
        d->label[d->cells[i]] = -1;
    d->n_cells = 0;
    d->n_groups = 0;
    d->best = -1;
    d->cluster = 0;

    for (i = 0; i < f->n_mv; i++) {
        const mvp_mv *v = &f->mv[i];
        int gx, gy, c;
        if (hypot(v->mx, v->my) / 4.0 < a->mv_min)
            continue;
        for (k = 0; k < d->n_zones; k++)
            if (v->dst_x >= d->zones[k][0] && v->dst_x < d->zones[k][2] &&
                v->dst_y >= d->zones[k][1] && v->dst_y < d->zones[k][3])
                break;
        if (k < d->n_zones)
            continue;
        area += v->w * v->h;
        gx = v->dst_x / 16;
        gy = v->dst_y / 16;
        if (gx >= d->gw || gy >= d->gh)
            continue;
        c = gy * d->gw + gx;
        if (d->label[c] == -1) {
            d->label[c] = -2;
            d->cells[d->n_cells++] = c;
        }
    }
    d->blocks = area / 256.0;
    d->frac = d->blocks / d->total_mb;
    if (a->min_cluster > 0 || a->debug || a->map)
        cluster(d);

    if (a->debug) {
        static const double thr[] = { 0.5, 1, 2, 3, 5, 8 };
        double by[6] = { 0 };
        int any = 0, x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1, t;
        double act = a->mv_min > 1.0 ? a->mv_min : 1.0;
        for (i = 0; i < f->n_mv; i++) {
            const mvp_mv *v = &f->mv[i];
            double mag = hypot(v->mx, v->my) / 4.0;
            for (t = 0; t < 6; t++)
                if (mag >= thr[t])
                    by[t] += v->w * v->h / 256.0;
            if (mag >= act) {
                any = 1;
                if (v->dst_x < x0) x0 = v->dst_x;
                if (v->dst_x > x1) x1 = v->dst_x;
                if (v->dst_y < y0) y0 = v->dst_y;
                if (v->dst_y > y1) y1 = v->dst_y;
            }
        }
        if (any)
            log_debug("blocks>=px  0.5:%.0f 1:%.0f 2:%.0f 3:%.0f 5:%.0f 8:%.0f bbox=%.2f,%.2f-%.2f,%.2f",
                      by[0], by[1], by[2], by[3], by[4], by[5],
                      (double)x0 / d->w, (double)y0 / d->h, (double)x1 / d->w, (double)y1 / d->h);
        else
            log_debug("blocks>=px  0.5:%.0f 1:%.0f 2:%.0f 3:%.0f 5:%.0f 8:%.0f",
                      by[0], by[1], by[2], by[3], by[4], by[5]);
    }
}

int mt_update(mt_det *d, const mvp_frame *f)
{
    const rc_opts *a = d->a;
    int moving;

    if (f->width && (f->width != d->w || f->height != d->h)) {
        mt_free(d);
        if (mt_init(d, a, f->width, f->height) < 0)
            return -1;
    }
    if (f->key) {
        d->last_valid = 0;
        return -1;
    }
    score(d, f);
    d->last_valid = 1;

    if (a->min_cluster > 0)
        moving = d->cluster >= a->min_cluster && d->frac <= a->global_limit;
    else
        moving = d->blocks >= a->min_blocks && d->frac <= a->global_limit;

    /* deque(maxlen=window).append() */
    if (d->hist_n == a->window) {
        d->hist_sum -= d->hist[d->hist_pos];
    } else {
        d->hist_n++;
    }
    d->hist[d->hist_pos] = moving;
    d->hist_sum += moving;
    d->hist_pos = (d->hist_pos + 1) % a->window;

    if (a->debug)
        log_debug("blocks=%.1f cluster=%d frac=%.2f moving=%s", d->blocks, d->cluster, d->frac,
                  moving ? "True" : "False");
    return moving;
}

int mt_triggered(const mt_det *d)
{
    return d->hist_sum >= d->a->trigger_frames;
}

void mt_record_get(mt_det *d, int moving, int trig, mt_record *r)
{
    const rc_opts *a = d->a;
    int i;

    r->flags = trig ? F_TRIG : 0;
    if (!d->last_valid) {
        r->cluster = 0;
        r->blocks = 0;
        r->flags |= F_KEY | F_SKIP;      /* no vectors in a key frame: not analysed either */
        r->n_cells = 0;
        r->cells = d->rec_cells;
        return;
    }
    if (moving > 0)
        r->flags |= F_MOVING;
    r->cluster = d->cluster;
    r->blocks = d->blocks;
    /* level 3: every group that reaches --min-cluster (block mode: the largest group once
     * --min-blocks is reached); 2: the largest group below the threshold; 1: the rest */
    for (i = 0; i < d->n_cells; i++) {
        int g = d->label[d->cells[i]], lvl;
        if (a->min_cluster > 0)
            lvl = d->group_size[g] >= a->min_cluster ? 3 : (g == d->best ? 2 : 1);
        else
            lvl = g == d->best ? (d->blocks >= a->min_blocks ? 3 : 2) : 1;
        d->rec_cells[i].cell = d->cells[i];
        d->rec_cells[i].level = lvl;
    }
    r->n_cells = d->n_cells;
    r->cells = d->rec_cells;
}
