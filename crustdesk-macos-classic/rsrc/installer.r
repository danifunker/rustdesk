#include "Processes.r"
#include "MacTypes.r"
#include "Dialogs.r"

resource 'vers' (1) {
    0x00, 0x01, development, 0x00, verUS,
    "0.1d",
    "Installs C-Desk-Vint 0.1d -- RustDesk for classic Mac OS"
};

resource 'SIZE' (-1) {
    reserved, acceptSuspendResumeEvents, reserved, canBackground,
    doesActivateOnFGSwitch, backgroundAndForeground, dontGetFrontClicks,
    ignoreChildDiedEvents, is32BitCompatible, notHighLevelEventAware,
    onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
    reserved, reserved, reserved,
    512 * 1024,
    384 * 1024
};

/* 1 Install, 2 Quit, 3 where (^0), 4 startup, 5 Control Strip, 6 note (^1).
 * src/installer/install.c knows these numbers. */
resource 'DLOG' (300, "Install") {
    { 0, 0, 212, 420 },
    movableDBoxProc,
    invisible,
    noGoAway,
    0x0,
    300,
    "Install C-Desk-Vint",
    alertPositionMainScreen
};

resource 'DITL' (300) {
    {
        { 180, 330, 200, 408 },
        Button { enabled, "Install" };
        { 180, 244, 200, 322 },
        Button { enabled, "Quit" };
        { 12, 12, 60, 408 },
        StaticText { disabled, "C-Desk-Vint, RustDesk for classic Mac OS, will be installed in ^0." };
        { 68, 12, 86, 408 },
        CheckBox { enabled, "Start C-Desk-Vint when this Mac starts up" };
        { 90, 12, 108, 408 },
        CheckBox { enabled, "Add C-Desk-Vint to the Control Strip" };
        { 116, 12, 172, 408 },
        StaticText { disabled, "^1" };
    }
};

resource 'ALRT' (301) {
    { 0, 0, 140, 400 },
    301,
    {
        OK, visible, silent;
        OK, visible, silent;
        OK, visible, silent;
        OK, visible, silent;
    },
    alertPositionMainScreen
};

resource 'DITL' (301) {
    {
        { 108, 290, 128, 388 },
        Button { enabled, "Open It" };
        { 108, 196, 128, 282 },
        Button { enabled, "Done" };
        { 10, 72, 100, 388 },
        StaticText { disabled, "C-Desk-Vint is installed in ^0.^1" };
        { 10, 20, 42, 52 },
        Icon { disabled, 128 };
    }
};

resource 'ALRT' (302) {
    { 0, 0, 120, 400 },
    302,
    {
        OK, visible, sound1;
        OK, visible, sound1;
        OK, visible, sound1;
        OK, visible, sound1;
    },
    alertPositionMainScreen
};

resource 'DITL' (302) {
    {
        { 88, 310, 108, 388 },
        Button { enabled, "OK" };
        { 10, 72, 80, 388 },
        StaticText { disabled, "^0" };
    }
};

resource 'BNDL' (128) {
    'CDVi', 0,
    {
        'ICN#', { 0, 128 };
        'FREF', { 0, 128 };
    }
};

resource 'FREF' (128) {
    'APPL', 0, ""
};

type 'CDVi' as 'STR ';
resource 'CDVi' (0) {
    "Install C-Desk-Vint 0.1d"
};
