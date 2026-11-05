#include "service.h"
#include "agent.h"
#include "log.h"
#include "plat.h"
#include "prefs.h"

#include "win.h"
#include <stdio.h>
#include <string.h>

static void say(char *err, size_t cap, const char *what)
{
    DWORD e = GetLastError();
    char msg[200];
    int n;
    msg[0] = 0;
    FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, msg,
                  sizeof msg, NULL);
    for (n = (int)strlen(msg); n > 0 && (msg[n - 1] == '\r' || msg[n - 1] == '\n'); n--)
        msg[n - 1] = 0;
    if (err)
        snprintf(err, cap, "%s: %s (%lu)", what, msg[0] ? msg : "failed", e);
}

/* ---- Windows 95 and 98: no service manager ------------------------------------
 *
 * A program named under RunServices starts at boot, before anyone logs on,
 * and one that calls RegisterServiceProcess lives through a logoff. "Running"
 * is a named mutex the agent holds; "stop" is a named event it waits on. */

#define RUNSERVICES "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunServices"
#define MUTEX_9X "C-Desk-Vint agent"
#define STOP_9X "C-Desk-Vint stop"

static int running_9x(void)
{
    HANDLE m = OpenMutex(SYNCHRONIZE, FALSE, MUTEX_9X);
    if (!m)
        return 0;
    CloseHandle(m);
    return 1;
}

static int path_9x(char *out, size_t cap)
{
    HKEY k;
    char buf[MAX_PATH + 32];
    DWORD type, len = sizeof buf;
    const char *p, *e;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, RUNSERVICES, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return 0;
    if (RegQueryValueEx(k, SERVICE_NAME, NULL, &type, (LPBYTE)buf, &len) != ERROR_SUCCESS ||
        type != REG_SZ) {
        RegCloseKey(k);
        return 0;
    }
    RegCloseKey(k);
    buf[sizeof buf - 1] = 0;
    p = buf;
    if (*p == '"') {
        p++;
        e = strchr(p, '"');
    } else {
        e = strchr(p, ' ');
    }
    if (!e)
        e = p + strlen(p);
    if (out && (size_t)(e - p) < cap) {
        memcpy(out, p, (size_t)(e - p));
        out[e - p] = 0;
    }
    return 1;
}

static int wait_9x(int want, int ms)
{
    for (; ms > 0; ms -= 250) {
        if (running_9x() == want)
            return 1;
        Sleep(250);
    }
    return running_9x() == want;
}

static int start_9x(char *err, size_t cap)
{
    char exe[MAX_PATH], cmd[MAX_PATH + 32];
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    if (running_9x())
        return 1;
    if (!path_9x(exe, sizeof exe)) {
        if (err)
            snprintf(err, cap, "not installed");
        return 0;
    }
    snprintf(cmd, sizeof cmd, "\"%s\" -service", exe);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcess(NULL, cmd, NULL, NULL, FALSE, DETACHED_PROCESS, NULL, NULL, &si, &pi)) {
        say(err, cap, "cannot start the agent");
        return 0;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (!wait_9x(1, 15000)) {
        if (err)
            snprintf(err, cap, "the agent did not start; see C-Desk-Vint.log");
        return 0;
    }
    return 1;
}

static int stop_9x(char *err, size_t cap)
{
    HANDLE ev;
    if (!running_9x())
        return 1;
    ev = OpenEvent(EVENT_MODIFY_STATE, FALSE, STOP_9X);
    if (ev) {
        SetEvent(ev);
        CloseHandle(ev);
    }
    if (!wait_9x(0, 15000)) {
        if (err)
            snprintf(err, cap, "the agent did not stop");
        return 0;
    }
    return 1;
}

static int install_9x(const char *exe, char *err, size_t cap)
{
    HKEY k;
    char cmd[MAX_PATH + 32];
    DWORD disp;
    snprintf(cmd, sizeof cmd, "\"%s\" -service", exe);
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, RUNSERVICES, 0, NULL, 0, KEY_WRITE, NULL, &k, &disp) !=
            ERROR_SUCCESS ||
        RegSetValueEx(k, SERVICE_NAME, 0, REG_SZ, (const BYTE *)cmd, (DWORD)strlen(cmd) + 1) !=
            ERROR_SUCCESS) {
        say(err, cap, "cannot write RunServices");
        return 0;
    }
    RegCloseKey(k);
    return 1;
}

static int remove_9x(char *err, size_t cap)
{
    HKEY k;
    stop_9x(NULL, 0);
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, RUNSERVICES, 0, KEY_WRITE, &k) != ERROR_SUCCESS) {
        say(err, cap, "cannot open RunServices");
        return 0;
    }
    RegDeleteValue(k, SERVICE_NAME);
    RegCloseKey(k);
    return 1;
}

/* Running as the "service": hold the mutex, live through logoffs, stop
 * when the event is set. */
static int run_9x(int verbose)
{
    typedef DWORD(WINAPI * rsp_fn)(DWORD, DWORD);
    rsp_fn rsp = (rsp_fn)GetProcAddress(GetModuleHandle("kernel32.dll"),
                                        "RegisterServiceProcess");
    HANDLE m, ev;
    int r;
    m = CreateMutex(NULL, TRUE, MUTEX_9X);
    if (!m || GetLastError() == ERROR_ALREADY_EXISTS)
        return 1; /* already running */
    ev = CreateEvent(NULL, TRUE, FALSE, STOP_9X);
    if (rsp)
        rsp(0, 1); /* RSP_SIMPLE_SERVICE: a logoff does not end it */
    r = agent_run(ev, 0, verbose);
    CloseHandle(ev);
    ReleaseMutex(m);
    CloseHandle(m);
    return r == 0;
}

/* ---- the tray program, started at every logon (NT) ----------------------------- */

#define RUN_KEY "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run"
#define TRAY_VALUE "C-Desk-Vint tray"

static void tray_autostart(const char *exe)
{
    HKEY k;
    char cmd[MAX_PATH + 32];
    DWORD disp;
    snprintf(cmd, sizeof cmd, "\"%s\" -tray", exe);
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, RUN_KEY, 0, NULL, 0, KEY_WRITE, NULL, &k, &disp) ==
        ERROR_SUCCESS) {
        RegSetValueEx(k, TRAY_VALUE, 0, REG_SZ, (const BYTE *)cmd, (DWORD)strlen(cmd) + 1);
        RegCloseKey(k);
    }
}

static void tray_autostart_remove(void)
{
    HKEY k;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, RUN_KEY, 0, KEY_WRITE, &k) == ERROR_SUCCESS) {
        RegDeleteValue(k, TRAY_VALUE);
        RegCloseKey(k);
    }
}

void service_tray_now(const char *exe)
{
    char cmd[MAX_PATH + 32];
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    if (!plat_nt())
        return;
    snprintf(cmd, sizeof cmd, "\"%s\" -tray", exe);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (CreateProcess(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

static SC_HANDLE open_svc(DWORD access, SC_HANDLE *scm)
{
    SC_HANDLE s;
    *scm = OpenSCManager(NULL, NULL, access & (SERVICE_CHANGE_CONFIG | DELETE)
                                         ? SC_MANAGER_ALL_ACCESS : SC_MANAGER_CONNECT);
    if (!*scm)
        return NULL;
    s = OpenService(*scm, SERVICE_NAME, access);
    if (!s) {
        DWORD e = GetLastError();
        CloseServiceHandle(*scm);
        *scm = NULL;
        SetLastError(e);
    }
    return s;
}

int service_state(void)
{
    SC_HANDLE scm, s;
    SERVICE_STATUS st;
    int r = SVC_UNKNOWN;
    if (!plat_nt())
        return !path_9x(NULL, 0) ? SVC_ABSENT : running_9x() ? SERVICE_RUNNING : SERVICE_STOPPED;
    s = open_svc(SERVICE_QUERY_STATUS, &scm);
    if (!s)
        return GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST ? SVC_ABSENT : SVC_UNKNOWN;
    if (QueryServiceStatus(s, &st))
        r = (int)st.dwCurrentState;
    CloseServiceHandle(s);
    CloseServiceHandle(scm);
    return r;
}

int service_path(char *out, size_t cap)
{
    SC_HANDLE scm, s;
    char buf[4096];
    QUERY_SERVICE_CONFIG *qc = (QUERY_SERVICE_CONFIG *)buf;
    DWORD need;
    int ok = 0;
    if (!plat_nt())
        return path_9x(out, cap);
    s = open_svc(SERVICE_QUERY_CONFIG, &scm);
    if (!s)
        return 0;
    if (QueryServiceConfig(s, qc, sizeof buf, &need)) {
        const char *p = qc->lpBinaryPathName, *e;
        if (*p == '"') {
            p++;
            e = strchr(p, '"');
        } else {
            e = strchr(p, ' ');
        }
        if (!e)
            e = p + strlen(p);
        if ((size_t)(e - p) < cap) {
            memcpy(out, p, (size_t)(e - p));
            out[e - p] = 0;
            ok = 1;
        }
    }
    CloseServiceHandle(s);
    CloseServiceHandle(scm);
    return ok;
}

int service_install(const char *exe, char *err, size_t cap)
{
    SC_HANDLE scm, s;
    char cmd[MAX_PATH + 32];
    if (!plat_nt())
        return install_9x(exe, err, cap);
    scm = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        say(err, cap, "cannot reach the service manager (are you an Administrator?)");
        return 0;
    }
    snprintf(cmd, sizeof cmd, "\"%s\" -service", exe);
    s = CreateService(scm, SERVICE_NAME, SERVICE_DISPLAY, SERVICE_ALL_ACCESS,
                      SERVICE_WIN32_OWN_PROCESS | SERVICE_INTERACTIVE_PROCESS, SERVICE_AUTO_START,
                      SERVICE_ERROR_NORMAL, cmd, NULL, NULL, "Tcpip\0\0", NULL, NULL);
    if (!s && GetLastError() == ERROR_SERVICE_EXISTS) {
        /* already there: point it at this program */
        s = OpenService(scm, SERVICE_NAME, SERVICE_ALL_ACCESS);
        if (s && !ChangeServiceConfig(s, SERVICE_WIN32_OWN_PROCESS | SERVICE_INTERACTIVE_PROCESS,
                                      SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, cmd, NULL, NULL,
                                      "Tcpip\0\0", NULL, NULL, SERVICE_DISPLAY)) {
            say(err, cap, "cannot change the service");
            CloseServiceHandle(s);
            s = NULL;
        }
    } else if (!s) {
        say(err, cap, "cannot install the service");
    }
    if (s) {
        CloseServiceHandle(s);
        tray_autostart(exe);
    }
    CloseServiceHandle(scm);
    return s != NULL;
}

static int wait_for(SC_HANDLE s, DWORD pending, DWORD want)
{
    SERVICE_STATUS st;
    int i;
    for (i = 0; i < 80; i++) {
        if (!QueryServiceStatus(s, &st))
            return 0;
        if (st.dwCurrentState == want)
            return 1;
        if (st.dwCurrentState != pending && i > 4)
            return 0;
        Sleep(250);
    }
    return 0;
}

int service_start(char *err, size_t cap)
{
    SC_HANDLE scm, s;
    int ok;
    if (!plat_nt())
        return start_9x(err, cap);
    s = open_svc(SERVICE_START | SERVICE_QUERY_STATUS, &scm);
    if (!s) {
        say(err, cap, "cannot open the service");
        return 0;
    }
    ok = StartService(s, 0, NULL) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
    if (!ok)
        say(err, cap, "cannot start the service");
    else if (!(ok = wait_for(s, SERVICE_START_PENDING, SERVICE_RUNNING)) && err)
        snprintf(err, cap, "the service did not start; see C-Desk-Vint.log");
    CloseServiceHandle(s);
    CloseServiceHandle(scm);
    return ok;
}

int service_stop(char *err, size_t cap)
{
    SC_HANDLE scm, s;
    SERVICE_STATUS st;
    int ok;
    if (!plat_nt())
        return stop_9x(err, cap);
    s = open_svc(SERVICE_STOP | SERVICE_QUERY_STATUS, &scm);
    if (!s) {
        say(err, cap, "cannot open the service");
        return 0;
    }
    ok = ControlService(s, SERVICE_CONTROL_STOP, &st) ||
         GetLastError() == ERROR_SERVICE_NOT_ACTIVE;
    if (!ok)
        say(err, cap, "cannot stop the service");
    else if (!(ok = wait_for(s, SERVICE_STOP_PENDING, SERVICE_STOPPED)) && err)
        snprintf(err, cap, "the service did not stop");
    CloseServiceHandle(s);
    CloseServiceHandle(scm);
    return ok;
}

int service_remove(char *err, size_t cap)
{
    SC_HANDLE scm, s;
    int ok;
    if (!plat_nt())
        return remove_9x(err, cap);
    service_stop(NULL, 0);
    s = open_svc(DELETE, &scm);
    if (!s) {
        say(err, cap, "cannot open the service");
        return 0;
    }
    ok = DeleteService(s) != 0;
    if (!ok)
        say(err, cap, "cannot remove the service");
    else
        tray_autostart_remove();
    CloseServiceHandle(s);
    CloseServiceHandle(scm);
    return ok;
}

/* ---- running as the service -------------------------------------------------- */

static SERVICE_STATUS_HANDLE status_h;
static SERVICE_STATUS status;
static HANDLE stop_ev;
static int svc_verbose;

static void report(DWORD state, DWORD exitcode, DWORD wait_hint)
{
    static DWORD check = 1;
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS | SERVICE_INTERACTIVE_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted = state == SERVICE_START_PENDING
                                    ? 0 : SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    status.dwWin32ExitCode = exitcode;
    status.dwServiceSpecificExitCode = 0;
    status.dwCheckPoint = state == SERVICE_RUNNING || state == SERVICE_STOPPED ? 0 : check++;
    status.dwWaitHint = wait_hint;
    SetServiceStatus(status_h, &status);
}

static VOID WINAPI handler(DWORD ctl)
{
    if (ctl == SERVICE_CONTROL_STOP || ctl == SERVICE_CONTROL_SHUTDOWN) {
        report(SERVICE_STOP_PENDING, NO_ERROR, 10000);
        SetEvent(stop_ev);
        return;
    }
    report(status.dwCurrentState, NO_ERROR, 0);
}

static VOID WINAPI service_main(DWORD argc, LPSTR *argv)
{
    int r;
    (void)argc;
    (void)argv;
    status_h = RegisterServiceCtrlHandler(SERVICE_NAME, handler);
    if (!status_h)
        return;
    stop_ev = CreateEvent(NULL, TRUE, FALSE, NULL);
    report(SERVICE_START_PENDING, NO_ERROR, 5000);
    report(SERVICE_RUNNING, NO_ERROR, 0);
    r = agent_run(stop_ev, 0, svc_verbose);
    report(SERVICE_STOPPED, r ? ERROR_SERVICE_SPECIFIC_ERROR : NO_ERROR, 0);
}

int service_dispatch(int verbose)
{
    SERVICE_TABLE_ENTRY t[2];
    if (!plat_nt())
        return run_9x(verbose);
    svc_verbose = verbose;
    t[0].lpServiceName = SERVICE_NAME;
    t[0].lpServiceProc = service_main;
    t[1].lpServiceName = NULL;
    t[1].lpServiceProc = NULL;
    return StartServiceCtrlDispatcher(t) != 0;
}
