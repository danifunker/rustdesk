/* The agent and the tray program ("C-Desk-Vint -tray", tray.c) talk over a
 * named pipe, \\.\pipe\C-Desk-Vint, in messages: a type byte, then UTF-8.
 *
 * The tray runs as whoever is logged on, so it can put windows on their
 * desktop (chat) and, later, run things as them. The agent accepts it only
 * if it is that user: after the tray's hello the agent impersonates it and
 * compares its account with the owner of the shell (Explorer), the person at
 * the console. Anyone may open the pipe; only the agent may serve it.
 *
 * NT only: Windows 95 and 98 cannot serve a named pipe, so there is no tray
 * there yet. */
#ifndef CDV_NT_LINK_H
#define CDV_NT_LINK_H

#include <stddef.h>

#define LINK_PIPE "\\\\.\\pipe\\C-Desk-Vint"
#define LINK_MAX 8192 /* the longest message, type byte included */

/* tray -> agent */
#define LINK_HELLO 'H'      /* the first message: the link's version, "1" */
#define LINK_CHAT 'C'       /* a chat line for the peer */
#define LINK_DISCONNECT 'D' /* end every remote session */
#define LINK_TERM_READY 'Y' /* the shell is up: "pid replay" (shell.h) */
#define LINK_TERM_OUT 'W'   /* what the shell shows, UTF-8 */

/* agent -> tray */
#define LINK_STATUS 'S'     /* the agent's status lines (as in C-Desk-Vint.sta) */
#define LINK_PEER_CHAT 'M'  /* a chat line from the peer: "name\ntext" */
#define LINK_NOTE 'N'       /* a line for the chat window from the agent itself */
#define LINK_LOCK 'L'       /* lock the workstation (Windows 2000 and later) */
#define LINK_REFUSED 'R'    /* why the agent will not talk to this tray, then it hangs up */
#define LINK_TERM_OPEN 'O'  /* a peer opened the terminal */
#define LINK_TERM_IN 'I'    /* the peer's keys, UTF-8 */

/* agent only: the tray connected and was accepted (from link_take) */
#define LINK_JOINED 'J'

/* The agent's side. link_start: a thread that serves the pipe; 0 where there
 * is none (9x, or the pipe is taken). Messages are queued both ways and
 * never block the caller. */
int link_start(void);
void link_stop(void);
int link_connected(void);
void link_send(int type, const char *data, size_t n);
/* The next message from the tray: its length (data in out, terminated), its
 * type in *type; -1 when there is none. */
long link_take(int *type, char *out, size_t cap);
/* Who may connect: the console user's SID (NULL: nobody yet). */
void link_set_console_sid(const void *sid);

#endif
