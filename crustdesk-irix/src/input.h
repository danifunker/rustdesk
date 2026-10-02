/* The peer's keyboard and pointer, injected with XTEST. */
#ifndef CDV_IRIX_INPUT_H
#define CDV_IRIX_INPUT_H

#include <X11/Xlib.h>
#include "session.h"

void input_init(Display *d);
void input_mouse(int mask, int x, int y);
void input_key(const cdv_key *k);
void input_release_all(void);

#endif
