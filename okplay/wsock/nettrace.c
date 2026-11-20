/* okplay wsock - network timing trace (okplay.ini wsock_nettrace=1; off by default).  [universal]
 *
 * For measuring what a TCP game sees, with okplay and without it (the game's own TCP/IP, okplay passing every call
 * through): every send() and recv() the game makes that moves data is written down with a microsecond time and the
 * stream offset it covers; with okplay also when okplay got the bytes from the other PC ("arrived") and when it
 * handed them to its lane ("sent"). Two PCs' traces, matched by stream offset, give the one-way time of every message
 * and the gaps the game waited through (tools/nettrace_report.py). Nothing is sent anywhere.
 *
 * File: okp_log_dir()\okplay-net-<yyyymmdd-hhmmss>-<pid>.csv, one line per event:
 *   t_us,kind,sock,bytes,offset      kind: S game send, R game recv, A okplay got bytes from the lane, W okplay sent
 * The header gives the PC name, the mode, and a wall-clock anchor (UTC, 100 ns) for the first t_us = 0.
 * The game's thread only copies the event into a buffer; a low-priority thread writes it out every 500 ms. */
#include "okp.h"
#include "nettrace.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define NT_MAX 65536            /* events buffered between writes (64/s for NFS3: never near) */
#define NT_SOCKS 32

typedef struct { uint32_t t_us; uint32_t sock; int32_t bytes; uint32_t off; char kind; } nt_ev;

static volatile LONG nt_on;
static CRITICAL_SECTION nt_lock;
static nt_ev *nt_buf;
static int nt_n, nt_lost;
static LARGE_INTEGER nt_f, nt_t0;
static HANDLE nt_file = INVALID_HANDLE_VALUE;
static struct { uint32_t sock; uint32_t off[4]; } nt_socks[NT_SOCKS];   /* per socket: stream offset per kind */

static int kind_ix(char k) { return k == 'S' ? 0 : k == 'R' ? 1 : k == 'A' ? 2 : 3; }

static uint32_t now_us(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint32_t)((t.QuadPart - nt_t0.QuadPart) * 1000000 / nt_f.QuadPart);
}

static void nt_write_all(void)
{
    static nt_ev out[4096];
    for (;;) {
        int n;
        EnterCriticalSection(&nt_lock);
        n = nt_n < 4096 ? nt_n : 4096;
        memcpy(out, nt_buf, n * sizeof(nt_ev));
        memmove(nt_buf, nt_buf + n, (nt_n - n) * sizeof(nt_ev));
        nt_n -= n;
        LeaveCriticalSection(&nt_lock);
        if (!n) return;
        char line[96];
        static char chunk[4096 * 64];
        int c = 0;
        for (int i = 0; i < n; i++) {
            int l = snprintf(line, sizeof(line), "%lu,%c,%lu,%ld,%lu\r\n", (unsigned long)out[i].t_us, out[i].kind,
                             (unsigned long)out[i].sock, (long)out[i].bytes, (unsigned long)out[i].off);
            memcpy(chunk + c, line, l);
            c += l;
        }
        DWORD put;
        WriteFile(nt_file, chunk, c, &put, NULL);
    }
}

static DWORD WINAPI nt_writer(LPVOID unused)
{
    (void)unused;
    for (;;) {
        Sleep(500);
        nt_write_all();
        if (nt_lost) {
            char l[64];
            DWORD put;
            int n = snprintf(l, sizeof(l), "# lost %d events (buffer full)\r\n", nt_lost);
            nt_lost = 0;
            WriteFile(nt_file, l, n, &put, NULL);
        }
    }
    return 0;
}

void nt_init(const char *ini)
{
    if (!GetPrivateProfileIntA("okplay", "wsock_nettrace", 0, ini)) return;
    InitializeCriticalSection(&nt_lock);
    nt_buf = (nt_ev *)malloc(NT_MAX * sizeof(nt_ev));
    if (!nt_buf) return;
    QueryPerformanceFrequency(&nt_f);
    QueryPerformanceCounter(&nt_t0);
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    SYSTEMTIME st;
    GetLocalTime(&st);
    char path[MAX_PATH], pc[64] = "";
    DWORD pl = sizeof(pc);
    GetComputerNameA(pc, &pl);
    snprintf(path, sizeof(path), "%sokplay-net-%04u%02u%02u-%02u%02u%02u-%lu.csv", okp_log_dir(), st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, (unsigned long)GetCurrentProcessId());
    nt_file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (nt_file == INVALID_HANDLE_VALUE) { okp_log(1, "nettrace: could not open %s", path); return; }
    char h[256];
    DWORD put;
    int n = snprintf(h, sizeof(h), "# okplay nettrace %s pc=%s anchor_utc100ns=%llu\r\n# t_us,kind,sock,bytes,offset\r\n",
                     OKP_VERSION_STR, pc, ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
    WriteFile(nt_file, h, n, &put, NULL);
    HANDLE th = CreateThread(NULL, 0, nt_writer, NULL, 0, NULL);
    if (th) { SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(th); }
    InterlockedExchange(&nt_on, 1);
    okp_log(1, "nettrace: on - every game send/recv timed into %s (okplay.ini wsock_nettrace=0 turns it off)", path);
}

void nt_mode(const char *mode)
{
    if (!nt_on) return;
    char l[96];
    DWORD put;
    nt_write_all();
    int n = snprintf(l, sizeof(l), "# mode %s at %lu\r\n", mode, (unsigned long)now_us());
    EnterCriticalSection(&nt_lock);   /* (in order with the events: written by this thread, after a flush) */
    WriteFile(nt_file, l, n, &put, NULL);
    LeaveCriticalSection(&nt_lock);
}

void nt_event(char kind, uintptr_t sock, int bytes)
{
    if (!nt_on || bytes <= 0) return;
    uint32_t t = now_us();
    EnterCriticalSection(&nt_lock);
    int ix = -1, fr = -1;
    for (int i = 0; i < NT_SOCKS; i++) {
        if (nt_socks[i].sock == (uint32_t)sock) { ix = i; break; }
        if (fr < 0 && !nt_socks[i].sock) fr = i;
    }
    if (ix < 0) {
        ix = fr >= 0 ? fr : (int)(sock % NT_SOCKS);
        memset(&nt_socks[ix], 0, sizeof(nt_socks[ix]));
        nt_socks[ix].sock = (uint32_t)sock;
    }
    uint32_t *o = &nt_socks[ix].off[kind_ix(kind)];
    if (nt_n < NT_MAX) {
        nt_ev *e = &nt_buf[nt_n++];
        e->t_us = t; e->kind = kind; e->sock = (uint32_t)sock; e->bytes = bytes; e->off = *o;
    } else nt_lost++;
    *o += (uint32_t)bytes;
    LeaveCriticalSection(&nt_lock);
}

void nt_closed(uintptr_t sock)   /* a closed socket's offsets start again (its number may be reused) */
{
    if (!nt_on) return;
    EnterCriticalSection(&nt_lock);
    for (int i = 0; i < NT_SOCKS; i++) if (nt_socks[i].sock == (uint32_t)sock) memset(&nt_socks[i], 0, sizeof(nt_socks[i]));
    LeaveCriticalSection(&nt_lock);
}

int nt_enabled(void) { return nt_on != 0; }
