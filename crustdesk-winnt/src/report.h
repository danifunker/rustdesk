/* Reporting in to a console (RustDesk Pro, CortenDesk), so the machine is in
 * its device list: /api/sysinfo, then /api/heartbeat every 15 s, over https
 * with BearSSL (the shared core's console.c and https.c).
 *
 * It runs in a thread of its own, with plain blocking sockets, below the
 * agent's priority: a TLS handshake is seconds of an old CPU's time, and no
 * session may wait for it. It shares nothing with the agent once started
 * but the log and the user-name callback. */
#ifndef CDV_REPORT_H
#define CDV_REPORT_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *url;      /* api= in the settings: https://host[:port][/prefix] */
    const char *id;
    const uint8_t *uuid;  /* 16 bytes; the console pins the first it sees */
    const uint8_t *secret;/* 32 bytes only this machine has: seeds TLS */
    const char *hostname;
    const char *(*username)(void); /* who is logged in, asked at each inventory */
} report_config;

/* Start the reporter: 1 if it is running, 0 if not (logged why). */
int report_start(const report_config *c);
void report_stop(void);

/* What the machine is, for the inventory. */
void report_describe(char *cpu, size_t ncpu, char *mem, size_t nmem, char *os, size_t nos);

#endif
