/* okplay - DirectPlay service provider entry point and callbacks (Online Play + LAN Play).
 *
 * dplayx hands us opaque DirectPlay messages plus player ids; we move the bytes.
 *   - player "addresses" are peer ids stored in shared SP player data
 *   - hosting: rdv thread keeps a room listed with the enum reply dplayx
 *     produced for our own synthetic enum request (self-probe)
 *   - joining: EnumSessions pulls the room list and injects each host's cached
 *     enum reply; Open(join) knocks + punches to the host before returning
 */
#define INITGUID
#include "okp.h"
#include <stdio.h>
#include <string.h>

#define DPENUM_REQ_SIZE 32   /* envelope(8) + guidApplication(16) + passwordOffset(4) + flags(4) */
#define DP_DEFAULT_VERSION 14

static okp_ctx *ctx_of(IDirectPlaySP *isp)
{
    okp_ctx **pp = NULL;
    DWORD sz = 0;
    if (!isp || IDirectPlaySP_GetSPData(isp, (void **)&pp, &sz, DPGET_LOCAL) != DP_OK ||
        !pp || sz != sizeof(okp_ctx *))
        return NULL;
    return *pp;
}

int okp_room_build(IDirectPlaySP *isp, const GUID *instance, char build[17], char *exe, int exesz)
{
    okp_ctx *c = ctx_of(isp);
    int found = 0;
    if (!c || !instance) return 0;
    EnterCriticalSection(&c->lock);
    for (int i = 0; i < c->nrooms && !found; i++) {
        okp_room *rm = &c->rooms[i];
        /* the room's session record: DPSESSIONDESC2 after the 8-byte envelope, instance at +8 */
        if (rm->enumlen >= 8 + 24 && rm->build[0] && !memcmp(rm->enumblob + 8 + 8, instance, sizeof(GUID))) {
            lstrcpynA(build, rm->build, 17);
            lstrcpynA(exe, rm->exe, exesz);
            found = 1;
        }
    }
    LeaveCriticalSection(&c->lock);
    return found;
}

static void hexdump(const char *what, const uint8_t *b, int n)
{
    if (okp_loglevel < 3) return;
    char line[3 * 48 + 1];
    for (int off = 0; off < n && off < 256; off += 48) {
        int k = 0;
        for (int i = off; i < n && i < off + 48; i++) k += snprintf(line + k, sizeof(line) - k, "%02x ", b[i]);
        okp_log(3, "%s +%03d: %s", what, off, line);
    }
}

static int ensure_started(okp_ctx *c)
{
    okp_transport_refresh();   /* transport= can change between games (okp.h) */
    if (!okp_net_start(c)) return 0;
    return okp_rdv_start(c);
}

static void wait_srflx(okp_ctx *c, DWORD ms)
{
    if (!okp_cfg.stun[0]) return;
    DWORD t0 = okp_now();
    while (!c->have_srflx && okp_now() - t0 < ms) Sleep(20);
}

static uint16_t dp_version(okp_ctx *c) { return c->dp_version ? c->dp_version : DP_DEFAULT_VERSION; }

/* ------------------------------------------------------------ enum logic */

void okp_self_probe(okp_ctx *c)
{
    uint8_t req[DPENUM_REQ_SIZE];
    memset(req, 0, sizeof(req));
    uint32_t magic = 0x79616c70;          /* "play" */
    uint16_t cmd = 2;                     /* ENUMSESSIONSREQUEST */
    uint16_t ver = dp_version(c);
    uint32_t flags = DPENUMSESSIONS_ALL;  /* reply even when full/started: we show status */
    memcpy(req, &magic, 4);
    memcpy(req + 4, &cmd, 2);
    memcpy(req + 6, &ver, 2);
    /* guidApplication = GUID_NULL (any), passwordOffset = 0 */
    memcpy(req + 28, &flags, 4);

    okp_sphdr h;
    h.magic = OKP_HDR_MAGIC;
    h.peer = c->self;
    h.room = c->room;
    h.flags = OKP_HF_SELFPROBE;
    IDirectPlaySP_HandleMessage(c->isp, req, sizeof(req), &h);
}

void okp_capture_enum_reply(okp_ctx *c, const uint8_t *msg, int len)
{
    if (len <= 0 || len > (int)sizeof(c->enum_blob)) return;
    EnterCriticalSection(&c->lock);
    int changed = len != c->enum_len || memcmp(c->enum_blob, msg, len) != 0;
    memcpy(c->enum_blob, msg, len);
    c->enum_len = len;
    LeaveCriticalSection(&c->lock);
    if (changed) {
        DWORD cur = 0, maxp = 0, fl = 0;
        if (len >= 8 + 80) {
            memcpy(&fl, msg + 8 + 4, 4);
            memcpy(&maxp, msg + 8 + 40, 4);
            memcpy(&cur, msg + 8 + 44, 4);
        }
        okp_log(1, "host: session state %lu/%lu players, flags 0x%lx (%d bytes)",
                (unsigned long)cur, (unsigned long)maxp, (unsigned long)fl, len);
        hexdump("enum reply", msg, len);
    }
}

/* Does a cached enum reply pass the joiner's EnumSessions filter? */
static int room_matches(okp_ctx *c, const okp_room *rm)
{
    if (rm->enumlen < 8 + 80) return 0;
    const uint8_t *sd = rm->enumblob + 8;
    DWORD fl, maxp, cur;
    GUID app;
    memcpy(&fl, sd + 4, 4);
    memcpy(&app, sd + 24, 16);
    memcpy(&maxp, sd + 40, 4);
    memcpy(&cur, sd + 44, 4);
    static const GUID any_app;
    if (!IsEqualGUID(&c->enum_app, &any_app) && !IsEqualGUID(&c->enum_app, &app)) return 0;
    if (!(c->enum_flags & DPENUMSESSIONS_ALL)) {
        if (fl & (DPSESSION_NEWPLAYERSDISABLED | DPSESSION_JOINDISABLED)) return 0;
        if (maxp && cur >= maxp) return 0;
    }
    return 1;
}

void okp_inject_rooms(okp_ctx *c)
{
    okp_room *copy = (okp_room *)malloc(sizeof(okp_room) * OKP_MAX_ROOMS);
    if (!copy) return;
    EnterCriticalSection(&c->lock);
    int n = c->nrooms;
    memcpy(copy, c->rooms, sizeof(okp_room) * n);
    LeaveCriticalSection(&c->lock);

    DWORD now = okp_now();
    for (int i = 0; i < n; i++) {
        if (c->mode == OKP_MODE_LAN && now - copy[i].seen > OKP_LAN_ROOM_TTL) continue;
        if (!okp_enum_blob_ok(copy[i].enumblob, copy[i].enumlen)) continue;   /* never feed dplayx junk */
        if (!room_matches(c, &copy[i])) continue;
        okp_sphdr h;
        h.magic = OKP_HDR_MAGIC;
        h.peer = copy[i].host;
        h.room = copy[i].room;
        h.flags = 0;
        IDirectPlaySP_HandleMessage(c->isp, copy[i].enumblob, copy[i].enumlen, &h);
    }
    free(copy);
}

/* ------------------------------------------------------------- callbacks */

static HRESULT OKP_EnumSessions_impl(LPDPSP_ENUMSESSIONSDATA d)
{
    okp_ctx *c = ctx_of(d->lpISP);
    if (!c) return DPERR_GENERIC;
    const uint8_t *msg = (const uint8_t *)d->lpMessage + OKP_HDR_SIZE;
    int len = (int)d->dwMessageSize - (int)OKP_HDR_SIZE;

    if (len >= DPENUM_REQ_SIZE) {
        uint16_t ver;
        memcpy(&ver, msg + 6, 2);
        if (!c->dp_version) {
            c->dp_version = ver;
            okp_log(2, "dplayx message version %u", ver);
            hexdump("enum request", msg, len);
        }
        memcpy(&c->enum_app, msg + 8, 16);
        memcpy(&c->enum_flags, msg + 28, 4);
    }
    if (!ensure_started(c)) return DPERR_GENERIC;

    if (c->mode == OKP_MODE_LAN && okp_now() - c->last_discover >= 300) okp_lan_discover(c);
    c->enum_pending = 1;
    okp_rdv_wake(c);
    okp_inject_rooms(c);          /* answer instantly from the last listing */
    return DP_OK;
}

static HRESULT OKP_Reply_impl(LPDPSP_REPLYDATA d)
{
    okp_ctx *c = ctx_of(d->lpISP);
    okp_sphdr *h = (okp_sphdr *)d->lpSPMessageHeader;
    if (!c || !h || h->magic != OKP_HDR_MAGIC) {
        okp_log(1, "Reply without our header (ns=%lu)", (unsigned long)d->idNameServer);
        return DPERR_GENERIC;
    }
    const uint8_t *msg = (const uint8_t *)d->lpMessage + OKP_HDR_SIZE;
    int len = (int)d->dwMessageSize - (int)OKP_HDR_SIZE;

    if (h->flags & OKP_HF_SELFPROBE) {
        okp_capture_enum_reply(c, msg, len);
        return DP_OK;
    }
    if (len >= 8) {
        uint16_t cmd;
        memcpy(&cmd, msg + 4, 2);
        okp_log(2, "Reply cmd %u (%d bytes) -> peer %016llx", cmd, len, (unsigned long long)h->peer);
        okp_trace("tx-reply", h->peer, msg, len);
    }
    return okp_send_data(c, h->peer, msg, len, 0) ? DP_OK : DPERR_GENERIC;
}

static int peer_of_player(okp_ctx *c, DPID id, uint64_t *out)
{
    void *buf = NULL;
    DWORD sz = 0;
    SOCKADDR_IN6 unused[OKP_PDATA_CANDS];
    if (IDirectPlaySP_GetSPPlayerData(c->isp, id, &buf, &sz, DPGET_REMOTE) != DP_OK) return 0;
    return okp_pdata_parse(buf, sz, out, unused, OKP_PDATA_CANDS) >= 0;
}

static HRESULT OKP_Send_impl(LPDPSP_SENDDATA d)
{
    okp_ctx *c = ctx_of(d->lpISP);
    if (!c) return DPERR_GENERIC;
    const uint8_t *msg = (const uint8_t *)d->lpMessage + OKP_HDR_SIZE;
    int len = (int)d->dwMessageSize - (int)OKP_HDR_SIZE;
    uint64_t to = 0;

    if (d->idPlayerTo == 0) {
        to = c->host_peer;
    } else if (!peer_of_player(c, d->idPlayerTo, &to)) {
        okp_log(1, "Send to player %lu: no okplay player data", (unsigned long)d->idPlayerTo);
        return DPERR_INVALIDPLAYER;
    }
    if (to == 0 || to == c->self) {
        okp_log(1, "Send to player %lu: no remote peer (to=%016llx)", (unsigned long)d->idPlayerTo,
                (unsigned long long)to);
        return DPERR_INVALIDPLAYER;
    }
    if (d->bSystemMessage && len >= 8 && okp_loglevel >= 2) {
        uint16_t cmd;
        memcpy(&cmd, msg + 4, 2);
        okp_trace("tx-send", to, msg, len);
        okp_log(2, "Send sys cmd %u (%d bytes, flags 0x%lx) -> player %lu peer %016llx", cmd, len,
                (unsigned long)d->dwFlags, (unsigned long)d->idPlayerTo, (unsigned long long)to);
    }
    return okp_send_data(c, to, msg, len, (d->dwFlags & DPSEND_GUARANTEED) ? OKP_SEND_GUARANTEED : 0) ? DP_OK
                                                                                                : DPERR_GENERIC;
}

static HRESULT OKP_CreatePlayer_impl(LPDPSP_CREATEPLAYERDATA d)
{
    okp_ctx *c = ctx_of(d->lpISP);
    if (!c) return DPERR_GENERIC;

    if (d->dwFlags & DPLAYI_PLAYER_PLAYERLOCAL) {
        okp_pdata pd;
        memset(&pd, 0, sizeof(pd));
        pd.magic = OKP_PD_MAGIC;
        pd.peer = c->self;
        SOCKADDR_IN6 lc[OKP_MAX_CANDS];
        int n;
        okp_local_cands(c, lc, &n, OKP_PDATA_CANDS);
        for (int i = 0; i < n; i++) {
            okp_ccand *cc = &pd.cands[i];
            cc->port = lc[i].sin6_port;
            if (okp_addr_is_v4(&lc[i])) {
                cc->fam = 4;
                memcpy(cc->addr, ((uint8_t *)&lc[i].sin6_addr) + 12, 4);
            } else {
                cc->fam = 6;
                memcpy(cc->addr, &lc[i].sin6_addr, 16);
            }
        }
        pd.ncands = (uint8_t)n;
        HRESULT hr = IDirectPlaySP_SetSPPlayerData(d->lpISP, d->idPlayer, &pd, sizeof(pd), DPSET_REMOTE);
        okp_log(1, "CreatePlayer local %lu flags 0x%lx (%d cands) -> 0x%08lx", (unsigned long)d->idPlayer,
                (unsigned long)d->dwFlags, n, (unsigned long)hr);
        return hr;
    }

    void *buf = NULL;
    DWORD sz = 0;
    SOCKADDR_IN6 cl[OKP_PDATA_CANDS];
    uint64_t peer = 0;
    int n = -1;
    if (IDirectPlaySP_GetSPPlayerData(d->lpISP, d->idPlayer, &buf, &sz, DPGET_REMOTE) == DP_OK)
        n = okp_pdata_parse(buf, sz, &peer, cl, OKP_PDATA_CANDS);
    if (n >= 0) {
        if (peer != c->self) {
            EnterCriticalSection(&c->lock);
            okp_peer *p = okp_peer_get(c, peer, 1);
            if (p) { okp_peer_add_cands(c, p, cl, n); p->nplayers++; }
            LeaveCriticalSection(&c->lock);
        }
        okp_log(1, "CreatePlayer remote %lu flags 0x%lx peer %016llx (%d cands)", (unsigned long)d->idPlayer,
                (unsigned long)d->dwFlags, (unsigned long long)peer, n);
    } else {
        okp_log(1, "CreatePlayer remote %lu flags 0x%lx: no okplay player data", (unsigned long)d->idPlayer,
                (unsigned long)d->dwFlags);
    }
    return DP_OK;
}

static HRESULT OKP_DeletePlayer_impl(LPDPSP_DELETEPLAYERDATA d)
{
    okp_ctx *c = ctx_of(d->lpISP);
    if (!c) return DP_OK;
    uint64_t peer = 0;
    if (!peer_of_player(c, d->idPlayer, &peer) || peer == c->self) {
        okp_log(2, "DeletePlayer %lu flags 0x%lx (local or no player data)", (unsigned long)d->idPlayer,
                (unsigned long)d->dwFlags);
        return DP_OK;
    }
    int left = -1;
    EnterCriticalSection(&c->lock);
    okp_peer *p = okp_peer_get(c, peer, 0);
    if (p) {
        if (p->nplayers > 0) p->nplayers--;
        left = p->nplayers;
        if (left == 0) p->keep_until = okp_now() + 5000;
    }
    LeaveCriticalSection(&c->lock);
    okp_log(1, "DeletePlayer %lu flags 0x%lx peer %016llx (%d player(s) left on that peer)", (unsigned long)d->idPlayer,
            (unsigned long)d->dwFlags, (unsigned long long)peer, left);
    return DP_OK;
}

static void reset_session(okp_ctx *c)
{
    EnterCriticalSection(&c->lock);
    for (int i = 0; i < OKP_MAX_PEERS; i++) {
        okp_peer_free_bufs(&c->peers[i]);
        c->peers[i].qn = 0;
        c->peers[i].used = 0;
    }
    c->enum_len = 0;
    c->host_peer = 0;
    c->is_host = 0;
    c->host_transport = -1;   /* the next host tells us its transport */
    LeaveCriticalSection(&c->lock);
}

static HRESULT OKP_Open_impl(LPDPSP_OPENDATA d)
{
    okp_ctx *c = ctx_of(d->lpISP);
    if (!c || !ensure_started(c)) return DPERR_GENERIC;
    reset_session(c);
    c->closing = 0;
    okp_report_session_begin(c, d->bCreate);
    if (c->mode == OKP_MODE_ONLINE) wait_srflx(c, 1500);

    if (d->bCreate) {
        c->room = (uint32_t)okp_rand64() | 1;
        snprintf(c->tok, sizeof(c->tok), "%016llx", (unsigned long long)okp_rand64());
        c->is_host = 1;
        c->session_open = 1;
        if (c->mode == OKP_MODE_LAN && !okp_lan_open_listen(c))
            okp_log(0, "Open: LAN discovery port %u unavailable - joiners will not see this game", OKP_LAN_PORT);
        okp_log(1, "Open: %s hosting room %08x as peer %016llx", c->mode_name, c->room, (unsigned long long)c->self);
        okp_rdv_wake(c);
        return DP_OK;
    }

    okp_sphdr *h = (okp_sphdr *)d->lpSPMessageHeader;
    if (!h || h->magic != OKP_HDR_MAGIC) {
        okp_log(0, "Open(join): session header is not ours");
        return DPERR_GENERIC;
    }
    c->room = h->room;
    c->host_peer = h->peer;

    SOCKADDR_IN6 cl[OKP_MAX_CANDS];
    int n = 0;
    EnterCriticalSection(&c->lock);
    for (int i = 0; i < c->nrooms; i++) {
        if (c->rooms[i].room == h->room && c->rooms[i].host == h->peer) {
            n = c->rooms[i].ncands;
            memcpy(cl, c->rooms[i].cands, sizeof(cl[0]) * n);
        }
    }
    okp_peer *p = okp_peer_get(c, h->peer, 1);
    if (p) { okp_peer_add_cands(c, p, cl, n); p->probe_start = okp_now(); }
    LeaveCriticalSection(&c->lock);

    okp_log(1, "Open: joining room %08x host %016llx (%d cands)", h->room, (unsigned long long)h->peer, n);
    /* LAN: the host hears our probes directly. Online: the knock tells the host to probe us - behind
     * some NATs (a VM's, a strict router) nothing connects until it does, so a lost knock (a server
     * hiccup) meant a failed join: knock again every 3 s while there's no path. */
    int have = 0;
    for (int waited = 0; waited < 10000 && !have; waited += 3000) {
        if (c->mode == OKP_MODE_ONLINE) okp_rdv_knock(c, h->room);
        have = okp_wait_path(c, h->peer, waited + 3000 > 10000 ? 10000 - waited : 3000);
    }
    if (!have) {
        okp_log(0, "Open: no path to host within 10s");
        okp_report_session_result(c, "no_path");
        okp_report_session_end(c);   /* dplayx doesn't call CloseEx after a failed Open */
        return DPERR_NOCONNECTION;
    }
    c->session_open = 1;
    return DP_OK;
}

static HRESULT OKP_CloseEx_impl(LPDPSP_CLOSEDATA d)
{
    okp_ctx *c = ctx_of(d->lpISP);
    if (!c) return DP_OK;
    okp_log(1, "CloseEx");
    c->closing = 1;
    c->session_open = 0;
    okp_report_session_end(c);
    okp_rdv_wake(c);
    return DP_OK;
}

static HRESULT OKP_ShutdownEx_impl(LPDPSP_SHUTDOWNDATA d)
{
    okp_ctx *c = ctx_of(d->lpISP);
    if (!c) return DP_OK;
    okp_log(1, "ShutdownEx");
    c->closing = 1;
    c->session_open = 0;
    okp_report_session_end(c);   /* (no-op after CloseEx) */
    c->running = 0;
    okp_rdv_stop(c);
    okp_net_stop(c);
    DeleteCriticalSection(&c->lock);
    okp_ctx *null = NULL;
    IDirectPlaySP_SetSPData(d->lpISP, &null, sizeof(null), DPSET_LOCAL);
    free(c);
    return DP_OK;
}

static HRESULT OKP_GetCaps_impl(LPDPSP_GETCAPSDATA d)
{
    if (d->lpCaps->dwSize < sizeof(DPCAPS)) return DPERR_INVALIDPARAMS;
    /* guaranteed messages go over our reliable in-order lane: the provider does it itself, as
       Windows' TCP/IP provider reports (0x10020); DirectPlay adds "supported", host and local bits */
    d->lpCaps->dwFlags = DPCAPS_ASYNCSUPPORTED | DPCAPS_GUARANTEEDOPTIMIZED;
    d->lpCaps->dwMaxBufferSize = OKP_CAPS_MAXBUF;   /* we split big messages ourselves - see okp.h */
    d->lpCaps->dwMaxQueueSize = 0;
    d->lpCaps->dwMaxPlayers = 65536;
    d->lpCaps->dwHundredBaud = 0;
    d->lpCaps->dwLatency = 50;
    d->lpCaps->dwMaxLocalPlayers = 65536;
    d->lpCaps->dwHeaderLength = OKP_HDR_SIZE;
    d->lpCaps->dwTimeout = 500;
    return DP_OK;
}

/* Every callback goes through a wrapper that logs failures and slow calls, so a
 * frozen or failing game screen can be matched to what DirectPlay asked of us. */
#define SLOW_MS 150
#define WRAP(name, type, expect_slow)                                                           \
    static HRESULT WINAPI name(type d)                                                         \
    {                                                                                          \
        DWORD t0 = okp_now();                                                                  \
        HRESULT hr = name##_impl(d);                                                           \
        DWORD dt = okp_now() - t0;                                                             \
        if (dt > SLOW_MS && !(expect_slow))                                                    \
            okp_log(1, #name " SLOW: %lums (hr 0x%08lx)", (unsigned long)dt, (unsigned long)hr); \
        else if (expect_slow)                                                                  \
            okp_log(2, #name " took %lums (hr 0x%08lx)", (unsigned long)dt, (unsigned long)hr); \
        if (FAILED(hr))                                                                        \
            okp_log(1, #name " FAILED: hr 0x%08lx", (unsigned long)hr);                        \
        return hr;                                                                             \
    }
WRAP(OKP_EnumSessions, LPDPSP_ENUMSESSIONSDATA, 0)
WRAP(OKP_Reply, LPDPSP_REPLYDATA, 0)
WRAP(OKP_Send, LPDPSP_SENDDATA, 0)
WRAP(OKP_CreatePlayer, LPDPSP_CREATEPLAYERDATA, 0)
WRAP(OKP_DeletePlayer, LPDPSP_DELETEPLAYERDATA, 0)
WRAP(OKP_Open, LPDPSP_OPENDATA, 1)
WRAP(OKP_CloseEx, LPDPSP_CLOSEDATA, 0)
WRAP(OKP_ShutdownEx, LPDPSP_SHUTDOWNDATA, 1)
WRAP(OKP_GetCaps, LPDPSP_GETCAPSDATA, 0)

static HMODULE g_hinst;

__declspec(dllexport) HRESULT WINAPI SPInit(LPSPINITDATA d)
{
    okp_crash_install(g_hinst);   /* first: it records our module for the header */
    okp_log_header();
    okp_log_sysinfo();            /* loads the optional DLLs - not allowed in DllMain */
    okp_reports_kick();           /* report.c: a crash report or connection reports waiting (asks first) */
    if (GetEnvironmentVariableA("OKP_CRASHTEST", NULL, 0)) {
        /* proves the crash capture works: set OKP_CRASHTEST=1 and run dptest */
        okp_log(0, "OKP_CRASHTEST set: faulting on purpose");
        volatile uintptr_t bad = 0x10;
        *(volatile int *)bad = 1;
    }
    int mode;
    char test_as[16] = "";
    GetEnvironmentVariableA("OKP_TEST_AS", test_as, sizeof(test_as));
    if (IsEqualGUID(d->lpGuid, &OKP_SPGUID)) {
        mode = OKP_MODE_ONLINE;
    } else if (IsEqualGUID(d->lpGuid, &OKP_LANGUID)) {
        mode = OKP_MODE_LAN;
    } else if (test_as[0] && IsEqualGUID(d->lpGuid, &DPSPGUID_IPX)) {
        /* dev loop without admin: dptest loads bin\dpwsockx.dll (a copy of this DLL)
         * through the IPX provider slot, whose registry Path is a bare file name */
        mode = strcmp(test_as, "lan") == 0 ? OKP_MODE_LAN : OKP_MODE_ONLINE;
        okp_log(1, "SPInit: TEST MODE via the IPX slot as %s", mode == OKP_MODE_LAN ? "LAN Play" : "Online Play");
    } else {
        okp_log(0, "SPInit: unexpected provider GUID");
        return DPERR_UNAVAILABLE;
    }
    okp_ctx *c = (okp_ctx *)calloc(1, sizeof(okp_ctx));
    if (!c) return DPERR_OUTOFMEMORY;
    c->host_transport = -1;
    InitializeCriticalSection(&c->lock);
    c->isp = d->lpISP;
    c->mode = mode;
    c->mode_name = mode == OKP_MODE_LAN ? "LAN Play" : "Online Play";
    c->sock = INVALID_SOCKET;
    c->lan_q = INVALID_SOCKET;
    c->lan_listen = INVALID_SOCKET;
    c->self = okp_rand64();

    HRESULT hr = IDirectPlaySP_SetSPData(d->lpISP, &c, sizeof(c), DPSET_LOCAL);
    if (hr != DP_OK) {
        DeleteCriticalSection(&c->lock);
        free(c);
        return DPERR_UNAVAILABLE;
    }
    d->lpCB->EnumSessions = OKP_EnumSessions;
    d->lpCB->Reply = OKP_Reply;
    d->lpCB->Send = OKP_Send;
    d->lpCB->CreatePlayer = OKP_CreatePlayer;
    d->lpCB->DeletePlayer = OKP_DeletePlayer;
    d->lpCB->GetCaps = OKP_GetCaps;
    d->lpCB->Open = OKP_Open;
    d->lpCB->CloseEx = OKP_CloseEx;
    d->lpCB->ShutdownEx = OKP_ShutdownEx;
    d->dwSPHeaderSize = OKP_HDR_SIZE;
    d->dwSPVersion = DPSP_MAJORVERSIONMASK & DPSP_MAJORVERSION;
    if (mode == OKP_MODE_LAN)
        okp_log(1, "SPInit: okplay " OKP_VERSION_STR " LAN Play, peer %016llx (no server, no internet)",
                (unsigned long long)c->self);
    else
        okp_log(1, "SPInit: okplay " OKP_VERSION_STR " Online Play, peer %016llx server %s",
                (unsigned long long)c->self, okp_cfg.server);
    return DP_OK;
}

/* process start: kernel32/ws2_32 work only (runs inside DllMain) */
static void okp_attach(HINSTANCE hinst)
{
    g_hinst = hinst;
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    okp_log_init(hinst);
}

#ifdef OKP_BUILTIN
/* built into dplayx.dll: DirectPlay's host migration made THIS machine the host (the old host left).
 * A real service provider is never told; ours must be, or the game drops off the server list (MTM2
 * hands the game on when its host quits). Keep the room id - every packet carries it and the players
 * still in the game drop any other (a new id cut them off, both sides then thought the other gone
 * and each became host) - and announce it with our own token: a host that left closed the room on
 * the server, a crashed one's expires in 15 s, then ours takes its place. LAN Play: start answering
 * discovery. */
void okp_became_host(IDirectPlaySP *isp)
{
    okp_ctx *c = ctx_of(isp);
    if (!c || !c->session_open || c->closing || c->is_host) return;
    EnterCriticalSection(&c->lock);
    snprintf(c->tok, sizeof(c->tok), "%016llx", (unsigned long long)okp_rand64());
    c->host_peer = 0;
    c->enum_len = 0;          /* refreshed from the new host's own enum reply */
    c->is_host = 1;
    LeaveCriticalSection(&c->lock);
    if (c->mode == OKP_MODE_LAN && !okp_lan_open_listen(c))
        okp_log(0, "host migration: LAN discovery port %u unavailable - joiners will not see this game", OKP_LAN_PORT);
    okp_log(1, "host migration: this PC is the new host - %s room %08x as peer %016llx", c->mode_name, c->room,
            (unsigned long long)c->self);
    okp_rdv_wake(c);
}

/* built into dplayx.dll: DirectPlay's join failed after our Open got a path to the host (the host
 * refused, or the request went unanswered) - for the connection report. dplayx calls CloseEx next. */
void okp_join_failed(IDirectPlaySP *isp, const char *step, HRESULT hr)
{
    okp_ctx *c = ctx_of(isp);
    char r[32];
    if (!c) return;
    snprintf(r, sizeof(r), "%s_%08lx", step, (unsigned long)hr);
    okp_report_session_result(c, r);
}

/* built into dplayx.dll: dplay.c asks who sent a message before acting on it. The SP header we attach
 * to every delivered message names the sending peer (handle_packet only delivers packets that carry
 * the token we gave that peer); a machine's system player carries its peer id in its SP data. */
uint64_t okp_header_peer(const void *hdr, DWORD hdrsize)
{
    const okp_sphdr *h = (const okp_sphdr *)hdr;
    if (!h || hdrsize != OKP_HDR_SIZE || h->magic != OKP_HDR_MAGIC) return 0;
    return h->peer;
}

uint64_t okp_spdata_peer(const void *data, DWORD size)
{
    uint64_t peer = 0;
    SOCKADDR_IN6 unused[OKP_PDATA_CANDS];
    return okp_pdata_parse(data, size, &peer, unused, OKP_PDATA_CANDS) >= 0 ? peer : 0;
}

/* built into dplayx.dll: its DllMain calls this. Crash capture starts with the game (not at the
 * first multiplayer menu), so a crash anywhere in a session is reported. */
void okp_builtin_attach(HINSTANCE hinst)
{
    okp_attach(hinst);
    okp_crash_install(hinst);
    okp_log_header();
}
#else
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        okp_attach(hinst);
    }
    return TRUE;
}
#endif
