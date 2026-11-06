/* The agent's side of the screen hooks (hook.h): the shared block, the DLL
 * beside the program (written there from the copy inside it, where the build
 * put one), and a thread on the Default desktop holding the hooks. Off by
 * the setting hook=0 (the settings window, -hook off), or when the DLL is not
 * there: the screen is then read the old way, every look. */
#ifndef CDV_NT_HOOKS_H
#define CDV_NT_HOOKS_H

#include <stdint.h>

int hooks_start(int width, int height);
void hooks_stop(void);
/* The hooks are in and marking. */
int hooks_active(void);
/* The screen changed size. */
void hooks_set_screen(int width, int height);
/* The macroblocks drawn on since last taken: want[my * mbw + mx] = 1 for
 * each (the rest untouched); how many. */
int hooks_take(uint8_t *want, int mbw, int mbh);

/* The DLL inside the program (nmake builds), written beside it if it is not
 * there or differs; 1 if a DLL is there afterwards. */
int hooks_unpack(void);

#endif
