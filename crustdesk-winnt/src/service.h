/* The agent as an NT service: LocalSystem, allowed to interact with the
 * desktop (so it reaches the interactive window station, the logon screen
 * included), started with Windows. */
#ifndef CDV_NT_SERVICE_H
#define CDV_NT_SERVICE_H

#include <stddef.h>

enum { SVC_ABSENT = -1, SVC_UNKNOWN = 0 };  /* else a SERVICE_* state */

int service_state(void);
/* The program path the service runs, if installed. */
int service_path(char *out, size_t cap);
/* On NT it also names "exe -tray" under HKLM\...\Run, so the tray program
 * (tray.c) starts at every logon; removing takes that away again. */
int service_install(const char *exe, char *err, size_t cap);
int service_remove(char *err, size_t cap);
/* Wait (up to ~20 s) for the state to settle. */
int service_start(char *err, size_t cap);
int service_stop(char *err, size_t cap);
/* Start the tray program for whoever is running this, now (NT). */
void service_tray_now(const char *exe);

/* Hand this process to the service control manager: returns once the
 * service has stopped (0 if this was not started as a service). */
int service_dispatch(int verbose);

#endif
