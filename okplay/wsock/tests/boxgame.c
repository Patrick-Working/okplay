/* boxgame: a stand-in for a hard full-screen DirectDraw game, to see okplay's boxes over a real exclusive
 * full-screen surface without a real game (docs/BOX_METHOD.md 5.1). It loads okplay wsock (wsock32.dll next to it)
 * as a game does, makes the socket calls of Road Rash (UDP, tests/rrsim.c) or Need for Speed III (TCP,
 * tests/tcpsim.c) so the shim asks its boxes, and writes a log of everything a game would notice.
 * Plain C, ANSI Win32 only, Windows 95/98..11. No sound.
 *
 *   boxgame [--net window|thread|timer] [--kind udp|tcp] [--role host|join] [--slow MS] [--clock MS]
 *           [--seconds N] [--windowed] [--log FILE] [--start MS] [--hold MS] [--addr IP:PORT] [--lo]
 *           [--keys TEXT] [--keydelay MS] [--grab]
 *
 *  --net     which thread makes the network calls: window = the thread of the game window (Road Rash's shape),
 *            thread = a second thread while the window thread keeps drawing (NFS3's), timer = a multimedia timer
 *            callback (default thread)
 *  --kind    udp = Road Rash's pattern (adverts on 6995, join request on 6996; okplay profile roadrash),
 *            tcp = NFS3's (menu listen on 9803, Host Game = close + restart Winsock + listen again; join = connect
 *            loop to an address okplay does not know, so the room list comes; profile nfs3) (default tcp)
 *  --role    host | join (default host)
 *  --slow MS the window thread reads its messages only every MS ms (NFS3 on Windows 11 while it connects)
 *  --clock MS the join clock: any single network call longer than MS logs "FAIL clock ran out"
 *  --seconds N ends by itself after N s, always (default 20); Esc ends it too. The display mode is put back on
 *            every way out (a watchdog ends a run whose window thread is held in a network call)
 *  --windowed the same in a window: no exclusive mode, no display change (dry runs on a PC in use)
 *  --log FILE the log (default boxgame.log next to the exe)
 *  --start MS when the network step begins (default 2000: the game has been drawing in front for a while)
 *  --hold MS how long the host advertises / listens, the joiner looks for adverts (default 3000)
 *  --addr    where a TCP joiner connects (default 127.0.0.1:9804: an address okplay knows no room for)
 *  --lo      bind sockets to 127.0.0.1 (dry runs with okplay out of the way: no firewall question)
 *  --keys T  synthetic keys typed into the first box, KEYDELAY ms after it is up: plain characters and {tab}
 *            {enter} {esc} {space} {bs}; only when the front window is this program's (never another program's)
 *  --grab    the game keeps the pointer: re-traps it every second and puts it in the middle every frame
 *
 * The log: "HH:MM:SS.mmm [tid] text" lines; window messages ("msg: ..."), one "tick:" line a second (frames,
 * front window, minimised), "pointer:" and "front:" lines before and after the network step, "net: begin/end"
 * of every network call with its time, "box:" lines from a watcher thread (a box = a visible dialog window of
 * this process: when it came, when it answered, how many at once, what its field holds, when it went), and a
 * final "SUMMARY key=value ..." line. tests/boxgame_check.py reads it with the okplay log of the same run. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock.h>
#include <mmsystem.h>
#include <ddraw.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 640
#define H 480

/* ------------------------------------------------------------------ settings */
static enum { NET_WINDOW, NET_THREAD, NET_TIMER } net_where = NET_THREAD;
static int kind_tcp = 1, role_host = 1, windowed, use_lo, grab;
static DWORD slow_ms, clock_ms, seconds = 20, start_ms = 2000, hold_ms = 3000, keydelay = 1000;
static char addr_s[64] = "127.0.0.1:9804", keys_s[128], log_path[MAX_PATH];

/* ------------------------------------------------------------------ the log */
static HANDLE log_h = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION log_cs;
static DWORD t_start;

static void logf_(const char *fmt, ...)
{
    char line[1200];
    SYSTEMTIME st;
    va_list ap;
    int n, m;
    DWORD w;
    GetLocalTime(&st);
    n = _snprintf(line, sizeof(line) - 3, "%02d:%02d:%02d.%03d [%5lu] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                  (unsigned long)GetCurrentThreadId());
    va_start(ap, fmt);
    m = _vsnprintf(line + n, sizeof(line) - 3 - n, fmt, ap);
    va_end(ap);
    n = m < 0 ? (int)sizeof(line) - 3 : n + m;
    line[n++] = '\r';
    line[n++] = '\n';
    if (log_h == INVALID_HANDLE_VALUE) return;
    EnterCriticalSection(&log_cs);
    SetFilePointer(log_h, 0, NULL, FILE_END);   /* (Windows 9x: no append access) */
    WriteFile(log_h, line, n, &w, NULL);
    FlushFileBuffers(log_h);
    LeaveCriticalSection(&log_cs);
}
#define LOG logf_

static DWORD ms_since(DWORD t) { DWORD d = GetTickCount() - t; return (LONG)d < 0 ? 0 : d; }   /* (t may be a tick | 1) */

/* ------------------------------------------------------------------ shared state */
static HWND game_wnd;
static DWORD game_tid;
static volatile LONG frames, frames_in_box, quit, net_done, net_failed, clock_failed, net_on_window_busy;
static volatile LONG n_deact, n_minimised, n_killfocus, n_keys, n_keys_in_box, n_lost;
static volatile LONG boxes_open, boxes_seen, boxes_max, keys_sent;
static volatile DWORD net_call_t0;      /* when the network call going on began (0 = none) */
static char net_call_name[64];
static LPDIRECTDRAW dd;
static LPDIRECTDRAWSURFACE prim, back;
static LPDIRECTDRAWCLIPPER clip;
static int bpp;
static volatile LONG screen_put_back;
static volatile LONG ending;          /* after the SUMMARY line: our own DestroyWindow, not counted */

/* ------------------------------------------------------------------ the pointer and the front */
static const char *wnd_desc(HWND w, char *b, int n)   /* "us", "box", "none" or another program's "class 'title'" */
{
    DWORD pid = 0;
    char cls[64] = "", t[64] = "";
    if (!w) return "none";
    GetWindowThreadProcessId(w, &pid);
    GetClassNameA(w, cls, sizeof(cls));
    if (pid == GetCurrentProcessId()) {
        if (w == game_wnd) return "us";
        if (!strcmp(cls, "#32770")) return "box";
        _snprintf(b, n, "ours:%s", cls);
        b[n - 1] = 0;
        return b;
    }
    GetWindowTextA(w, t, sizeof(t));   /* (another program's window: GetWindowText reads only its cached title) */
    _snprintf(b, n, "other:%s '%s'", cls, t);
    b[n - 1] = 0;
    return b;
}

typedef struct { DWORD cbSize, flags; HCURSOR hCursor; POINT pt; } my_cursorinfo;   /* CURSORINFO (not in 95's headers) */
static void log_pointer(const char *when)
{
    static BOOL (WINAPI *gci)(my_cursorinfo *);
    static int looked;
    my_cursorinfo ci;
    RECT c;
    POINT p;
    if (!looked) { looked = 1; gci = (BOOL (WINAPI *)(my_cursorinfo *))GetProcAddress(GetModuleHandleA("user32.dll"), "GetCursorInfo"); }
    memset(&ci, 0, sizeof(ci));
    ci.cbSize = sizeof(ci);
    GetCursorPos(&p);
    GetClipCursor(&c);
    if (gci && gci(&ci)) LOG("pointer: %s: showing=%d at %ld,%ld clip=%ld,%ld-%ld,%ld", when, (ci.flags & 1) ? 1 : 0, p.x, p.y,
                             c.left, c.top, c.right, c.bottom);
    else LOG("pointer: %s: showing=? at %ld,%ld clip=%ld,%ld-%ld,%ld", when, p.x, p.y, c.left, c.top, c.right, c.bottom);
}

static void log_front(const char *when)
{
    char b[160];
    HWND f = GetForegroundWindow();
    LOG("front: %s: %s%s", when, wnd_desc(f, b, sizeof(b)), game_wnd && IsIconic(game_wnd) ? " (game minimised)" : "");
}

/* ------------------------------------------------------------------ the screen */
static void put_screen_back(const char *why)   /* the window thread (or the last way out) */
{
    if (InterlockedExchange(&screen_put_back, 1)) return;
    if (dd) {
        if (!windowed) IDirectDraw_RestoreDisplayMode(dd);
        IDirectDraw_SetCooperativeLevel(dd, game_wnd, DDSCL_NORMAL);
    }
    if (back && !windowed) back = NULL;   /* (part of the flipping chain: goes with the primary) */
    if (back) { IDirectDrawSurface_Release(back); back = NULL; }
    if (clip) { IDirectDrawClipper_Release(clip); clip = NULL; }
    if (prim) { IDirectDrawSurface_Release(prim); prim = NULL; }
    if (dd) { IDirectDraw_Release(dd); dd = NULL; }
    ClipCursor(NULL);
    LOG("screen: display mode put back (%s)", why);
}

static void last_way_out(const char *why)   /* any thread: a run that must end now */
{
    if (!windowed) ChangeDisplaySettingsA(NULL, 0);   /* (cross-thread: DirectDraw's own reset follows at exit) */
    ClipCursor(NULL);
    InterlockedExchange(&screen_put_back, 1);
    LOG("screen: display mode put back (%s, from thread %lu)", why, (unsigned long)GetCurrentThreadId());
}

static const char *module_of(const void *p, char *b, int n)
{
    MEMORY_BASIC_INFORMATION mi;
    char path[MAX_PATH] = "", *s;
    if (!VirtualQuery(p, &mi, sizeof(mi)) || !mi.AllocationBase || !GetModuleFileNameA((HMODULE)mi.AllocationBase, path, sizeof(path)))
        return "(not a module)";
    s = strrchr(path, '\\');
    lstrcpynA(b, s ? s + 1 : path, n);
    return b;
}

static int screen_init(void)
{
    HRESULT (WINAPI *ddc)(GUID *, LPDIRECTDRAW *, IUnknown *);
    HMODULE m = LoadLibraryA("ddraw.dll");
    DDSURFACEDESC sd;
    HRESULT hr;
    char b[MAX_PATH], b2[MAX_PATH];
    static const int depths[3] = { 16, 8, 32 };
    /* DirectDrawCreate by GetProcAddress, as many games of the time: okplay's ddshow.c watches this program's
     * GetProcAddress (and its imports, for games that link ddraw.lib; Windows 98's export table for ddraw.dll is
     * not in winedplay/win98-exports, so an import would not pass check9x) */
    ddc = m ? (HRESULT (WINAPI *)(GUID *, LPDIRECTDRAW *, IUnknown *))GetProcAddress(m, "DirectDrawCreate") : NULL;
    if (!ddc) { LOG("FAIL ddraw: no DirectDrawCreate"); return 0; }
    LOG("ddraw: DirectDrawCreate as got is in %s (ddraw.dll's own = not watched; wsock32.dll = okplay's)", module_of((void *)ddc, b, sizeof(b)));
    hr = ddc(NULL, &dd, NULL);
    if (FAILED(hr)) { LOG("FAIL ddraw: DirectDrawCreate 0x%08lx", (unsigned long)hr); return 0; }
    {
        void **vt = *(void ***)dd;
        LOG("ddraw: object %p: QueryInterface in %s, SetDisplayMode in %s - %s", (void *)dd, module_of(vt[0], b, sizeof(b)),
            module_of(vt[21], b2, sizeof(b2)), _stricmp(b, "ddraw.dll") ? "wrapped (not ddraw.dll's own object)" : "ddraw.dll's own object (not wrapped)");
    }
    if (windowed) {
        hr = IDirectDraw_SetCooperativeLevel(dd, game_wnd, DDSCL_NORMAL);
        memset(&sd, 0, sizeof(sd));
        sd.dwSize = sizeof(sd);
        sd.dwFlags = DDSD_CAPS;
        sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
        if (FAILED(hr = IDirectDraw_CreateSurface(dd, &sd, &prim, NULL))) { LOG("FAIL ddraw: primary 0x%08lx", (unsigned long)hr); return 0; }
        if (SUCCEEDED(IDirectDraw_CreateClipper(dd, 0, &clip, NULL))) {
            IDirectDrawClipper_SetHWnd(clip, 0, game_wnd);
            IDirectDrawSurface_SetClipper(prim, clip);
        }
        memset(&sd, 0, sizeof(sd));
        sd.dwSize = sizeof(sd);
        sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
        sd.dwWidth = W;
        sd.dwHeight = H;
        if (FAILED(hr = IDirectDraw_CreateSurface(dd, &sd, &back, NULL))) { LOG("FAIL ddraw: back 0x%08lx", (unsigned long)hr); return 0; }
        LOG("ddraw: windowed (DDSCL_NORMAL), %dx%d blitted into the window", W, H);
        return 1;
    }
    hr = IDirectDraw_SetCooperativeLevel(dd, game_wnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_ALLOWREBOOT);
    if (FAILED(hr)) { LOG("FAIL ddraw: SetCooperativeLevel exclusive 0x%08lx", (unsigned long)hr); return 0; }
    for (int i = 0; i < 3 && !bpp; i++) {
        hr = IDirectDraw_SetDisplayMode(dd, W, H, depths[i]);
        if (SUCCEEDED(hr)) bpp = depths[i];
        else LOG("ddraw: SetDisplayMode %dx%dx%d refused 0x%08lx", W, H, depths[i], (unsigned long)hr);
    }
    if (!bpp) { LOG("FAIL ddraw: no display mode"); return 0; }
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
    sd.dwBackBufferCount = 1;
    if (FAILED(hr = IDirectDraw_CreateSurface(dd, &sd, &prim, NULL))) { LOG("FAIL ddraw: flipping chain 0x%08lx", (unsigned long)hr); return 0; }
    {
        DDSCAPS caps;
        caps.dwCaps = DDSCAPS_BACKBUFFER;
        if (FAILED(hr = IDirectDrawSurface_GetAttachedSurface(prim, &caps, &back))) { LOG("FAIL ddraw: back buffer 0x%08lx", (unsigned long)hr); return 0; }
        IDirectDrawSurface_Release(back);   /* (owned by the chain; released with the primary) */
    }
    LOG("ddraw: exclusive full screen %dx%dx%d, flipping chain of 2", W, H, bpp);
    return 1;
}

static void draw_frame(void)
{
    HDC dc;
    HRESULT hr;
    LONG f = frames;
    if (!back) return;
    if (SUCCEEDED(IDirectDrawSurface_GetDC(back, &dc))) {
        RECT r = { 0, 0, W, H };
        char t[96];
        HBRUSH bg = CreateSolidBrush(RGB(0, 40, 90)), bar = CreateSolidBrush(RGB(240, 200, 0));
        FillRect(dc, &r, bg);
        r.left = (f * 8) % (W - 40);
        r.right = r.left + 40;
        r.top = 200;
        r.bottom = 280;
        FillRect(dc, &r, bar);
        DeleteObject(bg);
        DeleteObject(bar);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        _snprintf(t, sizeof(t), "boxgame  frame %ld  %lu ms  boxes open %ld", f, (unsigned long)ms_since(t_start), boxes_open);
        TextOutA(dc, 20, 20, t, (int)strlen(t));
        _snprintf(t, sizeof(t), "net %s / %s / %s%s", kind_tcp ? "tcp" : "udp", role_host ? "host" : "join",
                  net_where == NET_WINDOW ? "window" : net_where == NET_THREAD ? "thread" : "timer", net_call_t0 ? "  (in a network call)" : "");
        TextOutA(dc, 20, 44, t, (int)strlen(t));
        IDirectDrawSurface_ReleaseDC(back, dc);
    }
    if (windowed) {
        RECT dst;
        POINT o = { 0, 0 };
        GetClientRect(game_wnd, &dst);
        ClientToScreen(game_wnd, &o);
        OffsetRect(&dst, o.x, o.y);
        hr = IDirectDrawSurface_Blt(prim, &dst, back, NULL, DDBLT_WAIT, NULL);
    } else {
        hr = IDirectDrawSurface_Flip(prim, NULL, DDFLIP_WAIT);
    }
    if (hr == DDERR_SURFACELOST) {
        if (InterlockedIncrement(&n_lost) <= 20) LOG("screen: surface lost (frame %ld) - restoring", f);
        IDirectDrawSurface_Restore(prim);
        if (windowed) IDirectDrawSurface_Restore(back);
    }
    InterlockedIncrement(&frames);
    if (boxes_open) InterlockedIncrement(&frames_in_box);
}

/* ------------------------------------------------------------------ the network step */
static void net_begin(const char *name)
{
    lstrcpynA(net_call_name, name, sizeof(net_call_name));
    net_call_t0 = GetTickCount() | 1;
    LOG("net: begin %s", name);
}

static void net_end(int r)
{
    DWORD d = ms_since(net_call_t0);
    int e = r == SOCKET_ERROR ? WSAGetLastError() : 0;
    if (e) LOG("net: end %s -> %d error %d in %lu ms", net_call_name, r, e, (unsigned long)d);
    else LOG("net: end %s -> %d in %lu ms", net_call_name, r, (unsigned long)d);
    if (clock_ms && d > clock_ms) {
        InterlockedExchange(&clock_failed, 1);
        LOG("FAIL clock ran out: %s took %lu ms (clock %lu ms)", net_call_name, (unsigned long)d, (unsigned long)clock_ms);
    }
    net_call_t0 = 0;
    WSASetLastError(e);
}
#define NET(name, call) (net_begin(name), _r = (int)(call), net_end(_r), _r)

/* a poll (select, FIONREAD, a would-block recv) is logged only when it was slow: a box can hold it too */
static int poll_end(const char *name, DWORD t0, int r)
{
    DWORD d = ms_since(t0);
    if (d >= 100) LOG("net: %s took %lu ms -> %d", name, (unsigned long)d, r);
    if (clock_ms && d > clock_ms) {
        InterlockedExchange(&clock_failed, 1);
        LOG("FAIL clock ran out: %s took %lu ms (clock %lu ms)", name, (unsigned long)d, (unsigned long)clock_ms);
    }
    return r;
}

static void pump_if_window_thread(void);

static void wait_ms(DWORD ms)   /* a game's wait inside its network step (the window thread keeps its messages read) */
{
    DWORD t0 = GetTickCount();
    while (ms_since(t0) < ms && !quit) { pump_if_window_thread(); Sleep(10); }
}

static struct sockaddr_in sa(unsigned long ip, unsigned short port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = ip;
    return a;
}

static unsigned long bind_ip(void) { return use_lo ? inet_addr("127.0.0.1") : INADDR_ANY; }

static SOCKET tcp_listen_on(unsigned short port)
{
    int _r;
    SOCKET s;
    struct sockaddr_in a = sa(bind_ip(), port);
    char n[64];
    net_begin("socket(TCP)");
    s = socket(AF_INET, SOCK_STREAM, 0);
    net_end(s == INVALID_SOCKET ? SOCKET_ERROR : 0);
    if (s == INVALID_SOCKET) return s;
    _snprintf(n, sizeof(n), "bind(TCP, %u)", port);
    if (NET(n, bind(s, (struct sockaddr *)&a, sizeof(a)))) { closesocket(s); return INVALID_SOCKET; }
    if (NET("listen(5)", listen(s, 5))) { closesocket(s); return INVALID_SOCKET; }
    return s;
}

static void tcp_host(void)
{
    int _r;
    SOCKET l, probe;
    WSADATA w;
    net_begin("socket(TCP) probe");
    probe = socket(AF_INET, SOCK_STREAM, 0);
    net_end(0);
    NET("closesocket(probe)", closesocket(probe));
    l = tcp_listen_on(9803);   /* the TCP menu: the game's port */
    LOG("game: at the TCP menu");
    wait_ms(500);
    LOG("game: Host Game");
    NET("closesocket(menu listener)", closesocket(l));
    NET("WSACleanup", WSACleanup());
    NET("WSAStartup", WSAStartup(0x0101, &w));
    net_begin("socket(TCP) probe");
    probe = socket(AF_INET, SOCK_STREAM, 0);
    net_end(0);
    NET("closesocket(probe)", closesocket(probe));
    l = tcp_listen_on(9803);   /* listening again within 2 s: okplay's host rule */
    if (l == INVALID_SOCKET) { InterlockedExchange(&net_failed, 1); return; }
    {
        u_long on = 1;
        NET("ioctlsocket(FIONBIO)", ioctlsocket(l, FIONBIO, &on));
    }
    LOG("game: hosting, waiting for players %lu ms", (unsigned long)hold_ms);
    {
        DWORD t0 = GetTickCount();
        while (ms_since(t0) < hold_ms && !quit) {
            fd_set r;
            struct timeval tv = { 0, 25000 };
            DWORD p0 = GetTickCount();
            r.fd_count = 1;
            r.fd_array[0] = l;
            if (poll_end("select(listen)", p0, select(0, &r, NULL, NULL, &tv)) > 0) {
                SOCKET s = accept(l, NULL, NULL);
                LOG("net: accepted a connection (%s)", s == INVALID_SOCKET ? "failed" : "closed again");
                if (s != INVALID_SOCKET) closesocket(s);
            }
            pump_if_window_thread();
        }
    }
    NET("closesocket(listener)", closesocket(l));
}

static void tcp_join(void)
{
    int _r, attempts = 0;
    char host[64], *colon;
    unsigned short port = 9803;
    SOCKET l = tcp_listen_on(9803);   /* the TCP menu: the joiner keeps its listener */
    struct sockaddr_in to;
    DWORD t0;
    LOG("game: at the TCP menu");
    wait_ms(500);
    lstrcpynA(host, addr_s, sizeof(host));
    if ((colon = strchr(host, ':'))) { *colon = 0; port = (unsigned short)atoi(colon + 1); }
    to = sa(inet_addr(host), port);
    LOG("game: Join %s:%u", host, port);
    t0 = GetTickCount();
    for (;;) {   /* the game's connect loop: a new socket per attempt, 1 s each */
        SOCKET s;
        struct sockaddr_in me = sa(bind_ip(), 0);
        u_long on = 1;
        int e;
        if (ms_since(t0) > (clock_ms ? clock_ms : 10000) || quit) { LOG("game: join gave up after %d attempts", attempts); InterlockedExchange(&net_failed, 1); break; }
        attempts++;
        net_begin("socket(TCP)");
        s = socket(AF_INET, SOCK_STREAM, 0);
        net_end(0);
        NET("bind(TCP, 0)", bind(s, (struct sockaddr *)&me, sizeof(me)));
        NET("ioctlsocket(FIONBIO)", ioctlsocket(s, FIONBIO, &on));
        NET("connect", connect(s, (struct sockaddr *)&to, sizeof(to)));
        e = WSAGetLastError();
        if (e == WSAEWOULDBLOCK) {
            DWORD a0 = GetTickCount();
            int got = 0;
            while (ms_since(a0) < 1000 && !got && !quit) {
                fd_set wr, ex;
                struct timeval tv = { 0, 25000 };
                DWORD p0 = GetTickCount();
                wr.fd_count = ex.fd_count = 1;
                wr.fd_array[0] = ex.fd_array[0] = s;
                int n = poll_end("select(connect)", p0, select(0, NULL, &wr, &ex, &tv));
                if (n > 0) got = FD_ISSET(s, &ex) ? 2 : 1;
                pump_if_window_thread();
            }
            if (got == 1) { LOG("game: connected after %lu ms (%d attempts)", (unsigned long)ms_since(t0), attempts); NET("closesocket", closesocket(s)); break; }
            NET("closesocket", closesocket(s));
            if (got == 2) { LOG("game: connection refused after %lu ms", (unsigned long)ms_since(t0)); InterlockedExchange(&net_failed, 1); break; }
            continue;
        }
        NET("closesocket", closesocket(s));
        if (e && e != WSAEWOULDBLOCK && e != WSAEINPROGRESS && e != WSAEALREADY && e != WSAETIMEDOUT) {
            LOG("game: connect error %d after %lu ms - the join ends", e, (unsigned long)ms_since(t0));
            InterlockedExchange(&net_failed, 1);
            break;
        }
    }
    if (l != INVALID_SOCKET) NET("closesocket(menu listener)", closesocket(l));
}

static unsigned long own_ip(void)
{
    char n[256] = "";
    struct hostent *h;
    unsigned long ip = 0;
    int _r;
    NET("gethostname", gethostname(n, sizeof(n)));
    net_begin("gethostbyname");
    h = gethostbyname(n);
    net_end(h ? 0 : SOCKET_ERROR);
    if (h && h->h_addr_list[0]) memcpy(&ip, h->h_addr_list[0], 4);
    return ip;
}

static SOCKET udp_on(unsigned short port)
{
    int _r;
    SOCKET s;
    struct sockaddr_in a = sa(bind_ip(), port);
    char n[48];
    net_begin("socket(UDP)");
    s = socket(AF_INET, SOCK_DGRAM, 0);
    net_end(s == INVALID_SOCKET ? SOCKET_ERROR : 0);
    _snprintf(n, sizeof(n), "bind(UDP, %u)", port);
    if (s != INVALID_SOCKET) NET(n, bind(s, (struct sockaddr *)&a, sizeof(a)));
    return s;
}

static void udp_host(void)   /* rrsim host: adverts to 255.255.255.255:6995 every 500 ms, join requests on 6996 */
{
    int _r, on = 1;
    unsigned long me = own_ip();
    SOCKET ad = udp_on(6995), jl = udp_on(6996);
    char advert[57];
    struct sockaddr_in bc = sa(use_lo ? inet_addr("127.255.255.255") : INADDR_BROADCAST, 6995);
    DWORD t0 = GetTickCount(), last = 0;
    if (ad == INVALID_SOCKET || jl == INVALID_SOCKET) { InterlockedExchange(&net_failed, 1); return; }
    NET("setsockopt(SO_BROADCAST)", setsockopt(ad, SOL_SOCKET, SO_BROADCAST, (char *)&on, sizeof(on)));
    memset(advert, 0, sizeof(advert));
    advert[0] = 2;
    memcpy(advert + 4, &me, 4);
    advert[17] = 7;
    lstrcpynA(advert + 37, "boxgame", 19);
    LOG("game: hosting, advertising %lu ms", (unsigned long)hold_ms);
    while (ms_since(t0) < hold_ms && !quit) {
        u_long n = 0;
        DWORD p0;
        if (ms_since(last) >= 500) {
            NET("sendto(broadcast advert)", sendto(ad, advert, sizeof(advert), 0, (struct sockaddr *)&bc, sizeof(bc)));
            last = GetTickCount();
        }
        p0 = GetTickCount();
        poll_end("ioctlsocket(FIONREAD)", p0, ioctlsocket(jl, FIONREAD, &n));
        if (n) {
            char req[64];
            int k = recv(jl, req, sizeof(req), 0);
            LOG("net: a join request (%d bytes) - not answered (a stand-in)", k);
        }
        wait_ms(33);
    }
    NET("closesocket(adverts)", closesocket(ad));
    NET("closesocket(join requests)", closesocket(jl));
}

static void udp_join(void)   /* rrsim join: listens on 6995 for an advert, sends a join request to its host:6996 */
{
    int _r;
    unsigned long me = own_ip();
    SOCKET ad = udp_on(6995);
    DWORD t0 = GetTickCount();
    if (ad == INVALID_SOCKET) { InterlockedExchange(&net_failed, 1); return; }
    LOG("game: looking for games %lu ms", (unsigned long)hold_ms);
    while (ms_since(t0) < hold_ms && !quit) {
        u_long n = 0;
        DWORD p0 = GetTickCount();
        poll_end("ioctlsocket(FIONREAD)", p0, ioctlsocket(ad, FIONREAD, &n));
        if (n) {
            char a[256];
            struct sockaddr_in from;
            int fl = sizeof(from), k;
            k = NET("recvfrom(advert)", recvfrom(ad, a, sizeof(a), 0, (struct sockaddr *)&from, &fl));
            if (k == 57) {
                unsigned long hip;
                char req[19];
                SOCKET js = udp_on(6996);
                struct sockaddr_in to;
                memcpy(&hip, a + 4, 4);
                LOG("game: advert \"%.19s\" - joining", a + 37);
                to = sa(hip, 6996);
                memset(req, 0, sizeof(req));
                req[0] = 2;
                memcpy(req + 4, &me, 4);
                NET("sendto(join request)", sendto(js, req, sizeof(req), 0, (struct sockaddr *)&to, sizeof(to)));
                NET("closesocket(join request)", closesocket(js));
                break;
            }
        }
        wait_ms(100);
    }
    NET("closesocket(adverts)", closesocket(ad));
}

static void net_step(void)
{
    DWORD t0 = GetTickCount();
    char b[160];
    log_pointer("before the network step");
    log_front("before the network step");
    LOG("net: step begins (%s %s, on the %s thread)", kind_tcp ? "TCP" : "UDP", role_host ? "host" : "join",
        net_where == NET_WINDOW ? "window" : net_where == NET_THREAD ? "second" : "multimedia timer");
    if (kind_tcp) { if (role_host) tcp_host(); else tcp_join(); }
    else { if (role_host) udp_host(); else udp_join(); }
    LOG("net: step ends after %lu ms", (unsigned long)ms_since(t0));
    log_pointer("after the network step");
    LOG("front: after the network step: %s%s", wnd_desc(GetForegroundWindow(), b, sizeof(b)),
        game_wnd && IsIconic(game_wnd) ? " (game minimised)" : "");
    InterlockedExchange(&net_done, 1);
}

static DWORD WINAPI net_thread(LPVOID a) { (void)a; net_step(); return 0; }
static void CALLBACK net_timer(UINT id, UINT msg, DWORD_PTR user, DWORD_PTR d1, DWORD_PTR d2) { (void)id; (void)msg; (void)user; (void)d1; (void)d2; net_step(); }

/* ------------------------------------------------------------------ the boxes, as the game's process sees them */
typedef struct { HWND w; int n; } box_list;
static BOOL CALLBACK find_boxes(HWND w, LPARAM lp)
{
    box_list *L = (box_list *)lp;
    DWORD pid = 0;
    char cls[16];
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(w) || !GetClassNameA(w, cls, sizeof(cls)) || strcmp(cls, "#32770")) return TRUE;
    if (!L->n) L->w = w;
    L->n++;
    return TRUE;
}

static BOOL CALLBACK find_edit(HWND w, LPARAM lp)
{
    char cls[16];
    if (GetClassNameA(w, cls, sizeof(cls)) && !_stricmp(cls, "Edit")) { *(HWND *)lp = w; return FALSE; }
    return TRUE;
}

static void send_keys(void)
{
    const char *p = keys_s;
    char b[160];
    HWND f = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(f, &pid);
    if (pid != GetCurrentProcessId()) { LOG("keys: not sent - another program is in front (%s)", wnd_desc(f, b, sizeof(b))); return; }
    LOG("keys: typing \"%s\" (front: %s)", keys_s, wnd_desc(f, b, sizeof(b)));
    InterlockedExchange(&keys_sent, 1);
    while (*p) {
        BYTE vk = 0, shift = 0;
        if (*p == '{') {
            const char *e = strchr(p, '}');
            char name[16] = "";
            if (!e) break;
            lstrcpynA(name, p + 1, (int)(e - p) < (int)sizeof(name) ? (int)(e - p) : (int)sizeof(name));
            vk = !_stricmp(name, "tab") ? VK_TAB : !_stricmp(name, "enter") ? VK_RETURN : !_stricmp(name, "esc") ? VK_ESCAPE :
                 !_stricmp(name, "space") ? VK_SPACE : !_stricmp(name, "bs") ? VK_BACK : 0;
            p = e + 1;
        } else {
            SHORT k = VkKeyScanA(*p++);
            if (k == -1) continue;
            vk = LOBYTE(k);
            shift = (HIBYTE(k) & 1) != 0;
        }
        if (!vk) continue;
        if (shift) keybd_event(VK_SHIFT, (BYTE)MapVirtualKeyA(VK_SHIFT, 0), 0, 0);
        keybd_event(vk, (BYTE)MapVirtualKeyA(vk, 0), 0, 0);
        keybd_event(vk, (BYTE)MapVirtualKeyA(vk, 0), KEYEVENTF_KEYUP, 0);
        if (shift) keybd_event(VK_SHIFT, (BYTE)MapVirtualKeyA(VK_SHIFT, 0), KEYEVENTF_KEYUP, 0);
        Sleep(60);
    }
    LOG("keys: done");
}

static DWORD WINAPI box_watch(LPVOID a)
{
    HWND cur = NULL;
    DWORD seen_at = 0, last_ping = 0, call_t0 = 0;
    int answered = 0, keys_done = 0, last_n = 0;
    char edit[96] = "", cname[64] = "";
    (void)a;
    while (!quit) {
        box_list L = { NULL, 0 };
        EnumWindows(find_boxes, (LPARAM)&L);
        if (L.n != last_n) {
            LOG("box: %d box window(s) of this program visible", L.n);
            last_n = L.n;
            if (L.n > boxes_max) InterlockedExchange(&boxes_max, L.n);
            if (L.n > 1) LOG("FAIL two boxes open at once (%d)", L.n);
        }
        InterlockedExchange(&boxes_open, L.n);
        if (L.w != cur) {
            if (cur) LOG("box: closed after %lu ms (field: \"%s\")", (unsigned long)ms_since(seen_at), edit);
            cur = L.w;
            edit[0] = 0;
            if (cur) {
                char t[160] = "", b[160];
                InterlockedIncrement(&boxes_seen);
                seen_at = GetTickCount();
                call_t0 = net_call_t0;
                lstrcpynA(cname, net_call_t0 ? net_call_name : "-", sizeof(cname));
                answered = 0;
                SendMessageTimeoutA(cur, WM_GETTEXT, sizeof(t), (LPARAM)t, SMTO_NORMAL, 200, NULL);
                LOG("box: up (window %p, thread %lu, title \"%s\") - %lu ms into the network call %s; front: %s; topmost: %s",
                    (void *)cur, (unsigned long)GetWindowThreadProcessId(cur, NULL), t,
                    (unsigned long)(call_t0 ? seen_at - call_t0 : 0), cname, wnd_desc(GetForegroundWindow(), b, sizeof(b)),
                    (GetWindowLongA(cur, GWL_EXSTYLE) & WS_EX_TOPMOST) ? "yes" : "no");
            }
        }
        if (cur) {
            DWORD_PTR r;
            DWORD p0 = GetTickCount();
            HWND ed = NULL;
            if (!answered || ms_since(last_ping) >= 1000) {
                BOOL ok = SendMessageTimeoutA(cur, WM_NULL, 0, 0, SMTO_NORMAL, 3000, &r) != 0;
                DWORD d = ms_since(p0);
                last_ping = GetTickCount();
                if (!answered && ok) {
                    answered = 1;
                    LOG("box: usable %lu ms after it came up (%lu ms after the network call began; its thread answered in %lu ms)",
                        (unsigned long)ms_since(seen_at), (unsigned long)(call_t0 ? GetTickCount() - call_t0 : 0), (unsigned long)d);
                } else if (!ok || d > 500) {
                    LOG("box: its thread took %lu ms to answer%s", (unsigned long)d, ok ? "" : " (no answer in 3 s)");
                }
            }
            EnumChildWindows(cur, find_edit, (LPARAM)&ed);
            if (ed) {
                char t[96] = "";
                SendMessageTimeoutA(ed, WM_GETTEXT, sizeof(t), (LPARAM)t, SMTO_NORMAL, 200, NULL);
                if (strcmp(t, edit)) { lstrcpynA(edit, t, sizeof(edit)); LOG("box: field now \"%s\"", edit); }
            }
            if (keys_s[0] && !keys_done && ms_since(seen_at) >= keydelay) { keys_done = 1; send_keys(); }
        }
        Sleep(50);
    }
    return 0;
}

/* ------------------------------------------------------------------ the window */
static const char *msg_name(UINT m)
{
    switch (m) {
    case WM_ACTIVATEAPP: return "WM_ACTIVATEAPP";
    case WM_ACTIVATE: return "WM_ACTIVATE";
    case WM_NCACTIVATE: return "WM_NCACTIVATE";
    case WM_SETFOCUS: return "WM_SETFOCUS";
    case WM_KILLFOCUS: return "WM_KILLFOCUS";
    case WM_SIZE: return "WM_SIZE";
    case WM_SYSCOMMAND: return "WM_SYSCOMMAND";
    case WM_KEYDOWN: return "WM_KEYDOWN";
    case WM_KEYUP: return "WM_KEYUP";
    case WM_CHAR: return "WM_CHAR";
    case WM_SYSKEYDOWN: return "WM_SYSKEYDOWN";
    case WM_SYSKEYUP: return "WM_SYSKEYUP";
    case WM_SYSCHAR: return "WM_SYSCHAR";
    case WM_DISPLAYCHANGE: return "WM_DISPLAYCHANGE";
    }
    return NULL;
}

static LRESULT CALLBACK wnd_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    const char *n = msg_name(m);
    if (n && ending) LOG("msg: %s wp %lx lp %lx (while ending)", n, (unsigned long)wp, (unsigned long)lp);
    else if (n) {
        const char *note = "";
        if ((m == WM_ACTIVATEAPP && !wp) || (m == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) || (m == WM_NCACTIVATE && !wp)) {
            note = " - deactivated";
            InterlockedIncrement(&n_deact);
        }
        if (m == WM_KILLFOCUS) { note = " - lost the keyboard"; InterlockedIncrement(&n_killfocus); }
        if (m == WM_SIZE && wp == SIZE_MINIMIZED) { note = " - MINIMISED"; InterlockedIncrement(&n_minimised); }
        if (m == WM_SYSCOMMAND && (wp & 0xfff0) == SC_MINIMIZE) note = " - minimise asked";
        if (m >= WM_KEYFIRST && m <= WM_KEYLAST) {
            InterlockedIncrement(&n_keys);
            if (boxes_open) { InterlockedIncrement(&n_keys_in_box); note = " - KEY WHILE A BOX IS OPEN"; }
        }
        LOG("msg: %s wp %lx lp %lx%s", n, (unsigned long)wp, (unsigned long)lp, note);
    }
    switch (m) {
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(NULL); return TRUE; }
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && !boxes_open) { LOG("game: Esc - ending"); InterlockedExchange(&quit, 1); }
        return 0;
    case WM_CLOSE:
        InterlockedExchange(&quit, 1);
        return 0;
    case WM_ACTIVATEAPP:
        if (wp && !windowed && prim) IDirectDrawSurface_Restore(prim);
        break;
    }
    return DefWindowProcA(w, m, wp, lp);
}

static void pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) InterlockedExchange(&quit, 1);
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

static void pump_if_window_thread(void)   /* a network step on the window thread: the game still draws between its calls */
{
    static DWORD last;
    if (GetCurrentThreadId() != game_tid) return;
    if (slow_ms && ms_since(last) < slow_ms) return;
    last = GetTickCount();
    pump();
    draw_frame();
}

static void grab_pointer(void)
{
    RECT r;
    GetWindowRect(game_wnd, &r);
    ClipCursor(&r);
}

/* ------------------------------------------------------------------ the last ways out */
static DWORD WINAPI watchdog(LPVOID a)
{
    (void)a;
    while (ms_since(t_start) < seconds * 1000 + 3000) Sleep(100);
    LOG("watchdog: the run did not end %lu s after it began (the window thread is %s) - ending it here",
        (unsigned long)seconds, net_on_window_busy ? "held in a network call" : "not answering");
    last_way_out("watchdog");
    LOG("SUMMARY result=watchdog frames=%ld deactivated=%ld minimised=%ld killfocus=%ld keys=%ld keys_in_box=%ld boxes_seen=%ld "
        "boxes_max=%ld surface_lost=%ld clock=%s net=%s front_end=? frames_in_box=%ld",
        frames, n_deact, n_minimised, n_killfocus, n_keys, n_keys_in_box, boxes_seen, boxes_max, n_lost,
        !clock_ms ? "off" : clock_failed ? "ran_out" : "ok", net_done ? (net_failed ? "failed" : "done") : "unfinished", frames_in_box);
    ExitProcess(3);
    return 0;
}

static LONG WINAPI on_crash(EXCEPTION_POINTERS *e)
{
    LOG("FAIL crash: exception 0x%08lx at %p", (unsigned long)e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress);
    last_way_out("crash");
    return EXCEPTION_EXECUTE_HANDLER;
}

/* MinGW's own _matherr (pulled in by its start-up code) is compiled with SSE2; boxgame does no floating point, and
 * this one keeps the exe to Pentium MMX instructions (check9x.sh) */
int __cdecl _matherr(struct _exception *e) { (void)e; return 0; }

/* ------------------------------------------------------------------ main */
static void usage(void)
{
    MessageBoxA(NULL, "boxgame [--net window|thread|timer] [--kind udp|tcp] [--role host|join] [--slow MS] [--clock MS]\n"
                "[--seconds N] [--windowed] [--log FILE] [--start MS] [--hold MS] [--addr IP:PORT] [--lo]\n"
                "[--keys TEXT] [--keydelay MS] [--grab]", "boxgame", MB_OK);
}

/* main, not WinMain: MinGW's own main that calls WinMain is built with P6 instructions (check9x.sh); a GUI program
 * (-mwindows) may start at main as well */
int main(int argc, char **argv)
{
    HINSTANCE inst = GetModuleHandleA(NULL);
    WNDCLASSA wc;
    WSADATA wsa;
    DWORD last_tick = 0, last_frames = 0, id;
    int net_started = 0, ok;
    char b[MAX_PATH], b2[160];
    UINT timer = 0;
    t_start = GetTickCount();
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(a, "--net")) { net_where = !strcmp(v, "window") ? NET_WINDOW : !strcmp(v, "timer") ? NET_TIMER : NET_THREAD; i++; }
        else if (!strcmp(a, "--kind")) { kind_tcp = strcmp(v, "udp") != 0; i++; }
        else if (!strcmp(a, "--role")) { role_host = strcmp(v, "join") != 0; i++; }
        else if (!strcmp(a, "--slow")) { slow_ms = (DWORD)atoi(v); i++; }
        else if (!strcmp(a, "--clock")) { clock_ms = (DWORD)atoi(v); i++; }
        else if (!strcmp(a, "--seconds")) { seconds = (DWORD)atoi(v); if (!seconds) seconds = 20; i++; }
        else if (!strcmp(a, "--start")) { start_ms = (DWORD)atoi(v); i++; }
        else if (!strcmp(a, "--hold")) { hold_ms = (DWORD)atoi(v); i++; }
        else if (!strcmp(a, "--addr")) { lstrcpynA(addr_s, v, sizeof(addr_s)); i++; }
        else if (!strcmp(a, "--keys")) { lstrcpynA(keys_s, v, sizeof(keys_s)); i++; }
        else if (!strcmp(a, "--keydelay")) { keydelay = (DWORD)atoi(v); i++; }
        else if (!strcmp(a, "--log")) { lstrcpynA(log_path, v, sizeof(log_path)); i++; }
        else if (!strcmp(a, "--windowed")) windowed = 1;
        else if (!strcmp(a, "--lo")) use_lo = 1;
        else if (!strcmp(a, "--grab")) grab = 1;
        else { usage(); return 2; }
    }
    if (seconds > 600) seconds = 600;
    if (!log_path[0]) {
        char *s;
        GetModuleFileNameA(NULL, log_path, sizeof(log_path));
        s = strrchr(log_path, '\\');
        lstrcpyA(s ? s + 1 : log_path, "boxgame.log");
    }
    InitializeCriticalSection(&log_cs);
    log_h = CreateFileA(log_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    SetUnhandledExceptionFilter(on_crash);
    LOG("boxgame 1 (docs/BOX_METHOD.md 5.1): net %s, kind %s, role %s, slow %lu ms, clock %lu ms, seconds %lu, %s, start %lu ms, hold %lu ms%s%s%s",
        net_where == NET_WINDOW ? "window" : net_where == NET_THREAD ? "thread" : "timer", kind_tcp ? "tcp" : "udp", role_host ? "host" : "join",
        (unsigned long)slow_ms, (unsigned long)clock_ms, (unsigned long)seconds, windowed ? "windowed" : "full screen",
        (unsigned long)start_ms, (unsigned long)hold_ms, use_lo ? ", loopback" : "", grab ? ", grab" : "", keys_s[0] ? ", keys" : "");
    {
        HMODULE ws = GetModuleHandleA("wsock32.dll");
        if (ws && GetModuleFileNameA(ws, b, sizeof(b))) LOG("wsock32.dll loaded from %s", b);
        else LOG("wsock32.dll not loaded?");
    }
    CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, &id));
    log_pointer("at start");
    log_front("at start");

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = NULL;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "boxgame";
    RegisterClassA(&wc);
    if (windowed) {
        RECT r = { 0, 0, W, H };
        AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
        game_wnd = CreateWindowExA(0, "boxgame", "boxgame (windowed)", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                   40, 40, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    } else {
        game_wnd = CreateWindowExA(WS_EX_TOPMOST, "boxgame", "boxgame", WS_POPUP, 0, 0, GetSystemMetrics(SM_CXSCREEN),
                                   GetSystemMetrics(SM_CYSCREEN), NULL, NULL, inst, NULL);
    }
    game_tid = GetCurrentThreadId();
    ShowWindow(game_wnd, SW_SHOW);
    UpdateWindow(game_wnd);
    SetForegroundWindow(game_wnd);
    SetFocus(game_wnd);
    pump();
    ok = screen_init();
    ShowCursor(FALSE);
    grab_pointer();
    LOG("game: window %p (thread %lu), pointer hidden and kept inside the window", (void *)game_wnd, (unsigned long)game_tid);
    CloseHandle(CreateThread(NULL, 0, box_watch, NULL, 0, &id));
    if (WSAStartup(0x0101, &wsa)) LOG("FAIL WSAStartup");
    if (net_where == NET_TIMER) {
        timer = timeSetEvent(start_ms, 10, net_timer, 0, TIME_ONESHOT);
        net_started = 1;
        LOG("net: multimedia timer %u set for %lu ms", timer, (unsigned long)start_ms);
    }
    while (ok && !quit && ms_since(t_start) < seconds * 1000) {
        DWORD now = GetTickCount();
        pump();
        if (quit) break;
        if (!net_started && now - t_start >= start_ms) {
            net_started = 1;
            if (net_where == NET_WINDOW) {
                InterlockedExchange(&net_on_window_busy, 1);
                net_step();
                InterlockedExchange(&net_on_window_busy, 0);
            } else {
                CloseHandle(CreateThread(NULL, 0, net_thread, NULL, 0, &id));
            }
        }
        draw_frame();
        if (grab) {
            RECT r;
            GetWindowRect(game_wnd, &r);
            SetCursorPos((r.left + r.right) / 2, (r.top + r.bottom) / 2);
        }
        if (now - last_tick >= 1000) {
            LOG("tick: %lu ms, frames %ld (+%lu), front %s%s, boxes open %ld", (unsigned long)(now - t_start), frames,
                (unsigned long)(frames - last_frames), wnd_desc(GetForegroundWindow(), b2, sizeof(b2)),
                IsIconic(game_wnd) ? ", MINIMISED" : "", boxes_open);
            if (IsIconic(game_wnd)) InterlockedIncrement(&n_minimised);
            last_frames = frames;
            last_tick = now;
            if (grab) grab_pointer();
        }
        if (slow_ms) Sleep(slow_ms);
        else if (windowed) Sleep(15);
    }
    InterlockedExchange(&quit, 1);
    log_pointer("at the end");
    {
        HWND f = GetForegroundWindow();
        const char *fe = wnd_desc(f, b2, sizeof(b2));
        LOG("front: at the end: %s%s", fe, IsIconic(game_wnd) ? " (game minimised)" : "");
        put_screen_back("end of run");
        ShowCursor(TRUE);
        if (timer) timeKillEvent(timer);
        LOG("SUMMARY result=%s frames=%ld deactivated=%ld minimised=%ld killfocus=%ld keys=%ld keys_in_box=%ld boxes_seen=%ld "
            "boxes_max=%ld surface_lost=%ld clock=%s net=%s front_end=%s frames_in_box=%ld",
            ok ? "ended" : "no_screen", frames, n_deact, n_minimised, n_killfocus, n_keys, n_keys_in_box, boxes_seen, boxes_max,
            n_lost, !clock_ms ? "off" : clock_failed ? "ran_out" : "ok",
            net_done ? (net_failed ? "failed" : "done") : "unfinished", strncmp(fe, "us", 3) ? "other" : "us", frames_in_box);
    }
    InterlockedExchange(&ending, 1);
    DestroyWindow(game_wnd);
    ExitProcess(0);   /* (a network call still in a box on another thread ends with the process) */
    return 0;
}
