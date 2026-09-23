/* The pointer, when it is not in the picture.
 *
 * Most 68k video draws the pointer into VRAM in software, so the capture has
 * it. From Mac OS 8.6, a card with a hardware cursor (most ATI cards in G3s
 * and G4s) draws it as an overlay the framebuffer never sees; then the peer
 * needs its shape and position sent separately.
 */
#ifndef CDV_CURSOR_H
#define CDV_CURSOR_H

#include <Multiverse.h>
#include <stdint.h>

/* Main loop: nonzero if the driver says it is drawing a hardware cursor. */
int cursor_is_hardware(short driver_refnum);

/* Engine: the current pointer from TheCrsr, as 16 x 16 RGBA, and a checksum
 * that changes when the shape does. Black where the image is set, white where
 * only the mask is, clear elsewhere; inverting pixels come out black. */
uint32_t cursor_shape(uint8_t rgba[16 * 16 * 4], int *hotx, int *hoty);

/* Engine: where the pointer is. */
void cursor_where(int *x, int *y);

#endif
