#include "traps.h"

#if CDV_PPC

pascal OSErr DTInstall(void *dtTaskPtr);
pascal void InsXTime(QElemPtr tmTaskPtr);
pascal void Microseconds(void *microTickCount);

pascal OSErr GetFrontProcess(ProcessSerialNumber *psn);
pascal void ReadLocation(void *loc);
pascal long IdleUpdate(void);

OSErr cdv_ppost_event(short what, long message, EvQElPtr *q)
{
    return PPostEvent(what, message, q);
}

OSErr cdv_get_front_process(ProcessSerialNumber *psn)
{
    return GetFrontProcess(psn);
}

OSErr cdv_dt_install(void *task)
{
    return DTInstall(task);
}

void cdv_ins_xtime(void *task)
{
    InsXTime((QElemPtr)task);
}

uint32_t cdv_microseconds(void)
{
    uint32_t t[2];
    Microseconds(t);
    return t[1];
}

static void read_location(long loc[3])
{
    ReadLocation(loc);
}

static void idle_update(void)
{
    IdleUpdate();
}

#else

/* _PPostEvent is register-based and Multiversal gives it no glue: the event
 * code goes in A0 and the message in D0 (Events.h: `#pragma parameter __D0
 * PPostEvent(__A0, __D0, __A1)`); A0 comes back as the queue element. The
 * other way round posts every event as a null event, successfully. */
OSErr cdv_ppost_event(short what, long message, EvQElPtr *q)
{
    register long a0 __asm__("a0") = what;
    register long d0 __asm__("d0") = message;
    __asm__ volatile(".short 0xA12F" : "+d"(d0), "+a"(a0) : : "d1", "d2", "a1", "cc", "memory");
    *q = (EvQElPtr)a0;
    return (OSErr)d0;
}

OSErr cdv_dt_install(void *task)
{
    register long a0 __asm__("a0") = (long)task;
    register long d0 __asm__("d0");
    __asm__ volatile(".short 0xA082" : "=d"(d0), "+a"(a0) : : "d1", "d2", "a1", "cc", "memory");
    return (OSErr)d0;
}

void cdv_ins_xtime(void *task)
{
    register long a0 __asm__("a0") = (long)task;
    __asm__ volatile(".short 0xA458" : "+a"(a0) : : "d0", "d1", "d2", "a1", "cc", "memory");
}

/* _OSDispatch selector 0x39 -- which also wants a longword of -1 pushed
 * before the selector (Processes.h: FIVEWORDINLINE(0x70FF, 0x2F00, 0x3F3C,
 * 0x0039, 0xA88F)). Without it the stack comes back unbalanced: a bus error. */
static pascal OSErr get_front_process(ProcessSerialNumber *psn)
    M68K_INLINE(0x70FF, 0x2F00, 0x3F3C, 0x0039, 0xA88F);

OSErr cdv_get_front_process(ProcessSerialNumber *psn)
{
    return get_front_process(psn);
}

/* _Microseconds: A0 high word, D0 low word. */
uint32_t cdv_microseconds(void)
{
    register long d0 __asm__("d0");
    register long a0 __asm__("a0");
    __asm__ volatile(".short 0xA193" : "=d"(d0), "=a"(a0) : : "d1", "d2", "a1", "cc", "memory");
    (void)a0;
    return (uint32_t)d0;
}

/* _IdleUpdate: the result in D0, nothing in. */
static void idle_update(void)
{
    register long d0 __asm__("d0");
    __asm__ volatile(".short 0xA285" : "=d"(d0) : : "d1", "d2", "a0", "a1", "cc", "memory");
    (void)d0;
}

/* _ReadLocation is _ReadXPRam of 12 bytes at 0xE4: A0 the record, D0 the
 * length and address. */
static pascal void read_location(long loc[3])
    M68K_INLINE(0x205F, 0x203C, 0x000C, 0x00E4, 0xA051);

#endif

/* The Power Manager's idle timer, reset: what an ADB keypress does, so
 * neither sleep nor dimming comes while it is kept up. Macs without a Power
 * Manager have no _IdleUpdate, and calling an unimplemented trap is a
 * crash: look first. */
void cdv_keep_awake(void)
{
    static int avail = -1;
    if (avail < 0)
        avail = NGetTrapAddress(0xA285, kOSTrapType) != NGetTrapAddress(0xA89F, kToolboxTrapType);
    if (avail)
        idle_update();
}

/* Seconds east of GMT, from the Map control panel's setting. The low 24
 * bits of the third longword, sign-extended; the top byte is daylight saving. */
long cdv_gmt_delta(void)
{
    long loc[3] = { 0, 0, 0 };
    long d;
    read_location(loc);
    d = loc[2] & 0x00FFFFFFL;
    if (d & 0x00800000L)
        d |= (long)0xFF000000UL;
    return d;
}

OSErr cdv_fsp_create(const FSSpec *spec, OSType creator, OSType type, short script)
{
    return FSpCreate((FSSpecPtr)spec, creator, type, script);
}
