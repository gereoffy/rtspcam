/* .mvmap writer, a port of mvmap.MapWriter: a JSON header followed by one zlib stream
 * that is sync-flushed after every frame record, so a truncated file stays readable. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "log.h"
#include "mvmap_writer.h"

static const char MAGIC[8] = { 'R', 'C', 'M', 'V', 'M', 'A', 'P', '1' };
static const char MAGIC_VEC[8] = { 'R', 'C', 'M', 'V', 'V', 'E', 'C', '2' };

struct mvmap_writer {
    char *path;
    FILE *f;
    z_stream z;
    uint8_t *rec;          /* uncompressed record */
    size_t rec_cap;
    uint8_t out[16384];
    int failed;            /* a write failed: the zlib stream is broken, the file is dropped */
};

void json_put_string(char *buf, size_t size, const char *s)
{
    size_t n = 0;
    if (size < 3)
        return;
    buf[n++] = '"';
    for (; *s && n + 8 < size; s++) {
        unsigned char c = *s;
        if (c == '"' || c == '\\') {
            buf[n++] = '\\';
            buf[n++] = c;
        } else if (c < 0x20 || c >= 0x7F) {
            n += snprintf(buf + n, size - n, "\\u%04x", c);   /* non-ASCII bytes are escaped one by one */
        } else {
            buf[n++] = c;
        }
    }
    buf[n++] = '"';
    buf[n] = 0;
}

/* shortest representation that reads back exactly, with ".0" for integral values (like Python's repr) */
void json_put_double(char *buf, size_t size, double v)
{
    int prec;
    for (prec = 1; prec <= 17; prec++) {
        snprintf(buf, size, "%.*g", prec, v);
        if (strtod(buf, NULL) == v)
            break;
    }
    if (!strpbrk(buf, ".eEn"))
        strncat(buf, ".0", size - strlen(buf) - 1);
}

static int write_out(mvmap_writer *m, int flush)
{
    if (m->failed)
        return -1;
    do {
        m->z.next_out = m->out;
        m->z.avail_out = sizeof(m->out);
        if (deflate(&m->z, flush) == Z_STREAM_ERROR ||
            fwrite(m->out, 1, sizeof(m->out) - m->z.avail_out, m->f) != sizeof(m->out) - m->z.avail_out) {
            m->failed = 1;
            return -1;
        }
    } while (m->z.avail_out == 0);
    return 0;
}

static mvmap_writer *open_stream(const char *path, const char *magic, const char *header_json, const char *extra);

mvmap_writer *mvmap_open(const char *path, const char *header_json)
{
    /* dict(header, version=1, cell=16, time_unit="ms") */
    return open_stream(path, MAGIC, header_json, ", \"version\": 1, \"cell\": 16, \"time_unit\": \"ms\"}");
}

mvmap_writer *mvvec_open(const char *path, const char *header_json)
{
    return open_stream(path, MAGIC_VEC, header_json,
                       ", \"version\": 2, \"cell\": 16, \"time_unit\": \"ms\", "
                       "\"encoding\": \"1 byte per cell, the longest vector of the cell: low 4 bits = its length in "
                       "pixels (0-15, floored, longer ones are 15), high 4 bits = its direction in 22.5 degree steps, "
                       "0 = +x (right), 4 = +y (down), 8 = left, 12 = up; 0 = no vector of 1 pixel or more\"}");
}

static mvmap_writer *open_stream(const char *path, const char *magic, const char *header_json, const char *extra)
{
    mvmap_writer *m = calloc(1, sizeof(*m));
    size_t hl = strlen(header_json), plen = strlen(path);
    char *hdr, *part;
    uint8_t len[4];

    if (!m)
        return NULL;
    m->path = strdup(path);
    part = malloc(plen + 6);
    hdr = malloc(hl + strlen(extra) + 1);
    if (!m->path || !part || !hdr || hl < 2 || header_json[hl - 1] != '}')
        goto fail;
    sprintf(part, "%s.part", path);
    memcpy(hdr, header_json, hl - 1);
    strcpy(hdr + hl - 1, extra);
    hl = strlen(hdr);

    m->f = fopen(part, "wb");
    if (!m->f) {
        log_error("cannot create %s", part);
        goto fail;
    }
    len[0] = hl & 0xFF;
    len[1] = (hl >> 8) & 0xFF;
    len[2] = (hl >> 16) & 0xFF;
    len[3] = (hl >> 24) & 0xFF;
    if (fwrite(magic, 1, 8, m->f) != 8 || fwrite(len, 1, 4, m->f) != 4 || fwrite(hdr, 1, hl, m->f) != hl ||
        fflush(m->f))                           /* a full disk shows up here, not at the close */
        goto fail;
    if (deflateInit(&m->z, 6) != Z_OK)
        goto fail;
    free(part);
    free(hdr);
    return m;
fail:
    if (m->f) {
        fclose(m->f);
        remove(part);
    }
    free(part);
    free(hdr);
    free(m->path);
    free(m);
    return NULL;
}

int mvmap_write(mvmap_writer *m, long t_ms, const mt_record *r)
{
    size_t need = 11 + 3 * (size_t)r->n_cells, p = 0;
    unsigned t = t_ms < 0 ? 0 : (unsigned)t_ms;
    unsigned cl = r->cluster > 65535 ? 65535 : r->cluster;
    double bl = r->blocks;
    unsigned b = bl >= 65535 ? 65535 : (unsigned)bl;
    unsigned n = r->n_cells;
    int i;

    if (need > m->rec_cap) {
        uint8_t *nb = realloc(m->rec, need);
        if (!nb)
            return -1;
        m->rec = nb;
        m->rec_cap = need;
    }
    /* struct "<IHHBH" */
    m->rec[p++] = t; m->rec[p++] = t >> 8; m->rec[p++] = t >> 16; m->rec[p++] = t >> 24;
    m->rec[p++] = cl; m->rec[p++] = cl >> 8;
    m->rec[p++] = b; m->rec[p++] = b >> 8;
    m->rec[p++] = r->flags;
    m->rec[p++] = n; m->rec[p++] = n >> 8;
    for (i = 0; i < r->n_cells; i++) {
        m->rec[p++] = r->cells[i].cell;
        m->rec[p++] = r->cells[i].cell >> 8;
        m->rec[p++] = r->cells[i].level;
    }
    m->z.next_in = m->rec;
    m->z.avail_in = p;
    return write_out(m, Z_SYNC_FLUSH);
}

int mvvec_write(mvmap_writer *m, long t_ms, uint32_t frame_bytes, uint32_t vcl_bytes, const uint8_t *grid, int n)
{
    size_t need = 13 + (grid ? (size_t)n : 0), p = 0;
    unsigned t = t_ms < 0 ? 0 : (unsigned)t_ms;
    if (need > m->rec_cap) {
        uint8_t *nb = realloc(m->rec, need);
        if (!nb)
            return -1;
        m->rec = nb;
        m->rec_cap = need;
    }
    /* struct "<IBII" + grid */
    m->rec[p++] = t; m->rec[p++] = t >> 8; m->rec[p++] = t >> 16; m->rec[p++] = t >> 24;
    m->rec[p++] = grid ? 1 : 0;
    m->rec[p++] = frame_bytes; m->rec[p++] = frame_bytes >> 8; m->rec[p++] = frame_bytes >> 16; m->rec[p++] = frame_bytes >> 24;
    m->rec[p++] = vcl_bytes; m->rec[p++] = vcl_bytes >> 8; m->rec[p++] = vcl_bytes >> 16; m->rec[p++] = vcl_bytes >> 24;
    if (grid) {
        memcpy(m->rec + p, grid, n);
        p += n;
    }
    m->z.next_in = m->rec;
    m->z.avail_in = p;
    return write_out(m, Z_SYNC_FLUSH);
}

/* direction sector without trigonometry: in each quadrant the 16-sector boundaries lie at
 * 11.25, 33.75, 56.25 and 78.75 degrees, i.e. |y| = tan(boundary) * |x| */
static int sector(double x, double y)
{
    double ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    int k = (ay > 0.19891237 * ax) + (ay > 0.66817864 * ax) + (ay > 1.49660576 * ax) + (ay > 5.02733949 * ax);
    if (x >= 0 && y >= 0)
        return k;
    if (x < 0 && y >= 0)
        return 8 - k;
    if (x < 0)
        return 8 + k;
    return (16 - k) & 15;
}

void mvvec_grid(const mvp_mv *mv, int n_mv, int gw, int gh, double *best, uint8_t *out)
{
    int i, n = gw * gh;
    for (i = 0; i < n; i++) {
        best[i] = -1;
        out[i] = 0;
    }
    /* the longest vector of each cell, measured exactly like the detector does (hypot / 4):
     * a cell is "moving" for --mv-min M iff its stored length (floored) is >= M, for integer M */
    for (i = 0; i < n_mv; i++) {
        int gx = mv[i].dst_x / 16, gy = mv[i].dst_y / 16, c, l;
        double mag;
        if (gx >= gw || gy >= gh)
            continue;
        c = gy * gw + gx;
        mag = hypot(mv[i].mx, mv[i].my) / 4.0;
        if (mag <= best[c])
            continue;
        best[c] = mag;
        l = mag >= 15 ? 15 : (int)mag;
        out[c] = l ? (uint8_t)(sector(mv[i].mx, mv[i].my) << 4 | l) : 0;
    }
}

int mvmap_close(mvmap_writer *m)
{
    int ret = 0;
    char *part;
    if (!m)
        return 0;
    m->z.next_in = NULL;
    m->z.avail_in = 0;
    if (write_out(m, Z_FINISH) < 0)
        ret = -1;
    deflateEnd(&m->z);
    if (fclose(m->f)) {
        m->failed = 1;
        ret = -1;
    }
    part = malloc(strlen(m->path) + 6);
    if (part) {
        sprintf(part, "%s.part", m->path);
        if (m->failed)
            remove(part);                   /* a broken zlib stream is useless */
        else if (rename(part, m->path))
            ret = -1;
        free(part);
    }
    free(m->rec);
    free(m->path);
    free(m);
    return ret;
}
