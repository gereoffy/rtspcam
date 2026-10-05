/* Minimal RTSP/1.0 client (RFC 2326): TCP only, RTP interleaved on the control
 * connection, first H.264 video stream of the SDP, no authentication. */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "md5.h"
#include "rtsp.h"

#define RBUF_SIZE (1 << 20)
#define USER_AGENT "rtspcam-c"

enum { AUTH_NONE, AUTH_BASIC, AUTH_DIGEST };

struct rtsp_client {
    int fd;
    char *user, *pass;      /* from the URL, NULL without credentials */
    int auth;               /* AUTH_*, chosen from the camera's challenge */
    char *realm, *nonce, *opaque;
    int qop_auth, md5_sess;
    unsigned nc;
    char cnonce[17];
    volatile int *stop;     /* checked while waiting for data */
    double timeout;
    char *url;              /* without user info */
    char *base;             /* base URL for relative controls */
    char *session;
    int cseq;
    int session_timeout;
    int use_get_parameter;
    double last_keepalive;

    char *video_control;
    char *session_control;
    int payload_type;
    int clock_rate;
    int rtp_channel;
    uint8_t *sprop;
    int sprop_len;

    uint8_t *rbuf;
    size_t rpos, rlen;      /* unread data is rbuf[rpos .. rlen) */
    long packets[4];        /* interleaved packets per channel (mod 4), for the debug log */
    long resyncs, resync_bytes;     /* times the interleaved framing was lost, bytes skipped */
    int in_resync;
};

typedef struct response {
    int status;
    char *headers;          /* raw header block, NUL terminated */
    char *body;
    int body_len;
} response;

static double mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---------------------------------------------------------------- socket I/O */

static int wait_fd(int fd, int events, double seconds)
{
    struct pollfd p;
    int r;
    p.fd = fd;
    p.events = events;
    do {
        r = poll(&p, 1, (int)(seconds * 1000));
    } while (r < 0 && errno == EINTR);
    return r;
}

static int tcp_connect(const char *host, int port, double timeout)
{
    struct addrinfo hints, *res, *ai;
    char ports[16];
    int fd = -1, r;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(ports, sizeof(ports), "%d", port);
    if ((r = getaddrinfo(host, ports, &hints, &res)) != 0) {
        log_warn("cannot resolve %s: %s", host, gai_strerror(r));
        return -1;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        int flags, err = 0;
        socklen_t el = sizeof(err);
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        flags = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0 && errno != EINPROGRESS) {
            close(fd);
            fd = -1;
            continue;
        }
        if (wait_fd(fd, POLLOUT, timeout) <= 0 ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err) {
            close(fd);
            fd = -1;
            continue;
        }
        fcntl(fd, F_SETFL, flags);
        break;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        int one = 1, size = 2 << 20;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        (void)one;
    } else {
        log_warn("cannot connect to %s:%d", host, port);
    }
    return fd;
}

static int send_all(rtsp_client *c, const char *buf, size_t len)
{
    while (len) {
        ssize_t n;
        if (wait_fd(c->fd, POLLOUT, c->timeout) <= 0)
            return -1;
#ifdef MSG_NOSIGNAL
        n = send(c->fd, buf, len, MSG_NOSIGNAL);
#else
        n = send(c->fd, buf, len, 0);
#endif
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        buf += n;
        len -= n;
    }
    return 0;
}

/* Make at least `need` unread bytes available. Returns 0, 1 if stopped, -1 on error/timeout/EOF. */
static int fill(rtsp_client *c, size_t need, volatile int *stop)
{
    double deadline = mono() + c->timeout;
    if (need > RBUF_SIZE)
        return -1;
    while (c->rlen - c->rpos < need) {
        ssize_t n;
        double left;
        if (c->rpos > 0 && (c->rpos > RBUF_SIZE / 2 || c->rlen + need > RBUF_SIZE)) {
            memmove(c->rbuf, c->rbuf + c->rpos, c->rlen - c->rpos);
            c->rlen -= c->rpos;
            c->rpos = 0;
        }
        if (stop && *stop)
            return 1;
        left = deadline - mono();
        if (left <= 0) {
            log_warn("no data for %.0f s", c->timeout);
            return -1;
        }
        if (wait_fd(c->fd, POLLIN, left < 0.25 ? left : 0.25) == 0)
            continue;
        n = recv(c->fd, c->rbuf + c->rlen, RBUF_SIZE - c->rlen, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n == 0) {
            log_warn("connection closed by the camera");
            return -1;
        }
        if (n < 0) {
            log_warn("receive error: %s", strerror(errno));
            return -1;
        }
        c->rlen += n;
    }
    return 0;
}

/* ---------------------------------------------------------------- RTSP messages */

static const char *header(const response *r, const char *name, char *out, size_t outlen)
{
    const char *p = r->headers;
    size_t nl = strlen(name);
    while (p && *p) {
        const char *eol = strstr(p, "\r\n");
        size_t ll = eol ? (size_t)(eol - p) : strlen(p);
        if (ll > nl && !strncasecmp(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1;
            size_t vl;
            while (*v == ' ' || *v == '\t')
                v++;
            vl = ll - (v - p);
            if (vl >= outlen)
                vl = outlen - 1;
            memcpy(out, v, vl);
            out[vl] = 0;
            return out;
        }
        p = eol ? eol + 2 : NULL;
    }
    return NULL;
}

static void free_response(response *r)
{
    free(r->headers);
    free(r->body);
    memset(r, 0, sizeof(*r));
}

/* Reads one RTSP response; interleaved data in front of it is skipped. */
static int read_response(rtsp_client *c, response *r, volatile int *stop)
{
    size_t hlen;
    char *end, cl[32];

    memset(r, 0, sizeof(*r));
    for (;;) {
        if (fill(c, 4, stop))
            return -1;
        if (c->rbuf[c->rpos] == '$') {          /* interleaved packet: skip */
            size_t n = (c->rbuf[c->rpos + 2] << 8) | c->rbuf[c->rpos + 3];
            if (fill(c, 4 + n, stop))
                return -1;
            c->rpos += 4 + n;
            continue;
        }
        if (!memcmp(c->rbuf + c->rpos, "RTSP", 4))
            break;
        c->rpos++;                              /* resync */
    }
    for (;;) {
        size_t avail = c->rlen - c->rpos;
        c->rbuf[c->rlen] = 0;
        end = strstr((char *)c->rbuf + c->rpos, "\r\n\r\n");
        if (end)
            break;
        if (avail > 65536 || fill(c, avail + 1, stop))
            return -1;
    }
    hlen = end + 4 - (char *)(c->rbuf + c->rpos);
    r->headers = malloc(hlen + 1);
    if (!r->headers)
        return -1;
    memcpy(r->headers, c->rbuf + c->rpos, hlen);
    r->headers[hlen] = 0;
    c->rpos += hlen;
    if (sscanf(r->headers, "RTSP/%*d.%*d %d", &r->status) != 1) {
        free_response(r);
        return -1;
    }
    if (header(r, "Content-Length", cl, sizeof(cl))) {
        int n = atoi(cl);
        if (n < 0 || n > 1 << 20 || fill(c, n, stop)) {
            free_response(r);
            return -1;
        }
        r->body = malloc(n + 1);
        if (!r->body) {
            free_response(r);
            return -1;
        }
        memcpy(r->body, c->rbuf + c->rpos, n);
        r->body[n] = 0;
        r->body_len = n;
        c->rpos += n;
    }
    return 0;
}

/* ---------------------------------------------------------------- authentication (RFC 2617 / 7616, MD5) */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64enc(const char *in, char *out, size_t outlen)
{
    size_t n = strlen(in), i, o = 0;
    for (i = 0; i < n && o + 5 < outlen; i += 3) {
        unsigned v = (unsigned char)in[i] << 16;
        if (i + 1 < n) v |= (unsigned char)in[i + 1] << 8;
        if (i + 2 < n) v |= (unsigned char)in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = i + 1 < n ? B64[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? B64[v & 63] : '=';
    }
    out[o] = 0;
}

/* value of key in a "key=value, key=\"value\"" list, or NULL (malloc'ed) */
static char *auth_param(const char *p, const char *key)
{
    size_t kl = strlen(key);
    while (p && *p) {
        while (*p == ' ' || *p == ',' || *p == '\t')
            p++;
        if (!strncasecmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1, *e;
            char *r;
            if (*v == '"') {
                v++;
                e = strchr(v, '"');
            } else {
                e = v + strcspn(v, ", \t");
            }
            if (!e)
                e = v + strlen(v);
            r = malloc(e - v + 1);
            if (r) {
                memcpy(r, v, e - v);
                r[e - v] = 0;
            }
            return r;
        }
        /* skip this parameter */
        p += strcspn(p, "=,");
        if (*p == '=') {
            p++;
            if (*p == '"') {
                p = strchr(p + 1, '"');
                if (p)
                    p++;
            } else {
                p += strcspn(p, ",");
            }
        }
    }
    return NULL;
}

/* is tok one of the comma separated words of list? ("auth" in "auth,auth-int") */
static int has_token(const char *list, const char *tok)
{
    size_t tl = strlen(tok);
    while (*list) {
        size_t n;
        while (*list == ' ' || *list == ',')
            list++;
        n = strcspn(list, ", ");
        if (n == tl && !strncasecmp(list, tok, tl))
            return 1;
        list += n;
    }
    return 0;
}

/* Picks Digest (preferred) or Basic from the WWW-Authenticate headers. Returns 0 if usable. */
static int parse_challenge(rtsp_client *c, const response *r)
{
    const char *p = r->headers, *digest = NULL, *basic = NULL;
    char *qop, *alg, *stale;
    while (p && *p) {
        const char *eol = strstr(p, "\r\n");
        if (!strncasecmp(p, "WWW-Authenticate:", 17)) {
            const char *v = p + 17;
            while (*v == ' ')
                v++;
            if (!strncasecmp(v, "Digest ", 7) && !digest)
                digest = v + 7;
            else if (!strncasecmp(v, "Basic", 5) && !basic)
                basic = v + 5;
        }
        p = eol ? eol + 2 : NULL;
    }
    if (!digest && !basic) {
        log_warn("the camera wants authentication of an unknown kind");
        return -1;
    }
    free(c->realm);
    free(c->nonce);
    free(c->opaque);
    c->realm = c->nonce = c->opaque = NULL;
    if (!digest) {
        c->auth = AUTH_BASIC;
        return 0;
    }
    /* the header block is one string: cut the parameters at the end of the line */
    {
        const char *eol = strstr(digest, "\r\n");
        size_t n = eol ? (size_t)(eol - digest) : strlen(digest);
        char *line = malloc(n + 1);
        if (!line)
            return -1;
        memcpy(line, digest, n);
        line[n] = 0;
        c->realm = auth_param(line, "realm");
        c->nonce = auth_param(line, "nonce");
        c->opaque = auth_param(line, "opaque");
        qop = auth_param(line, "qop");
        alg = auth_param(line, "algorithm");
        stale = auth_param(line, "stale");
        free(line);
    }
    c->qop_auth = qop && has_token(qop, "auth");
    c->md5_sess = alg && !strcasecmp(alg, "MD5-sess");
    if (alg && strcasecmp(alg, "MD5") && strcasecmp(alg, "MD5-sess")) {
        log_warn("digest algorithm %s is not supported (MD5 only)%s", alg, basic ? ", trying Basic" : "");
        free(qop);
        free(alg);
        free(stale);
        if (!basic)
            return -1;
        c->auth = AUTH_BASIC;
        return 0;
    }
    free(qop);
    free(alg);
    free(stale);
    if (!c->realm || !c->nonce)
        return -1;
    c->auth = AUTH_DIGEST;
    c->nc = 0;
    snprintf(c->cnonce, sizeof(c->cnonce), "%08x%08x", (unsigned)rand(), (unsigned)time(NULL));
    return 0;
}

/* "Authorization: ...\r\n" for this request, or "" */
static void auth_header(rtsp_client *c, const char *method, const char *url, char *out, size_t outlen)
{
    out[0] = 0;
    if (!c->user || c->auth == AUTH_NONE)
        return;
    if (c->auth == AUTH_BASIC) {
        char up[512], enc[700];
        snprintf(up, sizeof(up), "%s:%s", c->user, c->pass ? c->pass : "");
        b64enc(up, enc, sizeof(enc));
        snprintf(out, outlen, "Authorization: Basic %s\r\n", enc);
        return;
    }
    {
        char a1[1024], a2[1024], ha1[33], ha2[33], resp_in[1024], resp[33], nc[9];
        int n;
        snprintf(a1, sizeof(a1), "%s:%s:%s", c->user, c->realm, c->pass ? c->pass : "");
        md5_hex(a1, ha1);
        if (c->md5_sess) {
            snprintf(a1, sizeof(a1), "%s:%s:%s", ha1, c->nonce, c->cnonce);
            md5_hex(a1, ha1);
        }
        snprintf(a2, sizeof(a2), "%s:%s", method, url);
        md5_hex(a2, ha2);
        snprintf(nc, sizeof(nc), "%08x", ++c->nc);
        if (c->qop_auth)
            snprintf(resp_in, sizeof(resp_in), "%s:%s:%s:%s:auth:%s", ha1, c->nonce, nc, c->cnonce, ha2);
        else
            snprintf(resp_in, sizeof(resp_in), "%s:%s:%s", ha1, c->nonce, ha2);
        md5_hex(resp_in, resp);
        n = snprintf(out, outlen, "Authorization: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\"",
                     c->user, c->realm, c->nonce, url, resp);
        if (c->opaque && n < (int)outlen)
            n += snprintf(out + n, outlen - n, ", opaque=\"%s\"", c->opaque);
        if (c->md5_sess && n < (int)outlen)
            n += snprintf(out + n, outlen - n, ", algorithm=MD5-sess");
        if (c->qop_auth && n < (int)outlen)
            n += snprintf(out + n, outlen - n, ", qop=auth, nc=%s, cnonce=\"%s\"", nc, c->cnonce);
        if (n < (int)outlen)
            snprintf(out + n, outlen - n, "\r\n");
    }
}

static int send_request(rtsp_client *c, const char *method, const char *url, const char *extra)
{
    char buf[4096], auth[1500];
    int n;
    auth_header(c, method, url, auth, sizeof(auth));
    n = snprintf(buf, sizeof(buf), "%s %s RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: " USER_AGENT "\r\n%s%s%s%s%s\r\n",
                 method, url, ++c->cseq, auth, c->session ? "Session: " : "", c->session ? c->session : "",
                 c->session ? "\r\n" : "", extra ? extra : "");
    if (n >= (int)sizeof(buf))
        return -1;
    return send_all(c, buf, n);
}

static int request(rtsp_client *c, const char *method, const char *url, const char *extra, response *r)
{
    int attempt;
    for (attempt = 0;; attempt++) {
        if (send_request(c, method, url, extra) < 0) {
            log_warn("%s: send failed", method);
            return -1;
        }
        if (read_response(c, r, c->stop) < 0) {
            log_warn("%s: no response", method);
            return -1;
        }
        if (r->status != 401 || attempt >= 2)
            break;
        /* 401: (re)authenticate with the camera's challenge (a new nonce may come with stale=TRUE) */
        if (!c->user) {
            log_warn("%s: the camera wants a user name and password (rtsp://user:password@host/...)", method);
            free_response(r);
            return -1;
        }
        if (attempt == 1 && c->auth == AUTH_BASIC) {
            log_warn("%s: authentication failed (wrong user name or password?)", method);
            free_response(r);
            return -1;
        }
        if (parse_challenge(c, r) < 0) {
            free_response(r);
            return -1;
        }
        free_response(r);
    }
    if (r->status == 401) {
        log_warn("%s: authentication failed (wrong user name or password?)", method);
        free_response(r);
        return -1;
    }
    if (r->status != 200) {
        char *eol = strstr(r->headers, "\r\n");
        if (eol)
            *eol = 0;
        log_warn("%s %s: %s", method, url, r->headers);
        free_response(r);
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- SDP */

static int b64val(int ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+' || ch == '-') return 62;
    if (ch == '/' || ch == '_') return 63;
    return -1;
}

/* appends 00 00 00 01 + decoded NAL for every comma separated base64 item */
static void add_sprop(rtsp_client *c, const char *s, size_t len)
{
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        uint8_t *n;
        int bits = 0, acc = 0, out;
        while (j < len && s[j] != ',')
            j++;
        n = realloc(c->sprop, c->sprop_len + 4 + (j - i) * 3 / 4 + 4);
        if (!n)
            return;
        c->sprop = n;
        out = c->sprop_len + 4;
        for (; i < j; i++) {
            int v = b64val((unsigned char)s[i]);
            if (v < 0)
                continue;               /* '=' padding */
            acc = (acc << 6) | v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                c->sprop[out++] = (acc >> bits) & 0xFF;
            }
        }
        if (out > c->sprop_len + 4) {
            memcpy(c->sprop + c->sprop_len, "\0\0\0\1", 4);
            c->sprop_len = out;
        }
        i = j + 1;
    }
}

static char *dup_value(const char *s)
{
    size_t n = strcspn(s, "\r\n");
    char *d = malloc(n + 1);
    if (d) {
        memcpy(d, s, n);
        d[n] = 0;
    }
    return d;
}

static int parse_sdp(rtsp_client *c, const char *sdp)
{
    const char *line = sdp;
    int in_video = 0, found = 0, video_pt = -1;

    while (line && *line) {
        const char *eol = strchr(line, '\n');
        if (!strncmp(line, "m=", 2)) {
            in_video = 0;
            if (!found && !strncmp(line, "m=video ", 8)) {
                int port, pt;
                char proto[32];
                if (sscanf(line, "m=video %d %31s %d", &port, proto, &pt) == 3) {
                    in_video = 1;
                    found = 1;
                    video_pt = pt;
                }
            }
        } else if (!strncmp(line, "a=control:", 10)) {
            char **dst = in_video ? &c->video_control : (found ? NULL : &c->session_control);
            if (dst && !*dst)
                *dst = dup_value(line + 10);
        } else if (in_video && !strncmp(line, "a=rtpmap:", 9)) {
            int pt, rate;
            char enc[32];
            if (sscanf(line + 9, "%d %31[^/]/%d", &pt, enc, &rate) == 3 && pt == video_pt) {
                if (strcasecmp(enc, "H264")) {
                    log_warn("video codec %s is not supported (H.264 only)", enc);
                    return -1;
                }
                c->clock_rate = rate;
            }
        } else if (in_video && !strncmp(line, "a=fmtp:", 7)) {
            const char *p = strstr(line, "sprop-parameter-sets=");
            if (p && (!eol || p < eol)) {
                p += 21;
                add_sprop(c, p, strcspn(p, "; \r\n"));
            }
        }
        line = eol ? eol + 1 : NULL;
    }
    if (!found) {
        log_warn("no video stream in the SDP");
        return -1;
    }
    c->payload_type = video_pt;
    if (c->clock_rate <= 0)
        c->clock_rate = 90000;
    return 0;
}

static char *resolve(const char *base, const char *control)
{
    size_t bl;
    char *r;
    if (!control || !*control || !strcmp(control, "*"))
        return strdup(base);
    if (strstr(control, "://"))
        return strdup(control);
    bl = strlen(base);
    r = malloc(bl + strlen(control) + 2);
    if (r)
        sprintf(r, "%s%s%s", base, bl && base[bl - 1] == '/' ? "" : "/", control);
    return r;
}

/* ---------------------------------------------------------------- public API */

/* end of the user info ('@') in "user:pass@host/path", or NULL. The password may contain
 * '/' or '@', so the last '@' counts unless it is only in the path ("host/a@b"). */
static const char *userinfo_end(const char *hp)
{
    const char *at = strrchr(hp, '@'), *slash = strchr(hp, '/');
    if (at && slash && slash < at && !strchr(at, '/'))
        return NULL;
    return at;
}

static char *pct_decode(const char *s, size_t n)
{
    char *r = malloc(n + 1);
    size_t i, o = 0;
    if (!r)
        return NULL;
    for (i = 0; i < n; i++) {
        unsigned v;
        if (s[i] == '%' && i + 2 < n && sscanf(s + i + 1, "%2x", &v) == 1) {
            r[o++] = (char)v;
            i += 2;
        } else {
            r[o++] = s[i];
        }
    }
    r[o] = 0;
    return r;
}

void rtsp_redact(const char *url, char *out, size_t outlen)
{
    const char *hp = url, *at, *colon;
    if (!strncasecmp(url, "rtsp://", 7))
        hp = url + 7;
    at = userinfo_end(hp);
    colon = at ? memchr(hp, ':', at - hp) : NULL;
    if (!at || !colon)
        snprintf(out, outlen, "%s", url);
    else
        snprintf(out, outlen, "%.*s:***%s", (int)(colon - url), url, at);
}

static void keepalive(rtsp_client *c)
{
    const char *url = c->base;
    double interval = c->session_timeout / 2.0;
    if (interval > 30)
        interval = 30;
    if (mono() - c->last_keepalive < interval)
        return;
    c->last_keepalive = mono();
    /* the response arrives between the RTP packets and is skipped there */
    if (send_request(c, c->use_get_parameter ? "GET_PARAMETER" : "OPTIONS", url, NULL) < 0)
        log_warn("keepalive failed");
}

rtsp_client *rtsp_open(const char *url, double timeout, volatile int *stop)
{
    rtsp_client *c;
    response r;
    char host[256], val[1024], *path, *setup_url = NULL, *play_url = NULL;
    const char *hp, *at, *slash;
    int port = 554;
    size_t hl;

    if (strncasecmp(url, "rtsp://", 7)) {
        log_error("not an rtsp:// URL: %s", url);
        return NULL;
    }
    c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->fd = -1;
    c->stop = stop;
    c->timeout = timeout > 0 ? timeout : 10;
    c->session_timeout = 60;
    c->rtp_channel = 0;
    c->rbuf = malloc(RBUF_SIZE + 1);
    if (!c->rbuf)
        goto fail;

    hp = url + 7;
    at = userinfo_end(hp);
    if (at) {
        const char *colon = memchr(hp, ':', at - hp);
        c->user = pct_decode(hp, colon ? (size_t)(colon - hp) : (size_t)(at - hp));
        c->pass = colon ? pct_decode(colon + 1, at - colon - 1) : strdup("");
        hp = at + 1;
    }
    slash = strchr(hp, '/');
    hl = slash ? (size_t)(slash - hp) : strlen(hp);
    if (hl >= sizeof(host))
        goto fail;
    memcpy(host, hp, hl);
    host[hl] = 0;
    if (host[0] == '[') {                       /* [IPv6]:port */
        char *e = strchr(host, ']');
        if (!e)
            goto fail;
        if (e[1] == ':')
            port = atoi(e + 2);
        *e = 0;
        memmove(host, host + 1, strlen(host));
    } else if (strchr(host, ':')) {
        char *colon = strchr(host, ':');
        port = atoi(colon + 1);
        *colon = 0;
    }
    /* the request URL is the given one without the user info */
    path = slash ? strdup(slash) : strdup("/");
    c->url = malloc(hl + strlen(path) + 8);
    if (!path || !c->url) {
        free(path);
        goto fail;
    }
    sprintf(c->url, "rtsp://%.*s%s", (int)hl, hp, path);
    free(path);

    c->fd = tcp_connect(host, port, c->timeout);
    if (c->fd < 0)
        goto fail;

    if (request(c, "OPTIONS", c->url, NULL, &r) < 0)
        goto fail;
    if (header(&r, "Public", val, sizeof(val)) && strstr(val, "GET_PARAMETER"))
        c->use_get_parameter = 1;
    free_response(&r);

    if (request(c, "DESCRIBE", c->url, "Accept: application/sdp\r\n", &r) < 0)
        goto fail;
    if (header(&r, "Content-Base", val, sizeof(val)) || header(&r, "Content-Location", val, sizeof(val)))
        c->base = strdup(val);
    else
        c->base = strdup(c->url);
    if (!r.body || parse_sdp(c, r.body) < 0) {
        free_response(&r);
        goto fail;
    }
    free_response(&r);

    setup_url = resolve(c->base, c->video_control);
    if (!setup_url ||
        request(c, "SETUP", setup_url, "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n", &r) < 0)
        goto fail;
    if (header(&r, "Session", val, sizeof(val))) {
        char *semi = strchr(val, ';');
        const char *to = strstr(val, "timeout=");
        if (to && atoi(to + 8) > 0)
            c->session_timeout = atoi(to + 8);
        if (semi)
            *semi = 0;
        c->session = strdup(val);
    }
    if (header(&r, "Transport", val, sizeof(val))) {
        const char *il = strstr(val, "interleaved=");
        if (il)
            c->rtp_channel = atoi(il + 12);
        if (!strstr(val, "TCP"))
            log_warn("the camera did not accept RTP over TCP: %s", val);
    }
    free_response(&r);

    play_url = resolve(c->base, c->session_control);
    if (!play_url || request(c, "PLAY", play_url, "Range: npt=0.000-\r\n", &r) < 0)
        goto fail;
    free_response(&r);
    c->last_keepalive = mono();
    free(setup_url);
    free(play_url);
    log_info("connected: %s (payload %d, %d Hz, session timeout %d s)", c->url, c->payload_type,
             c->clock_rate, c->session_timeout);
    return c;
fail:
    free(setup_url);
    free(play_url);
    rtsp_close(c);
    return NULL;
}

const uint8_t *rtsp_sprop(const rtsp_client *c, int *size)
{
    *size = c->sprop_len;
    return c->sprop;
}

int rtsp_clock_rate(const rtsp_client *c)
{
    return c->clock_rate;
}

int rtsp_payload_type(const rtsp_client *c)
{
    return c->payload_type;
}

int rtsp_read_rtp(rtsp_client *c, const uint8_t **pkt, volatile int *stop)
{
    for (;;) {
        int r;
        keepalive(c);
        r = fill(c, 4, stop);
        if (r)
            return r > 0 ? 0 : -1;
        if (c->rbuf[c->rpos] == '$') {
            int ch = c->rbuf[c->rpos + 1];
            size_t n = (c->rbuf[c->rpos + 2] << 8) | c->rbuf[c->rpos + 3];
            r = fill(c, 4 + n, stop);
            if (r)
                return r > 0 ? 0 : -1;
            c->rpos += 4 + n;
            c->packets[ch & 3]++;
            c->in_resync = 0;
            if (ch == c->rtp_channel && n >= 12) {
                *pkt = c->rbuf + c->rpos - n;
                return (int)n;
            }
            continue;                           /* RTCP or another channel */
        }
        if (log_debug_enabled && c->rbuf[c->rpos] != '$') {
            size_t n = strcspn((char *)c->rbuf + c->rpos, "\r\n");
            log_debug("RTSP message from the camera: %.*s", (int)(n > 120 ? 120 : n), (char *)c->rbuf + c->rpos);
        }
        if (!memcmp(c->rbuf + c->rpos, "RTSP", 4) || !memcmp(c->rbuf + c->rpos, "ANNO", 4) ||
            !memcmp(c->rbuf + c->rpos, "GET_", 4) || !memcmp(c->rbuf + c->rpos, "SET_", 4) ||
            !memcmp(c->rbuf + c->rpos, "OPTI", 4)) {
            response resp;
            if (memcmp(c->rbuf + c->rpos, "RTSP", 4)) {
                /* a request from the server: skip its header block */
                char *end;
                c->rbuf[c->rlen] = 0;
                while (!(end = strstr((char *)c->rbuf + c->rpos, "\r\n\r\n")))
                    if (fill(c, c->rlen - c->rpos + 1, stop))
                        return -1;
                c->rpos = end + 4 - (char *)c->rbuf;
                continue;
            }
            if (read_response(c, &resp, stop) < 0)
                return -1;
            if (resp.status == 401 && c->user)
                parse_challenge(c, &resp);      /* new nonce for the next keepalive */
            else if (resp.status != 200)
                log_warn("keepalive answered with status %d", resp.status);
            free_response(&resp);
            continue;
        }
        if (!c->in_resync) {
            c->in_resync = 1;
            c->resyncs++;
        }
        c->resync_bytes++;
        c->rpos++;                              /* garbage: resync on the next '$' */
    }
}

void rtsp_close(rtsp_client *c)
{
    if (!c)
        return;
    log_debug("interleaved packets per channel: %ld %ld %ld %ld", c->packets[0], c->packets[1], c->packets[2],
              c->packets[3]);
    if (c->resyncs)
        log_warn("the interleaved RTP framing from the camera was broken %ld times (%ld bytes skipped)",
                 c->resyncs, c->resync_bytes);
    if (c->fd >= 0) {
        if (c->session && c->base) {
            double t = c->timeout;
            c->timeout = 1;
            send_request(c, "TEARDOWN", c->base, NULL);
            c->timeout = t;
        }
        close(c->fd);
    }
    free(c->url);
    free(c->user);
    free(c->pass);
    free(c->realm);
    free(c->nonce);
    free(c->opaque);
    free(c->base);
    free(c->session);
    free(c->video_control);
    free(c->session_control);
    free(c->sprop);
    free(c->rbuf);
    free(c);
}
