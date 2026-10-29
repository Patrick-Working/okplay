/* okagent - remote control for test automation, run INSIDE the machine being tested (a Windows 98
 * VM in 86Box, a real old PC, or a modern Windows). It dials the host's bridge (tools/vmctl.py) and
 * does what it is told: screenshots, mouse and keyboard through the machine's own input functions
 * (so nothing goes through 86Box's emulated hardware), windows, programs, files.
 *
 *   okagent.exe [host[:port]] [token]       defaults: okagent.ini next to the exe, then 10.0.2.2:7790
 *
 * okagent.ini ([okagent] host= port= token= name=). 10.0.2.2 is the host as 86Box's NAT networking
 * (SLiRP) shows it, so a VM needs no port forwarding: the agent connects out, the bridge listens on
 * the host's loopback. It reconnects every few seconds while the bridge is down.
 *
 * Protocol (both ways): u32 little-endian length, then that many bytes: a text header line ending
 * in '\n', then optional binary data. First frame from the agent: "hello <token> name=.. os=.. ...".
 * Requests are command lines (see do_command); replies are "ok ..." or "err ..." plus data.
 *
 * Windows 95/98 through 11 from one exe, Pentium MMX (check9x.sh): ANSI functions, wsock32, things
 * newer than Windows 95 looked up at run time. Clean room: nothing here comes from Microsoft code. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock.h>
#include <tlhelp32.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>   /* struct _exception, for our _matherr */

#define AGENT_VERSION "0.1"
#define DEFAULT_PORT 7790
#define MAX_FRAME (64u * 1024 * 1024)

static char g_host[128] = "10.0.2.2", g_token[96] = "", g_name[64] = "";
static int g_port = DEFAULT_PORT;
static HWND g_status_wnd;
static char g_status[256] = "okagent starting";

/* ------------------------------------------------------------------ growable reply buffer */
typedef struct { char *p; DWORD n, cap; } buf;

static int buf_add(buf *b, const void *data, DWORD len)
{
    if (b->n + len > b->cap) {
        DWORD cap = b->cap ? b->cap : 4096;
        char *np;
        while (cap < b->n + len) cap *= 2;
        if (!(np = (char *)realloc(b->p, cap))) return 0;
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->n, data, len);
    b->n += len;
    return 1;
}

static int buf_printf(buf *b, const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = _vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) n = sizeof(line) - 1;
    line[n] = 0;
    return buf_add(b, line, (DWORD)n);
}

/* ------------------------------------------------------------------ status window */
static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(g_status, sizeof(g_status) - 1, fmt, ap);
    va_end(ap);
    g_status[sizeof(g_status) - 1] = 0;
    if (g_status_wnd) SetWindowTextA(g_status_wnd, g_status);
}

/* ------------------------------------------------------------------ socket I/O */
static int send_all(SOCKET s, const char *p, int n)
{
    while (n > 0) {
        int k = send(s, p, n, 0);
        if (k <= 0) return 0;
        p += k;
        n -= k;
    }
    return 1;
}

static int recv_all(SOCKET s, char *p, int n)
{
    while (n > 0) {
        int k = recv(s, p, n, 0);
        if (k <= 0) return 0;
        p += k;
        n -= k;
    }
    return 1;
}

static int send_frame(SOCKET s, const char *header, const char *data, DWORD dlen)
{
    DWORD hl = (DWORD)strlen(header), total = hl + 1 + dlen;
    unsigned char len[4] = { (unsigned char)total, (unsigned char)(total >> 8), (unsigned char)(total >> 16),
                             (unsigned char)(total >> 24) };
    return send_all(s, (const char *)len, 4) && send_all(s, header, (int)hl) && send_all(s, "\n", 1) &&
           (!dlen || send_all(s, data, (int)dlen));
}

/* ------------------------------------------------------------------ screenshots */
/* a 24-bit .bmp of a screen rectangle (the whole screen when w = 0) */
static int screenshot(int x, int y, int w, int h, buf *out)
{
    HDC screen = GetDC(NULL), mem;
    BITMAPINFO bi;
    BITMAPFILEHEADER fh;
    HBITMAP bmp, old;
    void *bits = NULL;
    DWORD stride, size;
    int ok = 0;

    if (!screen) return 0;
    if (w <= 0 || h <= 0) {
        x = y = 0;
        w = GetSystemMetrics(SM_CXSCREEN);
        h = GetSystemMetrics(SM_CYSCREEN);
    }
    mem = CreateCompatibleDC(screen);
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h;   /* bottom-up, as a .bmp file stores it */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (mem && bmp && bits) {
        old = (HBITMAP)SelectObject(mem, bmp);
        BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY);
        GdiFlush();
        SelectObject(mem, old);
        stride = ((DWORD)w * 3 + 3) & ~3u;
        size = stride * (DWORD)h;
        memset(&fh, 0, sizeof(fh));
        fh.bfType = 0x4d42;
        fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
        fh.bfSize = fh.bfOffBits + size;
        bi.bmiHeader.biSizeImage = size;
        ok = buf_add(out, &fh, sizeof(fh)) && buf_add(out, &bi.bmiHeader, sizeof(bi.bmiHeader)) &&
             buf_add(out, bits, size);
    }
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    ReleaseDC(NULL, screen);
    return ok;
}

/* ------------------------------------------------------------------ input */
static void mouse_at(int x, int y)
{
    SetCursorPos(x, y);
    /* an absolute move event too: some games only notice movement that arrives as input */
    mouse_event(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, (DWORD)(x * 65535 / (GetSystemMetrics(SM_CXSCREEN) - 1)),
                (DWORD)(y * 65535 / (GetSystemMetrics(SM_CYSCREEN) - 1)), 0, 0);
}

static void button(int right, int down)
{
    mouse_event(right ? (down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP)
                      : (down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP), 0, 0, 0, 0);
}

static void key_event(BYTE vk, int up)
{
    BYTE scan = (BYTE)MapVirtualKeyA(vk, 0);
    DWORD flags = up ? KEYEVENTF_KEYUP : 0;
    if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_HOME || vk == VK_END ||
        vk == VK_PRIOR || vk == VK_NEXT || vk == VK_INSERT || vk == VK_DELETE)
        flags |= KEYEVENTF_EXTENDEDKEY;
    keybd_event(vk, scan, flags, 0);
}

static const struct { const char *name; BYTE vk; } key_names[] = {
    { "enter", VK_RETURN }, { "return", VK_RETURN }, { "esc", VK_ESCAPE }, { "escape", VK_ESCAPE },
    { "tab", VK_TAB }, { "space", VK_SPACE }, { "back", VK_BACK }, { "backspace", VK_BACK },
    { "delete", VK_DELETE }, { "del", VK_DELETE }, { "insert", VK_INSERT }, { "home", VK_HOME },
    { "end", VK_END }, { "pgup", VK_PRIOR }, { "pgdn", VK_NEXT }, { "up", VK_UP }, { "down", VK_DOWN },
    { "left", VK_LEFT }, { "right", VK_RIGHT }, { "ctrl", VK_CONTROL }, { "shift", VK_SHIFT },
    { "alt", VK_MENU }, { "win", VK_LWIN }, { "pause", VK_PAUSE }, { "f1", VK_F1 }, { "f2", VK_F2 },
    { "f3", VK_F3 }, { "f4", VK_F4 }, { "f5", VK_F5 }, { "f6", VK_F6 }, { "f7", VK_F7 }, { "f8", VK_F8 },
    { "f9", VK_F9 }, { "f10", VK_F10 }, { "f11", VK_F11 }, { "f12", VK_F12 },
};

static int vk_of(const char *name)
{
    size_t i;
    for (i = 0; i < sizeof(key_names) / sizeof(key_names[0]); i++)
        if (!lstrcmpiA(name, key_names[i].name)) return key_names[i].vk;
    if (name[0] && !name[1]) {   /* one character: its key on this keyboard layout */
        SHORT k = VkKeyScanA(name[0]);
        return k == -1 ? -1 : LOBYTE(k);
    }
    if ((name[0] == '0' && (name[1] == 'x' || name[1] == 'X'))) return (int)strtoul(name, NULL, 16);
    return -1;
}

/* "ctrl+shift+s": modifiers down, the key down and up, modifiers up */
static int press_combo(const char *combo, const char *mode)
{
    char tmp[128], *parts[8], *tok;
    int vks[8], n = 0, i, down = !mode || !lstrcmpiA(mode, "down"), up = !mode || !lstrcmpiA(mode, "up");

    if (mode && !down && !up) down = up = 1;
    if (mode && !lstrcmpiA(mode, "press")) down = up = 1;
    lstrcpynA(tmp, combo, sizeof(tmp));
    for (tok = strtok(tmp, "+"); tok && n < 8; tok = strtok(NULL, "+")) parts[n++] = tok;
    for (i = 0; i < n; i++)
        if ((vks[i] = vk_of(parts[i])) < 0) return 0;
    if (down)
        for (i = 0; i < n; i++) { key_event((BYTE)vks[i], 0); Sleep(20); }
    if (up)
        for (i = n - 1; i >= 0; i--) { key_event((BYTE)vks[i], 1); Sleep(20); }
    return 1;
}

static void type_text(const char *s)
{
    for (; *s; s++) {
        SHORT k = VkKeyScanA(*s);
        BYTE vk, mods;
        if (k == -1) continue;
        vk = LOBYTE(k);
        mods = HIBYTE(k);
        if (mods & 1) key_event(VK_SHIFT, 0);
        if (mods & 2) key_event(VK_CONTROL, 0);
        if (mods & 4) key_event(VK_MENU, 0);
        key_event(vk, 0);
        key_event(vk, 1);
        if (mods & 4) key_event(VK_MENU, 1);
        if (mods & 2) key_event(VK_CONTROL, 1);
        if (mods & 1) key_event(VK_SHIFT, 1);
        Sleep(15);
    }
}

/* ------------------------------------------------------------------ windows */
typedef struct { buf *out; int all; } enum_ctx;

static BOOL CALLBACK list_window(HWND w, LPARAM lp)
{
    enum_ctx *c = (enum_ctx *)lp;
    char title[200] = "", cls[80] = "";
    RECT r;
    DWORD pid = 0;
    if (!c->all && (!IsWindowVisible(w) || GetWindow(w, GW_OWNER))) return TRUE;
    GetWindowTextA(w, title, sizeof(title));
    if (!c->all && !title[0]) return TRUE;
    GetClassNameA(w, cls, sizeof(cls));
    GetWindowRect(w, &r);
    GetWindowThreadProcessId(w, &pid);
    buf_printf(c->out, "%lu\t%lu\t%ld,%ld,%ld,%ld\t%s\t%s\n", (unsigned long)(ULONG_PTR)w, (unsigned long)pid,
               (long)r.left, (long)r.top, (long)(r.right - r.left), (long)(r.bottom - r.top), cls, title);
    return TRUE;
}

typedef struct { const char *needle; HWND found; } find_ctx;

static BOOL CALLBACK find_window(HWND w, LPARAM lp)
{
    find_ctx *f = (find_ctx *)lp;
    char title[200] = "";
    if (!IsWindowVisible(w)) return TRUE;
    GetWindowTextA(w, title, sizeof(title));
    CharLowerA(title);
    if (title[0] && strstr(title, f->needle)) { f->found = w; return FALSE; }
    return TRUE;
}

/* a window by number, or the first visible one whose title contains the text (any case) */
static HWND window_arg(const char *a)
{
    char low[200];
    find_ctx f;
    char *end;
    unsigned long n = strtoul(a, &end, 10);
    if (*a && !*end && IsWindow((HWND)(ULONG_PTR)n)) return (HWND)(ULONG_PTR)n;
    lstrcpynA(low, a, sizeof(low));
    CharLowerA(low);
    f.needle = low;
    f.found = NULL;
    EnumWindows(find_window, (LPARAM)&f);
    return f.found;
}

/* Windows 98 and later only bring a window forward for the program that had the last input:
 * borrow the foreground thread's input state for a moment (works from 95 to 11) */
static int bring_front(HWND w)
{
    DWORD me = GetCurrentThreadId(), fg = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
    DWORD target = GetWindowThreadProcessId(w, NULL);
    if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
    if (fg && fg != me) AttachThreadInput(me, fg, TRUE);
    if (target && target != me) AttachThreadInput(me, target, TRUE);
    BringWindowToTop(w);
    SetForegroundWindow(w);
    if (target && target != me) AttachThreadInput(me, target, FALSE);
    if (fg && fg != me) AttachThreadInput(me, fg, FALSE);
    return GetForegroundWindow() == w;
}

/* ------------------------------------------------------------------ processes (Toolhelp: 95/98/2000+) */
typedef HANDLE (WINAPI *snap_t)(DWORD, DWORD);
typedef BOOL (WINAPI *proc_t)(HANDLE, PROCESSENTRY32 *);

static int list_processes(buf *out)
{
    HMODULE k = GetModuleHandleA("kernel32.dll");
    snap_t snap = (snap_t)GetProcAddress(k, "CreateToolhelp32Snapshot");
    proc_t first = (proc_t)GetProcAddress(k, "Process32First"), next = (proc_t)GetProcAddress(k, "Process32Next");
    PROCESSENTRY32 pe;
    HANDLE h;
    if (!snap || !first || !next) return 0;   /* NT 4 has no Toolhelp */
    if ((h = snap(TH32CS_SNAPPROCESS, 0)) == INVALID_HANDLE_VALUE) return 0;
    pe.dwSize = sizeof(pe);
    for (BOOL more = first(h, &pe); more; more = next(h, &pe))
        buf_printf(out, "%lu\t%s\n", (unsigned long)pe.th32ProcessID, pe.szExeFile);
    CloseHandle(h);
    return 1;
}

/* ------------------------------------------------------------------ files */
static int read_file(const char *path, buf *out)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    char chunk[16384];
    DWORD got;
    if (f == INVALID_HANDLE_VALUE) return 0;
    while (ReadFile(f, chunk, sizeof(chunk), &got, NULL) && got) buf_add(out, chunk, got);
    CloseHandle(f);
    return 1;
}

static int write_file(const char *path, const char *data, DWORD len)
{
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD put = 0;
    BOOL ok;
    if (f == INVALID_HANDLE_VALUE) return 0;
    ok = WriteFile(f, data, len, &put, NULL) && put == len;
    CloseHandle(f);
    return ok;
}

static int list_dir(const char *pattern, buf *out)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        buf_printf(out, "%s\t%lu\t%s\n", (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "dir" : "file",
                   (unsigned long)fd.nFileSizeLow, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 1;
}

/* ------------------------------------------------------------------ commands */
/* next space-separated word of *p (NULL at the end); the rest stays in *p */
static char *word(char **p)
{
    char *s = *p, *w;
    while (*s == ' ') s++;
    if (!*s) { *p = s; return NULL; }
    w = s;
    while (*s && *s != ' ') s++;
    if (*s) *s++ = 0;
    *p = s;
    return w;
}

static int g_quit;

/* one request -> reply header (hdr) and data (out). Returns 0 when the connection should close. */
static void do_command(char *line, const char *data, DWORD dlen, char *hdr, size_t hsize, buf *out)
{
    char *p = line, *cmd = word(&p), *a, *b;
    int x, y;

#define OK(...) _snprintf(hdr, hsize - 1, "ok " __VA_ARGS__)
#define ERR(...) _snprintf(hdr, hsize - 1, "err " __VA_ARGS__)
    hdr[hsize - 1] = 0;
    if (!cmd) { ERR("empty command"); return; }
    set_status("okagent: %s", cmd);

    if (!lstrcmpiA(cmd, "ping")) OK("pong");
    else if (!lstrcmpiA(cmd, "info")) {
        OSVERSIONINFOA v;
        v.dwOSVersionInfoSize = sizeof(v);
        GetVersionExA(&v);
        OK("agent=%s name=%s os=%lu.%lu.%lu platform=%lu screen=%dx%d bpp=%d", AGENT_VERSION, g_name,
           (unsigned long)v.dwMajorVersion, (unsigned long)v.dwMinorVersion, (unsigned long)(v.dwBuildNumber & 0xffff),
           (unsigned long)v.dwPlatformId, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
           GetDeviceCaps(GetDC(NULL), BITSPIXEL));
    }
    else if (!lstrcmpiA(cmd, "shot")) {   /* shot [x y w h | window] */
        int w = 0, h = 0;
        x = y = 0;
        a = word(&p);
        if (a && (b = word(&p))) {
            char *c = word(&p), *d = word(&p);
            x = atoi(a); y = atoi(b); w = c ? atoi(c) : 0; h = d ? atoi(d) : 0;
        } else if (a) {
            HWND wnd = window_arg(a);
            RECT r;
            if (!wnd) { ERR("no window %s", a); return; }
            GetWindowRect(wnd, &r);
            x = r.left; y = r.top; w = r.right - r.left; h = r.bottom - r.top;
        }
        if (screenshot(x, y, w, h, out)) OK("bmp %lu", (unsigned long)out->n);
        else ERR("screenshot failed");
    }
    else if (!lstrcmpiA(cmd, "move") || !lstrcmpiA(cmd, "click") || !lstrcmpiA(cmd, "rclick") ||
             !lstrcmpiA(cmd, "dclick")) {   /* click x y [hold_ms] */
        char *hold;
        int right = !lstrcmpiA(cmd, "rclick"), n = !lstrcmpiA(cmd, "dclick") ? 2 : 1, i;
        if (!(a = word(&p)) || !(b = word(&p))) { ERR("usage: %s x y", cmd); return; }
        hold = word(&p);
        mouse_at(atoi(a), atoi(b));
        if (lstrcmpiA(cmd, "move")) {
            Sleep(60);
            for (i = 0; i < n; i++) {
                button(right, 1);
                Sleep(hold ? atoi(hold) : 80);
                button(right, 0);
                Sleep(80);
            }
        }
        OK("%s %s %s", cmd, a, b);
    }
    else if (!lstrcmpiA(cmd, "down") || !lstrcmpiA(cmd, "up")) {   /* down|up [left|right] */
        a = word(&p);
        button(a && !lstrcmpiA(a, "right"), !lstrcmpiA(cmd, "down"));
        OK("%s", cmd);
    }
    else if (!lstrcmpiA(cmd, "drag")) {   /* drag x1 y1 x2 y2 */
        char *c, *d;
        int i, x1, y1, x2, y2;
        if (!(a = word(&p)) || !(b = word(&p)) || !(c = word(&p)) || !(d = word(&p))) { ERR("usage: drag x1 y1 x2 y2"); return; }
        x1 = atoi(a); y1 = atoi(b); x2 = atoi(c); y2 = atoi(d);
        mouse_at(x1, y1); Sleep(60); button(0, 1);
        for (i = 1; i <= 10; i++) { mouse_at(x1 + (x2 - x1) * i / 10, y1 + (y2 - y1) * i / 10); Sleep(30); }
        button(0, 0);
        OK("drag");
    }
    else if (!lstrcmpiA(cmd, "key")) {   /* key ctrl+s [down|up|press] */
        if (!(a = word(&p))) { ERR("usage: key name[+name] [down|up]"); return; }
        if (press_combo(a, word(&p))) OK("key %s", a);
        else ERR("unknown key in %s", a);
    }
    else if (!lstrcmpiA(cmd, "type")) {   /* type <the rest of the line> */
        while (*p == ' ') p++;
        type_text(p);
        OK("typed %d", (int)strlen(p));
    }
    else if (!lstrcmpiA(cmd, "windows")) {   /* windows [all] */
        enum_ctx c;
        a = word(&p);
        c.out = out;
        c.all = a && !lstrcmpiA(a, "all");
        EnumWindows(list_window, (LPARAM)&c);
        OK("windows (hwnd pid x,y,w,h class title)");
    }
    else if (!lstrcmpiA(cmd, "front") || !lstrcmpiA(cmd, "close") || !lstrcmpiA(cmd, "rect")) {
        HWND w;
        while (*p == ' ') p++;
        if (!*p || !(w = window_arg(p))) { ERR("no window %s", p); return; }
        if (!lstrcmpiA(cmd, "front")) OK("front %lu %s", (unsigned long)(ULONG_PTR)w, bring_front(w) ? "yes" : "refused");
        else if (!lstrcmpiA(cmd, "close")) { PostMessageA(w, WM_CLOSE, 0, 0); OK("close %lu", (unsigned long)(ULONG_PTR)w); }
        else {
            RECT r, c;
            POINT o = { 0, 0 };
            GetWindowRect(w, &r);
            GetClientRect(w, &c);
            ClientToScreen(w, &o);
            OK("rect %lu window %ld,%ld,%ld,%ld client %ld,%ld,%ld,%ld", (unsigned long)(ULONG_PTR)w, (long)r.left,
               (long)r.top, (long)(r.right - r.left), (long)(r.bottom - r.top), (long)o.x, (long)o.y, (long)c.right,
               (long)c.bottom);
        }
    }
    else if (!lstrcmpiA(cmd, "run")) {   /* run <command line>  (in the program's own folder) */
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        char dir[MAX_PATH], *slash, *q;
        while (*p == ' ') p++;
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        /* working folder = the exe's folder (games load their data relative to it) */
        lstrcpynA(dir, *p == '"' ? p + 1 : p, sizeof(dir));
        if (*p == '"' && (q = strchr(dir, '"'))) *q = 0;   /* "C:\path\game.exe" args */
        else if ((q = strstr(dir, ".exe ")) || (q = strstr(dir, ".EXE "))) q[4] = 0;
        else if ((q = strchr(dir, ' '))) *q = 0;            /* regedit /e ... (no folder) */
        slash = strrchr(dir, '\\');
        if (slash) *slash = 0;
        if (CreateProcessA(NULL, p, NULL, NULL, FALSE, 0, NULL, slash ? dir : NULL, &si, &pi)) {
            OK("pid %lu", (unsigned long)pi.dwProcessId);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else ERR("CreateProcess failed (%lu)", (unsigned long)GetLastError());
    }
    else if (!lstrcmpiA(cmd, "ps")) {
        if (list_processes(out)) OK("processes (pid exe)");
        else ERR("no process list on this Windows");
    }
    else if (!lstrcmpiA(cmd, "kill")) {
        HANDLE h;
        if (!(a = word(&p))) { ERR("usage: kill pid"); return; }
        h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)strtoul(a, NULL, 10));
        if (h && TerminateProcess(h, 1)) OK("killed %s", a);
        else ERR("could not kill %s (%lu)", a, (unsigned long)GetLastError());
        if (h) CloseHandle(h);
    }
    else if (!lstrcmpiA(cmd, "put")) {   /* put <path>  + the file as data */
        while (*p == ' ') p++;
        if (write_file(p, data, dlen)) OK("wrote %lu", (unsigned long)dlen);
        else ERR("could not write %s (%lu)", p, (unsigned long)GetLastError());
    }
    else if (!lstrcmpiA(cmd, "get")) {
        while (*p == ' ') p++;
        if (read_file(p, out)) OK("file %lu", (unsigned long)out->n);
        else ERR("could not read %s (%lu)", p, (unsigned long)GetLastError());
    }
    else if (!lstrcmpiA(cmd, "ls")) {   /* ls C:\GAME\*.* */
        while (*p == ' ') p++;
        if (list_dir(*p ? p : "*.*", out)) OK("listing (type size name)");
        else ERR("nothing matches %s", p);
    }
    else if (!lstrcmpiA(cmd, "del")) {
        while (*p == ' ') p++;
        if (DeleteFileA(p)) OK("deleted");
        else ERR("could not delete %s (%lu)", p, (unsigned long)GetLastError());
    }
    else if (!lstrcmpiA(cmd, "mkdir")) {
        while (*p == ' ') p++;
        if (CreateDirectoryA(p, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) OK("folder ready");
        else ERR("could not create %s (%lu)", p, (unsigned long)GetLastError());
    }
    else if (!lstrcmpiA(cmd, "setenv")) {   /* setenv NAME=VALUE (NAME= clears): for programs started after */
        char *eq;
        while (*p == ' ') p++;
        if (!(eq = strchr(p, '=')) || eq == p) { ERR("usage: setenv NAME=VALUE"); return; }
        *eq = 0;
        SetEnvironmentVariableA(p, eq[1] ? eq + 1 : NULL);
        OK("%s %s", p, eq[1] ? "set" : "cleared");
    }
    else if (!lstrcmpiA(cmd, "quit")) { g_quit = 1; OK("bye"); }
    else ERR("unknown command %s (ping info shot move click rclick dclick down up drag key type windows front "
             "close rect run ps kill put get ls del mkdir setenv quit)", cmd);
#undef OK
#undef ERR
}

/* ------------------------------------------------------------------ connection loop */
static void serve(SOCKET s)
{
    char hello[400];
    OSVERSIONINFOA v;
    v.dwOSVersionInfoSize = sizeof(v);
    GetVersionExA(&v);
    _snprintf(hello, sizeof(hello) - 1, "hello %s name=%s agent=%s os=%lu.%lu.%lu screen=%dx%d", g_token[0] ? g_token : "-",
              g_name, AGENT_VERSION, (unsigned long)v.dwMajorVersion, (unsigned long)v.dwMinorVersion,
              (unsigned long)(v.dwBuildNumber & 0xffff), GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    hello[sizeof(hello) - 1] = 0;
    if (!send_frame(s, hello, NULL, 0)) return;
    set_status("okagent: connected to %s:%d", g_host, g_port);
    while (!g_quit) {
        unsigned char lb[4];
        DWORD len, hl;
        char *frame, *nl, hdr[600];
        buf out = { 0 };
        if (!recv_all(s, (char *)lb, 4)) break;
        len = lb[0] | (lb[1] << 8) | (lb[2] << 16) | ((DWORD)lb[3] << 24);
        if (len == 0 || len > MAX_FRAME || !(frame = (char *)malloc(len + 1))) break;
        if (!recv_all(s, frame, (int)len)) { free(frame); break; }
        frame[len] = 0;
        nl = memchr(frame, '\n', len);
        hl = nl ? (DWORD)(nl - frame) : len;
        frame[hl] = 0;
        if (hl && frame[hl - 1] == '\r') frame[hl - 1] = 0;
        do_command(frame, nl ? nl + 1 : frame + len, nl ? len - hl - 1 : 0, hdr, sizeof(hdr), &out);
        free(frame);
        if (!send_frame(s, hdr, out.p, out.n)) { free(out.p); break; }
        free(out.p);
    }
}

static DWORD WINAPI net_thread(void *arg)
{
    WSADATA wd;
    (void)arg;
    if (WSAStartup(MAKEWORD(1, 1), &wd)) { set_status("okagent: no Winsock"); return 0; }
    while (!g_quit) {
        struct sockaddr_in sa;
        SOCKET s;
        unsigned long ip = inet_addr(g_host);
        if (ip == INADDR_NONE) {
            struct hostent *he = gethostbyname(g_host);
            if (he && he->h_addr_list[0]) memcpy(&ip, he->h_addr_list[0], 4);
        }
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons((unsigned short)g_port);
        sa.sin_addr.s_addr = ip;
        s = socket(AF_INET, SOCK_STREAM, 0);
        if (s != INVALID_SOCKET && ip != INADDR_NONE && connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
            int one = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
            serve(s);
            set_status("okagent: bridge %s:%d closed the connection - retrying", g_host, g_port);
        } else
            set_status("okagent: waiting for the bridge at %s:%d", g_host, g_port);
        if (s != INVALID_SOCKET) closesocket(s);
        if (!g_quit) Sleep(3000);
    }
    PostMessageA(g_status_wnd, WM_CLOSE, 0, 0);
    return 0;
}

/* ------------------------------------------------------------------ startup */
static LRESULT CALLBACK wnd_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(w, m, wp, lp);
}

/* "-B5731E": the last three bytes of the MAC of the first card Windows lists - clones of one VM keep the
 * computer name but get their own MAC (86Box config), so this tells them apart. iphlpapi is looked
 * up at run time (Windows 98 has it; Windows 95 may not - then no suffix). */
typedef DWORD (WINAPI *adapters_t)(PIP_ADAPTER_INFO, PULONG);

static void mac_suffix(char *out, int n)
{
    HMODULE ip = LoadLibraryA("iphlpapi.dll");
    adapters_t get = ip ? (adapters_t)GetProcAddress(ip, "GetAdaptersInfo") : NULL;
    ULONG size = 0;
    IP_ADAPTER_INFO *info, *a;
    out[0] = 0;
    if (!get || get(NULL, &size) != ERROR_BUFFER_OVERFLOW || !(info = (IP_ADAPTER_INFO *)malloc(size))) return;
    if (get(info, &size) == NO_ERROR)
        for (a = info; a; a = a->Next)
            if (a->AddressLength == 6) {
                _snprintf(out, n - 1, "-%02X%02X%02X", a->Address[3], a->Address[4], a->Address[5]);
                out[n - 1] = 0;
                break;
            }
    free(info);
}

static void load_settings(int argc, char **argv)
{
    char ini[MAX_PATH], *slash, *colon, v[128];
    DWORD n = sizeof(g_name);
    GetModuleFileNameA(NULL, ini, sizeof(ini));
    if ((slash = strrchr(ini, '\\'))) slash[1] = 0;
    lstrcatA(ini, "okagent.ini");
    GetComputerNameA(g_name, &n);
    {
        char sfx[16];
        mac_suffix(sfx, sizeof(sfx));
        if (strlen(g_name) + strlen(sfx) < sizeof(g_name)) lstrcatA(g_name, sfx);   /* (okagent.ini name= overrides) */
    }
    GetPrivateProfileStringA("okagent", "host", g_host, g_host, sizeof(g_host), ini);
    g_port = GetPrivateProfileIntA("okagent", "port", g_port, ini);
    GetPrivateProfileStringA("okagent", "token", g_token, g_token, sizeof(g_token), ini);
    GetPrivateProfileStringA("okagent", "name", g_name, v, sizeof(v), ini);
    lstrcpynA(g_name, v, sizeof(g_name));
    if (argc > 1) {
        lstrcpynA(g_host, argv[1], sizeof(g_host));
        if ((colon = strchr(g_host, ':'))) { *colon = 0; g_port = atoi(colon + 1); }
    }
    if (argc > 2) lstrcpynA(g_token, argv[2], sizeof(g_token));
}

/* okagent.exe /install host:port token - set this machine up with no keyboard or mouse (run from an
 * autorun CD): copy ourselves to C:\OKAGENT, write okagent.ini, start at every boot (the Run key),
 * and start the installed copy now. */
static int install(int argc, char **argv)
{
    char self[MAX_PATH], dst[MAX_PATH] = "C:\\OKAGENT", ini[MAX_PATH], hostport[160], *colon;
    HKEY run;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    if (argc < 4) {
        MessageBoxA(NULL, "usage: okagent.exe /install host:port token", "okagent", MB_OK);
        return 1;
    }
    /* an okagent already running (started at boot) holds C:\OKAGENT\OKAGENT.EXE: end it first */
    {
        HMODULE k = GetModuleHandleA("kernel32.dll");
        snap_t snap = (snap_t)GetProcAddress(k, "CreateToolhelp32Snapshot");
        proc_t first = (proc_t)GetProcAddress(k, "Process32First"), next = (proc_t)GetProcAddress(k, "Process32Next");
        PROCESSENTRY32 pe;
        HANDLE h;
        if (snap && first && next && (h = snap(TH32CS_SNAPPROCESS, 0)) != INVALID_HANDLE_VALUE) {
            pe.dwSize = sizeof(pe);
            for (BOOL more = first(h, &pe); more; more = next(h, &pe)) {
                char *b = strrchr(pe.szExeFile, '\\');
                HANDLE ph;
                if (pe.th32ProcessID == GetCurrentProcessId() || lstrcmpiA(b ? b + 1 : pe.szExeFile, "OKAGENT.EXE")) continue;
                if ((ph = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID))) {
                    TerminateProcess(ph, 0);
                    CloseHandle(ph);
                }
            }
            CloseHandle(h);
            Sleep(500);
        }
    }
    CreateDirectoryA(dst, NULL);
    GetModuleFileNameA(NULL, self, sizeof(self));
    lstrcatA(dst, "\\OKAGENT.EXE");
    if (lstrcmpiA(self, dst) && !CopyFileA(self, dst, FALSE)) {
        MessageBoxA(NULL, "could not copy okagent.exe to C:\\OKAGENT", "okagent", MB_OK);
        return 1;
    }
    SetFileAttributesA(dst, FILE_ATTRIBUTE_NORMAL);   /* (copied from a CD: read-only) */
    lstrcpynA(hostport, argv[2], sizeof(hostport));
    lstrcpyA(ini, "C:\\OKAGENT\\okagent.ini");
    if ((colon = strchr(hostport, ':'))) {
        *colon = 0;
        WritePrivateProfileStringA("okagent", "port", colon + 1, ini);
    }
    WritePrivateProfileStringA("okagent", "host", hostport, ini);
    WritePrivateProfileStringA("okagent", "token", argv[3], ini);
    /* Windows 9x: RunServices starts us before the logon box (so a test can answer it through us);
     * NT has no RunServices - there Run, after logon. Only one of the two, never both. */
    {
        OSVERSIONINFOA v;
        int nt;
        v.dwOSVersionInfoSize = sizeof(v);
        GetVersionExA(&v);
        nt = v.dwPlatformId == VER_PLATFORM_WIN32_NT;
        if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, nt ? "Software\\Microsoft\\Windows\\CurrentVersion\\Run"
                                                   : "Software\\Microsoft\\Windows\\CurrentVersion\\RunServices",
                            0, NULL, 0, KEY_SET_VALUE, NULL, &run, NULL) == ERROR_SUCCESS) {
            RegSetValueExA(run, "okagent", 0, REG_SZ, (const BYTE *)dst, (DWORD)strlen(dst) + 1);
            RegCloseKey(run);
        }
        if (!nt && RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                                 KEY_SET_VALUE, &run) == ERROR_SUCCESS) {
            RegDeleteValueA(run, "okagent");   /* (an earlier install's) */
            RegCloseKey(run);
        }
    }
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (CreateProcessA(dst, NULL, NULL, NULL, FALSE, 0, NULL, "C:\\OKAGENT", &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return 0;
}

/* MinGW's prebuilt _matherr has SSE instructions (check9x.sh): ours replaces it */
int __cdecl _matherr(struct _exception *e) { (void)e; return 0; }

/* our own main (not WinMain): MinGW's prebuilt main->WinMain wrapper has cmov, which a Pentium MMX lacks */
int main(int argc, char **argv)
{
    WNDCLASSA wc;
    MSG msg;
    HANDLE t;
    DWORD tid;

    if (argc > 1 && (!lstrcmpiA(argv[1], "/install") || !lstrcmpiA(argv[1], "--install")))
        return install(argc, argv);
    /* one agent per machine (RunServices, Run and a manual start must not fight over the name) */
    CreateMutexA(NULL, FALSE, "okagent-single-instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    load_settings(argc, argv);
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "okagent";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    /* a small window that only says what the agent is doing - minimized, out of the screenshots' way */
    g_status_wnd = CreateWindowA("okagent", g_status, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                 0, 0, 360, 60, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(g_status_wnd, SW_SHOWMINNOACTIVE);
    t = CreateThread(NULL, 0, net_thread, NULL, 0, &tid);   /* (Windows 9x fails it with a NULL thread id) */
    if (t) CloseHandle(t);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    g_quit = 1;
    return 0;
}
