/* File transfer: the controlled side of RustDesk's file manager, over any
 * file system that supplies cdv_fs_ops.
 *
 * A client opens a session of its own for files (LoginRequest.file_transfer)
 * and sends FileActions: list a directory, send me this (a download), take
 * these (an upload), make, remove or rename. This answers them the way
 * hbb_common's fs.rs and the server's connection manager do:
 *
 *   read_dir     -> FileResponse.dir {path, entries}
 *   send         -> dir {id, path, the files, relative names}, then blocks
 *                   {id, file_num, data} for each file in turn (a last empty
 *                   one for each, which is how an empty file gets made), then
 *                   done {id, file_num = the number of files}
 *   receive      -> a digest from the client for each file, answered with
 *                   send_confirm {offset 0}; blocks (zstd when smaller); the
 *                   client's done -> our done
 *   create, remove_dir, remove_file, rename -> done or error
 *
 * No overwrite detection is offered for downloads (no digest first), which a
 * client accepts; uploads always overwrite. Paths are UTF-8 with '/'.
 *
 * Driven from wherever the file system may be called: on the Mac, the main
 * loop, never interrupt time.
 */
#ifndef CDV_FILES_H
#define CDV_FILES_H

#include "session.h"

#include <stddef.h>
#include <stdint.h>

enum { ENT_DIR = 0, ENT_DRIVE = 3, ENT_FILE = 4 }; /* FileType */

typedef struct {
    int type;          /* ENT_* */
    int hidden;
    uint64_t size;
    uint64_t mtime;    /* seconds since 1970, UTC */
    char name[256];    /* UTF-8 */
} cdv_entry;

typedef struct {
    void *u;
    /* List `path` ("" or "~": home), calling each() per entry, and write the
     * path actually listed to real. 0, or -1 (then error() says why). */
    int (*list)(void *u, const char *path, int hidden, char *real, size_t cap,
                void (*each)(void *ctx, const cdv_entry *e), void *ctx);
    int (*stat)(void *u, const char *path, cdv_entry *e);
    int (*open_read)(void *u, const char *path, void **h);
    long (*read)(void *u, void *h, uint8_t *buf, size_t n); /* 0 at the end, -1 error */
    void (*close_read)(void *u, void *h);
    int (*open_write)(void *u, const char *path, void **h); /* makes parents */
    int (*write)(void *u, void *h, const uint8_t *buf, size_t n);
    /* complete: all of it arrived (else the partial file may go). */
    int (*close_write)(void *u, void *h, uint64_t mtime, int complete);
    int (*mkdir)(void *u, const char *path);                /* and parents */
    int (*remove_file)(void *u, const char *path);
    int (*remove_dir)(void *u, const char *path, int recursive);
    int (*rename)(void *u, const char *path, const char *new_name);
    const char *(*error)(void *u);
} cdv_fs_ops;

typedef struct cdv_files cdv_files;
size_t cdv_files_size(void);

/* `work`: a buffer for a block, compressed and not (at least 160 KB, which
 * is what a client's 128 KB blocks need). */
void cdv_files_init(cdv_files *f, cdv_session *s, const cdv_fs_ops *fs, uint8_t *work,
                    size_t workcap);
/* After the login: the directory the client asked to start in. */
void cdv_files_start(cdv_files *f);
/* From the session's file hook. */
void cdv_files_message(cdv_files *f, int field, const uint8_t *d, size_t n);
/* Queue the next download block if the session has room; nonzero while a
 * download is going. */
int cdv_files_pump(cdv_files *f);
/* The session ended: let go of open files. */
void cdv_files_close(cdv_files *f);

#endif
