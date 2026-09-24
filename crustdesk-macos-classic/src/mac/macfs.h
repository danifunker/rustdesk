/* The Mac's disks for file transfer (see macfs.c). Main loop only. */
#ifndef CDV_MACFS_H
#define CDV_MACFS_H

#include "../core/files.h"

const cdv_fs_ops *macfs(void);
/* Make a folder and any missing above it: 0, or -1. */
int mac_mkdir(const char *path);

#endif
