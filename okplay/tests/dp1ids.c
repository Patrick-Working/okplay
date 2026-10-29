/* dp1ids: which player ids does a DPLAY.DLL hand a DirectX 3 game? Black box only: loads the DLL,
 * calls DirectPlayEnumerate (ordinal 2) and DirectPlayCreate (ordinal 1) as Necrodome imports them,
 * hosts a session on each provider through IDirectPlay::Open and prints the ids CreatePlayer returns.
 *   dp1ids.exe [path\to\dplay.dll]      (default: dplay.dll from the usual search path)
 * Necrodome keeps per-player sequence numbers at 0x5a0a20 + id * 8, a table of 200 (GAME_NECRODOME.md). */
#include <windows.h>
#include <dplay.h>
#include <stdio.h>

typedef HRESULT (WINAPI *create_t)(LPGUID, LPDIRECTPLAY *, IUnknown *);
typedef HRESULT (WINAPI *enum_t)(LPDPENUMDPCALLBACKA, LPVOID);

static GUID guids[16];
static char names[16][80];
static int n;

static BOOL FAR PASCAL on_sp(LPGUID guid, LPSTR name, DWORD major, DWORD minor, LPVOID ctx)
{
    printf("provider %s %lu.%lu {%08lx-...}\n", name, (unsigned long)major, (unsigned long)minor,
           (unsigned long)guid->Data1);
    if (n < 16) { guids[n] = *guid; lstrcpynA(names[n], name, sizeof(names[0])); n++; }
    return TRUE;
}

int main(int argc, char **argv)
{
    HMODULE m = LoadLibraryA(argc > 1 ? argv[1] : "dplay.dll");
    create_t create = m ? (create_t)GetProcAddress(m, MAKEINTRESOURCEA(1)) : NULL;
    enum_t enumerate = m ? (enum_t)GetProcAddress(m, MAKEINTRESOURCEA(2)) : NULL;
    int i;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!create || !enumerate) { printf("no ordinal 1/2 in %s\n", argc > 1 ? argv[1] : "dplay.dll"); return 2; }
    printf("DirectPlayEnumerate 0x%08lx\n", (unsigned long)enumerate(on_sp, NULL));
    for (i = 0; i < n; i++) {
        IDirectPlay *dp = NULL;
        DPSESSIONDESC sd;
        DPID p1 = 0, p2 = 0;
        HANDLE e1 = NULL, e2 = NULL;
        HRESULT hr;
        if (argc > 2 && !strstr(names[i], argv[2])) continue;
        hr = create(&guids[i], &dp, NULL);
        printf("[%s] DirectPlayCreate 0x%08lx\n", names[i], (unsigned long)hr);
        if (FAILED(hr) || !dp) continue;
        memset(&sd, 0, sizeof(sd));
        sd.dwSize = sizeof(sd);
        sd.guidSession.Data1 = 0x0d1d5000;   /* any application GUID */
        sd.dwMaxPlayers = 4;
        sd.dwFlags = DPOPEN_CREATE;   /* DirectX 3: DPOPEN_CREATESESSION, the same value */
        strcpy(sd.szSessionName, "dp1ids");
        hr = IDirectPlay_Open(dp, &sd);
        printf("[%s] Open(create) 0x%08lx\n", names[i], (unsigned long)hr);
        if (SUCCEEDED(hr)) {
            HRESULT h1 = IDirectPlay_CreatePlayer(dp, &p1, (LPSTR)"one", (LPSTR)"Player One", &e1);
            HRESULT h2 = IDirectPlay_CreatePlayer(dp, &p2, (LPSTR)"two", (LPSTR)"Player Two", &e2);
            printf("[%s] ids P1=0x%08lx (0x%08lx) P2=0x%08lx (0x%08lx)%s\n", names[i], (unsigned long)p1,
                   (unsigned long)h1, (unsigned long)p2, (unsigned long)h2,
                   SUCCEEDED(h1) && p1 < 200 && p2 < 200 ? "  - inside Necrodome's table" : "");
            IDirectPlay_Close(dp);
        }
        if (e1) CloseHandle(e1);
        if (e2) CloseHandle(e2);
        IDirectPlay_Release(dp);
    }
    return 0;
}
