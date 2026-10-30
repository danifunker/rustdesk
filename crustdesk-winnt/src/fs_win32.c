/* cdv_fs_ops over NT's file system, in UTF-16 (the W calls), so a name in
 * any language arrives whole. */
#include "fs_win32.h"
#include "text.h"

#include "win.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WPATH 1024

static WCHAR root[MAX_PATH];  /* L"" for the whole machine */
static char err[256];

static int fail(const char *what)
{
    DWORD e = GetLastError();
    char msg[160];
    int n;
    msg[0] = 0;
    FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, msg,
                  sizeof msg, NULL);
    for (n = (int)strlen(msg); n > 0 && (msg[n - 1] == '\r' || msg[n - 1] == '\n' ||
                                         msg[n - 1] == '.'); n--)
        msg[n - 1] = 0;
    snprintf(err, sizeof err, "%s: %s", what, msg[0] ? msg : "failed");
    return -1;
}

static int refuse(const char *why)
{
    snprintf(err, sizeof err, "%s", why);
    return -1;
}

static int is_drive_list(const char *path)
{
    return !root[0] && (!strcmp(path, "/") || !strcmp(path, "\\"));
}

/* The client's path as an NT one. Whole machine: "/C:/x", "C:\x", "c:/x";
 * "" and "~" are the first fixed drive. Rooted: under the root. */
static int map(const char *path, WCHAR *out, size_t cap)
{
    WCHAR w[WPATH];
    size_t i, n;
    if (strstr(path, "..")) {
        refuse("no '..' here");
        return -1;
    }
    if (!root[0]) {
        if (!path[0] || !strcmp(path, "~")) {
            path = "C:\\";
        } else {
            if ((path[0] == '/' || path[0] == '\\') && path[1] && path[2] == ':')
                path++;
            if (!(((path[0] | 32) >= 'a' && (path[0] | 32) <= 'z') && path[1] == ':')) {
                refuse("not a path on this machine (C:\\...)");
                return -1;
            }
        }
        n = utf8_to_utf16(path, strlen(path), w, WPATH);
    } else {
        size_t r = wcslen(root);
        if (!path[0] || !strcmp(path, "~"))
            path = "/";
        if (r + strlen(path) + 2 >= WPATH)
            return refuse("path too long");
        memcpy(w, root, r * sizeof(WCHAR));
        if (path[0] != '/' && path[0] != '\\')
            w[r++] = '\\';
        n = r + utf8_to_utf16(path, strlen(path), w + r, WPATH - r);
    }
    for (i = 0; i < n; i++)
        if (w[i] == '/')
            w[i] = '\\';
    /* "C:" is the drive's current directory; "C:\" is its root */
    if (n == 2 && w[1] == ':')
        w[n++] = '\\';
    /* no trailing backslash but a root's */
    while (n > 3 && w[n - 1] == '\\')
        n--;
    w[n] = 0;
    if (n + 1 > cap)
        return refuse("path too long");
    memcpy(out, w, (n + 1) * sizeof(WCHAR));
    return 0;
}

static uint64_t unix_time(const FILETIME *ft)
{
    uint64_t t = (uint64_t)ft->dwHighDateTime << 32 | ft->dwLowDateTime;
    uint64_t epoch = UINT64_C(116444736000000000);
    return t > epoch ? (t - epoch) / 10000000 : 0;
}

static void file_time(uint64_t secs, FILETIME *ft)
{
    uint64_t t = secs * 10000000 + UINT64_C(116444736000000000);
    ft->dwLowDateTime = (DWORD)t;
    ft->dwHighDateTime = (DWORD)(t >> 32);
}

static void fill(cdv_entry *e, const WIN32_FIND_DATAW *fd)
{
    memset(e, 0, sizeof *e);
    e->type = fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? ENT_DIR : ENT_FILE;
    e->hidden = (fd->dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
    e->size = e->type == ENT_DIR ? 0 : (uint64_t)fd->nFileSizeHigh << 32 | fd->nFileSizeLow;
    e->mtime = unix_time(&fd->ftLastWriteTime);
    utf16_to_utf8(fd->cFileName, wcslen(fd->cFileName), e->name, sizeof e->name);
}

static int w_list(void *u, const char *path, int hidden, char *real, size_t cap,
                  void (*each)(void *ctx, const cdv_entry *e), void *ctx)
{
    WCHAR full[WPATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    size_t n;
    (void)u;
    if (is_drive_list(path)) {
        DWORD drives = GetLogicalDrives();
        int i;
        snprintf(real, cap, "/");
        for (i = 0; i < 26; i++) {
            char letter[4];
            UINT type;
            cdv_entry e;
            if (!(drives & (1ul << i)))
                continue;
            letter[0] = (char)('A' + i);
            letter[1] = ':';
            letter[2] = '\\';
            letter[3] = 0;
            type = GetDriveType(letter);
            if (type == DRIVE_NO_ROOT_DIR)
                continue;
            memset(&e, 0, sizeof e);
            e.type = ENT_DRIVE;
            letter[2] = 0;
            strcpy(e.name, letter);
            each(ctx, &e);
        }
        return 0;
    }
    if (map(path, full, WPATH - 4) < 0)
        return -1;
    if (!root[0])
        utf16_to_utf8(full, wcslen(full), real, cap);
    else
        snprintf(real, cap, "%s", path[0] && strcmp(path, "~") ? path : "/");
    n = wcslen(full);
    if (full[n - 1] != '\\')
        full[n++] = '\\';
    full[n++] = '*';
    full[n] = 0;
    h = FindFirstFileW(full, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND)
            return 0; /* an empty drive root */
        return fail(path);
    }
    do {
        cdv_entry e;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        fill(&e, &fd);
        if (e.hidden && !hidden)
            continue;
        each(ctx, &e);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 0;
}

static int w_stat(void *u, const char *path, cdv_entry *e)
{
    WCHAR full[WPATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    size_t n;
    (void)u;
    if (is_drive_list(path) || map(path, full, WPATH) < 0)
        return is_drive_list(path) ? refuse("not a file") : -1;
    n = wcslen(full);
    if (n <= 3 || (root[0] && n == wcslen(root))) {
        /* a drive's or the share's root: FindFirstFile cannot name it */
        DWORD a = GetFileAttributesW(full);
        if (a == 0xffffffff)
            return fail(path);
        memset(e, 0, sizeof *e);
        e->type = ENT_DIR;
        utf16_to_utf8(full, n, e->name, sizeof e->name);
        return 0;
    }
    h = FindFirstFileW(full, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return fail(path);
    FindClose(h);
    fill(e, &fd);
    return 0;
}

static int w_open_read(void *u, const char *path, void **h)
{
    WCHAR full[WPATH];
    HANDLE f;
    (void)u;
    if (map(path, full, WPATH) < 0)
        return -1;
    f = CreateFileW(full, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                    FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return fail(path);
    *h = f;
    return 0;
}

static long w_read(void *u, void *h, uint8_t *buf, size_t n)
{
    DWORD got;
    (void)u;
    if (!ReadFile((HANDLE)h, buf, (DWORD)n, &got, NULL))
        return fail("read");
    return (long)got;
}

static void w_close_read(void *u, void *h)
{
    (void)u;
    CloseHandle((HANDLE)h);
}

/* Every folder above the file (or in `full`, if it ends in a backslash). */
static int mkdirs(WCHAR *full)
{
    WCHAR *p = full;
    size_t skip = root[0] ? wcslen(root) : 3;
    if (wcslen(full) <= skip)
        return 0;
    for (p = full + skip; (p = wcschr(p, '\\')) != NULL; p++) {
        *p = 0;
        if (!CreateDirectoryW(full, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
            *p = '\\';
            return fail("cannot make the folder");
        }
        *p = '\\';
    }
    return 0;
}

typedef struct {
    HANDLE f;
    WCHAR path[WPATH];
} wfile;

static int w_open_write(void *u, const char *path, void **h)
{
    wfile *w = (wfile *)calloc(1, sizeof *w);
    (void)u;
    if (!w || map(path, w->path, WPATH) < 0 || mkdirs(w->path) < 0) {
        free(w);
        return -1;
    }
    w->f = CreateFileW(w->path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                       NULL);
    if (w->f == INVALID_HANDLE_VALUE) {
        free(w);
        return fail(path);
    }
    *h = w;
    return 0;
}

static int w_write(void *u, void *h, const uint8_t *buf, size_t n)
{
    DWORD put;
    (void)u;
    return WriteFile(((wfile *)h)->f, buf, (DWORD)n, &put, NULL) && put == n ? 0 : fail("write");
}

static int w_close_write(void *u, void *h, uint64_t mtime, int complete)
{
    wfile *w = (wfile *)h;
    (void)u;
    if (complete && mtime) {
        FILETIME ft;
        file_time(mtime, &ft);
        SetFileTime(w->f, NULL, NULL, &ft);
    }
    CloseHandle(w->f);
    if (!complete)
        DeleteFileW(w->path);
    free(w);
    return 0;
}

static int w_mkdir(void *u, const char *path)
{
    WCHAR full[WPATH];
    size_t n;
    (void)u;
    if (map(path, full, WPATH - 2) < 0)
        return -1;
    n = wcslen(full);
    if (full[n - 1] != '\\') {
        full[n] = '\\';
        full[n + 1] = 0;
    }
    return mkdirs(full);
}

static int w_remove_file(void *u, const char *path)
{
    WCHAR full[WPATH];
    (void)u;
    if (map(path, full, WPATH) < 0)
        return -1;
    SetFileAttributesW(full, FILE_ATTRIBUTE_NORMAL);
    return DeleteFileW(full) ? 0 : fail(path);
}

/* Recursive: the empty folders only, as fs.rs does -- the client deletes
 * the files first, one by one. */
static int rm_empty(WCHAR *full)
{
    WIN32_FIND_DATAW fd;
    HANDLE h;
    size_t n = wcslen(full);
    if (n + 3 >= WPATH)
        return refuse("path too long");
    wcscpy(full + n, L"\\*");
    h = FindFirstFileW(full, &fd);
    full[n] = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !wcscmp(fd.cFileName, L".") ||
                !wcscmp(fd.cFileName, L".."))
                continue;
            if (n + 2 + wcslen(fd.cFileName) < WPATH) {
                full[n] = '\\';
                wcscpy(full + n + 1, fd.cFileName);
                rm_empty(full);
                full[n] = 0;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(full) ? 0 : fail("cannot remove the folder");
}

static int w_remove_dir(void *u, const char *path, int recursive)
{
    WCHAR full[WPATH];
    (void)u;
    if (map(path, full, WPATH) < 0)
        return -1;
    if (wcslen(full) <= 3 || (root[0] && wcslen(full) <= wcslen(root)))
        return refuse("that is a drive");
    if (recursive)
        return rm_empty(full);
    return RemoveDirectoryW(full) ? 0 : fail(path);
}

static int w_rename(void *u, const char *path, const char *new_name)
{
    WCHAR full[WPATH], to[WPATH], *slash;
    (void)u;
    if (map(path, full, WPATH) < 0)
        return -1;
    if (strpbrk(new_name, "/\\:"))
        return refuse("a name, not a path");
    wcscpy(to, full);
    slash = wcsrchr(to, '\\');
    if (!slash)
        return refuse("cannot rename that");
    utf8_to_utf16(new_name, strlen(new_name), slash + 1, WPATH - (size_t)(slash + 1 - to));
    return MoveFileW(full, to) ? 0 : fail(path);
}

static const char *w_error(void *u)
{
    (void)u;
    return err;
}

const cdv_fs_ops *fs_win32(const char *dir)
{
    static const cdv_fs_ops ops = { NULL,          w_list,       w_stat,        w_open_read,
                                    w_read,        w_close_read, w_open_write,  w_write,
                                    w_close_write, w_mkdir,      w_remove_file, w_remove_dir,
                                    w_rename,      w_error };
    size_t n;
    if (!dir[0] || !strcmp(dir, "/") || !strcmp(dir, "\\")) {
        root[0] = 0;
        return &ops;
    }
    n = utf8_to_utf16(dir, strlen(dir), root, MAX_PATH);
    while (n > 3 && (root[n - 1] == '\\' || root[n - 1] == '/'))
        root[--n] = 0;
    CreateDirectoryW(root, NULL);
    return &ops;
}
