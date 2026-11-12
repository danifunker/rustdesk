#!/usr/bin/env python3
"""Stage cdvfb, the shadow frame buffer display driver for PINT, as a component
of PINT's build.

    stage.py NTROOT OUTDIR

NTROOT holds the NT 4.0 tree (NTROOT/private/ntos/w32/ntgdi/displays/framebuf).
OUTDIR becomes the component (PINT: RASPI/drivers/cdvfb, built with
`ROOT=/work/RASPI/drivers build/make-driver.sh cdvfb`): NT's framebuf with the
edits below, and shadow.c from beside this script. This repository carries no
NT source: only the edits.
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = "private/ntos/w32/ntgdi/displays/framebuf"

# file -> [(old, new)]; each old must be there exactly once
EDITS = {
    "driver.h": [
        ("    BOOL    bSupportDCI;                // Does the miniport support DCI?\n",
         "    BOOL    bSupportDCI;                // Does the miniport support DCI?\n"
         "    PBYTE   pjShadow;                   // cdvfb: the shadow's memory, or NULL\n"
         "    HSURF   hsurfShadow;                // cdvfb: the shadow as a bitmap\n"
         "    SURFOBJ *psoShadow;                 // cdvfb: that bitmap, locked\n"
         "    BOOL    bPtrVisible;                // cdvfb: its pointer is on the screen\n"
         "    POINTL  ptlPtr;                     // cdvfb: the pointer's top left\n"
         "    POINTL  ptlPtrHot;                  // cdvfb: its hot spot\n"
         "    SIZEL   sizlPtr;                    // cdvfb: its size, 0 when there is none\n"
         "    ULONG   *pulPtrAnd;                 // cdvfb: what stays of the screen\n"
         "    ULONG   *pulPtrXor;                 // cdvfb: what is put on it\n"
         "    PBYTE   pjDirty;                    // cdvfb: a bit a tile drawn on since asked\n"
         "    ULONG   cxTiles;                    // cdvfb: tiles across\n"
         "    ULONG   cyTiles;                    // cdvfb: tiles down\n"
         "    ULONG   cjDirty;                    // cdvfb: bytes of pjDirty\n"),
        ("VOID vDisableSURF(PPDEV);\n",
         "VOID vDisableSURF(PPDEV);\n"
         "HSURF hsurfShadowEnable(PPDEV, SIZEL, ULONG, FLONG);\n"
         "VOID vShadowDisable(PPDEV);\n"
         "VOID vShadowFlush(PPDEV, RECTL *);\n"
         "VOID vShadowFlushAll(PPDEV);\n"
         "VOID ShadowPrint(PCHAR, ...);\n"
         "VOID FbMovePointer(SURFOBJ *, LONG, LONG, RECTL *);\n"
         "ULONG FbSetPointerShape(SURFOBJ *, SURFOBJ *, SURFOBJ *, XLATEOBJ *, LONG, LONG, LONG,\n"
         "                        LONG, RECTL *, FLONG);\n"
         "#define PTR_MAX 64                      // cdvfb: the largest pointer it draws\n"
         "VOID vShadowOut(PPDEV, RECTL *);\n"
         "#define DIRTY_TILE 16                   // cdvfb: pixels a side of a tile of pjDirty\n"
         "#define CDVFB_ESC_TAKE 0x43445630       // cdvfb: the escape that says what changed\n"),
        ('L"framebuf"', 'L"cdvfb"'),
        ('"FRAMEBUF: "', '"CDVFB: "'),
    ],
    "enable.c": [
        ("    {   INDEX_DrvGetModes,              (PFN) DrvGetModes           }\n",
         "    {   INDEX_DrvGetModes,              (PFN) DrvGetModes           },\n"
         "    {   INDEX_DrvCopyBits,              (PFN) DrvCopyBits           },\n"
         "    {   INDEX_DrvBitBlt,                (PFN) DrvBitBlt             },\n"
         "    {   INDEX_DrvTextOut,               (PFN) DrvTextOut            },\n"
         "    {   INDEX_DrvStrokePath,            (PFN) DrvStrokePath         },\n"
         "    {   INDEX_DrvPaint,                 (PFN) DrvPaint              },\n"
         "    {   INDEX_DrvEscape,                (PFN) DrvEscape             }\n"),
        # DrvEnableSurface: the surface is the shadow's
        ("""    hsurf = (HSURF) EngCreateBitmap(sizl,
                                    ppdev->lDeltaScreen,
                                    ulBitmapType,
                                    (ppdev->lDeltaScreen > 0) ? BMF_TOPDOWN : 0,
                                        (PVOID) (ppdev->pjScreen));

    if (hsurf == (HSURF) 0)
    {
        RIP("DISP DrvEnableSurface failed EngCreateBitmap\\n");
        return(FALSE);
    }

    if (!EngAssociateSurface(hsurf, ppdev->hdevEng, flHooks))
    {
        RIP("DISP DrvEnableSurface failed EngAssociateSurface\\n");
        EngDeleteSurface(hsurf);
        return(FALSE);
    }
""",
         """    hsurf = hsurfShadowEnable(ppdev, sizl, ulBitmapType, flHooks);

    if (hsurf == (HSURF) 0)
    {
        RIP("DISP DrvEnableSurface failed to make its surface\\n");
        return(FALSE);
    }
"""),
        ("""    EngDeleteSurface(((PPDEV) dhpdev)->hsurfEng);
    vDisableSURF((PPDEV) dhpdev);
""",
         """    EngDeleteSurface(((PPDEV) dhpdev)->hsurfEng);
    vShadowDisable((PPDEV) dhpdev);
    vDisableSURF((PPDEV) dhpdev);
"""),
        # DrvAssertMode: the screen is ours again, and holds anything
        ("""        return (bInitSURF(ppdev, FALSE));
""",
         """        if (!bInitSURF(ppdev, FALSE))
        {
            return FALSE;
        }

        vShadowFlushAll(ppdev);
        return TRUE;
"""),
    ],
    # framebuf's pointer, a miniport's hardware one: cdvfb's own calls it
    # when there is no shadow
    "pointer.c": [
        ("VOID DrvMovePointer\n", "VOID FbMovePointer\n"),
        ("ULONG DrvSetPointerShape\n", "ULONG FbSetPointerShape\n"),
    ],
    "framebuf.rc": [
        ('"Framebuffer Display Driver"', '"C-Desk-Vint shadow frame buffer display driver"'),
        ('#define VER_INTERNALNAME_STR        "framebuf.dll"',
         '#define VER_INTERNALNAME_STR        "cdvfb.dll"'),
        ('#define VER_ORIGINALFILENAME_STR    "framebuf.dll"',
         '#define VER_ORIGINALFILENAME_STR    "cdvfb.dll"'),
    ],
}
COPIED = ["color.c", "debug.c", "debug.h", "palette.c", "screen.c", "makefile"]
RENAMED = {"framebuf.rc": "cdvfb.rc"}

SOURCES = """MAJORCOMP=ntos
MINORCOMP=cdvfb

TARGETNAME=cdvfb
TARGETPATH=obj
TARGETTYPE=GDI_DRIVER

INCLUDES=$(BASEDIR)\\private\\ntos\\w32\\ntgdi\\displays\\inc;$(BASEDIR)\\private\\ntos\\w32\\ntgdi\\inc

SOURCES=color.c     \\
        debug.c     \\
        enable.c    \\
        palette.c   \\
        pointer.c   \\
        screen.c    \\
        shadow.c    \\
        cdvfb.rc
"""


def text(path):
    return open(path, "rb").read().replace(b"\r", b"").replace(b"\x1a", b"").decode("latin-1")


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    src = os.path.join(sys.argv[1], SRC)
    out = sys.argv[2]
    os.makedirs(out, exist_ok=True)
    for name in COPIED + list(EDITS):
        t = text(os.path.join(src, name))
        for old, new in EDITS.get(name, []):
            if t.count(old) != 1:
                raise SystemExit("stage.py: %s: expected once, found %d times:\n%s" % (name, t.count(old), old))
            t = t.replace(old, new)
        with open(os.path.join(out, RENAMED.get(name, name)), "w", newline="\r\n", encoding="latin-1") as f:
            f.write(t)
    shutil.copy(os.path.join(HERE, "shadow.c"), os.path.join(out, "shadow.c"))
    with open(os.path.join(out, "sources"), "w", newline="\r\n") as f:
        f.write(SOURCES)
    print("staged cdvfb in", out)


if __name__ == "__main__":
    main()
