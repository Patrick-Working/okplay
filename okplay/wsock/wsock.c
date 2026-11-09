/* okplay wsock: a wsock32.dll for games that use Winsock directly (no DirectPlay) and only play on a
 * LAN: their UDP traffic is carried over okplay (Online Play rooms with hole punching and the relay,
 * or LAN Play), so they play over the internet with no port forwarding.
 *
 * How (black-box, from watching games through tools/wsocklog):
 *  - Every player gets a stand-in IPv4 address, 10.x.y.z, made from its okplay peer id; the game is told
 *    it is its own address (gethostbyname of its own name), so whatever the game writes into its packets
 *    (its address in adverts and join requests) is already right for the others. Nothing is rewritten.
 *  - The game's UDP sockets are virtual: bind/connect/sendto/send only record ports and peers, packets go
 *    to the peer over okplay with a small header (source and destination port), and arrive in the right
 *    socket's queue (a connected socket takes only its peer's packets, as Windows does). Ports are the
 *    shim's own, so a port that is busy on the real PC can't break a game.
 *  - A broadcast (255.255.255.255) by the game makes this PC the host of an okplay room: the room is
 *    listed (Online Play: oldkid.net's room server; LAN Play: okplay's LAN discovery) with the game's
 *    last broadcast inside it. Joiners get those broadcasts fed to their own socket on that port, so the
 *    game's own server browser lists the rooms; when the game then sends to a host's stand-in address,
 *    the shim joins that room.
 *  - Packets travel as okplay datagrams (unreliable): the games resend what they need themselves.
 *
 * First game: Road Rash (TCP/IP mode: adverts on 6995, join on 6996, one connected socket per link).
 * Settings: okplay.ini [okplay] wsock_mode=online|lan (default online); server, key, logs as the drop-in.
 * Calls other than UDP over IPv4 go to Windows' Winsock 2 (ws2_32.dll) unchanged.
 *
 * Status (2026-11-05): first version. Not yet: the Online/LAN + password window, IPX. */
#define WIN32_LEAN_AND_MEAN
#include "okp.h"
#include "rr_fix.h"
#include <mmsystem.h>
#include "okbox.h"
#include <commctrl.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

/* Winsock 1.1 / 2 share these numbers */
#define WS_FIONREAD 0x4004667fL
#define WS_FIONBIO  0x8004667eL

/* {7D3B2A61-4C5E-4F7A-9A1B-52524F414448} - "this is an okplay wsock room" (DPSESSIONDESC2.guidApplication
 * of the room record); the game is told apart by the exe build the room server already stores */
static const GUID OKW_APPGUID = { 0x7d3b2a61, 0x4c5e, 0x4f7a, { 0x9a, 0x1b, 0x52, 0x52, 0x4f, 0x41, 0x44, 0x48 } };

#pragma pack(push, 1)
typedef struct {
    uint8_t m0, m1;      /* 'W','K' */
    uint16_t sport;      /* host order */
    uint16_t dport;
    uint8_t flags;       /* 1 = was a broadcast */
} okw_hdr;
#pragma pack(pop)
#define OKW_BCAST 1
#define OKW_CTRL  2   /* okplay's own: 'A' + password hash (joiner -> host), 'Y' / 'N' (host -> joiner), 'V' (below) */

/* Game packets over a bad line (0.1.3). Road Rash resends what matters itself, so its packets stay
 * unreliable (a reliable lane would hold every packet behind a late one). Two helps instead, between
 * shims that both have them - each says so first with a CTRL 'V' + version; an older shim never answers
 * and keeps getting the 'W','K' packets above:
 *   - every game packet gets a sequence number: the log shows, per player and every 10 s, how many
 *     arrived, how many were lost, came late or out of order;
 *   - a packet carries a copy of the one before it (Online Play; okplay.ini wsock_redundancy=0/1), so a
 *     single lost packet is filled in by the next one. The copy that isn't needed is dropped here; the
 *     game never sees a packet twice.
 * Frame: 'W','S', version 1, entry count, then entries oldest first: flags, sport, dport, seq, len, data. */
#pragma pack(push, 1)
typedef struct { uint8_t flags; uint16_t sport, dport, seq, len; } okw_entry;
#pragma pack(pop)
typedef struct {
    uint64_t peer;
    int used, caps, sent_v, hellos;
    DWORD hello_tick, report_tick;
    uint16_t tx_seq;
    okw_entry prev;                 /* the last packet sent to this player (its copy goes with the next) */
    uint8_t prev_data[600];
    int have_prev;
    int rx_started;
    uint16_t rx_hi;                 /* newest sequence number seen */
    uint64_t rx_win;                /* bit i: rx_hi - i arrived */
    uint32_t n_new, n_lost, n_late, n_dup, n_filled;   /* since the last report */
} okw_link;

typedef struct okw_pkt {
    struct okw_pkt *next;
    uint32_t from_ip;    /* network order */
    uint16_t from_port;  /* host order */
    int len;
    uint8_t data[1];
} okw_pkt;

#define OKW_MAX_SOCKS 64
typedef struct {
    SOCKET s;            /* the real handle the game holds (never used for I/O) */
    int used;
    uint16_t port;       /* bound port, host order (0 = not bound) */
    uint32_t bound_ip;   /* network order, as the game asked (informational) */
    uint32_t peer_ip;    /* connected peer (0 = not connected) */
    uint16_t peer_port;
    int nonblock, bcast;
    okw_pkt *q, *qtail;
    int qbytes, qn;
} okw_sock;

static CRITICAL_SECTION okw_lock;
static okw_sock socks[OKW_MAX_SOCKS];
static okp_ctx *ctx;
static int started, want_lan;
/* the Online/LAN/Original choice: 0 = not made yet, 1 = the game's original TCP/IP (Windows' own Winsock,
 * okplay stays out), 2 = Online Play, 3 = LAN Play */
static volatile LONG mode_choice;
static CRITICAL_SECTION okw_ask_lock;   /* one box at a time; held while it is open (okw_lock is not) */
/* game passwords (as the drop-in: only "okp1-" + a salted SHA-256 travels) */
static uint32_t pending_room;           /* the host's room id, chosen before its password box */
static int host_pw_asked, host_has_pw;
static char host_hash[64];
static uint64_t authed[OKP_MAX_PEERS];  /* host: joiners that showed the right hash */
static int nauthed;
static uint64_t auth_host;              /* joiner: the password room it is joining */
static int auth_state;                  /* 0 not asked, 3 box open, 1 sent - waiting, 2 let in, -1 cancelled */
static uint32_t auth_room;
static char join_hash[64];
static DWORD auth_tick;
static volatile LONG auth_in_list;      /* the room list is waiting for the host's answer: it shows a "no" itself */
typedef struct { uint64_t peer; uint16_t sport, dport; int flags, len; uint8_t data[300]; } okw_held;
static okw_held held[64];               /* joiner: game packets waiting for the host's yes */
static int nheld;
static HMODULE g_self;
static okw_link links[OKP_MAX_PEERS];
static int redundancy;             /* send the previous packet along (Online Play by default) */
static okw_link *link_of(uint64_t peer, int create);
static uint16_t next_eph = 30000;
static char advert[1024];          /* host: the game's last broadcast */
static int advert_len;
static uint16_t advert_port;
static DWORD advert_tick, browse_tick, inject_tick;
static uint64_t okw_self;         /* our peer id, and so our stand-in address, for the whole game run */
static int mode_provisional;       /* okplay was started before the Online/LAN box was answered */
static DWORD socks_gone_tick;      /* when the game closed its last UDP socket */
static int socks_gone;             /* it has none open since then */
static DWORD room_closed_tick;     /* host: when the game closed its room */
static volatile DWORD last_race_tick;   /* the last in-race packet: notices wait until a race is over */
static int room_closed;

/* ---------------------------------------------------------------- stand-in addresses */

static uint32_t standin_of(uint64_t id)   /* network order: 10.a.b.c, c in 1..254 */
{
    uint32_t h = (uint32_t)(id ^ (id >> 32));
    h ^= h >> 16;
    h *= 0x45d9f3bu;
    h ^= h >> 16;
    uint8_t a = (uint8_t)(h >> 16), b = (uint8_t)(h >> 8), c = (uint8_t)(h % 254 + 1);
    return htonl((10u << 24) | ((uint32_t)a << 16) | ((uint32_t)b << 8) | c);
}

static int peer_of_standin(uint32_t ip, uint64_t *out)   /* okw_lock held (takes ctx->lock: always in that order) */
{
    int found = 0;
    if (!ctx) return 0;
    EnterCriticalSection(&ctx->lock);
    for (int i = 0; i < OKP_MAX_PEERS && !found; i++)
        if (ctx->peers[i].used && standin_of(ctx->peers[i].id) == ip) { *out = ctx->peers[i].id; found = 1; }
    for (int i = 0; i < ctx->nrooms && !found; i++)
        if (standin_of(ctx->rooms[i].host) == ip) { *out = ctx->rooms[i].host; found = 1; }
    LeaveCriticalSection(&ctx->lock);
    return found;
}

/* the game's instance id the password hash is salted with: made from the room id, the same on every PC */
static GUID room_guid(uint32_t room)
{
    GUID g = { room, 0x4f4b, 0x5753, { 'o', 'k', 'w', 's', 'o', 'c', 'k', 1 } };
    return g;
}

static void wide_to_ascii(const WCHAR *w, char *out, int n)
{
    int i;
    for (i = 0; i < n - 1 && w[i]; i++) out[i] = (char)w[i];
    out[i] = 0;
}

static const char *ip_s(uint32_t ip, char *b)
{
    const uint8_t *p = (const uint8_t *)&ip;
    sprintf(b, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    return b;
}

/* ---------------------------------------------------------------- the okplay side */

/* rdv.c / lan.c call these (in the DirectPlay build sp.c has them) */
void okp_self_probe(okp_ctx *c) { (void)c; }   /* the room record is refreshed from each broadcast */
void okp_inject_rooms(okp_ctx *c) { (void)c; } /* rooms reach the game from okw_tick */

static okw_sock *sock_of(SOCKET s)   /* okw_lock held */
{
    for (int i = 0; i < OKW_MAX_SOCKS; i++) if (socks[i].used && socks[i].s == s) return &socks[i];
    return NULL;
}

/* okplay.ini wsock_trace=1: every game packet the shim carries, into the log. Lines are kept in memory and
 * written by a thread of their own: the game sends from a multimedia-timer callback, and on Windows 9x file
 * I/O from there can stall it. */
#define TR_N 512
static char tr_lines[TR_N][128];
static DWORD tr_tick[TR_N];
static volatile LONG tr_w, tr_r;
static int tr_on;
static CRITICAL_SECTION tr_lock;
static void tr(const char *fmt, ...)
{
    if (!tr_on) return;
    EnterCriticalSection(&tr_lock);
    if (tr_w - tr_r < TR_N) {
        va_list ap;
        va_start(ap, fmt);
        _vsnprintf(tr_lines[tr_w % TR_N], 127, fmt, ap);
        va_end(ap);
        tr_lines[tr_w % TR_N][127] = 0;
        tr_tick[tr_w % TR_N] = GetTickCount();
        tr_w++;
    }
    LeaveCriticalSection(&tr_lock);
}
static DWORD WINAPI tr_writer(LPVOID arg)
{
    (void)arg;
    for (;;) {
        Sleep(300);
        while (tr_r < tr_w) {
            okp_log(1, "trace +%lu %s", (unsigned long)tr_tick[tr_r % TR_N], tr_lines[tr_r % TR_N]);
            InterlockedIncrement(&tr_r);
        }
    }
    return 0;
}

static void queue_pkt(okw_sock *k, uint32_t from_ip, uint16_t from_port, const void *data, int len)   /* lock held */
{
    if (k->qn > 256) { tr("rx DROP queue full port %u", k->port); return; }   /* a game that stopped reading: drop, as a full socket buffer does */
    okw_pkt *p = (okw_pkt *)malloc(sizeof(okw_pkt) + len);
    if (!p) return;
    p->next = NULL;
    p->from_ip = from_ip;
    p->from_port = from_port;
    p->len = len;
    memcpy(p->data, data, len);
    if (k->qtail) k->qtail->next = p; else k->q = p;
    k->qtail = p;
    k->qn++;
    k->qbytes += len;
}

/* deliver to the game: the socket bound to dport that is connected to this sender, else an unconnected one */
static void deliver(uint32_t from_ip, uint16_t sport, uint16_t dport, const void *data, int len)   /* lock held */
{
    int n = 0;
    for (int i = 0; i < OKW_MAX_SOCKS; i++) {
        okw_sock *k = &socks[i];
        if (!k->used || k->port != dport || !k->peer_ip) continue;
        if (k->peer_ip == from_ip && k->peer_port == sport) {
            tr("rx %u<-%u len %d type %d (connected, q %d)", dport, sport, len, len ? ((const uint8_t *)data)[0] : -1, k->qn);
            queue_pkt(k, from_ip, sport, data, len);
            return;
        }
    }
    /* Every unconnected socket on that port gets it. A game can end up with two: Road Rash made a second
     * browser socket on 6995 while the Online/LAN box was open, then listened on that one. */
    for (int i = 0; i < OKW_MAX_SOCKS; i++) {
        okw_sock *k = &socks[i];
        if (!k->used || k->port != dport || k->peer_ip) continue;
        tr("rx %u<-%u len %d type %d (unconnected, sock %u, q %d)", dport, sport, len, len ? ((const uint8_t *)data)[0] : -1,
           (unsigned)k->s, k->qn);
        queue_pkt(k, from_ip, sport, data, len);
        n++;
    }
    if (!n) {
        char t[20];
        tr("rx DROP %u<-%s:%u len %d type %d: no socket for it", dport, ip_s(from_ip, t), sport, len, len ? ((const uint8_t *)data)[0] : -1);
    }
}

static int send_to_peer(uint64_t peer, uint16_t sport, uint16_t dport, int flags, const void *buf, int len);

static int is_authed(uint64_t peer)   /* okw_lock held */
{
    for (int i = 0; i < nauthed; i++) if (authed[i] == peer) return 1;
    return 0;
}

static DWORD WINAPI wrong_password_box(LPVOID arg)
{
    (void)arg;
    okbox_wrong_password();
    return 0;
}

#ifdef OKW_LAB
void rr_lab_ctrl(uint64_t from, const uint8_t *b, int n);
#endif
static void control(uint64_t from, const uint8_t *b, int n)   /* okw_lock held */
{
    if (n < 1) return;
    if (b[0] == 'R') { rrf_ctrl(from, b + 1, n - 1); return; }   /* rr_fix.c: the clock-offset exchange */
#ifdef OKW_LAB
    if (b[0] == 'L') { rr_lab_ctrl(from, b + 1, n - 1); return; }
#endif
    if (b[0] == 'V') {   /* the other shim speaks the sequenced format */
        okw_link *L = link_of(from, 1);
        if (L && !L->caps) {
            L->caps = 1;
            okp_log(2, "wsock: player %016llx has okplay 0.1.3+ - numbered packets%s", (unsigned long long)from,
                    redundancy ? ", each with a copy of the one before" : "");
        }
        if (L && !L->sent_v) { L->sent_v = 1; send_to_peer(from, 0, 0, OKW_CTRL, "V\x01", 2); }
        return;
    }
    if (b[0] == 'A' && ctx && ctx->is_host) {   /* a joiner's password */
        char got[64];
        int k = n - 1 < 63 ? n - 1 : 63;
        memcpy(got, b + 1, k);
        got[k] = 0;
        int ok = !host_has_pw || !strcmp(got, host_hash);
        if (ok && !is_authed(from) && nauthed < OKP_MAX_PEERS) authed[nauthed++] = from;
        okp_log(1, "wsock: peer %016llx %s", (unsigned long long)from, ok ? "let in (password matched)" : "turned away (wrong password)");
        send_to_peer(from, 0, 0, OKW_CTRL, ok ? "Y" : "N", 1);
    } else if ((b[0] == 'Y' || b[0] == 'N') && from == auth_host && auth_state == 1) {
        if (b[0] == 'Y') {
            auth_state = 2;
            okp_log(1, "wsock: the host let us in - %d held packet(s) go now", nheld);
            for (int i = 0; i < nheld; i++)
                send_to_peer(held[i].peer, held[i].sport, held[i].dport, held[i].flags, held[i].data, held[i].len);
        } else {
            auth_state = 0;   /* the game's next join attempt asks again */
            SecureZeroMemory(join_hash, sizeof(join_hash));
            okp_log(1, "wsock: wrong password - the host did not let us in");
            if (!getenv("OKP_TEST_JOIN_PASSWORD") && !auth_in_list) {
                HANDLE t = CreateThread(NULL, 0, wrong_password_box, NULL, 0, NULL);
                if (t) CloseHandle(t);
            }
        }
        nheld = 0;
    }
}

static okw_link *link_of(uint64_t peer, int create)   /* okw_lock held */
{
    okw_link *freel = NULL;
    for (int i = 0; i < OKP_MAX_PEERS; i++) {
        if (links[i].used && links[i].peer == peer) return &links[i];
        if (!links[i].used && !freel) freel = &links[i];
    }
    if (!create || !freel) return NULL;
    memset(freel, 0, sizeof(*freel));
    freel->used = 1;
    freel->peer = peer;
    freel->report_tick = GetTickCount();
    return freel;
}

/* one sequenced packet arrived: 1 = hand it to the game, 0 = a copy of one it already has */
static int seq_accept(okw_link *L, uint16_t seq, int is_copy)
{
    if (!L->rx_started) {
        L->rx_started = 1;
        L->rx_hi = seq;
        L->rx_win = 1;
        L->n_new++;
        return 1;
    }
    int d = (int16_t)(uint16_t)(seq - L->rx_hi);
    if (d > 0) {   /* newer than any so far: anything skipped counts as lost until it turns up */
        L->n_lost += d - 1;
        L->rx_win = d >= 64 ? 1 : (L->rx_win << d) | 1;
        L->rx_hi = seq;
        L->n_new++;
        if (is_copy) L->n_filled++;
        return 1;
    }
    int back = -d;
    if (back >= 64) { L->n_late++; L->n_new++; return 1; }   /* very late: the game decides */
    if (L->rx_win & (1ull << back)) { L->n_dup++; return 0; }
    L->rx_win |= 1ull << back;   /* a gap filled: by the copy, or the packet itself coming late */
    if (L->n_lost) L->n_lost--;
    if (is_copy) L->n_filled++;
    else L->n_late++;
    L->n_new++;
    return 1;
}

static void link_reports(void)   /* okw_lock held; every 10 s per player, when something moved */
{
    DWORD now = GetTickCount();
    for (int i = 0; i < OKP_MAX_PEERS; i++) {
        okw_link *L = &links[i];
        if (!L->used || now - L->report_tick < 10000) continue;
        L->report_tick = now;
        if (!L->n_new && !L->n_lost) continue;
        uint32_t all = L->n_new + L->n_lost;
        okp_log(2, "wsock: player %016llx, last 10 s: %u game packets, %u lost (%u.%u%%), %u filled in from the copy, "
                "%u late or out of order, %u spare copies dropped", (unsigned long long)L->peer, L->n_new, L->n_lost,
                L->n_lost * 100 / all, L->n_lost * 1000 / all % 10, L->n_filled, L->n_late, L->n_dup);
        L->n_new = L->n_lost = L->n_late = L->n_dup = L->n_filled = 0;
    }
}

/* ---------------------------------------------------------------- race measurement and test knobs
 *
 * Road Rash's race state (type 10, 76 bytes): byte 1 is a presence mask for up to three 24-byte records at
 * +2, +26, +50; a record's first byte is kind (high nibble) and index (low nibble), and bytes 1..4 are the
 * sender's step counter (little-endian). Per sender and record stream the shim logs, every 10 s, how far
 * apart consecutive stamps are (in the sender's steps), how many steps per second the stamps advance, and
 * the longest wait between two arrivals. Large stamp gaps are what the game turns into jumps.
 *
 * Test knobs (okplay.ini, for lab runs; all 0 = off): test_delay_ms, test_jitter_ms (uniform 0..N extra),
 * test_blackout_ms + test_blackout_every_ms (drop everything from the other players for N ms every M ms),
 * test_loss_pct, test_seed. They act on game packets arriving from other players, before the game sees them. */
typedef struct {
    uint64_t peer;
    int used;
    uint32_t last_stamp[3][16];
    DWORD last_ms[3][16];
    uint8_t seen[3][16];
    uint32_t hist[66];          /* stamp gap 1..64 steps, 65 = more */
    uint32_t n, over20, maxgap, max_wait_ms, pkts;
    uint64_t stamp_sum, ms_sum;
    DWORD report_ms;
    int dumped;
} okw_race;
static okw_race races[OKP_MAX_PEERS];
#ifdef OKW_LAB   /* lab builds only (build.sh OKW_LAB_SRC=...): game-specific measurement kept outside this repo */
void rr_lab_init(void);
void rr_lab_knobs(const char *ini);
void rr_lab_game_out(const uint8_t *d, int len);            /* a game packet this player sends */
void rr_lab_game_in(uint64_t from, const uint8_t *d, int len);   /* a game packet from another player, as delivered */
int rr_lab_game_filter(uint64_t from, const uint8_t *d, int len, uint8_t *copy);   /* 1: deliver `copy` instead */
void rr_lab_ctrl(uint64_t from, const uint8_t *b, int n);   /* an 'L' message from another player's lab build */
int rr_lab_ctrl_due(uint8_t *b, int max);                   /* a message for every other player now, or 0 */
#endif
static int t_delay, t_jitter, t_bo_len, t_bo_every, t_loss;
static uint32_t t_rng = 1;
static DWORD t_start_ms;
static uint32_t t_dropped;
typedef struct { DWORD due; uint64_t from; uint16_t sport, dport; int len; uint8_t d[300]; } okw_pend;
static okw_pend pend[512];
static int npend;

static DWORD now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart && !QueryPerformanceFrequency(&f)) return GetTickCount();
    QueryPerformanceCounter(&c);
    return (DWORD)(c.QuadPart * 1000 / f.QuadPart);
}

static okw_race *race_of(uint64_t peer)
{
    okw_race *fr = NULL;
    for (int i = 0; i < OKP_MAX_PEERS; i++) {
        if (races[i].used && races[i].peer == peer) return &races[i];
        if (!races[i].used && !fr) fr = &races[i];
    }
    if (!fr) return NULL;
    memset(fr, 0, sizeof(*fr));
    fr->used = 1;
    fr->peer = peer;
    fr->report_ms = now_ms();
    return fr;
}

static void race_measure(uint64_t from, const uint8_t *p, int len)   /* okw_lock held */
{
#ifdef OKW_LAB
    rr_lab_game_in(from, p, len);
#endif
    if (len < 2 || p[0] != 10) return;
    last_race_tick = GetTickCount();
    okw_race *R = race_of(from);
    if (!R) return;
    DWORD now = now_ms();
    R->pkts++;
    if (R->dumped < 12) {   /* the first few race packets in the trace, to check the layout */
        R->dumped++;
        tr("race pkt from %016llx len %d mask %02x | %02x %02x%02x%02x%02x | %02x %02x%02x%02x%02x | %02x %02x%02x%02x%02x",
           (unsigned long long)from, len, p[1], p[2], p[3], p[4], p[5], p[6], len > 30 ? p[26] : 0, len > 30 ? p[27] : 0,
           len > 30 ? p[28] : 0, len > 30 ? p[29] : 0, len > 30 ? p[30] : 0, len > 54 ? p[50] : 0, len > 54 ? p[51] : 0,
           len > 54 ? p[52] : 0, len > 54 ? p[53] : 0, len > 54 ? p[54] : 0);
    }
    for (int slot = 0; slot < 3; slot++) {
        if (!(p[1] & (1 << slot))) continue;
        int off = 2 + 24 * slot;
        if (off + 5 > len) break;
        int kind = p[off] >> 4, idx = p[off] & 15;
        if (kind > 2) continue;
        uint32_t st;
        memcpy(&st, p + off + 1, 4);
        if (R->seen[kind][idx]) {
            int d = (int32_t)(st - R->last_stamp[kind][idx]);
            if (d > 0) {
                R->hist[d > 64 ? 65 : d]++;
                R->n++;
                if (d > 20) R->over20++;
                if ((uint32_t)d > R->maxgap) R->maxgap = d;
                DWORD w = now - R->last_ms[kind][idx];
                if (w > R->max_wait_ms) R->max_wait_ms = w;
                R->stamp_sum += d;
                R->ms_sum += w;
            }
        }
        R->seen[kind][idx] = 1;
        R->last_stamp[kind][idx] = st;
        R->last_ms[kind][idx] = now;
    }
}

static void race_reports(void)   /* okw_lock held */
{
    DWORD now = now_ms();
    for (int i = 0; i < OKP_MAX_PEERS; i++) {
        okw_race *R = &races[i];
        if (!R->used || now - R->report_ms < 10000) continue;
        R->report_ms = now;
        if (!R->n) { R->pkts = 0; continue; }
        uint32_t med = 0, p99 = 0, acc = 0;
        for (int g = 1; g <= 65; g++) {
            acc += R->hist[g];
            if (!med && acc * 2 >= R->n) med = g;
            if (!p99 && acc * 100 >= R->n * 99) p99 = g;
        }
        okp_log(2, "wsock: race from %016llx, last 10 s: %u packets, %u stamp steps; gap median %u, p99 %u%s, max %u "
                "steps, %u over 20; stamps run %u.%u steps/s; longest wait %u ms%s",
                (unsigned long long)R->peer, R->pkts, R->n, med, p99, p99 == 65 ? "+" : "", R->maxgap, R->over20,
                R->ms_sum ? (uint32_t)(R->stamp_sum * 1000 / R->ms_sum) : 0,
                R->ms_sum ? (uint32_t)(R->stamp_sum * 10000 / R->ms_sum % 10) : 0, R->max_wait_ms,
                t_dropped ? " (test knobs on)" : "");
        memset(R->hist, 0, sizeof(R->hist));
        R->n = R->over20 = R->maxgap = R->max_wait_ms = R->pkts = 0;
        R->stamp_sum = R->ms_sum = 0;
    }
}

static uint32_t t_rand(void)
{
    t_rng ^= t_rng << 13;
    t_rng ^= t_rng >> 17;
    t_rng ^= t_rng << 5;
    return t_rng;
}

/* a game packet from another player: measured, then to the game - now, or later with the test knobs */
static void game_in(uint64_t from, uint16_t sport, uint16_t dport, const uint8_t *d, int len)   /* okw_lock held */
{
    if (t_loss && (int)(t_rand() % 100) < t_loss) { t_dropped++; return; }
    uint8_t rr_copy[512];
    if (len <= (int)sizeof(rr_copy) && rrf_game_filter(d, len, rr_copy)) d = rr_copy;
#ifdef OKW_LAB
    uint8_t lab_copy[512];
    if (len <= (int)sizeof(lab_copy) && rr_lab_game_filter(from, d, len, lab_copy)) d = lab_copy;
#endif
    if (!t_delay && !t_jitter && !t_bo_len) {
        race_measure(from, d, len);
        deliver(standin_of(from), sport, dport, d, len);
        return;
    }
    DWORD now = now_ms();
    if (!t_start_ms) t_start_ms = now;
    if (t_bo_len && t_bo_every && (now - t_start_ms) % (DWORD)t_bo_every < (DWORD)t_bo_len) { t_dropped++; return; }
    if (npend >= (int)(sizeof(pend) / sizeof(pend[0])) || len > (int)sizeof(pend[0].d)) { t_dropped++; return; }
    okw_pend *q = &pend[npend++];
    q->due = now + t_delay + (t_jitter ? t_rand() % (DWORD)(t_jitter + 1) : 0);
    q->from = from; q->sport = sport; q->dport = dport; q->len = len;
    memcpy(q->d, d, len);
}

static void release_due(void)   /* okw_lock held: packets whose test delay is over, earliest first */
{
    DWORD now = now_ms();
    for (;;) {
        int best = -1;
        for (int i = 0; i < npend; i++)
            if ((int32_t)(now - pend[i].due) >= 0 && (best < 0 || (int32_t)(pend[i].due - pend[best].due) < 0)) best = i;
        if (best < 0) return;
        okw_pend q = pend[best];
        pend[best] = pend[--npend];
        race_measure(q.from, q.d, q.len);
        deliver(standin_of(q.from), q.sport, q.dport, q.d, q.len);
    }
}

static void okw_rx(void *c, uint64_t from, const void *data, int len)   /* okplay's net thread */
{
    (void)c;
    if (len < (int)sizeof(okw_hdr)) return;
    const okw_hdr *h = (const okw_hdr *)data;
    const uint8_t *d8 = (const uint8_t *)data;
    if (d8[0] == 'W' && d8[1] == 'S' && len >= 4 && d8[2] == 1) {   /* sequenced (0.1.3+) */
        EnterCriticalSection(&okw_lock);
        if (ctx && c == ctx && !(ctx->is_host && host_has_pw && !is_authed(from))) {
            okw_link *L = link_of(from, 1);
            int n = d8[3], off = 4;
            if (L) L->caps = 1;
            for (int i = 0; i < n && L; i++) {
                okw_entry e;
                if (off + (int)sizeof(e) > len) break;
                memcpy(&e, d8 + off, sizeof(e));
                off += sizeof(e);
                if (off + e.len > len) break;
                if (seq_accept(L, e.seq, i < n - 1)) game_in(from, e.sport, e.dport, d8 + off, e.len);
                off += e.len;
            }
            release_due();
        }
        LeaveCriticalSection(&okw_lock);
        return;
    }
    if (h->m0 != 'W' || h->m1 != 'K') return;
    const uint8_t *body = (const uint8_t *)data + sizeof(*h);
    int blen = len - (int)sizeof(*h);
    EnterCriticalSection(&okw_lock);
    if (!ctx || c != ctx) {   /* okplay of an earlier network session, being stopped */
        LeaveCriticalSection(&okw_lock);
        return;
    }
    if (h->flags & OKW_CTRL) {
        control(from, body, blen);
    } else if (ctx && ctx->is_host && host_has_pw && !is_authed(from)) {
        /* a password game: nothing from a joiner reaches the game before its password matched */
    } else {
        game_in(from, h->sport, h->dport, body, blen);
        release_due();
    }
    LeaveCriticalSection(&okw_lock);
}

static int send_to_peer(uint64_t peer, uint16_t sport, uint16_t dport, int flags, const void *buf, int len)
{
    uint8_t pkt[sizeof(okw_hdr) + 2048];
    if (len > 2048 || !ctx) return 0;
    if (!(flags & OKW_CTRL) && peer == auth_host && auth_state != 2) {   /* a password room: wait for its yes */
        if ((auth_state == 1 || auth_state == 3) && nheld < 64 && len <= (int)sizeof(held[0].data)) {
            okw_held *e = &held[nheld++];
            e->peer = peer; e->sport = sport; e->dport = dport; e->flags = flags; e->len = len;
            memcpy(e->data, buf, len);
        }
        return 1;
    }
    if (!(flags & OKW_CTRL)) {
        okw_link *L = link_of(peer, 1);
        if (L && !L->caps && L->hellos < 20 && GetTickCount() - L->hello_tick >= 1000) {   /* ask once a second, 20 s */
            L->hello_tick = GetTickCount();
            L->hellos++;
            L->sent_v = 1;
            send_to_peer(peer, 0, 0, OKW_CTRL, "V\x01", 2);
        }
        if (L && L->caps && len <= (int)sizeof(L->prev_data)) {
            uint8_t f[4 + 2 * sizeof(okw_entry) + 2 * 600];
            int off = 4, n = 0;
            f[0] = 'W'; f[1] = 'S'; f[2] = 1;
            if (redundancy && L->have_prev) {
                memcpy(f + off, &L->prev, sizeof(L->prev));
                memcpy(f + off + sizeof(L->prev), L->prev_data, L->prev.len);
                off += sizeof(L->prev) + L->prev.len;
                n++;
            }
            okw_entry e;
            e.flags = (uint8_t)flags; e.sport = sport; e.dport = dport; e.seq = ++L->tx_seq; e.len = (uint16_t)len;
            memcpy(f + off, &e, sizeof(e));
            memcpy(f + off + sizeof(e), buf, len);
            off += sizeof(e) + len;
            n++;
            f[3] = (uint8_t)n;
            L->prev = e;
            memcpy(L->prev_data, buf, len);
            L->have_prev = 1;
            return okp_send_data(ctx, peer, f, off, 0);
        }
    }
    okw_hdr *h = (okw_hdr *)pkt;
    h->m0 = 'W';
    h->m1 = 'K';
    h->sport = sport;
    h->dport = dport;
    h->flags = (uint8_t)flags;
    memcpy(pkt + sizeof(*h), buf, len);
    return okp_send_data(ctx, peer, pkt, (int)sizeof(*h) + len, 0);
}

/* the room record the room server and LAN discovery carry: a DirectPlay-style session record (the format
 * okplay already lists and checks), with the game's last broadcast after the name */
static void build_room_record(void)   /* okw_lock held */
{
    uint8_t b[1024];
    memset(b, 0, sizeof(b));
    memcpy(b, "play", 4);
    uint16_t cmd = 1, ver = 14;
    memcpy(b + 4, &cmd, 2);
    memcpy(b + 6, &ver, 2);
    uint8_t *sd = b + 8;
    uint32_t sdsize = 80, maxp = 8, curp = 1;
    memcpy(sd, &sdsize, 4);
    memcpy(sd + 8, &ctx->room, 4);                 /* guidInstance (enough to tell rooms apart) */
    memcpy(sd + 24, &OKW_APPGUID, 16);             /* guidApplication */
    /* Road Rash's broadcast: byte 17 = free slots, bytes 37.. = host name */
    char name[64] = "Road Rash";
    if (advert_len >= 57 && advert_port == 6995) {
        curp = 8 - (advert[17] < 8 ? advert[17] : 7);
        memcpy(name, advert + 37, 20);
        name[20] = 0;
        if (!name[0]) strcpy(name, "Road Rash");
    }
    memcpy(sd + 40, &maxp, 4);
    memcpy(sd + 44, &curp, 4);
    if (host_has_pw) { uint32_t fl = 0x400; memcpy(sd + 4, &fl, 4); }   /* DPSESSION_PASSWORDREQUIRED: the list shows a lock */
    uint32_t off = 92;
    memcpy(b + 88, &off, 4);
    int n = 92;
    for (int i = 0; name[i] && n < 600; i++) { b[n++] = (uint8_t)name[i]; b[n++] = 0; }
    b[n++] = 0;
    b[n++] = 0;
    /* then 'OKWA', port, length, the broadcast */
    if (advert_len > 0 && n + 8 + advert_len <= (int)sizeof(b)) {
        memcpy(b + n, "OKWA", 4);
        memcpy(b + n + 4, &advert_port, 2);
        uint16_t al = (uint16_t)advert_len;
        memcpy(b + n + 6, &al, 2);
        memcpy(b + n + 8, advert, advert_len);
        n += 8 + advert_len;
    }
    EnterCriticalSection(&ctx->lock);
    memcpy(ctx->enum_blob, b, n);
    ctx->enum_len = n;
    LeaveCriticalSection(&ctx->lock);
}

static int room_advert(const okp_room *rm, uint16_t *port, const uint8_t **ad)
{
    for (int i = 92; i + 8 <= rm->enumlen; i++) {
        if (memcmp(rm->enumblob + i, "OKWA", 4)) continue;
        uint16_t al;
        memcpy(port, rm->enumblob + i + 4, 2);
        memcpy(&al, rm->enumblob + i + 6, 2);
        if (i + 8 + al > rm->enumlen) return 0;
        *ad = rm->enumblob + i + 8;
        return al;
    }
    return 0;
}

static void start_okplay(void)   /* okw_lock held; the first time the game uses UDP */
{
    if (started) return;
    started = 1;
    okp_config_load();
    want_lan = mode_choice == 3;
    okp_ctx *c = (okp_ctx *)calloc(1, sizeof(okp_ctx));
    if (!c) return;
    c->host_transport = -1;
    InitializeCriticalSection(&c->lock);
    c->mode = want_lan ? OKP_MODE_LAN : OKP_MODE_ONLINE;
    c->mode_name = want_lan ? "LAN Play" : "Online Play";
    c->sock = INVALID_SOCKET;
    c->lan_q = INVALID_SOCKET;
    c->lan_listen = INVALID_SOCKET;
    if (!okw_self) okw_self = okp_rand64();
    c->self = okw_self;   /* the same id (and stand-in address) if okplay restarts in this session */
    c->raw_rx = okw_rx;
    c->enum_app = OKW_APPGUID;
    okp_log_header();
    okp_log_sysinfo();
    if (!okp_net_start(c) || !okp_rdv_start(c)) {
        okp_log(0, "wsock: okplay could not start");
        return;
    }
    ctx = c;
    {
        char ini[MAX_PATH];
        snprintf(ini, sizeof(ini), "%sokplay.ini", okp_dll_dir());
        int r = GetPrivateProfileIntA("okplay", "wsock_redundancy", -1, ini);
        redundancy = r >= 0 ? r != 0 : !want_lan;   /* default: on for Online Play, off on a LAN */
        t_delay = GetPrivateProfileIntA("okplay", "test_delay_ms", 0, ini);
        t_jitter = GetPrivateProfileIntA("okplay", "test_jitter_ms", 0, ini);
        t_bo_len = GetPrivateProfileIntA("okplay", "test_blackout_ms", 0, ini);
        t_bo_every = GetPrivateProfileIntA("okplay", "test_blackout_every_ms", 0, ini);
        t_loss = GetPrivateProfileIntA("okplay", "test_loss_pct", 0, ini);
        t_rng = (uint32_t)GetPrivateProfileIntA("okplay", "test_seed", 1, ini) | 1;
        if (t_delay || t_jitter || t_bo_len || t_loss)
            okp_log(1, "wsock: TEST KNOBS: incoming game packets delayed %d ms + 0..%d ms jitter, blackout %d ms every %d ms (seed %u)",
                    t_delay, t_jitter, t_bo_len, t_bo_every, t_rng);
    }
    char b[20];
    okp_log(1, "wsock: okplay " OKP_VERSION_STR " %s for a Winsock game, peer %016llx, stand-in address %s", c->mode_name,
            (unsigned long long)c->self, ip_s(standin_of(c->self), b));
}

/* The game left network play (it closed its last UDP socket a while ago): okplay stops and every answer
 * is forgotten, so the next time the player picks TCP/IP the Online/LAN box and the host's password box
 * come up again, as the drop-in asks at every new game. (A game that closes a socket and opens the next
 * one at once - Road Rash's host learns its port with a throwaway socket - stays in the same session.) */
static void stop_ctx(okp_ctx *c, int withdraw)   /* no lock held; c already detached from ctx */
{
    if (!c) return;
    c->closing = 1;
    c->session_open = 0;
    c->is_host = 0;
    if (withdraw) okp_rdv_close(c);
    c->running = 0;
    okp_rdv_stop(c);
    okp_net_stop(c);
    DeleteCriticalSection(&c->lock);
    free(c);
}

static void end_session(void)   /* no lock held */
{
    EnterCriticalSection(&okw_lock);
    okp_ctx *c = ctx;
    int withdraw = c && c->room && (c->is_host || c->closing) && c->mode == OKP_MODE_ONLINE;
    ctx = NULL;
    started = 0;   /* (okw_self stays: a game may keep the own address it looked up once) */
    mode_provisional = 0;
    InterlockedExchange(&mode_choice, 0);
    host_pw_asked = host_has_pw = 0;
    SecureZeroMemory(host_hash, sizeof(host_hash));
    pending_room = 0;
    nauthed = 0;
    auth_host = 0;
    auth_state = 0;
    auth_room = 0;
    SecureZeroMemory(join_hash, sizeof(join_hash));
    nheld = 0;
    advert_len = 0;
    advert_port = 0;
    browse_tick = inject_tick = 0;
    socks_gone = 0;
    room_closed = 0;
    memset(links, 0, sizeof(links));
    memset(races, 0, sizeof(races));
    npend = 0;
    t_start_ms = 0;
    LeaveCriticalSection(&okw_lock);
    if (!c) return;
    okp_log(1, "wsock: the game left network play - okplay stops; the next TCP/IP game asks again");
    if (c->sess_role == 2 && c->host_peer) {   /* a joiner that never heard from its host */
        int heard = 0;
        EnterCriticalSection(&c->lock);
        for (int i = 0; i < OKP_MAX_PEERS; i++)
            if (c->peers[i].used && c->peers[i].id == c->host_peer && c->peers[i].rx_pkts) heard = 1;
        LeaveCriticalSection(&c->lock);
        if (!heard) okp_report_session_result(c, "no_path");
    }
    okp_report_session_end(c);   /* report.c: one connection line for this game */
    stop_ctx(c, withdraw);
}

/* A new game, so the mode and the host's password are asked again: the game has had no UDP socket open for
 * 2 s (Disconnect), or it closed its room and broadcasts again with no player link alive (the host backed
 * out of its lobby and hosts again). Not at a race start: the host closes its room then, but its players
 * are still there. No lock held. */
static void check_new_game(void)
{
    int over = 0;
    EnterCriticalSection(&okw_lock);
    if (socks_gone && GetTickCount() - socks_gone_tick >= 2000) over = 1;
    else if (room_closed && GetTickCount() - room_closed_tick >= 2000 && ctx) {
        int live = 0;
        DWORD now = okp_now();
        EnterCriticalSection(&ctx->lock);
        for (int i = 0; i < OKP_MAX_PEERS; i++)
            if (ctx->peers[i].used && ctx->peers[i].have_path && now - ctx->peers[i].last_rx < 5000) live = 1;
        LeaveCriticalSection(&ctx->lock);
        if (!live) over = 1;
    }
    LeaveCriticalSection(&okw_lock);
    if (over) end_session();
}

/* become the host of a room (the game broadcast) */
static void host_room(void)   /* okw_lock held */
{
    if (!ctx || ctx->is_host) return;
    ctx->room = pending_room ? pending_room : ((uint32_t)okp_rand64() | 1);
    snprintf(ctx->tok, sizeof(ctx->tok), "%016llx", (unsigned long long)okp_rand64());
    ctx->is_host = 1;
    ctx->closing = 0;
    room_closed = 0;
    build_room_record();
    ctx->session_open = 1;
    if (ctx->mode == OKP_MODE_LAN && !okp_lan_open_listen(ctx))
        okp_log(0, "wsock: LAN discovery port %u unavailable - joiners will not see this game", OKP_LAN_PORT);
    okp_log(1, "wsock: hosting room %08x", ctx->room);
    okp_report_session_begin(ctx, 1);
    okp_rdv_wake(ctx);
}

/* join the room whose host is `host` (the game sent to that host's stand-in address) */
static void join_room(uint64_t host)   /* okw_lock held */
{
    if (!ctx || ctx->is_host || ctx->host_peer == host) return;
    EnterCriticalSection(&ctx->lock);
    for (int i = 0; i < ctx->nrooms; i++) {
        okp_room *rm = &ctx->rooms[i];
        if (rm->host != host) continue;
        ctx->room = rm->room;
        ctx->host_peer = host;
        okp_peer *p = okp_peer_get(ctx, host, 1);
        if (p) { okp_peer_add_cands(ctx, p, rm->cands, rm->ncands); p->probe_start = okp_now(); }
        uint32_t room = rm->room;
        int nc = rm->ncands;
        LeaveCriticalSection(&ctx->lock);
        ctx->session_open = 1;
        okp_log(1, "wsock: joining room %08x host %016llx (%d candidates)", room, (unsigned long long)host, nc);
        okp_report_session_begin(ctx, 0);
        if (ctx->mode == OKP_MODE_ONLINE) okp_rdv_knock(ctx, room);
        return;
    }
    LeaveCriticalSection(&ctx->lock);
}

/* undo join_room after the host said no: the list goes back to the room list (with its lock marker)
 * instead of that host's own broadcasts, so picking the room again asks again */
static void leave_room(uint64_t host)   /* okw_lock held */
{
    if (!ctx || ctx->is_host || ctx->host_peer != host) return;
    okp_report_session_result(ctx, "refused");   /* the host turned us away (password): this try ends */
    okp_report_session_end(ctx);
    EnterCriticalSection(&ctx->lock);
    okp_peer *p = okp_peer_get(ctx, host, 0);
    if (p) {
        okp_peer_free_bufs(p);
        p->qn = 0;
        p->used = 0;
    }
    ctx->host_peer = 0;
    ctx->room = 0;
    LeaveCriticalSection(&ctx->lock);
    ctx->session_open = 0;
    browse_tick = 0;
    okp_log(2, "wsock: left room of host %016llx", (unsigned long long)host);
}

/* every call from the game: while it isn't hosting or in a room and has a socket waiting for broadcasts
 * (a server browser), list the rooms and feed their broadcasts to that socket */
static void okw_tick(void)   /* okw_lock held */
{
    if (!started && mode_choice >= 2) start_okplay();   /* (re)started in the mode the player picked */
    link_reports();
    release_due();
    race_reports();
    {   /* rr_fix.c: the clock-offset exchange with the other players' okplay */
        uint8_t rb[256];
        int rn = rrf_ctrl_due(rb + 1, (int)sizeof(rb) - 1);
        if (rn > 0) {
            rb[0] = 'R';
            for (int i = 0; i < OKP_MAX_PEERS; i++)
                if (links[i].used) send_to_peer(links[i].peer, 0, 0, OKW_CTRL, rb, rn + 1);
        }
    }
#ifdef OKW_LAB
    {   /* the lab build's own messages between players (lab measurement only) */
        uint8_t lb[256];
        int ln = rr_lab_ctrl_due(lb + 1, (int)sizeof(lb) - 1);
        if (ln > 0) {
            lb[0] = 'L';
            for (int i = 0; i < OKP_MAX_PEERS; i++)
                if (links[i].used) send_to_peer(links[i].peer, 0, 0, OKW_CTRL, lb, ln + 1);
        }
    }
#endif
    {   /* the test knobs are read again every 2 s, so a lab script can change them between races */
        static DWORD knob_ms;
        DWORD t = now_ms();
        if (t - knob_ms >= 2000) {
            char ini[MAX_PATH];
            int d, j, bl, be, lo;
            knob_ms = t;
            snprintf(ini, sizeof(ini), "%sokplay.ini", okp_dll_dir());
            WritePrivateProfileStringA(NULL, NULL, NULL, ini);   /* Windows 9x caches .ini files: re-read it */
            d = GetPrivateProfileIntA("okplay", "test_delay_ms", 0, ini);
            j = GetPrivateProfileIntA("okplay", "test_jitter_ms", 0, ini);
            bl = GetPrivateProfileIntA("okplay", "test_blackout_ms", 0, ini);
            be = GetPrivateProfileIntA("okplay", "test_blackout_every_ms", 0, ini);
            lo = GetPrivateProfileIntA("okplay", "test_loss_pct", 0, ini);
            rrf_knobs(ini);
#ifdef OKW_LAB
            rr_lab_knobs(ini);
#endif
            if (d != t_delay || j != t_jitter || bl != t_bo_len || be != t_bo_every || lo != t_loss) {
                t_delay = d; t_jitter = j; t_bo_len = bl; t_bo_every = be; t_loss = lo;
                t_start_ms = 0;
                okp_log(1, "wsock: TEST KNOBS now: delay %d ms + 0..%d ms jitter, %d%% loss, blackout %d ms every %d ms", d, j, lo, bl, be);
            }
        }
    }
    DWORD now = okp_now();
    if (ctx && auth_state == 1 && now - auth_tick >= 500) {   /* our password, until the host answers */
        uint8_t a[64];
        int n = (int)strlen(join_hash);
        a[0] = 'A';
        memcpy(a + 1, join_hash, n);
        auth_tick = now;
        send_to_peer(auth_host, 0, 0, OKW_CTRL, a, n + 1);
    }
    if (!ctx || ctx->is_host || ctx->host_peer) return;
    int browsing = 0;
    for (int i = 0; i < OKW_MAX_SOCKS; i++) if (socks[i].used && socks[i].port && !socks[i].peer_ip) browsing = 1;
    if (!browsing) return;
    if (now - browse_tick >= 1500) {   /* the room list (Online) / the LAN (LAN Play) */
        browse_tick = now;
        ctx->enum_pending = 1;
        if (ctx->mode == OKP_MODE_LAN) okp_lan_discover(ctx);
        okp_rdv_wake(ctx);
    }
    if (now - inject_tick >= 500) {
        static okp_room rooms[OKP_MAX_ROOMS];
        int n;
        inject_tick = now;
        EnterCriticalSection(&ctx->lock);
        n = ctx->nrooms;
        memcpy(rooms, ctx->rooms, sizeof(rooms[0]) * n);
        LeaveCriticalSection(&ctx->lock);
        for (int i = 0; i < n; i++) {
            uint16_t port;
            const uint8_t *ad;
            int al = room_advert(&rooms[i], &port, &ad);
            if (al <= 0) continue;
            uint32_t fl = 0;
            if (rooms[i].enumlen >= 16) memcpy(&fl, rooms[i].enumblob + 8 + 4, 4);
            if ((fl & 0x400) && al == 57 && port == 6995) {   /* Road Rash: the name the list shows, 20 bytes at 37 */
                uint8_t ad2[57];
                char name[24];
                memcpy(ad2, ad, 57);
                memcpy(name, ad2 + 37, 20);
                name[20] = 0;
                name[14] = 0;   /* room for " (pw)" */
                snprintf((char *)ad2 + 37, 20, "%s (pw)", name);
                deliver(standin_of(rooms[i].host), port, port, ad2, 57);
            } else {
                deliver(standin_of(rooms[i].host), port, port, ad, al);
            }
        }
    }
}

/* Windows 9x fails CreateThread when lpThreadId is NULL (documented); okplay's core passes NULL. As the
 * drop-in's shim/win9x.c does, every CreateThread in this DLL comes here and gets somewhere to write. */
static HANDLE WINAPI okw_CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE start, LPVOID param,
                                      DWORD flags, LPDWORD id)
{
    static HANDLE (WINAPI *fn)(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
    DWORD dummy;
    if (!fn) fn = (HANDLE (WINAPI *)(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD))
                  GetProcAddress(GetModuleHandleA("kernel32.dll"), "CreateThread");
    return fn(sa, stack, start, param, flags, id ? id : &dummy);
}
void *const okw_imp_CreateThread __asm__("__imp__CreateThread@24") = (void *)okw_CreateThread;

/* ---------------------------------------------------------------- the boxes */

/* Where a box runs: on the game's own thread (Windows 9x deadlocked with the box on a thread of its own:
 * the game holds the Win16 lock while DirectDraw draws, and the box's thread needs it). While it is open,
 * its message loop would also deliver the game's own timer and input messages - the game then re-entered
 * us from inside the box and opened another box (five were found open), and redrew its screen over the
 * box. So a message hook on this thread drops those messages for every window that isn't the box; the
 * game's network keeps going (its tick runs on a multimedia-timer thread, not on messages). box_open
 * also makes any call that still comes back in go on without a box. */
typedef struct { int kind; GUID g; WCHAR w[64]; INT_PTR r; } okw_box;

/* Notices from okplay's core - a newer version, the room server out of reach, TCP/IP not working, a crash
 * report or connection reports waiting (the DirectPlay drop-in has the same boxes and shows them at once on
 * their own thread). Over a full-screen game a window of ours at a random moment knocks the game to the
 * desktop, so here they wait for a safe moment: a game network call, no box open, no race packet for 5 s;
 * then run_box shows them on the game's own thread with its messages held, like the other boxes. The two
 * questions come from report.c's own thread, which waits for the answer. */
static CRITICAL_SECTION ui_lock;
static volatile LONG ui_pending;
static struct { int on; char latest[24], note[256]; } ui_update;
static struct { int on; char server[160], why[200]; } ui_unreach;
static struct { int on; } ui_nonet;
static struct { int on; char path[MAX_PATH]; INT_PTR answer; HANDLE done; } ui_crash;
static struct { int on; INT_PTR answer; HANDLE done; } ui_conn;
static WCHAR build_text[600];   /* the different-build question */
static volatile LONG box_open;
/* While a box is open the game's other threads wait as soon as they touch the network: Road Rash's join
 * handshake runs on a multimedia-timer thread with its own 5 s clock, which ran out while the player was
 * typing the password (153 unanswered hellos, then it gave up). Its messages to its own windows are kept
 * and posted again when the box closes (dropping them lost state the game needed). */
static HANDLE box_done;      /* manual-reset event, set = no box open */
static DWORD box_tid;
typedef struct { HWND w; UINT m; WPARAM wp; LPARAM lp; } okw_msg;
static okw_msg deferred[64];
static int ndeferred;

static void wait_for_box(void)
{
    /* Windows 9x: not there - a multimedia-timer thread made to wait never sent again (the game's join then
     * times out while the player types; the password is kept, and joining again goes straight in) */
    if (okp_sys.win9x) return;
    if (box_open && GetCurrentThreadId() != box_tid && box_done) WaitForSingleObject(box_done, INFINITE);
}

static int in_box_window(HWND w)
{
    char cls[16];
    while (w && (GetWindowLongA(w, GWL_STYLE) & WS_CHILD)) w = GetParent(w);   /* up to the top-level window (a popup's GetParent is its owner) */
    return w && GetClassNameA(w, cls, sizeof(cls)) && !strcmp(cls, "#32770") &&
           GetWindowThreadProcessId(w, NULL) == GetCurrentThreadId();
}

static LRESULT CALLBACK box_filter(int code, WPARAM wp, LPARAM lp)
{
    if (code >= 0 && wp == PM_REMOVE) {
        MSG *m = (MSG *)lp;
        /* only its timer ticks and input: its own posted messages go through as usual (holding them back and
         * posting them again later left Road Rash's join without its network tick); box_open keeps any call
         * that comes back in from opening another box */
        int quiet = m->message == WM_TIMER || (m->message >= WM_KEYFIRST && m->message <= WM_KEYLAST) ||
                    (m->message >= WM_MOUSEFIRST && m->message <= WM_MOUSELAST);
        if (quiet && !in_box_window(m->hwnd)) {
            if (m->message >= WM_USER && m->hwnd && ndeferred < 64) {   /* the game's own: later, not never */
                okw_msg *d = &deferred[ndeferred++];
                d->w = m->hwnd; d->m = m->message; d->wp = m->wParam; d->lp = m->lParam;
            }
            m->message = WM_NULL;
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static INT_PTR run_box(okw_box *b)
{
    HHOOK h = SetWindowsHookExA(WH_GETMESSAGE, box_filter, NULL, GetCurrentThreadId());
    box_tid = GetCurrentThreadId();
    ndeferred = 0;
    if (box_done) ResetEvent(box_done);
    InterlockedExchange(&box_open, 1);
    if (b->kind == 0) b->r = okbox_ask_mode("TCP/IP");
    else if (b->kind == 1) b->r = okbox_ask_host_password(&b->g, b->w, 64);
    else if (b->kind == 3) b->r = (okbox_wrong_password(), 1);
    else if (b->kind == 4) b->r = (okbox_offer_update(ui_update.latest, ui_update.note, OKP_VERSION_STR), 1);
    else if (b->kind == 5) b->r = (okbox_server_unreachable(ui_unreach.server, ui_unreach.why), 1);
    else if (b->kind == 6) b->r = (okbox_network_unavailable(), 1);
    else if (b->kind == 7) b->r = okbox_ask_crash_report(ui_crash.path);
    else if (b->kind == 8) b->r = okbox_ask_conn_reports();
    else if (b->kind == 9) {
        struct pw_dialog d;
        WCHAR title[96];
        memset(&d, 0, sizeof(d));
        okbox_brand(title, 96);
        d.title = title;
        d.text = build_text;
        d.text_cy = 64;
        d.ok = L"Join anyway";
        d.cancel = L"Cancel";
        b->r = okbox_show(&d) == IDOK;
    }
    else b->r = okbox_ask_join_password(&b->g, b->w, 64);
    InterlockedExchange(&box_open, 0);
    if (h) UnhookWindowsHookEx(h);
    if (box_done) SetEvent(box_done);
    for (int i = 0; i < ndeferred; i++) PostMessageA(deferred[i].w, deferred[i].m, deferred[i].wp, deferred[i].lp);
    ndeferred = 0;
    return b->r;
}

void okp_ui_offer_update(const char *latest, const char *note_utf8)   /* any thread, from rdv.c */
{
    static LONG offered;
    if (!latest || !*latest || offered || !strcmp(latest, OKP_VERSION_STR)) return;
    if (InterlockedExchange(&offered, 1) || !okbox_update_wanted(latest, OKP_VERSION_STR)) return;
    EnterCriticalSection(&ui_lock);
    lstrcpynA(ui_update.latest, latest, sizeof(ui_update.latest));
    lstrcpynA(ui_update.note, note_utf8 ? note_utf8 : "", sizeof(ui_update.note));
    ui_update.on = 1;
    LeaveCriticalSection(&ui_lock);
    InterlockedExchange(&ui_pending, 1);
}

void okp_ui_server_unreachable(const char *server, const char *why)   /* once per game run (rdv.c decides when) */
{
    static LONG shown;
    if (InterlockedExchange(&shown, 1)) return;
    EnterCriticalSection(&ui_lock);
    lstrcpynA(ui_unreach.server, server ? server : "", sizeof(ui_unreach.server));
    lstrcpynA(ui_unreach.why, why ? why : "", sizeof(ui_unreach.why));
    ui_unreach.on = 1;
    LeaveCriticalSection(&ui_lock);
    InterlockedExchange(&ui_pending, 1);
}

void okp_ui_network_unavailable(int err)
{
    static LONG shown;
    okp_log(0, "network: no internet socket can be created (Winsock error %d) - TCP/IP isn't installed or "
               "isn't bound to a network card; Online and LAN Play can't start", err);
    if (InterlockedExchange(&shown, 1)) return;
    ui_nonet.on = 1;
    InterlockedExchange(&ui_pending, 1);
}

static INT_PTR ui_ask(int crash, const char *path)   /* report.c's thread: wait until the game shows it */
{
    HANDLE done = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!done) return 0;
    EnterCriticalSection(&ui_lock);
    if (crash) { lstrcpynA(ui_crash.path, path, sizeof(ui_crash.path)); ui_crash.done = done; ui_crash.on = 1; }
    else { ui_conn.done = done; ui_conn.on = 1; }
    LeaveCriticalSection(&ui_lock);
    InterlockedExchange(&ui_pending, 1);
    WaitForSingleObject(done, INFINITE);   /* the game closing first: asked again next time */
    CloseHandle(done);
    return crash ? ui_crash.answer : ui_conn.answer;
}
int okp_ui_ask_crash_report(const char *path) { return (int)ui_ask(1, path); }
int okp_ui_ask_conn_reports(void) { return (int)ui_ask(0, NULL); }

/* a game network call, no lock held: show what waits if this is a safe moment */
static void ui_show_pending(void)
{
    static volatile LONG showing;   /* (not TryEnterCriticalSection: on Windows 9x it is a stub that always fails) */
    if (!ui_pending || box_open || GetTickCount() - last_race_tick < 5000) return;
    if (InterlockedExchange(&showing, 1)) return;   /* one thread at a time; box_open keeps the other boxes apart */
    for (;;) {
        okw_box b;
        memset(&b, 0, sizeof(b));
        EnterCriticalSection(&ui_lock);
        b.kind = ui_update.on ? 4 : ui_unreach.on ? 5 : ui_nonet.on ? 6 : ui_crash.on ? 7 : ui_conn.on ? 8 : 0;
        if (!b.kind) InterlockedExchange(&ui_pending, 0);
        LeaveCriticalSection(&ui_lock);
        if (!b.kind) break;
        INT_PTR r = run_box(&b);
        EnterCriticalSection(&ui_lock);
        if (b.kind == 4) ui_update.on = 0;
        else if (b.kind == 5) ui_unreach.on = 0;
        else if (b.kind == 6) ui_nonet.on = 0;
        else if (b.kind == 7) { ui_crash.on = 0; ui_crash.answer = r; SetEvent(ui_crash.done); }
        else { ui_conn.on = 0; ui_conn.answer = r; SetEvent(ui_conn.done); }
        LeaveCriticalSection(&ui_lock);
    }
    InterlockedExchange(&showing, 0);
}

/* the first UDP socket (or own-address lookup) of a game run: Online Play, LAN Play or the original TCP/IP.
 * okplay.ini wsock_mode=ask (default) | online | lan | original; tests: OKP_WSOCK_MODE. No okw_lock held. */
static int choose_mode(void)
{
    if (mode_choice) return mode_choice;
    if (box_open) {
        /* Another thread of the game (Windows NT: its network thread) waits for the answer - starting
         * Online Play "for now" here made a LAN Play pick go nowhere. The box's own thread, back in from
         * inside it (Windows 9x), can't wait: okplay starts for now and restarts if the answer differs. */
        if (GetCurrentThreadId() != box_tid && box_done) {
            WaitForSingleObject(box_done, INFINITE);
            if (mode_choice) return mode_choice;
        }
        mode_provisional = 1;
        return 2;
    }
    EnterCriticalSection(&okw_ask_lock);
    if (!mode_choice) {
        char ini[MAX_PATH], v[16] = "";
        snprintf(ini, sizeof(ini), "%sokplay.ini", okp_dll_dir());
        GetPrivateProfileStringA("okplay", "wsock_mode", "ask", v, sizeof(v), ini);
        GetEnvironmentVariableA("OKP_WSOCK_MODE", v, sizeof(v));
        int m = !_stricmp(v, "online") ? 2 : !_stricmp(v, "lan") ? 3 : (!_stricmp(v, "original") || !_stricmp(v, "windows")) ? 1 : 0;
        if (!m) {
            okw_box b;
            memset(&b, 0, sizeof(b));
            b.kind = 0;
            m = (int)run_box(&b);
        }
        okp_log(1, "wsock: %s (okplay.ini wsock_mode=%s)", m == 2 ? "Online Play" : m == 3 ? "LAN Play" :
                "the game's original TCP/IP - okplay stays out", v);
        if (m >= 2) okp_reports_kick();   /* report.c: a crash report or connection reports waiting (asks first) */
        InterlockedExchange(&mode_choice, m);
        if (mode_provisional) {   /* it started as Online Play while the box was open: the answer decides */
            mode_provisional = 0;
            EnterCriticalSection(&okw_lock);
            okp_ctx *c = started && m != 2 ? ctx : NULL;
            if (c) { ctx = NULL; started = 0; }
            LeaveCriticalSection(&okw_lock);
            if (c) {
                okp_log(1, "wsock: okplay had started as Online Play while the box was open - %s", m == 3 ?
                        "restarting as LAN Play (same address)" : "stopping it");
                stop_ctx(c, 0);
            }
        }
    }
    LeaveCriticalSection(&okw_ask_lock);
    return mode_choice;
}

/* the game is about to host (its first broadcast): the password box, as the drop-in's */
static void ask_host_password(void)
{
    WCHAR w[64];
    if (box_open) return;   /* back in from inside a box (9x): asked when that one closes */
    EnterCriticalSection(&okw_ask_lock);
    if (!host_pw_asked) {
        host_pw_asked = 1;
        pending_room = (uint32_t)okp_rand64() | 1;
        okw_box b;
        memset(&b, 0, sizeof(b));
        b.kind = 1;
        b.g = room_guid(pending_room);
        if (run_box(&b)) {
            memcpy(w, b.w, sizeof(w));
            SecureZeroMemory(b.w, sizeof(b.w));
            wide_to_ascii(w, host_hash, sizeof(host_hash));
            host_has_pw = 1;
        }
    }
    LeaveCriticalSection(&okw_ask_lock);
}

/* the game is joining `host`'s room, which has a password. The box runs on a thread of its own and the game
 * goes on (shown in the middle of the game's join, a box on the game's thread derailed it on Windows 98);
 * packets to that host wait until the host's yes. Windows NT: the game's network calls also wait while the
 * box is open, so its join clock stops. Windows 9x: if typing outlasts the game's join (5 s in Road Rash),
 * that attempt fails and joining again goes straight in (the password is kept for that room). */
static DWORD WINAPI join_box_thread(LPVOID arg)
{
    WCHAR w[64];
    GUID g = room_guid(auth_room);
    (void)arg;
    box_tid = GetCurrentThreadId();
    if (box_done) ResetEvent(box_done);
    InterlockedExchange(&box_open, 1);
    int ok = okbox_ask_join_password(&g, w, 64);
    InterlockedExchange(&box_open, 0);
    EnterCriticalSection(&okw_lock);
    if (ok) {
        wide_to_ascii(w, join_hash, sizeof(join_hash));
        auth_state = 1;
        auth_tick = 0;   /* sent at the next call from the game (okw_tick) */
    } else {
        auth_state = -1;   /* cancelled: the game's join to this room just goes nowhere */
        nheld = 0;
    }
    LeaveCriticalSection(&okw_lock);
    SecureZeroMemory(w, sizeof(w));
    if (box_done) SetEvent(box_done);
    return 0;
}

static void ask_join_password(uint64_t host, uint32_t room)
{
    EnterCriticalSection(&okw_lock);
    int start = !(auth_host == host && (auth_state == 1 || auth_state == 2 || auth_state == 3));
    if (start) {
        auth_host = host;
        auth_room = room;
        auth_state = 3;
        nheld = 0;
    }
    LeaveCriticalSection(&okw_lock);
    if (start) {
        HANDLE t = CreateThread(NULL, 0, join_box_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
}

/* before a send (no okw_lock held): does it need a box first? */
static void boxes_before_send(uint32_t ip)
{
    if (mode_choice < 2) return;
    if (ip == 0xFFFFFFFFu || (ntohl(ip) & 0xFF) == 0xFF) {
        if (!host_pw_asked && !(ctx && ctx->host_peer)) ask_host_password();
        return;
    }
    uint64_t host = 0;
    uint32_t room = 0, flags = 0;
    EnterCriticalSection(&okw_lock);
    if (ctx && !ctx->is_host) {
        EnterCriticalSection(&ctx->lock);
        for (int i = 0; i < ctx->nrooms; i++) {
            if (standin_of(ctx->rooms[i].host) != ip) continue;
            host = ctx->rooms[i].host;
            room = ctx->rooms[i].room;
            if (ctx->rooms[i].enumlen >= 8 + 8) memcpy(&flags, ctx->rooms[i].enumblob + 8 + 4, 4);
        }
        LeaveCriticalSection(&ctx->lock);
    }
    int need = host && (flags & 0x400) && !(auth_host == host && (auth_state == 1 || auth_state == 2 || auth_state == 3));
    LeaveCriticalSection(&okw_lock);
    if (need) ask_join_password(host, room);
}

/* ---------------------------------------------------------------- game builds */

/* Builds of the game that race each other (fingerprint = okp_exe_build: the first 16 hex digits of the exe's
 * SHA-256; every room carries its host's). The DirectPlay drop-in asks whenever the host's build differs;
 * Road Rash's builds differ without mattering (the Steam exe and the 1996 CD exe race each other, and many CD
 * players run a no-CD exe), so here the question comes only when a build is not in this list. */
static const struct { const char *build, *what; } rr_builds[] = {
    { "73d7f9744b5b1a94", "Road Rash CD (1996), ROADRASH.EXE" },
    { "9e40e25737149ab0", "Road Rash (Steam or GOG - the same exe), RoadRash95.exe" },
    { "9c32eadc287cc8eb", "Road Rash CD with okplay's lab no-CD patch (testing)" },
};

static const char *rr_build_name(const char *b)
{
    for (unsigned i = 0; i < sizeof(rr_builds) / sizeof(rr_builds[0]); i++)
        if (!strcmp(b, rr_builds[i].build)) return rr_builds[i].what;
    return NULL;
}

/* the player picked the room of `host`: 1 = go on (same or known builds, or "Join anyway"), 0 = stay in the
 * list. okplay.ini build_check=ask (default) | off. Tests: OKP_TEST_BUILD_ANSWER=join|cancel. No lock held. */
static int build_ok(uint64_t host, const char *theirs, const char *theirs_exe)
{
    static uint64_t agreed;   /* "Join anyway" for this host: not asked again */
    char ini[MAX_PATH], mode[16] = "", mine[17] = "", mine_exe[64] = "";
    snprintf(ini, sizeof(ini), "%sokplay.ini", okp_dll_dir());
    GetPrivateProfileStringA("okplay", "build_check", "ask", mode, sizeof(mode), ini);
    if (!_stricmp(mode, "off") || !theirs[0]) return 1;   /* (a host before 0.8.1's core sends none) */
    okp_exe_build(mine, mine_exe, sizeof(mine_exe));
#ifdef OKW_LAB   /* lab builds: okplay.ini wsock_test_build=<16 hex> stands in for our own build (bench tests) */
    {
        char fake[17] = "";
        GetPrivateProfileStringA("okplay", "wsock_test_build", "", fake, sizeof(fake), ini);
        if (strlen(fake) == 16) { lstrcpynA(mine, fake, sizeof(mine)); okp_log(1, "build: testing as build %s", mine); }
    }
#endif
    if (!mine[0] || !strcmp(mine, theirs)) {
        okp_log(2, "build: the host runs the same build of the game (%s)", mine);
        return 1;
    }
    const char *a = rr_build_name(theirs), *b = rr_build_name(mine);
    if (a && b) {
        okp_log(1, "build: the host runs %s (%s), we run %s (%s) - builds that race each other", a, theirs, b, mine);
        return 1;
    }
    okp_log(1, "build: the host runs another build of the game - theirs %s %s%s, ours %s %s%s", theirs_exe, theirs,
            a ? "" : " (not in the known list)", mine_exe, mine, b ? "" : " (not in the known list)");
    if (host == agreed) return 1;
    char test[16] = "";
    GetEnvironmentVariableA("OKP_TEST_BUILD_ANSWER", test, sizeof(test));
    int go;
    if (test[0]) go = !_stricmp(test, "join");
    else {
        WCHAR tw[64], mw[64];
        MultiByteToWideChar(CP_UTF8, 0, theirs_exe[0] ? theirs_exe : "?", -1, tw, 64);
        MultiByteToWideChar(CP_UTF8, 0, mine_exe[0] ? mine_exe : "?", -1, mw, 64);
        _snwprintf(build_text, 599,
                   L"The host is running a different version of the game.\n\n"
                   L"Theirs: %ls (build %.8hs)\nYours: %ls (build %.8hs)\n\n"
                   L"Joining will probably fail, or the game will go out of sync. Join anyway?", tw, theirs, mw, mine);
        build_text[599] = 0;
        okw_box bx;
        memset(&bx, 0, sizeof(bx));
        bx.kind = 9;
        go = (int)run_box(&bx);
    }
    okp_log(1, "build: %s", go ? "joining anyway" : "the player cancelled the join - back to the list");
    if (go) agreed = host;
    return go;
}

/* ---------------------------------------------------------------- the game's room list */

/* A game that lists LAN games in a Windows dialog (Road Rash: "Pick from among the available TCP/IP games")
 * waits in that dialog until the player picks one - the one moment nothing is in progress. So the join
 * password is asked there: a window hook on the game's thread finds a dialog with a list in it, and its OK
 * button (or a double click in the list) for a locked room opens the password box over the list first.
 * Cancel keeps the player in the list; with the password, okplay starts connecting to that host and the
 * game's own join then goes straight in. (The game's files are not touched: the hook lives in memory.) */
static HHOOK cbt_hook;
static const char LIST_PROP[] = "okwsock.oldproc";

/* the selected row's host name -> its room; 1 = let the click through */
static int list_pick_ok(HWND dlg)
{
    HWND lv = FindWindowExA(dlg, NULL, "SysListView32", NULL);
    if (!lv || !ctx) return 1;
    int idx = (int)SendMessageA(lv, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    if (idx < 0) return 1;
    char text[64] = "";
    LVITEMA it;
    memset(&it, 0, sizeof(it));
    it.iSubItem = 0;
    it.pszText = text;
    it.cchTextMax = sizeof(text);
    SendMessageA(lv, LVM_GETITEMTEXTA, (WPARAM)idx, (LPARAM)&it);
    char *pw = strstr(text, " (pw)");
    int locked = pw != NULL;
    if (pw) *pw = 0;
    uint64_t host = 0;
    uint32_t room = 0;
    char theirs[17] = "", theirs_exe[64] = "";
    EnterCriticalSection(&okw_lock);
    EnterCriticalSection(&ctx->lock);
    for (int i = 0; i < ctx->nrooms && !host; i++) {
        uint16_t port;
        const uint8_t *ad;
        int al = room_advert(&ctx->rooms[i], &port, &ad);
        uint32_t fl = 0;
        if (ctx->rooms[i].enumlen >= 16) memcpy(&fl, ctx->rooms[i].enumblob + 8 + 4, 4);
        if (al == 57 && !(fl & 0x400) == !locked && !strncmp((const char *)ad + 37, text, strlen(text))) {
            host = ctx->rooms[i].host;
            room = ctx->rooms[i].room;
            lstrcpynA(theirs, ctx->rooms[i].build, sizeof(theirs));
            lstrcpynA(theirs_exe, ctx->rooms[i].exe, sizeof(theirs_exe));
        }
    }
    LeaveCriticalSection(&ctx->lock);
    int known = host && auth_host == host && (auth_state == 1 || auth_state == 2);
    LeaveCriticalSection(&okw_lock);
    if (host && !build_ok(host, theirs, theirs_exe)) return 0;   /* stay in the list */
    if (!locked) return 1;
    if (!host || known) return 1;
    okp_log(1, "wsock: the player picked a locked room (%s) - asking for its password", text);
    okw_box b;
    memset(&b, 0, sizeof(b));
    b.kind = 2;
    b.g = room_guid(room);
    if (!run_box(&b)) {
        okp_log(1, "wsock: password cancelled - back to the list");
        return 0;
    }
    EnterCriticalSection(&okw_lock);
    wide_to_ascii(b.w, join_hash, sizeof(join_hash));
    auth_host = host;
    auth_room = room;
    auth_state = 1;
    auth_tick = 0;
    nheld = 0;
    join_room(host);   /* connect now: the path is ready when the game's join starts */
    okw_tick();        /* and the password goes at once */
    InterlockedExchange(&auth_in_list, 1);
    LeaveCriticalSection(&okw_lock);
    SecureZeroMemory(b.w, sizeof(b.w));

    /* The host's answer is waited for here, still in the list: a "no" that came after the game had left its
     * list put a box over a full-screen game, and Windows 9x sent the game to the desktop (where Road Rash
     * shows only black). On a LAN the answer takes a few milliseconds; with no answer in 4 s the game goes
     * ahead and its packets wait for the host's yes as before. */
    HCURSOR old_cur = SetCursor(LoadCursor(NULL, IDC_WAIT));
    DWORD t0 = GetTickCount();
    int st;
    for (;;) {
        EnterCriticalSection(&okw_lock);
        okw_tick();
        st = auth_state;
        LeaveCriticalSection(&okw_lock);
        if (st != 1 || GetTickCount() - t0 > 4000) break;
        Sleep(30);
    }
    InterlockedExchange(&auth_in_list, 0);
    SetCursor(old_cur);
    if (st == 2) {
        okp_log(1, "wsock: the host let us in - joining (%lu ms)", (unsigned long)(GetTickCount() - t0));
        return 1;
    }
    if (st == 1) {
        okp_log(1, "wsock: no answer from the host yet - the game goes ahead, its packets wait for the yes");
        return 1;
    }
    EnterCriticalSection(&okw_lock);
    leave_room(host);
    LeaveCriticalSection(&okw_lock);
    memset(&b, 0, sizeof(b));
    b.kind = 3;
    run_box(&b);   /* "Wrong password": back to the list, which stays open */
    return 0;
}

static LRESULT CALLBACK list_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    WNDPROC old = (WNDPROC)GetPropA(w, LIST_PROP);
    if (m == WM_COMMAND && LOWORD(wp) == IDOK && !list_pick_ok(w)) return 0;
    if (m == WM_NOTIFY && lp && ((NMHDR *)lp)->code == NM_DBLCLK) {
        char cls[32];
        if (GetClassNameA(((NMHDR *)lp)->hwndFrom, cls, sizeof(cls)) && !strcmp(cls, "SysListView32") && !list_pick_ok(w)) return 0;
    }
    if (m == WM_NCDESTROY) {
        RemovePropA(w, LIST_PROP);
        if (old) SetWindowLongA(w, GWL_WNDPROC, (LONG)(LONG_PTR)old);
    }
    return old ? CallWindowProcA(old, w, m, wp, lp) : DefWindowProcA(w, m, wp, lp);
}

static LRESULT CALLBACK cbt_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HCBT_ACTIVATE && mode_choice != 1) {   /* (the list may come up before the game's first socket) */
        HWND w = (HWND)wp;
        char cls[16];
        if (GetClassNameA(w, cls, sizeof(cls)) && !strcmp(cls, "#32770") && !GetPropA(w, LIST_PROP) &&
            FindWindowExA(w, NULL, "SysListView32", NULL)) {   /* (our own boxes have no list) */
            LONG old = SetWindowLongA(w, GWL_WNDPROC, (LONG)(LONG_PTR)list_proc);
            SetPropA(w, LIST_PROP, (HANDLE)(LONG_PTR)old);
            okp_log(1, "wsock: watching the game's room list (its OK button and double click)");
        }
    }
    return CallNextHookEx(cbt_hook, code, wp, lp);
}

/* ---------------------------------------------------------------- the Winsock calls */

/* Is this a game okplay wsock is for? One download holds every okplay DLL, so this one may sit next to any
 * game (a DirectPlay game loads wsock32.dll too). Only for the games on this list (or named in okplay.ini
 * wsock_games=, or with OKP_WSOCK_MODE set, for tests) does any of okplay run; for every other program each
 * call goes straight to Windows' Winsock and nothing else happens - no box, no log, no thread, no hook. */
static int okw_on;
static const char *const okw_games[] = { "roadrash.exe", "roadrash95.exe" };   /* Road Rash: CD, Steam */

static int is_winsock_game(void)
{
    char path[MAX_PATH], *base, v[16] = "";
    if (GetEnvironmentVariableA("OKP_WSOCK_MODE", v, sizeof(v))) return 1;
    if (!GetModuleFileNameA(NULL, path, sizeof(path))) return 0;
    base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    for (unsigned i = 0; i < sizeof(okw_games) / sizeof(okw_games[0]); i++) if (!_stricmp(base, okw_games[i])) return 1;
    char ini[MAX_PATH], list[512] = "", dir[MAX_PATH];
    if (!GetModuleFileNameA(g_self, dir, sizeof(dir))) return 0;
    char *slash = strrchr(dir, '\\');
    if (slash) slash[1] = 0;
    snprintf(ini, sizeof(ini), "%sokplay.ini", dir);
    GetPrivateProfileStringA("okplay", "wsock_games", "", list, sizeof(list), ini);
    for (char *t = strtok(list, ",; "); t; t = strtok(NULL, ",; ")) if (!_stricmp(base, t)) return 1;
    return 0;
}


static int set_err(int e) { WSASetLastError(e); return SOCKET_ERROR; }

SOCKET WINAPI x_socket(int af, int type, int proto)
{
    if (!okw_on) return socket(af, type, proto);
    SOCKET s = socket(af, type, proto);
    if (s == INVALID_SOCKET || af != AF_INET || type != SOCK_DGRAM) return s;
    check_new_game();
    socks_gone = 0;
    if (choose_mode() == 1) return s;   /* the original TCP/IP: Windows' own socket, untouched */
    tr("game socket %u: mode chosen, thread %lu", (unsigned)s, (unsigned long)GetCurrentThreadId());
    EnterCriticalSection(&okw_lock);
    if (mode_choice >= 2) start_okplay();   /* (not while the Online/LAN box is still open) */
    for (int i = 0; i < OKW_MAX_SOCKS; i++) {
        if (socks[i].used) continue;
        memset(&socks[i], 0, sizeof(socks[i]));
        socks[i].used = 1;
        socks[i].s = s;
        tr("game socket %u (virtual, slot %d)", (unsigned)s, i);
        break;
    }
    LeaveCriticalSection(&okw_lock);
    return s;
}

int WINAPI x_closesocket(SOCKET s)
{
    if (!okw_on) return closesocket(s);
    EnterCriticalSection(&okw_lock);
    okw_sock *k = sock_of(s);
    if (k) {
        if (k->port && k->port == advert_port && ctx && ctx->is_host) {   /* the host's game closed its broadcast socket */
            ctx->closing = 1;
            ctx->session_open = 0;
            ctx->is_host = 0;
            advert_len = 0;
            okp_rdv_wake(ctx);
            okp_log(1, "wsock: the game closed its room");
            room_closed = 1;
            room_closed_tick = GetTickCount();
        }
        for (okw_pkt *p = k->q, *n; p; p = n) { n = p->next; free(p); }
        k->used = 0;
        tr("game closesocket %u port %u", (unsigned)s, k->port);
        int open = 0;
        for (int i = 0; i < OKW_MAX_SOCKS; i++) open += socks[i].used;
        if (!open) { socks_gone_tick = GetTickCount(); socks_gone = 1; }
    }
    LeaveCriticalSection(&okw_lock);
    return closesocket(s);
}

int WINAPI x_bind(SOCKET s, const struct sockaddr *a, int l)
{
    if (!okw_on) return bind(s, a, l);
    EnterCriticalSection(&okw_lock);
    okw_sock *k = sock_of(s);
    if (!k) { LeaveCriticalSection(&okw_lock); return bind(s, a, l); }
    if (!a || l < (int)sizeof(struct sockaddr_in) || a->sa_family != AF_INET) { LeaveCriticalSection(&okw_lock); return set_err(WSAEFAULT); }
    const struct sockaddr_in *in = (const struct sockaddr_in *)a;
    uint16_t port = ntohs(in->sin_port);
    if (!port) port = next_eph++;
    k->port = port;
    k->bound_ip = in->sin_addr.s_addr;
    tr("game bind sock %u port %u", (unsigned)s, port);
    LeaveCriticalSection(&okw_lock);
    return 0;
}

int WINAPI x_connect(SOCKET s, const struct sockaddr *a, int l)
{
    if (!okw_on) return connect(s, a, l);
    EnterCriticalSection(&okw_lock);
    okw_sock *k = sock_of(s);
    if (!k) { LeaveCriticalSection(&okw_lock); return connect(s, a, l); }
    const struct sockaddr_in *in = (const struct sockaddr_in *)a;
    if (!a || l < (int)sizeof(*in)) { LeaveCriticalSection(&okw_lock); return set_err(WSAEFAULT); }
    if (!k->port) k->port = next_eph++;
    k->peer_ip = in->sin_addr.s_addr;
    k->peer_port = ntohs(in->sin_port);
    uint64_t peer;
    if (ctx && peer_of_standin(k->peer_ip, &peer)) join_room(peer);
    LeaveCriticalSection(&okw_lock);
    return 0;
}

int WINAPI x_getsockname(SOCKET s, struct sockaddr *a, int *l)
{
    if (!okw_on) return getsockname(s, a, l);
    EnterCriticalSection(&okw_lock);
    okw_sock *k = sock_of(s);
    if (!k || !ctx) { LeaveCriticalSection(&okw_lock); return getsockname(s, a, l); }
    if (!a || !l || *l < (int)sizeof(struct sockaddr_in)) { LeaveCriticalSection(&okw_lock); return set_err(WSAEFAULT); }
    struct sockaddr_in *in = (struct sockaddr_in *)a;
    memset(in, 0, sizeof(*in));
    in->sin_family = AF_INET;
    in->sin_port = htons(k->port);
    in->sin_addr.s_addr = k->bound_ip ? k->bound_ip : 0;
    *l = sizeof(*in);
    LeaveCriticalSection(&okw_lock);
    return 0;
}

#ifdef OKW_LAB
void rr_lab_game_out(const uint8_t *d, int len);
#endif
static int do_send(okw_sock *k, uint32_t ip, uint16_t port, const char *b, int len)   /* lock held */
{
    rrf_game_out((const uint8_t *)b, len);
#ifdef OKW_LAB
    rr_lab_game_out((const uint8_t *)b, len);
#endif
    if (!ctx && !started && mode_choice >= 2) start_okplay();
    if (!ctx) return len;
    if (!k->port) k->port = next_eph++;
    if (ip == 0xFFFFFFFFu || (ntohl(ip) & 0xFF) == 0xFF) {   /* broadcast: we host; everyone in the room gets it */
        if (box_open && !ctx->is_host) return len;   /* the password box is still open (9x): no room yet */
        if (port != 6996) {
            memcpy(advert, b, len < (int)sizeof(advert) ? len : (int)sizeof(advert));
            advert_len = len < (int)sizeof(advert) ? len : (int)sizeof(advert);
            advert_port = port;
            advert_tick = okp_now();
            host_room();
            build_room_record();
        }
        uint64_t ids[OKP_MAX_PEERS];
        int n = 0;
        EnterCriticalSection(&ctx->lock);
        for (int i = 0; i < OKP_MAX_PEERS; i++)
            if (ctx->peers[i].used && ctx->peers[i].have_path) ids[n++] = ctx->peers[i].id;
        LeaveCriticalSection(&ctx->lock);
        for (int i = 0; i < n; i++) send_to_peer(ids[i], k->port, port, OKW_BCAST, b, len);
        return len;
    }
    uint64_t peer;
    if (!peer_of_standin(ip, &peer)) {
        char t[20];
        okp_log(2, "wsock: a %d-byte packet to %s:%u - not an okplay player, dropped", len, ip_s(ip, t), port);
        return len;
    }
    join_room(peer);
    tr("tx %u->%u len %d type %d", k->port, port, len, len ? (uint8_t)b[0] : -1);
    send_to_peer(peer, k->port, port, 0, b, len);
    return len;
}

int WINAPI x_sendto(SOCKET s, const char *b, int len, int f, const struct sockaddr *a, int al)
{
    if (!okw_on) return sendto(s, b, len, f, a, al);
    wait_for_box();
    ui_show_pending();
    if (a && al >= (int)sizeof(struct sockaddr_in)) {
        uint32_t ip = ((const struct sockaddr_in *)a)->sin_addr.s_addr;
        if (room_closed && (ip == 0xFFFFFFFFu || (ntohl(ip) & 0xFF) == 0xFF)) check_new_game();
        if (!mode_choice) {
            EnterCriticalSection(&okw_lock);
            int virt = sock_of(s) != NULL;
            LeaveCriticalSection(&okw_lock);
            if (virt) choose_mode();
        }
        boxes_before_send(ip);
    }
    EnterCriticalSection(&okw_lock);
    okw_sock *k = sock_of(s);
    if (!k) { LeaveCriticalSection(&okw_lock); return sendto(s, b, len, f, a, al); }
    const struct sockaddr_in *in = (const struct sockaddr_in *)a;
    if (!a || al < (int)sizeof(*in)) {
        int r = k->peer_ip ? do_send(k, k->peer_ip, k->peer_port, b, len) : set_err(WSAENOTCONN);
        LeaveCriticalSection(&okw_lock);
        return r;
    }
    int r = do_send(k, in->sin_addr.s_addr, ntohs(in->sin_port), b, len);
    okw_tick();
    LeaveCriticalSection(&okw_lock);
    return r;
}

int WINAPI x_send(SOCKET s, const char *b, int len, int f)
{
    if (!okw_on) return send(s, b, len, f);
    wait_for_box();
    EnterCriticalSection(&okw_lock);
    okw_sock *k = sock_of(s);
    if (!k) { LeaveCriticalSection(&okw_lock); return send(s, b, len, f); }
    int r = k->peer_ip ? do_send(k, k->peer_ip, k->peer_port, b, len) : set_err(WSAENOTCONN);
    LeaveCriticalSection(&okw_lock);
    return r;
}

static int take(okw_sock *k, char *b, int len, struct sockaddr *a, int *al)   /* lock held; one datagram */
{
    okw_pkt *p = k->q;
    k->q = p->next;
    if (!k->q) k->qtail = NULL;
    k->qn--;
    k->qbytes -= p->len;
    int n = p->len < len ? p->len : len;
    memcpy(b, p->data, n);
    if (a && al && *al >= (int)sizeof(struct sockaddr_in)) {
        struct sockaddr_in *in = (struct sockaddr_in *)a;
        memset(in, 0, sizeof(*in));
        in->sin_family = AF_INET;
        in->sin_addr.s_addr = p->from_ip;
        in->sin_port = htons(p->from_port);
        *al = sizeof(*in);
    }
    int trunc = p->len > len;
    tr("game read %u len %d type %d%s (left %d)", k->port, p->len, p->len ? p->data[0] : -1, trunc ? " TRUNCATED" : "", k->qn);
    free(p);
    if (trunc) return set_err(WSAEMSGSIZE);
    return n;
}

static int do_recv(SOCKET s, char *b, int len, struct sockaddr *a, int *al, int *handled)
{
    for (;;) {
        EnterCriticalSection(&okw_lock);
        okw_sock *k = sock_of(s);
        if (!k) { LeaveCriticalSection(&okw_lock); *handled = 0; return 0; }
        *handled = 1;
        okw_tick();
        if (k->q) {
            int r = take(k, b, len, a, al);
            LeaveCriticalSection(&okw_lock);
            return r;
        }
        int nb = k->nonblock;
        LeaveCriticalSection(&okw_lock);
        if (nb) return set_err(WSAEWOULDBLOCK);
        Sleep(10);   /* a blocking receive (the games seen poll first) */
    }
}

int WINAPI x_recv(SOCKET s, char *b, int len, int f)
{
    if (!okw_on) return recv(s, b, len, f);
    wait_for_box();
    int handled;
    int r = do_recv(s, b, len, NULL, NULL, &handled);
    return handled ? r : recv(s, b, len, f);
}

int WINAPI x_recvfrom(SOCKET s, char *b, int len, int f, struct sockaddr *a, int *al)
{
    if (!okw_on) return recvfrom(s, b, len, f, a, al);
    wait_for_box();
    ui_show_pending();
    int handled;
    int r = do_recv(s, b, len, a, al, &handled);
    return handled ? r : recvfrom(s, b, len, f, a, al);
}

int WINAPI x_ioctlsocket(SOCKET s, long cmd, u_long *arg)
{
    if (!okw_on) return ioctlsocket(s, cmd, arg);
    wait_for_box();
    EnterCriticalSection(&okw_lock);
    okw_sock *k = sock_of(s);
    if (!k) { LeaveCriticalSection(&okw_lock); return ioctlsocket(s, cmd, arg); }
    int r = 0;
    if (cmd == (long)WS_FIONBIO) {
        k->nonblock = arg && *arg;
    } else if (cmd == (long)WS_FIONREAD) {
        okw_tick();
        if (arg) *arg = (u_long)k->qbytes;
        { static DWORD last; if (k->qbytes || GetTickCount() - last > 1000) { last = GetTickCount(); tr("game FIONREAD sock %u port %u -> %d bytes", (unsigned)k->s, k->port, k->qbytes); } }
    } else {
        r = set_err(WSAEINVAL);
    }
    LeaveCriticalSection(&okw_lock);
    return r;
}

int WINAPI x_setsockopt(SOCKET s, int lv, int o, const char *v, int l)
{
    if (!okw_on) return setsockopt(s, lv, o, v, l);
    EnterCriticalSection(&okw_lock);
    okw_sock *k = sock_of(s);
    if (k) {
        if (lv == SOL_SOCKET && o == SO_BROADCAST && v && l >= 1) k->bcast = v[0] != 0;
        LeaveCriticalSection(&okw_lock);
        return 0;   /* options have no meaning on a virtual socket */
    }
    LeaveCriticalSection(&okw_lock);
    return setsockopt(s, lv, o, v, l);
}

int WINAPI x_getsockopt(SOCKET s, int lv, int o, char *v, int *l)
{
    return getsockopt(s, lv, o, v, l);
}

/* the game's own address = its stand-in */
struct hostent *WINAPI x_gethostbyname(const char *n)
{
    if (!okw_on) return gethostbyname(n);
    static struct hostent he;
    static char name[256];
    static uint32_t addr;
    static char *list[2];
    char me[256] = "";
    gethostname(me, sizeof(me));
    if (choose_mode() == 1) return gethostbyname(n);
    EnterCriticalSection(&okw_lock);
    if (mode_choice >= 2) start_okplay();
    if (!okw_self) okw_self = okp_rand64();   /* the stand-in is known before okplay runs (box still open) */
    int mine = !n || !n[0] || !_stricmp(n, me);
    if (!mine) { LeaveCriticalSection(&okw_lock); return gethostbyname(n); }
    addr = standin_of(okw_self);
    lstrcpynA(name, me, sizeof(name));
    list[0] = (char *)&addr;
    list[1] = NULL;
    he.h_name = name;
    he.h_aliases = NULL;
    he.h_addrtype = AF_INET;
    he.h_length = 4;
    he.h_addr_list = list;
    LeaveCriticalSection(&okw_lock);
    return &he;
}

/* plain pass-throughs (the .def forwards the rest to ws2_32 directly) */
SOCKET WINAPI x_accept(SOCKET s, struct sockaddr *a, int *l) { return accept(s, a, l); }
int WINAPI x_getpeername(SOCKET s, struct sockaddr *a, int *l)
{
    if (!okw_on) return getpeername(s, a, l);
    EnterCriticalSection(&okw_lock);
    okw_sock *k = sock_of(s);
    if (k && k->peer_ip && a && l && *l >= (int)sizeof(struct sockaddr_in)) {
        struct sockaddr_in *in = (struct sockaddr_in *)a;
        memset(in, 0, sizeof(*in));
        in->sin_family = AF_INET;
        in->sin_addr.s_addr = k->peer_ip;
        in->sin_port = htons(k->peer_port);
        *l = sizeof(*in);
        LeaveCriticalSection(&okw_lock);
        return 0;
    }
    LeaveCriticalSection(&okw_lock);
    return getpeername(s, a, l);
}
u_long WINAPI x_htonl(u_long v) { return htonl(v); }
u_short WINAPI x_htons(u_short v) { return htons(v); }
u_long WINAPI x_ntohl(u_long v) { return ntohl(v); }
u_short WINAPI x_ntohs(u_short v) { return ntohs(v); }
unsigned long WINAPI x_inet_addr(const char *c) { return inet_addr(c); }
char *WINAPI x_inet_ntoa(struct in_addr a) { return inet_ntoa(a); }
int WINAPI x_listen(SOCKET s, int b) { return listen(s, b); }
int WINAPI x_select(int n, fd_set *r, fd_set *w, fd_set *e, const struct timeval *t)
{
    if (okw_on) tr("game select (read set %d)", r ? (int)r->fd_count : -1);
    return select(n, r, w, e, t);
}
int WINAPI x_shutdown(SOCKET s, int h) { return shutdown(s, h); }
struct hostent *WINAPI x_gethostbyaddr(const char *a, int l, int t) { return gethostbyaddr(a, l, t); }
int WINAPI x_gethostname(char *n, int l) { return gethostname(n, l); }
int WINAPI x_WSAAsyncSelect(SOCKET s, HWND w, u_int m, long ev)
{
    if (okw_on) tr("game WSAAsyncSelect events %lx", (unsigned long)ev);
    return WSAAsyncSelect(s, w, m, ev);
}
HANDLE WINAPI x_WSAAsyncGetHostByName(HWND w, u_int m, const char *n, char *b, int l) { return WSAAsyncGetHostByName(w, m, n, b, l); }
int WINAPI x_WSAGetLastError(void) { return WSAGetLastError(); }
void WINAPI x_WSASetLastError(int e) { WSASetLastError(e); }
int WINAPI x_WSAStartup(WORD v, LPWSADATA d)
{
    if (!okw_on) return WSAStartup(v, d);
    if (!cbt_hook) cbt_hook = SetWindowsHookExA(WH_CBT, cbt_proc, NULL, GetCurrentThreadId());
    return WSAStartup(v, d);
}
/* the game is done with the network: a room it hosted comes off the list now (the process may end next) */
int WINAPI x_WSACleanup(void)
{
    if (!okw_on) return WSACleanup();
    okp_log(2, "wsock: the game calls WSACleanup (it is done with the network)");
    int withdraw = 0;
    EnterCriticalSection(&okw_lock);
    if (ctx && (ctx->is_host || ctx->closing) && ctx->room && ctx->mode == OKP_MODE_ONLINE) {
        withdraw = 1;
        ctx->is_host = 0;
        ctx->session_open = 0;
        ctx->closing = 1;
    }
    LeaveCriticalSection(&okw_lock);
    if (withdraw) okp_rdv_close(ctx);
    int r = WSACleanup();
    okp_log(2, "wsock: WSACleanup done (%d)", r);
    return r;
}
int WINAPI x___WSAFDIsSet(SOCKET s, fd_set *f) { return __WSAFDIsSet(s, f); }

/* ---------------------------------------------------------------- the exit watch
 *
 * Closed in the middle of a race, Road Rash's window goes but the game never reaches its own shutdown: the process
 * stays, with no window, until it is ended in Task Manager (on Windows 98, often a restart). When this process has
 * had no window of its own for exit_watch seconds (okplay.ini, default 10, 0 = off), okplay stops the game's
 * multimedia timers and ends the process. (A timer left running by a game ended on Windows 98 broke the next run's
 * 33 ms tick until Windows restarted; stopped first, the next run is as usual - tested on Windows 98.) */
static UINT ew_tmrs[16];
static volatile LONG ew_n;
static MMRESULT (WINAPI *ew_real_tse)(UINT, UINT, LPTIMECALLBACK, DWORD_PTR, UINT);
static MMRESULT (WINAPI *ew_real_tke)(UINT);

static MMRESULT WINAPI ew_tse(UINT delay, UINT res, LPTIMECALLBACK cb, DWORD_PTR user, UINT ev)
{
    MMRESULT id = ew_real_tse(delay, res, cb, user, ev);
    if (id) {
        LONG i = InterlockedIncrement(&ew_n) - 1;
        if (i < 16) ew_tmrs[i] = id; else InterlockedDecrement(&ew_n);
    }
    return id;
}

static MMRESULT WINAPI ew_tke(UINT id)
{
    for (LONG i = 0; i < ew_n && i < 16; i++) if (ew_tmrs[i] == id) ew_tmrs[i] = 0;
    return ew_real_tke(id);
}

/* the game's import of `fn` from `dll` now calls `to` (the exe's import table, in memory only) */
static int hook_exe_import(const char *dll, const char *fn, void *to, void **orig)
{
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    HMODULE m = GetModuleHandleA(dll);
    void *real = m ? (void *)GetProcAddress(m, fn) : NULL;
    if (!dd->VirtualAddress || !real) return 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dd->VirtualAddress); d->Name; d++) {
        if (_stricmp((char *)(base + d->Name), dll)) continue;
        for (void **slot = (void **)(base + d->FirstThunk); *slot; slot++) {
            if (*slot != real) continue;
            DWORD old;
            if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) return 0;
            *orig = *slot;
            *slot = to;
            VirtualProtect(slot, sizeof(*slot), old, &old);
            return 1;
        }
    }
    return 0;
}

static BOOL CALLBACK ew_any_window(HWND w, LPARAM lp)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(w)) { *(int *)lp = 1; return FALSE; }
    return TRUE;
}

static DWORD WINAPI ew_backstop(LPVOID arg)
{
    (void)arg;
    Sleep(6000);
    okp_log(0, "exit watch: the process did not end within 6 s - ending it now");
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}

static DWORD WINAPI ew_thread(LPVOID arg)
{
    int secs = (int)(INT_PTR)arg, seen = 0, gone = 0;
    for (;;) {
        Sleep(1000);
        int any = 0;
        EnumWindows(ew_any_window, (LPARAM)&any);
        if (any) { seen = 1; gone = 0; continue; }
        if (!seen) continue;   /* (no window yet: still starting) */
        if (++gone == 1) okp_log(2, "exit watch: the game has no window any more - waiting for it to end");
        if (gone < secs) continue;
        int k = 0;
        for (LONG i = 0; i < ew_n && i < 16; i++)
            if (ew_tmrs[i] && ew_real_tke) { ew_real_tke(ew_tmrs[i]); ew_tmrs[i] = 0; k++; }
        okp_log(0, "exit watch: the game's window closed %d s ago but the game did not end - %d timer(s) stopped, "
                   "ending it (okplay.ini exit_watch=0 turns this off)", gone, k);
        DWORD id;
        CloseHandle(CreateThread(NULL, 0, ew_backstop, NULL, 0, &id));
        ExitProcess(0);
    }
    return 0;
}

static void exit_watch_start(const char *ini)
{
    int secs = GetPrivateProfileIntA("okplay", "exit_watch", 10, ini);
    if (secs <= 0) { okp_log(2, "exit watch: off (okplay.ini exit_watch=0)"); return; }
    if (secs < 3) secs = 3;
    int t = hook_exe_import("WINMM.DLL", "timeSetEvent", (void *)ew_tse, (void **)&ew_real_tse);
    hook_exe_import("WINMM.DLL", "timeKillEvent", (void *)ew_tke, (void **)&ew_real_tke);
    DWORD id;
    CloseHandle(CreateThread(NULL, 0, ew_thread, (LPVOID)(INT_PTR)secs, 0, &id));
    okp_log(2, "exit watch: on - a game left running %d s after its window closed is ended%s", secs,
            t ? "" : " (its timers are not seen)");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD why, LPVOID res)
{
    (void)res;
    if (why == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        okw_on = is_winsock_game();
        if (!okw_on) return TRUE;   /* not a game this is for: Windows' Winsock only (see is_winsock_game) */
        InitializeCriticalSection(&okw_lock);
        okp_sys_init();
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
        okp_log_init(inst);
        okp_crash_install(inst);   /* a crash leaves okplay-crash-*.dmp in logs\ */
#ifdef OKW_LAB
        rr_lab_init();
#endif
        InitializeCriticalSection(&okw_ask_lock);
        InitializeCriticalSection(&ui_lock);
        box_done = CreateEventA(NULL, TRUE, TRUE, NULL);
        {
            static char ini[MAX_PATH];
            snprintf(ini, sizeof(ini), "%sokplay.ini", okp_dll_dir());
            okbox_init(inst, NULL, ini);
            okbox_set_topmost(TRUE);
            exit_watch_start(ini);
            rrf_init(ini);   /* rr_fix.c: Road Rash's netcode fixes, for the builds it knows */
            InitializeCriticalSection(&tr_lock);
            tr_on = GetPrivateProfileIntA("okplay", "wsock_trace", 0, ini);
            if (tr_on) { DWORD id; CloseHandle(CreateThread(NULL, 0, tr_writer, NULL, 0, &id)); }
        }
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}
