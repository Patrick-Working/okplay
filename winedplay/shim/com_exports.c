/* The COM entry points winecrt0 normally generates. This dplayx is loaded app-locally, never
 * registered, so (un)registering is a no-op. */
#include <windows.h>

HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }
HRESULT WINAPI DllRegisterServer(void) { return S_OK; }
HRESULT WINAPI DllUnregisterServer(void) { return S_OK; }
