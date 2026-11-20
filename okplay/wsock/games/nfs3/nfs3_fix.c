/* okplay wsock - Need for Speed III: what the game itself says about hosting.
 *
 * The game makes the same socket calls for "Host Game" and for entering its "Network (TCP)" menu again while an
 * earlier listener is still open (close the listener, restart Winsock, listen on the same port again), so the
 * socket calls alone can't tell them apart (wsock.c, x_listen). The game's own state can. Learned by studying the
 * game (nothing of it is copied here):
 *   - a DWORD "network game state": 0 = none, 3 = a network game is being set up or running;
 *   - a WORD "this PC is the server": 1 after the game's reset, 0 once it joins someone.
 *   Host Game resets, sets the state to 3 and the server flag to 1, and only then starts the TCP service (bind +
 *   listen). Entering the TCP menu starts the service without touching either. Joining sets the server flag to 0
 *   and the state to 3, and does not listen. Leaving a lobby resets (state 0, server 1; seen on 1.6.1) while the
 *   listener stays open until the next menu entry. Peer-to-Peer and Client-Server hosting show the same two values.
 * So at a listen on the game's port: state 3 and server flag set = Host Game; anything else = ready, not hosting.
 *
 * How many players the hosted game takes: right after the network game state the game keeps a DWORD "game type",
 * the host's choice in Host Game: 0 = Peer-to-Peer (2 players), 1 = Client-Server (up to 8), 2 = the game's old
 * internet service (not used with okplay). Learned by studying the game (nothing of it is copied here); the values
 * are still to be confirmed on the real game (the log shows the raw value at every listen and incoming connection).
 * nfs3_max_players gives 2 or 8 for those, 0 ("not known": okplay's own default) for anything else. Read only.
 *
 * The Join box ("Enter domain name or IP address"): the game keeps the address typed last in a text buffer of its
 * own (29 characters; the 1.6.x builds save it with the player's settings) and fills the box from it when it opens.
 * With okplay running (Online or LAN Play) the buffer is set to "oldkid.net", which wsock.c takes as "okplay's room
 * list" (never looked up): the player sees okplay is there and just presses Ok. What the game had there is kept and
 * put back if the game then runs with its original TCP/IP in the same run, so the game never connects to the real
 * oldkid.net (a later run that finds okplay's text there with the original TCP/IP empties it). okplay.ini
 * nfs3_join_prefill=0 turns it off. Data only: that one text buffer is written, never any code.
 *
 * The builds are recognised by their exe fingerprint (okp_exe_build); the addresses are absolute, for the exe loaded
 * at its usual base 0x400000 (anywhere else: not known). The table (nfs3_known.h) comes first; a build not in it,
 * or a field the table leaves 0, is looked up in the game itself (nfs3_find.c). Unknown builds the finder can't
 * read, or memory that can't be read: -1, and wsock.c keeps its listen-again rule. The values are only read, never
 * written (except the Join box text, above). */
#include "okp.h"
#include "nfs3_fix.h"
#include "nfs3_menu.h"
#define NFS3_KNOWN_FIX
#include "nfs3_known.h"
#include <string.h>

static nfs3_build found_b;   /* a build not in the table (or with fields the table leaves 0): what the finder found */

/* B: the build, once the exe's fingerprint is known (set by find_build's thread, once; never cleared: the exe
 * doesn't change while it runs). readable: both addresses were found readable once (set at the first good check,
 * never cleared: they are the exe's own data). */
static const nfs3_build *volatile B;
static volatile LONG readable, type_readable;
static int prefill_on = 1;   /* okplay.ini nfs3_join_prefill (read once, at load) */
#define PREFILL "oldkid.net"

/* The fingerprint is hashed in the background when okplay loads (okp_exe_build_early); asking for it waits until
 * that is done, which can take seconds on a slow PC. Asked from a thread of its own so the game never waits on it:
 * until then the build is "not known" (-1). */
static DWORD WINAPI find_build(LPVOID arg)
{
    char build[17] = "", exe[64] = "";
    const nfs3_build *b = NULL;
    (void)arg;
    okp_exe_build(build, exe, sizeof(exe));
    for (unsigned i = 0; i < sizeof(builds) / sizeof(builds[0]) && !b; i++)
        if (!strcmp(build, builds[i].build)) b = &builds[i];
    if (!b) {   /* not in the table: the game itself says where its state is */
        const nfs3_found *f = nfs3_find_running();
        if (!f) {
            okp_log(1, "nfs3: %s build %s is not one okplay knows, and its state was not found in it (%s) - Host Game is "
                    "told by the listen-again rule", exe, build[0] ? build : "?", nfs3_find_why());
            return 0;
        }
        found_b.build = "";
        found_b.name = "(not in okplay's table: found in the game)";
        found_b.net = f->fx.net;
        found_b.server = f->fx.server;
        found_b.join = f->join_len >= 29 ? f->fx.join : 0;   /* (prefill keeps 30 bytes) */
        found_b.pname = f->fx.pname;
        okp_log(1, "nfs3: %s build %s is not one okplay knows - its state was found in the game itself", exe, build[0] ? build : "?");
        okp_log(2, "nfs3: found { \"%s\", net 0x%x, server 0x%x, join 0x%x (%u chars), pname 0x%x } (for okplay's table)",
                build, f->fx.net, f->fx.server, f->fx.join, f->join_len, f->fx.pname);
        b = &found_b;
    } else if ((!b->join || !b->pname) && (uintptr_t)GetModuleHandleA(NULL) == 0x400000u) {   /* fields not pinned down */
        const nfs3_found *f = nfs3_find_running();
        if (f && f->fx.net == b->net && f->fx.server == b->server) {
            found_b = *b;
            if (!found_b.join && f->join_len >= 29) found_b.join = f->fx.join;
            if (!found_b.pname) found_b.pname = f->fx.pname;
            okp_log(1, "nfs3: %s - the Join box text (0x%x) and the player's name (0x%x) were found in the game itself", b->name,
                    found_b.join, found_b.pname);
            b = &found_b;
        } else okp_log(2, "nfs3: %s - the Join box text / player's name were not found in the game (%s)", b->name,
                       f ? "its state differs from okplay's table" : nfs3_find_why());
    }
    if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000u) {
        okp_log(1, "nfs3: %s (%s) is loaded at %p, not 0x400000 - Host Game is told by the listen-again rule", exe,
                b->name, (void *)GetModuleHandleA(NULL));
        return 0;
    }
    B = b;
    okp_log(1, "nfs3: Need for Speed III %s - Host Game is told from the game's own state", b->name);
    return 0;
}

int nfs3_init(const char *ini)
{
    DWORD id;
    HANDLE t;
    prefill_on = GetPrivateProfileIntA("okplay", "nfs3_join_prefill", 1, ini) != 0;
    nfs3_menu_init(ini);   /* okplay's entries in the game's own Network menu (nfs3_menu.c) - before its front end starts */
    t = CreateThread(NULL, 0, find_build, NULL, 0, &id);
    if (t) CloseHandle(t);
    return t != NULL;
}

static int can_read(uint32_t a, unsigned n)   /* 1 = [a, a+n) is committed, readable memory */
{
    MEMORY_BASIC_INFORMATION mi;
    const DWORD rd = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                     PAGE_EXECUTE_WRITECOPY;
    if (!VirtualQuery((LPCVOID)(uintptr_t)a, &mi, sizeof(mi))) return 0;
    if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(mi.Protect & rd)) return 0;
    return (uintptr_t)a + n <= (uintptr_t)mi.BaseAddress + mi.RegionSize;
}

int nfs3_hosting(int *net, int *server)
{
    const nfs3_build *b = B;
    if (!b) return -1;
    if (!readable) {
        if (!can_read(b->net, 4) || !can_read(b->server, 2)) return -1;
        InterlockedExchange(&readable, 1);
    }
    DWORD n = *(volatile DWORD *)(uintptr_t)b->net;
    WORD s = *(volatile WORD *)(uintptr_t)b->server;
    if (net) *net = (int)n;
    if (server) *server = (int)s;
    return n == 3 && s != 0;
}

int nfs3_max_players(int *type)
{
    const nfs3_build *b = B;
    if (type) *type = -1;
    if (!b) return 0;
    if (!type_readable) {
        if (!can_read(b->net + 4, 4)) return 0;
        InterlockedExchange(&type_readable, 1);
    }
    DWORD t = *(volatile DWORD *)(uintptr_t)(b->net + 4);
    if (type) *type = (int)t;
    return t == 0 ? 2 : t == 1 ? 8 : 0;
}

static int can_write(uint32_t a, unsigned n)   /* 1 = [a, a+n) is committed, writable memory */
{
    MEMORY_BASIC_INFORMATION mi;
    const DWORD wr = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!VirtualQuery((LPCVOID)(uintptr_t)a, &mi, sizeof(mi))) return 0;
    if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(mi.Protect & wr)) return 0;
    return (uintptr_t)a + n <= (uintptr_t)mi.BaseAddress + mi.RegionSize;
}

void nfs3_join_prefill(int okplay_on)
{
    static int said;        /* each log line once per run */
    static char kept[30];   /* what the game had in the box before okplay's text (this run) */
    const nfs3_build *b = B;
    char *buf;
    if (!b || !prefill_on) return;
    if (!b->join) {
        if (!(said & 1)) okp_log(2, "nfs3: %s - the Join box is not pre-filled (its buffer is not known for this build)", b->name);
        said |= 1;
        return;
    }
    if (!can_write(b->join, sizeof(kept))) return;
    buf = (char *)(uintptr_t)b->join;
    if (okplay_on && memcmp(buf, PREFILL, sizeof(PREFILL))) {
        memcpy(kept, buf, sizeof(kept) - 1);
        kept[sizeof(kept) - 1] = 0;
        memcpy(buf, PREFILL, sizeof(PREFILL));
        if (!(said & 2)) okp_log(1, "nfs3: the Join box is pre-filled with \"" PREFILL "\" (okplay's room list)");
        said |= 2;
    } else if (!okplay_on && !memcmp(buf, PREFILL, sizeof(PREFILL))) {
        memcpy(buf, kept, strlen(kept) + 1);   /* the game's own TCP/IP: never toward the real oldkid.net */
        okp_log(1, "nfs3: the original TCP/IP - okplay's \"" PREFILL "\" taken out of the Join box");
    }
}

/* The player's own name in the game (Player 1 Name): the menus' name buffer (b->pname, the one the lobby shows), else
 * the game's network name list (net - 0xCB, its first entry; filled only once a network game runs). Up to 10
 * characters and a 0. Read only; 1 = a name was read. */
int nfs3_player_name(char *out, int n)
{
    static volatile LONG name_readable;
    const nfs3_build *b = B;
    if (!b || n < 2) return 0;
    uint32_t at = b->pname ? b->pname : b->net - 0xCB;
    if (!name_readable) {
        if (!can_read(at, 11)) return 0;
        InterlockedExchange(&name_readable, 1);
    }
    const volatile unsigned char *p = (const volatile unsigned char *)(uintptr_t)at;
    int i = 0;
    for (; i < 10 && i < n - 1; i++) {
        unsigned char c = p[i];
        if (!c) break;
        if (c < 0x20 || c == 0x7f) return 0;   /* not a name */
        out[i] = (char)c;
    }
    out[i] = 0;
    if (!strcmp(out, "gamename")) return 0;   /* the game's own placeholder until a name is set: no name yet */
    return i > 0;
}
