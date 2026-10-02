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

/* In IRIX 5.3's libc, not in its headers. */
struct timeval;
int utimes(const char *path, const struct timeval *times);

#endif
