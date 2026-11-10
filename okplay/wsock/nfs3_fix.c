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
 * The Join box ("Enter domain name or IP address"): the game keeps the address typed last in a text buffer of its
 * own (29 characters; the 1.6.x builds save it with the player's settings) and fills the box from it when it opens.
 * With okplay running (Online or LAN Play) the buffer is set to "oldkid.net", which wsock.c takes as "okplay's room
 * list" (never looked up): the player sees okplay is there and just presses Ok. What the game had there is kept and
 * put back if the game then runs with its original TCP/IP in the same run, so the game never connects to the real
 * oldkid.net (a later run that finds okplay's text there with the original TCP/IP empties it). okplay.ini
 * nfs3_join_prefill=0 turns it off. Data only: that one text buffer is written, never any code.
 *
 * The builds are recognised by their exe fingerprint (okp_exe_build); the addresses are absolute, for the exe loaded
 * at its usual base 0x400000 (anywhere else: not known). Unknown builds, or memory that can't be read: -1, and
 * wsock.c keeps its listen-again rule. The values are only read, never written. */
#include "okp.h"
#include "nfs3_fix.h"
#include <string.h>

typedef struct {
    const char *build, *name;
    uint32_t net;      /* DWORD: the network game state */
    uint32_t server;   /* WORD: this PC is the server */
    uint32_t join;     /* char[30]: the Join box's address, typed last (0 = not pinned down for this build) */
} nfs3_build;

static const nfs3_build builds[] = {
    { "240a083a78ac3b83", "Modern Patch 1.6.1",              0x7a22e0, 0x7a22da, 0x6fbb7e },
    { "397dee58eb88822c", "portable 1.6.2 Beta",             0x7a22e0, 0x7a22da, 0x6fbb7e },
    { "b390be67925b2ad0", "1998 OEM release",                0x7a22e0, 0x7a22da, 0 },   /* (Join box buffer not studied) */
    { "53aa0dfcf438788d", "1998 CD (unpacked in memory)",    0x7c92e0, 0x7c92da, 0x722b7e },   /* (packed on disk) */
};

/* B: the build, once the exe's fingerprint is known (set by find_build's thread, once; never cleared: the exe
 * doesn't change while it runs). readable: both addresses were found readable once (set at the first good check,
 * never cleared: they are the exe's own data). */
static const nfs3_build *volatile B;
static volatile LONG readable;
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
    if (!b) {
        okp_log(1, "nfs3: %s build %s is not one okplay knows - Host Game is told by the listen-again rule", exe,
                build[0] ? build : "?");
        return 0;
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
