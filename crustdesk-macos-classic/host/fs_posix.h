#ifndef FS_POSIX_H
#define FS_POSIX_H
#include "../src/core/files.h"
/* A file system for the file-transfer protocol: `dir` as the client's "/". */
const cdv_fs_ops *fs_posix(const char *dir);
#endif
