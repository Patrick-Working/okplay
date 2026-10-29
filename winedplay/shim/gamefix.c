/*
 * Game fixes: a game whose own exe keeps it from reaching DirectPlay gets the smallest change
 * that undoes that - in memory only, never on disk, and only in the one exact exe it was made for.
 *
 * Each fix names an exe (file name, size and the SHA-256 of the whole file) and, for every place it
 * changes, the bytes it expects there and around it. All of it must match or nothing is touched:
 * an original release, another store's build, a patched or fan-fixed exe - any exe that differs in
 * a single byte - runs exactly as it is, and the log says why. okplay.ini game_fixes=0 turns this
 * off. Applied from DllMain (builtin.c DP_BUILTIN_Attach): this DLL loads at the game's first
 * DirectPlay call, before that call returns into the game's code.
 *
 * Necrodome (GOG, 2023 re-release by SNEG, NECRO95.EXE 795136 bytes): Network Game showed nothing
 * and went back to the menu. The callback the game hands DirectPlayEnumerate (0x44c16c) is meant
 * to format "<provider name> <major>.<minor>" (its sprintf, 0x495ccd) and add that provider to the
 * game's own list (0x44c8f4, which copies name and GUID and skips XBAND). In this exe both calls are
 * five NOP bytes, so the list stays empty with any DirectPlay - Microsoft's too. The fix puts the
 * two calls back. The Steam release shows the same symptom (players restored it with a patched exe);
 * its exe is not listed here until it has been checked byte for byte.
 *
 * Part of the Windows port for Classic Netplay for AoE2. LGPL 2.1+, like the rest of dplayx.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"

extern void okp_log( int level, const char *fmt, ... );
extern void DP_LimitObjectIds( DWORD limit );
extern void okp_sha256_parts( const void *const *parts, const size_t *lens, int n, unsigned char out[ 32 ] );

struct fix_site {
    DWORD rva;                    /* where the expected bytes start (relative to the exe's base) */
    const char *expect;           /* hex: what must be there now */
    DWORD patch_at;               /* rva of the bytes that change (0: a check only) */
    const char *patch;            /* hex: what they become */
};

struct game_fix {
    const char *game, *what;
    const char *exe;              /* file name, compared without case */
    DWORD size;                   /* file size */
    const char *sha256;           /* of the whole file, lowercase hex */
    DWORD timestamp, image_size;  /* PE header, checked in memory */
    DWORD max_id;                 /* 0, or the highest player/group id the game can take */
    struct fix_site sites[ 6 ];
};

static const struct game_fix fixes[] = {
    {
        "Necrodome (GOG 2023)", "Network Game: the provider list callback's two calls restored; player ids 1-199",
        "NECRO95.EXE", 795136, "0300090e6d6a8852e1a3927b9aa1e84459cf62ff9027ed222204e5bc00b3da7e",
        0x325acc81, 0x220000,
        /* its reliable layer keeps each player's sequence numbers at 0x5a0a20 + id * 8, a table of
         * 200; Windows' ids (0x12cd979f...) and Wine's (1002...) both land outside it, and the
         * "CALCULATING NETWORK LATENCY" step never ends (docs/GAME_NECRODOME.md) */
        199,
        {
            /* the game's sprintf and its add-provider function, as the restored calls expect them */
            { 0x95ccd, "53515283ec048d44241cb9ba5c490089", 0, NULL },
            { 0x4c8f4, "5351565789c389d6833d1c045a00140f8d8b00000089f0ba", 0, NULL },
            /* DirectPlayEnumerate( callback 0x44c16c, NULL ) */
            { 0x4c094, "6a00686cc14400e856ce0500", 0, NULL },
            /* the callback, whole: sprintf(buf, "%s %d.%d", name, major, minor) -> NOPs,
             * add_provider(eax = guid, edx = buf) -> NOPs */
            { 0x4c16c, "5383ec648b542478528b5c2478538b4c24785168c05d4b008d54241052909090909083c41489e2"
                       "8b44246c9090909090b80100000083c4645bc21400", 0x4c189, "e83f9b0400" },
            { 0x4c16c, NULL, 0x4c197, "e858070000" },
        },
    },
};

static int unhex( const char *hex, BYTE *out, int max )
{
    int n = 0;
    while ( hex[ 0 ] && hex[ 1 ] && n < max ) {
        unsigned v;
        if ( sscanf( hex, "%2x", &v ) != 1 ) return -1;
        out[ n++ ] = (BYTE)v;
        hex += 2;
    }
    return hex[ 0 ] ? -1 : n;
}

/* the exe's file name, size and SHA-256 (read only when name and size match a fix) */
static const struct game_fix *find_fix( const char *path, const char **why )
{
    const char *base = strrchr( path, '\\' );
    const struct game_fix *named = NULL;
    unsigned char h[ 32 ];
    char hex[ 65 ];
    HANDLE f;
    DWORD size, got;
    BYTE *data;
    size_t i;

    base = base ? base + 1 : path;
    for ( i = 0; i < sizeof(fixes) / sizeof(fixes[ 0 ]); i++ )
        if ( !lstrcmpiA( base, fixes[ i ].exe ) ) { named = &fixes[ i ]; break; }
    if ( !named ) return NULL;
    *why = "could not read the exe";
    f = CreateFileA( path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL );
    if ( f == INVALID_HANDLE_VALUE ) return NULL;
    size = GetFileSize( f, NULL );
    if ( size != named->size ) { CloseHandle( f ); *why = "a different exe (size)"; return NULL; }
    data = malloc( size );
    if ( !data || !ReadFile( f, data, size, &got, NULL ) || got != size ) { free( data ); CloseHandle( f ); return NULL; }
    CloseHandle( f );
    {
        const void *parts[ 1 ] = { data };
        size_t lens[ 1 ] = { size };
        okp_sha256_parts( parts, lens, 1, h );
    }
    free( data );
    for ( i = 0; i < 32; i++ ) snprintf( hex + 2 * i, 3, "%02x", h[ i ] );
    if ( strcmp( hex, named->sha256 ) ) {
        static char other[ 120 ];
        snprintf( other, sizeof(other), "a different exe (SHA-256 %s)", hex );
        *why = other;
        return NULL;
    }
    return named;
}

void DP_BUILTIN_GameFixes( void )
{
    char path[ MAX_PATH ];
    const char *why = NULL;
    const struct game_fix *fix;
    BYTE *base = (BYTE *)GetModuleHandleA( NULL );
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt;
    BYTE want[ 128 ], put[ 16 ];
    int i, n, done = 0, already = 0;

    if ( !base || !GetModuleFileNameA( NULL, path, sizeof(path) ) ) return;
    fix = find_fix( path, &why );
    if ( !fix ) {
        if ( why ) okp_log( 1, "game fix: %s found, but %s - left as it is", path, why );
        return;
    }
    /* the exe in memory: the same image, nothing else has changed the places we check */
    nt = (IMAGE_NT_HEADERS *)( base + dos->e_lfanew );
    if ( dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE ||
         nt->FileHeader.TimeDateStamp != fix->timestamp || nt->OptionalHeader.SizeOfImage != fix->image_size ) {
        okp_log( 1, "game fix: %s: the exe in memory is not the one checked - left as it is", fix->game );
        return;
    }
    for ( i = 0; i < 6 && fix->sites[ i ].rva; i++ ) {
        const struct fix_site *s = &fix->sites[ i ];
        if ( !s->expect ) continue;
        n = unhex( s->expect, want, sizeof(want) );
        if ( n <= 0 || s->rva + n > fix->image_size ) return;
        if ( memcmp( base + s->rva, want, n ) ) {
            /* already restored (by an earlier load of this DLL in this run)? */
            if ( s->patch_at ) {
                int p = unhex( s->patch, put, sizeof(put) );
                if ( p > 0 && !memcmp( base + s->patch_at, put, p ) ) { already = 1; continue; }
            }
            okp_log( 1, "game fix: %s: unexpected bytes at %#lx - left as it is", fix->game, (unsigned long)s->rva );
            return;
        }
    }
    if ( fix->max_id ) DP_LimitObjectIds( fix->max_id );
    if ( already ) { okp_log( 2, "game fix: %s: already applied in this run", fix->game ); return; }
    for ( i = 0; i < 6 && fix->sites[ i ].rva; i++ ) {
        const struct fix_site *s = &fix->sites[ i ];
        DWORD old;
        if ( !s->patch_at ) continue;
        n = unhex( s->patch, put, sizeof(put) );
        if ( n <= 0 || !VirtualProtect( base + s->patch_at, n, PAGE_EXECUTE_READWRITE, &old ) ) {
            okp_log( 0, "game fix: %s: could not change %#lx (%lu)", fix->game, (unsigned long)s->patch_at, GetLastError() );
            continue;
        }
        memcpy( base + s->patch_at, put, n );
        VirtualProtect( base + s->patch_at, n, old, &old );
        done++;
    }
    FlushInstructionCache( GetCurrentProcess(), NULL, 0 );
    okp_log( 1, "game fix: %s - %s (%d change%s in memory; the exe on disk is untouched; okplay.ini game_fixes=0 turns this off)",
             fix->game, fix->what, done, done == 1 ? "" : "s" );
}
