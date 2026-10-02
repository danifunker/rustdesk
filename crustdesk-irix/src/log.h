/* The agent's log: stderr, and a file if one is named. */
#ifndef CDV_IRIX_LOG_H
#define CDV_IRIX_LOG_H

void cdv_log_open(const char *path, int verbose);
void cdv_log(const char *fmt, ...);
/* The log file's descriptor (-1 for none): a child keeps it, closing the rest. */
int cdv_log_fd(void);
/* Only with -v. */
void cdv_debug(const char *fmt, ...);
extern int cdv_verbose;

#endif
