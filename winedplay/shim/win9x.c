/*
 * Windows 95/98 from the same DLL, for Wine's dplayx code: every function it imports that Windows 9x
 * lacks (or only stubs) is replaced here, without touching Wine's source. Each replacement is bound
 * to the import-table name the compiler emits for a dllimport call (__imp__Name@N), so the linker
 * takes it from this object and never imports the system's copy:
 *
 *   - on Windows NT/2000/XP and later it calls the system's own function (looked up once), so
 *     behaviour there is exactly as before;
 *   - on Windows 9x it does the job itself (ANSI APIs, plain C string functions).
 *
 *   lstrcmpW lstrcpyW lstrcpynW lstrlenW  9x: stubs that fail (except lstrlenW) -> wcs* functions
 *   CreateEventW                         9x: stub -> CreateEventA
 *   GetMessageW                          9x: stub -> GetMessageA (the lobby thread's message wait)
 *   RegGetValueA/W                       Vista+ -> RegQueryValueEx (W via A on 9x)
 *   InitOnceExecuteOnce                  Vista+ -> a compare-and-swap once
 *   GetModuleHandleExA                   XP+ -> VirtualQuery / GetModuleHandleA
 *   GetThreadId                          Vista+ -> 0 on systems without it (callers cope)
 *   SetThreadDescription                 Win10+ -> nothing (dpwsockx)
 *   IsBadStringPtrW                      9x: stub -> IsBadReadPtr per character (debugstr_w)
 *   CreateThread                         9x: needs a non-NULL lpThreadId -> always passes one
 *
 * winedplay/check9x.sh proves the built DLL imports nothing Windows 98 doesn't export.
 * Part of the Windows port for Classic Netplay for AoE2. LGPL 2.1+, like the rest of dplayx.
 */

#include <windows.h>
#include <wchar.h>
#include <string.h>

extern int okp_win9x_paths( void );   /* okplay compat.c: a real 9x, or OKP_EMULATE_9X */

static FARPROC real( const char *dll, const char *name )
{
    HMODULE m = GetModuleHandleA( dll );
    if ( !m ) m = LoadLibraryA( dll );
    return m ? GetProcAddress( m, name ) : NULL;
}

/* the NT function, or NULL on the 9x paths (cached; racing threads store the same value) */
#define NT_FN( type, dll, name ) \
    static __typeof__( type ) fn; static int looked; \
    if ( !looked ) { fn = okp_win9x_paths() ? NULL : (type)real( dll, name ); looked = 1; }

/* bind a replacement to the import symbol the callers use */
#define BIND( fn, sym ) void *const imp_##fn __asm__( sym ) = (void *)fn

/* ------------------------------------------------------------------ strings */

static int WINAPI x_lstrcmpW( LPCWSTR a, LPCWSTR b )
{
    NT_FN( int (WINAPI *)( LPCWSTR, LPCWSTR ), "kernel32.dll", "lstrcmpW" );
    if ( fn ) return fn( a, b );
    if ( !a || !b ) return a ? 1 : b ? -1 : 0;
    { int r = wcscmp( a, b ); return r < 0 ? -1 : r > 0; }
}
BIND( x_lstrcmpW, "__imp__lstrcmpW@8" );

static LPWSTR WINAPI x_lstrcpyW( LPWSTR d, LPCWSTR s )
{
    NT_FN( LPWSTR (WINAPI *)( LPWSTR, LPCWSTR ), "kernel32.dll", "lstrcpyW" );
    if ( fn ) return fn( d, s );
    if ( !d || !s ) return NULL;
    return wcscpy( d, s );
}
BIND( x_lstrcpyW, "__imp__lstrcpyW@8" );

static LPWSTR WINAPI x_lstrcpynW( LPWSTR d, LPCWSTR s, int n )
{
    NT_FN( LPWSTR (WINAPI *)( LPWSTR, LPCWSTR, int ), "kernel32.dll", "lstrcpynW" );
    if ( fn ) return fn( d, s, n );
    if ( !d || !s || n <= 0 ) return d;
    {
        int i = 0;
        while ( i < n - 1 && s[ i ] ) { d[ i ] = s[ i ]; i++; }
        d[ i ] = 0;
    }
    return d;
}
BIND( x_lstrcpynW, "__imp__lstrcpynW@12" );

static int WINAPI x_lstrlenW( LPCWSTR s )
{
    return s ? (int)wcslen( s ) : 0;   /* the same everywhere */
}
BIND( x_lstrlenW, "__imp__lstrlenW@4" );

/* ------------------------------------------------------------------ kernel objects, messages */

static HANDLE WINAPI x_CreateEventW( LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCWSTR name )
{
    NT_FN( HANDLE (WINAPI *)( LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCWSTR ), "kernel32.dll", "CreateEventW" );
    if ( fn ) return fn( sa, manual, initial, name );
    {
        char a[ MAX_PATH ];
        if ( name ) WideCharToMultiByte( CP_ACP, 0, name, -1, a, sizeof(a), NULL, NULL );
        return CreateEventA( sa, manual, initial, name ? a : NULL );
    }
}
BIND( x_CreateEventW, "__imp__CreateEventW@16" );

static BOOL WINAPI x_GetMessageW( MSG *msg, HWND wnd, UINT lo, UINT hi )
{
    NT_FN( BOOL (WINAPI *)( MSG *, HWND, UINT, UINT ), "user32.dll", "GetMessageW" );
    return fn ? fn( msg, wnd, lo, hi ) : GetMessageA( msg, wnd, lo, hi );
}
BIND( x_GetMessageW, "__imp__GetMessageW@16" );

/* ------------------------------------------------------------------ registry */

/* RegGetValue for what dplay.c asks: a value of the key itself (subkey NULL), REG_SZ or REG_DWORD,
 * with the size query (data NULL) and RRF_RT_* type checks */
static LSTATUS reg_get( HKEY key, LPCSTR subA, LPCWSTR subW, LPCSTR nameA, LPCWSTR nameW, DWORD flags,
                        LPDWORD type, PVOID data, LPDWORD size, BOOL wide )
{
    DWORD t = 0, n = size ? *size : 0;
    LSTATUS r;
    HKEY k = key;

    if ( (subA && *subA) || (subW && *subW) )
    {
        char a[ MAX_PATH ];
        if ( subW ) WideCharToMultiByte( CP_ACP, 0, subW, -1, a, sizeof(a), NULL, NULL );
        r = RegOpenKeyExA( key, subW ? a : subA, 0, KEY_QUERY_VALUE, &k );
        if ( r ) return r;
    }
    if ( !wide )
        r = RegQueryValueExA( k, nameA, NULL, &t, data, &n );
    else
    {
        /* 9x has no W registry: read the ANSI value and widen strings */
        char a[ 256 ], *buf = NULL;
        DWORD an = 0;
        if ( nameW ) WideCharToMultiByte( CP_ACP, 0, nameW, -1, a, sizeof(a), NULL, NULL );
        r = RegQueryValueExA( k, nameW ? a : NULL, NULL, &t, NULL, &an );
        if ( !r && (t == REG_SZ || t == REG_EXPAND_SZ) )
        {
            int wn;
            if ( !(buf = malloc( an + 1 )) ) r = ERROR_OUTOFMEMORY;
            else if ( !(r = RegQueryValueExA( k, nameW ? a : NULL, NULL, &t, (BYTE *)buf, &an )) )
            {
                buf[ an ] = 0;
                wn = MultiByteToWideChar( CP_ACP, 0, buf, -1, NULL, 0 );
                if ( data && n >= wn * sizeof(WCHAR) ) MultiByteToWideChar( CP_ACP, 0, buf, -1, data, wn );
                else if ( data ) r = ERROR_MORE_DATA;
                n = wn * sizeof(WCHAR);
            }
            free( buf );
        }
        else if ( !r )
        {
            DWORD want = an;
            r = data && n >= want ? RegQueryValueExA( k, nameW ? a : NULL, NULL, &t, data, &n )
                                  : (data ? ERROR_MORE_DATA : ERROR_SUCCESS);
            n = want;
        }
    }
    if ( k != key ) RegCloseKey( k );
    if ( r && r != ERROR_MORE_DATA ) return r;
    if ( (flags & 0xffff) != RRF_RT_ANY )
    {
        DWORD bit = t == REG_SZ ? RRF_RT_REG_SZ : t == REG_EXPAND_SZ ? RRF_RT_REG_EXPAND_SZ :
                    t == REG_DWORD ? RRF_RT_REG_DWORD : t == REG_BINARY ? RRF_RT_REG_BINARY :
                    t == REG_MULTI_SZ ? RRF_RT_REG_MULTI_SZ : t == REG_QWORD ? RRF_RT_REG_QWORD :
                    t == REG_NONE ? RRF_RT_REG_NONE : 0;
        if ( !(flags & bit) ) return ERROR_UNSUPPORTED_TYPE;
    }
    if ( type ) *type = t;
    if ( size ) *size = n;
    return r;
}

static LSTATUS WINAPI x_RegGetValueA( HKEY key, LPCSTR sub, LPCSTR name, DWORD flags, LPDWORD type,
                                      PVOID data, LPDWORD size )
{
    NT_FN( LSTATUS (WINAPI *)( HKEY, LPCSTR, LPCSTR, DWORD, LPDWORD, PVOID, LPDWORD ), "advapi32.dll", "RegGetValueA" );
    if ( fn ) return fn( key, sub, name, flags, type, data, size );
    return reg_get( key, sub, NULL, name, NULL, flags, type, data, size, FALSE );
}
BIND( x_RegGetValueA, "__imp__RegGetValueA@28" );

static LSTATUS WINAPI x_RegGetValueW( HKEY key, LPCWSTR sub, LPCWSTR name, DWORD flags, LPDWORD type,
                                      PVOID data, LPDWORD size )
{
    NT_FN( LSTATUS (WINAPI *)( HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD ), "advapi32.dll", "RegGetValueW" );
    if ( fn ) return fn( key, sub, name, flags, type, data, size );
    return reg_get( key, NULL, sub, NULL, name, flags, type, data, size, TRUE );
}
BIND( x_RegGetValueW, "__imp__RegGetValueW@28" );

/* ------------------------------------------------------------------ once, modules, threads */

/* InitOnceExecuteOnce, synchronous form only (what dplayx uses). The state word: 0 new, 1 running,
 * 2 done. On NT the system's (the INIT_ONCE layouts differ, so don't mix: one or the other per run). */
static BOOL WINAPI x_InitOnceExecuteOnce( PINIT_ONCE once, PINIT_ONCE_FN init, PVOID param, LPVOID *ctx )
{
    NT_FN( BOOL (WINAPI *)( PINIT_ONCE, PINIT_ONCE_FN, PVOID, LPVOID * ), "kernel32.dll", "InitOnceExecuteOnce" );
    if ( fn ) return fn( once, init, param, ctx );
    {
        volatile LONG *state = (volatile LONG *)once;
        if ( __sync_val_compare_and_swap( state, 0, 1 ) == 0 )
        {
            BOOL ok = init( once, param, ctx );
            *state = ok ? 2 : 0;
            return ok;
        }
        while ( *state == 1 ) Sleep( 0 );
        return *state == 2;
    }
}
BIND( x_InitOnceExecuteOnce, "__imp__InitOnceExecuteOnce@16" );

static BOOL WINAPI x_GetModuleHandleExA( DWORD flags, LPCSTR name, HMODULE *out )
{
    NT_FN( BOOL (WINAPI *)( DWORD, LPCSTR, HMODULE * ), "kernel32.dll", "GetModuleHandleExA" );
    if ( fn ) return fn( flags, name, out );
    if ( !out ) return FALSE;
    if ( flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS )
    {
        MEMORY_BASIC_INFORMATION mbi;
        *out = VirtualQuery( name, &mbi, sizeof(mbi) ) ? (HMODULE)mbi.AllocationBase : NULL;
    }
    else
        *out = GetModuleHandleA( name );
    /* callers here always pass UNCHANGED_REFCOUNT; a pinned or counted reference isn't needed */
    return *out != NULL;
}
BIND( x_GetModuleHandleExA, "__imp__GetModuleHandleExA@12" );

static DWORD WINAPI x_GetThreadId( HANDLE thread )
{
    NT_FN( DWORD (WINAPI *)( HANDLE ), "kernel32.dll", "GetThreadId" );
    return fn ? fn( thread ) : 0;
}
BIND( x_GetThreadId, "__imp__GetThreadId@4" );

/* Windows 10 1607+ (thread names for debuggers; Wine's dpwsockx names its thread): nothing to do
 * where it is missing */
static HRESULT WINAPI x_SetThreadDescription( HANDLE thread, PCWSTR name )
{
    NT_FN( HRESULT (WINAPI *)( HANDLE, PCWSTR ), "kernel32.dll", "SetThreadDescription" );
    return fn ? fn( thread, name ) : S_OK;
}
BIND( x_SetThreadDescription, "__imp__SetThreadDescription@8" );

/* 9x: a stub (wine/debug.h debugstr_w calls it) -> check the memory a character at a time */
static BOOL WINAPI x_IsBadStringPtrW( LPCWSTR str, UINT_PTR max )
{
    UINT_PTR i;
    NT_FN( BOOL (WINAPI *)( LPCWSTR, UINT_PTR ), "kernel32.dll", "IsBadStringPtrW" );
    if ( fn ) return fn( str, max );
    if ( !str ) return max != 0;
    for ( i = 0; i < max; i++ )
    {
        if ( IsBadReadPtr( str + i, sizeof(WCHAR) ) ) return TRUE;
        if ( !str[ i ] ) break;
    }
    return FALSE;
}
BIND( x_IsBadStringPtrW, "__imp__IsBadStringPtrW@8" );

/* Windows 9x fails CreateThread when lpThreadId is NULL (documented); give it somewhere to write */
static HANDLE WINAPI x_CreateThread( LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE start,
                                     LPVOID param, DWORD flags, LPDWORD id )
{
    static HANDLE (WINAPI *fn)( LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD );
    DWORD dummy, *tid = id ? id : &dummy;
    HANDLE h;
    if ( !fn ) fn = (void *)real( "kernel32.dll", "CreateThread" );   /* the system's, on every Windows */
    h = fn( sa, stack, start, param, flags, tid );
#ifdef OKP_BUILTIN   /* the drop-in has okplay's log; the stand-alone TCP/IP provider (build_dpwsockx.sh) not */
    {
        extern void okp_log( int level, const char *fmt, ... );
        /* which of our threads is which: a crash names only the thread id (no dbghelp on 9x) */
        if ( h ) okp_log( 3, "thread %lu started at %p", (unsigned long)*tid, (void *)start );
    }
#endif
    return h;
}
BIND( x_CreateThread, "__imp__CreateThread@24" );
