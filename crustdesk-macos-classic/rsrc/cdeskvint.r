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
    isHighLevelEventAware,
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
 * 18 New Password, then 19/20 the name (first on screen, last in number),
 * 21 keep awake. src/mac/settings.c knows these numbers. */
resource 'DLOG' (200, "Settings") {
    { 0, 0, 348, 540 },
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
        { 316, 458, 336, 528 },
        Button { enabled, "Save" };
        { 316, 374, 336, 444 },
        Button { enabled, "Cancel" };
        { 41, 12, 57, 104 },
        StaticText { disabled, "ID server:" };
        { 40, 112, 56, 528 },
        EditText { enabled, "" };
        { 67, 12, 83, 104 },
        StaticText { disabled, "Server key:" };
        { 66, 112, 82, 528 },
        EditText { enabled, "" };
        { 93, 12, 109, 104 },
        StaticText { disabled, "Relay:" };
        { 92, 112, 108, 528 },
        EditText { enabled, "" };
        { 119, 12, 135, 104 },
        StaticText { disabled, "Console:" };
        { 118, 112, 134, 528 },
        EditText { enabled, "" };
        { 145, 12, 161, 104 },
        StaticText { disabled, "Password:" };
        { 144, 112, 160, 528 },
        EditText { enabled, "" };
        { 171, 12, 187, 104 },
        StaticText { disabled, "Direct port:" };
        { 170, 112, 186, 528 },
        EditText { enabled, "" };
        { 197, 12, 213, 104 },
        StaticText { disabled, "Quality:" };
        { 196, 112, 212, 528 },
        EditText { enabled, "" };
        { 248, 12, 306, 528 },
        StaticText { disabled, "Name: shown in the console and to peers. Relay: empty for the one the ID server names. Console: the API server's URL, or empty. Quality: 0 best to 127 smallest." };
        { 316, 12, 336, 132 },
        Button { enabled, "New Password" };
        { 15, 12, 31, 104 },
        StaticText { disabled, "Name:" };
        { 14, 112, 30, 528 },
        EditText { enabled, "" };
        { 222, 112, 240, 528 },
        CheckBox { enabled, "Keep this Mac awake while sharing" };
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
    { 0, 0, 212, 400 },
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
        { 180, 310, 200, 388 },
        Button { enabled, "OK" };
        { 12, 64, 170, 388 },
        StaticText { disabled, "C-Desk-Vint 0.1d\rRustDesk for classic Mac OS: System 7.5.5 to Mac OS 9.2.2, 68k and PowerPC (^1).\r\rThis Mac's ID: ^0\r\rUses libsodium (ISC) and BearSSL (MIT)." };
        { 12, 16, 44, 48 },
        Icon { disabled, 128 };
    }
};

/* Chat: 1 Send, 2 the reply, 3 the history (drawn by src/mac/main.c). */
resource 'DLOG' (500, "Chat") {
    { 0, 0, 236, 360 },
    noGrowDocProc,
    invisible,
    goAway,
    0x0,
    500,
    "Chat",
    alertPositionMainScreen
};

resource 'DITL' (500) {
    {
        { 206, 284, 226, 350 },
        Button { enabled, "Send" };
        { 208, 12, 224, 272 },
        EditText { enabled, "" };
        { 10, 10, 196, 350 },
        UserItem { disabled };
    }
};
