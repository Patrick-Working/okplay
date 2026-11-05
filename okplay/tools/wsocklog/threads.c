/* threads NAME: list the threads (id, base and delta priority) of every process whose exe path contains
 * NAME, into C:/UDPDUMP/threads.txt. Toolhelp, Windows 95 and later. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
int main(int argc, char **argv)
{
    char want[64] = "ROADRASH";
    if (argc > 1) { int i; for (i = 0; argv[1][i] && i < 63; i++) want[i] = (char)toupper((unsigned char)argv[1][i]); want[i] = 0; }
    FILE *f = fopen("C:/UDPDUMP/threads.txt", "w");
    if (!f) return 1;
    HANDLE ps = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS | TH32CS_SNAPTHREAD, 0);
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    for (BOOL m = Process32First(ps, &pe); m; m = Process32Next(ps, &pe)) {
        char up[MAX_PATH];
        int i;
        for (i = 0; pe.szExeFile[i] && i < MAX_PATH - 1; i++) up[i] = (char)toupper((unsigned char)pe.szExeFile[i]);
        up[i] = 0;
        if (!strstr(up, want)) continue;
        fprintf(f, "process %lu %s (%lu threads, base priority %ld)\n", (unsigned long)pe.th32ProcessID, pe.szExeFile,
                (unsigned long)pe.cntThreads, (long)pe.pcPriClassBase);
        THREADENTRY32 te;
        te.dwSize = sizeof(te);
        for (BOOL k = Thread32First(ps, &te); k; k = Thread32Next(ps, &te))
            if (te.th32OwnerProcessID == pe.th32ProcessID)
                fprintf(f, "    thread %lu base %ld delta %ld\n", (unsigned long)te.th32ThreadID, (long)te.tpBasePri, (long)te.tpDeltaPri);
    }
    fclose(f);
    return 0;
}
