/* cdv_fs_ops over a POSIX directory, for testing the file-transfer protocol
 * on Linux: the client's "/" is the directory `root`. Paths with ".." are
 * refused. */
#include "fs_posix.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static char root[512];
static char err[256];

static int fail(const char *what)
{
    snprintf(err, sizeof err, "%s: %s", what, strerror(errno));
    return -1;
}

/* The client's path, under root. */
static int map(const char *path, char *out, size_t cap)
{
    if (strstr(path, "..")) {
        snprintf(err, sizeof err, "no '..' here");
        return -1;
    }
    if (!path[0] || !strcmp(path, "~"))
        path = "/";
    snprintf(out, cap, "%s%s%s", root, path[0] == '/' ? "" : "/", path);
    return 0;
}

static int to_entry(const char *full, const char *name, cdv_entry *e)
{
    struct stat st;
    if (stat(full, &st) < 0)
        return fail(name);
    memset(e, 0, sizeof *e);
    e->type = S_ISDIR(st.st_mode) ? ENT_DIR : ENT_FILE;
    e->hidden = name[0] == '.';
    e->size = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size;
    e->mtime = (uint64_t)st.st_mtime;
    snprintf(e->name, sizeof e->name, "%s", name);
    return 0;
}

static int p_list(void *u, const char *path, int hidden, char *real, size_t cap,
                  void (*each)(void *ctx, const cdv_entry *e), void *ctx)
{
    char full[1024], sub[1536];
    DIR *d;
    struct dirent *de;
    (void)u;
    if (map(path, full, sizeof full) < 0)
        return -1;
    snprintf(real, cap, "%s", path[0] && strcmp(path, "~") ? path : "/");
    d = opendir(full);
    if (!d)
        return fail(path);
    while ((de = readdir(d)) != NULL) {
        cdv_entry e;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (de->d_name[0] == '.' && !hidden)
            continue;
        snprintf(sub, sizeof sub, "%s/%s", full, de->d_name);
        if (to_entry(sub, de->d_name, &e) == 0)
            each(ctx, &e);
    }
    closedir(d);
    return 0;
}

static int p_stat(void *u, const char *path, cdv_entry *e)
{
    char full[1024];
    const char *slash = strrchr(path, '/');
    (void)u;
    if (map(path, full, sizeof full) < 0)
        return -1;
    return to_entry(full, slash ? slash + 1 : path, e);
}

static int p_open_read(void *u, const char *path, void **h)
{
    char full[1024];
    FILE *f;
    (void)u;
    if (map(path, full, sizeof full) < 0)
        return -1;
    f = fopen(full, "rb");
    if (!f)
        return fail(path);
    *h = f;
    return 0;
}

static long p_read(void *u, void *h, uint8_t *buf, size_t n)
{
    size_t got = fread(buf, 1, n, (FILE *)h);
    (void)u;
    if (!got && ferror((FILE *)h))
        return fail("read");
    return (long)got;
}

static void p_close_read(void *u, void *h)
{
    (void)u;
    fclose((FILE *)h);
}

static int mkdirs(char *full)
{
    char *p;
    for (p = full + strlen(root) + 1; (p = strchr(p, '/')) != NULL; p++) {
        *p = 0;
        if (mkdir(full, 0755) < 0 && errno != EEXIST) {
            *p = '/';
            return fail("mkdir");
        }
        *p = '/';
    }
    return 0;
}

typedef struct {
    FILE *f;
    char path[1024];
} wfile;

static int p_open_write(void *u, const char *path, void **h)
{
    wfile *w = calloc(1, sizeof *w);
    (void)u;
    if (!w || map(path, w->path, sizeof w->path) < 0 || mkdirs(w->path) < 0) {
        free(w);
        return -1;
    }
    w->f = fopen(w->path, "wb");
    if (!w->f) {
        free(w);
        return fail(path);
    }
    *h = w;
    return 0;
}

static int p_write(void *u, void *h, const uint8_t *buf, size_t n)
{
    (void)u;
    return fwrite(buf, 1, n, ((wfile *)h)->f) == n ? 0 : fail("write");
}

static int p_close_write(void *u, void *h, uint64_t mtime, int complete)
{
    wfile *w = (wfile *)h;
    (void)u;
    fclose(w->f);
    if (!complete) {
        unlink(w->path);
    } else if (mtime) {
        struct timeval tv[2] = { { (time_t)mtime, 0 }, { (time_t)mtime, 0 } };
        utimes(w->path, tv);
    }
    free(w);
    return 0;
}

static int p_mkdir(void *u, const char *path)
{
    char full[1024];
    (void)u;
    if (map(path, full, sizeof full) < 0)
        return -1;
    strcat(full, "/");
    return mkdirs(full);
}

static int p_remove_file(void *u, const char *path)
{
    char full[1024];
    (void)u;
    if (map(path, full, sizeof full) < 0)
        return -1;
    return unlink(full) < 0 ? fail(path) : 0;
}

/* Recursive: the empty folders only, as fs.rs does -- the client deletes
 * the files first, one by one. */
static int rm_empty(const char *full)
{
    DIR *d = opendir(full);
    struct dirent *de;
    char sub[1536];
    if (!d)
        return fail(full);
    while ((de = readdir(d)) != NULL) {
        struct stat st;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        snprintf(sub, sizeof sub, "%s/%s", full, de->d_name);
        if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode))
            rm_empty(sub);
    }
    closedir(d);
    return rmdir(full) < 0 ? fail(full) : 0;
}

static int p_remove_dir(void *u, const char *path, int recursive)
{
    char full[1024];
    (void)u;
    if (map(path, full, sizeof full) < 0)
        return -1;
    if (recursive)
        return rm_empty(full);
    return rmdir(full) < 0 ? fail(path) : 0;
}

static int p_rename(void *u, const char *path, const char *new_name)
{
    char full[1024], to[1536], *slash;
    (void)u;
    if (map(path, full, sizeof full) < 0 || strchr(new_name, '/'))
        return -1;
    snprintf(to, sizeof to, "%s", full);
    slash = strrchr(to, '/');
    snprintf(slash + 1, sizeof to - (size_t)(slash + 1 - to), "%s", new_name);
    return rename(full, to) < 0 ? fail(path) : 0;
}

static const char *p_error(void *u)
{
    (void)u;
    return err;
}

const cdv_fs_ops *fs_posix(const char *dir)
{
    static const cdv_fs_ops ops = { NULL,           p_list,        p_stat,     p_open_read,
                                    p_read,         p_close_read,  p_open_write, p_write,
                                    p_close_write,  p_mkdir,       p_remove_file, p_remove_dir,
                                    p_rename,       p_error };
    snprintf(root, sizeof root, "%s", dir);
    mkdir(root, 0755);
    return &ops;
}
