/* netcheck - "Connection Check" for the drop-in: lets a player see, before game night, whether
 * Online/LAN Play will work on this PC, and test a real game with a friend.
 *
 * Copy netcheck.exe next to the drop-in's dplayx.dll (the game folder). It drives that dplayx.dll
 * through the normal DirectPlay API, exactly as the game does, so a passing check means the game's
 * path works too.
 *
 *   netcheck.exe                         the window
 *   netcheck.exe --cli check             the checks, printed (no window)
 *   netcheck.exe --cli host [--secs N]   host a test game for N seconds (default 120)
 *   netcheck.exe --cli join [--session TEXT] [--secs N]
 *                                        join the first test game (whose name contains TEXT)
 *   common options: --lan (LAN Play instead of Online Play), --name NAME, --dplayx DLL,
 *                   --transport udp|udp+reliable|reliable (old names udp+tcp, tcp still accepted):
 *                   the mode when hosting - the host decides for the whole game (okplay/src/okp.h)
 *
 * In the window, "Find test games" lists the test games (refreshed every 1.5 s, through DirectPlay
 * like the game's own list) and a double-click joins one. Test games carry NETCHECK_APP, which the
 * room server keeps apart from real games (okplay.php TEST_APP).
 *
 * Checks: the files (dplayx.dll is the drop-in, okplay.ini), DirectPlay lists the drop-in's
 * connections, the room server answers (time, public address, open rooms, newest version), UDP to
 * the internet and the router's port behaviour (STUN), local addresses.
 * Host/join: the test game appears on the room server, join time, ping (now/avg/min/max), jitter,
 * loss, and a transmission test (game-like unguaranteed traffic, guaranteed messages, large
 * messages) whose delivery counts the other side reports back.
 *
 * Only Windows 95/98-era APIs are linked (wsock32, no getaddrinfo); WinHTTP and AttachConsole are
 * loaded at run time when present. Test games use their own application GUID, so the game never
 * lists them. Output lines: "[ OK ]", "[WARN]", "[FAIL]", "  -   " (info); the CLI ends with
 * "RESULT ok|fail".
 */
#define INITGUID
#define COBJMACROS
#include <windows.h>
#include <dplay.h>
#include <dplobby.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <netfw.h>
#ifndef OKP_BUILTIN   /* the drop-in's version number (Connection Check ships with it) */
#define OKP_BUILTIN
#include "../src/version.h"
#undef OKP_BUILTIN
#endif

DEFINE_GUID(OKP_SPGUID, 0x66d8dc59, 0x28e8, 0x4d3e, 0xba, 0x93, 0x2e, 0x98, 0xd2, 0x51, 0xc0, 0xf3);
DEFINE_GUID(OKP_LANGUID, 0x6bcff704, 0xce20, 0x4150, 0x98, 0xb4, 0x5a, 0x71, 0x98, 0x17, 0x5d, 0x7d);
/* {0E7C4B61-93A2-4F0B-8D35-6A1F2C9B7E58} netcheck test games (never listed by a game) */
DEFINE_GUID(NETCHECK_APP, 0x0e7c4b61, 0x93a2, 0x4f0b, 0x8d, 0x35, 0x6a, 0x1f, 0x2c, 0x9b, 0x7e, 0x58);

/* defaults: keep in sync with okp_config_load (okplay/src/util.c) */
#define DEF_SERVER "https://oldkid.net/aoe2net/okplay.php"
#define DEF_KEY    "okp_7Qm2xV9rLs4Hd8Wn"
#define DEF_STUN   "stun.cloudflare.com:3478,stun.l.google.com:19302,stun1.l.google.com:19302,stun.nextcloud.com:443"
#define SESSION_PREFIX "Connection test - "

enum { K_HEAD, K_OK, K_WARN, K_FAIL, K_INFO };

/* ------------------------------------------------------------------ output */

static int g_cli;
static HWND g_wnd;
#define WM_APP_LINE (WM_APP + 1)
#define WM_APP_DONE (WM_APP + 2)
static CRITICAL_SECTION g_cs;
static char *g_report;          /* everything shown, for Copy / Save */
static size_t g_report_len, g_report_cap;
static int g_fails, g_warns;
static DWORD g_t0;

static void report_add(const char *s)
{
    size_t n = strlen(s);
    EnterCriticalSection(&g_cs);
    if (g_report_len + n + 3 > g_report_cap) {
        size_t cap = (g_report_cap + n + 3) * 2;
        char *p = (char *)realloc(g_report, cap);
        if (!p) { LeaveCriticalSection(&g_cs); return; }
        g_report = p;
        g_report_cap = cap;
    }
    memcpy(g_report + g_report_len, s, n);
    g_report_len += n;
    memcpy(g_report + g_report_len, "\r\n", 3);
    g_report_len += 2;
    LeaveCriticalSection(&g_cs);
}

static void emit(int kind, const char *fmt, ...)
{
    static const char *tag[] = {"", "[ OK ] ", "[WARN] ", "[FAIL] ", "  -    "};
    char body[1024], line[1100];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(body, sizeof(body) - 1, fmt, ap);
    body[sizeof(body) - 1] = 0;
    va_end(ap);
    if (kind == K_FAIL) g_fails++;
    if (kind == K_WARN) g_warns++;
    if (kind == K_HEAD) _snprintf(line, sizeof(line), "\r\n== %s ==", body);
    else _snprintf(line, sizeof(line), "%s%s", tag[kind], body);
    line[sizeof(line) - 1] = 0;
    report_add(kind == K_HEAD ? line + 2 : line);
    if (g_cli) {
        DWORD w;
        char out[1200];
        int n = _snprintf(out, sizeof(out) - 2, "[%6lu] %s\n", (unsigned long)(GetTickCount() - g_t0),
                          kind == K_HEAD ? line + 2 : line);
        if (n < 0) n = (int)strlen(out);
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), out, (DWORD)n, &w, NULL);
    } else if (g_wnd) {
        PostMessageA(g_wnd, WM_APP_LINE, 0, (LPARAM)_strdup(line));
    }
}

/* ------------------------------------------------------------------ config */

static char g_dir[MAX_PATH];     /* this program's folder, with a trailing backslash */
static char g_server[512], g_key[128], g_stun[512], g_mode[16];
static char g_name[64] = "Player", g_filter[64], g_dplayx_arg[MAX_PATH];
static int g_lan, g_transport = 1;   /* 0 udp, 1 udp+reliable (the default), 2 reliable: okplay's transport= (okp.h) */
/* okplay's transport= values (okplay/src/okp.h); the host's choice is the game's */
static const char *transport_names[] = {"udp", "udp+reliable", "reliable"};
static const char *transport_labels[] = {"Fast (UDP)", "DirectPlay-style (guaranteed messages reliable)", "All reliable"};
static int g_host_mode = -1;   /* joining: the host's transmit mode, told in its NC_HELLO */

static void config_load(void)
{
    char ini[MAX_PATH];
    _snprintf(ini, sizeof(ini), "%sokplay.ini", g_dir);
    GetPrivateProfileStringA("okplay", "server", DEF_SERVER, g_server, sizeof(g_server), ini);
    GetPrivateProfileStringA("okplay", "key", DEF_KEY, g_key, sizeof(g_key), ini);
    GetPrivateProfileStringA("okplay", "stun", DEF_STUN, g_stun, sizeof(g_stun), ini);
    GetPrivateProfileStringA("okplay", "mode", "all", g_mode, sizeof(g_mode), ini);
    if (GetEnvironmentVariableA("OKP_SERVER", g_server, sizeof(g_server)) == 0)
        GetPrivateProfileStringA("okplay", "server", DEF_SERVER, g_server, sizeof(g_server), ini);
}

/* ------------------------------------------------------------------ small helpers */

static void urldec(const char *in, char *out, int n)
{
    int o = 0;
    for (; *in && o < n - 1; in++) {
        if (*in == '+') out[o++] = ' ';
        else if (*in == '%' && in[1] && in[2]) {
            char h[3] = {in[1], in[2], 0};
            out[o++] = (char)strtol(h, NULL, 16);
            in += 2;
        } else out[o++] = *in;
    }
    out[o] = 0;
}

static int split_url(const char *url, int *https, char *host, int hostsz, unsigned short *port, char *path, int pathsz)
{
    const char *p = url;
    *https = 0;
    if (!_strnicmp(p, "https://", 8)) { *https = 1; p += 8; }
    else if (!_strnicmp(p, "http://", 7)) p += 7;
    else return 0;
    const char *slash = strchr(p, '/');
    int hl = slash ? (int)(slash - p) : (int)strlen(p);
    if (hl <= 0 || hl >= hostsz) return 0;
    memcpy(host, p, hl);
    host[hl] = 0;
    *port = *https ? 443 : 80;
    char *colon = strchr(host, ':');
    if (colon) { *colon = 0; *port = (unsigned short)atoi(colon + 1); }
    lstrcpynA(path, slash ? slash : "/", pathsz);
    return 1;
}

static int resolve4(const char *host, struct in_addr *out)
{
    unsigned long a = inet_addr(host);
    if (a != INADDR_NONE) { out->s_addr = a; return 1; }
    struct hostent *he = gethostbyname(host);
    if (!he || he->h_addrtype != AF_INET || !he->h_addr_list[0]) return 0;
    memcpy(out, he->h_addr_list[0], 4);
    return 1;
}

/* ------------------------------------------------------------------ HTTP (room server) */

typedef void *HINET;
typedef HINET (WINAPI *WhOpen)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
typedef HINET (WINAPI *WhConnect)(HINET, LPCWSTR, WORD, DWORD);
typedef HINET (WINAPI *WhOpenRequest)(HINET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR *, DWORD);
typedef BOOL (WINAPI *WhSend)(HINET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
typedef BOOL (WINAPI *WhReceive)(HINET, LPVOID);
typedef BOOL (WINAPI *WhQuery)(HINET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD);
typedef BOOL (WINAPI *WhRead)(HINET, LPVOID, DWORD, LPDWORD);
typedef BOOL (WINAPI *WhClose)(HINET);
typedef BOOL (WINAPI *WhTimeouts)(HINET, int, int, int, int);
static struct { WhOpen open; WhConnect connect; WhOpenRequest req; WhSend send; WhReceive recv; WhQuery query;
                WhRead read; WhClose close; WhTimeouts timeouts; int tried, ok; } wh;

static int winhttp_load(void)
{
    if (wh.tried) return wh.ok;
    wh.tried = 1;
    HMODULE m = LoadLibraryA("winhttp.dll");
    if (!m) return 0;
    wh.open = (WhOpen)GetProcAddress(m, "WinHttpOpen");
    wh.connect = (WhConnect)GetProcAddress(m, "WinHttpConnect");
    wh.req = (WhOpenRequest)GetProcAddress(m, "WinHttpOpenRequest");
    wh.send = (WhSend)GetProcAddress(m, "WinHttpSendRequest");
    wh.recv = (WhReceive)GetProcAddress(m, "WinHttpReceiveResponse");
    wh.query = (WhQuery)GetProcAddress(m, "WinHttpQueryHeaders");
    wh.read = (WhRead)GetProcAddress(m, "WinHttpReadData");
    wh.close = (WhClose)GetProcAddress(m, "WinHttpCloseHandle");
    wh.timeouts = (WhTimeouts)GetProcAddress(m, "WinHttpSetTimeouts");
    wh.ok = wh.open && wh.connect && wh.req && wh.send && wh.recv && wh.query && wh.read && wh.close;
    return wh.ok;
}

/* POST body to url; returns a malloc'd response body (NUL-terminated) and the HTTP status, or NULL
 * with err filled in */
/* case-insensitive strstr (ASCII) */
static const char *ci_find(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return hay;
    }
    return NULL;
}

/* Something other than the room server answered (a web page instead of "ok ..."): say what it
 * probably is and what to do, and show its title and first words - they go into the report too.
 * Seen 2026-10-22: the web host's anti-bot page ("One moment, please...", Imunify360) for a player
 * whose address it had flagged; his browser passed it, the drop-in and this tool can't. */
static void explain_not_server(const char *page)
{
    char title[128] = "", start[180];
    const char *t = ci_find(page, "<title>");
    int n = 0;
    if (t) {
        for (t += 7; *t == ' ' || *t == '\r' || *t == '\n' || *t == '\t'; t++) {}
        while (t[n] && t[n] != '<' && t[n] != '\r' && t[n] != '\n' && n < (int)sizeof(title) - 1) { title[n] = t[n]; n++; }
        title[n] = 0;
    }
    /* the page's start, on one line */
    n = 0;
    for (const char *p = page; *p && n < (int)sizeof(start) - 1; p++) {
        char c = (*p == '\r' || *p == '\n' || *p == '\t') ? ' ' : *p;
        if (c == ' ' && n && start[n - 1] == ' ') continue;
        start[n++] = c;
    }
    start[n] = 0;
    if (title[0]) emit(K_INFO, "the page's title: \"%s\"", title);
    emit(K_INFO, "the page starts: %s", start);
    if (ci_find(page, "One moment, please") || ci_find(page, "imunify"))
        emit(K_INFO, "this is the web host's anti-bot page (Imunify360): it has flagged this internet address, so "
                     "Online Play can't reach the room server from here. The server's owner can whitelist the "
                     "address or exclude okplay.php from bot protection; meanwhile another connection (a phone "
                     "hotspot) or LAN Play over a VPN works");
    else if (ci_find(page, "Just a moment") || ci_find(page, "cf-chl") || ci_find(page, "cloudflare"))
        emit(K_INFO, "this is a Cloudflare check page: the server's owner has to let game clients through "
                     "(a WAF/bot exception for okplay.php)");
    else if (ci_find(page, "captcha"))
        emit(K_INFO, "this is a captcha page: a program can't solve it - the server's owner has to exclude "
                     "okplay.php from it (or whitelist this address)");
    else if (ci_find(page, "blocked") || ci_find(page, "access denied") || ci_find(page, "forbidden") ||
             ci_find(page, "filter"))
        emit(K_INFO, "this looks like a block page from a filter on the way - your internet provider, router, "
                     "antivirus, firewall or parental controls; try another connection, or allow oldkid.net there");
    else if (ci_find(page, "<html") || ci_find(page, "<!doctype"))
        emit(K_INFO, "a web page answered instead of the room server: a proxy, a Wi-Fi sign-in page (hotel, "
                     "airport), a filter, or the server's own protection - open the server address in a browser "
                     "to see it, and try another connection");
}

static char *http_post(const char *url, const char *body, int *status, char *err, int errsz)
{
    int https;
    char host[256], path[512];
    unsigned short port;
    *status = 0;
    err[0] = 0;
    if (!split_url(url, &https, host, sizeof(host), &port, path, sizeof(path))) {
        _snprintf(err, errsz, "the server address \"%s\" is not an http:// or https:// URL", url);
        return NULL;
    }
    if (winhttp_load()) {
        WCHAR whost[256], wpath[512], hdr[] = L"Content-Type: application/x-www-form-urlencoded\r\n";
        MultiByteToWideChar(CP_ACP, 0, host, -1, whost, 256);
        MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, 512);
        HINET s = wh.open(L"netcheck/1.0", 0 /* default proxy */, NULL, NULL, 0), c = NULL, r = NULL;
        char *resp = NULL;
        if (s && wh.timeouts) wh.timeouts(s, 5000, 5000, 8000, 8000);
        if (s) c = wh.connect(s, whost, port, 0);
        if (c) r = wh.req(c, L"POST", wpath, NULL, NULL, NULL, https ? 0x00800000 /* WINHTTP_FLAG_SECURE */ : 0);
        if (r && wh.send(r, hdr, (DWORD)-1, (LPVOID)body, (DWORD)strlen(body), (DWORD)strlen(body), 0) &&
            wh.recv(r, NULL)) {
            DWORD code = 0, sz = sizeof(code);
            wh.query(r, 19 /* STATUS_CODE */ | 0x20000000 /* FLAG_NUMBER */, NULL, &code, &sz, NULL);
            *status = (int)code;
            size_t len = 0, cap = 4096;
            resp = (char *)malloc(cap);
            for (;;) {
                DWORD got = 0;
                if (len + 2048 + 1 > cap) { cap *= 2; resp = (char *)realloc(resp, cap); }
                if (!wh.read(r, resp + len, 2048, &got) || got == 0) break;
                len += got;
            }
            resp[len] = 0;
        } else {
            _snprintf(err, errsz, "no answer from %s (Windows error %lu)", host, (unsigned long)GetLastError());
        }
        if (r) wh.close(r);
        if (c) wh.close(c);
        if (s) wh.close(s);
        return resp;
    }
    /* no WinHTTP (Windows 9x): plain HTTP, as the drop-in does there */
    if (https) port = 80;
    struct in_addr a;
    if (!resolve4(host, &a)) { _snprintf(err, errsz, "cannot find %s (DNS)", host); return NULL; }
    SOCKET sk = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr = a;
    if (sk == INVALID_SOCKET || connect(sk, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        _snprintf(err, errsz, "cannot connect to %s:%u", host, port);
        if (sk != INVALID_SOCKET) closesocket(sk);
        return NULL;
    }
    char req[8192];
    int n = _snprintf(req, sizeof(req), "POST %s HTTP/1.0\r\nHost: %s\r\nContent-Type: application/x-www-form-urlencoded\r\n"
                      "Content-Length: %d\r\nConnection: close\r\n\r\n%s", path, host, (int)strlen(body), body);
    send(sk, req, n, 0);
    size_t len = 0, cap = 8192;
    char *buf = (char *)malloc(cap);
    for (;;) {
        if (len + 2048 + 1 > cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
        int got = recv(sk, buf + len, 2048, 0);
        if (got <= 0) break;
        len += got;
    }
    buf[len] = 0;
    closesocket(sk);
    sscanf(buf, "HTTP/%*s %d", status);
    char *b = strstr(buf, "\r\n\r\n");
    if (!b) { free(buf); _snprintf(err, errsz, "a broken answer from %s", host); return NULL; }
    memmove(buf, b + 4, strlen(b + 4) + 1);
    return buf;
}

/* a=list on the room server (test=1: only test games, which the plain list leaves out) */
static char *server_list_ex(int tests, int *ms, char *err, int errsz)
{
    char body[512];
    int status;
    _snprintf(body, sizeof(body), "k=%s&a=list%s", g_key, tests ? "&test=1" : "");
    DWORD t = GetTickCount();
    char *r = http_post(g_server, body, &status, err, errsz);
    *ms = (int)(GetTickCount() - t);
    if (r && status != 200) {
        _snprintf(err, errsz, "the server answered HTTP %d: %.80s", status, r);
        free(r);
        return NULL;
    }
    return r;
}

static char *server_list(int *ms, char *err, int errsz) { return server_list_ex(0, ms, err, errsz); }

/* ------------------------------------------------------------------ STUN */

typedef struct { char name[96]; int ok, ms; struct in_addr ip; unsigned short port; } stun_res;

static int stun_query(SOCKET s, const char *server, stun_res *res)
{
    char host[96];
    unsigned short port = 3478;
    lstrcpynA(host, server, sizeof(host));
    char *colon = strchr(host, ':');
    if (colon) { *colon = 0; port = (unsigned short)atoi(colon + 1); }
    lstrcpynA(res->name, server, sizeof(res->name));
    res->ok = 0;
    struct in_addr a;
    if (!resolve4(host, &a)) return 0;
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr = a;
    unsigned char req[20], buf[512];
    memset(req, 0, sizeof(req));
    req[1] = 1;                                   /* Binding Request */
    req[4] = 0x21; req[5] = 0x12; req[6] = 0xa4; req[7] = 0x42;   /* magic cookie */
    for (int i = 8; i < 20; i++) req[i] = (unsigned char)(rand() & 0xff);
    for (int attempt = 0; attempt < 3 && !res->ok; attempt++) {
        DWORD t = GetTickCount();
        sendto(s, (char *)req, 20, 0, (struct sockaddr *)&to, sizeof(to));
        for (;;) {
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(s, &rd);
            struct timeval tv = {0, 300000};
            if (select(0, &rd, NULL, NULL, &tv) <= 0) break;
            struct sockaddr_in from;
            int fl = sizeof(from);
            int n = recvfrom(s, (char *)buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
            if (n < 20 || memcmp(buf + 8, req + 8, 12) != 0) continue;
            for (int off = 20; off + 4 <= n;) {
                int type = buf[off] << 8 | buf[off + 1], len = buf[off + 2] << 8 | buf[off + 3];
                if (off + 4 + len > n) break;
                const unsigned char *v = buf + off + 4;
                if ((type == 0x0020 || type == 0x0001) && len >= 8 && v[1] == 1) {
                    unsigned short p = (unsigned short)(v[2] << 8 | v[3]);
                    unsigned char ip[4] = {v[4], v[5], v[6], v[7]};
                    if (type == 0x0020) {   /* XOR-MAPPED-ADDRESS */
                        p ^= 0x2112;
                        ip[0] ^= 0x21; ip[1] ^= 0x12; ip[2] ^= 0xa4; ip[3] ^= 0x42;
                    }
                    res->port = p;
                    memcpy(&res->ip, ip, 4);
                    res->ok = 1;
                    res->ms = (int)(GetTickCount() - t);
                    if (type == 0x0020) break;
                }
                off += 4 + ((len + 3) & ~3);
            }
            if (res->ok) break;
        }
    }
    return res->ok;
}

/* ------------------------------------------------------------------ DirectPlay */

static IDirectPlay4A *dp;
static IDirectPlayLobby3A *lob;
static HMODULE g_dll;
static char g_dllpath[MAX_PATH], g_dllver[64];
static int g_is_dropin;

static void file_version(const char *path, char *out, int n)
{
    DWORD h, sz = GetFileVersionInfoSizeA(path, &h);
    out[0] = 0;
    if (!sz) return;
    void *b = malloc(sz);
    VS_FIXEDFILEINFO *fi;
    UINT l;
    if (GetFileVersionInfoA(path, 0, sz, b) && VerQueryValueA(b, "\\", (void **)&fi, &l))
        _snprintf(out, n, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                  HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
    free(b);
}

/* the dplayx.dll next to us (or --dplayx); Windows' own only if there is none */
static HRESULT create_dp(void)
{
    typedef HRESULT (WINAPI *gco_t)(REFCLSID, REFIID, void **);
    if (dp) return DP_OK;
    if (g_dplayx_arg[0]) lstrcpynA(g_dllpath, g_dplayx_arg, sizeof(g_dllpath));
    else _snprintf(g_dllpath, sizeof(g_dllpath), "%sdplayx.dll", g_dir);
    if (GetFileAttributesA(g_dllpath) != INVALID_FILE_ATTRIBUTES) g_dll = LoadLibraryA(g_dllpath);
    if (g_dll) {
        gco_t gco = (gco_t)GetProcAddress(g_dll, "DllGetClassObject");
        IClassFactory *cf;
        HRESULT hr = gco ? gco(&CLSID_DirectPlay, &IID_IClassFactory, (void **)&cf) : E_FAIL;
        if (FAILED(hr)) return hr;
        hr = cf->lpVtbl->CreateInstance(cf, NULL, &IID_IDirectPlay4A, (void **)&dp);
        cf->lpVtbl->Release(cf);
        if (FAILED(hr)) return hr;
        if (SUCCEEDED(gco(&CLSID_DirectPlayLobby, &IID_IClassFactory, (void **)&cf))) {
            cf->lpVtbl->CreateInstance(cf, NULL, &IID_IDirectPlayLobby3A, (void **)&lob);
            cf->lpVtbl->Release(cf);
        }
        g_is_dropin = GetProcAddress(g_dll, "SPInit") != NULL;
        file_version(g_dllpath, g_dllver, sizeof(g_dllver));
        return lob ? DP_OK : E_FAIL;
    }
    g_dllpath[0] = 0;
    HRESULT hr = CoCreateInstance(&CLSID_DirectPlay, NULL, CLSCTX_INPROC_SERVER, &IID_IDirectPlay4A, (void **)&dp);
    if (FAILED(hr)) return hr;
    return CoCreateInstance(&CLSID_DirectPlayLobby, NULL, CLSCTX_INPROC_SERVER, &IID_IDirectPlayLobby3A, (void **)&lob);
}

static void release_dp(void)
{
    if (dp) { IDirectPlayX_Close(dp); IDirectPlayX_Release(dp); dp = NULL; }
    if (lob) { IDirectPlayLobby_Release(lob); lob = NULL; }
}

static int conn_online, conn_lan, conn_count;
static char conn_names[512];

static BOOL FAR PASCAL conn_cb(LPCGUID sp, LPVOID conn, DWORD sz, LPCDPNAME name, DWORD flags, LPVOID ctx)
{
    (void)conn; (void)sz; (void)flags; (void)ctx;
    if (IsEqualGUID(sp, &OKP_SPGUID)) conn_online = 1;
    if (IsEqualGUID(sp, &OKP_LANGUID)) conn_lan = 1;
    conn_count++;
    if (strlen(conn_names) < sizeof(conn_names) - 80) {
        if (conn_names[0]) strcat(conn_names, ", ");
        strncat(conn_names, name && name->lpszShortNameA ? name->lpszShortNameA : "?", 70);
    }
    return TRUE;
}

static HRESULT init_connection(void)
{
    DPCOMPOUNDADDRESSELEMENT el;
    el.guidDataType = DPAID_ServiceProvider;
    el.dwDataSize = sizeof(GUID);
    el.lpData = (LPVOID)(g_lan ? &OKP_LANGUID : &OKP_SPGUID);
    DWORD asz = 0;
    IDirectPlayLobby_CreateCompoundAddress(lob, &el, 1, NULL, &asz);
    void *addr = malloc(asz);
    HRESULT hr = IDirectPlayLobby_CreateCompoundAddress(lob, &el, 1, addr, &asz);
    if (SUCCEEDED(hr)) hr = IDirectPlayX_InitializeConnection(dp, addr, 0);
    free(addr);
    return hr;
}

/* ------------------------------------------------------------------ live statistics */

#define MAXPEERS 16
#define HIST 120   /* samples, one per 500 ms: the last minute */
typedef struct {
    DPID id;
    char name[64];
    DWORD joined, sent_at_join, pongs, rtt_last, rtt_min, rtt_max, rtt_sum, last_pong;
    double jitter;
    int hist[HIST];   /* -1 = no answer in that half second */
    int hist_n;
    DWORD hist_pongs; /* pongs counted at the last sample */
    int gone;
    int fw_hint;      /* the firewall hint was shown */
} peer;

static peer peers[MAXPEERS];
static int npeers;
static DWORD g_pings_sent;
static volatile LONG g_stop, g_busy, g_want_xfer;
static DPID g_me;
static int g_hosting;   /* this tool is the host of the test game */
static char g_status[256];

static void remember_name(const char *name);   /* Send report leaves every name seen out */

static peer *peer_get(DPID id, const char *name)
{
    for (int i = 0; i < npeers; i++) if (peers[i].id == id) return &peers[i];
    if (npeers >= MAXPEERS) return NULL;
    peer *p = &peers[npeers];
    memset(p, 0, sizeof(*p));
    p->id = id;
    p->joined = GetTickCount();
    p->sent_at_join = g_pings_sent;
    p->rtt_min = 0xffffffff;
    lstrcpynA(p->name, name ? name : "?", sizeof(p->name));
    remember_name(name);
    if (!name && dp) {
        char buf[256];
        DWORD sz = sizeof(buf);
        if (IDirectPlayX_GetPlayerName(dp, id, buf, &sz) == DP_OK && ((DPNAME *)buf)->lpszShortNameA) {
            lstrcpynA(p->name, ((DPNAME *)buf)->lpszShortNameA, sizeof(p->name));
            remember_name(p->name);
        }
    }
    npeers++;
    return p;
}

static int loss_pct(const peer *p)
{
    /* pings sent since the peer appeared, minus the last second's (still in flight) */
    long expected = (long)(g_pings_sent - p->sent_at_join) - 5;
    if (expected < 10) return -1;
    long lost = expected - (long)p->pongs;
    if (lost < 0) lost = 0;
    return (int)(lost * 100 / expected);
}

static void peer_line(const peer *p, char *out, int n)
{
    int l = loss_pct(p);
    char ls[16];
    if (l < 0) strcpy(ls, "-"); else _snprintf(ls, sizeof(ls), "%d%%", l);
    if (!p->pongs) _snprintf(out, n, "%s: no answer yet", p->name);
    else _snprintf(out, n, "%s: ping %lu ms (avg %lu, min %lu, max %lu), jitter %.0f ms, loss %s",
                   p->name, (unsigned long)p->rtt_last, (unsigned long)(p->rtt_sum / p->pongs),
                   (unsigned long)p->rtt_min, (unsigned long)p->rtt_max, p->jitter, ls);
}

/* ------------------------------------------------------------------ test protocol */

#define NC_PING   0x4e430001
#define NC_PONG   0x4e430002
#define NC_BULK   0x4e430003
#define NC_END    0x4e430004
#define NC_REPORT 0x4e430005
#define NC_HELLO  0x4e430006   /* host -> new player: its transmit mode */
typedef struct { DWORD type, seq, tick; } nc_ping;
/* run = one press of the test: a receiver keeps one tally per (sender, run, phase), so the END that
 * asks for it can be resent until the REPORT arrives (both can be lost on a lossy line in udp mode) */
typedef struct { DWORD type, run, phase, seq, total; } nc_bulk;   /* + padding up to the phase's size */
typedef struct { DWORD type, run, phase, total; } nc_end;
typedef struct { DWORD type, run, phase, total, received, ooo, dup, bytes, span_ms; } nc_report;
typedef struct { DWORD type, mode; } nc_hello;

typedef struct { const char *what; DWORD count, size, flags, gap_ms; } phase_def;
static const phase_def phases[] = {
    {"game-like traffic (unguaranteed, 64 bytes, 50 per second)", 250, 64, 0, 20},
    {"guaranteed messages (64 bytes)", 50, 64, DPSEND_GUARANTEED, 20},
    {"large guaranteed messages (3000 bytes)", 10, 3000, DPSEND_GUARANTEED, 50},
};
#define NPHASES (int)(sizeof(phases) / sizeof(phases[0]))

typedef struct { DPID from; DWORD run, phase, received, maxseq, ooo, dup, bytes, first, last; unsigned char seen[32]; } rx_tally;
static rx_tally tallies[MAXPEERS * NPHASES];
static int ntallies;
static int reports_got[NPHASES];
static DWORD g_run;   /* our current test run (sender side) */

static rx_tally *tally_get(DPID from, DWORD run, DWORD phase)
{
    for (int i = 0; i < ntallies; i++)
        if (tallies[i].from == from && tallies[i].phase == phase) {
            if (tallies[i].run != run) {   /* a new run: start counting again */
                memset(&tallies[i], 0, sizeof(tallies[i]));
                tallies[i].from = from;
                tallies[i].phase = phase;
                tallies[i].run = run;
            }
            return &tallies[i];
        }
    if (ntallies >= MAXPEERS * NPHASES) return NULL;
    rx_tally *t = &tallies[ntallies++];
    memset(t, 0, sizeof(*t));
    t->from = from;
    t->run = run;
    t->phase = phase;
    return t;
}

static void handle_app(DPID from, const void *msg, DWORD size)
{
    const DWORD *type = (const DWORD *)msg;
    if (size < 4) return;
    DWORD now = timeGetTime();
    if (*type == NC_PING && size >= sizeof(nc_ping)) {
        /* test knob: NETCHECK_NO_PONG=1 never answers, as if a firewall dropped our traffic (proves
           the one-way hint fires) */
        static int no_pong = -1;
        if (no_pong < 0) { char e[4] = ""; no_pong = GetEnvironmentVariableA("NETCHECK_NO_PONG", e, sizeof(e)) && e[0] == '1'; }
        if (no_pong) return;
        nc_ping p = *(const nc_ping *)msg;
        p.type = NC_PONG;
        IDirectPlayX_Send(dp, g_me, from, 0, &p, sizeof(p));
    } else if (*type == NC_PONG && size >= sizeof(nc_ping)) {
        const nc_ping *p = (const nc_ping *)msg;
        EnterCriticalSection(&g_cs);
        peer *pr = peer_get(from, NULL);
        if (pr) {
            DWORD rtt = now - p->tick;
            if (pr->pongs) {
                double d = (double)rtt - (double)pr->rtt_last;
                if (d < 0) d = -d;
                pr->jitter += (d - pr->jitter) / 16.0;
            }
            pr->rtt_last = rtt;
            pr->pongs++;
            pr->rtt_sum += rtt;
            if (rtt < pr->rtt_min) pr->rtt_min = rtt;
            if (rtt > pr->rtt_max) pr->rtt_max = rtt;
            pr->last_pong = now;
        }
        LeaveCriticalSection(&g_cs);
    } else if (*type == NC_BULK && size >= sizeof(nc_bulk)) {
        const nc_bulk *b = (const nc_bulk *)msg;
        rx_tally *t = tally_get(from, b->run, b->phase);
        if (!t) return;
        if (b->seq < 256 && (t->seen[b->seq / 8] & (1 << (b->seq % 8)))) { t->dup++; return; }
        if (b->seq < 256) t->seen[b->seq / 8] |= (unsigned char)(1 << (b->seq % 8));
        if (t->received && b->seq < t->maxseq) t->ooo++;
        if (b->seq > t->maxseq) t->maxseq = b->seq;
        if (!t->received) t->first = now;
        t->last = now;
        t->received++;
        t->bytes += size;
    } else if (*type == NC_END && size >= sizeof(nc_end)) {
        const nc_end *e = (const nc_end *)msg;
        rx_tally *t = tally_get(from, e->run, e->phase);
        nc_report r = {NC_REPORT, e->run, e->phase, e->total, t ? t->received : 0, t ? t->ooo : 0, t ? t->dup : 0,
                       t ? t->bytes : 0, t && t->received ? t->last - t->first : 0};
        IDirectPlayX_Send(dp, g_me, from, DPSEND_GUARANTEED, &r, sizeof(r));
    } else if (*type == NC_HELLO && size >= sizeof(nc_hello)) {
        const nc_hello *h = (const nc_hello *)msg;
        if (h->mode <= 2 && g_host_mode != (int)h->mode) {
            g_host_mode = (int)h->mode;
            emit(K_INFO, "the host's transmit mode: %s - the whole game uses it", transport_labels[h->mode]);
        }
    } else if (*type == NC_REPORT && size >= sizeof(nc_report)) {
        const nc_report *r = (const nc_report *)msg;
        if (r->phase >= (DWORD)NPHASES || r->run != g_run || reports_got[r->phase]) return;   /* old or repeat */
        reports_got[r->phase]++;
        char who[64] = "?";
        EnterCriticalSection(&g_cs);
        peer *pr = peer_get(from, NULL);
        if (pr) lstrcpynA(who, pr->name, sizeof(who));
        LeaveCriticalSection(&g_cs);
        DWORD lost = r->total > r->received ? r->total - r->received : 0;
        int kind = K_OK;
        if (lost && phases[r->phase].flags & DPSEND_GUARANTEED) kind = K_FAIL;
        else if (lost * 100 > r->total * 2 || r->ooo || r->dup) kind = K_WARN;
        emit(kind, "%s -> %s: %lu of %lu arrived (%lu%% lost), %lu out of order, %lu twice, in %lu ms",
             phases[r->phase].what, who, (unsigned long)r->received, (unsigned long)r->total,
             (unsigned long)(r->total ? lost * 100 / r->total : 0), (unsigned long)r->ooo, (unsigned long)r->dup,
             (unsigned long)r->span_ms);
        if (kind == K_FAIL)
            emit(K_INFO, "Guaranteed messages must all arrive. This version of the drop-in sends them like "
                         "ordinary ones, so a game that relies on them can miss a message on a lossy line.");
    }
}

static void handle_sys(const DPMSG_GENERIC *g)
{
    switch (g->dwType) {
    case DPSYS_CREATEPLAYERORGROUP: {
        const DPMSG_CREATEPLAYERORGROUP *m = (const DPMSG_CREATEPLAYERORGROUP *)g;
        if (m->dwPlayerType != DPPLAYERTYPE_PLAYER || m->dpId == g_me) break;
        EnterCriticalSection(&g_cs);
        peer *p = peer_get(m->dpId, m->dpnName.lpszShortNameA);
        LeaveCriticalSection(&g_cs);
        emit(K_OK, "%s joined", p ? p->name : "a player");
        if (g_hosting) {
            nc_hello h = {NC_HELLO, (DWORD)g_transport};
            IDirectPlayX_Send(dp, g_me, m->dpId, DPSEND_GUARANTEED, &h, sizeof(h));
        }
        break;
    }
    case DPSYS_DESTROYPLAYERORGROUP: {
        const DPMSG_DESTROYPLAYERORGROUP *m = (const DPMSG_DESTROYPLAYERORGROUP *)g;
        EnterCriticalSection(&g_cs);
        for (int i = 0; i < npeers; i++)
            if (peers[i].id == m->dpId && !peers[i].gone) {
                char line[256];
                peers[i].gone = 1;
                peer_line(&peers[i], line, sizeof(line));
                LeaveCriticalSection(&g_cs);
                emit(K_INFO, "left - final: %s", line);
                EnterCriticalSection(&g_cs);
            }
        LeaveCriticalSection(&g_cs);
        break;
    }
    case DPSYS_HOST: emit(K_INFO, "the host left; this PC is the host now"); break;
    case DPSYS_SESSIONLOST: emit(K_FAIL, "the connection to the game was lost"); break;
    }
}

static void pump(void)
{
    static void *buf;
    static DWORD cap;
    if (!buf) { cap = 8192; buf = malloc(cap); }
    for (;;) {
        DPID from = 0, to = 0;
        DWORD sz = cap;
        HRESULT hr = IDirectPlayX_Receive(dp, &from, &to, DPRECEIVE_ALL, buf, &sz);
        if (hr == DPERR_BUFFERTOOSMALL) { cap = sz * 2; buf = realloc(buf, cap); continue; }
        if (hr != DP_OK) break;
        if (from == DPID_SYSMSG) handle_sys((DPMSG_GENERIC *)buf);
        else handle_app(from, buf, sz);
    }
}

static void sample_history(void)
{
    DWORD now = GetTickCount();
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < npeers; i++) {
        peer *p = &peers[i];
        if (p->gone) continue;
        int v = p->pongs > p->hist_pongs ? (int)p->rtt_last : (now - p->joined > 1500 ? -1 : (int)p->rtt_last);
        p->hist_pongs = p->pongs;
        memmove(p->hist, p->hist + 1, sizeof(int) * (HIST - 1));
        p->hist[HIST - 1] = v;
        if (p->hist_n < HIST) p->hist_n++;
    }
    LeaveCriticalSection(&g_cs);
}

/* the session loop: ping everyone 5 times a second, answer, run the transmission test on request */
static void session_loop(DWORD secs, int auto_xfer_at, int is_host)
{
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    DWORD start = GetTickCount(), last_ping = 0, last_sample = 0, last_summary = start, seq = 0;
    DWORD visible_check = is_host && !g_lan ? start + 4000 : 0;
    int visible = 0, visible_tries = 0;
    (void)ev;
    while (!g_stop && (secs == 0 || GetTickCount() - start < secs * 1000)) {
        Sleep(10);
        pump();
        DWORD now = GetTickCount();
        if (now - last_ping >= 200) {
            nc_ping p = {NC_PING, ++seq, timeGetTime()};
            if (npeers) IDirectPlayX_Send(dp, g_me, DPID_ALLPLAYERS, 0, &p, sizeof(p));
            EnterCriticalSection(&g_cs);
            g_pings_sent++;
            LeaveCriticalSection(&g_cs);
            last_ping = now;
        }
        if (now - last_sample >= 500) { sample_history(); last_sample = now; }
        if (visible_check && now >= visible_check && !visible) {
            char err[256];
            int ms;
            char *r = server_list_ex(1, &ms, err, sizeof(err));   /* test games: only in the test list */
            if (r) {
                char *line = strtok(r, "\n");
                for (; line; line = strtok(NULL, "\n")) {
                    char namee[512] = "", name[512];
                    if (sscanf(line, "room %*s %*s %*d %*s %*s %511s", namee) == 1) {
                        urldec(namee, name, sizeof(name));
                        if (strstr(name, g_name) && strstr(name, "Connection test")) visible = 1;
                    }
                }
                free(r);
            }
            if (visible) emit(K_OK, "your test game is listed on the room server - others can see it");
            else if (++visible_tries >= 6) {
                emit(K_FAIL, "your test game did not appear on the room server within 30 seconds");
                visible_check = 0;
            } else visible_check = now + 5000;
        }
        if (now - last_summary >= 10000 && npeers) {
            EnterCriticalSection(&g_cs);
            for (int i = 0; i < npeers; i++) {
                if (peers[i].gone) continue;
                char line[256];
                /* joined, but not one answer in 10 s: the classic sign of a firewall dropping the traffic */
                if (!peers[i].pongs && !peers[i].fw_hint && now - peers[i].joined > 10000) {
                    char who[64];
                    peers[i].fw_hint = 1;
                    lstrcpynA(who, peers[i].name, sizeof(who));
                    LeaveCriticalSection(&g_cs);
                    emit(K_WARN, "%s joined but nothing comes back from them: a firewall on one of the two PCs is "
                                 "probably dropping the traffic - allow the game (and Connection Check) in Windows "
                                 "Firewall, or answer Allow when Windows asks", who);
                    EnterCriticalSection(&g_cs);
                }
                peer_line(&peers[i], line, sizeof(line));
                LeaveCriticalSection(&g_cs);
                emit(K_INFO, "%s", line);
                EnterCriticalSection(&g_cs);
            }
            LeaveCriticalSection(&g_cs);
            last_summary = now;
        }
        if (auto_xfer_at && now - start >= (DWORD)auto_xfer_at * 1000 && npeers) {
            auto_xfer_at = 0;
            InterlockedExchange(&g_want_xfer, 1);
        }
        if (InterlockedExchange(&g_want_xfer, 0)) {
            DPID target = 0;
            EnterCriticalSection(&g_cs);
            for (int i = 0; i < npeers && !target; i++) if (!peers[i].gone) target = peers[i].id;
            LeaveCriticalSection(&g_cs);
            if (!target) { emit(K_WARN, "nobody to run the transmission test with yet"); continue; }
            int mode = !g_hosting && g_host_mode >= 0 ? g_host_mode : g_transport;
            emit(K_HEAD, "Transmission test (%s)", transport_labels[mode]);
            if (mode)
                emit(K_INFO, "reliable messages go over okplay's reliable lane on the same direct UDP path "
                             "(acknowledged, resent, in order)");
            memset(reports_got, 0, sizeof(reports_got));
            g_run = GetTickCount() | 1;
            for (int ph = 0; ph < NPHASES && !g_stop; ph++) {
                char *m = (char *)calloc(1, phases[ph].size);
                for (DWORD i = 0; i < phases[ph].count && !g_stop; i++) {
                    nc_bulk *b = (nc_bulk *)m;
                    b->type = NC_BULK; b->run = g_run; b->phase = ph; b->seq = i; b->total = phases[ph].count;
                    IDirectPlayX_Send(dp, g_me, target, phases[ph].flags, m, phases[ph].size);
                    DWORD until = GetTickCount() + phases[ph].gap_ms;
                    while (GetTickCount() < until) { Sleep(5); pump(); }
                }
                free(m);
                DWORD settle = GetTickCount() + 1500;   /* let the last ones arrive before END */
                while (GetTickCount() < settle && !g_stop) { Sleep(10); pump(); }
                nc_end e = {NC_END, g_run, (DWORD)ph, phases[ph].count};
                DWORD wait = GetTickCount() + 8000, last_end = 0;
                while (!reports_got[ph] && GetTickCount() < wait && !g_stop) {
                    if (GetTickCount() - last_end >= 1000) {   /* resend until answered */
                        IDirectPlayX_Send(dp, g_me, target, DPSEND_GUARANTEED, &e, sizeof(e));
                        last_end = GetTickCount();
                    }
                    Sleep(10);
                    pump();
                }
                if (!reports_got[ph]) emit(K_FAIL, "%s: the other side never reported back", phases[ph].what);
            }
        }
    }
    (void)is_host;
}

static BOOL FAR PASCAL players_cb(DPID id, DWORD type, LPCDPNAME name, DWORD flags, LPVOID ctx)
{
    (void)type; (void)flags; (void)ctx;
    if (id == g_me) return TRUE;
    EnterCriticalSection(&g_cs);
    peer_get(id, name && name->lpszShortNameA ? name->lpszShortNameA : NULL);
    LeaveCriticalSection(&g_cs);
    return TRUE;
}

typedef struct { int found; DPSESSIONDESC2 sd; char name[128]; int count; char listed[1024];
                 int n; DPSESSIONDESC2 sds[32]; char names[32][128]; DWORD cur[32], max[32]; } enum_ctx;
/* the last round of the test-game list, shown in the window; g_pick = the row the player chose */
static enum_ctx g_list;
static volatile LONG g_pick = -1;
#define WM_APP_LIST (WM_APP + 3)

static BOOL FAR PASCAL enum_cb(LPCDPSESSIONDESC2 sd, LPDWORD timeout, DWORD flags, LPVOID ctx)
{
    (void)timeout;
    enum_ctx *e = (enum_ctx *)ctx;
    if (flags & DPESC_TIMEDOUT) return FALSE;
    const char *n = sd->lpszSessionNameA ? sd->lpszSessionNameA : "";
    remember_name(n);   /* never sent: Send report replaces it */
    e->count++;
    if (e->n < 32) {
        e->sds[e->n] = *sd;
        e->sds[e->n].lpszSessionNameA = NULL;
        e->sds[e->n].lpszPasswordA = NULL;
        lstrcpynA(e->names[e->n], n, sizeof(e->names[0]));
        e->cur[e->n] = sd->dwCurrentPlayers;
        e->max[e->n] = sd->dwMaxPlayers;
        e->n++;
    }
    if (g_cli && !strstr(e->listed, n) && strlen(e->listed) + strlen(n) + 4 < sizeof(e->listed)) {
        strcat(e->listed, "\n");
        strcat(e->listed, n);
        emit(K_INFO, "found \"%s\" (%lu/%lu players)", n, (unsigned long)sd->dwCurrentPlayers,
             (unsigned long)sd->dwMaxPlayers);
    }
    if (!e->found && (!g_filter[0] || strstr(n, g_filter))) {
        e->found = 1;
        e->sd = *sd;
        lstrcpynA(e->name, n, sizeof(e->name));
        e->sd.lpszSessionNameA = NULL;
        e->sd.lpszPasswordA = NULL;
    }
    return TRUE;
}

static void reset_session_state(void)
{
    EnterCriticalSection(&g_cs);
    npeers = 0;
    memset(peers, 0, sizeof(peers));
    g_pings_sent = 0;
    ntallies = 0;
    LeaveCriticalSection(&g_cs);
}

static int create_player(void)
{
    DPNAME pn;
    char pname[80];
    memset(&pn, 0, sizeof(pn));
    pn.dwSize = sizeof(pn);
    lstrcpynA(pname, g_name, sizeof(pname));
    pn.lpszShortNameA = pname;
    pn.lpszLongNameA = pname;   /* games copy both names; never leave one NULL */
    HRESULT hr = IDirectPlayX_CreatePlayer(dp, &g_me, &pn, NULL, NULL, 0, 0);
    if (FAILED(hr)) { emit(K_FAIL, "creating your player failed (0x%08lx)", (unsigned long)hr); return 0; }
    return 1;
}

static int start_dp(void)
{
    /* the drop-in re-reads OKP_TRANSPORT at every session start (okplay/src/okp.h transport=) */
    SetEnvironmentVariableA("OKP_TRANSPORT", transport_names[g_transport]);
    HRESULT hr = create_dp();
    if (FAILED(hr)) {
        emit(K_FAIL, "DirectPlay could not be started (0x%08lx). Is dplayx.dll next to this program?", (unsigned long)hr);
        return 0;
    }
    hr = init_connection();
    if (FAILED(hr)) {
        emit(K_FAIL, "%s is not available in this dplayx.dll (0x%08lx)%s", g_lan ? "LAN Play" : "Online Play",
             (unsigned long)hr, g_is_dropin ? "" : " - it is not the drop-in");
        return 0;
    }
    return 1;
}

static int do_host(DWORD secs)
{
    emit(K_HEAD, "Hosting a test game (%s, %s)", g_lan ? "LAN Play" : "Online Play", transport_labels[g_transport]);
    g_hosting = 1;
    reset_session_state();
    if (!start_dp()) return 0;
    char sess[128];
    _snprintf(sess, sizeof(sess), SESSION_PREFIX "%s", g_name);
    DPSESSIONDESC2 sd;
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DPSESSION_KEEPALIVE | DPSESSION_MIGRATEHOST;
    sd.guidApplication = NETCHECK_APP;
    sd.dwMaxPlayers = 8;
    sd.lpszSessionNameA = sess;
    DWORD t = GetTickCount();
    HRESULT hr = IDirectPlayX_Open(dp, &sd, DPOPEN_CREATE);
    if (FAILED(hr)) { emit(K_FAIL, "could not host (0x%08lx)", (unsigned long)hr); release_dp(); return 0; }
    emit(K_OK, "hosting \"%s\" (took %lu ms)", sess, (unsigned long)(GetTickCount() - t));
    if (!create_player()) { release_dp(); return 0; }
    emit(K_INFO, "ask a friend to run Connection Check and click Join a test game%s",
         g_lan ? " (same network)" : "");
    _snprintf(g_status, sizeof(g_status), "Hosting \"%s\"", sess);
    session_loop(secs, 0, 1);
    release_dp();
    emit(K_INFO, "stopped hosting");
    return 1;
}

static int do_join(DWORD secs)
{
    emit(K_HEAD, "Joining a test game (%s; the host decides the transmit mode)", g_lan ? "LAN Play" : "Online Play");
    g_hosting = 0;
    g_host_mode = -1;
    reset_session_state();
    if (!start_dp()) return 0;
    enum_ctx e;
    memset(&e, 0, sizeof(e));
    DPSESSIONDESC2 sd;
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.guidApplication = NETCHECK_APP;
    if (g_cli)
        emit(K_INFO, "looking for test games%s%s%s...", g_filter[0] ? " named like \"" : "", g_filter,
             g_filter[0] ? "\"" : "");
    else
        emit(K_INFO, "looking for test games: double-click one in the list (right) to join it");
    _snprintf(g_status, sizeof(g_status), "Looking for test games...");
    InterlockedExchange(&g_pick, -1);
    DWORD until = GetTickCount() + 30000;
    while (!e.found && !g_stop && (!g_cli || GetTickCount() < until)) {
        char listed[1024];
        memcpy(listed, e.listed, sizeof(listed));
        int count = e.count;
        memset(&e, 0, sizeof(e));
        memcpy(e.listed, listed, sizeof(listed));
        e.count = count;
        HRESULT hr = IDirectPlayX_EnumSessions(dp, &sd, 1500, enum_cb, &e, DPENUMSESSIONS_AVAILABLE);
        if (FAILED(hr)) { emit(K_FAIL, "searching for games failed (0x%08lx)", (unsigned long)hr); release_dp(); return 0; }
        if (!g_cli) {
            /* the window picks: keep the list for it, and take the chosen row */
            e.found = 0;
            EnterCriticalSection(&g_cs);
            LONG pick = InterlockedExchange(&g_pick, -1);
            if (pick >= 0 && pick < g_list.n) {
                e.found = 1;
                e.sd = g_list.sds[pick];
                lstrcpynA(e.name, g_list.names[pick], sizeof(e.name));
            }
            g_list.n = e.n;
            memcpy(g_list.sds, e.sds, sizeof(e.sds));
            memcpy(g_list.names, e.names, sizeof(e.names));
            memcpy(g_list.cur, e.cur, sizeof(e.cur));
            memcpy(g_list.max, e.max, sizeof(e.max));
            LeaveCriticalSection(&g_cs);
            if (g_wnd) PostMessageA(g_wnd, WM_APP_LIST, 0, 0);
        }
    }
    if (g_wnd) {   /* empty the list: we are in a game or stopped */
        EnterCriticalSection(&g_cs);
        g_list.n = 0;
        LeaveCriticalSection(&g_cs);
        PostMessageA(g_wnd, WM_APP_LIST, 0, 0);
    }
    if (g_stop && !e.found) { release_dp(); emit(K_INFO, "stopped looking"); return 1; }
    if (!e.found) {
        emit(K_FAIL, "no test game found in 30 seconds%s", e.count ? " with that name" :
             g_lan ? " - is the host on the same network?" : " - is someone hosting one right now?");
        release_dp();
        return 0;
    }
    DWORD t = GetTickCount();
    HRESULT hr = IDirectPlayX_Open(dp, &e.sd, DPOPEN_JOIN);
    if (FAILED(hr)) {
        emit(K_FAIL, "joining \"%s\" failed (0x%08lx)%s", e.name, (unsigned long)hr,
             hr == DPERR_NOCONNECTION ? ": no direct path to the host (both routers block it, or a firewall)" : "");
        release_dp();
        return 0;
    }
    DWORD jt = GetTickCount() - t;
    emit(jt > 10000 ? K_WARN : K_OK, "joined \"%s\" in %lu ms", e.name, (unsigned long)jt);
    if (!create_player()) { release_dp(); return 0; }
    IDirectPlayX_EnumPlayers(dp, NULL, players_cb, NULL, 0);
    _snprintf(g_status, sizeof(g_status), "In \"%s\"", e.name);
    emit(K_INFO, "measuring ping; the transmission test starts in 8 seconds");
    session_loop(secs, 8, 0);
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < npeers; i++) {
        char line[256];
        peer_line(&peers[i], line, sizeof(line));
        int l = loss_pct(&peers[i]);
        int kind = !peers[i].pongs ? K_FAIL : (l > 5 || peers[i].rtt_sum / peers[i].pongs > 250) ? K_WARN : K_OK;
        LeaveCriticalSection(&g_cs);
        emit(kind, "%s", line);
        EnterCriticalSection(&g_cs);
    }
    LeaveCriticalSection(&g_cs);
    release_dp();
    return 1;
}

/* ------------------------------------------------------------------ Windows Firewall */

/* Read-only look at Windows Firewall (INetFwMgr, no admin rights needed): is it on for this network,
 * does it block every incoming connection, and is each game program in this folder allowed. A game
 * that isn't allowed yet gets Windows' prompt the first time it hosts. Windows 9x has no firewall
 * API: the check says so and stops. */
static void check_firewall(void)
{
    INetFwMgr *mgr = NULL;
    INetFwPolicy *pol = NULL;
    INetFwProfile *prof = NULL;
    INetFwAuthorizedApplications *apps = NULL;
    VARIANT_BOOL on = VARIANT_FALSE, noexc = VARIANT_FALSE;
    /* oleaut32 at run time: a bare Windows 95 has none (and no firewall either) */
    typedef BSTR (WINAPI *alloc_t)(const OLECHAR *);
    typedef void (WINAPI *free_t)(BSTR);
    HMODULE oa = LoadLibraryA("oleaut32.dll");
    alloc_t bstr_alloc = oa ? (alloc_t)GetProcAddress(oa, "SysAllocString") : NULL;
    free_t bstr_free = oa ? (free_t)GetProcAddress(oa, "SysFreeString") : NULL;
    if (!bstr_alloc || !bstr_free) {
        emit(K_INFO, "Windows Firewall: not present here (Windows 9x) - nothing to check");
        return;
    }
    if (FAILED(CoCreateInstance(&CLSID_NetFwMgr, NULL, CLSCTX_INPROC_SERVER, &IID_INetFwMgr, (void **)&mgr)) || !mgr) {
        emit(K_INFO, "Windows Firewall: not present here (Windows 9x, or another firewall) - nothing to check");
        return;
    }
    if (SUCCEEDED(INetFwMgr_get_LocalPolicy(mgr, &pol)) && pol && SUCCEEDED(INetFwPolicy_get_CurrentProfile(pol, &prof)) && prof) {
        INetFwProfile_get_FirewallEnabled(prof, &on);
        INetFwProfile_get_ExceptionsNotAllowed(prof, &noexc);
        if (!on) emit(K_INFO, "Windows Firewall is off for this network");
        else if (noexc)
            emit(K_WARN, "Windows Firewall blocks ALL incoming connections on this network (\"block all incoming\"): "
                         "others can't join a game you host - turn that option off in Windows Firewall");
        else {
            WIN32_FIND_DATAA fd;
            char spec[MAX_PATH];
            int shown = 0;
            INetFwProfile_get_AuthorizedApplications(prof, &apps);
            _snprintf(spec, sizeof(spec), "%s*.exe", g_dir);
            HANDLE h = FindFirstFileA(spec, &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    char full[MAX_PATH];
                    WCHAR w[MAX_PATH];
                    VARIANT_BOOL en = VARIANT_FALSE;
                    INetFwAuthorizedApplication *app = NULL;
                    if (!_stricmp(fd.cFileName, "netcheck.exe")) continue;
                    _snprintf(full, sizeof(full), "%s%s", g_dir, fd.cFileName);
                    MultiByteToWideChar(CP_ACP, 0, full, -1, w, MAX_PATH);
                    BSTR b = bstr_alloc(w);
                    if (apps && b && SUCCEEDED(INetFwAuthorizedApplications_Item(apps, b, &app)) && app) {
                        INetFwAuthorizedApplication_get_Enabled(app, &en);
                        INetFwAuthorizedApplication_Release(app);
                    }
                    bstr_free(b);
                    if (en) emit(K_OK, "%s is allowed through Windows Firewall", fd.cFileName);
                    else emit(K_INFO, "%s is not on Windows Firewall's allowed list yet: the first time it hosts, "
                                      "Windows asks - choose Allow, or others can't join", fd.cFileName);
                } while (++shown < 8 && FindNextFileA(h, &fd));
                FindClose(h);
            }
            if (!shown) emit(K_INFO, "Windows Firewall is on; no game program found next to Connection Check to look up");
        }
    }
    if (apps) INetFwAuthorizedApplications_Release(apps);
    if (prof) INetFwProfile_Release(prof);
    if (pol) INetFwPolicy_Release(pol);
    INetFwMgr_Release(mgr);
}

/* ------------------------------------------------------------------ the checks */

static void check_all(void)
{
    char err[512];
    emit(K_HEAD, "Files");
    char path[MAX_PATH];
    _snprintf(path, sizeof(path), "%sdplayx.dll", g_dir);
    if (g_dplayx_arg[0]) lstrcpynA(path, g_dplayx_arg, sizeof(path));
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
        emit(K_FAIL, "there is no dplayx.dll next to this program - put netcheck.exe in the game folder, "
                     "next to the drop-in's dplayx.dll");
    _snprintf(path, sizeof(path), "%sokplay.ini", g_dir);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) emit(K_INFO, "no okplay.ini: the defaults are used");
    else emit(K_OK, "okplay.ini found (mode=%s)", g_mode);
    if (!_stricmp(g_mode, "windows"))
        emit(K_WARN, "okplay.ini says mode=windows: the game uses only Windows' DirectPlay, not Online/LAN Play");
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, sizeof(sys));
    strcat(sys, "\\dplayx.dll");
    emit(K_INFO, "Windows' own DirectPlay is %s", GetFileAttributesA(sys) != INVALID_FILE_ATTRIBUTES
         ? "installed (the classic TCP/IP, IPX, modem and serial connections are available too)"
         : "not installed (fine: Online and LAN Play don't need it)");

    emit(K_HEAD, "DirectPlay");
    HRESULT hr = create_dp();
    if (FAILED(hr)) emit(K_FAIL, "DirectPlay could not be started (0x%08lx)", (unsigned long)hr);
    else {
        if (g_dllpath[0])
            emit(g_is_dropin ? K_OK : K_FAIL, "dplayx.dll %s, version %s", g_is_dropin ? "is the drop-in" :
                 "is NOT the drop-in (no Online/LAN Play in it)", g_dllver[0] ? g_dllver : "?");
        else emit(K_WARN, "using Windows' own DirectPlay (no dplayx.dll next to this program)");
        conn_online = conn_lan = conn_count = 0;
        conn_names[0] = 0;
        IDirectPlayX_EnumConnections(dp, NULL, conn_cb, NULL, DPCONNECTION_DIRECTPLAY);
        emit(K_INFO, "the game's connection list: %s", conn_names[0] ? conn_names : "(empty)");
        emit(conn_online ? K_OK : K_FAIL, "Online Play is %s", conn_online ? "listed" : "missing");
        emit(conn_lan ? K_OK : K_FAIL, "LAN Play is %s", conn_lan ? "listed" : "missing");
        release_dp();
    }

    emit(K_HEAD, "Room server");
    emit(K_INFO, "server: %s", g_server);
    int ms;
    struct in_addr pub;
    pub.s_addr = 0;
    char *r = server_list(&ms, err, sizeof(err));
    if (!r) emit(K_FAIL, "%s", err);
    else {
        int rooms = 0;
        char ip[64] = "", latest[32] = "", note[256] = "";
        char *whole = _strdup(r);   /* (strtok below cuts r into lines) */
        char *line = strtok(r, "\n");
        if (line && !strncmp(line, "ok ", 3)) {
            lstrcpynA(ip, line + 3, sizeof(ip));
            pub.s_addr = inet_addr(ip);
        }
        int answered = line && !strncmp(line, "ok", 2);
        for (; line; line = strtok(NULL, "\n")) {
            if (!strncmp(line, "room ", 5)) rooms++;
            if (!strncmp(line, "latest ", 7)) {
                char n2[256] = "";
                sscanf(line + 7, "%31s %255s", latest, n2);
                if (strcmp(n2, "-")) urldec(n2, note, sizeof(note));
            }
        }
        if (!answered) {
            emit(K_FAIL, "the server answered something unexpected: %.100s", r);
            explain_not_server(whole ? whole : r);
        }
        free(whole);
        if (answered) {
            emit(ms > 3000 ? K_WARN : K_OK, "the server answers (%d ms)", ms);
            if (ip[0]) emit(K_INFO, "your public address, as the server sees it: %s", ip);
            emit(K_INFO, "%d game%s open right now", rooms, rooms == 1 ? "" : "s");
            {
                char body[256], e2[256];
                int st;
                _snprintf(body, sizeof(body), "k=%s&a=list&test=1", g_key);
                char *t = http_post(g_server, body, &st, e2, sizeof(e2));
                if (t && st == 200) {
                    int n = 0;
                    for (char *q = strstr(t, "\nroom "); q; q = strstr(q + 1, "\nroom ")) n++;
                    emit(K_INFO, "%d Connection Check test game%s open right now", n, n == 1 ? "" : "s");
                }
                free(t);
            }
            if (latest[0]) emit(K_INFO, "newest drop-in: %s%s%s", latest, note[0] ? " - " : "", note);
            int times[3], n = 0;
            for (int i = 0; i < 3; i++) {
                char *again = server_list(&times[i], err, sizeof(err));
                if (again) { free(again); n++; }
            }
            if (n == 3) emit(K_INFO, "server round trips: %d, %d, %d ms", times[0], times[1], times[2]);
        }
        free(r);
    }

    emit(K_HEAD, "Direct connections (UDP)");
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in any;
    memset(&any, 0, sizeof(any));
    any.sin_family = AF_INET;
    bind(s, (struct sockaddr *)&any, sizeof(any));
    stun_res res[8];
    int nres = 0, nok = 0;
    char list[512];
    lstrcpynA(list, g_stun, sizeof(list));
    for (char *tok = strtok(list, ","); tok && nres < 8; tok = strtok(NULL, ",")) {
        while (*tok == ' ') tok++;
        if (!*tok) continue;
        stun_res *x = &res[nres++];
        if (stun_query(s, tok, x)) {
            nok++;
            emit(K_INFO, "%s sees you as %s:%u (%d ms)", x->name, inet_ntoa(x->ip), x->port, x->ms);
        } else emit(K_INFO, "%s: no answer", x->name);
    }
    closesocket(s);
    if (!nok) emit(K_FAIL, "no answer over UDP from any address server: UDP to the internet looks blocked "
                           "(a firewall?). Online Play needs UDP.");
    else {
        int same = 1;
        stun_res *first = NULL;
        for (int i = 0; i < nres; i++) {
            if (!res[i].ok) continue;
            if (!first) first = &res[i];
            else if (res[i].port != first->port || res[i].ip.s_addr != first->ip.s_addr) same = 0;
        }
        emit(K_OK, "UDP to the internet works");
        if (nok < 2) emit(K_INFO, "only one address server answered: can't tell how your router assigns ports");
        else if (same)
            emit(K_OK, "your router keeps the same public port for every destination: direct connections should work");
        else
            emit(K_WARN, "your router gives each destination a different public port (\"symmetric NAT\"): "
                         "joining may fail unless the other player's router is friendlier. Try hosting instead.");
        if (pub.s_addr && first && first->ip.s_addr != pub.s_addr)
            emit(K_WARN, "the room server and the address servers see different public addresses (%s): "
                         "two internet connections or a VPN?", inet_ntoa(first->ip));
    }

    emit(K_HEAD, "This PC");
    char hn[256];
    if (gethostname(hn, sizeof(hn)) == 0) {
        struct hostent *he = gethostbyname(hn);
        int behind_nat = 1;
        for (int i = 0; he && he->h_addr_list[i]; i++) {
            struct in_addr a;
            memcpy(&a, he->h_addr_list[i], 4);
            emit(K_INFO, "local address: %s", inet_ntoa(a));
            if (pub.s_addr && a.s_addr == pub.s_addr) behind_nat = 0;
        }
        if (pub.s_addr && !behind_nat) emit(K_INFO, "this PC has a public address directly (no router in between)");
    }
    check_firewall();
}

/* ------------------------------------------------------------------ worker */

enum { JOB_CHECK = 1, JOB_HOST, JOB_JOIN };
static DWORD g_job_secs;

static DWORD WINAPI worker(LPVOID arg)
{
    int job = (int)(INT_PTR)arg;
    timeBeginPeriod(1);   /* only while a test runs: the whole PC's timer changes with it */
    CoInitialize(NULL);
    if (job == JOB_CHECK) check_all();
    else if (job == JOB_HOST) do_host(g_job_secs);
    else if (job == JOB_JOIN) do_join(g_job_secs);
    if (job == JOB_CHECK)
        emit(K_HEAD, "Done: %d problem%s, %d warning%s", g_fails, g_fails == 1 ? "" : "s", g_warns,
             g_warns == 1 ? "" : "s");
    CoUninitialize();
    timeEndPeriod(1);
    g_status[0] = 0;
    InterlockedExchange(&g_busy, 0);
    if (g_wnd) PostMessageA(g_wnd, WM_APP_DONE, 0, 0);
    return 0;
}

static void start_job(int job)
{
    if (InterlockedExchange(&g_busy, 1)) return;
    InterlockedExchange(&g_stop, 0);
    g_fails = g_warns = 0;
    DWORD tid;
    HANDLE h = CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)job, 0, &tid);
    if (h) CloseHandle(h);
    else InterlockedExchange(&g_busy, 0);
}

/* ------------------------------------------------------------------ report files */

static void newest_log(char *out, int n)
{
    char spec[MAX_PATH];
    WIN32_FIND_DATAA fd;
    FILETIME best = {0, 0};
    out[0] = 0;
    _snprintf(spec, sizeof(spec), "%snetplay-logs\\okplay-*.log", g_dir);
    HANDLE h = FindFirstFileA(spec, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (CompareFileTime(&fd.ftLastWriteTime, &best) > 0) {
            best = fd.ftLastWriteTime;
            _snprintf(out, n, "%snetplay-logs\\%s", g_dir, fd.cFileName);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static int save_report(char *saved, int n)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snprintf(saved, n, "%snetcheck-report-%04d%02d%02d-%02d%02d%02d.txt", g_dir, st.wYear, st.wMonth, st.wDay,
              st.wHour, st.wMinute, st.wSecond);
    FILE *f = fopen(saved, "wb");
    if (!f) return 0;
    fprintf(f, "Connection Check report, %04d-%02d-%02d %02d:%02d\r\n", st.wYear, st.wMonth, st.wDay, st.wHour,
            st.wMinute);
    EnterCriticalSection(&g_cs);
    if (g_report) fwrite(g_report, 1, g_report_len, f);
    LeaveCriticalSection(&g_cs);
    char log[MAX_PATH];
    newest_log(log, sizeof(log));
    if (log[0]) {   /* the tail of the drop-in's own log: what the provider saw */
        FILE *l = fopen(log, "rb");
        if (l) {
            fseek(l, 0, SEEK_END);
            long sz = ftell(l), from = sz > 24000 ? sz - 24000 : 0;
            fseek(l, from, SEEK_SET);
            fprintf(f, "\r\n== Last part of %s ==\r\n", log);
            char buf[4096];
            size_t got;
            while ((got = fread(buf, 1, sizeof(buf), l)) > 0) fwrite(buf, 1, got, f);
            fclose(l);
        }
    }
    fclose(f);
    return 1;
}

/* ------------------------------------------------------------------ window */

enum { ID_NAME = 100, ID_CONN, ID_CHECK, ID_HOST, ID_JOIN, ID_XFER, ID_STOP, ID_COPY, ID_SAVE,
       ID_LOGS, ID_OUT, ID_STATS, ID_TRANSPORT, ID_LIST, ID_JOINSEL, ID_SEND, ID_TIMER = 1 };
static HWND h_name, h_conn, h_out, h_stats, h_btn[9], h_transport, h_list, h_listlabel, h_joinsel;
static HFONT g_font, g_mono;

/* ------------------------------------------------------------------ scrub (a copy of okplay/src/crash.c okp_scrub) */

static int nc_isdigit(char c) { return c >= '0' && c <= '9'; }
static int nc_ishex(char c) { return nc_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

/* one log line -> the same line without addresses and folders:
 *   C:\Users\Name\Games\AoE2\age2_x1.exe  ->  ...\age2_x1.exe   (a path ends at a double space, a quote,
 *                                            " (", " -", ", " or the end of the line)
 *   192.168.1.20 -> x.x.x.x  (four numbers up to 255; a version like v5.1.2600.0 stays)
 *   any word of hex digits with 2+ colons (IPv6, "[2001:db8::1]:2300") -> [v6]   (not a hh:mm:ss time) */
static int nc_put(char *out, int o, int n, const char *s)   /* (no snprintf here: see send_thread) */
{
    while (*s && o < n - 1) out[o++] = *s++;
    return o;
}

static void nc_scrub(const char *in, char *out, int n)
{
    int o = 0;
    const char *p = in;
    while (*p && o < n - 1) {
        /* a path: X:\ or \\server */
        if ((((*p | 0x20) >= 'a' && (*p | 0x20) <= 'z') && p[1] == ':' && p[2] == '\\') || (p[0] == '\\' && p[1] == '\\')) {
            const char *e = p, *last = NULL;
            while (*e && *e != '"' && *e != '\'' && *e != '\r' && *e != '\n' &&
                   !(e[0] == ' ' && (e[1] == ' ' || e[1] == '(' || e[1] == '-')) && !(e[0] == ',' && e[1] == ' ')) {
                if (*e == '\\') last = e;
                e++;
            }
            const char *keep = last ? last + 1 : e;
            o = nc_put(out, o, n, "...\\");
            if (o >= n - 1) break;
            while (keep < e && o < n - 1) out[o++] = *keep++;
            p = e;
            continue;
        }
        /* IPv4 */
        if (nc_isdigit(*p) && (p == in || (!nc_isdigit(p[-1]) && p[-1] != '.' && p[-1] != 'v'))) {
            const char *q = p;
            int parts = 0, ok = 1;
            while (parts < 4) {
                int v = 0, d = 0;
                while (nc_isdigit(*q) && d < 4) { v = v * 10 + (*q - '0'); q++; d++; }
                if (!d || v > 255) { ok = 0; break; }
                parts++;
                if (parts < 4) { if (*q != '.') { ok = 0; break; } q++; }
            }
            if (ok && parts == 4 && !nc_isdigit(*q) && !(*q == '.' && nc_isdigit(q[1]))) {
                o = nc_put(out, o, n, "x.x.x.x");
                p = q;
                continue;
            }
        }
        /* IPv6: a word of hex digits, colons, dots and brackets with at least two colons */
        if ((nc_ishex(*p) || *p == ':' || *p == '[') && (p == in || !(nc_ishex(p[-1]) || p[-1] == ':' || p[-1] == '.'))) {
            const char *q = p;
            int colons = 0, hex = 0, digits_only = 1;
            while (nc_ishex(*q) || *q == ':' || *q == '.' || *q == '[' || *q == ']') {
                if (*q == ':') colons++;
                if (nc_ishex(*q)) hex++;
                if (!(nc_isdigit(*q) || *q == ':' || *q == '.')) digits_only = 0;
                q++;
            }
            /* "12:34:56.789" (a time) is digits only with colons at 2 and 5: leave it */
            int is_time = digits_only && q - p >= 8 && p[2] == ':' && p[5] == ':';
            if (colons >= 2 && hex && !is_time) {
                o = nc_put(out, o, n, "[v6]");
                p = q;
                continue;
            }
            while (p < q && o < n - 1) out[o++] = *p++;   /* not an address: copy the whole word */
            continue;
        }
        out[o++] = *p++;
    }
    out[o < n ? o : n - 1] = 0;
}


/* ------------------------------------------------------------------ Send report */

/* "Send report" (0.8.3): the results go to the room server's a=report, after the player has seen
 * them. Addresses and folders are taken out (nc_scrub, the same rules as the drop-in's crash
 * reports: okplay/src/crash.c okp_scrub), and so are the names: this PC's player name, the Windows
 * user name, every player and test game name seen this run. */
static char g_seen_names[32][64];
static int g_nseen;

static void remember_name(const char *name)
{
    if (!name || strlen(name) < 2) return;
    for (int i = 0; i < g_nseen; i++) if (!strcmp(g_seen_names[i], name)) return;
    if (g_nseen < 32) lstrcpynA(g_seen_names[g_nseen++], name, 64);
}

/* every `what` in buf (in place, buf has room for cap bytes) -> `with` */
static void replace_all(char *buf, size_t cap, const char *what, const char *with)
{
    size_t wl = strlen(what), nl = strlen(with);
    char *p = buf;
    if (!wl) return;
    while ((p = strstr(p, what)) != NULL) {
        size_t rest = strlen(p + wl);
        if ((size_t)(p - buf) + nl + rest + 1 > cap) return;
        memmove(p + nl, p + wl, rest + 1);
        memcpy(p, with, nl);
        p += nl;
    }
}

/* the text that would be sent (malloc'd): the results, then the drop-in log's last error/warning/info
 * lines, scrubbed line by line, names replaced */
static char *build_send_body(void)
{
    size_t cap = 96 * 1024, o = 0;
    char *out = (char *)malloc(cap), *src, line[1200], clean[1400];
    if (!out) return NULL;
    o += _snprintf(out, cap, "Connection Check report\n");
    EnterCriticalSection(&g_cs);
    src = g_report ? _strdup(g_report) : _strdup("");
    LeaveCriticalSection(&g_cs);
    char log[MAX_PATH], *tail = NULL;
    int tail_cut = 0;   /* the tail starts mid-file: its first line is cut off */
    newest_log(log, sizeof(log));
    if (log[0]) {
        FILE *l = fopen(log, "rb");
        if (l) {
            fseek(l, 0, SEEK_END);
            long sz = ftell(l), from = sz > 16000 ? sz - 16000 : 0;
            tail_cut = from > 0;
            fseek(l, from, SEEK_SET);
            tail = (char *)malloc(16001);
            size_t got = tail ? fread(tail, 1, 16000, l) : 0;
            if (tail) tail[got] = 0;
            fclose(l);
        }
    }
    for (int part = 0; part < 2; part++) {
        char *s = part == 0 ? src : tail, *e;
        if (!s) continue;
        if (part == 1) o += _snprintf(out + o, cap - o, "\n== the add-on's last log lines ==\n");
        if (part == 1 && tail_cut) { e = strchr(s, '\n'); s = e ? e + 1 : s + strlen(s); }
        while (s && *s && o < cap - 1500) {
            e = strchr(s, '\n');
            size_t n = e ? (size_t)(e - s) : strlen(s);
            if (n >= sizeof(line)) n = sizeof(line) - 1;
            memcpy(line, s, n);
            line[n] = 0;
            if (n && line[n - 1] == '\r') line[n - 1] = 0;
            /* log lines: "hh:mm:ss.mmm [ pid] L text" - only error/warning/info (never the debug lines,
             * where DirectPlay's traces and message dumps go) */
            char *br = part == 1 ? strstr(line, "] ") : NULL;
            if (part == 0 || (br && (br[2] == 'E' || br[2] == 'W' || br[2] == 'I'))) {
                nc_scrub(line, clean, sizeof(clean));
                o += _snprintf(out + o, cap - o, "%s\n", clean);
            }
            s = e ? e + 1 : NULL;
        }
    }
    free(src);
    free(tail);
    out[o < cap ? o : cap - 1] = 0;
    char user[64];
    DWORD usz = sizeof(user);
    replace_all(out, cap, SESSION_PREFIX, "\x01");   /* the test game names: "Connection test - <name>" */
    for (char *p = out; (p = strchr(p, '\x01')) != NULL; ) {
        char *e = p + 1;
        while (*e && *e != '\n' && *e != '"' && *e != ')' && *e != ',') e++;
        memmove(p + 1, e, strlen(e) + 1);
        *p = '\x02';
    }
    replace_all(out, cap, "\x02", SESSION_PREFIX "<name>");
    for (int i = 0; i < g_nseen; i++) replace_all(out, cap, g_seen_names[i], "<player>");
    if (strlen(g_name) >= 2) replace_all(out, cap, g_name, "<you>");
    if (GetUserNameA(user, &usz) && strlen(user) >= 2) replace_all(out, cap, user, "<you>");
    return out;
}

/* the send itself, on its own thread (WM_APP_SENT with the result) */
#define WM_APP_SENT (WM_APP + 9)
static DWORD WINAPI send_thread(LPVOID body)
{
    char *b = (char *)body, err[256], *enc = (char *)malloc(strlen(b) * 3 + 1), *post;
    int status = 0, ok = 0;
    char ref[64] = "";
    if (enc) {
        char *q = enc;
        for (const unsigned char *s = (const unsigned char *)b; *s; s++) {
            if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || strchr("-_.~", *s))
                *q++ = (char)*s;
            else {   /* (no sprintf: MinGW's brings code a Pentium MMX can't run - check9x.sh) */
                static const char hex[] = "0123456789ABCDEF";
                *q++ = '%';
                *q++ = hex[*s >> 4];
                *q++ = hex[*s & 15];
            }
        }
        *q = 0;
        size_t plen = strlen(enc) + 256;
        post = (char *)malloc(plen);
        if (post) {
            _snprintf(post, plen, "k=%s&a=report&kind=check&ver=%s&body=%s", g_key, OKP_VERSION_STR, enc);
            post[plen - 1] = 0;
            char *r = http_post(g_server, post, &status, err, sizeof(err));
            ok = r && status == 200 && !strncmp(r, "ok", 2);
            if (ok) sscanf(r, "ok %63s", ref);
            free(r);
            free(post);
        }
        free(enc);
    }
    free(b);
    PostMessageA(g_wnd, WM_APP_SENT, ok, (LPARAM)_strdup(ok ? ref : (status ? "the server refused it" : "no answer from the server")));
    return 0;
}

/* the button: show what would be sent, then send it */
static void send_report_clicked(HWND w)
{
    char *body = build_send_body();
    if (!body) return;
    for (;;) {
        int r = MessageBoxA(w, "Send these results to oldkid.net so the problem can be looked at?\n\n"
                               "They include the test results and the add-on's last log lines. Addresses, folders and "
                               "player names are taken out.\n\n"
                               "Yes = send     No = show me first     Cancel = don't send",
                            "Connection Check - Send report", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) { free(body); return; }
        if (r == IDYES) break;
        char path[MAX_PATH];
        _snprintf(path, sizeof(path), "%snetcheck-send-preview.txt", g_dir);
        FILE *f = fopen(path, "wb");
        if (f) {
            for (const char *p = body; *p; p++) { if (*p == '\n') fputc('\r', f); fputc(*p, f); }
            fclose(f);
            char cmd[MAX_PATH + 32];
            STARTUPINFOA si;
            PROCESS_INFORMATION pi;
            memset(&si, 0, sizeof(si));
            si.cb = sizeof(si);
            _snprintf(cmd, sizeof(cmd), "notepad.exe \"%s\"", path);
            if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
                WaitForSingleObject(pi.hProcess, INFINITE);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
            DeleteFileA(path);
        }
    }
    EnableWindow(h_btn[8], FALSE);
    HANDLE t = CreateThread(NULL, 0, send_thread, body, 0, NULL);
    if (t) CloseHandle(t);
    else { free(body); EnableWindow(h_btn[8], TRUE); }
}

static void append_out(const char *line)
{
    int len = GetWindowTextLengthA(h_out);
    if (len > 200000) { SetWindowTextA(h_out, ""); len = 0; }
    SendMessageA(h_out, EM_SETSEL, len, len);
    SendMessageA(h_out, EM_REPLACESEL, FALSE, (LPARAM)line);
    SendMessageA(h_out, EM_REPLACESEL, FALSE, (LPARAM)"\r\n");
}

static void update_buttons(void)
{
    int busy = g_busy != 0;
    EnableWindow(h_btn[0], !busy);
    EnableWindow(h_btn[1], !busy);
    EnableWindow(h_btn[2], !busy);
    EnableWindow(h_btn[3], busy);
    EnableWindow(h_btn[4], busy);
    EnableWindow(h_name, !busy);
    EnableWindow(h_conn, !busy);
    EnableWindow(h_transport, !busy);
    EnableWindow(h_joinsel, busy && SendMessageA(h_list, LB_GETCOUNT, 0, 0) > 0);
}

static void read_inputs(void)
{
    GetWindowTextA(h_name, g_name, sizeof(g_name));
    if (!g_name[0]) lstrcpyA(g_name, "Player");
    g_filter[0] = 0;
    g_lan = (int)SendMessageA(h_conn, CB_GETCURSEL, 0, 0) == 1;
    g_transport = (int)SendMessageA(h_transport, CB_GETCURSEL, 0, 0);
    if (g_transport < 0 || g_transport > 2) g_transport = 1;
}

static void paint_stats(HWND w, HDC dc)
{
    RECT rc;
    GetClientRect(w, &rc);
    FillRect(dc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
    FrameRect(dc, &rc, (HBRUSH)GetStockObject(GRAY_BRUSH));
    HFONT old = (HFONT)SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);
    static const COLORREF col[] = {RGB(0, 102, 204), RGB(204, 51, 0), RGB(0, 153, 51), RGB(153, 0, 153),
                                   RGB(204, 153, 0), RGB(0, 153, 153)};
    int y = 6;
    char line[256];
    EnterCriticalSection(&g_cs);
    int active = 0;
    for (int i = 0; i < npeers; i++) if (!peers[i].gone) active++;
    if (!g_status[0]) lstrcpynA(line, "Not in a test game", sizeof(line));
    else if (!active && !strncmp(g_status, "Looking", 7)) lstrcpynA(line, g_status, sizeof(line));
    else if (!active) _snprintf(line, sizeof(line), "%s - waiting for players", g_status);
    else _snprintf(line, sizeof(line), "%s - %d other player%s", g_status, active, active == 1 ? "" : "s");
    line[sizeof(line) - 1] = 0;
    TextOutA(dc, 8, y, line, (int)strlen(line));
    y += 20;
    int shown = 0, maxv = 100;
    for (int i = 0; i < npeers; i++) {
        if (peers[i].gone) continue;
        for (int k = 0; k < peers[i].hist_n; k++) {
            int v = peers[i].hist[HIST - peers[i].hist_n + k];
            if (v > maxv) maxv = v;
        }
        peer_line(&peers[i], line, sizeof(line));
        SetTextColor(dc, col[shown % 6]);
        TextOutA(dc, 8, y, line, (int)strlen(line));
        y += 18;
        shown++;
    }
    SetTextColor(dc, RGB(0, 0, 0));

    /* ping graph: the last minute, one point per half second; red marks = no answer */
    RECT g = {8, y + 8, rc.right - 8, rc.bottom - 20};
    if (g.bottom - g.top > 40) {
        maxv = (maxv + 99) / 100 * 100;
        HPEN grid = CreatePen(PS_DOT, 1, RGB(200, 200, 200));
        HPEN oldp = (HPEN)SelectObject(dc, grid);
        for (int k = 0; k <= 4; k++) {
            int yy = g.bottom - (g.bottom - g.top) * k / 4;
            MoveToEx(dc, g.left + 40, yy, NULL);
            LineTo(dc, g.right, yy);
            _snprintf(line, sizeof(line), "%d ms", maxv * k / 4);
            TextOutA(dc, g.left, yy - 7, line, (int)strlen(line));
        }
        SelectObject(dc, oldp);
        DeleteObject(grid);
        const char *cap = "ping over the last minute";
        TextOutA(dc, g.left + 40, g.bottom + 2, cap, (int)strlen(cap));
        int gw = g.right - g.left - 40;
        shown = 0;
        for (int i = 0; i < npeers; i++) {
            if (peers[i].gone) continue;
            HPEN pen = CreatePen(PS_SOLID, 2, col[shown % 6]);
            oldp = (HPEN)SelectObject(dc, pen);
            int started = 0;
            for (int k = 0; k < peers[i].hist_n; k++) {
                int v = peers[i].hist[HIST - peers[i].hist_n + k];
                int x = g.left + 40 + gw * (HIST - peers[i].hist_n + k) / (HIST - 1);
                if (v < 0) {
                    RECT m = {x - 1, g.top, x + 2, g.bottom};
                    HBRUSH red = CreateSolidBrush(RGB(255, 180, 180));
                    FillRect(dc, &m, red);
                    DeleteObject(red);
                    started = 0;
                    continue;
                }
                int yy = g.bottom - (g.bottom - g.top) * v / maxv;
                if (!started) MoveToEx(dc, x, yy, NULL); else LineTo(dc, x, yy);
                started = 1;
            }
            SelectObject(dc, oldp);
            DeleteObject(pen);
            shown++;
        }
    }
    LeaveCriticalSection(&g_cs);
    SelectObject(dc, old);
}

static LRESULT CALLBACK stats_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT rc;
        GetClientRect(w, &rc);
        HDC mem = CreateCompatibleDC(dc);   /* double-buffered: no flicker on the 500 ms refresh */
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HBITMAP ob = (HBITMAP)SelectObject(mem, bmp);
        paint_stats(w, mem);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(w, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcA(w, m, wp, lp);
}

static void layout(HWND w)
{
    RECT rc;
    GetClientRect(w, &rc);
    int W = rc.right, H = rc.bottom;
    MoveWindow(h_out, 10, 104, W / 2 - 15, H - 150, TRUE);
    MoveWindow(h_listlabel, W / 2 + 5, 104, W / 2 - 150, 18, TRUE);
    MoveWindow(h_joinsel, W - 140, 100, 130, 24, TRUE);
    MoveWindow(h_list, W / 2 + 5, 126, W / 2 - 15, 96, TRUE);
    MoveWindow(h_stats, W / 2 + 5, 228, W / 2 - 15, H - 274, TRUE);
    for (int i = 5; i < 9; i++) MoveWindow(h_btn[i], 10 + (i - 5) * 130, H - 38, 120, 28, TRUE);
}

static HWND mk(const char *cls, const char *text, DWORD style, int x, int y, int w, int h, HWND parent, int id)
{
    HWND c = CreateWindowExA(!strcmp(cls, "EDIT") ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, parent, (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)(id == ID_OUT ? g_mono : g_font), TRUE);
    return c;
}

static LRESULT CALLBACK main_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE: {
        g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        g_mono = CreateFontA(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, FIXED_PITCH, "Courier New");
        mk("STATIC", "Your name:", 0, 10, 14, 70, 20, w, 0);
        h_name = mk("EDIT", g_name, ES_AUTOHSCROLL | WS_TABSTOP, 80, 10, 140, 22, w, ID_NAME);
        mk("STATIC", "Connection:", 0, 236, 14, 70, 20, w, 0);
        h_conn = mk("COMBOBOX", "", CBS_DROPDOWNLIST | WS_TABSTOP, 306, 10, 130, 200, w, ID_CONN);
        SendMessageA(h_conn, CB_ADDSTRING, 0, (LPARAM)"Online Play");
        SendMessageA(h_conn, CB_ADDSTRING, 0, (LPARAM)"LAN Play");
        SendMessageA(h_conn, CB_SETCURSEL, g_lan ? 1 : 0, 0);
        mk("STATIC", "Transmit mode (when hosting):", 0, 452, 14, 150, 20, w, 0);
        h_transport = mk("COMBOBOX", "", CBS_DROPDOWNLIST | WS_TABSTOP, 602, 10, 270, 200, w, ID_TRANSPORT);
        for (int i = 0; i < 3; i++) SendMessageA(h_transport, CB_ADDSTRING, 0, (LPARAM)transport_labels[i]);
        SendMessageA(h_transport, CB_SETCURSEL, g_transport, 0);
        h_btn[0] = mk("BUTTON", "1. Check my connection", BS_PUSHBUTTON | WS_TABSTOP, 10, 44, 170, 30, w, ID_CHECK);
        h_btn[1] = mk("BUTTON", "2. Host a test game", BS_PUSHBUTTON | WS_TABSTOP, 190, 44, 150, 30, w, ID_HOST);
        h_btn[2] = mk("BUTTON", "3. Find test games", BS_PUSHBUTTON | WS_TABSTOP, 350, 44, 150, 30, w, ID_JOIN);
        h_btn[3] = mk("BUTTON", "Transmission test", BS_PUSHBUTTON | WS_TABSTOP, 510, 44, 130, 30, w, ID_XFER);
        h_btn[4] = mk("BUTTON", "Stop", BS_PUSHBUTTON | WS_TABSTOP, 650, 44, 80, 30, w, ID_STOP);
        mk("STATIC", "Check first. Then one player hosts a test game; the other clicks Find test games and "
                     "double-clicks it: you both see the ping and the transmission results.", 0, 10, 82, 900, 18, w, 0);
        h_listlabel = mk("STATIC", "Test games (double-click to join):", 0, 0, 0, 10, 10, w, 0);
        h_list = mk("LISTBOX", "", LBS_NOTIFY | WS_VSCROLL | WS_BORDER | WS_TABSTOP, 0, 0, 10, 10, w, ID_LIST);
        h_joinsel = mk("BUTTON", "Join selected", BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 10, 10, w, ID_JOINSEL);
        h_out = mk("EDIT", "", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 0, 0, 10, 10, w, ID_OUT);
        WNDCLASSA sc;
        memset(&sc, 0, sizeof(sc));
        sc.lpfnWndProc = stats_proc;
        sc.hInstance = GetModuleHandleA(NULL);
        sc.hCursor = LoadCursor(NULL, IDC_ARROW);
        sc.lpszClassName = "NetcheckStats";
        RegisterClassA(&sc);
        h_stats = CreateWindowExA(0, "NetcheckStats", "", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, w,
                                  (HMENU)(INT_PTR)ID_STATS, GetModuleHandleA(NULL), NULL);
        h_btn[5] = mk("BUTTON", "Copy report", BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 10, 10, w, ID_COPY);
        h_btn[6] = mk("BUTTON", "Save report", BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 10, 10, w, ID_SAVE);
        h_btn[7] = mk("BUTTON", "Open log folder", BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 10, 10, w, ID_LOGS);
        h_btn[8] = mk("BUTTON", "Send report...", BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 10, 10, w, ID_SEND);
        SetTimer(w, ID_TIMER, 500, NULL);
        update_buttons();
        return 0;
    }
    case WM_SIZE: layout(w); return 0;
    case WM_GETMINMAXINFO: ((MINMAXINFO *)lp)->ptMinTrackSize.x = 800; ((MINMAXINFO *)lp)->ptMinTrackSize.y = 480; return 0;
    case WM_TIMER: InvalidateRect(h_stats, NULL, FALSE); return 0;
    case WM_APP_LINE: {
        char *line = (char *)lp;
        append_out(line);
        free(line);
        return 0;
    }
    case WM_APP_DONE: update_buttons(); InvalidateRect(h_stats, NULL, FALSE); return 0;
    case WM_APP_SENT: {
        char msg[200], *ref = (char *)lp;
        if (wp) _snprintf(msg, sizeof(msg), "Sent - thank you.%s%s",
                          ref && ref[0] ? "\n\nIf you write to us about it, mention report " : "", ref && ref[0] ? ref : "");
        else _snprintf(msg, sizeof(msg), "The report could not be sent (%s).\n\nYou can use Save report and send the file instead.",
                       ref ? ref : "?");
        msg[sizeof(msg) - 1] = 0;
        free(ref);
        EnableWindow(h_btn[8], TRUE);
        MessageBoxA(w, msg, "Connection Check", MB_OK | (wp ? MB_ICONINFORMATION : MB_ICONWARNING));
        return 0;
    }
    case WM_APP_LIST: {
        /* refill, keeping the selected game selected */
        char sel[128] = "";
        int cur = (int)SendMessageA(h_list, LB_GETCURSEL, 0, 0);
        if (cur >= 0) SendMessageA(h_list, LB_GETTEXT, cur, (LPARAM)sel);
        SendMessageA(h_list, WM_SETREDRAW, FALSE, 0);
        SendMessageA(h_list, LB_RESETCONTENT, 0, 0);
        EnterCriticalSection(&g_cs);
        for (int i = 0; i < g_list.n; i++) {
            char row[200];
            _snprintf(row, sizeof(row), "%s   (%lu/%lu players)", g_list.names[i], (unsigned long)g_list.cur[i],
                      (unsigned long)g_list.max[i]);
            row[sizeof(row) - 1] = 0;
            SendMessageA(h_list, LB_ADDSTRING, 0, (LPARAM)row);
        }
        LeaveCriticalSection(&g_cs);
        if (sel[0]) {
            int at = (int)SendMessageA(h_list, LB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)sel);
            if (at >= 0) SendMessageA(h_list, LB_SETCURSEL, at, 0);
        }
        SendMessageA(h_list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(h_list, NULL, TRUE);
        update_buttons();
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_CHECK: read_inputs(); start_job(JOB_CHECK); update_buttons(); break;
        case ID_HOST: read_inputs(); g_job_secs = 0; start_job(JOB_HOST); update_buttons(); break;
        case ID_JOIN: read_inputs(); g_job_secs = 0; start_job(JOB_JOIN); update_buttons(); break;
        case ID_XFER: InterlockedExchange(&g_want_xfer, 1); break;
        case ID_LIST:
            if (HIWORD(wp) != LBN_DBLCLK) break;
            /* fall through: a double-click joins */
        case ID_JOINSEL: {
            int cur = (int)SendMessageA(h_list, LB_GETCURSEL, 0, 0);
            if (cur >= 0) InterlockedExchange(&g_pick, cur);
            break;
        }
        case ID_STOP: InterlockedExchange(&g_stop, 1); break;
        case ID_COPY: {
            EnterCriticalSection(&g_cs);
            size_t n = g_report_len;
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n + 1);
            if (h) {
                char *p = (char *)GlobalLock(h);
                if (g_report) memcpy(p, g_report, n);
                p[n] = 0;
                GlobalUnlock(h);
                if (OpenClipboard(w)) { EmptyClipboard(); SetClipboardData(CF_TEXT, h); CloseClipboard(); }
                else GlobalFree(h);
            }
            LeaveCriticalSection(&g_cs);
            break;
        }
        case ID_SAVE: {
            char saved[MAX_PATH], msg[MAX_PATH + 80];
            if (save_report(saved, sizeof(saved))) _snprintf(msg, sizeof(msg), "Saved:\n%s", saved);
            else _snprintf(msg, sizeof(msg), "Could not write\n%s", saved);
            MessageBoxA(w, msg, "Connection Check", MB_OK | MB_ICONINFORMATION);
            break;
        }
        case ID_SEND: send_report_clicked(w); break;
        case ID_LOGS: {
            char d[MAX_PATH];
            _snprintf(d, sizeof(d), "%snetplay-logs", g_dir);
            if (GetFileAttributesA(d) == INVALID_FILE_ATTRIBUTES) lstrcpynA(d, g_dir, sizeof(d));
            ShellExecuteA(w, "open", d, NULL, NULL, SW_SHOWNORMAL);
            break;
        }
        }
        return 0;
    case WM_CLOSE:
        if (g_busy) {
            InterlockedExchange(&g_stop, 1);
            for (int i = 0; i < 300 && g_busy; i++) Sleep(20);   /* leave the game cleanly */
        }
        DestroyWindow(w);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(w, m, wp, lp);
}

/* ------------------------------------------------------------------ main */

/* MinGW's prebuilt _matherr has SSE instructions (check9x.sh): ours replaces it, so the exe runs on
   a Pentium MMX. Math errors keep the CRT's default result. */
struct _exception;
int __cdecl _matherr(struct _exception *e) { (void)e; return 0; }

/* our own main (not WinMain): MinGW's prebuilt main->WinMain wrapper has cmov, which a Pentium MMX lacks */
int main(int argc, char **argv)
{
    HINSTANCE inst = GetModuleHandleA(NULL);
    int show = SW_SHOWNORMAL;
    g_t0 = GetTickCount();
    InitializeCriticalSection(&g_cs);
    srand(GetTickCount() ^ GetCurrentProcessId());
    GetModuleFileNameA(NULL, g_dir, sizeof(g_dir));
    char *bs = strrchr(g_dir, '\\');
    if (bs) bs[1] = 0;
    config_load();
    /* host open games without the drop-in's password box (builtin.c: test answers) */
    SetEnvironmentVariableA("OKP_TEST_HOST_PASSWORD", "<none>");
    _putenv("OKP_TEST_HOST_PASSWORD=<none>");
    /* no report boxes over this tool (okplay/src/report.c): they wait for the game */
    SetEnvironmentVariableA("OKP_REPORTS_NO_ASK", "1");
    _putenv("OKP_REPORTS_NO_ASK=1");
    DWORD nsz = sizeof(g_name);
    char user[64];
    if (GetUserNameA(user, &nsz)) lstrcpynA(g_name, user, sizeof(g_name));

    const char *job = NULL;
    DWORD secs = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cli")) g_cli = 1;
        else if (!strcmp(argv[i], "--lan")) g_lan = 1;
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) lstrcpynA(g_name, argv[++i], sizeof(g_name));
        else if (!strcmp(argv[i], "--session") && i + 1 < argc) lstrcpynA(g_filter, argv[++i], sizeof(g_filter));
        else if (!strcmp(argv[i], "--secs") && i + 1 < argc) secs = (DWORD)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dplayx") && i + 1 < argc) lstrcpynA(g_dplayx_arg, argv[++i], sizeof(g_dplayx_arg));
        else if (!strcmp(argv[i], "--transport") && i + 1 < argc) {
            const char *t = argv[++i];
            g_transport = !strcmp(t, "reliable") || !strcmp(t, "tcp") ? 2
                        : !strcmp(t, "udp+reliable") || !strcmp(t, "udp+tcp") ? 1 : 0;
        }
        else job = argv[i];
    }
    WSADATA wd;
    WSAStartup(MAKEWORD(1, 1), &wd);
    /* 1 ms ping resolution and short sleeps (GetTickCount steps in 15.6 ms): the command line runs
     * one test and ends; the window raises it only while a test runs (worker) */
    if (g_cli) timeBeginPeriod(1);
    if (g_cli) {
        typedef BOOL (WINAPI *ac_t)(DWORD);
        ac_t ac = (ac_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "AttachConsole");
        if (ac && GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_UNKNOWN) ac((DWORD)-1);
        int j = !job ? 0 : !strcmp(job, "check") ? JOB_CHECK : !strcmp(job, "host") ? JOB_HOST
              : !strcmp(job, "join") ? JOB_JOIN : 0;
        if (!j) {
            emit(K_INFO, "usage: netcheck --cli check|host|join [--lan] [--name N] [--session TEXT] [--secs N] "
                         "[--transport udp|udp+reliable|reliable] [--dplayx DLL]");
            return 2;
        }
        g_job_secs = secs ? secs : (j == JOB_HOST ? 120 : 40);
        CoInitialize(NULL);
        if (j == JOB_CHECK) check_all();
        else if (j == JOB_HOST) do_host(g_job_secs);
        else do_join(g_job_secs);
        emit(K_INFO, "RESULT %s (%d problem%s, %d warning%s)", g_fails ? "fail" : "ok", g_fails,
             g_fails == 1 ? "" : "s", g_warns, g_warns == 1 ? "" : "s");
        return g_fails ? 1 : 0;
    }

    CoInitialize(NULL);
    {   /* the manifest asks for the themed controls: XP needs comctl32 loaded first (looked up at run
         * time - check9x knows no comctl32 on Windows 98, and nothing here needs it there) */
        HMODULE cc = LoadLibraryA("comctl32.dll");
        void (WINAPI *icc)(void) = cc ? (void (WINAPI *)(void))GetProcAddress(cc, "InitCommonControls") : NULL;
        if (icc) icc();
    }
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = main_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(1));
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "Netcheck";
    RegisterClassA(&wc);
    /* WS_CLIPCHILDREN: the window's background is not painted under its controls (they flickered
     * and repainted twice whenever part of the window was redrawn) */
    HWND w = CreateWindowExA(0, "Netcheck", "Connection Check - Online and LAN Play", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1000, 640, NULL, NULL, inst, NULL);
    g_wnd = w;
    ShowWindow(w, show);
    UpdateWindow(w);
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageA(w, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    return 0;
}
