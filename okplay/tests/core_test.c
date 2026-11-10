/* core_test - regression tests for the shared transport core (okplay/src), one per bug found in the
 * 2026-11-09 review. Each test names the review item; each failed before its fix. Runs on this PC only:
 * the one UDP socket it opens is on 127.0.0.1 and talks to itself.
 *
 *   tests/run_local.sh core      (builds bin/core_test.exe and runs it)
 *
 * The C library's realloc is wrapped to behave as Microsoft's does with size 0 (frees the block and
 * returns NULL - mingw's would not show the bug) and to notice a pointer freed twice. */
#define OKP_EXE_BUILD_WAIT_MS 300          /* util.c: the exe-hash wait, short for the test */
#include "../src/okp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
                                                           printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ------------------------------------------------------------------ allocation watch */
#define FREED_RING 64
static void *freed[FREED_RING];
static int freed_at, double_frees;
static void forget_freed(void *p)
{
    for (int i = 0; i < FREED_RING; i++) if (freed[i] == p) freed[i] = NULL;
}
static void t_free(void *p)
{
    if (!p) return;
    for (int i = 0; i < FREED_RING; i++)
        if (freed[i] == p) { double_frees++; printf("  double free of %p\n", p); return; }   /* (not freed again) */
    freed[freed_at++ % FREED_RING] = p;
    free(p);
}
static void *t_malloc(size_t n) { void *p = malloc(n); forget_freed(p); return p; }
static void *t_calloc(size_t a, size_t b) { void *p = calloc(a, b); forget_freed(p); return p; }
static void *t_realloc(void *p, size_t n)
{
    if (p && n == 0) { t_free(p); return NULL; }   /* what msvcrt does */
    void *q = realloc(p, n);
    forget_freed(q);
    return q;
}

/* ------------------------------------------------------------------ exe-hash gate (util.c) */
static HANDLE exe_gate;
static volatile LONG exe_gate_on;
static HANDLE WINAPI t_CreateFileA(LPCSTR n, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t)
{
    size_t l = strlen(n);
    if (exe_gate_on && l > 4 && !_stricmp(n + l - 4, ".exe")) WaitForSingleObject(exe_gate, INFINITE);
    return CreateFileA(n, a, s, sa, d, f, t);
}
#define CreateFileA t_CreateFileA
#include "../src/util.c"
#undef CreateFileA

#define malloc t_malloc
#define calloc t_calloc
#define realloc t_realloc
#define free t_free
#include "../src/net.c"
#include "../src/lan.c"
#include "../src/rdv.c"
#ifndef OKP_LAN_DRAIN_MAX
#define OKP_LAN_DRAIN_MAX 64   /* (lan.c before the fix had no cap) */
#endif
#undef malloc
#undef calloc
#undef realloc
#undef free

void okp_self_probe(okp_ctx *c) { (void)c; }
void okp_inject_rooms(okp_ctx *c) { (void)c; }
void okp_capture_enum_reply(okp_ctx *c, const uint8_t *m, int l) { (void)c; (void)m; (void)l; }

/* ------------------------------------------------------------------ helpers */
static okp_ctx *ctx;
static int got_n, got_len;
static uint8_t got[4096];
static void on_rx(void *c, uint64_t from, const void *data, int len)
{
    (void)c; (void)from;
    got_n++;
    got_len = len;
    if (len > 0 && len <= (int)sizeof(got)) memcpy(got, data, len);
}

static void reset(void)
{
    for (int i = 0; i < OKP_MAX_PEERS; i++) { okp_peer_free_bufs(&ctx->peers[i]); memset(&ctx->peers[i], 0, sizeof(okp_peer)); }
    ctx->host_peer = 0;
    ctx->is_host = 1;
    got_n = got_len = 0;
}

static SOCKADDR_IN6 addr(int a, int b, int c, int d, int port)
{
    SOCKADDR_IN6 s;
    uint8_t v4[4] = { (uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d };
    okp_addr_from_v4(&s, v4, htons((uint16_t)port));
    return s;
}

/* a peer we are connected to: path, token both ways, as after a probe exchange */
static okp_peer *connected(uint64_t id, SOCKADDR_IN6 at)
{
    okp_peer *p = okp_peer_get(ctx, id, OKP_PEER_CREATE);
    if (!p) return NULL;
    set_path(ctx, p, &at, "test");
    p->their_token = 0x7777000000000000ULL | id;
    p->their_epoch = 0x1234;
    return p;
}

static int pkt(uint8_t *b, int type, uint64_t from, uint64_t token, const void *body, int blen)
{
    okp_wire *w = (okp_wire *)b;
    w->m0 = 'O'; w->m1 = 'K'; w->ver = OKP_WIRE_VER; w->type = (uint8_t)type;
    w->room = ctx->room; w->from = from; w->to = ctx->self; w->token = token;
    memcpy(b + sizeof(okp_wire), body, blen);
    return (int)sizeof(okp_wire) + blen;
}

static void send_rel(okp_peer *p, int type, uint32_t seq, const char *data, int len, SOCKADDR_IN6 *from)
{
    uint8_t body[2048], b[2100];
    memcpy(body, &seq, 4);
    memcpy(body + 4, data, len);
    int n = pkt(b, type, p->id, p->my_token, body, 4 + len);
    handle_packet(ctx, b, n, from);
}

static void send_probe_from(uint64_t id, uint64_t token_in_hdr, uint32_t epoch, uint64_t their_tok, SOCKADDR_IN6 *from)
{
    okp_probe pr;
    memset(&pr, 0, sizeof(pr));
    pr.nonce = 1; pr.t_send = okp_now(); pr.epoch = epoch; pr.token = their_tok;
    uint8_t b[200];
    int n = pkt(b, OKP_T_PROBE, id, token_in_hdr, &pr, sizeof(pr));
    handle_packet(ctx, b, n, from);
}

static int used_peers(void)
{
    int n = 0;
    for (int i = 0; i < OKP_MAX_PEERS; i++) n += ctx->peers[i].used;
    return n;
}

/* ------------------------------------------------------------------ 2.2 a: empty reliable pieces */
static void t_empty_frags(void)
{
    printf("2.2a empty reliable pieces\n");
    reset();
    SOCKADDR_IN6 a = addr(10, 0, 0, 2, 2300);
    okp_peer *p = connected(0xB0B0, a);
    double_frees = 0;
    send_rel(p, OKP_T_RFRAG, 0, "", 0, &a);
    send_rel(p, OKP_T_RFRAG, 1, "", 0, &a);
    send_rel(p, OKP_T_RDATA, 2, "", 0, &a);
    CHECK(double_frees == 0, "empty RFRAG, RFRAG, RDATA: %d double free(s)", double_frees);
    CHECK(p->r_rx_next == 3, "the lane moved on past the empty pieces (next %lu)", (unsigned long)p->r_rx_next);
    CHECK(p->reasm == NULL && p->reasm_len == 0 && !p->reasm_bad, "nothing left half-assembled");

    /* pieces around an empty one still make the whole message */
    got_n = 0;
    send_rel(p, OKP_T_RFRAG, 3, "ab", 2, &a);
    send_rel(p, OKP_T_RFRAG, 4, "", 0, &a);
    send_rel(p, OKP_T_RDATA, 5, "cd", 2, &a);
    CHECK(got_n == 1 && got_len == 4 && !memcmp(got, "abcd", 4), "ab + empty + cd -> one 4-byte message (got %d, %d B)",
          got_n, got_len);
    /* the same out of order (pieces wait in rbuf, then go through rel_deliver as owned copies) */
    got_n = 0;
    double_frees = 0;
    send_rel(p, OKP_T_RDATA, 9, "", 0, &a);
    send_rel(p, OKP_T_RFRAG, 8, "", 0, &a);
    send_rel(p, OKP_T_RFRAG, 7, "", 0, &a);
    send_rel(p, OKP_T_RFRAG, 6, "x", 1, &a);
    CHECK(double_frees == 0, "out of order empty pieces: %d double free(s)", double_frees);
    CHECK(got_n == 1 && got_len == 1, "x + empty + empty + empty -> one 1-byte message (got %d, %d B)", got_n, got_len);
    CHECK(p->r_rx_next == 10, "next %lu", (unsigned long)p->r_rx_next);
}

/* ------------------------------------------------------------------ 2.2 b: holds and the peer table */
static void fill_connected(int n, uint64_t base)
{
    for (int i = 0; i < n; i++) connected(base + i, addr(10, 0, 1, (uint8_t)(i + 1), 2300));
}

static void t_table(void)
{
    printf("2.2b peer table: newcomers on a stranger's say-so\n");
    reset();
    fill_connected(OKP_MAX_PEERS, 0xC000);
    SOCKADDR_IN6 x = addr(10, 9, 9, 9, 4000);
    send_probe_from(0xDEAD01, 0, 5, 0x55, &x);   /* a first probe from a new id */
    CHECK(okp_peer_get(ctx, 0xDEAD01, 0) == NULL, "a first probe took the slot of a connected peer");
    CHECK(used_peers() == OKP_MAX_PEERS, "table still full of the connected peers");
    for (int i = 0; i < OKP_MAX_PEERS; i++)
        CHECK(okp_peer_get(ctx, 0xC000 + i, 0) != NULL, "connected peer %d still there", i);

    char line[256];
    snprintf(line, sizeof(line), "knock %016llx 10.9.9.8:4000\nknock %016llx 10.9.9.7:4000\n",
             (unsigned long long)0xDEAD02, (unsigned long long)0xDEAD03);
    parse_knocks(ctx, line);
    CHECK(okp_peer_get(ctx, 0xDEAD02, 0) == NULL && okp_peer_get(ctx, 0xDEAD03, 0) == NULL,
          "knock lines took the slots of connected peers");

    /* one slot holds a peer without a path (an old knock): a newcomer may have that one */
    okp_peer *idle = okp_peer_get(ctx, 0xC005, 0);
    idle->have_path = 0;
    send_probe_from(0xDEAD04, 0, 5, 0x55, &x);
    CHECK(okp_peer_get(ctx, 0xDEAD04, 0) != NULL && okp_peer_get(ctx, 0xC005, 0) == NULL,
          "a first probe takes the slot of a peer without a path");

    /* our own choices (OKP_PEER_CREATE: a send, Open, a player announced) still find room - never a held one */
    reset();
    fill_connected(OKP_MAX_PEERS, 0xC000);
    for (int i = 0; i < OKP_MAX_PEERS; i++) if (i != 7) okp_peer_hold(ctx, 0xC000 + i, 1);
    okp_peer *n = okp_peer_get(ctx, 0xD00D, OKP_PEER_CREATE);
    CHECK(n != NULL && okp_peer_get(ctx, 0xC007, 0) == NULL, "CREATE in a full table reuses the one peer not held");
    okp_peer_hold(ctx, 0xD00D, 1);
    CHECK(okp_peer_get(ctx, 0xD00E, OKP_PEER_CREATE) == NULL, "every peer held: CREATE is refused");
    okp_peer_hold(ctx, 0xC003, 0);
    CHECK(okp_peer_get(ctx, 0xD00E, OKP_PEER_CREATE) != NULL && okp_peer_get(ctx, 0xC003, 0) == NULL,
          "let go: that peer's slot can be reused");

    /* the peer table is not full: a first probe introduces a peer as before (0.8.x joiners) */
    reset();
    send_probe_from(0xE001, 0, 5, 0x66, &x);
    okp_peer *e = okp_peer_get(ctx, 0xE001, 0);
    CHECK(e && e->have_path && e->their_token == 0x66, "a first probe still introduces a peer");
}

static void t_hold_timers(void)
{
    printf("2.2b held peers and dead paths\n");
    reset();
    DWORD now = okp_now();
    SOCKADDR_IN6 a = addr(10, 0, 2, 1, 2300), b = addr(10, 0, 2, 2, 2300);
    okp_peer *h = connected(0xF001, a);
    okp_peer *u = connected(0xF002, b);
    okp_peer_hold(ctx, 0xF001, 1);
    CHECK(h->hold == 1, "hold counted");
    /* both older than their 60 s grace, both silent past PATH_DEAD_MS */
    for (okp_peer *p = h;; p = u) {
        p->keep_until = now - 1;
        p->last_path_rx = now - PATH_DEAD_MS - 1000;
        p->probe_start = now - 70000;
        p->last_probe = now - 5000;
        if (p == u) break;
    }
    run_timers(ctx);
    CHECK(okp_peer_get(ctx, 0xF002, 0) == NULL, "control: a peer without players or hold is forgotten");
    h = okp_peer_get(ctx, 0xF001, 0);
    CHECK(h != NULL, "a held peer with a dead path is not forgotten");
    if (!h) return;
    CHECK(!h->have_path, "its path is marked dead");
    CHECK(h->my_token != 0, "it keeps its token");
    CHECK(h->last_probe == okp_now() || okp_now() - h->last_probe < 100, "it is probed again (last probe %lu ms ago)",
          (unsigned long)(okp_now() - h->last_probe));
    /* still probing well after 15 s (the cut-off for peers that never joined) */
    h->probe_start = okp_now() - 40000;
    h->last_probe = okp_now() - 5000;
    run_timers(ctx);
    CHECK(okp_now() - h->last_probe < 100, "a held peer keeps probing after 15 s");
    /* and no cands + 2 minutes silent: still kept while held */
    h->ncands = 0;
    h->last_rx = okp_now() - PEER_EXPIRE_MS - 1000;
    run_timers(ctx);
    CHECK(okp_peer_get(ctx, 0xF001, 0) != NULL, "a held peer does not expire");
    okp_peer_hold(ctx, 0xF001, 1);
    okp_peer_hold(ctx, 0xF001, 0);
    CHECK(h->hold == 1, "holds are counted (2 on, 1 off -> 1)");
    okp_peer_hold(ctx, 0xF001, 0);
    okp_peer_hold(ctx, 0xF001, 0);
    CHECK(h->hold == 0, "never below 0");
    okp_peer_hold(ctx, 0xBADBAD, 1);   /* no such peer: nothing */
    CHECK(okp_peer_get(ctx, 0xBADBAD, 0) == NULL, "holding an unknown peer does not create it");
}

/* ------------------------------------------------------------------ 2.4 a / 2.2 g */
static void t_announce(void)
{
    printf("2.4a who may name a peer's addresses\n");
    reset();
    ctx->is_host = 0;
    SOCKADDR_IN6 ha = addr(10, 0, 3, 1, 2300), va = addr(10, 0, 3, 2, 2300), ma = addr(10, 0, 3, 3, 2300);
    connected(0xA001, ha);   /* the host */
    ctx->host_peer = 0xA001;
    okp_peer *v = connected(0xA002, va);
    connected(0xA003, ma);   /* M, a member */
    int before = v->ncands;
    okp_peer *r = okp_peer_announced(ctx, 0xA003, 0xA002, &ma, 1);   /* M: "a player on V, at M's address" */
    CHECK(r == v && v->nplayers == 1, "the player is counted");
    CHECK(v->ncands == before && !addr_known(v, &ma), "M's address was added to the connected V");
    /* M then probes as V from its own address, without V's token: not believed, no token handed out */
    uint64_t vtok = v->their_token;
    send_probe_from(0xA002, 0, 0x9999, 0xEEEE, &ma);
    CHECK(v->their_token == vtok && okp_addr_eq(&v->path, &va), "M passed as V");

    /* honest: the host's list names a player we never reached; a peer names its own addresses */
    SOCKADDR_IN6 wa = addr(10, 0, 3, 4, 2300), va2 = addr(10, 0, 3, 22, 2300);
    okp_peer *w = okp_peer_announced(ctx, 0xA001, 0xA004, &wa, 1);
    CHECK(w && w->nplayers == 1 && addr_known(w, &wa), "the host's list gives a new peer its addresses");
    okp_peer *w2 = okp_peer_announced(ctx, 0xA003, 0xA005, &wa, 1);
    CHECK(w2 && addr_known(w2, &wa), "a forward from another machine (a new host after migration) gives a new peer its addresses");
    okp_peer_announced(ctx, 0xA002, 0xA002, &va2, 1);
    CHECK(addr_known(v, &va2) && v->nplayers == 2, "V names its own addresses");
    okp_peer_announced(ctx, 0, 0xA006, &wa, 1);
    CHECK(addr_known(okp_peer_get(ctx, 0xA006, 0), &wa), "sender not known (no header): a never-reached peer still learns");

    printf("2.2g restart from a known address (old builds do this; must keep working)\n");
    /* V's game restarts its okplay peer for us (a retried join): new epoch, its new token, none of ours yet,
     * from the address we know */
    okp_peer *v3 = okp_peer_get(ctx, 0xA002, 0);
    uint32_t restarts = v3->r_restarts;
    send_probe_from(0xA002, 0, 0x4242, 0xABCD, &va);
    CHECK(v3->their_token == 0xABCD && v3->their_epoch == 0x4242 && v3->r_restarts == restarts + 1,
          "a restart from V's own address is believed (token 0x%llx, epoch 0x%lx)", (unsigned long long)v3->their_token,
          (unsigned long)v3->their_epoch);
}

/* ------------------------------------------------------------------ 2.2 i: crash report paths */
static void t_scrub(void)
{
    printf("2.2i crash report scrubbing\n");
    static const struct { const char *in, *out; } cases[] = {
        { "loaded C:\\Users\\Smith, John\\Games\\x.exe ok", "loaded ...\\x.exe ok" },
        { "C:\\Users\\Smith, John\\Games\\x.exe", "...\\x.exe" },
        { "C:\\Users\\O'Brien\\Games\\age2_x1.exe (build 1)", "...\\age2_x1.exe (build 1)" },
        { "C:\\Users\\Ann (home)\\x.exe", "...\\x.exe" },
        { "C:\\Users\\Name\\Games\\AoE2\\age2_x1.exe  next", "...\\age2_x1.exe  next" },
        { "module C:\\Windows\\system32\\dplayx.dll, base 0", "module ...\\dplayx.dll, base 0" },
        { "\"C:\\Users\\Smith - John\\a.exe\" -x", "\"...\\a.exe\" -x" },
        { "a C:\\Users\\Smith, John\\a.exe, b D:\\Games\\Bob\\b.exe", "a ...\\a.exe, b ...\\b.exe" },
        { "\\\\server\\share\\Smith, John\\x.exe", "...\\x.exe" },
        { "peer 192.168.1.20 v5.1.2600.0", "peer x.x.x.x v5.1.2600.0" },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char out[256];
        okp_scrub(cases[i].in, out, sizeof(out));
        CHECK(!strcmp(out, cases[i].out), "scrub(%s) = \"%s\", want \"%s\"", cases[i].in, out, cases[i].out);
        CHECK(!strstr(out, "Smith") && !strstr(out, "John") && !strstr(out, "Brien") && !strstr(out, "Users"),
              "a name or folder survived: \"%s\"", out);
    }
}

/* ------------------------------------------------------------------ 2.2 d: LAN drain cap */
static void t_lan_cap(void)
{
    printf("2.2d the LAN socket is read a batch at a time\n");
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    SOCKADDR_IN a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int al = sizeof(a);
    if (s == INVALID_SOCKET || bind(s, (SOCKADDR *)&a, sizeof(a)) || getsockname(s, (SOCKADDR *)&a, &al)) {
        printf("  (no loopback socket: %d) - not run\n", WSAGetLastError());
        CHECK(0, "LAN cap test could not open its socket");
        return;
    }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    int big = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (char *)&big, sizeof(big));
    const int sent = 300;
    for (int i = 0; i < sent; i++) sendto(s, "junk", 4, 0, (SOCKADDR *)&a, sizeof(a));   /* too short: skipped */
    Sleep(50);
    SOCKET old = ctx->lan_q;
    ctx->lan_q = s;
    okp_lan_readable(ctx, s);
    ctx->lan_q = old;
    int left = 0;
    char b[64];
    while (recv(s, b, sizeof(b), 0) > 0) left++;
    CHECK(left >= sent - OKP_LAN_DRAIN_MAX && left > 0, "one call read %d of %d datagrams (at most %d)", sent - left, sent,
          OKP_LAN_DRAIN_MAX);
    closesocket(s);
}

/* ------------------------------------------------------------------ 2.2 j: the exe-hash wait */
static DWORD WINAPI hash_thread(LPVOID a) { char b[17]; (void)a; okp_exe_build(b, NULL, 0); return 0; }
static char wb[17] = "x", wexe[64] = "";
static DWORD WINAPI wait_thread(LPVOID a) { (void)a; okp_exe_build(wb, wexe, sizeof(wexe)); return 0; }

static void t_exe_wait(void)
{
    printf("2.2j the exe-hash wait has a limit\n");
    exe_gate = CreateEventA(NULL, TRUE, FALSE, NULL);
    exe_gate_on = 1;
    HANDLE h = CreateThread(NULL, 0, hash_thread, NULL, 0, NULL);   /* starts hashing, then stalls on the exe */
    Sleep(100);
    HANDLE w = CreateThread(NULL, 0, wait_thread, NULL, 0, NULL);
    DWORD r = WaitForSingleObject(w, 5000);
    CHECK(r == WAIT_OBJECT_0, "a second caller waited more than 5 s for a stalled hash");
    if (r == WAIT_OBJECT_0) {
        CHECK(wb[0] == 0, "a timed-out caller gets no build (got \"%s\")", wb);
        CHECK(strstr(wexe, ".exe") != NULL, "it still gets the exe name (got \"%s\")", wexe);
    }
    SetEvent(exe_gate);   /* the hash finishes: later callers get it */
    WaitForSingleObject(h, 10000);
    WaitForSingleObject(w, 10000);
    char b2[17] = "";
    okp_exe_build(b2, NULL, 0);
    CHECK(strlen(b2) == 16, "after the hash finished the build is known (\"%s\")", b2);
    exe_gate_on = 0;
}

int main(void)
{
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    ctx = (okp_ctx *)calloc(1, sizeof(okp_ctx));
    InitializeCriticalSection(&ctx->lock);
    ctx->raw_rx = on_rx;
    ctx->sock = INVALID_SOCKET;   /* sendto fails: nothing leaves this PC */
    ctx->lan_q = ctx->lan_listen = INVALID_SOCKET;
    ctx->self = 0x1111ULL;
    ctx->room = 0x2222;
    ctx->mode = OKP_MODE_ONLINE;
    ctx->mode_name = "Online Play";
    okp_cfg.stun[0] = 0;
    okp_cfg.stats_ms = 0;

    t_empty_frags();
    t_table();
    t_hold_timers();
    t_announce();
    t_scrub();
    t_lan_cap();
    t_exe_wait();

    printf("%d checks, %d failed\nRESULT %s\n", checks, fails, fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
