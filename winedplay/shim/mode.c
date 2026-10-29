/*
 * The drop-in's mode switch: which DirectPlay the game gets. okplay.ini, [okplay] section:
 *
 *   mode=all       (default) one list: this DLL's Online/LAN connections, then Windows' classic ones
 *                  (TCP/IP, IPX, ...). Picking a classic one hands the game to Windows' DirectPlay
 *                  (switch.c), so it plays with people who don't have the drop-in. Without Windows'
 *                  DirectPlay installed this is the same as oldkid.
 *   mode=oldkid    this DLL only: Wine's DirectPlay with okplay's Online/LAN connections
 *   mode=windows   Windows' own DirectPlay (system dplayx.dll) only: classic TCP/IP, IPX and lobby
 *                  programs, exactly as without the drop-in
 *   mode=ask       a box at the game's first DirectPlay call: oldkid or windows
 *   lobby_auto=1   (default) a game started by a DirectPlay lobby program gets Windows' DirectPlay
 *                  whatever mode says: the lobby hands the game its connection through Windows'
 *                  DirectPlay, and only Windows' can pick it up. 0 = off.
 *
 * OKP_MODE in the environment overrides mode= (tests). Windows' DirectPlay is an optional Windows
 * feature: when it isn't installed, windows/ask fall back to this DLL (and say so).
 *
 * Decided once per game run, at the first call into any export: every export below then goes
 * either to Wine's function or to the same one in Windows' dplayx.dll, loaded by its full path.
 * The intro-video decoder (builtin.c) works in both modes. Windows' dpwsockx/dpmodemx import only
 * gdwDPlaySPRefCount from "DPLAYX.dll"; the loader may bind that to this DLL's copy - harmless,
 * it only counts provider references.
 *
 * Part of the Windows port for Classic Netplay for AoE2. LGPL 2.1+, like the rest of dplayx.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "objbase.h"
#include "dplay.h"
#include "dplobby.h"

#include "switch.h"

extern void okp_log( int level, const char *fmt, ... );
extern BOOL DP_BUILTIN_IsOurs( const GUID *guid );
extern BOOL DP_BUILTIN_Remap( GUID *sp, void **address, DWORD *size );
extern int  DP_BUILTIN_RemapMode( const GUID *sp );
extern BOOL DP_BUILTIN_Address( const GUID *sp, void **address, DWORD *size );
extern int okp_win9x_paths( void );   /* okplay compat.c: a real 9x, or OKP_EMULATE_9X */

typedef HRESULT (WINAPI *lobby_a_t)( LPGUID, LPDIRECTPLAYLOBBYA *, IUnknown *, LPVOID, DWORD );
typedef HRESULT (WINAPI *lobby_w_t)( LPGUID, LPDIRECTPLAYLOBBY *, IUnknown *, LPVOID, DWORD );
typedef HRESULT (WINAPI *classobj_t)( REFCLSID, REFIID, LPVOID * );

/* set once windows mode is chosen; NULL = this DLL */
static struct
{
    create_t create;
    enum_a_t enum_a;
    enum_w_t enum_w;
    lobby_a_t lobby_a;
    lobby_w_t lobby_w;
    classobj_t classobj;
} win;

/* mode=all: Windows' functions for the classic half of the list (create NULL = not in use) */
static struct
{
    create_t create;
    enum_a_t enum_a;
    enum_w_t enum_w;
} all;

static INIT_ONCE once = INIT_ONCE_STATIC_INIT;

static void ini_path( char *out, DWORD size )
{
    HMODULE self = NULL;
    char *slash;
    GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        (LPCSTR)ini_path, &self );
    GetModuleFileNameA( self, out, size );
    slash = strrchr( out, '\\' );
    if ( slash ) slash[ 1 ] = 0; else out[ 0 ] = 0;
    strncat( out, "okplay.ini", size - strlen( out ) - 1 );
}

/* Windows' dplayx.dll, if the DirectPlay feature is installed (a 32-bit process on 64-bit
 * Windows gets SysWOW64 through the file-system redirection) */
static HMODULE load_windows_dplayx( char *path, DWORD size )
{
    HMODULE self = NULL, m;
    char copy[ MAX_PATH ];
    UINT n = GetSystemDirectoryA( path, size );
    if ( !n || n + 12 >= size ) return NULL;
    strcat( path, "\\dplayx.dll" );
    if ( GetFileAttributesA( path ) == INVALID_FILE_ATTRIBUTES ) return NULL;   /* feature off */
    GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        (LPCSTR)load_windows_dplayx, &self );
    m = LoadLibraryA( path );
    if ( !m ) return NULL;
    if ( m != self && !okp_win9x_paths() ) return m;
    /* Windows 9x matches loaded DLLs by file name alone: asking for the system's dplayx.dll gave this
     * one back (calling it would loop into ourselves). Load a copy under another name instead - on
     * the 9x paths always, so OKP_EMULATE_9X runs this on a new Windows too. */
    FreeLibrary( m );
    /* the copy lives next to this DLL (the game folder), refreshed at every launch; the temp folder
     * only if the game folder can't be written */
    ini_path( copy, sizeof(copy) );
    *strrchr( copy, '\\' ) = 0;
    if ( strlen( copy ) + 32 >= sizeof(copy) ) return NULL;
    strcat( copy, "\\dplayx-windows9x.dll" );
    if ( !CopyFileA( path, copy, FALSE ) && GetFileAttributesA( copy ) == INVALID_FILE_ATTRIBUTES )
    {
        n = GetTempPathA( sizeof(copy), copy );
        if ( !n || n + 24 >= sizeof(copy) ) return NULL;
        strcat( copy, "dplayx-windows9x.dll" );
        CopyFileA( path, copy, FALSE );   /* fails harmlessly if another game has it loaded: same file */
    }
    m = LoadLibraryA( copy );
    if ( m == self ) { FreeLibrary( m ); m = NULL; }
    if ( m ) lstrcpynA( path, copy, size );
    okp_log( m ? 1 : 0, "mode: Windows' dplayx.dll has our file name on this Windows - %s", m ?
             "using a renamed copy" : "could not load a renamed copy, classic connections are off" );
    return m;
}

/* did a lobby program start this game? It leaves the connection with Windows' DirectPlay */
static BOOL launched_by_lobby( HMODULE windows )
{
    lobby_a_t create = (lobby_a_t)GetProcAddress( windows, "DirectPlayLobbyCreateA" );
    IDirectPlayLobbyA *lobby = NULL;
    DWORD size = 0;
    HRESULT hr;

    if ( !create || FAILED( create( NULL, &lobby, NULL, NULL, 0 ) ) || !lobby ) return FALSE;
    hr = IDirectPlayLobby_GetConnectionSettings( lobby, 0, NULL, &size );
    IDirectPlayLobby_Release( lobby );
    return hr == DPERR_BUFFERTOOSMALL;   /* DPERR_NOTLOBBIED when started normally */
}

static BOOL ask( const char *brand )
{
    char text[ 512 ];
    snprintf( text, sizeof(text),
              "Which multiplayer should the game use?\n\n"
              "Yes  -  %s (Online Play and LAN Play: no VPN, no port forwarding)\n\n"
              "No  -  Classic Windows DirectPlay (TCP/IP, IPX and lobby programs, for playing "
              "with people who don't have %s)", brand, brand );
    return MessageBoxA( NULL, text, brand, MB_YESNO | MB_ICONQUESTION | MB_TOPMOST |
                        MB_SETFOREGROUND | MB_TASKMODAL ) == IDNO;
}

static BOOL CALLBACK decide( INIT_ONCE *o, void *param, void **ctx )
{
    char ini[ MAX_PATH ], mode[ 32 ], brand[ 96 ], path[ MAX_PATH ];
    const char *env = getenv( "OKP_MODE" );
    BOOL lobby_auto, want_windows = FALSE;
    HMODULE windows;

    ini_path( ini, sizeof(ini) );
    GetPrivateProfileStringA( "okplay", "mode", "all", mode, sizeof(mode), ini );
    if ( env && *env ) lstrcpynA( mode, env, sizeof(mode) );
    GetPrivateProfileStringA( "okplay", "brand", "Classic Netplay", brand, sizeof(brand), ini );
    lobby_auto = GetPrivateProfileIntA( "okplay", "lobby_auto", 1, ini ) != 0;
    /* trace=1: log every DirectPlay call the game makes (mode=all's switcher sees them all) */
    SW_SetTrace( GetPrivateProfileIntA( "okplay", "trace", 0, ini ) != 0 );

    windows = load_windows_dplayx( path, sizeof(path) );

    if ( windows && lobby_auto && launched_by_lobby( windows ) )
    {
        okp_log( 1, "mode: started by a lobby program - using Windows' DirectPlay (%s)", path );
        want_windows = TRUE;
    }
    else if ( !_stricmp( mode, "windows" ) )
    {
        if ( windows ) want_windows = TRUE;
        else
        {
            char text[ 512 ];
            okp_log( 0, "mode: windows asked for, but Windows' DirectPlay is not installed - using %s", brand );
            snprintf( text, sizeof(text), "Windows' DirectPlay is not installed on this PC, so the "
                      "classic connections are not available.\n\nThe game uses %s instead. (To add it: "
                      "Control Panel > Programs > Turn Windows features on or off > Legacy Components > "
                      "DirectPlay.)", brand );
            MessageBoxA( NULL, text, brand, MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND |
                         MB_TASKMODAL );
        }
    }
    else if ( !_stricmp( mode, "ask" ) )
    {
        if ( windows ) want_windows = ask( brand );
        else okp_log( 1, "mode: ask, but Windows' DirectPlay is not installed - using %s without asking", brand );
    }
    else if ( !_stricmp( mode, "all" ) )
    {
        if ( windows )
        {
            all.create = (create_t)GetProcAddress( windows, "DirectPlayCreate" );
            all.enum_a = (enum_a_t)GetProcAddress( windows, "DirectPlayEnumerateA" );
            all.enum_w = (enum_w_t)GetProcAddress( windows, "DirectPlayEnumerateW" );
        }
        if ( all.create )
        {
            SW_SetWindows( all.create );
            okp_log( 1, "mode: all - %s's connections, then Windows' classic ones (%s)", brand, path );
            return TRUE;   /* keeps Windows' dplayx loaded */
        }
        memset( &all, 0, sizeof(all) );
        okp_log( 1, "mode: all, but Windows' DirectPlay is not installed - %s's connections only", brand );
    }
    else if ( _stricmp( mode, "oldkid" ) )
        okp_log( 0, "mode: unknown mode=%s in okplay.ini - using %s", mode, brand );

    if ( want_windows )
    {
        win.create   = (create_t)GetProcAddress( windows, "DirectPlayCreate" );
        win.enum_a   = (enum_a_t)GetProcAddress( windows, "DirectPlayEnumerateA" );
        win.enum_w   = (enum_w_t)GetProcAddress( windows, "DirectPlayEnumerateW" );
        win.lobby_a  = (lobby_a_t)GetProcAddress( windows, "DirectPlayLobbyCreateA" );
        win.lobby_w  = (lobby_w_t)GetProcAddress( windows, "DirectPlayLobbyCreateW" );
        win.classobj = (classobj_t)GetProcAddress( windows, "DllGetClassObject" );
        if ( !win.create || !win.lobby_a )
        {
            okp_log( 0, "mode: %s lacks DirectPlayCreate/DirectPlayLobbyCreateA - using %s", path, brand );
            memset( &win, 0, sizeof(win) );
        }
        else
            okp_log( 1, "mode: windows - the game uses Windows' DirectPlay (%s); %s's connections are off",
                     path, brand );
    }
    if ( !win.create )
    {
        okp_log( 1, "mode: oldkid - the game uses %s (Wine DirectPlay + Online/LAN Play)", brand );
        if ( windows ) FreeLibrary( windows );
    }
    return TRUE;
}

static void choose(void)
{
    InitOnceExecuteOnce( &once, decide, NULL, NULL );
}

/* trace=1 and one of our connections asked for by GUID (a DirectPlay 1 game picking Online Play from
 * its own list - Necrodome): the object made exactly as without trace, then wrapped in the switcher
 * so the call log sees it */
static HRESULT traced_create( LPGUID guid, LPDIRECTPLAY *dp )
{
    IDirectPlay *inner = NULL;
    HRESULT hr = DirectPlayCreate( guid, &inner, NULL );
    if ( FAILED( hr ) ) return hr;
    return SW_Wrap( inner, &IID_IDirectPlay, (void **)dp );
}

/* the exports (the drop-in's .def points the public names here) */

HRESULT WINAPI MODE_DirectPlayCreate( LPGUID guid, LPDIRECTPLAY *dp, IUnknown *outer )
{
    GUID mapped;
    choose();
    /* a game with a fixed connection list asking for TCP/IP or IPX by GUID: okplay.ini
     * classic_tcpip/classic_ipx can hand that entry to ours (not in mode=windows) */
    if ( guid && dp && !outer && !win.create &&
         ((all.create && DP_BUILTIN_RemapMode( guid ) == 3) || (mapped = *guid, DP_BUILTIN_Remap( &mapped, NULL, NULL ))) )
    {
        /* through the switcher when there is one (trace=1 sees the calls; classic_*=ask is deferred
         * there until the game uses the connection), else this DLL's object */
        IDirectPlay3A *dp3;
        HRESULT hr;
        BOOL ask = DP_BUILTIN_RemapMode( guid ) == 3;
        if ( !all.create ) return DirectPlayCreate( &mapped, dp, outer );
        if ( FAILED( hr = SW_Create( &IID_IDirectPlay, (void **)dp ) ) ) return hr;
        if ( SUCCEEDED( hr = IDirectPlay_QueryInterface( *dp, &IID_IDirectPlay3A, (void **)&dp3 ) ) )
        {
            void *address = NULL;
            DWORD size = 80;
            GUID again = *guid;
            /* ask: the switcher gets the classic address itself and defers; otherwise ours */
            if ( ask ? DP_BUILTIN_Address( guid, &address, &size ) : (DP_BUILTIN_Remap( &again, &address, &size ) && address) )
                hr = IDirectPlayX_InitializeConnection( dp3, address, 0 );   /* the Wine side swaps it for ours again */
            else hr = DPERR_UNAVAILABLE;
            IDirectPlayX_Release( dp3 );
            free( address );
        }
        if ( FAILED( hr ) ) { IDirectPlay_Release( *dp ); *dp = NULL; }
        return hr;
    }
    if ( all.create && guid && dp && !outer )
    {
        /* no provider yet: the switcher; ours: this DLL; a classic one: straight to Windows' */
        if ( IsEqualGUID( guid, &GUID_NULL ) ) return SW_Create( &IID_IDirectPlay, (void **)dp );
        if ( SW_Tracing() && DP_BUILTIN_IsOurs( guid ) ) return traced_create( guid, dp );
        if ( !DP_BUILTIN_IsOurs( guid ) )
        {
            HRESULT hr = all.create( guid, dp, outer );
            okp_log( 2, "all: DirectPlayCreate(%08lx-%04x-...) is a classic provider - Windows' DirectPlay: 0x%08lx",
                     guid->Data1, guid->Data2, hr );
            return hr;
        }
    }
    return win.create ? win.create( guid, dp, outer ) : DirectPlayCreate( guid, dp, outer );
}

HRESULT WINAPI MODE_DirectPlayEnumerateA( LPDPENUMDPCALLBACKA cb, LPVOID ctx )
{
    choose();
    if ( all.create ) return SW_EnumerateA( cb, ctx, all.enum_a );
    return win.create ? (win.enum_a ? win.enum_a( cb, ctx ) : DPERR_UNSUPPORTED) : DirectPlayEnumerateA( cb, ctx );
}

HRESULT WINAPI MODE_DirectPlayEnumerateW( LPDPENUMDPCALLBACKW cb, LPVOID ctx )
{
    choose();
    if ( all.create ) return SW_EnumerateW( cb, ctx, all.enum_w );
    return win.create ? (win.enum_w ? win.enum_w( cb, ctx ) : DPERR_UNSUPPORTED) : DirectPlayEnumerateW( cb, ctx );
}

HRESULT WINAPI MODE_DirectPlayLobbyCreateA( LPGUID guid, LPDIRECTPLAYLOBBYA *lobby, IUnknown *outer,
                                            LPVOID data, DWORD size )
{
    choose();
    return win.create ? win.lobby_a( guid, lobby, outer, data, size )
                      : DirectPlayLobbyCreateA( guid, lobby, outer, data, size );
}

HRESULT WINAPI MODE_DirectPlayLobbyCreateW( LPGUID guid, LPDIRECTPLAYLOBBY *lobby, IUnknown *outer,
                                            LPVOID data, DWORD size )
{
    choose();
    return win.create ? (win.lobby_w ? win.lobby_w( guid, lobby, outer, data, size ) : DPERR_UNSUPPORTED)
                      : DirectPlayLobbyCreateW( guid, lobby, outer, data, size );
}

extern HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID iid, LPVOID *ppv );

HRESULT WINAPI MODE_DllGetClassObject( REFCLSID clsid, REFIID iid, LPVOID *ppv )
{
    choose();
    if ( all.create && clsid && IsEqualCLSID( clsid, &CLSID_DirectPlay ) ) return SW_GetClassObject( iid, ppv );
    return win.create ? (win.classobj ? win.classobj( clsid, iid, ppv ) : CLASS_E_CLASSNOTAVAILABLE)
                      : DllGetClassObject( clsid, iid, ppv );
}
