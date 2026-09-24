#include "files.h"

#include "pb.h"
#include "unzstd.h"

#include <stdio.h>
#include <string.h>

enum { M_FILE_ACTION = 17, M_FILE_RESPONSE = 18 };
/* FileAction's union */
enum { FA_READ_DIR = 1, FA_SEND, FA_RECEIVE, FA_CREATE, FA_REMOVE_DIR, FA_REMOVE_FILE,
       FA_ALL_FILES, FA_CANCEL, FA_SEND_CONFIRM, FA_RENAME, FA_READ_EMPTY_DIRS };
/* FileResponse's union */
enum { FR_DIR = 1, FR_BLOCK, FR_ERROR, FR_DONE, FR_DIGEST, FR_EMPTY_DIRS };

#define CHUNK 16384        /* a download block: small enough to interleave */
#define NAMES 32768        /* the relative names of a job's files */
#define MAXFILES 1024
#define DIRQ 16384         /* folders waiting to be walked, relative names */

typedef struct {
    uint32_t name;         /* offset into names[] */
    uint64_t size, mtime;
} job_file;

/* A job's files: a download's and an upload's each have one. */
typedef struct {
    job_file files[MAXFILES];
    int n;
    char names[NAMES];
    size_t len;
} file_list;

struct cdv_files {
    cdv_session *s;
    const cdv_fs_ops *fs;
    uint8_t *work;
    size_t workcap;
    /* the download in progress */
    struct {
        int active, id, cur, count;
        int opened;
        void *h;
        char base[512];
    } rd;
    /* the upload in progress */
    struct {
        int active, id, cur, count;
        void *h;
        char base[512];
    } wr;
    file_list rdl, wrl;
    /* walking folders */
    char dirq[DIRQ];
    size_t dirq_len;
    int too_many;
    pbw *w;
};

size_t cdv_files_size(void)
{
    return sizeof(cdv_files);
}

void cdv_files_init(cdv_files *f, cdv_session *s, const cdv_fs_ops *fs, uint8_t *work,
                    size_t workcap)
{
    memset(f, 0, sizeof *f);
    f->s = s;
    f->fs = fs;
    f->work = work;
    f->workcap = workcap;
}

/* ---- replies -------------------------------------------------------------------- */

static void send_error(cdv_files *f, int id, const char *err, int file_num)
{
    pbw w;
    if (!cdv_msg_begin(f->s, &w))
        return;
    pbw_begin(&w, M_FILE_RESPONSE);
    pbw_begin(&w, FR_ERROR);
    pbw_varint(&w, 1, (uint32_t)id);
    pbw_string(&w, 2, err && *err ? err : "failed");
    pbw_sint(&w, 3, file_num);
    pbw_end(&w);
    pbw_end(&w);
    cdv_msg_end(f->s, &w);
}

static void send_done(cdv_files *f, int id, int file_num)
{
    pbw w;
    if (!cdv_msg_begin(f->s, &w))
        return;
    pbw_begin(&w, M_FILE_RESPONSE);
    pbw_begin(&w, FR_DONE);
    pbw_varint(&w, 1, (uint32_t)id);
    pbw_sint(&w, 2, file_num);
    pbw_end(&w);
    pbw_end(&w);
    cdv_msg_end(f->s, &w);
}

static void result(cdv_files *f, int ok, int id, int file_num)
{
    if (ok)
        send_done(f, id, file_num);
    else
        send_error(f, id, f->fs->error(f->fs->u), file_num);
}

static void put_entry(pbw *w, const cdv_entry *e, const char *name)
{
    pbw_begin(w, 3); /* entries */
    if (e->type)
        pbw_varint(w, 1, (uint32_t)e->type);
    pbw_string(w, 2, name);
    if (e->hidden)
        pbw_bool(w, 3, 1);
    if (e->size)
        pbw_varint(w, 4, e->size);
    if (e->mtime)
        pbw_varint(w, 5, e->mtime);
    pbw_end(w);
}

static void each_entry(void *ctx, const cdv_entry *e)
{
    cdv_files *f = (cdv_files *)ctx;
    put_entry(f->w, e, e->name);
}

/* FileResponse.dir for one directory, as the client browses. */
static void read_dir(cdv_files *f, const char *path, int hidden)
{
    pbw w;
    char real[512];
    int rc;
    /* A listing is one message, built in the queue: begin it now, and write
     * the path once the file system has said which it listed. */
    if (!cdv_msg_begin(f->s, &w))
        return;
    pbw_begin(&w, M_FILE_RESPONSE);
    pbw_begin(&w, FR_DIR);
    f->w = &w;
    real[0] = 0;
    rc = f->fs->list(f->fs->u, path, hidden, real, sizeof real, each_entry, f);
    pbw_string(&w, 2, real[0] ? real : path);
    pbw_end(&w);
    pbw_end(&w);
    f->w = NULL;
    if (rc < 0) {
        /* A listing that failed has nothing to say: send the error instead
         * (the half-built message is not committed). */
        send_error(f, 0, f->fs->error(f->fs->u), -1);
        return;
    }
    if (w.overflow) {
        send_error(f, 0, "Too many files to list", -1);
        return;
    }
    cdv_msg_end(f->s, &w);
}

/* ---- recursive listing, for a download or all_files ----------------------------- */

static int add_file(file_list *l, const char *rel, uint64_t size, uint64_t mtime)
{
    size_t n = strlen(rel) + 1;
    if (l->n >= MAXFILES || l->len + n > NAMES)
        return 0;
    l->files[l->n].name = (uint32_t)l->len;
    l->files[l->n].size = size;
    l->files[l->n].mtime = mtime;
    memcpy(l->names + l->len, rel, n);
    l->len += n;
    l->n++;
    return 1;
}

typedef void (*file_fn)(cdv_files *f, void *ctx, const char *rel, const cdv_entry *e);

typedef struct {
    cdv_files *f;
    const char *prefix;
    file_fn fn;
    void *ctx;
} walk_ctx;

static void walk_each(void *vc, const cdv_entry *e)
{
    walk_ctx *c = (walk_ctx *)vc;
    cdv_files *f = c->f;
    char rel[512];
    snprintf(rel, sizeof rel, "%s%s%s", c->prefix, c->prefix[0] ? "/" : "", e->name);
    if (e->type == ENT_FILE) {
        c->fn(f, c->ctx, rel, e);
    } else if (e->type == ENT_DIR) {
        size_t n = strlen(rel) + 1;
        if (f->dirq_len + n <= DIRQ) {
            memcpy(f->dirq + f->dirq_len, rel, n);
            f->dirq_len += n;
        } else {
            f->too_many = 1;
        }
    }
}

/* Every file under base, breadth first: fn() for each, with its name
 * relative to base. Folders are queued in dirq and taken from the front. */
static int walk(cdv_files *f, const char *base, int hidden, file_fn fn, void *ctx)
{
    size_t head = 0;
    char real[512], rel[512], path[1024];
    walk_ctx c;
    c.f = f;
    c.fn = fn;
    c.ctx = ctx;
    f->dirq_len = 0;
    f->too_many = 0;
    rel[0] = 0;
    for (;;) {
        c.prefix = rel;
        snprintf(path, sizeof path, "%s%s%s", base, rel[0] ? "/" : "", rel);
        if (f->fs->list(f->fs->u, path, hidden, real, sizeof real, walk_each, &c) < 0 && !rel[0])
            return -1;
        if (head >= f->dirq_len)
            return 0;
        strncpy(rel, f->dirq + head, sizeof rel - 1);
        rel[sizeof rel - 1] = 0;
        head += strlen(f->dirq + head) + 1;
    }
}

static void to_list(cdv_files *f, void *ctx, const char *rel, const cdv_entry *e)
{
    if (!add_file((file_list *)ctx, rel, e->size, e->mtime))
        f->too_many = 1;
}

/* All files under path (or path itself, with an empty name). */
static int list_job(cdv_files *f, file_list *l, const char *path, int hidden)
{
    cdv_entry e;
    l->n = 0;
    l->len = 0;
    f->too_many = 0;
    if (f->fs->stat(f->fs->u, path, &e) < 0)
        return -1;
    if (e.type == ENT_FILE)
        return add_file(l, "", e.size, e.mtime) ? 0 : -1;
    return walk(f, path, hidden, to_list, l);
}

static void send_job_dir(cdv_files *f, const file_list *l, int id, const char *path)
{
    pbw w;
    int i;
    if (!cdv_msg_begin(f->s, &w))
        return;
    pbw_begin(&w, M_FILE_RESPONSE);
    pbw_begin(&w, FR_DIR);
    pbw_varint(&w, 1, (uint32_t)id);
    pbw_string(&w, 2, path);
    for (i = 0; i < l->n; i++) {
        cdv_entry e;
        memset(&e, 0, sizeof e);
        e.type = ENT_FILE;
        e.size = l->files[i].size;
        e.mtime = l->files[i].mtime;
        put_entry(&w, &e, l->names + l->files[i].name);
    }
    pbw_end(&w);
    pbw_end(&w);
    if (w.overflow || f->too_many) {
        send_error(f, id, "Too many files for one transfer", 0);
        f->rd.active = 0;
        return;
    }
    cdv_msg_end(f->s, &w);
}

static void join(char *out, size_t cap, const char *base, const char *name)
{
    if (!name[0])
        snprintf(out, cap, "%s", base);
    else
        snprintf(out, cap, "%s/%s", base, name);
}

/* ---- downloads -------------------------------------------------------------------- */

static void start_send(cdv_files *f, const uint8_t *d, size_t n)
{
    pbr r;
    int id = 0, hidden = 0;
    char path[512] = "";
    pbr_init(&r, d, n);
    while (pbr_next(&r)) {
        if (r.field == 1)
            id = (int)r.v;
        else if (r.field == 2 && r.wire == PB_LEN) {
            size_t k = r.len < sizeof path - 1 ? r.len : sizeof path - 1;
            memcpy(path, r.data, k);
            path[k] = 0;
        } else if (r.field == 3)
            hidden = r.v != 0;
    }
    if (f->rd.active && f->rd.opened)
        f->fs->close_read(f->fs->u, f->rd.h);
    memset(&f->rd, 0, sizeof f->rd);
    if (list_job(f, &f->rdl, path, hidden) < 0) {
        send_error(f, id, f->fs->error(f->fs->u), 0);
        return;
    }
    f->rd.active = 1;
    f->rd.id = id;
    f->rd.count = f->rdl.n;
    strncpy(f->rd.base, path, sizeof f->rd.base - 1);
    send_job_dir(f, &f->rdl, id, path);
}

static void send_block(cdv_files *f, int file_num, const uint8_t *data, size_t n)
{
    pbw w;
    if (!cdv_msg_begin(f->s, &w))
        return;
    pbw_begin(&w, M_FILE_RESPONSE);
    pbw_begin(&w, FR_BLOCK);
    pbw_varint(&w, 1, (uint32_t)f->rd.id);
    pbw_sint(&w, 2, file_num);
    pbw_bytes(&w, 3, data, n);
    pbw_end(&w);
    pbw_end(&w);
    cdv_msg_end(f->s, &w);
}

int cdv_files_pump(cdv_files *f)
{
    int sent = 0;
    while (f->rd.active && sent < 4) {
        long got;
        if (cdv_ctl_room(f->s) < CHUNK + 256)
            return 1; /* the network is behind: later */
        if (f->rd.cur >= f->rd.count) {
            send_done(f, f->rd.id, f->rd.count);
            f->rd.active = 0;
            return 0;
        }
        if (!f->rd.opened) {
            char path[768];
            join(path, sizeof path, f->rd.base, f->rdl.names + f->rdl.files[f->rd.cur].name);
            if (f->fs->open_read(f->fs->u, path, &f->rd.h) < 0) {
                send_error(f, f->rd.id, f->fs->error(f->fs->u), f->rd.cur);
                f->rd.cur++;
                continue;
            }
            f->rd.opened = 1;
        }
        got = f->fs->read(f->fs->u, f->rd.h, f->work, CHUNK);
        if (got < 0) {
            send_error(f, f->rd.id, f->fs->error(f->fs->u), f->rd.cur);
            got = 0;
        }
        /* The end of a file is an empty block, as fs.rs sends it: for an
         * empty file it is the only one, and it is what creates the file. */
        send_block(f, f->rd.cur, f->work, (size_t)got);
        sent++;
        if (got == 0) {
            f->fs->close_read(f->fs->u, f->rd.h);
            f->rd.opened = 0;
            f->rd.cur++;
        }
    }
    return f->rd.active;
}

/* ---- uploads ---------------------------------------------------------------------- */

static void start_receive(cdv_files *f, const uint8_t *d, size_t n)
{
    pbr r;
    int id = 0;
    char path[512] = "";
    if (f->wr.active && f->wr.cur >= 0)
        f->fs->close_write(f->fs->u, f->wr.h, 0, 0);
    memset(&f->wr, 0, sizeof f->wr);
    f->wrl.n = 0;
    f->wrl.len = 0;
    pbr_init(&r, d, n);
    while (pbr_next(&r)) {
        if (r.field == 1) {
            id = (int)r.v;
        } else if (r.field == 2 && r.wire == PB_LEN) {
            size_t k = r.len < sizeof path - 1 ? r.len : sizeof path - 1;
            memcpy(path, r.data, k);
            path[k] = 0;
        } else if (r.field == 3 && r.wire == PB_LEN) { /* files */
            pbr e;
            char name[512] = "";
            uint64_t size = 0, mtime = 0;
            pbr_init(&e, r.data, r.len);
            while (pbr_next(&e)) {
                if (e.field == 2 && e.wire == PB_LEN) {
                    size_t k = e.len < sizeof name - 1 ? e.len : sizeof name - 1;
                    memcpy(name, e.data, k);
                    name[k] = 0;
                } else if (e.field == 4) {
                    size = e.v;
                } else if (e.field == 5) {
                    mtime = e.v;
                }
            }
            add_file(&f->wrl, name, size, mtime);
        }
    }
    f->wr.active = 1;
    f->wr.id = id;
    f->wr.cur = -1;
    f->wr.count = f->wrl.n;
    strncpy(f->wr.base, path, sizeof f->wr.base - 1);
}

/* The client's digest of a file it is about to upload: go ahead, from the
 * start (this side keeps no partial files to resume). */
static void digest(cdv_files *f, const uint8_t *d, size_t n)
{
    pbr r;
    int id = 0, file_num = 0;
    pbw w;
    pbr_init(&r, d, n);
    while (pbr_next(&r)) {
        if (r.field == 1)
            id = (int)r.v;
        else if (r.field == 2)
            file_num = pb_unzigzag32(r.v);
    }
    if (!cdv_msg_begin(f->s, &w))
        return;
    pbw_begin(&w, M_FILE_ACTION);
    pbw_begin(&w, FA_SEND_CONFIRM);
    pbw_varint(&w, 1, (uint32_t)id);
    pbw_sint(&w, 2, file_num);
    pbw_varint_always(&w, 4, 0); /* offset_blk (a oneof): from the first block */
    pbw_end(&w);
    pbw_end(&w);
    cdv_msg_end(f->s, &w);
}

static void close_current(cdv_files *f, int complete)
{
    if (f->wr.cur >= 0 && f->wr.cur < f->wrl.n) {
        f->fs->close_write(f->fs->u, f->wr.h, f->wrl.files[f->wr.cur].mtime, complete);
        f->wr.cur = -1;
    }
}

static void block(cdv_files *f, const uint8_t *d, size_t n)
{
    pbr r;
    int id = 0, file_num = 0, compressed = 0;
    const uint8_t *data = NULL;
    size_t len = 0;
    pbr_init(&r, d, n);
    while (pbr_next(&r)) {
        if (r.field == 1)
            id = (int)r.v;
        else if (r.field == 2)
            file_num = pb_unzigzag32(r.v);
        else if (r.field == 3 && r.wire == PB_LEN) {
            data = r.data;
            len = r.len;
        } else if (r.field == 4)
            compressed = r.v != 0;
    }
    if (!f->wr.active || id != f->wr.id || file_num < 0 || file_num >= f->wrl.n)
        return;
    if (file_num != f->wr.cur) {
        char path[768];
        close_current(f, 1);
        join(path, sizeof path, f->wr.base, f->wrl.names + f->wrl.files[file_num].name);
        if (f->fs->open_write(f->fs->u, path, &f->wr.h) < 0) {
            send_error(f, id, f->fs->error(f->fs->u), file_num);
            return;
        }
        f->wr.cur = file_num;
    }
    if (compressed && len) {
        long k = cdv_unzstd(f->work, f->workcap, data, len);
        if (k < 0) {
            send_error(f, id, "A block could not be decompressed", file_num);
            return;
        }
        data = f->work;
        len = (size_t)k;
    }
    if (len && f->fs->write(f->fs->u, f->wr.h, data, len) < 0)
        send_error(f, id, f->fs->error(f->fs->u), file_num);
}

static void upload_done(cdv_files *f, const uint8_t *d, size_t n)
{
    pbr r;
    int id = 0, file_num = 0;
    pbr_init(&r, d, n);
    while (pbr_next(&r)) {
        if (r.field == 1)
            id = (int)r.v;
        else if (r.field == 2)
            file_num = pb_unzigzag32(r.v);
    }
    if (!f->wr.active || id != f->wr.id)
        return;
    close_current(f, 1);
    f->wr.active = 0;
    send_done(f, id, file_num);
}

/* ---- the rest ----------------------------------------------------------------------- */

typedef struct {
    int id, hidden, recursive, file_num;
    char path[512], name[256];
} action_args;

static void to_message(cdv_files *f, void *ctx, const char *rel, const cdv_entry *e)
{
    (void)ctx;
    put_entry(f->w, e, rel);
}

/* Every file under a folder, straight into the reply (the client uses it to
 * delete a folder: each file, then the folders). */
static void all_files(cdv_files *f, const action_args *a)
{
    pbw w;
    cdv_entry e;
    int rc;
    if (f->fs->stat(f->fs->u, a->path, &e) < 0) {
        send_error(f, a->id, f->fs->error(f->fs->u), -1);
        return;
    }
    if (!cdv_msg_begin(f->s, &w))
        return;
    pbw_begin(&w, M_FILE_RESPONSE);
    pbw_begin(&w, FR_DIR);
    pbw_varint(&w, 1, (uint32_t)a->id);
    pbw_string(&w, 2, a->path);
    f->w = &w;
    if (e.type == ENT_FILE) {
        put_entry(&w, &e, "");
        rc = 0;
    } else {
        rc = walk(f, a->path, a->hidden, to_message, NULL);
    }
    f->w = NULL;
    pbw_end(&w);
    pbw_end(&w);
    if (rc < 0 || w.overflow || f->too_many) {
        send_error(f, a->id, rc < 0 ? f->fs->error(f->fs->u) : "Too many files", -1);
        return;
    }
    cdv_msg_end(f->s, &w);
}

static void parse_args(const uint8_t *d, size_t n, action_args *a, int name_field)
{
    pbr r;
    memset(a, 0, sizeof *a);
    pbr_init(&r, d, n);
    while (pbr_next(&r)) {
        if (r.wire == PB_LEN && (r.field == 2 || r.field == (unsigned)name_field)) {
            char *dst = r.field == 2 ? a->path : a->name;
            size_t cap = r.field == 2 ? sizeof a->path : sizeof a->name;
            size_t k = r.len < cap - 1 ? r.len : cap - 1;
            memcpy(dst, r.data, k);
            dst[k] = 0;
        } else if (r.field == 1 && r.wire == PB_VARINT) {
            a->id = (int)r.v;
        } else if (r.field == 3 && r.wire == PB_VARINT) {
            a->hidden = a->recursive = (int)r.v; /* include_hidden / recursive / file_num */
            a->file_num = pb_unzigzag32(r.v);
        }
    }
}

static void action(cdv_files *f, const uint8_t *d, size_t n)
{
    pbr r;
    pbr_init(&r, d, n);
    while (pbr_next(&r)) {
        action_args a;
        if (r.wire != PB_LEN)
            continue;
        switch (r.field) {
        case FA_READ_DIR: {
            /* ReadDir has no id: path 1, include_hidden 2. */
            pbr q;
            char path[512] = "";
            int hidden = 0;
            pbr_init(&q, r.data, r.len);
            while (pbr_next(&q)) {
                if (q.field == 1 && q.wire == PB_LEN) {
                    size_t k = q.len < sizeof path - 1 ? q.len : sizeof path - 1;
                    memcpy(path, q.data, k);
                    path[k] = 0;
                } else if (q.field == 2) {
                    hidden = q.v != 0;
                }
            }
            read_dir(f, path, hidden);
            break;
        }
        case FA_SEND:
            start_send(f, r.data, r.len);
            break;
        case FA_RECEIVE:
            start_receive(f, r.data, r.len);
            break;
        case FA_ALL_FILES:
            parse_args(r.data, r.len, &a, 0);
            all_files(f, &a);
            break;
        case FA_CREATE:
            parse_args(r.data, r.len, &a, 0);
            result(f, f->fs->mkdir(f->fs->u, a.path) == 0, a.id, 0);
            break;
        case FA_REMOVE_DIR:
            parse_args(r.data, r.len, &a, 0);
            result(f, f->fs->remove_dir(f->fs->u, a.path, a.recursive) == 0, a.id, 0);
            break;
        case FA_REMOVE_FILE:
            parse_args(r.data, r.len, &a, 0);
            result(f, f->fs->remove_file(f->fs->u, a.path) == 0, a.id, a.file_num);
            break;
        case FA_RENAME:
            parse_args(r.data, r.len, &a, 3);
            result(f, f->fs->rename(f->fs->u, a.path, a.name) == 0, a.id, 0);
            break;
        case FA_CANCEL:
            parse_args(r.data, r.len, &a, 0);
            if (f->rd.active && f->rd.id == a.id) {
                if (f->rd.opened)
                    f->fs->close_read(f->fs->u, f->rd.h);
                f->rd.active = 0;
            }
            if (f->wr.active && f->wr.id == a.id) {
                close_current(f, 0);
                f->wr.active = 0;
            }
            break;
        case FA_READ_EMPTY_DIRS: {
            /* Empty folders to recreate on the other side: none offered. */
            pbw w;
            pbr q;
            pbr_init(&q, r.data, r.len);
            if (!cdv_msg_begin(f->s, &w))
                break;
            pbw_begin(&w, M_FILE_RESPONSE);
            pbw_begin(&w, FR_EMPTY_DIRS);
            while (pbr_next(&q))
                if (q.field == 1 && q.wire == PB_LEN)
                    pbw_bytes(&w, 1, q.data, q.len);
            pbw_end(&w);
            pbw_end(&w);
            cdv_msg_end(f->s, &w);
            break;
        }
        default:
            break;
        }
    }
}

static void response(cdv_files *f, const uint8_t *d, size_t n)
{
    pbr r;
    pbr_init(&r, d, n);
    while (pbr_next(&r)) {
        if (r.wire != PB_LEN)
            continue;
        if (r.field == FR_BLOCK)
            block(f, r.data, r.len);
        else if (r.field == FR_DONE)
            upload_done(f, r.data, r.len);
        else if (r.field == FR_DIGEST)
            digest(f, r.data, r.len);
    }
}

void cdv_files_message(cdv_files *f, int field, const uint8_t *d, size_t n)
{
    if (field == M_FILE_ACTION)
        action(f, d, n);
    else if (field == M_FILE_RESPONSE)
        response(f, d, n);
}

void cdv_files_start(cdv_files *f)
{
    read_dir(f, f->s->ft_dir, f->s->ft_hidden);
}

void cdv_files_close(cdv_files *f)
{
    if (f->rd.active && f->rd.opened)
        f->fs->close_read(f->fs->u, f->rd.h);
    f->rd.active = 0;
    close_current(f, 0);
    f->wr.active = 0;
}
