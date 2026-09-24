#include "dnr.h"
#include "../core/dns.h"

#include <string.h>

#define kOnSystemDisk ((short)0x8000) /* Multiversal does not declare it */

enum { OPENRESOLVER = 1, CLOSERESOLVER = 2, STRTOADDR = 3 };
enum { cacheFault = -23042 };

#if defined(__powerpc__) || defined(__ppc__)
#define CDV_PPC 1
#pragma pack(push, 2)
#else
#define CDV_PPC 0
#endif
/* AddressXlation.h's hostInfo: the result, the canonical name, four addresses. */
typedef struct {
    long rtnCode;
    char cname[255];
    unsigned long addr[4];
} hostInfo;
#if CDV_PPC
#pragma pack(pop)
#endif

static Handle code;    /* the 'dnrp' resource, detached and locked */
static ProcPtr dnr;

/* Lookups in flight. The resolver writes into a slot's hostInfo until it
 * answers, so a slot is not reused before then -- even one its owner has
 * stopped waiting for. */
enum { SLOT_FREE, SLOT_BUSY, SLOT_ABANDONED, SLOT_READY };
#define SLOTS 4
static struct {
    int state;
    volatile hostInfo info;
    char name[256];
    uint32_t ip;   /* SLOT_READY with a dotted quad: no lookup needed */
} slot[SLOTS];

#if CDV_PPC
/* The DNR is 68k code and so is its calling convention: C, stack-based,
 * a long selector first, an OSErr back. */
#define PI_OPEN 0x03E1   /* (long, char *) -> short */
#define PI_STRTOADDR 0xFFE1 /* (long, char *, hostInfo *, proc, char *) -> short */
static UniversalProcPtr done_upp;
static void done(void *hi, char *user)
{
    (void)hi;
    (void)user;
}
#else
/* The result procedure: rtnCode is already set by the time this runs, and
 * the main loop polls that, so there is nothing to do here. */
static pascal void done(void *hi, char *user)
{
    (void)hi;
    (void)user;
}
#endif

static OSErr call_open(void)
{
#if CDV_PPC
    return (OSErr)CallUniversalProc((UniversalProcPtr)dnr, PI_OPEN, (long)OPENRESOLVER, (char *)0);
#else
    typedef OSErr (*open_fn)(long, char *);
    return ((open_fn)dnr)(OPENRESOLVER, 0);
#endif
}

static OSErr call_strtoaddr(char *name, volatile hostInfo *hi)
{
#if CDV_PPC
    return (OSErr)CallUniversalProc((UniversalProcPtr)dnr, PI_STRTOADDR, (long)STRTOADDR, name,
                                    (hostInfo *)hi, done_upp, (char *)0);
#else
    typedef OSErr (*sta_fn)(long, char *, hostInfo *, void *, char *);
    return ((sta_fn)dnr)(STRTOADDR, name, (hostInfo *)hi, (void *)done, 0);
#endif
}

/* 'dnrp' from the System file, or from the MacTCP / TCP/IP control panel in
 * the Control Panels or System Folder. */
static Handle find_dnrp(void)
{
    static const OSType where[2] = { kControlPanelFolderType, kSystemFolderType };
    Handle h = GetIndResource('dnrp', 1);
    int w;
    if (h)
        return h;
    for (w = 0; w < 2; w++) {
        short vref;
        long dir;
        HParamBlockRec pb;
        Str63 name;
        short i;
        if (FindFolder(kOnSystemDisk, where[w], 0, &vref, &dir) != noErr)
            continue;
        for (i = 1; i < 400; i++) {
            memset(&pb, 0, sizeof pb);
            pb.fileParam.ioNamePtr = name;
            pb.fileParam.ioVRefNum = vref;
            pb.fileParam.ioDirID = dir;
            pb.fileParam.ioFDirIndex = i;
            if (PBHGetFInfoSync(&pb) != noErr)
                break;
            if (pb.fileParam.ioFlFndrInfo.fdType == 'cdev' &&
                pb.fileParam.ioFlFndrInfo.fdCreator == 'ztcp') {
                short cur = CurResFile(), ref = HOpenResFile(vref, dir, name, fsRdPerm);
                if (ref == -1)
                    continue;
                UseResFile(ref);
                h = Get1IndResource('dnrp', 1);
                if (h)
                    DetachResource(h);
                UseResFile(cur);
                CloseResFile(ref);
                if (h)
                    return h;
            }
        }
    }
    return NULL;
}

OSErr dnr_open(void)
{
    OSErr err;
    if (dnr)
        return noErr;
    code = find_dnrp();
    if (!code)
        return resNotFound;
    MoveHHi(code);
    HLock(code);
    dnr = (ProcPtr)*code;
#if CDV_PPC
    done_upp = NewRoutineDescriptor((ProcPtr)done, 0x03C0 /* pascal (ptr, ptr) */, 1);
#endif
    err = call_open();
    if (err != noErr)
        dnr = NULL;
    return err;
}

void dnr_close(void)
{
    /* The resolver stays loaded while we run; the System reclaims it at quit
     * with the rest of the heap -- CLOSERESOLVER is only for giving it back
     * early, which nothing here needs. */
}

static int pending(int i)
{
    long r = slot[i].info.rtnCode;
    return r == 1 || r == cacheFault;
}

int dnr_start(const char *name)
{
    int i;
    OSErr err;
    uint32_t ip;
    for (i = 0; i < SLOTS; i++)
        if (slot[i].state == SLOT_ABANDONED && !pending(i))
            slot[i].state = SLOT_FREE;
    for (i = 0; i < SLOTS && slot[i].state != SLOT_FREE; i++)
        ;
    if (i == SLOTS)
        return -1;
    if (dns_parse_ip(name, &ip)) {
        slot[i].ip = ip;
        slot[i].state = SLOT_READY;
        return i;
    }
    if (!dnr && dnr_open() != noErr)
        return -1;
    strncpy(slot[i].name, name, sizeof slot[i].name - 1);
    slot[i].name[sizeof slot[i].name - 1] = 0;
    memset((void *)&slot[i].info, 0, sizeof slot[i].info);
    slot[i].info.rtnCode = 1; /* in progress, until the resolver says otherwise */
    slot[i].ip = 0;
    err = call_strtoaddr(slot[i].name, &slot[i].info);
    if (err != cacheFault) /* answered at once, from the cache -- or refused */
        slot[i].info.rtnCode = err;
    slot[i].state = SLOT_BUSY;
    return i;
}

int dnr_poll(int h, uint32_t *ip)
{
    int ok;
    if (h < 0 || h >= SLOTS)
        return -1;
    if (slot[h].state == SLOT_READY) {
        *ip = slot[h].ip;
        slot[h].state = SLOT_FREE;
        return 1;
    }
    if (slot[h].state != SLOT_BUSY)
        return -1;
    if (pending(h))
        return 0;
    ok = slot[h].info.rtnCode == noErr && slot[h].info.addr[0];
    if (ok)
        *ip = (uint32_t)slot[h].info.addr[0];
    slot[h].state = SLOT_FREE;
    return ok ? 1 : -1;
}

void dnr_forget(int h)
{
    if (h < 0 || h >= SLOTS)
        return;
    if (slot[h].state == SLOT_BUSY && pending(h))
        slot[h].state = SLOT_ABANDONED;
    else
        slot[h].state = SLOT_FREE;
}
