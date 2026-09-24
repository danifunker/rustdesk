/* Names to addresses with the Mac's own resolver: MacTCP's DNR, which Open
 * Transport also provides. It uses the DNS servers set in the MacTCP or
 * TCP/IP control panel, which is what the user expects and what a DNS client
 * of our own could only guess at.
 *
 * The resolver is a 68k code resource ('dnrp') in the System file or in the
 * MacTCP / TCP/IP control panel (creator 'ztcp'), called with a selector: the
 * interface of Apple's dnr.c, reached directly on 68k and through Mixed Mode
 * on PowerPC. Main loop only: it loads a resource and waits for the answer.
 */
#ifndef CDV_DNR_H
#define CDV_DNR_H

#include <Multiverse.h>
#include <stdint.h>

/* Load and open the resolver. noErr, or why not. */
OSErr dnr_open(void);
void dnr_close(void);

/* Lookups that never wait: start one, then poll it from the main loop.
 * dnr_start: a handle (a dotted quad is answered at once), or -1 if there is
 * no resolver or too many lookups are out. dnr_poll: 0 still going, 1 with
 * *ip set, -1 failed; either answer ends the lookup. dnr_forget: stop
 * waiting (the resolver may still answer; the slot is kept until it does).
 * Blocking here would stop the whole Mac -- with no network, for as long as
 * the resolver takes to give up. */
int dnr_start(const char *name);
int dnr_poll(int h, uint32_t *ip);
void dnr_forget(int h);

#endif
