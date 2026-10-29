/* okweb_sp.c - "Oldkid.net Online Play" for our DirectPlay in the browser: the service provider.
 *
 * The browser counterpart of okplay/src/sp.c, and deliberately the same shape: DirectPlay hands
 * us whole messages; we carry them to the right machine and hand incoming ones back with
 * IDirectPlaySP_HandleMessage. What okplay does with UDP, hole punching and its reliable lane,
 * okweb.js does with WebRTC (okweb-sp.js is the JS half: rooms, connections, queues).
 *
 *   EnumSessions  ask the room server for this game's rooms; each room carries the host's own
 *                 ENUMSESSIONSREPLY (captured by a self-probe, as okplay does), injected here
 *   Open(create)  host: ids picked now, the room announced from JS; the reply blob refreshed
 *                 every second (player counts change)
 *   Open(join)    connect to the room's host over WebRTC and wait for it (a yielding wait:
 *                 needs the program built with Asyncify, like DirectPlay's own reply waits)
 *   Send/Reply    to the machine of the player (its peer id rides in the player's SP data, as
 *                 okplay's does); DPSEND_GUARANTEED -> okweb's reliable lane, the rest -> the fast one
 *   incoming      queued in JS, delivered by the pump (winweb.c) - never from inside a JS callback */
#include "winweb.h"
#include <emscripten.h>
#include "dplay.h"
#include "wine/dplaysp.h"

#define OKW_HDR_MAGIC 0x57424B4Fu   /* "OKBW" */
#define OKW_HF_SELFPROBE 0x1u
#define OKW_PD_MAGIC 0x44574B4Fu    /* "OKWD": a player's SP data = its machine's peer id */
#define DPENUM_REQ_SIZE 32
#define DP_DEFAULT_VERSION 14
#define OKW_MAXBUF 16384            /* what GetCaps says (okweb carries up to 64 KB) */

#pragma pack(push, 1)
typedef struct { uint32_t magic; uint32_t peer_lo, peer_hi; uint32_t room; uint32_t flags; } okw_hdr;
typedef struct { uint32_t magic; uint32_t peer_lo, peer_hi; } okw_pdata;
#pragma pack(pop)
#define OKW_HDR_SIZE ((DWORD)sizeof(okw_hdr))   /* 20, as okplay's */

typedef struct okw_ctx {
    struct okw_ctx *next;
    IDirectPlaySP *isp;
    int id;                       /* the JS provider's handle */
    uint32_t self_lo, self_hi;    /* our peer id (okweb), set at Open */
    uint32_t room;
    uint32_t host_lo, host_hi;    /* the name server's machine (joined) */
    int is_host, open;
    uint8_t enum_blob[1024];      /* host: our own ENUMSESSIONSREPLY */
    int enum_len;
    uint16_t dp_version;
    GUID enum_app;
    DWORD enum_flags;
    DWORD last_probe;
    int rooms_seen;               /* the JS room list version last injected */
} okw_ctx;

static okw_ctx *g_ctxs;
static int g_next_id = 1;

/* ------------------------------------------------------------------ JS half (okweb-sp.js) */

#define OKW_JS(ret, name, args, body) EM_JS(ret, name, args, body)
OKW_JS(int, okw_js_ready, (void), { return (typeof Module !== 'undefined' && Module.okwebSP) ? 1 : 0; });
OKW_JS(void, okw_js_init, (int id), { Module.okwebSP.init(id); });
OKW_JS(void, okw_js_list, (int id, const char *app), { Module.okwebSP.list(id, UTF8ToString(app)); });
OKW_JS(int, okw_js_rooms, (int id), { return Module.okwebSP.rooms(id); });
OKW_JS(int, okw_js_rooms_version, (int id), { return Module.okwebSP.roomsVersion(id); });
OKW_JS(int, okw_js_room, (int id, int i, uint8_t *out, int cap, uint32_t *info),
       { return Module.okwebSP.room(id, i, HEAPU8, out, cap, info >> 2, HEAPU32); });
OKW_JS(void, okw_js_new_ids, (int id, uint32_t *out), { Module.okwebSP.newIds(id, HEAPU32, out >> 2); });
OKW_JS(void, okw_js_self, (int id, uint32_t *out), { Module.okwebSP.self(id, HEAPU32, out >> 2); });
OKW_JS(int, okw_js_host, (int id, const char *app, const uint8_t *blob, int len, const char *name, int maxp, int locked),
       { return Module.okwebSP.host(id, UTF8ToString(app), HEAPU8.slice(blob, blob + len), UTF8ToString(name), maxp, locked); });
OKW_JS(void, okw_js_set_enum, (int id, const uint8_t *blob, int len, int players, int maxp, int open, int locked),
       { Module.okwebSP.setEnum(id, HEAPU8.slice(blob, blob + len), players, maxp, open, locked); });
OKW_JS(void, okw_js_join, (int id, uint32_t room, uint32_t host_lo, uint32_t host_hi, const char *app),
       { Module.okwebSP.join(id, room >>> 0, host_lo >>> 0, host_hi >>> 0, UTF8ToString(app)); });
OKW_JS(int, okw_js_join_state, (int id), { return Module.okwebSP.joinState(id); });
OKW_JS(int, okw_js_send, (int id, uint32_t lo, uint32_t hi, const uint8_t *data, int len, int reliable),
       { return Module.okwebSP.send(id, lo >>> 0, hi >>> 0, HEAPU8.slice(data, data + len), reliable); });
OKW_JS(int, okw_js_next, (int id, uint8_t *out, int cap, uint32_t *info),
       { return Module.okwebSP.next(id, HEAPU8, out, cap, HEAPU32, info >> 2); });
OKW_JS(void, okw_js_close, (int id), { Module.okwebSP.close(id); });
OKW_JS(void, okw_js_log, (const char *s), { console.log('[okweb-sp]', UTF8ToString(s)); });

static void okw_log(const char *fmt, ...)
{
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    okw_js_log(buf);
}

static okw_ctx *ctx_of(IDirectPlaySP *isp)
{
    okw_ctx **pp = NULL;
    DWORD sz = 0;
    if (!isp || IDirectPlaySP_GetSPData(isp, (void **)&pp, &sz, DPGET_LOCAL) != DP_OK || !pp || sz != sizeof(okw_ctx *))
        return NULL;
    return *pp;
}

static void guid_hex(const GUID *g, char out[33])
{
    snprintf(out, 33, "%08lx%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x", (unsigned long)g->Data1, g->Data2, g->Data3,
             g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

static uint16_t dp_version(okw_ctx *c) { return c->dp_version ? c->dp_version : DP_DEFAULT_VERSION; }

/* never hand DirectPlay junk from the network (as okplay's okp_enum_blob_ok) */
static int enum_blob_ok(const uint8_t *b, int n)
{
    if (!b || n < 92 + 2 || n > 1024 || memcmp(b, "play", 4) != 0) return 0;
    uint16_t cmd;
    uint32_t sdsize, off;
    memcpy(&cmd, b + 4, 2);
    memcpy(&sdsize, b + 8, 4);
    memcpy(&off, b + 88, 4);
    if (cmd != 1 || sdsize != 80 || off < 92 || off >= (uint32_t)n) return 0;
    for (uint32_t i = off; i + 1 < (uint32_t)n; i += 2)
        if (b[i] == 0 && b[i + 1] == 0) return 1;
    return 0;
}

/* ------------------------------------------------------------------ host: our own enum reply */

static void self_probe(okw_ctx *c)
{
    uint8_t req[DPENUM_REQ_SIZE];
    uint32_t magic = 0x79616c70;   /* "play" */
    uint16_t cmd = 2, ver = dp_version(c);
    uint32_t flags = DPENUMSESSIONS_ALL;
    memset(req, 0, sizeof(req));
    memcpy(req, &magic, 4);
    memcpy(req + 4, &cmd, 2);
    memcpy(req + 6, &ver, 2);
    memcpy(req + 28, &flags, 4);
    okw_hdr h = { OKW_HDR_MAGIC, c->self_lo, c->self_hi, c->room, OKW_HF_SELFPROBE };
    IDirectPlaySP_HandleMessage(c->isp, req, sizeof(req), &h);
}

static void enum_info(const uint8_t *b, int n, DWORD *flags, DWORD *maxp, DWORD *cur, char *name, int namesz)
{
    *flags = *maxp = *cur = 0;
    name[0] = 0;
    if (n < 8 + 80) return;
    memcpy(flags, b + 8 + 4, 4);
    memcpy(maxp, b + 8 + 40, 4);
    memcpy(cur, b + 8 + 44, 4);
    uint32_t off;
    memcpy(&off, b + 88, 4);
    int o = 0;
    for (uint32_t i = off; i + 1 < (uint32_t)n && o < namesz - 1; i += 2) {
        WCHAR ch = b[i] | (b[i + 1] << 8);
        if (!ch) break;
        name[o++] = ch < 128 ? (char)ch : '?';
    }
    name[o] = 0;
}

/* ------------------------------------------------------------------ the pump: incoming, tasks */

static void inject_rooms(okw_ctx *c);

static void deliver(void)
{
    static uint8_t buf[65536 + 64];
    for (okw_ctx *c = g_ctxs; c; c = c->next) {
        uint32_t info[4];
        int kind, guard = 0;
        while (guard++ < 256 && (kind = okw_js_next(c->id, buf, sizeof(buf), info)) > 0) {
            if (kind == 1) {   /* a message from a peer: info = peer lo, hi, length */
                okw_hdr h = { OKW_HDR_MAGIC, info[0], info[1], c->room, 0 };
                IDirectPlaySP_HandleMessage(c->isp, buf, info[2], &h);
            }
        }
        /* a fresh room list: hand it to DirectPlay now (the game may not ask again for a while) */
        if (!c->is_host && !c->open) {
            int v = okw_js_rooms_version(c->id);
            if (v != c->rooms_seen) { c->rooms_seen = v; inject_rooms(c); }
        }
        /* host: keep the room's enum reply current (player counts), about once a second */
        if (c->is_host && c->open && GetTickCount() - c->last_probe >= 1000) {
            c->last_probe = GetTickCount();
            self_probe(c);
        }
    }
}

/* inject the listed rooms that match the joiner's filter (okplay: okp_inject_rooms) */
static void inject_rooms(okw_ctx *c)
{
    static uint8_t blob[1024];
    int n = okw_js_rooms(c->id);
    for (int i = 0; i < n; i++) {
        uint32_t info[4];   /* room, host lo, host hi, length */
        int len = okw_js_room(c->id, i, blob, sizeof(blob), info);
        if (len <= 0 || !enum_blob_ok(blob, len)) continue;
        DWORD fl, maxp, cur;
        char name[64];
        GUID app;
        enum_info(blob, len, &fl, &maxp, &cur, name, sizeof(name));
        memcpy(&app, blob + 8 + 24, 16);
        if (!IsEqualGUID(&c->enum_app, &GUID_NULL) && !IsEqualGUID(&c->enum_app, &app)) continue;
        if (!(c->enum_flags & DPENUMSESSIONS_ALL)) {
            if (fl & (DPSESSION_NEWPLAYERSDISABLED | DPSESSION_JOINDISABLED)) continue;
            if (maxp && cur >= maxp) continue;
        }
        okw_hdr h = { OKW_HDR_MAGIC, info[1], info[2], info[0], 0 };
        IDirectPlaySP_HandleMessage(c->isp, blob, len, &h);
    }
}

/* ------------------------------------------------------------------ callbacks */

static HRESULT WINAPI OKW_EnumSessions(LPDPSP_ENUMSESSIONSDATA d)
{
    okw_ctx *c = ctx_of(d->lpISP);
    char app[33];
    if (!c) return DPERR_GENERIC;
    const uint8_t *msg = (const uint8_t *)d->lpMessage + OKW_HDR_SIZE;
    int len = (int)d->dwMessageSize - (int)OKW_HDR_SIZE;
    if (len >= DPENUM_REQ_SIZE) {
        uint16_t ver;
        memcpy(&ver, msg + 6, 2);
        if (!c->dp_version) c->dp_version = ver;
        memcpy(&c->enum_app, msg + 8, 16);
        memcpy(&c->enum_flags, msg + 28, 4);
    }
    guid_hex(&c->enum_app, app);
    okw_js_list(c->id, app);   /* fresh list in the background */
    inject_rooms(c);           /* answer at once from the last one */
    return DP_OK;
}

static HRESULT WINAPI OKW_Reply(LPDPSP_REPLYDATA d)
{
    okw_ctx *c = ctx_of(d->lpISP);
    okw_hdr *h = (okw_hdr *)d->lpSPMessageHeader;
    if (!c || !h || h->magic != OKW_HDR_MAGIC) return DPERR_GENERIC;
    const uint8_t *msg = (const uint8_t *)d->lpMessage + OKW_HDR_SIZE;
    int len = (int)d->dwMessageSize - (int)OKW_HDR_SIZE;
    if (h->flags & OKW_HF_SELFPROBE) {   /* our own enum reply: what the room announces */
        if (len > 0 && len <= (int)sizeof(c->enum_blob) &&
            (len != c->enum_len || memcmp(c->enum_blob, msg, len))) {
            DWORD fl, maxp, cur;
            char name[64];
            memcpy(c->enum_blob, msg, len);
            c->enum_len = len;
            enum_info(msg, len, &fl, &maxp, &cur, name, sizeof(name));
            okw_js_set_enum(c->id, msg, len, (int)cur, (int)maxp,
                            !(fl & (DPSESSION_NEWPLAYERSDISABLED | DPSESSION_JOINDISABLED)),
                            (fl & DPSESSION_PASSWORDREQUIRED) ? 1 : 0);
        }
        return DP_OK;
    }
    return okw_js_send(c->id, h->peer_lo, h->peer_hi, msg, len, 1) ? DP_OK : DPERR_GENERIC;
}

static int peer_of_player(okw_ctx *c, DPID id, uint32_t *lo, uint32_t *hi)
{
    void *buf = NULL;
    DWORD sz = 0;
    if (IDirectPlaySP_GetSPPlayerData(c->isp, id, &buf, &sz, DPGET_REMOTE) != DP_OK || !buf || sz < sizeof(okw_pdata))
        return 0;
    okw_pdata pd;
    memcpy(&pd, buf, sizeof(pd));
    if (pd.magic != OKW_PD_MAGIC) return 0;
    *lo = pd.peer_lo;
    *hi = pd.peer_hi;
    return 1;
}

static HRESULT WINAPI OKW_Send(LPDPSP_SENDDATA d)
{
    okw_ctx *c = ctx_of(d->lpISP);
    uint32_t lo, hi;
    if (!c) return DPERR_GENERIC;
    const uint8_t *msg = (const uint8_t *)d->lpMessage + OKW_HDR_SIZE;
    int len = (int)d->dwMessageSize - (int)OKW_HDR_SIZE;
    if (d->idPlayerTo == 0) { lo = c->host_lo; hi = c->host_hi; }
    else if (!peer_of_player(c, d->idPlayerTo, &lo, &hi)) {
        okw_log("Send to player %lu: no okweb player data", (unsigned long)d->idPlayerTo);
        return DPERR_INVALIDPLAYER;
    }
    if ((!lo && !hi) || (lo == c->self_lo && hi == c->self_hi)) return DPERR_INVALIDPLAYER;
    return okw_js_send(c->id, lo, hi, msg, len, (d->dwFlags & DPSEND_GUARANTEED) ? 1 : 0) ? DP_OK : DPERR_GENERIC;
}

static HRESULT WINAPI OKW_CreatePlayer(LPDPSP_CREATEPLAYERDATA d)
{
    okw_ctx *c = ctx_of(d->lpISP);
    if (!c) return DPERR_GENERIC;
    if (d->dwFlags & DPLAYI_PLAYER_PLAYERLOCAL) {   /* tell everyone which machine this player lives on */
        okw_pdata pd = { OKW_PD_MAGIC, c->self_lo, c->self_hi };
        return IDirectPlaySP_SetSPPlayerData(d->lpISP, d->idPlayer, &pd, sizeof(pd), DPSET_REMOTE);
    }
    return DP_OK;
}

static HRESULT WINAPI OKW_DeletePlayer(LPDPSP_DELETEPLAYERDATA d) { return DP_OK; }

static HRESULT WINAPI OKW_Open(LPDPSP_OPENDATA d)
{
    okw_ctx *c = ctx_of(d->lpISP);
    char app[33];
    if (!c) return DPERR_GENERIC;
    if (!okw_js_ready()) { okw_log("Open: okweb-sp.js is not installed on the page"); return DPERR_UNAVAILABLE; }
    c->open = 0;
    c->enum_len = 0;

    if (d->bCreate) {
        uint32_t ids[3];
        DWORD fl, maxp, cur;
        char name[64];
        okw_js_new_ids(c->id, ids);   /* room, peer lo, peer hi - known now, announced from JS */
        c->room = ids[0]; c->self_lo = ids[1]; c->self_hi = ids[2];
        c->host_lo = c->host_hi = 0;
        c->is_host = 1;
        c->open = 1;
        self_probe(c);                /* -> OKW_Reply captures our enum reply */
        if (c->enum_len <= 0) { okw_log("Open(create): no enum reply from DirectPlay"); return DPERR_GENERIC; }
        GUID sapp;
        memcpy(&sapp, c->enum_blob + 8 + 24, 16);
        guid_hex(&sapp, app);
        enum_info(c->enum_blob, c->enum_len, &fl, &maxp, &cur, name, sizeof(name));
        c->last_probe = GetTickCount();
        /* (a password game: the room list shows a lock, as the drop-in's games do) */
        return okw_js_host(c->id, app, c->enum_blob, c->enum_len, name, (int)(maxp ? maxp : 8),
                           (fl & DPSESSION_PASSWORDREQUIRED) ? 1 : 0) ? DP_OK : DPERR_GENERIC;
    }

    okw_hdr *h = (okw_hdr *)d->lpSPMessageHeader;
    if (!h || h->magic != OKW_HDR_MAGIC) { okw_log("Open(join): the session header is not ours"); return DPERR_GENERIC; }
    c->room = h->room;
    c->host_lo = h->peer_lo;
    c->host_hi = h->peer_hi;
    c->is_host = 0;
    guid_hex(&c->enum_app, app);
    okw_js_join(c->id, c->room, c->host_lo, c->host_hi, app);
    /* wait for the host's welcome (Sleep pumps and, with Asyncify, lets the browser run) */
    DWORD t0 = GetTickCount();
    int st = 0;
    while ((st = okw_js_join_state(c->id)) == 0 && GetTickCount() - t0 < 20000) Sleep(20);
    if (st != 1) {
        okw_log("Open(join): no connection to the host (%s)", st < 0 ? "failed" : "timed out");
        return DPERR_NOCONNECTION;
    }
    {
        uint32_t self[2];
        okw_js_self(c->id, self);   /* our peer id in this session (for "is this me?") */
        c->self_lo = self[0]; c->self_hi = self[1];
    }
    c->open = 1;
    return DP_OK;
}

static HRESULT WINAPI OKW_CloseEx(LPDPSP_CLOSEDATA d)
{
    okw_ctx *c = ctx_of(d->lpISP);
    if (!c) return DP_OK;
    c->open = 0;
    okw_js_close(c->id);
    return DP_OK;
}

static HRESULT WINAPI OKW_ShutdownEx(LPDPSP_SHUTDOWNDATA d)
{
    okw_ctx *c = ctx_of(d->lpISP), **pp;
    if (!c) return DP_OK;
    okw_js_close(c->id);
    for (pp = &g_ctxs; *pp; pp = &(*pp)->next) if (*pp == c) { *pp = c->next; break; }
    okw_ctx *null = NULL;
    IDirectPlaySP_SetSPData(d->lpISP, &null, sizeof(null), DPSET_LOCAL);
    free(c);
    return DP_OK;
}

static HRESULT WINAPI OKW_GetCaps(LPDPSP_GETCAPSDATA d)
{
    if (d->lpCaps->dwSize < sizeof(DPCAPS)) return DPERR_INVALIDPARAMS;
    d->lpCaps->dwFlags = DPCAPS_ASYNCSUPPORTED | DPCAPS_GUARANTEEDOPTIMIZED;
    d->lpCaps->dwMaxBufferSize = OKW_MAXBUF;
    d->lpCaps->dwMaxQueueSize = 0;
    d->lpCaps->dwMaxPlayers = 65536;
    d->lpCaps->dwHundredBaud = 0;
    d->lpCaps->dwLatency = 50;
    d->lpCaps->dwMaxLocalPlayers = 65536;
    d->lpCaps->dwHeaderLength = OKW_HDR_SIZE;
    d->lpCaps->dwTimeout = 500;
    return DP_OK;
}

HRESULT WINAPI okweb_SPInit(void *arg)
{
    LPSPINITDATA d = arg;
    if (!okw_js_ready()) { okw_log("SPInit: okweb-sp.js is not installed on the page"); return DPERR_UNAVAILABLE; }
    okw_ctx *c = calloc(1, sizeof(*c));
    if (!c) return DPERR_OUTOFMEMORY;
    c->isp = d->lpISP;
    c->id = g_next_id++;
    if (IDirectPlaySP_SetSPData(d->lpISP, &c, sizeof(c), DPSET_LOCAL) != DP_OK) { free(c); return DPERR_UNAVAILABLE; }
    okw_js_init(c->id);
    c->next = g_ctxs;
    g_ctxs = c;
    winweb_set_pump_hook(deliver);
    d->lpCB->EnumSessions = OKW_EnumSessions;
    d->lpCB->Reply = OKW_Reply;
    d->lpCB->Send = OKW_Send;
    d->lpCB->CreatePlayer = OKW_CreatePlayer;
    d->lpCB->DeletePlayer = OKW_DeletePlayer;
    d->lpCB->GetCaps = OKW_GetCaps;
    d->lpCB->Open = OKW_Open;
    d->lpCB->CloseEx = OKW_CloseEx;
    d->lpCB->ShutdownEx = OKW_ShutdownEx;
    d->dwSPHeaderSize = OKW_HDR_SIZE;
    d->dwSPVersion = DPSP_MAJORVERSIONMASK & DPSP_MAJORVERSION;
    return DP_OK;
}
