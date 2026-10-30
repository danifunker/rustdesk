/* The clipboard, through a thread of its own: setting the clipboard needs a
 * window to own it, and the main thread must own none to follow the input
 * desktop. The clipboard belongs to the window station, so whichever
 * desktop is showing, this is the clipboard the person there uses. */
#ifndef CDV_NT_CLIP_H
#define CDV_NT_CLIP_H

#include <stddef.h>

void clip_start(void);
/* The peer's clipboard (UTF-8): becomes this machine's. */
void clip_from_peer(const char *utf8, size_t n);
/* Text copied here since the last call, as UTF-8 in buf: its length, or -1
 * if nothing new. */
long clip_take(char *buf, size_t cap);
/* Forget what was copied before now (a session is starting). */
void clip_reset(void);

#endif
