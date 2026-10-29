/* Force-included when building Wine's dplayx conformance tests for Windows. */
#include "wine_compat.h"
#define COBJMACROS
#define INITGUID   /* the test defines its GUIDs (and needs the firewall ones) in this file */
#include <windows.h>
#include <objbase.h>
/* Route COM creation of DirectPlay objects to whichever dplayx.dll this process loaded (a copy
 * next to the exe wins over System32), so the same test binary measures Windows' or Wine's. */
static HRESULT WINAPI wt_CoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx, REFIID riid, LPVOID *ppv)
{
    typedef HRESULT (WINAPI *gco_t)(REFCLSID, REFIID, void **);
    HMODULE m = GetModuleHandleA("dplayx.dll");
    gco_t gco = m ? (gco_t)GetProcAddress(m, "DllGetClassObject") : NULL;
    IClassFactory *cf;
    if (gco && SUCCEEDED(gco(clsid, &IID_IClassFactory, (void **)&cf))) {
        HRESULT hr = cf->lpVtbl->CreateInstance(cf, outer, riid, ppv);
        cf->lpVtbl->Release(cf);
        return hr;
    }
    return CoCreateInstance(clsid, outer, ctx, riid, ppv);
}
#define CoCreateInstance wt_CoCreateInstance
