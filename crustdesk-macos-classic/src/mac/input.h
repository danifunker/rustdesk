/* Mouse and keyboard injection through the low-memory globals and the event
 * queue -- the techniques ChromiVNC and MiniVNC established.
 *
 * Mouse position: write MTemp and RawMouse, then copy CrsrCouple to CrsrNew so
 * the cursor task picks it up. Buttons: MBState plus a posted mouseDown or
 * mouseUp, with MBTicks held in the future so the ROM's debounce does not
 * overwrite the state before the Toolbox reads it (MiniVNC's discovery; it is
 * what makes dragging and menus work, not only clicks).
 *
 * Keys arrive as Mac virtual keycodes in Map mode, because the session tells
 * the peer this is "Mac OS" -- and those are the ADB codes a classic Mac uses,
 * so the character comes from KeyTranslate over the current KCHR. Legacy-mode
 * characters are mapped back to a keycode through the same KCHR.
 */
#ifndef CDV_INPUT_H
#define CDV_INPUT_H

#include "../core/session.h"

#include <Multiverse.h>

void input_init(int screen_w, int screen_h);
/* The KCHR to translate keycodes with; the main loop keeps it current. */
void input_set_kchr(Ptr kchr);
void input_mouse(int mask, int x, int y);
void input_key(const cdv_key *k);
/* Nonzero once after the peer pressed Command-C or Command-X. */
int input_take_copy(void);

/* Let go of everything the peer held down. */
void input_release_all(void);

#endif
