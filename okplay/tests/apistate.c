/* apistate - DirectPlay API calls in awkward states: does a dplayx crash, and does it answer like
 * Windows' does?
 *
 *   apistate [--dplayx DLL] [--sp ipx|auto|tcpip] STATE   one state, prints "call <name> <hr>" lines
 *   apistate [--dplayx DLL] [--sp ipx|auto|tcpip] all     every state, each in its own process
 *                                                          (a crash is reported, not fatal)
 * States: fresh (no connection), init (provider loaded, no session), failedjoin (Open(join) of a
 * session that isn't there), closed (hosted, then Close), open (hosting: first every call with
 * unknown ids, then the same methods with a real player and group), dp1host (a session opened
 * through IDirectPlay::Open, as DirectX 3 games do). apistate_dp1.c: the IDirectPlay calls, with
 * what they return ("data" lines) as well as their HRESULTs.
 * Every IDirectPlay4A method is called, plus QueryInterface for each DirectPlay interface.
 * --sp auto/ipx run LAN Play (no network needed; ipx = the dev slot, from okplay's bin\ so it finds
 * dpwsockx.dll). --sp tcpip uses Windows' own TCP/IP provider: the same provider under Windows'
 * dplayx (no --dplayx) and under ours, so apidiff.sh can compare the two dplayx layers alone.
 */
#define INITGUID
#include <windows.h>
#include <dplay.h>
#include <dplobby.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

DEFINE_GUID(OKP_LANGUID, 0x6bcff704, 0xce20, 0x4150, 0x98, 0xb4, 0x5a, 0x71, 0x98, 0x17, 0x5d, 0x7d);
DEFINE_GUID(APISTATE_APP, 0x7a1e0c55, 0x2b8d, 0x4f11, 0x9c, 0x3e, 0x41, 0x6a, 0x2d, 0x8b, 0x77, 0x10);

static IDirectPlay4A *dp;
static IDirectPlayLobby3A *lob;

static HRESULT create(const char *dll)
{
    HRESULT hr;
    if (!dll || !*dll) {
        if (FAILED(hr = CoCreateInstance(&CLSID_DirectPlay, NULL, CLSCTX_INPROC_SERVER, &IID_IDirectPlay4A, (void **)&dp)))
            return hr;
        return CoCreateInstance(&CLSID_DirectPlayLobby, NULL, CLSCTX_INPROC_SERVER, &IID_IDirectPlayLobby3A, (void **)&lob);
    }
    HMODULE m = LoadLibraryA(dll);
    typedef HRESULT (WINAPI *gco_t)(REFCLSID, REFIID, void **);
    gco_t gco = m ? (gco_t)GetProcAddress(m, "DllGetClassObject") : NULL;
    IClassFactory *cf;
    if (!gco) return E_FAIL;
    if (FAILED(hr = gco(&CLSID_DirectPlay, &IID_IClassFactory, (void **)&cf))) return hr;
    hr = cf->lpVtbl->CreateInstance(cf, NULL, &IID_IDirectPlay4A, (void **)&dp);
    cf->lpVtbl->Release(cf);
    if (FAILED(hr)) return hr;
    if (FAILED(hr = gco(&CLSID_DirectPlayLobby, &IID_IClassFactory, (void **)&cf))) return hr;
    hr = cf->lpVtbl->CreateInstance(cf, NULL, &IID_IDirectPlayLobby3A, (void **)&lob);
    cf->lpVtbl->Release(cf);
    return hr;
}

static void *connection_for(const char *sp, DWORD *size)
{
    DPCOMPOUNDADDRESSELEMENT el[2];
    int n = 1;
    el[0].guidDataType = DPAID_ServiceProvider;
    el[0].dwDataSize = sizeof(GUID);
    el[0].lpData = (void *)(!strcmp(sp, "auto") ? &OKP_LANGUID : !strcmp(sp, "tcpip") ? &DPSPGUID_TCPIP : &DPSPGUID_IPX);
    if (!strcmp(sp, "tcpip")) {
        /* an (empty = broadcast) address: without one Windows' TCP/IP provider answers every
           EnumSessions with its "Locate Session" dialog - on the user's screen, and the call hangs on it */
        el[1].guidDataType = DPAID_INet;
        el[1].dwDataSize = 1;
        el[1].lpData = (void *)"";
        n = 2;
    }
    *size = 0;
    IDirectPlayLobby_CreateCompoundAddress(lob, el, n, NULL, size);
    void *addr = malloc(*size);
    IDirectPlayLobby_CreateCompoundAddress(lob, el, n, addr, size);
    return addr;
}

static HRESULT init_connection(const char *sp)
{
    DWORD sz;
    void *addr = connection_for(sp, &sz);
    return IDirectPlayX_InitializeConnection(dp, addr, 0);
}

static void sd_init(DPSESSIONDESC2 *sd, char *name)
{
    memset(sd, 0, sizeof(*sd));
    sd->dwSize = sizeof(*sd);
    sd->dwFlags = DPSESSION_KEEPALIVE | DPSESSION_MIGRATEHOST;
    sd->guidApplication = APISTATE_APP;
    sd->dwMaxPlayers = 8;
    sd->lpszSessionNameA = name;
}

static BOOL FAR PASCAL enum_players_cb(DPID id, DWORD type, LPCDPNAME name, DWORD flags, LPVOID ctx)
{
    (void)id; (void)type; (void)name; (void)flags;
    (*(int *)ctx)++;
    return TRUE;
}

static BOOL FAR PASCAL enum_sessions_cb(LPCDPSESSIONDESC2 sd, LPDWORD timeout, DWORD flags, LPVOID ctx)
{
    (void)sd; (void)timeout;
    /* DPESC_TIMEDOUT: TRUE would mean "enumerate again" - forever, on Windows and on ours alike */
    if (flags & DPESC_TIMEDOUT) return FALSE;
    (*(int *)ctx)++;
    return TRUE;
}

static BOOL FAR PASCAL enum_connections_cb(LPCGUID sp, LPVOID conn, DWORD size, LPCDPNAME name, DWORD flags, LPVOID ctx)
{
    (void)sp; (void)conn; (void)size; (void)name; (void)flags;
    (*(int *)ctx)++;
    return TRUE;
}

/* the session's NEWPLAYERSDISABLED / JOINDISABLED bits, or GetSessionDesc's error */
static HRESULT session_flags(void)
{
    char sdbuf[1024];
    DWORD sz = sizeof(sdbuf);
    HRESULT hr = IDirectPlayX_GetSessionDesc(dp, sdbuf, &sz);
    if (FAILED(hr)) return hr;
    return (HRESULT)(((DPSESSIONDESC2 *)sdbuf)->dwFlags & (DPSESSION_NEWPLAYERSDISABLED | DPSESSION_JOINDISABLED));
}

#define CALL(name, expr) do { HRESULT h_ = (expr); printf("call %-26s 0x%08lx\n", name, (unsigned long)h_); fflush(stdout); } while (0)

#include "apistate_dp1.c"   /* IDirectPlay (DirectPlay 1) */

/* every IDirectPlay4A method with the ids given (unknown ones, or a real player/group) */
static void method_calls(const char *tag, DPID pid, DPID gid, const char *sp)
{
    char buf[512], label[64];
    DWORD sz, flags, n4;
    DPID from = 0, to = 0, newgid = 0, msgid = 0;
    int n = 0;
    DPNAME pn;
    DPSESSIONDESC2 sd;
    DPCHAT chat;
    DPCAPS caps;
    DPLCONNECTION conn;
    DPSECURITYDESC sec;

    memset(&pn, 0, sizeof(pn));
    pn.dwSize = sizeof(pn);
    pn.lpszShortNameA = (char *)"x";
    sd_init(&sd, (char *)"apistate");
    memset(&chat, 0, sizeof(chat));
    chat.dwSize = sizeof(chat);
    chat.lpszMessageA = (char *)"hi";
    memset(&caps, 0, sizeof(caps));
    caps.dwSize = sizeof(caps);
    memset(&conn, 0, sizeof(conn));
    conn.dwSize = sizeof(conn);
    memset(&sec, 0, sizeof(sec));
    sec.dwSize = sizeof(sec);
    memset(buf, 0, sizeof(buf));

#define C(name, expr) do { snprintf(label, sizeof(label), "%s%s", tag, name); CALL(label, expr); } while (0)
    sz = 0;
    C("GetSessionDesc", IDirectPlayX_GetSessionDesc(dp, NULL, &sz));
    C("SetSessionDesc", IDirectPlayX_SetSessionDesc(dp, &sd, 0));
    {
        /* IDirectPlay (1)'s EnableNewPlayers, and what it does to the session flags (AoE2 calls it
           with FALSE when a game starts) */
        IDirectPlay *dp1 = NULL;
        if (SUCCEEDED(IDirectPlayX_QueryInterface(dp, &IID_IDirectPlay, (void **)&dp1)) && dp1) {
            C("EnableNewPlayers(FALSE)", IDirectPlay_EnableNewPlayers(dp1, FALSE));
            C("  session flags after", session_flags());
            C("EnableNewPlayers(TRUE)", IDirectPlay_EnableNewPlayers(dp1, TRUE));
            C("  session flags after", session_flags());
            IDirectPlay_Release(dp1);
        }
    }
    C("EnumPlayers", IDirectPlayX_EnumPlayers(dp, NULL, enum_players_cb, &n, 0));
    C("EnumGroups", IDirectPlayX_EnumGroups(dp, NULL, enum_players_cb, &n, 0));
    C("EnumGroupPlayers", IDirectPlayX_EnumGroupPlayers(dp, gid, NULL, enum_players_cb, &n, 0));
    C("EnumGroupsInGroup", IDirectPlayX_EnumGroupsInGroup(dp, gid, NULL, enum_players_cb, &n, 0));
    C("GetPlayerName", (sz = sizeof(buf), IDirectPlayX_GetPlayerName(dp, pid, buf, &sz)));
    C("SetPlayerName", IDirectPlayX_SetPlayerName(dp, pid, &pn, 0));
    C("GetGroupName", (sz = sizeof(buf), IDirectPlayX_GetGroupName(dp, gid, buf, &sz)));
    C("SetGroupName", IDirectPlayX_SetGroupName(dp, gid, &pn, 0));
    C("SetPlayerData", IDirectPlayX_SetPlayerData(dp, pid, buf, 4, DPSET_LOCAL));
    C("GetPlayerData", (sz = sizeof(buf), IDirectPlayX_GetPlayerData(dp, pid, buf, &sz, DPGET_LOCAL)));
    C("SetGroupData", IDirectPlayX_SetGroupData(dp, gid, buf, 4, DPSET_LOCAL));
    C("GetGroupData", (sz = sizeof(buf), IDirectPlayX_GetGroupData(dp, gid, buf, &sz, DPGET_LOCAL)));
    C("GetPlayerAddress", (sz = sizeof(buf), IDirectPlayX_GetPlayerAddress(dp, pid, buf, &sz)));
    C("GetPlayerAddress(0)", (sz = sizeof(buf), IDirectPlayX_GetPlayerAddress(dp, 0, buf, &sz)));
    printf("info GetPlayerAddress(0) wrote size=%lu first bytes %02x%02x%02x%02x\n", (unsigned long)sz, buf[0]&255, buf[1]&255, buf[2]&255, buf[3]&255);
    C("GetPlayerAccount", (sz = sizeof(buf), IDirectPlayX_GetPlayerAccount(dp, pid, 0, buf, &sz)));
    C("GetPlayerFlags", IDirectPlayX_GetPlayerFlags(dp, pid, &flags));
    C("GetGroupFlags", IDirectPlayX_GetGroupFlags(dp, gid, &flags));
    C("GetGroupParent", IDirectPlayX_GetGroupParent(dp, gid, &newgid));
    C("GetGroupOwner", IDirectPlayX_GetGroupOwner(dp, gid, &from));
    C("SetGroupOwner", IDirectPlayX_SetGroupOwner(dp, gid, pid));
    C("GetPlayerCaps", IDirectPlayX_GetPlayerCaps(dp, pid, &caps, 0));
    C("AddPlayerToGroup", IDirectPlayX_AddPlayerToGroup(dp, gid, pid));
    C("DeletePlayerFromGroup", IDirectPlayX_DeletePlayerFromGroup(dp, gid, pid));
    C("CreateGroupInGroup", IDirectPlayX_CreateGroupInGroup(dp, gid, &newgid, &pn, NULL, 0, 0));
    if (*tag) dp1_name_id(newgid, "H2");   /* ids by name in the messages apistate_dp1.c prints */
    C("AddGroupToGroup", IDirectPlayX_AddGroupToGroup(dp, gid, newgid));
    C("DeleteGroupFromGroup", IDirectPlayX_DeleteGroupFromGroup(dp, gid, newgid));
    C("GetGroupConnSettings", (sz = sizeof(buf), IDirectPlayX_GetGroupConnectionSettings(dp, 0, gid, buf, &sz)));
    C("SetGroupConnSettings", IDirectPlayX_SetGroupConnectionSettings(dp, 0, gid, &conn));
    C("StartSession", IDirectPlayX_StartSession(dp, 0, gid));
    C("Send", IDirectPlayX_Send(dp, pid, DPID_ALLPLAYERS, 0, buf, 8));
    C("Send(to self)", IDirectPlayX_Send(dp, pid, pid, 0, buf, 8));
    C("SendEx", IDirectPlayX_SendEx(dp, pid, pid, DPSEND_ASYNC, buf, 8, 0, 0, NULL, &msgid));
    C("GetMessageQueue", (n4 = 0, IDirectPlayX_GetMessageQueue(dp, pid, pid, DPMESSAGEQUEUE_SEND, &n4, NULL)));
    C("CancelMessage", IDirectPlayX_CancelMessage(dp, msgid, 0));
    C("CancelPriority", IDirectPlayX_CancelPriority(dp, 0, 0, 0));
    C("SendChatMessage", IDirectPlayX_SendChatMessage(dp, pid, DPID_ALLPLAYERS, 0, &chat));
    Sleep(50);   /* a message to ourselves arrives on the receive queue in the meantime */
    C("Receive", (sz = sizeof(buf), IDirectPlayX_Receive(dp, &from, &to, DPRECEIVE_ALL, buf, &sz)));
    C("Receive(peek)", (sz = sizeof(buf), IDirectPlayX_Receive(dp, &from, &to, DPRECEIVE_ALL | DPRECEIVE_PEEK, buf, &sz)));
    C("GetMessageCount", (n4 = 0, IDirectPlayX_GetMessageCount(dp, pid, &n4)));
    C("GetCaps", IDirectPlayX_GetCaps(dp, &caps, 0));
    C("GetCaps(guaranteed)", IDirectPlayX_GetCaps(dp, &caps, DPGETCAPS_GUARANTEED));
    /* asynchronous: a synchronous EnumSessions over Windows' TCP/IP provider never returns when
       the provider has been initialized but no session was ever opened (Windows' dplayx and ours alike) */
    C("EnumConnections", (n = 0, IDirectPlayX_EnumConnections(dp, &APISTATE_APP, enum_connections_cb, &n, 0)));
    C("EnumSessions(async)", (n = 0, IDirectPlayX_EnumSessions(dp, &sd, 100, enum_sessions_cb, &n, DPENUMSESSIONS_AVAILABLE | DPENUMSESSIONS_ASYNC)));
    Sleep(300);
    C("EnumSessions(stop async)", IDirectPlayX_EnumSessions(dp, &sd, 0, enum_sessions_cb, &n, DPENUMSESSIONS_STOPASYNC));
    Sleep(300);   /* Windows' dplayx can hang in the next call while its enumeration thread is still winding down */
    C("Initialize", IDirectPlayX_Initialize(dp, (GUID *)&DPSPGUID_TCPIP));
    {
        void *addr = connection_for(sp, &sz);
        C("InitializeConnection", IDirectPlayX_InitializeConnection(dp, addr, 0));
        free(addr);
    }
    C("SecureOpen(bad flags)", IDirectPlayX_SecureOpen(dp, &sd, 0x80000000, &sec, NULL));
    C("Open(bad flags)", IDirectPlayX_Open(dp, &sd, 0x80000000));
    C("DestroyGroup", IDirectPlayX_DestroyGroup(dp, gid));
    C("DestroyPlayer", IDirectPlayX_DestroyPlayer(dp, pid));
#undef C
}

static void qi_calls(void)
{
    static const struct { const IID *iid; const char *name; } ifs[] = {
        { &IID_IUnknown, "IUnknown" }, { &IID_IDirectPlay, "IDirectPlay" }, { &IID_IDirectPlay2, "IDirectPlay2" },
        { &IID_IDirectPlay2A, "IDirectPlay2A" }, { &IID_IDirectPlay3, "IDirectPlay3" }, { &IID_IDirectPlay3A, "IDirectPlay3A" },
        { &IID_IDirectPlay4, "IDirectPlay4" }, { &IID_IDirectPlay4A, "IDirectPlay4A" },
        { &IID_IDirectPlayLobby, "IDirectPlayLobby (on a DirectPlay object)" },
    };
    char label[64];
    for (size_t i = 0; i < sizeof(ifs) / sizeof(ifs[0]); i++) {
        IUnknown *u = NULL;
        HRESULT hr = IDirectPlayX_QueryInterface(dp, ifs[i].iid, (void **)&u);
        snprintf(label, sizeof(label), "QI %s", ifs[i].name);
        CALL(label, hr);
        if (u) u->lpVtbl->Release(u);
    }
}

static void calls(const char *state, const char *sp)
{
    DPID id = 0, gid = 0;
    DPNAME pn;
    memset(&pn, 0, sizeof(pn));
    pn.dwSize = sizeof(pn);
    pn.lpszShortNameA = (char *)"x";

    qi_calls();
    method_calls("", 0x1234, 0x5e7, sp);   /* unknown ids everywhere */
    CALL("CreatePlayer", IDirectPlayX_CreatePlayer(dp, &id, &pn, NULL, NULL, 0, 0));
    CALL("CreateGroup", IDirectPlayX_CreateGroup(dp, &gid, &pn, NULL, 0, 0));
    dp1_name_id(id, "Q1");
    dp1_name_id(gid, "H1");
    if (!strcmp(state, "open") && id && gid)
        method_calls("real:", id, gid, sp);   /* the same methods with a player and group that exist */
    dp1_calls(state);
    CALL("Close", IDirectPlayX_Close(dp));
    CALL("Close again", IDirectPlayX_Close(dp));
}

static int run_state(const char *state, const char *dll, const char *sp)
{
    HRESULT hr;
    CoInitialize(NULL);
    if (!strcmp(state, "dp1create")) { int r = dp1_create_direct(dll); printf("done\n"); return r; }
    if (FAILED(hr = create(dll))) { printf("create failed 0x%08lx\n", (unsigned long)hr); return 2; }
    if (strcmp(state, "fresh") != 0) {
        hr = init_connection(sp);
        printf("setup InitializeConnection 0x%08lx\n", (unsigned long)hr);
    }
    if (!strcmp(state, "failedjoin")) {
        DPSESSIONDESC2 sd;
        sd_init(&sd, (char *)"nobody-hosts-this");
        sd.guidInstance = APISTATE_APP;
        hr = IDirectPlayX_Open(dp, &sd, DPOPEN_JOIN);
        printf("setup Open(join) 0x%08lx\n", (unsigned long)hr);
    } else if (!strcmp(state, "dp1host")) {
        dp1_open_create();
    } else if (!strcmp(state, "closed") || !strcmp(state, "open")) {
        DPSESSIONDESC2 sd;
        char name[32] = "apistate host";
        sd_init(&sd, name);
        hr = IDirectPlayX_Open(dp, &sd, DPOPEN_CREATE);
        printf("setup Open(create) 0x%08lx\n", (unsigned long)hr);
        if (!strcmp(state, "closed")) printf("setup Close 0x%08lx\n", (unsigned long)IDirectPlayX_Close(dp));
    }
    calls(state, sp);
    printf("call %-26s 0x%08lx\n", "Release", (unsigned long)IDirectPlayX_Release(dp));
    printf("done\n");
    return 0;
}

int main(int argc, char **argv)
{
    const char *dll = getenv("DPTEST_DPLAYX"), *sp = "ipx", *state = NULL;
    /* no password box from the drop-in when a state hosts (child processes inherit this) */
    if (!getenv("OKP_TEST_HOST_PASSWORD")) _putenv("OKP_TEST_HOST_PASSWORD=<none>");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dplayx") && i + 1 < argc) dll = argv[++i];
        else if (!strcmp(argv[i], "--sp") && i + 1 < argc) sp = argv[++i];
        else state = argv[i];
    }
    if (!state) { fprintf(stderr, "usage: apistate [--dplayx DLL] [--sp ipx|auto|tcpip] fresh|init|failedjoin|closed|open|dp1host|dp1create|all\n"); return 2; }
    if (strcmp(sp, "tcpip") != 0) SetEnvironmentVariableA("OKP_TEST_AS", "lan");
    SetEnvironmentVariableA("OKP_STUN", "");
    if (strcmp(state, "all") != 0) return run_state(state, dll, sp);

    static const char *states[] = { "fresh", "init", "failedjoin", "closed", "open", "dp1host", "dp1create" };
    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, sizeof(self));
    int crashes = 0;
    for (int s = 0; s < (int)(sizeof(states) / sizeof(states[0])); s++) {
        char cmd[MAX_PATH * 3];
        snprintf(cmd, sizeof(cmd), "\"%s\" %s%s%s --sp %s %s", self, dll && *dll ? "--dplayx \"" : "",
                 dll && *dll ? dll : "", dll && *dll ? "\"" : "", sp, states[s]);
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        printf("state %s\n", states[s]);
        fflush(stdout);
        if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) { printf("cannot start\n"); return 2; }
        if (WaitForSingleObject(pi.hProcess, 90000) == WAIT_TIMEOUT) {
            TerminateProcess(pi.hProcess, 0xdead);
            printf("HANG in state %s\n", states[s]);
            crashes++;
        }
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        if (code != 0 && code != 0xdead) { printf("CRASH in state %s (exit 0x%08lx)\n", states[s], (unsigned long)code); crashes++; }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    printf("RESULT %s crashes=%d\n", crashes ? "fail" : "ok", crashes);
    return crashes ? 1 : 0;
}
