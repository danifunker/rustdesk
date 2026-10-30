/* The agent: registers with the ID server, listens, and serves sessions
 * until `stop` is signalled. The service runs it (service.c), and -run runs
 * it in a console window for testing. */
#ifndef CDV_NT_AGENT_H
#define CDV_NT_AGENT_H

#include "win.h"

#ifndef CDV_BUILD
#define CDV_BUILD "dev"
#endif
#define VERSION_STRING "C-Desk-Vint for Windows NT " CDV_BUILD

int agent_run(HANDLE stop, int console, int verbose);

#endif
