#include "mem.h"

#include <string.h>

#define MAX_TEMP 16

static struct {
    Handle h;
    void *p;
    long size;
} temp[MAX_TEMP];

void *big_alloc(long size)
{
    Ptr p = NewPtrClear(size);
    int i;
    if (p)
        return p;
    for (i = 0; i < MAX_TEMP; i++) {
        if (!temp[i].h) {
            OSErr err;
            Handle h = TempNewHandle(size, &err);
            if (!h || err != noErr)
                return NULL;
            HLock(h);
            memset(*h, 0, (size_t)size);
            temp[i].h = h;
            temp[i].p = *h;
            temp[i].size = size;
            return *h;
        }
    }
    return NULL;
}

void big_free(void *p)
{
    int i;
    if (!p)
        return;
    for (i = 0; i < MAX_TEMP; i++)
        if (temp[i].h && temp[i].p == p) {
            HUnlock(temp[i].h);
            DisposeHandle(temp[i].h);
            temp[i].h = NULL;
            temp[i].p = NULL;
            return;
        }
    DisposePtr((Ptr)p);
}

long big_temp_bytes(void)
{
    long n = 0;
    int i;
    for (i = 0; i < MAX_TEMP; i++)
        if (temp[i].h)
            n += temp[i].size;
    return n;
}
