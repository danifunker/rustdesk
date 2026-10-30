/* The settings window: who this machine is to RustDesk, the service, and
 * the settings the agent reads (prefs.h). It edits C-Desk-Vint.cfg beside
 * the program and restarts the service to apply it; what the agent is
 * doing comes back through C-Desk-Vint.sta.
 *
 * Run from anywhere else (a floppy, a download folder), it offers to
 * install: the program into Program Files, the service, and a Start Menu
 * entry for this window. */
#include "ui.h"
#include "agent.h"
#include "prefs.h"
#include "rng.h"
#include "service.h"
#include "resource.h"

#include "win.h"
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TIMER_ID 1

static HINSTANCE inst;
static int pw_shown;
static char last_status[1024];

static void get_text(HWND d, int id, char *out, int cap)
{
    GetDlgItemText(d, id, out, cap);
    out[cap - 1] = 0;
}

static void trim(char *s)
{
    char *p = s;
    size_t n;
    while (*p == ' ' || *p == '\t')
        p++;
    memmove(s, p, strlen(p) + 1);
    n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
}

static void fill(HWND d)
{
    char id[32];
    prefs_format_id(prefs.id, id, sizeof id);
    SetDlgItemText(d, IDC_ID, id);
    SetDlgItemText(d, IDC_PASSWORD, prefs.password);
    SetDlgItemText(d, IDC_SERVER, prefs.server);
    SetDlgItemText(d, IDC_KEY, prefs.key);
    SetDlgItemText(d, IDC_RELAY, prefs.relay);
    SetDlgItemText(d, IDC_API, prefs.api);
    SetDlgItemInt(d, IDC_PORT, (UINT)prefs.port, FALSE);
    CheckDlgButton(d, IDC_LAN, prefs.lan ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemText(d, IDC_FILES, prefs.files);
    SetDlgItemInt(d, IDC_QUALITY, (UINT)prefs.q, FALSE);
}

/* The dialog into prefs; 0 (and a message) if something is not right. */
static int take(HWND d)
{
    char pw[64], buf[256];
    BOOL ok;
    UINT v;
    get_text(d, IDC_PASSWORD, pw, sizeof pw);
    trim(pw);
    if (strlen(pw) < 6 || strlen(pw) >= sizeof prefs.password) {
        MessageBox(d, "The password must be 6 to 32 characters long.", "C-Desk-Vint",
                   MB_ICONEXCLAMATION);
        return 0;
    }
    v = GetDlgItemInt(d, IDC_PORT, &ok, FALSE);
    if (!ok || v > 65535) {
        MessageBox(d, "The direct IP port is a number from 0 (off) to 65535.", "C-Desk-Vint",
                   MB_ICONEXCLAMATION);
        return 0;
    }
    prefs.port = (int)v;
    v = GetDlgItemInt(d, IDC_QUALITY, &ok, FALSE);
    if (!ok || v > 127) {
        MessageBox(d, "Picture quality is a number from 0 to 127.", "C-Desk-Vint",
                   MB_ICONEXCLAMATION);
        return 0;
    }
    prefs.q = (int)v;
    strcpy(prefs.password, pw);
    get_text(d, IDC_SERVER, buf, sizeof prefs.server);
    trim(buf);
    strcpy(prefs.server, buf);
    get_text(d, IDC_KEY, buf, sizeof prefs.key);
    trim(buf);
    strcpy(prefs.key, buf);
    get_text(d, IDC_RELAY, buf, sizeof prefs.relay);
    trim(buf);
    strcpy(prefs.relay, buf);
    get_text(d, IDC_API, buf, sizeof prefs.api);
    trim(buf);
    strcpy(prefs.api, buf);
    get_text(d, IDC_FILES, buf, sizeof prefs.files);
    trim(buf);
    strcpy(prefs.files, buf[0] ? buf : "off");
    prefs.lan = IsDlgButtonChecked(d, IDC_LAN) == BST_CHECKED;
    return 1;
}

static const char *state_name(int st)
{
    switch (st) {
    case SVC_ABSENT: return "not installed";
    case SERVICE_STOPPED: return "stopped";
    case SERVICE_START_PENDING: return "starting";
    case SERVICE_STOP_PENDING: return "stopping";
    case SERVICE_RUNNING: return "running";
    case SERVICE_PAUSED: return "paused";
    }
    return "unknown (are you an Administrator?)";
}

/* One value from the agent's status lines. */
static int status_value(const char *text, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    const char *p = text;
    while (p && *p) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *e = strpbrk(p + kl + 1, "\r\n");
            size_t n = e ? (size_t)(e - p - kl - 1) : strlen(p + kl + 1);
            if (n >= cap)
                n = cap - 1;
            memcpy(out, p + kl + 1, n);
            out[n] = 0;
            return 1;
        }
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    return 0;
}

static void refresh(HWND d)
{
    int st = service_state();
    char line[256], text[1024], v[128], sessions[256];
    const char *p;
    snprintf(line, sizeof line, "The agent service is %s.", state_name(st));
    SetDlgItemText(d, IDC_SVCSTATE, line);
    EnableWindow(GetDlgItem(d, IDC_INSTALL), st == SVC_ABSENT);
    EnableWindow(GetDlgItem(d, IDC_START), st == SERVICE_STOPPED);
    EnableWindow(GetDlgItem(d, IDC_STOP), st == SERVICE_RUNNING);
    EnableWindow(GetDlgItem(d, IDC_REMOVE), st != SVC_ABSENT && st != SVC_UNKNOWN);

    text[0] = 0;
    if (st == SERVICE_RUNNING && prefs_status_read(text, sizeof text) &&
        !status_value(text, "stopped", v, sizeof v)) {
        if (!prefs.server[0])
            snprintf(line, sizeof line, "No ID server: reachable by IP address only.");
        else if (status_value(text, "registered", v, sizeof v) && v[0] == '1')
            snprintf(line, sizeof line, "Ready: registered with %s.", prefs.server);
        else if (status_value(text, "refused", v, sizeof v) && v[0] == '1')
            snprintf(line, sizeof line, "%s refused this ID (is the key right?).", prefs.server);
        else
            snprintf(line, sizeof line, "Not yet registered with %s...", prefs.server);
        /* and who is connected */
        sessions[0] = 0;
        for (p = text; (p = strstr(p, "session=")) != NULL; p += 8) {
            const char *e = strpbrk(p + 8, "\r\n");
            size_t n = e ? (size_t)(e - p - 8) : strlen(p + 8);
            if (strlen(sessions) + n + 4 < sizeof sessions) {
                strcat(sessions, sessions[0] ? "; " : " Connected: ");
                strncat(sessions, p + 8, n);
            }
        }
        strncat(line, sessions, sizeof line - strlen(line) - 1);
    } else if (st == SVC_ABSENT) {
        snprintf(line, sizeof line, "Install the service to make this machine reachable.");
    } else {
        line[0] = 0;
    }
    if (strcmp(line, last_status)) {
        SetDlgItemText(d, IDC_STATUS, line);
        strcpy(last_status, line);
    }
}

static void error_box(HWND d, const char *what, const char *err)
{
    char msg[512];
    snprintf(msg, sizeof msg, "%s\n\n%s", what, err);
    MessageBox(d, msg, "C-Desk-Vint", MB_ICONSTOP);
}

/* ---- installing ------------------------------------------------------------ */

static int reg_string(HKEY root, const char *key, const char *name, char *out, DWORD cap)
{
    HKEY k;
    DWORD type, len = cap;
    LONG r;
    if (RegOpenKeyEx(root, key, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return 0;
    r = RegQueryValueEx(k, name, NULL, &type, (LPBYTE)out, &len);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
        return 0;
    out[cap - 1] = 0;
    return 1;
}

static void install_dir(char *out, size_t cap)
{
    char pf[MAX_PATH];
    if (!reg_string(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion",
                    "ProgramFilesDir", pf, sizeof pf))
        strcpy(pf, "C:\\Program Files");
    snprintf(out, cap, "%s\\C-Desk-Vint\\", pf);
}

/* Everyone's Start Menu programs; on a 9x machine without user profiles
 * there is only the one, the current user's. */
static int programs_dir(char *out, DWORD cap)
{
    return reg_string(HKEY_LOCAL_MACHINE,
                      "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders",
                      "Common Programs", out, cap) ||
           reg_string(HKEY_CURRENT_USER,
                      "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders",
                      "Programs", out, cap);
}

/* A shortcut to the settings window in everyone's Start Menu. */
static void make_shortcut(const char *exe)
{
    char dir[MAX_PATH], lnk[MAX_PATH];
    WCHAR wlnk[MAX_PATH];
    IShellLink *sl;
    IPersistFile *pf;
    if (!programs_dir(dir, sizeof dir))
        return;
    snprintf(lnk, sizeof lnk, "%s\\C-Desk-Vint.lnk", dir);
    MultiByteToWideChar(CP_ACP, 0, lnk, -1, wlnk, MAX_PATH);
    if (FAILED(CoInitialize(NULL)))
        return;
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLink,
                                   (void **)&sl))) {
        sl->lpVtbl->SetPath(sl, exe);
        sl->lpVtbl->SetDescription(sl, "C-Desk-Vint settings");
        sl->lpVtbl->SetIconLocation(sl, exe, 0);
        if (SUCCEEDED(sl->lpVtbl->QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
            pf->lpVtbl->Save(pf, wlnk, TRUE);
            pf->lpVtbl->Release(pf);
        }
        sl->lpVtbl->Release(sl);
    }
    CoUninitialize();
}

static void remove_shortcut(void)
{
    char dir[MAX_PATH], lnk[MAX_PATH];
    if (!programs_dir(dir, sizeof dir))
        return;
    snprintf(lnk, sizeof lnk, "%s\\C-Desk-Vint.lnk", dir);
    DeleteFile(lnk);
}

/* Install: into Program Files unless this copy is there already; then the
 * service, started. 1 if this window should now close (the installed
 * copy's took over). */
static int do_install(HWND d)
{
    char dir[MAX_PATH], exe[MAX_PATH], self[MAX_PATH], err[300], cfg_from[MAX_PATH],
        cfg_to[MAX_PATH];
    int moved = 0;
    install_dir(dir, sizeof dir);
    GetModuleFileName(NULL, self, sizeof self);
    if (_strnicmp(prefs_dir(), dir, strlen(dir))) {
        char q[600];
        size_t n = strlen(dir);
        snprintf(q, sizeof q, "Install C-Desk-Vint in %.*s, and start it with Windows?",
                 (int)(n > 3 ? n - 1 : n), dir);
        if (MessageBox(d, q, "C-Desk-Vint", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
            return 0;
        CreateDirectory(dir, NULL);
        snprintf(exe, sizeof exe, "%sC-Desk-Vint.exe", dir);
        if (!CopyFile(self, exe, FALSE)) {
            snprintf(err, sizeof err, "Cannot copy the program there (error %lu).", GetLastError());
            error_box(d, "Not installed.", err);
            return 0;
        }
        /* the settings come too: this machine keeps its ID */
        prefs_path(cfg_from, sizeof cfg_from, "C-Desk-Vint.cfg");
        snprintf(cfg_to, sizeof cfg_to, "%sC-Desk-Vint.cfg", dir);
        if (GetFileAttributes(cfg_to) == 0xffffffff)
            CopyFile(cfg_from, cfg_to, TRUE);
        moved = 1;
    } else {
        strcpy(exe, self);
    }
    if (!service_install(exe, err, sizeof err)) {
        error_box(d, "The service could not be installed.", err);
        return 0;
    }
    make_shortcut(exe);
    if (!service_start(err, sizeof err))
        error_box(d, "Installed, but the service did not start.", err);
    if (moved) {
        STARTUPINFO si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        if (CreateProcess(exe, NULL, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return 1;
        }
    }
    return 0;
}

static void apply(HWND d)
{
    char err[300];
    if (!take(d))
        return;
    if (!prefs_save()) {
        char msg[400];
        snprintf(msg, sizeof msg, "Cannot save the settings in %s (error %lu).", prefs_dir(),
                 GetLastError());
        MessageBox(d, msg, "C-Desk-Vint", MB_ICONSTOP);
        return;
    }
    if (service_state() == SERVICE_RUNNING) {
        SetDlgItemText(d, IDC_STATUS, "Restarting the agent...");
        last_status[0] = 0;
        UpdateWindow(d);
        if (!service_stop(err, sizeof err) || !service_start(err, sizeof err))
            error_box(d, "The agent could not be restarted.", err);
    }
    refresh(d);
}

/* "Windows NT", "Windows 98"...: the title says which this is. */
static const char *windows_name(void)
{
    OSVERSIONINFO v;
    v.dwOSVersionInfoSize = sizeof v;
    if (!GetVersionEx(&v) || v.dwPlatformId == VER_PLATFORM_WIN32_NT)
        return "Windows NT";
    return v.dwMinorVersion >= 90 ? "Windows Me" : v.dwMinorVersion >= 10 ? "Windows 98"
                                                                           : "Windows 95";
}

static BOOL CALLBACK dlg_proc(HWND d, UINT m, WPARAM wp, LPARAM lp)
{
    char err[300];
    (void)lp;
    switch (m) {
    case WM_INITDIALOG: {
        HICON icon = LoadIcon(inst, MAKEINTRESOURCE(IDI_CDV));
        char title[128];
        SendMessage(d, WM_SETICON, ICON_BIG, (LPARAM)icon);
        SendMessage(d, WM_SETICON, ICON_SMALL, (LPARAM)icon);
        snprintf(title, sizeof title, "C-Desk-Vint for %s (%s)", windows_name(), CDV_BUILD);
        SetWindowText(d, title);
        SendDlgItemMessage(d, IDC_PASSWORD, EM_SETPASSWORDCHAR, '*', 0);
        fill(d);
        refresh(d);
        SetTimer(d, TIMER_ID, 1000, NULL);
        return TRUE;
    }
    case WM_TIMER:
        refresh(d);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SHOWPW:
            pw_shown = !pw_shown;
            SendDlgItemMessage(d, IDC_PASSWORD, EM_SETPASSWORDCHAR, pw_shown ? 0 : '*', 0);
            SetDlgItemText(d, IDC_SHOWPW, pw_shown ? "Hide" : "Show");
            InvalidateRect(GetDlgItem(d, IDC_PASSWORD), NULL, TRUE);
            return TRUE;
        case IDC_NEWPW: {
            char pw[9];
            prefs_gather_entropy();
            prefs_new_password(pw);
            SetDlgItemText(d, IDC_PASSWORD, pw);
            if (!pw_shown)
                SendMessage(d, WM_COMMAND, IDC_SHOWPW, 0);
            SetDlgItemText(d, IDC_STATUS, "A new password: Apply to use it.");
            last_status[0] = 0;
            return TRUE;
        }
        case IDC_INSTALL:
            if (take(d) && prefs_save() && do_install(d))
                EndDialog(d, 0);
            refresh(d);
            return TRUE;
        case IDC_START:
            if (!service_start(err, sizeof err))
                error_box(d, "The agent did not start.", err);
            refresh(d);
            return TRUE;
        case IDC_STOP:
            if (!service_stop(err, sizeof err))
                error_box(d, "The agent did not stop.", err);
            refresh(d);
            return TRUE;
        case IDC_REMOVE:
            if (MessageBox(d, "Stop the agent and remove the service? This machine will not "
                              "be reachable until it is installed again.",
                           "C-Desk-Vint", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
                return TRUE;
            if (!service_remove(err, sizeof err))
                error_box(d, "The service could not be removed.", err);
            else
                remove_shortcut();
            refresh(d);
            return TRUE;
        case IDC_LOG: {
            char log[MAX_PATH], cmd[MAX_PATH + 16];
            STARTUPINFO si;
            PROCESS_INFORMATION pi;
            prefs_path(log, sizeof log, "C-Desk-Vint.log");
            snprintf(cmd, sizeof cmd, "notepad.exe \"%s\"", log);
            memset(&si, 0, sizeof si);
            si.cb = sizeof si;
            if (CreateProcess(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            }
            return TRUE;
        }
        case IDC_APPLY:
            apply(d);
            return TRUE;
        case IDCANCEL:
            EndDialog(d, 0);
            return TRUE;
        }
        break;
    case WM_DESTROY:
        KillTimer(d, TIMER_ID);
        break;
    }
    return FALSE;
}

int ui_run(HINSTANCE hinst)
{
    inst = hinst;
    if (!prefs_load(1)) {
        char msg[400];
        snprintf(msg, sizeof msg,
                 "C-Desk-Vint cannot write its settings in\n%s\n\nCopy it to a disk it can write "
                 "to and run it from there.", prefs_dir());
        MessageBox(NULL, msg, "C-Desk-Vint", MB_ICONSTOP);
        return 1;
    }
    DialogBox(hinst, MAKEINTRESOURCE(IDD_MAIN), NULL, dlg_proc);
    return 0;
}
