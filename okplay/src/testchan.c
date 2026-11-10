/* okplay - the test channel: Connection Check's test game, carried by the shared core in both editions.
 *
 * The drop-in (dplayx.dll) and okplay wsock (wsock32.dll) export the same OkplayTest* functions (below), so
 * Connection Check tests through whichever of our DLLs sits in the game's folder, and players in a DirectPlay
 * folder and in a Winsock folder test with each other. Nothing of DirectPlay or of wsock's virtual sockets is in
 * the path: an okp_ctx of its own (raw_rx, as wsock's), with the same room server calls, candidates, STUN, hole
 * punching, tokens and reliable lane as every game - so a game in the same process is never disturbed.
 *
 * Rooms: a record with the DirectPlay session record's layout under "okTC" instead of "play" (net.c
 * okp_tc_blob_ok), guidApplication = Connection Check's TEST_APP. The room server (okplay.php, any version
 * since TEST_APP) files it with the test games: never on the web page, never in a game's list. Every DirectPlay
 * path - also in released drop-ins 0.8.3-0.8.5 and their Connection Check - checks the record for "play"
 * (okp_enum_blob_ok) and passes it over; only this file's list keeps it (okp_ctx.tc). The list here also shows
 * released Connection Checks' test games (a "play" record with TEST_APP): those speak DirectPlay, so they are
 * joined through dplayx.dll's DirectPlay, not here.
 *
 * Players: a joiner sends 'H' (its name) to the host; the host answers 'W' (its name, then every other player
 * with its addresses), tells the others 'N' (the newcomer), and 'L' when one leaves. Everyone then reaches
 * everyone directly (hole punching both ways, as DirectPlay games do with okplay's player data). Up to 8
 * players. 'D' carries Connection Check's own messages; 'B' = leaving; 'R' = refused (full).
 * Frame: 'T','C', TC_VER, type, body. Control frames ride the reliable lane. The host leaving ends the test.
 *
 * Calls come from one thread of the caller (Connection Check's worker); okplay's network thread only queues
 * what arrives (tc_rx). */
#include "okp.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define TC_VER 1
#define TC_MAXP 8          /* players in a test game, the host included */
#define TC_JOIN_MS 30000   /* a join gives up after this */
#define TC_CANDS 4         /* addresses told per player */

/* {0E7C4B61-93A2-4F0B-8D35-6A1F2C9B7E58} Connection Check's test games (okplay.php / rdv_standin.py TEST_APP) */
static const GUID TC_TEST_APP = { 0x0e7c4b61, 0x93a2, 0x4f0b, { 0x8d, 0x35, 0x6a, 0x1f, 0x2c, 0x9b, 0x7e, 0x58 } };

enum { EV_DATA = 0, EV_JOINED = 1, EV_LEFT = 2, EV_LOST = 3 };   /* OkplayTestRecv's kinds */

typedef struct tc_msg {
    struct tc_msg *next;
    uint64_t from;
    int kind;
    DWORD id;
    int len;
    uint8_t data[1];
} tc_msg;

typedef struct { int used; DWORD id; uint64_t peer; char name[64]; int heard; } tc_member;

static CRITICAL_SECTION tc_cs;    /* the two queues */
static volatile LONG tc_cs_state;
static tc_msg *in_head, *in_tail;   /* arrived (network thread) */
static tc_msg *ev_head, *ev_tail;   /* for the caller */
static okp_ctx *tc_c;
static int tc_hosting, tc_in, tc_refused, tc_lost;
static uint64_t tc_host;            /* joiner: the room's host */
static tc_member tc_m[TC_MAXP + 4];
static DWORD tc_next_id;
static char tc_name[64];

static void tc_init(void)
{
    if (tc_cs_state == 2) return;
    if (__sync_val_compare_and_swap(&tc_cs_state, 0, 1) == 0) {
        InitializeCriticalSection(&tc_cs);
        tc_cs_state = 2;
    }
    while (tc_cs_state != 2) Sleep(1);
}

okp_ctx *okp_tc_ctx(void) { return tc_c; }

static void q_push(tc_msg **head, tc_msg **tail, tc_msg *m)
{
    m->next = NULL;
    EnterCriticalSection(&tc_cs);
    if (*tail) (*tail)->next = m; else *head = m;
    *tail = m;
    LeaveCriticalSection(&tc_cs);
}

static tc_msg *q_pop(tc_msg **head, tc_msg **tail)
{
    EnterCriticalSection(&tc_cs);
    tc_msg *m = *head;
    if (m) { *head = m->next; if (!*head) *tail = NULL; }
    LeaveCriticalSection(&tc_cs);
    return m;
}

static void q_free(tc_msg **head, tc_msg **tail)
{
    tc_msg *m;
    while ((m = q_pop(head, tail)) != NULL) free(m);
}

static void event(int kind, DWORD id, const void *data, int len)
{
    tc_msg *m = (tc_msg *)malloc(sizeof(tc_msg) + len);
    if (!m) return;
    m->kind = kind;
    m->id = id;
    m->from = 0;
    m->len = len;
    if (len) memcpy(m->data, data, len);
    q_push(&ev_head, &ev_tail, m);
}

/* okplay's network thread: a frame from a player (the token check already passed) */
static void tc_rx(void *ctx, uint64_t from, const void *data, int len)
{
    const uint8_t *d = (const uint8_t *)data;
    (void)ctx;
    if (len < 4 || d[0] != 'T' || d[1] != 'C' || d[2] != TC_VER) return;
    tc_msg *m = (tc_msg *)malloc(sizeof(tc_msg) + len);
    if (!m) return;
    m->from = from;
    m->kind = d[3];
    m->len = len - 4;
    memcpy(m->data, d + 4, len - 4);
    q_push(&in_head, &in_tail, m);
}

static int tc_send(uint64_t to, int type, const void *body, int blen, int flags)
{
    uint8_t *b = (uint8_t *)malloc(4 + blen);
    if (!b || !tc_c) { free(b); return 0; }
    b[0] = 'T'; b[1] = 'C'; b[2] = TC_VER; b[3] = (uint8_t)type;
    if (blen) memcpy(b + 4, body, blen);
    int ok = okp_send_data(tc_c, to, b, 4 + blen, flags);
    free(b);
    return ok;
}

/* ---------------------------------------------------------------- players */

static tc_member *mem_find(uint64_t peer)
{
    for (int i = 0; i < TC_MAXP + 4; i++) if (tc_m[i].used && tc_m[i].peer == peer) return &tc_m[i];
    return NULL;
}

static int mem_count(void)
{
    int n = 0;
    for (int i = 0; i < TC_MAXP + 4; i++) n += tc_m[i].used;
    return n;
}

static void tc_record(void);

static tc_member *mem_add(uint64_t peer, const char *name)
{
    tc_member *m = mem_find(peer);
    if (m) return m;
    for (int i = 0; i < TC_MAXP + 4 && !m; i++) if (!tc_m[i].used) m = &tc_m[i];
    if (!m) return NULL;
    memset(m, 0, sizeof(*m));
    m->used = 1;
    m->id = ++tc_next_id;
    m->peer = peer;
    lstrcpynA(m->name, name && name[0] ? name : "?", sizeof(m->name));
    okp_peer_hold(tc_c, peer, 1);
    okp_log(1, "test: %s (peer %016llx) is in the test game", m->name, (unsigned long long)peer);
    event(EV_JOINED, m->id, m->name, (int)strlen(m->name) + 1);
    if (tc_hosting) tc_record();
    return m;
}

static void mem_drop(tc_member *m, const char *why, int closed)   /* closed: it said goodbye ('B') */
{
    if (!m || !m->used) return;
    okp_log(1, "test: %s (peer %016llx) left the test game (%s)", m->name, (unsigned long long)m->peer, why);
    m->used = 0;
    okp_peer_hold(tc_c, m->peer, 0);
    event(EV_LEFT, m->id, NULL, 0);
    if (tc_hosting) {
        tc_record();
        for (int i = 0; i < TC_MAXP + 4; i++)
            if (tc_m[i].used) tc_send(tc_m[i].peer, 'L', &m->peer, 8, OKP_SEND_RELIABLE);
    } else if (m->peer == tc_host && !tc_lost) {
        uint8_t c = (uint8_t)closed;
        tc_lost = 1;
        event(EV_LOST, 0, &c, 1);   /* (1: the host closed the test game; 0: it went silent) */
    }
}

/* one player as the host tells it: peer, name, its addresses (where the host reaches it first) */
static int put_entry(uint8_t *out, uint64_t peer, const char *name)
{
    int o = 0, nl = (int)strlen(name);
    SOCKADDR_IN6 a[TC_CANDS];
    int n = 0;
    if (nl > 63) nl = 63;
    memcpy(out, &peer, 8);
    o = 8;
    out[o++] = (uint8_t)nl;
    memcpy(out + o, name, nl);
    o += nl;
    EnterCriticalSection(&tc_c->lock);
    okp_peer *p = okp_peer_get(tc_c, peer, 0);
    if (p && p->have_path) a[n++] = p->path;
    for (int i = 0; p && i < p->ncands && n < TC_CANDS; i++) {
        int dup = 0;
        for (int k = 0; k < n; k++) if (okp_addr_eq(&a[k], &p->cands[i])) dup = 1;
        if (!dup) a[n++] = p->cands[i];
    }
    LeaveCriticalSection(&tc_c->lock);
    out[o++] = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        okp_ccand cc;
        memset(&cc, 0, sizeof(cc));
        cc.port = a[i].sin6_port;
        if (okp_addr_is_v4(&a[i])) { cc.fam = 4; memcpy(cc.addr, (const uint8_t *)&a[i].sin6_addr + 12, 4); }
        else { cc.fam = 6; memcpy(cc.addr, &a[i].sin6_addr, 16); }
        memcpy(out + o, &cc, sizeof(cc));
        o += sizeof(cc);
    }
    return o;
}

#define ENTRY_MAX (8 + 1 + 63 + 1 + TC_CANDS * (int)sizeof(okp_ccand))

/* a player the host told us of: reach it directly; returns the bytes read, 0 = broken */
static int take_entry(const uint8_t *b, int n)
{
    uint64_t peer;
    char name[64];
    SOCKADDR_IN6 a[TC_CANDS];
    int o, nl, nc;
    if (n < 10) return 0;
    memcpy(&peer, b, 8);
    nl = b[8];
    if (nl > 63 || 9 + nl + 1 > n) return 0;
    memcpy(name, b + 9, nl);
    name[nl] = 0;
    o = 9 + nl;
    nc = b[o++];
    if (nc > TC_CANDS || o + nc * (int)sizeof(okp_ccand) > n) return 0;
    for (int i = 0; i < nc; i++) {
        okp_ccand cc;
        memcpy(&cc, b + o + i * sizeof(cc), sizeof(cc));
        if (cc.fam == 4) okp_addr_from_v4(&a[i], cc.addr, cc.port);
        else { memset(&a[i], 0, sizeof(a[i])); a[i].sin6_family = AF_INET6; a[i].sin6_port = cc.port; memcpy(&a[i].sin6_addr, cc.addr, 16); }
    }
    o += nc * sizeof(okp_ccand);
    if (peer == tc_c->self || peer == 0) return o;
    EnterCriticalSection(&tc_c->lock);
    okp_peer *p = okp_peer_get(tc_c, peer, OKP_PEER_CREATE);
    /* (as okp_peer_announced: addresses from the host only for a player we never reached) */
    if (p && p->path_changes == 0) { okp_peer_add_cands(tc_c, p, a, nc); if (!p->have_path) p->probe_start = okp_now(); }
    LeaveCriticalSection(&tc_c->lock);
    if (p) mem_add(peer, name);
    return o;
}

/* ---------------------------------------------------------------- what arrives */

static void handle(tc_msg *m)
{
    const uint8_t *b = m->data;
    int n = m->len;
    tc_member *from = mem_find(m->from);
    switch (m->kind) {
    case 'H': {   /* host: a player asks in */
        char name[64];
        int nl = n < 63 ? n : 63;
        if (!tc_hosting || from) break;
        memcpy(name, b, nl);
        name[nl] = 0;
        if (mem_count() + 1 >= TC_MAXP) {
            uint8_t r = 1;
            okp_log(1, "test: %s asks in - the test game is full", name);
            tc_send(m->from, 'R', &r, 1, OKP_SEND_RELIABLE);
            break;
        }
        uint8_t *w = (uint8_t *)malloc(64 + (TC_MAXP + 4) * ENTRY_MAX), *e = (uint8_t *)malloc(ENTRY_MAX);
        if (!w || !e) { free(w); free(e); break; }
        int o = 0, hl = (int)strlen(tc_name), cnt = 0;
        w[o++] = (uint8_t)hl;
        memcpy(w + o, tc_name, hl);
        o += hl;
        int at = o++;
        for (int i = 0; i < TC_MAXP + 4; i++)
            if (tc_m[i].used) { o += put_entry(w + o, tc_m[i].peer, tc_m[i].name); cnt++; }
        w[at] = (uint8_t)cnt;
        tc_member *nm = mem_add(m->from, name);
        if (nm) {
            int el = put_entry(e, m->from, nm->name);
            for (int i = 0; i < TC_MAXP + 4; i++)
                if (tc_m[i].used && &tc_m[i] != nm) tc_send(tc_m[i].peer, 'N', e, el, OKP_SEND_RELIABLE);
            tc_send(m->from, 'W', w, o, OKP_SEND_RELIABLE);
        }
        free(w);
        free(e);
        break;
    }
    case 'W': {   /* joiner: in - the host's name, then the others */
        char name[64];
        if (tc_hosting || tc_in || m->from != tc_host || n < 2) break;
        int hl = b[0];
        if (hl > 63 || 1 + hl + 1 > n) break;
        memcpy(name, b + 1, hl);
        name[hl] = 0;
        mem_add(tc_host, name);
        int o = 1 + hl, cnt = b[o++];
        for (int i = 0; i < cnt && o < n; i++) {
            int k = take_entry(b + o, n - o);
            if (!k) break;
            o += k;
        }
        tc_in = 1;
        break;
    }
    case 'N':     /* joiner: a newcomer */
        if (!tc_hosting && tc_in && m->from == tc_host) take_entry(b, n);
        break;
    case 'L':     /* joiner: one left */
        if (!tc_hosting && m->from == tc_host && n >= 8) {
            uint64_t peer;
            memcpy(&peer, b, 8);
            mem_drop(mem_find(peer), "the host says so", 1);
        }
        break;
    case 'B':
        mem_drop(from, "it closed the test", 1);
        break;
    case 'R':
        if (m->from == tc_host) tc_refused = n ? b[0] : 1;
        break;
    case 'D':
        if (from) event(EV_DATA, from->id, b, n);
        break;
    }
}

/* the queue, and players gone silent (a player we once reached and haven't heard from for drop_after) */
static void tc_tick(void)
{
    tc_msg *m;
    while ((m = q_pop(&in_head, &in_tail)) != NULL) { if (tc_c) handle(m); free(m); }
    if (!tc_c) return;
    for (int i = 0; i < TC_MAXP + 4; i++) {
        okp_peer_info_t pi;
        tc_member *t = &tc_m[i];
        if (!t->used) continue;
        if (!okp_peer_info(tc_c, t->peer, &pi)) { mem_drop(t, "okplay forgot it", 0); continue; }
        if (pi.have_path) t->heard = 1;
        if (t->heard && pi.rx_age_ms > (DWORD)okp_drop_after_ms()) mem_drop(t, "silent", 0);
    }
}

/* ---------------------------------------------------------------- the room */

static void tc_record(void)
{
    uint8_t b[512];
    char name[96];
    uint32_t sdsize = 80, maxp = TC_MAXP, curp = 1 + (uint32_t)mem_count(), off = 92;
    uint16_t cmd = 1, ver = TC_VER;
    int n = 92;
    memset(b, 0, sizeof(b));
    memcpy(b, "okTC", 4);
    memcpy(b + 4, &cmd, 2);
    memcpy(b + 6, &ver, 2);
    memcpy(b + 8, &sdsize, 4);
    memcpy(b + 8 + 8, &tc_c->room, 4);         /* guidInstance */
    memcpy(b + 8 + 24, &TC_TEST_APP, 16);      /* guidApplication: the room server's test games */
    memcpy(b + 8 + 40, &maxp, 4);
    memcpy(b + 8 + 44, &curp, 4);
    memcpy(b + 88, &off, 4);
    _snprintf(name, sizeof(name), "Connection test - %s", tc_name);
    name[sizeof(name) - 1] = 0;
    for (int i = 0; name[i] && n < 400; i++) { b[n++] = (uint8_t)name[i]; b[n++] = 0; }
    b[n++] = 0;
    b[n++] = 0;
    EnterCriticalSection(&tc_c->lock);
    memcpy(tc_c->enum_blob, b, n);
    tc_c->enum_len = n;
    LeaveCriticalSection(&tc_c->lock);
}

static void tc_stop(void)
{
    okp_ctx *c = tc_c;
    if (!c) { q_free(&in_head, &in_tail); q_free(&ev_head, &ev_tail); return; }
    for (int i = 0; i < TC_MAXP + 4; i++) if (tc_m[i].used) tc_send(tc_m[i].peer, 'B', NULL, 0, OKP_SEND_RELIABLE);
    DWORD t0 = okp_now();   /* the goodbyes acked (at most 1.5 s) */
    for (int busy = 1; busy && okp_now() - t0 < 1500;) {
        busy = 0;
        for (int i = 0; i < TC_MAXP + 4; i++) {
            okp_peer_info_t pi;
            if (tc_m[i].used && okp_peer_info(c, tc_m[i].peer, &pi) && pi.have_path && (pi.unacked_bytes || pi.waiting_bytes)) busy = 1;
        }
        if (busy) Sleep(20);
    }
    int withdraw = tc_hosting && c->room && c->mode == OKP_MODE_ONLINE;
    c->closing = 1;
    c->session_open = 0;
    c->is_host = 0;
    if (withdraw) okp_rdv_close(c);
    c->running = 0;
    okp_rdv_stop(c);
    okp_net_stop(c);
    tc_c = NULL;
    DeleteCriticalSection(&c->lock);
    free(c);
    memset(tc_m, 0, sizeof(tc_m));
    tc_hosting = tc_in = tc_refused = tc_lost = 0;
    tc_host = 0;
    q_free(&in_head, &in_tail);
    q_free(&ev_head, &ev_tail);
    okp_log(1, "test: the test game's okplay stopped");
}

static okp_ctx *tc_start(int lan)
{
    okp_config_load();   /* (server, STUN, transport= as they are now: env and okplay.ini) */
    okp_ctx *c = (okp_ctx *)calloc(1, sizeof(okp_ctx));
    if (!c) return NULL;
    c->host_transport = -1;
    InitializeCriticalSection(&c->lock);
    c->mode = lan ? OKP_MODE_LAN : OKP_MODE_ONLINE;
    c->mode_name = lan ? "LAN Play" : "Online Play";
    c->sock = c->lan_q = c->lan_listen = INVALID_SOCKET;
    c->self = okp_rand64();
    c->raw_rx = tc_rx;
    c->enum_app = TC_TEST_APP;   /* the room server's list: test games only */
    c->tc = 1;
    if (!okp_net_start(c) || !okp_rdv_start(c)) {
        okp_log(0, "test: okplay could not start for the test game");
        c->running = 0;
        okp_rdv_stop(c);
        okp_net_stop(c);
        DeleteCriticalSection(&c->lock);
        free(c);
        return NULL;
    }
    okp_log(1, "test: okplay " OKP_VERSION_STR " %s for a Connection Check test game, peer %016llx", c->mode_name,
            (unsigned long long)c->self);
    return c;
}

/* ---------------------------------------------------------------- the exports
 *
 * OkplayTestHost(name, lan, transport)  host "Connection test - <name>" (transport: 0 udp, 1 udp+reliable,
 *                                       2 reliable, -1 okplay.ini's); 1 = listed / answering on the LAN
 * OkplayTestList(lan, out, outsz)       the test games seen, one per line "<kind> <host> <room> <players> <max>
 *                                       <name>" (kind n = this channel, d = a released Connection Check's
 *                                       DirectPlay test game: join that one through DirectPlay); the list
 *                                       refreshes in the background - call it again every second or so
 * OkplayTestJoin(host, name)            join the room of <host> (16 hex digits) from the last list. 1 = in;
 *                                       -1 not listed, -2 no connection to the host, -3 full, -4 no answer
 * OkplayTestSend(to, buf, len, flags)   to a player (its id from OkplayTestRecv), 0 = everyone;
 *                                       flags 1 = guaranteed (as DPSEND_GUARANTEED, the host's transport
 *                                       decides), 2 = always the reliable lane
 * OkplayTestRecv(&from, &kind, buf, cap) the next event: length (data cut to cap), -1 = none. kind 0 data,
 *                                       1 joined (buf: the name), 2 left, 3 the host is gone (buf: 1 byte,
 *                                       1 = it closed the test game, 0 = it went silent)
 * OkplayTestClose()                     leave / stop hosting
 * Old DLLs have none of these: Connection Check looks them up with GetProcAddress. */

static int ready(void)
{
    tc_init();
    return okp_tc_dll_ready();
}

int WINAPI OkplayTestHost(const char *name, int lan, int transport)
{
    if (!ready()) return 0;
    tc_stop();
    lstrcpynA(tc_name, name && name[0] ? name : "Player", sizeof(tc_name));
    tc_c = tc_start(lan);
    if (!tc_c) return 0;
    if (transport >= 0 && transport <= 2) okp_cfg.transport = transport;   /* (after okp_config_load: the host decides) */
    tc_hosting = 1;
    tc_c->room = (uint32_t)okp_rand64() | 1;
    snprintf(tc_c->tok, sizeof(tc_c->tok), "%016llx", (unsigned long long)okp_rand64());
    tc_c->is_host = 1;
    tc_record();
    tc_c->session_open = 1;
    if (lan && !okp_lan_open_listen(tc_c))
        okp_log(0, "test: LAN discovery port %u unavailable - joiners will not see this test game", OKP_LAN_PORT);
    okp_log(1, "test: hosting room %08x, %s, transport %s", tc_c->room, tc_c->mode_name, okp_transport_name(okp_cfg.transport));
    okp_rdv_wake(tc_c);
    return 1;
}

int WINAPI OkplayTestList(int lan, char *out, int outsz)
{
    int n = 0, len = 0;
    if (!out || outsz < 1) return -1;
    out[0] = 0;
    if (!ready()) return -1;
    if (!tc_c || tc_hosting || tc_host || tc_c->mode != (lan ? OKP_MODE_LAN : OKP_MODE_ONLINE)) {
        tc_stop();
        tc_c = tc_start(lan);
        if (!tc_c) return -1;
    }
    tc_c->enum_pending = 1;
    okp_rdv_wake(tc_c);
    okp_room *rooms = (okp_room *)malloc(sizeof(okp_room) * OKP_MAX_ROOMS);
    if (!rooms) return -1;
    EnterCriticalSection(&tc_c->lock);
    int nr = tc_c->nrooms;
    memcpy(rooms, tc_c->rooms, sizeof(okp_room) * nr);
    LeaveCriticalSection(&tc_c->lock);
    DWORD now = okp_now();
    for (int i = 0; i < nr; i++) {
        okp_room *rm = &rooms[i];
        int kind = okp_tc_blob_ok(rm->enumblob, rm->enumlen) ? 'n' :
                   okp_enum_blob_ok(rm->enumblob, rm->enumlen) && !memcmp(rm->enumblob + 8 + 24, &TC_TEST_APP, 16) ? 'd' : 0;
        if (!kind || (lan && now - rm->seen > OKP_LAN_ROOM_TTL)) continue;
        uint32_t cur, mx;
        memcpy(&mx, rm->enumblob + 8 + 40, 4);
        memcpy(&cur, rm->enumblob + 8 + 44, 4);
        WCHAR w[64];
        char name[128];
        int k = 0;
        for (int j = 92; j + 1 < rm->enumlen && k < 63; j += 2) {
            w[k] = (WCHAR)(rm->enumblob[j] | rm->enumblob[j + 1] << 8);
            if (!w[k]) break;
            k++;
        }
        w[k] = 0;
        if (!WideCharToMultiByte(CP_ACP, 0, w, -1, name, sizeof(name), NULL, NULL)) name[0] = 0;
        for (char *p = name; *p; p++) if (*p == '\r' || *p == '\n') *p = ' ';
        char line[200];
        int l = _snprintf(line, sizeof(line), "%c %016llx %08x %lu %lu %s\n", kind, (unsigned long long)rm->host, rm->room,
                          (unsigned long)cur, (unsigned long)mx, name);
        if (l < 0 || len + l >= outsz) break;
        memcpy(out + len, line, l + 1);
        len += l;
        n++;
    }
    free(rooms);
    return n;
}

int WINAPI OkplayTestJoin(const char *host_hex, const char *name)
{
    if (!ready() || !tc_c || tc_hosting || !host_hex) return -1;
    uint64_t host = strtoull(host_hex, NULL, 16);
    uint32_t room = 0;
    int found = 0;
    lstrcpynA(tc_name, name && name[0] ? name : "Player", sizeof(tc_name));
    EnterCriticalSection(&tc_c->lock);
    for (int i = 0; i < tc_c->nrooms && !found; i++) {
        okp_room *rm = &tc_c->rooms[i];
        if (rm->host != host || !okp_tc_blob_ok(rm->enumblob, rm->enumlen)) continue;
        found = 1;
        room = rm->room;
        tc_c->room = room;
        tc_c->host_peer = host;
        okp_peer *p = okp_peer_get(tc_c, host, OKP_PEER_CREATE);
        if (p) { okp_peer_add_cands(tc_c, p, rm->cands, rm->ncands); p->probe_start = okp_now(); }
    }
    LeaveCriticalSection(&tc_c->lock);
    if (!found) return -1;
    tc_host = host;
    tc_c->session_open = 1;
    okp_log(1, "test: joining room %08x of host %016llx", room, (unsigned long long)host);
    DWORD t0 = okp_now(), knocked = 0;
    int hello = 0;
    while (okp_now() - t0 < TC_JOIN_MS) {
        okp_peer_info_t pi;
        int path = okp_peer_info(tc_c, host, &pi) && pi.have_path;
        if (tc_c->mode == OKP_MODE_ONLINE && !path && (!knocked || okp_now() - knocked >= 3000)) {
            okp_rdv_knock(tc_c, room);   /* (again every 3 s until the host answers, as the drop-in's join) */
            knocked = okp_now() | 1;
        }
        if (path && !hello) {
            hello = 1;
            tc_send(host, 'H', tc_name, (int)strlen(tc_name), OKP_SEND_RELIABLE);
        }
        tc_tick();
        if (tc_refused) return -3;
        if (tc_in) return 1;
        Sleep(20);
    }
    okp_log(1, "test: the join gave up after %d s (%s)", TC_JOIN_MS / 1000, hello ? "no answer from the host" : "no connection");
    return hello ? -4 : -2;
}

int WINAPI OkplayTestSend(DWORD to, const void *buf, int len, int flags)
{
    int f = (flags & 1 ? OKP_SEND_GUARANTEED : 0) | (flags & 2 ? OKP_SEND_RELIABLE : 0), sent = 0;
    if (!tc_c || len < 0 || len > OKP_MSG_MAX - 4) return 0;
    for (int i = 0; i < TC_MAXP + 4; i++)
        if (tc_m[i].used && (!to || tc_m[i].id == to)) sent += tc_send(tc_m[i].peer, 'D', buf, len, f);
    return sent;
}

int WINAPI OkplayTestRecv(DWORD *from, int *kind, void *buf, int cap)
{
    if (!tc_c && !ev_head) return -1;
    tc_tick();
    tc_msg *m = q_pop(&ev_head, &ev_tail);
    if (!m) return -1;
    int n = m->len < cap ? m->len : cap;
    if (from) *from = m->id;
    if (kind) *kind = m->kind;
    if (n > 0 && buf) memcpy(buf, m->data, n);
    free(m);
    return n;
}

void WINAPI OkplayTestClose(void)
{
    tc_init();
    tc_stop();
}
