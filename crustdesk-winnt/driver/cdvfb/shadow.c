/* cdvfb: NT 4.0's frame buffer display driver with a shadow.
 *
 * On PINT (NT 4.0 on the Raspberry Pi) the frame buffer is uncached memory:
 * reading it is some 60 times slower than reading ordinary memory, so every
 * scroll, every drag and every look of a remote-desktop agent crawls. NT's own
 * driver (framebuf) gives GDI a bitmap that is the frame buffer; this one
 * gives it a device surface, draws everything into a bitmap in ordinary
 * (cached) memory by handing each call to GDI's own Eng routine, as NT's
 * banked VGA drivers do (displays\vga64k), and then copies the rectangle that
 * changed out to the frame buffer. Nothing ever reads the frame buffer.
 *
 * The rest of the driver is framebuf's, with the few edits of stage.py.
 * If the shadow cannot be had (no memory), it is framebuf: see
 * hsurfShadowEnable. */

#include "driver.h"

#undef RtlMoveMemory
VOID NTAPI RtlMoveMemory(PVOID Destination, CONST VOID *Source, ULONG Length);

#define SHADOW_HOOKS (HOOK_BITBLT | HOOK_TEXTOUT | HOOK_COPYBITS | HOOK_STROKEPATH | HOOK_PAINT)

VOID ShadowPrint(PCHAR pszFormat, ...)
{
    va_list ap;
    va_start(ap, pszFormat);
    EngDebugPrint(STANDARD_DEBUG_PREFIX, pszFormat, ap);
    va_end(ap);
}

/* The surface GDI draws on: a device surface over a bitmap in ordinary
 * memory laid out as the frame buffer is. Without that memory, framebuf's
 * surface: the frame buffer as a bitmap, nothing hooked. */
HSURF hsurfShadowEnable(PPDEV ppdev, SIZEL sizl, ULONG ulBitmapType, FLONG flHooks)
{
    HSURF hsurf, hsurfBm;
    LONG lDelta = ppdev->lDeltaScreen;
    ULONG cj = (ULONG)(lDelta < 0 ? -lDelta : lDelta) * (ULONG)sizl.cy;
    FLONG fl = lDelta > 0 ? BMF_TOPDOWN : 0;

    ppdev->pjShadow = NULL;
    ppdev->psoShadow = NULL;
    ppdev->hsurfShadow = (HSURF)0;

    ppdev->pjShadow = (PBYTE)EngAllocMem(FL_ZERO_MEMORY, cj, ALLOC_TAG);
    if (ppdev->pjShadow != NULL) {
        hsurfBm = (HSURF)EngCreateBitmap(sizl, lDelta, ulBitmapType, fl, (PVOID)ppdev->pjShadow);
        if (hsurfBm != (HSURF)0 && EngAssociateSurface(hsurfBm, ppdev->hdevEng, 0))
            ppdev->psoShadow = EngLockSurface(hsurfBm);
        if (ppdev->psoShadow != NULL) {
            hsurf = EngCreateDeviceSurface((DHSURF)ppdev, sizl, ulBitmapType);
            if (hsurf != (HSURF)0 && EngAssociateSurface(hsurf, ppdev->hdevEng, SHADOW_HOOKS)) {
                ppdev->hsurfShadow = hsurfBm;
                ShadowPrint("shadow of %ld bytes at %p for the frame buffer at %p\n", cj,
                            ppdev->pjShadow, ppdev->pjScreen);
                vShadowFlushAll(ppdev);
                return hsurf;
            }
            if (hsurf != (HSURF)0)
                EngDeleteSurface(hsurf);
            EngUnlockSurface(ppdev->psoShadow);
            ppdev->psoShadow = NULL;
        }
        if (hsurfBm != (HSURF)0)
            EngDeleteSurface(hsurfBm);
        EngFreeMem(ppdev->pjShadow);
        ppdev->pjShadow = NULL;
    }

    ShadowPrint("no shadow: drawing on the frame buffer\n");
    hsurf = (HSURF)EngCreateBitmap(sizl, lDelta, ulBitmapType, fl, (PVOID)ppdev->pjScreen);
    if (hsurf == (HSURF)0)
        return (HSURF)0;
    if (!EngAssociateSurface(hsurf, ppdev->hdevEng, flHooks)) {
        EngDeleteSurface(hsurf);
        return (HSURF)0;
    }
    return hsurf;
}

VOID vShadowDisable(PPDEV ppdev)
{
    if (ppdev->psoShadow != NULL) {
        EngUnlockSurface(ppdev->psoShadow);
        ppdev->psoShadow = NULL;
    }
    if (ppdev->hsurfShadow != (HSURF)0) {
        EngDeleteSurface(ppdev->hsurfShadow);
        ppdev->hsurfShadow = (HSURF)0;
    }
    if (ppdev->pjShadow != NULL) {
        EngFreeMem(ppdev->pjShadow);
        ppdev->pjShadow = NULL;
    }
}

/* Copy a rectangle of the shadow out to the frame buffer. */
VOID vShadowFlush(PPDEV ppdev, RECTL *prcl)
{
    SURFOBJ *pso = ppdev->psoShadow;
    LONG l = prcl->left, t = prcl->top, r = prcl->right, b = prcl->bottom, y;
    ULONG cjPel = ppdev->ulBitCount / 8, cj;
    PBYTE pj;

    if (pso == NULL)
        return;
    if (l > r) {
        y = l;
        l = r;
        r = y;
    }
    if (t > b) {
        y = t;
        t = b;
        b = y;
    }
    if (l < 0)
        l = 0;
    if (t < 0)
        t = 0;
    if (r > (LONG)ppdev->cxScreen)
        r = (LONG)ppdev->cxScreen;
    if (b > (LONG)ppdev->cyScreen)
        b = (LONG)ppdev->cyScreen;
    if (l >= r || t >= b)
        return;

    cj = (ULONG)(r - l) * cjPel;
    pj = (PBYTE)pso->pvScan0 + t * pso->lDelta + (ULONG)l * cjPel;
    for (y = t; y < b; y++) {
        /* the frame buffer is laid out as the shadow is */
        RtlMoveMemory(ppdev->pjScreen + (pj - ppdev->pjShadow), pj, cj);
        pj += pso->lDelta;
    }
}

VOID vShadowFlushAll(PPDEV ppdev)
{
    RECTL rcl;
    rcl.left = 0;
    rcl.top = 0;
    rcl.right = (LONG)ppdev->cxScreen;
    rcl.bottom = (LONG)ppdev->cyScreen;
    vShadowFlush(ppdev, &rcl);
}

/* What was drawn lies inside prcl and inside the clip's bounds. */
static VOID vFlushClipped(PPDEV ppdev, CLIPOBJ *pco, RECTL *prcl)
{
    RECTL rcl;
    LONG v;
    rcl = *prcl;
    if (rcl.left > rcl.right) {
        v = rcl.left;
        rcl.left = rcl.right;
        rcl.right = v;
    }
    if (rcl.top > rcl.bottom) {
        v = rcl.top;
        rcl.top = rcl.bottom;
        rcl.bottom = v;
    }
    if (pco != NULL && pco->iDComplexity != DC_TRIVIAL) {
        if (rcl.left < pco->rclBounds.left)
            rcl.left = pco->rclBounds.left;
        if (rcl.top < pco->rclBounds.top)
            rcl.top = pco->rclBounds.top;
        if (rcl.right > pco->rclBounds.right)
            rcl.right = pco->rclBounds.right;
        if (rcl.bottom > pco->rclBounds.bottom)
            rcl.bottom = pco->rclBounds.bottom;
    }
    vShadowFlush(ppdev, &rcl);
}

/* When the call gives no rectangle: the clip's bounds, else everything. */
static VOID vFlushClip(PPDEV ppdev, CLIPOBJ *pco)
{
    if (pco != NULL && pco->iDComplexity != DC_TRIVIAL)
        vShadowFlush(ppdev, &pco->rclBounds);
    else
        vShadowFlushAll(ppdev);
}

/* Between the screen and the usual 32-bit DIB (blue in the low byte) when
 * the screen's pixels have red there, as the Pi's do, GDI translates pixel
 * by pixel through a function, at some 20 MB/s, reading or drawing. Those
 * copies are done here, a row at a time: red and blue change places. */
static VOID vSwapRect(SURFOBJ *psoDest, SURFOBJ *psoSrc, RECTL *prcl, LONG dx, LONG dy)
{
    LONG cx = prcl->right - prcl->left, x, y;
    ULONG *pulSrc, *pulDst, ul;

    for (y = prcl->top; y < prcl->bottom; y++) {
        pulSrc = (ULONG *)((PBYTE)psoSrc->pvScan0 + (y + dy) * psoSrc->lDelta) + prcl->left + dx;
        pulDst = (ULONG *)((PBYTE)psoDest->pvScan0 + y * psoDest->lDelta) + prcl->left;
        for (x = 0; x < cx; x++) {
            ul = pulSrc[x];
            pulDst[x] = (ul & 0x0000FF00) | ((ul >> 16) & 0xFF) | ((ul & 0xFF) << 16);
        }
    }
}

static BOOL bIntersect(RECTL *prcl, RECTL *prclClip)
{
    if (prcl->left < prclClip->left)
        prcl->left = prclClip->left;
    if (prcl->top < prclClip->top)
        prcl->top = prclClip->top;
    if (prcl->right > prclClip->right)
        prcl->right = prclClip->right;
    if (prcl->bottom > prclClip->bottom)
        prcl->bottom = prclClip->bottom;
    return prcl->left < prcl->right && prcl->top < prcl->bottom;
}

/* FALSE: not such a copy, nothing done. */
static BOOL bCopySwapped(SURFOBJ *psoDest, SURFOBJ *psoSrc, CLIPOBJ *pco, XLATEOBJ *pxlo,
                         RECTL *prclDest, POINTL *pptlSrc)
{
    struct {
        ULONG c;
        RECTL arcl[8];
    } ce;
    RECTL rcl;
    LONG dx = pptlSrc->x - prclDest->left, dy = pptlSrc->y - prclDest->top;
    BOOL bMore;
    ULONG i;

    if (pxlo == NULL || (pxlo->flXlate & XO_TRIVIAL) || psoDest->iType != STYPE_BITMAP ||
        psoSrc->iType != STYPE_BITMAP || psoDest->iBitmapFormat != BMF_32BPP ||
        psoSrc->iBitmapFormat != BMF_32BPP || psoDest->pvScan0 == NULL || psoSrc->pvScan0 == NULL)
        return FALSE;
    rcl = *prclDest;
    if (rcl.left < 0 || rcl.top < 0 || rcl.right > psoDest->sizlBitmap.cx ||
        rcl.bottom > psoDest->sizlBitmap.cy || rcl.left >= rcl.right || rcl.top >= rcl.bottom ||
        rcl.left + dx < 0 || rcl.top + dy < 0 || rcl.right + dx > psoSrc->sizlBitmap.cx ||
        rcl.bottom + dy > psoSrc->sizlBitmap.cy)
        return FALSE;
    if (XLATEOBJ_iXlate(pxlo, 0x000000FF) != 0x00FF0000 ||
        XLATEOBJ_iXlate(pxlo, 0x0000FF00) != 0x0000FF00 ||
        XLATEOBJ_iXlate(pxlo, 0x00FF0000) != 0x000000FF)
        return FALSE;

    if (pco == NULL || pco->iDComplexity == DC_TRIVIAL) {
        vSwapRect(psoDest, psoSrc, &rcl, dx, dy);
    } else if (pco->iDComplexity == DC_RECT) {
        if (bIntersect(&rcl, &pco->rclBounds))
            vSwapRect(psoDest, psoSrc, &rcl, dx, dy);
    } else {
        CLIPOBJ_cEnumStart(pco, FALSE, CT_RECTANGLES, CD_ANY, 0);
        do {
            bMore = CLIPOBJ_bEnum(pco, sizeof ce, (ULONG *)&ce);
            for (i = 0; i < ce.c; i++) {
                rcl = *prclDest;
                if (bIntersect(&rcl, &ce.arcl[i]))
                    vSwapRect(psoDest, psoSrc, &rcl, dx, dy);
            }
        } while (bMore);
    }
    return TRUE;
}

BOOL DrvCopyBits(SURFOBJ *psoDest, SURFOBJ *psoSrc, CLIPOBJ *pco, XLATEOBJ *pxlo, RECTL *prclDest,
                 POINTL *pptlSrc)
{
    PPDEV ppdev = NULL;
    BOOL b;

    if (psoSrc->iType == STYPE_DEVICE)
        psoSrc = ((PPDEV)psoSrc->dhpdev)->psoShadow;
    if (psoDest->iType == STYPE_DEVICE) {
        ppdev = (PPDEV)psoDest->dhpdev;
        psoDest = ppdev->psoShadow;
    }
    b = bCopySwapped(psoDest, psoSrc, pco, pxlo, prclDest, pptlSrc);
    if (!b)
        b = EngCopyBits(psoDest, psoSrc, pco, pxlo, prclDest, pptlSrc);
    if (ppdev != NULL)
        vFlushClipped(ppdev, pco, prclDest);
    return b;
}

BOOL DrvBitBlt(SURFOBJ *psoDest, SURFOBJ *psoSrc, SURFOBJ *psoMask, CLIPOBJ *pco, XLATEOBJ *pxlo,
               RECTL *prclDest, POINTL *pptlSrc, POINTL *pptlMask, BRUSHOBJ *pbo, POINTL *pptlBrush,
               ROP4 rop4)
{
    PPDEV ppdev = NULL;
    BOOL b;

    /* a plain copy (SRCCOPY) is DrvCopyBits', with its swap */
    if (rop4 == 0x0000CCCC && psoSrc != NULL && psoMask == NULL)
        return DrvCopyBits(psoDest, psoSrc, pco, pxlo, prclDest, pptlSrc);

    if (psoSrc != NULL && psoSrc->iType == STYPE_DEVICE)
        psoSrc = ((PPDEV)psoSrc->dhpdev)->psoShadow;
    if (psoDest->iType == STYPE_DEVICE) {
        ppdev = (PPDEV)psoDest->dhpdev;
        psoDest = ppdev->psoShadow;
    }
    b = EngBitBlt(psoDest, psoSrc, psoMask, pco, pxlo, prclDest, pptlSrc, pptlMask, pbo, pptlBrush,
                  rop4);
    if (ppdev != NULL)
        vFlushClipped(ppdev, pco, prclDest);
    return b;
}

BOOL DrvTextOut(SURFOBJ *pso, STROBJ *pstro, FONTOBJ *pfo, CLIPOBJ *pco, RECTL *prclExtra,
                RECTL *prclOpaque, BRUSHOBJ *pboFore, BRUSHOBJ *pboOpaque, POINTL *pptlOrg, MIX mix)
{
    PPDEV ppdev = (PPDEV)pso->dhpdev;
    RECTL rcl;
    BOOL b;

    b = EngTextOut(ppdev->psoShadow, pstro, pfo, pco, prclExtra, prclOpaque, pboFore, pboOpaque,
                   pptlOrg, mix);
    if (prclExtra != NULL) {
        /* underlines and strike-outs, anywhere */
        vFlushClip(ppdev, pco);
        return b;
    }
    rcl = pstro->rclBkGround;
    if (prclOpaque != NULL) {
        if (prclOpaque->left < rcl.left)
            rcl.left = prclOpaque->left;
        if (prclOpaque->top < rcl.top)
            rcl.top = prclOpaque->top;
        if (prclOpaque->right > rcl.right)
            rcl.right = prclOpaque->right;
        if (prclOpaque->bottom > rcl.bottom)
            rcl.bottom = prclOpaque->bottom;
    }
    vFlushClipped(ppdev, pco, &rcl);
    return b;
}

BOOL DrvStrokePath(SURFOBJ *pso, PATHOBJ *ppo, CLIPOBJ *pco, XFORMOBJ *pxo, BRUSHOBJ *pbo,
                   POINTL *pptlBrushOrg, LINEATTRS *plineattrs, MIX mix)
{
    PPDEV ppdev = (PPDEV)pso->dhpdev;
    RECTFX rcfx;
    RECTL rcl;
    BOOL b;

    b = EngStrokePath(ppdev->psoShadow, ppo, pco, pxo, pbo, pptlBrushOrg, plineattrs, mix);
    if (plineattrs != NULL && (plineattrs->fl & LA_GEOMETRIC)) {
        /* a wide line: as wide as its transform says */
        vFlushClip(ppdev, pco);
        return b;
    }
    /* a cosmetic line, one pixel wide: the path's bounds (28.4 fixed point)
     * and a pixel around them */
    PATHOBJ_vGetBounds(ppo, &rcfx);
    rcl.left = (rcfx.xLeft >> 4) - 1;
    rcl.top = (rcfx.yTop >> 4) - 1;
    rcl.right = ((rcfx.xRight + 15) >> 4) + 2;
    rcl.bottom = ((rcfx.yBottom + 15) >> 4) + 2;
    vFlushClipped(ppdev, pco, &rcl);
    return b;
}

BOOL DrvPaint(SURFOBJ *pso, CLIPOBJ *pco, BRUSHOBJ *pbo, POINTL *pptlBrushOrg, MIX mix)
{
    PPDEV ppdev = (PPDEV)pso->dhpdev;
    BOOL b;

    b = EngPaint(ppdev->psoShadow, pco, pbo, pptlBrushOrg, mix);
    vFlushClip(ppdev, pco);
    return b;
}
