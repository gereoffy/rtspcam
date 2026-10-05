/* Live stream output over a Unix domain socket, see live.h. */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "bytebuf.h"
#include "live.h"
#include "log.h"

#define MAX_CLIENTS 8
#define MAX_PENDING (8 << 20)       /* a viewer this far behind is dropped */
#define MAX_GOP (16 << 20)          /* the kept pictures since the last key frame */

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0              /* macOS: SIGPIPE is ignored by the program */
#endif

typedef struct client {
    int fd;
    bytebuf q;                      /* pending bytes */
    size_t off;                     /* already sent part of q */
} client;

struct live_srv {
    int fd;
    char *path;
    bytebuf init;                   /* framed init message */
    bytebuf gop;                    /* framed messages since the last key frame */
    int gop_ok;                     /* the cache holds a whole GOP from its key frame */
    uint8_t status[2];              /* last status: alarm, recording */
    int have_status;
    client c[MAX_CLIENTS];
    int n;
};

static void put_msg(bytebuf *b, int type, const uint8_t *d, size_t len)
{
    bb_u8(b, type);
    bb_u32(b, (uint32_t)len);
    bb_put(b, d, len);
}

live_srv *live_open(const char *path)
{
    struct sockaddr_un a;
    live_srv *l;
    int fd;

    if (strlen(path) >= sizeof(a.sun_path)) {
        log_error("live socket path too long: %s", path);
        return NULL;
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return NULL;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, path);
    unlink(path);                   /* a socket file left by an earlier run */
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 8) < 0) {
        log_error("cannot create the live socket %s: %s", path, strerror(errno));
        close(fd);
        return NULL;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    l = calloc(1, sizeof(*l));
    if (!l) {
        close(fd);
        return NULL;
    }
    l->fd = fd;
    l->path = strdup(path);
    log_info("live stream on %s", path);
    return l;
}

static void drop(live_srv *l, int i, const char *why)
{
    log_info("live viewer %d disconnected (%s)", l->c[i].fd, why);
    close(l->c[i].fd);
    bb_free(&l->c[i].q);
    l->c[i] = l->c[--l->n];
    memset(&l->c[l->n], 0, sizeof(l->c[l->n]));
}

static void queue_all(live_srv *l, const uint8_t *d, size_t len)
{
    int i;
    for (i = l->n - 1; i >= 0; i--) {
        client *c = &l->c[i];
        if (c->q.len - c->off + len > MAX_PENDING) {
            drop(l, i, "too slow");
            continue;
        }
        if (c->off && c->off == c->q.len) {     /* everything sent: reuse the buffer */
            c->q.len = 0;
            c->off = 0;
        }
        bb_put(&c->q, d, len);
    }
}

void live_send_init(live_srv *l, const uint8_t *init, size_t len)
{
    l->init.len = 0;
    put_msg(&l->init, 1, init, len);
    l->gop.len = 0;
    l->gop_ok = 0;
    queue_all(l, l->init.d, l->init.len);
}

void live_send_frame(live_srv *l, const uint8_t *frag, size_t len, int key)
{
    size_t start;
    if (key) {
        l->gop.len = 0;
        l->gop_ok = 1;
    }
    start = l->gop.len;
    put_msg(&l->gop, key ? 2 : 3, frag, len);
    queue_all(l, l->gop.d + start, l->gop.len - start);
    if (l->gop.len > MAX_GOP) {                 /* no key frame for a long time: stop keeping */
        l->gop.len = 0;
        l->gop_ok = 0;
    }
}

void live_send_status(live_srv *l, int alarm, int recording)
{
    uint8_t msg[7];
    l->status[0] = alarm ? 1 : 0;
    l->status[1] = recording ? 1 : 0;
    l->have_status = 1;
    msg[0] = 4;
    msg[1] = msg[2] = msg[3] = 0;
    msg[4] = 2;
    msg[5] = l->status[0];
    msg[6] = l->status[1];
    queue_all(l, msg, sizeof(msg));
}

void live_poll(live_srv *l)
{
    int i;
    for (;;) {                                  /* new viewers */
        int fd = accept(l->fd, NULL, NULL);
        if (fd < 0)
            break;
        if (l->n == MAX_CLIENTS) {
            close(fd);
            continue;
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        {
            /* the default send buffer of a Unix socket is tiny (8 KB on macOS): a 13 Mbit/s
             * camera would not fit through between two pictures */
            int size = 4 << 20;
            if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) < 0) {
                size = 1 << 20;
                setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
            }
        }
#ifdef SO_NOSIGPIPE
        {
            int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        }
#endif
        memset(&l->c[l->n], 0, sizeof(l->c[l->n]));
        l->c[l->n].fd = fd;
        if (l->init.len) {
            bb_put(&l->c[l->n].q, l->init.d, l->init.len);
            if (l->gop_ok)
                bb_put(&l->c[l->n].q, l->gop.d, l->gop.len);
        }
        if (l->have_status)
            put_msg(&l->c[l->n].q, 4, l->status, 2);
        log_info("live viewer %d connected", fd);
        l->n++;
    }
    for (i = l->n - 1; i >= 0; i--) {           /* send what is pending */
        client *c = &l->c[i];
        while (c->off < c->q.len) {
            ssize_t k = send(c->fd, c->q.d + c->off, c->q.len - c->off, MSG_NOSIGNAL);
            if (k > 0) {
                c->off += k;
                continue;
            }
            if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                break;
            drop(l, i, k == 0 ? "closed" : strerror(errno));
            goto next;
        }
        if (c->off == c->q.len) {
            c->q.len = 0;
            c->off = 0;
        }
next:;
    }
}

void live_close(live_srv *l)
{
    if (!l)
        return;
    while (l->n)
        drop(l, l->n - 1, "stopping");
    close(l->fd);
    unlink(l->path);
    free(l->path);
    bb_free(&l->init);
    bb_free(&l->gop);
    free(l);
}
