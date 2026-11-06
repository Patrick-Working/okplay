/* mmtest: does this machine fire multimedia timers? timeSetEvent(33 ms, periodic) for 3 s, count the
 * callbacks, write C:/UDPDUMP/mmtest.txt. Windows 95 and later. */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
static volatile LONG n;
static void CALLBACK tick(UINT id, UINT msg, DWORD_PTR u, DWORD_PTR a, DWORD_PTR b) { InterlockedIncrement(&n); }
int main(void)
{
    FILE *f = fopen("C:/UDPDUMP/mmtest.txt", "w");
    UINT id = timeSetEvent(33, 1, tick, 0, TIME_PERIODIC);
    Sleep(3000);
    if (id) timeKillEvent(id);
    if (f) { fprintf(f, "timeSetEvent -> %u, callbacks in 3 s: %ld (expected about 90)\n", id, n); fclose(f); }
    return 0;
}
