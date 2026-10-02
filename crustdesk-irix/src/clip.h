/* The clipboard through X selections; see clip.c. */
#ifndef CDV_IRIX_CLIP_H
#define CDV_IRIX_CLIP_H

#include <stddef.h>
#include <X11/Xlib.h>

/* `send` gets text copied on the IRIX side, as UTF-8. */
void clip_init(Display *d, void (*send)(const char *utf8, size_t n));
/* The peer's clipboard (UTF-8): becomes CLIPBOARD and PRIMARY here. */
void clip_from_peer(const char *utf8, size_t n);
/* A client set a selection's owner (reported by XTrap). */
void clip_owner_changed(Atom selection, Window owner);
/* Call often: fetches a changed selection once the owner has settled. */
void clip_tick(void);
/* An event for the clipboard's window? Nonzero if it was handled. */
int clip_event(XEvent *e);

#endif
