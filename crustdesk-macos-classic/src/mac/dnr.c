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
static volatile hostInfo info;

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

static OSErr call_strtoaddr(char *name)
{
#if CDV_PPC
    return (OSErr)CallUniversalProc((UniversalProcPtr)dnr, PI_STRTOADDR, (long)STRTOADDR, name,
                                    (hostInfo *)&info, done_upp, (char *)0);
#else
    typedef OSErr (*sta_fn)(long, char *, hostInfo *, void *, char *);
    return ((sta_fn)dnr)(STRTOADDR, name, (hostInfo *)&info, (void *)done, 0);
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

int dnr_lookup(const char *name, uint32_t *ip, unsigned long ticks)
{
    char buf[256];
    unsigned long start = TickCount();
    OSErr err;
    if (dns_parse_ip(name, ip))
        return 1;
    if (!dnr && dnr_open() != noErr)
        return 0;
    strncpy(buf, name, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    memset((void *)&info, 0, sizeof info);
    info.rtnCode = 1; /* in progress, until the resolver says otherwise */
    err = call_strtoaddr(buf);
    if (err == cacheFault) {
        while (info.rtnCode == 1 || info.rtnCode == cacheFault) {
            EventRecord ev;
            if (TickCount() - start > ticks)
                return 0;
            WaitNextEvent(0, &ev, 1, NULL); /* let MacTCP and everyone else run */
        }
        err = (OSErr)info.rtnCode;
    }
    if (err != noErr || !info.addr[0])
        return 0;
    *ip = info.addr[0];
    return 1;
}
