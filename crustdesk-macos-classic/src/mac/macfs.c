/* The Mac's disks for the file-transfer protocol (core/files.h), through the
 * File Manager's PB calls. Main loop only.
 *
 * Paths are the client's: UTF-8, '/' between parts. "/" is the mounted
 * disks, "/Disk Name/Folder/File" what it looks like. A Mac name may hold a
 * '/', which could not go in a path: it shows as ':', which a Mac name
 * cannot hold.
 *
 * Resource forks: a file with one is listed as "Name.bin" and read as
 * MacBinary (header, data fork, resource fork), so it survives a stay on a
 * modern disk. An upload named .bin that really is MacBinary, or any .hqx
 * (BinHex), becomes the Mac file it holds; anything else becomes a plain
 * file, given a type and creator its extension suggests.
 */
#include "macfs.h"

#include "traps.h"
#include "../core/macbin.h"
#include "../core/macroman.h"

#include <Multiverse.h>
#include <stdio.h>
#include <string.h>

#define kHasBeenInited 0x0100
#define kIsInvisible 0x4000
#define MAC_EPOCH_TO_UNIX 2082844800UL

static char err[96];

static int fail(OSErr e, const char *what)
{
    const char *why;
    switch (e) {
    case fnfErr: case dirNFErr: why = "not found"; break;
    case dupFNErr: why = "already exists"; break;
    case fLckdErr: why = "locked"; break;
    case vLckdErr: case wPrErr: why = "the disk is locked"; break;
    case dskFulErr: why = "the disk is full"; break;
    case fBsyErr: why = "in use"; break;
    case bdNamErr: why = "not a name the Mac takes"; break;
    default: why = NULL;
    }
    if (why)
        snprintf(err, sizeof err, "%s: %s", what, why);
    else
        snprintf(err, sizeof err, "%s: error %d", what, e);
    return -1;
}

/* ---- names and dates --------------------------------------------------------------- */

/* One part of a client path to a Mac name (Pascal string, at most 31). */
static void to_mac(const char *utf8, size_t n, unsigned char *p)
{
    uint8_t mr[256];
    size_t k = utf8_to_macroman((const uint8_t *)utf8, n, mr, sizeof mr), i;
    for (i = 0; i < k; i++)
        if (mr[i] == ':')
            mr[i] = '/';
    if (k > 31) {
        /* Keep the extension, the part that says what it is. */
        size_t dot = k;
        while (dot > 0 && mr[dot - 1] != '.')
            dot--;
        if (dot > 0 && k - dot + 1 <= 8) {
            size_t ext = k - dot + 1;
            memmove(mr + 31 - ext, mr + dot - 1, ext);
        }
        k = 31;
    }
    p[0] = (unsigned char)k;
    memcpy(p + 1, mr, k);
}

static void from_mac(const unsigned char *p, char *utf8, size_t cap)
{
    uint8_t mr[64];
    size_t i, n = p[0] < sizeof mr ? p[0] : sizeof mr - 1, k;
    memcpy(mr, p + 1, n);
    for (i = 0; i < n; i++)
        if (mr[i] == '/')
            mr[i] = ':';
    k = macroman_to_utf8(mr, n, (uint8_t *)utf8, cap - 1);
    utf8[k] = 0;
}

static uint64_t to_unix(unsigned long mac)
{
    long gmt = cdv_gmt_delta();
    if (mac < MAC_EPOCH_TO_UNIX + (unsigned long)(gmt > 0 ? gmt : 0))
        return 0;
    return (uint64_t)(mac - MAC_EPOCH_TO_UNIX - (unsigned long)gmt);
}

static unsigned long to_mac_date(uint64_t unix_secs)
{
    return (unsigned long)unix_secs + MAC_EPOCH_TO_UNIX + (unsigned long)cdv_gmt_delta();
}

static int ends_with(const char *s, size_t n, const char *ext)
{
    size_t k = strlen(ext), i;
    if (n < k)
        return 0;
    for (i = 0; i < k; i++) {
        char c = s[n - k + i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c + 32);
        if (c != ext[i])
            return 0;
    }
    return 1;
}

/* ---- finding things ------------------------------------------------------------------ */

enum { L_ROOT, L_VOL, L_DIR, L_FILE, L_MISSING };

typedef struct {
    int kind;
    short vref;
    long dir;            /* the folder (L_VOL, L_DIR), or the one it is in */
    Str63 name;          /* L_FILE, L_DIR, L_MISSING: its name in `dir` */
    CInfoPBRec info;     /* L_FILE, L_DIR */
    int virt_bin;        /* named Name.bin for its resource fork */
} loc;

static OSErr cat_info(short vref, long dir, const unsigned char *name, CInfoPBRec *pb)
{
    static Str63 nm;
    memset(pb, 0, sizeof *pb);
    memcpy(nm, name, name[0] + 1);
    pb->hFileInfo.ioNamePtr = nm;
    pb->hFileInfo.ioVRefNum = vref;
    pb->hFileInfo.ioDirID = dir;
    return PBGetCatInfoSync(pb);
}

static int find_volume(const char *utf8, size_t n, short *vref)
{
    Str63 want, have;
    HParamBlockRec pb;
    short i;
    to_mac(utf8, n, want);
    for (i = 1; i < 64; i++) {
        memset(&pb, 0, sizeof pb);
        have[0] = 0;
        pb.volumeParam.ioNamePtr = have;
        pb.volumeParam.ioVolIndex = i;
        if (PBHGetVInfoSync(&pb) != noErr)
            break;
        if (EqualString(want, have, false, true)) {
            *vref = pb.volumeParam.ioVRefNum;
            return 1;
        }
    }
    return 0;
}

/* Where a client path leads. -1 (with err) if it goes nowhere; L_MISSING
 * if all but the last part exist. */
static int resolve(const char *path, loc *L)
{
    const char *p = path, *e;
    memset(L, 0, sizeof *L);
    while (*p == '/')
        p++;
    if (!*p || !strcmp(path, "~")) {
        L->kind = L_ROOT;
        return 0;
    }
    e = p + strcspn(p, "/");
    if (!find_volume(p, (size_t)(e - p), &L->vref)) {
        snprintf(err, sizeof err, "no disk called that");
        return -1;
    }
    L->kind = L_VOL;
    L->dir = 2; /* a volume's root folder */
    for (p = e; *p;) {
        size_t n;
        while (*p == '/')
            p++;
        if (!*p)
            break;
        e = p + strcspn(p, "/");
        n = (size_t)(e - p);
        if (L->kind == L_FILE || L->kind == L_MISSING)
            return fail(dirNFErr, "folder");
        to_mac(p, n, L->name);
        if (cat_info(L->vref, L->dir, L->name, &L->info) == noErr) {
            if (L->info.hFileInfo.ioFlAttrib & 0x10) {
                L->dir = L->info.dirInfo.ioDrDirID;
                L->kind = L_DIR;
            } else {
                L->kind = L_FILE;
            }
        } else if (ends_with(p, n, ".bin")) {
            /* The MacBinary face of a file with a resource fork. */
            to_mac(p, n - 4, L->name);
            if (cat_info(L->vref, L->dir, L->name, &L->info) == noErr &&
                !(L->info.hFileInfo.ioFlAttrib & 0x10) && L->info.hFileInfo.ioFlRLgLen > 0) {
                L->kind = L_FILE;
                L->virt_bin = 1;
            } else {
                to_mac(p, n, L->name);
                L->kind = L_MISSING;
            }
        } else {
            L->kind = L_MISSING;
        }
        p = e;
    }
    return 0;
}

static void entry_of(const CInfoPBRec *pb, cdv_entry *e)
{
    memset(e, 0, sizeof *e);
    from_mac(pb->hFileInfo.ioNamePtr, e->name, sizeof e->name);
    if (pb->hFileInfo.ioFlAttrib & 0x10) {
        e->type = ENT_DIR;
        e->hidden = (pb->dirInfo.ioDrUsrWds.frFlags & kIsInvisible) != 0;
        e->mtime = to_unix(pb->dirInfo.ioDrMdDat);
    } else {
        unsigned long d = (unsigned long)pb->hFileInfo.ioFlLgLen;
        unsigned long r = (unsigned long)pb->hFileInfo.ioFlRLgLen;
        e->type = ENT_FILE;
        e->hidden = (pb->hFileInfo.ioFlFndrInfo.fdFlags & kIsInvisible) != 0;
        e->mtime = to_unix(pb->hFileInfo.ioFlMdDat);
        if (r) {
            e->size = macbin_size((uint32_t)d, (uint32_t)r);
            strncat(e->name, ".bin", sizeof e->name - strlen(e->name) - 1);
        } else {
            e->size = d;
        }
    }
}

/* ---- listing --------------------------------------------------------------------------- */

static int m_list(void *u, const char *path, int hidden, char *real, size_t cap,
                  void (*each)(void *ctx, const cdv_entry *e), void *ctx)
{
    loc L;
    short i;
    (void)u;
    if (resolve(path, &L) < 0)
        return -1;
    snprintf(real, cap, "%s", L.kind == L_ROOT ? "/" : path);
    if (L.kind == L_ROOT) {
        for (i = 1; i < 64; i++) {
            HParamBlockRec pb;
            Str63 name;
            cdv_entry e;
            memset(&pb, 0, sizeof pb);
            name[0] = 0;
            pb.volumeParam.ioNamePtr = name;
            pb.volumeParam.ioVolIndex = i;
            if (PBHGetVInfoSync(&pb) != noErr)
                break;
            memset(&e, 0, sizeof e);
            e.type = ENT_DRIVE;
            from_mac(name, e.name, sizeof e.name);
            e.mtime = to_unix(pb.volumeParam.ioVLsMod);
            each(ctx, &e);
        }
        return 0;
    }
    if (L.kind != L_VOL && L.kind != L_DIR)
        return fail(dirNFErr, "folder");
    for (i = 1; i < 32767; i++) {
        CInfoPBRec pb;
        Str63 name;
        cdv_entry e;
        memset(&pb, 0, sizeof pb);
        name[0] = 0;
        pb.hFileInfo.ioNamePtr = name;
        pb.hFileInfo.ioVRefNum = L.vref;
        pb.hFileInfo.ioDirID = L.dir;
        pb.hFileInfo.ioFDirIndex = i;
        if (PBGetCatInfoSync(&pb) != noErr)
            break;
        entry_of(&pb, &e);
        if (e.hidden && !hidden)
            continue;
        each(ctx, &e);
    }
    return 0;
}

static int m_stat(void *u, const char *path, cdv_entry *e)
{
    loc L;
    (void)u;
    if (resolve(path, &L) < 0)
        return -1;
    memset(e, 0, sizeof *e);
    switch (L.kind) {
    case L_ROOT:
    case L_VOL:
        e->type = ENT_DIR;
        return 0;
    case L_DIR:
    case L_FILE:
        entry_of(&L.info, e);
        if (L.kind == L_FILE && !L.virt_bin && L.info.hFileInfo.ioFlRLgLen) {
            /* Asked for by its own name: the data fork alone. */
            e->size = (uint64_t)L.info.hFileInfo.ioFlLgLen;
        }
        return 0;
    default:
        return fail(fnfErr, "file");
    }
}

/* ---- reading: a data fork, or MacBinary ------------------------------------------------ */

enum { RD_HDR, RD_DATA, RD_DPAD, RD_RSRC, RD_RPAD, RD_END };

static struct {
    int macbin, phase;
    short dref, rref;
    uint8_t hdr[128];
    uint32_t left, pos;
} R;

static OSErr open_fork(short vref, long dir, const unsigned char *name, int rsrc, SInt8 perm,
                       short *ref)
{
    HParamBlockRec pb;
    OSErr e;
    memset(&pb, 0, sizeof pb);
    pb.ioParam.ioNamePtr = (StringPtr)name;
    pb.ioParam.ioVRefNum = vref;
    pb.fileParam.ioDirID = dir;
    pb.ioParam.ioPermssn = perm;
    e = rsrc ? PBHOpenRFSync(&pb) : PBHOpenDFSync(&pb);
    *ref = pb.ioParam.ioRefNum;
    return e;
}

static int m_open_read(void *u, const char *path, void **h)
{
    loc L;
    OSErr e;
    (void)u;
    if (resolve(path, &L) < 0)
        return -1;
    if (L.kind != L_FILE)
        return fail(fnfErr, "file");
    memset(&R, 0, sizeof R);
    R.macbin = L.virt_bin;
    R.rref = 0;
    if ((e = open_fork(L.vref, L.dir, L.name, 0, fsRdPerm, &R.dref)) != noErr)
        return fail(e, "open");
    if (R.macbin) {
        macbin_info i;
        const FInfo *fi = &L.info.hFileInfo.ioFlFndrInfo;
        if ((e = open_fork(L.vref, L.dir, L.name, 1, fsRdPerm, &R.rref)) != noErr) {
            FSClose(R.dref);
            return fail(e, "open");
        }
        memset(&i, 0, sizeof i);
        memcpy(i.name, L.name + 1, L.name[0]);
        i.type = fi->fdType;
        i.creator = fi->fdCreator;
        i.flags = (uint16_t)(fi->fdFlags & ~kHasBeenInited);
        i.dlen = (uint32_t)L.info.hFileInfo.ioFlLgLen;
        i.rlen = (uint32_t)L.info.hFileInfo.ioFlRLgLen;
        i.created = (uint32_t)L.info.hFileInfo.ioFlCrDat;
        i.modified = (uint32_t)L.info.hFileInfo.ioFlMdDat;
        macbin_encode_header(R.hdr, &i);
        R.phase = RD_HDR;
        R.left = 128;
    } else {
        R.phase = RD_DATA;
        R.left = 0xFFFFFFFFu; /* to the end of the fork */
    }
    *h = &R;
    return 0;
}

static long read_fork(short ref, uint8_t *buf, uint32_t want)
{
    long n = (long)want;
    OSErr e = FSRead(ref, &n, buf);
    if (e != noErr && e != eofErr)
        return fail(e, "read");
    return n;
}

static long m_read(void *u, void *h, uint8_t *buf, size_t cap)
{
    size_t got = 0;
    (void)u;
    (void)h;
    while (got < cap && R.phase != RD_END) {
        uint32_t want = cap - got < R.left ? (uint32_t)(cap - got) : R.left;
        long k;
        switch (R.phase) {
        case RD_HDR:
            memcpy(buf + got, R.hdr + (128 - R.left), want);
            k = (long)want;
            break;
        case RD_DATA:
        case RD_RSRC:
            k = read_fork(R.phase == RD_DATA ? R.dref : R.rref, buf + got, want);
            if (k < 0)
                return -1;
            if (k == 0 && !R.macbin) {
                R.phase = RD_END;
                continue;
            }
            if (k == 0) /* shorter than the header said: pad the rest */
                R.left = 0;
            break;
        default: /* the padding */
            memset(buf + got, 0, want);
            k = (long)want;
            break;
        }
        got += (size_t)k;
        R.left -= (uint32_t)k;
        if (!R.left) {
            macbin_info i;
            macbin_decode_header(R.hdr, &i);
            R.phase++;
            switch (R.phase) {
            case RD_DATA: R.left = i.dlen; break;
            case RD_DPAD: R.left = macbin_pad(i.dlen) - i.dlen; break;
            case RD_RSRC: R.left = i.rlen; break;
            case RD_RPAD: R.left = macbin_pad(i.rlen) - i.rlen; break;
            default: break;
            }
        }
    }
    return (long)got;
}

static void m_close_read(void *u, void *h)
{
    (void)u;
    (void)h;
    if (R.dref)
        FSClose(R.dref);
    if (R.rref)
        FSClose(R.rref);
    R.dref = R.rref = 0;
}

/* ---- writing: a plain file, or MacBinary / BinHex back into a Mac file ------------- */

enum { WR_PLAIN, WR_MAYBE_BIN, WR_BIN, WR_HQX };

static struct {
    int mode, created;
    short vref, dref, rref;
    long dir;
    Str63 name;          /* the name as uploaded */
    Str63 made;          /* the name of the file made */
    uint8_t head[128];
    size_t headlen;
    uint32_t dleft, dpad, rleft;
    binhex_dec bh;
    uint8_t buf[2][4096];
    size_t blen[2];
    OSErr error;
    macbin_info info;
} W;

/* A type and creator for a plain file, by its extension, so it opens. */
static void guess(const unsigned char *name, OSType *type, OSType *creator)
{
    static const struct {
        const char *ext;
        OSType type, creator;
    } t[] = {
        { ".txt", 'TEXT', 'ttxt' }, { ".text", 'TEXT', 'ttxt' }, { ".md", 'TEXT', 'ttxt' },
        { ".c", 'TEXT', 'ttxt' },    { ".h", 'TEXT', 'ttxt' },    { ".html", 'TEXT', 'MOSS' },
        { ".htm", 'TEXT', 'MOSS' },  { ".jpg", 'JPEG', 'ogle' },  { ".jpeg", 'JPEG', 'ogle' },
        { ".gif", 'GIFf', 'ogle' },  { ".png", 'PNGf', 'ogle' },  { ".pict", 'PICT', 'ttxt' },
        { ".pdf", 'PDF ', 'CARO' },  { ".sit", 'SITD', 'SIT!' },  { ".sea", 'APPL', 'aust' },
        { ".zip", 'ZIP ', 'SITx' },  { ".hqx", 'TEXT', 'SITx' },  { ".bin", 'BINA', 'SITx' },
        { ".mp3", 'MPG3', 'TVOD' },  { ".mov", 'MooV', 'TVOD' },  { ".aiff", 'AIFF', 'TVOD' },
        { ".wav", 'WAVE', 'TVOD' },  { ".img", 'rohd', 'ddsk' },  { ".dsk", 'dImg', 'dCpy' },
    };
    size_t i;
    *type = '????';
    *creator = '????';
    for (i = 0; i < sizeof t / sizeof t[0]; i++)
        if (ends_with((const char *)name + 1, name[0], t[i].ext)) {
            *type = t[i].type;
            *creator = t[i].creator;
            return;
        }
}

static OSErr create(const unsigned char *name, OSType type, OSType creator, uint16_t flags)
{
    HParamBlockRec pb;
    OSErr e;
    memcpy(W.made, name, name[0] + 1);
    memset(&pb, 0, sizeof pb);
    pb.fileParam.ioNamePtr = W.made;
    pb.fileParam.ioVRefNum = W.vref;
    pb.fileParam.ioDirID = W.dir;
    PBHDeleteSync(&pb); /* an upload replaces what is there */
    memset(&pb, 0, sizeof pb);
    pb.fileParam.ioNamePtr = W.made;
    pb.fileParam.ioVRefNum = W.vref;
    pb.fileParam.ioDirID = W.dir;
    if ((e = PBHCreateSync(&pb)) != noErr)
        return e;
    memset(&pb, 0, sizeof pb);
    pb.fileParam.ioNamePtr = W.made;
    pb.fileParam.ioVRefNum = W.vref;
    pb.fileParam.ioDirID = W.dir;
    if ((e = PBHGetFInfoSync(&pb)) != noErr)
        return e;
    pb.fileParam.ioFlFndrInfo.fdType = type;
    pb.fileParam.ioFlFndrInfo.fdCreator = creator;
    pb.fileParam.ioFlFndrInfo.fdFlags = (UInt16)(flags & ~kHasBeenInited);
    pb.fileParam.ioDirID = W.dir;
    if ((e = PBHSetFInfoSync(&pb)) != noErr)
        return e;
    W.created = 1;
    if ((e = open_fork(W.vref, W.dir, W.made, 0, fsWrPerm, &W.dref)) != noErr)
        return e;
    return open_fork(W.vref, W.dir, W.made, 1, fsWrPerm, &W.rref);
}

static void put(int fork, const uint8_t *d, size_t n)
{
    short ref = fork ? W.rref : W.dref;
    long k = (long)n;
    if (W.error || !n)
        return;
    W.error = FSWrite(ref, &k, d);
}

/* BinHex hands over a byte at a time: gather them. */
static void flush(int fork)
{
    put(fork, W.buf[fork], W.blen[fork]);
    W.blen[fork] = 0;
}

static void bh_header(void *u, const macbin_info *i)
{
    Str63 name;
    size_t n = strlen(i->name);
    (void)u;
    name[0] = (unsigned char)(n > 31 ? 31 : n);
    memcpy(name + 1, i->name, name[0]);
    W.info = *i;
    W.error = create(name, i->type, i->creator, i->flags);
}

static void bh_out(void *u, int fork, const uint8_t *d, size_t n)
{
    (void)u;
    while (n--) {
        W.buf[fork][W.blen[fork]++] = *d++;
        if (W.blen[fork] == sizeof W.buf[fork])
            flush(fork);
    }
}

/* MacBinary's forks, as bytes arrive. */
static void bin_bytes(const uint8_t *d, size_t n)
{
    while (n) {
        size_t k;
        if (W.dleft) {
            k = n < W.dleft ? n : W.dleft;
            put(0, d, k);
            W.dleft -= (uint32_t)k;
        } else if (W.dpad) {
            k = n < W.dpad ? n : W.dpad;
            W.dpad -= (uint32_t)k;
        } else if (W.rleft) {
            k = n < W.rleft ? n : W.rleft;
            put(1, d, k);
            W.rleft -= (uint32_t)k;
        } else {
            return; /* its padding, or anything after it */
        }
        d += k;
        n -= k;
    }
}

static void plain_now(void)
{
    OSType type, creator;
    guess(W.name, &type, &creator);
    W.mode = WR_PLAIN;
    W.error = create(W.name, type, creator, 0);
}

static int m_open_write(void *u, const char *path, void **h)
{
    loc L;
    const char *last = strrchr(path, '/');
    size_t n;
    char parent[512];
    (void)u;
    /* The folder first, made if it is not there. */
    snprintf(parent, sizeof parent, "%.*s", last ? (int)(last - path) : 0, path);
    if (mac_mkdir(parent) < 0 || resolve(path, &L) < 0)
        return -1;
    if (L.kind != L_MISSING && L.kind != L_FILE)
        return fail(dupFNErr, "a folder by that name");
    memset(&W, 0, sizeof W);
    W.vref = L.vref;
    W.dir = L.dir;
    last = last ? last + 1 : path;
    n = strlen(last);
    if (L.kind == L_FILE && L.virt_bin) {
        /* "Name.bin" matched Name: the upload is still named Name.bin. */
        to_mac(last, n, W.name);
    } else {
        memcpy(W.name, L.name, L.name[0] + 1);
    }
    if (ends_with(last, n, ".bin")) {
        W.mode = WR_MAYBE_BIN; /* decided by its first 128 bytes */
    } else if (ends_with(last, n, ".hqx")) {
        W.mode = WR_HQX;
        binhex_init(&W.bh, bh_header, bh_out, NULL);
    } else {
        plain_now();
        if (W.error)
            return fail(W.error, "create");
    }
    *h = &W;
    return 0;
}

static int m_write(void *u, void *h, const uint8_t *d, size_t n)
{
    (void)u;
    (void)h;
    switch (W.mode) {
    case WR_PLAIN:
        put(0, d, n);
        break;
    case WR_BIN:
        bin_bytes(d, n);
        break;
    case WR_HQX:
        binhex_feed(&W.bh, d, n);
        if (binhex_done(&W.bh) < 0 && !W.error) {
            snprintf(err, sizeof err, "not a BinHex file, or a damaged one");
            return -1;
        }
        break;
    case WR_MAYBE_BIN: {
        size_t k = 128 - W.headlen < n ? 128 - W.headlen : n;
        memcpy(W.head + W.headlen, d, k);
        W.headlen += k;
        d += k;
        n -= k;
        if (W.headlen < 128)
            return 0;
        if (macbin_decode_header(W.head, &W.info)) {
            Str63 name;
            size_t nl = strlen(W.info.name);
            name[0] = (unsigned char)(nl > 31 ? 31 : nl);
            memcpy(name + 1, W.info.name, name[0]);
            W.mode = WR_BIN;
            W.error = create(name, W.info.type, W.info.creator, W.info.flags);
            W.dleft = W.info.dlen;
            W.dpad = macbin_pad(W.info.dlen) - W.info.dlen;
            W.rleft = W.info.rlen;
            bin_bytes(d, n);
        } else {
            plain_now(); /* a .bin that is not MacBinary: kept as it is */
            put(0, W.head, 128);
            put(0, d, n);
        }
        break;
    }
    }
    return W.error ? fail(W.error, "write") : 0;
}

static void set_dates(unsigned long created, unsigned long modified)
{
    CInfoPBRec pb;
    if (cat_info(W.vref, W.dir, W.made, &pb) != noErr)
        return;
    if (created)
        pb.hFileInfo.ioFlCrDat = created;
    if (modified)
        pb.hFileInfo.ioFlMdDat = modified;
    pb.hFileInfo.ioDirID = W.dir;
    PBSetCatInfoSync(&pb);
}

static int m_close_write(void *u, void *h, uint64_t mtime, int complete)
{
    int ok;
    (void)u;
    (void)h;
    if (W.mode == WR_MAYBE_BIN) { /* shorter than a header: a plain file */
        plain_now();
        put(0, W.head, W.headlen);
    }
    if (W.mode == WR_HQX) {
        flush(0);
        flush(1);
        if (complete && binhex_done(&W.bh) != 1 && !W.error) {
            snprintf(err, sizeof err, "not a BinHex file, or a damaged one");
            complete = 0;
        }
    }
    if (W.dref)
        FSClose(W.dref);
    if (W.rref)
        FSClose(W.rref);
    W.dref = W.rref = 0;
    ok = complete && !W.error;
    if (W.created && !ok) {
        HParamBlockRec pb;
        memset(&pb, 0, sizeof pb);
        pb.fileParam.ioNamePtr = W.made;
        pb.fileParam.ioVRefNum = W.vref;
        pb.fileParam.ioDirID = W.dir;
        PBHDeleteSync(&pb);
    } else if (ok) {
        if (W.mode == WR_BIN)
            set_dates(W.info.created, W.info.modified);
        else if (mtime)
            set_dates(0, to_mac_date(mtime));
    }
    if (W.error && complete)
        return fail(W.error, "write");
    return ok || !complete ? 0 : -1;
}

/* ---- folders and names ------------------------------------------------------------------ */

int mac_mkdir(const char *path)
{
    char part[512];
    const char *p = path;
    loc L;
    /* Each part in turn, made where missing. */
    while (*p == '/')
        p++;
    p += strcspn(p, "/"); /* the disk itself is never made */
    for (;;) {
        snprintf(part, sizeof part, "%.*s", (int)(p - path), path);
        if (resolve(part, &L) < 0)
            return -1;
        if (L.kind == L_MISSING) {
            HParamBlockRec pb;
            OSErr e;
            memset(&pb, 0, sizeof pb);
            pb.fileParam.ioNamePtr = L.name;
            pb.fileParam.ioVRefNum = L.vref;
            pb.fileParam.ioDirID = L.dir;
            if ((e = PBDirCreateSync(&pb)) != noErr)
                return fail(e, "new folder");
        } else if (L.kind == L_FILE) {
            return fail(dupFNErr, "a file by that name");
        }
        if (!*p)
            return 0;
        p++;
        p += strcspn(p, "/");
    }
}

static int m_mkdir(void *u, const char *path)
{
    (void)u;
    return mac_mkdir(path);
}

static int delete_named(short vref, long dir, const unsigned char *name, const char *what)
{
    HParamBlockRec pb;
    OSErr e;
    memset(&pb, 0, sizeof pb);
    pb.fileParam.ioNamePtr = (StringPtr)name;
    pb.fileParam.ioVRefNum = vref;
    pb.fileParam.ioDirID = dir;
    e = PBHDeleteSync(&pb);
    return e == noErr ? 0 : fail(e, what);
}

static int m_remove_file(void *u, const char *path)
{
    loc L;
    (void)u;
    if (resolve(path, &L) < 0)
        return -1;
    if (L.kind != L_FILE)
        return fail(fnfErr, "file");
    return delete_named(L.vref, L.dir, L.name, "delete");
}

/* The empty folders under one, then it: the client deletes files first. */
static int rm_empty(short vref, long dir, long parent, const unsigned char *name)
{
    static Str63 subs[32];
    int nsub = 0, i;
    short k;
    for (k = 1; k < 32767 && nsub < 32; k++) {
        CInfoPBRec pb;
        memset(&pb, 0, sizeof pb);
        subs[nsub][0] = 0;
        pb.hFileInfo.ioNamePtr = subs[nsub];
        pb.hFileInfo.ioVRefNum = vref;
        pb.hFileInfo.ioDirID = dir;
        pb.hFileInfo.ioFDirIndex = k;
        if (PBGetCatInfoSync(&pb) != noErr)
            break;
        if (pb.hFileInfo.ioFlAttrib & 0x10)
            nsub++;
    }
    for (i = nsub - 1; i >= 0; i--) {
        CInfoPBRec pb;
        Str63 nm;
        memcpy(nm, subs[i], subs[i][0] + 1);
        if (cat_info(vref, dir, nm, &pb) == noErr)
            rm_empty(vref, pb.dirInfo.ioDrDirID, dir, nm);
    }
    return delete_named(vref, parent, name, "delete folder");
}

static int m_remove_dir(void *u, const char *path, int recursive)
{
    loc L;
    long parent;
    (void)u;
    if (resolve(path, &L) < 0)
        return -1;
    if (L.kind != L_DIR)
        return fail(dirNFErr, "folder");
    parent = L.info.dirInfo.ioDrParID;
    if (recursive)
        return rm_empty(L.vref, L.dir, parent, L.name);
    return delete_named(L.vref, parent, L.name, "delete folder");
}

static int m_rename(void *u, const char *path, const char *new_name)
{
    loc L;
    HParamBlockRec pb;
    Str63 to;
    long dir;
    size_t n = strlen(new_name);
    OSErr e;
    (void)u;
    if (resolve(path, &L) < 0)
        return -1;
    if (L.kind != L_FILE && L.kind != L_DIR)
        return fail(fnfErr, "rename");
    if (L.virt_bin && ends_with(new_name, n, ".bin"))
        n -= 4; /* the name it shows is not the one it has */
    to_mac(new_name, n, to);
    dir = L.kind == L_DIR ? L.info.dirInfo.ioDrParID : L.dir;
    memset(&pb, 0, sizeof pb);
    pb.fileParam.ioNamePtr = L.name;
    pb.fileParam.ioVRefNum = L.vref;
    pb.fileParam.ioDirID = dir;
    pb.ioParam.ioMisc = (Ptr)to;
    e = PBHRenameSync(&pb);
    return e == noErr ? 0 : fail(e, "rename");
}

static const char *m_error(void *u)
{
    (void)u;
    return err;
}

const cdv_fs_ops *macfs(void)
{
    static const cdv_fs_ops ops = { NULL,          m_list,       m_stat,       m_open_read,
                                    m_read,        m_close_read, m_open_write, m_write,
                                    m_close_write, m_mkdir,      m_remove_file, m_remove_dir,
                                    m_rename,      m_error };
    return &ops;
}
