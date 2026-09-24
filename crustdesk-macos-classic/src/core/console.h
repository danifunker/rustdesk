/* Reporting in to a console (CortenDesk, RustDesk Pro), so the machine is in
 * its device list -- registering with hbbs only makes it reachable by ID.
 *
 * Two tokenless POSTs, as rustdesk-ppc-agent/src/api.rs does them:
 *
 *   /api/sysinfo    id, uuid, cpu, memory, os, hostname, username, version.
 *                   Creates the device row. Replies in plain text:
 *                   SYSINFO_UPDATED, or ID_NOT_FOUND.
 *   /api/heartbeat  id and uuid, every 15 s (a console calls a device offline
 *                   60 s after its last one). Replies in JSON; a top-level
 *                   "sysinfo" is the console asking for the inventory again.
 *
 * The uuid is sent as base64 and must never change: the console pins the
 * first one it sees for an ID and ignores heartbeats that disagree.
 *
 * This is the protocol only; the platform connects and moves the bytes
 * through https.h.
 */
#ifndef CDV_CONSOLE_H
#define CDV_CONSOLE_H

#include <stddef.h>
#include <stdint.h>

#define CONSOLE_INTERVAL_MS 15000UL

typedef struct {
    int secure;
    char host[64];
    uint16_t port;
    char prefix[64]; /* a console under a path: "/rustdesk", or "" */
} console_url;

/* "https://host[:port][/prefix]", "http://...", or a bare host (https: the
 * safe guess, as the PowerPC agent makes it). 0 if it is not a URL. */
int console_parse_url(const char *s, console_url *u);

/* The request bodies. Names arrive in Mac Roman and leave as JSON strings
 * in UTF-8. Return the length (0 if it did not fit). */
size_t console_sysinfo_json(char *out, size_t cap, const char *id, const uint8_t uuid[16],
                            const char *cpu, const char *memory, const char *os,
                            const char *hostname, const char *username);
size_t console_heartbeat_json(char *out, size_t cap, const char *id, const uint8_t uuid[16]);

/* Does a heartbeat reply ask for the inventory? Only a top-level "sysinfo"
 * that is not false, null, 0 or "" does ("strategy":{"sysinfo":1} does not). */
int console_wants_sysinfo(const char *json);

void console_base64(const uint8_t *in, size_t n, char *out);

#endif
