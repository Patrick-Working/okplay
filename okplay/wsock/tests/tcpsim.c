/* tcpsim: a stand-in for a game that plays over TCP the way Need for Speed III does, to test okplay wsock's
 * stream sockets without the game. Plain Winsock 1.1 (wsock32), Windows 95 and later.
 *
 *   tcpsim host [--port 9803] [--players N] [--frames F] [--hz 30] [--secs S] [--edge NAME]
 *               [--claim-delay MS] [--die-at FRAME --die soft|hard] [--menu-ms MS]
 *   tcpsim join ADDR[:PORT] [--orig] [--frames F] [--hz 30] [--secs S] [--edge NAME] [--expect-loss MS]
 *               [--menu-port P] [--menu-ms MS]
 * As the game (a capture of Need for Speed III 1.6.1): both sides listen on the game's port as soon as the TCP screen
 *   opens (the "menu"); Host Game stops the comms thread, closes that listener, restarts Winsock (WSACleanup +
 *   WSAStartup), makes a probe socket and listens again on the same port; a joiner keeps its menu listener and
 *   connects from new bind(0) sockets. --menu-ms: time at the menu (500 ms). --menu-port: the joiner's menu
 *   listener (9803; --orig 1030; on one PC with Windows' own TCP both can't have 9803, so tests give 0 there).
 * --claim-delay: the host leaves each accepted socket unclaimed that long (the game parks it, up to 4 s, and doesn't
 *   poll its listen socket meanwhile). --die-at: the host process ends at that frame without closing anything (soft:
 *   ExitProcess, hard: TerminateProcess). --expect-loss: the joiner expects its connection to break mid-race, within MS
 *   of the last frame it got (RESULT ok then).
 *   tcpsim selftest
 *   tcpsim probe      (socket + closesocket only, and the own-address lookup: through okplay no box may come)
 *   tcpsim script [--port 9803] [--menu-port P] [--secs S] STEP...
 *               what a player does, one step after the other, in one run of the game (see "scripts" below)
 *
 * The patterns are the game's (described in okplay's docs, not copied code):
 *  - a probe socket that is only created and closed;
 *  - host: socket / bind(port) / listen(5) on a blocking socket, and a comms thread that is the only place that
 *    selects (25 ms), accepts and receives: read set = listen socket (not while an accepted socket waits to be
 *    claimed) + connections, write set = connecting socket + connections with unsent output, except set =
 *    connecting socket + every connection;
 *  - every data socket: FIONBIO, SO_KEEPALIVE, TCP_NODELAY;
 *  - messages framed with a 2-byte big-endian length; receive = recv(2, MSG_PEEK), recv(len+2, MSG_PEEK) (fewer
 *    bytes: wait for the next select), then recv(len+2) to consume; max message 1024 bytes;
 *  - send = one send() per message; partial: the rest stays pending and goes when select says writable;
 *    WSAEWOULDBLOCK/WSAENOBUFS: message dropped, "busy", retried up to 9 times with Sleep(50*i+10), then the
 *    link is dead;
 *  - join: bind(0)+getsockname+listen(5) first (--orig: bind(1030)), then the connect loop: a new socket per
 *    attempt, bind(0), FIONBIO etc., connect must give WSAEWOULDBLOCK (anything else: fatal or retry classes),
 *    completion through the comms thread's write/except sets, 1 s per attempt;
 *  - close = shutdown(SD_BOTH), Sleep(1), closesocket.
 * Traffic: version 0x29 B, game info 0x7c B, race-start burst 10 x 209 + 113 B, car records 196 B per player,
 * a barrier (4 B), then F frames of lockstep: each joiner sends [6][lead][4-byte input] per frame, the host
 * sends [6][8 x 4-byte inputs] (33 B) to everyone once a frame is complete; inputs carry (player, frame), so
 * order, loss and duplicates are checked exactly. The leaving side sends a 4-byte leave message, the other must
 * read it before EOF.
 *
 * Edge cases (host --edge NAME and join ADDR --edge NAME; both sides know the scenario):
 *   refuse     join connects to PORT+1 where nothing listens: must never connect (how it ends is reported)
 *   peek       host sends messages cut at odd places; join peeks 1, 2, len+2 bytes across the cuts
 *   halfclose  join sends 10 KB then shutdown(SD_SEND); host reads 10 KB then EOF, answers the count, closes
 *   rstunread  host sends 100 B; join closes without reading; host's recv must fail with WSAECONNRESET
 *   window     host doesn't read for 3 s; join sends until WSAEWOULDBLOCK, resumes when writable, all arrives
 * Output: EVENT lines, STAT lines, RESULT ok | RESULT fail <reason>.
 *
 * TCPSIM_GAMESTATE=modern|cd: the game's own two values (okplay's nfs3_fix.c reads them on the builds it knows; run
 * with OKP_TEST_EXEBUILD set to such a build) at that build's addresses, set as the game sets them: Host Game sets
 * net = 3 and server = 1 before it restarts the service and listens; a join sets server = 0 and net = 3 before it
 * connects; entering the TCP menu leaves them; leaving a lobby, a failed join and leaving network play reset them
 * (net 0, server 1). TCPSIM_GAMETYPE: the game type the game keeps after the network state (0 Peer-to-Peer,
 * 1 Client-Server; default 1). The exe must sit at 0x400000 (build.sh links tcpsim without ASLR); a large zeroed block makes
 * its image reach those addresses, as the game's does. */
#define WIN32_LEAN_AND_MEAN
#include <winsock.h>
#include <mmsystem.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SD_RECEIVE
#define SD_RECEIVE 0
#define SD_SEND 1
#define SD_BOTH 2
#endif
#define MAXMSG 1024
#define MAXP 8
#define LEAD 8          /* frames a player may run ahead of the last complete frame */

typedef struct {
    SOCKET s;
    int used, player, connected;
    unsigned char out[MAXMSG + 2];
    int pend, pend_off;               /* bytes of out still to send (the game's out+6) */
    int got_version, got_car, got_barrier, got_leave, closed;
    unsigned next_in;                 /* host: next frame expected from this player */
} conn;

static CRITICAL_SECTION comms;        /* the game's comms lock: every send/recv and all game state below */
static conn conns[MAXP];
static SOCKET lsock = INVALID_SOCKET;
static SOCKET parked = INVALID_SOCKET;   /* accepted, waiting to be claimed */
static DWORD parked_deadline;
static conn *volatile connecting;              /* the connection being connected (0 = none) */
static volatile LONG stop_flag, comms_running;
static int is_host, nplayers = 2, frames = 300, hz = 30, secs = 60, max_frames;
static char fail_reason[200];
static volatile LONG failed;
static unsigned n_wouldblock, n_partial, n_partial_peek, n_dead, n_busy_retries;
static int claim_delay, die_at = -1, die_hard, expect_loss, lost_err;
static unsigned n_accepted;
static int menu_ms = 500, menu_port = -1;
static volatile DWORD *gs_net;           /* TCPSIM_GAMESTATE: the game's network state (NULL = not emulated) */
static volatile WORD *gs_srv;            /* ... and "this PC is the server" */
static char *gs_join;                    /* ... and the Join box's address, typed last (the box opens with it) */
/* (the game's values sit in its exe's image: this zeroed block stretches tcpsim's image over both builds' addresses) */
static char gs_cover[0x600000];

static void gs_set(int net, int srv)
{
    if (!gs_net) return;
    *gs_srv = (WORD)srv;
    *gs_net = (DWORD)net;
    printf("EVENT game state: net %d, server %d\n", net, srv);
}

static int gs_init(void)   /* 0 = TCPSIM_GAMESTATE asked for, but the addresses could not be had */
{
    char v[16] = "";
    DWORD net, srv;
    if (!GetEnvironmentVariableA("TCPSIM_GAMESTATE", v, sizeof(v)) || !v[0]) return 1;
    DWORD join;
    if (!strcmp(v, "modern")) { net = 0x7a22e0; srv = 0x7a22da; join = 0x6fbb7e; }
    else if (!strcmp(v, "cd")) { net = 0x7c92e0; srv = 0x7c92da; join = 0x722b7e; }
    else { printf("EVENT TCPSIM_GAMESTATE=%s: not modern or cd\n", v); return 0; }
    if ((DWORD)(UINT_PTR)GetModuleHandleA(NULL) != 0x400000) { printf("EVENT game state: the exe is not at 0x400000\n"); return 0; }
    if ((UINT_PTR)gs_cover > net || (UINT_PTR)gs_cover + sizeof(gs_cover) < net + 4) {
        printf("EVENT game state: %08lx is not in this exe's image\n", (unsigned long)net);
        return 0;
    }
    gs_net = (volatile DWORD *)(UINT_PTR)net;
    gs_srv = (volatile WORD *)(UINT_PTR)srv;
    gs_join = (char *)(UINT_PTR)join;
    GetEnvironmentVariableA("TCPSIM_JOINTEXT", gs_join, 30);   /* what the game remembered from its last run (empty without) */
    {   /* the game type after the network state (net + 4): TCPSIM_GAMETYPE, else 1 (Client-Server, up to 8 players) */
        char t[16] = "1";
        GetEnvironmentVariableA("TCPSIM_GAMETYPE", t, sizeof(t));
        *(volatile DWORD *)(UINT_PTR)(net + 4) = (DWORD)atoi(t);
        printf("EVENT game type %d\n", atoi(t));
    }
    gs_set(0, 1);   /* (the game's reset at start) */
    return 1;
}
static DWORD parked_at, t_lost;
static int script_mode;               /* a broken connection is an event, not a failure (the steps decide) */
static int got_info, pongs;           /* joiner: the host's game info came (in its lobby); ping answers */

/* game state (under comms) */
static int my_player, race_on, burst_sent;
static unsigned inputs[MAXP][LEAD * 4];  /* host: inputs by frame % (LEAD*4) */
static unsigned char have[MAXP][LEAD * 4];
static unsigned bundles;              /* host: bundles sent; join: bundles received */
static unsigned sent_frames;          /* own inputs made */
static DWORD *t_sent, *rtts;
static DWORD last_bundle_ms;
static unsigned stalls;
static int bulk_seen, cars_seen, barrier_seen;

static void fail(const char *fmt, ...)
{
    va_list ap;
    if (InterlockedExchange(&failed, 1)) return;
    va_start(ap, fmt);
    _vsnprintf(fail_reason, sizeof(fail_reason) - 1, fmt, ap);
    va_end(ap);
    printf("EVENT failure: %s\n", fail_reason);
}

static DWORD now_ms(void) { return timeGetTime(); }

static void data_opts(SOCKET s)   /* every data socket, as the game's option helper */
{
    u_long on = 1;
    int one = 1;
    ioctlsocket(s, FIONBIO, &on);
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (char *)&one, sizeof(one));
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof(one));
}

static SOCKET tcp_bound(unsigned short port)   /* socket + bind, as the game makes every TCP socket */
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    if (s == INVALID_SOCKET) return s;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&a, sizeof(a))) {
        printf("EVENT bind port %u failed: %d\n", port, WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

/* ------------------------------------------------------------------ the send path */

static int msg_send(conn *c, const void *m, int len)   /* 1 = sent (or looks sent), 0 = busy */
{
    int r = 1, n;
    EnterCriticalSection(&comms);
    if (!c->used || c->s == INVALID_SOCKET) { LeaveCriticalSection(&comms); return 1; }
    if (c->pend > 0) { LeaveCriticalSection(&comms); return 0; }
    c->out[0] = (unsigned char)(len >> 8);
    c->out[1] = (unsigned char)len;
    memcpy(c->out + 2, m, len);
    n = send(c->s, (char *)c->out, len + 2, 0);
    if (n > 0) {
        if (n < len + 2) { c->pend_off = n; c->pend = len + 2 - n; n_partial++; }
    } else if (n == SOCKET_ERROR && (WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAENOBUFS)) {
        c->pend = 0;   /* dropped here: the caller retries */
        n_wouldblock++;
        r = 0;
    }
    LeaveCriticalSection(&comms);
    return r;
}

static void send_retry(conn *c, const void *m, int len)
{
    for (int i = 0; i < 10; i++) {
        if (msg_send(c, m, len)) return;
        if (i == 9) break;
        n_busy_retries++;
        Sleep(50 * i + 10);
    }
    n_dead++;
    fail("link to player %d dead (send busy 10 times)", c->player);
}

static int flush(conn *c)   /* writable: 1 = error */
{
    int n = send(c->s, (char *)c->out + c->pend_off, c->pend, 0);
    if (n == SOCKET_ERROR) return WSAGetLastError() != WSAEWOULDBLOCK && WSAGetLastError() != WSAENOBUFS;
    c->pend_off += n;
    c->pend -= n;
    return 0;
}

/* ------------------------------------------------------------------ the messages */

static void put32(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v; }
static unsigned get32(const unsigned char *p) { return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3]; }

static void fill(unsigned char *b, int len, int seed)
{
    for (int i = 1; i < len; i++) b[i] = (unsigned char)(seed * 31 + i * 7);
}
static int check_fill(const unsigned char *b, int len, int seed)
{
    for (int i = 1; i < len; i++) if (b[i] != (unsigned char)(seed * 31 + i * 7)) return 0;
    return 1;
}

static void send_bundles(void)   /* host, comms held: every complete frame goes to everyone, in order */
{
    for (;;) {
        unsigned f = bundles, slot = f % (LEAD * 4);
        if ((int)f >= frames) return;
        for (int p = 0; p < nplayers; p++) if (!have[p][slot]) return;
        unsigned char b[33];
        memset(b, 0, sizeof(b));
        b[0] = 6;
        for (int p = 0; p < nplayers; p++) { put32(b + 1 + 4 * p, inputs[p][slot]); have[p][slot] = 0; }
        bundles++;
        for (int i = 0; i < MAXP; i++) if (conns[i].used && conns[i].connected && !conns[i].closed) send_retry(&conns[i], b, 33);
    }
}

static void host_msg(conn *c, const unsigned char *m, int len)   /* comms thread, comms held */
{
    unsigned char b[0x7c];
    switch (m[0]) {
    case 0:
        if (len != 0x29 || memcmp(m + 1, "27, 2.0", 7)) { fail("bad version message (%d B)", len); return; }
        c->got_version = 1;
        memset(b, 0, sizeof(b));
        b[0] = 1;
        b[1] = (unsigned char)c->player;
        b[2] = (unsigned char)nplayers;
        put32(b + 3, (unsigned)frames);
        fill(b + 7, 0x7c - 7, 1);
        send_retry(c, b, 0x7c);
        break;
    case 0x0c:
        if (len != 196 || m[1] != c->player || !check_fill(m + 1, 195, 100 + c->player)) { fail("bad car record from player %d", c->player); return; }
        c->got_car = 1;
        break;
    case 4:
        if (len != 4) { fail("bad barrier/leave length %d", len); return; }
        if (m[2] == 0 && m[3] == 0x0c) { c->got_leave = 1; printf("EVENT player %d leaves\n", c->player); }
        else if (m[3] == 0x5a) c->got_barrier = 1;
        break;
    case 6: {
        if (len != 6) { fail("bad input length %d", len); return; }
        unsigned in = get32(m + 2), f = in & 0xffffff;
        if ((int)(in >> 24) != c->player) { fail("input from player %d says player %u", c->player, in >> 24); return; }
        if (f != c->next_in) { fail("player %d: input for frame %u, expected %u (%s)", c->player, f, c->next_in, f < c->next_in ? "duplicate" : "gap"); return; }
        if (f >= bundles + LEAD * 4) { fail("player %d ran %u frames ahead", c->player, f - bundles); return; }
        c->next_in++;
        inputs[c->player][f % (LEAD * 4)] = in;
        have[c->player][f % (LEAD * 4)] = 1;
        send_bundles();
        break;
    }
    case 9:   /* scripts: a ping, answered at once */
        if (len != 4) { fail("bad ping length %d", len); return; }
        send_retry(c, m, 4);
        break;
    default:
        fail("unknown message type %d", m[0]);
    }
}

static void join_msg(conn *c, const unsigned char *m, int len)   /* comms thread, comms held */
{
    unsigned char b[196];
    switch (m[0]) {
    case 1:
        if (len != 0x7c || !check_fill(m + 7, 0x7c - 7, 1)) { fail("bad game info"); return; }
        my_player = m[1];
        nplayers = m[2];
        frames = (int)get32(m + 3);
        printf("EVENT game info: I am player %d of %d, %d frames\n", my_player, nplayers, frames);
        got_info = 1;
        if (frames > max_frames) { fail("the host wants %d frames (at most %d here)", frames, max_frames); return; }
        b[0] = 0x0c;
        b[1] = (unsigned char)my_player;
        fill(b + 1, 195, 100 + my_player);
        send_retry(c, b, 196);
        break;
    case 5:
        if ((bulk_seen < 10 && len != 209) || (bulk_seen == 10 && len != 113) || bulk_seen > 10 || !check_fill(m, len, 50 + bulk_seen)) {
            fail("bad bulk message %d (%d B)", bulk_seen, len);
            return;
        }
        bulk_seen++;
        break;
    case 0x0c:
        if (len != 196 || !check_fill(m + 1, 195, 100 + m[1])) { fail("bad car record for player %d", m[1]); return; }
        cars_seen++;
        break;
    case 4:
        if (len != 4 || m[3] != 0x5a) { fail("bad barrier"); return; }
        if (bulk_seen != 11 || cars_seen != nplayers - 1) { fail("barrier before the start burst (bulk %d, cars %d)", bulk_seen, cars_seen); return; }
        barrier_seen = 1;
        b[0] = 4; b[1] = (unsigned char)my_player; b[2] = 0; b[3] = 0x5a;
        send_retry(c, b, 4);
        race_on = 1;
        last_bundle_ms = now_ms();
        printf("EVENT race starts\n");
        break;
    case 6: {
        unsigned f = bundles;
        DWORD t = now_ms();
        if (len != 33) { fail("bad bundle length %d", len); return; }
        for (int p = 0; p < nplayers; p++) {
            unsigned in = get32(m + 1 + 4 * p);
            if (in != ((unsigned)p << 24 | f)) { fail("bundle %u: player %d's input is %08x", f, p, in); return; }
        }
        if (f >= sent_frames) { fail("bundle %u before our input for it", f); return; }
        rtts[f] = t - t_sent[f];
        if (f && t - last_bundle_ms > 1000u / hz + 100) stalls++;
        last_bundle_ms = t;
        bundles++;
        break;
    }
    case 9:
        if (len != 4) { fail("bad ping answer"); return; }
        pongs++;
        break;
    default:
        fail("unknown message type %d", m[0]);
    }
}

static int receive(conn *c)   /* 1 = error (the connection goes) */
{
    unsigned char b[0x424];
    int n = recv(c->s, (char *)b, 2, MSG_PEEK);
    if (n == 0) return 1;
    if (n == SOCKET_ERROR) return WSAGetLastError() != WSAEWOULDBLOCK;
    if (n < 2) return 0;
    int len = b[0] << 8 | b[1];
    if (len > MAXMSG || len < 1) return 1;
    n = recv(c->s, (char *)b, len + 2, MSG_PEEK);
    if (n == SOCKET_ERROR) return WSAGetLastError() != WSAEWOULDBLOCK;
    if (n < len + 2) { n_partial_peek++; return 0; }
    if (is_host) host_msg(c, b + 2, len); else join_msg(c, b + 2, len);
    n = recv(c->s, (char *)b, len + 2, 0);
    if (n == 0) return 1;
    if (n == SOCKET_ERROR) return WSAGetLastError() != WSAEWOULDBLOCK;
    return 0;
}

static void conn_error(conn *c)   /* comms thread: closesocket, out of the list */
{
    if (script_mode) printf("EVENT connection %d closed (%s, error %d)\n", c->player,
                            c->got_leave ? "after its leave message" : "no leave message", WSAGetLastError());
    else if (!is_host && expect_loss && !t_lost) { t_lost = now_ms(); lost_err = WSAGetLastError(); }
    else if (!c->got_leave && !failed) {
        if (is_host) fail("player %d's connection broke before it left (error %d)", c->player, WSAGetLastError());
        else if ((int)bundles < frames) fail("the host's connection broke (error %d)", WSAGetLastError());
    }
    closesocket(c->s);
    c->s = INVALID_SOCKET;
    c->closed = 1;
}

/* ------------------------------------------------------------------ the comms thread */

#define ADD(set, sock) do { if ((set).fd_count < FD_SETSIZE) (set).fd_array[(set).fd_count++] = (sock); } while (0)

static DWORD WINAPI comms_thread(LPVOID arg)
{
    (void)arg;
    InterlockedExchange(&comms_running, 1);
    while (!stop_flag) {
        fd_set r, w, e;
        r.fd_count = w.fd_count = e.fd_count = 0;
        if (parked != INVALID_SOCKET) {
            if ((int)(now_ms() - parked_deadline) >= 0) {
                printf("EVENT accepted socket not claimed in 4 s - closed\n");
                closesocket(parked);
                parked = INVALID_SOCKET;
            } else {
                Sleep(1);
            }
        }
        EnterCriticalSection(&comms);
        if (parked == INVALID_SOCKET && lsock != INVALID_SOCKET) ADD(r, lsock);
        for (int i = 0; i < MAXP; i++) {
            conn *c = &conns[i];
            if (!c->used || c->s == INVALID_SOCKET || c == connecting || !c->connected) continue;
            ADD(r, c->s);
            if (c->pend > 0) ADD(w, c->s);
            ADD(e, c->s);
        }
        conn *cc = connecting;
        SOCKET cs = cc ? cc->s : INVALID_SOCKET;
        if (cc) { ADD(w, cs); ADD(e, cs); }
        LeaveCriticalSection(&comms);
        struct timeval tv = { 0, 25000 };
        int n = select(0, &r, &w, &e, &tv);
        if (n == SOCKET_ERROR) {
            if (parked == INVALID_SOCKET && !r.fd_count) Sleep(1);
            continue;
        }
        if (lsock != INVALID_SOCKET && FD_ISSET(lsock, &r)) {
            struct sockaddr_in a;
            int al = sizeof(a);
            SOCKET s = accept(lsock, (struct sockaddr *)&a, &al);
            if (s == INVALID_SOCKET) {   /* the game's comms thread ends here: the whole service is dead */
                fail("accept failed after select said readable (%d)", WSAGetLastError());
                break;
            }
            printf("EVENT accepted a connection from %s:%u\n", inet_ntoa(a.sin_addr), ntohs(a.sin_port));
            data_opts(s);
            parked = s;
            parked_at = now_ms();
            parked_deadline = parked_at + 4000;
            n_accepted++;
        }
        EnterCriticalSection(&comms);
        if (cc && connecting == cc) {
            if (FD_ISSET(cs, &e)) { cc->connected = 0; connecting = NULL; }
            else if (FD_ISSET(cs, &w)) { cc->connected = 1; connecting = NULL; }
        }
        for (int i = 0; i < MAXP; i++) {
            conn *c = &conns[i];
            if (!c->used || c->s == INVALID_SOCKET || c == connecting || !c->connected || c == cc) continue;
            int err = 0;
            if (FD_ISSET(c->s, &r)) err = receive(c);
            if (!err && c->s != INVALID_SOCKET && FD_ISSET(c->s, &w) && c->pend > 0) err = flush(c);
            if (!err && c->s != INVALID_SOCKET && FD_ISSET(c->s, &e)) err = 1;
            if (err) conn_error(c);
        }
        LeaveCriticalSection(&comms);
    }
    InterlockedExchange(&comms_running, 0);
    return 0;
}

static void start_comms(void)
{
    DWORD id;
    InterlockedExchange(&stop_flag, 0);
    HANDLE t = CreateThread(NULL, 0, comms_thread, NULL, 0, &id);
    SetThreadPriority(t, THREAD_PRIORITY_HIGHEST);
    CloseHandle(t);
    while (!comms_running) Sleep(1);
}

static void stop_comms(void)
{
    InterlockedExchange(&stop_flag, 1);
    while (comms_running) Sleep(1);
}

static void close_conn(conn *c)   /* the game's connection close */
{
    SOCKET s;
    EnterCriticalSection(&comms);
    s = c->s;
    c->s = INVALID_SOCKET;
    c->closed = 1;
    LeaveCriticalSection(&comms);
    if (s == INVALID_SOCKET) return;
    shutdown(s, SD_BOTH);
    Sleep(1);
    closesocket(s);
}

/* ------------------------------------------------------------------ stats */

static int cmp_dw(const void *a, const void *b) { DWORD x = *(const DWORD *)a, y = *(const DWORD *)b; return x < y ? -1 : x > y; }

static void stats(void)
{
    unsigned n = bundles;
    DWORD p50 = 0, p99 = 0, mx = 0;
    if (!is_host && n) {
        DWORD *s = (DWORD *)malloc(n * sizeof(DWORD));
        memcpy(s, rtts, n * sizeof(DWORD));
        qsort(s, n, sizeof(DWORD), cmp_dw);
        p50 = s[n / 2];
        p99 = s[(n * 99) / 100 < n ? (n * 99) / 100 : n - 1];
        mx = s[n - 1];
        free(s);
    }
    printf("STAT frames %u, rtt p50/p99/max %lu/%lu/%lu ms, stalls >100 ms %u, wouldblock %u, partial %u, partial peeks %u, busy retries %u\n",
           n, (unsigned long)p50, (unsigned long)p99, (unsigned long)mx, stalls, n_wouldblock, n_partial, n_partial_peek, n_busy_retries);
}

/* ------------------------------------------------------------------ host / join */

/* the menu: the game's port listened on; then Host Game: closed, Winsock restarted, a probe, listened on again */
static SOCKET menu_then_host(unsigned short port, int comms)
{
    SOCKET l = tcp_bound(port);
    if (l == INVALID_SOCKET || listen(l, 5)) { printf("EVENT menu listen on %u failed (%d)\n", port, WSAGetLastError()); return INVALID_SOCKET; }
    printf("EVENT at the menu, listening on port %u\n", port);
    if (comms) { lsock = l; start_comms(); }
    Sleep(menu_ms);
    DWORD t0 = now_ms();
    gs_set(3, 1);   /* Host Game: before the service restarts */
    if (comms) stop_comms();
    closesocket(l);
    WSACleanup();
    WSADATA w;
    WSAStartup(0x0101, &w);
    closesocket(socket(AF_INET, SOCK_STREAM, 0));
    l = tcp_bound(port);
    if (l == INVALID_SOCKET || listen(l, 5)) { printf("EVENT Host Game: listen on %u failed (%d)\n", port, WSAGetLastError()); return INVALID_SOCKET; }
    struct sockaddr_in a;
    int al = sizeof(a);
    getsockname(l, (struct sockaddr *)&a, &al);
    printf("EVENT Host Game: listening again on port %u after %lu ms\n", ntohs(a.sin_port), (unsigned long)(now_ms() - t0));
    return l;
}

static int run_host(unsigned short port)
{
    DWORD t0 = now_ms(), tick = 0;
    SOCKET probe = socket(AF_INET, SOCK_STREAM, 0);   /* the game's network probe */
    closesocket(probe);
    lsock = menu_then_host(port, 1);
    if (lsock == INVALID_SOCKET) { printf("RESULT fail listen\n"); return 1; }
    {   /* the game shows its own address (through okplay: its stand-in) */
        char me[256];
        struct hostent *h;
        gethostname(me, sizeof(me));
        h = gethostbyname(me);
        if (h && h->h_addr_list[0]) printf("EVENT my address %s\n", inet_ntoa(*(struct in_addr *)h->h_addr_list[0]));
    }
    start_comms();
    int next_player = 1, leaving = 0;
    DWORD race_t0 = 0;
    while (!failed) {
        if (now_ms() - t0 > (DWORD)secs * 1000) { fail("timeout (%d/%d frames, %d players)", bundles, frames, next_player - 1); break; }
        EnterCriticalSection(&comms);
        if (parked != INVALID_SOCKET && next_player < nplayers && now_ms() - parked_at >= (DWORD)claim_delay) {   /* claim it */
            conn *c = &conns[next_player];
            memset(c, 0, sizeof(*c));
            c->used = 1;
            c->s = parked;
            c->player = next_player++;
            c->connected = 1;
            parked = INVALID_SOCKET;
            printf("EVENT player %d connected after %lu ms\n", c->player, (unsigned long)(now_ms() - t0));
        }
        int all_cars = next_player == nplayers, all_bar = all_cars, open = 0, all_left = 1;
        for (int i = 1; i < next_player; i++) {
            conn *c = &conns[i];
            if (!c->got_car) all_cars = 0;
            if (!c->got_barrier) all_bar = 0;
            if (!c->closed) { open++; all_left = 0; }
        }
        if (all_cars && !burst_sent) {   /* the race-start burst to everyone */
            burst_sent = 1;
            unsigned char b[209];
            for (int i = 1; i < nplayers; i++) {
                conn *c = &conns[i];
                for (int k = 0; k < 11; k++) {
                    int len = k < 10 ? 209 : 113;
                    b[0] = 5;
                    fill(b, len, 50 + k);
                    send_retry(c, b, len);
                }
                unsigned char car[196];
                for (int p = 0; p < nplayers; p++) {
                    if (p == i) continue;
                    car[0] = 0x0c;
                    car[1] = (unsigned char)p;
                    fill(car + 1, 195, 100 + p);
                    send_retry(c, car, 196);
                }
                b[0] = 4; b[1] = 0; b[2] = 0; b[3] = 0x5a;
                send_retry(c, b, 4);
            }
            printf("EVENT start burst sent\n");
        }
        if (all_bar && !race_on) { race_on = 1; race_t0 = now_ms(); printf("EVENT race starts\n"); }
        if (race_on && (int)sent_frames < frames && now_ms() - tick >= 1000u / hz && sent_frames < bundles + LEAD) {
            tick = now_ms();
            unsigned f = sent_frames++;
            inputs[0][f % (LEAD * 4)] = f;
            have[0][f % (LEAD * 4)] = 1;
            send_bundles();
        }
        if (die_at >= 0 && (int)bundles >= die_at) {   /* the process ends with its connections open */
            printf("EVENT the host process ends now (%s) at frame %u\n", die_hard ? "TerminateProcess" : "ExitProcess", bundles);
            if (die_hard) TerminateProcess(GetCurrentProcess(), 3);
            ExitProcess(3);
        }
        if (race_on && (int)bundles >= frames && !leaving) { leaving = 1; printf("EVENT all %d frames done in %lu ms\n", frames, (unsigned long)(now_ms() - race_t0)); }
        LeaveCriticalSection(&comms);
        if (leaving && all_left && next_player == nplayers) break;
        (void)open;
        Sleep(1);
    }
    stop_comms();
    closesocket(lsock);
    printf("EVENT accepted %u connection(s) in all\n", n_accepted);
    int ok = !failed;
    for (int i = 1; i < MAXP; i++) if (conns[i].used && !conns[i].got_leave) ok = 0;
    if (ok) for (int i = 1; i < MAXP; i++) if (conns[i].used && conns[i].next_in != (unsigned)frames) { ok = 0; fail("player %d sent %u inputs", i, conns[i].next_in); }
    stats();
    if (ok) printf("RESULT ok\n"); else printf("RESULT fail %s\n", failed ? fail_reason : "a player did not leave cleanly");
    return ok ? 0 : 1;
}

static int parse_addr(const char *arg, struct sockaddr_in *to, unsigned short defport)
{
    char host[256];
    lstrcpynA(host, arg, sizeof(host));
    char *colon = strchr(host, ':');
    memset(to, 0, sizeof(*to));
    to->sin_family = AF_INET;
    to->sin_port = htons(colon ? (unsigned short)atoi(colon + 1) : defport);
    if (colon) *colon = 0;
    to->sin_addr.s_addr = inet_addr(host);
    if (to->sin_addr.s_addr == INADDR_NONE) {
        struct hostent *h = gethostbyname(host);
        if (!h || !h->h_addr_list[0]) return 0;
        memcpy(&to->sin_addr, h->h_addr_list[0], 4);
    }
    return 1;
}

static int fatal_connect_error(int e)
{
    switch (e) {
    case WSAEFAULT: case WSAEINVAL: case WSAEAFNOSUPPORT: case WSAEADDRINUSE: case WSAEADDRNOTAVAIL: case WSAENETDOWN:
    case WSAENETUNREACH: case WSAEISCONN: case WSAECONNREFUSED: case WSAEHOSTDOWN: case WSAEHOSTUNREACH:
        return 1;
    }
    return 0;
}

/* the game's connect loop, completed by the comms thread. 1 = connected; *how says how it ended */
static int connect_loop(conn *c, const struct sockaddr_in *to, DWORD abort_ms, int *attempts, char *how)
{
    DWORD t0 = now_ms();
    *attempts = 0;
    for (;;) {
        if (now_ms() - t0 > abort_ms) { strcpy(how, "gave up (timeout)"); return 0; }
        SOCKET s = tcp_bound(0);
        if (s == INVALID_SOCKET) { strcpy(how, "socket/bind failed"); return 0; }
        DWORD deadline = GetTickCount() + 1000;
        (*attempts)++;
        data_opts(s);
        connect(s, (const struct sockaddr *)to, sizeof(*to));
        int e = WSAGetLastError();   /* always read, whatever connect returned */
        if (e == WSAEWOULDBLOCK) {
            EnterCriticalSection(&comms);
            c->s = s;
            c->connected = 0;
            connecting = c;
            LeaveCriticalSection(&comms);
            while (connecting && (int)(GetTickCount() - deadline) < 0) Sleep(10);
            EnterCriticalSection(&comms);
            int timed_out = connecting == c;
            connecting = NULL;
            if (c->connected) { LeaveCriticalSection(&comms); strcpy(how, "connected"); return 1; }
            c->s = INVALID_SOCKET;
            LeaveCriticalSection(&comms);
            closesocket(s);
            if (!timed_out) { strcpy(how, "refused (except set)"); return 0; }
            continue;
        }
        closesocket(s);
        if (fatal_connect_error(e)) { sprintf(how, "refused (connect error %d)", e); return 0; }
        /* retry class (including 0 = connected at once) */
    }
}

static int run_join(const char *addr, int orig)
{
    struct sockaddr_in to, me;
    int ml = sizeof(me), attempts;
    char how[64];
    DWORD t0 = now_ms(), tick = 0;
    /* the menu: the game's port listened on (kept: the comms thread polls it) */
    lsock = tcp_bound((unsigned short)(menu_port >= 0 ? menu_port : orig ? 1030 : 9803));
    if (lsock == INVALID_SOCKET) { printf("RESULT fail bind\n"); return 1; }
    getsockname(lsock, (struct sockaddr *)&me, &ml);
    if (listen(lsock, 5)) { printf("RESULT fail listen %d\n", WSAGetLastError()); return 1; }
    printf("EVENT at the menu, listening on port %u\n", ntohs(me.sin_port));
    start_comms();
    Sleep(menu_ms);
    t0 = now_ms();
    if (!parse_addr(addr, &to, 9803)) { printf("RESULT fail address %s does not resolve\n", addr); return 1; }
    conn *c = &conns[0];
    memset(c, 0, sizeof(*c));
    c->used = 1;
    c->s = INVALID_SOCKET;
    gs_set(3, 0);   /* a join */
    int ok = connect_loop(c, &to, 20000, &attempts, how);
    printf("EVENT %s after %lu ms (%d attempts)\n", how, (unsigned long)(now_ms() - t0), attempts);
    if (!ok) { stop_comms(); printf("RESULT fail %s\n", how); return 1; }
    unsigned char v[0x29];
    memset(v, 0, sizeof(v));
    memcpy(v + 1, "27, 2.0", 7);
    send_retry(c, v, 0x29);
    while (!failed) {
        if (now_ms() - t0 > (DWORD)secs * 1000) { fail("timeout (%u/%d frames)", bundles, frames); break; }
        EnterCriticalSection(&comms);
        if (c->closed && expect_loss && t_lost) {
            DWORD dt = t_lost - last_bundle_ms;
            LeaveCriticalSection(&comms);
            printf("EVENT connection lost %lu ms after the last frame (frame %u, error %d)\n", (unsigned long)dt, bundles, lost_err);
            stop_comms();
            stats();
            if (race_on && dt <= (DWORD)expect_loss) { printf("RESULT ok\n"); return 0; }
            printf("RESULT fail the loss came %lu ms after the last frame (expected within %d)\n", (unsigned long)dt, expect_loss);
            return 1;
        }
        if (c->closed) { LeaveCriticalSection(&comms); if (!failed) fail("connection closed"); break; }
        if (race_on && (int)sent_frames < frames && now_ms() - tick >= 1000u / hz && sent_frames < bundles + LEAD) {
            unsigned char m[6];
            unsigned f = sent_frames;
            tick = now_ms();
            m[0] = 6;
            m[1] = (unsigned char)(signed char)(f - bundles);
            put32(m + 2, (unsigned)my_player << 24 | f);
            t_sent[f] = now_ms();
            sent_frames++;
            send_retry(c, m, 6);
        }
        int done = race_on && (int)bundles >= frames;
        LeaveCriticalSection(&comms);
        if (done) break;
        Sleep(1);
    }
    if (!failed && expect_loss) fail("the connection never broke");
    if (!failed) {
        unsigned char lv[4] = { 4, (unsigned char)my_player, 0, 0x0c };
        send_retry(c, lv, 4);
        printf("EVENT all %d frames done, leaving\n", frames);
    }
    close_conn(c);
    stop_comms();
    closesocket(lsock);
    stats();
    if (!failed) printf("RESULT ok\n"); else printf("RESULT fail %s\n", fail_reason);
    return failed ? 1 : 0;
}

/* ------------------------------------------------------------------ edge cases */

static int wait_sock(SOCKET s, int rd, int wr, DWORD ms)   /* 1 readable/writable, 2 except, 0 timeout, -1 error */
{
    DWORD t0 = now_ms();
    for (;;) {
        fd_set r, w, e;
        struct timeval tv = { 0, 25000 };
        r.fd_count = w.fd_count = e.fd_count = 0;
        if (rd) ADD(r, s);
        if (wr) ADD(w, s);
        ADD(e, s);
        int n = select(0, &r, &w, &e, &tv);
        if (n == SOCKET_ERROR) return -1;
        if (n > 0) return FD_ISSET(s, &e) ? 2 : 1;
        if (now_ms() - t0 >= ms) return 0;
    }
}

static unsigned char patt(unsigned i) { return (unsigned char)(i * 7 + (i >> 8) * 13 + 3); }

static int recv_all(SOCKET s, unsigned char *b, int len, DWORD ms)   /* non-blocking socket: exactly len bytes */
{
    int got = 0;
    DWORD t0 = now_ms();
    while (got < len) {
        int n = recv(s, (char *)b + got, len - got, 0);
        if (n > 0) { got += n; continue; }
        if (n == 0) return got;
        if (WSAGetLastError() != WSAEWOULDBLOCK) return -WSAGetLastError();
        if (now_ms() - t0 > ms) return got;
        wait_sock(s, 1, 0, 100);
    }
    return got;
}

static int send_all(SOCKET s, const unsigned char *b, int len, DWORD ms)
{
    int put = 0;
    DWORD t0 = now_ms();
    while (put < len) {
        int n = send(s, (const char *)b + put, len - put, 0);
        if (n > 0) { put += n; continue; }
        if (n == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) return -WSAGetLastError();
        if (now_ms() - t0 > ms) return put;
        wait_sock(s, 0, 1, 100);
    }
    return put;
}

/* the edge cases' connect: the game's loop, inline (no comms thread) */
static SOCKET edge_connect(const struct sockaddr_in *to, DWORD abort_ms, char *how, int *attempts)
{
    DWORD t0 = now_ms();
    *attempts = 0;
    for (;;) {
        if (now_ms() - t0 > abort_ms) { strcpy(how, "gave up (timeout)"); return INVALID_SOCKET; }
        SOCKET s = tcp_bound(0);
        (*attempts)++;
        data_opts(s);
        connect(s, (const struct sockaddr *)to, sizeof(*to));
        int e = WSAGetLastError();
        if (e == WSAEWOULDBLOCK) {
            int r = wait_sock(s, 0, 1, 1000);
            if (r == 1) { strcpy(how, "connected"); return s; }
            closesocket(s);
            if (r == 2) { strcpy(how, "refused (except set)"); return INVALID_SOCKET; }
            continue;
        }
        closesocket(s);
        if (fatal_connect_error(e)) { sprintf(how, "refused (connect error %d)", e); return INVALID_SOCKET; }
    }
}

static int edge_host(const char *edge, unsigned short port)
{
    SOCKET l = menu_then_host(port, 0), s;
    unsigned char *b;
    int ok = 0;
    char why[160] = "";
    if (l == INVALID_SOCKET) { printf("RESULT fail listen\n"); return 1; }
    printf("EVENT listening on port %u (edge %s)\n", port, edge);
    if (!strcmp(edge, "refuse")) {   /* only here so a joiner through okplay finds a room; nobody connects. Two listen
                                        * sockets (port, port+2): okplay sends a connect to a port nobody listens on to the
                                        * game's one listening port; with two, the joiner's port+1 is refused */
        SOCKET l2 = tcp_bound((unsigned short)(port + 2));
        if (l2 != INVALID_SOCKET) listen(l2, 5);
        Sleep((DWORD)secs * 1000 < 15000 ? (DWORD)secs * 1000 : 15000);
        closesocket(l);
        printf("RESULT ok\n");
        return 0;
    }
    if (wait_sock(l, 1, 0, (DWORD)secs * 1000) != 1) { printf("RESULT fail no connection\n"); return 1; }
    struct sockaddr_in a;
    int al = sizeof(a);
    s = accept(l, (struct sockaddr *)&a, &al);
    if (s == INVALID_SOCKET) { printf("RESULT fail accept %d\n", WSAGetLastError()); return 1; }
    printf("EVENT accepted %s:%u\n", inet_ntoa(a.sin_addr), ntohs(a.sin_port));
    data_opts(s);
    b = (unsigned char *)malloc(1 << 20);
    if (!strcmp(edge, "peek")) {
        /* messages: 300 of them, lengths 1..1024 (one of 1024, some tiny), sent in pieces cut at odd places */
        int total = 0;
        for (int m = 0; m < 300; m++) {
            int len = m == 7 ? 1024 : m % 50 == 3 ? 1000 : 1 + (m * 37) % 90;
            b[total++] = (unsigned char)(len >> 8);
            b[total++] = (unsigned char)len;
            for (int i = 0; i < len; i++, total++) b[total] = patt((unsigned)total);
        }
        static const int cuts[] = { 1, 1, 3, 7, 1500, 2, 5, 333, 1, 2049, 64, 9 };
        int off = 0, k = 0;
        while (off < total) {
            int n = cuts[k++ % 12];
            if (n > total - off) n = total - off;
            if (send_all(s, b + off, n, 5000) != n) { sprintf(why, "send failed"); break; }
            off += n;
            if (k % 3 == 0) Sleep(15);   /* let the other side see partial messages */
        }
        unsigned char r[4];
        int n = recv_all(s, r, 4, 20000);
        ok = n == 4 && get32(r) == 300 && off == total;
        if (!ok && !why[0]) sprintf(why, "joiner's answer %d bytes, value %u", n, n == 4 ? get32(r) : 0);
    } else if (!strcmp(edge, "halfclose")) {
        int n = 0, k;
        while ((k = recv(s, (char *)b + n, (1 << 20) - n, 0)) != 0) {
            if (k > 0) { n += k; continue; }
            if (WSAGetLastError() != WSAEWOULDBLOCK) { sprintf(why, "recv error %d", WSAGetLastError()); break; }
            if (wait_sock(s, 1, 0, 20000) != 1) { sprintf(why, "no EOF"); break; }
        }
        int good = n == 10240;
        for (int i = 0; i < n && good; i++) if (b[i] != patt((unsigned)i)) good = 0;
        printf("EVENT read %d bytes then EOF%s\n", n, good ? ", pattern ok" : "");
        unsigned char r[4];
        put32(r, (unsigned)n);
        ok = good && !why[0] && send_all(s, r, 4, 5000) == 4;   /* still allowed: only their side is shut */
        if (!good && !why[0]) sprintf(why, "got %d bytes", n);
    } else if (!strcmp(edge, "rstunread")) {
        for (int i = 0; i < 100; i++) b[i] = (unsigned char)i;
        send_all(s, b, 100, 5000);
        int w = wait_sock(s, 1, 0, 20000);
        int n = recv(s, (char *)b, 100, 0);
        int e = n == SOCKET_ERROR ? WSAGetLastError() : 0;
        printf("EVENT after the joiner closed: select %d, recv %d, error %d\n", w, n, e);
        ok = n == SOCKET_ERROR && e == WSAECONNRESET;
        if (!ok) sprintf(why, "recv gave %d / error %d, expected WSAECONNRESET", n, e);
    } else if (!strcmp(edge, "window")) {
        Sleep(3000);   /* not reading */
        printf("EVENT reading now\n");
        unsigned total = 0, bad = 0;
        int k;
        for (;;) {
            k = recv(s, (char *)b, 1 << 20, 0);
            if (k > 0) { for (int i = 0; i < k; i++) if (b[i] != patt(total + i)) bad++; total += k; continue; }
            if (k == 0) break;
            if (WSAGetLastError() != WSAEWOULDBLOCK) { sprintf(why, "recv error %d", WSAGetLastError()); break; }
            if (wait_sock(s, 1, 0, 30000) != 1) { sprintf(why, "stalled"); break; }
        }
        printf("EVENT read %u bytes then EOF, %u wrong\n", total, bad);
        unsigned char r[4];
        put32(r, total);
        send_all(s, r, 4, 5000);
        ok = !why[0] && !bad;
    } else {
        sprintf(why, "unknown edge case %s", edge);
    }
    shutdown(s, SD_BOTH);
    Sleep(1);
    closesocket(s);
    closesocket(l);
    free(b);
    if (ok) printf("RESULT ok\n"); else printf("RESULT fail %s\n", why);
    return ok ? 0 : 1;
}

static int edge_join(const char *edge, const char *addr)
{
    struct sockaddr_in to;
    char how[64], why[160] = "";
    int attempts, ok = 0;
    DWORD t0 = now_ms();
    if (!parse_addr(addr, &to, 9803)) { printf("RESULT fail address\n"); return 1; }
    SOCKET menu = tcp_bound((unsigned short)(menu_port >= 0 ? menu_port : 9803));   /* the menu listener, kept */
    if (menu != INVALID_SOCKET) listen(menu, 5);
    Sleep(menu_ms);
    t0 = now_ms();
    if (!strcmp(edge, "refuse")) to.sin_port = htons((unsigned short)(ntohs(to.sin_port) + 1));
    gs_set(3, 0);   /* a join */
    SOCKET s = edge_connect(&to, !strcmp(edge, "refuse") ? 10000 : 20000, how, &attempts);
    printf("EVENT %s after %lu ms (%d attempts)\n", how, (unsigned long)(now_ms() - t0), attempts);
    if (!strcmp(edge, "refuse")) {
        /* must never connect. How it ends differs: Windows' own TCP retries a refused SYN for about 2 s, longer
         * than the game's 1 s attempt, so on real TCP the loop retries until it gives up; okplay answers at once */
        ok = s == INVALID_SOCKET;
        if (s != INVALID_SOCKET) closesocket(s);
        printf(ok ? "RESULT ok\n" : "RESULT fail %s\n", how);
        return ok ? 0 : 1;
    }
    if (s == INVALID_SOCKET) { printf("RESULT fail %s\n", how); return 1; }
    unsigned char *b = (unsigned char *)malloc(1 << 20);
    if (!strcmp(edge, "peek")) {
        unsigned pos = 0, msgs = 0, spins = 0, partial = 0;
        DWORD t1 = now_ms();
        while (msgs < 300 && !why[0]) {
            if (now_ms() - t1 > 30000) { sprintf(why, "timeout after %u messages", msgs); break; }
            int w = wait_sock(s, 1, 0, 25);
            if (w <= 0) continue;
            int n = recv(s, (char *)b, 1, MSG_PEEK);
            if (n <= 0) { if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) sprintf(why, "peek 1: %d/%d", n, WSAGetLastError()); else spins++; continue; }
            n = recv(s, (char *)b, 2, MSG_PEEK);
            if (n < 2) { partial++; continue; }
            int len = b[0] << 8 | b[1];
            n = recv(s, (char *)b, len + 2, MSG_PEEK);
            if (n < len + 2) { partial++; continue; }
            u_long fr = 0;
            ioctlsocket(s, FIONREAD, &fr);
            if (fr < (u_long)(len + 2)) { sprintf(why, "FIONREAD %lu < %d peeked", (unsigned long)fr, len + 2); break; }
            for (int i = 0; i < len; i++) if (b[2 + i] != patt(pos + 2 + i)) { sprintf(why, "message %u byte %d wrong", msgs, i); break; }
            n = recv(s, (char *)b, len + 2, 0);   /* consume: must be the same bytes */
            if (n != len + 2 || b[2] != patt(pos + 2)) { sprintf(why, "consume gave %d", n); break; }
            pos += len + 2;
            msgs++;
        }
        printf("EVENT %u messages, %u partial peeks, %u empty wake-ups\n", msgs, partial, spins);
        unsigned char r[4];
        put32(r, msgs);
        send_all(s, r, 4, 5000);
        ok = msgs == 300 && !why[0];
        if (!ok && !why[0]) sprintf(why, "%u messages", msgs);
        wait_sock(s, 1, 0, 5000);   /* the host closes */
    } else if (!strcmp(edge, "halfclose")) {
        for (int i = 0; i < 10240; i++) b[i] = patt((unsigned)i);
        int n = send_all(s, b, 10240, 10000);
        shutdown(s, SD_SEND);
        unsigned char r[4];
        int k = recv_all(s, r, 4, 20000);
        int k2 = recv(s, (char *)b, 16, 0);
        if (k2 == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) { wait_sock(s, 1, 0, 10000); k2 = recv(s, (char *)b, 16, 0); }
        printf("EVENT sent %d then shut our side; host says %u, then recv %d\n", n, k == 4 ? get32(r) : 0, k2);
        ok = n == 10240 && k == 4 && get32(r) == 10240 && k2 == 0;
        if (!ok) sprintf(why, "sent %d, answer %d bytes, then %d", n, k, k2);
    } else if (!strcmp(edge, "rstunread")) {
        int w = wait_sock(s, 1, 0, 10000);
        Sleep(200);
        u_long fr = 0;
        ioctlsocket(s, FIONREAD, &fr);
        printf("EVENT %lu bytes waiting (select %d): closing without reading them\n", (unsigned long)fr, w);
        ok = w == 1 && fr == 100;
        if (!ok) sprintf(why, "select %d, %lu bytes waiting", w, (unsigned long)fr);
        closesocket(s);
        s = INVALID_SOCKET;
    } else if (!strcmp(edge, "window")) {
        unsigned sent = 0, blocked_at = 0, target = 0;
        DWORD tb = 0, resumed_ms = 0;
        for (int i = 0; i < 1024; i++) b[i] = 0;
        while (!why[0]) {
            if (blocked_at && sent >= target) break;
            unsigned char chunk[1024];
            for (int i = 0; i < 1024; i++) chunk[i] = patt(sent + i);
            int n = send(s, (char *)chunk, 1024, 0);
            if (n > 0) {
                if (blocked_at && !resumed_ms) { resumed_ms = now_ms() - tb; }
                sent += n;
                if (!blocked_at && sent > 64u << 20) { sprintf(why, "never blocked (64 MB sent)"); break; }
                continue;
            }
            if (WSAGetLastError() != WSAEWOULDBLOCK) { sprintf(why, "send error %d", WSAGetLastError()); break; }
            if (!blocked_at) { blocked_at = sent; target = sent + 256 * 1024; tb = now_ms(); printf("EVENT WSAEWOULDBLOCK after %u bytes\n", sent); }
            if (wait_sock(s, 0, 1, 15000) != 1) { sprintf(why, "never writable again"); break; }
        }
        if (!why[0]) {
            printf("EVENT resumed after %lu ms; %u bytes in all\n", (unsigned long)resumed_ms, sent);
            shutdown(s, SD_SEND);
            unsigned char r[4];
            int k = recv_all(s, r, 4, 30000);
            ok = k == 4 && get32(r) == sent;
            if (!ok) sprintf(why, "host read %u of %u", k == 4 ? get32(r) : 0, sent);
        }
    } else {
        sprintf(why, "unknown edge case %s", edge);
    }
    if (s != INVALID_SOCKET) { shutdown(s, SD_BOTH); Sleep(1); closesocket(s); }
    free(b);
    if (ok) printf("RESULT ok\n"); else printf("RESULT fail %s\n", why);
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ selftest (one process, no peers) */

static int selftest(void)
{
    int bad = 0;
    SOCKET l = tcp_bound(0);
    struct sockaddr_in me;
    int ml = sizeof(me);
    getsockname(l, (struct sockaddr *)&me, &ml);
    listen(l, 5);
    printf("EVENT listen socket on port %u\n", ntohs(me.sin_port));
    /* 1. a 25 ms select with nothing ready */
    DWORD mn = 0xffffffff, mx = 0, sum = 0;
    for (int i = 0; i < 20; i++) {
        fd_set r;
        struct timeval tv = { 0, 25000 };
        r.fd_count = 0;
        ADD(r, l);
        DWORD t = now_ms();
        int n = select(0, &r, NULL, NULL, &tv);
        t = now_ms() - t;
        if (n != 0) { printf("EVENT select returned %d (expected 0)\n", n); bad++; }
        sum += t; if (t < mn) mn = t; if (t > mx) mx = t;
    }
    printf("EVENT 25 ms select: avg %lu, min %lu, max %lu ms\n", (unsigned long)(sum / 20), (unsigned long)mn, (unsigned long)mx);
    if (sum / 20 < 20 || sum / 20 > 30) { printf("EVENT select timing off\n"); bad++; }
    /* 2. all sets empty */
    {
        fd_set r, w, e;
        struct timeval tv = { 0, 25000 };
        r.fd_count = w.fd_count = e.fd_count = 0;
        DWORD t = now_ms();
        int n = select(0, &r, &w, &e, &tv);
        int er = WSAGetLastError();
        t = now_ms() - t;
        printf("EVENT empty sets: select %d, error %d, %lu ms\n", n, er, (unsigned long)t);
        if (n != SOCKET_ERROR || er != WSAEINVAL || t > 5) bad++;
    }
    /* 3. poll: zero timeout, listen socket in read and write sets: nothing */
    {
        fd_set r, w;
        struct timeval tv = { 0, 0 };
        r.fd_count = w.fd_count = 0;
        ADD(r, l);
        ADD(w, l);
        int n = select(0, &r, &w, NULL, &tv);
        printf("EVENT poll of the idle listen socket: %d\n", n);
        if (n != 0) bad++;
    }
    /* 4. FIONREAD on the listen socket, SO_TYPE */
    {
        u_long fr = 99;
        int r = ioctlsocket(l, FIONREAD, &fr);
        int ty = 0, tl = sizeof(ty);
        getsockopt(l, SOL_SOCKET, SO_TYPE, (char *)&ty, &tl);
        printf("EVENT FIONREAD on the listen socket: %d, %lu; SO_TYPE %d\n", r, (unsigned long)fr, ty);
        if (r != 0 || fr != 0 || ty != SOCK_STREAM) bad++;
    }
    /* 5. UDP: writable at once; its own datagram (real UDP only) makes it readable; mixed with the TCP socket */
    {
        SOCKET u = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in ua;
        int ul = sizeof(ua);
        memset(&ua, 0, sizeof(ua));
        ua.sin_family = AF_INET;
        bind(u, (struct sockaddr *)&ua, sizeof(ua));
        getsockname(u, (struct sockaddr *)&ua, &ul);
        fd_set w;
        struct timeval tv = { 0, 25000 };
        w.fd_count = 0;
        ADD(w, u);
        int n = select(0, NULL, &w, NULL, &tv);
        printf("EVENT UDP socket writable: %d\n", n);
        if (n != 1) bad++;
        ua.sin_addr.s_addr = inet_addr("127.0.0.1");
        sendto(u, "x", 1, 0, (struct sockaddr *)&ua, sizeof(ua));
        fd_set r;
        r.fd_count = 0;
        ADD(r, l);
        ADD(r, u);
        tv.tv_usec = 200000;
        DWORD t = now_ms();
        n = select(0, &r, NULL, NULL, &tv);
        printf("EVENT listen + UDP with a datagram waiting: %d ready (%s) in %lu ms\n", n,
               n == 1 && r.fd_array[0] == u ? "the UDP socket" : "?", (unsigned long)(now_ms() - t));
        closesocket(u);
    }
    closesocket(l);
    printf(bad ? "RESULT fail %d check(s)\n" : "RESULT ok\n", bad);
    return bad ? 1 : 0;
}

/* ------------------------------------------------------------------ scripts
 *
 * One run of the game as a list of what the player does, so a new menu path is one line in tcp_run.py:
 *   menu          enter the "Network (TCP)" menu: the game listens on its port (--menu-port, else --port). With a
 *                 listener still open from before it first closes it, restarts Winsock and makes a probe - the same
 *                 calls as Host Game
 *   back          back to Connect Players (the game keeps its listener open: nothing happens on the sockets)
 *   host          Host Game: close, Winsock restart, probe, listen again on --port
 *   close         the host backs out of its lobby: its connections and its listener close
 *   quit          leave network play: everything closes, Winsock is cleaned up, 2.5 s pass
 *   lobby:N       host: wait (--secs) until N players in all have reached the lobby
 *   left:N        host: wait until N players in all have left
 *   quiet:MS      host: nobody may connect for MS ms
 *   join:ADDR     connect (the game's loop, 20 s) and reach the host's lobby; joinfail:ADDR: the connect must fail
 *   leave         the joiner leaves the lobby (leave message, then close)
 *   ping          a message to the host that must be answered within 10 s
 *   wait:MS       nothing for MS ms
 *   say:NAME      tell the others (a file NAME.sig in this folder or TCPSIM_SIGDIR), await:NAME waits for it (120 s)
 *   env:K=V       a setting for what comes next (the test answers okplay reads, e.g. OKP_TEST_JOIN_PICK)
 * Every step prints EVENT step ...; the first one that does not go as expected ends the run: RESULT fail step N. */
static int next_slot = 1;
static unsigned short port_arg = 9803;   /* --port: Host Game's */

static int listen_on(unsigned short port, const char *what)
{
    SOCKET l = tcp_bound(port);
    struct sockaddr_in a;
    int al = sizeof(a);
    if (l == INVALID_SOCKET || listen(l, 5)) {
        printf("EVENT %s: listen on %u failed (%d)\n", what, port, WSAGetLastError());
        if (l != INVALID_SOCKET) closesocket(l);
        return 0;
    }
    getsockname(l, (struct sockaddr *)&a, &al);
    EnterCriticalSection(&comms);
    lsock = l;
    LeaveCriticalSection(&comms);
    printf("EVENT %s: listening on port %u\n", what, ntohs(a.sin_port));
    if (!comms_running) start_comms();
    return 1;
}

static void service_restart(void)   /* the game's service close, Winsock restart and probe (Host Game, a menu re-entry) */
{
    WSADATA w;
    if (comms_running) stop_comms();
    if (lsock != INVALID_SOCKET) { closesocket(lsock); lsock = INVALID_SOCKET; }
    WSACleanup();
    WSAStartup(0x0101, &w);
    closesocket(socket(AF_INET, SOCK_STREAM, 0));
}

static void host_claim(void)   /* host: accepted connections become players */
{
    EnterCriticalSection(&comms);
    if (parked != INVALID_SOCKET && next_slot < MAXP) {
        conn *c = &conns[next_slot];
        memset(c, 0, sizeof(*c));
        c->used = 1;
        c->s = parked;
        c->player = next_slot++;
        c->connected = 1;
        parked = INVALID_SOCKET;
        printf("EVENT player %d connected\n", c->player);
    }
    LeaveCriticalSection(&comms);
}

static int host_count(int what)   /* 0: players that reached the lobby, 1: players that left */
{
    int n = 0;
    EnterCriticalSection(&comms);
    for (int i = 1; i < next_slot; i++) n += what ? (conns[i].got_leave && conns[i].closed) : conns[i].got_car;
    LeaveCriticalSection(&comms);
    return n;
}

static void close_all(void)
{
    if (comms_running) stop_comms();
    for (int i = 0; i < MAXP; i++) if (conns[i].used) close_conn(&conns[i]);
    if (parked != INVALID_SOCKET) { closesocket(parked); parked = INVALID_SOCKET; }
    if (lsock != INVALID_SOCKET) { closesocket(lsock); lsock = INVALID_SOCKET; }
}

static int script_join(const char *addr, int must_fail, char *why)
{
    struct sockaddr_in to;
    char how[64];
    int attempts;
    DWORD t0 = now_ms();
    if (!comms_running) { strcpy(why, "not at the menu (no comms thread)"); return 0; }
    if (!parse_addr(addr, &to, 9803)) { sprintf(why, "address %s does not resolve", addr); return must_fail; }
    conn *c = &conns[0];
    EnterCriticalSection(&comms);
    memset(c, 0, sizeof(*c));
    c->used = 1;
    c->s = INVALID_SOCKET;
    got_info = 0;
    pongs = 0;
    LeaveCriticalSection(&comms);
    gs_set(3, 0);   /* a join: before the connect */
    int ok = connect_loop(c, &to, 20000, &attempts, how);
    printf("EVENT %s after %lu ms (%d attempts)\n", how, (unsigned long)(now_ms() - t0), attempts);
    if (!ok) gs_set(0, 1);   /* (a failed join: back to the menu, reset) */
    if (must_fail) {
        if (ok) { close_conn(c); strcpy(why, "connected, but this join must fail"); return 0; }
        return 1;
    }
    if (!ok) { sprintf(why, "connect: %s", how); return 0; }
    unsigned char v[0x29];
    memset(v, 0, sizeof(v));
    memcpy(v + 1, "27, 2.0", 7);
    send_retry(c, v, 0x29);
    for (DWORD t1 = now_ms(); !got_info && !c->closed && now_ms() - t1 < 10000;) Sleep(10);
    if (!got_info) { strcpy(why, c->closed ? "the host closed the connection before its game info" : "no game info from the host in 10 s"); return 0; }
    printf("EVENT in the lobby as player %d\n", my_player);
    return 1;
}

static int run_script(int n, char **steps)
{
    char why[200] = "";
    int i, ok = 1;
    unsigned short mport = (unsigned short)(menu_port >= 0 ? menu_port : 9803);
    script_mode = 1;
    is_host = 0;
    for (i = 0; i < n && ok && !failed; i++) {
        const char *st = steps[i], *arg = strchr(st, ':');
        arg = arg ? arg + 1 : "";
        DWORD t0 = now_ms();
        printf("EVENT step %d %s\n", i + 1, st);
        why[0] = 0;
        if (!strcmp(st, "menu")) {
            if (lsock != INVALID_SOCKET) service_restart();
            else closesocket(socket(AF_INET, SOCK_STREAM, 0));   /* the game's probe */
            is_host = 0;
            ok = listen_on(mport, "the TCP menu");
            if (!ok) strcpy(why, "menu listen failed");
        } else if (!strcmp(st, "back")) {
            printf("EVENT back at Connect Players (the menu's listener stays open)\n");
        } else if (!strcmp(st, "host")) {
            gs_set(3, 1);   /* Host Game: the reset, then net 3 and server 1, before the service restarts */
            service_restart();
            ok = listen_on(port_arg, "Host Game");
            if (ok) {
                char me[256];
                struct hostent *h;
                is_host = 1;
                printf("EVENT Host Game: listening again on port %u after %lu ms\n", port_arg, (unsigned long)(now_ms() - t0));
                gethostname(me, sizeof(me));
                h = gethostbyname(me);
                if (h && h->h_addr_list[0]) printf("EVENT my address %s\n", inet_ntoa(*(struct in_addr *)h->h_addr_list[0]));
            } else strcpy(why, "Host Game's listen failed");
        } else if (!strcmp(st, "close")) {
            close_all();
            is_host = 0;
            gs_set(0, 1);
            printf("EVENT the host left its lobby: connections and listener closed\n");
        } else if (!strcmp(st, "disconnect")) {   /* the lobby's Disconnect: the game keeps its listener until the next menu entry */
            for (int i = 0; i < MAXP; i++) if (conns[i].used) close_conn(&conns[i]);   /* (the comms thread goes on selecting) */
            is_host = 0;
            gs_set(0, 1);
            printf("EVENT the host left its lobby: connections closed, the listener stays open\n");
        } else if (!strcmp(st, "quit")) {
            WSADATA w;
            close_all();
            gs_set(0, 1);
            WSACleanup();
            Sleep(2500);
            WSAStartup(0x0101, &w);
            is_host = 0;
            printf("EVENT left network play\n");
        } else if (!strncmp(st, "lobby:", 6) || !strncmp(st, "left:", 5)) {
            int want = atoi(arg), what = st[1] == 'e', have;
            while ((have = host_count(what)) < want && now_ms() - t0 < (DWORD)secs * 1000 && !failed) { host_claim(); Sleep(5); }
            printf("EVENT %d player(s) %s\n", have, what ? "left" : "in the lobby");
            if (have < want) { ok = 0; sprintf(why, "%d of %d players %s in %d s", have, want, what ? "left" : "came", secs); }
        } else if (!strncmp(st, "quiet:", 6)) {
            unsigned before = n_accepted;
            while (now_ms() - t0 < (DWORD)atoi(arg) && n_accepted == before) { host_claim(); Sleep(5); }
            if (n_accepted != before) { ok = 0; strcpy(why, "a player connected"); }
        } else if (!strncmp(st, "join:", 5) || !strncmp(st, "joinfail:", 9)) {
            ok = script_join(arg, st[4] == 'f', why);
        } else if (!strcmp(st, "joinbox")) {   /* the Join box: opens with the address typed last, the player presses Ok */
            char a[64];
            if (!gs_join) { ok = 0; strcpy(why, "joinbox needs TCPSIM_GAMESTATE"); }
            else {
                printf("EVENT the Join box shows \"%s\"\n", gs_join);
                snprintf(a, sizeof(a), "%s:%u", gs_join, port_arg);
                ok = gs_join[0] && script_join(a, 0, why);
                if (!gs_join[0]) strcpy(why, "the Join box is empty");
            }
        } else if (!strcmp(st, "leave")) {
            unsigned char lv[4] = { 4, (unsigned char)my_player, 0, 0x0c };
            if (conns[0].used && !conns[0].closed) send_retry(&conns[0], lv, 4);
            Sleep(100);
            close_conn(&conns[0]);
            gs_set(0, 1);
            printf("EVENT left the lobby\n");
        } else if (!strcmp(st, "ping")) {
            unsigned char pg[4] = { 9, (unsigned char)my_player, 1, 2 };
            int before = pongs;
            if (conns[0].closed || !conns[0].used) { ok = 0; strcpy(why, "no connection to the host"); }
            else {
                send_retry(&conns[0], pg, 4);
                while (pongs == before && !conns[0].closed && now_ms() - t0 < 10000) Sleep(10);
                if (pongs == before) { ok = 0; strcpy(why, conns[0].closed ? "the connection closed" : "no answer in 10 s"); }
                else printf("EVENT ping answered in %lu ms\n", (unsigned long)(now_ms() - t0));
            }
        } else if (!strncmp(st, "wait:", 5)) {
            DWORD ms = (DWORD)atoi(arg);
            while (now_ms() - t0 < ms) { if (is_host) host_claim(); Sleep(5); }
        } else if (!strncmp(st, "say:", 4) || !strncmp(st, "await:", 6)) {
            char f[MAX_PATH], dir[MAX_PATH] = ".";
            GetEnvironmentVariableA("TCPSIM_SIGDIR", dir, sizeof(dir));   /* (processes in different folders) */
            snprintf(f, sizeof(f), "%s/%s.sig", dir, arg);
            if (st[0] == 's') {
                FILE *fp = fopen(f, "w");
                if (fp) fclose(fp);
                printf("EVENT say %s\n", arg);
            } else {
                while (GetFileAttributesA(f) == INVALID_FILE_ATTRIBUTES && now_ms() - t0 < 120000) { if (is_host) host_claim(); Sleep(20); }
                if (GetFileAttributesA(f) == INVALID_FILE_ATTRIBUTES) { ok = 0; sprintf(why, "nobody said %s", arg); }
            }
        } else if (!strncmp(st, "env:", 4)) {
            _putenv(arg);
        } else {
            ok = 0;
            sprintf(why, "unknown step");
        }
        if (failed && ok) { ok = 0; lstrcpynA(why, fail_reason, sizeof(why)); }
    }
    close_all();
    if (ok && !failed) { printf("RESULT ok\n"); return 0; }
    printf("RESULT fail step %d (%s): %s\n", i, steps[i - 1], why[0] ? why : fail_reason);
    return 1;
}

int main(int argc, char **argv)
{
    WSADATA w;
    const char *edge = NULL, *addr = NULL;
    unsigned short port = 9803;
    int orig = 0, nsteps = 0;
    char **steps = (char **)calloc(argc, sizeof(char *));
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2 || WSAStartup(0x0101, &w)) {
        printf("usage: tcpsim host|join ADDR[:PORT]|selftest [--port P] [--players N] [--frames F] [--hz H] [--secs S] [--orig] [--edge NAME]\n");
        return 2;
    }
    if (!gs_init()) { printf("RESULT fail game state\n"); return 2; }
    timeBeginPeriod(1);
    InitializeCriticalSection(&comms);
    is_host = !strcmp(argv[1], "host");
    int i = 2;
    if (!strcmp(argv[1], "join") && argc > 2) addr = argv[i++];
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = (unsigned short)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--players") && i + 1 < argc) nplayers = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hz") && i + 1 < argc) hz = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--secs") && i + 1 < argc) secs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--edge") && i + 1 < argc) edge = argv[++i];
        else if (!strcmp(argv[i], "--orig")) orig = 1;
        else if (!strcmp(argv[i], "--claim-delay") && i + 1 < argc) claim_delay = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--die-at") && i + 1 < argc) die_at = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--die") && i + 1 < argc) die_hard = !strcmp(argv[++i], "hard");
        else if (!strcmp(argv[i], "--expect-loss") && i + 1 < argc) expect_loss = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--menu-ms") && i + 1 < argc) menu_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--menu-port") && i + 1 < argc) menu_port = atoi(argv[++i]);
        else if (argv[i][0] != '-' && steps) steps[nsteps++] = argv[i];   /* script steps */
    }
    port_arg = port;
    if (nplayers < 2 || nplayers > MAXP || frames < 1 || hz < 1) return 2;
    max_frames = frames > 100000 ? frames : 100000;   /* a joiner takes the host's frame count */
    t_sent = (DWORD *)calloc(max_frames + 1, sizeof(DWORD));
    rtts = (DWORD *)calloc(max_frames + 1, sizeof(DWORD));
    int r;
    if (!strcmp(argv[1], "probe")) {   /* the game's network probes only: create and close, own address (no box may come) */
        for (int k = 0; k < 3; k++) closesocket(socket(AF_INET, SOCK_STREAM, 0));
        char me[256];
        gethostname(me, sizeof(me));
        struct hostent *h = gethostbyname(me);
        printf("EVENT probes done, own address %s\nRESULT ok\n", h && h->h_addr_list[0] ? inet_ntoa(*(struct in_addr *)h->h_addr_list[0]) : "?");
        r = 0;
    } else if (!strcmp(argv[1], "selftest")) r = selftest();
    else if (!strcmp(argv[1], "script")) r = run_script(nsteps, steps);
    else if (edge) r = is_host ? edge_host(edge, port) : edge_join(edge, addr ? addr : "127.0.0.1");
    else if (is_host) r = run_host(port);
    else if (addr) r = run_join(addr, orig);
    else r = 2;
    timeEndPeriod(1);
    WSACleanup();
    return r;
}
