/* okplay - rendezvous client (room list + join knocks) over HTTP(S): WinHTTP, or plain HTTP on 9x.
 *
 * Protocol: form-encoded POST, plain-text line responses (no JSON parser needed).
 *   a=announce  room peer tok cands enum name players maxp flags  -> "ok <ip>\n" + "knock <peer> <cands>\n"*
 *   a=list                                                        -> "ok <ip>\n" + "room <room> <peer> <age> <cands> <enum> <name>\n"*
 *   a=knock     room peer cands                                   -> "ok <ip>\n"
 *   a=close     room tok                                          -> "ok\n"
 */
#include "okp.h"
#include <winhttp.h>
#include <stdio.h>
#include <string.h>

/* the biggest reply worth reading: a full list is 200 rooms of at most ~3.3 KB each (~660 KB);
 * anything bigger is a broken or hostile server, not a list - stop rather than grow without end */
#define RDV_REPLY_MAX (1024 * 1024)

/* WinHTTP (HTTPS) where Windows has it - loaded at run time, so the DLL still loads on Windows 9x,
 * which gets a plain HTTP/1.0 client over a socket instead (and http:// in place of https://) */
static struct {
    int tried, ok;
    HINTERNET (WINAPI *Open)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
    HINTERNET (WINAPI *Connect)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
    HINTERNET (WINAPI *OpenRequest)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR *, DWORD);
    BOOL (WINAPI *SendRequest)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
    BOOL (WINAPI *ReceiveResponse)(HINTERNET, LPVOID);
    BOOL (WINAPI *QueryHeaders)(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD);
    BOOL (WINAPI *QueryDataAvailable)(HINTERNET, LPDWORD);
    BOOL (WINAPI *ReadData)(HINTERNET, LPVOID, DWORD, LPDWORD);
    BOOL (WINAPI *CloseHandle)(HINTERNET);
    BOOL (WINAPI *SetTimeouts)(HINTERNET, int, int, int, int);
    BOOL (WINAPI *CrackUrl)(LPCWSTR, DWORD, DWORD, LPURL_COMPONENTS);
} wh;

static int winhttp_load(void)
{
    static const char *const names[] = { "WinHttpOpen", "WinHttpConnect", "WinHttpOpenRequest",
        "WinHttpSendRequest", "WinHttpReceiveResponse", "WinHttpQueryHeaders", "WinHttpQueryDataAvailable",
        "WinHttpReadData", "WinHttpCloseHandle", "WinHttpSetTimeouts", "WinHttpCrackUrl" };
    if (!wh.tried) {   /* only the rendezvous threads get here, and they agree on the answer */
        HMODULE m = okp_sys.emulate9x ? NULL : LoadLibraryA("winhttp.dll");
        FARPROC *f = (FARPROC *)&wh.Open;
        int i;
        wh.ok = m != NULL;
        for (i = 0; m && i < 11; i++) if (!(f[i] = GetProcAddress(m, names[i]))) wh.ok = 0;
        wh.tried = 1;
        if (!wh.ok) okp_log(1, "rdv: %s - plain HTTP to the server",
                            okp_sys.emulate9x ? "OKP_EMULATE_9X set" : "no WinHTTP on this Windows");
    }
    return wh.ok;
}

typedef struct {
    HINTERNET session, connect;
    WCHAR path[512];
    int secure;
    int ok;
    int raw;                  /* the plain HTTP client below */
    char host[256], pathA[512];
    uint16_t port;
    okp_ctx *ctx;             /* whose session this connection serves */
} rdv_conn;

/* scheme://host[:port]/path -> parts */
static int split_url(const char *url, char *host, int hostsz, uint16_t *port, char *path, int pathsz)
{
    const char *p = strstr(url, "://"), *h, *e, *colon, *hend;
    h = p ? p + 3 : url;
    e = strchr(h, '/');
    if (!e) e = h + strlen(h);
    colon = memchr(h, ':', e - h);
    hend = colon ? colon : e;
    if (hend == h || hend - h >= hostsz) return 0;
    memcpy(host, h, hend - h);
    host[hend - h] = 0;
    *port = colon ? (uint16_t)atoi(colon + 1) : 0;
    lstrcpynA(path, *e ? e : "/", pathsz);
    return 1;
}

static int rdv_open(okp_ctx *c, rdv_conn *r)
{
    WCHAR url[512], host[256];
    memset(r, 0, sizeof(*r));
    r->ctx = c;
    if (!winhttp_load()) {
        if (!split_url(okp_cfg.server, r->host, sizeof(r->host), &r->port, r->pathA, sizeof(r->pathA))) {
            okp_log(0, "rdv: bad server url '%s'", okp_cfg.server);
            return 0;
        }
        if (!r->port) r->port = 80;   /* an https:// server without TLS here: it also answers plain http */
        r->raw = r->ok = 1;
        return 1;
    }
    MultiByteToWideChar(CP_UTF8, 0, okp_cfg.server, -1, url, 512);
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = r->path;
    uc.dwUrlPathLength = 512;
    if (!wh.CrackUrl(url, 0, 0, &uc)) {
        okp_log(0, "rdv: bad server url '%s'", okp_cfg.server);
        return 0;
    }
    r->secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    r->session = wh.Open(L"okplay/" OKP_VERSION_STR, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!r->session) return 0;
    wh.SetTimeouts(r->session, 3000, 3000, 3000, 5000);
    r->connect = wh.Connect(r->session, host, uc.nPort, 0);
    r->ok = r->connect != NULL;
    return r->ok;
}

static void rdv_close_conn(rdv_conn *r)
{
    okp_ctx *c = r->ctx;
    if (r->connect) wh.CloseHandle(r->connect);
    if (r->session) wh.CloseHandle(r->session);
    memset(r, 0, sizeof(*r));
    r->ctx = c;
}

/* is a game running? Then nothing may pop up over it: a joiner who is in a session at all (it only
 * talks to the server before joining), or a host whose game has started (the session refuses new
 * players: DPSESSION_NEWPLAYERSDISABLED or JOINDISABLED, what AoE2 sets at the start) or who has
 * anyone else in it - MTM2 never locks its session, so its races looked like an empty lobby and the
 * box popped up mid-race. A host alone still gets the box: that's when the listing matters. */
static int in_match(okp_ctx *c)
{
    DWORD flags = 0, players = 0;
    if (!c || !c->session_open || c->closing) return 0;
    if (!c->is_host) return 1;
    EnterCriticalSection(&c->lock);
    /* enum blob = 8-byte header + DPSESSIONDESC2 (dwFlags at +4, dwCurrentPlayers at +44) */
    if (c->enum_len >= 8 + 48) {
        memcpy(&flags, c->enum_blob + 8 + 4, 4);
        memcpy(&players, c->enum_blob + 8 + 44, 4);
    }
    LeaveCriticalSection(&c->lock);
    return (flags & (DPSESSION_NEWPLAYERSDISABLED | DPSESSION_JOINDISABLED)) != 0 || players >= 2;
}

/* wait up to ms for the socket to be writable (connect done) or readable */
static int wait_sock(SOCKET s, int write, int ms)
{
    fd_set fs, es;
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    FD_ZERO(&fs);
    FD_ZERO(&es);
    FD_SET(s, &fs);
    FD_SET(s, &es);
    return select(0, write ? NULL : &fs, write ? &fs : NULL, &es, &tv) > 0 && FD_ISSET(s, &fs);
}

static int send_all(SOCKET s, const char *p, int n)
{
    int off = 0;
    while (off < n) {
        int m = send(s, p + off, n - off, 0);
        if (m > 0) off += m;
        else if (WSAGetLastError() != WSAEWOULDBLOCK || !wait_sock(s, 1, 3000)) return 0;
    }
    return 1;
}

/* one plain-HTTP POST (Windows 9x): HTTP/1.0, so the server closes when the reply is complete.
 * Returns the malloc'd body (status in *status) or NULL with err filled in. */
static char *raw_post(rdv_conn *r, const char *body, DWORD *status, int *outlen, char *err, int errsz)
{
    SOCKADDR_IN6 to[4];
    int n = okp_resolve(r->host, htons(r->port), 4, to, 4), i;
    *status = 0;
    if (!n) {
        snprintf(err, errsz, "cannot resolve %s", r->host);
        return NULL;
    }
    for (i = 0; i < n; i++) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        SOCKADDR_IN a;
        u_long nb = 1;
        char head[1024], *out, *bodyp;
        int hl, cap = 8192, len = 0;
        unsigned long st = 0;
        if (s == INVALID_SOCKET) {
            snprintf(err, errsz, "socket: %d", WSAGetLastError());
            return NULL;
        }
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = to[i].sin6_port;
        memcpy(&a.sin_addr, ((uint8_t *)&to[i].sin6_addr) + 12, 4);
        ioctlsocket(s, FIONBIO, &nb);
        if ((connect(s, (SOCKADDR *)&a, sizeof(a)) != 0 && WSAGetLastError() != WSAEWOULDBLOCK) ||
            !wait_sock(s, 1, 3000)) {
            snprintf(err, errsz, "cannot connect to %s:%u", r->host, r->port);
            closesocket(s);
            continue;
        }
        hl = snprintf(head, sizeof(head), "POST %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: okplay/" OKP_VERSION_STR
                      "\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: %d\r\n"
                      "Connection: close\r\n\r\n", r->pathA, r->host, (int)strlen(body));
        if (!send_all(s, head, hl) || !send_all(s, body, (int)strlen(body))) {
            snprintf(err, errsz, "send failed: %d", WSAGetLastError());
            closesocket(s);
            continue;
        }
        if (!(out = (char *)malloc(cap))) {
            closesocket(s);
            return NULL;
        }
        while (wait_sock(s, 0, 5000)) {
            int m;
            if (len + 4096 + 1 > cap) {
                char *bigger;
                if (cap >= RDV_REPLY_MAX) {
                    okp_log(1, "rdv: reply from %s is over %d bytes - the rest is ignored", r->host, RDV_REPLY_MAX);
                    break;
                }
                bigger = (char *)realloc(out, cap * 2);
                if (!bigger) break;
                out = bigger;
                cap *= 2;
            }
            m = recv(s, out + len, cap - len - 1, 0);
            if (m <= 0) break;
            len += m;
        }
        closesocket(s);
        out[len] = 0;
        bodyp = strstr(out, "\r\n\r\n");
        if (sscanf(out, "HTTP/%*d.%*d %lu", &st) != 1 || !bodyp) {
            snprintf(err, errsz, "bad HTTP reply (%d bytes)", len);
            free(out);
            continue;
        }
        *status = st;
        bodyp += 4;
        len -= (int)(bodyp - out);
        memmove(out, bodyp, len + 1);
        *outlen = len;
        return out;
    }
    return NULL;
}

/* why the last request failed, in words for the "can't reach the server" box */
static char last_why[200];

static void why_winhttp(DWORD e)
{
    snprintf(last_why, sizeof(last_why), "%s (WinHTTP error %lu)",
             e == 12007 ? "The server's name couldn't be looked up - no internet connection, or DNS isn't working" :
             e == 12002 ? "The server didn't answer in time" :
             e == 12029 || e == 12030 ? "No connection to the server - offline, or a firewall is blocking the game" :
             e == 12175 || e == 12157 || e == 12045 ? "The secure (HTTPS) connection failed" :
             "The connection to the server failed", (unsigned long)e);
}

/* WinHTTP: send the POST and wait for the reply headers. 1 = got a response (*req open); 0 = failed
 * (*req open, GetLastError() says why - or *req NULL if no request could be made).
 * A pooled keep-alive connection the server had just closed (it keeps idle ones 5 s) fails in less
 * than a round trip with 12152 (bad response) or 12030/12031 (reset), and WinHTTP never retries a
 * POST itself: nothing reached the server, so send it again. Seen live: ~1 request in 4 failed and a
 * joiner's single knock was lost, so its join timed out. */
static int wh_send(rdv_conn *r, const char *body, const char *action, DWORD t0, HINTERNET *req)
{
    DWORD blen = (DWORD)strlen(body), e;
    for (int attempt = 0;; attempt++) {
        *req = wh.OpenRequest(r->connect, L"POST", r->path, NULL, WINHTTP_NO_REFERER,
                              WINHTTP_DEFAULT_ACCEPT_TYPES, r->secure ? WINHTTP_FLAG_SECURE : 0);
        if (!*req) return 0;
        if (wh.SendRequest(*req, L"Content-Type: application/x-www-form-urlencoded\r\n", (DWORD)-1,
                           (LPVOID)body, blen, blen, 0) &&
            wh.ReceiveResponse(*req, NULL))
            return 1;
        e = GetLastError();
        if ((e != 12152 && e != 12030 && e != 12031) || attempt >= 2) {
            SetLastError(e);
            return 0;
        }
        okp_log(3, "rdv: %s: WinHTTP error %lu after %lums - a closed keep-alive connection, sending again",
                action, (unsigned long)e, (unsigned long)(okp_now() - t0));
        wh.CloseHandle(*req);
    }
}

/* POST body; returns malloc'd NUL-terminated response or NULL. */
static char *rdv_post_once(rdv_conn *r, const char *body)
{
    if (!r->ok) return NULL;
    char action[24] = "?";
    const char *ap = strstr(body, "&a=");
    if (ap) sscanf(ap + 3, "%23[^&]", action);
    DWORD t0 = okp_now();
    if (r->raw) {
        DWORD status;
        int len = 0;
        char err[160] = "";
        char *out = raw_post(r, body, &status, &len, err, sizeof(err));
        DWORD dt = okp_now() - t0;
        if (!out) {
            okp_log(1, "rdv: %s to http://%s:%u%s failed: %s after %lums", action, r->host, r->port, r->pathA,
                    err, (unsigned long)dt);
            snprintf(last_why, sizeof(last_why), "%s", !strncmp(err, "cannot resolve", 14)
                     ? "The server's name couldn't be looked up - no internet connection, or DNS isn't working"
                     : "No connection to the server - offline, or a firewall is blocking the game");
        } else if (status != 200) {
            okp_log(1, "rdv: %s -> HTTP %lu in %lums: %.200s", action, (unsigned long)status, (unsigned long)dt, out);
            if (status == 404) last_why[0] = 0;   /* "no such room": the server is fine */
            else snprintf(last_why, sizeof(last_why), "The server answered with an error (HTTP %lu)", (unsigned long)status);
            free(out);
            out = NULL;
        } else {
            okp_log(dt > 1500 ? 1 : 4, "rdv: %s -> 200, %d bytes, %lums (plain HTTP)%s", action, len,
                    (unsigned long)dt, dt > 1500 ? " (SLOW)" : "");
        }
        return out;
    }
    HINTERNET req = NULL;
    char *out = NULL;
    int sent = wh_send(r, body, action, t0, &req);
    if (!req) {
        why_winhttp(GetLastError());
        return NULL;
    }
    if (sent) {
        DWORD status = 0, sz = sizeof(status);
        wh.QueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        int cap = 8192, len = 0;
        out = (char *)malloc(cap);
        for (;;) {
            DWORD avail = 0, got = 0;
            if (!out || !wh.QueryDataAvailable(req, &avail) || avail == 0) break;
            if ((DWORD)len + avail > RDV_REPLY_MAX) {   /* (DWORD math first: avail could be anything) */
                okp_log(1, "rdv: %s: reply over %d bytes - the rest is ignored", action, RDV_REPLY_MAX);
                break;
            }
            if (len + (int)avail + 1 > cap) {
                char *bigger;
                cap = len + (int)avail + 8192;
                bigger = (char *)realloc(out, cap);
                if (!bigger) break;
                out = bigger;
            }
            if (!wh.ReadData(req, out + len, avail, &got) || got == 0) break;
            len += got;
        }
        if (!out) {
            wh.CloseHandle(req);
            return NULL;
        }
        out[len] = 0;
        DWORD dt = okp_now() - t0;
        if (status != 200) {
            okp_log(1, "rdv: %s -> HTTP %lu in %lums: %.200s", action, (unsigned long)status, (unsigned long)dt, out);
            if (status == 404) last_why[0] = 0;   /* "no such room": the server is fine */
            else snprintf(last_why, sizeof(last_why), "The server answered with an error (HTTP %lu)", (unsigned long)status);
            free(out);
            out = NULL;
        } else {
            /* per-request success is trace (4): at 1/s it would drown the debug log */
            okp_log(dt > 1500 ? 1 : 4, "rdv: %s -> 200, %d bytes, %lums%s", action, len, (unsigned long)dt,
                    dt > 1500 ? " (SLOW)" : "");
        }
    } else {
        /* 12002 timeout, 12007 name not resolved, 12029 cannot connect, 12175 TLS failure */
        DWORD e = GetLastError();
        okp_log(1, "rdv: %s to %s failed: WinHTTP error %lu after %lums", action, okp_cfg.server,
                (unsigned long)e, (unsigned long)(okp_now() - t0));
        why_winhttp(e);
    }
    wh.CloseHandle(req);
    return out;
}

/* every server request goes through here: 3 failures in a row (a few seconds) = tell the player once
 * per game run, with the reason - otherwise an unreachable server looks like "nobody is playing" */
static char *rdv_post(rdv_conn *r, const char *body)
{
    static LONG fails, told;
    char *out;
    last_why[0] = 0;
    out = rdv_post_once(r, body);
    if (out) {
        if (fails >= 3) okp_log(1, "rdv: the server is reachable again");
        InterlockedExchange(&fails, 0);
        return out;
    }
    if (!r->ok) snprintf(last_why, sizeof(last_why), "The server address in okplay.ini (server=) isn't valid");
    else if (!last_why[0]) {   /* answered, just not with what we asked for */
        InterlockedExchange(&fails, 0);
        return NULL;
    }
    LONG k = InterlockedIncrement(&fails);
    if (k == 3)
        okp_log(0, "rdv: can't reach the server %s - %s", okp_cfg.server, last_why);
#ifdef OKP_BUILTIN
    /* tell the player once - but never over a running match: then it waits until the match is over */
    if (k >= 3 && !told) {
        if (in_match(r->ctx)) {
            if (k == 3) okp_log(1, "rdv: not showing the server box now - a match is in progress");
        } else if (!InterlockedExchange(&told, 1)) {
            okp_log(1, "rdv: showing the player the can't-reach-the-server box");
            DP_BUILTIN_ServerUnreachable(okp_cfg.server, last_why);
        }
    }
#endif
    return NULL;
}

/* "latest <version> <note>" (okplay.php's newest drop-in): the drop-in offers the update. Only replies
 * to Online Play's own requests get here - classic connections and LAN Play never ask the server. */
static void check_latest(const char *rest)
{
#ifdef OKP_BUILTIN
    const char *p = rest;
    while (p && *p) {
        if (!strncmp(p, "latest ", 7)) {
            char ver[24] = "", notee[600] = "", note[256] = "";
            if (sscanf(p, "latest %23s %599s", ver, notee) >= 1) {
                if (strcmp(notee, "-") != 0) okp_urldec(notee, note, sizeof(note));
                DP_BUILTIN_OfferUpdate(ver, note);
            }
            return;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
#else
    (void)rest;
#endif
}

/* Handles the "ok <ip>" first line. Returns pointer to the rest, or NULL. */
static char *rdv_ok(okp_ctx *c, char *resp)
{
    if (!resp || strncmp(resp, "ok", 2) != 0) {
        if (resp) okp_log(1, "rdv: error reply: %.200s", resp);
        return NULL;
    }
    char *nl = strchr(resp, '\n');
    char ip[64] = "";
    sscanf(resp, "ok %63s", ip);
    if (ip[0] && strcmp(ip, c->public_ip) != 0) {
        lstrcpynA(c->public_ip, ip, sizeof(c->public_ip));
        okp_log(2, "rdv: server sees us as %s", ip);
        struct in_addr v4;
        SOCKADDR_IN6 g;
        if (okp_pton(AF_INET, ip, &v4) == 1) {
            okp_addr_from_v4(&g, &v4, htons(c->port));
            if (okp_addr_is_public_v4(&g)) {
                c->guess = g;
                c->have_guess = 1;
            }
        }
    }
    char *rest = nl ? nl + 1 : resp + strlen(resp);
    check_latest(rest);
    return rest;
}

static void cands_field(okp_ctx *c, char *out, int n)
{
    SOCKADDR_IN6 lc[OKP_MAX_CANDS];
    int k;
    okp_local_cands(c, lc, &k, OKP_MAX_CANDS);
    okp_cands_to_str(lc, k, out, n);
}

/* ------------------------------------------------ response parsing (fuzzed) */

/* "knock <peer> <cands>" lines: joiners that want in. Register them so the net thread punches back. */
static void parse_knocks(okp_ctx *c, char *rest)
{
    char *st = rest;
    for (char *line = okp_tok(&st, "\n"); line; line = okp_tok(&st, "\n")) {
        char kpeer[32], kcands[1024];
        if (sscanf(line, "knock %31s %1023s", kpeer, kcands) != 2) continue;
        uint64_t pid = strtoull(kpeer, NULL, 16);
        SOCKADDR_IN6 cl[OKP_MAX_CANDS];
        int n = okp_cands_from_str(kcands, cl, OKP_MAX_CANDS);
        EnterCriticalSection(&c->lock);
        okp_peer *p = okp_peer_get(c, pid, 1);
        if (p) {
            /* only a peer we have never connected to takes its addresses from a knock: a knock in the
             * name of an established peer (its id is known to everyone in the game) would aim our probes
             * - and the token we give that peer - at whoever sent the knock */
            if (p->path_changes == 0) okp_peer_add_cands(c, p, cl, n);
            else okp_log(1, "rdv: knock in the name of the connected peer %016llx ignored (its addresses stay)",
                         (unsigned long long)pid);
            if (!p->have_path) p->probe_start = okp_now();
        }
        LeaveCriticalSection(&c->lock);
        okp_log(1, "rdv: knock from %016llx cands=%s", (unsigned long long)pid, kcands);
    }
}

/* "room <room> <peer> <age> <cands> <enum b64> <name>" lines -> rooms[] (at most OKP_MAX_ROOMS). */
static int parse_list(okp_ctx *c, char *rest, okp_room *rooms)
{
    int n = 0;
    char *st = rest;
    for (char *line = okp_tok(&st, "\n"); line && n < OKP_MAX_ROOMS; line = okp_tok(&st, "\n")) {
        char roomh[16], peerh[32], cands[1024], b64[2048], namee[512], build[24] = "", exee[200] = "";
        int age, wire = 0;
        namee[0] = 0;
        /* (0.8.1 servers add the host's build and exe name; older ones stop after wire) */
        if (sscanf(line, "room %15s %31s %d %1023s %2047s %511s %d %23s %199s", roomh, peerh, &age, cands, b64, namee,
                   &wire, build, exee) < 5)
            continue;
        if (wire != 0 && wire != OKP_WIRE_VER) {
            /* a host on another protocol version: joining would just time out, so don't list it */
            okp_log(2, "rdv: room %s is from an incompatible version (protocol %d, ours %d) - not listed", roomh,
                    wire, OKP_WIRE_VER);
            continue;
        }
        okp_room *rm = &rooms[n];
        memset(rm, 0, sizeof(*rm));
        rm->room = (uint32_t)strtoul(roomh, NULL, 16);
        rm->host = strtoull(peerh, NULL, 16);
        rm->ncands = okp_cands_from_str(cands, rm->cands, OKP_MAX_CANDS);
        rm->enumlen = okp_b64_decode(b64, rm->enumblob, sizeof(rm->enumblob));
        okp_urldec(namee, rm->name, sizeof(rm->name));
        if (strlen(build) == 16 && strspn(build, "0123456789abcdef") == 16) {
            memcpy(rm->build, build, 17);
            okp_urldec(exee, rm->exe, sizeof(rm->exe));
        }
        if (rm->room == 0 || rm->host == 0 || rm->host == c->self) continue;
        if (!okp_enum_blob_ok(rm->enumblob, rm->enumlen)) {
            okp_log(1, "rdv: room %08x has a malformed session record - ignored", rm->room);
            continue;
        }
        n++;
    }
    return n;
}

/* ------------------------------------------------------------- announce */

static void enum_blob_info(const uint8_t *b, int n, char *name, int namesz,
                           DWORD *players, DWORD *maxp, DWORD *flags)
{
    name[0] = 0;
    *players = *maxp = *flags = 0;
    if (n < 8 + 80 + 4) return;
    const uint8_t *sd = b + 8;
    memcpy(flags, sd + 4, 4);
    memcpy(maxp, sd + 40, 4);
    memcpy(players, sd + 44, 4);
    uint32_t off;
    memcpy(&off, b + 88, 4);
    if (off >= (uint32_t)n) return;
    const uint8_t *s = b + off;
    int avail = n - off;
    if (avail >= 2 && s[1] != 0) {
        /* AoE2 hands dplayx a char* where a WCHAR* belongs, so the "wide" name is
         * really ANSI text (plus "[host]" after the first NUL). Take the first string. */
        char ansi[128];
        int k = 0;
        while (k < avail && k < 127 && s[k]) { ansi[k] = (char)s[k]; k++; }
        ansi[k] = 0;
        WCHAR w[128];
        MultiByteToWideChar(CP_ACP, 0, ansi, -1, w, 128);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, name, namesz, NULL, NULL);
        return;
    }
    int wchars = avail / 2;
    WCHAR wname[128];
    if (wchars > 127) wchars = 127;
    memcpy(wname, s, wchars * 2);
    wname[wchars] = 0;
    WideCharToMultiByte(CP_UTF8, 0, wname, -1, name, namesz, NULL, NULL);
}

/* a GUID as 32 lowercase hex digits in its usual reading order (no braces/dashes), for app= */
static void guid_hex(const void *g16, char *out)
{
    GUID g;
    memcpy(&g, g16, sizeof(g));
    snprintf(out, 33, "%08lx%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x", (unsigned long)g.Data1, g.Data2, g.Data3,
             g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

/* ------------------------------------------------------------------ which game this is */

/* one string from the exe's version resource (its first language, then US English) */
static int version_string(const void *vi, const char *key, char *out, int n)
{
    char q[96];
    const WORD *tr = NULL;
    UINT len = 0;
    char *val = NULL;
    if (VerQueryValueA((void *)vi, "\\VarFileInfo\\Translation", (void **)&tr, &len) && tr && len >= 4) {
        snprintf(q, sizeof(q), "\\StringFileInfo\\%04x%04x\\%s", tr[0], tr[1], key);
        if (VerQueryValueA((void *)vi, q, (void **)&val, &len) && val && *val) { lstrcpynA(out, val, n); return 1; }
    }
    snprintf(q, sizeof(q), "\\StringFileInfo\\040904b0\\%s", key);
    if (VerQueryValueA((void *)vi, q, (void **)&val, &len) && val && *val) { lstrcpynA(out, val, n); return 1; }
    snprintf(q, sizeof(q), "\\StringFileInfo\\040904e4\\%s", key);
    if (VerQueryValueA((void *)vi, q, (void **)&val, &len) && val && *val) { lstrcpynA(out, val, n); return 1; }
    return 0;
}

/* The game's name and version for the room list (game=, gamever=), worked out once: the name the
 * game registered for its DirectPlay application GUID (HKLM\\...\\DirectPlay\\Applications\\<name>,
 * Guid=), else the exe's ProductName / FileDescription, else the exe's file name; the version is the
 * exe's file version. UTF-8. The room server only shows them - nothing depends on them. */
static void game_label(const void *app_guid16, char *name, int namesz, char *ver, int versz)
{
    static char cname[160], cver[32];
    static int done;
    if (!done) {
        char exe[MAX_PATH], acp[160] = "";
        WCHAR w[160];
        HKEY apps;
        GUID g;
        done = 1;
        memcpy(&g, app_guid16, sizeof(g));
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\DirectPlay\\Applications", 0, KEY_READ, &apps) == ERROR_SUCCESS) {
            char want[40], sub[128], gs[64];
            DWORD i, n, t, gl;
            snprintf(want, sizeof(want), "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", (unsigned long)g.Data1,
                     g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5],
                     g.Data4[6], g.Data4[7]);
            for (i = 0; n = sizeof(sub), RegEnumKeyExA(apps, i, sub, &n, NULL, NULL, NULL, NULL) == ERROR_SUCCESS; i++) {
                HKEY k;
                if (RegOpenKeyExA(apps, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
                gl = sizeof(gs);
                if (RegQueryValueExA(k, "Guid", NULL, &t, (BYTE *)gs, &gl) == ERROR_SUCCESS && t == REG_SZ &&
                    !lstrcmpiA(gs, want))
                    lstrcpynA(acp, sub, sizeof(acp));
                RegCloseKey(k);
                if (acp[0]) break;
            }
            RegCloseKey(apps);
        }
        lstrcpynA(cver, "", sizeof(cver));
        if (GetModuleFileNameA(NULL, exe, sizeof(exe))) {
            DWORD h = 0, sz = GetFileVersionInfoSizeA(exe, &h);
            void *vi = sz ? malloc(sz) : NULL;
            if (vi && GetFileVersionInfoA(exe, 0, sz, vi)) {
                VS_FIXEDFILEINFO *fi = NULL;
                UINT fl = 0;
                if (!acp[0] && !version_string(vi, "ProductName", acp, sizeof(acp)))
                    version_string(vi, "FileDescription", acp, sizeof(acp));
                if (VerQueryValueA(vi, "\\", (void **)&fi, &fl) && fi)
                    snprintf(cver, sizeof(cver), "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                             HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
            }
            free(vi);
            if (!acp[0]) {   /* no version resource: the file name, without .exe */
                char *b = strrchr(exe, '\\'), *dot;
                lstrcpynA(acp, b ? b + 1 : exe, sizeof(acp));
                if ((dot = strrchr(acp, '.'))) *dot = 0;
            }
        }
        MultiByteToWideChar(CP_ACP, 0, acp, -1, w, sizeof(w) / sizeof(w[0]));
        WideCharToMultiByte(CP_UTF8, 0, w, -1, cname, sizeof(cname), NULL, NULL);
        okp_log(2, "room list: this game is \"%s\" version %s", cname, cver[0] ? cver : "?");
    }
    lstrcpynA(name, cname, namesz);
    lstrcpynA(ver, cver, versz);
}

/* ------------------------------------------------------------------ the room's state */

/* What the room is doing, for the room list: in its lobby, in a game, or between games (back in a
 * lobby with the same players - games with a post-game lobby). Session flags say it only for games
 * that use them (NEWPLAYERSDISABLED/JOINDISABLED); AoE2 never changes them when a match starts. So
 * the traffic decides too: messages delivered per second per player's machine (game + DirectPlay's
 * own, whatever lane they came on), compared with the room's own lobby rate - the absolute rates
 * depend on the machines (AoE2 runs everyone at the slowest one's pace): fast PCs ~8/s in the room
 * and ~56/s in a match, a Pentium 233 (86Box) ~1.7/s and ~7.5/s (2026-10-15). In a game after the
 * rate stays >= 3x the lobby's (and >= 3/s) for 8 s, or >= 30/s outright; between games after it
 * stays below a quarter of the way from the lobby rate to the game rate for 20 s (a lobby, or a
 * match paused at AoE2's drop vote, runs at about the lobby rate; slow machines dip to half the
 * game rate mid-match for half a minute - with the middle and 15 s, a 45-minute 3-player match on
 * the 98 VMs read "between" twice, 2026-10-16). */
enum { ROOM_LOBBY, ROOM_PLAYING, ROOM_BETWEEN };
static const char *const room_state_names[] = { "lobby", "playing", "between" };
#define STATE_FAST_PER_S 30   /* this much is a game on any machine */
#define STATE_MIN_PER_S  3    /* a game is at least this busy */

static const char *room_state(okp_ctx *c, DWORD flags)
{
    uint64_t total = 0;
    int machines = 0, prev = c->room_state;
    DWORD now = GetTickCount(), dt;
    EnterCriticalSection(&c->lock);
    for (int i = 0; i < OKP_MAX_PEERS; i++) {
        okp_peer *p = &c->peers[i];
        if (!p->used || p->nplayers <= 0) continue;
        total += (uint64_t)p->rx_sys + p->rx_game;
        machines++;
    }
    LeaveCriticalSection(&c->lock);
    dt = c->state_tick ? now - c->state_tick : 0;
    if (dt >= 500 && machines > 0) {
        int inst = (int)((total - c->state_msgs) * 10000 / dt / (unsigned)machines);   /* tenths per second */
        /* smoothed: a quarter of each new look (slow rooms send 1-2 messages a second) */
        c->state_rate = c->state_rate ? (c->state_rate * 3 + inst) / 4 : inst;
        int rate = c->state_rate, base = c->state_base;
        int high = base * 3 > STATE_MIN_PER_S * 10 ? base * 3 : STATE_MIN_PER_S * 10;
        int mid = c->state_play ? base + (c->state_play - base) / 4 : high;
        int busy = rate >= STATE_FAST_PER_S * 10 || (c->state_base_n >= 6 && rate >= high);
        if (c->room_state != ROOM_PLAYING) {
            /* the lobby's own rate: its quietest smoothed rate, rising only slowly (more players, more
             * chat) - not the burst of DirectPlay's join messages in the first seconds, so the first
             * 5 looks with players here are skipped */
            if (c->state_base_n < 5) c->state_base_n++;
            else if (c->state_base_n == 5) { if (rate < STATE_FAST_PER_S * 10) { c->state_base = rate; c->state_base_n++; } }
            else if (!busy) c->state_base = rate < c->state_base ? rate : c->state_base + (rate - c->state_base) / 32;
            if (busy) c->state_hi += dt; else c->state_hi = 0;
            if (c->state_hi >= 8000) { c->room_state = ROOM_PLAYING; c->state_lo = 0; c->state_play = rate; }
        } else {
            if (busy) c->state_play = (c->state_play * 7 + rate) / 8;   /* (learn the game rate only from game traffic) */
            if (rate < mid && rate < STATE_FAST_PER_S * 10) c->state_lo += dt; else c->state_lo = 0;
            if (c->state_lo >= 20000) { c->room_state = ROOM_BETWEEN; c->state_hi = 0; }
        }
        if (c->room_state != prev)
            okp_log(1, "room state: %s -> %s (%d.%d messages/s per player's machine, lobby %d.%d, %d machine(s))",
                    room_state_names[prev], room_state_names[c->room_state], rate / 10, rate % 10, base / 10,
                    base % 10, machines);
    } else if (machines == 0 && (c->room_state != ROOM_LOBBY || c->state_base_n)) {
        if (c->room_state != ROOM_LOBBY)
            okp_log(1, "room state: %s -> lobby (nobody else here)", room_state_names[c->room_state]);
        c->room_state = ROOM_LOBBY;
        c->state_hi = c->state_lo = c->state_rate = c->state_base = c->state_base_n = c->state_play = 0;
    }
    if (dt >= 500 || !c->state_tick) {
        c->state_msgs = total;
        c->state_tick = now;
    }
    if ((flags & 0x21) && machines > 0) return "playing";   /* NEWPLAYERSDISABLED | JOINDISABLED */
    return room_state_names[c->room_state];
}

static int do_announce(okp_ctx *c, rdv_conn *r)
{
    uint8_t blob[1024];
    int blen;
    EnterCriticalSection(&c->lock);
    blen = c->enum_len;
    memcpy(blob, c->enum_blob, blen);
    LeaveCriticalSection(&c->lock);
    if (blen <= 0) return 0;   /* nothing to advertise until dplayx answered our probe */

    char name[256], namee[768], cands[1024], b64[1500], body[8192];
    DWORD players, maxp, flags;
    enum_blob_info(blob, blen, name, sizeof(name), &players, &maxp, &flags);
    okp_urlenc(name, namee, sizeof(namee));
    cands_field(c, cands, sizeof(cands));
    okp_b64_encode(blob, blen, b64, sizeof(b64));
    char b64e[2048], candse[1500];
    okp_urlenc(b64, b64e, sizeof(b64e));
    okp_urlenc(cands, candse, sizeof(candse));
    char app[33] = "", game[160] = "", gamee[480], gamever[32] = "", gamevere[96];
    if (blen >= 8 + 40) {
        guid_hex(blob + 8 + 24, app);   /* DPSESSIONDESC2.guidApplication */
        game_label(blob + 8 + 24, game, sizeof(game), gamever, sizeof(gamever));
    }
    okp_urlenc(game, gamee, sizeof(gamee));
    okp_urlenc(gamever, gamevere, sizeof(gamevere));
    char build[17], exe[64], exee[200];
    okp_exe_build(build, exe, sizeof(exe));
    okp_urlenc(exe, exee, sizeof(exee));
    snprintf(body, sizeof(body),
             "k=%s&a=announce&room=%08x&peer=%016llx&tok=%s&cands=%s&enum=%s&name=%s"
             "&players=%lu&maxp=%lu&flags=%lu&ver=" OKP_VERSION_STR "&wire=%d&app=%s&game=%s&gamever=%s&state=%s"
             "&build=%s&exe=%s",
             okp_cfg.key, c->room, (unsigned long long)c->self, c->tok, candse, b64e, namee,
             (unsigned long)players, (unsigned long)maxp, (unsigned long)flags, OKP_WIRE_VER, app, gamee, gamevere,
             room_state(c, flags), build, exee);

    char *resp = rdv_post(r, body);
    char *rest = rdv_ok(c, resp);
    if (!rest) { free(resp); return 0; }
    parse_knocks(c, rest);
    free(resp);
    return 1;
}

/* ------------------------------------------------------------------ list */

static void do_list(okp_ctx *c, rdv_conn *r)
{
    char body[256], app[33];
    static const GUID any_app;
    snprintf(body, sizeof(body), "k=%s&a=list", okp_cfg.key);
    if (!IsEqualGUID(&c->enum_app, &any_app)) {   /* only this game's rooms (and test games only for the test tool) */
        guid_hex(&c->enum_app, app);
        strcat(body, "&app=");
        strcat(body, app);
    }
    char *resp = rdv_post(r, body);
    char *rest = rdv_ok(c, resp);
    if (!rest) { free(resp); return; }
    okp_room rooms[OKP_MAX_ROOMS];
    int n = parse_list(c, rest, rooms);
    free(resp);

    EnterCriticalSection(&c->lock);
    memcpy(c->rooms, rooms, sizeof(rooms[0]) * n);
    c->nrooms = n;
    LeaveCriticalSection(&c->lock);
    okp_log(2, "rdv: %d room(s) listed", n);
    okp_inject_rooms(c);
}

int okp_rdv_knock(okp_ctx *c, uint32_t room)
{
    rdv_conn r;
    if (!rdv_open(c, &r)) return 0;
    char cands[1024], candse[1500], body[2048];
    cands_field(c, cands, sizeof(cands));
    okp_urlenc(cands, candse, sizeof(candse));
    snprintf(body, sizeof(body), "k=%s&a=knock&room=%08x&peer=%016llx&cands=%s",
             okp_cfg.key, room, (unsigned long long)c->self, candse);
    char *resp = rdv_post(&r, body);
    int ok = rdv_ok(c, resp) != NULL;
    free(resp);
    rdv_close_conn(&r);
    okp_log(1, "rdv: knock room %08x cands=%s -> %s", room, cands, ok ? "ok" : "FAILED");
    return ok;
}

int okp_rdv_close(okp_ctx *c)
{
    rdv_conn r;
    if (!rdv_open(c, &r)) return 0;
    char body[256];
    snprintf(body, sizeof(body), "k=%s&a=close&room=%08x&tok=%s", okp_cfg.key, c->room, c->tok);
    char *resp = rdv_post(&r, body);
    int ok = resp && strncmp(resp, "ok", 2) == 0;
    free(resp);
    rdv_close_conn(&r);
    okp_log(1, "rdv: close room %08x -> %s", c->room, ok ? "ok" : "FAILED");
    return ok;
}

/* report.c: one request outside any game (a report). No can't-reach-the-server box: a report that
 * doesn't get through just waits for the next time. */
int okp_rdv_post_simple(const char *body)
{
    rdv_conn r;
    if (!rdv_open(NULL, &r)) return 0;
    char *resp = rdv_post_once(&r, body);
    int ok = resp && strncmp(resp, "ok", 2) == 0;
    if (resp && !ok) okp_log(1, "rdv: report refused: %.200s", resp);
    free(resp);
    rdv_close_conn(&r);
    return ok;
}

/* ---------------------------------------------------------------- thread */

static DWORD WINAPI rdv_main(LPVOID arg)
{
    okp_ctx *c = (okp_ctx *)arg;
    rdv_conn r;
    memset(&r, 0, sizeof(r));
    if (c->mode == OKP_MODE_ONLINE) rdv_open(c, &r);
    DWORD last_announce = 0, last_probe = 0, last_list = 0;
    int announced = 0;

    while (c->running) {
        WaitForSingleObject(c->rdv_wake, 100);
        if (!c->running) break;
        DWORD now = okp_now();

        if (c->mode == OKP_MODE_LAN) {
            /* LAN Play: no server. Keep our enum reply fresh for OFFERs and re-ask the LAN. */
            if (c->is_host && c->session_open && !c->closing && now - last_probe >= 500) {
                okp_self_probe(c);
                last_probe = now;
            }
            if (c->enum_pending && now - c->last_discover >= 1000) {
                c->enum_pending = 0;
                okp_lan_discover(c);
            }
            continue;
        }

        if (c->is_host && c->session_open && !c->closing) {
            if (now - last_probe >= (DWORD)okp_cfg.announce_ms / 2) {
                okp_self_probe(c);       /* refreshes enum_blob via our Reply callback */
                last_probe = now;
            }
            /* plain HTTP (9x) opens a new TCP connection per request: at the 1 s default that was a
             * connection every ~2 s all race long, and the server/NAT started refusing some */
            DWORD every = (DWORD)okp_cfg.announce_ms;
            if (r.raw && every < 3000) every = 3000;
            if (now - last_announce >= every) {
                if (do_announce(c, &r) && !announced) {
                    okp_log(1, "rdv: room %08x is listed", c->room);
                    announced = 1;
                }
                last_announce = now;
            }
        }
        if (announced && (c->closing || !c->session_open)) {
            okp_rdv_close(c);
            announced = 0;
        }
        if (c->enum_pending && now - last_list >= 1000) {
            c->enum_pending = 0;
            do_list(c, &r);
            last_list = now;
        }
        if (!c->stun_resolved && okp_cfg.stun[0]) okp_stun_resolve(c);   /* after the first listing */
    }
    if (announced) okp_rdv_close(c);
    rdv_close_conn(&r);
    return 0;
}

int okp_rdv_start(okp_ctx *c)
{
    if (c->rdv_thread) return 1;
    if (!c->rdv_wake) c->rdv_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    c->rdv_thread = CreateThread(NULL, 0, rdv_main, c, 0, NULL);
    return c->rdv_thread != NULL;
}

void okp_rdv_wake(okp_ctx *c)
{
    if (c->rdv_wake) SetEvent(c->rdv_wake);
}

void okp_rdv_stop(okp_ctx *c)
{
    if (c->rdv_thread) {
        okp_rdv_wake(c);
        if (WaitForSingleObject(c->rdv_thread, 8000) == WAIT_TIMEOUT)
            TerminateThread(c->rdv_thread, 0);
        CloseHandle(c->rdv_thread);
        c->rdv_thread = NULL;
    }
    if (c->rdv_wake) {
        CloseHandle(c->rdv_wake);
        c->rdv_wake = NULL;
    }
}
