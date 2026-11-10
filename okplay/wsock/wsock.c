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
 * TCP games (Need for Speed III, profile "nfs3"): stream sockets are virtual too. Each connection travels as
 * 'W','T' frames on okplay's reliable lane (whole and in order, like TCP), with a credit window per connection;
 * listen/accept/connect/send/recv/shutdown/closesocket and select (emulated: an event, not polling) behave as
 * Windows' TCP for the patterns such games use (tests/tcpsim.c copies them; tests/tcp_run.py compares the two).
 * listen() on a nonzero port makes this PC the host of a room ('OKWT' in the room record); a joiner's connect goes
 * to a room host by stand-in address, the host's own address, the room picked before, wsock_join=auto, or okplay's
 * room list box; passwords and the build check as for Road Rash; a broken lane, a silent player (drop_after) or a
 * player whose game ended reset the connections. Still provisional: the original 1998 exe binds 1030 on both sides
 * (a listen on 1030 hosts only after 3 s without a connect).
 *
 * First game: Road Rash (TCP/IP mode: adverts on 6995, join on 6996, one connected socket per link).
 * Settings: okplay.ini [okplay] wsock_mode=online|lan (default online); server, key, logs as the drop-in.
 * Calls other than UDP over IPv4 go to Windows' Winsock 2 (ws2_32.dll) unchanged.
 *
 * Status (2026-11-05): first version. Not yet: the Online/LAN + password window, IPX. */
#define WIN32_LEAN_AND_MEAN
#include "okp.h"
#include "rr_fix.h"
#include "nfs3_fix.h"
#include "ddshow.h"
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

/* Stream (TCP) sockets, for games that play over TCP (Need for Speed III): each connection is carried over
 * okplay's reliable lane as 'W','T' frames (below), so it arrives whole and in order like a TCP stream, through
 * the same hole-punched path or relay as everything else. Ports are virtual, per process. */
enum { K_UDP = 1, K_TCP = 2 };
enum { T_NEW, T_BOUND, T_LISTEN, T_SYN_SENT, T_ESTAB, T_FAILED };
#define OKW_MAX_CONNS 32
#define OKW_BACKLOG 8
#define OKW_TDATA_MAX 1100   /* stream bytes per frame: header + body stay under okplay's 1200-byte datagram */
typedef struct okw_tconn {          /* one stream connection (also one waiting in a listener's backlog) */
    int used, state;
    uint64_t peer;                  /* the other player's okplay id */
    uint32_t id;                    /* chosen by the side that connected (random, nonzero) */
    int initiator;                  /* 1 = we connected; frames carry it, so the two sides' ids can't collide */
    uint16_t lport, rport;          /* virtual ports (ours, theirs) */
    uint32_t rip;                   /* their stand-in, network order */
    uint32_t snd_next, snd_limit, snd_wnd;   /* our next byte's offset; we may send below snd_limit; their window */
    int wr_shut, rd_shut;           /* FIN sent / shutdown(SD_RECEIVE) */
    uint8_t *ring;                  /* bytes the game hasn't read; cap = our window */
    uint32_t cap, head, count;
    uint32_t rx_next;               /* the offset expected next (the lane kept the stream whole) */
    uint32_t consumed, adv;         /* bytes the game has read; the last such count told to the sender */
    int fin_rx, err, so_error;      /* their FIN came; pending error for recv/send; SO_ERROR of a failed connect */
    int rst_sent, rst_rx;
    uint32_t dst_ip;                /* joiner: the address the game connected to (SYN adoption) */
    int orphan, syn_held;           /* the game closed it while connecting (kept 3 s) / the SYN waits until syn_due */
    DWORD orphan_tick, syn_due;
    int info_init;                  /* okplay lane counters when it began: a change = the lane broke */
    uint32_t base_giveup, base_restarts;
    uint32_t st_txf, st_txb, st_rxf, st_rxb, st_wb;   /* the 10 s statistics */
    DWORD st_tick, st_lastrx, st_maxgap;
    uint32_t rx_gen;                /* bumped on every arrival (data, FIN, RST): select's no-progress rule */
    uint32_t sel_gen;
    int sel_shown, sel_consumed;
    DWORD t_state;
    int held;                       /* okp_peer_hold taken on the peer for this connection (tconn_hold) */
} okw_tconn;

#define OKW_MAX_SOCKS 64
typedef struct {
    SOCKET s;            /* the real handle the game holds (never used for I/O) */
    int used;
    int kind;            /* K_UDP, K_TCP (a TCP socket is virtual only once Online/LAN Play was chosen) */
    uint16_t port;       /* bound port, host order (0 = not bound) */
    uint32_t bound_ip;   /* network order, as the game asked (informational) */
    uint32_t peer_ip;    /* connected peer (0 = not connected) */
    uint16_t peer_port;
    int nonblock, bcast;
    okw_pkt *q, *qtail;
    int qbytes, qn;
    /* K_TCP */
    int tstate, explicit_port;      /* T_*; bind asked for a nonzero port (listen on one = hosting) */
    okw_tconn *c;                   /* T_SYN_SENT / T_ESTAB / T_FAILED */
    okw_tconn *backlog[OKW_BACKLOG];   /* T_LISTEN: connections the game hasn't accepted yet, oldest first */
    int nbacklog, backlog_max;
    int keepalive, nodelay;         /* recorded, no effect (frames go at once anyway) */
    struct linger linger;
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
static okw_tconn conns[OKW_MAX_CONNS];
static uint32_t tcp_window = 65536;   /* okplay.ini wsock_tcp_window: receive buffer per connection */
static HANDLE okw_ev;                 /* manual-reset: set whenever a virtual socket may have become ready */
static volatile LONG okw_waiters;     /* threads waiting on okw_ev (select, blocking calls) */
static uint64_t picked_host;          /* joiner: the room host a stream connect went to (its retries go there too) */
static int join_auto;                 /* okplay.ini wsock_join=auto (OKP_WSOCK_JOIN): the only room of the game, no list */
static uint16_t relisten_port;        /* the explicit port of the last listener the game closed, and when: listening */
static DWORD relisten_tick;           /* there again within 2 s is NFS3's Host Game (see x_listen) */
/* The game's own word on hosting (game->says_hosts, nfs3_fix.c), on the builds it knows:
 * gs_net, gs_srv: the two values as last read, for the log only (-1 = not read yet).
 *   Set: x_listen at every listen on an explicit port; game_state_watch (x_select) while a listener is open.
 *   Cleared: never - they describe the game, not an okplay session.
 * close_owed: a room this PC hosted that the game closed (tcp_close), whose close may not have reached the room
 *   server yet; it goes from here with that room's id and token before ctx->room changes (send_owed_close).
 *   Set: host_game_over from tcp_close (known builds, Online Play, nobody left in the room).
 *   Cleared: send_owed_close (x_listen before a new room, tcp_unhost_for_connect before a join); x_WSACleanup (it
 *   withdrew that room itself); host_room (a room is hosted: whatever was owed is past); end_session. */
static int gs_net = -1, gs_srv = -1, gs_type = -1;   /* (gs_type: the game type, game->max_players; -1 = not read) */
static uint32_t close_owed;
static struct { uint64_t peer; DWORD tick; } closed_peers[8];   /* players we had stream connections with (tcp_linger) */
static int nclosed_peers;
static int player_name_set;           /* okplay.ini has a name= of the player's own */
static char player_name[32];          /* okplay.ini name=, else the computer's name: in a TCP game's room name */
/* The player limit of a hosted TCP game (room_max_players):
 * max_players_ini: okplay.ini wsock_max_players (2..64; 0 = not set). Read once, at load.
 * rec_curp, rec_maxp: the players and the limit the room record says now (-1 = none built). Set: build_room_record.
 *   Cleared: end_session. room_record_follow rebuilds the record when either would change.
 * full_told: joiner: the host whose "the game is full" the player was told (once per host). Set: tcp_rx (REFUSE
 *   reason 4). Cleared: a connection to that host comes up (ACCEPT); end_session. */
static int max_players_ini;
static int rec_curp = -1, rec_maxp = -1;
static uint64_t full_told;

/* Per-game profiles: which socket types are carried over okplay, and what makes a PC the host. */
#define OKW_UDP 1
#define OKW_TCP 2
#define HOST_BCAST 1      /* UDP: the game's first broadcast */
#define HOST_LISTEN 2     /* TCP: listen() on a socket bound to a nonzero port (a joiner binds port 0) */
typedef struct {
    const char *id;       /* OKP_WSOCK_GAME / okplay.ini wsock_game= */
    const char *exes;     /* exe names that pick it, "a.exe,b.exe"; NULL = only by id */
    const char *name;     /* room name */
    int kinds, host_rule;
    int (*fix_init)(const char *ini);         /* DllMain: the game's own file (rr_fix.c, nfs3_fix.c), NULL = none */
    int (*says_hosts)(int *net, int *server); /* does the game say it hosts? 1 yes, 0 no, -1 not known (x_listen) */
    void (*at_listen)(int okplay_on);         /* the game listens (x_listen): 1 = through okplay, 0 = its own TCP/IP */
    int (*max_players)(int *type);            /* how many players the game being hosted takes, host included; 0 = not
                                               * known (room_max_players); type: the game's own value, for the log */
    int (*player_name)(char *out, int n);     /* the player's name in the game itself (the room is "<name>'s game");
                                               * 1 = read, 0 = not known. NULL = the game has none we can read */
} okw_game;
static const okw_game okw_profiles[] = {
    { "roadrash", "roadrash.exe,roadrash95.exe", "Road Rash", OKW_UDP, HOST_BCAST, rrf_init, NULL, NULL, NULL, NULL },   /* Road Rash: CD, Steam/GOG */
    /* Need for Speed III (the 1998 CD's NFS3.EXE, Modern Patch, portable): TCP only (its IPX sockets pass through) */
    { "nfs3", "nfs3.exe", "Need for Speed III", OKW_TCP, HOST_LISTEN, nfs3_init, nfs3_hosting, nfs3_join_prefill,
      nfs3_max_players, nfs3_player_name },
};
static const okw_game *game = &okw_profiles[0];

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

static okw_sock *sock_of(SOCKET s)   /* okw_lock held; a virtual UDP socket */
{
    for (int i = 0; i < OKW_MAX_SOCKS; i++) if (socks[i].used && socks[i].s == s && socks[i].kind == K_UDP) return &socks[i];
    return NULL;
}

static okw_sock *tsock_of(SOCKET s)   /* okw_lock held; a TCP socket in the table (virtual once mode_choice >= 2) */
{
    for (int i = 0; i < OKW_MAX_SOCKS; i++) if (socks[i].used && socks[i].s == s && socks[i].kind == K_TCP) return &socks[i];
    return NULL;
}

static void wake(void) { if (okw_ev) SetEvent(okw_ev); }

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
    wake();   /* (select) */
}

/* deliver to the game: the socket bound to dport that is connected to this sender, else an unconnected one */
static void deliver(uint32_t from_ip, uint16_t sport, uint16_t dport, const void *data, int len)   /* lock held */
{
    int n = 0;
    for (int i = 0; i < OKW_MAX_SOCKS; i++) {
        okw_sock *k = &socks[i];
        if (!k->used || k->kind != K_UDP || k->port != dport || !k->peer_ip) continue;
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
        if (!k->used || k->kind != K_UDP || k->port != dport || k->peer_ip) continue;
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

static volatile LONG ui_pending;   /* (the notices, below) */
static volatile LONG ui_wrongpw;   /* a TCP game's late "wrong password": shown through run_box like the other notices */
static volatile LONG ui_full;      /* a TCP game's host said its game is full (REFUSE reason 4): "That game is full." */
static volatile LONG ui_noanswer;  /* a TCP join came up and the host's game never sent a byte before our game gave up: "No answer from the host." */

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
                if (game->kinds & OKW_TCP) {   /* a full-screen game: only run_box keeps a box in sight (ddshow.c) */
                    InterlockedExchange(&ui_wrongpw, 1);
                    InterlockedExchange(&ui_pending, 1);
                } else {
                    HANDLE t = CreateThread(NULL, 0, wrong_password_box, NULL, 0, NULL);
                    if (t) CloseHandle(t);
                }
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

/* ---------------------------------------------------------------- stream frames
 *
 * 'W','T' frames, always on okplay's reliable lane (OKP_SEND_RELIABLE, whatever the host's transport setting),
 * so per peer they arrive whole and in send order: FIN after the last DATA, a RST after any data before it.
 *   1 SYN     joiner -> host  u16 sport, u16 dport, u32 window, u8 caps   open a connection to the host's port
 *   2 ACCEPT  host -> joiner  u16 lport, u32 window   in the host's backlog (TCP: the handshake is done before accept())
 *   3 REFUSE  host -> joiner  u8 reason: 1 nothing listening, 2 backlog full, 3 not let in (password),
 *                             4 the game is full (newer hosts; every reason is "connection refused" for the game)
 *   4 DATA    both            stream bytes at `off` (at most OKW_TDATA_MAX: never cut into pieces)
 *   5 WND     both            the game has read up to `off`: the sender may go to off + window
 *   6 FIN     both            no more bytes after `off`
 *   7 RST     both            u8 reason: 1 closed with unread data, 2 unknown connection, 3 stream broken,
 *                             4 listener closed, 5 connect abandoned
 * A connection is (peer, id, initiator); a frame says whether the initiator sent it. */
#pragma pack(push, 1)
typedef struct {
    uint8_t m0, m1;         /* 'W','T' */
    uint8_t ver;            /* 1 */
    uint8_t type;
    uint32_t conn;
    uint8_t flags;          /* 1 = sent by the initiator */
    uint8_t rsv;
    uint32_t off;
} okw_thdr;
#pragma pack(pop)
enum { TF_SYN = 1, TF_ACCEPT, TF_REFUSE, TF_DATA, TF_WND, TF_FIN, TF_RST };
static const char *const tf_name[] = { "?", "SYN", "ACCEPT", "REFUSE", "DATA", "WND", "FIN", "RST" };

static void tframe(uint64_t peer, uint32_t id, int initiator, int type, uint32_t off, const void *body, int blen)   /* okw_lock held */
{
    uint8_t f[sizeof(okw_thdr) + OKW_TDATA_MAX];
    okw_thdr *h = (okw_thdr *)f;
    if (!ctx || blen > OKW_TDATA_MAX) return;
    h->m0 = 'W'; h->m1 = 'T'; h->ver = 1; h->type = (uint8_t)type;
    h->conn = id; h->flags = initiator ? 1 : 0; h->rsv = 0; h->off = off;
    if (blen) memcpy(f + sizeof(*h), body, blen);
    if (type != TF_DATA) tr("tcp tx %s conn %08x off %u", tf_name[type], (unsigned)id, (unsigned)off);
    okp_send_data(ctx, peer, f, (int)sizeof(*h) + blen, OKP_SEND_RELIABLE);
}
#define TFRAME(c, type, off, body, blen) tframe((c)->peer, (c)->id, (c)->initiator, type, off, body, blen)

static okw_tconn *tconn_new(void)   /* okw_lock held */
{
    for (int i = 0; i < OKW_MAX_CONNS; i++) {
        okw_tconn *c = &conns[i];
        if (c->used) continue;
        memset(c, 0, sizeof(*c));
        c->ring = (uint8_t *)malloc(tcp_window);
        if (!c->ring) return NULL;
        c->used = 1;
        c->cap = tcp_window;
        c->t_state = GetTickCount();
        return c;
    }
    okp_log(1, "wsock: no room for another TCP connection (%d in use)", OKW_MAX_CONNS);
    return NULL;
}

/* The core forgets a peer that has no DirectPlay players once its path is quiet for a while (it never knows the
 * shim's players), and with it the lane a stream lives on. So each stream connection holds its peer: taken once
 * when the connection begins, given back once - when it dies (reset, refused, given up) or is freed, whichever
 * comes first. okw_lock held; ctx->lock is taken inside (the usual order). */
static void tconn_hold(okw_tconn *c)
{
    if (!ctx || c->held) return;
    EnterCriticalSection(&ctx->lock);   /* (okp_peer_hold enters it again: the peer can't go in between) */
    if (okp_peer_get(ctx, c->peer, 0)) { okp_peer_hold(ctx, c->peer, 1); c->held = 1; }
    LeaveCriticalSection(&ctx->lock);
}

static void tconn_unhold(okw_tconn *c)   /* okw_lock held; ctx is the one the hold was taken on (end_session) */
{
    if (!c->held) return;
    c->held = 0;
    if (ctx) okp_peer_hold(ctx, c->peer, 0);
}

static void tconn_free(okw_tconn *c)   /* okw_lock held */
{
    if (!c) return;
    tconn_unhold(c);
    free(c->ring);
    memset(c, 0, sizeof(*c));
}

static okw_tconn *tconn_find(uint64_t peer, uint32_t id, int initiator)   /* okw_lock held */
{
    for (int i = 0; i < OKW_MAX_CONNS; i++)
        if (conns[i].used && conns[i].peer == peer && conns[i].id == id && conns[i].initiator == initiator) return &conns[i];
    return NULL;
}

/* abort: RST to the other side (once), the error for the game here */
static void tconn_abort(okw_tconn *c, int reason, int err)   /* okw_lock held */
{
    uint8_t r = (uint8_t)reason;
    if (!c->rst_sent && !c->rst_rx && (c->state == T_ESTAB || c->state == T_SYN_SENT)) TFRAME(c, TF_RST, c->snd_next, &r, 1);
    c->rst_sent = 1;
    if (!c->err) c->err = err;
    tconn_unhold(c);
    c->rx_gen++;
    okp_log(2, "wsock: TCP connection %08x with %016llx reset here (reason %d)", (unsigned)c->id, (unsigned long long)c->peer, reason);
    wake();
}

static void ring_put(okw_tconn *c, const uint8_t *d, uint32_t n)
{
    uint32_t tail = (c->head + c->count) % c->cap, first = n < c->cap - tail ? n : c->cap - tail;
    memcpy(c->ring + tail, d, first);
    memcpy(c->ring, d + first, n - first);
    c->count += n;
}

static void ring_peek(const okw_tconn *c, uint8_t *out, uint32_t n)
{
    uint32_t first = n < c->cap - c->head ? n : c->cap - c->head;
    memcpy(out, c->ring + c->head, first);
    memcpy(out + first, c->ring, n - first);
}

static int is_authed(uint64_t peer);
static void build_room_record(void);

/* The players in this PC's room: each player (okplay peer) that holds a live incoming stream connection, accepted by
 * the game or still in a listener's backlog, counted once - a joiner that closes and connects again at once (NFS3
 * does) must never count twice. A connection that was reset, or whose player sent its FIN, does not count. */
static int room_conn(const okw_tconn *c)
{
    return c->used && !c->initiator && c->state == T_ESTAB && !c->err && !c->fin_rx;
}

static int peer_in_room(uint64_t peer)   /* okw_lock held: that player holds a connection that counts */
{
    for (int i = 0; i < OKW_MAX_CONNS; i++) if (room_conn(&conns[i]) && conns[i].peer == peer) return 1;
    return 0;
}

static int tcp_room_players(void)   /* okw_lock held: players connected to this PC's room (accepted or not yet), each once */
{
    int n = 0;
    for (int i = 0; i < OKW_MAX_CONNS; i++) {
        int seen = 0;
        if (!room_conn(&conns[i])) continue;
        for (int j = 0; j < i && !seen; j++) if (room_conn(&conns[j]) && conns[j].peer == conns[i].peer) seen = 1;
        if (!seen) n++;
    }
    return n;
}

/* How many players the hosted game takes, this PC included: okplay.ini wsock_max_players (2..64) first, else what the
 * game itself says (game->max_players, on the builds its own file knows), else 8. okw_lock held (the hook only reads). */
static int room_max_players(void)
{
    int n;
    if (max_players_ini) return max_players_ini;
    n = game->max_players ? game->max_players(NULL) : 0;
    return n >= 2 ? n : 8;
}

/* The room record follows the room: rebuilt when the players it says or the limit would change - a player left (its
 * connection was closed, reset or got its FIN), or the game set its own limit after it listened. The room server gets
 * the new record with rdv.c's next announce (every announce_ms: 1 s; 3 s over plain HTTP), LAN Play with its next
 * offer. Called from tcp_rx (every frame), okw_tick (every game call) and tcp_close. okw_lock held. */
static char rec_who[32];   /* the player name the record carries (room_who) */

/* Whose room it is, for a TCP game's room name: okplay.ini name= when the player set one, else the name the player has
 * in the game itself (game->player_name), else the computer's name. okw_lock held (the hook only reads). */
static void room_who(char *out, int n)
{
    out[0] = 0;
    if (player_name_set) lstrcpynA(out, player_name, n);
    else if (!game->player_name || !game->player_name(out, n) || !out[0]) lstrcpynA(out, player_name, n);
}

static void room_record_follow(void)
{
    char who[32];
    if (!ctx || !ctx->is_host || !(game->kinds & OKW_TCP)) return;
    room_who(who, sizeof(who));
    if (1 + tcp_room_players() != rec_curp || room_max_players() != rec_maxp || strcmp(who, rec_who)) build_room_record();
}

static int room_counts(uint64_t host, int *curp, int *maxp);

/* a SYN: a listener on that port takes it into its backlog (the game's accept() comes later) */
static void tcp_syn(uint64_t from, const okw_thdr *h, const uint8_t *b, int bl)   /* okw_lock held */
{
    uint16_t sport, dport;
    uint32_t win;
    uint8_t why = 0;
    int players = 0, maxp = 0;
    okw_sock *L = NULL;
    okw_tconn *c = NULL;
    if (bl < 9 || !(h->flags & 1)) return;
    memcpy(&sport, b, 2);
    memcpy(&dport, b + 2, 2);
    memcpy(&win, b + 4, 4);
    if (tconn_find(from, h->conn, 0)) return;   /* (the lane doesn't repeat; a restarted peer picks a new id) */
    if (ctx->is_host && host_has_pw && !is_authed(from)) why = 3;
    else {
        for (int i = 0; i < OKW_MAX_SOCKS && !L; i++)
            if (socks[i].used && socks[i].kind == K_TCP && socks[i].tstate == T_LISTEN && socks[i].port == dport) L = &socks[i];
        if (!L) {   /* nothing on that port: the one port the game listens on, if there is just one (an original joiner,
                     * always 1030, reaches a modern host on 9803) */
            okw_sock *one = NULL;
            int n = 0;
            for (int i = 0; i < OKW_MAX_SOCKS; i++)
                if (socks[i].used && socks[i].kind == K_TCP && socks[i].tstate == T_LISTEN && socks[i].explicit_port) { one = &socks[i]; n++; }
            if (n == 1) {
                L = one;
                okp_log(1, "wsock: TCP connection from %016llx to port %u: nothing listens there - taken by port %u, the one the game listens on",
                        (unsigned long long)from, dport, one->port);
            }
        }
        if (!L) why = 1;
        else if (ctx->is_host && L->explicit_port) {   /* the room's own listener: the game's player limit */
            int type = -1;
            players = 1 + tcp_room_players();
            maxp = room_max_players();
            if (game->max_players) game->max_players(&type);
            okp_log(2, "wsock: TCP connection from %016llx: %d of %d players in the room (game type %d)%s",
                    (unsigned long long)from, players, maxp, type, peer_in_room(from) ? " - this player is one of them" : "");
            if (players >= maxp && !peer_in_room(from)) why = 4;
        }
        if (!why && L->nbacklog >= L->backlog_max) why = 2;
    }
    if (!why && !(c = tconn_new())) why = 2;
    if (why) {
        if (why == 4)
            okp_log(1, "wsock: TCP connection from %016llx to port %u refused (the game is full: %d of %d players)",
                    (unsigned long long)from, dport, players, maxp);
        else
            okp_log(1, "wsock: TCP connection from %016llx to port %u refused (%s)", (unsigned long long)from, dport,
                    why == 1 ? "nothing listens there" : why == 2 ? "backlog full" : "not let in: no password yet");
        tframe(from, h->conn, 0, TF_REFUSE, 0, &why, 1);
        return;
    }
    c->state = T_ESTAB;
    c->peer = from;
    c->id = h->conn;
    c->initiator = 0;
    c->lport = L->port;
    c->rport = sport;
    c->rip = standin_of(from);
    c->snd_wnd = c->snd_limit = win;
    tconn_hold(c);
    {
        okp_peer_info_t pi;
        if (okp_peer_info(ctx, from, &pi)) { c->base_giveup = pi.r_giveup; c->base_restarts = pi.r_restarts; c->info_init = 1; }
    }
    L->backlog[L->nbacklog++] = c;
    if (ctx->is_host) build_room_record();   /* (players in the room record) */
    uint8_t a[6];
    memcpy(a, &c->lport, 2);
    memcpy(a + 2, &tcp_window, 4);
    tframe(from, c->id, 0, TF_ACCEPT, 0, a, 6);
    okp_log(2, "wsock: TCP connection %08x from %016llx port %u to our port %u (waiting for accept, %d in backlog)",
            (unsigned)c->id, (unsigned long long)from, sport, dport, L->nbacklog);
    wake();
}

static void tcp_rx(uint64_t from, const uint8_t *d, int len)   /* okw_lock held, ctx is the live one */
{
    okw_thdr h;
    if (len < (int)sizeof(h)) return;
    memcpy(&h, d, sizeof(h));
    if (h.ver != 1 || h.type < TF_SYN || h.type > TF_RST) return;
    const uint8_t *b = d + sizeof(h);
    int bl = len - (int)sizeof(h);
    if (h.type != TF_DATA) tr("tcp rx %s conn %08x off %u", tf_name[h.type], (unsigned)h.conn, (unsigned)h.off);
    if (h.type == TF_SYN) { tcp_syn(from, &h, b, bl); return; }
    okw_tconn *c = tconn_find(from, h.conn, !(h.flags & 1));
    if (!c) {   /* a connection that is gone here (closed, or never known) */
        if (h.type == TF_DATA || h.type == TF_ACCEPT) {
            uint8_t r = 2;
            tframe(from, h.conn, !(h.flags & 1), TF_RST, 0, &r, 1);
        }
        return;
    }
    switch (h.type) {
    case TF_ACCEPT:
        if (c->state == T_SYN_SENT && bl >= 6) {
            memcpy(&c->snd_wnd, b + 2, 4);
            c->snd_limit = c->snd_wnd;
            c->state = T_ESTAB;
            c->t_state = GetTickCount();
            if (full_told == from) full_told = 0;   /* (full again later: told again) */
            okp_log(2, "wsock: TCP connection %08x to %016llx port %u is up", (unsigned)c->id, (unsigned long long)from, c->rport);
            wake();
        }
        break;
    case TF_REFUSE:
        if (c->state == T_SYN_SENT) {
            c->state = T_FAILED;
            c->err = c->so_error = WSAECONNREFUSED;
            c->rst_rx = 1;
            tconn_unhold(c);
            okp_log(1, "wsock: TCP connect to %016llx port %u refused (reason %d)", (unsigned long long)from, c->rport, bl ? b[0] : 0);
            if (bl && b[0] == 4) {   /* the game is full: the game sees "connection refused" as for every reason */
                int cur, mx;
                if (room_counts(from, &cur, &mx))
                    okp_log(1, "wsock: the host says its game is full (%d of %d)", cur, mx);
                else
                    okp_log(1, "wsock: the host says its game is full");
                if (full_told != from) {   /* once per host: the game's own connect loop is refused again every second */
                    full_told = from;
                    okp_log(1, "wsock: notice to the player: That game is full.");
                    if (!(okbox_test_answers() && getenv("OKP_TEST_JOIN_PASSWORD"))) {   /* (shown by ui_show_pending) */
                        InterlockedExchange(&ui_full, 1);
                        InterlockedExchange(&ui_pending, 1);
                    }
                }
            }
            wake();
        }
        break;
    case TF_DATA:
        if (c->state != T_ESTAB || c->err) break;
        if (h.off != c->rx_next || c->fin_rx || c->count + (uint32_t)bl > c->cap) {   /* a hole or overrun: never hand the game a corrupt stream */
            okp_log(0, "wsock: TCP connection %08x: stream broken (data at %u, expected %u, %u buffered) - reset",
                    (unsigned)c->id, (unsigned)h.off, (unsigned)c->rx_next, (unsigned)c->count);
            tconn_abort(c, 3, WSAECONNRESET);
            break;
        }
        c->rx_next += bl;
        tr("tcp rx DATA conn %08x off %u len %d type %d", (unsigned)c->id, (unsigned)h.off, bl, bl >= 3 ? b[2] : -1);
        {
            DWORD t = GetTickCount();
            if (c->st_lastrx && t - c->st_lastrx > c->st_maxgap) c->st_maxgap = t - c->st_lastrx;
            c->st_lastrx = t;
            c->st_rxf++;
            c->st_rxb += bl;
        }
        if (c->rd_shut) { tconn_abort(c, 1, WSAECONNABORTED); break; }   /* as Windows: data after shutdown(SD_RECEIVE) resets */
        ring_put(c, b, (uint32_t)bl);
        c->rx_gen++;
        last_race_tick = GetTickCount();   /* (notices wait while a game is going) */
        wake();
        break;
    case TF_WND:
        if ((int32_t)(h.off + c->snd_wnd - c->snd_limit) > 0) { c->snd_limit = h.off + c->snd_wnd; wake(); }
        break;
    case TF_FIN:
        if (h.off != c->rx_next) { tconn_abort(c, 3, WSAECONNRESET); break; }
        c->fin_rx = 1;
        c->rx_gen++;
        wake();
        break;
    case TF_RST:
        c->rst_rx = 1;
        if (c->state == T_SYN_SENT) { c->state = T_FAILED; c->err = c->so_error = WSAECONNREFUSED; }
        else if (!c->err) c->err = WSAECONNRESET;
        tconn_unhold(c);
        c->rx_gen++;
        okp_log(2, "wsock: TCP connection %08x reset by %016llx (reason %d)", (unsigned)c->id, (unsigned long long)from, bl ? b[0] : 0);
        wake();
        break;
    }
    room_record_follow();   /* (a FIN, a reset: a player may have left the room) */
}

static void send_syn(okw_tconn *c);
static void host_room(void);
static void tcp_conn_gone(uint64_t peer);
static int room_live(int i);

/* okplay's lane to a player broke (messages given up, or the player started over), or nothing at all came from it
 * for drop_after: its connections get a reset instead of waiting forever (B.10). Every 250 ms. */
static void tcp_liveness(DWORD now)   /* okw_lock held */
{
    static DWORD last;
    if (!ctx || now - last < 250) return;
    last = now;
    for (int i = 0; i < OKW_MAX_CONNS; i++) {
        okw_tconn *c = &conns[i];
        okp_peer_info_t pi;
        if (!c->used || c->err || c->orphan || (c->state != T_ESTAB && c->state != T_SYN_SENT) || c->syn_held) continue;
        int f = okp_peer_info(ctx, c->peer, &pi);
        if (f && !c->info_init) { c->base_giveup = pi.r_giveup; c->base_restarts = pi.r_restarts; c->info_init = 1; continue; }
        if (f && (pi.r_giveup != c->base_giveup || pi.r_restarts != c->base_restarts)) {
            okp_log(1, "wsock: TCP connection %08x: okplay's lane to %016llx broke (%s) - reset", (unsigned)c->id,
                    (unsigned long long)c->peer, pi.r_giveup != c->base_giveup ? "messages given up" : "the player started over");
            tconn_abort(c, 3, WSAECONNRESET);
        } else if (c->state == T_ESTAB && (!f || pi.rx_age_ms >= (DWORD)okp_drop_after_ms())) {
            okp_log(1, "wsock: TCP connection %08x: nothing from %016llx for %d s - the connection is dropped", (unsigned)c->id,
                    (unsigned long long)c->peer, okp_drop_after_ms() / 1000);
            tconn_abort(c, 4, WSAECONNABORTED);
        }
    }
}

static void tcp_stats(DWORD now)   /* okw_lock held: per connection, every 10 s when something moved */
{
    for (int i = 0; i < OKW_MAX_CONNS; i++) {
        okw_tconn *c = &conns[i];
        if (!c->used || now - c->st_tick < 10000) continue;
        if (c->st_txf || c->st_rxf || c->st_wb)
            okp_log(2, "wsock: TCP %08x with %016llx, last 10 s: sent %u frames %u B, got %u frames %u B, longest gap %lu ms, "
                    "would-block %u, unread %u B, room to send %u B", (unsigned)c->id, (unsigned long long)c->peer, c->st_txf, c->st_txb,
                    c->st_rxf, c->st_rxb, (unsigned long)c->st_maxgap, c->st_wb, (unsigned)c->count, (unsigned)(c->snd_limit - c->snd_next));
        c->st_tick = now;
        c->st_txf = c->st_txb = c->st_rxf = c->st_rxb = c->st_wb = 0;
        c->st_maxgap = 0;
    }
}

static void tcp_tick(void)   /* okw_lock held */
{
    DWORD now = GetTickCount();
    for (int i = 0; i < OKW_MAX_CONNS; i++) {
        okw_tconn *c = &conns[i];
        if (!c->used) continue;
        if (c->syn_held && (int32_t)(now - c->syn_due) >= 0 && !c->orphan) send_syn(c);
        if (c->orphan && now - c->orphan_tick >= 3000) {   /* nobody took it over */
            if (c->syn_held) ;   /* never sent: nothing to tell */
            else if (c->state == T_SYN_SENT) tconn_abort(c, 5, WSAECONNABORTED);
            else if (c->state == T_ESTAB && !c->err) TFRAME(c, TF_FIN, c->snd_next, NULL, 0);
            okp_log(2, "wsock: TCP connection %08x: the game's abandoned connect was not taken over - closed", (unsigned)c->id);
            uint64_t peer = c->peer;
            tconn_free(c);
            tcp_conn_gone(peer);
            continue;
        }
        if (c->state == T_SYN_SENT && !c->syn_held && now - c->t_state >= 20000) {
            tconn_abort(c, 5, WSAETIMEDOUT);
            c->state = T_FAILED;
            c->so_error = WSAETIMEDOUT;
            okp_log(1, "wsock: TCP connect to %016llx port %u: no answer in 20 s", (unsigned long long)c->peer, c->rport);
        }
    }
    tcp_liveness(now);
    tcp_stats(now);
}

/* The process is ending with connections still open: Windows would reset them at once. Here a best-effort reset goes
 * to each player, twice, as plain datagrams, so the others don't wait drop_after to notice. Called from the game's
 * ExitProcess (hooked) and the exit watch - not from DLL detach: Winsock is already shut down in the process by then
 * (seen: WSANOTINITIALISED). Takes no lock of okplay's core. */
static void tcp_bye(void)
{
    okp_ctx *c = ctx;
    if (!c || !(game->kinds & OKW_TCP)) return;
    for (int r = 0; r < 2; r++)
        for (int i = 0; i < OKW_MAX_CONNS; i++) {
            okw_tconn *t = &conns[i];
            if (!t->used || t->rst_rx || t->rst_sent || t->syn_held || (t->state != T_ESTAB && t->state != T_SYN_SENT)) continue;
            uint8_t f[sizeof(okw_thdr) + 1];
            okw_thdr *h = (okw_thdr *)f;
            h->m0 = 'W'; h->m1 = 'T'; h->ver = 1; h->type = TF_RST;
            h->conn = t->id; h->flags = t->initiator ? 1 : 0; h->rsv = 0; h->off = t->snd_next;
            f[sizeof(okw_thdr)] = 4;
            okp_send_unlocked(c, t->peer, f, (int)sizeof(f));
        }
}

static void okw_rx(void *c, uint64_t from, const void *data, int len)   /* okplay's net thread */
{
    (void)c;
    if (len >= 4 && ((const uint8_t *)data)[0] == 'W' && ((const uint8_t *)data)[1] == 'T') {   /* a stream frame */
        EnterCriticalSection(&okw_lock);
        if (ctx && c == ctx) tcp_rx(from, (const uint8_t *)data, len);
        LeaveCriticalSection(&okw_lock);
        return;
    }
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
    uint32_t sdsize = 80, maxp = (uint32_t)room_max_players(), curp = 1;
    memcpy(sd, &sdsize, 4);
    memcpy(sd + 8, &ctx->room, 4);                 /* guidInstance (enough to tell rooms apart) */
    memcpy(sd + 24, &OKW_APPGUID, 16);             /* guidApplication */
    /* Road Rash's broadcast: byte 17 = free slots, bytes 37.. = host name */
    char name[64];
    lstrcpynA(name, game->name, sizeof(name));
    if (advert_len >= 57 && advert_port == 6995) {
        curp = 8 - (advert[17] < 8 ? advert[17] : 7);
        memcpy(name, advert + 37, 20);
        name[20] = 0;
        if (!name[0]) lstrcpynA(name, game->name, sizeof(name));
    }
    if (game->kinds & OKW_TCP) {
        room_who(rec_who, sizeof(rec_who));
        if (rec_who[0]) snprintf(name, sizeof(name), "%s - %s's game", game->name, rec_who);
        curp += (uint32_t)tcp_room_players();
        rec_curp = (int)curp;
        rec_maxp = (int)maxp;
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
    {   /* TCP games: 'OKWT', version 1, count, the ports the host listens on */
        uint16_t ports[8];
        int np = 0;
        for (int i = 0; i < OKW_MAX_SOCKS && np < 8; i++)
            if (socks[i].used && socks[i].kind == K_TCP && socks[i].tstate == T_LISTEN && socks[i].explicit_port) ports[np++] = socks[i].port;
        int idl = (int)strlen(game->id);
        if (np && n + 7 + 2 * np + idl <= (int)sizeof(b)) {   /* then the profile id: joiners list their own game's rooms */
            memcpy(b + n, "OKWT", 4);
            b[n + 4] = 1;
            b[n + 5] = (uint8_t)np;
            memcpy(b + n + 6, ports, 2 * np);
            n += 6 + 2 * np;
            b[n++] = (uint8_t)idl;
            memcpy(b + n, game->id, idl);
            n += idl;
        }
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

/* okplay that did not start (no network, no thread): kept here and stopped later with no lock held (its network
 * thread may be waiting for okw_lock); the next time the game opens its network menu okplay tries again */
static okp_ctx *failed_ctx;
static int start_failed;
static DWORD start_failed_tick;

static void start_okplay(void)   /* okw_lock held; the first time the game uses UDP */
{
    if (started) return;
    started = 1;
    okp_config_load();
    want_lan = mode_choice == 3;
    okp_ctx *c = (okp_ctx *)calloc(1, sizeof(okp_ctx));
    if (!c) { start_failed = 1; start_failed_tick = GetTickCount(); return; }
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
        /* (no network: okp_net_start told the player, once per game run) */
        okp_log(0, "wsock: okplay could not start - it tries again when the game opens its network menu again");
        failed_ctx = c;
        start_failed = 1;
        start_failed_tick = GetTickCount();
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
    okp_ctx *c = ctx, *fc = failed_ctx;
    int withdraw = c && c->room && (c->is_host || c->closing) && c->mode == OKP_MODE_ONLINE;
    for (int i = 0; i < OKW_MAX_CONNS; i++) tconn_unhold(&conns[i]);   /* (on the okplay that stops: once each) */
    ctx = NULL;
    failed_ctx = NULL;
    start_failed = 0;
    started = 0;   /* (okw_self stays: a game may keep the own address it looked up once) */
    mode_provisional = 0;
    InterlockedExchange(&mode_choice, 0);
    host_pw_asked = host_has_pw = 0;
    SecureZeroMemory(host_hash, sizeof(host_hash));
    pending_room = 0;
    close_owed = 0;
    nauthed = 0;
    auth_host = 0;
    auth_state = 0;
    auth_room = 0;
    SecureZeroMemory(join_hash, sizeof(join_hash));
    nheld = 0;
    advert_len = 0;
    advert_port = 0;
    browse_tick = inject_tick = 0;
    picked_host = 0;
    relisten_port = 0;
    nclosed_peers = 0;
    rec_curp = rec_maxp = -1;
    full_told = 0;
    for (int i = 0; i < OKW_MAX_CONNS; i++) if (conns[i].used && !conns[i].err) { conns[i].err = WSAECONNABORTED; conns[i].rx_gen++; }
    wake();
    socks_gone = 0;
    room_closed = 0;
    memset(links, 0, sizeof(links));
    memset(races, 0, sizeof(races));
    npend = 0;
    t_start_ms = 0;
    LeaveCriticalSection(&okw_lock);
    if (fc) stop_ctx(fc, 0);
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

/* the game opens its network menu (a listen) after okplay failed to start: one more try. No lock held */
static void retry_start(void)
{
    if (!start_failed) return;
    EnterCriticalSection(&okw_lock);
    int again = start_failed && !ctx && GetTickCount() - start_failed_tick >= 2000;
    okp_ctx *fc = again ? failed_ctx : NULL;
    if (again) { failed_ctx = NULL; start_failed = 0; started = 0; }
    LeaveCriticalSection(&okw_lock);
    if (fc) stop_ctx(fc, 0);
    if (!again) return;
    okp_log(1, "wsock: trying to start okplay again");
    EnterCriticalSection(&okw_lock);
    if (!started && mode_choice >= 2) start_okplay();
    LeaveCriticalSection(&okw_lock);
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
        for (int i = 0; i < OKW_MAX_SOCKS; i++) if (socks[i].used && socks[i].kind == K_TCP) live = 1;   /* (a race goes on) */
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

/* become the host of a room (the game broadcast, or a TCP game's host rule).
 * The room id: pending_room is chosen with the host's password box (which salts the hash with it). end_session
 * clears it, and so does host_game_over on a build whose game state is known (a new Host Game: new box, new id;
 * send_owed_close has taken the old room off the list first). Otherwise every room this PC hosts in one okplay
 * session has the same id. That is why
 * a close followed by a host within the room-server thread's 100 ms (tcp_close, then the game listens again) never
 * left the old id listed: there is no old id. But a fresh token for the same id was refused by the room server
 * ("owned by another host") until the old entry expired, so a TCP game keeps the token while the id stays. */
static void host_room(void)   /* okw_lock held */
{
    if (!ctx || ctx->is_host) return;
    uint32_t room = pending_room ? pending_room : ((uint32_t)okp_rand64() | 1);
    if (!(game->kinds & OKW_TCP) || ctx->room != room || !ctx->tok[0])
        snprintf(ctx->tok, sizeof(ctx->tok), "%016llx", (unsigned long long)okp_rand64());
    ctx->room = room;
    ctx->is_host = 1;
    ctx->closing = 0;
    room_closed = 0;
    close_owed = 0;
    build_room_record();
    ctx->session_open = 1;
    if (ctx->mode == OKP_MODE_LAN && !okp_lan_open_listen(ctx))
        okp_log(0, "wsock: LAN discovery port %u unavailable - joiners will not see this game", OKP_LAN_PORT);
    okp_log(1, "wsock: hosting room %08x", ctx->room);
    okp_report_session_begin(ctx, 1);
    okp_rdv_wake(ctx);
}

/* join the room whose host is `host` (the game sent to that host's stand-in address). 1 = in that room (now or
 * already), 0 = not: no okplay, this PC hosts, or the room is not listed. Quiet: a UDP host calls it with every
 * packet it sends to a player; the TCP paths (tcp_join) say why. */
static int join_room_in(uint64_t host, uint32_t only)   /* okw_lock held; only: that room of the host (0 = any) */
{
    if (!ctx || ctx->is_host) return 0;
    if (ctx->host_peer == host) return 1;
    EnterCriticalSection(&ctx->lock);
    for (int i = 0; i < ctx->nrooms; i++) {
        okp_room *rm = &ctx->rooms[i];
        if (rm->host != host || (only && rm->room != only)) continue;
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
        return 1;
    }
    LeaveCriticalSection(&ctx->lock);
    return 0;
}

static int join_room(uint64_t host) { return join_room_in(host, 0); }   /* okw_lock held */

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

/* ---------------------------------------------------------------- TCP: a join and the host role, taken back
 *
 * What a TCP join sets - the room (ctx->host_peer, ctx->room), the host the game's connects go to (picked_host), the
 * password state (auth_*) - is set on the way in and taken back on every way out by tcp_join_undo: the join was
 * refused, cancelled or not answered, or the last stream connection to that host closed. Otherwise a later connect
 * goes to the old host without asking, and Host Game never hosts. */

static int tcp_conns_to(uint64_t peer)   /* okw_lock held: our stream connections with that player (any state) */
{
    int n = 0;
    for (int i = 0; i < OKW_MAX_CONNS; i++) if (conns[i].used && conns[i].peer == peer) n++;
    return n;
}

static int tcp_outbound(void)   /* okw_lock held: a connect of ours that is going on, waits to be taken over, or is up */
{
    for (int i = 0; i < OKW_MAX_CONNS; i++)
        if (conns[i].used && conns[i].initiator && !conns[i].err && (conns[i].state == T_SYN_SENT || conns[i].state == T_ESTAB))
            return 1;
    return 0;
}

/* drop: also forget the host as a peer, with what waits in its queue (a password): only for a join that never got
 * a connection - a connection just closed still has its last frames (the FIN) to deliver */
static void tcp_join_undo(uint64_t host, const char *why, int drop)   /* okw_lock held */
{
    int was_in = ctx && host && ctx->host_peer == host;
    if (host && picked_host == host) picked_host = 0;
    if (host && auth_host == host) {   /* (a late answer from that host is then ignored: it is not auth_host any more) */
        auth_host = 0;
        auth_room = 0;
        auth_state = 0;
        SecureZeroMemory(join_hash, sizeof(join_hash));
        nheld = 0;
    }
    if (!was_in) return;
    okp_report_session_end(ctx);   /* (the join's connection line) */
    EnterCriticalSection(&ctx->lock);
    okp_peer *p = okp_peer_get(ctx, host, 0);
    if (drop && p && !p->hold && !tcp_conns_to(host)) {
        okp_peer_free_bufs(p);
        p->qn = 0;
        p->used = 0;
    }
    ctx->host_peer = 0;
    ctx->room = 0;
    LeaveCriticalSection(&ctx->lock);
    ctx->session_open = 0;
    browse_tick = 0;
    okp_log(1, "wsock: not in the room of host %016llx any more (%s)", (unsigned long long)host, why);
}

/* join `host`'s room for a stream connect: 1 = in it. Never quiet: each refusal says why. okw_lock held, ctx set */
static int tcp_join(uint64_t host)
{
    if (ctx->is_host) {
        okp_log(1, "wsock: not joining host %016llx - this PC hosts room %08x", (unsigned long long)host, ctx->room);
        return 0;
    }
    if (ctx->host_peer && ctx->host_peer != host && !tcp_conns_to(ctx->host_peer))
        tcp_join_undo(ctx->host_peer, "the game joins another room", 0);
    if (ctx->host_peer && ctx->host_peer != host) {
        okp_log(1, "wsock: not joining host %016llx - still connected to host %016llx", (unsigned long long)host,
                (unsigned long long)ctx->host_peer);
        return 0;
    }
    uint32_t live = 0;
    EnterCriticalSection(&ctx->lock);
    for (int i = 0; i < ctx->nrooms; i++) if (ctx->rooms[i].host == host && room_live(i)) live = ctx->rooms[i].room;
    /* a peer left from an earlier join, with nothing of ours on it: forgotten, so this join starts its lane afresh (the
     * host may have started okplay again since - a new session with the same id - and the old lane answers nothing) */
    okp_peer *p = ctx->host_peer != host ? okp_peer_get(ctx, host, 0) : NULL;
    if (p && !p->hold && !tcp_conns_to(host)) {
        okp_peer_free_bufs(p);
        p->qn = 0;
        p->used = 0;
    }
    LeaveCriticalSection(&ctx->lock);
    if (join_room_in(host, live)) return 1;   /* (its newest room: an older one of the same host may still be listed) */
    okp_log(1, "wsock: not joining host %016llx - its room is not listed (any more)", (unsigned long long)host);
    return 0;
}

/* a stream connection was freed: the last one to the room's host ends the join */
static void tcp_conn_gone(uint64_t peer)   /* okw_lock held */
{
    if (ctx && peer && ctx->host_peer == peer && !ctx->is_host && !tcp_conns_to(peer))
        tcp_join_undo(peer, "the game closed its last connection to that host", 0);
}

/* The game closed the room this PC hosted (tcp_close: it stopped listening; tcp_unhost_for_connect) and nobody is
 * connected to it: that game is over. On a build whose game state is known (game->says_hosts), the next Host Game
 * is a new game, so its password is asked again and its room gets a new id (as the drop-in asks at every new game):
 * host_pw_asked, host_has_pw, host_hash, pending_room and the players let in (authed) are forgotten here.
 * On other builds a menu re-entry looks like Host Game (x_listen's listen-again rule), so the answer is kept for the
 * okplay session there (only end_session forgets it) - else the box would come at every re-entry.
 * Players still connected: the same game goes on (the game might close its listener at a race start - not seen
 * yet), nothing is forgotten. UDP games (Road Rash) close their room at every race start with the players still
 * there; their new game is found by check_new_game, and end_session forgets it all. okw_lock held. */
static void host_game_over(int owe_close)
{
    if (!game->says_hosts || game->says_hosts(NULL, NULL) < 0 || tcp_room_players()) return;
    if (owe_close && ctx && ctx->room && ctx->mode == OKP_MODE_ONLINE) close_owed = ctx->room;
    if (!host_pw_asked && !pending_room) return;
    host_pw_asked = host_has_pw = 0;
    SecureZeroMemory(host_hash, sizeof(host_hash));
    pending_room = 0;
    nauthed = 0;
    okp_log(2, "wsock: the hosted game is over - the next Host Game asks for a password again and lists a new room");
}

/* The room the game closed (close_owed) comes off the room server's list now, with its own id and token, before
 * ctx->room changes (a new room id, or a join): the room server's thread reads ctx->room when it gets to it, and
 * a new id would leave the old room listed. If that thread has sent the close already, this one is a repeat the
 * room server answers harmlessly. No lock held (plain HTTP takes over a second on Windows 9x). */
static void send_owed_close(void)
{
    EnterCriticalSection(&okw_lock);
    okp_ctx *c = ctx;
    int owed = c && close_owed && c->room == close_owed && !c->is_host && c->mode == OKP_MODE_ONLINE;
    close_owed = 0;
    LeaveCriticalSection(&okw_lock);
    if (owed) okp_rdv_close(c);
}

/* The game connects out while this PC counts as a host: its last listen was not Host Game but the player entering
 * the TCP menu again (the game makes the same calls). With nobody in the room the host role goes and the room comes
 * off the list now - sent from here with this room's id and token, before a join changes ctx->room (the room
 * server's thread reads it later and would close the wrong room). With players in the room the connect is refused:
 * a host does not walk out on its players. No lock held: 0 = go on as a joiner, -1 = refuse. */
static int tcp_unhost_for_connect(void)
{
    EnterCriticalSection(&okw_lock);
    okp_ctx *c = ctx;
    if (!c || !c->is_host || game->host_rule != HOST_LISTEN) {
        LeaveCriticalSection(&okw_lock);
        send_owed_close();   /* (a room the game closed a moment ago: off the list before the join changes ctx->room) */
        return 0;
    }
    int n = tcp_room_players();
    if (n) {
        okp_log(1, "wsock: the game connects out while %d player(s) are in its room %08x - the connect is refused", n, c->room);
        LeaveCriticalSection(&okw_lock);
        return -1;
    }
    okp_log(1, "wsock: the game connects out - this PC joins; its room %08x comes off the list", c->room);
    int withdraw = c->room && c->mode == OKP_MODE_ONLINE;
    c->is_host = 0;
    c->session_open = 0;
    c->closing = 1;          /* (nothing announces it meanwhile) */
    room_closed = 0;         /* (not "the game left its room": no new session comes of this) */
    relisten_port = 0;
    close_owed = 0;          /* (this room's close goes from here) */
    host_game_over(0);       /* (known builds: the next Host Game asks again; the fallback rule's ghost rooms keep it) */
    LeaveCriticalSection(&okw_lock);
    DWORD t0 = GetTickCount();
    if (withdraw) okp_rdv_close(c);   /* (no lock held: plain HTTP takes over a second on Windows 9x) */
    EnterCriticalSection(&okw_lock);
    if (ctx == c) {
        /* a close the room server's thread may still send goes out with a token that owns no room */
        snprintf(c->tok, sizeof(c->tok), "%016llx", (unsigned long long)okp_rand64());
        c->room = 0;
        c->closing = 0;
        c->sess_role = 0;    /* (a room nobody was in: no connection line for it) */
        if (withdraw) okp_log(2, "wsock: room withdrawn in %lu ms", (unsigned long)(GetTickCount() - t0));
    }
    LeaveCriticalSection(&okw_lock);
    return 0;
}

/* every call from the game: while it isn't hosting or in a room and has a socket waiting for broadcasts
 * (a server browser), list the rooms and feed their broadcasts to that socket */
static void okw_tick(void)   /* okw_lock held */
{
    if (!started && mode_choice >= 2) start_okplay();   /* (re)started in the mode the player picked */
    tcp_tick();
    room_record_follow();
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
        if (t - knob_ms >= 2000 && (game->kinds & OKW_UDP)) {   /* (never file I/O from a TCP game's comms thread) */
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
 * drop-in's shim/win9x.c does, every CreateThread in this DLL comes here and gets somewhere to write.
 * A thread made with stack size 0 gets the exe's stack reserve, and a game exe may reserve tiny stacks (the 1998
 * NFS3 exes: 32,000 bytes, too small for okplay's threads), so ours ask for 256 KB. */
static HANDLE WINAPI okw_CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE start, LPVOID param,
                                      DWORD flags, LPDWORD id)
{
    static HANDLE (WINAPI *fn)(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
    DWORD dummy;
    if (!fn) fn = (HANDLE (WINAPI *)(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD))
                  GetProcAddress(GetModuleHandleA("kernel32.dll"), "CreateThread");
    if (!stack) stack = 256 * 1024;   /* a game exe may reserve tiny stacks (NFS3 1998: 32,000 bytes); ours need room */
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
typedef struct { int kind; GUID g; WCHAR w[64]; INT_PTR r; void *p; } okw_box;
struct okw_listbox_s;
static INT_PTR lb_run_any(void *L);
static const WCHAR no_answer_text[] = L"No answer from the host. The game may have started or closed.";   /* (notice_no_answer) */
static const WCHAR full_text[] = L"That game is full.";   /* (ui_full: the host refused with reason 4) */

/* Notices from okplay's core - a newer version, the room server out of reach, TCP/IP not working, a crash
 * report or connection reports waiting (the DirectPlay drop-in has the same boxes and shows them at once on
 * their own thread). Over a full-screen game a window of ours at a random moment knocks the game to the
 * desktop, so here they wait for a safe moment: a game network call, no box open, no race packet for 5 s;
 * then run_box shows them on the game's own thread with its messages held, like the other boxes. The two
 * questions come from report.c's own thread, which waits for the answer. */
static CRITICAL_SECTION ui_lock;
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
    /* a full-screen DirectDraw game: Windows' own screen page in front first, or the box is never seen (ddshow.c) */
    if (game->kinds & OKW_TCP) dds_show_gdi();
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
    else if (b->kind == 10) b->r = lb_run_any(b->p);   /* a TCP game's room list / "Connecting..." */
    else if (b->kind == 11 || b->kind == 12) {   /* "No answer from the host" (notice_no_answer), "That game is full." */
        struct pw_dialog d;
        WCHAR title[96];
        memset(&d, 0, sizeof(d));
        okbox_brand(title, 96);
        d.title = title;
        d.text = b->kind == 11 ? no_answer_text : full_text;
        d.ok = L"OK";
        b->r = (okbox_show(&d), 1);
    }
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
    dds_guard_end();
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
        b.kind = ui_update.on ? 4 : ui_unreach.on ? 5 : ui_nonet.on ? 6 : ui_crash.on ? 7 : ui_conn.on ? 8 : ui_wrongpw ? 3 : ui_full ? 12 : ui_noanswer ? 11 : 0;
        if (!b.kind) InterlockedExchange(&ui_pending, 0);
        LeaveCriticalSection(&ui_lock);
        if (!b.kind) break;
        INT_PTR r = run_box(&b);
        EnterCriticalSection(&ui_lock);
        if (b.kind == 4) ui_update.on = 0;
        else if (b.kind == 5) ui_unreach.on = 0;
        else if (b.kind == 6) ui_nonet.on = 0;
        else if (b.kind == 3) InterlockedExchange(&ui_wrongpw, 0);
        else if (b.kind == 12) InterlockedExchange(&ui_full, 0);
        else if (b.kind == 11) InterlockedExchange(&ui_noanswer, 0);
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
            if (c) {
                for (int i = 0; i < OKW_MAX_CONNS; i++) tconn_unhold(&conns[i]);
                ctx = NULL;
                started = 0;
            }
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
static const struct { const char *game, *build, *what; char group; } known_builds[] = {
    { "roadrash", "73d7f9744b5b1a94", "Road Rash CD (1996), ROADRASH.EXE", 'R' },
    { "roadrash", "9e40e25737149ab0", "Road Rash (Steam or GOG - the same exe), RoadRash95.exe", 'R' },
    { "roadrash", "9c32eadc287cc8eb", "Road Rash CD with okplay's lab no-CD patch (testing)", 'R' },
    /* Need for Speed III: the 1.6.x builds race each other; the 1998 CD exe may compute the race differently */
    { "nfs3", "240a083a78ac3b83", "Need for Speed III Modern Patch 1.6.1, nfs3.exe", 'A' },
    { "nfs3", "397dee58eb88822c", "Need for Speed III portable 1.6.2 Beta, nfs3.exe", 'A' },
    { "nfs3", "53aa0dfcf438788d", "Need for Speed III CD (1998), NFS3.EXE", 'B' },
    { "nfs3", "b390be67925b2ad0", "Need for Speed III (1998, OEM release), NFS3.EXE", 'B' },
};

static int known_build(const char *b)   /* index in known_builds for this game, -1 = not known */
{
    for (unsigned i = 0; i < sizeof(known_builds) / sizeof(known_builds[0]); i++)
        if (!strcmp(game->id, known_builds[i].game) && !strcmp(b, known_builds[i].build)) return (int)i;
    return -1;
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
    int ia = known_build(theirs), ib = known_build(mine);
    const char *a = ia >= 0 ? known_builds[ia].what : NULL, *b = ib >= 0 ? known_builds[ib].what : NULL;
    if (a && b && known_builds[ia].group == known_builds[ib].group) {
        okp_log(1, "build: the host runs %s (%s), we run %s (%s) - builds that race each other", a, theirs, b, mine);
        return 1;
    }
    if (a && b) okp_log(1, "build: the host runs %s (%s), we run %s (%s) - known builds, but they may not race each other", a, theirs, b, mine);
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
                   L"%ls", tw, theirs, mw, mine, (game->kinds & OKW_TCP) ?
                   L"Different versions may go out of sync during the race. Join anyway?" :
                   L"Joining will probably fail, or the game will go out of sync. Join anyway?");
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

static int name_in_list(const char *list, const char *base)
{
    char l[512];
    lstrcpynA(l, list, sizeof(l));
    for (char *t = strtok(l, ",; "); t; t = strtok(NULL, ",; ")) if (!_stricmp(base, t)) return 1;
    return 0;
}

/* ...and which profile (okw_profiles): by exe name, else okplay.ini wsock_game= / OKP_WSOCK_GAME (tests), else Road
 * Rash's (UDP) */
static int is_winsock_game(void)
{
    char path[MAX_PATH], *base, v[16] = "", gid[32] = "";
    char ini[MAX_PATH], list[512] = "", dir[MAX_PATH] = "";
    const okw_game *byid = NULL;
    if (GetModuleFileNameA(g_self, dir, sizeof(dir))) {
        char *slash = strrchr(dir, '\\');
        if (slash) slash[1] = 0;
    }
    snprintf(ini, sizeof(ini), "%sokplay.ini", dir);
    GetPrivateProfileStringA("okplay", "wsock_game", "", gid, sizeof(gid), ini);
    GetEnvironmentVariableA("OKP_WSOCK_GAME", gid, sizeof(gid));
    for (unsigned i = 0; i < sizeof(okw_profiles) / sizeof(okw_profiles[0]) && gid[0]; i++)
        if (!_stricmp(gid, okw_profiles[i].id)) byid = &okw_profiles[i];
    if (GetEnvironmentVariableA("OKP_WSOCK_MODE", v, sizeof(v))) { game = byid ? byid : &okw_profiles[0]; return 1; }
    if (!GetModuleFileNameA(NULL, path, sizeof(path))) return 0;
    base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    for (unsigned i = 0; i < sizeof(okw_profiles) / sizeof(okw_profiles[0]); i++)
        if (okw_profiles[i].exes && name_in_list(okw_profiles[i].exes, base)) { game = &okw_profiles[i]; return 1; }
    if (!dir[0]) return 0;
    GetPrivateProfileStringA("okplay", "wsock_games", "", list, sizeof(list), ini);
    if (name_in_list(list, base)) { game = byid ? byid : &okw_profiles[0]; return 1; }
    return 0;
}


static int set_err(int e) { WSASetLastError(e); return SOCKET_ERROR; }

/* ---------------------------------------------------------------- stream sockets: the game's calls
 *
 * A stream socket is entered in the table at socket() but stays a plain Windows socket until its first bind,
 * listen or connect: only then comes the Online / LAN / original TCP/IP box (games make probe sockets they only
 * create and close). Options and FIONBIO set before that go to Windows as well as being recorded, so with
 * "original" nothing needs replaying: the table entries are dropped and the call goes to Windows. With Online or
 * LAN Play every stream socket of the run is virtual: the real handle is only the game's SOCKET value. */

#define OKW_SIOCATMARK 0x40047307L

static int tcp_virtual(const okw_sock *k) { return k && k->kind == K_TCP && mode_choice >= 2; }

static int tcp_entry(SOCKET s, int virt_only)   /* no lock held */
{
    EnterCriticalSection(&okw_lock);
    okw_sock *k = tsock_of(s);
    int r = k && (!virt_only || tcp_virtual(k));
    LeaveCriticalSection(&okw_lock);
    return r;
}

/* 1 = the call goes to Windows (the player chose the original TCP/IP). No lock held. */
static int tcp_mode_pass(void)
{
    if (mode_choice >= 2) return 0;
    if (mode_choice == 1) return 1;
    int m = choose_mode();
    EnterCriticalSection(&okw_lock);
    if (m == 1) {
        for (int i = 0; i < OKW_MAX_SOCKS; i++) if (socks[i].used && socks[i].kind == K_TCP) socks[i].used = 0;
        socks_gone = 0;   /* (the original TCP/IP stays for this game run) */
    } else if (m >= 2) {
        start_okplay();
    }
    LeaveCriticalSection(&okw_lock);
    return m < 2;
}

static int tcp_port_used(uint16_t port)   /* okw_lock held */
{
    for (int i = 0; i < OKW_MAX_SOCKS; i++)
        if (socks[i].used && socks[i].kind == K_TCP && socks[i].port == port && !socks[i].c &&
            (socks[i].tstate == T_BOUND || socks[i].tstate == T_LISTEN)) return 1;
    return 0;
}

static uint16_t eph_port(void)   /* okw_lock held */
{
    uint16_t p;
    do { p = next_eph++; if (next_eph < 30000) next_eph = 30000; } while (tcp_port_used(p));
    return p;
}

static int tcp_state(const okw_sock *k) { return k->c ? k->c->state : k->tstate; }

/* wait for okw_ev (no lock held); with other threads waiting too, at most 10 ms (one shared event) */
static void ev_wait(DWORD ms)
{
    if (okw_waiters > 1 && ms > 10) ms = 10;
    WaitForSingleObject(okw_ev, ms);
}

static int tcp_bind(okw_sock *k, const struct sockaddr *a, int l)   /* okw_lock held */
{
    if (!a || l < (int)sizeof(struct sockaddr_in)) return set_err(WSAEFAULT);
    if (a->sa_family != AF_INET) return set_err(WSAEAFNOSUPPORT);
    if (k->tstate != T_NEW || k->c) return set_err(WSAEINVAL);
    const struct sockaddr_in *in = (const struct sockaddr_in *)a;
    uint16_t port = ntohs(in->sin_port);
    if (port && tcp_port_used(port)) return set_err(WSAEADDRINUSE);
    k->explicit_port = port != 0;
    k->port = port ? port : eph_port();
    k->bound_ip = in->sin_addr.s_addr;
    k->tstate = T_BOUND;
    okp_log(2, "wsock: TCP socket %u bound to port %u%s", (unsigned)k->s, k->port, port ? "" : " (any)");
    return 0;
}

/* ---------------------------------------------------------------- TCP rooms: what a joiner sees */

/* the room record's 'OKWT' block (a TCP game's room): 'OKWT', version 1, n, n x u16 ports, id length, profile id.
 * 1 = a room of this game; *ports / *np: the ports its host listens on */
static int room_tcp(const okp_room *rm, uint16_t *ports, int *np)
{
    for (int i = 92; i + 6 <= rm->enumlen; i++) {
        if (memcmp(rm->enumblob + i, "OKWT", 4) || rm->enumblob[i + 4] != 1) continue;
        int n = rm->enumblob[i + 5], at = i + 6 + 2 * n;
        if (n > 8 || at + 1 > rm->enumlen) return 0;
        int idl = rm->enumblob[at];
        if (at + 1 + idl > rm->enumlen || idl != (int)strlen(game->id) || _strnicmp((const char *)rm->enumblob + at + 1, game->id, idl)) return 0;
        if (ports) memcpy(ports, rm->enumblob + i + 6, 2 * n);
        if (np) *np = n;
        return 1;
    }
    return 0;
}

/* a room a joiner should see (ctx->lock held): on a LAN an offer that is still fresh, and per host only its newest
 * room - a host that hosted again in a new okplay session leaves its old room in the LAN list until that slot is
 * reused, so "the only room of this game" and the list saw two. (The room server's list is replaced whole.) */
static int room_live(int i)
{
    const okp_room *rm = &ctx->rooms[i];
    if (ctx->mode != OKP_MODE_LAN) return 1;
    if (okp_now() - rm->seen > OKP_LAN_ROOM_TTL) return 0;
    for (int j = 0; j < ctx->nrooms; j++)
        if (j != i && ctx->rooms[j].host == rm->host && (int)(ctx->rooms[j].seen - rm->seen) > 0) return 0;
    return 1;
}

typedef struct {
    uint64_t host;
    uint32_t room;
    int locked, curp, maxp;
    char name[64], build[17], exe[64];
} okw_roominfo;

static void room_read(const okp_room *rm, okw_roominfo *ri)
{
    uint32_t fl = 0, cur = 0, mx = 0;
    int k = 0;
    memset(ri, 0, sizeof(*ri));
    ri->host = rm->host;
    ri->room = rm->room;
    if (rm->enumlen >= 56) { memcpy(&fl, rm->enumblob + 12, 4); memcpy(&mx, rm->enumblob + 48, 4); memcpy(&cur, rm->enumblob + 52, 4); }
    ri->locked = (fl & 0x400) != 0;
    ri->curp = (int)cur;
    ri->maxp = (int)mx;
    for (int i = 92; i + 1 < rm->enumlen && k < (int)sizeof(ri->name) - 1; i += 2) {   /* the UTF-16 name */
        uint16_t ch = (uint16_t)(rm->enumblob[i] | rm->enumblob[i + 1] << 8);
        if (!ch) break;
        ri->name[k++] = ch < 128 ? (char)ch : '?';
    }
    ri->name[k] = 0;
    lstrcpynA(ri->build, rm->build, sizeof(ri->build));
    lstrcpynA(ri->exe, rm->exe, sizeof(ri->exe));
}

static int room_of(uint64_t host, okw_roominfo *ri);

static int room_counts(uint64_t host, int *curp, int *maxp)   /* okw_lock held: the players a listed room says it has */
{
    okw_roominfo ri;
    if (!room_of(host, &ri) || ri.maxp <= 0) return 0;
    *curp = ri.curp;
    *maxp = ri.maxp;
    return 1;
}

static int room_of(uint64_t host, okw_roominfo *ri)   /* okw_lock held */
{
    int found = 0;
    EnterCriticalSection(&ctx->lock);
    for (int i = 0; i < ctx->nrooms && !found; i++)
        if (ctx->rooms[i].host == host && room_live(i) && room_tcp(&ctx->rooms[i], NULL, NULL)) { room_read(&ctx->rooms[i], ri); found = 1; }
    LeaveCriticalSection(&ctx->lock);
    return found;
}

static void ask_for_rooms(void)   /* okw_lock held: refresh the room list (Online: the room server, LAN Play: the LAN) */
{
    ctx->enum_pending = 1;
    if (ctx->mode == OKP_MODE_LAN) okp_lan_discover(ctx);
    okp_rdv_wake(ctx);
}

/* The joiner's destination -> the room host, without asking (B.8): 1. a stand-in address (of a room host or a
 * player); 2. one of a room's host addresses (its public address, or a LAN address: players type the host's IP as in
 * 1998), the room listening on that port first; 3. the room picked earlier in this game; 4. okplay.ini
 * wsock_join=auto and exactly one room of this game. okw_lock held, ctx set. */
static int resolve_rules(uint32_t ip, uint16_t port, uint64_t *host, const char **how, int *nrooms)
{
    uint64_t only = 0, cand = 0, cand_port = 0, picked = 0;
    int n = 0, loop = !ip || (ntohl(ip) >> 24) == 127;   /* (every host lists its loopback address: never "that host") */
    if (peer_of_standin(ip, host)) { *how = "its stand-in address"; *nrooms = -1; return 1; }
    EnterCriticalSection(&ctx->lock);
    for (int i = 0; i < ctx->nrooms; i++) {
        okp_room *rm = &ctx->rooms[i];
        uint16_t ports[8];
        int np = 0;
        if (!room_live(i) || !room_tcp(rm, ports, &np)) continue;
        n++;
        only = rm->host;
        if (picked_host && rm->host == picked_host) picked = rm->host;
        for (int c = 0; c < rm->ncands && !loop; c++) {
            if (!okp_addr_is_v4(&rm->cands[c]) || memcmp((const uint8_t *)&rm->cands[c].sin6_addr + 12, &ip, 4)) continue;
            if (!cand) cand = rm->host;
            for (int k = 0; k < np; k++) if (ports[k] == port && !cand_port) cand_port = rm->host;
        }
    }
    LeaveCriticalSection(&ctx->lock);
    *nrooms = n;
    if (cand_port || cand) { *host = cand_port ? cand_port : cand; *how = "one of the host's own addresses"; return 1; }
    if (picked) { *host = picked; *how = "the room picked before"; return 1; }
    if (join_auto && n == 1) { *host = only; *how = "the only room of this game (wsock_join=auto)"; return 1; }
    return 0;
}

/* ---------------------------------------------------------------- the room list / "Connecting..." box
 *
 * A Windows dialog of our own, on the game's thread (run_box: the game's timer and input messages are held while it
 * is open): the rooms of this game with their player counts and a password marker; Join picks one. The same box
 * waits, with Cancel, for the okplay connection to that host (or the host's answer to a password), so the game's own
 * 1 s connect attempts begin only when the first one can succeed. Tests: OKP_TEST_JOIN_PICK=first | <part of a room
 * name> | cancel answers it, and the window is then never shown. */
enum { LB_LIST = 1, LB_PATH, LB_AUTH };
typedef struct {
    int mode;
    uint64_t host;                 /* LB_LIST: the pick; else: the host waited for */
    char name[64];
    DWORD t0, max_ms, last_enum;
    int hidden;
    char pick[64];
    uint64_t hosts[32];
    uint32_t shown[32];            /* what each line says (lb_sig): a changed player count redraws the list too */
    char full[32];                 /* the line says "full": Join stays in the list and says so (the host refuses too) */
    int nhosts;
} okw_listbox;

static int test_ui(char *pick, int n)   /* 1 = tests answer okplay's TCP join boxes */
{
    char v[64] = "";
    if (!GetEnvironmentVariableA("OKP_TEST_JOIN_PICK", v, sizeof(v))) return 0;
    if (pick) lstrcpynA(pick, v, n);
    return 1;
}

static uint32_t lb_sig(const okw_roominfo *r)   /* a room's line in the list, as a number (" - full" comes from curp/maxp) */
{
    uint32_t h = 2166136261u;
    for (const char *p = r->name; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return h ^ ((uint32_t)r->curp << 1) ^ ((uint32_t)r->maxp << 9) ^ (r->locked ? 1u : 0u) ^ 0x10000u;
}

static void lb_refresh(HWND dlg, okw_listbox *L)
{
    /* static: 5.6 KB, and the game's own threads may have little stack (NFS3 1998: 32,000 bytes). Only the room list
     * box uses it, on the game's thread, and the list is filled and read within this call */
    static okw_roominfo ri[32];
    int n = 0, sel = -1;
    EnterCriticalSection(&okw_lock);
    if (ctx) {
        if (GetTickCount() - L->last_enum >= 1500) { L->last_enum = GetTickCount(); ask_for_rooms(); }
        EnterCriticalSection(&ctx->lock);
        for (int i = 0; i < ctx->nrooms && n < 32; i++) if (room_live(i) && room_tcp(&ctx->rooms[i], NULL, NULL)) room_read(&ctx->rooms[i], &ri[n++]);
        LeaveCriticalSection(&ctx->lock);
    }
    LeaveCriticalSection(&okw_lock);
    int same = n == L->nhosts;
    for (int i = 0; i < n && same; i++) same = ri[i].host == L->hosts[i] && lb_sig(&ri[i]) == L->shown[i];
    if (same && n) return;
    HWND lb = GetDlgItem(dlg, 101);
    int cur = (int)SendMessageA(lb, LB_GETCURSEL, 0, 0);
    uint64_t keep = cur >= 0 && cur < L->nhosts ? L->hosts[cur] : 0;
    SendMessageA(lb, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < n; i++) {
        char line[128];
        snprintf(line, sizeof(line), "%s   (%d/%d players)%s%s", ri[i].name, ri[i].curp, ri[i].maxp,
                 ri[i].maxp > 0 && ri[i].curp >= ri[i].maxp ? " - full" : "", ri[i].locked ? "   [password]" : "");
        SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM)line);
        L->hosts[i] = ri[i].host;
        L->shown[i] = lb_sig(&ri[i]);
        L->full[i] = ri[i].maxp > 0 && ri[i].curp >= ri[i].maxp;
        if (ri[i].host == keep) sel = i;
    }
    L->nhosts = n;
    if (n) SendMessageA(lb, LB_SETCURSEL, sel >= 0 ? sel : 0, 0);
    SetDlgItemTextA(dlg, 100, n ? "Pick the game to join:" : "Looking for games... (none found yet)");
    if (L->pick[0] && n) {   /* tests */
        for (int i = 0; i < n; i++) {
            if (_stricmp(L->pick, "first") && !strstr(ri[i].name, L->pick)) continue;
            L->host = ri[i].host;
            EndDialog(dlg, IDOK);
            return;
        }
    }
}

static int lb_waited(okw_listbox *L)   /* LB_PATH / LB_AUTH: what we wait for is there */
{
    int done = 0;
    EnterCriticalSection(&okw_lock);
    if (ctx) {
        okw_tick();   /* (the password goes again every 500 ms from here) */
        if (L->mode == LB_AUTH) done = auth_state != 1;
        else {
            okp_peer_info_t pi;
            done = okp_peer_info(ctx, L->host, &pi) && pi.have_path;
        }
    }
    LeaveCriticalSection(&okw_lock);
    return done;
}

static INT_PTR CALLBACK lb_proc(HWND dlg, UINT m, WPARAM wp, LPARAM lp)
{
    okw_listbox *L = (okw_listbox *)GetWindowLongA(dlg, DWL_USER);
    switch (m) {
    case WM_INITDIALOG:
        L = (okw_listbox *)lp;
        SetWindowLongA(dlg, DWL_USER, (LONG)(LONG_PTR)L);
        L->t0 = GetTickCount();
        if (L->mode != LB_LIST) {
            char t[160];
            ShowWindow(GetDlgItem(dlg, 101), SW_HIDE);
            ShowWindow(GetDlgItem(dlg, IDOK), SW_HIDE);
            snprintf(t, sizeof(t), L->mode == LB_AUTH ? "Waiting for %s to check the password..." : "Connecting to %s...", L->name);
            SetDlgItemTextA(dlg, 100, t);
        } else {
            lb_refresh(dlg, L);
        }
        SetTimer(dlg, 1, L->mode == LB_LIST ? 300 : 50, NULL);
        if (!L->hidden) {
            SetWindowPos(dlg, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            SetForegroundWindow(dlg);
        }
        return TRUE;
    case WM_TIMER:
        if (!L) break;
        if (L->mode == LB_LIST) {
            if (L->pick[0] && !_stricmp(L->pick, "cancel")) { EndDialog(dlg, IDCANCEL); break; }
            lb_refresh(dlg, L);
            if (L->hidden && GetTickCount() - L->t0 > 15000) EndDialog(dlg, IDCANCEL);   /* (tests: nothing to pick) */
        } else if (lb_waited(L)) EndDialog(dlg, IDOK);
        else if (GetTickCount() - L->t0 > L->max_ms) EndDialog(dlg, IDABORT);
        break;
    case WM_COMMAND:
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); break; }
        if (L && L->mode == LB_LIST && (LOWORD(wp) == IDOK || (LOWORD(wp) == 101 && HIWORD(wp) == LBN_DBLCLK))) {
            int cur = (int)SendDlgItemMessageA(dlg, 101, LB_GETCURSEL, 0, 0);
            if (cur >= 0 && cur < L->nhosts && L->full[cur])   /* (the text goes back with the list's next change) */
                SetDlgItemTextA(dlg, 100, "That game is full. Pick another, or wait for a place.");
            else if (cur >= 0 && cur < L->nhosts) { L->host = L->hosts[cur]; EndDialog(dlg, IDOK); }
        }
        break;
    case WM_DESTROY:
        KillTimer(dlg, 1);
        break;
    }
    return FALSE;
}

static WORD *dlg_str(WORD *p, const WCHAR *s) { while ((*p++ = *s++) != 0) ; return p; }
static WORD *dlg_item(WORD *p, DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const WCHAR *text)
{
    p = (WORD *)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3);
    DLGITEMTEMPLATE *it = (DLGITEMTEMPLATE *)p;
    it->style = style | WS_CHILD | WS_VISIBLE;
    it->dwExtendedStyle = 0;
    it->x = x; it->y = y; it->cx = cx; it->cy = cy; it->id = id;
    p = (WORD *)(it + 1);
    *p++ = 0xFFFF;
    *p++ = cls;   /* 0x80 button, 0x82 static, 0x83 list box */
    p = dlg_str(p, text);
    *p++ = 0;     /* no creation data */
    return p;
}

static INT_PTR lb_run(okw_listbox *L)   /* the game's thread, inside run_box */
{
    static DWORD buf[512];   /* (2 KB off the game's stack: the template is read once, when the dialog is made) */
    WCHAR title[96];
    memset(buf, 0, sizeof(buf));
    okbox_brand(title, 96);
    DLGTEMPLATE *t = (DLGTEMPLATE *)buf;
    t->style = DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU | (L->hidden ? 0 : WS_VISIBLE);
    t->cdit = 4;
    t->cx = 260;
    t->cy = 150;
    WORD *p = (WORD *)(t + 1);
    *p++ = 0;   /* no menu */
    *p++ = 0;   /* the standard dialog class */
    p = dlg_str(p, title);
    *p++ = 8;
    p = dlg_str(p, L"MS Shell Dlg");
    p = dlg_item(p, SS_LEFT, 8, 8, 244, 20, 100, 0x82, L"Looking for games...");
    p = dlg_item(p, LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_BORDER | WS_TABSTOP, 8, 30, 244, 92, 101, 0x83, L"");
    p = dlg_item(p, BS_DEFPUSHBUTTON | WS_TABSTOP, 136, 128, 56, 14, IDOK, 0x80, L"Join");
    p = dlg_item(p, BS_PUSHBUTTON | WS_TABSTOP, 196, 128, 56, 14, IDCANCEL, 0x80, L"Cancel");
    return DialogBoxIndirectParamA(g_self, t, NULL, lb_proc, (LPARAM)L);
}

/* the list box: 1 = *host picked */
static int room_list_box(uint64_t *host)
{
    okw_listbox L;
    okw_box b;
    memset(&L, 0, sizeof(L));
    L.mode = LB_LIST;
    L.hidden = test_ui(L.pick, sizeof(L.pick));
    memset(&b, 0, sizeof(b));
    b.kind = 10;
    b.p = &L;
    if (run_box(&b) != IDOK || !L.host) return 0;
    *host = L.host;
    return 1;
}

/* "Connecting..." until the okplay path to `host` is up (LB_PATH) or the password was answered (LB_AUTH): IDOK = there,
 * IDCANCEL = the player cancelled, IDABORT = max_ms passed */
static INT_PTR wait_box(int mode, uint64_t host, const char *name, DWORD max_ms)
{
    okw_listbox L;
    okw_box b;
    memset(&L, 0, sizeof(L));
    L.mode = mode;
    L.host = host;
    L.max_ms = max_ms;
    lstrcpynA(L.name, name && name[0] ? name : "the host", sizeof(L.name));
    if (lb_waited(&L)) return IDOK;
    Sleep(100);   /* (on a LAN it is there at once: no window for that) */
    if (lb_waited(&L)) return IDOK;
    L.hidden = test_ui(NULL, 0);
    memset(&b, 0, sizeof(b));
    b.kind = 10;
    b.p = &L;
    return run_box(&b);
}

/* The host never answered about the password (its PC froze, its game started or closed): the player is told, as for a
 * wrong password, instead of finding the game back at its menu with no word. Tests (OKP_TEST_JOIN_PASSWORD) get the
 * log line only. Its sentence (no_answer_text, with the boxes) belongs with okbox.c's texts, one place for every
 * sentence a player reads; it stays in this file until that shared code takes it. */
static void notice_no_answer(void)   /* the game's thread, no lock held */
{
    okw_box b;
    okp_log(1, "wsock: notice to the player: No answer from the host. The game may have started or closed.");
    if (okbox_test_answers() && getenv("OKP_TEST_JOIN_PASSWORD")) return;
    memset(&b, 0, sizeof(b));
    b.kind = 11;
    run_box(&b);
}

/* the room has a password: asked here (game thread), then the host's yes / no awaited. 1 = let in; on every other way
 * out the join is taken back (tcp_join_undo: nothing more goes to that host, the next try asks again) */
static int tcp_password(uint64_t host, uint32_t room, const char *name)
{
    okw_box b;
    EnterCriticalSection(&okw_lock);
    int known = auth_host == host && auth_room == room && auth_state == 2;   /* (a new room of the same host asks again) */
    LeaveCriticalSection(&okw_lock);
    if (known) return 1;
    okp_log(1, "wsock: %s has a password - asking for it", name);
    memset(&b, 0, sizeof(b));
    b.kind = 2;
    b.g = room_guid(room);
    if (!run_box(&b)) {
        okp_log(1, "wsock: password cancelled - not joining");
        EnterCriticalSection(&okw_lock);
        tcp_join_undo(host, "password cancelled", 1);
        LeaveCriticalSection(&okw_lock);
        return 0;
    }
    EnterCriticalSection(&okw_lock);
    wide_to_ascii(b.w, join_hash, sizeof(join_hash));
    auth_host = host;
    auth_room = room;
    auth_state = 1;
    auth_tick = 0;
    nheld = 0;
    int in = ctx && tcp_join(host);
    if (in) okw_tick();   /* (the password goes now) */
    InterlockedExchange(&auth_in_list, 1);   /* a "no" is shown here, not by control() */
    LeaveCriticalSection(&okw_lock);
    SecureZeroMemory(b.w, sizeof(b.w));
    INT_PTR w = in ? wait_box(LB_AUTH, host, name, 15000) : IDCANCEL;
    InterlockedExchange(&auth_in_list, 0);
    EnterCriticalSection(&okw_lock);
    int st = in ? auth_state : -2;
    if (st == 0 && ctx) okp_report_session_result(ctx, "refused");   /* the host turned us away */
    if (st != 2)
        tcp_join_undo(host, st == 0 ? "wrong password" : st == -2 ? "not joined" : w == IDABORT ? "no answer about the password" :
                      "the player stopped waiting for the host", 1);
    LeaveCriticalSection(&okw_lock);
    if (st == 2) { okp_log(1, "wsock: the host let us in"); return 1; }
    if (st == 1 && w == IDABORT) {
        okp_log(1, "wsock: no answer from the host about the password - not joining");
        notice_no_answer();
    } else if (st == 1 || st == -2) {
        okp_log(1, "wsock: not joining");
    } else {
        okp_log(1, "wsock: wrong password - not joining");
        if (!getenv("OKP_TEST_JOIN_PASSWORD")) {
            memset(&b, 0, sizeof(b));
            b.kind = 3;
            run_box(&b);
        }
    }
    return 0;
}

/* The game's connect, on its own thread: which room host it goes to, with the boxes that may need (room list, build
 * question, password, waiting for the connection). 1 = go (*host set, joined), 0 = refused, with nothing of a join
 * left behind. No lock held. */
static int tcp_find_host(uint32_t ip, uint16_t port, uint64_t *host)
{
    DWORD t0 = GetTickCount();
    const char *how = "";
    int n = 0, found = 0, listed = 0;
    char t[20];
    for (;;) {   /* (a joiner that just started may not have the room list yet: up to 2 s for it) */
        EnterCriticalSection(&okw_lock);
        if (!ctx) { LeaveCriticalSection(&okw_lock); return 0; }
        found = resolve_rules(ip, port, host, &how, &n);
        if (!found && !n) ask_for_rooms();
        LeaveCriticalSection(&okw_lock);
        if (found || n || GetTickCount() - t0 >= 2000) break;
        Sleep(100);
    }
    if (!found) {
        okp_log(1, "wsock: TCP connect to %s:%u - not an address okplay knows: the room list (%d rooms of this game)", ip_s(ip, t), port, n);
        EnterCriticalSection(&okw_lock);
        picked_host = 0;   /* (the player picks again) */
        LeaveCriticalSection(&okw_lock);
        if (!room_list_box(host)) { okp_log(1, "wsock: no room picked - the connect is refused"); return 0; }
        listed = 1;
        how = "picked from the list";
    }
    okp_log(1, "wsock: TCP connect to %s:%u goes to host %016llx (%s)", ip_s(ip, t), port, (unsigned long long)*host, how);
    okw_roominfo ri;
    EnterCriticalSection(&okw_lock);
    int have = ctx && room_of(*host, &ri);
    LeaveCriticalSection(&okw_lock);
    if (have && !build_ok(*host, ri.build, ri.exe)) {
        EnterCriticalSection(&okw_lock);
        tcp_join_undo(*host, "another build, the player did not join", 1);
        LeaveCriticalSection(&okw_lock);
        return 0;
    }
    EnterCriticalSection(&okw_lock);
    int in = ctx && tcp_join(*host);
    if (!in) tcp_join_undo(*host, "not joined", 1);
    LeaveCriticalSection(&okw_lock);
    if (!in) return 0;
    if (have && ri.locked && !tcp_password(*host, ri.room, ri.name)) return 0;
    if (listed && wait_box(LB_PATH, *host, have ? ri.name : "", 15000) != IDOK) {
        okp_log(1, "wsock: no connection to that host (cancelled, or 15 s) - the connect is refused");
        EnterCriticalSection(&okw_lock);
        tcp_join_undo(*host, "no connection to that host", 1);
        LeaveCriticalSection(&okw_lock);
        return 0;
    }
    return 1;
}

static void send_syn(okw_tconn *c)   /* okw_lock held */
{
    uint8_t syn[9];
    memcpy(syn, &c->lport, 2);
    memcpy(syn + 2, &c->rport, 2);
    memcpy(syn + 4, &tcp_window, 4);
    syn[8] = 0;
    c->syn_held = 0;
    c->t_state = GetTickCount();
    TFRAME(c, TF_SYN, 0, syn, 9);
}

static void lane_base(okw_tconn *c)   /* okw_lock held: the lane's counters now, to see it break later */
{
    okp_peer_info_t pi;
    if (ctx && okp_peer_info(ctx, c->peer, &pi)) { c->base_giveup = pi.r_giveup; c->base_restarts = pi.r_restarts; c->info_init = 1; }
}

/* a connect the game gave up on (its 1 s attempt ran out and it closed the socket) waits 3 s here: its next attempt
 * to the same address takes it over instead of opening another connection (B.6.3 "SYN adoption") */
static okw_tconn *orphan_for(uint32_t ip, uint16_t port)   /* okw_lock held */
{
    for (int i = 0; i < OKW_MAX_CONNS; i++) {
        okw_tconn *c = &conns[i];
        if (c->used && c->orphan && c->dst_ip == ip && c->rport == port && !c->err && GetTickCount() - c->orphan_tick < 3000) return c;
    }
    return NULL;
}

static int tcp_connect(SOCKET s, const struct sockaddr *a, int l)   /* a virtual TCP socket; no lock held */
{
    DWORD t_in = GetTickCount();
    if (!a || l < (int)sizeof(struct sockaddr_in)) return set_err(WSAEFAULT);
    if (a->sa_family != AF_INET) return set_err(WSAEAFNOSUPPORT);
    const struct sockaddr_in *in = (const struct sockaddr_in *)a;
    uint16_t port = ntohs(in->sin_port);
    int e = 0, adopted = 0;
    {
        char t[20];
        tr("game connect %u to %s:%u", (unsigned)s, ip_s(in->sin_addr.s_addr, t), port);
    }
    EnterCriticalSection(&okw_lock);
    okw_sock *k = tsock_of(s);
    if (!k) e = WSAENOTSOCK;
    else if (tcp_state(k) == T_SYN_SENT) e = WSAEALREADY;
    else if (tcp_state(k) == T_ESTAB) e = WSAEISCONN;
    else if (k->tstate == T_LISTEN || k->c) e = WSAEINVAL;
    else if (relisten_port) relisten_port = 0;   /* the game connects: this PC joins (a listen after this doesn't host) */
    okw_tconn *o = e ? NULL : orphan_for(in->sin_addr.s_addr, port);
    if (o) {
        if (k->tstate == T_NEW || !k->explicit_port) k->port = o->lport;
        k->tstate = T_BOUND;
        k->c = o;
        o->orphan = 0;
        adopted = 1;
        okp_log(2, "wsock: TCP connection %08x taken over by the game's next connect (socket %u)", (unsigned)o->id, (unsigned)s);
    }
    LeaveCriticalSection(&okw_lock);
    if (e) return set_err(e);
    uint64_t host = 0;
    if (!adopted && tcp_unhost_for_connect() < 0) return set_err(WSAECONNREFUSED);
    if (!adopted && !tcp_find_host(in->sin_addr.s_addr, port, &host)) return set_err(WSAECONNREFUSED);
    EnterCriticalSection(&okw_lock);
    k = tsock_of(s);
    okw_tconn *c = NULL;
    if (adopted) { if (!k || k->c != o) e = WSAEINVAL; }
    else if (!k || k->c || (k->tstate != T_NEW && k->tstate != T_BOUND)) e = WSAEINVAL;
    else if (!ctx || !tcp_join(host)) e = WSAECONNREFUSED;   /* (okplay stopped meanwhile) */
    else if (!(c = tconn_new())) e = WSAENOBUFS;
    if (e) {
        if (!adopted && host) tcp_join_undo(host, "the connect failed here", 0);
        LeaveCriticalSection(&okw_lock);
        return set_err(e);
    }
    if (!adopted) {
        if (k->tstate == T_NEW) { k->port = eph_port(); k->tstate = T_BOUND; }
        c->state = T_SYN_SENT;
        c->peer = host;
        do c->id = (uint32_t)okp_rand64(); while (!c->id);
        c->initiator = 1;
        c->lport = k->port;
        c->rport = port;
        c->dst_ip = in->sin_addr.s_addr;
        c->rip = standin_of(host);
        k->c = c;
        picked_host = host;   /* (the game's retries go there too; taken back with the join) */
        tconn_hold(c);
        lane_base(c);
        /* After a box that took most of the game's 1 s, it is about to close this socket and try again: the SYN waits
         * 50 ms, and the next attempt takes the connection over (tests: OKP_TEST_SYN_DELAY_MS holds every SYN). */
        DWORD hold = GetTickCount() - t_in > 900 ? 50 : 0;
        char v[16];
        if (GetEnvironmentVariableA("OKP_TEST_SYN_DELAY_MS", v, sizeof(v))) hold += (DWORD)atoi(v);
        okp_log(2, "wsock: TCP connection %08x: connecting from port %u to %016llx port %u%s", (unsigned)c->id, c->lport,
                (unsigned long long)host, c->rport, hold ? " (the SYN waits)" : "");
        if (hold) { c->syn_held = 1; c->syn_due = GetTickCount() + hold; }
        else send_syn(c);
    }
    int nb = k->nonblock;
    LeaveCriticalSection(&okw_lock);
    if (nb) return set_err(WSAEWOULDBLOCK);   /* always, never 0: completion shows in select (games rely on it) */
    InterlockedIncrement(&okw_waiters);
    for (;;) {   /* blocking: until accepted, refused or 20 s without an answer (tcp_tick) */
        ResetEvent(okw_ev);
        EnterCriticalSection(&okw_lock);
        okw_tick();
        k = tsock_of(s);
        int st = k && k->c ? k->c->state : -1;
        if (st == T_FAILED) { e = k->c->so_error ? k->c->so_error : WSAECONNREFUSED; k->c->so_error = 0; }
        else if (st != T_ESTAB && st != T_SYN_SENT) e = WSAEINTR;
        LeaveCriticalSection(&okw_lock);
        if (st == T_ESTAB || e) break;
        ev_wait(50);
    }
    InterlockedDecrement(&okw_waiters);
    return e ? set_err(e) : 0;
}

static INT_PTR lb_run_any(void *L) { return lb_run((okw_listbox *)L); }

static SOCKET tcp_accept(SOCKET s, struct sockaddr *a, int *l)   /* a virtual TCP socket; no lock held */
{
    SOCKET r = INVALID_SOCKET;
    int e = 0;
    if (a && (!l || *l < (int)sizeof(struct sockaddr_in))) { WSASetLastError(WSAEFAULT); return INVALID_SOCKET; }
    InterlockedIncrement(&okw_waiters);
    for (;;) {
        ResetEvent(okw_ev);
        EnterCriticalSection(&okw_lock);
        okw_sock *k = tsock_of(s);
        if (!k) e = WSAEINTR;   /* closed by another thread while we waited */
        else if (k->tstate != T_LISTEN) e = WSAEINVAL;
        else if (k->nbacklog) {
            okw_sock *n = NULL;
            for (int i = 0; i < OKW_MAX_SOCKS && !n; i++) if (!socks[i].used) n = &socks[i];
            SOCKET ns = n ? socket(AF_INET, SOCK_STREAM, 0) : INVALID_SOCKET;   /* a handle for the game, never used for I/O */
            if (!n) e = WSAEMFILE;
            else if (ns == INVALID_SOCKET) e = WSAGetLastError();
            else {
                okw_tconn *c = k->backlog[0];
                memmove(k->backlog, k->backlog + 1, (k->nbacklog - 1) * sizeof(k->backlog[0]));
                k->nbacklog--;
                memset(n, 0, sizeof(*n));
                n->used = 1;
                n->s = ns;
                n->kind = K_TCP;
                n->tstate = T_BOUND;
                n->port = k->port;
                n->bound_ip = k->bound_ip;
                n->nonblock = k->nonblock;   /* (Winsock: an accepted socket takes the listener's options) */
                n->keepalive = k->keepalive;
                n->nodelay = k->nodelay;
                n->c = c;
                socks_gone = 0;
                if (a) {
                    struct sockaddr_in *in = (struct sockaddr_in *)a;
                    memset(in, 0, sizeof(*in));
                    in->sin_family = AF_INET;
                    in->sin_addr.s_addr = c->rip;
                    in->sin_port = htons(c->rport);
                    *l = sizeof(*in);
                }
                r = ns;
                okp_log(2, "wsock: TCP connection %08x accepted as socket %u%s", (unsigned)c->id, (unsigned)ns,
                        c->err ? " (already reset by the other side)" : "");
                {
                    char t[20];
                    tr("game accept on %u -> socket %u, peer %s:%u", (unsigned)s, (unsigned)ns, ip_s(c->rip, t), c->rport);
                }
            }
        } else if (k->nonblock) e = WSAEWOULDBLOCK;
        LeaveCriticalSection(&okw_lock);
        if (r != INVALID_SOCKET || e) break;
        ev_wait(50);
    }
    InterlockedDecrement(&okw_waiters);
    if (e && e != WSAEWOULDBLOCK) tr("game accept on %u -> error %d", (unsigned)s, e);
    if (e) WSASetLastError(e);
    return r;
}

static int tcp_send(SOCKET s, const char *b, int len, int f)   /* a virtual TCP socket; no lock held */
{
    int r = SOCKET_ERROR, e = 0, waited = 0;
    if (f & MSG_OOB) return set_err(WSAEOPNOTSUPP);
    if (len < 0 || (len && !b)) return set_err(WSAEFAULT);
    for (;;) {
        ResetEvent(okw_ev);
        EnterCriticalSection(&okw_lock);
        okw_sock *k = tsock_of(s);
        okw_tconn *c = k ? k->c : NULL;
        if (!k) e = WSAENOTSOCK;
        else if (!c || c->state != T_ESTAB) e = WSAENOTCONN;
        else if (c->err) e = c->err;
        else if (c->wr_shut) e = WSAESHUTDOWN;
        else {
            uint32_t room = c->snd_limit - c->snd_next;
            if (room || !len) {
                int n = (uint32_t)len < room ? len : (int)room;
                for (int off = 0; off < n;) {
                    int m = n - off < OKW_TDATA_MAX ? n - off : OKW_TDATA_MAX;
                    tr("tcp tx DATA conn %08x off %u len %d type %d", (unsigned)c->id, (unsigned)c->snd_next, m, m >= 3 ? (uint8_t)b[off + 2] : -1);
                    c->st_txf++;
                    c->st_txb += m;
                    TFRAME(c, TF_DATA, c->snd_next, b + off, m);
                    c->snd_next += m;
                    off += m;
                }
                if (n) last_race_tick = GetTickCount();
                r = n;
            } else if (k->nonblock) { e = WSAEWOULDBLOCK; c->st_wb++; }
        }
        LeaveCriticalSection(&okw_lock);
        if (r != SOCKET_ERROR || e) break;
        if (!waited++) InterlockedIncrement(&okw_waiters);
        ev_wait(50);   /* blocking: until the other side's game reads (WND) */
    }
    if (waited) InterlockedDecrement(&okw_waiters);
    return e ? set_err(e) : r;
}

static int tcp_recv(SOCKET s, char *b, int len, int f)   /* a virtual TCP socket; no lock held */
{
    int r = SOCKET_ERROR, e = 0, waited = 0;
    if (f & MSG_OOB) return set_err(WSAEINVAL);
    if (len < 0 || (len && !b)) return set_err(WSAEFAULT);
    for (;;) {
        ResetEvent(okw_ev);
        EnterCriticalSection(&okw_lock);
        okw_sock *k = tsock_of(s);
        okw_tconn *c = k ? k->c : NULL;
        if (!k) e = WSAENOTSOCK;
        else if (!c || c->state != T_ESTAB) e = WSAENOTCONN;
        else if (c->count && len) {
            /* MSG_PEEK: the bytes stay (the game peeks the length, then the whole message); one copy across frames */
            uint32_t n = (uint32_t)len < c->count ? (uint32_t)len : c->count;
            ring_peek(c, (uint8_t *)b, n);
            if (!(f & MSG_PEEK)) {
                int was_full = c->count * 4 >= c->cap * 3;
                c->head = (c->head + n) % c->cap;
                c->count -= n;
                c->consumed += n;
                c->sel_consumed = 1;
                if (!c->err && !c->rst_rx && !c->fin_rx && (c->consumed - c->adv >= c->cap / 4 || was_full)) {
                    TFRAME(c, TF_WND, c->consumed, NULL, 0);   /* the sender may go on */
                    c->adv = c->consumed;
                }
            }
            r = (int)n;
        }
        else if (c->err) e = c->err;
        else if (c->fin_rx || c->rd_shut || !len) r = 0;
        else if (k->nonblock) e = WSAEWOULDBLOCK;
        LeaveCriticalSection(&okw_lock);
        if (r != SOCKET_ERROR || e) break;
        if (!waited++) InterlockedIncrement(&okw_waiters);
        ev_wait(50);
    }
    if (waited) InterlockedDecrement(&okw_waiters);
    return e ? set_err(e) : r;
}

static int tcp_shutdown(SOCKET s, int how)   /* a virtual TCP socket; no lock held */
{
    int e = 0;
    EnterCriticalSection(&okw_lock);
    okw_sock *k = tsock_of(s);
    okw_tconn *c = k ? k->c : NULL;
    if (!k) e = WSAENOTSOCK;
    else if (how < 0 || how > 2) e = WSAEINVAL;
    else if (!c || c->state != T_ESTAB) e = WSAENOTCONN;
    else {
        if (how != 1) {   /* SD_RECEIVE / SD_BOTH: with bytes still unread Windows resets the connection */
            c->rd_shut = 1;
            if (c->count) { tconn_abort(c, 1, WSAECONNABORTED); c->wr_shut = 1; }
        }
        if (how != 0 && !c->wr_shut) {
            c->wr_shut = 1;
            if (!c->err) TFRAME(c, TF_FIN, c->snd_next, NULL, 0);
        }
        wake();
    }
    LeaveCriticalSection(&okw_lock);
    return e ? set_err(e) : 0;
}

static void tcp_close(okw_sock *k)   /* okw_lock held; a virtual TCP socket the game closes */
{
    if (k->tstate == T_LISTEN && k->explicit_port) { relisten_port = k->port; relisten_tick = GetTickCount(); }
    if (k->c && k->c->state == T_ESTAB) {   /* (WSACleanup gives the lane to this player a moment to drain) */
        int i = 0;
        while (i < nclosed_peers && closed_peers[i].peer != k->c->peer) i++;
        if (i == nclosed_peers) { if (nclosed_peers < 8) nclosed_peers++; else i = 0; }
        closed_peers[i].peer = k->c->peer;
        closed_peers[i].tick = GetTickCount();
    }
    if (k->tstate == T_LISTEN) {
        for (int i = 0; i < k->nbacklog; i++) { tconn_abort(k->backlog[i], 4, WSAECONNABORTED); tconn_free(k->backlog[i]); }
        k->nbacklog = 0;
        k->tstate = T_BOUND;
        if (ctx && ctx->is_host && game->host_rule == HOST_LISTEN && k->explicit_port) {
            int other = 0;
            for (int i = 0; i < OKW_MAX_SOCKS; i++)
                if (&socks[i] != k && socks[i].used && socks[i].kind == K_TCP && socks[i].tstate == T_LISTEN && socks[i].explicit_port) other = 1;
            if (other) build_room_record();
            else {   /* the game stopped listening: its room comes off the list (connections made stay) */
                ctx->closing = 1;
                ctx->session_open = 0;
                ctx->is_host = 0;
                okp_rdv_wake(ctx);
                okp_log(1, "wsock: the game closed its room (it stopped listening on port %u)", k->port);
                room_closed = 1;
                room_closed_tick = GetTickCount();
                host_game_over(1);
            }
        }
    }
    okw_tconn *c = k->c;
    if (c && c->initiator && !c->err && (c->state == T_SYN_SENT ||
        (c->state == T_ESTAB && !c->snd_next && !c->rx_next && GetTickCount() - c->t_state < 1500))) {
        c->orphan = 1;   /* the game's connect attempt ran out: its next attempt may take this one over (3 s) */
        c->orphan_tick = GetTickCount();
        k->c = NULL;
        okp_log(2, "wsock: TCP connection %08x: the game closed it while connecting - kept 3 s for its next attempt", (unsigned)c->id);
        c = NULL;
    }
    if (c && c->initiator && !c->err && c->state == T_ESTAB && !c->rx_next) {
        /* shared TCP rule: the join came up, the host's game never sent a byte (a race is running, or its game hangs)
         * and our game gave up: without a word the player would only find the game back at its menu */
        okp_log(1, "wsock: notice to the player: No answer from the host. The game may have started or closed.");
        if (!(okbox_test_answers() && getenv("OKP_TEST_JOIN_PASSWORD"))) {   /* (shown by ui_show_pending) */
            InterlockedExchange(&ui_noanswer, 1);
            InterlockedExchange(&ui_pending, 1);
        }
    }
    if (c) {
        if (c->state == T_SYN_SENT) tconn_abort(c, 5, WSAECONNABORTED);
        else if (c->state == T_ESTAB) {
            if (c->count && !c->rst_rx) tconn_abort(c, 1, WSAECONNABORTED);   /* unread bytes: Windows resets too */
            else if (!c->wr_shut && !c->err) TFRAME(c, TF_FIN, c->snd_next, NULL, 0);   /* graceful: what was sent still goes */
        }
        okp_log(2, "wsock: TCP connection %08x closed by the game", (unsigned)c->id);
        uint64_t peer = c->peer;
        tconn_free(c);
        k->c = NULL;
        tcp_conn_gone(peer);
    }
    room_record_follow();   /* (a player's connection closed by the host's game: one fewer in the room) */
    wake();   /* (a select or accept waiting on it in another thread) */
}

static int tcp_name(SOCKET s, struct sockaddr *a, int *l, int peer)   /* getsockname / getpeername */
{
    int e = 0;
    if (!a || !l || *l < (int)sizeof(struct sockaddr_in)) return set_err(WSAEFAULT);
    EnterCriticalSection(&okw_lock);
    okw_sock *k = tsock_of(s);
    struct sockaddr_in *in = (struct sockaddr_in *)a;
    if (!k) e = WSAENOTSOCK;
    else if (peer && (!k->c || k->c->state != T_ESTAB)) e = WSAENOTCONN;
    else if (!peer && k->tstate == T_NEW && !k->c) e = WSAEINVAL;
    else {
        memset(in, 0, sizeof(*in));
        in->sin_family = AF_INET;
        if (peer) { in->sin_addr.s_addr = k->c->rip; in->sin_port = htons(k->c->rport); }
        else {   /* a connected socket names the address it uses: here, our stand-in */
            in->sin_addr.s_addr = k->c ? standin_of(okw_self) : k->bound_ip;
            in->sin_port = htons(k->port);
        }
        *l = sizeof(*in);
    }
    LeaveCriticalSection(&okw_lock);
    return e ? set_err(e) : 0;
}

static int opt_int(const char *v, int l) { int x = 0; if (!v || l < 1) return 0; if (l >= 4) memcpy(&x, v, 4); else x = (unsigned char)v[0]; return x; }

static int tcp_setopt(SOCKET s, int lv, int o, const char *v, int l)   /* any TCP socket in the table; no lock held */
{
    EnterCriticalSection(&okw_lock);
    okw_sock *k = tsock_of(s);
    int virt = tcp_virtual(k);
    if (k) {
        if (lv == SOL_SOCKET && o == SO_KEEPALIVE) k->keepalive = opt_int(v, l) != 0;
        else if (lv == IPPROTO_TCP && o == TCP_NODELAY) k->nodelay = opt_int(v, l) != 0;
        else if (lv == SOL_SOCKET && o == SO_LINGER && v && l >= (int)sizeof(struct linger)) memcpy(&k->linger, v, sizeof(k->linger));
        else if (lv == SOL_SOCKET && o == (int)SO_DONTLINGER) k->linger.l_onoff = !opt_int(v, l);
    }
    LeaveCriticalSection(&okw_lock);
    if (virt) return 0;                 /* recorded; no meaning on a virtual socket */
    return setsockopt(s, lv, o, v, l);  /* not decided yet: Windows' socket gets it too (the original TCP/IP keeps it) */
}

static int tcp_getopt(SOCKET s, int lv, int o, char *v, int *l)   /* a virtual TCP socket; no lock held */
{
    int val = 0, have = 1;
    EnterCriticalSection(&okw_lock);
    okw_sock *k = tsock_of(s);
    if (!k) { LeaveCriticalSection(&okw_lock); return set_err(WSAENOTSOCK); }
    if (lv == SOL_SOCKET && o == SO_LINGER) {
        if (!v || !l || *l < (int)sizeof(struct linger)) { LeaveCriticalSection(&okw_lock); return set_err(WSAEFAULT); }
        memcpy(v, &k->linger, sizeof(k->linger));
        *l = sizeof(k->linger);
        LeaveCriticalSection(&okw_lock);
        return 0;
    }
    if (lv == SOL_SOCKET && o == SO_ERROR) { if (k->c) { val = k->c->so_error; k->c->so_error = 0; } }   /* cleared once read */
    else if (lv == SOL_SOCKET && o == SO_TYPE) val = SOCK_STREAM;
    else if (lv == SOL_SOCKET && o == SO_ACCEPTCONN) val = k->tstate == T_LISTEN;
    else if (lv == SOL_SOCKET && o == SO_KEEPALIVE) val = k->keepalive;
    else if (lv == SOL_SOCKET && o == (int)SO_DONTLINGER) val = !k->linger.l_onoff;
    else if (lv == SOL_SOCKET && (o == SO_RCVBUF || o == SO_SNDBUF)) val = (int)tcp_window;
    else if (lv == IPPROTO_TCP && o == TCP_NODELAY) val = k->nodelay;
    else have = 0;
    LeaveCriticalSection(&okw_lock);
    if (!have) return getsockopt(s, lv, o, v, l);
    if (!v || !l || *l < (int)sizeof(int)) return set_err(WSAEFAULT);
    memcpy(v, &val, sizeof(val));
    *l = sizeof(int);
    return 0;
}

static int tcp_ioctl(SOCKET s, long cmd, u_long *arg)   /* any TCP socket in the table; no lock held */
{
    int r = 0;
    EnterCriticalSection(&okw_lock);
    okw_sock *k = tsock_of(s);
    int virt = tcp_virtual(k);
    if (k && cmd == (long)WS_FIONBIO) k->nonblock = arg && *arg;
    if (k && virt) {
        if (cmd == (long)WS_FIONBIO) ;
        else if (cmd == (long)WS_FIONREAD) { if (arg) *arg = k->c && k->c->state == T_ESTAB ? k->c->count : 0; }
        else if (cmd == OKW_SIOCATMARK) { if (arg) *arg = 1; }   /* no urgent data, ever */
        else r = set_err(WSAEINVAL);
    }
    LeaveCriticalSection(&okw_lock);
    if (!virt) return ioctlsocket(s, cmd, arg);
    return r;
}

/* ---------------------------------------------------------------- select over virtual sockets
 *
 * Virtual sockets (UDP, and TCP once Online/LAN Play is on) are ready when:
 *   read:   a listener with a connection waiting; a connection with bytes, FIN or an error; UDP with a packet
 *   write:  a connection with room in the other side's window, not shut; UDP always
 *   except: only a connect that failed (never an established connection: games close what shows there)
 * The wait is on okw_ev (set by arrivals and state changes), not polling; okw_tick runs every round, so a game
 * that only selects still drives the okplay side. No-progress rule: a connection reported readable whose bytes have
 * not changed since, and which the game only peeked, is not reported again until something new arrives (Windows
 * would return at once, and a game waiting for the rest of a message would spin its comms thread - on one pinned
 * core, against our own network thread that has to deliver that rest). Handles Windows owns are polled alongside. */
static okw_sock *vsock_of(SOCKET s)   /* okw_lock held */
{
    for (int i = 0; i < OKW_MAX_SOCKS; i++)
        if (socks[i].used && socks[i].s == s && (socks[i].kind == K_UDP || mode_choice >= 2)) return &socks[i];
    return NULL;
}

static int sel_ready(okw_sock *k, int set)   /* okw_lock held */
{
    if (k->kind == K_UDP) return set == 0 ? k->q != NULL : set == 1;
    okw_tconn *c = k->c;
    if (set == 1) return c && c->state == T_ESTAB && !c->err && !c->wr_shut && c->snd_limit != c->snd_next;
    if (set == 2) return c && c->state == T_FAILED;
    if (k->tstate == T_LISTEN) return k->nbacklog > 0;
    if (!c || c->state != T_ESTAB || !(c->count || c->fin_rx || c->err)) return 0;
    if (c->sel_shown && c->sel_gen == c->rx_gen && !c->sel_consumed) return 0;   /* the no-progress rule */
    c->sel_shown = 1;
    c->sel_gen = c->rx_gen;
    c->sel_consumed = 0;
    return 1;
}

/* The game's two values while it listens on its port (known builds), read at every select (the game's comms thread,
 * often): a scan of the socket table and two reads. okw_lock held.
 *  - logged when they change, to see what the game does when a host leaves its lobby or a race starts;
 *  - a host that left its lobby: the game says "not hosting" again but keeps its listener open until the player
 *    enters the network menu the next time (seen: 40-50 s) - the room would stay on the list that long. So when
 *    this PC hosts, the game has said "not hosting" for GS_LEFT_MS and no player is connected, the room comes off
 *    the list here, the same way as when the listener closes (tcp_close). */
#define GS_LEFT_MS 1500
static void game_state_watch(void)
{
    static DWORD left_tick;
    int net, srv, open = 0, said;
    if (!game->says_hosts) return;
    for (int i = 0; i < OKW_MAX_SOCKS && !open; i++)
        if (socks[i].used && socks[i].kind == K_TCP && socks[i].tstate == T_LISTEN && socks[i].explicit_port) open = 1;
    if (!open || (said = game->says_hosts(&net, &srv)) < 0) { left_tick = 0; return; }
    int type = -1;
    if (game->max_players) game->max_players(&type);
    if (net != gs_net || srv != gs_srv || type != gs_type) {
        gs_net = net;
        gs_srv = srv;
        gs_type = type;
        okp_log(2, "wsock: game state now: net %d, server %d, game type %d", net, srv, type);
    }
    if (said || !ctx || !ctx->is_host || game->host_rule != HOST_LISTEN || tcp_room_players()) { left_tick = 0; return; }
    if (!left_tick) { left_tick = GetTickCount() | 1; return; }
    if (GetTickCount() - left_tick < GS_LEFT_MS) return;
    left_tick = 0;
    ctx->closing = 1;
    ctx->session_open = 0;
    ctx->is_host = 0;
    okp_rdv_wake(ctx);
    okp_log(1, "wsock: the game left its lobby (its listener stays open) - its room comes off the list");
    room_closed = 1;
    room_closed_tick = GetTickCount();
    host_game_over(1);
}

int WINAPI x_select(int nfds, fd_set *r, fd_set *w, fd_set *e, const struct timeval *t)
{
    if (!okw_on) return select(nfds, r, w, e, t);
    fd_set *set[3] = { r, w, e };
    static const int max = FD_SETSIZE;
    char virt[3][FD_SETSIZE];
    int nv = 0, nr = 0;
    for (int i = 0; i < 3; i++) if (set[i] && (int)set[i]->fd_count > max) return select(nfds, r, w, e, t);
    EnterCriticalSection(&okw_lock);
    for (int i = 0; i < 3; i++)
        for (int j = 0; set[i] && j < (int)set[i]->fd_count; j++) {
            virt[i][j] = vsock_of(set[i]->fd_array[j]) != NULL;
            if (virt[i][j]) nv++; else nr++;
        }
    game_state_watch();
    LeaveCriticalSection(&okw_lock);
    if (!nv) return select(nfds, r, w, e, t);   /* (all sets empty: Windows' WSAEINVAL, as the game expects) */
    fd_set in[3];
    for (int i = 0; i < 3; i++) if (set[i]) memcpy(&in[i], set[i], sizeof(in[i]));
    DWORD limit = t ? (DWORD)t->tv_sec * 1000 + (DWORD)(t->tv_usec + 999) / 1000 : INFINITE;
    DWORD t0 = now_ms();
    int result;
    InterlockedIncrement(&okw_waiters);
    for (;;) {
        fd_set out[3];
        int ready = 0, gone = 0;
        for (int i = 0; i < 3; i++) out[i].fd_count = 0;
        ResetEvent(okw_ev);
        EnterCriticalSection(&okw_lock);
        okw_tick();
        for (int i = 0; i < 3; i++)
            for (int j = 0; set[i] && j < (int)in[i].fd_count; j++) {
                if (!virt[i][j]) continue;
                okw_sock *k = vsock_of(in[i].fd_array[j]);
                if (!k) gone = 1;
                else if (sel_ready(k, i)) { out[i].fd_array[out[i].fd_count++] = in[i].fd_array[j]; ready++; }
            }
        LeaveCriticalSection(&okw_lock);
        if (gone) { result = set_err(WSAENOTSOCK); break; }   /* closed by another thread meanwhile */
        if (nr) {   /* Windows' own handles: a poll */
            fd_set rs[3];
            struct timeval zero = { 0, 0 };
            for (int i = 0; i < 3; i++) {
                rs[i].fd_count = 0;
                for (int j = 0; set[i] && j < (int)in[i].fd_count; j++) if (!virt[i][j]) rs[i].fd_array[rs[i].fd_count++] = in[i].fd_array[j];
            }
            int k = select(0, rs[0].fd_count ? &rs[0] : NULL, rs[1].fd_count ? &rs[1] : NULL, rs[2].fd_count ? &rs[2] : NULL, &zero);
            if (k == SOCKET_ERROR) { result = SOCKET_ERROR; break; }
            for (int i = 0; i < 3 && k > 0; i++)
                for (unsigned j = 0; j < rs[i].fd_count; j++) { out[i].fd_array[out[i].fd_count++] = rs[i].fd_array[j]; ready++; }
        }
        DWORD el = now_ms() - t0;
        if (ready || (limit != INFINITE && el >= limit)) {
            for (int i = 0; i < 3; i++) if (set[i]) { set[i]->fd_count = out[i].fd_count; memcpy(set[i]->fd_array, out[i].fd_array, out[i].fd_count * sizeof(SOCKET)); }
            result = ready;
            break;
        }
        DWORD wt = limit == INFINITE ? 50 : limit - el;
        if (wt > 50) wt = 50;          /* okw_tick runs at least every 50 ms */
        if (nr && wt > 5) wt = 5;      /* Windows' handles: polled every 5 ms */
        ev_wait(wt);
    }
    InterlockedDecrement(&okw_waiters);
    return result;
}

SOCKET WINAPI x_socket(int af, int type, int proto)
{
    if (!okw_on) return socket(af, type, proto);
    SOCKET s = socket(af, type, proto);
    if (s == INVALID_SOCKET || af != AF_INET) return s;
    int kind = type == SOCK_DGRAM && (game->kinds & OKW_UDP) ? K_UDP :
               type == SOCK_STREAM && (game->kinds & OKW_TCP) && (!proto || proto == IPPROTO_TCP) ? K_TCP : 0;
    if (!kind) return s;   /* a type this game's profile leaves to Windows (Road Rash's TCP, NFS3's IPX) */
    check_new_game();
    socks_gone = 0;
    if (kind == K_UDP) {
        if (choose_mode() == 1) return s;   /* the original TCP/IP: Windows' own socket, untouched */
        tr("game socket %u: mode chosen, thread %lu", (unsigned)s, (unsigned long)GetCurrentThreadId());
    } else if (mode_choice == 1) {
        return s;   /* a stream socket: the box comes at its first bind/listen/connect (tcp_mode_pass) */
    }
    EnterCriticalSection(&okw_lock);
    if (kind == K_UDP && mode_choice >= 2) start_okplay();   /* (not while the Online/LAN box is still open) */
    for (int i = 0; i < OKW_MAX_SOCKS; i++) {
        if (socks[i].used) continue;
        memset(&socks[i], 0, sizeof(socks[i]));
        socks[i].used = 1;
        socks[i].s = s;
        socks[i].kind = kind;
        tr("game socket %u (%s, slot %d)", (unsigned)s, kind == K_UDP ? "virtual UDP" : "TCP", i);
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
    okw_sock *tk = k ? NULL : tsock_of(s);
    if (tk) {
        if (tcp_virtual(tk)) tcp_close(tk);
        tk->used = 0;
        tr("game closesocket %u (TCP) port %u", (unsigned)s, tk->port);
        int open = 0;
        for (int i = 0; i < OKW_MAX_SOCKS; i++) open += socks[i].used;
        if (!open) { socks_gone_tick = GetTickCount(); socks_gone = 1; }
    }
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
    int r = closesocket(s);
    /* "That game is full.": a game that drops back to a menu after the refusal makes no further network call there,
     * so the notice is shown as the game closes the refused socket (no lock held; ui_show_pending's own guards apply) */
    if (tk && (ui_full || ui_noanswer)) ui_show_pending();
    return r;
}

int WINAPI x_bind(SOCKET s, const struct sockaddr *a, int l)
{
    if (!okw_on) return bind(s, a, l);
    if (tcp_entry(s, 0)) {
        if (tcp_mode_pass()) return bind(s, a, l);
        EnterCriticalSection(&okw_lock);
        okw_sock *tk = tsock_of(s);
        int r = tk ? tcp_bind(tk, a, l) : bind(s, a, l);
        int window = relisten_port && GetTickCount() - relisten_tick < 2000;
        LeaveCriticalSection(&okw_lock);
        if (!window) ui_show_pending();   /* (never between a listener's close and the next listen: the host rule's 2 s) */
        return r;
    }
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
    if (tcp_entry(s, 0)) {
        if (tcp_mode_pass()) return connect(s, a, l);
        ui_show_pending();
        return tcp_connect(s, a, l);
    }
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
    if (tcp_entry(s, 1)) {
        int r = tcp_name(s, a, l, 0);
        char t[20];
        if (r) tr("game getsockname %u (TCP) -> error %d", (unsigned)s, WSAGetLastError());
        else tr("game getsockname %u (TCP) -> %s:%u", (unsigned)s, ip_s(((struct sockaddr_in *)a)->sin_addr.s_addr, t),
                ntohs(((struct sockaddr_in *)a)->sin_port));
        return r;
    }
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
    if (tcp_entry(s, 1)) return tcp_send(s, b, len, f);   /* (a connected stream socket: the address is ignored) */
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
    if (tcp_entry(s, 1)) {   /* (no box waits here: the game's comms thread) */
        static LONG told;
        if (!InterlockedExchange(&told, 1))
            okp_log(2, "wsock: the game sends TCP from a thread at priority %d", GetThreadPriority(GetCurrentThread()));
        return tcp_send(s, b, len, f);
    }
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
    if (tcp_entry(s, 1)) return tcp_recv(s, b, len, f);
    wait_for_box();
    int handled;
    int r = do_recv(s, b, len, NULL, NULL, &handled);
    return handled ? r : recv(s, b, len, f);
}

int WINAPI x_recvfrom(SOCKET s, char *b, int len, int f, struct sockaddr *a, int *al)
{
    if (!okw_on) return recvfrom(s, b, len, f, a, al);
    if (tcp_entry(s, 1)) return tcp_recv(s, b, len, f);
    wait_for_box();
    ui_show_pending();
    int handled;
    int r = do_recv(s, b, len, a, al, &handled);
    return handled ? r : recvfrom(s, b, len, f, a, al);
}

int WINAPI x_ioctlsocket(SOCKET s, long cmd, u_long *arg)
{
    if (!okw_on) return ioctlsocket(s, cmd, arg);
    if (tcp_entry(s, 0)) return tcp_ioctl(s, cmd, arg);
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
    if (tcp_entry(s, 0)) return tcp_setopt(s, lv, o, v, l);
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
    if (okw_on && tcp_entry(s, 1)) return tcp_getopt(s, lv, o, v, l);
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
    if (!(game->kinds & OKW_UDP) && mode_choice < 2) return gethostbyname(n);   /* (TCP games: the box comes at bind) */
    if (choose_mode() == 1) return gethostbyname(n);
    EnterCriticalSection(&okw_lock);
    if (mode_choice >= 2) start_okplay();
    if (!okw_self) okw_self = okp_rand64();   /* the stand-in is known before okplay runs (box still open) */
    int mine = !n || !n[0] || !_stricmp(n, me);
    if (!mine) {
        LeaveCriticalSection(&okw_lock);
        /* "oldkid.net" (what NFS3's Join box is pre-filled with, nfs3_fix.c) and "okplay" mean "okplay's room list":
         * never looked up - oldkid.net is a real host, and the game must never connect to it */
        int list_name = (game->kinds & OKW_TCP) && mode_choice >= 2 && n && (!_stricmp(n, "oldkid.net") || !_stricmp(n, "okplay"));
        struct hostent *h = list_name ? NULL : gethostbyname(n);
        if (h || !(game->kinds & OKW_TCP) || mode_choice < 2) return h;
        /* A TCP game gives up on a name that doesn't resolve before it connects: 10.0.0.1 ("pick from the list")
         * lets a typed word such as "okplay" reach connect, where okplay finds the room. */
        if (list_name) okp_log(1, "wsock: \"%s\" - the game gets 10.0.0.1, its connect goes to okplay's room list", n);
        else okp_log(1, "wsock: \"%s\" doesn't resolve - the game gets 10.0.0.1, its connect goes to okplay's room list", n ? n : "");
        static uint32_t any;
        static char *alist[2];
        static struct hostent ahe;
        any = htonl(0x0a000001);
        alist[0] = (char *)&any;
        alist[1] = NULL;
        ahe.h_name = (char *)"okplay";
        ahe.h_aliases = NULL;
        ahe.h_addrtype = AF_INET;
        ahe.h_length = 4;
        ahe.h_addr_list = alist;
        return &ahe;
    }
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
SOCKET WINAPI x_accept(SOCKET s, struct sockaddr *a, int *l)
{
    if (okw_on && tcp_entry(s, 1)) return tcp_accept(s, a, l);
    return accept(s, a, l);
}
int WINAPI x_getpeername(SOCKET s, struct sockaddr *a, int *l)
{
    if (!okw_on) return getpeername(s, a, l);
    if (tcp_entry(s, 1)) return tcp_name(s, a, l, 1);
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
int WINAPI x_listen(SOCKET s, int b)
{
    if (!okw_on) return listen(s, b);
    if (!tcp_entry(s, 0)) {
        /* no entry: the game's own TCP/IP was chosen before this listen (the box came at bind and dropped the
         * entries, or okplay.ini wsock_mode=original) - okplay's Join box text still has to come out */
        if (mode_choice == 1 && game->at_listen) game->at_listen(0);
        return listen(s, b);
    }
    if (tcp_mode_pass()) {
        if (game->at_listen) game->at_listen(0);   /* (nfs3_fix.c: okplay's Join box text out again) */
        int lr = listen(s, b);
        tr("game listen %u backlog %d -> %d (Windows' own TCP)", (unsigned)s, b, lr ? WSAGetLastError() : 0);
        return lr;
    }
    retry_start();
    if (game->at_listen) game->at_listen(1);   /* (nfs3_fix.c: the Join box pre-filled with "oldkid.net") */
    int r = 0, host = 0;
    uint16_t port = 0;
    EnterCriticalSection(&okw_lock);
    okw_sock *k = tsock_of(s);
    if (!k) r = set_err(WSAENOTSOCK);
    else if (k->c) r = set_err(k->c->state == T_ESTAB ? WSAEISCONN : WSAEINVAL);
    else {
        if (k->tstate == T_NEW) { k->port = eph_port(); k->tstate = T_BOUND; }   /* (Windows: WSAEINVAL; the games bind first) */
        k->tstate = T_LISTEN;
        k->backlog_max = b < 1 ? 1 : b > OKW_BACKLOG ? OKW_BACKLOG : b;
        port = k->port;
        /* The host rule (seen in a capture of Need for Speed III 1.6.1): both sides listen on the game's port as soon
         * as the player opens the TCP screen - that alone is "ready", not hosting. Host Game closes that listener,
         * restarts Winsock and listens on the same port again within about 100 ms; a joiner keeps its listener and
         * connects from another socket. So a listen on a port whose listener this game closed less than 2 s ago
         * hosts. (Port 1030, the original 1998 exe's, follows the same rule: no capture of it yet - if its Host Game
         * doesn't re-listen, it won't host, which fails visibly instead of listing every joiner.)
         * The game makes the same calls when the player enters the TCP menu again with its listener still open, so this
         * PC is only a candidate host: when the game connects out, tcp_unhost_for_connect takes the role back. A PC
         * with a connect of its own going on or up is joining, not hosting; a join left over from before (no such
         * connect) is taken back here.
         * That rule is the fallback: on a build whose game state is known (game->says_hosts, nfs3_fix.c) the game's
         * own word decides instead - Host Game sets it before it listens, a menu re-entry leaves it as it was - so a
         * re-entry lists no room and asks nothing. */
        int again = k->explicit_port && relisten_port == port && GetTickCount() - relisten_tick < 2000;
        int listening = 0;
        for (int i = 0; i < OKW_MAX_SOCKS; i++)
            if (&socks[i] != k && socks[i].used && socks[i].kind == K_TCP && socks[i].tstate == T_LISTEN && socks[i].explicit_port) listening = 1;
        int gnet = 0, gsrv = 0;
        int said = k->explicit_port && game->says_hosts ? game->says_hosts(&gnet, &gsrv) : -1;
        int may = game->host_rule == HOST_LISTEN && k->explicit_port && ctx && !tcp_outbound();
        if (said >= 0) host = may && said;
        else host = may && (again || (ctx->is_host && listening));
        if (again && ctx) relisten_port = 0;
        if (host && ctx->host_peer) tcp_join_undo(ctx->host_peer, "the game hosts", 0);
        if (said >= 0) {
            int gtype = -1;
            if (game->max_players) game->max_players(&gtype);
            gs_net = gnet;
            gs_srv = gsrv;
            gs_type = gtype;
            okp_log(2, "wsock: TCP socket %u listens on port %u - game state: net %d, server %d - %s (game type %d)", (unsigned)s,
                    port, gnet, gsrv, host ? "Host Game: this PC hosts" : said ? "Host Game, but okplay is not running here or "
                    "a connect of the game's is going on - not hosting" : "ready, not hosting", gtype);
        } else
            okp_log(2, "wsock: TCP socket %u listens on port %u%s%s", (unsigned)s, port,
                    host ? " - this PC hosts (the game listens again: Host Game)" : k->explicit_port ? " - ready, not hosting" : "",
                    k->explicit_port && game->says_hosts ? " (build not known: the listen-again rule)" : "");
        tr("game listen %u port %u backlog %d%s", (unsigned)s, port, b, host ? " - hosts" : "");
        if (host && ctx->is_host) { build_room_record(); host = 0; }
    }
    LeaveCriticalSection(&okw_lock);
    if (host) {
        send_owed_close();   /* (the last room's close, with its id, before this one gets a new id) */
        ask_host_password();
        EnterCriticalSection(&okw_lock);
        host_room();
        LeaveCriticalSection(&okw_lock);
    }
    ui_show_pending();
    return r;
}
int WINAPI x_shutdown(SOCKET s, int h)
{
    if (okw_on && tcp_entry(s, 1)) return tcp_shutdown(s, h);
    return shutdown(s, h);
}
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
    if (!cbt_hook && (game->kinds & OKW_UDP)) cbt_hook = SetWindowsHookExA(WH_CBT, cbt_proc, NULL, GetCurrentThreadId());
    int r = WSAStartup(v, d);
    tr("game WSAStartup version %u.%u -> %d", LOBYTE(v), HIBYTE(v), r);
    return r;
}
/* A TCP game done with the network may end its process next. Windows would go on sending what the game closed
 * gracefully (its last message, the FIN) after that; here it is okplay's reliable lane in this process, so it gets
 * up to 3 s to have everything acked (seen: a joiner's last frames lost on a lossy line, then the process gone). */
static void tcp_linger(void)   /* no lock held */
{
    DWORD t0 = GetTickCount();
    int busy;
    if (!(game->kinds & OKW_TCP)) return;
    do {
        uint64_t ids[OKP_MAX_PEERS];
        int n = 0;
        busy = 0;
        EnterCriticalSection(&okw_lock);
        if (ctx) {   /* only players this game has (or just had) stream connections with */
            for (int i = 0; i < OKW_MAX_CONNS && n < OKP_MAX_PEERS; i++) if (conns[i].used && conns[i].state == T_ESTAB) ids[n++] = conns[i].peer;
            for (int i = 0; i < nclosed_peers && n < OKP_MAX_PEERS; i++) if (t0 - closed_peers[i].tick < 5000) ids[n++] = closed_peers[i].peer;
            for (int i = 0; i < n; i++) {
                okp_peer_info_t pi;
                if (okp_peer_info(ctx, ids[i], &pi) && pi.have_path && (pi.unacked_bytes || pi.waiting_bytes)) busy = 1;
            }
        }
        LeaveCriticalSection(&okw_lock);
        if (busy) Sleep(20);
    } while (busy && GetTickCount() - t0 < 3000);
    if (GetTickCount() - t0 >= 20)
        okp_log(2, "wsock: WSACleanup waited %lu ms for the last stream frames to be acked%s",
                (unsigned long)(GetTickCount() - t0), busy ? " (some still are not)" : "");
}

/* the game is done with the network: a room it hosted comes off the list now (the process may end next) */
int WINAPI x_WSACleanup(void)
{
    if (!okw_on) return WSACleanup();
    DWORD t0 = GetTickCount();
    tcp_linger();
    okp_log(2, "wsock: the game calls WSACleanup (it is done with the network)");
    int withdraw = 0;
    EnterCriticalSection(&okw_lock);
    if (ctx && (ctx->is_host || ctx->closing) && ctx->room && ctx->mode == OKP_MODE_ONLINE) {
        withdraw = 1;
        ctx->is_host = 0;
        ctx->session_open = 0;
        ctx->closing = 1;
        close_owed = 0;   /* (that room's close goes from here) */
    }
    okp_ctx *wc = withdraw ? ctx : NULL;
    LeaveCriticalSection(&okw_lock);
    if (wc) okp_rdv_close(wc);
    EnterCriticalSection(&okw_lock);
    if (relisten_port) relisten_tick += GetTickCount() - t0;   /* okplay's own wait is not the game's: out of the 2 s */
    LeaveCriticalSection(&okw_lock);
    int r = WSACleanup();
    okp_log(2, "wsock: WSACleanup done (%d)", r);
    tr("game WSACleanup -> %d", r);
    return r;
}
int WINAPI x___WSAFDIsSet(SOCKET s, fd_set *f) { return __WSAFDIsSet(s, f); }

/* ---------------------------------------------------------------- Connection Check (okplay/src/testchan.c)
 *
 * The test channel (OkplayTest*) and OkplayPaths are the core's, the same as the drop-in's. They work in any
 * program that loads this DLL, not only a game on the list: the channel has an okplay of its own and never
 * goes through the virtual sockets above. In a program that isn't a game here (okw_on 0: Connection Check),
 * DllMain set nothing up, so the first call does what the core needs - logs (logs\), Winsock - and nothing
 * else: no box, no hook. */
int okp_tc_dll_ready(void)
{
    static int done;
    if (okw_on || done) return 1;
    done = 1;
    okp_sys_init();
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    okp_log_init(g_self);
    okp_log_header();
    okp_log(1, "wsock: okplay " OKP_VERSION_STR " loaded by a program that is not a game here - only its test channel runs");
    return 1;
}

/* how this PC reaches each player (net.c okp_paths_text): the test game's players while one runs, else the game's */
int WINAPI OkplayPaths(char *out, int outsz)
{
    okp_ctx *c = okp_tc_ctx();
    if (c || !okw_on) return okp_paths_text(c, out, outsz);
    EnterCriticalSection(&okw_lock);   /* (okw_lock, then ctx->lock: the order everywhere here) */
    int n = okp_paths_text(ctx, out, outsz);
    LeaveCriticalSection(&okw_lock);
    return n;
}

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
        EnterCriticalSection(&okw_lock);
        tcp_bye();
        LeaveCriticalSection(&okw_lock);
        okp_log(0, "exit watch: the game's window closed %d s ago but the game did not end - %d timer(s) stopped, "
                   "ending it (okplay.ini exit_watch=0 turns this off)", gone, k);
        DWORD id;
        CloseHandle(CreateThread(NULL, 0, ew_backstop, NULL, 0, &id));
        ExitProcess(0);
    }
    return 0;
}

/* the game ends its process: its open TCP connections get their reset first (tcp_bye) */
static VOID (WINAPI *real_ExitProcess)(UINT);
static VOID WINAPI hook_ExitProcess(UINT code)
{
    EnterCriticalSection(&okw_lock);
    tcp_bye();
    LeaveCriticalSection(&okw_lock);
    real_ExitProcess(code);
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
        okp_exe_build_early();
#ifdef OKW_LAB
        rr_lab_init();
#endif
        InitializeCriticalSection(&okw_ask_lock);
        InitializeCriticalSection(&ui_lock);
        box_done = CreateEventA(NULL, TRUE, TRUE, NULL);
        okw_ev = CreateEventA(NULL, TRUE, FALSE, NULL);
        {
            static char ini[MAX_PATH];
            snprintf(ini, sizeof(ini), "%sokplay.ini", okp_dll_dir());
            okbox_init(inst, NULL, ini);
            okbox_set_topmost(TRUE);
            if (game->kinds & OKW_TCP) dds_init();   /* okplay's boxes over a full-screen DirectDraw game (ddshow.c) */
            exit_watch_start(ini);
            if ((game->kinds & OKW_TCP) && !hook_exe_import("KERNEL32.dll", "ExitProcess", (void *)hook_ExitProcess, (void **)&real_ExitProcess))
                okp_log(2, "wsock: the game's ExitProcess is not seen - no resets for open connections when it ends");
            if (game->fix_init) game->fix_init(ini);   /* rr_fix.c: Road Rash's netcode fixes; nfs3_fix.c: NFS3's game state */
            int win = GetPrivateProfileIntA("okplay", "wsock_tcp_window", 65536, ini);
            tcp_window = win < 4096 ? 4096 : win > (1 << 20) ? (1 << 20) : (uint32_t)win;
            {   /* wsock_max_players: the players a hosted room takes, host included (2..64; else the game's own word, or 8) */
                int mp = GetPrivateProfileIntA("okplay", "wsock_max_players", 0, ini);
                max_players_ini = mp >= 2 && mp <= 64 ? mp : 0;
                if (max_players_ini) okp_log(1, "wsock: okplay.ini wsock_max_players=%d - a hosted room takes that many players", max_players_ini);
                else if (mp) okp_log(1, "wsock: okplay.ini wsock_max_players=%d is not 2..64 - not used", mp);
            }
            {
                char v[16] = "";
                GetPrivateProfileStringA("okplay", "wsock_join", "", v, sizeof(v), ini);
                GetEnvironmentVariableA("OKP_WSOCK_JOIN", v, sizeof(v));
                join_auto = !_stricmp(v, "auto");
                DWORD nl = sizeof(player_name);
                GetPrivateProfileStringA("okplay", "name", "", player_name, sizeof(player_name), ini);
                player_name_set = player_name[0] != 0;
                if (!player_name[0] && !GetComputerNameA(player_name, &nl)) player_name[0] = 0;
            }
            okp_log(2, "wsock: game profile %s (%s%s%s)", game->id, game->kinds & OKW_UDP ? "UDP" : "",
                    game->kinds == (OKW_UDP | OKW_TCP) ? " + " : "", game->kinds & OKW_TCP ? "TCP" : "");
            InitializeCriticalSection(&tr_lock);
            tr_on = GetPrivateProfileIntA("okplay", "wsock_trace", 0, ini);
            if (tr_on) { DWORD id; CloseHandle(CreateThread(NULL, 0, tr_writer, NULL, 0, &id)); }
        }
        DisableThreadLibraryCalls(inst);
    } else if (why == DLL_PROCESS_DETACH && okw_on && !res && (game->kinds & OKW_TCP)) {
        dds_detach();   /* unloaded while the game goes on: its windows must not keep calling into this DLL */
    }
    return TRUE;
}
