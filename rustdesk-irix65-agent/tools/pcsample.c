/*
 * pcsample.c -- where is each thread of a running IRIX process, right now?
 *
 *     pcsample PID [seconds [interval_ms [stop]]]
 *
 * IRIX has no dbx on a stock machine, and `par` sees only system calls, so a
 * thread spinning in user code is invisible to both: it shows up as CPU time
 * and nothing else. This asks /proc instead. Every interval it walks the
 * process's kernel threads with PIOCTHREAD + PIOCSTATUS and prints, per thread,
 * the program counter, the return address and the thread's CPU time. Pipe the
 * output through tools/pcsample-report.py on the build host to turn addresses
 * into function names.
 *
 * `stop` stops the whole process around each sample (PIOCSTOP ... PIOCRUN).
 * Without it the registers of a thread that is on the CPU at the moment of the
 * read may be stale; on a single-processor machine that thread is this one, so
 * the default is usually fine, and it does not disturb the target at all.
 *
 * Output: the executable mappings once (see below), then one line per thread
 * per sample:
 *
 *     T <sample> <tid> <flags-hex> <epc> <ra> <sp> <utime-ms> <stime-ms> <syscall>
 *
 * Built with irix-cc; it needs nothing beyond libc:
 *
 *     irix-cc -O2 -o pcsample tools/pcsample.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/signal.h>
#include <sys/fault.h>
#include <sys/syscall.h>
#include <sys/procfs.h>
#include <sys/ucontext.h>

static long ms_of(const timespec_t *t)
{
    return (long)t->tv_sec * 1000L + (long)(t->tv_nsec / 1000000L);
}

static int open_proc(pid_t pid)
{
    char path[64];
    int fd;
    sprintf(path, "/proc/%d", (int)pid);
    fd = open(path, O_RDWR);
    if (fd < 0) {
        /* Older IRIX names them zero-padded. */
        sprintf(path, "/proc/%05d", (int)pid);
        fd = open(path, O_RDWR);
    }
    return fd;
}

int main(int argc, char **argv)
{
    pid_t pid;
    int secs, ms, stop, fd, n, s, threads;
    struct timespec gap;

    if (argc < 2) {
        fprintf(stderr, "usage: %s PID [seconds [interval_ms [stop]]]\n", argv[0]);
        return 2;
    }
    pid = (pid_t)atoi(argv[1]);
    secs = argc > 2 ? atoi(argv[2]) : 5;
    ms = argc > 3 ? atoi(argv[3]) : 20;
    stop = argc > 4 && strcmp(argv[4], "stop") == 0;
    if (secs < 1) secs = 1;
    if (ms < 1) ms = 1;

    fd = open_proc(pid);
    if (fd < 0) {
        fprintf(stderr, "pcsample: cannot open /proc for %d: %s\n", (int)pid, strerror(errno));
        return 1;
    }

    /* The executable mappings first, so addresses inside shared libraries can
     * be told apart: `ls -i` on the machine turns an inode into a file name.
     *
     *     M <vaddr> <size> <offset> <flags-hex> <inode>
     */
    {
        static prmap_sgi_t maps[512];
        prmap_sgi_arg_t arg;
        int i, nmaps;
        arg.pr_vaddr = (caddr_t)maps;
        arg.pr_size = sizeof maps;
        nmaps = ioctl(fd, PIOCMAP_SGI, &arg);
        if (nmaps < 0)
            fprintf(stderr, "pcsample: PIOCMAP_SGI: %s\n", strerror(errno));
        for (i = 0; i < nmaps; i++) {
            if (!(maps[i].pr_mflags & MA_EXEC))
                continue;
            printf("M %lx %lx %llx %lx %llu\n",
                   (unsigned long)maps[i].pr_vaddr, (unsigned long)maps[i].pr_size,
                   (unsigned long long)maps[i].pr_off, (unsigned long)maps[i].pr_mflags,
                   (unsigned long long)maps[i].pr_ino);
        }
    }

    gap.tv_sec = ms / 1000;
    gap.tv_nsec = (long)(ms % 1000) * 1000000L;
    n = secs * 1000 / ms;
    for (s = 0; s < n; s++) {
        tid_t tid = 0;
        prstatus_t st;

        if (stop && ioctl(fd, PIOCSTOP, &st) < 0) {
            fprintf(stderr, "pcsample: PIOCSTOP: %s\n", strerror(errno));
            stop = 0;
        }
        for (threads = 0; threads < 256; threads++) {
            prthreadctl_t tc;
            memset(&st, 0, sizeof st);
            tc.pt_tid = tid;
            tc.pt_cmd = PIOCSTATUS;
            tc.pt_flags = PTFD_GEQ | PTFS_ALL;
            tc.pt_data = (caddr_t)&st;
            if (ioctl(fd, PIOCTHREAD, &tc) < 0) {
                if (threads == 0 && s == 0)
                    fprintf(stderr, "pcsample: PIOCTHREAD: %s\n", strerror(errno));
                break;
            }
            printf("T %d %d %lx %llx %llx %llx %ld %ld %ld\n",
                   s, (int)st.pr_who, (unsigned long)st.pr_flags,
                   (unsigned long long)st.pr_reg[CTX_EPC],
                   (unsigned long long)st.pr_reg[CTX_RA],
                   (unsigned long long)st.pr_reg[CTX_SP],
                   ms_of(&st.pr_utime), ms_of(&st.pr_stime),
                   (long)st.pr_syscall);
            if ((tid_t)st.pr_who < tid)
                break;              /* no progress: do not loop for ever */
            tid = (tid_t)st.pr_who + 1;
        }
        if (stop) {
            prrun_t r;
            memset(&r, 0, sizeof r);
            if (ioctl(fd, PIOCRUN, &r) < 0) {
                fprintf(stderr, "pcsample: PIOCRUN: %s\n", strerror(errno));
                return 1;
            }
        }
        fflush(stdout);
        nanosleep(&gap, NULL);
    }
    close(fd);
    return 0;
}
