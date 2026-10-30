/* The agent's log: a file beside the program (C-Desk-Vint.log, the last
 * one kept as .old once it passes 512 KB), and the console when there is
 * one (-run). Any thread may log. */
#ifndef CDV_NT_LOG_H
#define CDV_NT_LOG_H

void cdv_log_open(const char *path, int to_console, int verbose);
void cdv_log(const char *fmt, ...);
/* Only with -v. */
void cdv_debug(const char *fmt, ...);
extern int cdv_verbose;

#endif
