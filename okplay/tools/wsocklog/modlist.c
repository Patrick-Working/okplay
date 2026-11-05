/* modlist [NAME...]: list every process and, for each, its loaded modules whose file name contains one
 * of NAME (default: WSOCK32 WS2_32), into C:/UDPDUMP/modlist.txt. Toolhelp, Windows 95 and later.
 * Answers "who holds wsock32.dll?" on Windows 9x, where one loaded copy serves every program. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static int has(const char *path, int n, char **names)
{
    char up[MAX_PATH];
    int i;
    for (i = 0; path[i] && i < MAX_PATH - 1; i++) up[i] = (char)toupper((unsigned char)path[i]);
    up[i] = 0;
    for (i = 0; i < n; i++) if (strstr(up, names[i])) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    char *def[] = { "WSOCK32", "WS2_32" };
    char **names = argc > 1 ? argv + 1 : def;
    int nn = argc > 1 ? argc - 1 : 2;
    FILE *f = fopen("C:/UDPDUMP/modlist.txt", "w");
    if (!f) return 1;
    HANDLE ps = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    for (BOOL m = Process32First(ps, &pe); m; m = Process32Next(ps, &pe)) {
        fprintf(f, "%lu %s\n", (unsigned long)pe.th32ProcessID, pe.szExeFile);
        HANDLE ms = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pe.th32ProcessID);
        if (ms == INVALID_HANDLE_VALUE) continue;
        MODULEENTRY32 me;
        me.dwSize = sizeof(me);
        for (BOOL k = Module32First(ms, &me); k; k = Module32Next(ms, &me))
            if (has(me.szExePath, nn, names)) fprintf(f, "    %s (usage %lu)\n", me.szExePath, (unsigned long)me.GlblcntUsage);
        CloseHandle(ms);
    }
    CloseHandle(ps);
    fclose(f);
    return 0;
}
