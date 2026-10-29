/* dp1switch_test - a DirectX 3 game's calls on a DirectPlay 1 object that sits behind the drop-in's
 * switcher (okplay.ini trace=1 with Windows' connections listed). IDirectPlay::Open has one argument;
 * the switcher once handed it DirectPlay 2's two-argument Open, which popped 4 bytes too many, so the
 * game's stack pointer drifted and the calls after it got their results and arguments trampled
 * (CreatePlayer "returned" 0x006da073, Receive crashed - 2026-10-22). dp1switch_test.sh sets it up.
 *   dp1switch_test path\to\dplay.dll        prints each result, then "RESULT ok|fail"
 */
#include <windows.h>
#include <dplay.h>
#include <stdio.h>
#include <string.h>

typedef HRESULT (WINAPI *create_t)(LPGUID, LPDIRECTPLAY *, IUnknown *);
typedef HRESULT (WINAPI *enum_t)(LPDPENUMDPCALLBACKA, LPVOID);
static GUID lan;
static int have;

static BOOL FAR PASCAL on_sp(LPGUID g, LPSTR name, DWORD major, DWORD minor, LPVOID ctx)
{
    if (strstr(name, "LAN") && !have) { lan = *g; have = 1; }
    return TRUE;
}

int main(int argc, char **argv)
{
    HMODULE m = LoadLibraryA(argc > 1 ? argv[1] : "dplay.dll");
    create_t create = m ? (create_t)GetProcAddress(m, MAKEINTRESOURCEA(1)) : NULL;
    enum_t enumerate = m ? (enum_t)GetProcAddress(m, MAKEINTRESOURCEA(2)) : NULL;
    IDirectPlay *dp = NULL;
    DPSESSIONDESC sd;
    HRESULT hr;
    int i, bad = 0;
    volatile DWORD guard = 0x5afe5afe;   /* a local the drift would trample */

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!create || !enumerate) { printf("no DirectPlayCreate/DirectPlayEnumerate\nRESULT fail\n"); return 1; }
    enumerate(on_sp, NULL);
    if (!have) { printf("no LAN Play connection\nRESULT fail\n"); return 1; }
    hr = create(&lan, &dp, NULL);
    printf("DirectPlayCreate 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) { printf("RESULT fail\n"); return 1; }
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.guidSession.Data1 = 0x0d1d5003;
    sd.dwMaxPlayers = 4;
    sd.dwFlags = DPOPEN_CREATE;
    strcpy(sd.szSessionName, "dp1switch");
    for (i = 0; i < 2; i++) {   /* the second Open fails (already open) but must not move the stack either */
        hr = IDirectPlay_Open(dp, &sd);
        printf("Open 0x%08lx\n", (unsigned long)hr);
        if (i == 0 && FAILED(hr)) bad++;
    }
    for (i = 0; i < 3; i++) {
        DPID id = 0;
        HANDLE ev = NULL;
        hr = IDirectPlay_CreatePlayer(dp, &id, (LPSTR)"p", (LPSTR)"Player", &ev);
        printf("CreatePlayer 0x%08lx id %lu\n", (unsigned long)hr, (unsigned long)id);
        if (hr != DP_OK || !id) bad++;
        if (ev) CloseHandle(ev);
    }
    for (i = 0; i < 3; i++) {
        char buf[512];
        DPID from = 0, to = 0;
        DWORD size = sizeof(buf);
        hr = IDirectPlay_Receive(dp, &from, &to, DPRECEIVE_ALL, buf, &size);
        printf("Receive 0x%08lx %lu bytes\n", (unsigned long)hr, (unsigned long)size);
        if (hr != DP_OK && hr != DPERR_NOMESSAGES) bad++;
    }
    if (guard != 0x5afe5afe) { printf("a local was overwritten: 0x%08lx\n", (unsigned long)guard); bad++; }
    IDirectPlay_Close(dp);
    IDirectPlay_Release(dp);
    printf("RESULT %s\n", bad ? "fail" : "ok");
    return bad ? 1 : 0;
}
