/* moviehelper - gives the intro-movie decoder to a game whose movies are played by ANOTHER program.
 *
 * dplayx.dll registers okindeo.dll (Indeo Video 5) for the process that loads it: the game. Some
 * releases never let the game play its own intros - Steam's "Galactic Battlegrounds Saga" starts
 * player.exe, which plays xlogo1.avi and xintro.avi itself and only then starts the game. That
 * program never loads dplayx.dll, so the movies have sound but no picture.
 *
 * This DLL is named version.dll: nearly every Windows program uses version.dll, and Windows looks
 * in the program's own folder first. Dropped next to such a player it
 *   - passes every version.dll call straight on to Windows' own version.dll, and
 *   - registers okindeo.dll (next to it) as the "vidc.IV50" decoder for that process only.
 * No registry change, no install; delete the file to undo. Windows XP and later (Windows 9x has
 * Intel's Indeo itself, and there every call is still passed on).
 *
 * In a process that also has the drop-in dplayx.dll (the game itself) it stands back: dplayx.dll
 * looks after the movies there.
 *
 * No C runtime: imports kernel32, user32 and advapi32 only; msvfw32 is loaded at run time, on a
 * thread, never from DllMain. A log goes to netplay-logs\moviehelper-*.log next to this DLL.
 */
#include <windows.h>
#include <mmsystem.h>
#include <vfw.h>

static HINSTANCE self;
static char dir[ MAX_PATH ];          /* this DLL's folder, with a trailing backslash */
static HMODULE sysver;                /* Windows' own version.dll */

/* ------------------------------------------------------------------ log */

static HANDLE logf = INVALID_HANDLE_VALUE;
static LONG log_tried;

static void log_open( void )
{
    char path[ MAX_PATH + 80 ];
    SYSTEMTIME t;
    int n;

    if ( InterlockedExchange( &log_tried, 1 ) ) return;
    GetLocalTime( &t );
    n = wsprintfA( path, "%snetplay-logs", dir );
    CreateDirectoryA( path, NULL );
    wsprintfA( path + n, "\\moviehelper-%04d%02d%02d-%02d%02d%02d-%lu.log", t.wYear, t.wMonth, t.wDay,
               t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId() );
    logf = CreateFileA( path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    if ( logf == INVALID_HANDLE_VALUE && GetTempPathA( MAX_PATH, path ) )
    {   /* a folder we may not write to: the temp folder, like okplay does */
        wsprintfA( path + lstrlenA( path ), "moviehelper-%lu.log", GetCurrentProcessId() );
        logf = CreateFileA( path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    }
}

static void logline( const char *fmt, ... )
{
    char buf[ 1200 ];   /* wvsprintf writes at most 1024 */
    SYSTEMTIME t;
    DWORD done;
    va_list ap;
    int n;

    log_open();
    if ( logf == INVALID_HANDLE_VALUE ) return;
    GetLocalTime( &t );
    n = wsprintfA( buf, "%02d:%02d:%02d.%03d [%5lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                   GetCurrentProcessId() );
    va_start( ap, fmt );
    n += wvsprintfA( buf + n, fmt, ap );
    va_end( ap );
    buf[ n++ ] = '\r';
    buf[ n++ ] = '\n';
    WriteFile( logf, buf, n, &done, NULL );
}

/* ------------------------------------------------------------------ the decoder */

typedef LRESULT (CALLBACK *drvproc_t)( DWORD_PTR, HDRVR, UINT, LPARAM, LPARAM );
static drvproc_t codec_proc;          /* okindeo.dll's DriverProc, once loaded */
static LONG codec_failed, frames_decoded, frames_failed, refusals;
static char codec_path[ MAX_PATH + 16 ];

static long absl( long v ) { return v < 0 ? -v : v; }

/* okindeo.dll is loaded when the player first asks for the decoder, not before */
static drvproc_t codec_load( void )
{
    HMODULE m;
    drvproc_t p;

    if ( codec_proc || codec_failed ) return codec_proc;
    m = LoadLibraryA( codec_path );
    p = m ? (drvproc_t)GetProcAddress( m, "DriverProc" ) : NULL;
    if ( p )
    {
        codec_proc = p;
        logline( "video: the player asked for Indeo 5 - okindeo.dll loaded" );
    }
    else if ( !InterlockedExchange( &codec_failed, 1 ) )
        logline( "video: okindeo.dll would not load (Windows error %lu) - no picture", GetLastError() );
    return codec_proc;
}

/* what Video for Windows calls: okindeo.dll behind it, counting, so the log shows the movies played */
static LRESULT CALLBACK codec_entry( DWORD_PTR id, HDRVR drv, UINT msg, LPARAM p1, LPARAM p2 )
{
    drvproc_t proc = codec_proc;
    LRESULT r;

    /* housekeeping needs no decoder */
    if ( !proc && (msg == DRV_LOAD || msg == DRV_ENABLE || msg == DRV_DISABLE || msg == DRV_FREE) ) return DRV_OK;
    if ( !proc && !(proc = codec_load()) ) return (msg == DRV_OPEN || msg < DRV_USER) ? 0 : ICERR_UNSUPPORTED;

    r = proc( id, drv, msg, p1, p2 );
    if ( msg == ICM_DECOMPRESS || msg == ICM_DECOMPRESSEX )
    {
        InterlockedIncrement( &frames_decoded );
        if ( r != ICERR_OK ) InterlockedIncrement( &frames_failed );
    }
    else if ( msg == ICM_DECOMPRESS_BEGIN && p1 && p2 )
    {
        const BITMAPINFOHEADER *in = (const BITMAPINFOHEADER *)p1, *out = (const BITMAPINFOHEADER *)p2;
        logline( "video: Indeo 5 %ldx%ld playing -> %d-bit (%s)", in->biWidth, absl( in->biHeight ),
                 out->biBitCount, r == ICERR_OK ? "ok" : "refused" );
    }
    else if ( msg == ICM_DECOMPRESS_QUERY && p1 && p2 && r != ICERR_OK )
    {
        /* a player that wants an output we don't make (we make 24- and 32-bit RGB) normally tries
         * the next format; if the picture stays away, this says what it asked for */
        const BITMAPINFOHEADER *out = (const BITMAPINFOHEADER *)p2;
        if ( InterlockedIncrement( &refusals ) <= 8 )
            logline( "video: the player asked for %d-bit output (compression 0x%08lx) - not offered",
                     out->biBitCount, out->biCompression );
    }
    else if ( msg == ICM_DECOMPRESS_END )
        logline( "video: finished, %ld frames decoded so far (%ld failed)", frames_decoded, frames_failed );
    return r;
}

/* a system-wide Indeo 5 decoder (Intel's, or the old installer's): leave it in charge */
static BOOL system_has_indeo5( void )
{
    char file[ MAX_PATH ], found[ MAX_PATH ];
    DWORD size = sizeof(file) - 1, type = 0;
    BOOL ok = FALSE;
    HKEY key;

    if ( RegOpenKeyExA( HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32", 0,
                        KEY_QUERY_VALUE, &key ) )
        return FALSE;
    if ( !RegQueryValueExA( key, "vidc.iv50", NULL, &type, (BYTE *)file, &size ) && type == REG_SZ && size > 1 )
    {
        file[ size ] = 0;
        ok = SearchPathA( NULL, file, NULL, sizeof(found), found, NULL ) != 0;   /* the entry may be stale */
    }
    RegCloseKey( key );
    return ok;
}

static DWORD WINAPI video_setup( void *unused )
{
    typedef BOOL (WINAPI *icinstall_t)( DWORD, DWORD, LPARAM, LPSTR, UINT );
    char exe[ MAX_PATH ];
    icinstall_t install;
    HMODULE dp, vfw;

    GetModuleFileNameA( NULL, exe, sizeof(exe) );
    logline( "moviehelper (version.dll) in %s", exe );
    logline( "command line: %s", GetCommandLineA() );   /* wvsprintf cuts it at 1024 characters */

    if ( (dp = GetModuleHandleA( "dplayx.dll" )) && GetProcAddress( dp, "SPInit" ) )
    {
        logline( "video: this program has the drop-in dplayx.dll, which looks after the movies itself - nothing to do" );
        return 0;
    }
    if ( GetVersion() & 0x80000000 )
    {
        logline( "video: Windows 9x has its own Indeo 5 (okindeo.dll needs XP+) - nothing to do" );
        return 0;
    }
    if ( system_has_indeo5() )
    {
        logline( "video: an Indeo 5 decoder is already installed on this PC - using it" );
        return 0;
    }
    wsprintfA( codec_path, "%sokindeo.dll", dir );
    if ( GetFileAttributesA( codec_path ) == INVALID_FILE_ATTRIBUTES )
    {
        logline( "video: okindeo.dll not found next to version.dll (%s) - movies will have no picture", codec_path );
        return 0;
    }
    if ( !(vfw = LoadLibraryA( "msvfw32.dll" )) ||
         !(install = (icinstall_t)GetProcAddress( vfw, "ICInstall" )) ||
         !install( ICTYPE_VIDEO, mmioFOURCC( 'I', 'V', '5', '0' ), (LPARAM)codec_entry, NULL, ICINSTALL_FUNCTION ) )
    {
        logline( "video: could not register the decoder with Video for Windows (Windows error %lu)", GetLastError() );
        return 0;
    }
    logline( "video: Indeo 5 decoder ready for this program (okindeo.dll, this process only)" );
    return 0;
}

/* ------------------------------------------------------------------ passing version.dll on */

/* Windows' own version.dll, by full path. Windows 9x knows loaded DLLs by name only and would hand
 * us ourselves: there a copy under another name is loaded instead (as dplayx-windows9x.dll is). */
static void load_system_version( void )
{
    char path[ MAX_PATH + 32 ], copy[ MAX_PATH + 32 ];
    UINT n = GetSystemDirectoryA( path, MAX_PATH );

    if ( !n || n >= MAX_PATH ) return;
    lstrcpyA( path + n, "\\version.dll" );
    sysver = LoadLibraryA( path );
    if ( sysver != self ) return;
    FreeLibrary( sysver );
    sysver = NULL;
    wsprintfA( copy, "%sversion-windows9x.dll", dir );
    if ( CopyFileA( path, copy, FALSE ) || GetFileAttributesA( copy ) != INVALID_FILE_ATTRIBUTES )
        sysver = LoadLibraryA( copy );
    if ( sysver == self ) { FreeLibrary( sysver ); sysver = NULL; }
}

static const char *const names[] = {
    "GetFileVersionInfoA", "GetFileVersionInfoByHandle", "GetFileVersionInfoExA", "GetFileVersionInfoExW",
    "GetFileVersionInfoSizeA", "GetFileVersionInfoSizeExA", "GetFileVersionInfoSizeExW",
    "GetFileVersionInfoSizeW", "GetFileVersionInfoW", "VerFindFileA", "VerFindFileW", "VerInstallFileA",
    "VerInstallFileW", "VerLanguageNameA", "VerLanguageNameW", "VerQueryValueA", "VerQueryValueW",
};
void *moviehelper_real[ sizeof(names) / sizeof(names[ 0 ]) ];

void *moviehelper_resolve( int i )
{
    if ( !sysver ) return NULL;
    return moviehelper_real[ i ] = (void *)GetProcAddress( sysver, names[ i ] );
}

/* one exported entry per function: jump to Windows' function (looked up on first use); if this
 * Windows has none (the Ex ones before Vista), fail the way a failed call does - 0, arguments popped */
#define FWD( i, name, nargs ) __asm__( \
    ".text\n.globl _" #name "\n_" #name ":\n" \
    "    movl _moviehelper_real+4*" #i ", %eax\n" \
    "    testl %eax, %eax\n" \
    "    jnz 1f\n" \
    "    pushl $" #i "\n" \
    "    call _moviehelper_resolve\n" \
    "    addl $4, %esp\n" \
    "    testl %eax, %eax\n" \
    "    jz 2f\n" \
    "1:  jmp *%eax\n" \
    "2:  xorl %eax, %eax\n" \
    "    ret $4*" #nargs "\n" );

FWD( 0, GetFileVersionInfoA, 4 )
FWD( 1, GetFileVersionInfoByHandle, 4 )
FWD( 2, GetFileVersionInfoExA, 5 )
FWD( 3, GetFileVersionInfoExW, 5 )
FWD( 4, GetFileVersionInfoSizeA, 2 )
FWD( 5, GetFileVersionInfoSizeExA, 3 )
FWD( 6, GetFileVersionInfoSizeExW, 3 )
FWD( 7, GetFileVersionInfoSizeW, 2 )
FWD( 8, GetFileVersionInfoW, 4 )
FWD( 9, VerFindFileA, 8 )
FWD( 10, VerFindFileW, 8 )
FWD( 11, VerInstallFileA, 8 )
FWD( 12, VerInstallFileW, 8 )
FWD( 13, VerLanguageNameA, 3 )
FWD( 14, VerLanguageNameW, 3 )
FWD( 15, VerQueryValueA, 4 )
FWD( 16, VerQueryValueW, 4 )

BOOL WINAPI DllMain( HINSTANCE inst, DWORD reason, void *reserved )
{
    if ( reason == DLL_PROCESS_ATTACH )
    {
        HANDLE thread;
        DWORD tid;
        int n;

        self = inst;
        DisableThreadLibraryCalls( inst );
        n = GetModuleFileNameA( inst, dir, sizeof(dir) );
        while ( n > 0 && dir[ n - 1 ] != '\\' ) n--;
        dir[ n ] = 0;
        load_system_version();
        /* the decoder is set up on a thread: it loads DLLs, which DllMain must not. It is in place
         * long before a player has opened its window and its first movie. */
        thread = CreateThread( NULL, 0, video_setup, NULL, 0, &tid );
        if ( thread ) CloseHandle( thread );
    }
    else if ( reason == DLL_PROCESS_DETACH && logf != INVALID_HANDLE_VALUE )
    {
        logline( "video: %ld frames decoded in this program (%ld failed)", frames_decoded, frames_failed );
        CloseHandle( logf );
    }
    return TRUE;
}
