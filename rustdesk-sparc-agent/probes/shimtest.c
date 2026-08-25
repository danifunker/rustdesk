/*
 * shimtest.c -- a C driver for src/capture_shim.c.
 *
 * The same sequence captest.rs performs, without a Rust toolchain in the way,
 * so the shim can be exercised against any X server -- including this Linux
 * box's, where it was first proved out before the SPARC build existed.
 *
 *   gcc -O1 -I../src -o shimtest shimtest.c ../src/capture_shim.c \
 *       -lXext -lXdamage -lX11                      # on a build host
 *
 *   gcc-5.5 -m64 -O2 -I/usr/openwin/include -I../src -o shimtest \
 *       shimtest.c ../src/capture_shim.c \
 *       -L/usr/openwin/lib/sparcv9 -R/usr/openwin/lib/sparcv9 \
 *       -L/usr/openwin/sfw/lib/sparcv9 -R/usr/openwin/sfw/lib/sparcv9 \
 *       -lXext -lXdamage -lX11                      # on the Blade
 *
 * Argument 1 is the display, argument 2 an RD_PATH_* ceiling (2 forces the
 * XGetImage fallback).
 */
#include "capture_shim.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

static double now_ms(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

int main(int argc, char **argv) {
    const char *disp = argc > 1 ? argv[1] : NULL;
    int max_path = argc > 2 ? atoi(argv[2]) : RD_PATH_DAMAGE;
    rd_capture *c;
    int rects[64 * 4];
    int w = 0, h = 0, i, n;
    double t, best = 1e9, total = 0;

    if (rd_display_size(&w, &h) == 0) printf("display_size    %dx%d\n", w, h);
    c = rd_capture_open_forced(disp, max_path);
    if (!c) { fprintf(stderr, "open failed\n"); return 1; }

    printf("screen          %dx%d depth %d\n", rd_capture_width(c), rd_capture_height(c), rd_capture_depth(c));
    printf("path            %d  order %d  stride %d  cursor_embedded %d\n",
           rd_capture_path(c), rd_capture_pixel_order(c), rd_capture_stride(c),
           rd_capture_cursor_embedded(c));

    t = now_ms();
    if (rd_capture_full(c) != 0) { fprintf(stderr, "full: %s\n", rd_capture_last_error(c)); return 1; }
    printf("first full read %.1f ms\n", now_ms() - t);
    for (i = 0; i < 10; i++) {
        double dt;
        t = now_ms();
        if (rd_capture_full(c) != 0) { fprintf(stderr, "full: %s\n", rd_capture_last_error(c)); return 1; }
        dt = now_ms() - t;
        if (dt < best) best = dt;
        total += dt;
    }
    printf("10 full reads   best %.1f ms, mean %.1f ms\n", best, total / 10);

    n = rd_capture_poll(c, rects, 64);
    printf("first poll      %d rect(s) (after a full read this is damage, not the whole screen: %d,%d %dx%d)\n",
           n, rects[0], rects[1], rects[2], rects[3]);

    t = now_ms();
    for (i = 0; i < 20; i++) n = rd_capture_poll(c, rects, 64);
    printf("20 polls        %.1f ms total, last reported %d\n", now_ms() - t, n);

    /* Something is bound to change on a live desktop; report what damage says. */
    printf("watching 2s ...\n");
    t = now_ms();
    while (now_ms() - t < 2000) {
        n = rd_capture_poll(c, rects, 64);
        if (n > 0) {
            printf("  %d rect(s), first %dx%d at %d,%d\n", n, rects[2], rects[3], rects[0], rects[1]);
            break;
        }
    }
    { const unsigned char *b = rd_capture_buffer(c);
      printf("first 8 bytes   %02x %02x %02x %02x %02x %02x %02x %02x\n",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]); }
    printf("dead            %d   err '%s'\n", rd_capture_is_dead(c), rd_capture_last_error(c));
    rd_capture_close(c);
    return 0;
}
