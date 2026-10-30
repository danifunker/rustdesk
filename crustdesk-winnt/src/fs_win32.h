#ifndef CDV_NT_FS_WIN32_H
#define CDV_NT_FS_WIN32_H

#include "files.h"

/* A file system for the file-transfer protocol. `root` "/": the whole
 * machine, as upstream's Windows server shows it -- "/" lists the drives,
 * paths are C:\like\this (either slash). Anything else: that directory is
 * the client's "/", and nothing outside it can be reached. */
const cdv_fs_ops *fs_win32(const char *root);

#endif
