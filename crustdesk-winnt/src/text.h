/* UTF-8, which the protocol speaks, and UTF-16, which NT does. Written out
 * rather than asked of MultiByteToWideChar, whose CP_UTF8 is not on every
 * NT 4.0. Both always terminate the output and return its length. */
#ifndef CDV_NT_TEXT_H
#define CDV_NT_TEXT_H

#include <stddef.h>
#include "win.h"

size_t utf8_to_utf16(const char *s, size_t n, WCHAR *out, size_t cap);
size_t utf16_to_utf8(const WCHAR *w, size_t n, char *out, size_t cap);

#endif
