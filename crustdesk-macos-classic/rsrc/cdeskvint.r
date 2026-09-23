#include "Processes.r"
#include "MacTypes.r"

resource 'vers' (1) {
    0x00, 0x01, development, 0x00, verUS,
    "0.1d",
    "C-Desk-Vint 0.1d -- RustDesk for classic Mac OS"
};

/* The screen is copied twice (a shadow for change detection and the YUV
 * planes the encoder reads) and the encoder keeps its own reconstruction:
 * about 2 MB at 640x480x8, 6 MB at 1024x768 in thousands of colours. */
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
    8 * 1024 * 1024,
    3 * 1024 * 1024
};
