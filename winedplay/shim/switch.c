/*
 * mode=all: one connection list with this DLL's connections AND Windows' classic ones (TCP/IP,
 * IPX, modem, serial, lobby providers), each working as it always did.
 *
 * The game gets a "switcher" DirectPlay object. Until it picks a connection, every call goes to
 * this DLL's (Wine's) DirectPlay object, except EnumConnections, which lists ours first and then
 * Windows' (from a Windows DirectPlay object, duplicates and Windows' view of our GUIDs dropped).
 * InitializeConnection looks at the provider GUID in the connection it is handed:
 *   ours      -> Wine's object, as before
 *   any other -> a Windows DirectPlay object (system dplayx.dll) initializes it, and from then on
 *                every interface the game holds forwards to that object - so a classic game is
 *                Windows' DirectPlay end to end and talks to plain Windows players.
 *
 * Forwarding is done by thunks: each interface the switcher hands out is { vtable, target, owner }
 * and vtable slot n is a 5-instruction thunk that swaps `this` for `target` and jumps to slot n of
 * the target's vtable. The same thunks serve every IDirectPlay* interface (the slot means the same
 * method on both sides); only QueryInterface/AddRef/Release, EnumConnections and
 * InitializeConnection are the switcher's own. The Wine object is kept until the switcher is
 * released, so a call in flight on another thread during the switch still lands on a live object.
 *
 * Part of the Windows port for Classic Netplay for AoE2. LGPL 2.1+, like the rest of dplayx.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "objbase.h"
#include "dplay.h"
#include "dplobby.h"
#include "switch.h"

extern void okp_log( int level, const char *fmt, ... );
extern BOOL DP_BUILTIN_IsOurs( const GUID *guid );
extern BOOL DP_BUILTIN_Remap( GUID *sp, void **address, DWORD *size );
extern int  DP_BUILTIN_RemapMode( const GUID *sp );
extern BOOL DP_BUILTIN_Address( const GUID *sp, void **address, DWORD *size );
extern BOOL DP_BUILTIN_OurAddress( BOOL lan, void **address, DWORD *size );
extern BOOL DP_BUILTIN_IsLan( const GUID *sp );
extern void DP_BUILTIN_RemapForget( const GUID *sp );

#define SLOTS        53   /* IDirectPlay4's vtable; the older interfaces use a prefix of it */
#define SLOT_ENUMCONNECTIONS      35
#define SLOT_INITIALIZECONNECTION 38
/* where a game starts USING a connection (classic_*=ask is answered there, not at the probe) */
#define SLOT_ENUMSESSIONS   13
#define SLOT_OPEN           24
#define SLOT_SECUREOPEN     39
#define SLOT1_ENUMSESSIONS  14   /* IDirectPlay (DirectX 3) has its own method order */
#define SLOT1_OPEN          20

/* the thunks (i686, stdcall: `this` is at 4(%esp)) and a table of their addresses */
__asm__(
    ".text\n"
    ".macro SW_THUNK n\n"
    "_sw_thunk\\n:\n"
    "    movl 4(%esp), %ecx\n"      /* the switcher's interface */
    "    movl 4(%ecx), %ecx\n"      /* ->target */
    "    movl %ecx, 4(%esp)\n"      /* becomes `this` */
    "    movl (%ecx), %eax\n"       /* target's vtable */
    "    jmp *4*\\n(%eax)\n"
    ".endm\n"
    ".irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,"
    "32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52\n"
    "SW_THUNK \\n\n"
    ".endr\n"
    ".section .rdata,\"dr\"\n"
    ".p2align 2\n"
    ".globl _sw_thunks\n"
    "_sw_thunks:\n"
    ".irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,"
    "32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52\n"
    "    .long _sw_thunk\\n\n"
    ".endr\n"
    ".text\n" );
extern void *const sw_thunks[ SLOTS ];

/* ------------------------------------------------------------------ trace=1 */

/* okplay.ini trace=1: every DirectPlay call the game makes through the switcher is logged with its
 * arguments, result and duration - on Windows' DirectPlay (a classic connection) and on ours alike -
 * so one game session recorded on each can be compared line by line. The tracing thunk for slot n
 * first hands the caller's stack to sw_trace_enter, which keeps the return address and the arguments
 * on a per-thread stack and points the return address at sw_trace_return; that logs the HRESULT
 * (sw_trace_leave) and returns to the caller. Polling calls that come back empty (Receive ->
 * DPERR_NOMESSAGES, GetMessageCount, GetMessageQueue) are counted, not logged one by one. */
__asm__(
    ".text\n"
    ".macro SW_TRACE_THUNK n\n"
    "_sw_trace_thunk\\n:\n"
    "    pushl %ecx\n"
    "    pushl %edx\n"
    "    pushl $\\n\n"               /* the slot */
    "    leal 12(%esp), %eax\n"      /* the caller's return address; `this` and the arguments follow */
    "    pushl %eax\n"
    "    call _sw_trace_enter\n"
    "    addl $8, %esp\n"
    "    popl %edx\n"
    "    popl %ecx\n"
    "    movl 4(%esp), %ecx\n"      /* then as sw_thunk */
    "    movl 4(%ecx), %ecx\n"
    "    movl %ecx, 4(%esp)\n"
    "    movl (%ecx), %eax\n"
    "    jmp *4*\\n(%eax)\n"
    ".endm\n"
    ".irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,"
    "32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52\n"
    "SW_TRACE_THUNK \\n\n"
    ".endr\n"
    "_sw_trace_return:\n"           /* the method returned here with its HRESULT in eax */
    "    pushl %eax\n"
    "    pushl %ecx\n"
    "    pushl %edx\n"
    "    pushl %eax\n"
    "    call _sw_trace_leave\n"     /* logs; returns the caller's return address */
    "    addl $4, %esp\n"
    "    popl %edx\n"
    "    popl %ecx\n"
    "    xchgl %eax, (%esp)\n"       /* eax = the HRESULT again, the return address under it */
    "    ret\n"
    ".section .rdata,\"dr\"\n"
    ".p2align 2\n"
    ".globl _sw_trace_thunks\n"
    "_sw_trace_thunks:\n"
    ".irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,"
    "32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52\n"
    "    .long _sw_trace_thunk\\n\n"
    ".endr\n"
    ".text\n" );
extern void *const sw_trace_thunks[ SLOTS ];
extern void sw_trace_return( void );

static BOOL tracing;   /* the tracer's own side is further down, after the object it traces */
struct trace_stack;
static struct trace_stack *trace_stack( void );
static void trace_flush_quiet( struct trace_stack *st );
static const char *hr_name( HRESULT hr );

/* the interfaces a DirectPlay object has, in the order of struct sw's arrays */
enum { I_DP, I_DP2, I_DP2A, I_DP3, I_DP3A, I_DP4, I_DP4A, I_COUNT };
static const IID *const iids[ I_COUNT ] = { &IID_IDirectPlay, &IID_IDirectPlay2, &IID_IDirectPlay2A,
    &IID_IDirectPlay3, &IID_IDirectPlay3A, &IID_IDirectPlay4, &IID_IDirectPlay4A };
static const char *const iid_names[ I_COUNT ] = { "IDirectPlay", "IDirectPlay2", "IDirectPlay2A",
    "IDirectPlay3", "IDirectPlay3A", "IDirectPlay4", "IDirectPlay4A" };

struct sw;
struct sw_if
{
    void *const *vtbl;   /* must stay first... */
    IUnknown *target;    /* ...and this second: the thunks read it at offset 4 */
    struct sw *owner;
    int idx;
};

struct sw
{
    struct sw_if ifs[ I_COUNT ];
    IUnknown *wine[ I_COUNT ];   /* this DLL's object, per interface (a reference each) */
    IUnknown *win[ I_COUNT ];    /* Windows' object, once created */
    LONG ref;
    BOOL switched;               /* a classic connection was picked: targets are Windows' */
    BOOL pending;                /* classic_*=ask: initialised on a stand-in, the question still open */
    GUID pending_sp;             /* the classic provider the game named ... */
    void *pending_addr;          /* ... and its full address, for Windows' object if that is the answer */
    DWORD pending_size;
    BOOL answered;               /* an ask was answered for this object: forget it when the object goes */
    GUID answered_sp;
    CRITICAL_SECTION cs;
};

static void *vt_v1[ SLOTS ];     /* IDirectPlay (DirectX 3): its own method order */
static void *vt_old[ SLOTS ];    /* IDirectPlay2(A): no connections methods */
static void *vt_new[ SLOTS ];    /* IDirectPlay3(A) and 4(A) */

static create_t win_create;      /* Windows' DirectPlayCreate, NULL = no classic connections */

static inline struct sw_if *impl( void *iface ) { return iface; }
static HRESULT unk_qi( IUnknown *u, REFIID riid, void **out ) { return u->lpVtbl->QueryInterface( u, riid, out ); }
static ULONG unk_release( IUnknown *u ) { return u->lpVtbl->Release( u ); }

/* the provider GUID a connection (a DirectPlay compound address) names. The elements are
 * { GUID type, DWORD size, data }; the provider comes first, or right after a DPAID_TotalSize
 * element (whose DWORD bounds the walk) */
static BOOL provider_of( const void *connection, GUID *sp )
{
    const BYTE *p = connection;
    DWORD off = 0, total = 256;   /* without a TotalSize: providers are near the start */
    int n;

    if ( !connection ) return FALSE;
    for ( n = 0; n < 8 && off + sizeof(DPADDRESS) <= total; n++ )
    {
        const DPADDRESS *el = (const DPADDRESS *)(p + off);
        const BYTE *data = p + off + sizeof(DPADDRESS);
        if ( el->dwDataSize > total ) return FALSE;
        if ( IsEqualGUID( &el->guidDataType, &DPAID_TotalSize ) && el->dwDataSize == sizeof(DWORD) )
            total = *(const DWORD *)data;
        else if ( ( IsEqualGUID( &el->guidDataType, &DPAID_ServiceProvider ) ||
                    IsEqualGUID( &el->guidDataType, &DPAID_LobbyProvider ) ) && el->dwDataSize >= sizeof(GUID) )
        {
            memcpy( sp, data, sizeof(GUID) );
            return TRUE;
        }
        off += sizeof(DPADDRESS) + el->dwDataSize;
    }
    return FALSE;
}

/* how long a compound address is: its DPAID_TotalSize, else the elements we can walk */
static DWORD address_size( const void *connection )
{
    const BYTE *p = connection;
    DWORD off = 0;
    int n;
    for ( n = 0; n < 16 && off + sizeof(DPADDRESS) <= 1024; n++ )
    {
        const DPADDRESS *el = (const DPADDRESS *)(p + off);
        if ( IsEqualGUID( &el->guidDataType, &DPAID_TotalSize ) && el->dwDataSize == sizeof(DWORD) )
            return *(const DWORD *)(p + off + sizeof(DPADDRESS));
        if ( el->dwDataSize > 1024 ) break;
        off += sizeof(DPADDRESS) + el->dwDataSize;
        if ( IsEqualGUID( &el->guidDataType, &DPAID_ServiceProvider ) && n > 0 ) break;
    }
    return off ? off : 80;
}

static const char *guid_str( const GUID *g )
{
    static char buf[ 4 ][ 40 ];
    static LONG n;
    char *b = buf[ InterlockedIncrement( &n ) & 3 ];
    snprintf( b, 40, "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", g->Data1, g->Data2, g->Data3,
              g->Data4[ 0 ], g->Data4[ 1 ], g->Data4[ 2 ], g->Data4[ 3 ], g->Data4[ 4 ], g->Data4[ 5 ],
              g->Data4[ 6 ], g->Data4[ 7 ] );
    return b;
}

/* Windows' object, interface idx (created on first use; caller holds cs) */
static IUnknown *windows_if( struct sw *s, int idx )
{
    if ( !s->win[ I_DP ] )
    {
        IDirectPlay *dp = NULL;
        HRESULT hr = win_create( (GUID *)&GUID_NULL, &dp, NULL );
        if ( FAILED( hr ) || !dp )
        {
            okp_log( 0, "all: Windows' DirectPlayCreate failed 0x%08lx - classic connections unavailable", hr );
            return NULL;
        }
        s->win[ I_DP ] = (IUnknown *)dp;
    }
    if ( !s->win[ idx ] &&
         FAILED( unk_qi( s->win[ I_DP ], iids[ idx ], (void **)&s->win[ idx ] ) ) )
        s->win[ idx ] = NULL;
    return s->win[ idx ];
}

/* this DLL's object, interface idx (caller holds cs) */
static IUnknown *wine_if( struct sw *s, int idx )
{
    if ( !s->wine[ idx ] &&
         FAILED( unk_qi( s->wine[ I_DP ], iids[ idx ], (void **)&s->wine[ idx ] ) ) )
        s->wine[ idx ] = NULL;
    return s->wine[ idx ];
}

static HRESULT WINAPI sw_QueryInterface( void *iface, REFIID riid, void **out )
{
    struct sw *s = impl( iface )->owner;
    IUnknown *target;
    int i;

    if ( !out ) return E_POINTER;
    *out = NULL;
    if ( IsEqualIID( riid, &IID_IUnknown ) ) i = I_DP;
    else
    {
        for ( i = 0; i < I_COUNT; i++ ) if ( IsEqualIID( riid, iids[ i ] ) ) break;
        if ( i == I_COUNT ) return E_NOINTERFACE;
    }
    EnterCriticalSection( &s->cs );
    target = s->switched ? windows_if( s, i ) : wine_if( s, i );
    if ( target ) s->ifs[ i ].target = target;
    LeaveCriticalSection( &s->cs );
    if ( !target ) return E_NOINTERFACE;
    InterlockedIncrement( &s->ref );
    *out = &s->ifs[ i ];
    return S_OK;
}

static ULONG WINAPI sw_AddRef( void *iface )
{
    return InterlockedIncrement( &impl( iface )->owner->ref );
}

static ULONG WINAPI sw_Release( void *iface )
{
    struct sw *s = impl( iface )->owner;
    LONG ref = InterlockedDecrement( &s->ref );
    int i;

    if ( ref ) return ref;
    free( s->pending_addr );
    if ( s->answered ) DP_BUILTIN_RemapForget( &s->answered_sp );
    if ( tracing )
    {
        struct trace_stack *st = trace_stack();
        if ( st ) trace_flush_quiet( st );
        okp_log( 1, "trace: DirectPlay object released" );
    }
    /* Windows' first (it may be the one in a game), then ours; the base interface last */
    for ( i = I_COUNT - 1; i >= 0; i-- ) if ( s->win[ i ] ) unk_release( s->win[ i ] );
    for ( i = I_COUNT - 1; i >= 0; i-- ) if ( s->wine[ i ] ) unk_release( s->wine[ i ] );
    DeleteCriticalSection( &s->cs );
    free( s );
    return 0;
}

/* EnumConnections: ours, then Windows' */

typedef HRESULT (WINAPI *enumconn_t)( void *, const GUID *, LPDPENUMCONNECTIONSCALLBACK, void *, DWORD );
typedef HRESULT (WINAPI *initconn_t)( void *, void *, DWORD );

#define MAX_LISTED 64
struct enum_ctx
{
    LPDPENUMCONNECTIONSCALLBACK cb;
    void *ctx;
    BOOL stopped;
    GUID listed[ MAX_LISTED ];
    int n;
};

static BOOL CALLBACK enum_ours( const GUID *sp, void *conn, DWORD size, const DPNAME *name, DWORD flags, void *c )
{
    struct enum_ctx *e = c;
    if ( e->n < MAX_LISTED ) e->listed[ e->n++ ] = *sp;
    if ( !e->cb( sp, conn, size, name, flags, e->ctx ) ) e->stopped = TRUE;
    return !e->stopped;
}

static BOOL CALLBACK enum_windows( const GUID *sp, void *conn, DWORD size, const DPNAME *name, DWORD flags, void *c )
{
    struct enum_ctx *e = c;
    int i;
    if ( DP_BUILTIN_IsOurs( sp ) ) return TRUE;   /* an old okplay install registered with Windows */
    for ( i = 0; i < e->n; i++ ) if ( IsEqualGUID( &e->listed[ i ], sp ) ) return TRUE;
    if ( !e->cb( sp, conn, size, name, flags, e->ctx ) ) e->stopped = TRUE;
    return !e->stopped;
}

static HRESULT WINAPI sw_EnumConnections( void *iface, const GUID *app, LPDPENUMCONNECTIONSCALLBACK cb,
                                          void *ctx, DWORD flags )
{
    struct sw_if *i = impl( iface );
    struct sw *s = i->owner;
    struct enum_ctx e;
    IUnknown *target, *win;
    HRESULT hr;

    EnterCriticalSection( &s->cs );
    target = i->target;
    win = s->switched || !cb ? NULL : windows_if( s, i->idx );
    LeaveCriticalSection( &s->cs );

    if ( !win ) return ((enumconn_t)(*(void ***)target)[ SLOT_ENUMCONNECTIONS ])( target, app, cb, ctx, flags );

    memset( &e, 0, sizeof(e) );
    e.cb = cb;
    e.ctx = ctx;
    hr = ((enumconn_t)(*(void ***)target)[ SLOT_ENUMCONNECTIONS ])( target, app, enum_ours, &e, flags );
    if ( FAILED( hr ) || e.stopped ) return hr;
    ((enumconn_t)(*(void ***)win)[ SLOT_ENUMCONNECTIONS ])( win, app, enum_windows, &e, flags );
    return hr;   /* Windows' list is a bonus: its failure doesn't fail ours */
}

/* InitializeConnection: ours stays here, anything else moves the whole object to Windows' */
static HRESULT WINAPI sw_InitializeConnection( void *iface, void *connection, DWORD flags )
{
    struct sw_if *i = impl( iface );
    struct sw *s = i->owner;
    IUnknown *target, *win;
    HRESULT hr;
    GUID sp;
    int k;

    EnterCriticalSection( &s->cs );
    /* classic_*=ask: not now. Games probe every entry of their Multiplayer screen (create,
     * initialise, release) before the player picks one; a quiet stand-in - LAN Play, no server, nothing
     * sent - answers the probe, and the question is put when the game uses the connection
     * (EnumSessions / Open: sw_resolve_pending). */
    if ( !s->switched && !s->pending && provider_of( connection, &sp ) && DP_BUILTIN_RemapMode( &sp ) == 3 )
    {
        void *lan = NULL;
        DWORD lsz = 80;
        target = i->target;
        if ( DP_BUILTIN_OurAddress( TRUE, &lan, &lsz ) )
        {
            hr = ((initconn_t)(*(void ***)target)[ SLOT_INITIALIZECONNECTION ])( target, lan, flags );
            free( lan );
        }
        else hr = DPERR_UNAVAILABLE;
        if ( SUCCEEDED( hr ) )
        {
            s->pending_size = address_size( connection );
            if ( (s->pending_addr = malloc( s->pending_size )) )
            {
                memcpy( s->pending_addr, connection, s->pending_size );
                s->pending_sp = sp;
                s->pending = TRUE;
                okp_log( 1, "connections: the game set up its %s entry (classic_*=ask) - the question waits until it uses it",
                         guid_str( &sp ) );
            }
        }
        LeaveCriticalSection( &s->cs );
        return hr;
    }
    /* (DP_BUILTIN_Remap: a classic provider the ini hands to ours - it stays on this DLL's object,
     *  which swaps the address itself in InitializeConnection) */
    if ( s->switched || !provider_of( connection, &sp ) || DP_BUILTIN_Remap( &sp, NULL, NULL ) || DP_BUILTIN_IsOurs( &sp ) )
    {
        target = i->target;
        LeaveCriticalSection( &s->cs );
        hr = ((initconn_t)(*(void ***)target)[ SLOT_INITIALIZECONNECTION ])( target, connection, flags );
        if ( tracing )
            okp_log( 1, "trace: %s.InitializeConnection(flags=0x%lx) -> 0x%08lx %s [%s]", iid_names[ i->idx ], flags, hr,
                     hr_name( hr ), s->switched ? "Windows' DirectPlay" : "ours" );
        return hr;
    }

    if ( !(win = windows_if( s, i->idx )) )
    {
        LeaveCriticalSection( &s->cs );
        return DPERR_UNAVAILABLE;
    }
    hr = ((initconn_t)(*(void ***)win)[ SLOT_INITIALIZECONNECTION ])( win, connection, flags );
    if ( FAILED( hr ) )
    {
        LeaveCriticalSection( &s->cs );
        okp_log( 0, "all: classic connection %s - Windows' InitializeConnection failed 0x%08lx", guid_str( &sp ), hr );
        return hr;
    }
    /* every interface the game holds now points at Windows' object */
    for ( k = 0; k < I_COUNT; k++ )
    {
        if ( !s->ifs[ k ].target ) continue;
        if ( windows_if( s, k ) ) s->ifs[ k ].target = s->win[ k ];
        else okp_log( 0, "all: Windows' DirectPlay has no %s - that interface stays on ours", iid_names[ k ] );
    }
    s->switched = TRUE;
    LeaveCriticalSection( &s->cs );
    okp_log( 1, "all: classic connection %s - this game uses Windows' DirectPlay", guid_str( &sp ) );
    return hr;
}

/* ---- the tracer's own side (see the thunks above) */

static DWORD trace_tls = TLS_OUT_OF_INDEXES;

#define TRACE_DEPTH 16   /* calls nested on one thread (a callback calling DirectPlay again) */
#define TRACE_ARGS  8   /* SendEx has the most arguments worth showing: its timeout is the 7th */
struct trace_frame { void *ret; struct sw_if *iface; int slot; DWORD t0; DWORD args[ TRACE_ARGS ]; };
struct trace_stack { int depth; int quiet_slot; long quiet; struct trace_frame f[ TRACE_DEPTH ]; };

/* IDirectPlay (the DirectX 3 interface) has its own method order */
static const char *const slot_names_v1[] = { "QueryInterface", "AddRef", "Release", "AddPlayerToGroup",
    "Close", "CreatePlayer", "CreateGroup", "DeletePlayerFromGroup", "DestroyPlayer", "DestroyGroup",
    "EnableNewPlayers", "EnumGroupPlayers", "EnumGroups", "EnumPlayers", "EnumSessions", "GetCaps",
    "GetMessageCount", "GetPlayerCaps", "GetPlayerName", "Initialize", "Open", "Receive", "SaveSession",
    "Send", "SetPlayerName" };
/* IDirectPlay2/3/4: each a prefix of the next */
static const char *const slot_names[ SLOTS ] = { "QueryInterface", "AddRef", "Release", "AddPlayerToGroup",
    "Close", "CreateGroup", "CreatePlayer", "DeletePlayerFromGroup", "DestroyGroup", "DestroyPlayer",
    "EnumGroupPlayers", "EnumGroups", "EnumPlayers", "EnumSessions", "GetCaps", "GetGroupData", "GetGroupName",
    "GetMessageCount", "GetPlayerAddress", "GetPlayerCaps", "GetPlayerData", "GetPlayerName", "GetSessionDesc",
    "Initialize", "Open", "Receive", "Send", "SetGroupData", "SetGroupName", "SetPlayerData", "SetPlayerName",
    "SetSessionDesc", "AddGroupToGroup", "CreateGroupInGroup", "DeleteGroupFromGroup", "EnumConnections",
    "EnumGroupsInGroup", "GetGroupConnectionSettings", "InitializeConnection", "SecureOpen", "SendChatMessage",
    "SetGroupConnectionSettings", "StartSession", "GetGroupFlags", "GetGroupParent", "GetPlayerAccount",
    "GetPlayerFlags", "GetGroupOwner", "SetGroupOwner", "SendEx", "GetMessageQueue", "CancelMessage",
    "CancelPriority" };
enum { V2_CREATEPLAYER = 6, V2_DESTROYPLAYER = 9, V2_ENUMSESSIONS = 13, V2_GETCAPS = 14, V2_GETMESSAGECOUNT = 17,
       V2_OPEN = 24, V2_RECEIVE = 25, V2_SEND = 26, V2_SETSESSIONDESC = 31, V3_SECUREOPEN = 39, V4_SENDEX = 49,
       V4_GETMESSAGEQUEUE = 50 };
enum { V1_CREATEPLAYER = 5, V1_DESTROYPLAYER = 8, V1_ENUMSESSIONS = 14, V1_GETMESSAGECOUNT = 16, V1_OPEN = 20,
       V1_RECEIVE = 21, V1_SEND = 23 };

static const char *slot_name( const struct sw_if *i, int slot )
{
    if ( i->idx == I_DP ) return slot < (int)(sizeof(slot_names_v1) / sizeof(slot_names_v1[ 0 ])) ? slot_names_v1[ slot ] : "?";
    return slot < SLOTS ? slot_names[ slot ] : "?";
}

static const char *hr_name( HRESULT hr )
{
    static const struct { HRESULT hr; const char *name; } names[] = {
        { DP_OK, "DP_OK" }, { DPERR_ALREADYINITIALIZED, "DPERR_ALREADYINITIALIZED" },
        { DPERR_ACCESSDENIED, "DPERR_ACCESSDENIED" }, { DPERR_ACTIVEPLAYERS, "DPERR_ACTIVEPLAYERS" },
        { DPERR_BUFFERTOOSMALL, "DPERR_BUFFERTOOSMALL" }, { DPERR_CANTADDPLAYER, "DPERR_CANTADDPLAYER" },
        { DPERR_CANTCREATEGROUP, "DPERR_CANTCREATEGROUP" }, { DPERR_CANTCREATEPLAYER, "DPERR_CANTCREATEPLAYER" },
        { DPERR_CANTCREATESESSION, "DPERR_CANTCREATESESSION" }, { DPERR_CAPSNOTAVAILABLEYET, "DPERR_CAPSNOTAVAILABLEYET" },
        { DPERR_EXCEPTION, "DPERR_EXCEPTION" }, { DPERR_GENERIC, "DPERR_GENERIC" },
        { DPERR_INVALIDFLAGS, "DPERR_INVALIDFLAGS" }, { DPERR_INVALIDOBJECT, "DPERR_INVALIDOBJECT" },
        { DPERR_INVALIDPARAMS, "DPERR_INVALIDPARAMS" }, { DPERR_INVALIDPLAYER, "DPERR_INVALIDPLAYER" },
        { DPERR_INVALIDGROUP, "DPERR_INVALIDGROUP" }, { DPERR_NOCAPS, "DPERR_NOCAPS" },
        { DPERR_NOCONNECTION, "DPERR_NOCONNECTION" }, { DPERR_NOMEMORY, "DPERR_NOMEMORY" },
        { DPERR_NOMESSAGES, "DPERR_NOMESSAGES" }, { DPERR_NONAMESERVERFOUND, "DPERR_NONAMESERVERFOUND" },
        { DPERR_NOPLAYERS, "DPERR_NOPLAYERS" }, { DPERR_NOSESSIONS, "DPERR_NOSESSIONS" },
        { DPERR_PENDING, "DPERR_PENDING" }, { DPERR_SENDTOOBIG, "DPERR_SENDTOOBIG" },
        { DPERR_TIMEOUT, "DPERR_TIMEOUT" }, { DPERR_UNAVAILABLE, "DPERR_UNAVAILABLE" },
        { DPERR_UNSUPPORTED, "DPERR_UNSUPPORTED" }, { DPERR_BUSY, "DPERR_BUSY" },
        { DPERR_USERCANCEL, "DPERR_USERCANCEL" }, { DPERR_NOINTERFACE, "DPERR_NOINTERFACE" },
        { DPERR_CANNOTCREATESERVER, "DPERR_CANNOTCREATESERVER" }, { DPERR_PLAYERLOST, "DPERR_PLAYERLOST" },
        { DPERR_SESSIONLOST, "DPERR_SESSIONLOST" }, { DPERR_UNINITIALIZED, "DPERR_UNINITIALIZED" },
        { DPERR_NONEWPLAYERS, "DPERR_NONEWPLAYERS" }, { DPERR_INVALIDPASSWORD, "DPERR_INVALIDPASSWORD" },
        { DPERR_CONNECTING, "DPERR_CONNECTING" }, { DPERR_CONNECTIONLOST, "DPERR_CONNECTIONLOST" },
        { DPERR_UNKNOWNMESSAGE, "DPERR_UNKNOWNMESSAGE" }, { DPERR_CANCELFAILED, "DPERR_CANCELFAILED" },
        { DPERR_INVALIDPRIORITY, "DPERR_INVALIDPRIORITY" }, { DPERR_NOTHANDLED, "DPERR_NOTHANDLED" },
        { DPERR_CANCELLED, "DPERR_CANCELLED" }, { DPERR_ABORTED, "DPERR_ABORTED" },
        { DPERR_BUFFERTOOLARGE, "DPERR_BUFFERTOOLARGE" }, { DPERR_NOSERVICEPROVIDER, "DPERR_NOSERVICEPROVIDER" },
        { DPERR_NOTLOBBIED, "DPERR_NOTLOBBIED" }, { DPERR_SERVICEPROVIDERLOADED, "DPERR_SERVICEPROVIDERLOADED" },
        { DPERR_AUTHENTICATIONFAILED, "DPERR_AUTHENTICATIONFAILED" }, { DPERR_LOGONDENIED, "DPERR_LOGONDENIED" },
    };
    size_t n;
    for ( n = 0; n < sizeof(names) / sizeof(names[ 0 ]); n++ ) if ( names[ n ].hr == hr ) return names[ n ].name;
    return "";
}

static const char *sysmsg_name( DWORD type )
{
    switch ( type )
    {
    case DPSYS_CREATEPLAYERORGROUP: return "CREATEPLAYERORGROUP";
    case DPSYS_DESTROYPLAYERORGROUP: return "DESTROYPLAYERORGROUP";
    case DPSYS_ADDPLAYERTOGROUP: return "ADDPLAYERTOGROUP";
    case DPSYS_DELETEPLAYERFROMGROUP: return "DELETEPLAYERFROMGROUP";
    case DPSYS_SESSIONLOST: return "SESSIONLOST";
    case DPSYS_HOST: return "HOST";
    case DPSYS_SETPLAYERORGROUPDATA: return "SETPLAYERORGROUPDATA";
    case DPSYS_SETPLAYERORGROUPNAME: return "SETPLAYERORGROUPNAME";
    case DPSYS_SETSESSIONDESC: return "SETSESSIONDESC";
    case DPSYS_ADDGROUPTOGROUP: return "ADDGROUPTOGROUP";
    case DPSYS_DELETEGROUPFROMGROUP: return "DELETEGROUPFROMGROUP";
    case DPSYS_SECUREMESSAGE: return "SECUREMESSAGE";
    case DPSYS_STARTSESSION: return "STARTSESSION";
    case DPSYS_CHAT: return "CHAT";
    case DPSYS_SETGROUPOWNER: return "SETGROUPOWNER";
    case DPSYS_SENDCOMPLETE: return "SENDCOMPLETE";
    }
    return "?";
}

static struct trace_stack *trace_stack( void )
{
    struct trace_stack *st = TlsGetValue( trace_tls );
    if ( !st && (st = calloc( 1, sizeof(*st) )) )   /* one per thread, kept for the process' life (trace mode only) */
    {
        st->quiet_slot = -1;
        TlsSetValue( trace_tls, st );
    }
    return st;
}

static void trace_flush_quiet( struct trace_stack *st )
{
    if ( st->quiet > 0 )
        okp_log( 1, "trace: (%s x%ld - nothing waiting)", slot_names[ st->quiet_slot ], st->quiet );
    st->quiet = 0;
    st->quiet_slot = -1;
}

/* called by the thunk, cdecl: stack[ 0 ] = the caller's return address, [ 1 ] = `this`, [ 2.. ] = arguments */
void sw_trace_enter( DWORD *stack, int slot )
{
    struct trace_stack *st = trace_stack();
    struct trace_frame *f;
    if ( !st || st->depth >= TRACE_DEPTH ) return;   /* too deep: this call passes untraced */
    f = &st->f[ st->depth++ ];
    f->ret = (void *)stack[ 0 ];
    f->iface = (struct sw_if *)stack[ 1 ];
    f->slot = slot;
    f->t0 = GetTickCount();
    memcpy( f->args, stack + 2, sizeof(f->args) );
    stack[ 0 ] = (DWORD)sw_trace_return;
}

static DWORD deref( DWORD p ) { return p ? *(DWORD *)p : 0; }

/* called by sw_trace_return, cdecl: logs the call and gives back the caller's return address */
void *sw_trace_leave( HRESULT hr )
{
    struct trace_stack *st = trace_stack();
    struct trace_frame *f;
    char detail[ 200 ] = "";
    const struct sw_if *i;
    BOOL v1;
    int slot, quiet = 0;
    DWORD ms;

    if ( !st || !st->depth ) return NULL;   /* can't happen unless an exception skipped a frame */
    f = &st->f[ --st->depth ];
    i = f->iface;
    v1 = i->idx == I_DP;
    slot = f->slot;
    ms = GetTickCount() - f->t0;

    /* the polling calls: counted while they come back empty */
    if ( (slot == (v1 ? V1_RECEIVE : V2_RECEIVE) && hr == DPERR_NOMESSAGES) ||
         (slot == (v1 ? V1_GETMESSAGECOUNT : V2_GETMESSAGECOUNT) && SUCCEEDED( hr ) && deref( f->args[ 1 ] ) == 0) ||
         (!v1 && slot == V4_GETMESSAGEQUEUE && SUCCEEDED( hr )) )
        quiet = 1;
    if ( quiet )
    {
        int key = v1 ? (slot == V1_RECEIVE ? V2_RECEIVE : V2_GETMESSAGECOUNT) : slot;
        if ( st->quiet_slot != key ) trace_flush_quiet( st );
        st->quiet_slot = key;
        st->quiet++;
        return f->ret;
    }
    trace_flush_quiet( st );

    if ( slot == (v1 ? V1_OPEN : V2_OPEN) || (!v1 && slot == V3_SECUREOPEN) )
    {
        const DPSESSIONDESC2 *sd = (const DPSESSIONDESC2 *)f->args[ 0 ];
        snprintf( detail, sizeof(detail), "flags=0x%lx session.flags=0x%lx max=%lu", f->args[ 1 ],
                  sd ? sd->dwFlags : 0, sd ? sd->dwMaxPlayers : 0 );
    }
    else if ( slot == (v1 ? V1_ENUMSESSIONS : V2_ENUMSESSIONS) )
        snprintf( detail, sizeof(detail), "timeout=%lu flags=0x%lx", f->args[ 1 ], f->args[ 4 ] );
    else if ( slot == (v1 ? V1_SEND : V2_SEND) )
        snprintf( detail, sizeof(detail), "from=%lu to=%lu flags=0x%lx %lu bytes", f->args[ 0 ], f->args[ 1 ],
                  f->args[ 2 ], f->args[ 4 ] );
    else if ( !v1 && slot == V4_SENDEX )
        snprintf( detail, sizeof(detail), "from=%lu to=%lu flags=0x%lx %lu bytes prio=%lu timeout=%lu",
                  f->args[ 0 ], f->args[ 1 ], f->args[ 2 ], f->args[ 4 ], f->args[ 5 ], f->args[ 6 ] );
    else if ( slot == (v1 ? V1_RECEIVE : V2_RECEIVE) )
    {
        DWORD from = deref( f->args[ 0 ] ), to = deref( f->args[ 1 ] ), size = deref( f->args[ 4 ] );
        if ( SUCCEEDED( hr ) && from == DPID_SYSMSG && f->args[ 3 ] && size >= sizeof(DWORD) )
            snprintf( detail, sizeof(detail), "system message %s (0x%lx) %lu bytes", sysmsg_name( *(DWORD *)f->args[ 3 ] ),
                      *(DWORD *)f->args[ 3 ], size );
        else
            snprintf( detail, sizeof(detail), "flags=0x%lx from=%lu to=%lu %lu bytes", f->args[ 2 ], from, to, size );
    }
    else if ( slot == (v1 ? V1_CREATEPLAYER : V2_CREATEPLAYER) )
    {
        if ( v1 )   /* (id, short name, long name, event): no flags */
            snprintf( detail, sizeof(detail), "id=%lu", deref( f->args[ 0 ] ) );
        else
            snprintf( detail, sizeof(detail), "id=%lu flags=0x%lx", deref( f->args[ 0 ] ), f->args[ 5 ] );
    }
    else if ( slot == (v1 ? V1_DESTROYPLAYER : V2_DESTROYPLAYER) )
        snprintf( detail, sizeof(detail), "id=%lu", f->args[ 0 ] );
    else if ( !v1 && slot == V2_GETCAPS && SUCCEEDED( hr ) && f->args[ 0 ] )
    {
        const DPCAPS *c = (const DPCAPS *)f->args[ 0 ];
        snprintf( detail, sizeof(detail), "caps.flags=0x%lx maxbuffer=%lu maxplayers=%lu latency=%lu timeout=%lu",
                  c->dwFlags, c->dwMaxBufferSize, c->dwMaxPlayers, c->dwLatency, c->dwTimeout );
    }
    else if ( !v1 && slot == V2_SETSESSIONDESC && f->args[ 0 ] )
        snprintf( detail, sizeof(detail), "session.flags=0x%lx", ((const DPSESSIONDESC2 *)f->args[ 0 ])->dwFlags );
    else
        snprintf( detail, sizeof(detail), "0x%lx 0x%lx 0x%lx", f->args[ 0 ], f->args[ 1 ], f->args[ 2 ] );

    okp_log( 1, "trace: %s.%s(%s) -> 0x%08lx %s%s%lu ms [%s, thread %lx]", iid_names[ i->idx ], slot_name( i, slot ),
             detail, hr, hr_name( hr ), hr_name( hr )[ 0 ] ? " " : "", ms,
             i->owner->switched ? "Windows' DirectPlay" : "ours", GetCurrentThreadId() );
    return f->ret;
}

BOOL SW_Tracing( void )
{
    return tracing;
}

void SW_SetTrace( BOOL on )
{
    if ( !on ) return;
    if ( trace_tls == TLS_OUT_OF_INDEXES ) trace_tls = TlsAlloc();
    tracing = trace_tls != TLS_OUT_OF_INDEXES;
    okp_log( 1, "trace: on (okplay.ini trace=1) - every DirectPlay call the game makes is logged with its result" );
}

/* the open question of classic_*=ask, answered now: the game is about to use the connection */
static void sw_resolve_pending( struct sw *s, struct sw_if *i )
{
    void *addr = NULL;
    DWORD size = 80;
    GUID sp;
    BOOL ours;
    int k;

    EnterCriticalSection( &s->cs );
    if ( !s->pending ) { LeaveCriticalSection( &s->cs ); return; }
    sp = s->pending_sp;
    LeaveCriticalSection( &s->cs );   /* the box must not be shown with the lock held */
    ours = DP_BUILTIN_Remap( &sp, &addr, &size );
    EnterCriticalSection( &s->cs );
    if ( !s->pending ) { LeaveCriticalSection( &s->cs ); free( addr ); return; }
    s->pending = FALSE;
    s->answered = TRUE;
    s->answered_sp = s->pending_sp;
    if ( ours && DP_BUILTIN_IsLan( &sp ) )
        ;   /* the stand-in already is LAN Play */
    else if ( ours && addr )
    {
        /* Online Play: a fresh object of ours on that address replaces the stand-in */
        IDirectPlay *dp = NULL;
        IDirectPlay3A *dp3 = NULL;
        HRESULT hr = DirectPlayCreate( (GUID *)&GUID_NULL, &dp, NULL );
        if ( SUCCEEDED( hr ) ) hr = IDirectPlay_QueryInterface( dp, &IID_IDirectPlay3A, (void **)&dp3 );
        if ( SUCCEEDED( hr ) ) hr = IDirectPlayX_InitializeConnection( dp3, addr, 0 );
        if ( dp3 ) IDirectPlayX_Release( dp3 );
        if ( SUCCEEDED( hr ) )
        {
            for ( k = I_COUNT - 1; k >= 0; k-- ) if ( s->wine[ k ] ) { unk_release( s->wine[ k ] ); s->wine[ k ] = NULL; }
            s->wine[ I_DP ] = (IUnknown *)dp;
            for ( k = 0; k < I_COUNT; k++ ) if ( s->ifs[ k ].target ) s->ifs[ k ].target = wine_if( s, k );
        }
        else
        {
            if ( dp ) IDirectPlay_Release( dp );
            okp_log( 0, "connections: could not start Online Play for the game's entry (0x%08lx) - staying on LAN Play", hr );
        }
    }
    else
    {
        /* Windows' own: its object gets the address the game built, every interface moves over */
        IUnknown *win = windows_if( s, i->idx );
        HRESULT hr = win ? ((initconn_t)(*(void ***)win)[ SLOT_INITIALIZECONNECTION ])( win, s->pending_addr, 0 ) : DPERR_UNAVAILABLE;
        if ( SUCCEEDED( hr ) )
        {
            for ( k = 0; k < I_COUNT; k++ )
            {
                if ( !s->ifs[ k ].target ) continue;
                if ( windows_if( s, k ) ) s->ifs[ k ].target = s->win[ k ];
            }
            s->switched = TRUE;
            okp_log( 1, "connections: classic connection %s - this game uses Windows' DirectPlay", guid_str( &s->pending_sp ) );
        }
        else
            okp_log( 0, "connections: Windows' InitializeConnection failed 0x%08lx for the game's entry - staying on LAN Play", hr );
    }
    free( s->pending_addr );
    s->pending_addr = NULL;
    LeaveCriticalSection( &s->cs );
    free( addr );
}

typedef HRESULT (WINAPI *enumsess_t)( void *, void *, DWORD, void *, void *, DWORD );
typedef HRESULT (WINAPI *open_t)( void *, void *, DWORD );
typedef HRESULT (WINAPI *open1_t)( void *, void * );
typedef HRESULT (WINAPI *secureopen_t)( void *, void *, DWORD, void *, void * );

static HRESULT WINAPI sw_EnumSessions( void *iface, void *sd, DWORD timeout, void *cb, void *ctx, DWORD flags )
{
    struct sw_if *i = impl( iface );
    int slot = i->idx == I_DP ? SLOT1_ENUMSESSIONS : SLOT_ENUMSESSIONS;
    HRESULT hr;
    if ( i->owner->pending ) sw_resolve_pending( i->owner, i );
    hr = ((enumsess_t)(*(void ***)i->target)[ slot ])( i->target, sd, timeout, cb, ctx, flags );
    if ( tracing ) okp_log( 1, "trace: %s.EnumSessions(timeout=%lu flags=0x%lx) -> 0x%08lx %s [%s]", iid_names[ i->idx ], timeout, flags, hr,
                            hr_name( hr ), i->owner->switched ? "Windows' DirectPlay" : "ours" );
    return hr;
}

static HRESULT WINAPI sw_Open( void *iface, void *sd, DWORD flags )
{
    struct sw_if *i = impl( iface );
    int slot = i->idx == I_DP ? SLOT1_OPEN : SLOT_OPEN;
    HRESULT hr;
    if ( i->owner->pending ) sw_resolve_pending( i->owner, i );
    hr = ((open_t)(*(void ***)i->target)[ slot ])( i->target, sd, flags );
    if ( tracing ) okp_log( 1, "trace: %s.Open(flags=0x%lx) -> 0x%08lx %s [%s]", iid_names[ i->idx ], flags, hr, hr_name( hr ),
                            i->owner->switched ? "Windows' DirectPlay" : "ours" );
    return hr;
}

/* IDirectPlay::Open (DirectX 3) has one argument, the session desc - not DirectPlay 2's two. With
 * sw_Open in its slot every Open popped 4 bytes too many (stdcall), the game's stack pointer drifted
 * and its next calls pushed their arguments over its own locals: a CreatePlayer result the game read
 * as 0x006da073, a Receive that crashed (2026-10-22, only behind the switcher: trace=1, or a game
 * creating its DirectPlay 1 object with GUID_NULL while Windows' connections are listed) */
static HRESULT WINAPI sw_Open1( void *iface, void *sd )
{
    struct sw_if *i = impl( iface );
    HRESULT hr;
    if ( i->owner->pending ) sw_resolve_pending( i->owner, i );
    hr = ((open1_t)(*(void ***)i->target)[ SLOT1_OPEN ])( i->target, sd );
    if ( tracing ) okp_log( 1, "trace: %s.Open -> 0x%08lx %s [%s]", iid_names[ i->idx ], hr, hr_name( hr ),
                            i->owner->switched ? "Windows' DirectPlay" : "ours" );
    return hr;
}

static HRESULT WINAPI sw_SecureOpen( void *iface, void *sd, DWORD flags, void *security, void *credentials )
{
    struct sw_if *i = impl( iface );
    HRESULT hr;
    if ( i->owner->pending ) sw_resolve_pending( i->owner, i );
    hr = ((secureopen_t)(*(void ***)i->target)[ SLOT_SECUREOPEN ])( i->target, sd, flags, security, credentials );
    if ( tracing ) okp_log( 1, "trace: %s.SecureOpen(flags=0x%lx) -> 0x%08lx %s [%s]", iid_names[ i->idx ], flags, hr, hr_name( hr ),
                            i->owner->switched ? "Windows' DirectPlay" : "ours" );
    return hr;
}

static void build_vtables(void)
{
    int n;
    /* idempotent: racing callers write the same values */
    for ( n = 0; n < SLOTS; n++ ) vt_v1[ n ] = vt_old[ n ] = vt_new[ n ] = tracing ? sw_trace_thunks[ n ] : sw_thunks[ n ];
    vt_v1[ 0 ] = vt_old[ 0 ] = vt_new[ 0 ] = (void *)sw_QueryInterface;
    vt_v1[ 1 ] = vt_old[ 1 ] = vt_new[ 1 ] = (void *)sw_AddRef;
    vt_v1[ 2 ] = vt_old[ 2 ] = vt_new[ 2 ] = (void *)sw_Release;
    vt_v1[ SLOT1_ENUMSESSIONS ] = vt_old[ SLOT_ENUMSESSIONS ] = vt_new[ SLOT_ENUMSESSIONS ] = (void *)sw_EnumSessions;
    vt_v1[ SLOT1_OPEN ] = (void *)sw_Open1;
    vt_old[ SLOT_OPEN ] = vt_new[ SLOT_OPEN ] = (void *)sw_Open;
    vt_new[ SLOT_SECUREOPEN ] = (void *)sw_SecureOpen;
    vt_new[ SLOT_ENUMCONNECTIONS ] = (void *)sw_EnumConnections;
    vt_new[ SLOT_INITIALIZECONNECTION ] = (void *)sw_InitializeConnection;
}

void SW_SetWindows( create_t create )
{
    win_create = create;
}

/* a switcher over a fresh Wine DirectPlay object, as interface riid */
HRESULT SW_Create( REFIID riid, void **out )
{
    IDirectPlay *dp = NULL;
    HRESULT hr;

    if ( !out ) return DPERR_INVALIDPARAMS;
    *out = NULL;
    if ( !win_create ) return E_UNEXPECTED;
    hr = DirectPlayCreate( (GUID *)&GUID_NULL, &dp, NULL );
    if ( FAILED( hr ) ) return hr;
    return SW_Wrap( dp, riid, out );
}

/* a switcher over a Wine DirectPlay object made elsewhere (takes over the caller's reference) */
HRESULT SW_Wrap( IDirectPlay *dp, REFIID riid, void **out )
{
    struct sw *s;
    HRESULT hr;
    int k;

    if ( !out ) { IDirectPlay_Release( dp ); return DPERR_INVALIDPARAMS; }
    *out = NULL;
    if ( !(s = calloc( 1, sizeof(*s) )) )
    {
        IDirectPlay_Release( dp );
        return DPERR_OUTOFMEMORY;
    }
    build_vtables();
    InitializeCriticalSection( &s->cs );
    for ( k = 0; k < I_COUNT; k++ )
    {
        s->ifs[ k ].vtbl = k == I_DP ? vt_v1 : k <= I_DP2A ? vt_old : vt_new;
        s->ifs[ k ].owner = s;
        s->ifs[ k ].idx = k;
    }
    s->wine[ I_DP ] = (IUnknown *)dp;
    s->ref = 1;
    hr = sw_QueryInterface( &s->ifs[ I_DP ], riid, out );
    sw_Release( &s->ifs[ I_DP ] );
    return hr;
}

/* DirectPlayEnumerateA/W: ours, then Windows' */

struct denum_ctx
{
    void *cb;
    void *ctx;
    BOOL stopped;
    GUID listed[ MAX_LISTED ];
    int n;
};

static BOOL CALLBACK denum_ours_a( LPGUID sp, LPSTR name, DWORD major, DWORD minor, LPVOID c )
{
    struct denum_ctx *e = c;
    if ( e->n < MAX_LISTED ) e->listed[ e->n++ ] = *sp;
    if ( !((LPDPENUMDPCALLBACKA)e->cb)( sp, name, major, minor, e->ctx ) ) e->stopped = TRUE;
    return !e->stopped;
}

static BOOL CALLBACK denum_windows_a( LPGUID sp, LPSTR name, DWORD major, DWORD minor, LPVOID c )
{
    struct denum_ctx *e = c;
    int i;
    if ( DP_BUILTIN_IsOurs( sp ) ) return TRUE;
    for ( i = 0; i < e->n; i++ ) if ( IsEqualGUID( &e->listed[ i ], sp ) ) return TRUE;
    if ( !((LPDPENUMDPCALLBACKA)e->cb)( sp, name, major, minor, e->ctx ) ) e->stopped = TRUE;
    return !e->stopped;
}

static BOOL CALLBACK denum_ours_w( LPGUID sp, LPWSTR name, DWORD major, DWORD minor, LPVOID c )
{
    struct denum_ctx *e = c;
    if ( e->n < MAX_LISTED ) e->listed[ e->n++ ] = *sp;
    if ( !((LPDPENUMDPCALLBACKW)e->cb)( sp, name, major, minor, e->ctx ) ) e->stopped = TRUE;
    return !e->stopped;
}

static BOOL CALLBACK denum_windows_w( LPGUID sp, LPWSTR name, DWORD major, DWORD minor, LPVOID c )
{
    struct denum_ctx *e = c;
    int i;
    if ( DP_BUILTIN_IsOurs( sp ) ) return TRUE;
    for ( i = 0; i < e->n; i++ ) if ( IsEqualGUID( &e->listed[ i ], sp ) ) return TRUE;
    if ( !((LPDPENUMDPCALLBACKW)e->cb)( sp, name, major, minor, e->ctx ) ) e->stopped = TRUE;
    return !e->stopped;
}

HRESULT SW_EnumerateA( LPDPENUMDPCALLBACKA cb, LPVOID ctx, enum_a_t windows )
{
    struct denum_ctx e;
    HRESULT hr;
    if ( !cb ) return DPERR_INVALIDPARAMS;
    memset( &e, 0, sizeof(e) );
    e.cb = (void *)cb;
    e.ctx = ctx;
    hr = DirectPlayEnumerateA( denum_ours_a, &e );
    if ( SUCCEEDED( hr ) && !e.stopped && windows ) windows( denum_windows_a, &e );
    return hr;
}

HRESULT SW_EnumerateW( LPDPENUMDPCALLBACKW cb, LPVOID ctx, enum_w_t windows )
{
    struct denum_ctx e;
    HRESULT hr;
    if ( !cb ) return DPERR_INVALIDPARAMS;
    memset( &e, 0, sizeof(e) );
    e.cb = (void *)cb;
    e.ctx = ctx;
    hr = DirectPlayEnumerateW( denum_ours_w, &e );
    if ( SUCCEEDED( hr ) && !e.stopped && windows ) windows( denum_windows_w, &e );
    return hr;
}

/* CoCreateInstance-style creation (DllGetClassObject(CLSID_DirectPlay)) makes a switcher too */

static HRESULT WINAPI cf_QueryInterface( IClassFactory *cf, REFIID riid, void **out )
{
    if ( IsEqualIID( riid, &IID_IUnknown ) || IsEqualIID( riid, &IID_IClassFactory ) )
    {
        *out = cf;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI cf_AddRef( IClassFactory *cf ) { return 2; }
static ULONG WINAPI cf_Release( IClassFactory *cf ) { return 1; }
static HRESULT WINAPI cf_CreateInstance( IClassFactory *cf, IUnknown *outer, REFIID riid, void **out )
{
    if ( out ) *out = NULL;
    if ( outer ) return CLASS_E_NOAGGREGATION;
    return SW_Create( riid, out );
}
static HRESULT WINAPI cf_LockServer( IClassFactory *cf, BOOL lock ) { return S_OK; }

static const IClassFactoryVtbl cf_vtbl = { cf_QueryInterface, cf_AddRef, cf_Release, cf_CreateInstance,
                                           cf_LockServer };
static IClassFactory cf_dp = { (IClassFactoryVtbl *)&cf_vtbl };

HRESULT SW_GetClassObject( REFIID riid, void **out )
{
    return cf_QueryInterface( &cf_dp, riid, out );
}
