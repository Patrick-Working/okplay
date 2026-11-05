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
#include <stdio.h>
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

static void queue_pkt(okw_sock *k, uint32_t from_ip, uint16_t from_port, const void *data, int len)   /* lock held */
{
    if (k->qn > 256) return;   /* a game that stopped reading: drop, as a full socket buffer does */
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
            if (k->peer_ip == from_ip && k->peer_port == sport) { queue_pkt(k, from_ip, sport, data, len); return; }
        } else if (!any) {
            any = k;
        }
    }
    if (any) queue_pkt(any, from_ip, sport, data, len);
}

static void okw_rx(void *c, uint64_t from, const void *data, int len)   /* okplay's net thread */
{
    (void)c;
    if (len < (int)sizeof(okw_hdr)) return;
    const okw_hdr *h = (const okw_hdr *)data;
    if (h->m0 != 'W' || h->m1 != 'K') return;
    EnterCriticalSection(&okw_lock);
    deliver(standin_of(from), h->sport, h->dport, (const uint8_t *)data + sizeof(*h), len - (int)sizeof(*h));
    LeaveCriticalSection(&okw_lock);
}

static int send_to_peer(uint64_t peer, uint16_t sport, uint16_t dport, int flags, const void *buf, int len)
{
    uint8_t pkt[sizeof(okw_hdr) + 2048];
    if (len > 2048 || !ctx) return 0;
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
    char ini[MAX_PATH], v[16] = "";
    snprintf(ini, sizeof(ini), "%sokplay.ini", okp_dll_dir());
    GetPrivateProfileStringA("okplay", "wsock_mode", "online", v, sizeof(v), ini);
    GetEnvironmentVariableA("OKP_WSOCK_MODE", v, sizeof(v));   /* tests: online / lan */
    want_lan = !_stricmp(v, "lan");
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
    ctx->room = (uint32_t)okp_rand64() | 1;
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

/* every call from the game: while it isn't hosting or in a room and has a socket waiting for broadcasts
 * (a server browser), list the rooms and feed their broadcasts to that socket */
static void okw_tick(void)   /* okw_lock held */
{
    DWORD now = okp_now();
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
            if (al > 0) deliver(standin_of(rooms[i].host), port, port, ad, al);
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

/* ---------------------------------------------------------------- the Winsock calls */

static int set_err(int e) { WSASetLastError(e); return SOCKET_ERROR; }

SOCKET WINAPI x_socket(int af, int type, int proto)
{
    SOCKET s = socket(af, type, proto);
    if (s == INVALID_SOCKET || af != AF_INET || type != SOCK_DGRAM) return s;
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
    send_to_peer(peer, k->port, port, 0, b, len);
    return len;
}

int WINAPI x_sendto(SOCKET s, const char *b, int len, int f, const struct sockaddr *a, int al)
{
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
    int handled;
    int r = do_recv(s, b, len, NULL, NULL, &handled);
    return handled ? r : recv(s, b, len, f);
}

int WINAPI x_recvfrom(SOCKET s, char *b, int len, int f, struct sockaddr *a, int *al)
{
    int handled;
    int r = do_recv(s, b, len, a, al, &handled);
    return handled ? r : recvfrom(s, b, len, f, a, al);
}

int WINAPI x_ioctlsocket(SOCKET s, long cmd, u_long *arg)
{
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
int WINAPI x_WSAStartup(WORD v, LPWSADATA d) { return WSAStartup(v, d); }
int WINAPI x_WSACleanup(void) { return WSACleanup(); }
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
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}
