/* okplay - transport: one dual-stack UDP socket, peers by id, probing/punching, STUN. */
#include "okp.h"
#include <iphlpapi.h>
#include <mstcpip.h>
#include <stdio.h>
#include <string.h>

#define PROBE_EVERY_MS     200
#define PROBE_SLOW_MS      2000
#define PROBE_FAST_FOR_MS  30000
#define KEEPALIVE_MS       2000
#define PATH_DEAD_MS       15000
#define QUEUE_MAX_AGE_MS   10000
#define PEER_EXPIRE_MS     120000

#define STUN_MAGIC 0x2112A442u
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

/* ------------------------------------------------------------------ peers */

okp_peer *okp_peer_get(okp_ctx *c, uint64_t id, int create)
{
    okp_peer *freep = NULL;
    for (int i = 0; i < OKP_MAX_PEERS; i++) {
        okp_peer *p = &c->peers[i];
        if (p->used && p->id == id) return p;
        if (!p->used && !freep) freep = p;
    }
    if (!create || id == 0 || id == c->self) return NULL;
    if (!freep) {
        /* full: junk (e.g. forged knocks) must not lock real players out - reuse the quietest
         * peer that has no DirectPlay players and isn't our host */
        okp_peer *victim = NULL;
        for (int i = 0; i < OKP_MAX_PEERS; i++) {
            okp_peer *p = &c->peers[i];
            if (p->nplayers > 0 || p->id == c->host_peer) continue;
            if (!victim || (int)(p->last_rx - victim->last_rx) < 0) victim = p;
        }
        if (!victim) return NULL;
        okp_log(1, "peer table full: dropping idle peer %016llx for %016llx", (unsigned long long)victim->id,
                (unsigned long long)id);
        okp_peer_free_bufs(victim);
        victim->used = 0;
        freep = victim;
    }
    memset(freep, 0, sizeof(*freep));
    freep->used = 1;
    freep->id = id;
    freep->probe_start = okp_now();
    freep->last_rx = okp_now();
    freep->keep_until = okp_now() + 60000;
    freep->my_epoch = (uint32_t)okp_rand64() | 1;   /* never 0: 0 = "an older version, no id" */
    freep->my_token = okp_rand64();
    if (!freep->my_token) freep->my_token = 1;      /* 0 = "no token" on the wire */
    okp_log(2, "peer %016llx: new", (unsigned long long)id);
    return freep;
}

/* Only real unicast endpoints may become probe targets: a forged knock or listing must not
 * aim our probes at broadcast/multicast addresses, port 0, or (online) our own loopback. */
int okp_cand_ok(const okp_ctx *c, const SOCKADDR_IN6 *a)
{
    if (a->sin6_family != AF_INET6 || a->sin6_port == 0) return 0;
    const uint8_t *b = (const uint8_t *)&a->sin6_addr;
    if (okp_addr_is_v4(a)) {
        const uint8_t *v4 = b + 12;
        if (v4[0] == 0 || v4[0] >= 224) return 0;                       /* this-net, multicast, broadcast */
        if (v4[0] == 127) return c && c->mode == OKP_MODE_LAN;          /* same-PC LAN tests only */
        return 1;
    }
    /* an IPv4-only socket (Windows 9x, no IPv6 installed) can't reach these: probing them anyway
     * failed ~5 times a second for the whole game (and filled the Windows 98 log) */
    if (c && c->v4only) return 0;
    static const uint8_t zero[16];
    if (memcmp(b, zero, 16) == 0) return 0;                              /* :: */
    if (b[0] == 0xff) return 0;                                          /* multicast */
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return 0;                 /* link-local needs a scope id */
    if (memcmp(b, zero, 15) == 0 && b[15] == 1) return 0;                /* ::1 */
    return 1;
}

/* Our shared player data, as replicated by dplayx from another player: trust nothing about
 * its size. Returns the number of candidates, or -1 if it isn't okplay player data. */
int okp_pdata_parse(const void *buf, DWORD sz, uint64_t *peer, SOCKADDR_IN6 *out, int max)
{
    const DWORD fixed = (DWORD)offsetof(okp_pdata, cands);
    if (!buf || sz < fixed) return -1;
    const okp_pdata *pd = (const okp_pdata *)buf;
    if (pd->magic != OKP_PD_MAGIC) return -1;
    *peer = pd->peer;
    int avail = (int)((sz - fixed) / sizeof(okp_ccand));
    int n = 0;
    for (int i = 0; i < pd->ncands && i < avail && i < OKP_PDATA_CANDS && n < max; i++) {
        const okp_ccand *cc = &pd->cands[i];
        if (cc->fam == 4) {
            okp_addr_from_v4(&out[n], cc->addr, cc->port);
        } else if (cc->fam == 6) {
            memset(&out[n], 0, sizeof(out[n]));
            out[n].sin6_family = AF_INET6;
            out[n].sin6_port = cc->port;
            memcpy(&out[n].sin6_addr, cc->addr, 16);
        } else {
            continue;
        }
        n++;
    }
    return n;
}

/* A session record (dplayx ENUMSESSIONSREPLY) from the server or the LAN is handed straight to
 * Windows' dplayx, so check its layout first: envelope, DPSESSIONDESC2 size, and a session name
 * that is NUL-terminated (as a 2-byte NUL, like dplayx reads it) inside the record. */
int okp_enum_blob_ok(const uint8_t *b, int n)
{
    if (!b || n < 92 + 2 || n > 1024) return 0;
    if (memcmp(b, "play", 4) != 0) return 0;
    uint16_t cmd;
    uint32_t sdsize, off;
    memcpy(&cmd, b + 4, 2);
    memcpy(&sdsize, b + 8, 4);
    memcpy(&off, b + 88, 4);
    if (cmd != 1 || sdsize != 80) return 0;
    if (off < 92 || off >= (uint32_t)n) return 0;
    for (uint32_t i = off; i + 1 < (uint32_t)n; i += 2)
        if (b[i] == 0 && b[i + 1] == 0) return 1;
    return 0;
}

void okp_peer_add_cands(okp_ctx *c, okp_peer *p, const SOCKADDR_IN6 *cands, int n)
{
    for (int i = 0; i < n; i++) {
        if (!okp_cand_ok(c, &cands[i])) continue;
        int dup = 0;
        for (int j = 0; j < p->ncands; j++)
            if (okp_addr_eq(&p->cands[j], &cands[i])) { dup = 1; break; }
        if (dup) continue;
        if (p->ncands < OKP_MAX_CANDS) {
            p->cands[p->ncands++] = cands[i];
        } else {
            /* keep the list fresh: newest candidates replace the oldest */
            memmove(&p->cands[0], &p->cands[1], sizeof(p->cands[0]) * (OKP_MAX_CANDS - 1));
            p->cands[OKP_MAX_CANDS - 1] = cands[i];
        }
    }
}

static void pend_free(okp_peer *p)
{
    while (p->pend) {
        okp_pend *e = p->pend;
        p->pend = e->next;
        free(e);
    }
    p->pend_tail = NULL;
    p->npend = p->pend_bytes = 0;
}

void okp_peer_free_bufs(okp_peer *p)
{
    for (int k = 0; k < p->qn; k++) free(p->q[k].data);
    for (int k = 0; k < p->nrtx; k++) free(p->rtx[k].pkt);
    for (int k = 0; k < p->nrbuf; k++) free(p->rbuf[k].data);
    pend_free(p);
    free(p->reasm);
    p->reasm = NULL;
    p->reasm_len = p->reasm_bad = 0;
    p->qn = p->nrtx = p->nrbuf = 0;
}

/* ------------------------------------------------------------- candidates */

/* ---- Router-simulator test hook (tests/natsim.py). Off unless OKP_TEST_NAT is set.
 * Every datagram goes to the simulator wrapped as  "NATS" | private ip(4) | dest ip(16) | dest port(2) | data,
 * and comes back as  "NATR" | src ip(16) | src port(2) | data.  The simulator applies each fake
 * router's address translation and filtering, so NAT combinations can be tested on one PC. */
static int nat_on;
static SOCKADDR_IN6 nat_router;
static uint8_t nat_private[4];

static void nat_init(void)
{
    char v[64];
    nat_on = 0;
    if (!GetEnvironmentVariableA("OKP_TEST_NAT", v, sizeof(v)) || !okp_addr_parse(v, &nat_router)) return;
    char ip[32];
    if (!GetEnvironmentVariableA("OKP_TEST_PRIVATE", ip, sizeof(ip)) || okp_pton(AF_INET, ip, nat_private) != 1) return;
    nat_on = 1;
    okp_log(1, "TEST: router simulator at %s, private address %s", v, ip);
}

/* one IPv4 candidate from dotted text (GetAdaptersInfo's form) */
static void add_v4_cand(okp_ctx *c, const char *ip)
{
    uint8_t b[4];
    if (c->ncands >= OKP_MAX_CANDS - 1 || !okp_pton(AF_INET, ip, b)) return;   /* leave room for srflx */
    if (b[0] == 127 || b[0] == 0) return;
    if (b[0] == 169 && b[1] == 254 && c->mode != OKP_MODE_LAN) return;   /* see below */
    for (int i = 0; i < c->ncands; i++)
        if (!memcmp(((uint8_t *)&c->cands[i].sin6_addr) + 12, b, 4)) return;
    okp_addr_from_v4(&c->cands[c->ncands++], b, htons(c->port));
}

/* Windows 98/ME/2000 (no GetAdaptersAddresses): the adapters' IPv4 addresses; Windows 95 has no
 * iphlpapi at all - the host name's addresses then */
static void gather_cands_v4(okp_ctx *c)
{
    c->ncands = 0;
    if (okp_sys.GetAdaptersInfo) {
        ULONG sz = 16 * 1024;
        IP_ADAPTER_INFO *ai = NULL;
        DWORD rc = ERROR_BUFFER_OVERFLOW;
        for (int tries = 0; tries < 3 && rc == ERROR_BUFFER_OVERFLOW; tries++) {
            free(ai);
            ai = (IP_ADAPTER_INFO *)malloc(sz);
            rc = ai ? okp_sys.GetAdaptersInfo(ai, &sz) : ERROR_NOT_ENOUGH_MEMORY;
        }
        if (rc == NO_ERROR)
            for (IP_ADAPTER_INFO *a = ai; a; a = a->Next)
                for (IP_ADDR_STRING *ip = &a->IpAddressList; ip; ip = ip->Next)
                    add_v4_cand(c, ip->IpAddress.String);
        free(ai);
    }
    if (!c->ncands) {
        char name[256];
        SOCKADDR_IN6 addrs[8];
        int n = gethostname(name, sizeof(name)) == 0 ? okp_resolve(name, 0, 4, addrs, 8) : 0;
        for (int i = 0; i < n; i++) {
            char ip[32];
            okp_ntop(AF_INET, ((uint8_t *)&addrs[i].sin6_addr) + 12, ip, sizeof(ip));
            add_v4_cand(c, ip);
        }
    }
    for (int i = 0; i < c->ncands; i++) {
        char s[80];
        okp_log(2, "local cand: %s (IPv4 only)", okp_addr_str(&c->cands[i], s, sizeof(s)));
    }
}

void okp_gather_cands(okp_ctx *c)
{
    if (nat_on) {   /* behind a simulated router: our only local address is the fake private one */
        okp_addr_from_v4(&c->cands[0], nat_private, htons(c->port));
        c->ncands = 1;
        return;
    }
    okp_sys_load();
    if (!okp_sys.GetAdaptersAddresses) {   /* Windows 9x/2000: IPv4 only, from GetAdaptersInfo */
        gather_cands_v4(c);
        return;
    }
    ULONG sz = 32 * 1024;
    IP_ADAPTER_ADDRESSES *aa = NULL;
    ULONG rc;
    for (int tries = 0; tries < 3; tries++) {
        aa = (IP_ADAPTER_ADDRESSES *)malloc(sz);
        rc = okp_sys.GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                  GAA_FLAG_SKIP_DNS_SERVER, NULL, aa, &sz);
        if (rc != ERROR_BUFFER_OVERFLOW) break;
        free(aa);
        aa = NULL;
    }
    c->ncands = 0;
    if (!aa || rc != NO_ERROR) { free(aa); return; }

    int nv6 = 0;
    /* pass 0: IPv4; pass 1: stable IPv6; pass 2: temporary IPv6 */
    for (int pass = 0; pass < 3; pass++) {
        for (IP_ADAPTER_ADDRESSES *a = aa; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
                if (c->ncands >= OKP_MAX_CANDS - 1) break;   /* leave room for srflx */
                SOCKADDR *sa = u->Address.lpSockaddr;
                SOCKADDR_IN6 cand;
                if (sa->sa_family == AF_INET && pass == 0) {
                    uint8_t *b = (uint8_t *)&((SOCKADDR_IN *)sa)->sin_addr;
                    if (b[0] == 127 || b[0] == 0) continue;
                    /* 169.254 = no DHCP: useless online, but it is how two PCs on a bare switch talk */
                    if (b[0] == 169 && b[1] == 254 && c->mode != OKP_MODE_LAN) continue;
                    okp_addr_from_v4(&cand, b, htons(c->port));
                } else if (sa->sa_family == AF_INET6 && pass > 0 && !c->v4only) {
                    SOCKADDR_IN6 *s6 = (SOCKADDR_IN6 *)sa;
                    uint8_t *b = (uint8_t *)&s6->sin6_addr;
                    int temp = u->SuffixOrigin == IpSuffixOriginRandom;
                    if ((pass == 1 && temp) || (pass == 2 && !temp)) continue;
                    if ((b[0] & 0xe0) != 0x20) continue;             /* global unicast 2000::/3 only */
                    if (u->DadState != IpDadStatePreferred) continue;
                    if (nv6 >= 2) continue;
                    memset(&cand, 0, sizeof(cand));
                    cand.sin6_family = AF_INET6;
                    cand.sin6_addr = s6->sin6_addr;
                    cand.sin6_port = htons(c->port);
                    nv6++;
                } else {
                    continue;
                }
                c->cands[c->ncands++] = cand;
            }
        }
    }
    free(aa);
    for (int i = 0; i < c->ncands; i++) {
        char s[80];
        okp_log(2, "local cand: %s", okp_addr_str(&c->cands[i], s, sizeof(s)));
    }
}

void okp_local_cands(okp_ctx *c, SOCKADDR_IN6 *out, int *n, int max)
{
    int k = 0, fam = okp_cfg.family;
    /* srflx first: it is the address remote players most likely need */
    if (c->have_srflx && k < max && fam != 6) out[k++] = c->srflx;
    else if (c->have_guess && k < max && fam != 6) out[k++] = c->guess;   /* no STUN answer: best guess */
    for (int i = 0; i < c->ncands && k < max; i++) {
        int v4 = okp_addr_is_v4(&c->cands[i]);
        if ((fam == 4 && !v4) || (fam == 6 && v4)) continue;
        int dup = 0;
        for (int j = 0; j < k; j++) if (okp_addr_eq(&out[j], &c->cands[i])) dup = 1;
        if (!dup) out[k++] = c->cands[i];
    }
    *n = k;
}

int okp_cands_to_str(const SOCKADDR_IN6 *cands, int n, char *out, int outsz)
{
    int o = 0;
    out[0] = 0;
    for (int i = 0; i < n; i++) {
        char s[80];
        okp_addr_str(&cands[i], s, sizeof(s));
        int m = snprintf(out + o, outsz - o, "%s%s", i ? "," : "", s);
        if (m < 0 || m >= outsz - o) break;
        o += m;
    }
    return o;
}

int okp_cands_from_str(const char *s, SOCKADDR_IN6 *cands, int max)
{
    int n = 0;
    char tok[80];
    while (*s && n < max) {
        const char *e = strchr(s, ',');
        int len = e ? (int)(e - s) : (int)strlen(s);
        if (len > 0 && len < (int)sizeof(tok)) {
            memcpy(tok, s, len);
            tok[len] = 0;
            if (okp_addr_parse(tok, &cands[n])) n++;
        }
        if (!e) break;
        s = e + 1;
    }
    return n;
}

/* ------------------------------------------------------------------- send */

/* test knob: OKP_BLACKOUT_FILE=<path> - while that file exists this machine sends and receives
 * nothing (its cable pulled). For machines whose clocks can't be trusted (86Box VMs drift minutes
 * behind): the test creates and deletes the file through okagent. Looked at every 250 ms. */
static int file_blackout(void)
{
    static volatile LONG parsed;
    static char path[MAX_PATH];
    static DWORD checked;
    static int on;
    if (!parsed) {
        if (!GetEnvironmentVariableA("OKP_BLACKOUT_FILE", path, sizeof(path))) path[0] = 0;
        InterlockedExchange(&parsed, 1);
    }
    if (!path[0]) return 0;
    DWORD now = GetTickCount();
    if (now - checked >= 250) {
        checked = now;
        on = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
    }
    return on;
}

/* test knob: OKP_BLACKOUT=<unix time>:<seconds> - nothing is sent in that wall-clock window (every
 * process of a test gets the same window, so the line goes dead both ways, like an internet outage) */
static int in_blackout(void)
{
    static int parsed;
    static unsigned long long from_ms, to_ms;
    static int was;
    FILETIME ft;
    unsigned long long now;
    int in;
    if (!parsed) {
        char v[64];
        unsigned long at = 0, secs = 0;
        parsed = 1;
        if (GetEnvironmentVariableA("OKP_BLACKOUT", v, sizeof(v)) && sscanf(v, "%lu:%lu", &at, &secs) == 2 && secs) {
            from_ms = (unsigned long long)at * 1000;
            to_ms = from_ms + (unsigned long long)secs * 1000;
        }
    }
    if (!to_ms && !file_blackout() && !was) return 0;
    GetSystemTimeAsFileTime(&ft);
    now = ((((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000) - 11644473600000ULL;
    in = (to_ms && now >= from_ms && now < to_ms) || file_blackout();
    if (in != was) {
        was = in;
        if (to_ms)
            okp_log(1, "test blackout %s (OKP_BLACKOUT)", in ? "starts: sending nothing" : "over: sending again");
        else
            okp_log(1, "test blackout %s (OKP_BLACKOUT_FILE)", in ? "starts: sending and receiving nothing"
                                                                  : "over: the line is back");
    }
    return in;
}

static void raw_send(okp_ctx *c, const SOCKADDR_IN6 *to, const void *buf, int len)
{
    if (okp_cfg.drop_pct > 0 && (int)(okp_rand64() % 100) < okp_cfg.drop_pct) return;   /* test knob */
    if (in_blackout()) return;
    if (nat_on) {
        uint8_t *w = (uint8_t *)malloc(26 + len);
        if (!w) return;
        memcpy(w, "NATS", 4);
        memcpy(w + 4, nat_private, 4);
        memcpy(w + 8, &to->sin6_addr, 16);
        memcpy(w + 24, &to->sin6_port, 2);
        memcpy(w + 26, buf, len);
        okp_sendto6(c->sock, c->v4only, w, 26 + len, &nat_router);
        free(w);
        return;
    }
    if (okp_sendto6(c->sock, c->v4only, buf, len, to) < 0) {
        char s[80];
        okp_log(3, "sendto %s failed: %d", okp_addr_str(to, s, sizeof(s)), WSAGetLastError());
    }
}

static void fill_wire(okp_ctx *c, okp_wire *w, int type, uint64_t to)
{
    w->m0 = 'O';
    w->m1 = 'K';
    w->ver = OKP_WIRE_VER;
    w->type = (uint8_t)type;
    w->room = c->room;
    w->from = c->self;
    w->to = to;
    w->token = 0;
}

/* every packet to a peer carries the token that peer gave us - stamped at send time, because
 * queued and resent packets may have been built before we were told it */
static void peer_send(okp_ctx *c, okp_peer *p, const SOCKADDR_IN6 *to, uint8_t *pkt, int len)
{
    ((okp_wire *)pkt)->token = p->their_token;
    raw_send(c, to, pkt, len);
}

/* an address we know for this peer: its path, or a candidate from a source we trust */
static int addr_known(const okp_peer *p, const SOCKADDR_IN6 *a)
{
    if (p->have_path && okp_addr_eq(&p->path, a)) return 1;
    for (int i = 0; i < p->ncands; i++)
        if (okp_addr_eq(&p->cands[i], a)) return 1;
    return 0;
}

static void send_probe(okp_ctx *c, okp_peer *p, const SOCKADDR_IN6 *to)
{
    uint8_t buf[sizeof(okp_wire) + sizeof(okp_probe)];
    fill_wire(c, (okp_wire *)buf, OKP_T_PROBE, p->id);
    okp_probe *pr = (okp_probe *)(buf + sizeof(okp_wire));
    pr->nonce = (uint32_t)okp_rand64();
    pr->t_send = okp_now();
    pr->epoch = p->my_epoch;
    pr->token = p->my_token;   /* (to addresses we trust for this peer - its candidates - only) */
    pr->host_mode = c->is_host ? (uint8_t)(1 + okp_cfg.transport) : 0;   /* the host decides the transport */
    peer_send(c, p, to, buf, sizeof(buf));
}

/* a joiner adopts its host's transport (told in the host's probes / acks) */
static void heard_host_mode(okp_ctx *c, uint64_t from, uint8_t mode)
{
    if (!mode || mode > 3 || c->is_host || (c->host_peer && c->host_peer != from)) return;
    int t = mode - 1;
    if (c->host_transport != t) {
        okp_log(1, "transport: the host uses %s%s", okp_transport_name(t),
                okp_transport_force() >= 0 ? " (ignored: transport_force is set)" : " - so do we");
        c->host_transport = t;
    }
}

/* The other side started over with this peer (new attempt id): whatever our reliable lane still
 * holds belongs to the old attempt. Keeping it made a retried join hang: its request (seq 0 again)
 * looked like a duplicate of the refused attempt's, was acked, and never reached DirectPlay. */
static void lane_restart(okp_peer *p)
{
    for (int k = 0; k < p->nrtx; k++) free(p->rtx[k].pkt);
    for (int k = 0; k < p->nrbuf; k++) free(p->rbuf[k].data);
    pend_free(p);
    free(p->reasm);
    p->reasm = NULL;
    p->reasm_len = p->reasm_bad = 0;
    p->nrtx = p->nrbuf = 0;
    p->r_tx_next = p->r_rx_next = 0;
}

static void flush_queue(okp_ctx *c, okp_peer *p)
{
    for (int i = 0; i < p->qn; i++) {
        peer_send(c, p, &p->path, p->q[i].data, p->q[i].len);
        free(p->q[i].data);
        p->tx_pkts++;
    }
    if (p->qn) okp_log(2, "peer %016llx: flushed %d queued packets", (unsigned long long)p->id, p->qn);
    p->qn = 0;
}

static void set_path(okp_ctx *c, okp_peer *p, const SOCKADDR_IN6 *from, const char *why)
{
    char s[80];
    if (p->have_path && okp_addr_eq(&p->path, from)) return;
    if (p->have_path && okp_now() - p->last_path_rx < 5000) return;   /* sticky while alive */
    if (p->have_path) {
        char o[80];
        okp_log(1, "peer %016llx: path changing from %s (silent %lums)", (unsigned long long)p->id,
                okp_addr_str(&p->path, o, sizeof(o)), (unsigned long)(okp_now() - p->last_path_rx));
    }
    p->path = *from;
    p->have_path = 1;
    p->path_changes++;
    p->path_since = okp_now();
    p->last_path_rx = okp_now();
    okp_log(1, "peer %016llx: path %s via %s (%s, %lums after first probe)", (unsigned long long)p->id,
            okp_addr_str(from, s, sizeof(s)), okp_addr_is_v4(from) ? "IPv4" : "IPv6", why,
            (unsigned long)(okp_now() - p->probe_start));
    okp_peer_add_cands(c, p, from, 1);
    flush_queue(c, p);
}

/* lock held: queue one reliable datagram (RDATA or RFRAG) and send it now if we have a path;
 * the timer resends until acked. The caller has checked the window has room. */
static void rel_send_locked(okp_ctx *c, okp_peer *p, int type, const uint8_t *buf, int len)
{
    int total = sizeof(okp_wire) + 4 + len;
    uint8_t *pkt = (uint8_t *)malloc(total);
    if (!pkt) return;
    fill_wire(c, (okp_wire *)pkt, type, p->id);
    uint32_t seq = p->r_tx_next++;
    memcpy(pkt + sizeof(okp_wire), &seq, 4);
    memcpy(pkt + sizeof(okp_wire) + 4, buf, len);
    okp_rtx *r = &p->rtx[p->nrtx++];
    r->seq = seq;
    r->len = total;
    r->pkt = pkt;
    r->first = okp_now();
    r->last = 0;
    r->tries = 0;
    p->r_sent++;
    if (p->have_path) {
        peer_send(c, p, &p->path, pkt, total);
        r->last = okp_now();
        r->tries = 1;
        p->tx_pkts++;
        p->tx_bytes += total;
    }
}

/* The window is a span of sequence numbers: nothing goes out more than OKP_RTX_MAX past the oldest
 * unacked one (rtx[] is in seq order). Counting unacked messages was not enough: with selective
 * acks under loss the sender ran far past a lost message, and the receiver - which can hold only
 * OKP_RTX_MAX early arrivals - had to throw arrivals away. */
static int rel_room(const okp_peer *p)
{
    int used = p->nrtx ? (int)(p->r_tx_next - p->rtx[0].seq) : 0;
    return OKP_RTX_MAX - used;
}

/* lock held: move waiting pieces into the window while it has room (they get their sequence
 * numbers now, so the order on the wire is the order they were sent in) */
static void rel_drain_locked(okp_ctx *c, okp_peer *p)
{
    int moved = 0;
    while (p->pend && rel_room(p) > 0) {
        okp_pend *e = p->pend;
        p->pend = e->next;
        if (!p->pend) p->pend_tail = NULL;
        p->npend--;
        p->pend_bytes -= e->len;
        rel_send_locked(c, p, e->type, e->data, e->len);
        free(e);
        moved++;
    }
    if (moved && !p->pend)
        okp_log(3, "peer %016llx: reliable queue drained", (unsigned long long)p->id);
}

int okp_send_data(okp_ctx *c, uint64_t to, const void *buf, int len, int flags)
{
    if (len < 0 || len > OKP_MSG_MAX) return 0;
    /* dplayx's own messages ("play" envelope) go on the reliable lane: Windows dplayx does
     * not resend its join handshake, so one lost packet used to mean a 30 s failed join.
     * Anything too big for one datagram goes there too, split into in-order pieces.
     * The game's own (small) traffic stays unreliable - AoE2 recovers from loss itself. */
    int reliable = (len >= 4 && memcmp(buf, "play", 4) == 0) || len > OKP_FRAG_MAX;
    /* transport= (okp.h): guaranteed sends reliable too (udp+tcp), or everything (tcp) */
    int tr = okp_effective_transport(c);   /* the host's choice, unless forced (okp.h) */
    if (tr == OKP_TRANSPORT_TCP || (tr == OKP_TRANSPORT_UDP_TCP && (flags & OKP_SEND_GUARANTEED)))
        reliable = 1;

    int ok = 1;
    uint8_t *pkt = NULL;
    int total = sizeof(okp_wire) + len;
    if (!reliable) {
        pkt = (uint8_t *)malloc(total);
        if (!pkt) return 0;
        fill_wire(c, (okp_wire *)pkt, OKP_T_DATA, to);
        memcpy(pkt + sizeof(okp_wire), buf, len);
    }

    EnterCriticalSection(&c->lock);
    okp_peer *p = okp_peer_get(c, to, 1);
    if (p && reliable) {
        int pieces = (len + OKP_FRAG_MAX - 1) / OKP_FRAG_MAX;
        if (pieces < 1) pieces = 1;
        if (p->pend || rel_room(p) < pieces) {
            /* window full (or others already waiting): queue the pieces behind them */
            if (p->pend_bytes + len > OKP_PEND_MAX_BYTES) {
                okp_log(1, "peer %016llx: reliable queue full (%d bytes waiting, %d unacked: it isn't acking)"
                        " - message dropped", (unsigned long long)p->id, p->pend_bytes, p->nrtx);
                p->r_giveup++;
                ok = 0;
            } else {
                if (!p->pend)
                    okp_log(3, "peer %016llx: reliable window full (%d unacked) - queueing",
                            (unsigned long long)p->id, p->nrtx);
                for (int off = 0;; off += OKP_FRAG_MAX) {
                    int chunk = len - off < OKP_FRAG_MAX ? len - off : OKP_FRAG_MAX;
                    int last = off + chunk >= len;
                    okp_pend *e = (okp_pend *)malloc(sizeof(okp_pend) + chunk);
                    if (!e) { ok = 0; break; }   /* (pieces already queued still go: the receiver drops
                                                    the cut message when the next message's pieces join it) */
                    e->next = NULL;
                    e->type = last ? OKP_T_RDATA : OKP_T_RFRAG;
                    e->len = chunk;
                    memcpy(e->data, (const uint8_t *)buf + off, chunk);
                    if (p->pend_tail) p->pend_tail->next = e; else p->pend = e;
                    p->pend_tail = e;
                    p->npend++;
                    p->pend_bytes += chunk;
                    p->r_queued++;
                    if (last) break;
                }
                if (p->pend_bytes > p->pend_peak) p->pend_peak = p->pend_bytes;
                rel_drain_locked(c, p);
            }
        } else {
            /* all pieces under one lock: another thread can't slip a message in between */
            for (int off = 0;; off += OKP_FRAG_MAX) {
                int chunk = len - off < OKP_FRAG_MAX ? len - off : OKP_FRAG_MAX;
                int last = off + chunk >= len;
                rel_send_locked(c, p, last ? OKP_T_RDATA : OKP_T_RFRAG, (const uint8_t *)buf + off, chunk);
                if (last) break;
            }
            if (pieces > 1)
                okp_log(3, "peer %016llx: %d-byte message sent as %d reliable pieces", (unsigned long long)p->id,
                        len, pieces);
        }
    } else if (!p) {
        ok = 0;
    } else if (p->have_path) {
        peer_send(c, p, &p->path, pkt, total);
        p->tx_pkts++;
        p->tx_bytes += total;
    } else if (p->qn < OKP_QMAX) {
        p->q[p->qn].data = pkt;
        p->q[p->qn].len = total;
        p->q[p->qn].t = okp_now();
        p->qn++;
        pkt = NULL;
    } else {
        p->dropped_q++;
        if (p->dropped_q == 1 || p->dropped_q % 100 == 0)
            okp_log(1, "peer %016llx: send queue full, dropped %lu so far (no path yet)",
                    (unsigned long long)p->id, (unsigned long)p->dropped_q);
    }
    if (!p) okp_log(1, "send to %016llx: peer table full", (unsigned long long)to);
    LeaveCriticalSection(&c->lock);
    free(pkt);
    return ok;
}

int okp_wait_path(okp_ctx *c, uint64_t peer, DWORD timeout_ms)
{
    DWORD t0 = okp_now();
    for (;;) {
        EnterCriticalSection(&c->lock);
        okp_peer *p = okp_peer_get(c, peer, 0);
        int have = p && p->have_path;
        LeaveCriticalSection(&c->lock);
        if (have) return 1;
        if (okp_now() - t0 >= timeout_ms) return 0;
        Sleep(20);
    }
}

/* ------------------------------------------------------------------- STUN */

/* the STUN servers' addresses: looked up once, on the rdv thread - a name lookup can block for seconds
 * when DNS is slow or offline, and on the net thread that froze every packet and timer during a join */
void okp_stun_resolve(okp_ctx *c)
{
    char list[256];
    SOCKADDR_IN6 found[8];
    int n = 0;
    lstrcpynA(list, okp_cfg.stun, sizeof(list));
    char *st = list;
    for (char *tok = okp_tok(&st, ","); tok && n < 8; tok = okp_tok(&st, ",")) {
        while (*tok == ' ') tok++;
        char *colon = strrchr(tok, ':');
        const char *port = "3478";
        if (colon) { *colon = 0; port = colon + 1; }
        if (okp_resolve(tok, htons((uint16_t)atoi(port)), 4, &found[n], 1)) n++;
        else okp_log(3, "stun: cannot resolve %s", tok);
    }
    EnterCriticalSection(&c->lock);
    memcpy(c->stun_addr, found, sizeof(found[0]) * n);
    c->nstun = n;
    LeaveCriticalSection(&c->lock);
    c->stun_resolved = 1;
    okp_log(2, "stun: %d server(s) resolved", n);
}

static void stun_send(okp_ctx *c)
{
    if (!c->stun_resolved) return;   /* the rdv thread is still looking them up */
    uint8_t req[20];
    req[0] = 0x00; req[1] = 0x01;                  /* Binding Request */
    req[2] = 0; req[3] = 0;
    uint32_t mc = htonl(STUN_MAGIC);
    memcpy(req + 4, &mc, 4);
    memcpy(req + 8, c->stun_txid, 12);

    for (int i = 0; i < c->nstun; i++) raw_send(c, &c->stun_addr[i], req, sizeof(req));
    c->stun_sent = okp_now();
    c->stun_tries++;
}

/* Returns 1 if the datagram was a STUN response (consumed). */
static int stun_handle(okp_ctx *c, const uint8_t *b, int n)
{
    if (n < 20 || b[0] != 0x01 || b[1] != 0x01) return 0;   /* Binding Success Response */
    uint32_t mc;
    memcpy(&mc, b + 4, 4);
    if (ntohl(mc) != STUN_MAGIC || memcmp(b + 8, c->stun_txid, 12) != 0) return 0;
    int len = (b[2] << 8) | b[3];
    int off = 20;
    while (off + 4 <= 20 + len && off + 4 <= n) {
        int t = (b[off] << 8) | b[off + 1];
        int l = (b[off + 2] << 8) | b[off + 3];
        const uint8_t *v = b + off + 4;
        if (off + 4 + l > n) break;
        if ((t == 0x0020 || t == 0x0001) && l >= 8 && v[1] == 0x01) {   /* IPv4 */
            uint16_t port = (v[2] << 8) | v[3];
            uint8_t ip[4];
            memcpy(ip, v + 4, 4);
            if (t == 0x0020) {
                port ^= STUN_MAGIC >> 16;
                for (int i = 0; i < 4; i++) ip[i] ^= (uint8_t)(STUN_MAGIC >> (24 - 8 * i));
            }
            SOCKADDR_IN6 a;
            okp_addr_from_v4(&a, ip, htons(port));
            if (!c->have_srflx || !okp_addr_eq(&a, &c->srflx)) {
                char s[80];
                okp_log(1, "stun: public address %s", okp_addr_str(&a, s, sizeof(s)));
            }
            c->srflx = a;
            c->have_srflx = 1;
            c->srflx_src = "STUN";
            if (t == 0x0020) break;
        }
        off += 4 + ((l + 3) & ~3);
    }
    return 1;
}

/* ---------------------------------------------------------------- receive */

typedef struct { uint8_t *data; int len; int own; } okp_ready;
#define OKP_READY_MAX (OKP_RTX_MAX + 2)

/* lock held: one in-order reliable payload. Pieces (RFRAG) accumulate until the final RDATA,
 * then the whole message is queued for dplayx. */
static void rel_deliver(okp_peer *p, uint8_t *data, int len, int frag, int own, okp_ready *ready, int *nready)
{
    if (frag || p->reasm_len > 0 || p->reasm_bad) {
        if (!p->reasm_bad) {
            if (p->reasm_len + len > OKP_MSG_MAX) {
                okp_log(1, "peer %016llx: reassembled message over %d bytes - dropped", (unsigned long long)p->id,
                        OKP_MSG_MAX);
                free(p->reasm);
                p->reasm = NULL;
                p->reasm_len = 0;
                p->reasm_bad = 1;
            } else {
                uint8_t *nb = (uint8_t *)realloc(p->reasm, p->reasm_len + len);
                if (nb) {
                    memcpy(nb + p->reasm_len, data, len);
                    p->reasm = nb;
                    p->reasm_len += len;
                } else {
                    p->reasm_bad = 1;
                }
            }
        }
        if (own) free(data);
        if (!frag) {   /* final piece */
            if (!p->reasm_bad && p->reasm && *nready < OKP_READY_MAX) {
                ready[*nready].data = p->reasm;
                ready[*nready].len = p->reasm_len;
                ready[*nready].own = 1;
                (*nready)++;
            } else {
                free(p->reasm);
            }
            p->reasm = NULL;
            p->reasm_len = 0;
            p->reasm_bad = 0;
        }
        return;
    }
    if (*nready < OKP_READY_MAX) {
        ready[*nready].data = data;
        ready[*nready].len = len;
        ready[*nready].own = own;
        (*nready)++;
    } else if (own) {
        free(data);
    }
}

static void hand_to_dplay(okp_ctx *c, const okp_wire *w, void *body, int blen)
{
    if (!c->isp) return;
    okp_sphdr h;
    h.magic = OKP_HDR_MAGIC;
    h.peer = w->from;
    h.room = w->room;
    h.flags = 0;
    HRESULT hr = IDirectPlaySP_HandleMessage(c->isp, body, blen, &h);
    if (hr != DP_OK) {
        static LONG bad;
        LONG k = InterlockedIncrement(&bad);
        if (k > 20 && k % 100 != 0)
            ;
        else if (hr == DPERR_INVALIDPLAYER)   /* normal for a moment after a player leaves */
            okp_log(2, "a %d-byte message from peer %016llx was for a player who has left - dropped (#%ld)",
                    blen, (unsigned long long)w->from, k);
        else
            okp_log(1, "dplayx rejected a %d-byte message from peer %016llx: hr 0x%08lx (#%ld)", blen,
                    (unsigned long long)w->from, (unsigned long)hr, k);
    }
}

static void handle_packet(okp_ctx *c, uint8_t *b, int n, SOCKADDR_IN6 *from)
{
    if (stun_handle(c, b, n)) return;
    if (n < (int)sizeof(okp_wire)) return;
    okp_wire *w = (okp_wire *)b;
    if (w->m0 != 'O' || w->m1 != 'K' || w->ver != OKP_WIRE_VER) return;
    if (w->to != c->self || w->from == 0 || w->from == c->self) return;
    if (c->room == 0 || w->room != c->room) return;

    uint8_t *body = b + sizeof(okp_wire);
    int blen = n - sizeof(okp_wire);
    int deliver = 0;
    okp_ready ready[OKP_READY_MAX];
    int nready = 0;

    char s[80];
    EnterCriticalSection(&c->lock);
    /* only a probe introduces a peer; anything else from an id we don't know is noise */
    okp_peer *p = okp_peer_get(c, w->from, w->type == OKP_T_PROBE);
    if (!p) { LeaveCriticalSection(&c->lock); return; }
    /* the sender proves it is that peer by carrying the token we gave it */
    int trusted = w->token != 0 && w->token == p->my_token;

    if (w->type == OKP_T_PROBE && blen >= OKP_PROBE_MIN) {
        okp_probe *pr = (okp_probe *)body;
        uint32_t epoch = blen >= 12 ? pr->epoch : 0;
        uint64_t tok = blen >= OKP_PROBE_V4 ? pr->token : 0;   /* 0.8.0 probes end at token */
        uint8_t ack[sizeof(okp_wire) + sizeof(okp_probe_ack)];
        fill_wire(c, (okp_wire *)ack, OKP_T_PROBE_ACK, w->from);
        okp_probe_ack *pa = (okp_probe_ack *)(ack + sizeof(okp_wire));
        memset(pa, 0, sizeof(*pa));
        pa->nonce = pr->nonce;
        pa->t_echo = pr->t_send;
        pa->obs_fam = 6;
        pa->obs_port = from->sin6_port;
        memcpy(pa->obs_addr, &from->sin6_addr, 16);
        /* a probe without our token is believed only from an address we know for this peer, or as
         * the id's very first contact (whoever introduces an id is that peer). From anywhere else it
         * may be someone trying to be told the token of an established player: answer so a real peer
         * still learns its public address, but give nothing away and change nothing. */
        if (!trusted && p->path_changes != 0 && !addr_known(p, from)) {
            p->rx_untrusted_probe++;
            if (p->rx_untrusted_probe <= 3 || p->rx_untrusted_probe % 1000 == 0)
                okp_log(1, "peer %016llx: probe from an unknown address %s without our token - answered without it (#%lu)",
                        (unsigned long long)w->from, okp_addr_str(from, s, sizeof(s)), (unsigned long)p->rx_untrusted_probe);
            raw_send(c, from, ack, sizeof(ack));   /* token 0 in the header and the body */
            LeaveCriticalSection(&c->lock);
            return;
        }
        p->last_rx = okp_now();
        if (p->have_path && okp_addr_eq(&p->path, from)) p->last_path_rx = p->last_rx;
        p->rx_pkts++;
        if (epoch) {
            if (p->their_epoch && p->their_epoch != epoch) {
                okp_log(1, "peer %016llx started over (a new connection attempt): reliable lane reset",
                        (unsigned long long)w->from);
                lane_restart(p);
            }
            p->their_epoch = epoch;
        }
        if (tok) p->their_token = tok;
        if (blen >= (int)sizeof(okp_probe)) heard_host_mode(c, w->from, pr->host_mode);
        pa->token = p->my_token;
        pa->host_mode = c->is_host ? (uint8_t)(1 + okp_cfg.transport) : 0;
        peer_send(c, p, from, ack, sizeof(ack));
        set_path(c, p, from, "probe in");
        LeaveCriticalSection(&c->lock);
        return;
    }

    if (!trusted) {
        p->rx_badtoken++;
        if (p->rx_badtoken <= 3 || p->rx_badtoken % 1000 == 0)
            okp_log(1, "peer %016llx: type %u packet from %s with the wrong token dropped (#%lu: forged, or from before a restart)",
                    (unsigned long long)w->from, w->type, okp_addr_str(from, s, sizeof(s)), (unsigned long)p->rx_badtoken);
        LeaveCriticalSection(&c->lock);
        return;
    }
    p->last_rx = okp_now();
    if (p->have_path && okp_addr_eq(&p->path, from)) p->last_path_rx = p->last_rx;
    p->rx_pkts++;

    if (w->type == OKP_T_PROBE_ACK && blen >= OKP_PROBE_ACK_V4) {   /* 0.8.0 acks end at token */
        okp_probe_ack *pa = (okp_probe_ack *)body;
        if (pa->token) p->their_token = pa->token;
        if (blen >= (int)sizeof(okp_probe_ack)) heard_host_mode(c, w->from, pa->host_mode);
        p->rtt = okp_now() - pa->t_echo;
        if (!c->have_srflx && pa->obs_fam == 6) {
            SOCKADDR_IN6 obs;
            memset(&obs, 0, sizeof(obs));
            obs.sin6_family = AF_INET6;
            obs.sin6_port = pa->obs_port;
            memcpy(&obs.sin6_addr, pa->obs_addr, 16);
            if (okp_addr_is_public_v4(&obs)) {
                char s[80];
                c->srflx = obs;
                c->have_srflx = 1;
                c->srflx_src = "peer";
                okp_log(1, "public address %s learned from peer %016llx (no STUN answer)",
                        okp_addr_str(&obs, s, sizeof(s)), (unsigned long long)w->from);
            }
        }
        set_path(c, p, from, "probe ack");
    } else if (w->type == OKP_T_RACK && blen >= 8) {
        uint32_t cum, seq;
        memcpy(&cum, body, 4);
        memcpy(&seq, body + 4, 4);
        int k = 0;
        for (int i = 0; i < p->nrtx; i++) {
            okp_rtx *r = &p->rtx[i];
            if ((int32_t)(r->seq - cum) < 0 || r->seq == seq) free(r->pkt);
            else p->rtx[k++] = *r;
        }
        p->nrtx = k;
        rel_drain_locked(c, p);
    } else if ((w->type == OKP_T_RDATA || w->type == OKP_T_RFRAG) && blen >= 4) {
        int frag = w->type == OKP_T_RFRAG;
        uint32_t seq;
        memcpy(&seq, body, 4);
        set_path(c, p, from, "data");
        uint8_t *pl = body + 4;
        int plen = blen - 4;
        int have = 1;   /* delivered or held: only then is this seq confirmed in the ack */
        if (seq == p->r_rx_next) {
            p->r_rx_next++;
            rel_deliver(p, pl, plen, frag, 0, ready, &nready);
            /* release anything that was waiting for this one */
            for (int again = 1; again;) {
                again = 0;
                for (int i = 0; i < p->nrbuf; i++) {
                    if (p->rbuf[i].seq != p->r_rx_next) continue;
                    okp_rbuf rb = p->rbuf[i];
                    p->rbuf[i] = p->rbuf[--p->nrbuf];
                    p->r_rx_next++;
                    rel_deliver(p, rb.data, rb.len, rb.frag, 1, ready, &nready);
                    again = 1;
                    break;
                }
            }
        } else if ((int32_t)(seq - p->r_rx_next) > 0 && (int32_t)(seq - p->r_rx_next) < 4096) {
            int dup = 0;
            for (int i = 0; i < p->nrbuf; i++) if (p->rbuf[i].seq == seq) dup = 1;
            if (dup) p->r_dups++;
            else {
                uint8_t *copy = p->nrbuf < OKP_RTX_MAX ? (uint8_t *)malloc(plen > 0 ? plen : 1) : NULL;
                if (copy) {
                    okp_rbuf *rb = &p->rbuf[p->nrbuf++];
                    rb->seq = seq;
                    rb->len = plen;
                    rb->frag = frag;
                    rb->data = copy;
                    memcpy(rb->data, pl, plen);
                } else {
                    have = 0;   /* no room (a sender without the span limit): it must resend this one */
                }
            }
        } else if ((int32_t)(seq - p->r_rx_next) < 0) {
            p->r_dups++;   /* already delivered: our ack was lost, ack again below */
        } else {
            have = 0;      /* absurdly far ahead */
        }
        uint8_t ack[sizeof(okp_wire) + 8];
        fill_wire(c, (okp_wire *)ack, OKP_T_RACK, w->from);
        uint32_t aseq = have ? seq : p->r_rx_next - 1;   /* (below cum: confirms nothing new) */
        memcpy(ack + sizeof(okp_wire), &p->r_rx_next, 4);
        memcpy(ack + sizeof(okp_wire) + 4, &aseq, 4);
        peer_send(c, p, from, ack, sizeof(ack));
        p->rx_bytes += n;
        p->rx_sys += nready;
    } else if (w->type == OKP_T_DATA) {
        set_path(c, p, from, "data");
        deliver = 1;
        p->rx_bytes += n;
        /* dplayx system messages start with "play"; everything else is the game's own traffic */
        if (blen >= 8 && memcmp(body, "play", 4) == 0) {
            uint16_t cmd;
            memcpy(&cmd, body + 4, 2);
            p->rx_sys++;
            okp_log(3, "recv sys cmd %u (%d bytes) from peer %016llx", cmd, blen, (unsigned long long)w->from);
        } else {
            p->rx_game++;
        }
    }
    LeaveCriticalSection(&c->lock);

    if (deliver) hand_to_dplay(c, w, body, blen);
    for (int i = 0; i < nready; i++) {
        if (ready[i].len >= 8 && okp_loglevel >= 3) {
            uint16_t cmd;
            memcpy(&cmd, ready[i].data + 4, 2);
            okp_trace("rx", w->from, ready[i].data, ready[i].len);
            okp_log(3, "recv sys cmd %u (%d bytes, reliable) from peer %016llx", cmd, ready[i].len,
                    (unsigned long long)w->from);
        }
        hand_to_dplay(c, w, ready[i].data, ready[i].len);
        if (ready[i].own) free(ready[i].data);
    }
}

static void log_stats(okp_ctx *c, DWORD now)
{
    char s[80];
    for (int i = 0; i < OKP_MAX_PEERS; i++) {
        okp_peer *p = &c->peers[i];
        if (!p->used) continue;
        if (p->have_path) {
            okp_log(2, "stats peer %016llx path %s rtt %lums | tx %lu pkts %lu B | rx %lu pkts %lu B (sys %lu, game %lu)"
                    " | qdrop %lu | path changes %lu | last rx %lums ago | reliable sent %lu resent %lu"
                    " unacked %d gave-up %lu dups %lu queued %lu (waiting %d, peak %d B) | bad token %lu untrusted probes %lu",
                    (unsigned long long)p->id, okp_addr_str(&p->path, s, sizeof(s)), (unsigned long)p->rtt,
                    (unsigned long)p->tx_pkts, (unsigned long)p->tx_bytes, (unsigned long)p->rx_pkts,
                    (unsigned long)p->rx_bytes, (unsigned long)p->rx_sys, (unsigned long)p->rx_game,
                    (unsigned long)p->dropped_q, (unsigned long)p->path_changes,
                    (unsigned long)(now - p->last_rx), (unsigned long)p->r_sent, (unsigned long)p->r_retrans,
                    p->nrtx, (unsigned long)p->r_giveup, (unsigned long)p->r_dups,
                    (unsigned long)p->r_queued, p->npend, p->pend_peak,
                    (unsigned long)p->rx_badtoken, (unsigned long)p->rx_untrusted_probe);
        } else if (!p->last_nopath_log || now - p->last_nopath_log >= 60000) {
            p->last_nopath_log = now;
            okp_log(1, "stats peer %016llx NO PATH: probing %d candidate(s) for %lus, %d packet(s) queued, qdrop %lu",
                    (unsigned long long)p->id, p->ncands, (unsigned long)((now - p->probe_start) / 1000), p->qn,
                    (unsigned long)p->dropped_q);
            for (int k = 0; k < p->ncands; k++)
                okp_log(2, "      candidate %s", okp_addr_str(&p->cands[k], s, sizeof(s)));
        }
    }
}

/* ----------------------------------------------------------------- timers */

static void run_timers(okp_ctx *c)
{
    DWORD now = okp_now();

    if (c->mode == OKP_MODE_ONLINE && !c->have_srflx && okp_cfg.stun[0]) {
        if (c->stun_tries < 4 && now - c->stun_sent > 700) {
            stun_send(c);
        } else if (c->stun_tries == 4 && now - c->stun_sent > 1500) {
            char g[80] = "none";
            if (c->have_guess) okp_addr_str(&c->guess, g, sizeof(g));
            okp_log(1, "stun: no answer from any server after 4 tries - using fallbacks (oldkid.net guess %s,"
                       " then addresses peers report)", g);
            c->stun_tries++;
        }
    }

    EnterCriticalSection(&c->lock);
    /* again, under the lock: a sender may have queued a message (first = now) while we waited for
     * it, and "now - r->first" would wrap and give that message up on the spot (seen on a slow
     * Windows 98 VM, 2026-10-16) */
    now = okp_now();
    if (okp_cfg.stats_ms > 0 && now - c->last_stats >= (DWORD)okp_cfg.stats_ms) {
        log_stats(c, now);
        c->last_stats = now;
    }
    for (int i = 0; i < OKP_MAX_PEERS; i++) {
        okp_peer *p = &c->peers[i];
        if (!p->used) continue;

        if (p->have_path && now - p->last_path_rx > PATH_DEAD_MS) {
            char s[80];
            okp_log(1, "peer %016llx: path %s went silent, re-probing", (unsigned long long)p->id,
                    okp_addr_str(&p->path, s, sizeof(s)));
            p->have_path = 0;
            p->probe_start = now;
        }

        int retained = p->nplayers > 0 || p->id == c->host_peer || (int)(p->keep_until - now) > 0;
        if (!p->have_path && !retained) {
            okp_log(2, "peer %016llx: forgotten (no players left and no path)", (unsigned long long)p->id);
            okp_peer_free_bufs(p);
            p->used = 0;
            continue;
        }

        if (!p->have_path) {
            DWORD every = now - p->probe_start < PROBE_FAST_FOR_MS ? PROBE_EVERY_MS : PROBE_SLOW_MS;
            int passive = p->nplayers == 0 && p->id != c->host_peer;
            if (passive && now - p->probe_start > 15000) every = 0xFFFFFFFF;   /* stop: never joined */
            if (p->ncands && every != 0xFFFFFFFF && now - p->last_probe >= every) {
                for (int k = 0; k < p->ncands; k++) send_probe(c, p, &p->cands[k]);
                p->last_probe = now;
            }
            if (!p->ncands && now - p->last_rx > PEER_EXPIRE_MS) {
                okp_log(2, "peer %016llx: expired", (unsigned long long)p->id);
                okp_peer_free_bufs(p);
                p->used = 0;
                continue;
            }
        } else if (now - p->last_probe >= KEEPALIVE_MS) {
            send_probe(c, p, &p->path);
            p->last_probe = now;
        }

        /* reliable lane: (re)send anything unacked; back off per try, give up after 30 s */
        if (p->nrtx) {
            /* these messages are tiny and rare, so be aggressive: a duplicate 40 ms after the
             * first send, then retries from ~2x RTT growing 1.5x per try, capped at 1 s -
             * dplayx's own join timers expire long before slow TCP-style backoff would */
            DWORD base = 2 * p->rtt + 50;
            if (base < 150) base = 150;
            int k = 0;
            for (int j = 0; j < p->nrtx; j++) {
                okp_rtx *r = &p->rtx[j];
                /* at least as long as DirectPlay waits before dropping the machine (drop_after=):
                 * its own messages from during an outage must still arrive afterwards */
                if (now - r->first > (DWORD)(okp_drop_after_ms() + 15000)) {
                    uint16_t cmd = 0;
                    if (r->len >= (int)sizeof(okp_wire) + 10) memcpy(&cmd, r->pkt + sizeof(okp_wire) + 8, 2);
                    okp_log(1, "peer %016llx: gave up on reliable seq %lu (cmd %u) after %d tries",
                            (unsigned long long)p->id, (unsigned long)r->seq, cmd, r->tries);
                    free(r->pkt);
                    p->r_giveup++;
                    continue;
                }
                DWORD rto = base;
                for (int t = 2; t < r->tries && rto < 1000; t++) rto = rto * 3 / 2;
                if (r->tries == 1) rto = 40;
                if (rto > 1000) rto = 1000;
                if (p->have_path && (r->tries == 0 || now - r->last >= rto)) {
                    peer_send(c, p, &p->path, r->pkt, r->len);
                    if (r->tries > 0) p->r_retrans++;
                    r->tries++;
                    r->last = now;
                    p->tx_pkts++;
                    p->tx_bytes += r->len;
                }
                p->rtx[k++] = *r;
            }
            p->nrtx = k;
        }
        rel_drain_locked(c, p);   /* give-ups free slots too */

        /* age out queued packets that never found a path */
        int k = 0;
        for (int j = 0; j < p->qn; j++) {
            if (now - p->q[j].t > QUEUE_MAX_AGE_MS) { free(p->q[j].data); p->dropped_q++; }
            else p->q[k++] = p->q[j];
        }
        p->qn = k;
    }
    LeaveCriticalSection(&c->lock);
}

static DWORD WINAPI net_main(LPVOID arg)
{
    okp_ctx *c = (okp_ctx *)arg;
    uint8_t *buf = (uint8_t *)malloc(OKP_MAX_PKT + 64);
    while (c->running) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(c->sock, &rs);
        SOCKET lq = c->lan_q, ll = c->lan_listen;
        if (lq != INVALID_SOCKET) FD_SET(lq, &rs);
        if (ll != INVALID_SOCKET) FD_SET(ll, &rs);
        struct timeval tv = {0, 20000};
        int r = select(0, &rs, NULL, NULL, &tv);
        if (r > 0 && lq != INVALID_SOCKET && FD_ISSET(lq, &rs)) okp_lan_readable(c, lq);
        if (r > 0 && ll != INVALID_SOCKET && FD_ISSET(ll, &rs)) okp_lan_readable(c, ll);
        if (r > 0 && FD_ISSET(c->sock, &rs)) {
            /* socket is non-blocking: drain until WSAEWOULDBLOCK */
            for (;;) {
                SOCKADDR_IN6 from;
                int n = okp_recvfrom6(c->sock, c->v4only, buf, OKP_MAX_PKT + 64, &from);
                if (n < 0) {
                    int e = WSAGetLastError();
                    if (e == WSAEWOULDBLOCK) break;
                    if (e == WSAEMSGSIZE || e == WSAECONNRESET) continue;
                    okp_log(1, "recvfrom: %d", e);
                    break;
                }
                if (from.sin6_family != AF_INET6) continue;
                if (file_blackout()) continue;   /* (test knob: nothing arrives either) */
                if (nat_on) {   /* only the simulator talks to us; unwrap the real source */
                    if (n < 22 || memcmp(buf, "NATR", 4) != 0 || !okp_addr_eq(&from, &nat_router)) continue;
                    memset(&from, 0, sizeof(from));
                    from.sin6_family = AF_INET6;
                    memcpy(&from.sin6_addr, buf + 4, 16);
                    memcpy(&from.sin6_port, buf + 20, 2);
                    handle_packet(c, buf + 22, n - 22, &from);
                    continue;
                }
                handle_packet(c, buf, n, &from);
            }
        }
        run_timers(c);
    }
    free(buf);
    return 0;
}

/* ---------------------------------------------------------- start / stop */

int okp_net_start(okp_ctx *c)
{
    if (c->sock != INVALID_SOCKET) return 1;
    okp_sys_load();
    char nonet[4];   /* test knob OKP_TEST_NONET=1: behave as if TCP/IP weren't installed */
    int no_tcpip = GetEnvironmentVariableA("OKP_TEST_NONET", nonet, sizeof(nonet)) && nonet[0] == '1';
    SOCKET s = okp_sys.emulate9x || no_tcpip ? INVALID_SOCKET : socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    c->v4only = 0;
    if (s == INVALID_SOCKET) {
        /* no IPv6 on this system (Windows 9x, XP without it installed): IPv4 only */
        int e6 = no_tcpip ? WSAEAFNOSUPPORT : WSAGetLastError();
        s = no_tcpip ? INVALID_SOCKET : socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (no_tcpip) WSASetLastError(WSAEAFNOSUPPORT);
        if (s == INVALID_SOCKET) {
            int e4 = WSAGetLastError();
            okp_log(0, "socket: IPv6 %d, IPv4 %d", e6, e4);
#ifdef OKP_BUILTIN
            DP_BUILTIN_NetworkUnavailable(e4);   /* tell the player why, not just the game */
#endif
            return 0;
        }
        c->v4only = 1;
        okp_log(1, "%s: no IPv6 on this system (%d) - IPv4 only", c->mode_name, e6);
    } else {
        DWORD off = 0;
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&off, sizeof(off));
    }
    okp_udp_no_connreset(s);
    int rcvbuf = 256 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (char *)&rcvbuf, sizeof(rcvbuf));
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);

    /* the port sits at the same offset in SOCKADDR_IN and SOCKADDR_IN6 */
    SOCKADDR_IN6 a;
    int al = c->v4only ? (int)sizeof(SOCKADDR_IN) : (int)sizeof(a);
    memset(&a, 0, sizeof(a));
    a.sin6_family = c->v4only ? AF_INET : AF_INET6;
    a.sin6_port = htons((uint16_t)okp_cfg.port);
    if (bind(s, (SOCKADDR *)&a, al) != 0 && okp_cfg.port) {
        okp_log(1, "bind port %d failed (%d), using a random port", okp_cfg.port, WSAGetLastError());
        a.sin6_port = 0;
        bind(s, (SOCKADDR *)&a, al);
    }
    al = sizeof(a);
    getsockname(s, (SOCKADDR *)&a, &al);
    c->sock = s;
    c->port = ntohs(a.sin6_port);
    okp_log(1, "%s: udp socket bound to port %u (%s)", c->mode_name, c->port, c->v4only ? "IPv4 only" : "dual-stack");

    nat_init();
    okp_gather_cands(c);
    for (int i = 0; i < 12; i++) c->stun_txid[i] = (uint8_t)okp_rand64();

    c->running = 1;
    c->net_thread = CreateThread(NULL, 0, net_main, c, 0, NULL);
    return c->net_thread != NULL;
}

void okp_net_stop(okp_ctx *c)
{
    c->running = 0;
    if (c->net_thread) {
        if (WaitForSingleObject(c->net_thread, 3000) == WAIT_TIMEOUT)
            TerminateThread(c->net_thread, 0);
        CloseHandle(c->net_thread);
        c->net_thread = NULL;
    }
    if (c->sock != INVALID_SOCKET) {
        closesocket(c->sock);
        c->sock = INVALID_SOCKET;
    }
    okp_lan_close(c);
    for (int i = 0; i < OKP_MAX_PEERS; i++) {
        okp_peer_free_bufs(&c->peers[i]);
        c->peers[i].used = 0;
    }
}
