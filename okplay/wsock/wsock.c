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
#define OKW_CTRL  2   /* okplay's own: 'A' + password hash (joiner -> host), 'Y' / 'N' (host -> joiner) */

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
static uint16_t next_eph = 30000;
static char advert[1024];          /* host: the game's last broadcast */
static int advert_len;
static uint16_t advert_port;
static DWORD advert_tick, browse_tick, inject_tick;

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
    okw_sock *any = NULL;
    for (int i = 0; i < OKW_MAX_SOCKS; i++) {
        okw_sock *k = &socks[i];
        if (!k->used || k->port != dport) continue;
        if (k->peer_ip) {
            if (k->peer_ip == from_ip && k->peer_port == sport) {
                tr("rx %u<-%u len %d type %d (connected, q %d)", dport, sport, len, len ? ((const uint8_t *)data)[0] : -1, k->qn);
                queue_pkt(k, from_ip, sport, data, len);
                return;
            }
        } else if (!any) {
            any = k;
        }
    }
    if (any) {
        tr("rx %u<-%u len %d type %d (unconnected, q %d)", dport, sport, len, len ? ((const uint8_t *)data)[0] : -1, any->qn);
        queue_pkt(any, from_ip, sport, data, len);
    } else {
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

static void control(uint64_t from, const uint8_t *b, int n)   /* okw_lock held */
{
    if (n < 1) return;
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

static void okw_rx(void *c, uint64_t from, const void *data, int len)   /* okplay's net thread */
{
    (void)c;
    if (len < (int)sizeof(okw_hdr)) return;
    const okw_hdr *h = (const okw_hdr *)data;
    if (h->m0 != 'W' || h->m1 != 'K') return;
    const uint8_t *body = (const uint8_t *)data + sizeof(*h);
    int blen = len - (int)sizeof(*h);
    EnterCriticalSection(&okw_lock);
    if (h->flags & OKW_CTRL) {
        control(from, body, blen);
    } else if (ctx && ctx->is_host && host_has_pw && !is_authed(from)) {
        /* a password game: nothing from a joiner reaches the game before its password matched */
    } else {
        deliver(standin_of(from), h->sport, h->dport, body, blen);
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
    c->self = okp_rand64();
    c->raw_rx = okw_rx;
    c->enum_app = OKW_APPGUID;
    okp_log_header();
    okp_log_sysinfo();
    if (!okp_net_start(c) || !okp_rdv_start(c)) {
        okp_log(0, "wsock: okplay could not start");
        return;
    }
    ctx = c;
    char b[20];
    okp_log(1, "wsock: okplay " OKP_VERSION_STR " %s for a Winsock game, peer %016llx, stand-in address %s", c->mode_name,
            (unsigned long long)c->self, ip_s(standin_of(c->self), b));
}

/* become the host of a room (the game broadcast) */
static void host_room(void)   /* okw_lock held */
{
    if (!ctx || ctx->is_host) return;
    ctx->room = pending_room ? pending_room : ((uint32_t)okp_rand64() | 1);
    snprintf(ctx->tok, sizeof(ctx->tok), "%016llx", (unsigned long long)okp_rand64());
    ctx->is_host = 1;
    ctx->closing = 0;
    build_room_record();
    ctx->session_open = 1;
    if (ctx->mode == OKP_MODE_LAN && !okp_lan_open_listen(ctx))
        okp_log(0, "wsock: LAN discovery port %u unavailable - joiners will not see this game", OKP_LAN_PORT);
    okp_log(1, "wsock: hosting room %08x", ctx->room);
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
    else b->r = okbox_ask_join_password(&b->g, b->w, 64);
    InterlockedExchange(&box_open, 0);
    if (h) UnhookWindowsHookEx(h);
    if (box_done) SetEvent(box_done);
    for (int i = 0; i < ndeferred; i++) PostMessageA(deferred[i].w, deferred[i].m, deferred[i].wp, deferred[i].lp);
    ndeferred = 0;
    return b->r;
}

/* the first UDP socket (or own-address lookup) of a game run: Online Play, LAN Play or the original TCP/IP.
 * okplay.ini wsock_mode=ask (default) | online | lan | original; tests: OKP_WSOCK_MODE. No okw_lock held. */
static int choose_mode(void)
{
    if (mode_choice) return mode_choice;
    if (box_open) return 2;   /* back in from inside the box (9x): okplay for now, the box decides */
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
        InterlockedExchange(&mode_choice, m);
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
    if (!pw) return 1;   /* not a locked room */
    *pw = 0;
    uint64_t host = 0;
    uint32_t room = 0;
    EnterCriticalSection(&okw_lock);
    EnterCriticalSection(&ctx->lock);
    for (int i = 0; i < ctx->nrooms && !host; i++) {
        uint16_t port;
        const uint8_t *ad;
        int al = room_advert(&ctx->rooms[i], &port, &ad);
        uint32_t fl = 0;
        if (ctx->rooms[i].enumlen >= 16) memcpy(&fl, ctx->rooms[i].enumblob + 8 + 4, 4);
        if (al == 57 && (fl & 0x400) && !strncmp((const char *)ad + 37, text, strlen(text))) {
            host = ctx->rooms[i].host;
            room = ctx->rooms[i].room;
        }
    }
    LeaveCriticalSection(&ctx->lock);
    int known = host && auth_host == host && (auth_state == 1 || auth_state == 2);
    LeaveCriticalSection(&okw_lock);
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

static int set_err(int e) { WSASetLastError(e); return SOCKET_ERROR; }

SOCKET WINAPI x_socket(int af, int type, int proto)
{
    SOCKET s = socket(af, type, proto);
    if (s == INVALID_SOCKET || af != AF_INET || type != SOCK_DGRAM) return s;
    if (choose_mode() == 1) return s;   /* the original TCP/IP: Windows' own socket, untouched */
    EnterCriticalSection(&okw_lock);
    start_okplay();
    for (int i = 0; i < OKW_MAX_SOCKS; i++) {
        if (socks[i].used) continue;
        memset(&socks[i], 0, sizeof(socks[i]));
        socks[i].used = 1;
        socks[i].s = s;
        break;
    }
    LeaveCriticalSection(&okw_lock);
    return s;
}

int WINAPI x_closesocket(SOCKET s)
{
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
        }
        for (okw_pkt *p = k->q, *n; p; p = n) { n = p->next; free(p); }
        k->used = 0;
    }
    LeaveCriticalSection(&okw_lock);
    return closesocket(s);
}

int WINAPI x_bind(SOCKET s, const struct sockaddr *a, int l)
{
    EnterCriticalSection(&okw_lock);
    okw_sock *k = sock_of(s);
    if (!k) { LeaveCriticalSection(&okw_lock); return bind(s, a, l); }
    if (!a || l < (int)sizeof(struct sockaddr_in) || a->sa_family != AF_INET) { LeaveCriticalSection(&okw_lock); return set_err(WSAEFAULT); }
    const struct sockaddr_in *in = (const struct sockaddr_in *)a;
    uint16_t port = ntohs(in->sin_port);
    if (!port) port = next_eph++;
    k->port = port;
    k->bound_ip = in->sin_addr.s_addr;
    LeaveCriticalSection(&okw_lock);
    return 0;
}

int WINAPI x_connect(SOCKET s, const struct sockaddr *a, int l)
{
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

static int do_send(okw_sock *k, uint32_t ip, uint16_t port, const char *b, int len)   /* lock held */
{
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
    wait_for_box();
    if (a && al >= (int)sizeof(struct sockaddr_in)) boxes_before_send(((const struct sockaddr_in *)a)->sin_addr.s_addr);
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
    wait_for_box();
    int handled;
    int r = do_recv(s, b, len, NULL, NULL, &handled);
    return handled ? r : recv(s, b, len, f);
}

int WINAPI x_recvfrom(SOCKET s, char *b, int len, int f, struct sockaddr *a, int *al)
{
    wait_for_box();
    int handled;
    int r = do_recv(s, b, len, a, al, &handled);
    return handled ? r : recvfrom(s, b, len, f, a, al);
}

int WINAPI x_ioctlsocket(SOCKET s, long cmd, u_long *arg)
{
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
    } else {
        r = set_err(WSAEINVAL);
    }
    LeaveCriticalSection(&okw_lock);
    return r;
}

int WINAPI x_setsockopt(SOCKET s, int lv, int o, const char *v, int l)
{
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
    static struct hostent he;
    static char name[256];
    static uint32_t addr;
    static char *list[2];
    char me[256] = "";
    gethostname(me, sizeof(me));
    if (choose_mode() == 1) return gethostbyname(n);
    EnterCriticalSection(&okw_lock);
    start_okplay();
    int mine = ctx && (!n || !n[0] || !_stricmp(n, me));
    if (!mine) { LeaveCriticalSection(&okw_lock); return gethostbyname(n); }
    addr = standin_of(ctx->self);
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
int WINAPI x_select(int n, fd_set *r, fd_set *w, fd_set *e, const struct timeval *t) { return select(n, r, w, e, t); }
int WINAPI x_shutdown(SOCKET s, int h) { return shutdown(s, h); }
struct hostent *WINAPI x_gethostbyaddr(const char *a, int l, int t) { return gethostbyaddr(a, l, t); }
int WINAPI x_gethostname(char *n, int l) { return gethostname(n, l); }
int WINAPI x_WSAAsyncSelect(SOCKET s, HWND w, u_int m, long ev) { return WSAAsyncSelect(s, w, m, ev); }
HANDLE WINAPI x_WSAAsyncGetHostByName(HWND w, u_int m, const char *n, char *b, int l) { return WSAAsyncGetHostByName(w, m, n, b, l); }
int WINAPI x_WSAGetLastError(void) { return WSAGetLastError(); }
void WINAPI x_WSASetLastError(int e) { WSASetLastError(e); }
int WINAPI x_WSAStartup(WORD v, LPWSADATA d)
{
    if (!cbt_hook) cbt_hook = SetWindowsHookExA(WH_CBT, cbt_proc, NULL, GetCurrentThreadId());
    return WSAStartup(v, d);
}
/* the game is done with the network: a room it hosted comes off the list now (the process may end next) */
int WINAPI x_WSACleanup(void)
{
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
    return WSACleanup();
}
int WINAPI x___WSAFDIsSet(SOCKET s, fd_set *f) { return __WSAFDIsSet(s, f); }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD why, LPVOID res)
{
    (void)res;
    if (why == DLL_PROCESS_ATTACH) {
        g_self = inst;
        InitializeCriticalSection(&okw_lock);
        okp_sys_init();
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
        okp_log_init(inst);
        InitializeCriticalSection(&okw_ask_lock);
        box_done = CreateEventA(NULL, TRUE, TRUE, NULL);
        {
            static char ini[MAX_PATH];
            snprintf(ini, sizeof(ini), "%sokplay.ini", okp_dll_dir());
            okbox_init(inst, NULL, ini);
            okbox_set_topmost(TRUE);
            InitializeCriticalSection(&tr_lock);
            tr_on = GetPrivateProfileIntA("okplay", "wsock_trace", 0, ini);
            if (tr_on) { DWORD id; CloseHandle(CreateThread(NULL, 0, tr_writer, NULL, 0, &id)); }
        }
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}
