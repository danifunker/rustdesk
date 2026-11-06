#include "prefs.h"
#include "plat.h"
#include "rng.h"

#include "win.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

cdv_prefs prefs;

const char *prefs_dir(void)
{
    static char dir[MAX_PATH];
    if (!dir[0]) {
        char *s;
        GetModuleFileName(NULL, dir, sizeof dir);
        s = strrchr(dir, '\\');
        if (s)
            s[1] = 0;
        else
            strcpy(dir, ".\\");
    }
    return dir;
}

void prefs_path(char *out, size_t cap, const char *name)
{
    snprintf(out, cap, "%s%s", prefs_dir(), name);
}

void prefs_log_path(char *out, size_t cap)
{
    size_t n = strlen(prefs.logdir);
    DWORD a = n ? GetFileAttributes(prefs.logdir) : 0xffffffff;
    if (a == 0xffffffff || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
        prefs_path(out, cap, "C-Desk-Vint.log");
        return;
    }
    snprintf(out, cap, "%s%sC-Desk-Vint.log", prefs.logdir,
             prefs.logdir[n - 1] == '\\' || prefs.logdir[n - 1] == '/' ? "" : "\\");
}

static void copy(char *dst, const char *src, size_t cap)
{
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}

static const char *hex(const uint8_t *b, int n, char *out)
{
    int i;
    for (i = 0; i < n; i++)
        sprintf(out + 2 * i, "%02x", b[i]);
    out[2 * n] = 0;
    return out;
}

static int unhex(const char *s, uint8_t *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1)
            return 0;
        b[i] = (uint8_t)v;
    }
    return 1;
}

static void read_file(void)
{
    char path[MAX_PATH], line[512];
    FILE *f;
    prefs_path(path, sizeof path, "C-Desk-Vint.cfg");
    f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *nl = strpbrk(line, "\r\n");
        if (nl)
            *nl = 0;
        if (!eq || line[0] == '#')
            continue;
        *eq++ = 0;
        if (!strcmp(line, "password"))
            copy(prefs.password, eq, sizeof prefs.password);
        else if (!strcmp(line, "port"))
            prefs.port = atoi(eq);
        else if (!strcmp(line, "quality"))
            prefs.q = atoi(eq);
        else if (!strcmp(line, "id"))
            copy(prefs.id, eq, sizeof prefs.id);
        else if (!strcmp(line, "uuid"))
            prefs.have_uuid = unhex(eq, prefs.uuid, 16);
        else if (!strcmp(line, "seed"))
            prefs.have_seed = unhex(eq, prefs.seed, 32);
        else if (!strcmp(line, "server"))
            copy(prefs.server, eq, sizeof prefs.server);
        else if (!strcmp(line, "key"))
            copy(prefs.key, eq, sizeof prefs.key);
        else if (!strcmp(line, "relay"))
            copy(prefs.relay, eq, sizeof prefs.relay);
        else if (!strcmp(line, "name"))
            copy(prefs.name, eq, sizeof prefs.name);
        else if (!strcmp(line, "files"))
            copy(prefs.files, eq, sizeof prefs.files);
        else if (!strcmp(line, "api"))
            copy(prefs.api, eq, sizeof prefs.api);
        else if (!strcmp(line, "lan"))
            prefs.lan = atoi(eq);
        else if (!strcmp(line, "hook"))
            prefs.hook = atoi(eq);
        else if (!strcmp(line, "terminal"))
            prefs.terminal = atoi(eq);
        else if (!strcmp(line, "tunnel"))
            prefs.tunnel = atoi(eq);
        else if (!strcmp(line, "logdir"))
            copy(prefs.logdir, eq, sizeof prefs.logdir);
    }
    fclose(f);
}

/* Only Administrators and SYSTEM may read or change it: it holds the
 * password and the private key. Nothing happens on FAT, which keeps no
 * permissions. */
static void restrict_file(const char *path)
{
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID admins = NULL, system = NULL;
    SECURITY_DESCRIPTOR sd;
    char aclbuf[256];
    PACL acl = (PACL)aclbuf;
    if (!plat_nt())
        return; /* 9x keeps no permissions on any disk */
    if (!AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                  0, 0, 0, 0, 0, 0, &admins) ||
        !AllocateAndInitializeSid(&nt, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &system))
        goto out;
    if (InitializeAcl(acl, sizeof aclbuf, ACL_REVISION) &&
        AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, admins) &&
        AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, system) &&
        InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) &&
        SetSecurityDescriptorDacl(&sd, TRUE, acl, FALSE))
        SetFileSecurity(path, DACL_SECURITY_INFORMATION, &sd);
out:
    if (admins)
        FreeSid(admins);
    if (system)
        FreeSid(system);
}

int prefs_save(void)
{
    char path[MAX_PATH], tmp[MAX_PATH], u[33], k[65];
    FILE *f;
    prefs_path(path, sizeof path, "C-Desk-Vint.cfg");
    prefs_path(tmp, sizeof tmp, "C-Desk-Vint.new");
    f = fopen(tmp, "w");
    if (!f)
        return 0;
    restrict_file(tmp);
    fprintf(f, "# C-Desk-Vint settings. The seed is this machine's private key.\n");
    fprintf(f, "id=%s\npassword=%s\nport=%d\nquality=%d\n", prefs.id, prefs.password, prefs.port,
            prefs.q);
    fprintf(f, "server=%s\nkey=%s\nrelay=%s\nname=%s\nfiles=%s\napi=%s\nlan=%d\n", prefs.server,
            prefs.key, prefs.relay, prefs.name, prefs.files, prefs.api, prefs.lan);
    fprintf(f, "hook=%d\nlogdir=%s\nterminal=%d\ntunnel=%d\n", prefs.hook, prefs.logdir,
            prefs.terminal, prefs.tunnel);
    fprintf(f, "uuid=%s\nseed=%s\n", hex(prefs.uuid, 16, u), hex(prefs.seed, 32, k));
    if (fclose(f) != 0)
        return 0;
    /* replace in one step, so a reader never sees half a file */
    DeleteFile(path);
    if (!MoveFile(tmp, path))
        return 0;
    return 1;
}

/* CryptGenRandom, where the CryptoAPI is installed (it came with NT 4.0 and
 * Internet Explorer 3); looked up at run time so the program starts without. */
static void crypto_entropy(void)
{
    typedef BOOL(WINAPI * acq_fn)(ULONG *, LPCSTR, LPCSTR, DWORD, DWORD);
    typedef BOOL(WINAPI * gen_fn)(ULONG, DWORD, BYTE *);
    typedef BOOL(WINAPI * rel_fn)(ULONG, DWORD);
    HMODULE m = LoadLibrary("advapi32.dll");
    acq_fn acq;
    gen_fn gen;
    rel_fn rel;
    ULONG prov;
    BYTE b[32];
    if (!m)
        return;
    acq = (acq_fn)GetProcAddress(m, "CryptAcquireContextA");
    gen = (gen_fn)GetProcAddress(m, "CryptGenRandom");
    rel = (rel_fn)GetProcAddress(m, "CryptReleaseContext");
    if (acq && gen && rel && acq(&prov, NULL, NULL, 1 /* PROV_RSA_FULL */,
                                 0xF0000000 /* CRYPT_VERIFYCONTEXT */)) {
        if (gen(prov, sizeof b, b))
            rng_add(b, sizeof b);
        rel(prov, 0);
    }
    FreeLibrary(m);
}

void prefs_gather_entropy(void)
{
    LARGE_INTEGER pc;
    MEMORYSTATUS ms;
    POINT pt;
    FILETIME ft;
    DWORD v[6];
    int i;
    for (i = 0; i < 64; i++) {
        volatile int spin = 0, j;
        QueryPerformanceCounter(&pc);
        for (j = 0; j < (int)(pc.LowPart & 255); j++)
            spin += j;
        rng_add(&pc, sizeof pc);
    }
    GetSystemTimeAsFileTime(&ft);
    rng_add(&ft, sizeof ft);
    ms.dwLength = sizeof ms;
    GlobalMemoryStatus(&ms);
    rng_add(&ms, sizeof ms);
    if (GetCursorPos(&pt))
        rng_add(&pt, sizeof pt);
    v[0] = GetCurrentProcessId();
    v[1] = GetCurrentThreadId();
    v[2] = GetTickCount();
    v[3] = (DWORD)GetDiskFreeSpace;
    v[4] = (DWORD)&ms;
    v[5] = GetLogicalDrives();
    rng_add(v, sizeof v);
    crypto_entropy();
}

void prefs_new_password(char out[9])
{
    static const char a[] = "abcdefghjkmnpqrstuvwxyz23456789";
    int i;
    for (i = 0; i < 8; i++)
        out[i] = a[rng_u32() % (sizeof a - 1)];
    out[8] = 0;
}

int prefs_load(int create)
{
    memset(&prefs, 0, sizeof prefs);
    prefs.port = DEFAULT_PORT;
    prefs.q = DEFAULT_Q;
    prefs.lan = 1;
    prefs.hook = 1;
    prefs.terminal = 1;
    prefs.tunnel = 1;
    copy(prefs.files, "/", sizeof prefs.files);
    copy(prefs.server, DEFAULT_SERVER, sizeof prefs.server);
    copy(prefs.key, DEFAULT_KEY, sizeof prefs.key);
    copy(prefs.api, DEFAULT_API, sizeof prefs.api);
    read_file();
    if (prefs.q < 0 || prefs.q > 127)
        prefs.q = DEFAULT_Q;
    if (prefs.port < 0 || prefs.port > 65535)
        prefs.port = DEFAULT_PORT;
    if (create && (!prefs.password[0] || !prefs.id[0] || !prefs.have_uuid || !prefs.have_seed)) {
        /* First run: a password nobody chose, an ID and a key of our own. */
        static const char digits[] = "123456789";
        int i;
        prefs_gather_entropy();
        if (!prefs.password[0])
            prefs_new_password(prefs.password);
        if (!prefs.id[0]) {
            for (i = 0; i < 9; i++)
                prefs.id[i] = digits[rng_u32() % (sizeof digits - 1)];
            prefs.id[9] = 0;
        }
        if (!prefs.have_uuid) {
            rng_bytes(prefs.uuid, 16);
            prefs.have_uuid = 1;
        }
        if (!prefs.have_seed) {
            rng_bytes(prefs.seed, 32);
            prefs.have_seed = 1;
        }
        return prefs_save();
    }
    return 1;
}

void prefs_status_write(const char *text)
{
    char path[MAX_PATH];
    HANDLE f;
    DWORD w;
    prefs_path(path, sizeof path, "C-Desk-Vint.sta");
    f = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS,
                   FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    WriteFile(f, text, (DWORD)strlen(text), &w, NULL);
    CloseHandle(f);
}

int prefs_status_read(char *out, size_t cap)
{
    char path[MAX_PATH];
    HANDLE f;
    DWORD got = 0;
    prefs_path(path, sizeof path, "C-Desk-Vint.sta");
    f = CreateFile(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                   FILE_ATTRIBUTE_NORMAL, NULL);
    out[0] = 0;
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    ReadFile(f, out, (DWORD)cap - 1, &got, NULL);
    CloseHandle(f);
    out[got] = 0;
    return 1;
}

void prefs_format_id(const char *id, char *out, size_t cap)
{
    size_t n = strlen(id), i, o = 0;
    for (i = 0; i < n && o + 2 < cap; i++) {
        if (i && (n - i) % 3 == 0)
            out[o++] = ' ';
        out[o++] = id[i];
    }
    out[o] = 0;
}
