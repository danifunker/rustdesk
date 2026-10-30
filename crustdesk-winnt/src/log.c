#include "log.h"

#include "win.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOG_MAX (512L * 1024)

int cdv_verbose;
static char log_path[MAX_PATH];
static int log_console;
static CRITICAL_SECTION lock;
static int ready;

void cdv_log_open(const char *path, int to_console, int verbose)
{
    if (!ready) {
        InitializeCriticalSection(&lock);
        ready = 1;
    }
    log_path[0] = 0;
    if (path) {
        strncpy(log_path, path, sizeof log_path - 1);
        log_path[sizeof log_path - 1] = 0;
    }
    log_console = to_console;
    cdv_verbose = verbose;
}

static void emit(const char *fmt, va_list ap)
{
    char line[1024];
    SYSTEMTIME t;
    int n;
    GetLocalTime(&t);
    n = snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d ", t.wYear, t.wMonth, t.wDay,
                 t.wHour, t.wMinute, t.wSecond);
    vsnprintf(line + n, sizeof line - n - 2, fmt, ap);
    strcat(line, "\r\n");
    if (!ready)
        return;
    EnterCriticalSection(&lock);
    if (log_console) {
        DWORD w;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, (DWORD)strlen(line), &w, NULL);
    }
    if (log_path[0]) {
        HANDLE f = CreateFile(log_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD w, size = GetFileSize(f, NULL);
            if (size != 0xffffffff && size > LOG_MAX) {
                char old[MAX_PATH + 8];
                CloseHandle(f);
                strcpy(old, log_path);
                strcat(old, ".old");
                DeleteFile(old);
                MoveFile(log_path, old);
                f = CreateFile(log_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            }
            if (f != INVALID_HANDLE_VALUE) {
                SetFilePointer(f, 0, NULL, FILE_END);
                WriteFile(f, line, (DWORD)strlen(line), &w, NULL);
                CloseHandle(f);
            }
        }
    }
    LeaveCriticalSection(&lock);
}

void cdv_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit(fmt, ap);
    va_end(ap);
}

void cdv_debug(const char *fmt, ...)
{
    va_list ap;
    if (!cdv_verbose)
        return;
    va_start(ap, fmt);
    emit(fmt, ap);
    va_end(ap);
}
