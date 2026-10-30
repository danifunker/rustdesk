/* plist: the running processes, with their modules' paths and threads, for
 * Windows 95/98 (ToolHelp; NT 4.0 has none -- use the Resource Kit's tlist).
 *
 *   plist             list them
 *   plist kill NAME   end every process whose path contains NAME
 *
 * Build on 9x:  cl plist.c
 */
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 p;
    if (s == INVALID_HANDLE_VALUE)
        return 1;
    p.dwSize = sizeof p;
    if (Process32First(s, &p))
        do
            if (argc > 2 && !strcmp(argv[1], "kill")) {
                if (strstr(_strupr(p.szExeFile), _strupr(argv[2]))) {
                    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, p.th32ProcessID);
                    printf("%08lx %s: %s\n", p.th32ProcessID, p.szExeFile,
                           h && TerminateProcess(h, 1) ? "ended" : "could not end");
                    if (h)
                        CloseHandle(h);
                }
            } else
                printf("%08lx %3lu threads  %s\n", p.th32ProcessID, p.cntThreads, p.szExeFile);
        while (Process32Next(s, &p));
    CloseHandle(s);
    return 0;
}
