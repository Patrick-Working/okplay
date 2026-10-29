/*
 * Hosting: letting other machines join a session this dplayx created.
 *
 * Wine's dplayx could join a session hosted by Windows, but as a host it only answered session
 * enumeration and player-id requests; the join itself (FORWARDADDPLAYER) was a stub. This adds
 * the host side of the join, in the message formats Windows uses (as captured in Wine's own
 * conformance tests, dlls/dplayx/tests/dplayx.c):
 *
 *   joiner -> host   FORWARDADDPLAYER (19): the joiner's system player, packed, + password
 *   host  -> others  ADDFORWARD (46): the same player, so every machine knows the newcomer
 *   host  -> joiner  SUPERENUMPLAYERSREPLY (41): session description, name and every player and
 *                    group ("super packed"), or FORWARDADDPLAYERNACK (36) with an error
 *
 * Part of the Windows port for Classic Netplay for AoE2. LGPL 2.1+, like the rest of dplayx.
 */

#include <stdarg.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "wine/debug.h"

#include <wchar.h>
#include "dplay_global.h"
#include "dplayx_global.h"
#include "dplayx_messages.h"

WINE_DEFAULT_DEBUG_CHANNEL(dplay);

/* the player/protocol version Windows (DirectX 7+) puts in these messages */
#define HOST_WIRE_VERSION 14

/* growable reply buffer */
struct buf
{
    char *data;
    DWORD size, cap;
    BOOL failed;
};

static void *buf_add( struct buf *b, const void *src, DWORD len )
{
    char *p;
    if ( b->failed ) return NULL;
    if ( b->size + len > b->cap )
    {
        DWORD cap = max( b->cap * 2, b->size + len + 256 );
        char *n = realloc( b->data, cap );
        if ( !n ) { b->failed = TRUE; return NULL; }
        b->data = n;
        b->cap = cap;
    }
    p = b->data + b->size;
    if ( src ) memcpy( p, src, len );
    else memset( p, 0, len );
    b->size += len;
    return p;
}

/* smallest size code (1: BYTE, 2: WORD, 3: DWORD; 0: value is 0 and not sent) */
static DWORD size_code( DWORD value )
{
    return !value ? 0 : value <= 0xff ? 1 : value <= 0xffff ? 2 : 3;
}

static void add_int( struct buf *b, DWORD code, DWORD value )
{
    BYTE b1 = (BYTE)value;
    WORD w = (WORD)value;
    if ( code == 1 ) buf_add( b, &b1, 1 );
    else if ( code == 2 ) buf_add( b, &w, 2 );
    else if ( code == 3 ) buf_add( b, &value, 4 );
}

static void add_wstr( struct buf *b, const WCHAR *s )
{
    buf_add( b, s, (lstrlenW( s ) + 1) * sizeof(WCHAR) );
}

/* one DPLAYI_SUPERPACKEDPLAYER entry, as Windows sends it */
static void add_super_packed( IDirectPlayImpl *This, struct buf *b, DPID id, DWORD flags,
                              const DPNAME *name, const void *data, DWORD dataSize,
                              DPID systemPlayerId, const DPID *members, DWORD memberCount,
                              DPID parentId, BOOL group )
{
    DPLAYI_SUPERPACKEDPLAYER sp;
    void *spData = NULL;
    DWORD spDataSize = 0, mask = 0;
    BOOL shortName = name && name->lpszShortName && name->lpszShortName[ 0 ];
    BOOL longName = name && name->lpszLongName && name->lpszLongName[ 0 ];

    if ( !group )
        IDirectPlaySP_GetSPPlayerData( This->dp2->spData.lpISP, id, &spData, &spDataSize, DPSET_REMOTE );

    if ( shortName ) mask |= DPLAYI_SUPERPACKEDPLAYER_SHORT_NAME_PRESENT;
    if ( longName ) mask |= DPLAYI_SUPERPACKEDPLAYER_LONG_NAME_PRESENT;
    mask |= size_code( spDataSize ) << 2;
    mask |= size_code( dataSize ) << 4;
    mask |= size_code( memberCount ) << 6;
    if ( parentId ) mask |= DPLAYI_SUPERPACKEDPLAYER_PARENT_ID_PRESENT;

    sp.size = 16;
    sp.flags = flags;
    sp.id = id;
    sp.infoMask = mask;
    sp.versionOrSystemPlayerId = ( flags & DPLAYI_PLAYER_SYSPLAYER ) ? HOST_WIRE_VERSION : systemPlayerId;
    buf_add( b, &sp, sizeof(sp) );
    if ( shortName ) add_wstr( b, name->lpszShortName );
    if ( longName ) add_wstr( b, name->lpszLongName );
    add_int( b, size_code( dataSize ), dataSize );
    if ( dataSize ) buf_add( b, data, dataSize );
    add_int( b, size_code( spDataSize ), spDataSize );
    if ( spDataSize ) buf_add( b, spData, spDataSize );
    add_int( b, size_code( memberCount ), memberCount );
    if ( memberCount ) buf_add( b, members, memberCount * sizeof(DPID) );
    if ( parentId ) buf_add( b, &parentId, sizeof(DPID) );
}

static HRESULT nack( IDirectPlayImpl *This, HRESULT error, void **reply, DWORD *replySize )
{
    DWORD header = This->dp2->spData.dwSPHeaderSize;
    DPSP_MSG_ADDFORWARDREPLY *msg;

    WARN( "refusing the join: %s\n", DPLAYX_HresultToString( error ) );
    *replySize = header + sizeof(*msg);
    if ( !(*reply = calloc( 1, *replySize )) )
        return DPERR_OUTOFMEMORY;
    msg = (DPSP_MSG_ADDFORWARDREPLY *)((char *)*reply + header);
    msg->envelope.dwMagic = DPMSGMAGIC_DPLAYMSG;
    msg->envelope.wCommandId = DPMSGCMD_FORWARDADDPLAYERNACK;
    msg->envelope.wVersion = HOST_WIRE_VERSION;
    msg->error = error;
    return DP_OK;
}

/* tell one other machine about the newcomer (it answers with ADDFORWARDACK) */
static void send_add_forward( IDirectPlayImpl *This, DPID to, const DPMSG_FORWARDADDPLAYER *request,
                              const void *packed, DWORD packedSize )
{
    DPSP_MSG_ADDFORWARD msg;
    SGBUFFER buffers[ 3 ] = { 0 };
    DPSP_SENDEXDATA data = { 0 };
    HRESULT hr;

    msg.envelope.dwMagic = DPMSGMAGIC_DPLAYMSG;
    msg.envelope.wCommandId = DPMSGCMD_ADDFORWARD;
    msg.envelope.wVersion = HOST_WIRE_VERSION;
    msg.toId = 0;
    msg.playerId = request->playerId;
    msg.groupId = 0;
    msg.createOffset = sizeof(msg);
    msg.passwordOffset = 0;

    buffers[ 0 ].len = This->dp2->spData.dwSPHeaderSize;
    buffers[ 1 ].len = sizeof(msg);
    buffers[ 1 ].pData = (UCHAR *)&msg;
    buffers[ 2 ].len = packedSize;
    buffers[ 2 ].pData = (UCHAR *)packed;

    data.lpISP = This->dp2->spData.lpISP;
    data.dwFlags = DPSEND_GUARANTEED;
    data.idPlayerTo = to;
    data.idPlayerFrom = This->dp2->systemPlayerId;
    data.lpSendBuffers = buffers;
    data.cBuffers = ARRAYSIZE( buffers );
    data.dwMessageSize = DP_MSG_ComputeMessageSize( buffers, ARRAYSIZE( buffers ) );
    data.bSystemMessage = TRUE;
    hr = DP_SP_SendEx( This, &data );
    TRACE( "ADDFORWARD 0x%08lx -> machine 0x%08lx: %s\n", request->playerId, to, DPLAYX_HresultToString( hr ) );
}

HRESULT DP_HOST_HandleForwardAddPlayer( IDirectPlayImpl *This, void *body, DWORD bodySize,
                                        void *header, void **reply, DWORD *replySize )
{
    const DPMSG_FORWARDADDPLAYER *request = body;
    DPSP_MSG_SUPERENUMPLAYERSREPLY *out;
    DPSESSIONDESC2 *sd, sdCopy;
    struct PlayerList *plist;
    struct GroupList *glist;
    struct buf b = { 0 };
    DPPLAYERINFO info;
    const WCHAR *password = NULL;
    DWORD offset, packedStart, headerSize = This->dp2->spData.dwSPHeaderSize;
    DWORD players = 0, groups = 0;
    HRESULT hr;

    if ( bodySize < sizeof(*request) || request->createOffset < sizeof(*request) || request->createOffset > bodySize )
        return DPERR_GENERIC;
    offset = request->createOffset;
    packedStart = offset;
    hr = DP_MSG_ReadPackedPlayer( (char *)body, &offset, bodySize, &info );
    if ( FAILED( hr ) )
        return hr;
    if ( request->passwordOffset && request->passwordOffset < bodySize &&
         (bodySize - request->passwordOffset) >= sizeof(WCHAR) &&
         wmemchr( (const WCHAR *)((const char *)body + request->passwordOffset), 0,
                  (bodySize - request->passwordOffset) / sizeof(WCHAR) ) )
        password = (const WCHAR *)((const char *)body + request->passwordOffset);

    EnterCriticalSection( &This->lock );
    if ( !This->dp2->bConnectionOpen || !This->dp2->bHostInterface )
    {
        LeaveCriticalSection( &This->lock );
        WARN( "join request, but this interface is not hosting\n" );
        return DP_OK;
    }
    sd = This->dp2->lpSessionDesc;
    TRACE( "join request from system player 0x%08lx (flags 0x%08lx, %lu bytes SP data)\n",
           request->playerId, info.flags, info.spDataLength );

    /* who may join */
    if ( sd->dwFlags & (DPSESSION_JOINDISABLED | DPSESSION_NEWPLAYERSDISABLED) )
    {
        LeaveCriticalSection( &This->lock );
        return nack( This, DPERR_CANTCREATEPLAYER, reply, replySize );
    }
    /* the session desc is stored WCHAR, as the password arrives */
    if ( sd->lpszPassword && sd->lpszPassword[ 0 ] && (!password || lstrcmpW( sd->lpszPassword, password )) )
    {
        LeaveCriticalSection( &This->lock );
        return nack( This, DPERR_INVALIDPASSWORD, reply, replySize );
    }
    if ( !(info.flags & DPLAYI_PLAYER_SYSPLAYER) || info.id != request->playerId )
    {
        LeaveCriticalSection( &This->lock );
        return nack( This, DPERR_INVALIDPARAMS, reply, replySize );
    }

    /* the joining machine */
    if ( DP_PlayerExists( This, request->playerId ) )
    {
        /* an id already in the session: only its own machine may ask again (a resent request). From
         * anyone else it is an attempt to take an existing player over, so refuse it. */
        BOOL known;
        DPID sender = DP_SenderMachine( This, header, &known );
        if ( known && sender != request->playerId )
        {
            LeaveCriticalSection( &This->lock );
            WARN( "join for existing player 0x%08lx from machine 0x%08lx refused\n", request->playerId, sender );
            return nack( This, DPERR_INVALIDPARAMS, reply, replySize );
        }
    }
    else
    {
        DPID id = request->playerId;
        hr = DP_CreatePlayer( This, header, &id, &info.name, info.playerData, info.playerDataLength,
                              info.spData, info.spDataLength, info.flags & ~DPLAYI_PLAYER_PLAYERLOCAL,
                              NULL, NULL, FALSE );
        if ( FAILED( hr ) )
        {
            LeaveCriticalSection( &This->lock );
            return nack( This, hr, reply, replySize );
        }
        DP_SetPlayerSystemId( This, id, id );
    }

    /* every other machine learns about it */
    for ( plist = DPQ_FIRST( This->dp2->lpSysGroup->players ); plist; plist = DPQ_NEXT( plist->players ) )
    {
        struct PlayerData *p = plist->lpPData;
        if ( !(p->dwFlags & DPLAYI_PLAYER_SYSPLAYER) || (p->dwFlags & DPLAYI_PLAYER_PLAYERLOCAL) ||
             p->dpid == request->playerId )
            continue;
        send_add_forward( This, p->dpid, request, (const char *)body + packedStart, offset - packedStart );
    }

    /* the reply: everything the joiner needs to build its copy of the session */
    buf_add( &b, NULL, headerSize );
    buf_add( &b, NULL, sizeof(*out) );
    sdCopy = *sd;
    sdCopy.lpszSessionName = NULL;
    sdCopy.lpszPassword = NULL;
    buf_add( &b, &sdCopy, sizeof(sdCopy) );
    add_wstr( &b, sd->lpszSessionName ? sd->lpszSessionName : L"" );   /* byte for byte, as Windows */
    {
        DWORD packedOffset = b.size - headerSize;
        struct PlayerData *order[ 256 ];
        int count = 0, pass, k;

        /* Windows' order (S3 tap, 2026-10-16): the system players newest first - the machine
         * joining now leads - then the game players newest first (five players: 4 2 0 3 1) */
        for ( plist = DPQ_FIRST( This->dp2->lpSysGroup->players ); plist && count < ARRAYSIZE( order );
              plist = DPQ_NEXT( plist->players ) )
            order[ count++ ] = plist->lpPData;
        for ( pass = 0; pass < 2; pass++ )
            for ( k = count - 1; k >= 0; k-- )
            {
                struct PlayerData *p = order[ k ];
                /* every player is in the system group: Windows marks each record so (S3 tap: its
                 * joiners create the host's system player with 0x7 and its game player with 0x4;
                 * ours, without the bit, created them with 0x3 and 0x0) */
                DWORD flags = p->dwFlags | DPLAYI_PLAYER_PLAYERINGROUP;
                if ( !!(p->dwFlags & DPLAYI_PLAYER_SYSPLAYER) != !pass )
                    continue;
                if ( p->dpid == This->dp2->systemPlayerId )
                    flags |= DPLAYI_PLAYER_NAMESRVR;
                add_super_packed( This, &b, p->dpid, flags, p->name, p->lpRemoteData, p->dwRemoteDataSize,
                                  p->systemPlayerId, NULL, 0, 0, FALSE );
                players++;
            }
        for ( glist = DPQ_FIRST( This->dp2->lpSysGroup->groups ); glist; glist = DPQ_NEXT( glist->groups ) )
        {
            struct GroupData *g = glist->lpGData;
            DPID ids[ 256 ];
            DWORD n = 0;
            for ( plist = DPQ_FIRST( g->players ); plist && n < ARRAYSIZE( ids ); plist = DPQ_NEXT( plist->players ) )
                ids[ n++ ] = plist->lpPData->dpid;
            add_super_packed( This, &b, g->dpid, g->dwFlags, g->name, g->lpRemoteData, g->dwRemoteDataSize,
                              This->dp2->systemPlayerId, ids, n, g->parent, TRUE );
            groups++;
        }
        if ( b.failed )
        {
            LeaveCriticalSection( &This->lock );
            free( b.data );
            return DPERR_OUTOFMEMORY;
        }
        out = (DPSP_MSG_SUPERENUMPLAYERSREPLY *)(b.data + headerSize);
        out->envelope.dwMagic = DPMSGMAGIC_DPLAYMSG;
        out->envelope.wCommandId = DPMSGCMD_SUPERENUMPLAYERSREPLY;
        out->envelope.wVersion = HOST_WIRE_VERSION;
        out->playerCount = players;
        out->groupCount = groups;
        out->packedOffset = packedOffset;
        out->shortcutCount = 0;
        out->descriptionOffset = sizeof(*out);
        out->nameOffset = sizeof(*out) + sizeof(sdCopy);
        out->passwordOffset = 0;
    }
    LeaveCriticalSection( &This->lock );

    TRACE( "join of 0x%08lx accepted: %lu players, %lu groups, %lu bytes\n", request->playerId, players,
           groups, b.size - headerSize );
    *reply = b.data;
    *replySize = b.size;
    return DP_OK;
}
