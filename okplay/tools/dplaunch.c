/* dplaunch - a minimal DirectPlay lobby program: starts a lobbyable game into a classic TCP/IP
 * session through the DirectPlay lobby API (IDirectPlayLobby::RunApplication), the way a lobby
 * client (MSN Gaming Zone, ...) does. Tests run it on a machine WITHOUT the drop-in, so the game
 * plays on Microsoft's own DirectPlay - the other end of an interop test against ours.
 *
 *   dplaunch host <session> <player> [max players] [--stay S]
 *   dplaunch join <session> <player> <host address> [--stay S]
 *
 * join finds the session by name first (EnumSessions on TCP/IP to the host's address) - a lobby
 * hands the game the session's instance GUID. The program stays S seconds (default 600) after the
 * launch, answering the game's lobby messages: GETPROPERTY gets DPERR_UNKNOWNMESSAGE, as from a
 * lobby that keeps no properties. The game: Age of Empires II: The Conquerors (its lobby GUID).
 * Uses only the documented DirectPlay API, loaded from the system's dplayx.dll at run time.
 */
#define INITGUID
#include <windows.h>
#include <dplay.h>
#include <dplobby.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const GUID AOE2TC_APP = { 0x5de93f3f, 0xfc90, 0x4ee1, { 0xae, 0x5a, 0x63, 0xda, 0xfa, 0x05, 0x59, 0x50 } };

typedef HRESULT (WINAPI *lobbycreate_t)(LPGUID, LPDIRECTPLAYLOBBYA *, IUnknown *, LPVOID, DWORD);
typedef HRESULT (WINAPI *dpcreate_t)(LPGUID, LPDIRECTPLAY *, IUnknown *);

static DWORD t0;
static void say(const char *fmt, ...)
{
    va_list ap;
    printf("[%6lu] ", GetTickCount() - t0);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

/* a TCP/IP address: provider, and the host's address when there is one */
static void *tcpip_address(LPDIRECTPLAYLOBBY3A lobby, const char *ip, DWORD *size)
{
    DPCOMPOUNDADDRESSELEMENT el[2];
    DWORD n = 1;
    void *a;
    el[0].guidDataType = DPAID_ServiceProvider;
    el[0].dwDataSize = sizeof(GUID);
    el[0].lpData = (void *)&DPSPGUID_TCPIP;
    if (ip && *ip) {
        el[1].guidDataType = DPAID_INet;
        el[1].dwDataSize = (DWORD)strlen(ip) + 1;
        el[1].lpData = (void *)ip;
        n = 2;
    }
    *size = 0;
    if (IDirectPlayLobby_CreateCompoundAddress(lobby, el, n, NULL, size) != DPERR_BUFFERTOOSMALL) return NULL;
    if (!(a = calloc(1, *size))) return NULL;
    if (FAILED(IDirectPlayLobby_CreateCompoundAddress(lobby, el, n, a, size))) { free(a); return NULL; }
    return a;
}

struct find { const char *name; GUID instance; int found; };

static BOOL CALLBACK on_session(LPCDPSESSIONDESC2 d, LPDWORD timeout, DWORD flags, LPVOID ctx)
{
    struct find *f = (struct find *)ctx;
    if (flags & DPESC_TIMEDOUT) return FALSE;
    say("session \"%s\" %lu/%lu players", d->lpszSessionNameA ? d->lpszSessionNameA : "?",
        d->dwCurrentPlayers, d->dwMaxPlayers);
    if (d->lpszSessionNameA && !strcmp(d->lpszSessionNameA, f->name)) {
        f->instance = d->guidInstance;
        f->found = 1;
    }
    return TRUE;
}

/* the session's instance GUID, by name, from the host's address */
static int find_session(HMODULE dll, LPDIRECTPLAYLOBBY3A lobby, const char *ip, const char *name, GUID *instance)
{
    dpcreate_t create = (dpcreate_t)GetProcAddress(dll, "DirectPlayCreate");
    LPDIRECTPLAY dp1 = NULL;
    LPDIRECTPLAY4A dp = NULL;
    DPSESSIONDESC2 sd;
    struct find f = { name, { 0 }, 0 };
    DWORD size;
    void *addr;
    HRESULT hr;
    int tries;
    static GUID none;   /* GUID_NULL: an uninitialised object */
    if (!create || FAILED(create(&none, &dp1, NULL))) { say("DirectPlayCreate failed"); return 0; }
    hr = IDirectPlay_QueryInterface(dp1, &IID_IDirectPlay4A, (void **)&dp);
    IDirectPlay_Release(dp1);
    if (FAILED(hr)) { say("no IDirectPlay4A: 0x%08lx", hr); return 0; }
    if (!(addr = tcpip_address(lobby, ip, &size)) || FAILED(hr = IDirectPlayX_InitializeConnection(dp, addr, 0))) {
        say("InitializeConnection (TCP/IP %s) failed: 0x%08lx", ip, hr);
        IDirectPlayX_Release(dp);
        free(addr);
        return 0;
    }
    free(addr);
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.guidApplication = AOE2TC_APP;
    for (tries = 0; tries < 30 && !f.found; tries++) {
        hr = IDirectPlayX_EnumSessions(dp, &sd, 1500, on_session, &f, DPENUMSESSIONS_AVAILABLE);
        if (FAILED(hr)) say("EnumSessions: 0x%08lx", hr);
        if (!f.found) Sleep(1000);
    }
    IDirectPlayX_Release(dp);
    if (f.found) *instance = f.instance;
    return f.found;
}

int main(int argc, char **argv)
{
    HMODULE dll;
    lobbycreate_t lcreate;
    LPDIRECTPLAYLOBBYA lobby1 = NULL;
    LPDIRECTPLAYLOBBY3A lobby = NULL;
    DPLCONNECTION conn;
    DPSESSIONDESC2 sd;
    DPNAME nm;
    DWORD appid = 0, size, stay = 600, maxp = 2;
    void *addr;
    const char *ip = NULL;
    HRESULT hr;
    int host, i, argn = 0;
    char *args[5];

    t0 = GetTickCount();
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--stay") && i + 1 < argc) stay = (DWORD)atoi(argv[++i]);
        else if (argn < 5) args[argn++] = argv[i];
    }
    if (argn < 3 || (strcmp(args[0], "host") && strcmp(args[0], "join")) || (!strcmp(args[0], "join") && argn < 4)) {
        fprintf(stderr, "usage: dplaunch host <session> <player> [max players] [--stay S]\n"
                        "       dplaunch join <session> <player> <host address> [--stay S]\n");
        return 2;
    }
    host = !strcmp(args[0], "host");
    if (host && argn >= 4) maxp = (DWORD)atoi(args[3]);
    if (!host) ip = args[3];

    if (!(dll = LoadLibraryA("dplayx.dll")) || !(lcreate = (lobbycreate_t)GetProcAddress(dll, "DirectPlayLobbyCreateA"))) {
        say("RESULT fail: no DirectPlay on this machine");
        return 1;
    }
    {
        char path[MAX_PATH];
        GetModuleFileNameA(dll, path, sizeof(path));
        say("dplayx %s", path);
    }
    if (FAILED(hr = lcreate(NULL, &lobby1, NULL, NULL, 0))) { say("RESULT fail: DirectPlayLobbyCreate 0x%08lx", hr); return 1; }
    hr = IDirectPlayLobby_QueryInterface(lobby1, &IID_IDirectPlayLobby3A, (void **)&lobby);
    IDirectPlayLobby_Release(lobby1);
    if (FAILED(hr)) { say("RESULT fail: no IDirectPlayLobby3A (DirectX 6 or later needed): 0x%08lx", hr); return 1; }

    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DPSESSION_KEEPALIVE | DPSESSION_MIGRATEHOST;
    sd.guidApplication = AOE2TC_APP;
    sd.dwMaxPlayers = maxp;
    sd.lpszSessionNameA = args[1];
    sd.lpszPasswordA = (char *)"";
    if (!host) {
        if (!find_session(dll, lobby, ip, args[1], &sd.guidInstance)) { say("RESULT fail: session \"%s\" not found at %s", args[1], ip); return 1; }
        say("found \"%s\" at %s", args[1], ip);
    }
    memset(&nm, 0, sizeof(nm));
    nm.dwSize = sizeof(nm);
    nm.lpszShortNameA = nm.lpszLongNameA = args[2];
    if (!(addr = tcpip_address(lobby, ip, &size))) { say("RESULT fail: address"); return 1; }
    memset(&conn, 0, sizeof(conn));
    conn.dwSize = sizeof(conn);
    conn.dwFlags = host ? DPLCONNECTION_CREATESESSION : DPLCONNECTION_JOINSESSION;
    conn.lpSessionDesc = &sd;
    conn.lpPlayerName = &nm;
    conn.guidSP = DPSPGUID_TCPIP;
    conn.lpAddress = addr;
    conn.dwAddressSize = size;
    hr = IDirectPlayLobby_RunApplication(lobby, 0, &appid, &conn, NULL);
    if (FAILED(hr)) { say("RESULT fail: RunApplication 0x%08lx (is the game registered for lobbies?)", hr); return 1; }
    say("RunApplication: %s \"%s\" as \"%s\" over TCP/IP%s%s - app id %lu", host ? "host" : "join", args[1], args[2],
        ip ? " to " : "", ip ? ip : "", appid);

    /* stay, answering the game's lobby messages */
    for (DWORD start = GetTickCount(); GetTickCount() - start < stay * 1000;) {
        BYTE buf[1024];
        DWORD flags = 0, n = sizeof(buf);
        hr = IDirectPlayLobby_ReceiveLobbyMessage(lobby, 0, appid, &flags, buf, &n);
        if (hr == DPERR_NOMESSAGES) { Sleep(100); continue; }
        if (FAILED(hr)) { say("ReceiveLobbyMessage: 0x%08lx", hr); Sleep(500); continue; }
        {
            /* (GETPROPERTY comes as a standard message, the DPLSYS_ ones as system messages) */
            DWORD type = *(DWORD *)buf;
            say("lobby %s message 0x%lx, %lu bytes", flags & DPLMSG_SYSTEM ? "system" : "standard", type, n);
            if (type == DPLSYS_GETPROPERTY && n >= sizeof(DPLMSG_GETPROPERTY)) {
                DPLMSG_GETPROPERTY *q = (DPLMSG_GETPROPERTY *)buf;
                DPLMSG_GETPROPERTYRESPONSE r;
                memset(&r, 0, sizeof(r));
                r.dwType = DPLSYS_GETPROPERTYRESPONSE;
                r.dwRequestID = q->dwRequestID;
                r.guidPlayer = q->guidPlayer;
                r.guidPropertyTag = q->guidPropertyTag;
                r.hr = DPERR_UNKNOWNMESSAGE;
                hr = IDirectPlayLobby_SendLobbyMessage(lobby, 0, appid, &r, sizeof(r));
                say("  GETPROPERTY answered DPERR_UNKNOWNMESSAGE (0x%08lx)", hr);
            }
            if ((flags & DPLMSG_SYSTEM) && type == DPLSYS_APPTERMINATED) { say("the game ended"); break; }
        }
    }
    IDirectPlayLobby_Release(lobby);
    say("RESULT ok");
    return 0;
}

/* MinGW's prebuilt _matherr has SSE instructions (check9x.sh): ours replaces it, so the exe runs on
 * a Pentium MMX (as in tools/netcheck.c) */
#include <math.h>
int __cdecl _matherr(struct _exception *e) { (void)e; return 0; }
