/* wsocklog: a pass-through wsock32.dll that logs a game's Winsock 1.1 calls.
 *
 * Put the built wsock32.dll next to a game's exe (the game's files are not touched; delete the DLL
 * to undo). Every call goes to Windows' own wsock32.dll (loaded from the system folder by full path)
 * and is written to wsocklog.txt beside the exe: sockets made (TCP, UDP, IPX), addresses and ports,
 * sizes and the first bytes of each packet, results and errors. It answers "how does this game
 * use the network?" before writing a shim for it. Not a product: a diagnostics tool.
 *
 * Build: okplay/tools/wsocklog/build.sh (32-bit MinGW). */
#define WIN32_LEAN_AND_MEAN
#include <winsock.h>
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef AF_IPX
#define AF_IPX 6
#endif

static HMODULE sys;
static HANDLE logf = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION loglock;
static DWORD t0;

/* Steering (wsocklog.ini beside the exe, [wsocklog]), for machines with more than one network card,
 * where a game picks the wrong one: prefer=192.168.77. puts this machine's addresses that start with
 * it first in gethostbyname(own name); broadcast=192.168.77.255 sends what the game broadcasts to
 * 255.255.255.255 there instead. The first piece of okplay's Winsock shim. */
static int trace_entry;   /* wsocklog.ini trace_entry=1: log calls on the way in, and connect's local address */
static int bind_steer = 1;   /* wsocklog.ini bind=0: leave binds to "any address" alone */
static char prefer[32];
static unsigned long bcast_to;   /* network order, 0 = off */

/* Winsock 1.1 (winsock.h) and Winsock 2 (ws2tcpip.h) number the IPPROTO_IP options differently; when the
 * calls go to ws2_32 the old numbers are translated (both sets are in the public headers). */
static int via_ws2;
static int ip_opt(int lv, int o)
{
    static const int map[][2] = { { 2, 9 }, { 3, 10 }, { 4, 11 }, { 5, 12 }, { 6, 13 }, { 7, 4 }, { 8, 3 }, { 9, 14 } };
    if (!via_ws2 || lv != 0 /* IPPROTO_IP */) return o;
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) if (map[i][0] == o) return map[i][1];
    return o;
}

/* The line is built in a static buffer under the lock: a game's network thread may have a tiny stack
 * (Road Rash's worker on Windows 98 vanished right after its first calls into this DLL). */
static char lgbuf[2048];
static DWORD seen_threads[64];
static int nseen;
static int log_on = 1;   /* wsocklog.ini log=0: steering only, no log lines after the start */
static void lg(const char *fmt, ...)
{
    if (logf == INVALID_HANDLE_VALUE || !log_on) return;
    EnterCriticalSection(&loglock);
    DWORD tid = GetCurrentThreadId(), w;
    int i, n;
    for (i = 0; i < nseen && seen_threads[i] != tid; i++) ;
    if (i == nseen && nseen < 64) {
        /* a thread's first line says how much stack it has left (TIB: fs:[8] = stack limit) */
        char *limit;
        __asm__ volatile ("movl %%fs:8, %0" : "=r"(limit));
        char here;
        seen_threads[nseen++] = tid;
        n = _snprintf(lgbuf, sizeof(lgbuf), "%8lu [%5lu] (new thread: %ld bytes of stack below this call)\r\n",
                      (unsigned long)(GetTickCount() - t0), (unsigned long)tid, (long)(&here - limit));
        if (n > 0) WriteFile(logf, lgbuf, n, &w, NULL);
    }
    n = _snprintf(lgbuf, sizeof(lgbuf), "%8lu [%5lu] ", (unsigned long)(GetTickCount() - t0), (unsigned long)tid);
    va_list ap;
    va_start(ap, fmt);
    int m = _vsnprintf(lgbuf + n, sizeof(lgbuf) - n - 3, fmt, ap);
    va_end(ap);
    if (m < 0) m = sizeof(lgbuf) - n - 3;
    n += m;
    if (n > (int)sizeof(lgbuf) - 3) n = sizeof(lgbuf) - 3;
    lgbuf[n++] = '\r';
    lgbuf[n++] = '\n';
    WriteFile(logf, lgbuf, n, &w, NULL);
    LeaveCriticalSection(&loglock);
}

static const char *addr_s(const struct sockaddr *a, int len, char *o, int size)
{
    if (!a) { _snprintf(o, size, "(null)"); return o; }
    if (a->sa_family == AF_INET && len >= (int)sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *i = (const struct sockaddr_in *)a;
        const unsigned char *p = (const unsigned char *)&i->sin_addr;
        _snprintf(o, size, "%u.%u.%u.%u:%u", p[0], p[1], p[2], p[3], (unsigned)((((const unsigned char *)&i->sin_port)[0] << 8) | ((const unsigned char *)&i->sin_port)[1]));
    } else if (a->sa_family == AF_IPX && len >= 14) {
        const unsigned char *p = (const unsigned char *)a + 2;
        _snprintf(o, size, "ipx %02x%02x%02x%02x-%02x%02x%02x%02x%02x%02x:%04x", p[0], p[1], p[2], p[3], p[4], p[5],
                 p[6], p[7], p[8], p[9], (p[10] << 8) | p[11]);
    } else {
        _snprintf(o, size, "family %d len %d", a->sa_family, len);
    }
    return o;
}

static const char *hex_s(const void *buf, int len, char *o, int size)
{
    const unsigned char *p = (const unsigned char *)buf;
    int n = 0, show = len < 48 ? len : 48;
    o[0] = 0;
    for (int i = 0; i < show && n < size - 4; i++) n += _snprintf(o + n, size - n, "%02x", p[i]);
    if (len > show && n < size - 4) _snprintf(o + n, size - n, "..");
    return o;
}

static void roadrash_pool(const char *when);
/* would-block results are logged once per socket every few seconds, as a count */
#define NQUIET 64
static struct { SOCKET s; int op; DWORD last; unsigned long n; } quiet[NQUIET];
static int would_block(SOCKET s, int op, const char *name)
{
    DWORD now = GetTickCount();
    for (int i = 0; i < NQUIET; i++) {
        if (quiet[i].n && quiet[i].s == s && quiet[i].op == op) {
            quiet[i].n++;
            if (now - quiet[i].last >= 5000) {
                lg("%s(%u): would block x%lu in the last %lus", name, (unsigned)s, quiet[i].n,
                   (unsigned long)((now - quiet[i].last) / 1000));
                quiet[i].last = now;
                quiet[i].n = 1;
                if (op == 4) roadrash_pool("every 5 s");
            }
            return 1;
        }
    }
    for (int i = 0; i < NQUIET; i++) {
        if (!quiet[i].n) {
            quiet[i].s = s; quiet[i].op = op; quiet[i].last = now; quiet[i].n = 1;
            lg("%s(%u): would block (counted from now on)", name, (unsigned)s);
            return 1;
        }
    }
    return 1;
}

/* the first call of each kind on each socket is logged on the way IN, so a call that never returns
 * (a blocking recv, say) still shows up */
static struct { SOCKET s; int op; } firsts[256];
static int nfirsts;
static void first_call(SOCKET s, int op, const char *name)
{
    for (int i = 0; i < nfirsts; i++) if (firsts[i].s == s && firsts[i].op == op) return;
    if (nfirsts < 256) { firsts[nfirsts].s = s; firsts[nfirsts].op = op; nfirsts++; }
    lg("%s(%u): first call, entering", name, (unsigned)s);
}

#define REAL(ret, name, args) \
    typedef ret (WINAPI *name##_t) args; \
    static name##_t r_##name; \
    static name##_t get_##name(void) { if (!r_##name) r_##name = (name##_t)GetProcAddress(sys, #name); return r_##name; }

REAL(SOCKET, accept, (SOCKET, struct sockaddr *, int *))
REAL(int, bind, (SOCKET, const struct sockaddr *, int))
REAL(int, closesocket, (SOCKET))
REAL(int, connect, (SOCKET, const struct sockaddr *, int))
REAL(int, getpeername, (SOCKET, struct sockaddr *, int *))
REAL(int, getsockname, (SOCKET, struct sockaddr *, int *))
REAL(int, getsockopt, (SOCKET, int, int, char *, int *))
REAL(u_long, htonl, (u_long))
REAL(u_short, htons, (u_short))
REAL(unsigned long, inet_addr, (const char *))
REAL(char *, inet_ntoa, (struct in_addr))
REAL(int, ioctlsocket, (SOCKET, long, u_long *))
REAL(int, listen, (SOCKET, int))
REAL(u_long, ntohl, (u_long))
REAL(u_short, ntohs, (u_short))
REAL(int, recv, (SOCKET, char *, int, int))
REAL(int, recvfrom, (SOCKET, char *, int, int, struct sockaddr *, int *))
REAL(int, select, (int, fd_set *, fd_set *, fd_set *, const struct timeval *))
REAL(int, send, (SOCKET, const char *, int, int))
REAL(int, sendto, (SOCKET, const char *, int, int, const struct sockaddr *, int))
REAL(int, setsockopt, (SOCKET, int, int, const char *, int))
REAL(int, shutdown, (SOCKET, int))
REAL(SOCKET, socket, (int, int, int))
REAL(struct hostent *, gethostbyaddr, (const char *, int, int))
REAL(struct hostent *, gethostbyname, (const char *))
REAL(int, gethostname, (char *, int))
REAL(int, WSAAsyncSelect, (SOCKET, HWND, u_int, long))
REAL(HANDLE, WSAAsyncGetHostByName, (HWND, u_int, const char *, char *, int))
REAL(int, WSAGetLastError, (void))
REAL(void, WSASetLastError, (int))
REAL(int, WSAStartup, (WORD, LPWSADATA))
REAL(int, WSACleanup, (void))
REAL(int, __WSAFDIsSet, (SOCKET, fd_set *))

#define ERR() (get_WSAGetLastError()())
#define KEEP_ERR(expr) do { int e_ = ERR(); expr; get_WSASetLastError()(e_); } while (0)

struct hostent *WINAPI x_gethostbyname(const char *n);
SOCKET WINAPI x_accept(SOCKET s, struct sockaddr *a, int *l)
{
    SOCKET r = get_accept()(s, a, l);
    char t[96];
    if (r == INVALID_SOCKET && ERR() == WSAEWOULDBLOCK) { KEEP_ERR(would_block(s, 1, "accept")); return r; }
    KEEP_ERR(lg("accept(%u) -> %d from %s err %d", (unsigned)s, (int)r, r != INVALID_SOCKET && a && l ? addr_s(a, *l, t, sizeof(t)) : "-",
                r == INVALID_SOCKET ? ERR() : 0));
    return r;
}
int WINAPI x_bind(SOCKET s, const struct sockaddr *a, int l)
{
    char t[96];
    struct sockaddr_in steer;
    const char *note = "";
    /* prefer=: a socket bound to "any address" is bound to the preferred one instead, so what it sends
     * leaves with that source address (Windows 9x picked the other card's for a connected UDP socket) */
    if (prefer[0] && bind_steer && a && l >= (int)sizeof(struct sockaddr_in) && a->sa_family == AF_INET &&
        ((const struct sockaddr_in *)a)->sin_addr.s_addr == 0) {
        static unsigned long pref_ip;
        if (!pref_ip) {
            char hn[256];
            struct hostent *h;
            if (get_gethostname()(hn, sizeof(hn)) == 0 && (h = x_gethostbyname(hn)) && h->h_addr_list[0])
                memcpy(&pref_ip, h->h_addr_list[0], 4);
        }
        if (pref_ip) {
            steer = *(const struct sockaddr_in *)a;
            steer.sin_addr.s_addr = pref_ip;
            a = (const struct sockaddr *)&steer;
            l = sizeof(steer);
            note = " (steered from 0.0.0.0)";
        }
    }
    int r = get_bind()(s, a, l);
    KEEP_ERR(lg("bind(%u, %s%s) -> %d err %d", (unsigned)s, addr_s(a, l, t, sizeof(t)), note, r, r ? ERR() : 0));
    return r;
}
int WINAPI x_closesocket(SOCKET s)
{
    int r = get_closesocket()(s);
    KEEP_ERR(lg("closesocket(%u) -> %d", (unsigned)s, r));
    return r;
}
/* Test-rig diagnostic, Road Rash 1996 CD exe only (ROADRASH.EXE of 1996-08-23, fixed addresses, read only):
 * its network buffers come from a 6 MB pool when LALLOC.VXD isn't there; log how full the pool is. */
static void roadrash_pool(const char *when)
{
    const unsigned char *code = (const unsigned char *)0x442050;
    if (GetModuleHandleA(NULL) != (HMODULE)0x400000 || IsBadReadPtr(code, 4) || code[0] != 0x8b || code[1] != 0x4c ||
        IsBadReadPtr((void *)0x467834, 8) || IsBadReadPtr((void *)0x482318, 0x18) || IsBadReadPtr((void *)0x49e550, 0x20))
        return;
    unsigned long cur = *(unsigned long *)0x48232c, end = *(unsigned long *)0x482318, start = *(unsigned long *)0x48231c;
    unsigned long *sz = (unsigned long *)0x49e560, *got = (unsigned long *)0x49e550;
    lg("roadrash pool (%s): mode %u, LALLOC handle %ld, used %lu of %lu KB, %lu KB free; net buffers wanted %lu %lu %lu %lu bytes, got %08lx %08lx %08lx %08lx",
       when, *(unsigned char *)0x467838, *(long *)0x467834, (cur - start) / 1024, (end - start) / 1024, end > cur ? (end - cur) / 1024 : 0,
       sz[0], sz[1], sz[2], sz[3], got[0], got[1], got[2], got[3]);
}

/* watch_pool=1: a thread of ours samples the same values every 500 ms and logs any change, with the race
 * worker's handle (0x4b7f24, set when it is created) and the player-slot count (0x4755f8) */
static DWORD WINAPI pool_watch(LPVOID arg)
{
    unsigned long last[8] = { 0 };
    (void)arg;
    for (;;) {
        Sleep(500);
        if (GetModuleHandleA(NULL) != (HMODULE)0x400000 || IsBadReadPtr((void *)0x4b7f24, 4)) continue;
        unsigned long now[8] = { *(unsigned long *)0x48232c, *(unsigned long *)0x4b7f24, *(unsigned long *)0x4755f8,
                                 *(unsigned long *)0x49e560, *(unsigned long *)0x49e550, *(unsigned long *)0x49bdd0,
                                 *(unsigned long *)0x4c1200, 0 };
        if (memcmp(now, last, sizeof(now))) {
            lg("roadrash state: pool at %08lx, race worker handle %08lx, slots %lu, size[0] %lu, buffer[0] %08lx, flag 49bdd0 %lu, 4c1200 %08lx",
               now[0], now[1], now[2], now[3], now[4], now[5], now[6]);
            roadrash_pool("change");
            memcpy(last, now, sizeof(now));
        }
    }
    return 0;
}

int WINAPI x_connect(SOCKET s, const struct sockaddr *a, int l)
{
    roadrash_pool("connect");
    char t[96];
    int r = get_connect()(s, a, l);
    char me[96] = "?";
    struct sockaddr_in la;
    int ll = sizeof(la);
    KEEP_ERR(if (trace_entry && get_getsockname()(s, (struct sockaddr *)&la, &ll) == 0) addr_s((struct sockaddr *)&la, ll, me, sizeof(me)));
    KEEP_ERR(lg("connect(%u, %s) -> %d err %d (local %s)", (unsigned)s, addr_s(a, l, t, sizeof(t)), r, r ? ERR() : 0, me));
    return r;
}
int WINAPI x_getpeername(SOCKET s, struct sockaddr *a, int *l)
{
    char t[96];
    int r = get_getpeername()(s, a, l);
    KEEP_ERR(lg("getpeername(%u) -> %d %s", (unsigned)s, r, r == 0 ? addr_s(a, *l, t, sizeof(t)) : "-"));
    return r;
}
int WINAPI x_getsockname(SOCKET s, struct sockaddr *a, int *l)
{
    char t[96];
    int r = get_getsockname()(s, a, l);
    KEEP_ERR(lg("getsockname(%u) -> %d %s", (unsigned)s, r, r == 0 ? addr_s(a, *l, t, sizeof(t)) : "-"));
    return r;
}
int WINAPI x_getsockopt(SOCKET s, int lv, int o, char *v, int *l)
{
    int r = get_getsockopt()(s, lv, ip_opt(lv, o), v, l);
    KEEP_ERR(lg("getsockopt(%u, level 0x%x, opt 0x%x) -> %d value %ld", (unsigned)s, lv, o, r,
                r == 0 && l && *l >= 4 ? *(long *)v : -1L));
    return r;
}
u_long WINAPI x_htonl(u_long v) { return get_htonl()(v); }
u_short WINAPI x_htons(u_short v) { return get_htons()(v); }
u_long WINAPI x_ntohl(u_long v) { return get_ntohl()(v); }
u_short WINAPI x_ntohs(u_short v) { return get_ntohs()(v); }
unsigned long WINAPI x_inet_addr(const char *c)
{
    unsigned long r = get_inet_addr()(c);
    KEEP_ERR(lg("inet_addr(\"%s\") -> %08lx", c ? c : "(null)", r));
    return r;
}
char *WINAPI x_inet_ntoa(struct in_addr a) { return get_inet_ntoa()(a); }
static int repeat_quiet(SOCKET s, int op, int len, const char *name);
int WINAPI x_ioctlsocket(SOCKET s, long cmd, u_long *a)
{
    if (trace_entry) first_call(s, 14, "ioctlsocket");
    u_long in = a ? *a : 0;
    int r = get_ioctlsocket()(s, cmd, a);
    if (cmd == (long)FIONREAD && r == 0 && a && *a == 0) { KEEP_ERR(would_block(s, 4, "ioctlsocket FIONREAD = 0")); return r; }   /* polled constantly: counted */
    if (cmd == (long)FIONREAD && r == 0 && a) { int q; KEEP_ERR(q = repeat_quiet(s, 24, (int)*a, "ioctlsocket FIONREAD")); if (q) return r; }
    KEEP_ERR(lg("ioctlsocket(%u, 0x%lx, in %lu) -> %d out %lu", (unsigned)s, (unsigned long)cmd, in, r, a ? *a : 0));
    return r;
}
int WINAPI x_listen(SOCKET s, int b)
{
    int r = get_listen()(s, b);
    KEEP_ERR(lg("listen(%u, %d) -> %d err %d", (unsigned)s, b, r, r ? ERR() : 0));
    return r;
}
/* repeated packets (same socket, call and size) are logged 3 times, then counted: one line every 5 s.
 * Writing a line per packet slowed the game's network threads on the 98 VMs. */
#define NREP 64
static struct { SOCKET s; int op, len; unsigned long n, since; DWORD last; } reps[NREP];
static int repeat_quiet(SOCKET s, int op, int len, const char *name)
{
    DWORD now = GetTickCount();
    int i, freei = -1;
    for (i = 0; i < NREP; i++) {
        if (reps[i].n && reps[i].s == s && reps[i].op == op && reps[i].len == len) break;
        if (!reps[i].n && freei < 0) freei = i;
    }
    if (i == NREP) {
        if (freei < 0) return 0;
        i = freei;
        reps[i].s = s; reps[i].op = op; reps[i].len = len; reps[i].n = 0; reps[i].since = 0; reps[i].last = now;
    }
    reps[i].n++;
    if (reps[i].n <= 3) return 0;
    reps[i].since++;
    if (now - reps[i].last >= 5000) {
        lg("%s(%u, %d bytes): x%lu more in the last %lus (%lu in all)", name, (unsigned)s, len, reps[i].since,
           (unsigned long)((now - reps[i].last) / 1000), reps[i].n);
        reps[i].since = 0;
        reps[i].last = now;
    }
    return 1;
}

int WINAPI x_recv(SOCKET s, char *b, int l, int f)
{
    char h[120];
    if (trace_entry) first_call(s, 12, "recv");
    int r = get_recv()(s, b, l, f);
    if (r < 0 && ERR() == WSAEWOULDBLOCK) { KEEP_ERR(would_block(s, 2, "recv")); return r; }
    if (r > 0) { int q; KEEP_ERR(q = repeat_quiet(s, 22, r, "recv")); if (q) return r; }
    KEEP_ERR(lg("recv(%u, %d) -> %d %s err %d", (unsigned)s, l, r, r > 0 ? hex_s(b, r, h, sizeof(h)) : "", r < 0 ? ERR() : 0));
    return r;
}
int WINAPI x_recvfrom(SOCKET s, char *b, int l, int f, struct sockaddr *a, int *al)
{
    char h[120], t[96];
    if (trace_entry) first_call(s, 13, "recvfrom");
    int r = get_recvfrom()(s, b, l, f, a, al);
    if (r < 0 && ERR() == WSAEWOULDBLOCK) { KEEP_ERR(would_block(s, 3, "recvfrom")); return r; }
    if (r > 0) { int q; KEEP_ERR(q = repeat_quiet(s, 23, r, "recvfrom")); if (q) return r; }
    KEEP_ERR(lg("recvfrom(%u, %d) -> %d from %s %s err %d", (unsigned)s, l, r,
                r >= 0 && a && al ? addr_s(a, *al, t, sizeof(t)) : "-", r > 0 ? hex_s(b, r, h, sizeof(h)) : "", r < 0 ? ERR() : 0));
    return r;
}
int WINAPI x_select(int n, fd_set *rd, fd_set *wr, fd_set *ex, const struct timeval *tv)
{
    int nr = rd ? (int)rd->fd_count : 0, nw = wr ? (int)wr->fd_count : 0;
    SOCKET first = rd && rd->fd_count ? rd->fd_array[0] : (wr && wr->fd_count ? wr->fd_array[0] : 0);
    int r = get_select()(n, rd, wr, ex, tv);
    if (r == 0) { KEEP_ERR(would_block(first, 5, "select (first socket) nothing ready")); return r; }
    KEEP_ERR(lg("select(read %d, write %d, timeout %ld ms) -> %d", nr, nw, tv ? tv->tv_sec * 1000 + tv->tv_usec / 1000 : -1L, r));
    return r;
}
int WINAPI x_send(SOCKET s, const char *b, int l, int f)
{
    char h[120];
    int r = get_send()(s, b, l, f);
    if (r == l) { int q; KEEP_ERR(q = repeat_quiet(s, 20, l, "send")); if (q) return r; }
    KEEP_ERR(lg("send(%u, %d) -> %d %s err %d", (unsigned)s, l, r, hex_s(b, l, h, sizeof(h)), r < 0 ? ERR() : 0));
    return r;
}
int WINAPI x_sendto(SOCKET s, const char *b, int l, int f, const struct sockaddr *a, int al)
{
    char h[120], t[96];
    struct sockaddr_in steer;
    const char *note = "";
    if (bcast_to && a && al >= (int)sizeof(struct sockaddr_in) && a->sa_family == AF_INET &&
        ((const struct sockaddr_in *)a)->sin_addr.s_addr == 0xFFFFFFFFu) {
        steer = *(const struct sockaddr_in *)a;
        steer.sin_addr.s_addr = bcast_to;
        a = (const struct sockaddr *)&steer;
        al = sizeof(steer);
        note = " (steered from 255.255.255.255)";
    }
    int r = get_sendto()(s, b, l, f, a, al);
    if (r == l) { int q; KEEP_ERR(q = repeat_quiet(s, 21, l, "sendto")); if (q) return r; }
    KEEP_ERR(lg("sendto(%u, %d, %s%s) -> %d %s err %d", (unsigned)s, l, addr_s(a, al, t, sizeof(t)), note, r,
                hex_s(b, l, h, sizeof(h)), r < 0 ? ERR() : 0));
    return r;
}
int WINAPI x_setsockopt(SOCKET s, int lv, int o, const char *v, int l)
{
    int r = get_setsockopt()(s, lv, ip_opt(lv, o), v, l);
    KEEP_ERR(lg("setsockopt(%u, level 0x%x, opt 0x%x, value %ld, len %d) -> %d err %d", (unsigned)s, lv, o,
                v && l >= 4 ? *(const long *)v : (v && l > 0 ? (long)*(const unsigned char *)v : -1L), l, r, r ? ERR() : 0));
    return r;
}
int WINAPI x_shutdown(SOCKET s, int h)
{
    int r = get_shutdown()(s, h);
    KEEP_ERR(lg("shutdown(%u, %d) -> %d", (unsigned)s, h, r));
    return r;
}
SOCKET WINAPI x_socket(int af, int type, int proto)
{
    SOCKET r = get_socket()(af, type, proto);
    KEEP_ERR(lg("socket(af %d%s, type %d, proto %d) -> %d err %d", af, af == AF_INET ? " inet" : af == AF_IPX ? " IPX" : "",
                type, proto, (int)r, r == INVALID_SOCKET ? ERR() : 0));
    return r;
}
struct hostent *WINAPI x_gethostbyaddr(const char *a, int l, int t)
{
    struct hostent *r = get_gethostbyaddr()(a, l, t);
    KEEP_ERR(lg("gethostbyaddr(len %d) -> %s", l, r && r->h_name ? r->h_name : "(none)"));
    return r;
}
struct hostent *WINAPI x_gethostbyname(const char *n)
{
    struct hostent *r = get_gethostbyname()(n);
    char t[64] = "(none)";
    if (prefer[0] && r && r->h_addr_list && r->h_addr_list[0] && r->h_addrtype == AF_INET) {
        /* move the first address that starts with prefer= to the front (only the pointers move) */
        for (int i = 1; r->h_addr_list[i]; i++) {
            const unsigned char *p = (const unsigned char *)r->h_addr_list[i];
            char d[20];
            _snprintf(d, sizeof(d), "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
            d[sizeof(d) - 1] = 0;
            if (!strncmp(d, prefer, strlen(prefer))) {
                char *x = r->h_addr_list[i];
                for (int j = i; j > 0; j--) r->h_addr_list[j] = r->h_addr_list[j - 1];
                r->h_addr_list[0] = x;
                break;
            }
        }
    }
    if (r && r->h_addr_list && r->h_addr_list[0]) {
        const unsigned char *p = (const unsigned char *)r->h_addr_list[0];
        _snprintf(t, sizeof(t), "%u.%u.%u.%u%s", p[0], p[1], p[2], p[3], r->h_addr_list[1] ? " (+more)" : "");
    }
    KEEP_ERR(lg("gethostbyname(\"%s\") -> %s", n ? n : "(null)", t));
    return r;
}
int WINAPI x_gethostname(char *n, int l)
{
    int r = get_gethostname()(n, l);
    KEEP_ERR(lg("gethostname -> %d \"%s\"", r, r == 0 ? n : ""));
    return r;
}
int WINAPI x_WSAAsyncSelect(SOCKET s, HWND w, u_int m, long ev)
{
    int r = get_WSAAsyncSelect()(s, w, m, ev);
    KEEP_ERR(lg("WSAAsyncSelect(%u, msg 0x%x, events 0x%lx) -> %d", (unsigned)s, m, ev, r));
    return r;
}
HANDLE WINAPI x_WSAAsyncGetHostByName(HWND w, u_int m, const char *n, char *b, int l)
{
    HANDLE r = get_WSAAsyncGetHostByName()(w, m, n, b, l);
    KEEP_ERR(lg("WSAAsyncGetHostByName(\"%s\") -> %p", n ? n : "(null)", r));
    return r;
}
int WINAPI x_WSAGetLastError(void) { return get_WSAGetLastError()(); }
void WINAPI x_WSASetLastError(int e) { get_WSASetLastError()(e); }
int WINAPI x_WSAStartup(WORD v, LPWSADATA d)
{
    int r = get_WSAStartup()(v, d);
    lg("WSAStartup(version %d.%d) -> %d (max udp %u)", LOBYTE(v), HIBYTE(v), r, r == 0 && d ? d->iMaxUdpDg : 0);
    return r;
}
int WINAPI x_WSACleanup(void)
{
    int r = get_WSACleanup()();
    lg("WSACleanup -> %d", r);
    return r;
}
int WINAPI x___WSAFDIsSet(SOCKET s, fd_set *f) { return get___WSAFDIsSet()(s, f); }

/* watch_threads=1 (wsocklog.ini): the game's own CreateThread calls are logged (its import table is
 * pointed at x_CreateThread in memory; the game's files are not touched). For a game whose network
 * thread sometimes never appears. */
typedef HANDLE (WINAPI *ct_t)(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
static ct_t real_ct;
static HANDLE WINAPI x_CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags,
                                    LPDWORD id)
{
    HANDLE h = real_ct(sa, stack, fn, arg, flags, id);
    DWORD e = GetLastError();
    lg("CreateThread(stack %lu, start %p, arg %p, flags 0x%lx, id %s) -> %p id %lu err %lu", (unsigned long)stack, (void *)fn, arg,
       (unsigned long)flags, id ? "given" : "NULL", (void *)h, id ? (unsigned long)*id : 0UL, h ? 0UL : (unsigned long)e);
    SetLastError(e);
    return h;
}
static void watch_create_thread(void)
{
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd->VirtualAddress) return;
    real_ct = (ct_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "CreateThread");
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dd->VirtualAddress); d->Name; d++) {
        if (lstrcmpiA((char *)(base + d->Name), "KERNEL32.dll")) continue;
        IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        IMAGE_THUNK_DATA *names = d->OriginalFirstThunk ? (IMAGE_THUNK_DATA *)(base + d->OriginalFirstThunk) : NULL;
        for (; iat->u1.Function; iat++, names = names ? names + 1 : NULL) {
            /* by name when the hint/name table is there, else by address */
            int match = (ct_t)iat->u1.Function == real_ct;
            if (names && !(names->u1.Ordinal & IMAGE_ORDINAL_FLAG))
                match = !lstrcmpA((char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name, "CreateThread");
            if (!match) continue;
            DWORD old;
            if (VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function), PAGE_READWRITE, &old)) {
                iat->u1.Function = (ULONG_PTR)x_CreateThread;
                VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function), old, &old);
                lg("watching the game's CreateThread calls");
            }
            return;
        }
    }
    lg("CreateThread not found in the game's imports");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD why, LPVOID res)
{
    (void)res;
    if (why == DLL_PROCESS_ATTACH) {
        char p[MAX_PATH];
        t0 = GetTickCount();
        InitializeCriticalSection(&loglock);
        /* Calls go to Winsock 2 (ws2_32.dll; Windows 98 and later, and Windows 95 with its update). Not to the
         * system's wsock32.dll: on Windows 9x a second module named WSOCK32 (the system's, beside us) failed
         * every socket() with WSAEAFNOSUPPORT. Only the IP-level option numbers differ (x_setsockopt). */
        {   /* test knob: wsocksys.dll beside the exe (a copy of the system's wsock32.dll under another file name,
             * so Windows 9x keeps it apart from us) is used instead of ws2_32 */
            char q[MAX_PATH];
            DWORD k = GetModuleFileNameA(NULL, q, sizeof(q));
            while (k && q[k - 1] != '\\') k--;
            q[k] = 0;
            lstrcatA(q, "wsocksys.dll");
            if (GetFileAttributesA(q) != INVALID_FILE_ATTRIBUTES) sys = LoadLibraryA(q);
        }
        int local_copy = sys != NULL;
        if (!sys && (sys = LoadLibraryA("ws2_32.dll"))) via_ws2 = 1;
        if (!sys) {   /* Windows 95 without Winsock 2: the system's own wsock32 */
            UINT n = GetSystemDirectoryA(p, sizeof(p));
            if (n && n < sizeof(p) - 16) {
                lstrcatA(p, "\\wsock32.dll");
                sys = LoadLibraryA(p);
            }
        }
        DWORD l = GetModuleFileNameA(NULL, p, sizeof(p));
        while (l && p[l - 1] != '\\') l--;
        p[l] = 0;
        lstrcatA(p, "wsocklog.txt");
        /* GENERIC_WRITE + seek to the end: Windows 9x has no FILE_APPEND_DATA access */
        logf = CreateFileA(p, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
        if (logf != INVALID_HANDLE_VALUE) SetFilePointer(logf, 0, NULL, FILE_END);
        GetModuleFileNameA(NULL, p, sizeof(p));
        lg("---- wsocklog: %s, calls go to %s", p, !sys ? "NOTHING (not loaded)" : local_copy ? "wsocksys.dll (local copy)" :
           via_ws2 ? "ws2_32.dll" : "the system wsock32.dll");
        {
            char ini[MAX_PATH], b[32];
            DWORD k = GetModuleFileNameA(NULL, ini, sizeof(ini));
            while (k && ini[k - 1] != '\\') k--;
            ini[k] = 0;
            lstrcatA(ini, "wsocklog.ini");
            GetPrivateProfileStringA("wsocklog", "prefer", "", prefer, sizeof(prefer), ini);
            trace_entry = GetPrivateProfileIntA("wsocklog", "trace_entry", 0, ini);
            bind_steer = GetPrivateProfileIntA("wsocklog", "bind", 1, ini);
            if (GetPrivateProfileIntA("wsocklog", "watch_pool", 0, ini)) { DWORD id; CloseHandle(CreateThread(NULL, 0, pool_watch, NULL, 0, &id)); }
            GetPrivateProfileStringA("wsocklog", "broadcast", "", b, sizeof(b), ini);
            if (b[0] && get_inet_addr()) bcast_to = get_inet_addr()(b);
            if (bcast_to == 0xFFFFFFFFu) bcast_to = 0;
            if (prefer[0] || bcast_to) lg("steering: prefer \"%s\", broadcast to %s", prefer, b[0] ? b : "(off)");
            if (GetPrivateProfileIntA("wsocklog", "watch_threads", 0, ini)) watch_create_thread();
            if (!GetPrivateProfileIntA("wsocklog", "log", 1, ini)) { lg("log=0: logging stops here"); log_on = 0; }
        }
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}
