/*
 * xpoke.c -- aim the agent's own injection shim at a coordinate, by hand.
 *
 * The Motif panel is the first thing on this machine with a button on it, and
 * pressing one is the last unproven step in the input path. `r-deskvint-irix
 * --probe-keys X Y` does part of it, but it types a fixed word and takes a
 * photograph; driving a settings panel needs an arbitrary sequence -- click a
 * field, type a hostname, click Apply -- and needs it in ONE process, because
 * each run costs an X connection setup and a fork on a machine where both are
 * slow.
 *
 * So: the same `input_shim.c` the agent links, with a command line in front of
 * it. Nothing here is agent-specific and nothing here is test scaffolding
 * inside the agent -- if this file is deleted the agent is unchanged.
 *
 * Deliberately NOT libXtst: SGI ships XTEST as a static archive that LLD
 * refuses (see the header of input_shim.c), which is the whole reason the shim
 * writes the protocol requests out by hand.
 *
 *   xpoke click 640 512
 *   xpoke click 400 300 type rustdesk.example.net click 900 300
 *   xpoke move 100 100 sleep 500 click 100 100
 *
 * Commands run left to right and can repeat. `type` takes ONE argument, so
 * anything with a space in it needs quoting on the shell side.
 *
 * Copyright (C) 2026 Dani Sarfati. Same licence as the rest of the tree.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>

/* input_shim.c's interface, as input.rs declares it. */
extern void rd_mouse(int type, double x, double y, int button);
extern void rd_key_char(unsigned int cp, int down, unsigned int flags);
extern int  rd_keycode_for_char(unsigned int cp, int *needs_shift);
extern void rd_key(int keycode, int down);

/* Mac mouse event types, which is what the shim speaks: 0 move, 1 down, 2 up. */
#define M_MOVE 0
#define M_DOWN 1
#define M_UP   2

/* select(2) rather than usleep(3): usleep is undefined past a second and IRIX
 * 5.3, where this has to build too, has no nanosleep. */
static void nap(int ms)
{
    struct timeval tv;
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (long)(ms % 1000) * 1000L;
    select(0, NULL, NULL, NULL, &tv);
}

/*
 * Move, settle, press, hold, release.
 *
 * The two waits are not padding. XTEST motion and the button press that follows
 * it are separate requests, and Motif decides which widget a press belongs to
 * from the pointer position it has *at that moment* -- so a press issued in the
 * same millisecond as the move can be delivered to whatever was under the
 * pointer before. And a press and release closer together than the server's
 * event granularity has been seen to arrive as a press with no release, which
 * leaves a PushButton armed and the panel unresponsive to everything after.
 */
static void click_at(int x, int y)
{
    printf("click %d,%d\n", x, y);
    fflush(stdout);
    rd_mouse(M_MOVE, (double)x, (double)y, 0);
    nap(250);
    rd_mouse(M_DOWN, (double)x, (double)y, 0);
    nap(120);
    rd_mouse(M_UP, (double)x, (double)y, 0);
    nap(250);
}

/*
 * Type a string the way a client sends one: a codepoint at a time, through
 * `rd_key_char`, which is the path a real peer's keystrokes take. Reports the
 * mapping for each character first -- an unmapped character falls back to
 * remapping a spare keycode, which works but is worth knowing about, and a
 * whole string of them means the layout lookup is broken rather than the
 * injection.
 */
static void type_str(const char *s)
{
    const unsigned char *p;

    printf("type \"%s\"\n", s);
    for (p = (const unsigned char *)s; *p != '\0'; p++) {
        int shift = 0;
        int code = rd_keycode_for_char((unsigned int)*p, &shift);

        if (code < 0)
            printf("  '%c' unmapped -- falling back to unicode entry\n", *p);
        (void)shift;
        rd_key_char((unsigned int)*p, 1, 0);
        nap(40);
        rd_key_char((unsigned int)*p, 0, 0);
        nap(90);
    }
    fflush(stdout);
}

static int want_int(const char *what, int argc, char **argv, int i)
{
    if (i >= argc) {
        fprintf(stderr, "xpoke: %s needs another argument\n", what);
        exit(2);
    }
    return atoi(argv[i]);
}

int main(int argc, char **argv)
{
    int i = 1;

    if (getenv("DISPLAY") == NULL) {
        fprintf(stderr, "xpoke: DISPLAY is not set\n");
        return 2;
    }
    if (argc < 2) {
        fprintf(stderr,
                "usage: xpoke [click X Y | move X Y | type TEXT | sleep MS]...\n");
        return 2;
    }

    while (i < argc) {
        const char *cmd = argv[i++];

        if (strcmp(cmd, "click") == 0) {
            int x = want_int("click", argc, argv, i++);
            int y = want_int("click", argc, argv, i++);
            click_at(x, y);
        } else if (strcmp(cmd, "move") == 0) {
            int x = want_int("move", argc, argv, i++);
            int y = want_int("move", argc, argv, i++);
            printf("move %d,%d\n", x, y);
            fflush(stdout);
            rd_mouse(M_MOVE, (double)x, (double)y, 0);
            nap(200);
        } else if (strcmp(cmd, "type") == 0) {
            if (i >= argc) {
                fprintf(stderr, "xpoke: type needs a string\n");
                return 2;
            }
            type_str(argv[i++]);
        } else if (strcmp(cmd, "sleep") == 0) {
            int ms = want_int("sleep", argc, argv, i++);
            nap(ms);
        } else {
            fprintf(stderr, "xpoke: unknown command \"%s\"\n", cmd);
            return 2;
        }
    }
    printf("done\n");
    return 0;
}
