/*
 * xrace.c -- several threads, one input connection: does a thread get stuck?
 *
 * Each session thread calls `rd_cursor_pos` once per pass of its loop, about
 * 25 times a second, and before input_shim.c took a lock they all did it on the
 * one Display the shim keeps -- with no XInitThreads, so Xlib's LockDisplay was
 * a no-op. Two sessions at once meant two threads doing XQueryPointer round
 * trips on one connection. On the O2 that left one session frozen and a thread
 * spinning in user code for the life of the process, one more each time.
 *
 * This is that, without the agent: N threads calling `rd_cursor_pos` as fast as
 * they can. It links input_shim.c exactly as the agent does, so it tests the
 * shim that ships.
 *
 *     xrace [threads [seconds [cut_at]]]
 *
 * Every second it prints the process CPU and each thread's call count. A thread
 * whose count stops moving is stuck; if the CPU keeps climbing meanwhile, it is
 * stuck spinning.
 *
 * `cut_at` shuts the X socket down at that second, the way a server restart at
 * logout does, without touching the server: the shim should say it lost the
 * connection, reconnect on the next call, and the counts carry on. Build:
 *
 *     irix-cc -O2 -o xrace probes/xrace.c src/input_shim.c src/capture_shim.c \
 *         -lXext -lX11 -lpthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/socket.h>

void rd_cursor_pos(double *x, double *y);

#define MAX_THREADS 8
static volatile unsigned long g_count[MAX_THREADS];

static void *worker(void *arg)
{
    int i = (int)(long)arg;
    double x, y;
    for (;;) {
        rd_cursor_pos(&x, &y);
        g_count[i]++;
    }
    return NULL;
}

static long cpu_ms(void)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000L
         + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000L;
}

/* Shut down every socket this process has open. In this probe the only ones
 * are X connections. Returns how many. */
static int cut_sockets(void)
{
    int fd, n = 0;
    struct stat st;
    for (fd = 3; fd < 256; fd++)
        if (fstat(fd, &st) == 0 && S_ISSOCK(st.st_mode) && shutdown(fd, 2) == 0)
            n++;
    return n;
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 2;
    int secs = argc > 2 ? atoi(argv[2]) : 30;
    int cut_at = argc > 3 ? atoi(argv[3]) : 0;
    unsigned long last[MAX_THREADS] = {0};
    pthread_t t[MAX_THREADS];
    double x, y;
    int i, s;

    if (n < 1) n = 1;
    if (n > MAX_THREADS) n = MAX_THREADS;

    /* Open the shared connection before any thread exists, as the agent does
     * once its first session has run. */
    rd_cursor_pos(&x, &y);
    printf("pointer at %.0f,%.0f; %d threads for %d s\n", x, y, n, secs);

    for (i = 0; i < n; i++)
        pthread_create(&t[i], NULL, worker, (void *)(long)i);

    for (s = 1; s <= secs; s++) {
        int stuck = 0;
        sleep(1);
        if (s == cut_at)
            printf("--- shutting down %d X socket(s)\n", cut_sockets());
        printf("%3ds cpu %6ld ms  calls/s:", s, cpu_ms());
        for (i = 0; i < n; i++) {
            unsigned long c = g_count[i];
            printf(" %6lu", c - last[i]);
            if (c == last[i])
                stuck++;
            last[i] = c;
        }
        printf("%s\n", stuck ? "   <- STUCK" : "");
        fflush(stdout);
    }
    return 0;
}
