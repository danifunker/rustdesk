/* Install C-Desk-Vint: copies the application from the installer's own folder
 * into a "C-Desk-Vint" folder in Applications (the startup disk, before Mac
 * OS 8 has one), and, as the user chooses, puts an alias of it in Startup
 * Items and the Control Strip module in Control Strip Modules. Unticking a
 * box on a later run takes that piece away again. The prefs file is left
 * alone: an update keeps the ID, keys and password.
 *
 * 68k only: PowerPC Macs run it in their emulator. File Manager calls are
 * the PB forms, which Multiversal has (it has none of the FSp ones).
 */
#include <Multiverse.h>
#include <string.h>

#define kOnSystemDisk ((short)0x8000)
#define kApplicationsFolder 'apps'
#define kStripModulesFolder 'sdev'
#define kIsAlias 0x8000
#define kHasBeenInited 0x0100

enum { D_INSTALL = 1, D_QUIT, D_WHERE, D_STARTUP, D_STRIP, D_NOTE };
enum { DLG_MAIN = 300, ALRT_DONE = 301, ALRT_ERROR = 302 };

static const unsigned char APP_NAME[] = "\pC-Desk-Vint";
static const unsigned char STRIP_NAME[] = "\pC-Desk-Vint Strip";
static const unsigned char ALIAS_NAME[] = "\pC-Desk-Vint alias";

static void pcopy(unsigned char *d, const unsigned char *s)
{
    memcpy(d, s, s[0] + 1);
}

static void pcat(unsigned char *d, const unsigned char *s)
{
    int n = s[0];
    if (d[0] + n > 255)
        n = 255 - d[0];
    memcpy(d + 1 + d[0], s + 1, n);
    d[0] += n;
}

static void pcatc(unsigned char *d, const char *c)
{
    unsigned char p[256];
    size_t n = strlen(c);
    p[0] = (unsigned char)n;
    memcpy(p + 1, c, n);
    pcat(d, p);
}

static void fail(const char *what, OSErr err)
{
    unsigned char a[256], b[32];
    long e = err;
    int neg = e < 0, i = 0;
    char digits[12];
    a[0] = 0;
    pcatc(a, what);
    if (neg)
        e = -e;
    do
        digits[i++] = (char)('0' + e % 10);
    while ((e /= 10) != 0);
    b[0] = 0;
    pcatc(b, neg ? " (error -" : " (error ");
    while (i) {
        char c[2] = { digits[--i], 0 };
        pcatc(b, c);
    }
    pcatc(b, ")");
    pcat(a, b);
    ParamText(a, "\p", "\p", "\p");
    StopAlert(ALRT_ERROR, NULL);
}

/* ---- files --------------------------------------------------------------------- */

static OSErr file_info(short vref, long dir, const unsigned char *name, FInfo *fi)
{
    HParamBlockRec pb;
    OSErr err;
    memset(&pb, 0, sizeof pb);
    pb.fileParam.ioNamePtr = (StringPtr)name;
    pb.fileParam.ioVRefNum = vref;
    pb.fileParam.ioDirID = dir;
    err = PBHGetFInfoSync(&pb);
    if (err == noErr && fi)
        *fi = pb.fileParam.ioFlFndrInfo;
    return err;
}

static OSErr set_file_info(short vref, long dir, const unsigned char *name, const FInfo *fi)
{
    HParamBlockRec pb;
    OSErr err;
    memset(&pb, 0, sizeof pb);
    pb.fileParam.ioNamePtr = (StringPtr)name;
    pb.fileParam.ioVRefNum = vref;
    pb.fileParam.ioDirID = dir;
    err = PBHGetFInfoSync(&pb); /* for the dates, which SetFInfo also writes */
    if (err != noErr)
        return err;
    pb.fileParam.ioFlFndrInfo = *fi;
    pb.fileParam.ioDirID = dir;
    return PBHSetFInfoSync(&pb);
}

static OSErr remove_file(short vref, long dir, const unsigned char *name)
{
    HParamBlockRec pb;
    OSErr err;
    memset(&pb, 0, sizeof pb);
    pb.fileParam.ioNamePtr = (StringPtr)name;
    pb.fileParam.ioVRefNum = vref;
    pb.fileParam.ioDirID = dir;
    err = PBHDeleteSync(&pb);
    return err == fnfErr ? noErr : err;
}

static OSErr open_fork(short vref, long dir, const unsigned char *name, int rsrc, SInt8 perm,
                       short *ref)
{
    HParamBlockRec pb;
    OSErr err;
    memset(&pb, 0, sizeof pb);
    pb.ioParam.ioNamePtr = (StringPtr)name;
    pb.ioParam.ioVRefNum = vref;
    pb.fileParam.ioDirID = dir;
    pb.ioParam.ioPermssn = perm;
    err = rsrc ? PBHOpenRFSync(&pb) : PBHOpenDFSync(&pb);
    *ref = pb.ioParam.ioRefNum;
    return err;
}

static OSErr copy_fork(short vs, long ds, short vd, long dd, const unsigned char *name, int rsrc)
{
    static char buf[16384];
    short in, out;
    OSErr err = open_fork(vs, ds, name, rsrc, fsRdPerm, &in);
    if (err != noErr)
        return err;
    err = open_fork(vd, dd, name, rsrc, fsWrPerm, &out);
    if (err != noErr) {
        FSClose(in);
        return err;
    }
    for (;;) {
        long n = sizeof buf;
        OSErr r = FSRead(in, &n, buf);
        if (n > 0 && (err = FSWrite(out, &n, buf)) != noErr)
            break;
        if (r == eofErr) {
            err = noErr;
            break;
        }
        if (r != noErr) {
            err = r;
            break;
        }
    }
    FSClose(in);
    FSClose(out);
    return err;
}

/* Copy a file, both forks and its Finder info, replacing one of the same name. */
static OSErr copy_file(short vs, long ds, short vd, long dd, const unsigned char *name)
{
    HParamBlockRec pb;
    FInfo fi;
    OSErr err = file_info(vs, ds, name, &fi);
    if (err != noErr)
        return err;
    err = remove_file(vd, dd, name);
    if (err != noErr)
        return err; /* fBsyErr: it is running */
    memset(&pb, 0, sizeof pb);
    pb.fileParam.ioNamePtr = (StringPtr)name;
    pb.fileParam.ioVRefNum = vd;
    pb.fileParam.ioDirID = dd;
    if ((err = PBHCreateSync(&pb)) != noErr)
        return err;
    if ((err = copy_fork(vs, ds, vd, dd, name, 0)) != noErr ||
        (err = copy_fork(vs, ds, vd, dd, name, 1)) != noErr)
        return err;
    fi.fdFlags &= ~kHasBeenInited; /* so the Finder reads its bundle */
    fi.fdLocation.h = fi.fdLocation.v = 0;
    fi.fdFldr = 0;
    return set_file_info(vd, dd, name, &fi);
}

static OSErr folder(short vref, long parent, const unsigned char *name, long *dir)
{
    HParamBlockRec pb;
    CInfoPBRec cpb;
    OSErr err;
    memset(&pb, 0, sizeof pb);
    pb.fileParam.ioNamePtr = (StringPtr)name;
    pb.fileParam.ioVRefNum = vref;
    pb.fileParam.ioDirID = parent;
    err = PBDirCreateSync(&pb);
    if (err == noErr) {
        *dir = pb.fileParam.ioDirID;
        return noErr;
    }
    if (err != dupFNErr)
        return err;
    memset(&cpb, 0, sizeof cpb);
    cpb.dirInfo.ioNamePtr = (StringPtr)name;
    cpb.dirInfo.ioVRefNum = vref;
    cpb.dirInfo.ioDrDirID = parent;
    err = PBGetCatInfoSync(&cpb);
    if (err == noErr && !(cpb.dirInfo.ioFlAttrib & 0x10))
        return dupFNErr; /* a file by that name */
    *dir = cpb.dirInfo.ioDrDirID;
    return err;
}

/* An alias file to `target`, as the Finder's Make Alias makes one. */
static OSErr make_alias(const FSSpec *target, short vref, long dir, const unsigned char *name)
{
    AliasHandle alias = NULL;
    FInfo fi;
    short ref;
    OSErr err = remove_file(vref, dir, name);
    if (err != noErr)
        return err;
    if ((err = NewAlias(NULL, (FSSpec *)target, &alias)) != noErr)
        return err;
    HCreateResFile(vref, dir, (ConstStr255Param)name);
    if ((err = ResError()) != noErr)
        return err;
    ref = HOpenResFile(vref, dir, (ConstStr255Param)name, fsRdWrPerm);
    if (ref == -1)
        return ResError();
    AddResource((Handle)alias, 'alis', 0, "\p");
    err = ResError();
    CloseResFile(ref);
    if (err != noErr)
        return err;
    memset(&fi, 0, sizeof fi);
    fi.fdType = 'adrp'; /* an alias to an application */
    fi.fdCreator = 'CDVt';
    fi.fdFlags = kIsAlias;
    return set_file_info(vref, dir, name, &fi);
}

/* ---- the dialog ------------------------------------------------------------------ */

static int check(DialogPtr d, short item, int set)
{
    short type;
    Handle h;
    Rect r;
    GetDialogItem(d, item, &type, &h, &r);
    if (set >= 0)
        SetControlValue((ControlHandle)h, set);
    return GetControlValue((ControlHandle)h);
}

static void disable(DialogPtr d, short item)
{
    short type;
    Handle h;
    Rect r;
    GetDialogItem(d, item, &type, &h, &r);
    HiliteControl((ControlHandle)h, 255);
}

int main(void)
{
    ProcessSerialNumber psn;
    ProcessInfoRec info;
    FSSpec me, app;
    FInfo fi;
    DialogPtr d;
    short item, vol, s_vol, sdev_vol;
    long apps, dir, startup, sdev_dir, v;
    unsigned char where[256], note[256], volname[64];
    int has_strip, has_startup;
    HParamBlockRec vpb;
    OSErr err;

    MaxApplZone();
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();

    /* Where the installer is: the application to install is beside it. */
    memset(&info, 0, sizeof info);
    info.processInfoLength = sizeof info;
    info.processAppSpec = &me;
    GetCurrentProcess(&psn);
    GetProcessInformation(&psn, &info);
    if (file_info(me.vRefNum, me.parID, APP_NAME, &fi) != noErr || fi.fdType != 'APPL') {
        ParamText("\pC-Desk-Vint is not in the installer's folder. Keep the two together "
                  "(and \"C-Desk-Vint Strip\" with them) and try again.",
                  "\p", "\p", "\p");
        StopAlert(ALRT_ERROR, NULL);
        return 0;
    }

    /* Where it goes: Applications, or the startup disk before Mac OS 8. */
    if (FindFolder(kOnSystemDisk, kApplicationsFolder, 1, &vol, &apps) != noErr) {
        FindFolder(kOnSystemDisk, kSystemFolderType, 0, &vol, &dir);
        apps = 2; /* the root */
    }
    memset(&vpb, 0, sizeof vpb);
    volname[0] = 0;
    vpb.volumeParam.ioNamePtr = volname;
    vpb.volumeParam.ioVRefNum = vol;
    PBHGetVInfoSync(&vpb);
    pcopy(where, volname);
    if (apps != 2) {
        CInfoPBRec cpb;
        unsigned char fname[64];
        memset(&cpb, 0, sizeof cpb);
        fname[0] = 0;
        cpb.dirInfo.ioNamePtr = fname;
        cpb.dirInfo.ioVRefNum = vol;
        cpb.dirInfo.ioDrDirID = apps;
        cpb.dirInfo.ioFDirIndex = -1;
        if (PBGetCatInfoSync(&cpb) == noErr) {
            pcatc(where, ":");
            pcat(where, fname);
        }
    }
    pcatc(where, ":C-Desk-Vint");

    has_startup = FindFolder(kOnSystemDisk, kStartupFolderType, 1, &s_vol, &startup) == noErr;
    /* The Control Strip answers Gestalt 'sdev'; Classic under Mac OS X has
     * none to show, whatever the folder says. */
    has_strip = Gestalt('sdev', &v) == noErr && Gestalt('bbox', &v) != noErr &&
                FindFolder(kOnSystemDisk, kStripModulesFolder, 1, &sdev_vol, &sdev_dir) == noErr &&
                file_info(me.vRefNum, me.parID, STRIP_NAME, NULL) == noErr;
    note[0] = 0;
    if (!has_strip)
        pcatc(note, Gestalt('bbox', &v) == noErr
                        ? "Classic under Mac OS X has no Control Strip. "
                        : "This Mac has no Control Strip (or its module is missing here). ");
    pcatc(note, "Settings, the ID and the password are kept when you update.");

    ParamText(where, note, "\p", "\p");
    d = GetNewDialog(DLG_MAIN, NULL, (WindowPtr)-1);
    if (!d)
        return 0;
    SetDialogDefaultItem(d, D_INSTALL);
    SetDialogCancelItem(d, D_QUIT);
    check(d, D_STARTUP, has_startup);
    check(d, D_STRIP, has_strip);
    if (!has_startup)
        disable(d, D_STARTUP);
    if (!has_strip)
        disable(d, D_STRIP);
    ShowWindow(d);
    for (;;) {
        ModalDialog(NULL, &item);
        if (item == D_STARTUP || item == D_STRIP)
            check(d, item, !check(d, item, -1));
        if (item == D_INSTALL || item == D_QUIT)
            break;
    }
    if (item == D_QUIT)
        return 0;
    {
        int want_startup = has_startup && check(d, D_STARTUP, -1);
        int want_strip = has_strip && check(d, D_STRIP, -1);
        DisposeDialog(d);
        SetCursor(*GetCursor(watchCursor));

        if ((err = folder(vol, apps, APP_NAME, &dir)) != noErr) {
            fail("Could not make the C-Desk-Vint folder.", err);
            return 0;
        }
        err = copy_file(me.vRefNum, me.parID, vol, dir, APP_NAME);
        if (err == fBsyErr) {
            fail("C-Desk-Vint is open. Quit it, then install again.", err);
            return 0;
        }
        if (err != noErr) {
            fail("Could not copy C-Desk-Vint.", err);
            return 0;
        }
        FSMakeFSSpec(vol, dir, APP_NAME, &app);
        if (has_startup) {
            err = want_startup ? make_alias(&app, s_vol, startup, ALIAS_NAME)
                               : remove_file(s_vol, startup, ALIAS_NAME);
            if (err != noErr)
                fail("Could not change Startup Items.", err);
        }
        if (has_strip) {
            err = want_strip ? copy_file(me.vRefNum, me.parID, sdev_vol, sdev_dir, STRIP_NAME)
                             : remove_file(sdev_vol, sdev_dir, STRIP_NAME);
            if (err != noErr)
                fail("Could not change the Control Strip modules.", err);
        }
        InitCursor();
        note[0] = 0;
        if (want_startup)
            pcatc(note, " It will start when this Mac starts up.");
        if (want_strip)
            pcatc(note, " Its Control Strip module appears after a restart.");
        ParamText(where, note, "\p", "\p");
        if (Alert(ALRT_DONE, NULL) == 1) {
            LaunchParamBlockRec lp;
            memset(&lp, 0, sizeof lp);
            lp.launchBlockID = extendedBlock;
            lp.launchEPBLength = extendedBlockLen;
            lp.launchControlFlags = launchContinue | 0x0800; /* launchNoFileFlags */
            lp.launchAppSpec = &app;
            LaunchApplication(&lp);
        }
    }
    return 0;
}
