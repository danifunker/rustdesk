/* What MSVC 4 lacks, for code written against C99. Included into every file
 * of the NT build (/FI), the shared core too. */
#ifndef CDV_NT_COMPAT_H
#define CDV_NT_COMPAT_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#define inline __inline
#define __attribute__(x)
/* __declspec(align()) came with Visual C++ 7; the reference crypto needs no
 * alignment beyond the natural one. */
#define CRYPTO_ALIGN(x)
/* Keywords to MSVC (16-bit pointers); the encoder has a variable called near.
 * <windows.h> redefines them to nothing, after which they are not used. */
#define near cdv_near
#define far cdv_far
/* zstd marks old calls with __declspec(deprecated()), from Visual C++ 7 too. */
#define ZSTD_DISABLE_DEPRECATE_WARNINGS

int cdv_snprintf(char *buf, size_t n, const char *fmt, ...);
int cdv_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
#define snprintf cdv_snprintf
#define vsnprintf cdv_vsnprintf

/* Milliseconds on a clock that only goes forward. */
uint32_t cdv_ms(void);

#endif
