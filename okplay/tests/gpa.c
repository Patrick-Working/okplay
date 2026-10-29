/* gpa - what does GetPlayerAddress answer, on Windows' dplayx and on ours? (Midtown Madness calls
 * GetPlayerAddress(0, NULL, &size) right after picking a connection.)
 *   gpa [--dplayx DLL] [--sp tcpip|auto]   -> lines "state call hr size bytes..." */
#define INITGUID
#include <windows.h>
#include <dplay.h>
#include <dplobby.h>
#include <stdio.h>
#include <string.h>
DEFINE_GUID(OKP_LANGUID, 0x6bcff704, 0xce20, 0x4150, 0x98, 0xb4, 0x5a, 0x71, 0x98, 0x17, 0x5d, 0x7d);
DEFINE_GUID(GPA_APP, 0x7a1e0c55, 0x2b8d, 0x4f11, 0x9c, 0x3e, 0x41, 0x6a, 0x2d, 0x8b, 0x77, 0x11);
static IDirectPlay4A *dp; static IDirectPlayLobby3A *lob;
static HRESULT create(const char *dll) {
    HRESULT hr; IClassFactory *cf;
    if (!dll || !*dll) {
        if (FAILED(hr = CoCreateInstance(&CLSID_DirectPlay, NULL, CLSCTX_INPROC_SERVER, &IID_IDirectPlay4A, (void **)&dp))) return hr;
        return CoCreateInstance(&CLSID_DirectPlayLobby, NULL, CLSCTX_INPROC_SERVER, &IID_IDirectPlayLobby3A, (void **)&lob);
    }
    HMODULE m = LoadLibraryA(dll); typedef HRESULT (WINAPI *gco_t)(REFCLSID, REFIID, void **);
    gco_t gco = m ? (gco_t)GetProcAddress(m, "DllGetClassObject") : NULL; if (!gco) return E_FAIL;
    if (FAILED(hr = gco(&CLSID_DirectPlay, &IID_IClassFactory, (void **)&cf))) return hr;
    hr = cf->lpVtbl->CreateInstance(cf, NULL, &IID_IDirectPlay4A, (void **)&dp); cf->lpVtbl->Release(cf); if (FAILED(hr)) return hr;
    if (FAILED(hr = gco(&CLSID_DirectPlayLobby, &IID_IClassFactory, (void **)&cf))) return hr;
    hr = cf->lpVtbl->CreateInstance(cf, NULL, &IID_IDirectPlayLobby3A, (void **)&lob); cf->lpVtbl->Release(cf); return hr;
}
static void caps(const char *state, DPID id) {
    DPCAPS c; HRESULT hr;
    for (int g = 0; g < 2; g++) {
        memset(&c, 0, sizeof(c)); c.dwSize = sizeof(c);
        hr = id ? IDirectPlayX_GetPlayerCaps(dp, id, &c, g ? DPGETCAPS_GUARANTEED : 0) : IDirectPlayX_GetCaps(dp, &c, g ? DPGETCAPS_GUARANTEED : 0);
        printf("%-6s %s(%s) hr=0x%08lx flags=0x%lx maxbuf=%lu maxqueue=%lu maxplayers=%lu hundredbaud=%lu latency=%lu maxlocal=%lu headerlen=%lu timeout=%lu\n",
               state, id ? "GetPlayerCaps" : "GetCaps", g ? "guaranteed" : "0", (unsigned long)hr, (unsigned long)c.dwFlags, (unsigned long)c.dwMaxBufferSize,
               (unsigned long)c.dwMaxQueueSize, (unsigned long)c.dwMaxPlayers, (unsigned long)c.dwHundredBaud, (unsigned long)c.dwLatency,
               (unsigned long)c.dwMaxLocalPlayers, (unsigned long)c.dwHeaderLength, (unsigned long)c.dwTimeout);
    }
}
static void probe(const char *state, DPID id) {
    BYTE buf[512]; DWORD sz = 0; HRESULT hr = IDirectPlayX_GetPlayerAddress(dp, id, NULL, &sz);
    printf("%-6s GetPlayerAddress(%lu, NULL, &size)   hr=0x%08lx size=%lu\n", state, (unsigned long)id, (unsigned long)hr, (unsigned long)sz);
    DWORD sz2 = sizeof(buf); memset(buf, 0, sizeof(buf)); hr = IDirectPlayX_GetPlayerAddress(dp, id, buf, &sz2);
    printf("%-6s GetPlayerAddress(%lu, buf, 512)      hr=0x%08lx size=%lu bytes:", state, (unsigned long)id, (unsigned long)hr, (unsigned long)sz2);
    for (DWORD i = 0; i < sz2 && i < 96; i++) printf("%02x", buf[i]); printf("\n");
    if (SUCCEEDED(hr) && sz2 > 24) { printf("       as text: "); for (DWORD i = 0; i < sz2; i++) putchar(buf[i] >= 32 && buf[i] < 127 ? buf[i] : '.'); printf("\n"); }
}
int main(int argc, char **argv) {
    const char *dll = NULL, *sp = "tcpip";
    for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "--dplayx") && i + 1 < argc) dll = argv[++i]; else if (!strcmp(argv[i], "--sp") && i + 1 < argc) sp = argv[++i]; }
    if (!getenv("OKP_TEST_HOST_PASSWORD")) _putenv("OKP_TEST_HOST_PASSWORD=<none>");
    if (strcmp(sp, "tcpip")) SetEnvironmentVariableA("OKP_TEST_AS", "lan");
    CoInitialize(NULL);
    /* --import: also import dplayx.dll (as games do), so a drop-in next to this exe is loaded and can
       redirect CoCreateInstance; create() then uses CoCreateInstance */
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--import")) { DirectPlayEnumerateA(NULL, NULL); dll = NULL; }
    HRESULT hr = create(dll); if (FAILED(hr)) { printf("create failed 0x%08lx\n", (unsigned long)hr); return 2; }
    DPCOMPOUNDADDRESSELEMENT el[2]; int nel = 1; el[0].guidDataType = DPAID_ServiceProvider; el[0].dwDataSize = sizeof(GUID);
    el[0].lpData = (void *)(!strcmp(sp, "tcpip") ? &DPSPGUID_TCPIP : &OKP_LANGUID);
    if (!strcmp(sp, "tcpip")) { el[1].guidDataType = DPAID_INet; el[1].dwDataSize = 1; el[1].lpData = (void *)""; nel = 2; }   /* no "Locate Session" dialog */
    DWORD asz = 0; IDirectPlayLobby_CreateCompoundAddress(lob, el, nel, NULL, &asz); void *addr = malloc(asz);
    IDirectPlayLobby_CreateCompoundAddress(lob, el, nel, addr, &asz);
    printf("InitializeConnection hr=0x%08lx\n", (unsigned long)IDirectPlayX_InitializeConnection(dp, addr, 0));
    probe("init", 0);
    DPSESSIONDESC2 sd; memset(&sd, 0, sizeof(sd)); sd.dwSize = sizeof(sd); sd.dwFlags = DPSESSION_KEEPALIVE | DPSESSION_MIGRATEHOST;
    sd.guidApplication = GPA_APP; sd.dwMaxPlayers = 8; sd.lpszSessionNameA = (char *)"gpa";
    printf("Open(create) hr=0x%08lx\n", (unsigned long)IDirectPlayX_Open(dp, &sd, DPOPEN_CREATE));
    probe("open", 0);
    DPNAME pn; memset(&pn, 0, sizeof(pn)); pn.dwSize = sizeof(pn); pn.lpszShortNameA = (char *)"me"; DPID me = 0;
    printf("CreatePlayer hr=0x%08lx id=%lu\n", (unsigned long)IDirectPlayX_CreatePlayer(dp, &me, &pn, NULL, NULL, 0, 0), (unsigned long)me);
    probe("player", 0); probe("player", me);
    caps("init*", 0); caps("player", me);
    { DWORD f = 0; HRESULT h = IDirectPlayX_GetPlayerFlags(dp, me, &f); printf("player GetPlayerFlags hr=0x%08lx flags=0x%lx\n", (unsigned long)h, (unsigned long)f); }
    IDirectPlayX_Close(dp); IDirectPlayX_Release(dp); return 0;
}
