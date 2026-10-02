#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

int cdv_verbose;
static FILE *logf;

void cdv_log_open(const char *path, int verbose)
{
    cdv_verbose = verbose;
    if (path && *path) {
        logf = fopen(path, "a");
        if (logf)
            setvbuf(logf, NULL, _IOLBF, 0);
    }
}

static void vlog(const char *fmt, va_list ap)
{
    char line[512];
    char stamp[32];
    time_t t = time(NULL);
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", localtime(&t));
    vsnprintf(line, sizeof line, fmt, ap);
    fprintf(stderr, "%s %s\n", stamp, line);
    if (logf)
        fprintf(logf, "%s %s\n", stamp, line);
}

void cdv_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

void cdv_debug(const char *fmt, ...)
{
    va_list ap;
    if (!cdv_verbose)
        return;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}
