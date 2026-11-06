/* The terminal's shell: see shell.h.
 *
 * cmd on pipes, measured on NT 4.0: it prints its prompt, reads a line, then
 * writes the line itself before its output. So keys are echoed here while
 * they are typed, and on Enter that echo is rubbed out again and cmd's stands.
 * There is no console to send Ctrl+C to: a program that will not end is
 * ended by pressing it twice, which restarts the shell. */
#include "shell.h"
#include "plat.h"
#include "text.h"

#include "win.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HISTORY (64 * 1024)
#define LINE_MAX_BYTES 1024
#define CTRLC_AGAIN_MS 2000

static shell_out_fn out_fn;
static shell_exit_fn ended_fn;
static CRITICAL_SECTION lock;
static int ready;
static char history[HISTORY];
static size_t history_len;

static HANDLE child, in_w, out_r, reader;
static unsigned long child_pid;
static volatile LONG stopping;

static char line[LINE_MAX_BYTES];
static size_t line_len;
static int esc_state; /* in an escape sequence the keys send: 1 ESC, 2 CSI */
static DWORD ctrlc_at;

/* To the peer, and into the history. */
static void show(const char *s, size_t n)
{
    EnterCriticalSection(&lock);
    if (n >= HISTORY) {
        memcpy(history, s + n - HISTORY, HISTORY);
        history_len = HISTORY;
    } else {
        if (history_len + n > HISTORY) {
            size_t drop = history_len + n - HISTORY;
            memmove(history, history + drop, history_len - drop);
            history_len -= drop;
        }
        memcpy(history + history_len, s, n);
        history_len += n;
    }
    LeaveCriticalSection(&lock);
    out_fn(s, n);
}

static void say(const char *s)
{
    show(s, strlen(s));
}

/* What cmd writes: the OEM code page, to UTF-8. */
static DWORD WINAPI read_thread(LPVOID pipe)
{
    /* on its own stack: one left behind by a restart may still be running */
    char raw[1024];
    WCHAR wide[1024];
    char utf8[3 * 1024];
    DWORD got;
    while (ReadFile((HANDLE)pipe, raw, sizeof raw, &got, NULL) && got) {
        int w = MultiByteToWideChar(CP_OEMCP, 0, raw, (int)got, wide, 1024);
        size_t n = utf16_to_utf8(wide, w > 0 ? (size_t)w : 0, utf8, sizeof utf8);
        show(utf8, n);
    }
    if (!stopping && ended_fn)
        ended_fn();
    return 0;
}

void shell_init(shell_out_fn out, shell_exit_fn ended)
{
    out_fn = out;
    ended_fn = ended;
    if (!ready)
        InitializeCriticalSection(&lock);
    ready = 1;
}

int shell_running(void)
{
    return child != NULL;
}

unsigned long shell_start(void)
{
    SECURITY_ATTRIBUTES sa;
    HANDLE pin_r = NULL, pin_w = NULL, pout_r = NULL, pout_w = NULL, me = GetCurrentProcess();
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    char cmd[MAX_PATH];
    if (child)
        return child_pid;
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&pin_r, &pin_w, &sa, 0))
        return 0;
    if (!CreatePipe(&pout_r, &pout_w, &sa, 0)) {
        CloseHandle(pin_r);
        CloseHandle(pin_w);
        return 0;
    }
    /* our ends must not go to cmd, or it would never see end of file */
    DuplicateHandle(me, pin_w, me, &in_w, 0, FALSE, DUPLICATE_SAME_ACCESS);
    DuplicateHandle(me, pout_r, me, &out_r, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CloseHandle(pin_w);
    CloseHandle(pout_r);
    if (!GetEnvironmentVariable("ComSpec", cmd, sizeof cmd))
        strcpy(cmd, "cmd.exe");
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = pin_r;
    si.hStdOutput = pout_w;
    si.hStdError = pout_w;
    if (!CreateProcess(NULL, cmd, NULL, NULL, TRUE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) {
        CloseHandle(pin_r);
        CloseHandle(pout_w);
        CloseHandle(in_w);
        CloseHandle(out_r);
        in_w = out_r = NULL;
        return 0;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pin_r);
    CloseHandle(pout_w);
    child = pi.hProcess;
    child_pid = pi.dwProcessId;
    InterlockedExchange((LONG *)&stopping, 0);
    line_len = 0;
    esc_state = 0;
    reader = plat_thread(read_thread, out_r);
    return child_pid;
}

/* Everything of the last cmd let go of. */
static void reap(void)
{
    InterlockedExchange((LONG *)&stopping, 1);
    if (child) {
        TerminateProcess(child, 1);
        WaitForSingleObject(child, 3000);
        CloseHandle(child);
    }
    if (in_w)
        CloseHandle(in_w);
    if (reader) {
        /* the pipe breaks with cmd, unless a program it started holds it:
         * closing our end cannot free a blocked read, so it is left behind */
        if (WaitForSingleObject(reader, 2000) == WAIT_OBJECT_0 && out_r)
            CloseHandle(out_r);
        CloseHandle(reader);
    } else if (out_r) {
        CloseHandle(out_r);
    }
    child = in_w = out_r = reader = NULL;
    child_pid = 0;
}

void shell_stop(void)
{
    reap();
}

void shell_restart(void)
{
    reap();
    say("\r\n[terminal restarted]\r\n");
    if (!shell_start())
        say("[cmd would not start]\r\n");
}

void shell_exited(void)
{
    reap();
    say("\r\n[the shell ended; starting another]\r\n");
    if (!shell_start())
        say("[cmd would not start]\r\n");
}

/* Rub out what the line shows: a character is one column, whatever its bytes. */
static void erase_line(void)
{
    size_t i;
    for (i = 0; i < line_len; i++)
        if (((unsigned char)line[i] & 0xC0) != 0x80)
            show("\b \b", 3);
    line_len = 0;
}

/* The line to cmd, in its code page. */
static void send_line(void)
{
    static WCHAR wide[LINE_MAX_BYTES + 1];
    static char oem[LINE_MAX_BYTES * 2 + 2];
    size_t w = utf8_to_utf16(line, line_len, wide, LINE_MAX_BYTES + 1);
    int n = WideCharToMultiByte(CP_OEMCP, 0, wide, (int)w, oem, sizeof oem - 2, NULL, NULL);
    DWORD put;
    if (n < 0)
        n = 0;
    oem[n++] = '\r';
    oem[n++] = '\n';
    if (in_w)
        WriteFile(in_w, oem, (DWORD)n, &put, NULL);
}

void shell_input(const char *s, size_t n)
{
    size_t i;
    if (!child)
        return;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (esc_state == 1) { /* ESC [ starts a sequence; the rest are dropped */
            esc_state = c == '[' || c == 'O' ? 2 : 0;
            continue;
        }
        if (esc_state == 2) {
            if (c >= 0x40 && c <= 0x7E)
                esc_state = 0;
            continue;
        }
        if (c == 0x1B) {
            esc_state = 1;
        } else if (c == '\r' || c == '\n') {
            if (c == '\n' && i && s[i - 1] == '\r')
                continue;
            {
                size_t keep = line_len;
                static char copy[LINE_MAX_BYTES];
                memcpy(copy, line, keep);
                erase_line();
                memcpy(line, copy, keep);
                line_len = keep;
                send_line();
                line_len = 0;
            }
        } else if (c == 0x7F || c == 0x08) {
            if (line_len) {
                while (line_len && ((unsigned char)line[line_len - 1] & 0xC0) == 0x80)
                    line_len--;
                if (line_len)
                    line_len--;
                show("\b \b", 3);
            }
        } else if (c == 0x15) { /* Ctrl+U */
            erase_line();
        } else if (c == 0x03) { /* Ctrl+C */
            if (line_len) {
                erase_line();
            } else if (ctrlc_at && GetTickCount() - ctrlc_at < CTRLC_AGAIN_MS) {
                ctrlc_at = 0;
                shell_restart();
                return;
            } else {
                ctrlc_at = GetTickCount();
                say("\r\n[Ctrl+C reaches no program here: press it again to restart the "
                    "terminal]\r\n");
            }
        } else if (c >= 0x20 && line_len < sizeof line) {
            line[line_len++] = (char)c;
            show(s + i, 1);
        }
    }
}

size_t shell_history(char *out, size_t cap)
{
    size_t n;
    EnterCriticalSection(&lock);
    n = history_len < cap ? history_len : cap;
    memcpy(out, history + history_len - n, n);
    LeaveCriticalSection(&lock);
    return n;
}
