/* The peer's keyboard and pointer, injected with keybd_event and
 * mouse_event (SendInput came with Service Pack 3). They reach whatever
 * desktop the calling thread is on: call from the thread screen_follow()
 * keeps on the input desktop. */
#ifndef CDV_NT_INPUT_H
#define CDV_NT_INPUT_H

#include "session.h"

void input_init(int width, int height);
void input_mouse(int mask, int x, int y);
void input_key(const cdv_key *k);
void input_release_all(void);
/* Ctrl-Alt-Del, which no injected key makes: Winlogon is asked directly. */
void input_sas(void);

#endif
