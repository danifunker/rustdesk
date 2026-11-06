/* The terminal's shell: cmd.exe, run by the tray as the user logged on, and
 * kept running between sessions so its output can be replayed to a peer that
 * comes back. NT 4.0 has no pseudo-console, so cmd is on pipes: programs that
 * write text work, ones that draw on a console (edit) do not, and this module
 * does what a console would have for the typing -- it echoes keys, takes
 * Backspace, and hands cmd a whole line on Enter. */
#ifndef CDV_NT_SHELL_H
#define CDV_NT_SHELL_H

#include <stddef.h>

/* Text for the peer, UTF-8, from any thread. */
typedef void (*shell_out_fn)(const char *utf8, size_t n);
/* cmd ended by itself, from the reader's thread: call shell_exited(). */
typedef void (*shell_exit_fn)(void);

void shell_init(shell_out_fn out, shell_exit_fn ended);
/* Start cmd if it is not running: its process id, 0 if it would not start. */
unsigned long shell_start(void);
int shell_running(void);
/* End it and start another: what the tray's Restart terminal does. */
void shell_restart(void);
/* cmd ended by itself: say so and start another. On the tray's own thread. */
void shell_exited(void);
void shell_stop(void);
/* The peer's keys, UTF-8. */
void shell_input(const char *utf8, size_t n);
/* The last of what was shown, for a peer that comes back; its length. */
size_t shell_history(char *out, size_t cap);

#endif
