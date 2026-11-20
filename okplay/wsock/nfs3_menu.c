/* okplay wsock - Need for Speed III: okplay's choice in the game's own Connection menu.  [game: NFS3]
 *
 * Instead of okplay's Windows box ("Online Play / LAN Play / Original"), the game's own Network menu offers the
 * three: "Oldkid.net Online", "Oldkid.net LAN", and the game's own "TCP/IP" (direct, okplay stays out). The game
 * draws them, in its own style, and the choice is known before any socket exists - nothing is asked inside a
 * network call (docs/OVERLAY.md: why).
 *
 * How (learned by studying the game; nothing of it is copied here - only addresses and what they mean; the study
 * notes are private, not in this repo):
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
    uint32_t lobby_entry, lobby_name, lobby_idle;   /* the lobby menu (mainmult): its handler entry, "mainmult", its idle */
    uint32_t pop_choice, pop_notice, pop_text;      /* the game's popups: choice, notice, text entry (open) */
    uint32_t pop_active, pop_button, pop_edit;      /* is one open (al), the button chosen, the text entry's buffer */
    uint32_t pop_cbvar, pop_btnvar;                 /* the open popup's callback, its chosen button (variables) */
    uint32_t tcp_choice_cb, join_cb;                /* the "TCP Connection" box's callback; its Join box's callback */
    uint32_t tcp_btns, tcp_help;                    /* that box's 4 button texts and help lines (char *[]) */
} nm_build;

static const nm_build nm_builds[] = {
    { "1.6.x (Modern Patch 1.6.1, portable 1.6.2 Beta)", 0x555f00, 0x537d1c, 0x4405c0, 0x440730, 0x442a40, 0x43fe50,
      0x555ed0, 0x537d04, 0x4403f0, 0x4446d0, 0x444a80, 0x444c20, 0x4438b0, 0x4446c0, 0x444c10,
      0x558b18, 0x558b3c, 0x43f760, 0x43f680, 0x5f4064, 0x5f4074 },
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
uint32_t nm_orig_lobby_idle, nm_f_choice, nm_f_notice, nm_f_text, nm_f_active, nm_f_button, nm_f_edit;
void __cdecl nm_idle(uint32_t menu, int lobby);
int __cdecl nm_popup_cb(int result, uint8_t *close);
void nm_idle_connect_thunk(void);
void nm_idle_lobby_thunk(void);
void nm_popup_cb_thunk(void);
void __cdecl nm_open_choice(int ntitle, char **titles, int nbtn, char **btns, char **help, int def, int layout, void *cb);
void __cdecl nm_open_text(int ntitle, char **titles, int nbtn, char **btns, int def, char *buf, int maxlen, int charset, void *cb);
void __cdecl nm_open_notice(int n, char **lines, int timeout, int keys, void *cb);
uint32_t __cdecl nm_call0(uint32_t fn);   /* a game function with no arguments: its eax */
uint32_t nm_tcp_choice_orig;
void nm_tcp_choice_thunk(void);
void __cdecl nm_tcp_choice_pre(void);
uint32_t __cdecl nm_after_tcp_choice(uint32_t action, int result, uint8_t *close);
void __cdecl nm_after_tcp(void);
uint32_t __cdecl nm_call_cb(uint32_t fn, int result, uint8_t *close);   /* a popup callback of the game's: its action */
__asm__(
    ".text\n"
    ".globl _nm_idle_connect_thunk\n"
    "_nm_idle_connect_thunk:\n"     /* the connect menu had no idle handler: okplay's, nothing to chain */
    "  pushl %ecx\n  pushl %edx\n"
    "  pushl $0\n  pushl %eax\n  call _nm_idle\n  addl $8, %esp\n"
    "  xorl %eax, %eax\n"
    "  popl %edx\n  popl %ecx\n  ret\n"
    ".globl _nm_idle_lobby_thunk\n"
    "_nm_idle_lobby_thunk:\n"       /* the lobby's own idle first (eax = menu as given), then okplay's */
    "  pushl %ecx\n  pushl %edx\n  pushl %eax\n"
    "  call *_nm_orig_lobby_idle\n"
    "  movl (%esp), %ecx\n"
    "  pushl %eax\n"
    "  pushl $1\n  pushl %ecx\n  call _nm_idle\n  addl $8, %esp\n"
    "  popl %eax\n  addl $4, %esp\n"
    "  popl %edx\n  popl %ecx\n  ret\n"
    ".globl _nm_popup_cb_thunk\n"
    "_nm_popup_cb_thunk:\n"         /* a popup's callback: eax = result, edx = the close byte; eax back = menu action */
    "  pushl %ebx\n  pushl %ecx\n  pushl %edx\n  pushl %esi\n  pushl %edi\n  pushl %ebp\n"
    "  pushl %edx\n  pushl %eax\n  call _nm_popup_cb\n  addl $8, %esp\n"
    "  popl %ebp\n  popl %edi\n  popl %esi\n  popl %edx\n  popl %ecx\n  popl %ebx\n  ret\n"
    ".globl _nm_open_choice\n"
    "_nm_open_choice:\n"            /* (ntitle, titles, nbtn, btns, help, def, layout, cb) */
    "  pushl %ebx\n  pushl %esi\n  pushl %edi\n  pushl %ebp\n"
    "  movl 48(%esp), %eax\n  pushl %eax\n"
    "  movl 48(%esp), %eax\n  pushl %eax\n"
    "  movl 48(%esp), %eax\n  pushl %eax\n"
    "  movl 48(%esp), %eax\n  pushl %eax\n"
    "  movl 36(%esp), %eax\n  movl 40(%esp), %edx\n  movl 44(%esp), %ebx\n  movl 48(%esp), %ecx\n"
    "  call *_nm_f_choice\n"
    "  popl %ebp\n  popl %edi\n  popl %esi\n  popl %ebx\n  ret\n"
    ".globl _nm_open_text\n"
    "_nm_open_text:\n"              /* (ntitle, titles, nbtn, btns, def, buf, maxlen, charset, cb) */
    "  pushl %ebx\n  pushl %esi\n  pushl %edi\n  pushl %ebp\n"
    "  movl 52(%esp), %eax\n  pushl %eax\n"
    "  movl 52(%esp), %eax\n  pushl %eax\n"
    "  movl 52(%esp), %eax\n  pushl %eax\n"
    "  movl 52(%esp), %eax\n  pushl %eax\n"
    "  movl 52(%esp), %eax\n  pushl %eax\n"
    "  movl 40(%esp), %eax\n  movl 44(%esp), %edx\n  movl 48(%esp), %ebx\n  movl 52(%esp), %ecx\n"
    "  call *_nm_f_text\n"
    "  popl %ebp\n  popl %edi\n  popl %esi\n  popl %ebx\n  ret\n"
    ".globl _nm_open_notice\n"
    "_nm_open_notice:\n"            /* (n, lines, timeout, keys, cb) */
    "  pushl %ebx\n  pushl %esi\n  pushl %edi\n  pushl %ebp\n"
    "  movl 36(%esp), %eax\n  pushl %eax\n"
    "  movl 24(%esp), %eax\n  movl 28(%esp), %edx\n  movl 32(%esp), %ebx\n  movl 36(%esp), %ecx\n"
    "  call *_nm_f_notice\n"
    "  popl %ebp\n  popl %edi\n  popl %esi\n  popl %ebx\n  ret\n"
    ".globl _nm_tcp_choice_thunk\n"
    "_nm_tcp_choice_thunk:\n"      /* the TCP box's callback: the game's own first, then okplay's look at what it opened */
    "  pushl %ecx\n  pushl %edx\n  pushl %eax\n"
    "  call _nm_tcp_choice_pre\n"
    "  movl (%esp), %eax\n  movl 4(%esp), %edx\n  movl 8(%esp), %ecx\n"
    "  call *_nm_tcp_choice_orig\n"
    "  movl 4(%esp), %edx\n  movl (%esp), %ecx\n"
    "  pushl %edx\n  pushl %ecx\n  pushl %eax\n  call _nm_after_tcp_choice\n  addl $12, %esp\n"
    "  addl $4, %esp\n  popl %edx\n  popl %ecx\n  ret\n"
    ".globl _nm_call_cb\n"
    "_nm_call_cb:\n"               /* (fn, result, close): eax = result, edx = close */
    "  pushl %ebx\n  pushl %esi\n  pushl %edi\n  pushl %ebp\n"
    "  movl 20(%esp), %ecx\n  movl 24(%esp), %eax\n  movl 28(%esp), %edx\n  call *%ecx\n"
    "  popl %ebp\n  popl %edi\n  popl %esi\n  popl %ebx\n  ret\n"
    ".globl _nm_call0\n"
    "_nm_call0:\n"
    "  pushl %ebx\n  pushl %ecx\n  pushl %edx\n  pushl %esi\n  pushl %edi\n  pushl %ebp\n"
    "  movl 28(%esp), %eax\n  call *%eax\n"
    "  popl %ebp\n  popl %edi\n  popl %esi\n  popl %edx\n  popl %ecx\n  popl %ebx\n  ret\n");
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
    "  call *_nm_tcp_cb\n"       /* (it takes eax only, and keeps every register) */
    "  pushl %ecx\n  pushl %edx\n  pushl %eax\n"
    "  call _nm_after_tcp\n"
    "  popl %eax\n  popl %edx\n  popl %ecx\n"
    "  ret\n"
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

/* ---- okplay's questions and notices in the game's own popups (on the game's thread, from the menus' idle) ---- */
int okw_host_pw_pending(void);
void okw_host_pw_answer(const char *typed);
int okw_ui_take(char *a, int an, char *b, int bn);
void okw_ui_answer(int kind, int r);
void okw_set_ui_ingame(int on);

enum { NM_NONE, NM_HOSTPW, NM_ASK7, NM_ASK8, NM_ROOMS, NM_JOINPW };
static int nm_cur;                       /* what okplay's open popup asks (its callback says what to do) */
static char nm_l[6][96], *nm_lines[6], *nm_btns[3], nm_b[3][24], nm_buf[64];

static int nm_lines_set(const char *a, const char *b, const char *c, const char *d)
{
    const char *in[4] = { a, b, c, d };
    int n = 0;
    for (int i = 0; i < 4; i++) if (in[i] && in[i][0]) { lstrcpynA(nm_l[n], in[i], sizeof(nm_l[n])); nm_lines[n] = nm_l[n]; n++; }
    return n;
}

/* ---- joining: the game list in the game's own popup (instead of the game's Join box with an address) ---- */
typedef struct { uint64_t host; uint32_t room; int locked, curp, maxp; char name[64]; } okw_room_brief;
int okw_rooms(okw_room_brief *out, int max);
void okw_join_pick(uint64_t host, uint32_t room, const char *typed);

#define NM_MAXROOMS 7
static okw_room_brief nm_room[NM_MAXROOMS];
static int nm_nroom, nm_pick_i;
static uint32_t nm_list_sig;
static DWORD nm_list_t;
static char nm_rb[NM_MAXROOMS + 2][64], nm_rh[NM_MAXROOMS + 2][64], *nm_rbtn[NM_MAXROOMS + 2], *nm_rhelp[NM_MAXROOMS + 2];

static uint32_t rooms_sig(const okw_room_brief *r, int n)
{
    uint32_t h = 2166136261u ^ (uint32_t)n;
    for (int i = 0; i < n; i++) { h = (h ^ (uint32_t)r[i].host ^ (uint32_t)(r[i].host >> 32)) * 16777619u; h = (h ^ (uint32_t)(r[i].curp << 8 | r[i].maxp << 16 | r[i].locked)) * 16777619u; }
    return h;
}

static void nm_show_rooms(void)   /* (re)opens okplay's game list popup */
{
    int n = okw_rooms(nm_room, NM_MAXROOMS);
    if (n < 0) n = 0;
    nm_nroom = n;
    nm_list_sig = rooms_sig(nm_room, n);
    nm_list_t = GetTickCount();
    int lines = nm_mode == 3 ? nm_lines_set("Oldkid.net LAN - games on this network:", n ? NULL : "Looking for games...", NULL, NULL)
                             : nm_lines_set("Oldkid.net Online - pick a game to join:", n ? NULL : "Looking for games...", NULL, NULL);
    int b = 0;
    for (int i = 0; i < n; i++, b++) {
        snprintf(nm_rb[b], sizeof(nm_rb[b]), "%s", nm_room[i].name);
        for (char *c = nm_rb[b]; *c; c++) if ((unsigned char)*c < 0x20) *c = ' ';
        if (nm_room[i].maxp > 0)
            snprintf(nm_rh[b], sizeof(nm_rh[b]), "%d/%d players%s%s", nm_room[i].curp, nm_room[i].maxp,
                     nm_room[i].curp >= nm_room[i].maxp ? ", full" : "", nm_room[i].locked ? ", password" : "");
        else snprintf(nm_rh[b], sizeof(nm_rh[b]), "%s", nm_room[i].locked ? "Password" : "Open game");
    }
    lstrcpynA(nm_rb[b], "Refresh", sizeof(nm_rb[b])); lstrcpynA(nm_rh[b], "Look for games again", sizeof(nm_rh[b])); b++;
    lstrcpynA(nm_rb[b], "Cancel", sizeof(nm_rb[b])); nm_rh[b][0] = 0; b++;
    for (int i = 0; i < b; i++) { nm_rbtn[i] = nm_rb[i]; nm_rhelp[i] = nm_rh[i]; }
    nm_cur = NM_ROOMS;
    nm_open_choice(lines, nm_lines, b, nm_rbtn, nm_rhelp, 0, 1, (void *)nm_popup_cb_thunk);
}

void __cdecl nm_after_tcp(void)   /* the game's TCP box is open: okplay's callback goes in front of the game's */
{
    if (nm_mode != 2 && nm_mode != 3) return;
    uint32_t *cb = (uint32_t *)NB->pop_cbvar;
    if (*cb == NB->tcp_choice_cb) {   /* opened again, the same box with okplay's header and callback */
        static char head[2][24] = { "Oldkid.net Online", "Oldkid.net LAN" };
        static char *title[1];
        title[0] = head[nm_mode == 3];
        nm_tcp_choice_orig = NB->tcp_choice_cb;
        char **gb = (char **)NB->tcp_btns, **gh = (char **)NB->tcp_help;
        static char *btn[3], *help[3];   /* Host, Join, Cancel: no "Port" (okplay carries the game's connections) */
        btn[0] = gb[0]; btn[1] = gb[1]; btn[2] = gb[3];
        help[0] = gh[0]; help[1] = gh[1]; help[2] = gh[3];
        nm_open_choice(1, title, 3, btn, help, 0, 1, (void *)nm_tcp_choice_thunk);
    }
    else okp_log(1, "nfs3 menu: the TCP box is not as known (callback %08x) - the game's own Join box stays", (unsigned)*cb);
}

void __cdecl nm_tcp_choice_pre(void)   /* okplay's box has no Port: its 3rd button is the game's 4th (Cancel) */
{
    uint32_t *b = (uint32_t *)NB->pop_btnvar;
    if (*b == 2) *b = 3;
}

uint32_t __cdecl nm_after_tcp_choice(uint32_t action, int result, uint8_t *close)
{
    (void)result; (void)close;
    if ((nm_mode == 2 || nm_mode == 3) && (nm_call0(NB->pop_active) & 0xff) && *(uint32_t *)NB->pop_cbvar == NB->join_cb) {
        okp_log(1, "nfs3 menu: Join - the game list in the game's own popup instead of the address box");
        nm_show_rooms();   /* (opened over the game's Join box, as the game opened that over its TCP box) */
    }
    return action;
}

static uint32_t nm_join_now(uint8_t *close)   /* the game's own Join box, answered: the address okplay knows */
{
    char *edit = (char *)nm_call0(NB->pop_edit);
    lstrcpynA(edit, "oldkid.net", 30);
    *(uint32_t *)NB->pop_btnvar = 0;   /* Ok */
    nm_cur = NM_NONE;
    okp_log(1, "nfs3 menu: joining - the game's own connect follows");
    return nm_call_cb(NB->join_cb, 1, close);
}

void __cdecl nm_idle(uint32_t menu, int lobby)
{
    char a[200], b[200];
    (void)menu;
    int active = (int)(nm_call0(NB->pop_active) & 0xff);
    if (nm_cur != NM_NONE && !active) {   /* okplay's popup went without an answer (menu left): asked again */
        okp_log(2, "nfs3 menu: okplay's popup %d closed without an answer - asked again", nm_cur);
        nm_cur = NM_NONE;
    }
    if (nm_cur == NM_ROOMS && active && GetTickCount() - nm_list_t > 1000) {   /* the list follows the rooms */
        okw_room_brief r[NM_MAXROOMS];
        int n = okw_rooms(r, NM_MAXROOMS);
        nm_list_t = GetTickCount();
        if (n >= 0 && rooms_sig(r, n) != nm_list_sig) nm_show_rooms();
        return;
    }
    if (nm_cur != NM_NONE || active) return;   /* one popup at a time */
    if (lobby && okw_host_pw_pending()) {
        int n = nm_lines_set("Password for your game:", "Leave it empty for an open game.", NULL, NULL);
        lstrcpynA(nm_b[0], "Ok", sizeof(nm_b[0])); lstrcpynA(nm_b[1], "No Password", sizeof(nm_b[1]));
        nm_btns[0] = nm_b[0]; nm_btns[1] = nm_b[1];
        nm_buf[0] = 0;
        nm_cur = NM_HOSTPW;
        nm_open_text(n, nm_lines, 2, nm_btns, 0, nm_buf, 20, 1, (void *)nm_popup_cb_thunk);
        okp_log(2, "nfs3 menu: the host password asked in the game's own popup");
        return;
    }
    int k = okw_ui_take(a, sizeof(a), b, sizeof(b));
    if (!k) return;
    if (k == 7 || k == 8) {
        int n = k == 8 ? nm_lines_set("Oldkid.net Online Play: send anonymous", "connection reports? They help fix problems.", NULL, NULL)
                       : nm_lines_set("Oldkid.net Online Play: the game stopped last time.", "Send the crash report to help fix it?", NULL, NULL);
        lstrcpynA(nm_b[0], "Yes", sizeof(nm_b[0])); lstrcpynA(nm_b[1], "No", sizeof(nm_b[1]));
        nm_btns[0] = nm_b[0]; nm_btns[1] = nm_b[1];
        nm_cur = k == 8 ? NM_ASK8 : NM_ASK7;
        nm_open_choice(n, nm_lines, 2, nm_btns, NULL, 0, 1, (void *)nm_popup_cb_thunk);
        okp_log(2, "nfs3 menu: question %d asked in the game's own popup", k);
        return;
    }
    char t[120];
    int n;
    if (k == 4) { snprintf(t, sizeof(t), "A newer version (%s) is at oldkid.net.", a); n = nm_lines_set("Oldkid.net Online Play", t, NULL, NULL); }
    else if (k == 5) n = nm_lines_set("The Oldkid.net room server can't be reached.", "Online Play shows no rooms for now.", NULL, NULL);
    else if (k == 6) n = nm_lines_set("No network: Oldkid.net Online and LAN Play", "can't start on this PC.", NULL, NULL);
    else if (k == 3) n = nm_lines_set("Oldkid.net Online Play:", "Wrong password - the host said no.", NULL, NULL);
    else if (k == 12) n = nm_lines_set("Oldkid.net Online Play:", "That game is full.", NULL, NULL);
    else if (k == 11) n = nm_lines_set("Oldkid.net Online Play:", "No answer from the game's host.", NULL, NULL);
    else { okw_ui_answer(k, 0); return; }
    nm_open_notice(n, nm_lines, 0x226, 1, NULL);
    okw_ui_answer(k, 1);
    okp_log(2, "nfs3 menu: notice %d shown in the game's own popup", k);
}

int __cdecl nm_popup_cb(int result, uint8_t *close)
{
    int cur = nm_cur, btn = (int)nm_call0(NB->pop_button);
    nm_cur = NM_NONE;
    (void)close;   /* (left as the game set it: the popup closes) */
    if (cur == NM_HOSTPW) {
        const char *typed = (const char *)nm_call0(NB->pop_edit);
        okw_host_pw_answer(result == 1 && btn == 0 && typed ? typed : NULL);
    } else if (cur == NM_ASK7 || cur == NM_ASK8) {
        okw_ui_answer(cur == NM_ASK8 ? 8 : 7, result == 1 && btn == 0 ? 1 : 0);
    } else if (cur == NM_ROOMS) {
        if (result != 1 || btn == nm_nroom + 1) { okp_log(1, "nfs3 menu: the game list was cancelled"); return 0; }
        *close = 0;   /* (another popup of okplay's follows: kept open, as the game does) */
        if (btn < 0 || btn >= nm_nroom) { nm_show_rooms(); return 0; }   /* Refresh */
        nm_pick_i = btn;
        if (nm_room[btn].locked) {
            char t[96];
            snprintf(t, sizeof(t), "Password for %s:", nm_room[btn].name);
            int n = nm_lines_set(t, NULL, NULL, NULL);
            lstrcpynA(nm_b[0], "Ok", sizeof(nm_b[0])); lstrcpynA(nm_b[1], "Back", sizeof(nm_b[1]));
            nm_btns[0] = nm_b[0]; nm_btns[1] = nm_b[1];
            nm_buf[0] = 0;
            nm_cur = NM_JOINPW;
            nm_open_text(n, nm_lines, 2, nm_btns, 0, nm_buf, 20, 1, (void *)nm_popup_cb_thunk);
            return 0;
        }
        okw_join_pick(nm_room[btn].host, nm_room[btn].room, NULL);
        return nm_join_now(close);
    } else if (cur == NM_JOINPW) {
        const char *typed = (const char *)nm_call0(NB->pop_edit);
        if (result != 1 || btn != 0 || !typed) { *close = 0; nm_show_rooms(); return 0; }   /* Back: the list again */
        okw_join_pick(nm_room[nm_pick_i].host, nm_room[nm_pick_i].room, typed);
        return nm_join_now(close);
    }
    return 0;
}

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
    /* the popups (step 2): the connect menu's idle (it has none) and the lobby's (chained) - only if the lobby entry is as known */
    uint32_t *lob = (uint32_t *)NB->lobby_entry, *cidle = (uint32_t *)(NB->hentry + 20);
    if (readable(NB->lobby_entry, 24) && readable(NB->lobby_name, 9) && lob[0] == NB->lobby_name && !strcmp((const char *)NB->lobby_name, "mainmult")
        && lob[5] == NB->lobby_idle && *cidle == 0) {
        nm_orig_lobby_idle = NB->lobby_idle;
        nm_f_choice = NB->pop_choice; nm_f_notice = NB->pop_notice; nm_f_text = NB->pop_text;
        nm_f_active = NB->pop_active; nm_f_button = NB->pop_button; nm_f_edit = NB->pop_edit;
        if (VirtualProtect(cidle, 4, PAGE_READWRITE, &old)) { *cidle = (uint32_t)nm_idle_connect_thunk; VirtualProtect(cidle, 4, old, &old); }
        if (VirtualProtect(&lob[5], 4, PAGE_READWRITE, &old)) { lob[5] = (uint32_t)nm_idle_lobby_thunk; VirtualProtect(&lob[5], 4, old, &old); }
        okw_set_ui_ingame(1);
        okp_log(1, "nfs3 menu: okplay's questions and notices use the game's own popups (host password, reports, notices)");
    } else okp_log(1, "nfs3 menu: the lobby is not as known - okplay's questions stay Windows boxes");
    okp_log(1, "nfs3 menu: on (%s) - Online / LAN / TCP/IP are chosen in the game's own Network menu (okplay.ini nfs3_menu=0: the Windows box)", NB->name);
}
