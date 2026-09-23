/* Mac Roman <-> UTF-8, for the clipboard. The table (bytes 0x80-0xFF as
 * Unicode) is generated from Python's mac_roman codec. */
#ifndef MACROMAN_H
#define MACROMAN_H

#include <stddef.h>
#include <stdint.h>

/* Mac Roman to UTF-8; CR becomes LF. Returns bytes written (at most cap). */
size_t macroman_to_utf8(const uint8_t *in, size_t n, uint8_t *out, size_t cap);
/* UTF-8 to Mac Roman; LF and CRLF become CR, unmappable characters '?'. */
size_t utf8_to_macroman(const uint8_t *in, size_t n, uint8_t *out, size_t cap);

#endif
