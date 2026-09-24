#include "unzstd.h"

#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"

long cdv_unzstd(void *dst, size_t cap, const void *src, size_t n)
{
    static ZSTD_DCtx *dctx;
    size_t r;
    if (!dctx)
        dctx = ZSTD_createDCtx();
    if (!dctx)
        return -1;
    r = ZSTD_decompressDCtx(dctx, dst, cap, src, n);
    return ZSTD_isError(r) ? -1 : (long)r;
}
