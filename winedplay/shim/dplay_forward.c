/* dplay.dll - DirectPlay 1/2 for games of the DirectX 3 era, which import DPLAY.DLL instead of
 * dplayx.dll (the SDK's DPLAY.LIB: DirectPlayCreate@12, DirectPlayEnumerate@8, by name). Windows'
 * own dplay.dll hands these to dplayx; this one hands them to the drop-in's dplayx.dll in the same
 * folder, so those games get Online/LAN Play too. Real functions instead of export forwarders, so
 * Windows 95's loader is fine with it, and the dplayx.dll next to us wins over the system's.
 * LGPL 2.1+, like the rest of the drop-in.
 */
#include <windows.h>

typedef HRESULT (WINAPI *create_t)(LPGUID, void **, IUnknown *);
typedef HRESULT (WINAPI *enum_t)(void *, void *);

static HMODULE dplayx(void)
{
    static HMODULE m;
    if (!m) {
        char path[MAX_PATH];
        HMODULE self = NULL;
        MEMORY_BASIC_INFORMATION mbi;
        /* our own folder (VirtualQuery: GetModuleHandleEx is XP+) */
        if (VirtualQuery((void *)dplayx, &mbi, sizeof(mbi))) self = (HMODULE)mbi.AllocationBase;
        if (self && GetModuleFileNameA(self, path, sizeof(path))) {
            char *bs = strrchr(path, '\\');
            if (bs) {
                lstrcpyA(bs + 1, "dplayx.dll");
                m = LoadLibraryA(path);
            }
        }
        if (!m) m = LoadLibraryA("dplayx.dll");
    }
    return m;
}

HRESULT WINAPI DirectPlayCreate(LPGUID sp, void **dp, IUnknown *outer)
{
    HMODULE m = dplayx();
    create_t fn = m ? (create_t)GetProcAddress(m, "DirectPlayCreate") : NULL;
    static int numbered;
    if (m && !numbered) {
        /* player ids 1, 2, 3... like the DirectPlay 1.1 dplay.dll we stand in for (dplay.c) */
        void (WINAPI *number)(void) = (void (WINAPI *)(void))GetProcAddress(m, "OkpDplay11Numbering");
        numbered = 1;
        if (number) number();
    }
    return fn ? fn(sp, dp, outer) : E_FAIL;
}

/* DirectX 3's DirectPlayEnumerate is the ANSI one (later DirectPlayEnumerateA) */
HRESULT WINAPI DirectPlayEnumerate(void *callback, void *context)
{
    HMODULE m = dplayx();
    enum_t fn = m ? (enum_t)GetProcAddress(m, "DirectPlayEnumerateA") : NULL;
    if (!fn && m) fn = (enum_t)GetProcAddress(m, "DirectPlayEnumerate");
    return fn ? fn(callback, context) : E_FAIL;
}
