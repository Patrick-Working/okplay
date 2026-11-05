/*
 * The drop-in edition: this dplayx.dll sits next to the game's exe and carries everything.
 *
 *  - okplay's two connections ("Online Play", "LAN Play") are built in: listed first, loaded from
 *    this very DLL (it exports okplay's SPInit). Windows' registered providers (TCP/IP, IPX, Modem,
 *    Serial) are left out unless okplay.ini says show_windows_connections=1.
 *  - the Indeo 5 intro-video decoder (okindeo.dll next to this DLL, optional) is registered for
 *    this process only, so the game's intro movies play with picture - no codec install.
 *
 * okplay.ini, [okplay] section, next to this DLL:
 *   online_name=...  lan_name=...          names in the game's connection list
 *   show_windows_connections=0|1            also list Windows' own providers (default 0)
 *   videos=0|1                              intro-video decoder (default 1)
 *   intro=0|1                               0 = skip AoE2's intro movies (NOSTARTUP; default 1)
 *   simple_textboxes=auto|0|1               plain Windows dialogs instead of the AoE2-styled boxes
 *   classic_tcpip=online|lan|ask            a game's fixed "TCP/IP" entry runs on Online/LAN Play (default: off)
 *   classic_ipx=online|lan                  the same for its "IPX" entry (classic_serial, classic_modem likewise)
 *   keep_desktop_mode=0|1                   1 = the game cannot switch the desktop resolution (default 0)
 *   com_redirect=0|1                        1 = CoCreateInstance(DirectPlay) in the game comes to this DLL (default 1)
 *                                           (default auto: styled in AoE2, plain in any other game)
 *
 * Part of the Windows port for Classic Netplay for AoE2. LGPL 2.1+, like the rest of dplayx.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "wingdi.h"
#include "winuser.h"
#include "mmsystem.h"
#include "vfw.h"
#include <wchar.h>
#include "wine/debug.h"
#include "wine/list.h"

#include "dplay_global.h"
#include "dplayx_global.h"
#include "version.h"   /* okplay/src: OKP_VERSION_STR */

WINE_DEFAULT_DEBUG_CHANNEL(dplay);

extern HRESULT DPL_CreateCompoundAddress( LPCDPCOMPOUNDADDRESSELEMENT lpElements, DWORD dwElementCount,
                                          LPVOID lpAddress, LPDWORD lpdwAddressSize, BOOL bAnsiInterface );
/* okplay (built into this DLL) */
extern void okp_builtin_attach( HINSTANCE hinst );
extern void DP_BUILTIN_GameFixes( void );
extern void okp_log( int level, const char *fmt, ... );
/* okplay compat.c: the Windows 9x paths, and hashing that is the same on every Windows */
extern int okp_win9x_paths( void );
extern void okp_sha256_parts( const void *const *parts, const size_t *lens, int n, unsigned char out[ 32 ] );
extern int okp_utf16_to_utf8( const WCHAR *in, char *out, int outsz );

static const GUID OKP_SPGUID  = { 0x66d8dc59, 0x28e8, 0x4d3e, { 0xba, 0x93, 0x2e, 0x98, 0xd2, 0x51, 0xc0, 0xf3 } };
static const GUID OKP_LANGUID = { 0x6bcff704, 0xce20, 0x4150, { 0x98, 0xb4, 0x5a, 0x71, 0x98, 0x17, 0x5d, 0x7d } };

static HINSTANCE self;
static char dir[ MAX_PATH ];   /* this DLL's folder, with a trailing backslash */
static char ini[ MAX_PATH ];

static void ini_str( const char *key, const char *def, char *out, DWORD size )
{
    GetPrivateProfileStringA( "okplay", key, def, out, size, ini );
}

static int ini_int( const char *key, int def )
{
    return GetPrivateProfileIntA( "okplay", key, def, ini );
}

BOOL DP_BUILTIN_IsOurs( const GUID *guid )
{
    return IsEqualGUID( guid, &OKP_SPGUID ) || IsEqualGUID( guid, &OKP_LANGUID );
}


BOOL DP_BUILTIN_ShowWindowsConnections(void)
{
    return ini_int( "show_windows_connections", 0 ) != 0;
}

/* OKP_TEST_LOBBY (tests only, environment variable - never okplay.ini): the real-game pair test
 * starts the game with "Lobby" on its command line (as the MSN Gaming Zone did) and this connection
 * as the lobby's settings, so one copy hosts and another joins without anyone clicking menus.
 * OKP_TEST_LOBBY_PLAYER names the player (default Host / Joiner), OKP_TEST_LOBBY_MAX the room size. */
static const GUID AOE2TC_APP = { 0x5de93f3f, 0xfc90, 0x4ee1, { 0xae, 0x5a, 0x63, 0xda, 0xfa, 0x05, 0x59, 0x50 } };
static LONG test_lobby_state;   /* 0 not looked yet, 1 set, -1 not set */
static DPLCONNECTION test_connA, test_connW;
static DPSESSIONDESC2 test_descA, test_descW;
static DPNAME test_nameA, test_nameW;
static char test_sessA[ 64 ], test_playerA[ 32 ];
static WCHAR test_sessW[ 64 ], test_playerW[ 32 ];

const DPLCONNECTION *DP_BUILTIN_TestLobby( BOOL unicode )
{
    if ( !test_lobby_state )
    {
        char v[ 96 ], *colon, sp[ 16 ], ip[ 64 ];
        BOOL online, tcpip;
        void *addr = NULL;
        DWORD addrSize = 0, nelem = 1;
        DPCOMPOUNDADDRESSELEMENT element[ 2 ];
        const GUID *spguid;
        BOOL host;
        LONG state = -1;

        if ( GetEnvironmentVariableA( "OKP_TEST_LOBBY", v, sizeof(v) ) && (colon = strchr( v, ':' )) && colon[ 1 ] &&
             ((host = !_strnicmp( v, "host:", 5 )) || !_strnicmp( v, "join:", 5 )) )
        {
            lstrcpynA( test_sessA, colon + 1, sizeof(test_sessA) );
            if ( !GetEnvironmentVariableA( "OKP_TEST_LOBBY_PLAYER", test_playerA, sizeof(test_playerA) ) )
                lstrcpyA( test_playerA, host ? "Host" : "Joiner" );
            MultiByteToWideChar( CP_ACP, 0, test_sessA, -1, test_sessW, ARRAY_SIZE( test_sessW ) );
            MultiByteToWideChar( CP_ACP, 0, test_playerA, -1, test_playerW, ARRAY_SIZE( test_playerW ) );

            /* LAN Play, or Online Play (through the room server) with OKP_TEST_LOBBY_SP=online, or
             * Windows' TCP/IP provider run by this DirectPlay with OKP_TEST_LOBBY_SP=tcpip (+ OKP_MODE=
             * oldkid, so no switch to Windows' DirectPlay; OKP_TEST_LOBBY_ADDR = the host's address
             * when joining): our DirectPlay on the wire against Microsoft's */
            sp[ 0 ] = 0;
            GetEnvironmentVariableA( "OKP_TEST_LOBBY_SP", sp, sizeof(sp) );
            online = !_stricmp( sp, "online" );
            tcpip = !_stricmp( sp, "tcpip" );
            spguid = tcpip ? &DPSPGUID_TCPIP : online ? &OKP_SPGUID : &OKP_LANGUID;
            element[ 0 ].guidDataType = DPAID_ServiceProvider;
            element[ 0 ].dwDataSize = sizeof(GUID);
            element[ 0 ].lpData = (void *)spguid;
            if ( tcpip && GetEnvironmentVariableA( "OKP_TEST_LOBBY_ADDR", ip, sizeof(ip) ) && ip[ 0 ] )
            {
                element[ 1 ].guidDataType = DPAID_INet;
                element[ 1 ].dwDataSize = strlen( ip ) + 1;
                element[ 1 ].lpData = ip;
                nelem = 2;
            }
            if ( DPL_CreateCompoundAddress( element, nelem, NULL, &addrSize, TRUE ) == DPERR_BUFFERTOOSMALL &&
                 (addr = calloc( 1, addrSize )) &&
                 DPL_CreateCompoundAddress( element, nelem, addr, &addrSize, TRUE ) == DP_OK )
            {
                test_descA.dwSize = sizeof(DPSESSIONDESC2);
                test_descA.dwFlags = DPSESSION_KEEPALIVE | DPSESSION_MIGRATEHOST;
                test_descA.guidApplication = AOE2TC_APP;
                /* another game: OKP_TEST_LOBBY_APP={guid} (its DirectPlay application GUID) */
                if ( GetEnvironmentVariableA( "OKP_TEST_LOBBY_APP", v, sizeof(v) ) )
                {
                    unsigned long d1;
                    unsigned int d2, d3, b[ 8 ], k;
                    if ( sscanf( v, "{%8lx-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}", &d1, &d2, &d3, &b[ 0 ], &b[ 1 ], &b[ 2 ],
                                 &b[ 3 ], &b[ 4 ], &b[ 5 ], &b[ 6 ], &b[ 7 ] ) == 11 )
                    {
                        test_descA.guidApplication.Data1 = d1;
                        test_descA.guidApplication.Data2 = (WORD)d2;
                        test_descA.guidApplication.Data3 = (WORD)d3;
                        for ( k = 0; k < 8; k++ ) test_descA.guidApplication.Data4[ k ] = (BYTE)b[ k ];
                    }
                    else okp_log( 0, "test_lobby: OKP_TEST_LOBBY_APP=%s is not a {GUID} - using Age of Empires II's", v );
                }
                /* a lobby launches a full room: AoE2 waits at "Setting up multiplayer game" until this
                 * many players are in (OKP_TEST_LOBBY_MAX, default 2 - the pair test) */
                test_descA.dwMaxPlayers = 2;
                if ( GetEnvironmentVariableA( "OKP_TEST_LOBBY_MAX", v, sizeof(v) ) && atoi( v ) >= 2 && atoi( v ) <= 8 )
                    test_descA.dwMaxPlayers = atoi( v );
                test_descW = test_descA;
                test_descA.lpszSessionNameA = test_sessA;
                test_descW.lpszSessionName = test_sessW;
                /* an open game, as a lobby passes it: an empty password, not NULL (AoE2 reads it
                 * back with GetSessionDesc after a lobby Connect and takes its length) */
                test_descA.lpszPasswordA = (char *)"";
                test_descW.lpszPassword = (WCHAR *)L"";
                test_nameA.dwSize = test_nameW.dwSize = sizeof(DPNAME);
                /* a long name too: AoE2 takes the length of both (a NULL long name crashed a real
                 * AoE2 host - tests/run_local.sh aoe2names) */
                test_nameA.lpszShortNameA = test_nameA.lpszLongNameA = test_playerA;
                test_nameW.lpszShortName = test_nameW.lpszLongName = test_playerW;

                test_connA.dwSize = sizeof(DPLCONNECTION);
                test_connA.dwFlags = host ? DPLCONNECTION_CREATESESSION : DPLCONNECTION_JOINSESSION;
                test_connA.guidSP = *spguid;
                test_connA.lpAddress = addr;
                test_connA.dwAddressSize = addrSize;
                test_connW = test_connA;
                test_connA.lpSessionDesc = &test_descA;
                test_connA.lpPlayerName = &test_nameA;
                test_connW.lpSessionDesc = &test_descW;
                test_connW.lpPlayerName = &test_nameW;
                state = 1;
                okp_log( 0, "test_lobby: OKP_TEST_LOBBY is set - the game is told a lobby launched it: %s \"%s\" "
                         "as player \"%s\" over %s (tests only)", host ? "host" : "join", test_sessA, test_playerA,
                         tcpip ? "Windows' TCP/IP provider (our DirectPlay)" : online ? "Online Play" : "LAN Play" );
            }
            else
            {
                free( addr );
                okp_log( 0, "test_lobby: could not build the address" );
            }
        }
        InterlockedCompareExchange( &test_lobby_state, state, 0 );
    }
    if ( test_lobby_state < 0 ) return NULL;
    return unicode ? &test_connW : &test_connA;
}

static void add_connection( struct list *connections, const GUID *guid, const char *name )
{
    DPCOMPOUNDADDRESSELEMENT element;
    DPCONNECTION *c;
    char path[ MAX_PATH ];
    int wlen;

    if ( !(c = calloc( 1, sizeof(*c) )) ) return;
    c->flags = DPCONNECTION_DIRECTPLAY;
    c->spGuid = *guid;
    c->name.dwSize = sizeof(DPNAME);
    c->nameA.dwSize = sizeof(DPNAME);
    wlen = MultiByteToWideChar( CP_ACP, 0, name, -1, NULL, 0 );
    c->name.lpszShortName = malloc( wlen * sizeof(WCHAR) );
    c->nameA.lpszShortNameA = strdup( name );
    if ( c->name.lpszShortName ) MultiByteToWideChar( CP_ACP, 0, name, -1, c->name.lpszShortName, wlen );
    c->reserved1 = 50;   /* what okplay's installer registers */
    c->reserved2 = 0;
    /* "loading" the provider loads this DLL again (a reference), which exports okplay's SPInit */
    GetModuleFileNameA( self, path, sizeof(path) );
    c->path = strdup( path );

    element.guidDataType = DPAID_ServiceProvider;
    element.dwDataSize = sizeof(GUID);
    element.lpData = &c->spGuid;
    if ( DPL_CreateCompoundAddress( &element, 1, NULL, &c->addressSize, TRUE ) == DPERR_BUFFERTOOSMALL &&
         (c->address = calloc( 1, c->addressSize )) &&
         DPL_CreateCompoundAddress( &element, 1, c->address, &c->addressSize, TRUE ) == DP_OK &&
         DP_AddAddressTerminator( &c->address, &c->addressSize ) &&
         c->name.lpszShortName && c->nameA.lpszShortNameA && c->path )
    {
        list_add_tail( connections, &c->entry );
        TRACE( "built-in connection %s = %s\n", name, debugstr_guid( guid ) );
        return;
    }
    ERR( "could not build the %s connection\n", name );
    free( c->address );
    free( c->path );
    free( c->nameA.lpszShortNameA );
    free( c->name.lpszShortName );
    free( c );
}

void DP_BUILTIN_AddConnections( struct list *connections )
{
    char online[ 128 ], lan[ 128 ];
    ini_str( "online_name", "Classic Netplay Online", online, sizeof(online) );
    ini_str( "lan_name", "Classic Netplay LAN", lan, sizeof(lan) );
    add_connection( connections, &OKP_SPGUID, online );
    add_connection( connections, &OKP_LANGUID, lan );
    okp_log( 2, "connections: %s, %s%s", online, lan,
             DP_BUILTIN_ShowWindowsConnections() ? " + Windows' own (show_windows_connections=1)" : "" );
}

/* ------------------------------------------------------------------ intro videos */

typedef LRESULT (CALLBACK *drvproc_t)( DWORD_PTR, HDRVR, UINT, LPARAM, LPARAM );
static drvproc_t codec_proc;
static volatile LONG frames_decoded, frames_failed;

/* between Video for Windows and okindeo.dll: counts frames, so the log shows the movies played */
static LRESULT CALLBACK codec_spy( DWORD_PTR id, HDRVR drv, UINT msg, LPARAM p1, LPARAM p2 )
{
    LRESULT r = codec_proc( id, drv, msg, p1, p2 );
    if ( msg == ICM_DECOMPRESS || msg == ICM_DECOMPRESSEX )
    {
        InterlockedIncrement( &frames_decoded );
        if ( r != ICERR_OK ) InterlockedIncrement( &frames_failed );
    }
    else if ( msg == ICM_DECOMPRESS_BEGIN && p1 && p2 )
    {
        const BITMAPINFOHEADER *in = (const BITMAPINFOHEADER *)p1, *out = (const BITMAPINFOHEADER *)p2;
        okp_log( 1, "video: Indeo 5 %ldx%ld playing -> %d-bit (%s)", in->biWidth, labs( in->biHeight ),
                 out->biBitCount, r == ICERR_OK ? "ok" : "refused" );
    }
    else if ( msg == ICM_DECOMPRESS_END )
    {
        okp_log( 1, "video: finished, %ld frames decoded so far (%ld failed)", frames_decoded, frames_failed );
    }
    return r;
}

static DWORD WINAPI video_setup( void *unused )
{
    char path[ MAX_PATH ];
    HIC existing;
    HMODULE m;

    if ( !ini_int( "videos", 1 ) )
    {
        okp_log( 2, "video: decoder disabled in okplay.ini (videos=0)" );
        return 0;
    }
    /* a system-wide Indeo 5 decoder (Intel's, or the installer's) works too - leave it alone */
    existing = ICOpen( ICTYPE_VIDEO, mmioFOURCC( 'I', 'V', '5', '0' ), ICMODE_DECOMPRESS );
    if ( existing )
    {
        ICClose( existing );
        okp_log( 2, "video: an Indeo 5 decoder is already installed on this PC - using it" );
        return 0;
    }
    if ( okp_win9x_paths() )
    {
        /* okindeo.dll (FFmpeg) needs Windows XP or later; Windows 9x ships Intel's Indeo 5 itself */
        okp_log( 1, "video: no Indeo 5 decoder on this Windows 9x (okindeo.dll needs XP+) - "
                    "reinstall the Indeo codec or DirectX Media for picture in the intro movies" );
        return 0;
    }
    snprintf( path, sizeof(path), "%sokindeo.dll", dir );
    if ( !(m = LoadLibraryA( path )) )
    {
        DWORD err = GetLastError();
        if ( GetFileAttributesA( path ) == INVALID_FILE_ATTRIBUTES )
            okp_log( 1, "video: okindeo.dll not found next to dplayx.dll - intro movies will have no picture" );
        else
            okp_log( 0, "video: okindeo.dll is there but would not load (Windows error %lu) - "
                        "intro movies will have no picture", err );
        return 0;
    }
    if ( !(codec_proc = (drvproc_t)GetProcAddress( m, "DriverProc" )) ||
         !ICInstall( ICTYPE_VIDEO, mmioFOURCC( 'I', 'V', '5', '0' ), (LPARAM)codec_spy, NULL, ICINSTALL_FUNCTION ) )
    {
        okp_log( 0, "video: could not register okindeo.dll with Video for Windows" );
        return 0;
    }
    okp_log( 1, "video: Indeo 5 decoder ready for this game (okindeo.dll, this process only)" );
    return 0;
}

/* ------------------------------------------------------------------ game passwords */

/* The boxes and the password scrambling live in okplay/src/okbox.c (shared with okplay wsock). */
#include "okbox.h"
static BOOL is_aoe2_exe( void );
#define hash_password okbox_hash_password
#define show_pw_dialog okbox_show
#define brand_w okbox_brand
#define test_answers okbox_test_answers

BOOL DP_BUILTIN_AskHostPassword( const GUID *instance, WCHAR *hashed, DWORD size )
{
    return okbox_ask_host_password( instance, hashed, size );
}

HRESULT DP_BUILTIN_AskJoinPassword( const GUID *instance, WCHAR *hashed, DWORD size )
{
    return okbox_ask_join_password( instance, hashed, size ) ? DP_OK : DPERR_USERCANCEL;
}

/* okplay/src/util.c and sp.c: the game exe's fingerprint, ours and the room host's */
extern void okp_exe_build( char build[ 17 ], char *exe, int exesz );
extern int okp_room_build( IDirectPlaySP *isp, const GUID *instance, char build[ 17 ], char *exe, int exesz );
extern int okp_utf8_to_utf16( const char *in, WCHAR *out, int outn );

HRESULT DP_BUILTIN_CheckBuild( IDirectPlaySP *isp, const GUID *instance )
{
    struct pw_dialog d;
    WCHAR title[ 96 ], text[ 600 ], theirs_w[ 64 ], mine_w[ 64 ];
    char mine[ 17 ], mine_exe[ 64 ], theirs[ 17 ], theirs_exe[ 64 ], mode[ 16 ];
    const char *test = test_answers() ? getenv( "OKP_TEST_BUILD_ANSWER" ) : NULL;
    INT_PTR r;

    ini_str( "build_check", "ask", mode, sizeof(mode) );
    if ( !_stricmp( mode, "off" ) ) return DP_OK;
    if ( !okp_room_build( isp, instance, theirs, theirs_exe, sizeof(theirs_exe) ) ) return DP_OK;   /* host < 0.8.1 */
    okp_exe_build( mine, mine_exe, sizeof(mine_exe) );
    if ( !mine[ 0 ] || !strcmp( mine, theirs ) )
    {
        okp_log( 2, "build: the host runs the same build of the game (%s)", mine );
        return DP_OK;
    }
    okp_log( 1, "build: the host runs another build of the game - theirs %s %s, ours %s %s", theirs_exe, theirs,
             mine_exe, mine );
    if ( test )   /* tests: "join" or "cancel" */
        r = !_stricmp( test, "join" ) ? IDOK : IDCANCEL;
    else
    {
        MultiByteToWideChar( CP_UTF8, 0, theirs_exe, -1, theirs_w, ARRAY_SIZE( theirs_w ) );
        MultiByteToWideChar( CP_UTF8, 0, mine_exe, -1, mine_w, ARRAY_SIZE( mine_w ) );
        _snwprintf( text, ARRAY_SIZE( text ) - 1,
                  L"The host is running a different version of the game.\n\n"
                  L"Theirs: %ls (build %.8hs)\nYours: %ls (build %.8hs)\n\n"
                  L"Joining will probably fail, or the game will go out of sync. Join anyway?",
                  theirs_w[ 0 ] ? theirs_w : L"?", theirs, mine_w[ 0 ] ? mine_w : L"?", mine );
        text[ ARRAY_SIZE( text ) - 1 ] = 0;
        memset( &d, 0, sizeof(d) );
        brand_w( title, ARRAY_SIZE( title ) );
        d.title = title;
        d.text = text;
        d.text_cy = 64;
        d.ok = L"Join anyway";
        d.cancel = L"Cancel";
        r = show_pw_dialog( &d );
    }
    if ( r != IDOK )
    {
        okp_log( 1, "build: the player cancelled the join" );
        return DPERR_USERCANCEL;
    }
    okp_log( 1, "build: joining anyway" );
    return DP_OK;
}

void DP_BUILTIN_JoinFailed( HRESULT hr )
{
    if ( hr == DPERR_INVALIDPASSWORD ) okbox_wrong_password();
}

/* ------------------------------------------------------------------ fixed connection lists */

/* Some games never enumerate providers: their Multiplayer screen is a fixed list (Zone, IPX, TCP/IP,
 * Serial, Modem) and they ask for each by its GUID (Midtown Madness does). Our connections can't
 * appear there, so okplay.ini can hand any of those names to ours: classic_tcpip, classic_ipx,
 * classic_serial, classic_modem = online|lan|ask (default: off, the classic entries stay Windows'; ask = a box
 * offers Online Play / LAN Play / Windows' own the first time the game picks that entry). Applies to
 * DirectPlayCreate(GUID) and to InitializeConnection with that provider's address.
 * TRUE = *sp was replaced by ours; with address/size, a compound address for it is returned (calloc). */
/* classic_*=ask: the box. 1 = Windows' own, 2 = Online Play, 3 = LAN Play; asked once per entry per
 * game run (the game creates its DirectPlay object more than once). Tests: OKP_TEST_CLASSIC_ASK=windows|online|lan */
static int ask_classic( const char *what )
{
    return okbox_ask_mode( what );
}

static int asked[ 4 ];   /* classic_*=ask: the answer per entry (TCP/IP, IPX, Serial, Modem), once per game run */

static int classic_index( const GUID *sp, const char **key, const char **what )
{
    if ( IsEqualGUID( sp, &DPSPGUID_TCPIP ) ) { *key = "classic_tcpip"; *what = "TCP/IP"; return 0; }
    if ( IsEqualGUID( sp, &DPSPGUID_IPX ) ) { *key = "classic_ipx"; *what = "IPX"; return 1; }
    if ( IsEqualGUID( sp, &DPSPGUID_SERIAL ) ) { *key = "classic_serial"; *what = "Serial"; return 2; }
    if ( IsEqualGUID( sp, &DPSPGUID_MODEM ) ) { *key = "classic_modem"; *what = "Modem"; return 3; }
    return -1;
}

/* what okplay.ini says about a classic provider: 0 = Windows' own (or not a classic one), 1 = Online
 * Play, 2 = LAN Play, 3 = ask - and not asked yet this run (once answered, the answer: 0/1/2) */
int DP_BUILTIN_RemapMode( const GUID *sp )
{
    const char *key, *what;
    char v[ 16 ];
    int idx = classic_index( sp, &key, &what );
    if ( idx < 0 ) return 0;
    ini_str( key, "", v, sizeof(v) );
    if ( !_stricmp( v, "online" ) ) return 1;
    if ( !_stricmp( v, "lan" ) ) return 2;
    if ( !_stricmp( v, "ask" ) ) return asked[ idx ] ? asked[ idx ] - 1 : 3;
    return 0;
}

BOOL DP_BUILTIN_IsLan( const GUID *sp ) { return IsEqualGUID( sp, &OKP_LANGUID ); }

/* the game let go of the DirectPlay object the answer was given for (it went back to its connection
 * menu): next time it picks that entry, ask again */
void DP_BUILTIN_RemapForget( const GUID *sp )
{
    const char *key, *what;
    int idx = classic_index( sp, &key, &what );
    if ( idx >= 0 && asked[ idx ] )
    {
        asked[ idx ] = 0;
        okp_log( 2, "connections: the game released its %s connection - it will be asked again next time", what );
    }
}

/* a DirectPlay compound address naming a provider (calloc, at least *size bytes; *size = its size) */
BOOL DP_BUILTIN_Address( const GUID *sp, void **address, DWORD *size )
{
    DPCOMPOUNDADDRESSELEMENT element;
    DWORD n = 0;
    element.guidDataType = DPAID_ServiceProvider;
    element.dwDataSize = sizeof(GUID);
    element.lpData = (void *)sp;
    *address = NULL;
    if ( DPL_CreateCompoundAddress( &element, 1, NULL, &n, TRUE ) == DPERR_BUFFERTOOSMALL &&
         (*address = calloc( 1, n < *size ? *size : n )) &&
         DPL_CreateCompoundAddress( &element, 1, *address, &n, TRUE ) == DP_OK )
    {
        if ( n > *size ) *size = n;
        return TRUE;
    }
    free( *address );
    *address = NULL;
    return FALSE;
}

BOOL DP_BUILTIN_OurAddress( BOOL lan, void **address, DWORD *size )
{
    return DP_BUILTIN_Address( lan ? &OKP_LANGUID : &OKP_SPGUID, address, size );
}

BOOL DP_BUILTIN_Remap( GUID *sp, void **address, DWORD *size )
{
    const char *key, *what;
    const GUID *to;
    char v[ 16 ];
    int idx = classic_index( sp, &key, &what );

    if ( idx < 0 ) return FALSE;
    ini_str( key, "", v, sizeof(v) );
    if ( !_stricmp( v, "online" ) ) to = &OKP_SPGUID;
    else if ( !_stricmp( v, "lan" ) ) to = &OKP_LANGUID;
    else if ( !_stricmp( v, "ask" ) )
    {
        if ( !asked[ idx ] )
        {
            asked[ idx ] = ask_classic( what );
            okp_log( 1, "connections: %s=ask - the player chose %s for the game's %s entry", key,
                     asked[ idx ] == 2 ? "Online Play" : asked[ idx ] == 3 ? "LAN Play" : "the original (Windows' DirectPlay)", what );
        }
        if ( asked[ idx ] == 1 ) return FALSE;
        to = asked[ idx ] == 2 ? &OKP_SPGUID : &OKP_LANGUID;
    }
    else return FALSE;
    okp_log( 1, "connections: the game asked for its %s entry by GUID - okplay.ini %s=%s: it runs on %s Play",
             what, key, v, to == &OKP_SPGUID ? "Online" : "LAN" );
    *sp = *to;
    if ( address && !DP_BUILTIN_Address( sp, address, size ) ) *address = NULL;
    return TRUE;
}

/* ------------------------------------------------------------------ no TCP/IP */

/* okplay couldn't create any internet socket: TCP/IP isn't installed (or not bound to a network card).
 * Without this box the game just reports a failed session and drops out. Once per game run. */
static DWORD WINAPI no_network_thread( void *param )
{
    struct pw_dialog d;
    WCHAR title[ 96 ];
    memset( &d, 0, sizeof(d) );
    brand_w( title, ARRAY_SIZE( title ) );
    d.title = title;
    d.text = okp_win9x_paths()
        ? L"This PC's TCP/IP networking isn't working, so Online and LAN Play can't start.\n\n"
          L"In Control Panel > Network, TCP/IP must be listed and bound to your network card "
          L"(Add > Protocol > Microsoft > TCP/IP), then restart."
        : L"This PC's TCP/IP networking isn't working, so Online and LAN Play can't start.\n\n"
          L"Check that the network adapter is enabled and has IPv4 turned on.";
    d.text_cy = 64;
    d.ok = L"OK";
    show_pw_dialog( &d );
    return 0;
}

void DP_BUILTIN_NetworkUnavailable( int err )
{
    static LONG shown;
    HANDLE t;
    okp_log( 0, "network: no internet socket can be created (Winsock error %d) - TCP/IP isn't installed or "
                "isn't bound to a network card; Online and LAN Play can't start", err );
    if ( InterlockedExchange( &shown, 1 ) ||
         (test_answers() && (getenv( "OKP_TEST_JOIN_PASSWORD" ) || getenv( "OKP_TEST_HOST_PASSWORD" ))) )
        return;   /* tests: log only */
    if ( !test_answers() ) { no_network_thread( NULL ); return; }   /* OKP_PREVIEW_BOXES: right here */
    /* its own thread: the game gets its error straight away and shows its own message too */
    if ( (t = CreateThread( NULL, 0, no_network_thread, NULL, 0, NULL )) ) CloseHandle( t );
}

/* ------------------------------------------------------------------ server unreachable */

/* Online Play's room server (okplay.php) failed 3 times in a row: say so, with the reason, instead of
 * an empty game list that looks like nobody is playing. Once per game run (rdv.c decides when). */
struct unreachable { WCHAR text[ 600 ]; };

static DWORD WINAPI unreachable_thread( void *param )
{
    struct unreachable *u = param;
    struct pw_dialog d;
    WCHAR title[ 96 ];
    memset( &d, 0, sizeof(d) );
    brand_w( title, ARRAY_SIZE( title ) );
    d.title = title;
    d.text = u->text;
    d.text_cy = 78;
    d.ok = L"OK";
    show_pw_dialog( &d );
    free( u );
    return 0;
}

void DP_BUILTIN_ServerUnreachable( const char *server, const char *why )
{
    struct unreachable *u;
    char host[ 128 ] = "";
    const char *h = strstr( server, "://" );
    WCHAR whost[ 128 ], wwhy[ 200 ];
    HANDLE t;
    int i;

    if ( test_answers() && (getenv( "OKP_TEST_JOIN_PASSWORD" ) || getenv( "OKP_TEST_HOST_PASSWORD" )) )
        return;   /* tests: log only (rdv.c already logged it) */
    h = h ? h + 3 : server;
    for ( i = 0; h[ i ] && h[ i ] != '/' && h[ i ] != ':' && i < (int)sizeof(host) - 1; i++ ) host[ i ] = h[ i ];
    host[ i ] = 0;
    MultiByteToWideChar( CP_ACP, 0, host, -1, whost, ARRAY_SIZE( whost ) );
    MultiByteToWideChar( CP_ACP, 0, why, -1, wwhy, ARRAY_SIZE( wwhy ) );
    if ( !(u = calloc( 1, sizeof(*u) )) ) return;
    _snwprintf( u->text, ARRAY_SIZE( u->text ) - 1, L"Can't reach the Online Play server (%ls).\n\n%ls.\n\n"
                L"Online games can't be listed or hosted until it's reachable. LAN Play still works.", whost, wwhy );
    if ( !test_answers() ) { unreachable_thread( u ); return; }   /* OKP_PREVIEW_BOXES: right here */
    if ( (t = CreateThread( NULL, 0, unreachable_thread, u, 0, NULL )) ) CloseHandle( t );
    else free( u );
}

/* ------------------------------------------------------------------ update notice */

/* The room server's replies carry "latest <version> <note>" (okplay.php LATEST_VERSION/LATEST_NOTE).
 * When it is newer than this DLL, one box per game run offers the download - never blocking:
 * games, hosting and joining go on as before. okplay.ini: update_check=0 turns it off;
 * update_seen=<version> is written by "Don't remind me about this version". The Download button
 * opens the server's own folder (from server=), never an address the server sends.
 * Tests: OKP_TEST_UPDATE=download|later|never answers without a box (and opens nothing). */

static int version_cmp( const char *a, const char *b )   /* "0.7.10" > "0.7.9" */
{
    while ( *a || *b )
    {
        long x = strtol( a, (char **)&a, 10 ), y = strtol( b, (char **)&b, 10 );
        if ( x != y ) return x < y ? -1 : 1;
        if ( *a == '.' ) a++;
        if ( *b == '.' ) b++;
        if ( (*a && (*a < '0' || *a > '9')) || (*b && (*b < '0' || *b > '9')) ) break;
    }
    return 0;
}

/* UTF-8 -> WCHAR without the system (Windows 95 has no CP_UTF8); control characters become spaces */
static void utf8_to_wide( const char *in, WCHAR *out, int outsz )
{
    const unsigned char *s = (const unsigned char *)in;
    int o = 0;
    while ( *s && o < outsz - 1 )
    {
        unsigned int c = *s++;
        int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        if ( more ) c &= 0x3f >> more;
        while ( more-- > 0 && (*s & 0xc0) == 0x80 ) c = (c << 6) | (*s++ & 0x3f);
        if ( c < 0x20 ) c = ' ';
        if ( c > 0xffff )
        {
            if ( o + 2 >= outsz ) break;
            c -= 0x10000;
            out[ o++ ] = (WCHAR)(0xd800 + (c >> 10));
            c = 0xdc00 + (c & 0x3ff);
        }
        out[ o++ ] = (WCHAR)c;
    }
    out[ o ] = 0;
}

struct update_offer { char latest[ 24 ]; char note[ 256 ]; };

static DWORD WINAPI update_box_thread( void *param )
{
    struct update_offer *u = param;
    struct pw_dialog d;
    WCHAR title[ 96 ], text[ 600 ], note[ 200 ], latest[ 24 ], current[ 24 ];
    const char *test = getenv( "OKP_TEST_UPDATE" );
    char url[ 300 ], *slash;
    int download;

    memset( &d, 0, sizeof(d) );
    utf8_to_wide( u->note, note, ARRAY_SIZE( note ) );
    MultiByteToWideChar( CP_ACP, 0, u->latest, -1, latest, ARRAY_SIZE( latest ) );
    MultiByteToWideChar( CP_ACP, 0, OKP_VERSION_STR, -1, current, ARRAY_SIZE( current ) );
    if ( note[ 0 ] )
        _snwprintf( text, ARRAY_SIZE( text ), L"A newer version is available: v%ls\n(you have v%ls)\n\n%ls\n\n"
                    L"You can keep playing. If you run into connection problems, please update.",
                    latest, current, note );
    else
        _snwprintf( text, ARRAY_SIZE( text ), L"A newer version is available: v%ls\n(you have v%ls)\n\n"
                    L"You can keep playing. If you run into connection problems, please update.", latest, current );
    text[ ARRAY_SIZE( text ) - 1 ] = 0;

    if ( test && *test )
    {
        download = !strcmp( test, "download" );
        d.checked = !strcmp( test, "never" );
    }
    else
    {
        brand_w( title, ARRAY_SIZE( title ) );
        d.title = title;
        d.text = text;
        d.text_cy = note[ 0 ] ? 78 : 56;
        d.check = L"Don't remind me about this version";
        d.ok = L"Download";
        d.cancel = L"Not Now";
        download = show_pw_dialog( &d ) == IDOK;
    }
    if ( d.checked )
    {
        WritePrivateProfileStringA( "okplay", "update_seen", u->latest, ini );
        okp_log( 1, "update: v%s - the player chose not to be reminded (okplay.ini update_seen)", u->latest );
    }
    /* the download page = the room server's own folder (https://oldkid.net/aoe2net/ for okplay.php);
     * Windows 9x browsers can't do today's HTTPS, so they get http:// */
    ini_str( "server", "https://oldkid.net/aoe2net/okplay.php", url, sizeof(url) - 8 );
    if ( (slash = strrchr( url, '/' )) && slash > strstr( url, "://" ) + 2 ) slash[ 1 ] = 0;
    if ( okp_win9x_paths() && !_strnicmp( url, "https://", 8 ) ) memmove( url + 4, url + 5, strlen( url + 5 ) + 1 );
    if ( download )
    {
        okp_log( 1, "update: v%s - the player chose Download (%s)", u->latest, url );
        if ( !(test && *test) )
        {
            HMODULE shell = LoadLibraryA( "shell32.dll" );
            HINSTANCE (WINAPI *open)( HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT ) =
                shell ? (void *)GetProcAddress( shell, "ShellExecuteA" ) : NULL;
            if ( !open || (INT_PTR)open( NULL, "open", url, NULL, NULL, SW_SHOWNORMAL ) <= 32 )
                okp_log( 0, "update: could not open %s in the browser", url );
        }
    }
    else if ( !d.checked )
        okp_log( 1, "update: v%s - the player chose Not Now", u->latest );
    free( u );
    return 0;
}

/* from okplay's room-server client (any thread): offer the update once per game run */
void DP_BUILTIN_OfferUpdate( const char *latest, const char *note_utf8 )
{
    static LONG offered;
    struct update_offer *u;
    char seen[ 24 ];
    HANDLE t;

    if ( !latest || !*latest || version_cmp( latest, OKP_VERSION_STR ) <= 0 ) return;
    if ( InterlockedExchange( &offered, 1 ) ) return;
    if ( !ini_int( "update_check", 1 ) )
    {
        okp_log( 1, "update: v%s is available (you have v%s); not asking (update_check=0)", latest, OKP_VERSION_STR );
        return;
    }
    ini_str( "update_seen", "", seen, sizeof(seen) );
    if ( seen[ 0 ] && version_cmp( latest, seen ) <= 0 )
    {
        okp_log( 1, "update: v%s is available (you have v%s); not asking (update_seen=%s)", latest, OKP_VERSION_STR, seen );
        return;
    }
    okp_log( 1, "update: v%s is available (you have v%s) - offering it", latest, OKP_VERSION_STR );
    if ( !(u = calloc( 1, sizeof(*u) )) ) return;
    lstrcpynA( u->latest, latest, sizeof(u->latest) );
    lstrcpynA( u->note, note_utf8 ? note_utf8 : "", sizeof(u->note) );
    if ( !test_answers() ) { update_box_thread( u ); return; }   /* OKP_PREVIEW_BOXES: right here */
    /* its own thread: the room-server client (and the game) carry on while the box is up */
    if ( (t = CreateThread( NULL, 0, update_box_thread, u, 0, NULL )) ) CloseHandle( t );
    else free( u );
}

/* ------------------------------------------------------------------ reports (okplay/src/report.c) */

/* A crash report from the last run is waiting: ask before sending it. "View report" opens it in
 * Notepad and asks again. Runs on report.c's own thread, never over a match (asked at SPInit).
 * 0 = don't send (the report is deleted), 1 = send, 2 = send, and from now on without asking. */
int DP_BUILTIN_AskCrashReport( const char *path )
{
    struct pw_dialog d;
    WCHAR title[ 96 ];
    INT_PTR r;

    for (;;)
    {
        memset( &d, 0, sizeof(d) );
        brand_w( title, ARRAY_SIZE( title ) );
        d.title = title;
        d.text = L"The game closed unexpectedly last time.\n\n"
                 L"Send a crash report to oldkid.net to help fix it? It says where the crash happened and "
                 L"which versions of the game, Windows and this add-on you have. "
                 L"It never contains player names, chat or addresses.";
        d.text_cy = 78;
        d.check = L"Always send crash reports without asking";
        d.ok = L"Send";
        d.third = L"View Report";
        d.cancel = L"Don't Send";
        r = show_pw_dialog( &d );
        if ( r != PW_THIRD ) break;
        {
            char cmd[ MAX_PATH + 32 ];
            STARTUPINFOA si;
            PROCESS_INFORMATION pi;
            memset( &si, 0, sizeof(si) );
            si.cb = sizeof(si);
            snprintf( cmd, sizeof(cmd), "notepad.exe \"%s\"", path );
            if ( CreateProcessA( NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi ) )
            {
                WaitForSingleObject( pi.hProcess, INFINITE );   /* ask again once it's closed */
                CloseHandle( pi.hThread );
                CloseHandle( pi.hProcess );
            }
        }
    }
    okp_log( 1, "report: crash report box - the player chose %s%s", r == IDOK ? "Send" : "Don't Send",
             r == IDOK && d.checked ? " (always)" : "" );
    return r == IDOK ? (d.checked ? 2 : 1) : 0;
}

/* The first game is over and connection reports were never asked about (connection_reports=ask):
 * ask once. 1 = yes (report.c writes connection_reports=yes), 0 = no. */
int DP_BUILTIN_AskConnReports( void )
{
    struct pw_dialog d;
    WCHAR title[ 96 ];
    INT_PTR r;

    memset( &d, 0, sizeof(d) );
    brand_w( title, ARRAY_SIZE( title ) );
    d.title = title;
    d.text = L"Help improve Online Play?\n\n"
             L"After each game, send oldkid.net a short summary of how well it connected: whether joining "
             L"worked, the ping, and how many messages had to be resent. "
             L"No player names, chat or addresses. You can change this later in okplay.ini (connection_reports=).";
    d.text_cy = 86;
    d.ok = L"Yes, Send";
    d.cancel = L"No Thanks";
    r = show_pw_dialog( &d );
    okp_log( 1, "report: connection reports box - the player chose %s", r == IDOK ? "Yes" : "No" );
    return r == IDOK;
}

/* ------------------------------------------------------------------ intro=0 */

/* AoE2 skips its intro movies when its command line says NOSTARTUP. With intro=0 the game's own
 * GetCommandLineA (its import, patched here before any game code runs) answers with the real
 * command line plus " NOSTARTUP". Nothing on disk changes; intro=1 (or no okplay.ini) = as before. */
static char *nostartup_cmdline;

static LPSTR WINAPI cmdline_with_nostartup( void )
{
    return nostartup_cmdline;
}

static BOOL has_word_nocase( const char *s, const char *word )
{
    size_t n = strlen( word );
    for ( ; *s; s++ ) if ( !_strnicmp( s, word, n ) ) return TRUE;
    return FALSE;
}

static BOOL is_aoe2_exe( void )
{
    char exe[ MAX_PATH ], *base;
    GetModuleFileNameA( NULL, exe, sizeof(exe) );
    base = strrchr( exe, '\\' );
    base = base ? base + 1 : exe;
    return !_strnicmp( base, "age2", 4 ) || !_strnicmp( base, "empires2", 8 );
}

/* the game exe's import `func` from `dll` is pointed at `mine`; returns what it pointed at before
 * (NULL = not imported). By name, not by value: a Windows compatibility shim may already sit in
 * the slot (DWM8And16BitMitigation hooks GetDeviceCaps that way), and then it is the one we call on. */
static FARPROC patch_exe_import( const char *dll, const char *func, FARPROC mine )
{
    BYTE *exe = (BYTE *)GetModuleHandleA( NULL );
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[ IMAGE_DIRECTORY_ENTRY_IMPORT ];
    IMAGE_IMPORT_DESCRIPTOR *imp;
    FARPROC was = NULL;

    if ( !dir->VirtualAddress ) return NULL;
    for ( imp = (IMAGE_IMPORT_DESCRIPTOR *)(exe + dir->VirtualAddress); imp->Name; imp++ )
    {
        IMAGE_THUNK_DATA *names, *t;
        if ( _stricmp( (const char *)(exe + imp->Name), dll ) ) continue;
        names = (IMAGE_THUNK_DATA *)(exe + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        for ( t = (IMAGE_THUNK_DATA *)(exe + imp->FirstThunk); t->u1.Function && names->u1.AddressOfData; t++, names++ )
        {
            DWORD old;
            if ( IMAGE_SNAP_BY_ORDINAL( names->u1.Ordinal ) ) continue;
            if ( strcmp( (const char *)((IMAGE_IMPORT_BY_NAME *)(exe + names->u1.AddressOfData))->Name, func ) ) continue;
            if ( !VirtualProtect( &t->u1.Function, sizeof(t->u1.Function), PAGE_READWRITE, &old ) ) continue;
            was = (FARPROC)t->u1.Function;
            t->u1.Function = (ULONG_PTR)mine;
            VirtualProtect( &t->u1.Function, sizeof(t->u1.Function), old, &old );
        }
    }
    return was;
}

static void skip_intro_movies( void )
{
    const char *orig = GetCommandLineA();

    if ( !is_aoe2_exe() )
    {
        okp_log( 2, "intro: intro=0 is only for Age of Empires II - leaving this program's command line alone" );
        return;
    }
    if ( has_word_nocase( orig, "NOSTARTUP" ) )
    {
        okp_log( 2, "intro: the command line already says NOSTARTUP - intro movies skipped" );
        return;
    }
    if ( !(nostartup_cmdline = malloc( strlen( orig ) + sizeof(" NOSTARTUP") )) ) return;
    strcpy( nostartup_cmdline, orig );
    strcat( nostartup_cmdline, " NOSTARTUP" );
    if ( patch_exe_import( "kernel32.dll", "GetCommandLineA", (FARPROC)cmdline_with_nostartup ) )
        okp_log( 1, "intro: intro movies skipped (intro=0 in okplay.ini: the game sees NOSTARTUP)" );
    else
        okp_log( 0, "intro: intro=0, but the game's GetCommandLineA import was not found - movies will play" );
}

/* ------------------------------------------------------------------ com_redirect=1 */

/* Many games create DirectPlay with CoCreateInstance(CLSID_DirectPlay / CLSID_DirectPlayLobby)
 * instead of the DirectPlayCreate import. COM loads the server named in the registry - Windows'
 * dplayx.dll - so such a game would run on Windows' DirectPlay no matter which dplayx.dll sits in
 * its folder (a .local file does not redirect COM activation). com_redirect=1 (default) points the
 * game's CoCreateInstance import at us: the two DirectPlay classes come from this DLL (through the
 * same mode switch as the exports), every other class goes to the real CoCreateInstance. */
extern HRESULT WINAPI MODE_DllGetClassObject( REFCLSID rclsid, REFIID riid, void **out );
static HRESULT (WINAPI *real_cocreate)( REFCLSID, IUnknown *, DWORD, REFIID, void ** );

static HRESULT WINAPI our_cocreate( REFCLSID rclsid, IUnknown *outer, DWORD ctx, REFIID riid, void **out )
{
    IClassFactory *cf;
    HRESULT hr;

    if ( !rclsid || (!IsEqualGUID( rclsid, &CLSID_DirectPlay ) && !IsEqualGUID( rclsid, &CLSID_DirectPlayLobby )) )
        return real_cocreate( rclsid, outer, ctx, riid, out );
    if ( !out ) return E_POINTER;
    *out = NULL;
    if ( outer ) return CLASS_E_NOAGGREGATION;
    hr = MODE_DllGetClassObject( rclsid, &IID_IClassFactory, (void **)&cf );
    if ( SUCCEEDED( hr ) )
    {
        hr = cf->lpVtbl->CreateInstance( cf, NULL, riid, out );
        cf->lpVtbl->Release( cf );
    }
    okp_log( 1, "com: the game created %s with CoCreateInstance - served by this DLL (0x%08lx)",
             IsEqualGUID( rclsid, &CLSID_DirectPlay ) ? "DirectPlay" : "DirectPlayLobby", hr );
    return hr;
}

static void com_redirect( void )
{
    real_cocreate = (void *)patch_exe_import( "ole32.dll", "CoCreateInstance", (FARPROC)our_cocreate );
    if ( real_cocreate )
        okp_log( 2, "com: CoCreateInstance(DirectPlay*) in the game comes to this DLL (com_redirect=1)" );
}

/* ------------------------------------------------------------------ keep_desktop_mode=1 */

/* Some games switch the whole desktop to their resolution with ChangeDisplaySettings (outside
 * DirectDraw, so a wrapper such as dgVoodoo can't stop it) and leave it there if they crash.
 * keep_desktop_mode=1 answers those calls with "done" without changing anything; the wrapper then
 * scales the game's picture to the desktop. Default 0. */
static DWORD want_w, want_h, want_bpp;   /* the mode the game asked for; 0 = none yet */
static int (WINAPI *real_gsm)( int );
static int (WINAPI *real_gdc)( HDC, int );

static void remember_mode( DWORD fields, DWORD w, DWORD h, DWORD bpp )
{
    if ( !fields ) { want_w = want_h = want_bpp = 0; return; }   /* NULL devmode = back to the desktop's own */
    if ( fields & DM_PELSWIDTH ) want_w = w;
    if ( fields & DM_PELSHEIGHT ) want_h = h;
    if ( fields & DM_BITSPERPEL ) want_bpp = bpp;
    okp_log( 1, "display: the game asked for %lux%lu %lu-bit - told it \"done\", the desktop stays as it is", want_w, want_h, want_bpp );
}
static LONG WINAPI cds_a( DEVMODEA *dm, DWORD flags )
{ remember_mode( dm ? dm->dmFields : 0, dm ? dm->dmPelsWidth : 0, dm ? dm->dmPelsHeight : 0, dm ? dm->dmBitsPerPel : 0 ); return DISP_CHANGE_SUCCESSFUL; }
static LONG WINAPI cds_w( DEVMODEW *dm, DWORD flags )
{ remember_mode( dm ? dm->dmFields : 0, dm ? dm->dmPelsWidth : 0, dm ? dm->dmPelsHeight : 0, dm ? dm->dmBitsPerPel : 0 ); return DISP_CHANGE_SUCCESSFUL; }
static LONG WINAPI cds_ex_a( LPCSTR dev, DEVMODEA *dm, HWND wnd, DWORD flags, void *p ) { return cds_a( dm, flags ); }
static LONG WINAPI cds_ex_w( LPCWSTR dev, DEVMODEW *dm, HWND wnd, DWORD flags, void *p ) { return cds_w( dm, flags ); }
/* what the game then measures: the mode it asked for */
static int WINAPI gsm( int index )
{
    if ( index == SM_CXSCREEN && want_w ) return want_w;
    if ( index == SM_CYSCREEN && want_h ) return want_h;
    return real_gsm( index );
}
static int WINAPI gdc( HDC dc, int index )
{
    if ( index == HORZRES && want_w ) return want_w;
    if ( index == VERTRES && want_h ) return want_h;
    if ( index == BITSPIXEL && want_bpp ) return want_bpp;
    return real_gdc( dc, index );
}

static void keep_desktop_mode( void )
{
    int n = 0;
    n += patch_exe_import( "user32.dll", "ChangeDisplaySettingsA", (FARPROC)cds_a ) != NULL;
    n += patch_exe_import( "user32.dll", "ChangeDisplaySettingsW", (FARPROC)cds_w ) != NULL;
    n += patch_exe_import( "user32.dll", "ChangeDisplaySettingsExA", (FARPROC)cds_ex_a ) != NULL;
    n += patch_exe_import( "user32.dll", "ChangeDisplaySettingsExW", (FARPROC)cds_ex_w ) != NULL;
    if ( !n ) { okp_log( 1, "display: keep_desktop_mode=1, but the game imports no ChangeDisplaySettings - nothing to do" ); return; }
    real_gsm = (void *)patch_exe_import( "user32.dll", "GetSystemMetrics", (FARPROC)gsm );
    real_gdc = (void *)patch_exe_import( "gdi32.dll", "GetDeviceCaps", (FARPROC)gdc );
    if ( !real_gsm ) real_gsm = (void *)GetProcAddress( GetModuleHandleA( "user32.dll" ), "GetSystemMetrics" );
    if ( !real_gdc ) real_gdc = (void *)GetProcAddress( GetModuleHandleA( "gdi32.dll" ), "GetDeviceCaps" );
    okp_log( 1, "display: keep_desktop_mode=1 - %d display-mode import(s) answer \"done\" without changing the desktop; "
                "GetSystemMetrics/GetDeviceCaps report the mode the game asked for", n );
}

/* from DllMain (process detach) */
void DP_BUILTIN_Detach( void )
{
    /* a game that switched the desktop mode itself and is now dying would leave it switched;
     * asking for the registry default is a no-op when nothing was changed */
    ChangeDisplaySettingsA( NULL, 0 );
    __wine_dbg_census( "game exit" );
    okp_log( 1, "video: %ld intro frames decoded this session (%ld failed)", frames_decoded, frames_failed );
}

/* ------------------------------------------------------------------ OKP_TEST_MULTI=1 (tests only) */

/* The real-game pair test runs two copies of the game on one PC; AoE2 quits when it finds another
 * copy running. Environment variable only - never an okplay.ini setting, so players can't stumble
 * on it. Every named kernel object the game creates gets this process's id appended (each copy its
 * own), FindWindowA does not see other processes' windows, and each call is logged. */
static HANDLE (WINAPI *real_create_mutex)( SECURITY_ATTRIBUTES *, BOOL, const char * );
static HANDLE (WINAPI *real_create_semaphore)( SECURITY_ATTRIBUTES *, LONG, LONG, const char * );
static HANDLE (WINAPI *real_create_mapping)( HANDLE, SECURITY_ATTRIBUTES *, DWORD, DWORD, DWORD, const char * );
static HWND (WINAPI *real_find_window)( const char *, const char * );

static const char *multi_name( const char *name, char *buf, size_t size )
{
    if ( !name || !*name ) return name;
    snprintf( buf, size, "%s-okptest%lu", name, GetCurrentProcessId() );
    return buf;
}

static HANDLE WINAPI multi_create_mutex( SECURITY_ATTRIBUTES *sa, BOOL owner, const char *name )
{
    char buf[ MAX_PATH + 32 ];
    HANDLE h = real_create_mutex( sa, owner, multi_name( name, buf, sizeof(buf) ) );
    DWORD err = GetLastError();
    okp_log( 1, "test_multi: CreateMutexA(\"%s\") -> %p%s", name ? name : "(unnamed)", h,
             err == ERROR_ALREADY_EXISTS ? " (already existed)" : "" );
    SetLastError( err );
    return h;
}

static HANDLE WINAPI multi_create_semaphore( SECURITY_ATTRIBUTES *sa, LONG init, LONG max, const char *name )
{
    char buf[ MAX_PATH + 32 ];
    HANDLE h = real_create_semaphore( sa, init, max, multi_name( name, buf, sizeof(buf) ) );
    DWORD err = GetLastError();
    if ( name ) okp_log( 1, "test_multi: CreateSemaphoreA(\"%s\") -> %p%s", name, h,
                         err == ERROR_ALREADY_EXISTS ? " (already existed)" : "" );
    SetLastError( err );
    return h;
}

static HANDLE WINAPI multi_create_mapping( HANDLE file, SECURITY_ATTRIBUTES *sa, DWORD prot, DWORD hi, DWORD lo,
                                           const char *name )
{
    char buf[ MAX_PATH + 32 ];
    HANDLE h = real_create_mapping( file, sa, prot, hi, lo, multi_name( name, buf, sizeof(buf) ) );
    DWORD err = GetLastError();
    if ( name ) okp_log( 1, "test_multi: CreateFileMappingA(\"%s\") -> %p%s", name, h,
                         err == ERROR_ALREADY_EXISTS ? " (already existed)" : "" );
    SetLastError( err );
    return h;
}

static HWND WINAPI multi_find_window( const char *cls, const char *title )
{
    HWND w = real_find_window( cls, title );
    DWORD pid = 0;
    if ( w ) GetWindowThreadProcessId( w, &pid );
    okp_log( 1, "test_multi: FindWindowA(\"%s\", \"%s\") -> %p%s", HIWORD( cls ) ? cls : "(atom)",
             title ? title : "(any)", w, w && pid != GetCurrentProcessId() ? " - another process: hidden" : "" );
    return w && pid != GetCurrentProcessId() ? NULL : w;
}

/* Two copies side by side: only one can be the foreground window, and the game ignores the mouse
 * when it is not. Each copy is told its own window is the active one (clicks still only reach the
 * window under the cursor). */
static HWND (WINAPI *real_get_foreground)( void );
static HWND (WINAPI *real_get_active)( void );
static HWND (WINAPI *real_get_focus)( void );
static HWND multi_main;

/* input census (OKP_TEST_MULTI): how this game reads input - calls per function, every 5 s, and
 * each mouse-button message it takes from its queue */
enum { IN_FOREGROUND, IN_ACTIVE, IN_FOCUS, IN_PEEK, IN_GETMSG, IN_ASYNCKEY, IN_KEYSTATE, IN_CURSORPOS, IN_COUNT };
static const char *const in_names[ IN_COUNT ] = { "GetForegroundWindow", "GetActiveWindow", "GetFocus",
    "PeekMessageA", "GetMessageA", "GetAsyncKeyState", "GetKeyState", "GetCursorPos" };
static LONG in_calls[ IN_COUNT ], in_lbutton_asks, in_mouse_msgs;

static DWORD WINAPI input_census( void *arg )
{
    LONG last[ IN_COUNT ] = { 0 };
    for ( ;; )
    {
        char line[ 512 ];
        int i, n = 0;
        Sleep( 5000 );
        for ( i = 0; i < IN_COUNT; i++ )
        {
            LONG c = in_calls[ i ];
            n += snprintf( line + n, sizeof(line) - n, "%s %ld  ", in_names[ i ], c - last[ i ] );
            last[ i ] = c;
        }
        okp_log( 1, "input census (5 s): %s| left-button asks %ld, mouse messages taken %ld", line, in_lbutton_asks,
                 in_mouse_msgs );
    }
    return 0;
}

static BOOL CALLBACK multi_find_main( HWND w, LPARAM param )
{
    DWORD pid = 0;
    GetWindowThreadProcessId( w, &pid );
    if ( pid != GetCurrentProcessId() || !IsWindowVisible( w ) || GetWindow( w, GW_OWNER ) ) return TRUE;
    multi_main = w;
    return FALSE;
}

static HWND multi_own( HWND w )
{
    DWORD pid = 0;
    if ( w ) GetWindowThreadProcessId( w, &pid );
    if ( w && pid == GetCurrentProcessId() ) return w;
    if ( !multi_main || !IsWindow( multi_main ) ) { multi_main = NULL; EnumWindows( multi_find_main, 0 ); }
    return multi_main ? multi_main : w;
}

static HWND WINAPI multi_get_foreground( void ) { InterlockedIncrement( &in_calls[ IN_FOREGROUND ] ); return multi_own( real_get_foreground() ); }
static HWND WINAPI multi_get_active( void ) { InterlockedIncrement( &in_calls[ IN_ACTIVE ] ); return multi_own( real_get_active() ); }
static HWND WINAPI multi_get_focus( void ) { InterlockedIncrement( &in_calls[ IN_FOCUS ] ); return multi_own( real_get_focus() ); }

/* A click the test posts (WM_LBUTTONDOWN/UP) leaves the real button state alone; games of this era
 * also ask "is the left button down now?" (GetAsyncKeyState/GetKeyState). Between the game taking a
 * button-down message from its queue and taking the button-up, the answer is "down". */
static BOOL (WINAPI *real_peek)( MSG *, HWND, UINT, UINT, UINT );
static BOOL (WINAPI *real_get_message)( MSG *, HWND, UINT, UINT );
static SHORT (WINAPI *real_async_key)( int );
static SHORT (WINAPI *real_key_state)( int );
static LONG multi_lbutton;
static BOOL (WINAPI *real_cursor_pos)( POINT * );
static POINT multi_pt;          /* screen position of the last mouse message the game took */
static BOOL multi_pt_valid;

static void multi_saw( const MSG *m )
{
    if ( m->message == WM_MOUSEMOVE || m->message == WM_LBUTTONDOWN || m->message == WM_LBUTTONUP )
    {
        POINT pt;
        pt.x = (short)LOWORD( m->lParam );
        pt.y = (short)HIWORD( m->lParam );
        if ( m->hwnd && ClientToScreen( m->hwnd, &pt ) ) { multi_pt = pt; multi_pt_valid = TRUE; }
    }
    if ( m->message == WM_LBUTTONDOWN ) InterlockedExchange( &multi_lbutton, 1 );
    else if ( m->message == WM_LBUTTONUP ) InterlockedExchange( &multi_lbutton, 0 );
    if ( m->message >= WM_MOUSEFIRST && m->message <= WM_MOUSELAST ) InterlockedIncrement( &in_mouse_msgs );
    if ( m->message == WM_LBUTTONDOWN || m->message == WM_LBUTTONUP || m->message == WM_ACTIVATEAPP ||
         m->message == WM_ACTIVATE || m->message == WM_CHAR )
        okp_log( 1, "input: the game took msg 0x%04x wparam 0x%lx lparam 0x%lx (hwnd %p)", m->message,
                 (unsigned long)m->wParam, (unsigned long)m->lParam, m->hwnd );
}

/* the game asks where the cursor is when it handles a click: with the real cursor outside this
 * copy's window (the other copy has it, or nobody), answer with where the last mouse message was */
static BOOL WINAPI multi_cursor_pos( POINT *pt )
{
    BOOL r = real_cursor_pos( pt );
    HWND under, mine;
    InterlockedIncrement( &in_calls[ IN_CURSORPOS ] );
    DWORD pid = 0;

    if ( !r || !pt || !multi_pt_valid ) return r;
    under = WindowFromPoint( *pt );
    if ( under ) GetWindowThreadProcessId( under, &pid );
    mine = multi_own( NULL );
    if ( pid != GetCurrentProcessId() || !mine ) *pt = multi_pt;
    return r;
}

static BOOL WINAPI multi_peek( MSG *m, HWND w, UINT lo, UINT hi, UINT remove )
{
    BOOL r = real_peek( m, w, lo, hi, remove );
    InterlockedIncrement( &in_calls[ IN_PEEK ] );
    if ( r && m && (remove & PM_REMOVE) ) multi_saw( m );
    return r;
}

static BOOL WINAPI multi_get_message( MSG *m, HWND w, UINT lo, UINT hi )
{
    BOOL r = real_get_message( m, w, lo, hi );
    InterlockedIncrement( &in_calls[ IN_GETMSG ] );
    if ( r > 0 && m ) multi_saw( m );
    return r;
}

static SHORT WINAPI multi_async_key( int vk )
{
    SHORT r = real_async_key( vk );
    InterlockedIncrement( &in_calls[ IN_ASYNCKEY ] );
    if ( vk == VK_LBUTTON ) InterlockedIncrement( &in_lbutton_asks );
    return vk == VK_LBUTTON && multi_lbutton ? (SHORT)(r | 0x8001) : r;
}

static SHORT WINAPI multi_key_state( int vk )
{
    SHORT r = real_key_state( vk );
    InterlockedIncrement( &in_calls[ IN_KEYSTATE ] );
    if ( vk == VK_LBUTTON ) InterlockedIncrement( &in_lbutton_asks );
    return vk == VK_LBUTTON && multi_lbutton ? (SHORT)(r | 0x8000) : r;
}

static void test_multi( void )
{
    real_create_mutex = (void *)patch_exe_import( "kernel32.dll", "CreateMutexA", (FARPROC)multi_create_mutex );
    real_create_semaphore = (void *)patch_exe_import( "kernel32.dll", "CreateSemaphoreA", (FARPROC)multi_create_semaphore );
    real_create_mapping = (void *)patch_exe_import( "kernel32.dll", "CreateFileMappingA", (FARPROC)multi_create_mapping );
    real_find_window = (void *)patch_exe_import( "user32.dll", "FindWindowA", (FARPROC)multi_find_window );
    real_get_foreground = (void *)patch_exe_import( "user32.dll", "GetForegroundWindow", (FARPROC)multi_get_foreground );
    real_get_active = (void *)patch_exe_import( "user32.dll", "GetActiveWindow", (FARPROC)multi_get_active );
    real_get_focus = (void *)patch_exe_import( "user32.dll", "GetFocus", (FARPROC)multi_get_focus );
    real_peek = (void *)patch_exe_import( "user32.dll", "PeekMessageA", (FARPROC)multi_peek );
    real_get_message = (void *)patch_exe_import( "user32.dll", "GetMessageA", (FARPROC)multi_get_message );
    real_async_key = (void *)patch_exe_import( "user32.dll", "GetAsyncKeyState", (FARPROC)multi_async_key );
    real_key_state = (void *)patch_exe_import( "user32.dll", "GetKeyState", (FARPROC)multi_key_state );
    real_cursor_pos = (void *)patch_exe_import( "user32.dll", "GetCursorPos", (FARPROC)multi_cursor_pos );
    if ( !real_cursor_pos ) real_cursor_pos = GetCursorPos;
    if ( !real_peek ) real_peek = PeekMessageA;
    if ( !real_get_message ) real_get_message = GetMessageA;
    if ( !real_async_key ) real_async_key = GetAsyncKeyState;
    if ( !real_key_state ) real_key_state = GetKeyState;
    if ( !real_get_foreground ) real_get_foreground = GetForegroundWindow;
    if ( !real_get_active ) real_get_active = GetActiveWindow;
    if ( !real_get_focus ) real_get_focus = GetFocus;
    /* (an import the game does not have is simply not patched; keep the pointers valid anyway) */
    if ( !real_create_mutex ) real_create_mutex = CreateMutexA;
    if ( !real_create_semaphore ) real_create_semaphore = CreateSemaphoreA;
    if ( !real_create_mapping ) real_create_mapping = CreateFileMappingA;
    if ( !real_find_window ) real_find_window = FindWindowA;
    okp_log( 0, "test_multi: OKP_TEST_MULTI is set - this copy of the game ignores other running copies (tests only)" );
    {
        HANDLE t = CreateThread( NULL, 0, input_census, NULL, 0, NULL );
        if ( t ) CloseHandle( t );
    }
}

/* from DllMain (process attach) */
void DP_BUILTIN_Attach( HINSTANCE hinst )
{
    char *slash;
    HANDLE thread;

    self = hinst;
    GetModuleFileNameA( hinst, dir, sizeof(dir) );
    slash = strrchr( dir, '\\' );
    if ( slash ) slash[ 1 ] = 0; else dir[ 0 ] = 0;
    snprintf( ini, sizeof(ini), "%sokplay.ini", dir );
    okbox_init( hinst, is_aoe2_exe, ini );

    okp_builtin_attach( hinst );
    /* how the game was started (a launcher that played the intros itself passes e.g. NOSTARTUP) */
    okp_log( 1, "command line: %.900s", GetCommandLineA() );
    /* here, in DllMain: the game's own code (its C runtime reads the command line) runs only
     * after every DLL it imports has been initialised */
    if ( ini_int( "game_fixes", 1 ) ) DP_BUILTIN_GameFixes();   /* shim/gamefix.c: exact exes only */
    if ( !ini_int( "intro", 1 ) ) skip_intro_movies();
    if ( ini_int( "keep_desktop_mode", 0 ) ) keep_desktop_mode();
    if ( ini_int( "com_redirect", 1 ) ) com_redirect();
    if ( GetEnvironmentVariableA( "OKP_TEST_MULTI", NULL, 0 ) ) test_multi();
    /* not in DllMain itself: it loads a DLL. The thread starts once loading is done, well
     * before the game gets to its intro movies */
    thread = CreateThread( NULL, 0, video_setup, NULL, 0, NULL );
    if ( thread ) CloseHandle( thread );
}
