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

/* Resolve `name` (or parse it, if it is a dotted quad), waiting at most
 * `ticks`. 1 and *ip set on success, 0 if not. */
int dnr_lookup(const char *name, uint32_t *ip, unsigned long ticks);

#endif
