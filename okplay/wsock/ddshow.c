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
#include <ctype.h>
#include <stdlib.h>
#include <ddraw.h>
#include "ddshow.h"
#include "okbox.h"
#include "ovl.h"

#define NSLOT 32   /* IDirectDraw7 has 30 methods; the older interfaces fewer, in the same order */
#define NSURF 52   /* IDirectDrawSurface7 has 49 methods; the older surface interfaces fewer, in the same order */
#define NTHUNK NSURF

typedef struct dd_px {
    void **vtbl;
    void *real;            /* the real interface */
    int v4;                /* objects: 0 = IDirectDraw/2, 1 = IDirectDraw4, 2 = IDirectDraw7 (non-zero: DDSURFACEDESC2); surfaces: 1..5 = IDirectDrawSurface, 2, 3, 4, 7 */
    volatile LONG live;
    struct dd_px *next;
} dd_px;

static CRITICAL_SECTION dd_lock;
static dd_px *dd_list;
static void *dd_vtbl[NSLOT];
static void *dd_thunk[NTHUNK];   /* the plain thunks, by slot (the same code serves objects and surfaces) */
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
            p->v4 = which >= 3 ? which - 2 : 0;
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
static int hook_import(HMODULE m, const char *dll, const char *fn, void *to, void **orig);
int dds_hook_import(HMODULE m, const char *dll, const char *fn, void *to, void **orig) { return hook_import(m, dll, fn, to, orig); }

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

/* ---- the game lets go of the mouse while a box is open (Windows NT family) ----
 * A game that still runs behind a box keeps putting the pointer back in the middle of its screen and keeps it
 * inside its window (Need for Speed III, Windows 11, 2026-11-11: the pointer "jumped through" the room list).
 * While a box is open those two calls from the game's threads do nothing; the box's own thread is not touched.
 * Shared: any game whose imports are watched here. */
static volatile LONG dd_guard;
static volatile LONG dd_wrap_now;   /* the guard is on over a game a DirectDraw wrapper shows (dds_wrapper) */
/* diagnosis of a box that stops (0.3.2): the step the box's own drawing is at, and when its last round finished;
 * dds_watch logs them (and, with okplay.ini wsock_box_stall_log=1, where the box's thread is) if a round takes > 1.5 s */
static volatile LONG dd_step;
static volatile DWORD dd_beat;
/* The game's picture as it was when the box opened, over a wrapper's window: painted back wherever the box uncovers
 * the game window (the game itself does not redraw while it waits on us - dragging the box left a "hall of mirrors"
 * with DDrawCompat, 2026-11-16).  Taken from the screen, the size of the game window's monitor (a wrapper may tell
 * this program the game's own 640x480 instead of the real window size); painted at the window's top left. */
static HBITMAP dd_snap;
static int dd_snap_w, dd_snap_h;
static CRITICAL_SECTION dd_snap_cs;
static int dd_drag = 1;             /* okplay.ini wsock_box_drag: 0 = the box cannot be dragged over a wrapper's window */
void dds_set_drag(int on) { dd_drag = on; }
static volatile DWORD dd_box_tid;   /* the thread of the open box */
static int dd_keep;                 /* 1 = the game is not told it lost the front (every Windows; dds_set_keep) */
static BOOL (WINAPI *real_scp)(int, int);
static BOOL (WINAPI *real_clip)(const RECT *);

static BOOL WINAPI hk_scp(int x, int y)
{
    if (dd_guard && GetCurrentThreadId() != dd_box_tid) return TRUE;
    return real_scp ? real_scp(x, y) : SetCursorPos(x, y);
}

static BOOL WINAPI hk_clip(const RECT *r)
{
    if (dd_guard && r && GetCurrentThreadId() != dd_box_tid) return TRUE;
    return real_clip ? real_clip(r) : ClipCursor(r);
}

void dds_set_keep(int keep) { dd_keep = keep; }
static int dd_stall_log;            /* dds_set_stall_log */
void dds_set_stall_log(int on) { dd_stall_log = on; }

/* ---- the game's page flips are held while a box is open (opt-in: okplay.ini wsock_box_flip=hold) ----
 * dds_show_gdi puts Windows' screen page in front once; a game whose drawing thread goes on calling Flip while
 * the box is open (its network calls on another thread) puts its own page back at once and the box is never seen
 * (boxgame.c on Windows 98, 2026-11-11).  With the hold on, the stand-in's CreateSurface is ours: a primary
 * surface the game makes is handed over as a stand-in too (every call thunks straight to the real surface, as for
 * the DirectDraw object), and its Flip does nothing while a box is open - it waits 15 ms (a game that spins on
 * Flip must not eat the processor) and answers DD_OK.  Off (the default), CreateSurface is the plain thunk and
 * nothing here runs: games that work today run exactly as before.
 * Known limit - why this is opt-in per game: the game holds our stand-in pointer for its primary.  Calls made ON
 * the stand-in are fine, and where one of them takes a surface argument that may be one of our stand-ins (Blt
 * and BltFast from itself, Flip's target, Add/DeleteAttachedSurface) it is given the real surface.  NOT covered:
 * the stand-in handed as an argument to a call on any OTHER object - e.g. a back buffer's Blt/BltFast FROM the
 * primary, an overlay's UpdateOverlay/UpdateOverlayZOrder with the primary as destination,
 * IDirectDraw::DuplicateSurface of the primary, a Direct3D device made on the primary (IDirect3D*::CreateDevice),
 * a primary set as render target, or the primary inside a DDBLTBATCH.  Real DirectDraw would then read our
 * stand-in as its own surface: wrong results or a fault.  Also not covered: a primary reached another way than
 * CreateSurface/QueryInterface on a stand-in (GetAttachedSurface/EnumAttachedSurfaces asking for the front
 * buffer, GetSurfaceFromDC) is the real one, and its Flip is not held; QueryInterface for IUnknown (or anything
 * not a surface interface) answers the real pointer, so an identity compare with the stand-in fails.
 * [universal; found with: boxgame.c (the stand-in game)] */
static int dd_hold;                 /* dds_set_flip_hold */
static int dd_ovl;                  /* dds_set_overlay: the primary's stand-in also draws okplay's overlay into each frame */
static volatile LONG dd_held;       /* flips held during the box now open */
static volatile LONG dd_stop;       /* flips stop now: set before the switch to Windows' page, so none slips in after it */
static CRITICAL_SECTION ds_flip_cs; /* a flip under way ends before that switch */
static CRITICAL_SECTION ds_lock_cs; /* (dd_lock serves too; kept apart so the surface list never waits on FlipToGDISurface) */
static dd_px *ds_list;
static void *ds_vtbl[NSURF];

static const GUID ds_iids[5] = {   /* IDirectDrawSurface, 2, 3, 4, 7 (ddraw.h) */
    { 0x6C14DB81, 0xA733, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } },
    { 0x57805885, 0x6EEC, 0x11CF, { 0x94, 0x41, 0xA8, 0x23, 0x03, 0xC1, 0x0E, 0x27 } },
    { 0xDA044E00, 0x69B2, 0x11D0, { 0xA1, 0xD5, 0x00, 0xAA, 0x00, 0xB8, 0xDF, 0xBB } },
    { 0x0B2B8630, 0xAD35, 0x11D0, { 0x8E, 0xA6, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B } },
    { 0x06675A80, 0x3B9B, 0x11D2, { 0xB9, 0x2F, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B } },
};

static void ds_ovl_on(void *surf, int which, const char *how);   /* (below: the overlay drawn into a frame) */
static int ds_whole_frame(void *surf, int which);                 /* (below) */

static int ds_iid(REFIID r)   /* 1..5 = IDirectDrawSurface, 2, 3, 4, 7; 0 = another interface */
{
    for (int i = 0; i < 5; i++) if (!memcmp(r, &ds_iids[i], sizeof(GUID))) return i + 1;
    return 0;
}

static void *ds_unwrap(void *s)   /* a surface argument: our stand-in -> the real surface; anything else as it is */
{
    return s && *(void ***)s == ds_vtbl ? ((dd_px *)s)->real : s;
}

static void *ds_wrap(void *real, int which)   /* the stand-in for a real primary surface (one per real interface) */
{
    if (!real) return NULL;
    EnterCriticalSection(&ds_lock_cs);
    dd_px *p;
    for (p = ds_list; p; p = p->next) if (p->real == real && p->live) break;
    if (!p) {
        p = (dd_px *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*p));
        if (p) {
            p->vtbl = ds_vtbl;
            p->real = real;
            p->v4 = which;
            p->live = 1;
            p->next = ds_list;
            ds_list = p;
            okp_log(3, "ddshow: the game has a primary surface (%p, IDirectDrawSurface%s) - its flips are watched (%s%s%s)",
                    real, which == 1 ? "" : which == 2 ? "2" : which == 3 ? "3" : which == 4 ? "4" : "7",
                    dd_hold ? "held while a box is open" : "", dd_hold && dd_ovl ? ", " : dd_ovl ? "" : "", dd_ovl ? "okplay's overlay drawn in" : "");
        }
    }
    LeaveCriticalSection(&ds_lock_cs);
    return p ? (void *)p : real;
}

static HRESULT WINAPI ps_qi(dd_px *p, REFIID r, void **out)
{
    void **rv = *(void ***)p->real;
    HRESULT hr = ((HRESULT (WINAPI *)(void *, REFIID, void **))rv[0])(p->real, r, out);
    if (SUCCEEDED(hr) && out && *out && ds_iid(r)) *out = ds_wrap(*out, ds_iid(r));
    return hr;
}

static HRESULT WINAPI ps_blt(dd_px *p, RECT *dst, void *src, RECT *sr, DWORD flags, void *fx)   /* slot 5 */
{
    void **rv = *(void ***)p->real;
    /* a game that copies its frame onto the primary (a windowed game): the overlay goes on the frame first, the frame
     * being the copy's source (the primary is the whole screen there, and gives no device context on Windows 10) */
    if (src && dd_ovl && !(flags & DDBLT_COLORFILL) && ovl_wants_frames() && ds_whole_frame(ds_unwrap(src), p->v4))
        ds_ovl_on(ds_unwrap(src), p->v4, "the game's frame, before its copy to the screen");
    return ((HRESULT (WINAPI *)(void *, RECT *, void *, RECT *, DWORD, void *))rv[5])(p->real, dst, ds_unwrap(src), sr, flags, fx);
}

static HRESULT WINAPI ps_bltfast(dd_px *p, DWORD x, DWORD y, void *src, RECT *sr, DWORD t)   /* slot 7 */
{
    void **rv = *(void ***)p->real;
    return ((HRESULT (WINAPI *)(void *, DWORD, DWORD, void *, RECT *, DWORD))rv[7])(p->real, x, y, ds_unwrap(src), sr, t);
}

static HRESULT WINAPI ps_addatt(dd_px *p, void *s)   /* slot 3: AddAttachedSurface */
{
    void **rv = *(void ***)p->real;
    return ((HRESULT (WINAPI *)(void *, void *))rv[3])(p->real, ds_unwrap(s));
}

static HRESULT WINAPI ps_delatt(dd_px *p, DWORD f, void *s)   /* slot 8: DeleteAttachedSurface */
{
    void **rv = *(void ***)p->real;
    return ((HRESULT (WINAPI *)(void *, DWORD, void *))rv[8])(p->real, f, ds_unwrap(s));
}

/* ---- okplay's overlay drawn into the game's frames (ovl.c; docs/OVERLAY.md) ----
 * Just before the game shows a frame (its primary's Flip), the frame about to be shown - the back buffer - gets the
 * overlay drawn on it through a device context (IDirectDrawSurface*::GetDC/ReleaseDC; slots are the same in every
 * surface interface: GetAttachedSurface 12, GetDC 17, GetSurfaceDesc 22, ReleaseDC 26).  A game that copies its
 * frame onto the primary instead (Blt) gets it drawn on the primary just after the copy.  On the game's own drawing
 * thread, inside its own frame: nothing of ours competes with the game or a wrapper for the screen. */
static void ds_ovl_on(void *surf, int which, const char *how)
{
    void **sv = *(void ***)surf;
    HDC dc = NULL;
    int w = 0, h = 0, bpp = 0;
    if (which >= 4) {
        DDSURFACEDESC2 d; memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d);
        if (SUCCEEDED(((HRESULT (WINAPI *)(void *, void *))sv[22])(surf, &d))) { w = d.dwWidth; h = d.dwHeight; bpp = d.ddpfPixelFormat.dwRGBBitCount; }
    } else {
        DDSURFACEDESC d; memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d);
        if (SUCCEEDED(((HRESULT (WINAPI *)(void *, void *))sv[22])(surf, &d))) { w = d.dwWidth; h = d.dwHeight; bpp = d.ddpfPixelFormat.dwRGBBitCount; }
    }
    if (w <= 0 || h <= 0) return;
    HRESULT hr = ((HRESULT (WINAPI *)(void *, HDC *))sv[17])(surf, &dc);
    if (FAILED(hr) || !dc) {
        static int told;
        if (!told++) okp_log(1, "overlay: the game's frame (%s) gives no device context (0x%08lx) - nothing drawn", how, (unsigned long)hr);
        return;
    }
    ovl_draw(dc, w, h, bpp);
    ((HRESULT (WINAPI *)(void *, HDC))sv[26])(surf, dc);
}

static int ds_whole_frame(void *surf, int which)   /* a copy's source that is a whole frame (320x200 and up), not a piece */
{
    void **sv = *(void ***)surf;
    DWORD w = 0, h = 0;
    if (which >= 4) { DDSURFACEDESC2 d; memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d); if (SUCCEEDED(((HRESULT (WINAPI *)(void *, void *))sv[22])(surf, &d))) { w = d.dwWidth; h = d.dwHeight; } }
    else { DDSURFACEDESC d; memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d); if (SUCCEEDED(((HRESULT (WINAPI *)(void *, void *))sv[22])(surf, &d))) { w = d.dwWidth; h = d.dwHeight; } }
    return w >= 320 && h >= 200;
}

static void ds_ovl_frame(dd_px *p)   /* before the flip: on the back buffer */
{
    void **rv = *(void ***)p->real, *back = NULL;
    DDSCAPS2 caps;   /* (DDSCAPS is its first member: right for every surface interface) */
    memset(&caps, 0, sizeof(caps));
    caps.dwCaps = DDSCAPS_BACKBUFFER;
    if (FAILED(((HRESULT (WINAPI *)(void *, void *, void **))rv[12])(p->real, &caps, &back)) || !back) return;
    ds_ovl_on(back, p->v4, "back buffer");
    ((ULONG (WINAPI *)(void *))(*(void ***)back)[2])(back);   /* (GetAttachedSurface added a reference) */
}

static HRESULT WINAPI ps_flip(dd_px *p, void *target, DWORD flags)   /* slot 11 in every IDirectDrawSurface */
{
    void **rv = *(void ***)p->real;
    HRESULT hr;
    if (dd_ovl && ovl_wants_frames()) ds_ovl_frame(p);
    if (!dd_hold) return ((HRESULT (WINAPI *)(void *, void *, DWORD))rv[11])(p->real, ds_unwrap(target), flags);
    EnterCriticalSection(&ds_flip_cs);
    if (dd_stop) {
        LeaveCriticalSection(&ds_flip_cs);
        InterlockedIncrement(&dd_held);
        Sleep(15);
        return DD_OK;
    }
    hr = ((HRESULT (WINAPI *)(void *, void *, DWORD))rv[11])(p->real, ds_unwrap(target), flags);
    LeaveCriticalSection(&ds_flip_cs);
    return hr;
}

/* IDirectDraw*::CreateSurface (slot 6), in the stand-in's table only while the hold is on */
static HRESULT WINAPI px_cs(dd_px *p, void *desc, void **out, IUnknown *outer)
{
    void **rv = *(void ***)p->real;
    HRESULT hr = ((HRESULT (WINAPI *)(void *, void *, void **, IUnknown *))rv[6])(p->real, desc, out, outer);
    if (FAILED(hr) || !desc || !out || !*out || outer || !(dd_hold || dd_ovl)) return hr;
    DWORD size = *(DWORD *)desc, flags = ((DWORD *)desc)[1], caps;
    /* (ddsCaps sits at the same place in both today's headers; read through the right type all the same) */
    if (p->v4) {
        if (size < offsetof(DDSURFACEDESC2, ddsCaps) + sizeof(DWORD)) return hr;
        caps = ((DDSURFACEDESC2 *)desc)->ddsCaps.dwCaps;
    } else {
        if (size < offsetof(DDSURFACEDESC, ddsCaps) + sizeof(DWORD)) return hr;
        caps = ((DDSURFACEDESC *)desc)->ddsCaps.dwCaps;
    }
    if (!(flags & DDSD_CAPS) || !(caps & DDSCAPS_PRIMARYSURFACE)) return hr;
    /* the surface interface follows the object's: IDirectDraw/2 -> IDirectDrawSurface, 4 -> 4, 7 -> 7 */
    *out = ds_wrap(*out, p->v4 == 2 ? 5 : p->v4 ? 4 : 1);
    return hr;
}

static void dd_put_cs(void)   /* slot 6 of the stand-in: ours with the hold or the overlay on, the plain thunk without */
{
    if (dd_thunk[6]) dd_vtbl[6] = (dd_hold || dd_ovl) ? (void *)px_cs : dd_thunk[6];
}

void dds_set_overlay(int on)   /* before dds_init (DllMain) or after */
{
    dd_ovl = on != 0;
    dd_put_cs();
    okp_log(2, "ddshow: okplay's overlay %s the game's frames (okplay.ini wsock_overlay)", dd_ovl ? "is drawn into" : "is not drawn into");
}

void dds_set_flip_hold(int on)   /* before dds_init (DllMain) or after: both work */
{
    dd_hold = on != 0;
    dd_put_cs();
    okp_log(2, "ddshow: page flips of the game %s while a box is open (okplay.ini wsock_box_flip)", dd_hold ? "held" : "not held");
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
    if (!okp_sys.win9x) {
        hook_import(m, "user32.dll", "SetCursorPos", (void *)hk_scp, (void **)&real_scp);
        hook_import(m, "user32.dll", "ClipCursor", (void *)hk_clip, (void **)&real_clip);
    }
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
    uint8_t *code = (uint8_t *)VirtualAlloc(NULL, NTHUNK * 20, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!code) return;
    for (int i = 0; i < NTHUNK; i++) {
        static const uint8_t t[13] = { 0x8B, 0x44, 0x24, 0x04, 0x8B, 0x40, 0x04, 0x89, 0x44, 0x24, 0x04, 0x8B, 0x00 };
        uint8_t *c = code + i * 20;
        DWORD disp = (DWORD)(i * 4);
        memcpy(c, t, 13);
        c[13] = 0xFF; c[14] = 0xA0;
        memcpy(c + 15, &disp, 4);
        c[19] = 0xCC;
        dd_thunk[i] = c;
        if (i < NSLOT) dd_vtbl[i] = c;
        ds_vtbl[i] = c;
    }
    dd_vtbl[0] = (void *)px_qi;
    dd_vtbl[1] = (void *)px_addref;
    dd_vtbl[2] = (void *)px_release;
    ds_vtbl[0] = (void *)ps_qi;          /* the surface stand-ins (made only with the hold on: dds_set_flip_hold) */
    ds_vtbl[1] = (void *)px_addref;
    ds_vtbl[2] = (void *)px_release;
    ds_vtbl[3] = (void *)ps_addatt;
    ds_vtbl[5] = (void *)ps_blt;
    ds_vtbl[7] = (void *)ps_bltfast;
    ds_vtbl[8] = (void *)ps_delatt;
    ds_vtbl[11] = (void *)ps_flip;
    FlushInstructionCache(GetCurrentProcess(), code, NTHUNK * 20);
    InitializeCriticalSection(&dd_lock);
    InitializeCriticalSection(&dd_snap_cs);
    InitializeCriticalSection(&ds_lock_cs);
    InitializeCriticalSection(&ds_flip_cs);
    dd_put_cs();                         /* (the hold may have been asked for before this: slot 6 now follows it) */
    if (!GetWindowsDirectoryA(win_dir, sizeof(win_dir))) win_dir[0] = 0;
    if (!okp_sys.win9x) {
        /* a window that does not read its messages for 5 s is swapped for a pale "not responding" copy by
         * Windows; the game's window does that while it connects, under our box (Need for Speed III) */
        void (WINAPI *noghost)(void) = (void (WINAPI *)(void))GetProcAddress(GetModuleHandleA("user32.dll"), "DisableProcessWindowsGhosting");
        if (noghost) noghost();
    }
    dd_ready = 1;
    patch_module(GetModuleHandleA(NULL));
}


/* ---- the game's window must not learn that a box of ours took its place ----
 * okplay's boxes open on the thread that called into Winsock; a game whose window belongs to another thread
 * (Need for Speed III) is then told it was switched away from, and DirectDraw answers that by giving up the
 * full-screen mode: the screen keeps the last picture and the box is never seen.  While a box is open the
 * "you are no longer in front" messages stop at okplay. */
static HWND dd_game_wnd;            /* the window guarded last: in front again when the box closes */
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
/* What the box side wants last (1 = pointer shown, 0 = given back).  A "show" that timed out is posted, and a later
 * "hide" sent may arrive before it (open point 6.7): so the message only says "look again", and the game window's
 * thread always puts the pointer in the state wanted last, whatever order the messages come in. */
static volatile LONG dd_cur_want;

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
    InterlockedExchange(&dd_cur_want, on ? 1 : 0);   /* (before the message: whichever message comes last reads this) */
    if (GetWindowThreadProcessId(w, NULL) == GetCurrentThreadId()) SendMessageA(w, dd_cur_msg, on, 0);
    else if (!SendMessageTimeoutA(w, dd_cur_msg, on, 0, SMTO_ABORTIFHUNG, 300, &r))
        PostMessageA(w, dd_cur_msg, on, 0);   /* (busy just now: when it next reads its messages) */
}

/* Where a thread is, for the log: its instruction pointer and what on its stack points into a loaded module
 * (mostly return addresses).  Read while it is held for a moment; named and logged after it runs again (it
 * may hold the locks that naming and logging need). */
#ifdef _WIN64   /* (64-bit proxy build: the stall log reads 32-bit registers - not there) */
static void dds_where(DWORD tid, const char *who, DWORD silent) { (void)tid; (void)who; (void)silent; }
#else
static void dds_where(DWORD tid, const char *who, DWORD silent)
{
    DWORD a[400], out[14];
    SIZE_T got = 0;
    int n = 0, na = 0;
    CONTEXT c;
    /* (by name: Windows 98's KERNEL32 has no OpenThread, and the DLL must still load there) */
    HANDLE (WINAPI *open_thread)(DWORD, BOOL, DWORD) = (HANDLE (WINAPI *)(DWORD, BOOL, DWORD))GetProcAddress(GetModuleHandleA("kernel32.dll"), "OpenThread");
    HANDLE t = tid && open_thread ? open_thread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid) : NULL;
    if (!t) return;
    memset(&c, 0, sizeof(c));
    c.ContextFlags = CONTEXT_CONTROL;
    if (SuspendThread(t) != (DWORD)-1) {
        if (GetThreadContext(t, &c) && ReadProcessMemory(GetCurrentProcess(), (void *)(ULONG_PTR)c.Esp, a, sizeof(a), &got))
            na = (int)(got / 4);
        ResumeThread(t);
    }
    CloseHandle(t);
    if (!c.Eip) return;
    out[n++] = c.Eip;
    for (int i = 0; i < na && n < 14; i++) {
        MEMORY_BASIC_INFORMATION mi;
        if (a[i] < 0x10000 || !VirtualQuery((void *)(ULONG_PTR)a[i], &mi, sizeof(mi))) continue;
        if (mi.Type != MEM_IMAGE || !(mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) continue;
        out[n++] = a[i];
    }
    char line[900];
    int o = 0;
    for (int i = 0; i < n && o < (int)sizeof(line) - 80; i++) {
        MEMORY_BASIC_INFORMATION mi;
        char path[MAX_PATH] = "", *name = path;
        if (!VirtualQuery((void *)(ULONG_PTR)out[i], &mi, sizeof(mi)) || !mi.AllocationBase) continue;
        GetModuleFileNameA((HMODULE)mi.AllocationBase, path, sizeof(path));
        for (char *q = path; *q; q++) if (*q == '\\' || *q == '/') name = q + 1;
        o += snprintf(line + o, sizeof(line) - o, "%s%s+0x%lx", i ? (i == 1 ? " | stack: " : ", ") : "", name[0] ? name : "?",
                      (unsigned long)(out[i] - (DWORD)(ULONG_PTR)mi.AllocationBase));
    }
    okp_log(1, "stall: %s (thread %lx)%s is at %s", who, (unsigned long)tid, silent ? " has not read its messages" : "", line);
    if (silent) okp_log(1, "stall: ... for %lu ms", (unsigned long)silent);
}
#endif

/* While a box is open: does the thread of the game window still read its messages?  Asked four times a second;
 * when it has not answered for 700 ms that goes to the log (8 times a box at most) - with where it and the box's
 * thread are only when okplay.ini wsock_box_stall_log=1 asks for it. */
static volatile DWORD dd_pong;
static DWORD WINAPI dds_watch(LPVOID arg)
{
    DWORD asked = 0, last = 0, blast = 0;
    int shots = 0, was = 0, bshots = 0;
    (void)arg;
    while (dd_guard) {
        HWND w = dd_game_wnd;
        DWORD now = GetTickCount() | 1;
        if (!w || !IsWindow(w) || !dd_cur_msg) break;
        if (!asked) { asked = now; PostMessageA(w, dd_cur_msg, 2, (LPARAM)asked); }
        else if (dd_pong == asked) {
            if (was) okp_log(2, "stall: the game window's thread reads its messages again (after %lu ms)", (unsigned long)(now - asked));
            was = 0; asked = 0;
        }
        if (dd_wrap_now && dd_beat && (LONG)(now - dd_beat) > 1500 && bshots < 4 && now - blast > 2000) {   /* (signed: a round may end after now was read) */
            static const char *steps[] = { "between rounds", "waiting for the kept picture's lock", "composing the picture",
                "waiting for the DirectDraw objects' lock", "in the wrapper: GetGDISurface", "in the wrapper: GetDC on the game's screen",
                "drawing onto the game's screen", "in the wrapper: ReleaseDC", "in the wrapper: Release", "tidying up" };
            LONG st = dd_step;
            const char *sn = st >= 0 && st <= 9 ? steps[st] : st == 20 ? "round start" : st == 21 ? "the window order" : st == 22 ? "the box's drawing" : "?";
            okp_log(1, "stall: the box's own round has not finished for %lu ms - step %ld (%s); the game window's thread %s",
                    (unsigned long)(now - dd_beat), (long)st, sn, dd_pong == asked || !asked ? "answers" : "does not answer");
            if (dd_stall_log && dd_box_tid) dds_where(dd_box_tid, "the box's thread", 0);
            bshots++; blast = now;
        }
        if (now - asked > 700 && asked && dd_pong != asked && shots < 8 && now - last > 1000) {
            DWORD gt = GetWindowThreadProcessId(w, NULL);
            /* where the threads are is read by holding them for a moment: a diagnosis, only when asked for
             * (okplay.ini wsock_box_stall_log=1).  Otherwise one line, and no thread is touched. */
            if (dd_stall_log) {
                dds_where(gt, "the game window's thread", now - asked);
                if (dd_box_tid != gt) dds_where(dd_box_tid, "the box's thread", 0);
            }
            else okp_log(2, "stall: the game window's thread (%lx) has not read its messages for %lu ms", (unsigned long)gt, (unsigned long)(now - asked));
            shots++; last = now; was = 1;
        }
        Sleep(250);
    }
    return 0;
}

static LRESULT CALLBACK dds_game_wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    WNDPROC old = (WNDPROC)GetPropA(w, DD_PROP);
    if (!old) return DefWindowProcA(w, m, wp, lp);   /* (never: the property is set before this is put in place) */
    if (dd_cur_msg && m == dd_cur_msg) {
        if (wp == 2) dd_pong = (DWORD)lp;   /* the watcher's question: this thread reads its messages */
        else dds_cursor_on_game_thread(dd_cur_want != 0);   /* (wp: what was wanted when sent; the last wish counts) */
        return 0;
    }
    if (m == WM_NCDESTROY) dd_cur_shows = 0;
    if (dd_guard && dd_wrap_now && dd_snap && (m == WM_PAINT || m == WM_ERASEBKGND)) {
        if (m == WM_ERASEBKGND) return 1;
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        EnterCriticalSection(&dd_snap_cs);
        if (dc && dd_snap) {
            HDC mdc = CreateCompatibleDC(dc);
            HGDIOBJ was = mdc ? SelectObject(mdc, dd_snap) : NULL;
            if (mdc) BitBlt(dc, 0, 0, dd_snap_w, dd_snap_h, mdc, 0, 0, SRCCOPY);
            if (mdc) { SelectObject(mdc, was); DeleteDC(mdc); }
        }
        LeaveCriticalSection(&dd_snap_cs);
        EndPaint(w, &ps);
        return 0;
    }
    if (dd_guard && m == WM_SETCURSOR) { SetCursor(LoadCursorA(NULL, (LPCSTR)IDC_ARROW)); return TRUE; }
    if (dd_guard) {
        int off = (m == WM_ACTIVATEAPP && !wp) || (m == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) || m == WM_KILLFOCUS ||
                  (m == WM_NCACTIVATE && !wp);
        /* "in front again" while the box is still open (the player clicked the game, then the box): the game was
         * never told it left the front, and told "in front" a second time it started drawing with the screen not
         * its own - a fault inside its drawing followed (Need for Speed III, Windows 11, 2026-11-11).  Kept from
         * the game too; Windows 9x as tested, unchanged. */
        int on = !okp_sys.win9x && ((m == WM_ACTIVATEAPP && wp) || (m == WM_ACTIVATE && LOWORD(wp) != WA_INACTIVE) ||
                                    m == WM_SETFOCUS || (m == WM_NCACTIVATE && wp));
        int kept = dd_keep && (off || on);
        /* over a wrapper's window (dds_wrapper) the box takes the front, so the game window must not be put away
         * meanwhile: a minimise asked by message is refused (the game came down under a front-taking box with
         * DDrawCompat, 2026-11-16); one made another way is logged below and undone when the box closes */
        int nomin = dd_wrap_now && m == WM_SYSCOMMAND && (wp & 0xfff0) == SC_MINIMIZE;
        if (off || m == WM_ACTIVATEAPP || m == WM_ACTIVATE || m == WM_SETFOCUS || m == WM_NCACTIVATE || m == WM_SIZE ||
            m == WM_SYSCOMMAND || m == WM_DISPLAYCHANGE || m == WM_SHOWWINDOW || m == WM_WINDOWPOSCHANGED)
            okp_log(3, "ddshow: game window message 0x%04x wp %lx lp %lx%s%s", m, (unsigned long)wp, (unsigned long)lp,
                    kept || nomin ? " - kept from the game" : "", IsIconic(w) ? " (minimised)" : "");
        if (nomin) return 0;
        /* okplay.ini wsock_box_focus=release: the game is told instead (it then puts itself away while a box is open) */
        if (kept) return m == WM_NCACTIVATE ? TRUE : 0;
    }
    else if (dd_tail && GetTickCount() - dd_tail < 4000) {   /* just after a box: what reaches the game (log only) */
        if (m == WM_ACTIVATEAPP || m == WM_ACTIVATE || m == WM_SETFOCUS || m == WM_KILLFOCUS || m == WM_NCACTIVATE || m == WM_SIZE ||
            m == WM_SYSCOMMAND || m == WM_DISPLAYCHANGE || m == WM_SHOWWINDOW || m == WM_WINDOWPOSCHANGED)
            okp_log(3, "ddshow: after the box, game window message 0x%04x wp %lx lp %lx%s", m, (unsigned long)wp, (unsigned long)lp,
                    IsIconic(w) ? " (minimised)" : "");
    }
    if (m == WM_NCDESTROY) {   /* the window goes: its own procedure back, and forgotten here */
        RemovePropA(w, DD_PROP);
        if ((WNDPROC)GetWindowLongPtrA(w, GWLP_WNDPROC) == dds_game_wndproc) SetWindowLongPtrA(w, GWLP_WNDPROC, (LONG_PTR)old);
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

static int dds_guard_begin(void)   /* 0: no game window found */
{
    HWND w = NULL;
    EnumWindows(dds_game_win, (LPARAM)&w);
    if (!w) return 0;
    if (!GetPropA(w, DD_PROP)) {   /* not taken over yet */
        WNDPROC cur = (WNDPROC)GetWindowLongPtrA(w, GWLP_WNDPROC);
        int room = 0;
        if (cur && cur != dds_game_wndproc) {
            EnterCriticalSection(&dd_lock);
            room = dd_nwnd < DD_MAXWND;
            if (room) dd_wnds[dd_nwnd++] = w;
            LeaveCriticalSection(&dd_lock);
        }
        if (room) {
            SetPropA(w, DD_PROP, (HANDLE)cur);   /* the old procedure first, then ours in its place */
            WNDPROC was = (WNDPROC)SetWindowLongPtrA(w, GWLP_WNDPROC, (LONG_PTR)dds_game_wndproc);
            if (was && was != cur && was != dds_game_wndproc) SetPropA(w, DD_PROP, (HANDLE)was);   /* (changed meanwhile) */
            okp_log(2, "ddshow: watching the game window %p (thread %lx; the box opens on thread %lx)", (void *)w,
                    (unsigned long)GetWindowThreadProcessId(w, NULL), (unsigned long)GetCurrentThreadId());
        }
    } else if ((WNDPROC)GetWindowLongPtrA(w, GWLP_WNDPROC) != dds_game_wndproc) {
        /* someone put a window procedure in front of ours since: ours still runs behind it, left as it is */
        okp_log(3, "ddshow: the game window's procedure changed since okplay took its place in front");
    }
    dd_game_wnd = w;
    dd_box_tid = GetCurrentThreadId();
    InterlockedExchange(&dd_guard, 1);
    if (GetPropA(w, DD_PROP)) dds_cursor(w, 1);
    if (!okp_sys.win9x && GetPropA(w, DD_PROP)) {   /* (log only; Windows 9x has no OpenThread) */
        HANDLE t = CreateThread(NULL, 64 * 1024, dds_watch, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return 1;
}

void dds_detach(void)   /* this DLL is unloaded while the process goes on: every window gets its own procedure back */
{
    if (!dd_ready) return;
    EnterCriticalSection(&dd_lock);
    for (int i = 0; i < dd_nwnd; i++) {
        HWND w = dd_wnds[i];
        WNDPROC old = (WNDPROC)GetPropA(w, DD_PROP);
        if (!IsWindow(w) || !old) continue;
        if ((WNDPROC)GetWindowLongPtrA(w, GWLP_WNDPROC) == dds_game_wndproc) {
            SetWindowLongPtrA(w, GWLP_WNDPROC, (LONG_PTR)old);
            RemovePropA(w, DD_PROP);
        }   /* (else another procedure sits in front of ours and calls it: nothing can be undone safely) */
    }
    dd_nwnd = 0;
    LeaveCriticalSection(&dd_lock);
}

/* ---- a DirectDraw wrapper shows the game (universal: any game that ships one) ----
 * Some game packs bring their own ddraw.dll that turns "full screen" into a borderless window and shows ordinary
 * Windows windows over it itself - DDrawCompat (Need for Speed III portable 1.6.2: FullscreenMode = borderless,
 * GdiInterops = all, AltTabFix = noactivateapp, CpuAffinity = 1).  Our own full-screen measures then fight the
 * wrapper's: on a laptop, with Windows' screen page asked for, the box kept out of the front and the keyboard taken
 * by a system-wide hook, the box could not be seen or used and the game froze (2026-11-16).  So with such a wrapper
 * the box takes the front as over a windowed game, with none of the screen-page switch, out-of-the-front box or
 * system-wide keyboard hook.  The guard stays (test 1 without it: the game minimised itself under the box): the game
 * window is not told it lost the front, a minimise asked of it is refused, its pointer is looked after; afterwards
 * it gets the front back (restored if it was put away anyway).
 * Found by the wrapper's own version text (RT_VERSION of the loaded ddraw.dll, read in place) and, for DDrawCompat,
 * its FullscreenMode setting: "exclusive" there means real full screen, and the usual measures apply.
 * okplay.ini wsock_box_wrapper=off turns this off. */
static int dd_wrap_mode = 0;        /* dds_set_wrapper: 1 = auto, 0 = off (the default since 0.3.4: the 0.8.7 method held over DDrawCompat, 2026-11-19) */
static int dd_wrapped = -1;         /* -1 = not looked yet; 0 = none (or exclusive); 1 = a borderless wrapper */
void dds_set_wrapper(int on) { dd_wrap_mode = on; }

static int dd_has_wtext(const BYTE *p, DWORD n, const char *t)   /* an ASCII text stored as UTF-16 in p[0..n) */
{
    size_t k = strlen(t);
    for (DWORD i = 0; i + 2 * k <= n; i++) {
        size_t j = 0;
        while (j < k && p[i + 2 * j] == (BYTE)t[j] && p[i + 2 * j + 1] == 0) j++;
        if (j == k) return 1;
    }
    return 0;
}

static void dd_ini_value(const char *path, const char *key, char *out, int n)   /* "Key = value" lines, no sections */
{
    char buf[8192];
    DWORD got = 0;
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    if (!ReadFile(h, buf, sizeof(buf) - 1, &got, NULL)) got = 0;
    CloseHandle(h);
    buf[got] = 0;
    size_t k = strlen(key);
    for (char *l = buf; l && *l; ) {
        char *e = strpbrk(l, "\r\n");
        if (e) *e = 0;
        while (*l == ' ' || *l == '\t') l++;
        if (!_strnicmp(l, key, k) && (l[k] == ' ' || l[k] == '\t' || l[k] == '=')) {
            char *v = strchr(l, '=');
            if (v) {
                v++;
                while (*v == ' ' || *v == '\t') v++;
                lstrcpynA(out, v, n);
                for (int i = (int)strlen(out) - 1; i >= 0 && (out[i] == ' ' || out[i] == '\t'); i--) out[i] = 0;
            }
            return;
        }
        l = e ? e + 1 : NULL;
        while (l && (*l == '\r' || *l == '\n')) l++;
    }
}

static int dds_wrapper(void)
{
    char path[MAX_PATH] = "", dir[MAX_PATH], exe[MAX_PATH] = "", ini[MAX_PATH], mode[32] = "";
    const char *name = NULL;
    if (!dd_wrap_mode) return 0;
    if (dd_wrapped >= 0) return dd_wrapped;
    /* the DirectDraw the game got (a wrapper loads Windows' own ddraw.dll too, under the same name): the module
     * its DirectDrawCreate is in; by name only if the game's call was not seen */
    MEMORY_BASIC_INFORMATION mb;
    void *f = real_ddc ? (void *)real_ddc : (void *)real_ddcex;
    HMODULE m = NULL;
    if (f && VirtualQuery(f, &mb, sizeof(mb))) m = (HMODULE)mb.AllocationBase;
    if (!m) m = GetModuleHandleA("ddraw.dll");
    if (!m) return 0;   /* (not loaded yet: looked at again at the next box) */
    dd_wrapped = 0;
    GetModuleFileNameA(m, path, sizeof(path));
    HRSRC r = FindResourceA(m, MAKEINTRESOURCEA(1), (LPCSTR)RT_VERSION);
    HGLOBAL g = r ? LoadResource(m, r) : NULL;
    const BYTE *v = g ? (const BYTE *)LockResource(g) : NULL;
    DWORD vn = r ? SizeofResource(m, r) : 0;
    if (v && dd_has_wtext(v, vn, "DDrawCompat")) name = "DDrawCompat";
    else if (v && dd_has_wtext(v, vn, "dgVoodoo")) name = "dgVoodoo";
    else if (v && dd_has_wtext(v, vn, "cnc-ddraw")) name = "cnc-ddraw";
    if (!name) { okp_log(3, "ddshow: DirectDraw is %s - no wrapper known", path); return 0; }
    if (strcmp(name, "DDrawCompat")) {   /* known, but not tested yet: the usual measures, and a note in the log */
        okp_log(1, "ddshow: DirectDraw is %s (%s) - not tested with okplay's boxes yet, the usual measures apply", name, path);
        return 0;
    }
    /* DDrawCompat's settings: DDrawCompat-<exe name>.ini beside the exe, else DDrawCompat.ini beside the wrapper */
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    lstrcpynA(dir, exe, sizeof(dir));
    char *sl = strrchr(dir, '\\'); if (sl) sl[1] = 0;
    char *base = strrchr(exe, '\\'); base = base ? base + 1 : exe;
    char *dot = strrchr(base, '.'); if (dot) *dot = 0;
    char gini[MAX_PATH], on[16] = "";
    snprintf(ini, sizeof(ini), "%sDDrawCompat-%s.ini", dir, base);
    lstrcpynA(gini, path, sizeof(gini));
    sl = strrchr(gini, '\\'); if (sl) lstrcpyA(sl + 1, "DDrawCompat.ini");
    dd_ini_value(ini, "FullscreenMode", mode, sizeof(mode));   /* (the program's own settings first, then the shared ones) */
    if (!mode[0]) dd_ini_value(gini, "FullscreenMode", mode, sizeof(mode));
    dd_ini_value(ini, "EnableDDrawCompat", on, sizeof(on));
    if (!on[0]) dd_ini_value(gini, "EnableDDrawCompat", on, sizeof(on));
    if (!_stricmp(on, "off")) {   /* switched off: it hands everything to Windows' own DirectDraw */
        okp_log(1, "ddshow: DirectDraw is DDrawCompat (%s) with EnableDDrawCompat = off - the usual measures apply", path);
        return 0;
    }
    dd_wrapped = _strnicmp(mode, "exclusive", 9) != 0;   /* (borderless is DDrawCompat's own default) */
    okp_log(1, "ddshow: DirectDraw is DDrawCompat (%s), FullscreenMode = %s - %s", path, mode[0] ? mode : "(not set: borderless)",
            dd_wrapped ? "okplay's boxes open as over a windowed game (no full-screen measures)" : "real full screen, the usual measures apply");
    return dd_wrapped;
}

void dds_guard_end(void)   /* the box is gone: the game window in front again, then its messages as usual */
{
    int fg = -1;   /* SetForegroundWindow's answer; -1 = no game window to call it for */
    DWORD fpid = 0;
    HWND f;
    ovl_pause(0);
    if (!dd_guard) return;
    if (dd_game_wnd && IsWindow(dd_game_wnd) && GetPropA(dd_game_wnd, DD_PROP)) dds_cursor(dd_game_wnd, 0);
    if (dd_game_wnd && IsWindow(dd_game_wnd)) {
        if ((!dd_keep || dd_wrap_now) && IsIconic(dd_game_wnd)) {   /* (it put itself away) */
            okp_log(1, "ddshow: the game window was minimised under the box - restored");
            ShowWindowAsync(dd_game_wnd, SW_RESTORE);
        }
        fg = SetForegroundWindow(dd_game_wnd) ? 1 : 0;
    }
    dd_tail = GetTickCount() | 1;
    InterlockedExchange(&dd_guard, 0);
    InterlockedExchange(&dd_wrap_now, 0);
    if (dd_snap) {
        EnterCriticalSection(&dd_snap_cs);
        DeleteObject(dd_snap);
        dd_snap = NULL;
        LeaveCriticalSection(&dd_snap_cs);
        if (dd_game_wnd && IsWindow(dd_game_wnd)) InvalidateRect(dd_game_wnd, NULL, FALSE);
    }
    InterlockedExchange(&dd_stop, 0);
    f = GetForegroundWindow();
    if (f) GetWindowThreadProcessId(f, &fpid);
    okp_log(2, "ddshow: box over - guard off, pointer given back, game window to the front: %d, foreground now %s", fg,
            f && fpid == GetCurrentProcessId() ? "ours" : "other");
    if (dd_hold) okp_log(2, "ddshow: held %ld page flips of the game while the box was open", (long)InterlockedExchange(&dd_held, 0));
}

int dds_box_over_game(void) { return dd_guard != 0; }
int dds_box_wrapped(void) { return dd_guard && dd_wrap_now; }

/* ---- the box stays above the game over a wrapper's window (universal; any wrapper the box method is used for) ----
 * Over DDrawCompat the box, though owned by the game window, was sometimes not seen: right after it opened the game
 * window was moved in the window order (WM_WINDOWPOSCHANGED, from outside the game - laptop, 2026-11-16: one launch
 * fine, the next not).  So while the box is open, ten times a second on the box's own thread (a thread timer read
 * by the box's message filter, wsock.c), the window order is looked at: if the game window, or another window of
 * this program that is not a box, is above the box, the box is put back on top and given the front.  Logged the first
 * time and when what was found changes, so the log says who covered it. */
static BOOL CALLBACK dds_box_of_thread(HWND w, LPARAM lp)
{
    char cls[16] = "";
    if (!IsWindowVisible(w)) return TRUE;
    GetClassNameA(w, cls, sizeof(cls));
    if (strcmp(cls, "#32770")) return TRUE;
    *(HWND *)lp = w;   /* (the last one listed: the top box of this thread) */
    return TRUE;
}

static int dd_watch_raised, dd_watch_seen, dd_watch_ticks;
static int dd_paint = 2;            /* okplay.ini wsock_box_paint: 2 = draw (default), 1 = present, 0 = off (see dds_box_watch) */
static int dd_paint_ok, dd_paint_fail;
void dds_set_paint(int on) { dd_paint = on; }

/* ---- the box drawn into the game's own screen, over a wrapper's window ----
 * On top, in front and repainted, the box was still not seen over DDrawCompat (laptop, 2026-11-19, 0.2.13/0.2.14):
 * the wrapper puts on the screen what the game draws, and the game does not draw while it waits on us.  So we draw
 * for it: the game's picture kept when the box opened, with the box (PrintWindow) at its place, onto the surface
 * the game's DirectDraw shows (IDirectDraw*::GetGDISurface - the documented way to ask for it; every interface
 * version has it at the same place, as GetDC/ReleaseDC on the surface).  The box window itself stays where it is
 * (on top, in front, invisible or not), so the pointer and the keys still reach it.  Ten times a second while the box
 * is open, on the box's thread - the game's own thread, waiting in our call.  The game draws over it again itself
 * once the box is answered. */
static int dds_paint_box(HWND box, int with_box)
{
    BOOL (WINAPI *pw)(HWND, HDC, UINT) = (BOOL (WINAPI *)(HWND, HDC, UINT))GetProcAddress(GetModuleHandleA("user32.dll"), "PrintWindow");
    HDC sdc, mdc = NULL, kdc = NULL;
    HBITMAP bm = NULL;
    HGDIOBJ mwas = NULL, kwas = NULL;
    RECT r;
    int done = 0, w, h;
    dd_step = 1;   /* waiting for the kept picture's lock */
    EnterCriticalSection(&dd_snap_cs);
    dd_step = 2;   /* composing the picture (memory only) */
    if (!dd_snap) { LeaveCriticalSection(&dd_snap_cs); return 0; }
    w = dd_snap_w; h = dd_snap_h;
    sdc = GetDC(NULL);
    if (sdc) { mdc = CreateCompatibleDC(sdc); kdc = CreateCompatibleDC(sdc); bm = CreateCompatibleBitmap(sdc, w, h); }
    if (mdc && kdc && bm) {
        mwas = SelectObject(mdc, bm);
        kwas = SelectObject(kdc, dd_snap);
        BitBlt(mdc, 0, 0, w, h, kdc, 0, 0, SRCCOPY);
        SelectObject(kdc, kwas);
        if (with_box && box && pw && GetWindowRect(box, &r)) {   /* (screen and picture: the same 640x480 as the game sees it) */
            int bw = r.right - r.left, bh = r.bottom - r.top;
            HBITMAP bb = CreateCompatibleBitmap(sdc, bw, bh);
            if (bb) {
                kwas = SelectObject(kdc, bb);
                if (pw(box, kdc, 0)) BitBlt(mdc, r.left, r.top, bw, bh, kdc, 0, 0, SRCCOPY);
                SelectObject(kdc, kwas);
                DeleteObject(bb);
            }
        }
        /* onto the game's shown surface, through the first DirectDraw object of the game that hands one out */
        dd_step = 3;   /* waiting for the DirectDraw objects' lock */
        EnterCriticalSection(&dd_lock);
        for (dd_px *p = dd_list; p && !done; p = p->next) {
            void *surf = NULL, **sv;
            HDC gdc = NULL;
            if (!p->live) continue;
            void **rv = *(void ***)p->real;
            dd_step = 4;   /* in the wrapper: GetGDISurface */
            if (FAILED(((HRESULT (WINAPI *)(void *, void **))rv[14])(p->real, &surf)) || !surf) continue;   /* GetGDISurface */
            sv = *(void ***)surf;
            dd_step = 5;   /* in the wrapper: GetDC on the game's screen */
            if (SUCCEEDED(((HRESULT (WINAPI *)(void *, HDC *))sv[17])(surf, &gdc)) && gdc) {   /* GetDC */
                dd_step = 6;   /* drawing onto the game's screen */
                BitBlt(gdc, 0, 0, w, h, mdc, 0, 0, SRCCOPY);
                dd_step = 7;   /* in the wrapper: ReleaseDC */
                ((HRESULT (WINAPI *)(void *, HDC))sv[26])(surf, gdc);   /* ReleaseDC */
                done = 1;
            }
            dd_step = 8;   /* in the wrapper: Release */
            ((ULONG (WINAPI *)(void *))sv[2])(surf);   /* Release (GetGDISurface added a reference) */
        }
        LeaveCriticalSection(&dd_lock);
        dd_step = 9;   /* tidying up (memory only) */
        SelectObject(mdc, mwas);
    }
    if (bm) DeleteObject(bm);
    if (mdc) DeleteDC(mdc);
    if (kdc) DeleteDC(kdc);
    if (sdc) ReleaseDC(NULL, sdc);
    LeaveCriticalSection(&dd_snap_cs);
    if (done) { if (!dd_paint_ok++) okp_log(2, with_box ? "ddshow: the box is drawn into the game's screen (its DirectDraw GDI surface)"
                                                         : "ddshow: the game's kept picture is put on its screen (its DirectDraw GDI surface), the wrapper draws the box"); }
    else if (!dd_paint_fail++) okp_log(1, "ddshow: the box could not be drawn into the game's screen (no GDI surface / no DC)");
    return done;
}

/* "present" after the first picture: one pixel of the kept picture put back on the game's screen, in a corner the
 * box does not cover - enough for the wrapper to draw again (with the box), nothing of its drawing is wiped */
static void dds_kick(HWND box)
{
    RECT r;
    int x = 0, y = 0, done = 0;
    COLORREF c = 0;
    EnterCriticalSection(&dd_snap_cs);
    if (!dd_snap) { LeaveCriticalSection(&dd_snap_cs); return; }
    if (box && GetWindowRect(box, &r) && r.left <= 0 && r.top <= 0) { x = dd_snap_w - 1; y = dd_snap_h - 1; }
    HDC sdc = GetDC(NULL), kdc = sdc ? CreateCompatibleDC(sdc) : NULL;
    if (kdc) { HGDIOBJ was = SelectObject(kdc, dd_snap); c = GetPixel(kdc, x, y); SelectObject(kdc, was); DeleteDC(kdc); }
    if (sdc) ReleaseDC(NULL, sdc);
    LeaveCriticalSection(&dd_snap_cs);
    EnterCriticalSection(&dd_lock);
    for (dd_px *p = dd_list; p && !done; p = p->next) {
        void *surf = NULL, **sv;
        HDC gdc = NULL;
        if (!p->live) continue;
        void **rv = *(void ***)p->real;
        if (FAILED(((HRESULT (WINAPI *)(void *, void **))rv[14])(p->real, &surf)) || !surf) continue;   /* GetGDISurface */
        sv = *(void ***)surf;
        if (SUCCEEDED(((HRESULT (WINAPI *)(void *, HDC *))sv[17])(surf, &gdc)) && gdc) {   /* GetDC */
            SetPixel(gdc, x, y, c);
            ((HRESULT (WINAPI *)(void *, HDC))sv[26])(surf, gdc);   /* ReleaseDC */
            done = 1;
        }
        ((ULONG (WINAPI *)(void *))sv[2])(surf);
    }
    LeaveCriticalSection(&dd_lock);
}

void dds_box_paint_end(void)   /* the box closed: the kept picture back on the game's screen, until the game draws */
{
    if (dd_paint && dd_paint_ok && dd_wrap_now) dds_paint_box(NULL, 0);
}
void dds_box_watch(int first)
{
    HWND box = NULL, w, cover = NULL, g = dd_game_wnd;
    char cls[64] = "", txt[64] = "";
    DWORD pid = 0;
    int n = 0;
    if (!dd_guard || !dd_wrap_now) return;
    dd_step = 20;   /* the box's look round starts */
    if (first) { dd_watch_raised = 0; dd_watch_seen = -1; dd_watch_ticks = 0; dd_paint_ok = dd_paint_fail = 0; dd_step = 0; dd_beat = GetTickCount() | 1; return; }   /* (the box is not up yet: the first look logs) */
    EnumThreadWindows(GetCurrentThreadId(), dds_box_of_thread, (LPARAM)&box);
    if (!box) { dd_step = 0; dd_beat = GetTickCount() | 1; return; }
    dd_step = 21;   /* the window order */
    for (w = GetTopWindow(NULL); w && w != box && n < 2000; w = GetWindow(w, GW_HWNDNEXT), n++) {
        if (!IsWindowVisible(w)) continue;
        GetWindowThreadProcessId(w, &pid);
        if (w == g || (pid == GetCurrentProcessId() && GetWindow(box, GW_OWNER) != w)) {
            RECT a, b, i;
            if (GetWindowRect(w, &a) && GetWindowRect(box, &b) && IntersectRect(&i, &a, &b)) { cover = w; break; }
        }
    }
    if ((cover != NULL) != dd_watch_seen) {
        HWND f = GetForegroundWindow();
        DWORD fpid = 0;
        if (f) { GetWindowThreadProcessId(f, &fpid); GetClassNameA(f, cls, sizeof(cls)); GetWindowTextA(f, txt, sizeof(txt)); }
        okp_log(2, "ddshow: box %p %s; in front: %p \"%s\" \"%s\" (%s)", (void *)box,
                cover ? "is under a window of this program" : "is on top", (void *)f, cls, txt,
                !f ? "none" : f == box ? "the box" : fpid == GetCurrentProcessId() ? "this program" : "another program");
        if (cover) {
            char ccls[64] = "";
            GetClassNameA(cover, ccls, sizeof(ccls));
            okp_log(1, "ddshow: the box was covered by %p \"%s\"%s - put back on top", (void *)cover, ccls, cover == g ? " (the game window)" : "");
        }
        dd_watch_seen = cover != NULL;
    }
    /* The box on top and in front, and still not seen (laptop, 2026-11-19, 0.2.13): the wrapper shows only what it
     * last put on the screen, and the game does not draw while it waits on us.  For the box's first 2 s the game
     * window (painted from the kept picture) and the box are repainted, so the wrapper has something new to show -
     * as when the box was dragged, which it showed cleanly (0.2.8). */
    dd_step = 22;   /* the box's drawing */
    if (dd_paint == 1) {
        /* [default] "present": the wrapper draws the box itself.  DDrawCompat draws a program's ordinary windows into
         * the game's screen (GdiInterops) whenever something new reaches it - seen in 0.2.15, where it drew its own
         * copy of the box over ours.  The game does not draw while it waits on us, so: the game's kept picture once
         * (and the box painted once), then ten times a second one pixel of it in a corner the box does not cover.
         * 0.2.17 put the whole picture back each time and repainted the box: that wiped the wrapper's box each time
         * and it blinked hard.  The box is the real window, title bar and close button as Windows draws them. */
        /* The box painted once more a few times just after the wrapper started drawing it: painted only before
         * (0.2.18) the wrapper's copy was an empty frame until the box was dragged, which repainted it. */
        if (!dd_paint_ok) {
            RedrawWindow(box, NULL, NULL, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
            dds_paint_box(box, 0);
            dd_watch_ticks = 0;
        }
        else {
            ++dd_watch_ticks;
            if (dd_watch_ticks == 2 || dd_watch_ticks == 3 || dd_watch_ticks == 5 || dd_watch_ticks == 10)
                RedrawWindow(box, NULL, NULL, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
            dds_kick(box);
        }
    }
    else if (dd_paint == 2) {
        /* "draw" (0.2.16): the box window itself made see-through (opacity 1 of 255): the wrapper drew it too, without its text,
         * between our pictures - the box blinked (laptop, 2026-11-19, 0.2.15).  A layered window that is not fully
         * transparent still takes the pointer and keys, and PrintWindow still draws it whole for our picture. */
        /* (drawn first, see-through after: made see-through before the first picture was in, the box blinked once) */
        int drawn = dds_paint_box(box, 1);
        LONG ex = GetWindowLongA(box, GWL_EXSTYLE);
        if (drawn && !(ex & WS_EX_LAYERED)) {
            BOOL (WINAPI *sla)(HWND, COLORREF, BYTE, DWORD) =
                (BOOL (WINAPI *)(HWND, COLORREF, BYTE, DWORD))GetProcAddress(GetModuleHandleA("user32.dll"), "SetLayeredWindowAttributes");
            if (sla) {
                SetWindowLongA(box, GWL_EXSTYLE, ex | WS_EX_LAYERED);
                sla(box, 0, 1, LWA_ALPHA);
                okp_log(2, "ddshow: box %p made see-through - what is seen of it is drawn into the game's screen", (void *)box);
            }
        }
    }
    else if (dd_watch_ticks < 8) {
        if (++dd_watch_ticks == 1) okp_log(2, "ddshow: repainting the game window and the box for 2 s (the wrapper shows them)");
        if (g && IsWindow(g)) RedrawWindow(g, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        RedrawWindow(box, NULL, NULL, RDW_INVALIDATE | RDW_FRAME | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
    if (cover) {
        SetWindowPos(box, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        BringWindowToTop(box);
        if (GetForegroundWindow() != box) SetForegroundWindow(box);
        if (++dd_watch_raised == 40) okp_log(1, "ddshow: the box was put back on top 40 times - something keeps covering it");
    }
    dd_step = 0;
    dd_beat = GetTickCount() | 1;   /* this look round finished */
}
int dds_box_can_drag(void) { return dd_guard && dd_wrap_now && dd_snap; }
HWND dds_game_window(void) { return dd_guard ? dd_game_wnd : NULL; }

int dds_on_game_window_thread(void)   /* the calling thread owns the game window a box is open over */
{
    HWND w = dd_game_wnd;
    return dd_guard && w && GetWindowThreadProcessId(w, NULL) == GetCurrentThreadId();
}

int dds_show_gdi(void)   /* the thread that will show the window (the game's own, for okplay's boxes) */
{
    if (!dd_ready) return 0;
    int n = 0;
    ovl_pause(1);   /* one drawer at a time: the overlay stands aside while an old box is open (dds_guard_end) */
    if (dds_wrapper()) {   /* the wrapper shows our window itself: no screen-page switch (see dds_wrapper) */
        if (dd_guard) return 0;   /* (an inner box: the outer one's guard stays) */
        /* (okbox_prefer_styled - okplay's own box style, no Windows title bar - is not used yet: the styled box paints
         * only on screen, so PrintWindow got nothing and nothing was drawn, laptop 2026-11-19, 0.2.20) */
        InterlockedExchange(&dd_wrap_now, 1);
        if (!dds_guard_begin()) {
            InterlockedExchange(&dd_wrap_now, 0);
            okp_log(2, "ddshow: over a wrapper's window, but no game window found - the box opens the plain way");
            return 0;
        }
        if (dd_drag && !dd_snap) {
            MONITORINFO mi;
            HMONITOR mon = MonitorFromWindow(dd_game_wnd, MONITOR_DEFAULTTONEAREST);
            memset(&mi, 0, sizeof(mi)); mi.cbSize = sizeof(mi);
            if (mon && GetMonitorInfoA(mon, &mi)) {
                int cw = mi.rcMonitor.right - mi.rcMonitor.left, ch = mi.rcMonitor.bottom - mi.rcMonitor.top;
                HDC sdc = GetDC(NULL);
                HDC mdc = sdc ? CreateCompatibleDC(sdc) : NULL;
                HBITMAP bm = sdc ? CreateCompatibleBitmap(sdc, cw, ch) : NULL;
                if (mdc && bm) {
                    HGDIOBJ was = SelectObject(mdc, bm);
                    BOOL ok = BitBlt(mdc, 0, 0, cw, ch, sdc, mi.rcMonitor.left, mi.rcMonitor.top, SRCCOPY);
                    SelectObject(mdc, was);
                    if (ok) {
                        EnterCriticalSection(&dd_snap_cs);
                        dd_snap = bm; dd_snap_w = cw; dd_snap_h = ch; bm = NULL;
                        LeaveCriticalSection(&dd_snap_cs);
                    }
                }
                if (bm) DeleteObject(bm);
                if (mdc) DeleteDC(mdc);
                if (sdc) ReleaseDC(NULL, sdc);
            }
            okp_log(2, "ddshow: the game's picture kept for under the box: %s (%dx%d)", dd_snap ? "yes" : "no - the box cannot be dragged",
                    dd_snap_w, dd_snap_h);
        }
        okp_log(2, "ddshow: box over a wrapper's window - no screen-page switch; the game is not told it lost the front; the box takes the front");
        return 0;
    }
    if (dd_hold) {   /* no flip of the game after the switch below: one under way ends first, later ones are held */
        EnterCriticalSection(&ds_flip_cs);
        InterlockedExchange(&dd_stop, 1);
        LeaveCriticalSection(&ds_flip_cs);
    }
    EnterCriticalSection(&dd_lock);
    for (dd_px *p = dd_list; p; p = p->next) {
        if (!p->live) continue;
        void **rv = *(void ***)p->real;
        HRESULT hr = ((HRESULT (WINAPI *)(void *))rv[10])(p->real);   /* IDirectDraw*::FlipToGDISurface */
        okp_log(3, "ddshow: FlipToGDISurface on %p -> 0x%08lx", p->real, (unsigned long)hr);
        n++;
    }
    LeaveCriticalSection(&dd_lock);
    InterlockedExchange(&dd_held, 0);
    if (!n || !dds_guard_begin()) {
        InterlockedExchange(&dd_stop, 0);   /* (no guard: nothing would switch the hold off again) */
        okp_log(2, "ddshow: no DirectDraw object of the game / no game window found - the box opens the plain way");
    }
    return n;
}


/* ---- a screenshot on a key (universal: any game the shim is in) ----
 * Windows' own screenshot keys often get nothing of a game a wrapper shows (NFS3 portable with DDrawCompat,
 * 2026-11-16), and never the game and okplay's box together.  A key pressed in a box (default
 * Ctrl+F12, okplay.ini wsock_screenshot_key=off|ctrl+f12|alt+f11|...) saves what is on the screen of the game, with
 * okplay's open boxes drawn on top where they are, as a BMP in "okplay-screenshots" beside the game.
 * Where the work is done (the first try read the screen from the key's own thread while a box was open over
 * DDrawCompat, and the game froze, 2026-11-16 - every step is logged now, so a stop shows where it was):
 *  - only while a box is open, on the box's own thread (its message filter in wsock.c calls dds_shot_key for each
 *    key): over a wrapper's window the game's picture is the one kept when the box opened (dd_snap: that screen read
 *    worked), otherwise the screen is read, the size of the game window's monitor.
 * The boxes are drawn in by PrintWindow (XP and later, looked up at run time). */
static BOOL CALLBACK shot_box_win(HWND w, LPARAM lp)   /* okplay's visible dialogs, top first */
{
    HWND *list = (HWND *)lp;
    DWORD pid = 0;
    char cls[16] = "";
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(w)) return TRUE;
    GetClassNameA(w, cls, sizeof(cls));
    if (strcmp(cls, "#32770")) return TRUE;
    for (int i = 0; i < 15; i++) if (!list[i]) { list[i] = w; break; }
    return TRUE;
}

static int shot_save(HBITMAP bm, int w, int h, const char *path)   /* a 24-bit BMP */
{
    BITMAPINFOHEADER bi;
    BITMAPFILEHEADER bf;
    int stride = ((w * 3 + 3) & ~3), ok = 0;
    BYTE *px = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)stride * h);
    HDC dc = GetDC(NULL);
    if (!px || !dc) { if (px) HeapFree(GetProcessHeap(), 0, px); if (dc) ReleaseDC(NULL, dc); return 0; }
    memset(&bi, 0, sizeof(bi));
    bi.biSize = sizeof(bi); bi.biWidth = w; bi.biHeight = h; bi.biPlanes = 1; bi.biBitCount = 24; bi.biCompression = BI_RGB;
    if (GetDIBits(dc, bm, 0, h, px, (BITMAPINFO *)&bi, DIB_RGB_COLORS) == h) {
        HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD n;
            memset(&bf, 0, sizeof(bf));
            bf.bfType = 0x4d42; bf.bfOffBits = sizeof(bf) + sizeof(bi); bf.bfSize = bf.bfOffBits + (DWORD)stride * h;
            ok = WriteFile(f, &bf, sizeof(bf), &n, NULL) && WriteFile(f, &bi, sizeof(bi), &n, NULL) &&
                 WriteFile(f, px, (DWORD)stride * h, &n, NULL);
            CloseHandle(f);
        }
    }
    HeapFree(GetProcessHeap(), 0, px);
    ReleaseDC(NULL, dc);
    return ok;
}

static void shot_take(int on_box_thread)
{
    MONITORINFO mi;
    HWND g = NULL, boxes[16];
    char dir[MAX_PATH], path[MAX_PATH];
    SYSTEMTIME t;
    BOOL (WINAPI *pw)(HWND, HDC, UINT) = (BOOL (WINAPI *)(HWND, HDC, UINT))GetProcAddress(GetModuleHandleA("user32.dll"), "PrintWindow");
    EnumWindows(dds_game_win, (LPARAM)&g);
    memset(&mi, 0, sizeof(mi)); mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromWindow(g ? g : GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    if (!mon || !GetMonitorInfoA(mon, &mi)) SetRect(&mi.rcMonitor, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    int w = mi.rcMonitor.right - mi.rcMonitor.left, h = mi.rcMonitor.bottom - mi.rcMonitor.top, nb = 0, ok = 0;
    int kept = on_box_thread && dd_wrap_now && dd_snap;
    if (kept) { w = dd_snap_w; h = dd_snap_h; }
    okp_log(3, "screenshot: %dx%d, the game's picture %s", w, h, kept ? "kept when the box opened" : "read from the screen now");
    HDC sdc = GetDC(NULL);
    HDC mdc = sdc ? CreateCompatibleDC(sdc) : NULL;
    HBITMAP bm = sdc ? CreateCompatibleBitmap(sdc, w, h) : NULL;
    if (mdc && bm) {
        HGDIOBJ was = SelectObject(mdc, bm);
        if (kept) {
            EnterCriticalSection(&dd_snap_cs);
            HDC kdc = dd_snap ? CreateCompatibleDC(sdc) : NULL;
            HGDIOBJ kwas = kdc ? SelectObject(kdc, dd_snap) : NULL;
            if (kdc) { BitBlt(mdc, 0, 0, w, h, kdc, 0, 0, SRCCOPY); SelectObject(kdc, kwas); DeleteDC(kdc); }
            LeaveCriticalSection(&dd_snap_cs);
        }
        else BitBlt(mdc, 0, 0, w, h, sdc, mi.rcMonitor.left, mi.rcMonitor.top, SRCCOPY);
        okp_log(3, "screenshot: game picture copied");
        memset(boxes, 0, sizeof(boxes));
        EnumWindows(shot_box_win, (LPARAM)boxes);
        for (int i = 14; i >= 0; i--) {   /* bottom first, so the top box ends on top */
            RECT r;
            if (!boxes[i] || !pw || !GetWindowRect(boxes[i], &r)) continue;
            int bw = r.right - r.left, bh = r.bottom - r.top;
            HDC bdc = CreateCompatibleDC(sdc);
            HBITMAP bb = bdc ? CreateCompatibleBitmap(sdc, bw, bh) : NULL;
            if (bb) {
                HGDIOBJ bwas = SelectObject(bdc, bb);
                okp_log(3, "screenshot: drawing box %p (%dx%d)", (void *)boxes[i], bw, bh);
                if (pw(boxes[i], bdc, 0)) {
                    BitBlt(mdc, r.left - mi.rcMonitor.left, r.top - mi.rcMonitor.top, bw, bh, bdc, 0, 0, SRCCOPY);
                    nb++;
                }
                SelectObject(bdc, bwas);
                DeleteObject(bb);
            }
            if (bdc) DeleteDC(bdc);
        }
        SelectObject(mdc, was);
        okp_log(3, "screenshot: saving");
        snprintf(dir, sizeof(dir), "%sokplay-screenshots", okp_dll_dir());
        CreateDirectoryA(dir, NULL);
        GetLocalTime(&t);
        snprintf(path, sizeof(path), "%s\\okplay-%04u%02u%02u-%02u%02u%02u-%03u.bmp", dir, t.wYear, t.wMonth, t.wDay,
                 t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        ok = shot_save(bm, w, h, path);
    }
    if (bm) DeleteObject(bm);
    if (mdc) DeleteDC(mdc);
    if (sdc) ReleaseDC(NULL, sdc);
    if (ok) okp_log(1, "screenshot: %s (%dx%d, %d okplay box(es) drawn on top)", path, w, h, nb);
    else okp_log(1, "screenshot: could not be taken (error %lu)", (unsigned long)GetLastError());
}

static UINT shot_mods, shot_vk;

/* The box's message filter (wsock.c box_filter), for every key going to a box: 1 = it was the screenshot key, taken
 * here on the box's own thread.  No thread or hotkey of our own: 0.2.10/0.2.11 started one at game start for the key,
 * and over DDrawCompat the box was then never seen (laptop, 2026-11-16) - so the key works while a box is open. */
int dds_shot_key(const MSG *m)
{
    if (!shot_vk || (m->message != WM_KEYDOWN && m->message != WM_SYSKEYDOWN) || m->wParam != shot_vk) return 0;
    UINT mods = ((GetKeyState(VK_CONTROL) & 0x8000) ? MOD_CONTROL : 0) | ((GetKeyState(VK_MENU) & 0x8000) ? MOD_ALT : 0) |
                ((GetKeyState(VK_SHIFT) & 0x8000) ? MOD_SHIFT : 0);
    if (mods != shot_mods) return 0;
    if (m->lParam & 0x40000000) return 1;   /* (held down: one picture per press) */
    okp_log(2, "screenshot: key pressed in a box");
    shot_take(1);
    return 1;
}

void dds_shot_start(const char *key)   /* "off", or modifiers + a key: ctrl+f12, alt+shift+s, f11 */
{
    char k[32];
    UINT mods = 0, vk = 0;
    if (!key || !_stricmp(key, "off") || !key[0]) { okp_log(2, "screenshot: off (okplay.ini wsock_screenshot_key=off)"); return; }
    lstrcpynA(k, key, sizeof(k));
    CharLowerA(k);
    for (char *p = k; p && *p; ) {
        char *e = strchr(p, '+');
        if (e) *e = 0;
        if (!strcmp(p, "ctrl")) mods |= MOD_CONTROL;
        else if (!strcmp(p, "alt")) mods |= MOD_ALT;
        else if (!strcmp(p, "shift")) mods |= MOD_SHIFT;
        else if (p[0] == 'f' && atoi(p + 1) >= 1 && atoi(p + 1) <= 24) vk = VK_F1 + atoi(p + 1) - 1;
        else if (!p[1] && ((p[0] >= 'a' && p[0] <= 'z') || (p[0] >= '0' && p[0] <= '9'))) vk = (UINT)toupper((unsigned char)p[0]);
        p = e ? e + 1 : NULL;
    }
    if (!vk) { okp_log(1, "screenshot: key \"%s\" not understood (okplay.ini wsock_screenshot_key) - off", key); return; }
    shot_mods = mods; shot_vk = vk;
    okp_log(2, "screenshot: %s while an okplay box is open saves the game with the box to okplay-screenshots (okplay.ini wsock_screenshot_key)", key);
}

