/* okplay loaded as dbghelp.dll (build_proxy.sh, -DOKW_PROXY).
 *
 * dethrace (Carmageddon) imports ws2_32.dll, a Windows "known DLL" that is always taken from System32, so okplay's
 * wsock32.dll is never loaded. It also imports dbghelp.dll for its crash report - not a known DLL - so a dbghelp.dll
 * next to the exe is loaded instead of Windows' own. This file is that DLL's dbghelp part: every function dethrace's
 * builds import (32-bit: StackWalk, SymGetModuleBase, ...; 64-bit: StackWalk64, SymGetModuleBase64, ...) passes to
 * Windows' dbghelp.dll, loaded from System32 the first time one is called (only when the game crashes). wsock.c
 * (okw_proxy_attach) does the rest: the game's Winsock imports are pointed at okplay in memory.
 *
 * Own types instead of <dbghelp.h>: the header declares these names as imports. Pointer-sized arguments are void *. */
#include <windows.h>

typedef unsigned long long u64;

static HMODULE real_dbghelp(void)
{
    static HMODULE m;
    if (!m) {
        char p[MAX_PATH];
        UINT n = GetSystemDirectoryA(p, MAX_PATH - 16);
        if (n) {
            lstrcatA(p, "\\dbghelp.dll");
            m = LoadLibraryA(p);
        }
    }
    return m;
}

static FARPROC real_fn(const char *name)
{
    HMODULE m = real_dbghelp();
    return m ? GetProcAddress(m, name) : NULL;
}

#define PASS(name, ret, fail, params, args)                         \
    ret WINAPI pdh_##name params                                    \
    {                                                               \
        typedef ret(WINAPI * fn_t) params;                          \
        static fn_t real_;                                            \
        if (!real_) real_ = (fn_t)(void *)real_fn(#name);                 \
        if (!real_) { SetLastError(ERROR_PROC_NOT_FOUND); return fail; } \
        return real_ args;                                            \
    }

PASS(StackWalk64, BOOL, FALSE, (DWORD m, HANDLE p, HANDLE t, void *f, void *c, void *r, void *fa, void *mb, void *ta), (m, p, t, f, c, r, fa, mb, ta))
PASS(StackWalk, BOOL, FALSE, (DWORD m, HANDLE p, HANDLE t, void *f, void *c, void *r, void *fa, void *mb, void *ta), (m, p, t, f, c, r, fa, mb, ta))
PASS(SymFunctionTableAccess64, void *, NULL, (HANDLE p, u64 a), (p, a))
PASS(SymFunctionTableAccess, void *, NULL, (HANDLE p, DWORD a), (p, a))
PASS(SymGetModuleBase64, u64, 0, (HANDLE p, u64 a), (p, a))
PASS(SymGetModuleBase, DWORD, 0, (HANDLE p, DWORD a), (p, a))
PASS(SymGetModuleInfo64, BOOL, FALSE, (HANDLE p, u64 a, void *i), (p, a, i))
PASS(SymGetModuleInfo, BOOL, FALSE, (HANDLE p, DWORD a, void *i), (p, a, i))
PASS(SymGetLineFromAddr64, BOOL, FALSE, (HANDLE p, u64 a, DWORD *d, void *l), (p, a, d, l))
PASS(SymGetLineFromAddr, BOOL, FALSE, (HANDLE p, DWORD a, DWORD *d, void *l), (p, a, d, l))
PASS(SymFromAddr, BOOL, FALSE, (HANDLE p, u64 a, u64 *d, void *s), (p, a, d, s))
PASS(SymInitialize, BOOL, FALSE, (HANDLE p, const char *path, BOOL inv), (p, path, inv))
PASS(SymCleanup, BOOL, FALSE, (HANDLE p), (p))
PASS(SymRefreshModuleList, BOOL, FALSE, (HANDLE p), (p))
PASS(SymSetOptions, DWORD, 0, (DWORD o), (o))
PASS(SymGetOptions, DWORD, 0, (void), ())
PASS(MiniDumpWriteDump, BOOL, FALSE, (HANDLE p, DWORD pid, HANDLE f, int type, void *e, void *u, void *cb), (p, pid, f, type, e, u, cb))
