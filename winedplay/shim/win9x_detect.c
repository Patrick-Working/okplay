/* okp_win9x_paths for DLLs built without okplay (build_dpwsockx.sh): the same answer as okplay's
 * compat.c - this is Windows 9x, or OKP_EMULATE_9X=1 asks the 9x paths for a test on NT. */
#include <windows.h>

int okp_win9x_paths(void)
{
    static int is9x = -1;
    if (is9x < 0) {
        OSVERSIONINFOA v;
        char e[8];
        memset(&v, 0, sizeof(v));
        v.dwOSVersionInfoSize = sizeof(v);
        GetVersionExA(&v);
        is9x = v.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS ||
               (GetEnvironmentVariableA("OKP_EMULATE_9X", e, sizeof(e)) && e[0] == '1');
    }
    return is9x;
}
