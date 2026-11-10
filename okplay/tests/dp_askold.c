/* dp_askold - classic_*=ask answered "Windows' DirectPlay" while the game talks through an OLD
 * interface. The question is put at the game's first EnumSessions/Open; the switcher then hands the
 * game's address to Windows' object with InitializeConnection - an IDirectPlay3/4 method. Asked
 * through IDirectPlay2A (vtable of 30 methods) it once called past the end of Windows' interface
 * (shim/switch.c sw_resolve_pending). Fixed: it goes through Windows' IDirectPlay3A.
 *
 *   dp_askold dplayx.dll     with okplay.ini classic_tcpip=ask next to the DLL and
 *                            OKP_TEST_CLASSIC_ASK=windows answering the box (dp_askold.sh)
 * Prints each result and "RESULT ok|fail"; the caller also reads the DLL's log for
 * "this game uses Windows' DirectPlay". Uses Windows' TCP/IP provider (Open with no session:
 * nothing is hosted).
 */
#define INITGUID
#include <windows.h>
#include <dplay.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    typedef HRESULT (WINAPI *create_t)(LPGUID, LPDIRECTPLAY *, IUnknown *);
    HMODULE m;
    create_t create;
    IDirectPlay *dp = NULL;
    IDirectPlay2A *dp2 = NULL;
    DPSESSIONDESC2 sd;
    HRESULT hr;
    if (argc < 2) { printf("usage: dp_askold dplayx.dll\n"); return 2; }
    if (!(m = LoadLibraryA(argv[1])) || !(create = (create_t)GetProcAddress(m, "DirectPlayCreate"))) {
        printf("cannot load %s\n", argv[1]); return 2;
    }
    CoInitialize(NULL);
    hr = create((GUID *)&DPSPGUID_TCPIP, &dp, NULL);
    printf("DirectPlayCreate(TCP/IP) 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) { printf("RESULT fail\n"); return 1; }
    hr = IDirectPlay_QueryInterface(dp, &IID_IDirectPlay2A, (void **)&dp2);
    printf("QueryInterface(IDirectPlay2A) 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) { printf("RESULT fail\n"); return 1; }
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    /* the question is answered here, through IDirectPlay2A; no session exists, so the join fails */
    hr = IDirectPlayX_Open(dp2, &sd, DPOPEN_JOIN);
    printf("IDirectPlay2A::Open(join, no session) 0x%08lx\n", (unsigned long)hr);
    IDirectPlayX_Release(dp2);
    printf("released, refs left %lu\n", (unsigned long)IDirectPlay_Release(dp));
    printf("RESULT ok\n");
    return 0;
}
