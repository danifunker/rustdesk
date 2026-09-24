/* Toolbox calls Multiversal leaves out, one way for 68k and another for
 * PowerPC.
 *
 * On 68k these are inline traps (several are register-based and have no
 * glue); on PowerPC every one is an ordinary InterfaceLib export.
 */
#ifndef CDV_TRAPS_H
#define CDV_TRAPS_H

#include <Multiverse.h>
#include <stdint.h>

#if defined(__powerpc__) || defined(__ppc__)
#define CDV_PPC 1
#else
#define CDV_PPC 0
#endif

OSErr cdv_ppost_event(short what, long message, EvQElPtr *q);
OSErr cdv_dt_install(void *task);
void cdv_ins_xtime(void *task);
uint32_t cdv_microseconds(void);
OSErr cdv_get_front_process(ProcessSerialNumber *psn);
long cdv_gmt_delta(void);
/* Tell the Power Manager someone is here (no sleep, no dimming). */
void cdv_keep_awake(void);
OSErr cdv_fsp_create(const FSSpec *spec, OSType creator, OSType type, short script);

#endif
