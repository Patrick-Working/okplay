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

/* AoE2 has no password box, but DirectPlay has passwords (and the host checks them on every join).
 * On Online/LAN Play we ask: the host when it creates a game (okplay.ini use_password=ask|off; v0.5.0 called it host_password),
 * a joiner when the game it picked needs one. What DirectPlay gets - and what goes on the wire - is
 * never the typed text: both sides turn it into "okp1-" + SHA-256(text + the session's instance
 * GUID), so a sniffed value is useless for any other game and does not reveal the password.
 * Tests answer without a box: OKP_TEST_HOST_PASSWORD (<none> = open game) / OKP_TEST_JOIN_PASSWORD
 * (<cancel> = Cancel). */

static void hash_password( const WCHAR *typed, const GUID *instance, WCHAR *out, DWORD size )
{
    static const char salt[] = "okplay game password v1|";
    UCHAR digest[ 32 ];
    char utf8[ 512 ];
    int n = okp_utf16_to_utf8( typed, utf8, sizeof(utf8) );   /* no CP_UTF8 on Windows 95 */
    const void *parts[ 3 ];
    size_t lens[ 3 ];
    DWORD i;

    if ( n < 0 ) n = 0;   /* an unconvertible password still hashes (as "") rather than failing open */
    parts[ 0 ] = salt;     lens[ 0 ] = sizeof(salt) - 1;
    parts[ 1 ] = utf8;     lens[ 1 ] = n;
    parts[ 2 ] = instance; lens[ 2 ] = sizeof(GUID);
    okp_sha256_parts( parts, lens, 3, digest );   /* the same digest bcrypt gave, on every Windows */
    lstrcpynW( out, L"okp1-", size );
    for ( i = 0; i < 20 && 5 + i * 2 + 2 < size; i++ )   /* 160 bits is plenty for a lobby lock */
    {
        out[ 5 + i * 2 ] = L"0123456789abcdef"[ digest[ i ] >> 4 ];
        out[ 5 + i * 2 + 1 ] = L"0123456789abcdef"[ digest[ i ] & 15 ];
        out[ 5 + i * 2 + 2 ] = 0;
    }
}

/* The boxes look like AoE2's own dialogs: a blue speckled panel with a gold frame, a gothic title
 * (AoE2's own Lucida Blackletter, else Old English Text MT, else Georgia), dark-ink serif
 * text, a black field and tan-edged dark buttons. All drawn here: no caption bar, owner-drawn
 * buttons and checkbox, and the panel can be dragged anywhere. */
#define PW_NOASK  102
#define PW_THIRD  103   /* an optional third button, between the first and the last */

/* Colours: only the 16 basic VGA colours. On Windows 9x AoE2 runs the screen in 256 colours and sets
 * the palette itself through DirectDraw, which GDI can't see: any other colour is dithered or lands on
 * the wrong palette entry (the panel came out teal, then red). Games leave these 16 alone, so the boxes
 * look exactly the same in 16 colours, in-game on 98 and in full colour on Windows 10/11.
 * Scheme "Midnight Gold" (2026-09-30), one colour per part. */
#define VGA_BLACK   RGB(   0,   0,   0 )
#define VGA_MAROON  RGB( 128,   0,   0 )
#define VGA_GREEN   RGB(   0, 128,   0 )
#define VGA_OLIVE   RGB( 128, 128,   0 )
#define VGA_NAVY    RGB(   0,   0, 128 )
#define VGA_PURPLE  RGB( 128,   0, 128 )
#define VGA_TEAL    RGB(   0, 128, 128 )
#define VGA_SILVER  RGB( 192, 192, 192 )
#define VGA_GREY    RGB( 128, 128, 128 )
#define VGA_RED     RGB( 255,   0,   0 )
#define VGA_LIME    RGB(   0, 255,   0 )
#define VGA_YELLOW  RGB( 255, 255,   0 )
#define VGA_BLUE    RGB(   0,   0, 255 )
#define VGA_FUCHSIA RGB( 255,   0, 255 )
#define VGA_AQUA    RGB(   0, 255, 255 )
#define VGA_WHITE   RGB( 255, 255, 255 )

#define C_PANEL      VGA_BLACK     /* the panel ... */
#define C_SPECK      VGA_NAVY    /* ... and its specks (1 pixel in 16) */
#define C_TITLE      VGA_YELLOW
#define C_TEXT       VGA_SILVER    /* body text and the checkbox label */
#define C_FRAME      VGA_OLIVE    /* the 2 px frames */
#define C_EDGE       VGA_BLACK    /* the 1 px edges around them */
#define C_FIELD      VGA_NAVY    /* password field and checkbox */
#define C_FIELDTEXT  VGA_WHITE    /* typed text */
#define C_BUTTON     VGA_NAVY
#define C_BUTTONDOWN VGA_BLUE     /* pressed */
#define C_BUTTONTEXT VGA_YELLOW
#define C_CHECK      VGA_YELLOW    /* the tick */

struct pw_dialog
{
    const WCHAR *title, *text;
    const WCHAR *ok, *cancel;       /* cancel NULL: one button */
    const WCHAR *third;             /* a third choice between them (PW_THIRD), NULL: none */
    const WCHAR *check;             /* NULL: no checkbox */
    BOOL edit;                      /* a password field */
    WCHAR typed[ 128 ];
    BOOL checked;
    short text_cy;                  /* height of the text block (dialog units), 0 = 26 */
    /* while shown */
    HBRUSH panel, field;
    HFONT title_font, text_font, button_font, small_font;
    short cx, cy, y_text, y_edit, y_check, y_buttons;   /* layout, dialog units */
    short x_check, x_buttons;                           /* left edges: checkbox control, first button */
    short x_text, text_h;                               /* the text block: side margin and planned height */
    int grow;                                           /* pixels the text block grew to fit its text */
};

#define PW_TEXT   100   /* the text, in the plain Windows box (simple_textboxes=1) */

static HBRUSH make_panel_brush( void )
{
    /* a 64x64 tile of panel colour with specks, the same every time */
    BITMAPINFO bi;
    DWORD *px = NULL;
    HBITMAP bmp;
    HBRUSH brush;
    UINT seed = 20260928;
    HDC dc = GetDC( NULL );
    int i;

    memset( &bi, 0, sizeof(bi) );
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = 64;
    bi.bmiHeader.biHeight = -64;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection( dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0 );
    ReleaseDC( NULL, dc );
    if ( !bmp || !px ) return CreateSolidBrush( C_PANEL );
    for ( i = 0; i < 64 * 64; i++ )
    {
        COLORREF c;
        seed = seed * 1103515245 + 12345;
        c = ((seed >> 9) & 15) == 0 ? C_SPECK : C_PANEL;   /* 1 in 16 pixels a speck */
        px[ i ] = ((DWORD)GetRValue( c ) << 16) | ((DWORD)GetGValue( c ) << 8) | GetBValue( c );
    }
    brush = CreatePatternBrush( bmp );
    DeleteObject( bmp );
    return brush ? brush : CreateSolidBrush( C_PANEL );
}

static int CALLBACK font_found( const LOGFONTA *lf, const TEXTMETRICA *tm, DWORD type, LPARAM lp )
{
    *(BOOL *)lp = TRUE;
    return 0;
}

static BOOL font_installed( const WCHAR *face )
{
    LOGFONTA lf;
    BOOL have = FALSE;
    HDC dc = GetDC( NULL );
    memset( &lf, 0, sizeof(lf) );
    lf.lfCharSet = DEFAULT_CHARSET;
    WideCharToMultiByte( CP_ACP, 0, face, -1, lf.lfFaceName, LF_FACESIZE, NULL, NULL );
    EnumFontFamiliesExA( dc, &lf, (FONTENUMPROCA)font_found, (LPARAM)&have, 0 );
    ReleaseDC( NULL, dc );
    return have;
}

/* the first of these faces this PC has (the last one is used regardless) */
static HFONT make_font( const WCHAR *const *faces, int px, int weight )
{
    HDC dc = GetDC( NULL );
    int dpi = GetDeviceCaps( dc, LOGPIXELSY );
    ReleaseDC( NULL, dc );
    char face[ LF_FACESIZE ];
    while ( faces[ 1 ] && !font_installed( faces[ 0 ] ) ) faces++;
    WideCharToMultiByte( CP_ACP, 0, faces[ 0 ], -1, face, sizeof(face), NULL, NULL );
    /* ClearType where Windows has it (XP+); 98 ignores the unknown quality and uses its default */
    return CreateFontA( -MulDiv( px, dpi, 96 ), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_ROMAN,
                        face );
}

/* AoE2's own fonts (its language.dll names them): Lucida Blackletter for titles and buttons,
 * Georgia for text. Lucida Blackletter comes with Office / older AoE installs, not with Windows. */
static const WCHAR *const gothic_faces[] = { L"Lucida Blackletter", L"Old English Text MT", L"Georgia", NULL };
static const WCHAR *const text_faces[]   = { L"Georgia", L"Times New Roman", NULL };

/* the dialogs keep their text as WCHAR; Windows 9x only draws ANSI (its W functions are stubs,
 * and not every 9x exports them, so the NT ones are looked up rather than imported) */
static FARPROC user32_w( const char *name )
{
    return okp_win9x_paths() ? NULL : GetProcAddress( GetModuleHandleA( "user32.dll" ), name );
}

static int draw_text( HDC dc, const WCHAR *text, RECT *r, UINT flags )
{
    static int (WINAPI *w)( HDC, LPCWSTR, int, RECT *, UINT );
    char a[ 1024 ];   /* the longest text (server unreachable / update notice) is 600 characters */
    if ( !w ) w = (void *)user32_w( "DrawTextW" );
    if ( w ) return w( dc, text, -1, r, flags );
    a[ sizeof(a) - 1 ] = 0;
    WideCharToMultiByte( CP_ACP, 0, text, -1, a, sizeof(a) - 1, NULL, NULL );
    return DrawTextA( dc, a, -1, r, flags );
}

static void set_text( HWND wnd, const WCHAR *text )
{
    BOOL (WINAPI *set_w)( HWND, LPCWSTR ) = (void *)user32_w( "SetWindowTextW" );
    char a[ 1024 ];
    if ( set_w ) { set_w( wnd, text ); return; }
    a[ sizeof(a) - 1 ] = 0;
    WideCharToMultiByte( CP_ACP, 0, text, -1, a, sizeof(a) - 1, NULL, NULL );
    SetWindowTextA( wnd, a );
}

static RECT dlu_rect( HWND dlg, int x, int y, int cx, int cy )
{
    RECT r = { x, y, x + cx, y + cy };
    MapDialogRect( dlg, &r );
    return r;
}

static void frame( HDC dc, RECT r, COLORREF c, int width )
{
    HBRUSH b = CreateSolidBrush( c );
    int i;
    for ( i = 0; i < width; i++ ) { FrameRect( dc, &r, b ); InflateRect( &r, -1, -1 ); }
    DeleteObject( b );
}

/* The text block is planned in dialog units, but how tall the text really is depends on the font this
 * PC has, its DPI and the text itself (the update note comes from the server): measure it, and if it
 * needs more room move everything below it down and grow the box. Nothing is ever cut off.
 * text_ctl: the control that shows the text (plain box), NULL when we paint it ourselves. */
static BOOL CALLBACK move_down( HWND child, LPARAM lp )
{
    LPARAM *a = (LPARAM *)lp;   /* dialog, the text control, pixels */
    RECT r;
    if ( GetParent( child ) != (HWND)a[ 0 ] ) return TRUE;
    GetWindowRect( child, &r );
    MapWindowPoints( NULL, (HWND)a[ 0 ], (POINT *)&r, 2 );
    if ( child == (HWND)a[ 1 ] )
        SetWindowPos( child, NULL, 0, 0, r.right - r.left, r.bottom - r.top + (int)a[ 2 ], SWP_NOMOVE | SWP_NOZORDER );
    else
        SetWindowPos( child, NULL, r.left, r.top + (int)a[ 2 ], 0, 0, SWP_NOSIZE | SWP_NOZORDER );
    return TRUE;
}

static void fit_text( HWND dlg, struct pw_dialog *d, HFONT font, HWND text_ctl )
{
    RECT have = dlu_rect( dlg, d->x_text, d->y_text, d->cx - 2 * d->x_text, d->text_h ), need = have, wr;
    HDC dc = GetDC( dlg );
    HGDIOBJ old = SelectObject( dc, font );
    LPARAM args[ 3 ];
    int want;

    need.bottom = need.top;
    draw_text( dc, d->text, &need, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX );
    SelectObject( dc, old );
    ReleaseDC( dlg, dc );
    want = need.bottom - need.top + 2;
    d->grow = want > have.bottom - have.top ? want - (have.bottom - have.top) : 0;
    okp_log( 2, "box: the text needs %d px, the layout planned %ld px - %s%d px", want, have.bottom - have.top,
             d->grow ? "box grown by " : "fits, spare ", d->grow ? d->grow : (int)(have.bottom - have.top) - want );
    if ( !d->grow ) return;
    args[ 0 ] = (LPARAM)dlg; args[ 1 ] = (LPARAM)text_ctl; args[ 2 ] = d->grow;
    EnumChildWindows( dlg, move_down, (LPARAM)args );
    GetWindowRect( dlg, &wr );
    SetWindowPos( dlg, NULL, wr.left, wr.top - d->grow / 2, wr.right - wr.left, wr.bottom - wr.top + d->grow,
                  SWP_NOZORDER );
}

/* both kinds of box, once laid out: in front, focus on the field (or the first button).
 * Tests: OKP_TEST_BOX_CLOSE_MS=<ms> closes a box by itself (as Cancel) so its layout can be checked. */
static void box_shown( HWND dlg, struct pw_dialog *d )
{
    const char *ms = getenv( "OKP_TEST_BOX_CLOSE_MS" );
    SendMessageA( dlg, DM_SETDEFID, IDOK, 0 );
    SetWindowPos( dlg, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE );
    SetForegroundWindow( dlg );
    SetFocus( GetDlgItem( dlg, d->edit ? 101 : IDOK ) );
    if ( ms && atoi( ms ) > 0 ) SetTimer( dlg, 1, atoi( ms ), NULL );
}

static void read_typed( HWND dlg, struct pw_dialog *d )
{
    UINT (WINAPI *get_w)( HWND, int, LPWSTR, int ) = (void *)user32_w( "GetDlgItemTextW" );
    if ( !d->edit ) return;
    if ( get_w ) get_w( dlg, 101, d->typed, ARRAY_SIZE( d->typed ) );
    else
    {
        char a[ ARRAY_SIZE( d->typed ) ];
        GetDlgItemTextA( dlg, 101, a, sizeof(a) );
        MultiByteToWideChar( CP_ACP, 0, a, -1, d->typed, ARRAY_SIZE( d->typed ) );
        SecureZeroMemory( a, sizeof(a) );
    }
}

static void pw_paint( HWND dlg, struct pw_dialog *d )
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint( dlg, &ps );
    RECT rc, r;

    GetClientRect( dlg, &rc );
    frame( dc, rc, C_EDGE, 1 );
    InflateRect( &rc, -1, -1 );
    frame( dc, rc, C_FRAME, 2 );
    InflateRect( &rc, -3, -3 );
    frame( dc, rc, C_EDGE, 1 );

    SetBkMode( dc, TRANSPARENT );
    SetTextColor( dc, C_TITLE );
    r = dlu_rect( dlg, 8, 3, d->cx - 16, 22 );
    SelectObject( dc, d->title_font );
    draw_text( dc, d->title, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX );
    r = dlu_rect( dlg, d->x_text, d->y_text, d->cx - 2 * d->x_text, d->text_h );
    r.bottom += d->grow;
    SelectObject( dc, d->text_font );
    SetTextColor( dc, C_TEXT );
    draw_text( dc, d->text, &r, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX );   /* centred over the field */
    if ( d->edit )   /* a tan edge around the black field, like AoE2's */
    {
        GetWindowRect( GetDlgItem( dlg, 101 ), &r );
        MapWindowPoints( NULL, dlg, (POINT *)&r, 2 );
        InflateRect( &r, 2, 2 );
        frame( dc, r, C_FRAME, 2 );
    }
    EndPaint( dlg, &ps );
}

static void pw_draw_item( HWND dlg, struct pw_dialog *d, const DRAWITEMSTRUCT *di )
{
    HDC dc = di->hDC;
    RECT r = di->rcItem;

    SetBkMode( dc, TRANSPARENT );
    if ( di->CtlID == PW_NOASK )
    {
        /* the panel behind it, lined up with the dialog's texture */
        POINT o = { 0, 0 };
        RECT box, label;
        MapWindowPoints( di->hwndItem, dlg, &o, 1 );
        SetBrushOrgEx( dc, -o.x, -o.y, NULL );
        FillRect( dc, &r, d->panel );
        /* the box and its label, centred together under the password field */
        SelectObject( dc, d->small_font );
        {
            TEXTMETRICA tm;
            RECT m = { 0, 0, 0, 0 };
            int bs, gap, x0, y0;
            GetTextMetricsA( dc, &tm );
            bs = tm.tmAscent + 4;                   /* box size follows the font (+ its 2 px edge) */
            gap = bs / 2;
            m.right = (r.right - r.left) - bs - gap - 4;
            draw_text( dc, d->check, &m, DT_CALCRECT | DT_LEFT | DT_WORDBREAK | DT_NOPREFIX );
            {   /* the box lines up with the left edge of the first button (Host) */
                RECT a = { d->x_check, 0, d->x_buttons, 0 };
                MapDialogRect( dlg, &a );
                x0 = r.left + (a.right - a.left);
            }
            y0 = r.top + ((r.bottom - r.top) - (m.bottom - m.top)) / 2;
            box.left = x0;
            box.top = r.top + ((r.bottom - r.top) - bs) / 2;
            box.right = box.left + bs;
            box.bottom = box.top + bs;
            label.left = box.right + gap;
            label.top = y0;
            label.right = label.left + (m.right - m.left);
            label.bottom = y0 + (m.bottom - m.top);
        }
        { HBRUSH f = CreateSolidBrush( C_FIELD ); FillRect( dc, &box, f ); DeleteObject( f ); }
        frame( dc, box, C_FRAME, 2 );   /* the same 2 px frame as the field and the buttons */
        if ( d->checked )
        {
            HPEN pen = CreatePen( PS_SOLID, 2, C_CHECK ), old = SelectObject( dc, pen );
            int w = box.right - box.left;
            MoveToEx( dc, box.left + w / 5, box.top + w / 2, NULL );
            LineTo( dc, box.left + w * 2 / 5, box.bottom - w / 4 );
            LineTo( dc, box.right - w / 6, box.top + w / 5 );
            SelectObject( dc, old );
            DeleteObject( pen );
        }
        SetTextColor( dc, C_TEXT );
        draw_text( dc, d->check, &label, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX );
        if ( di->itemState & ODS_FOCUS ) { RECT f = box; InflateRect( &f, 2, 2 ); DrawFocusRect( dc, &f ); }
        return;
    }
    {   /* Host / No password / Join / Cancel / OK */
        HBRUSH fill = CreateSolidBrush( (di->itemState & ODS_SELECTED) ? C_BUTTONDOWN : C_BUTTON );
        const WCHAR *text = di->CtlID == IDOK ? d->ok : di->CtlID == PW_THIRD ? d->third : d->cancel;
        FillRect( dc, &r, fill );
        DeleteObject( fill );
        frame( dc, r, C_EDGE, 1 );
        InflateRect( &r, -1, -1 );
        frame( dc, r, C_FRAME, 2 );
        if ( di->itemState & ODS_SELECTED ) OffsetRect( &r, 1, 1 );
        SetTextColor( dc, C_BUTTONTEXT );
        SelectObject( dc, d->button_font );
        draw_text( dc, text, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX );
        if ( di->itemState & ODS_FOCUS ) { InflateRect( &r, -4, -4 ); DrawFocusRect( dc, &r ); }
    }
}

static INT_PTR CALLBACK pw_proc( HWND dlg, UINT msg, WPARAM wp, LPARAM lp )
{
    struct pw_dialog *d = (struct pw_dialog *)GetWindowLongPtrA( dlg, DWLP_USER );
    switch ( msg )
    {
    case WM_INITDIALOG:
        SetWindowLongPtrA( dlg, DWLP_USER, lp );
        d = (struct pw_dialog *)lp;
        d->panel = make_panel_brush();
        d->field = CreateSolidBrush( C_FIELD );
        d->title_font = make_font( gothic_faces, 28, FW_BOLD );
        d->text_font = make_font( text_faces, 15, FW_BOLD );
        d->small_font = make_font( text_faces, 13, FW_BOLD );
        d->button_font = make_font( gothic_faces, 19, FW_BOLD );
        set_text( dlg, d->title );   /* for the taskbar and Alt+Tab */
        if ( d->edit )
        {
            SendDlgItemMessageA( dlg, 101, WM_SETFONT, (WPARAM)d->text_font, FALSE );
            SendDlgItemMessageA( dlg, 101, EM_LIMITTEXT, ARRAY_SIZE( d->typed ) - 1, 0 );
        }
        fit_text( dlg, d, d->text_font, NULL );
        box_shown( dlg, d );
        return FALSE;
    case WM_TIMER:
        EndDialog( dlg, IDCANCEL );
        return TRUE;
    case WM_ERASEBKGND:
    {
        RECT rc;
        GetClientRect( dlg, &rc );
        SetBrushOrgEx( (HDC)wp, 0, 0, NULL );
        FillRect( (HDC)wp, &rc, d->panel );
        SetWindowLongPtrA( dlg, DWLP_MSGRESULT, TRUE );
        return TRUE;
    }
    case WM_PAINT:
        pw_paint( dlg, d );
        return TRUE;
    case WM_CTLCOLOREDIT:
        SetTextColor( (HDC)wp, C_FIELDTEXT );
        SetBkColor( (HDC)wp, C_FIELD );
        return (INT_PTR)d->field;
    case WM_DRAWITEM:
        pw_draw_item( dlg, d, (const DRAWITEMSTRUCT *)lp );
        return TRUE;
    case WM_NCHITTEST:   /* drag the panel anywhere */
        SetWindowLongPtrA( dlg, DWLP_MSGRESULT, HTCAPTION );
        return TRUE;
    case WM_COMMAND:
        if ( LOWORD( wp ) == PW_NOASK && HIWORD( wp ) == BN_CLICKED )
        {
            d->checked = !d->checked;
            InvalidateRect( GetDlgItem( dlg, PW_NOASK ), NULL, TRUE );
            return TRUE;
        }
        if ( LOWORD( wp ) == IDOK || LOWORD( wp ) == IDCANCEL || LOWORD( wp ) == PW_THIRD )
        {
            read_typed( dlg, d );
            EndDialog( dlg, LOWORD( wp ) );
            return TRUE;
        }
        break;
    case WM_DESTROY:
        DeleteObject( d->panel );
        DeleteObject( d->field );
        DeleteObject( d->title_font );
        DeleteObject( d->text_font );
        DeleteObject( d->small_font );
        DeleteObject( d->button_font );
        break;
    }
    return FALSE;
}

/* the dialog is built in memory (no resource file) */
static WORD *dlg_align( WORD *p ) { return (WORD *)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3); }
static WORD *dlg_str( WORD *p, const WCHAR *s ) { while ( (*p++ = *s++) ); return p; }
static WORD *dlg_item( WORD *p, DWORD style, short x, short y, short cx, short cy, WORD id,
                       WORD cls, const WCHAR *text )
{
    DLGITEMTEMPLATE *it = (DLGITEMTEMPLATE *)dlg_align( p );
    it->style = style | WS_CHILD | WS_VISIBLE;
    it->dwExtendedStyle = 0;
    it->x = x; it->y = y; it->cx = cx; it->cy = cy; it->id = id;
    p = (WORD *)(it + 1);
    *p++ = 0xffff; *p++ = cls;              /* 0x80 button, 0x81 edit */
    p = dlg_str( p, text );
    *p++ = 0;                               /* no creation data */
    return p;
}

static INT_PTR run_dialog( DLGTEMPLATE *t, DLGPROC proc, struct pw_dialog *d )
{
    /* DLGTEMPLATE strings are WCHAR for both calls; 9x only creates ANSI dialogs */
    INT_PTR (WINAPI *box_w)( HINSTANCE, LPCDLGTEMPLATEW, HWND, DLGPROC, LPARAM ) =
        (void *)user32_w( "DialogBoxIndirectParamW" );
    if ( box_w ) return box_w( self, t, GetActiveWindow(), proc, (LPARAM)d );
    return DialogBoxIndirectParamA( self, t, GetActiveWindow(), proc, (LPARAM)d );
}

/* okplay.ini simple_textboxes=1: the same boxes as plain Windows dialogs - a title bar, the system's
 * dialog font and its ordinary field, checkbox and buttons; nothing drawn by us. Same questions,
 * same answers. */
static INT_PTR CALLBACK simple_proc( HWND dlg, UINT msg, WPARAM wp, LPARAM lp )
{
    struct pw_dialog *d = (struct pw_dialog *)GetWindowLongPtrA( dlg, DWLP_USER );
    switch ( msg )
    {
    case WM_INITDIALOG:
        SetWindowLongPtrA( dlg, DWLP_USER, lp );
        d = (struct pw_dialog *)lp;
        set_text( dlg, d->title );
        set_text( GetDlgItem( dlg, PW_TEXT ), d->text );
        set_text( GetDlgItem( dlg, IDOK ), d->ok );
        if ( d->cancel ) set_text( GetDlgItem( dlg, IDCANCEL ), d->cancel );
        if ( d->third ) set_text( GetDlgItem( dlg, PW_THIRD ), d->third );
        if ( d->check ) set_text( GetDlgItem( dlg, PW_NOASK ), d->check );
        if ( d->edit ) SendDlgItemMessageA( dlg, 101, EM_LIMITTEXT, ARRAY_SIZE( d->typed ) - 1, 0 );
        fit_text( dlg, d, (HFONT)SendMessageA( dlg, WM_GETFONT, 0, 0 ), GetDlgItem( dlg, PW_TEXT ) );
        box_shown( dlg, d );
        return FALSE;
    case WM_TIMER:
        EndDialog( dlg, IDCANCEL );
        return TRUE;
    case WM_COMMAND:
        if ( LOWORD( wp ) == IDOK || LOWORD( wp ) == IDCANCEL || LOWORD( wp ) == PW_THIRD )   /* the title bar's X is Cancel */
        {
            if ( d->check ) d->checked = IsDlgButtonChecked( dlg, PW_NOASK ) == BST_CHECKED;
            read_typed( dlg, d );
            EndDialog( dlg, LOWORD( wp ) );
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* simple_textboxes=auto (the default): the styled boxes belong to Age of Empires II - any other
 * game gets the plain ones. 0 / 1 force one kind everywhere. */
static BOOL is_aoe2_exe( void );

static BOOL simple_boxes( void )
{
    static int simple = -1;
    char v[ 16 ];
    if ( simple >= 0 ) return simple;
    ini_str( "simple_textboxes", "auto", v, sizeof(v) );
    if ( v[ 0 ] == '0' || v[ 0 ] == '1' ) simple = v[ 0 ] == '1';
    else simple = !is_aoe2_exe();
    okp_log( 1, "boxes: %s (okplay.ini simple_textboxes=%s%s)", simple ? "plain Windows dialogs" : "Age of Empires II style",
             v, v[ 0 ] == '0' || v[ 0 ] == '1' ? "" : is_aoe2_exe() ? ": this is Age of Empires II" : ": not Age of Empires II" );
    return simple;
}

static INT_PTR show_simple_dialog( struct pw_dialog *d )
{
    WORD buf[ 512 ], *p;
    DLGTEMPLATE *t;
    short y, bw = 62, bh = 14, m = 8;   /* button size, margin */
    int n = 2;

    /* layout, top to bottom (dialog units); the text starts as one line and fit_text grows it */
    d->cx = 236;
    d->x_text = m;
    d->y_text = m;
    d->text_h = 9;
    y = d->y_text + d->text_h + 6;
    d->y_edit = d->edit ? y : 0;
    if ( d->edit ) y += 18;
    d->y_check = d->check ? y : 0;
    if ( d->check ) y += (wcschr( d->check, '\n' ) ? 20 : 10) + 6;
    d->y_buttons = y;
    d->cy = d->y_buttons + bh + m;

    memset( buf, 0, sizeof(buf) );
    t = (DLGTEMPLATE *)dlg_align( buf );
    t->style = DS_SETFONT | DS_CENTER | DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    t->cx = d->cx; t->cy = d->cy;
    p = (WORD *)(t + 1);
    *p++ = 0; *p++ = 0;                     /* no menu, default class */
    p = dlg_str( p, L"" );                  /* caption and texts are set in WM_INITDIALOG */
    *p++ = 8; p = dlg_str( p, L"MS Shell Dlg" );
    p = dlg_item( p, SS_LEFT | SS_NOPREFIX, m, d->y_text, d->cx - 2 * m, d->text_h, PW_TEXT, 0x82, L"" );
    if ( d->edit )
    {
        p = dlg_item( p, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, m, d->y_edit, d->cx - 2 * m, 13, 101, 0x81, L"" );
        n++;
    }
    if ( d->check )
    {
        p = dlg_item( p, BS_AUTOCHECKBOX | BS_MULTILINE | BS_TOP | WS_TABSTOP, m, d->y_check, d->cx - 2 * m,
                      wcschr( d->check, '\n' ) ? 20 : 10, PW_NOASK, 0x80, L"" );
        n++;
    }
    if ( d->cancel && d->third )
    {
        short b3 = (d->cx - 2 * m - 8) / 3;
        p = dlg_item( p, BS_DEFPUSHBUTTON | WS_TABSTOP, m, d->y_buttons, b3, bh, IDOK, 0x80, L"" );
        p = dlg_item( p, BS_PUSHBUTTON | WS_TABSTOP, m + b3 + 4, d->y_buttons, b3, bh, PW_THIRD, 0x80, L"" );
        p = dlg_item( p, BS_PUSHBUTTON | WS_TABSTOP, m + 2 * (b3 + 4), d->y_buttons, b3, bh, IDCANCEL, 0x80, L"" );
        n += 2;
    }
    else if ( d->cancel )
    {
        p = dlg_item( p, BS_DEFPUSHBUTTON | WS_TABSTOP, d->cx - m - 2 * bw - 4, d->y_buttons, bw, bh, IDOK, 0x80, L"" );
        p = dlg_item( p, BS_PUSHBUTTON | WS_TABSTOP, d->cx - m - bw, d->y_buttons, bw, bh, IDCANCEL, 0x80, L"" );
        n++;
    }
    else
        p = dlg_item( p, BS_DEFPUSHBUTTON | WS_TABSTOP, d->cx / 2 - bw / 2, d->y_buttons, bw, bh, IDOK, 0x80, L"" );
    t->cdit = n;
    return run_dialog( t, simple_proc, d );
}

static INT_PTR show_pw_dialog( struct pw_dialog *d )
{
    WORD buf[ 1024 ], *p;
    DLGTEMPLATE *t;
    short y, bw = 104;   /* room for "No Password" in the gothic font */
    int n = 1;

    if ( simple_boxes() ) return show_simple_dialog( d );

    /* layout, top to bottom (dialog units) */
    d->cx = d->third ? 3 * bw + 24 + 40 : 280;   /* three buttons need the wider panel */
    d->x_text = 12;
    d->y_text = 28;
    d->text_h = (d->text_cy ? d->text_cy : 26) - (d->edit ? 2 : 0);
    y = d->y_text + (d->text_cy ? d->text_cy : 26);
    d->y_edit = d->edit ? y : 0;
    if ( d->edit ) y += 20;
    d->y_check = d->check ? y : 0;
    if ( d->check ) y += 22;
    d->y_buttons = y + 4;
    d->cy = d->y_buttons + 26;
    d->x_check = 14;
    d->x_buttons = d->third ? (d->cx - (3 * bw + 24)) / 2 : d->cancel ? d->cx / 2 - bw - 6 : d->cx / 2 - bw / 2;

    memset( buf, 0, sizeof(buf) );
    t = (DLGTEMPLATE *)dlg_align( buf );
    t->style = DS_SETFONT | DS_CENTER | WS_POPUP;
    t->cx = d->cx; t->cy = d->cy;
    p = (WORD *)(t + 1);
    *p++ = 0; *p++ = 0;                     /* no menu, default class */
    p = dlg_str( p, L"" );                  /* no caption: we draw the title */
    *p++ = 9; p = dlg_str( p, L"MS Shell Dlg" );
    if ( d->edit )
    {
        p = dlg_item( p, ES_AUTOHSCROLL | WS_TABSTOP, 14, d->y_edit, d->cx - 28, 14, 101, 0x81, L"" );
        n++;
    }
    if ( d->check )
    {
        p = dlg_item( p, BS_OWNERDRAW | WS_TABSTOP, 14, d->y_check, d->cx - 28, 20, PW_NOASK, 0x80, L"" );
        n++;
    }
    if ( d->cancel && d->third )
    {
        p = dlg_item( p, BS_OWNERDRAW | WS_TABSTOP, d->x_buttons, d->y_buttons, bw, 18, IDOK, 0x80, L"" );
        p = dlg_item( p, BS_OWNERDRAW | WS_TABSTOP, d->x_buttons + bw + 12, d->y_buttons, bw, 18, PW_THIRD, 0x80, L"" );
        p = dlg_item( p, BS_OWNERDRAW | WS_TABSTOP, d->x_buttons + 2 * (bw + 12), d->y_buttons, bw, 18, IDCANCEL, 0x80, L"" );
        n += 2;
    }
    else if ( d->cancel )
    {
        p = dlg_item( p, BS_OWNERDRAW | WS_TABSTOP, d->cx / 2 - bw - 6, d->y_buttons, bw, 18, IDOK, 0x80, L"" );
        p = dlg_item( p, BS_OWNERDRAW | WS_TABSTOP, d->cx / 2 + 6, d->y_buttons, bw, 18, IDCANCEL, 0x80, L"" );
        n++;
    }
    else
        p = dlg_item( p, BS_OWNERDRAW | WS_TABSTOP, d->cx / 2 - bw / 2, d->y_buttons, bw, 18, IDOK, 0x80, L"" );
    t->cdit = n;
    return run_dialog( t, pw_proc, d );
}

static void brand_w( WCHAR *out, DWORD size )
{
    char b[ 96 ];
    ini_str( "brand", "Classic Netplay", b, sizeof(b) );
    MultiByteToWideChar( CP_ACP, 0, b, -1, out, size );
}

/* OKP_PREVIEW_BOXES=1: show the boxes even where tests would answer them (to look at the design) */
static BOOL test_answers( void )
{
    const char *p = getenv( "OKP_PREVIEW_BOXES" );
    return !(p && *p == '1');
}

BOOL DP_BUILTIN_AskHostPassword( const GUID *instance, WCHAR *hashed, DWORD size )
{
    struct pw_dialog d;
    WCHAR title[ 96 ];
    const char *test = test_answers() ? getenv( "OKP_TEST_HOST_PASSWORD" ) : NULL;
    char mode[ 16 ];

    memset( &d, 0, sizeof(d) );
    if ( test )   /* tests: the password to host with; "<none>" = an open game, no box */
    {
        if ( strcmp( test, "<none>" ) )
            MultiByteToWideChar( CP_ACP, 0, test, -1, d.typed, ARRAY_SIZE( d.typed ) );
    }
    else
    {
        ini_str( "use_password", "", mode, sizeof(mode) );
        if ( !mode[ 0 ] ) ini_str( "host_password", "ask", mode, sizeof(mode) );   /* v0.5.0's name */
        if ( !_stricmp( mode, "off" ) )
        {
            okp_log( 2, "password: not asking (use_password=off) - an open game" );
            return FALSE;
        }
        brand_w( title, ARRAY_SIZE( title ) );
        d.title = title;
        d.text = L"Enter a password for this game (optional).\nAn empty entry will allow anyone to join.";
        d.edit = TRUE;
        d.check = L"Disable Hosting Password\n(okplay.ini: use_password=ask to re-enable)";
        d.ok = L"Host";
        d.cancel = L"No Password";
        if ( show_pw_dialog( &d ) != IDOK ) d.typed[ 0 ] = 0;   /* "No password" */
        if ( d.checked )
        {
            WritePrivateProfileStringA( "okplay", "use_password", "off", ini );
            okp_log( 1, "password: use_password=off written to okplay.ini (the player ticked \"Disable Hosting Password\")" );
        }
    }
    if ( !d.typed[ 0 ] )
    {
        okp_log( 1, "password: none - an open game" );
        return FALSE;
    }
    hash_password( d.typed, instance, hashed, size );
    SecureZeroMemory( d.typed, sizeof(d.typed) );
    okp_log( 1, "password: this game needs a password (only its salted hash is used)" );
    return TRUE;
}

HRESULT DP_BUILTIN_AskJoinPassword( const GUID *instance, WCHAR *hashed, DWORD size )
{
    struct pw_dialog d;
    WCHAR title[ 96 ];
    const char *test = test_answers() ? getenv( "OKP_TEST_JOIN_PASSWORD" ) : NULL;
    INT_PTR r;

    memset( &d, 0, sizeof(d) );
    if ( test )   /* "a|b|c": one answer per join attempt, the last one repeats */
    {
        static int attempt;
        char one[ 128 ];
        const char *s = test, *bar;
        int i;
        for ( i = 0; i < attempt && (bar = strchr( s, '|' )); i++ ) s = bar + 1;
        attempt++;
        lstrcpynA( one, s, sizeof(one) );
        if ( (bar = strchr( one, '|' )) ) *(char *)bar = 0;
        if ( !strcmp( one, "<cancel>" ) ) r = IDCANCEL;
        else { MultiByteToWideChar( CP_ACP, 0, one, -1, d.typed, ARRAY_SIZE( d.typed ) ); r = IDOK; }
    }
    else
    {
        brand_w( title, ARRAY_SIZE( title ) );
        d.title = title;
        d.text = L"This game has a password.\nType it to join:";
        d.edit = TRUE;
        d.ok = L"Join";
        d.cancel = L"Cancel";
        r = show_pw_dialog( &d );
    }
    if ( r != IDOK )
    {
        okp_log( 1, "password: the player cancelled the join" );
        return DPERR_USERCANCEL;
    }
    hash_password( d.typed, instance, hashed, size );
    SecureZeroMemory( d.typed, sizeof(d.typed) );
    okp_log( 1, "password: joining a password game (sending only the salted hash)" );
    return DP_OK;
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
    struct pw_dialog d;
    WCHAR title[ 96 ];
    if ( hr != DPERR_INVALIDPASSWORD ) return;
    okp_log( 1, "password: wrong password - the host did not let us in" );
    if ( test_answers() && getenv( "OKP_TEST_JOIN_PASSWORD" ) ) return;
    memset( &d, 0, sizeof(d) );
    brand_w( title, ARRAY_SIZE( title ) );
    d.title = title;
    d.text = L"Wrong password - the host did not let you in.";
    d.ok = L"OK";
    show_pw_dialog( &d );
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
    struct pw_dialog d;
    WCHAR title[ 96 ], text[ 400 ], wwhat[ 16 ], online[ 64 ], lan[ 64 ];
    char a[ 64 ];
    const char *test = getenv( "OKP_TEST_CLASSIC_ASK" );
    INT_PTR r;

    if ( test && *test ) return !_stricmp( test, "online" ) ? 2 : !_stricmp( test, "lan" ) ? 3 : 1;
    memset( &d, 0, sizeof(d) );
    brand_w( title, ARRAY_SIZE( title ) );
    MultiByteToWideChar( CP_ACP, 0, what, -1, wwhat, ARRAY_SIZE( wwhat ) );
    ini_str( "online_name", "Classic Netplay Online", a, sizeof(a) ); MultiByteToWideChar( CP_ACP, 0, a, -1, online, ARRAY_SIZE( online ) );
    ini_str( "lan_name", "Classic Netplay LAN", a, sizeof(a) ); MultiByteToWideChar( CP_ACP, 0, a, -1, lan, ARRAY_SIZE( lan ) );
    _snwprintf( text, ARRAY_SIZE( text ), L"The game is about to start Multiplayer with its \"%ls\" connection.\n\n"
                L"How would you like to proceed?\n\n"
                L"Online Play = %ls\n"
                L"LAN Play = %ls\n"
                L"Original = The game's original %ls connection.", wwhat, online, lan, wwhat );
    text[ ARRAY_SIZE( text ) - 1 ] = 0;
    d.title = title;
    d.text = text;
    d.text_cy = 64;
    d.ok = L"Online Play";
    d.third = L"LAN Play";
    d.cancel = L"Original";
    r = show_pw_dialog( &d );
    return r == IDOK ? 2 : r == PW_THIRD ? 3 : 1;
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
