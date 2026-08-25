/*
 * cliptest.c -- does the clipboard shim actually talk to other X clients?
 *
 * Reading your own selection proves nothing: the shim short-circuits that, and
 * an implementation that never answers a SelectionRequest would still pass. So
 * this is two processes -- one owns the clipboard and serves it, the other asks
 * for it over the wire -- which is the arrangement the agent is in.
 *
 *   cliptest set "some text" 20   # own it and serve for 20 seconds
 *   cliptest get                  # ask whoever owns it
 *   cliptest watch 10             # report changes for 10 seconds
 *
 *   gcc -O1 -o cliptest cliptest.c ../src/clipboard_shim.c -lXfixes -lX11
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int rd_clip_ok(void);
int rd_clip_changed(void);
int rd_clip_get(unsigned char *out, int cap);
int rd_clip_set(const unsigned char *text, int len);

int main(int argc, char **argv)
{
    static unsigned char buf[64 * 1024];
    const char *verb = argc > 1 ? argv[1] : "get";

    if (!rd_clip_ok()) {
        fprintf(stderr, "cliptest: clipboard unavailable\n");
        return 1;
    }

    if (strcmp(verb, "set") == 0) {
        const char *text = argc > 2 ? argv[2] : "hello from sparc";
        int secs = argc > 3 ? atoi(argv[3]) : 10;
        int i;
        if (rd_clip_set((const unsigned char *)text, (int)strlen(text)) != 0) {
            fprintf(stderr, "cliptest: set failed\n");
            return 1;
        }
        printf("owning the clipboard with %d bytes for %ds\n", (int)strlen(text), secs);
        fflush(stdout);
        /* Serving is what ownership means: a request arrives as an event, and
         * the shim answers it from inside any of its entry points. */
        for (i = 0; i < secs * 10; i++) {
            rd_clip_changed();
            usleep(100 * 1000);
        }
        puts("done owning");
        return 0;
    }

    if (strcmp(verb, "watch") == 0) {
        int secs = argc > 2 ? atoi(argv[2]) : 10;
        int i, seen = 0;
        printf("watching for %ds\n", secs);
        for (i = 0; i < secs * 10; i++) {
            if (rd_clip_changed()) {
                int n = rd_clip_get(buf, (int)sizeof buf - 1);
                buf[n > 0 ? n : 0] = 0;
                printf("  changed: %d bytes %s\n", n, n > 0 ? (char *)buf : "");
                seen++;
            }
            usleep(100 * 1000);
        }
        printf("%d change(s)\n", seen);
        return 0;
    }

    {
        int n = rd_clip_get(buf, (int)sizeof buf - 1);
        if (n < 0) { fprintf(stderr, "cliptest: get failed\n"); return 1; }
        buf[n] = 0;
        printf("%d bytes: %s\n", n, (char *)buf);
        return n > 0 ? 0 : 2;
    }
}
