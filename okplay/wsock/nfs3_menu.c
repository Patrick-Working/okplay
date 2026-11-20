/* okplay wsock - Need for Speed III: okplay's choice in the game's own Connection menu.  [game: NFS3]
 *
 * Instead of okplay's Windows box ("Online Play / LAN Play / Original"), the game's own Network menu offers the
 * three: "Oldkid.net Online", "Oldkid.net LAN", and the game's own "TCP/IP" (direct, okplay stays out). The game
 * draws them, in its own style, and the choice is known before any socket exists - nothing is asked inside a
 * network call (docs/OVERLAY.md: why).
 *
 * How (learned by studying the game; nothing of it is copied here - only addresses and what they mean; private
 * study notes C:\nfs3lab\notes\NFS3_MENUS.md):
 *  - The menus are text files (fedata\menus\*.mnu) the game reads with CreateFileA at front-end start. okplay hooks
 *    the exe's CreateFileA import and hands the game an edited copy of connect.mnu, made at run time from the
 *    player's own file: the one "tcp" tab becomes three ("tcp okp_online", "tcp okp_lan", "tcp okp_direct"), the
 *    tabs below move down two places. The labels are texts okplay puts into three unused entries of the game's text
 *    file (fedata\text\text.<lang>, also handed over as an edited copy). Nothing in the game folder changes.
 *  - A menu's behaviour comes from a handler table in the exe (per menu: enter, leave, idle). The connect menu's
 *    enter handler gives the game's TCP callback to the FIRST element tagged "tcp" only; okplay chains that handler
 *    (one pointer in the table, written at load) and then gives okplay's own callback to all three, which notes the
 *    choice and goes on into the game's TCP callback (the game's "TCP Connection: Host / Join" box follows).
 *  - The game's leave handler already sets the link type for any pressed element tagged "tcp".
 *  - The game's functions use Watcom's register calls (eax, edx, ...; every register but eax kept): small thunks.
 * Builds: the 1.6.x exes (Modern Patch 1.6.1, portable 1.6.2 Beta) share every address used here; the build is
 * recognised by the game's own data at those addresses (the handler table entry pointing at "connect" with the
 * known enter/leave handlers), not by code bytes. Anything else: the Windows box as before. okplay.ini
 * nfs3_menu=0 turns this off. */
#include "okp.h"
#include "nfs3_menu.h"
#include "ddshow.h"
#include <string.h>
#include <stdlib.h>

typedef struct {
    const char *name;
    uint32_t hentry;       /* the connect menu's entry in the handler table: {name, -, -, enter, leave, idle} */
    uint32_t name_str;     /* "connect" */
    uint32_t enter, leave; /* its enter / leave handlers */
    uint32_t find;         /* element of a menu by codelink token: eax = menu, edx = token -> eax = element or 0 */
    uint32_t tcp_cb;       /* the game's TCP callback (its "TCP Connection" box): eax = element */
} nm_build;

static const nm_build nm_builds[] = {
    { "1.6.x (Modern Patch 1.6.1, portable 1.6.2 Beta)", 0x555f00, 0x537d1c, 0x4405c0, 0x440730, 0x442a40, 0x43fe50 },
};

/* text entries okplay uses (unused by the game's menus and code in the 1.6.x builds) */
#define T_ONLINE 1718
#define T_LAN    1719
#define T_HONL   1720
#define T_HLAN   1721

static const nm_build *NB;
static int nm_on = 1;
static volatile LONG nm_mode;              /* 0 = nothing picked; 2 = Online, 3 = LAN, 1 = direct (the game's own) */
static uint32_t el_online, el_lan, el_direct;
static HANDLE (WINAPI *real_cfa)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static char served_mnu[MAX_PATH], served_txt[MAX_PATH];

/* ---- Watcom register-call thunks (the game calls these; they call the game) ---- */
uint32_t nm_orig_enter, nm_find_fn, nm_tcp_cb;
void __cdecl nm_after_enter(uint32_t menu);
void __cdecl nm_pick(uint32_t elem);
void nm_enter_thunk(void);
void nm_pick_thunk(void);
uint32_t __cdecl nm_find(uint32_t menu, const char *token);
__asm__(
    ".text\n"
    ".globl _nm_enter_thunk\n"
    "_nm_enter_thunk:\n"          /* eax = the menu; keep every register but eax */
    "  pushl %ecx\n"
    "  pushl %edx\n"
    "  pushl %eax\n"
    "  call *_nm_orig_enter\n"    /* the game's own enter handler (eax = menu, unchanged) */
    "  popl %ecx\n"
    "  pushl %eax\n"
    "  pushl %ecx\n"
    "  call _nm_after_enter\n"
    "  addl $4, %esp\n"
    "  popl %eax\n"
    "  popl %edx\n"
    "  popl %ecx\n"
    "  ret\n"
    ".globl _nm_pick_thunk\n"
    "_nm_pick_thunk:\n"           /* eax = the pressed element; then into the game's TCP callback as it was called */
    "  pushl %ecx\n"
    "  pushl %edx\n"
    "  pushl %eax\n"
    "  call _nm_pick\n"
    "  popl %eax\n"
    "  popl %edx\n"
    "  popl %ecx\n"
    "  jmp *_nm_tcp_cb\n"
    ".globl _nm_find\n"
    "_nm_find:\n"                 /* cdecl (menu, token) -> the game's finder (eax, edx) */
    "  pushl %ebx\n"
    "  pushl %ecx\n"
    "  pushl %edx\n"
    "  movl 16(%esp), %eax\n"
    "  movl 20(%esp), %edx\n"
    "  call *_nm_find_fn\n"
    "  popl %edx\n"
    "  popl %ecx\n"
    "  popl %ebx\n"
    "  ret\n");

void __cdecl nm_after_enter(uint32_t menu)
{
    uint32_t tcp = nm_find(menu, "tcp");
    uint32_t *e[3] = { &el_online, &el_lan, &el_direct };
    static const char *tok[3] = { "okp_online", "okp_lan", "okp_direct" };
    int n = 0;
    for (int i = 0; i < 3; i++) {
        uint32_t el = nm_find(menu, tok[i]);
        *e[i] = el;
        if (!el) continue;
        *(uint32_t *)(el + 0x64) = (uint32_t)nm_pick_thunk;   /* the element's action */
        if (tcp) *(uint8_t *)(el + 4) = (uint8_t)((*(uint8_t *)(el + 4) & ~1) | (*(uint8_t *)(tcp + 4) & 1));   /* greyed as TCP/IP */
        n++;
    }
    static int told;
    if (!told++) okp_log(1, "nfs3 menu: the Network menu has okplay's %d entries%s", n, n ? "" : " - none found (served menu not read?)");
}

void __cdecl nm_pick(uint32_t elem)
{
    int m = elem == el_online ? 2 : elem == el_lan ? 3 : elem == el_direct ? 1 : 0;
    if (!m) return;
    InterlockedExchange(&nm_mode, m);
    okp_log(1, "nfs3 menu: the player picked %s in the game's Network menu", m == 2 ? "Oldkid.net Online" : m == 3 ? "Oldkid.net LAN" : "TCP/IP (the game's own)");
}

int nfs3_menu_mode(void) { return nm_on && NB ? (int)nm_mode : 0; }

/* ---- the served files ---- */
static char *read_all(const char *path, DWORD *n)
{
    HANDLE h = real_cfa(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD size = GetFileSize(h, NULL), got = 0;
    char *b = (size != INVALID_FILE_SIZE && size < 4u << 20) ? (char *)malloc(size + 1) : NULL;
    if (b && (!ReadFile(h, b, size, &got, NULL) || got != size)) { free(b); b = NULL; }
    CloseHandle(h);
    if (b) { b[size] = 0; *n = size; }
    return b;
}

static int write_all(const char *path, const char *b, DWORD n)
{
    HANDLE h = real_cfa(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD put = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = WriteFile(h, b, n, &put, NULL) && put == n;
    CloseHandle(h);
    return ok;
}

/* connect.mnu: the [button] section with "codelink=tcp" becomes three (Online, LAN, then the game's own TCP/IP);
 * the tab buttons after it move down two places.  Line by line: a section runs from a "[...]" line to the next. */
#define NM_MAXL 400
static int make_mnu(const char *src, const char *dst)
{
    DWORD n = 0;
    char *in = read_all(src, &n);
    if (!in) return 0;
    char *line[NM_MAXL];
    int nl = 0;
    for (char *p = in; *p && nl < NM_MAXL; ) {   /* split into lines, CR/LF removed */
        char *e = p + strcspn(p, "\r\n");
        char c = *e;
        *e = 0;
        line[nl++] = p;
        p = c ? e + 1 : e;
        if (c == '\r' && *p == '\n') p++;
    }
    char *out = (char *)malloc(n + 4096), *q = out;
    int done = 0;
    if (!out) { free(in); return 0; }
    for (int a = 0; a < nl; ) {
        int b = a + 1;
        while (b < nl && line[b][0] != '[') b++;          /* section [a, b) */
        int button = !strcmp(line[a], "[button]"), tab = 0, tcp = 0, y = -1;
        for (int k = a + 1; k < b; k++) {
            if (!strcmp(line[k], "buttontype=tab")) tab = 1;
            if (!strcmp(line[k], "codelink=tcp")) tcp = 1;
            if (!strncmp(line[k], "ypos=", 5)) y = atoi(line[k] + 5);
        }
        if (button && tab && tcp && !done && y >= 0) {
            q += sprintf(q, "[button]\r\nbuttontype=tab\r\nypos=%d\r\npriority=6\r\nhelp=str_%d\r\ncodelink=tcp okp_online\r\ntext=str_%d\r\nnextmenu=mainmult\r\n\r\n", y, T_HONL, T_ONLINE);
            q += sprintf(q, "[button]\r\nbuttontype=tab\r\nypos=%d\r\npriority=6\r\nhelp=str_%d\r\ncodelink=tcp okp_lan\r\ntext=str_%d\r\nnextmenu=mainmult\r\n\r\n", y + 1, T_HLAN, T_LAN);
        }
        for (int k = a; k < b; k++) {
            if (button && tab && (done || tcp) && y >= 0 && !strncmp(line[k], "ypos=", 5)) q += sprintf(q, "ypos=%d\r\n", y + 2);
            else if (button && tab && tcp && !done && !strcmp(line[k], "codelink=tcp")) q += sprintf(q, "codelink=tcp okp_direct\r\n");
            else q += sprintf(q, "%s\r\n", line[k]);
        }
        if (button && tab && tcp && !done && y >= 0) done = 1;
        a = b;
    }
    int ok = done && write_all(dst, out, (DWORD)(q - out));
    free(in); free(out);
    return ok;
}

/* text.<lang>: uint32 offsets (the first = the table's size) + NUL-ended strings; okplay's four texts go in */
static int make_txt(const char *src, const char *dst)
{
    static const char *mine[4] = { "Oldkid.net Online", "Oldkid.net LAN",
        "Play over the Internet with Oldkid.net Online Play", "Play on this network with Oldkid.net Online Play" };
    DWORD n = 0;
    char *in = read_all(src, &n);
    if (!in || n < 8) { free(in); return 0; }
    uint32_t cnt = *(uint32_t *)in / 4;
    if (cnt <= T_HLAN || cnt * 4 > n) { free(in); return 0; }
    char *out = (char *)malloc(n + 512);
    if (!out) { free(in); return 0; }
    uint32_t *off = (uint32_t *)out, pos = cnt * 4;
    for (uint32_t i = 0; i < cnt; i++) {
        int own = i >= T_ONLINE && i <= T_HLAN;
        uint32_t o = ((uint32_t *)in)[i];
        if (!own && (o < cnt * 4 || o >= n || !memchr(in + o, 0, n - o))) { free(in); free(out); return 0; }   /* (a broken file: the game's own is used) */
        const char *s = own ? mine[i - T_ONLINE] : in + o;
        size_t l = strlen(s) + 1;
        if (pos + l > n + 512) { free(in); free(out); return 0; }
        off[i] = pos;
        memcpy(out + pos, s, l);
        pos += (uint32_t)l;
    }
    int ok = write_all(dst, out, pos);
    free(in); free(out);
    return ok;
}

static int ends_with(const char *s, const char *tail)
{
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && !_stricmp(s + a - b, tail);
}

static HANDLE WINAPI hk_cfa(LPCSTR path, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD fl, HANDLE tpl)
{
    if (path && nm_on && NB && !(acc & GENERIC_WRITE)) {
        const char *use = NULL;
        if (ends_with(path, "\\menus\\connect.mnu")) {
            if (!served_mnu[0]) {
                char d[MAX_PATH];
                snprintf(d, sizeof(d), "%sokplay-nfs3-connect.mnu", okp_log_dir());
                if (make_mnu(path, d)) lstrcpynA(served_mnu, d, sizeof(served_mnu));
                okp_log(1, "nfs3 menu: the game's Network menu %s", served_mnu[0] ? "gets okplay's entries (an edited copy, made from the game's own file)" : "could not be edited - the game's own is used");
            }
            use = served_mnu[0] ? served_mnu : NULL;
        } else if (strstr(path, "\\text\\text.") || strstr(path, "\\TEXT\\TEXT.")) {
            const char *dot = strrchr(path, '.');
            if (!served_txt[0] && dot && strlen(dot) <= 4) {
                char d[MAX_PATH];
                snprintf(d, sizeof(d), "%sokplay-nfs3-text%s", okp_log_dir(), dot);
                if (make_txt(path, d)) lstrcpynA(served_txt, d, sizeof(served_txt));
                okp_log(1, "nfs3 menu: the game's texts %s", served_txt[0] ? "get okplay's four (an edited copy)" : "could not be edited - the game's own are used");
            }
            use = served_txt[0] ? served_txt : NULL;
        }
        if (use) return real_cfa(use, acc, share, sa, disp, fl, tpl);
    }
    return real_cfa(path, acc, share, sa, disp, fl, tpl);
}

static int readable(uint32_t a, unsigned n)
{
    MEMORY_BASIC_INFORMATION mb;
    if (!VirtualQuery((void *)a, &mb, sizeof(mb)) || mb.State != MEM_COMMIT || (mb.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    return a + n <= (uint32_t)mb.BaseAddress + mb.RegionSize;
}

void nfs3_menu_init(const char *ini)
{
    nm_on = GetPrivateProfileIntA("okplay", "nfs3_menu", 1, ini) != 0;
    if (!nm_on) { okp_log(2, "nfs3 menu: off (okplay.ini nfs3_menu=0) - okplay's Windows box asks"); return; }
    if ((uint32_t)GetModuleHandleA(NULL) != 0x400000) return;
    for (unsigned i = 0; i < sizeof(nm_builds) / sizeof(nm_builds[0]); i++) {
        const nm_build *b = &nm_builds[i];
        if (!readable(b->hentry, 24) || !readable(b->name_str, 8)) continue;
        uint32_t *h = (uint32_t *)b->hentry;
        if (h[0] != b->name_str || strcmp((const char *)b->name_str, "connect") || h[3] != b->enter || h[4] != b->leave) continue;
        NB = b;
    }
    if (!NB) { okp_log(2, "nfs3 menu: not a build okplay's menu entries know - okplay's Windows box asks"); return; }
    nm_orig_enter = NB->enter; nm_find_fn = NB->find; nm_tcp_cb = NB->tcp_cb;
    if (!dds_hook_import(GetModuleHandleA(NULL), "kernel32.dll", "CreateFileA", (void *)hk_cfa, (void **)&real_cfa) || !real_cfa) {
        NB = NULL;
        okp_log(1, "nfs3 menu: the game's file opening could not be watched - okplay's Windows box asks");
        return;
    }
    DWORD old;
    uint32_t *slot = (uint32_t *)(NB->hentry + 12);
    if (VirtualProtect(slot, 4, PAGE_READWRITE, &old)) { *slot = (uint32_t)nm_enter_thunk; VirtualProtect(slot, 4, old, &old); }
    okp_log(1, "nfs3 menu: on (%s) - Online / LAN / TCP/IP are chosen in the game's own Network menu (okplay.ini nfs3_menu=0: the Windows box)", NB->name);
}
