/* Reporting in to the console (see core/console.h): the main loop's side.
 *
 * Main loop, not the engine: a TLS handshake is seconds of arithmetic on a
 * 68040, and at deferred-task time that would stop every session with it. In
 * the main loop it only stops the Mac's own event loop, and only when the
 * connection is new -- one connection carries every heartbeat after it. If a
 * menu is held open the heartbeat waits; a console allows four missed beats.
 */
#ifndef CDV_REPORT_H
#define CDV_REPORT_H

#include "net.h"

#include <stdint.h>

/* What the console is told. Mac Roman strings, kept by the caller. */
typedef struct {
    const char *url;      /* "" for no console */
    const char *id;
    const uint8_t *uuid;  /* 16 bytes */
    const uint8_t *secret;/* 32 bytes nobody else knows, to seed TLS from */
    const char *hostname, *username;
} report_config;

/* `t` is a TCP stream for the reporter alone. `say` logs a line. */
void report_start(const report_config *c, cdv_tcp *t, void (*say)(const char *));
void report_stop(void);
void report_step(void);

/* For the window: "" when there is no console, else a short state. */
const char *report_status(void);

/* The inventory, for the console and the window. */
void report_describe_cpu(char *out, size_t cap);
void report_describe_memory(char *out, size_t cap);
void report_describe_os(char *out, size_t cap);

#endif
