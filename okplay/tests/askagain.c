/* askagain - classic_*=ask must ask again after the game lets go of its DirectPlay object.
 *
 * A game with a fixed connection list creates a DirectPlay object for its "TCP/IP" entry, uses it,
 * and releases it when the player backs out to the connection menu. Picking the entry again must
 * put the question again (the first answer belonged to the object that is gone).
 * This does that twice in one process, the way such a game does: DirectPlayCreate(TCP/IP GUID),
 * EnumSessions (where the deferred question is answered), Release.
 *
 *   askagain dplayx.dll     with okplay.ini classic_tcpip=ask next to the DLL and
 *                           OKP_TEST_CLASSIC_ASK=lan answering the box
 * The caller counts "the player chose" lines in the DLL's log: 2 = asked again, 1 = not.
 */
#define INITGUID
#include <windows.h>
#include <dplay.h>
#include <stdio.h>

static BOOL FAR PASCAL cb(LPCDPSESSIONDESC2 sd, LPDWORD timeout, DWORD flags, LPVOID ctx)
{
    (void)sd; (void)timeout; (void)ctx;
    return (flags & DPESC_TIMEDOUT) ? FALSE : TRUE;
}

int main(int argc, char **argv)
{
    typedef HRESULT (WINAPI *create_t)(LPGUID, LPDIRECTPLAY *, IUnknown *);
    HMODULE m;
    create_t create;
    int round;
    if (argc < 2) { printf("usage: askagain dplayx.dll\n"); return 2; }
    if (!(m = LoadLibraryA(argv[1])) || !(create = (create_t)GetProcAddress(m, "DirectPlayCreate"))) {
        printf("cannot load %s\n", argv[1]); return 2;
    }
    CoInitialize(NULL);
    for (round = 1; round <= 2; round++) {
        IDirectPlay *dp = NULL;
        IDirectPlay4A *dp4 = NULL;
        DPSESSIONDESC2 sd;
        HRESULT hr = create((GUID *)&DPSPGUID_TCPIP, &dp, NULL);
        printf("round %d: DirectPlayCreate(TCP/IP) 0x%08lx\n", round, (unsigned long)hr);
        if (FAILED(hr)) return 1;
        hr = IDirectPlay_QueryInterface(dp, &IID_IDirectPlay4A, (void **)&dp4);
        if (FAILED(hr)) { printf("no IDirectPlay4A 0x%08lx\n", (unsigned long)hr); return 1; }
        memset(&sd, 0, sizeof(sd));
        sd.dwSize = sizeof(sd);
        hr = IDirectPlayX_EnumSessions(dp4, &sd, 0, cb, NULL, DPENUMSESSIONS_AVAILABLE | DPENUMSESSIONS_ASYNC);
        printf("round %d: EnumSessions 0x%08lx\n", round, (unsigned long)hr);
        Sleep(300);
        IDirectPlayX_EnumSessions(dp4, &sd, 0, cb, NULL, DPENUMSESSIONS_STOPASYNC);
        IDirectPlayX_Release(dp4);
        printf("round %d: released, refs left %lu\n", round, (unsigned long)IDirectPlay_Release(dp));
        Sleep(300);
    }
    printf("done\n");
    return 0;
}
