/* dplay1_test - a DirectX 3 era game's view: load DPLAY.DLL (the drop-in's forwarder, from the folder
 * given or next to this exe), enumerate providers with DirectPlayEnumerate, create a DirectPlay 1
 * object for Online Play with DirectPlayCreate, and say which dplayx.dll answered.
 *   dplay1_test [folder-with-dplay.dll]      prints "RESULT ok|fail"
 */
#define INITGUID
#include <windows.h>
#include <dplay.h>
#include <stdio.h>

DEFINE_GUID(OKP_SPGUID, 0x66d8dc59, 0x28e8, 0x4d3e, 0xba, 0x93, 0x2e, 0x98, 0xd2, 0x51, 0xc0, 0xf3);
static int n_online, n_all;

static BOOL WINAPI enum_cb(LPGUID guid, LPSTR name, DWORD major, DWORD minor, LPVOID ctx)
{
    (void)major; (void)minor; (void)ctx;
    n_all++;
    if (IsEqualGUID(guid, &OKP_SPGUID)) n_online++;
    printf("provider: %s\n", name ? name : "?");
    return TRUE;
}

int main(int argc, char **argv)
{
    char path[MAX_PATH], dx[MAX_PATH] = "?";
    if (argc > 1) snprintf(path, sizeof(path), "%s/dplay.dll", argv[1]);
    else lstrcpyA(path, "dplay.dll");
    HMODULE m = LoadLibraryA(path);
    if (!m) { printf("cannot load %s\nRESULT fail\n", path); return 1; }
    HRESULT (WINAPI *en)(LPDPENUMDPCALLBACKA, LPVOID) = (void *)GetProcAddress(m, "DirectPlayEnumerate");
    HRESULT (WINAPI *cr)(LPGUID, LPDIRECTPLAY *, IUnknown *) = (void *)GetProcAddress(m, "DirectPlayCreate");
    if (!en || !cr) { printf("exports missing\nRESULT fail\n"); return 1; }
    HRESULT hr = en(enum_cb, NULL);
    printf("DirectPlayEnumerate -> 0x%08lx, %d providers, online listed %d\n", (unsigned long)hr, n_all, n_online);
    LPDIRECTPLAY dp = NULL;
    HRESULT hc = cr((LPGUID)&OKP_SPGUID, &dp, NULL);
    printf("DirectPlayCreate(Online) -> 0x%08lx, %p\n", (unsigned long)hc, (void *)dp);
    GetModuleFileNameA(GetModuleHandleA("dplayx.dll"), dx, sizeof(dx));
    printf("dplayx used: %s\n", dx);
    if (dp) IDirectPlay_Release(dp);
    int ok = hr == DP_OK && n_online && hc == DP_OK && dp;
    printf("RESULT %s\n", ok ? "ok" : "fail");
    return ok ? 0 : 1;
}
