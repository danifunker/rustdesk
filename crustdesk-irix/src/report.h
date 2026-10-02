/* Reporting in to a console (RustDesk Pro, CortenDesk), so the machine is in
 * its device list: /api/sysinfo, then /api/heartbeat every 15 s, over https
 * with BearSSL (the shared core's console.c and https.c).
 *
 * A TLS handshake is seconds of an R3000's time, so this runs in a process of
 * its own, with plain blocking I/O, and no session ever waits for it. It
 * needs nothing from the agent once started, and leaves when the agent does.
 */
#ifndef CDV_REPORT_H
#define CDV_REPORT_H

#include <stdint.h>
#include <sys/types.h>

typedef struct {
    const char *url;      /* api= in prefs: https://host[:port][/prefix] */
    const char *id;
    const uint8_t *uuid;  /* 16 bytes; the console pins the first it sees */
    const uint8_t *secret;/* 32 bytes only this machine has: seeds TLS */
    const char *hostname;
    const char *(*username)(void); /* who is logged in, asked at each inventory */
} report_config;

/* Fork the reporter: its pid; 0 if it could not (try again later); -1 if
 * the URL is not one. Logs why. */
pid_t report_start(const report_config *c);

/* What the machine is, for the inventory. */
void report_describe(char *cpu, size_t ncpu, char *mem, size_t nmem, char *os, size_t nos);

#endif
