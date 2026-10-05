#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "log.h"

int log_debug_enabled;
int log_quiet;
static char camera[64] = "?";

void log_setup(const char *name, int debug)
{
    snprintf(camera, sizeof(camera), "%s", name ? name : "?");
    log_debug_enabled = debug;
}

void log_msg(const char *level, const char *fmt, ...)
{
    struct timeval tv;
    struct tm tm;
    char ts[32];
    va_list ap;

    if (log_quiet)
        return;
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(stderr, "%s,%03d rtspcam[%s] %s ", ts, (int)(tv.tv_usec / 1000), camera, level);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
