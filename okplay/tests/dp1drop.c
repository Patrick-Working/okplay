/* dp1drop: does a DirectPlay 1 (IDirectPlay) host drop a player whose machine vanishes? Black box,
 * on Windows' dplayx (default) or a given one, over Windows' TCP/IP provider on this PC:
 *   dp1drop host [dplayx.dll]   hosts through IDirectPlay::Open, prints every message it receives
 *                               with the seconds since the joiner's player arrived, for 90 s
 *   dp1drop join [dplayx.dll]   joins the session on 127.0.0.1 and waits - kill it to "vanish"
 *   dp1drop host|join path\dplay.dll   the same on a DirectPlay 1.1 dplay.dll (ordinals 1 and 2), over
 *                               its IPX provider (broadcast; on Windows 10 through ipxwrapper)
 * Run both, then kill the joiner (docs/GAME_NECRODOME.md). A DirectPlay 1 session carries no
 * DPSESSION_KEEPALIVE flag (Open sets its own flags), so the question is what Windows does then. */
#include <windows.h>
#include <dplay.h>
#include <dplobby.h>
#include <stdio.h>
#include <string.h>

static const GUID APP = { 0x0d1d5001, 0x1234, 0x4321, { 1, 2, 3, 4, 5, 6, 7, 8 } };
static IDirectPlay4A *dp4;
static IDirectPlayLobby3A *lob;

static HRESULT create(const char *dll)
{
    typedef HRESULT (WINAPI *gco_t)(REFCLSID, REFIID, void **);
    HMODULE m;
    gco_t gco;
    IClassFactory *cf;
    HRESULT hr;
    if (!dll) {
        if (FAILED(hr = CoCreateInstance(&CLSID_DirectPlay, NULL, CLSCTX_INPROC_SERVER, &IID_IDirectPlay4A, (void **)&dp4)))
            return hr;
        return CoCreateInstance(&CLSID_DirectPlayLobby, NULL, CLSCTX_INPROC_SERVER, &IID_IDirectPlayLobby3A, (void **)&lob);
    }
    m = LoadLibraryA(dll);
    gco = m ? (gco_t)GetProcAddress(m, "DllGetClassObject") : NULL;
    if (!gco) return E_FAIL;
    if (FAILED(hr = gco(&CLSID_DirectPlay, &IID_IClassFactory, (void **)&cf))) return hr;
    hr = cf->lpVtbl->CreateInstance(cf, NULL, &IID_IDirectPlay4A, (void **)&dp4);
    cf->lpVtbl->Release(cf);
    if (FAILED(hr) || FAILED(hr = gco(&CLSID_DirectPlayLobby, &IID_IClassFactory, (void **)&cf))) return hr;
    hr = cf->lpVtbl->CreateInstance(cf, NULL, &IID_IDirectPlayLobby3A, (void **)&lob);
    cf->lpVtbl->Release(cf);
    return hr;
}

static HRESULT connect_tcpip(const char *host)
{
    DPCOMPOUNDADDRESSELEMENT el[2];
    DWORD size = 0;
    void *addr;
    HRESULT hr;
    el[0].guidDataType = DPAID_ServiceProvider;
    el[0].dwDataSize = sizeof(GUID);
    el[0].lpData = (void *)&DPSPGUID_TCPIP;
    el[1].guidDataType = DPAID_INet;
    el[1].dwDataSize = (DWORD)strlen(host) + 1;
    el[1].lpData = (void *)host;
    IDirectPlayLobby_CreateCompoundAddress(lob, el, 2, NULL, &size);
    addr = malloc(size);
    IDirectPlayLobby_CreateCompoundAddress(lob, el, 2, addr, &size);
    hr = IDirectPlayX_InitializeConnection(dp4, addr, 0);
    free(addr);
    return hr;
}

typedef HRESULT (WINAPI *create1_t)(LPGUID, LPDIRECTPLAY *, IUnknown *);
typedef HRESULT (WINAPI *enum1_t)(LPDPENUMDPCALLBACKA, LPVOID);
static GUID ipx;
static BOOL have_ipx;
static BOOL FAR PASCAL on_sp(LPGUID guid, LPSTR name, DWORD major, DWORD minor, LPVOID ctx)
{
    if (strstr(name, "IPX") && !have_ipx) { ipx = *guid; have_ipx = TRUE; printf("provider %s\n", name); }
    return TRUE;
}

/* DirectPlay 1.1: DirectPlayCreate(IPX provider) from the dplay.dll itself */
static HRESULT create11(const char *dll, IDirectPlay **dp)
{
    HMODULE m = LoadLibraryA(dll);
    create1_t create1 = m ? (create1_t)GetProcAddress(m, MAKEINTRESOURCEA(1)) : NULL;
    enum1_t enum1 = m ? (enum1_t)GetProcAddress(m, MAKEINTRESOURCEA(2)) : NULL;
    if (!create1 || !enum1) return E_FAIL;
    enum1(on_sp, NULL);
    if (!have_ipx) return DPERR_UNAVAILABLE;
    return create1(&ipx, dp, NULL);
}

static DWORD found;
static BOOL FAR PASCAL on_session(LPDPSESSIONDESC sd, LPVOID ctx, LPDWORD timeout, DWORD flags)
{
    if (flags & DPESC_TIMEDOUT) return FALSE;
    if (!found) { found = sd->dwSession; printf("session %lu \"%s\"\n", (unsigned long)sd->dwSession, sd->szSessionName); }
    return FALSE;
}

int main(int argc, char **argv)
{
    BOOL host = argc > 1 && !strncmp(argv[1], "host", 4);
    BOOL enp = argc > 1 && !strcmp(argv[1], "hostenp");
    int enp_step = 0;
    BOOL j4 = argc > 1 && !strcmp(argv[1], "join4");
    IDirectPlay *dp;
    DPSESSIONDESC sd;
    DPID me = 0;
    HANDLE ev = NULL;
    HRESULT hr;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage: dp1drop host|hostenp|join|join4 [dplayx.dll | dplay.dll]\n"); return 2; }
    CoInitialize(NULL);
    if (argc > 2 && strlen(argv[2]) >= 9 && !lstrcmpiA(argv[2] + strlen(argv[2]) - 9, "dplay.dll")) {
        if (FAILED(hr = create11(argv[2], &dp))) { printf("DirectPlay 1.1 create 0x%08lx\n", (unsigned long)hr); return 2; }
    } else {
        if (FAILED(hr = create(argc > 2 ? argv[2] : NULL))) { printf("create 0x%08lx\n", (unsigned long)hr); return 2; }
        if (FAILED(hr = connect_tcpip(host ? "" : "127.0.0.1"))) { printf("InitializeConnection 0x%08lx\n", (unsigned long)hr); return 2; }
        if (FAILED(hr = IDirectPlayX_QueryInterface(dp4, &IID_IDirectPlay, (void **)&dp))) { printf("QI 0x%08lx\n", (unsigned long)hr); return 2; }
    }
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.guidSession = APP;
    sd.dwMaxPlayers = 4;
    if (host) {
        char buf[1024];
        DWORD sz = sizeof(buf);
        DWORD t0 = 0, start = GetTickCount();
        sd.dwFlags = DPOPEN_CREATE;
        strcpy(sd.szSessionName, "dp1drop");
        printf("Open(create) 0x%08lx\n", (unsigned long)IDirectPlay_Open(dp, &sd));
        if (dp4 && SUCCEEDED(IDirectPlayX_GetSessionDesc(dp4, buf, &sz)))
            printf("session flags 0x%08lx (DP2 view)\n", (unsigned long)((DPSESSIONDESC2 *)buf)->dwFlags);
        printf("CreatePlayer 0x%08lx\n", (unsigned long)IDirectPlay_CreatePlayer(dp, &me, (LPSTR)"host", (LPSTR)"Host Player", &ev));
        while (GetTickCount() - start < 180000 && (!t0 || GetTickCount() - t0 < 90000)) {
            DPID from = 0, to = 0;
            DWORD size = sizeof(buf);
            /* hostenp: EnableNewPlayers(TRUE) four times while already TRUE (no change), then FALSE and
             * TRUE (two changes), one a second from 2 s after the joiner's player arrived */
            if (enp && t0 && enp_step < 6 && GetTickCount() - t0 >= 2000 + 1000u * enp_step) {
                BOOL on = enp_step != 4;
                printf("t=%5.1fs EnableNewPlayers(%s) 0x%08lx\n", (GetTickCount() - t0) / 1000.0, on ? "TRUE" : "FALSE",
                       (unsigned long)IDirectPlay_EnableNewPlayers(dp, on));
                enp_step++;
            }
            hr = IDirectPlay_Receive(dp, &from, &to, DPRECEIVE_ALL, buf, &size);
            if (hr == DPERR_NOMESSAGES) { Sleep(50); continue; }
            if (FAILED(hr)) { printf("Receive 0x%08lx\n", (unsigned long)hr); break; }
            if (from == 0) {
                DWORD type = *(DWORD *)buf;
                if (type == 3 && !t0) t0 = GetTickCount();   /* DPSYS_ADDPLAYER: the joiner's player */
                printf("t=%5.1fs system message type 0x%lx (%lu bytes)\n", t0 ? (GetTickCount() - t0) / 1000.0 : 0.0,
                       (unsigned long)type, (unsigned long)size);
            }
        }
        printf("done\n");
    } else {
        found = 0;
        hr = IDirectPlay_EnumSessions(dp, &sd, 3000, on_session, NULL, 0);
        if (!found) { printf("EnumSessions 0x%08lx, nothing found\n", (unsigned long)hr); return 2; }
        sd.dwSession = found;
        sd.dwFlags = DPOPEN_JOIN;   /* DirectX 3: DPOPEN_OPENSESSION */
        printf("Open(join) 0x%08lx\n", (unsigned long)IDirectPlay_Open(dp, &sd));
        printf("CreatePlayer 0x%08lx - receiving until killed\n",
               (unsigned long)IDirectPlay_CreatePlayer(dp, &me, (LPSTR)"joiner", (LPSTR)"Joining Player", &ev));
        {
            DWORD start = GetTickCount();
            char buf[1024];
            for (;;) {
                DPID from = 0, to = 0;
                DWORD size = sizeof(buf);
                /* join4: the same object read through IDirectPlay4 - what the wire brought, before the
                 * DirectPlay 1 interface picks what a DirectX 3 game gets */
                hr = j4 ? IDirectPlayX_Receive(dp4, &from, &to, DPRECEIVE_ALL, buf, &size)
                        : IDirectPlay_Receive(dp, &from, &to, DPRECEIVE_ALL, buf, &size);
                if (hr == DPERR_NOMESSAGES) { Sleep(50); continue; }
                if (FAILED(hr)) { printf("Receive 0x%08lx\n", (unsigned long)hr); Sleep(1000); continue; }
                if (from == 0)
                    printf("t=%5.1fs system message type 0x%lx (%lu bytes)\n", (GetTickCount() - start) / 1000.0,
                           (unsigned long)*(DWORD *)buf, (unsigned long)size);
            }
        }
    }
    return 0;
}
