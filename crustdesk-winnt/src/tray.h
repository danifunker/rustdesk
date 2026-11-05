#ifndef CDV_NT_TRAY_H
#define CDV_NT_TRAY_H

#include "win.h"

/* "C-Desk-Vint -tray": the icon by the clock and the chat window, as the
 * user logged on (link.h). Returns when the user exits it. */
int tray_run(HINSTANCE inst);

#endif
