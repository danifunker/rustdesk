/* What IRIX 5.3's libc lacks, for code written against C99. Included into
 * every file of the IRIX build (-include), the shared core too. */
#ifndef CDV_IRIX_COMPAT_H
#define CDV_IRIX_COMPAT_H

#include <stdarg.h>
#include <stddef.h>

int cdv_snprintf(char *buf, size_t n, const char *fmt, ...);
int cdv_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
#define snprintf cdv_snprintf
#define vsnprintf cdv_vsnprintf

/* Milliseconds on a clock that only goes forward. IRIX 5.3 has no monotonic
 * clock, and deadlines kept on gettimeofday stall for as long as the time is
 * set back (setting the Indigo's clock 3 hours back stopped the ID server
 * retries for 3 hours). A step back, or forward by over a minute, is
 * absorbed; the main loop reads the clock several times a second. */
#include <stdint.h>
uint32_t cdv_ms(void);

/* In IRIX 5.3's libc, not in its headers. */
struct timeval;
int utimes(const char *path, const struct timeval *times);

#endif
