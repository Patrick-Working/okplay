/* dp_hostile - another machine in the session (or a stray packet) sends the drop-in's DirectPlay
 * (winedplay/port/dlls/dplayx) messages an honest DirectPlay never sends. Nothing may crash, and the
 * checks on what we accept must hold. The dplayx sources are built into this exe with a fake
 * service provider (no network, no threads of its own); each case runs in its own process.
 *
 *   dp_hostile <case> [iterations] [seed]      prints PASS/FAIL <case>; a crash prints CRASH, exit 1
 *   cases: joinoffset sizedstring unsolicited shortreq dp1sysmsg idwrap dupcreate
 *          gameowner replysource (these two need the OKP_BUILTIN build: they SKIP otherwise)
 *          fuzz fuzzjoin
 * Built and run by dp_hostile.sh (both builds: plain and OKP_BUILTIN). Inputs to the message
 * handler end at a no-access page, so a read past the message faults at once.
 */
#define COBJMACROS
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "windef.h"
#include "winerror.h"
#include "winbase.h"
#include "winnls.h"
#include "dplayx_global.h"
#include "name_server.h"
#include "dplayx_queue.h"
#include "wine/dplaysp.h"
#include "dplay_global.h"
#include "dplayx_messages.h"

HRESULT WINAPI DirectPlayCreate( LPGUID lpGUID, LPDIRECTPLAY *lplpDP, IUnknown *pUnk );

/* ------------------------------------------------------------------ the fake SP header: a peer id */
#define HDR 20
#pragma pack(push, 1)
struct fakehdr { DWORD magic; unsigned long long peer; DWORD room, flags; };
#pragma pack(pop)
#define FAKE_MAGIC 0x50504B4Fu

#ifdef DP_HOSTILE_STUBS
/* builtin.c and okplay are not in this exe: quiet stand-ins (no connections, boxes or passwords) */
void DP_BUILTIN_Attach( HINSTANCE hinst ) { (void)hinst; }
void DP_BUILTIN_Detach( void ) { }
void DP_BUILTIN_AddConnections( struct list *connections ) { (void)connections; }
BOOL DP_BUILTIN_IsOurs( const GUID *guid ) { (void)guid; return FALSE; }
BOOL DP_BUILTIN_Remap( GUID *sp, void **address, DWORD *size ) { (void)sp; (void)address; (void)size; return FALSE; }
BOOL DP_BUILTIN_ShowWindowsConnections( void ) { return FALSE; }
const DPLCONNECTION *DP_BUILTIN_TestLobby( BOOL unicode ) { (void)unicode; return NULL; }
BOOL DP_BUILTIN_AskHostPassword( const GUID *instance, WCHAR *hashed, DWORD size ) { (void)instance; (void)hashed; (void)size; return FALSE; }
HRESULT DP_BUILTIN_AskJoinPassword( const GUID *instance, WCHAR *hashed, DWORD size ) { (void)instance; (void)hashed; (void)size; return DP_OK; }
HRESULT DP_BUILTIN_CheckBuild( IDirectPlaySP *isp, const GUID *instance ) { (void)isp; (void)instance; return DP_OK; }
void DP_BUILTIN_JoinFailed( HRESULT hr ) { (void)hr; }
void okp_became_host( IDirectPlaySP *isp ) { (void)isp; }
int okp_drop_after_ms( void ) { return 15000; }
void okp_join_failed( IDirectPlaySP *isp, const char *step, HRESULT hr ) { (void)isp; (void)step; (void)hr; }
void okp_log( int level, const char *fmt, ... )   /* the dplayx notes (DP_NOTE), shown with the case */
{
    va_list a;
    (void)level;
    if ( !getenv( "DP_HOSTILE_NOTES" ) ) return;
    va_start( a, fmt );
    printf( "    note: " );
    vprintf( fmt, a );
    va_end( a );
}
#endif

#ifdef OKP_BUILTIN
/* what the dplayx code asks okplay (okplay/src/sp.c): here our own fake header and SP data */
unsigned long long okp_header_peer( const void *hdr, DWORD hdrsize )
{
    const struct fakehdr *h = hdr;
    return h && hdrsize == HDR && h->magic == FAKE_MAGIC ? h->peer : 0;
}
unsigned long long okp_spdata_peer( const void *data, DWORD size )
{
    unsigned long long p = 0;
    if ( data && size == 8 ) memcpy( &p, data, 8 );
    return p;
}
#endif

/* ------------------------------------------------------------------ guard page + crash report */
static BYTE *guard_base;
#define DATA_SPAN ( 4 * 4096 )
static const char *cur_case = "?";
static long cur_iter = -1;

static void *place( const void *src, DWORD n )
{
    BYTE *dst = guard_base + DATA_SPAN - n;
    memcpy( dst, src, n );
    return dst;
}

static LONG WINAPI on_crash( EXCEPTION_POINTERS *ep )
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    printf( "CRASH in %s (iteration %ld): exception 0x%08lx at %p", cur_case, cur_iter,
            (unsigned long)er->ExceptionCode, er->ExceptionAddress );
    if ( er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2 )
        printf( " (%s of %p)", er->ExceptionInformation[ 0 ] ? "write" : "read", (void *)er->ExceptionInformation[ 1 ] );
    printf( "\nFAIL %s\n", cur_case );
    fflush( stdout );
    ExitProcess( 1 );
    return EXCEPTION_EXECUTE_HANDLER;
}

/* ------------------------------------------------------------------ fake service provider */
static IDirectPlayImpl *cur;              /* the object under test */
static unsigned long long self_peer = 0x1111;
static unsigned long long host_peer = 0x2222;   /* when joining: the session's host */
static DPID last_reply_id;                /* the id in the last NEWPLAYERIDREPLY we sent */
/* when joining: what the "host" answers our requests with */
static BYTE *join_reply;
static DWORD join_reply_size;
static unsigned long long rogue_peer;     /* if set: a rogue NACK arrives first, from this peer */

static void mkhdr( struct fakehdr *h, unsigned long long peer )
{
    memset( h, 0, sizeof(*h) );
    h->magic = FAKE_MAGIC;
    h->peer = peer;
}

static HRESULT deliver( const void *body, DWORD n, unsigned long long peer )
{
    struct fakehdr h;
    mkhdr( &h, peer );
    return IDirectPlaySP_HandleMessage( cur->dp2->spData.lpISP, place( body, n ), n, &h );
}

static void envelope( void *p, WORD cmd )
{
    DPMSG_SENDENVELOPE *e = p;
    e->dwMagic = DPMSGMAGIC_DPLAYMSG;
    e->wCommandId = cmd;
    e->wVersion = DPMSGVER_DP6;
}

static HRESULT WINAPI fake_send( LPDPSP_SENDDATA d )
{
    const BYTE *m = (const BYTE *)d->lpMessage + HDR;
    DPMSG_SENDENVELOPE *e = (DPMSG_SENDENVELOPE *)m;
    if ( d->dwMessageSize < HDR + 8 || e->dwMagic != DPMSGMAGIC_DPLAYMSG ) return DP_OK;
    if ( e->wCommandId == DPMSGCMD_REQUESTNEWPLAYERID && join_reply )
    {
        DPMSG_NEWPLAYERIDREPLY r;
        memset( &r, 0, sizeof(r) );
        envelope( &r, DPMSGCMD_NEWPLAYERIDREPLY );
        r.dpidNewPlayerId = 0x300;
        deliver( &r, sizeof(r), host_peer );
    }
    else if ( e->wCommandId == DPMSGCMD_FORWARDADDPLAYER && join_reply )
    {
        if ( rogue_peer )
        {
            DPMSG_FORWARDADDPLAYERNACK nack;
            envelope( &nack, DPMSGCMD_FORWARDADDPLAYERNACK );
            nack.errorCode = DPERR_ACCESSDENIED;
            deliver( &nack, sizeof(nack), rogue_peer );
        }
        deliver( join_reply, join_reply_size, host_peer );
    }
    return DP_OK;
}

static HRESULT WINAPI fake_reply( LPDPSP_REPLYDATA d )
{
    const DPMSG_NEWPLAYERIDREPLY *r = (const void *)((const BYTE *)d->lpMessage + HDR);
    if ( d->dwMessageSize >= HDR + sizeof(*r) && r->envelope.wCommandId == DPMSGCMD_NEWPLAYERIDREPLY )
        last_reply_id = r->dpidNewPlayerId;
    return DP_OK;
}

static HRESULT WINAPI fake_enum( LPDPSP_ENUMSESSIONSDATA d ) { (void)d; return DP_OK; }

static HRESULT WINAPI fake_createplayer( LPDPSP_CREATEPLAYERDATA d )
{
    /* our own players carry our peer in their SP data, as okplay's provider does */
    if ( !d->lpSPMessageHeader )
        IDirectPlaySP_SetSPPlayerData( d->lpISP, d->idPlayer, &self_peer, 8, DPSET_REMOTE );
    return DP_OK;
}

static IDirectPlay4 *mk( void )
{
    IDirectPlay *dp1 = NULL;
    IDirectPlay4 *dp4 = NULL;
    if ( FAILED( DirectPlayCreate( (GUID *)&GUID_NULL, &dp1, NULL ) ) || !dp1 ) { printf( "DirectPlayCreate failed\n" ); exit( 2 ); }
    IDirectPlay_QueryInterface( dp1, &IID_IDirectPlay4, (void **)&dp4 );
    IDirectPlay_Release( dp1 );
    cur = CONTAINING_RECORD( dp4, IDirectPlayImpl, IDirectPlay4_iface );
    cur->dp2->spData.lpCB->EnumSessions = fake_enum;
    cur->dp2->spData.lpCB->Reply = fake_reply;
    cur->dp2->spData.lpCB->Send = fake_send;
    cur->dp2->spData.lpCB->CreatePlayer = fake_createplayer;
    cur->dp2->spData.dwSPHeaderSize = HDR;
    cur->dp2->bSPInitialized = TRUE;
    cur->dp2->connectionInitialized = DP_SERVICE_PROVIDER;
    return dp4;
}

static const GUID app = { 0x12345678, 0x1234, 0x1234, { 1, 2, 3, 4, 5, 6, 7, 8 } };
static const GUID inst = { 0x87654321, 0x4321, 0x4321, { 8, 7, 6, 5, 4, 3, 2, 1 } };
static DPID local_player;

static IDirectPlay4 *host( void )
{
    IDirectPlay4 *dp = mk();
    DPSESSIONDESC2 sd;
    DPNAME n;
    HRESULT hr;
    memset( &sd, 0, sizeof(sd) );
    sd.dwSize = sizeof(sd);
    sd.guidApplication = app;
    sd.dwMaxPlayers = 16;
    sd.lpszSessionName = (WCHAR *)L"hostile";
    if ( FAILED( hr = IDirectPlayX_Open( dp, &sd, DPOPEN_CREATE ) ) ) { printf( "host Open 0x%08lx\n", hr ); exit( 2 ); }
    memset( &n, 0, sizeof(n) );
    n.dwSize = sizeof(n);
    n.lpszShortName = (WCHAR *)L"local";
    if ( FAILED( hr = IDirectPlayX_CreatePlayer( dp, &local_player, &n, NULL, NULL, 0, 0 ) ) ) { printf( "CreatePlayer 0x%08lx\n", hr ); exit( 2 ); }
    return dp;
}

/* another machine in the session: its system player and one game player */
static void add_machine( DPID sys, DPID player, unsigned long long peer )
{
    struct fakehdr h;
    DPNAME n;
    mkhdr( &h, peer );
    memset( &n, 0, sizeof(n) );
    n.dwSize = sizeof(n);
    n.lpszShortName = (WCHAR *)L"remote";
    EnterCriticalSection( &cur->lock );
    DP_CreatePlayer( cur, &h, &sys, NULL, NULL, 0, &peer, 8, DPLAYI_PLAYER_SYSPLAYER, NULL, NULL, FALSE );
    if ( player )
    {
        DP_CreatePlayer( cur, &h, &player, &n, NULL, 0, &peer, 8, 0, NULL, NULL, FALSE );
        DP_SetPlayerSystemId( cur, player, sys );
    }
    LeaveCriticalSection( &cur->lock );
}

/* empty the receive queue; counts messages "from" id `from` */
static int drain( IDirectPlay4 *dp, DPID from )
{
    static BYTE buf[ 65536 ];
    int n = 0;
    for ( ;; )
    {
        DPID f = 0, t = 0;
        DWORD sz = sizeof(buf);
        if ( FAILED( IDirectPlayX_Receive( dp, &f, &t, 0, buf, &sz ) ) ) break;
        if ( f == from ) n++;
    }
    return n;
}

/* the same through DirectPlay 1's Receive (it converts the system messages) */
static void drain1( IDirectPlay4 *dp )
{
    static BYTE buf[ 65536 ];
    IDirectPlay *d1 = &cur->IDirectPlay_iface;
    int i;
    for ( i = 0; i < 10000; i++ )
    {
        DPID f = 0, t = 0;
        DWORD sz = sizeof(buf);
        if ( FAILED( IDirectPlay_Receive( d1, &f, &t, 0, buf, &sz ) ) ) break;
    }
    (void)dp;
}

static int result( int ok ) { printf( "%s %s\n", ok ? "PASS" : "FAIL", cur_case ); return ok ? 0 : 1; }

/* ------------------------------------------------------------------ joining */
static void session_reply( unsigned long long from )
{
    BYTE b[ sizeof(DPMSG_ENUMSESSIONSREPLY) + 32 ];
    DPMSG_ENUMSESSIONSREPLY *r = (void *)b;
    memset( b, 0, sizeof(b) );
    envelope( r, DPMSGCMD_ENUMSESSIONSREPLY );
    r->sd.dwSize = sizeof(DPSESSIONDESC2);
    r->sd.guidInstance = inst;
    r->sd.guidApplication = app;
    r->sd.dwMaxPlayers = 16;
    r->sd.dwCurrentPlayers = 1;
    memcpy( r + 1, L"hostile", 16 );
    deliver( b, sizeof(DPMSG_ENUMSESSIONSREPLY) + 16, from );
}

/* the host's player list: its system player (name server) with its peer as SP data */
static DWORD build_list( BYTE *b, DPID hostSys, unsigned long long peer, DWORD packedOffset )
{
    DPSP_MSG_SUPERENUMPLAYERSREPLY *r = (void *)b;
    DPLAYI_SUPERPACKEDPLAYER *p = (void *)(r + 1);
    BYTE *q = (BYTE *)(p + 1);
    memset( b, 0, sizeof(*r) + sizeof(*p) + 9 );
    envelope( r, DPMSGCMD_SUPERENUMPLAYERSREPLY );
    r->playerCount = 1;
    r->packedOffset = packedOffset ? packedOffset : sizeof(*r);
    p->size = sizeof(*p);
    p->flags = DPLAYI_PLAYER_SYSPLAYER | DPLAYI_PLAYER_NAMESRVR;
    p->id = hostSys;
    p->infoMask = 1 << 2;   /* SP data length: one byte */
    p->versionOrSystemPlayerId = DPMSGVER_DP6;
    *q++ = 8;
    memcpy( q, &peer, 8 );
    return sizeof(*r) + sizeof(*p) + 9;
}

static HRESULT join( IDirectPlay4 *dp )
{
    DPSESSIONDESC2 sd;
    memset( &sd, 0, sizeof(sd) );
    sd.dwSize = sizeof(sd);
    sd.guidInstance = inst;
    sd.guidApplication = app;
    return IDirectPlayX_Open( dp, &sd, DPOPEN_JOIN );
}

/* ------------------------------------------------------------------ cases */

/* the join reply's player list starts far outside the message */
static int c_joinoffset( void )
{
    static BYTE b[ 256 ];
    IDirectPlay4 *dp = mk();
    HRESULT hr;
    session_reply( host_peer );
    join_reply = b;
    join_reply_size = build_list( b, 0x200, host_peer, 0x7fffff00 );
    hr = join( dp );
    printf( "  Open(JOIN) with the list at offset 0x7fffff00: 0x%08lx (want a failure)\n", hr );
    return result( FAILED( hr ) );
}

/* a name length of one byte (no room for the terminator), at the very end of the message */
static int c_sizedstring( void )
{
    BYTE b[ 256 ];
    DPMSG_CREATESESSION *m = (void *)b;
    DPLAYI_PACKEDPLAYER *p = (void *)(m + 1);
    BYTE *q = (BYTE *)(p + 1);
    DWORD n;
    IDirectPlay4 *dp = host();
    add_machine( 0x50, 0x51, 0x5555 );
    memset( b, 0, sizeof(b) );
    envelope( m, DPMSGCMD_CREATESESSION );
    m->playerId = 0x52;
    p->size = sizeof(*p) + 1;
    p->id = 0x52;
    p->shortNameLength = 1;
    p->systemPlayerId = 0x50;
    p->fixedSize = sizeof(*p);
    p->version = DPMSGVER_DP6;
    memset( q, 'A', 7 );   /* one name byte, then the 6 that end a CREATESESSION - none 0 */
    n = (DWORD)(q + 7 - b);
    deliver( b, n, 0x5555 );
    drain( dp, 0 );
    printf( "  survived; player 0x52 %s\n", DP_PlayerExists( cur, 0x52 ) ? "created" : "not created" );
    return result( !DP_PlayerExists( cur, 0x52 ) );
}

/* a reply (player id) nobody asked for */
static int c_unsolicited( void )
{
    DPMSG_NEWPLAYERIDREPLY r;
    IDirectPlay4 *dp = host();
    memset( &r, 0, sizeof(r) );
    envelope( &r, DPMSGCMD_NEWPLAYERIDREPLY );
    r.dpidNewPlayerId = 5;
    deliver( &r, sizeof(r), 0x5555 );
    (void)dp;
    printf( "  survived\n" );
    return result( 1 );
}

/* an id request cut short after the envelope */
static int c_shortreq( void )
{
    DPMSG_SENDENVELOPE e;
    IDirectPlay4 *dp = host();
    envelope( &e, DPMSGCMD_REQUESTNEWPLAYERID );
    last_reply_id = 0;
    deliver( &e, sizeof(e), 0x5555 );
    (void)dp;
    printf( "  survived; answered with id 0x%08lx (want none)\n", last_reply_id );
    return result( last_reply_id == 0 );
}

/* a game message "from player 0" shaped as DPSYS_CREATEPLAYERORGROUP with a name pointer from the wire */
static int c_dp1sysmsg( void )
{
    BYTE b[ sizeof(DPMSG_SYSMSGENVELOPE) + sizeof(DPMSG_CREATEPLAYERORGROUP) ];
    DPMSG_SYSMSGENVELOPE *e = (void *)b;
    DPMSG_CREATEPLAYERORGROUP *c = (void *)(e + 1);
    IDirectPlay4 *dp = host();
    int from0;
    add_machine( 0x50, 0x51, 0x5555 );
    drain( dp, 0 );
    memset( b, 0, sizeof(b) );
    e->dwPlayerFrom = 0;
    e->dwPlayerTo = local_player;
    c->dwType = DPSYS_CREATEPLAYERORGROUP;
    c->dwPlayerType = DPPLAYERTYPE_PLAYER;
    c->dpId = 0x77;
    c->dpnName.dwSize = sizeof(DPNAME);
    c->dpnName.lpszLongName = (WCHAR *)(ULONG_PTR)0x10;
    c->dpnName.lpszShortName = (WCHAR *)(ULONG_PTR)0x7ffe0000;
    deliver( b, sizeof(b), 0x5555 );
    drain1( dp );            /* DirectPlay 1's Receive converts it (names!) */
    deliver( b, sizeof(b), 0x5555 );
    from0 = drain( dp, 0 );
    printf( "  survived; game messages queued as from player 0: %d (want 0)\n", from0 );
    return result( from0 == 0 );
}

/* with an id limit (gamefix.c), numbering starts again at 1: never on an id in use */
static int c_idwrap( void )
{
    DPMSG_REQUESTNEWPLAYERID q;
    IDirectPlay4 *dp;
    DPID p2 = 0, hostSys;
    DPNAME n;
    int i, ok = 1;
    DP_LimitObjectIds( 8 );
    dp = host();                  /* system player 1, local player 2 */
    memset( &n, 0, sizeof(n) );
    n.dwSize = sizeof(n);
    IDirectPlayX_CreatePlayer( dp, &p2, &n, NULL, NULL, 0, 0 );   /* 3 */
    hostSys = cur->dp2->systemPlayerId;
    memset( &q, 0, sizeof(q) );
    envelope( &q, DPMSGCMD_REQUESTNEWPLAYERID );
    for ( i = 0; i < 8; i++ )
    {
        last_reply_id = 0;
        deliver( &q, sizeof(q), 0x5555 );
        printf( "  request %d -> id %lu\n", i + 1, last_reply_id );
        if ( last_reply_id == hostSys || last_reply_id == local_player || last_reply_id == p2 || !last_reply_id )
            ok = 0;
    }
    printf( "  ids in use: %lu %lu %lu\n", hostSys, local_player, p2 );
    return result( ok );
}

/* "create player" for an id that exists, and a system player that is not its own machine */
static int c_dupcreate( void )
{
    BYTE b[ 256 ];
    DPMSG_CREATESESSION *m = (void *)b;
    DPLAYI_PACKEDPLAYER *p = (void *)(m + 1);
    DWORD n = sizeof(*m) + sizeof(*p) + 6;
    IDirectPlay4 *dp = host();
    int count = 0, before;
    DPID *ids;
    add_machine( 0x50, 0x51, 0x5555 );
    before = cur->dp2->lpSessionDesc->dwCurrentPlayers;
    memset( b, 0, sizeof(b) );
    envelope( m, DPMSGCMD_CREATESESSION );
    m->playerId = local_player;    /* an id in use */
    p->size = sizeof(*p);
    p->id = local_player;
    p->systemPlayerId = 0x50;
    p->fixedSize = sizeof(*p);
    p->version = DPMSGVER_DP6;
    deliver( b, n, 0x5555 );
    m->playerId = p->id = 0x60;    /* a "system player" announced for machine 0x50 */
    p->flags = DPLAYI_PLAYER_SYSPLAYER;
    deliver( b, n, 0x5555 );
    count = cur->dp2->lpSessionDesc->dwCurrentPlayers;
    printf( "  players %d -> %d (want no change); phantom system player 0x60 %s\n", before, count,
            DP_PlayerExists( cur, 0x60 ) ? "created" : "not created" );
    (void)ids; (void)dp;
    return result( count == before && !DP_PlayerExists( cur, 0x60 ) );
}

/* a machine sends game data as another machine's player */
static int c_gameowner( void )
{
#ifndef OKP_BUILTIN
    printf( "SKIP %s (needs the OKP_BUILTIN build)\n", cur_case );
    return 0;
#else
    BYTE b[ sizeof(DPMSG_SYSMSGENVELOPE) + 8 ];
    DPMSG_SYSMSGENVELOPE *e = (void *)b;
    IDirectPlay4 *dp = host();
    int forged, honest, unknown;
    add_machine( 0x50, 0x51, 0x5555 );
    add_machine( 0x70, 0, 0x7777 );
    drain( dp, 0 );
    memset( b, 0, sizeof(b) );
    e->dwPlayerTo = local_player;
    e->dwPlayerFrom = 0x51;
    deliver( b, sizeof(b), 0x7777 );     /* machine 0x70 sends as 0x50's player */
    forged = drain( dp, 0x51 );
    deliver( b, sizeof(b), 0x5555 );     /* its own machine */
    honest = drain( dp, 0x51 );
    e->dwPlayerFrom = 0x99;              /* a player nobody announced yet: let through */
    deliver( b, sizeof(b), 0x7777 );
    unknown = drain( dp, 0x99 );
    printf( "  forged %d (want 0), from its own machine %d (want 1), unknown player %d (want 1)\n", forged, honest, unknown );
    return result( forged == 0 && honest == 1 && unknown == 1 );
#endif
}

/* while we join, a third machine answers our join request first */
static int c_replysource( void )
{
#ifndef OKP_BUILTIN
    printf( "SKIP %s (needs the OKP_BUILTIN build)\n", cur_case );
    return 0;
#else
    static BYTE b[ 256 ];
    IDirectPlay4 *dp = mk();
    HRESULT hr;
    session_reply( host_peer );
    join_reply = b;
    join_reply_size = build_list( b, 0x200, host_peer, 0 );
    rogue_peer = 0x6666;
    hr = join( dp );
    printf( "  Open(JOIN) with a refusal from another machine first: 0x%08lx (want DP_OK), host 0x%08lx\n", hr,
            cur->dp2->nameServerId );
    return result( hr == DP_OK && cur->dp2->nameServerId == 0x200 );
#endif
}

/* ------------------------------------------------------------------ fuzz */
static unsigned long long rs = 88172645463325252ull;
static unsigned rnd( void ) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 11); }
static DWORD pick_dword( DWORD size )
{
    static const DWORD ids[] = { 0, 1, 2, 3, 0x50, 0x51, 0x70, 0x99, 0xffffffff, 0x7fffffff, 0x80000000, 8, 0x400 };
    switch ( rnd() % 6 )
    {
    case 0: return ids[ rnd() % (sizeof(ids) / sizeof(ids[0])) ];
    case 1: return local_player + rnd() % 3 - 1;
    case 2: return rnd() % (size + 16);              /* an offset near the message */
    case 3: return size - rnd() % 8;
    case 4: return rnd() % 64;
    default: return rnd() * 2654435761u;
    }
}

static const WORD cmds[] = { 1, 2, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 16, 19, 20, 21, 22, 23, 24, 26, 29, 30, 31,
                             32, 36, 38, 39, 40, 41, 45, 46, 47, 53, 1000, 1001 };

static void fuzz_one( DWORD maxlen )
{
    static BYTE b[ 4096 ];
    DWORD n = 8 + rnd() % maxlen, i;
    unsigned long long from = (rnd() & 1) ? 0x5555 : (rnd() & 1) ? 0x7777 : 0x9999;
    for ( i = 0; i < n; i++ ) b[ i ] = (BYTE)rnd();
    for ( i = 8; i + 4 <= n; i += 4 )
        if ( rnd() % 3 ) { DWORD v = pick_dword( n ); memcpy( b + i, &v, 4 ); }
    if ( rnd() % 8 )
        envelope( b, cmds[ rnd() % (sizeof(cmds) / sizeof(cmds[0])) ] );
    else
    {
        DWORD v = (rnd() & 1) ? 0 : pick_dword( n );   /* a game message: from, to */
        memcpy( b, &v, 4 );
        v = pick_dword( n );
        memcpy( b + 4, &v, 4 );
    }
    deliver( b, n, from );
}

static int c_fuzz( long iters )
{
    IDirectPlay4 *dp = NULL;
    for ( cur_iter = 0; cur_iter < iters; cur_iter++ )
    {
        if ( cur_iter % 300 == 0 )
        {
            if ( dp ) IDirectPlayX_Release( dp );
            dp = host();
            add_machine( 0x50, 0x51, 0x5555 );
            add_machine( 0x70, 0x71, 0x7777 );
        }
        fuzz_one( 200 );
        if ( rnd() & 1 ) drain1( dp ); else drain( dp, 0 );
    }
    printf( "  %ld messages\n", iters );
    return result( 1 );
}

static int c_fuzzjoin( long iters )
{
    static BYTE base[ 256 ], b[ 512 ];
    DWORD bn = build_list( base, 0x200, host_peer, 0 ), i;
    for ( cur_iter = 0; cur_iter < iters; cur_iter++ )
    {
        IDirectPlay4 *dp = mk();
        DPSP_MSG_SUPERENUMPLAYERSREPLY *r = (void *)b;
        DWORD n = bn + rnd() % 64;
        memcpy( b, base, bn );
        for ( i = bn; i < n; i++ ) b[ i ] = (BYTE)rnd();
        for ( i = 0; i < 4; i++ )
        {
            DWORD at = 8 + (rnd() % ((n - 8) / 4)) * 4, v = pick_dword( n );
            memcpy( b + at, &v, 4 );
        }
        if ( rnd() % 4 == 0 ) r->packedOffset = pick_dword( n );
        if ( rnd() % 4 == 0 ) r->playerCount = rnd() % 5;
        if ( rnd() % 4 == 0 ) r->groupCount = rnd() % 3;
        session_reply( host_peer );
        join_reply = b;
        join_reply_size = n;
        join( dp );
        drain1( dp );
        IDirectPlayX_Release( dp );
    }
    printf( "  %ld join replies\n", iters );
    return result( 1 );
}

int main( int argc, char **argv )
{
    long iters = argc > 2 ? atol( argv[ 2 ] ) : 3000;
    setvbuf( stdout, NULL, _IONBF, 0 );
    if ( argc > 3 ) rs = (unsigned long long)atol( argv[ 3 ] ) * 2654435761ull + 1;
    SetErrorMode( SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX );
    SetUnhandledExceptionFilter( on_crash );
    guard_base = VirtualAlloc( NULL, DATA_SPAN + 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
    { DWORD old; VirtualProtect( guard_base + DATA_SPAN, 4096, PAGE_NOACCESS, &old ); }
    if ( argc < 2 ) { printf( "usage: dp_hostile <case> [iterations] [seed]\n" ); return 2; }
    cur_case = argv[ 1 ];
    if ( !strcmp( cur_case, "joinoffset" ) ) return c_joinoffset();
    if ( !strcmp( cur_case, "sizedstring" ) ) return c_sizedstring();
    if ( !strcmp( cur_case, "unsolicited" ) ) return c_unsolicited();
    if ( !strcmp( cur_case, "shortreq" ) ) return c_shortreq();
    if ( !strcmp( cur_case, "dp1sysmsg" ) ) return c_dp1sysmsg();
    if ( !strcmp( cur_case, "idwrap" ) ) return c_idwrap();
    if ( !strcmp( cur_case, "dupcreate" ) ) return c_dupcreate();
    if ( !strcmp( cur_case, "gameowner" ) ) return c_gameowner();
    if ( !strcmp( cur_case, "replysource" ) ) return c_replysource();
    if ( !strcmp( cur_case, "fuzz" ) ) return c_fuzz( iters );
    if ( !strcmp( cur_case, "fuzzjoin" ) ) return c_fuzzjoin( iters );
    printf( "unknown case %s\n", cur_case );
    return 2;
}
