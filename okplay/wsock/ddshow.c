/* okplay wsock - okplay's windows over a full-screen DirectDraw game.
 *
 * A full-screen DirectDraw game owns the screen, and a window of ours (the room list, a password) opening over
 * it has two ways to go unseen (Need for Speed III's "Connecting..." with okplay's room list open and invisible,
 * Windows 98, 2026-11-09):
 *  - the game shows another screen page than Windows draws on;
 *  - the game window is told it is no longer in front, and DirectDraw gives up the full-screen mode (NFS3: its
 *    window belongs to another thread than the one calling Winsock; the screen then kept the last picture).
 * The second is answered at the end of this file (the game window's messages).
 * For the first, DirectDraw's documented answer is IDirectDraw::FlipToGDISurface, called by the program that owns the
 * DirectDraw object - so the game's objects are needed. DirectDrawCreate/DirectDrawCreateEx (from the game's
 * imports, its GetProcAddress, and the DLLs it loads) hand the game a thin stand-in for each object: every call
 * goes straight to the real object (a small thunk swaps `this` and jumps to the real method), only
 * QueryInterface (a DirectDraw interface asked for gets its own stand-in) and Release (a released object is
 * forgotten) are ours. Nothing of Windows' DirectDraw is looked into or changed; Windows' own DLLs are never
 * patched (on Windows 9x they are shared between programs). */
#include "okp.h"
#include <string.h>
#include <ddraw.h>
#include "ddshow.h"

#define NSLOT 32   /* IDirectDraw7 has 30 methods; the older interfaces fewer, in the same order */

typedef struct dd_px {
    void **vtbl;
    void *real;            /* the real interface */
    int v4;                /* IDirectDraw4/7: DDSURFACEDESC2 */
    volatile LONG live;
    struct dd_px *next;
} dd_px;

static CRITICAL_SECTION dd_lock;
static dd_px *dd_list;
static void *dd_vtbl[NSLOT];
static HRESULT (WINAPI *real_ddc)(GUID *, void **, IUnknown *);
static HRESULT (WINAPI *real_ddcex)(GUID *, void **, REFIID, IUnknown *);
static HMODULE (WINAPI *real_lla)(LPCSTR);
static HMODULE (WINAPI *real_llexa)(LPCSTR, HANDLE, DWORD);
static FARPROC (WINAPI *real_gpa)(HMODULE, LPCSTR);
static char win_dir[MAX_PATH];
static int dd_ready;

static const GUID dd_iids[4] = {   /* IDirectDraw, 2, 4, 7 (ddraw.h) */
    { 0x6C14DB80, 0xA733, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } },
    { 0xB3A6F3E0, 0x2B43, 0x11CF, { 0xA2, 0xDE, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56 } },
    { 0x9C59509A, 0x39BD, 0x11D1, { 0x8C, 0x4A, 0x00, 0xC0, 0x4F, 0xD9, 0x30, 0xC5 } },
    { 0x15E65EC0, 0x3B9C, 0x11D2, { 0xB9, 0x2F, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B } },
};

static int dd_iid(REFIID r)   /* 1..4 = IDirectDraw, 2, 4, 7; 0 = another interface */
{
    for (int i = 0; i < 4; i++) if (!memcmp(r, &dd_iids[i], sizeof(GUID))) return i + 1;
    return 0;
}

static void *dd_wrap(void *real, int which)   /* the stand-in for `real` (one per real interface) */
{
    if (!real) return NULL;
    EnterCriticalSection(&dd_lock);
    dd_px *p;
    for (p = dd_list; p; p = p->next) if (p->real == real && p->live) break;
    if (!p) {
        p = (dd_px *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*p));
        if (p) {
            p->vtbl = dd_vtbl;
            p->real = real;
            p->v4 = which >= 3;
            p->live = 1;
            p->next = dd_list;
            dd_list = p;
            okp_log(3, "ddshow: the game has a DirectDraw object (%p, IDirectDraw%s)", real,
                    which == 1 ? "" : which == 2 ? "2" : which == 3 ? "4" : "7");
        }
    }
    LeaveCriticalSection(&dd_lock);
    return p ? (void *)p : real;
}

static HRESULT WINAPI px_qi(dd_px *p, REFIID r, void **out)
{
    void **rv = *(void ***)p->real;
    HRESULT hr = ((HRESULT (WINAPI *)(void *, REFIID, void **))rv[0])(p->real, r, out);
    if (SUCCEEDED(hr) && out && *out && dd_iid(r)) *out = dd_wrap(*out, dd_iid(r));
    return hr;
}

static ULONG WINAPI px_addref(dd_px *p)
{
    void **rv = *(void ***)p->real;
    return ((ULONG (WINAPI *)(void *))rv[1])(p->real);
}

static ULONG WINAPI px_release(dd_px *p)
{
    void **rv = *(void ***)p->real;
    ULONG n = ((ULONG (WINAPI *)(void *))rv[2])(p->real);
    if (!n) InterlockedExchange(&p->live, 0);   /* (the stand-in itself is kept: a few bytes, never a dangling pointer) */
    return n;
}

static HRESULT WINAPI hk_ddc(GUID *g, void **out, IUnknown *outer)
{
    if (!real_ddc) return E_FAIL;
    HRESULT hr = real_ddc(g, out, outer);
    if (SUCCEEDED(hr) && out && *out && !outer) *out = dd_wrap(*out, 1);
    return hr;
}

static HRESULT WINAPI hk_ddcex(GUID *g, void **out, REFIID r, IUnknown *outer)
{
    if (!real_ddcex) return E_FAIL;
    HRESULT hr = real_ddcex(g, out, r, outer);
    if (SUCCEEDED(hr) && out && *out && !outer) *out = dd_wrap(*out, dd_iid(r) ? dd_iid(r) : 4);
    return hr;
}

static void patch_module(HMODULE m);

static HMODULE WINAPI hk_lla(LPCSTR name)
{
    HMODULE h = real_lla(name);
    patch_module(h);
    return h;
}

static HMODULE WINAPI hk_llexa(LPCSTR name, HANDLE f, DWORD flags)
{
    HMODULE h = real_llexa(name, f, flags);
    if (!(flags & LOAD_LIBRARY_AS_DATAFILE)) patch_module(h);
    return h;
}

static FARPROC WINAPI hk_gpa(HMODULE m, LPCSTR name)
{
    FARPROC f = real_gpa(m, name);
    if (!f || !HIWORD((DWORD_PTR)name)) return f;
    if (!strcmp(name, "DirectDrawCreate")) {
        if (!real_ddc) real_ddc = (void *)f;
        return (FARPROC)hk_ddc;
    }
    if (!strcmp(name, "DirectDrawCreateEx")) {
        if (!real_ddcex) real_ddcex = (void *)f;
        return (FARPROC)hk_ddcex;
    }
    return f;
}

/* m's import of dll!fn now calls `to`; the slot's old value goes to *orig if that is still empty */
static int hook_import(HMODULE m, const char *dll, const char *fn, void *to, void **orig)
{
    uint8_t *base = (uint8_t *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd->VirtualAddress) return 0;
    int n = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dd->VirtualAddress); d->Name; d++) {
        if (_stricmp((char *)(base + d->Name), dll) || !d->OriginalFirstThunk) continue;   /* (no name table: left alone) */
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + d->OriginalFirstThunk);
        void **slot = (void **)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, slot++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((char *)ibn->Name, fn) || *slot == to) continue;
            DWORD old;
            if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) continue;
            if (orig && !*orig) *orig = *slot;
            *slot = to;
            VirtualProtect(slot, sizeof(*slot), old, &old);
            n++;
        }
    }
    return n;
}

static void patch_module(HMODULE m)
{
    char path[MAX_PATH];
    if (!m || !dd_ready || !GetModuleFileNameA(m, path, sizeof(path))) return;
    size_t w = strlen(win_dir);
    if (w && !_strnicmp(path, win_dir, w)) return;   /* Windows' own DLLs: never */
    int n = hook_import(m, "ddraw.dll", "DirectDrawCreate", (void *)hk_ddc, (void **)&real_ddc) +
            hook_import(m, "ddraw.dll", "DirectDrawCreateEx", (void *)hk_ddcex, (void **)&real_ddcex);
    hook_import(m, "kernel32.dll", "LoadLibraryA", (void *)hk_lla, NULL);
    hook_import(m, "kernel32.dll", "LoadLibraryExA", (void *)hk_llexa, NULL);
    hook_import(m, "kernel32.dll", "GetProcAddress", (void *)hk_gpa, NULL);
    if (n) okp_log(3, "ddshow: %s imports DirectDraw - watched", path);
}

void dds_init(void)
{
    HMODULE k = GetModuleHandleA("kernel32.dll");
    real_lla = (void *)GetProcAddress(k, "LoadLibraryA");
    real_llexa = (void *)GetProcAddress(k, "LoadLibraryExA");
    real_gpa = (void *)GetProcAddress(k, "GetProcAddress");
    if (!real_lla || !real_llexa || !real_gpa) return;
    /* the thunks: mov eax,[esp+4]; mov eax,[eax+4] (the real interface); mov [esp+4],eax; mov eax,[eax];
     * jmp [eax+slot*4] - the real method, with the real `this`, the caller's return address and arguments */
    uint8_t *code = (uint8_t *)VirtualAlloc(NULL, NSLOT * 20, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!code) return;
    for (int i = 0; i < NSLOT; i++) {
        static const uint8_t t[13] = { 0x8B, 0x44, 0x24, 0x04, 0x8B, 0x40, 0x04, 0x89, 0x44, 0x24, 0x04, 0x8B, 0x00 };
        uint8_t *c = code + i * 20;
        DWORD disp = (DWORD)(i * 4);
        memcpy(c, t, 13);
        c[13] = 0xFF; c[14] = 0xA0;
        memcpy(c + 15, &disp, 4);
        c[19] = 0xCC;
        dd_vtbl[i] = c;
    }
    dd_vtbl[0] = (void *)px_qi;
    dd_vtbl[1] = (void *)px_addref;
    dd_vtbl[2] = (void *)px_release;
    FlushInstructionCache(GetCurrentProcess(), code, NSLOT * 20);
    InitializeCriticalSection(&dd_lock);
    if (!GetWindowsDirectoryA(win_dir, sizeof(win_dir))) win_dir[0] = 0;
    dd_ready = 1;
    patch_module(GetModuleHandleA(NULL));
}


/* ---- the game's window must not learn that a box of ours took its place ----
 * okplay's boxes open on the thread that called into Winsock; a game whose window belongs to another thread
 * (Need for Speed III) is then told it was switched away from, and DirectDraw answers that by giving up the
 * full-screen mode: the screen keeps the last picture and the box is never seen.  While a box is open the
 * "you are no longer in front" messages stop at okplay. */
static HWND dd_game_wnd;            /* the window guarded last: in front again when the box closes */
static volatile LONG dd_guard;
static volatile DWORD dd_tail;   /* when the last box closed (tick) */
/* A window we take the place of keeps its own procedure in this property: each window is taken over once, and our
 * procedure is never stored as "the old one" (box 1 over window A, box 2 over window B, box 3 over A again used to do
 * that with one shared variable - the procedure then called itself until the stack ran out). */
static const char DD_PROP[] = "okwsock.ddproc";
#define DD_MAXWND 8
static HWND dd_wnds[DD_MAXWND];     /* windows taken over, given back when this DLL is unloaded */
static int dd_nwnd;

static void dd_forget(HWND w)
{
    EnterCriticalSection(&dd_lock);
    for (int i = 0; i < dd_nwnd; i++) if (dd_wnds[i] == w) { dd_wnds[i] = dd_wnds[--dd_nwnd]; break; }
    LeaveCriticalSection(&dd_lock);
}

/* The mouse pointer while a box is open.  The game hides it on the thread of its window (ShowCursor counts
 * per thread, WM_SETCURSOR answered with no pointer); a box on another thread then still has no pointer
 * wherever Windows asks the game window (seen with DDrawCompat, which draws the pointer itself).  So the
 * game window's thread is asked - by a message, as only that thread can - to show the pointer while a box
 * is open and to put its count back afterwards; WM_SETCURSOR gets the arrow meanwhile.  Shared: any game. */
static UINT dd_cur_msg;
static int dd_cur_shows;     /* ShowCursor(TRUE) calls to undo; the game window's thread only */

static void dds_cursor_on_game_thread(int on)
{
    if (on) {
        int n = 0;
        if (dd_cur_shows) return;
        for (int i = 0; i < 64; i++) { n = ShowCursor(TRUE); dd_cur_shows++; if (n >= 0) break; }
        if (dd_cur_shows == 1 && n > 0) { ShowCursor(FALSE); dd_cur_shows = 0; }   /* was showing already */
        SetCursor(LoadCursorA(NULL, (LPCSTR)IDC_ARROW));
        okp_log(2, "ddshow: mouse pointer for the box - the game's thread had it hidden %d deep", dd_cur_shows);
    } else {
        while (dd_cur_shows > 0) { ShowCursor(FALSE); dd_cur_shows--; }
    }
}

static void dds_cursor(HWND w, int on)
{
    DWORD_PTR r = 0;
    CURSORINFO ci;
    RECT clip = { 0, 0, 0, 0 };
    memset(&ci, 0, sizeof(ci)); ci.cbSize = sizeof(ci);
    if (!dd_cur_msg) dd_cur_msg = RegisterWindowMessageA("okplay.ddshow.cursor");
    if (!dd_cur_msg || !w || !IsWindow(w)) return;
    if (on) {
        GetCursorInfo(&ci);
        GetClipCursor(&clip);
        okp_log(2, "ddshow: mouse pointer before the box: %s, at %ld,%ld, kept inside %ld,%ld-%ld,%ld",
                (ci.flags & CURSOR_SHOWING) ? "showing" : "hidden", ci.ptScreenPos.x, ci.ptScreenPos.y,
                clip.left, clip.top, clip.right, clip.bottom);
    }
    if (GetWindowThreadProcessId(w, NULL) == GetCurrentThreadId()) SendMessageA(w, dd_cur_msg, on, 0);
    else if (!SendMessageTimeoutA(w, dd_cur_msg, on, 0, SMTO_ABORTIFHUNG, 300, &r))
        PostMessageA(w, dd_cur_msg, on, 0);   /* (busy just now: when it next reads its messages) */
}

static LRESULT CALLBACK dds_game_wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    WNDPROC old = (WNDPROC)GetPropA(w, DD_PROP);
    if (!old) return DefWindowProcA(w, m, wp, lp);   /* (never: the property is set before this is put in place) */
    if (dd_cur_msg && m == dd_cur_msg) { dds_cursor_on_game_thread(wp != 0); return 0; }
    if (m == WM_NCDESTROY) dd_cur_shows = 0;
    if (dd_guard && m == WM_SETCURSOR) { SetCursor(LoadCursorA(NULL, (LPCSTR)IDC_ARROW)); return TRUE; }
    if (dd_guard) {
        int off = (m == WM_ACTIVATEAPP && !wp) || (m == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) || m == WM_KILLFOCUS ||
                  (m == WM_NCACTIVATE && !wp);
        if (off || m == WM_ACTIVATEAPP || m == WM_ACTIVATE || m == WM_SETFOCUS || m == WM_NCACTIVATE || m == WM_SIZE ||
            m == WM_SYSCOMMAND || m == WM_DISPLAYCHANGE || m == WM_SHOWWINDOW)
            okp_log(3, "ddshow: game window message 0x%04x wp %lx lp %lx%s", m, (unsigned long)wp, (unsigned long)lp, off ? " - kept from the game" : "");
        if (off) return m == WM_NCACTIVATE ? TRUE : 0;
    }
    else if (dd_tail && GetTickCount() - dd_tail < 4000) {   /* just after a box: what reaches the game (log only) */
        if (m == WM_ACTIVATEAPP || m == WM_ACTIVATE || m == WM_SETFOCUS || m == WM_KILLFOCUS || m == WM_NCACTIVATE || m == WM_SIZE ||
            m == WM_SYSCOMMAND || m == WM_DISPLAYCHANGE || m == WM_SHOWWINDOW || m == WM_WINDOWPOSCHANGED)
            okp_log(3, "ddshow: after the box, game window message 0x%04x wp %lx lp %lx%s", m, (unsigned long)wp, (unsigned long)lp,
                    IsIconic(w) ? " (minimised)" : "");
    }
    if (m == WM_NCDESTROY) {   /* the window goes: its own procedure back, and forgotten here */
        RemovePropA(w, DD_PROP);
        if ((WNDPROC)GetWindowLongA(w, GWL_WNDPROC) == dds_game_wndproc) SetWindowLongA(w, GWL_WNDPROC, (LONG)old);
        dd_forget(w);
        if (w == dd_game_wnd) dd_game_wnd = NULL;
    }
    return CallWindowProcA(old, w, m, wp, lp);
}

static BOOL CALLBACK dds_game_win(HWND w, LPARAM lp)   /* a visible top-level window of this program, not a dialog */
{
    DWORD pid = 0;
    char cls[64] = "";
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(w)) return TRUE;
    GetClassNameA(w, cls, sizeof(cls));
    if (!strcmp(cls, "#32770")) return TRUE;
    *(HWND *)lp = w;
    return FALSE;
}

static void dds_guard_begin(void)
{
    HWND w = NULL;
    EnumWindows(dds_game_win, (LPARAM)&w);
    if (!w) return;
    if (!GetPropA(w, DD_PROP)) {   /* not taken over yet */
        WNDPROC cur = (WNDPROC)GetWindowLongA(w, GWL_WNDPROC);
        int room = 0;
        if (cur && cur != dds_game_wndproc) {
            EnterCriticalSection(&dd_lock);
            room = dd_nwnd < DD_MAXWND;
            if (room) dd_wnds[dd_nwnd++] = w;
            LeaveCriticalSection(&dd_lock);
        }
        if (room) {
            SetPropA(w, DD_PROP, (HANDLE)cur);   /* the old procedure first, then ours in its place */
            WNDPROC was = (WNDPROC)SetWindowLongA(w, GWL_WNDPROC, (LONG)dds_game_wndproc);
            if (was && was != cur && was != dds_game_wndproc) SetPropA(w, DD_PROP, (HANDLE)was);   /* (changed meanwhile) */
            okp_log(2, "ddshow: watching the game window %p (thread %lx; the box opens on thread %lx)", (void *)w,
                    (unsigned long)GetWindowThreadProcessId(w, NULL), (unsigned long)GetCurrentThreadId());
        }
    } else if ((WNDPROC)GetWindowLongA(w, GWL_WNDPROC) != dds_game_wndproc) {
        /* someone put a window procedure in front of ours since: ours still runs behind it, left as it is */
        okp_log(3, "ddshow: the game window's procedure changed since okplay took its place in front");
    }
    dd_game_wnd = w;
    InterlockedExchange(&dd_guard, 1);
    if (GetPropA(w, DD_PROP)) dds_cursor(w, 1);
}

void dds_detach(void)   /* this DLL is unloaded while the process goes on: every window gets its own procedure back */
{
    if (!dd_ready) return;
    EnterCriticalSection(&dd_lock);
    for (int i = 0; i < dd_nwnd; i++) {
        HWND w = dd_wnds[i];
        WNDPROC old = (WNDPROC)GetPropA(w, DD_PROP);
        if (!IsWindow(w) || !old) continue;
        if ((WNDPROC)GetWindowLongA(w, GWL_WNDPROC) == dds_game_wndproc) {
            SetWindowLongA(w, GWL_WNDPROC, (LONG)old);
            RemovePropA(w, DD_PROP);
        }   /* (else another procedure sits in front of ours and calls it: nothing can be undone safely) */
    }
    dd_nwnd = 0;
    LeaveCriticalSection(&dd_lock);
}

void dds_guard_end(void)   /* the box is gone: the game window in front again, then its messages as usual */
{
    if (!dd_guard) return;
    if (dd_game_wnd && IsWindow(dd_game_wnd) && GetPropA(dd_game_wnd, DD_PROP)) dds_cursor(dd_game_wnd, 0);
    if (dd_game_wnd && IsWindow(dd_game_wnd)) SetForegroundWindow(dd_game_wnd);
    dd_tail = GetTickCount() | 1;
    InterlockedExchange(&dd_guard, 0);
}

int dds_show_gdi(void)   /* the thread that will show the window (the game's own, for okplay's boxes) */
{
    if (!dd_ready) return 0;
    int n = 0;
    EnterCriticalSection(&dd_lock);
    for (dd_px *p = dd_list; p; p = p->next) {
        if (!p->live) continue;
        void **rv = *(void ***)p->real;
        HRESULT hr = ((HRESULT (WINAPI *)(void *))rv[10])(p->real);   /* IDirectDraw*::FlipToGDISurface */
        okp_log(3, "ddshow: FlipToGDISurface on %p -> 0x%08lx", p->real, (unsigned long)hr);
        n++;
    }
    LeaveCriticalSection(&dd_lock);
    if (n) dds_guard_begin();
    return n;
}
