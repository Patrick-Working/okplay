/* dpweb_test.c - our DirectPlay in the browser, end to end, the way a 1999 game uses it.
 *
 * Two tabs: ?role=host and ?role=join (dpweb_test.html). Both create DirectPlay on the
 * "Oldkid.net Online Play" provider (DirectPlayCreate with its GUID, as games do), the host
 * opens a session and a player, the joiner finds it with EnumSessions(ASYNC), joins, creates a
 * player and pings the host's player; the host answers each ping. Plain DirectPlay 4 calls only:
 * dplayx does the protocol, okweb does the transport. Written to the page as PASS/FAIL lines.
 * Needs -sASYNCIFY (DirectPlay waits for replies while joining). */
#include "winweb.h"
#include <emscripten.h>
#include "dplay.h"

static const GUID OKP_SPGUID = { 0x66d8dc59, 0x28e8, 0x4d3e, { 0xba, 0x93, 0x2e, 0x98, 0xd2, 0x51, 0xc0, 0xf3 } };
/* a test application (not a real game, so no game's list shows it) */
static const GUID TEST_APP = { 0x5e5e0001, 0x1234, 0x4321, { 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x07, 0x18 } };

static int fails;
EM_JS(void, out_line, (const char *s), { const t = UTF8ToString(s); console.log(t); Module.dpwebLine && Module.dpwebLine(t); });
EM_JS(int, is_host_role, (void), { return new URLSearchParams(location.search).get('role') === 'host' ? 1 : 0; });
EM_JS(void, done, (int f), { Module.dpwebDone && Module.dpwebDone(f); });

static void line(const char *fmt, ...)
{
    char b[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    out_line(b);
}
static void check(int ok, const char *what) { line("%s %s", ok ? "PASS" : "FAIL", what); if (!ok) fails++; }

static IDirectPlay4A *create(void)
{
    LPDIRECTPLAY dp1 = NULL;
    IDirectPlay4A *dp = NULL;
    GUID sp = OKP_SPGUID;
    HRESULT hr = DirectPlayCreate(&sp, &dp1, NULL);
    check(hr == DP_OK && dp1, "DirectPlayCreate(Oldkid.net Online Play)");
    if (hr != DP_OK) { line("  DirectPlayCreate: 0x%08lx", (unsigned long)hr); if (dp1) IDirectPlay_Release(dp1); return NULL; }
    hr = IDirectPlay_QueryInterface(dp1, &IID_IDirectPlay4A, (void **)&dp);
    IDirectPlay_Release(dp1);
    check(hr == DP_OK && dp, "QueryInterface(IID_IDirectPlay4A)");
    return dp;
}

/* ?vanish (host): leave the page mid-session without closing DirectPlay, as a closed or crashed tab
 * does; ?hostgone (join): then expect DirectPlay to report the host's machine gone */
EM_JS(int, has_param, (const char *name), { return new URLSearchParams(location.search).has(UTF8ToString(name)) ? 1 : 0; });
EM_JS(void, leave_page, (void), { location.href = 'about:blank'; });
static int g_seen_destroy, g_seen_host, g_seen_lost;
static DWORD g_t_mark;

/* receive everything waiting; returns how many application messages matched `want` */
static int pump(IDirectPlay4A *dp, DPID me, const char *want, DPID *from_out, int *joined)
{
    static char buf[4096];
    int got = 0;
    for (;;) {
        DPID from = 0, to = 0;
        DWORD size = sizeof(buf);
        HRESULT hr = IDirectPlayX_Receive(dp, &from, &to, DPRECEIVE_ALL, buf, &size);
        if (hr == DPERR_NOMESSAGES) break;
        if (hr != DP_OK) { static int told; if (!told++) line("Receive: 0x%08lx", (unsigned long)hr); break; }
        if (from == DPID_SYSMSG) {
            DPMSG_GENERIC *g = (DPMSG_GENERIC *)buf;
            if (g->dwType == DPSYS_CREATEPLAYERORGROUP) {
                DPMSG_CREATEPLAYERORGROUP *c = (DPMSG_CREATEPLAYERORGROUP *)buf;
                line("  system: player %lu joined (%s)", (unsigned long)c->dpId,
                     c->dpnName.lpszShortNameA ? c->dpnName.lpszShortNameA : "?");
                if (joined) (*joined)++;
            } else {
                line("  system message 0x%lx after %lu ms", (unsigned long)g->dwType, (unsigned long)(GetTickCount() - g_t_mark));
                if (g->dwType == DPSYS_DESTROYPLAYERORGROUP) g_seen_destroy++;
                if (g->dwType == DPSYS_HOST) g_seen_host++;
                if (g->dwType == DPSYS_SESSIONLOST) g_seen_lost++;
            }
            continue;
        }
        buf[size < sizeof(buf) ? size : sizeof(buf) - 1] = 0;
        if (want && !strncmp(buf, want, strlen(want))) { got++; if (from_out) *from_out = from; }
        if (!strncmp(buf, "ping ", 5)) {   /* the host answers every ping */
            char reply[64];
            snprintf(reply, sizeof(reply), "pong %s", buf + 5);
            IDirectPlayX_Send(dp, me, from, DPSEND_GUARANTEED, reply, (DWORD)strlen(reply) + 1);
        }
    }
    return got;
}

static BOOL FAR PASCAL player_cb(DPID id, DWORD type, LPCDPNAME name, DWORD flags, LPVOID ctx);

static BOOL FAR PASCAL session_cb(LPCDPSESSIONDESC2 sd, LPDWORD timeout, DWORD flags, LPVOID ctx)
{
    if (flags & DPESC_TIMEDOUT) return FALSE;
    if (IsEqualGUID(&sd->guidApplication, &TEST_APP)) *(GUID *)ctx = sd->guidInstance;
    return TRUE;
}

static void host(void)
{
    IDirectPlay4A *dp = create();
    if (!dp) return;
    DPSESSIONDESC2 sd;
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.guidApplication = TEST_APP;
    sd.dwMaxPlayers = 4;
    sd.dwFlags = DPSESSION_KEEPALIVE | DPSESSION_MIGRATEHOST;
    sd.lpszSessionNameA = (char *)"dpweb test";
    HRESULT hr = IDirectPlayX_Open(dp, &sd, DPOPEN_CREATE);
    check(hr == DP_OK, "host: Open(DPOPEN_CREATE)");
    DPNAME name = { sizeof(DPNAME), 0 };
    name.lpszShortNameA = (char *)"Hana";
    DPID me = 0;
    hr = IDirectPlayX_CreatePlayer(dp, &me, &name, NULL, NULL, 0, 0);
    check(hr == DP_OK && me, "host: CreatePlayer");
    int joined = 0;
    DWORD t0 = GetTickCount();
    line("host: waiting for a player (open the join tab)");
    while (GetTickCount() - t0 < 120000) {
        pump(dp, me, NULL, NULL, &joined);
        if (joined && GetTickCount() - t0 > 2000) {
            DWORD n = 0;
            IDirectPlayX_GetMessageCount(dp, me, &n);
        }
        if (joined >= 1) break;
        emscripten_sleep(50);
    }
    check(joined >= 1, "host: a player joined");
    DWORD t1 = GetTickCount();
    DWORD stay = has_param("vanish") ? 6000 : 15000;
    while (GetTickCount() - t1 < stay) { pump(dp, me, NULL, NULL, &joined); emscripten_sleep(20); }   /* answer pings */
    if (has_param("vanish")) { line("host: leaving the page without closing"); leave_page(); emscripten_sleep(60000); }
    IDirectPlayX_Close(dp);
    IDirectPlayX_Release(dp);
}

static void join(void)
{
    IDirectPlay4A *dp = create();
    if (!dp) return;
    DPSESSIONDESC2 sd;
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.guidApplication = TEST_APP;
    GUID found = GUID_NULL;
    DWORD t0 = GetTickCount();
    while (IsEqualGUID(&found, &GUID_NULL) && GetTickCount() - t0 < 30000) {
        IDirectPlayX_EnumSessions(dp, &sd, 0, session_cb, &found, DPENUMSESSIONS_AVAILABLE | DPENUMSESSIONS_ASYNC);
        emscripten_sleep(300);
    }
    check(!IsEqualGUID(&found, &GUID_NULL), "join: EnumSessions(ASYNC) found the session");
    if (IsEqualGUID(&found, &GUID_NULL)) return;
    IDirectPlayX_EnumSessions(dp, &sd, 0, session_cb, &found, DPENUMSESSIONS_STOPASYNC);
    sd.guidInstance = found;
    DWORD tj = GetTickCount();
    HRESULT hr = IDirectPlayX_Open(dp, &sd, DPOPEN_JOIN);
    check(hr == DP_OK, "join: Open(DPOPEN_JOIN)");
    line("  (join took %lu ms)", (unsigned long)(GetTickCount() - tj));
    if (hr != DP_OK) { line("  Open: 0x%08lx", (unsigned long)hr); return; }
    DPNAME name = { sizeof(DPNAME), 0 };
    name.lpszShortNameA = (char *)"Ali";
    DPID me = 0;
    hr = IDirectPlayX_CreatePlayer(dp, &me, &name, NULL, NULL, 0, 0);
    check(hr == DP_OK && me, "join: CreatePlayer");
    int joined = 0;
    DPID host_player = 0;
    pump(dp, me, NULL, NULL, &joined);   /* (no DPSYS_CREATEPLAYERORGROUP for players who were there first:
                                            Windows' DirectPlay doesn't send those either - EnumPlayers lists them) */
    /* the host's player (the one remote player), and ping it */
    IDirectPlayX_EnumPlayers(dp, NULL, player_cb, &host_player, DPENUMPLAYERS_REMOTE);
    check(host_player != 0, "join: EnumPlayers lists the host's player");
    int pongs = 0;
    for (int i = 0; i < 10; i++) {
        char msg[32];
        snprintf(msg, sizeof(msg), "ping %d", i);
        IDirectPlayX_Send(dp, me, host_player, DPSEND_GUARANTEED, msg, (DWORD)strlen(msg) + 1);
    }
    DWORD tp = GetTickCount();
    while (pongs < 10 && GetTickCount() - tp < 10000) { pongs += pump(dp, me, "pong ", NULL, NULL); emscripten_sleep(20); }
    line("  %d of 10 pongs", pongs);
    check(pongs == 10, "join: 10 guaranteed pings answered");
    if (has_param("hostgone")) {
        /* the host tab leaves (?vanish) within a few seconds: its machine has to go, not linger */
        g_t_mark = GetTickCount();
        while (GetTickCount() - g_t_mark < 45000 && !g_seen_destroy && !g_seen_lost) { pump(dp, me, NULL, NULL, NULL); emscripten_sleep(50); }
        line("  host gone after %lu ms: destroy %d, host %d, session lost %d", (unsigned long)(GetTickCount() - g_t_mark),
             g_seen_destroy, g_seen_host, g_seen_lost);
        check(g_seen_destroy || g_seen_lost, "join: the host's machine is reported gone");
        DWORD t2 = GetTickCount();
        while (GetTickCount() - t2 < 1500) { pump(dp, me, NULL, NULL, NULL); emscripten_sleep(50); }   /* and what follows */
    }
    IDirectPlayX_Close(dp);
    IDirectPlayX_Release(dp);
}

static BOOL FAR PASCAL player_cb(DPID id, DWORD type, LPCDPNAME name, DWORD flags, LPVOID ctx)
{
    *(DPID *)ctx = id;
    return TRUE;
}

int main(void)
{
    line("dpweb test (%s)", is_host_role() ? "host" : "join");
    if (is_host_role()) host(); else join();
    line("done: %d failed", fails);
    done(fails);
    return 0;
}
