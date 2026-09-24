#include "Processes.r"
#include "MacTypes.r"
#include "Dialogs.r"

resource 'vers' (1) {
    0x00, 0x01, development, 0x00, verUS,
    "0.1d",
    "C-Desk-Vint 0.1d -- RustDesk for classic Mac OS"
};

/* The screen is copied twice (a shadow for change detection and the YUV
 * planes the encoder reads) and the encoder keeps its own reconstruction:
 * about 2 MB at 640x480x8, 11 MB at 1280x1024 in millions. What the
 * partition cannot hold comes from temporary memory (src/mac/mem.c). */
resource 'SIZE' (-1) {
    reserved,
    acceptSuspendResumeEvents,
    reserved,
    canBackground,
    doesActivateOnFGSwitch,
    backgroundAndForeground,
    dontGetFrontClicks,
    ignoreChildDiedEvents,
    is32BitCompatible,
    notHighLevelEventAware,
    onlyLocalHLEvents,
    notStationeryAware,
    dontUseTextEditServices,
    reserved,
    reserved,
    reserved,
    12 * 1024 * 1024,
    3 * 1024 * 1024
};

/* Settings. Items: 1 Save, 2 Cancel, then label/field pairs (4 ID server,
 * 6 key, 8 relay, 10 console, 12 password, 14 port, 16 quality), 17 help,
 * 18 New Password. src/mac/settings.c knows these numbers. */
resource 'DLOG' (200, "Settings") {
    { 0, 0, 294, 416 },
    movableDBoxProc,
    invisible,
    noGoAway,
    0x0,
    200,
    "C-Desk-Vint Settings",
    alertPositionMainScreen
};

resource 'DITL' (200) {
    {
        { 262, 334, 282, 404 },
        Button { enabled, "Save" };
        { 262, 250, 282, 320 },
        Button { enabled, "Cancel" };
        { 15, 12, 31, 104 },
        StaticText { disabled, "ID server:" };
        { 14, 112, 30, 404 },
        EditText { enabled, "" };
        { 41, 12, 57, 104 },
        StaticText { disabled, "Server key:" };
        { 40, 112, 56, 404 },
        EditText { enabled, "" };
        { 67, 12, 83, 104 },
        StaticText { disabled, "Relay:" };
        { 66, 112, 82, 404 },
        EditText { enabled, "" };
        { 93, 12, 109, 104 },
        StaticText { disabled, "Console:" };
        { 92, 112, 108, 404 },
        EditText { enabled, "" };
        { 119, 12, 135, 104 },
        StaticText { disabled, "Password:" };
        { 118, 112, 134, 404 },
        EditText { enabled, "" };
        { 145, 12, 161, 104 },
        StaticText { disabled, "Direct port:" };
        { 144, 112, 160, 404 },
        EditText { enabled, "" };
        { 171, 12, 187, 104 },
        StaticText { disabled, "Quality:" };
        { 170, 112, 186, 404 },
        EditText { enabled, "" };
        { 194, 12, 252, 404 },
        StaticText { disabled, "Relay: empty to use the one the ID server names. Console: the API server whose device list shows this Mac (empty for none). Quality: 0 best, 127 smallest." };
        { 262, 12, 282, 132 },
        Button { enabled, "New Password" };
    }
};

/* The Finder's side: the icon family is rsrc/icons.r (tools/make-icons.py).
 * The file needs its "has bundle" flag for the Finder to read these;
 * tools/fatmerge.py sets it. */
resource 'BNDL' (128) {
    'CDVt', 0,
    {
        'ICN#', { 0, 128 };
        'FREF', { 0, 128 };
    }
};

resource 'FREF' (128) {
    'APPL', 0, ""
};

type 'CDVt' as 'STR ';
resource 'CDVt' (0) {
    "C-Desk-Vint 0.1d -- RustDesk for classic Mac OS"
};

/* About C-Desk-Vint: ^0 the ID, ^1 which half and when it was built. */
resource 'ALRT' (400) {
    { 0, 0, 170, 380 },
    400,
    {
        OK, visible, silent;
        OK, visible, silent;
        OK, visible, silent;
        OK, visible, silent;
    },
    alertPositionMainScreen
};

resource 'DITL' (400) {
    {
        { 138, 290, 158, 368 },
        Button { enabled, "OK" };
        { 12, 64, 130, 368 },
        StaticText { disabled, "C-Desk-Vint 0.1d\rRustDesk for classic Mac OS: System 7.5.5 to Mac OS 9.2.2, 68k and PowerPC (^1).\r\rThis Mac's ID: ^0\r\rUses libsodium (ISC) and BearSSL (MIT)." };
        { 12, 16, 44, 48 },
        Icon { disabled, 128 };
    }
};
