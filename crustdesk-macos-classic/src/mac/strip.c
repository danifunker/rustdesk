#include "strip.h"

#include <string.h>

static cdv_strip *B;

void strip_publish(void)
{
    long v = 0;
    ProcessInfoRec info;
    if (Gestalt(STRIP_SELECTOR, &v) == noErr && v &&
        ((cdv_strip *)v)->magic == STRIP_MAGIC) {
        B = (cdv_strip *)v; /* an earlier run made it */
    } else {
        uint32_t a;
        B = (cdv_strip *)NewPtrSysClear(sizeof *B);
        if (!B)
            return;
        a = (uint32_t)B;
        /* pascal OSErr f(OSType selector, long *response): *response = B. */
        B->code[0] = 0x206F; B->code[1] = 0x0004;         /* movea.l 4(sp),a0 */
        B->code[2] = 0x20BC;                               /* move.l #B,(a0) */
        B->code[3] = (uint16_t)(a >> 16); B->code[4] = (uint16_t)a;
        B->code[5] = 0x426F; B->code[6] = 0x000C;         /* clr.w 12(sp): noErr */
        B->code[7] = 0x205F;                               /* movea.l (sp)+,a0 */
        B->code[8] = 0x508F;                               /* addq.l #8,sp */
        B->code[9] = 0x4ED0;                               /* jmp (a0) */
        B->magic = STRIP_MAGIC;
#if !(defined(__powerpc__) || defined(__ppc__))
        FlushCodeCache(); /* written as data, run as code: the 68040's caches */
#endif
        /* (On PowerPC the 68k emulator has never seen this block, so it has
         * no stale translation of it to flush.) */
        /* On PowerPC a plain address is a 68k routine to Mixed Mode. */
        if (NewGestalt(STRIP_SELECTOR, (SelectorFunctionUPP)B->code) != noErr) {
            B = NULL; /* left in the system heap: a few dozen bytes */
            return;
        }
    }
    memset(&info, 0, sizeof info);
    info.processInfoLength = sizeof info;
    info.processAppSpec = &B->app;
    GetCurrentProcess(&B->psn);
    GetProcessInformation(&B->psn, &info);
    B->cmd = STRIP_CMD_NONE;
    B->app_running = 1;
    B->changes++;
}

void strip_update(int sharing, int live, int registered, int listed, const char *id)
{
    if (!B)
        return;
    if (B->sharing != sharing || B->live != live || B->registered != registered ||
        B->listed != listed || strncmp(B->id, id, sizeof B->id)) {
        B->sharing = (uint8_t)sharing;
        B->live = (uint8_t)live;
        B->registered = (uint8_t)registered;
        B->listed = (uint8_t)listed;
        strncpy(B->id, id, sizeof B->id - 1);
        B->changes++;
    }
}

int strip_command(void)
{
    int c;
    if (!B || !B->cmd)
        return STRIP_CMD_NONE;
    c = B->cmd;
    B->cmd = STRIP_CMD_NONE;
    return c;
}

void strip_quit(void)
{
    if (!B)
        return;
    B->app_running = 0;
    B->sharing = B->live = B->registered = B->listed = 0;
    B->changes++;
}
