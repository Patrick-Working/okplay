/*
 * Session lifecycle: leaving, keepalive and host migration.
 *
 * Wine's dplayx never told anyone when a player or machine went away, never noticed a machine that
 * vanished, and had no host migration ("FIXME: Need to find a new host I assume (how?)"). Here:
 *
 *  leaving    Close() sends DELETEPLAYER (11) for each local player and then the system player;
 *             DestroyPlayer() sends it for that player. Receivers remove the player and queue
 *             DPSYS_DESTROYPLAYERORGROUP for the application.
 *  keepalive  with DPSESSION_KEEPALIVE, every machine pings every other machine's system player
 *             (PING 22 / PINGREPLY 23) every 1.5 s; any message from a machine counts as a sign of
 *             life. 15 s of silence and the machine is gone: its players are removed.
 *  migration  when the host's machine goes and the session has DPSESSION_MIGRATEHOST, the
 *             remaining machine with the lowest system player id becomes the host - every machine
 *             computes the same answer from the same player list. The new host gets DPSYS_HOST,
 *             announces itself (IAMNAMESERVER 53) and answers name-server requests from then on.
 *             Without MIGRATEHOST the session is lost (DPSYS_SESSIONLOST).
 *
 * Part of the Windows port for Classic Netplay for AoE2. LGPL 2.1+, like the rest of dplayx.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "wine/debug.h"

#include "dplay_global.h"
#include "dplayx_global.h"
#include "dplayx_messages.h"

#ifdef OKP_BUILTIN
void okp_became_host( IDirectPlaySP *isp );   /* okplay/src/sp.c */
int okp_drop_after_ms( void );                /* okplay.ini drop_after= (okplay/src/okp.h) */
#define KEEPALIVE_DEAD_NOW() ((DWORD)okp_drop_after_ms())
#else
#define KEEPALIVE_DEAD_NOW() KEEPALIVE_DEAD_MS
#endif

WINE_DEFAULT_DEBUG_CHANNEL(dplay);

#define KEEPALIVE_PING_MS  1500
#define KEEPALIVE_DEAD_MS 15000   /* Windows notices in ~14.5 s */

static struct PlayerData *find_player( IDirectPlayImpl *This, DPID id )
{
    struct PlayerList *plist;
    if ( !This->dp2->lpSysGroup ) return NULL;
    for ( plist = DPQ_FIRST( This->dp2->lpSysGroup->players ); plist; plist = DPQ_NEXT( plist->players ) )
        if ( plist->lpPData->dpid == id ) return plist->lpPData;
    return NULL;
}

static HRESULT send_system( IDirectPlayImpl *This, DPID to, void *msg, DWORD size, const void *extra,
                            DWORD extraSize, DWORD flags )
{
    SGBUFFER buffers[ 3 ] = { 0 };
    DPSP_SENDEXDATA data = { 0 };

    buffers[ 0 ].len = This->dp2->spData.dwSPHeaderSize;
    buffers[ 1 ].len = size;
    buffers[ 1 ].pData = msg;
    buffers[ 2 ].len = extraSize;
    buffers[ 2 ].pData = (UCHAR *)extra;
    data.lpISP = This->dp2->spData.lpISP;
    data.dwFlags = flags;
    data.idPlayerTo = to;
    data.idPlayerFrom = This->dp2->systemPlayerId;
    data.lpSendBuffers = buffers;
    data.cBuffers = extraSize ? 3 : 2;
    data.dwMessageSize = DP_MSG_ComputeMessageSize( buffers, data.cBuffers );
    data.bSystemMessage = TRUE;
    return DP_SP_SendEx( This, &data );
}

/* one message to every other machine */
void DP_SendToAllMachines( IDirectPlayImpl *This, void *msg, DWORD size, const void *extra, DWORD extraSize )
{
    struct PlayerList *plist;
    for ( plist = DPQ_FIRST( This->dp2->lpSysGroup->players ); plist; plist = DPQ_NEXT( plist->players ) )
    {
        struct PlayerData *p = plist->lpPData;
        if ( (p->dwFlags & DPLAYI_PLAYER_SYSPLAYER) && !(p->dwFlags & DPLAYI_PLAYER_PLAYERLOCAL) )
            send_system( This, p->dpid, msg, size, extra, extraSize, DPSEND_GUARANTEED );
    }
}

HRESULT DP_MSG_SendDeletePlayer( IDirectPlayImpl *This, DPID playerId )
{
    DPSP_MSG_DELETEPLAYER msg = { 0 };

    msg.envelope.dwMagic = DPMSGMAGIC_DPLAYMSG;
    msg.envelope.wCommandId = DPMSGCMD_DELETEPLAYER;
    msg.envelope.wVersion = DPMSGVER_DP6;
    msg.playerId = playerId;
    EnterCriticalSection( &This->lock );
    if ( This->dp2->lpSysGroup ) DP_SendToAllMachines( This, &msg, sizeof(msg), NULL, 0 );
    LeaveCriticalSection( &This->lock );
    return DP_OK;
}

/* Close(): say goodbye for our players, then for this machine */
void DP_LIFE_Leaving( IDirectPlayImpl *This )
{
    struct PlayerList *plist;
    DPID ids[ 64 ];
    int n = 0, i;

    EnterCriticalSection( &This->lock );
    if ( !This->dp2->bConnectionOpen || !This->dp2->lpSysGroup )
    {
        LeaveCriticalSection( &This->lock );
        return;
    }
    for ( plist = DPQ_FIRST( This->dp2->lpSysGroup->players ); plist && n < ARRAYSIZE( ids ); plist = DPQ_NEXT( plist->players ) )
    {
        struct PlayerData *p = plist->lpPData;
        if ( (p->dwFlags & DPLAYI_PLAYER_PLAYERLOCAL) && !(p->dwFlags & DPLAYI_PLAYER_SYSPLAYER) )
            ids[ n++ ] = p->dpid;
    }
    for ( i = 0; i < n; i++ ) DP_MSG_SendDeletePlayer( This, ids[ i ] );
    if ( This->dp2->systemPlayerId ) DP_MSG_SendDeletePlayer( This, This->dp2->systemPlayerId );
    LeaveCriticalSection( &This->lock );
    DP_NOTE( "leaving the session: told the other machines about %d player(s) and this machine\n", n );
}

/* any message from a machine proves it is alive */
void DP_LIFE_Heard( IDirectPlayImpl *This, DPID anyPlayer )
{
    struct PlayerData *p;
    EnterCriticalSection( &This->lock );
    p = find_player( This, anyPlayer );
    if ( p && !(p->dwFlags & DPLAYI_PLAYER_SYSPLAYER) && p->systemPlayerId ) p = find_player( This, p->systemPlayerId );
    if ( p ) p->lastHeard = GetTickCount();
    LeaveCriticalSection( &This->lock );
}

static void become_or_follow_new_host( IDirectPlayImpl *This, DPID newHost )
{
    struct PlayerList *plist;
    struct PlayerData *p;
    DPID highest = 0;

    for ( plist = DPQ_FIRST( This->dp2->lpSysGroup->players ); plist; plist = DPQ_NEXT( plist->players ) )
    {
        p = plist->lpPData;
        if ( p->dpid > highest ) highest = p->dpid;
        if ( p->dwFlags & DPLAYI_PLAYER_SYSPLAYER )
        {
            if ( p->dpid == newHost ) p->dwFlags |= DPLAYI_PLAYER_NAMESRVR;
            else p->dwFlags &= ~DPLAYI_PLAYER_NAMESRVR;
        }
    }
    This->dp2->nameServerId = newHost;
    This->dp2->hostMigrated = TRUE;

    if ( newHost == This->dp2->systemPlayerId && !This->dp2->bHostInterface )
    {
        DPSP_MSG_IAMNAMESERVER msg = { 0 };
        void *spData = NULL;
        DWORD spDataSize = 0;

        This->dp2->bHostInterface = TRUE;
        DP_BumpObjectIds( highest );   /* ids we hand out must not collide with the old host's */
        DP_QueueSystemMessage( This, DPSYS_HOST );
        IDirectPlaySP_GetSPPlayerData( This->dp2->spData.lpISP, newHost, &spData, &spDataSize, DPSET_REMOTE );
        msg.envelope.dwMagic = DPMSGMAGIC_DPLAYMSG;
        msg.envelope.wCommandId = DPMSGCMD_IAMNAMESERVER;
        msg.envelope.wVersion = DPMSGVER_DP6;
        msg.hostId = newHost;
        msg.flags = DPLAYI_PLAYER_SYSPLAYER | DPLAYI_PLAYER_NAMESRVR;
        msg.spDataSize = spDataSize;
        DP_SendToAllMachines( This, &msg, sizeof(msg), spData, spDataSize );
        DP_NOTE( "host migration: this machine (0x%08lx) is the new host\n", newHost );
#ifdef OKP_BUILTIN
        /* Online/LAN Play: list the game under this machine now (no SP callback exists for this) */
        okp_became_host( This->dp2->spData.lpISP );
#endif
    }
    else
        DP_NOTE( "host migration: machine 0x%08lx is the new host\n", newHost );
}

/* the host's machine left: pick the new host (or lose the session) */
static void host_gone( IDirectPlayImpl *This )
{
    struct PlayerList *plist;
    DPID newHost = 0;

    if ( !(This->dp2->lpSessionDesc->dwFlags & DPSESSION_MIGRATEHOST) )
    {
        DP_NOTE( "the host left and the session does not migrate: session lost\n" );
        DP_QueueSystemMessage( This, DPSYS_SESSIONLOST );
        return;
    }
    for ( plist = DPQ_FIRST( This->dp2->lpSysGroup->players ); plist; plist = DPQ_NEXT( plist->players ) )
    {
        struct PlayerData *p = plist->lpPData;
        if ( (p->dwFlags & DPLAYI_PLAYER_SYSPLAYER) && (!newHost || p->dpid < newHost) )
            newHost = p->dpid;
    }
    if ( newHost )
        become_or_follow_new_host( This, newHost );
}

/* a machine is gone (said goodbye, or went silent): its players leave with it */
void DP_LIFE_MachineGone( IDirectPlayImpl *This, DPID systemPlayerId, const char *why )
{
    struct PlayerList *plist;
    struct PlayerData *sys;
    DPID ids[ 64 ];
    int n = 0, i;
    BOOL wasHost;

    EnterCriticalSection( &This->lock );
    sys = find_player( This, systemPlayerId );
    if ( !This->dp2->bConnectionOpen || !sys || (sys->dwFlags & DPLAYI_PLAYER_PLAYERLOCAL) ||
         !(sys->dwFlags & DPLAYI_PLAYER_SYSPLAYER) )
    {
        LeaveCriticalSection( &This->lock );
        return;
    }
    wasHost = systemPlayerId == This->dp2->nameServerId || (sys->dwFlags & DPLAYI_PLAYER_NAMESRVR);
    for ( plist = DPQ_FIRST( This->dp2->lpSysGroup->players ); plist && n < ARRAYSIZE( ids ); plist = DPQ_NEXT( plist->players ) )
    {
        struct PlayerData *p = plist->lpPData;
        if ( p->dpid != systemPlayerId && p->systemPlayerId == systemPlayerId &&
             !(p->dwFlags & DPLAYI_PLAYER_PLAYERLOCAL) )
            ids[ n++ ] = p->dpid;
    }
    for ( i = 0; i < n; i++ ) DP_RemovePlayer( This, ids[ i ], FALSE );
    DP_RemovePlayer( This, systemPlayerId, FALSE );
    DP_NOTE( "machine 0x%08lx left (%s)%s: %d player(s) removed\n", systemPlayerId, why,
             wasHost ? ", it was the host" : "", n );
    if ( wasHost )
        host_gone( This );
    LeaveCriticalSection( &This->lock );
}

HRESULT DP_LIFE_HandleDeletePlayer( IDirectPlayImpl *This, const void *body, DWORD size, const void *header )
{
    const DPSP_MSG_DELETEPLAYER *msg = body;
    struct PlayerData *p;
    BOOL machine;

    if ( size < sizeof(*msg) ) return DPERR_GENERIC;
    EnterCriticalSection( &This->lock );
    p = find_player( This, msg->playerId );
    /* a machine says goodbye for its own players only - nobody kicks another machine out */
    if ( !This->dp2->bConnectionOpen || !p || (p->dwFlags & DPLAYI_PLAYER_PLAYERLOCAL) ||
         !DP_SenderOwns( This, header, msg->playerId, "DELETEPLAYER" ) )
    {
        LeaveCriticalSection( &This->lock );
        return DP_OK;
    }
    machine = (p->dwFlags & DPLAYI_PLAYER_SYSPLAYER) != 0;
    if ( machine )
        DP_LIFE_MachineGone( This, msg->playerId, "said goodbye" );
    else
    {
        DP_RemovePlayer( This, msg->playerId, FALSE );
        DP_NOTE( "player 0x%08lx left\n", msg->playerId );
    }
    LeaveCriticalSection( &This->lock );
    return DP_OK;
}

HRESULT DP_LIFE_HandleIAmNameServer( IDirectPlayImpl *This, const void *body, DWORD size, const void *header )
{
    const DPSP_MSG_IAMNAMESERVER *msg = body;

    if ( size < sizeof(*msg) ) return DPERR_GENERIC;
    EnterCriticalSection( &This->lock );
    if ( This->dp2->bConnectionOpen && find_player( This, msg->hostId ) &&
         msg->hostId != This->dp2->nameServerId && msg->hostId != This->dp2->systemPlayerId )
    {
        /* a machine may announce only itself as the new host, and only once the host we know is
         * actually gone - while it is still here nobody takes its place by saying so (our own keepalive
         * elects a new host when the old one really leaves). Normally we elected the same machine already. */
        if ( !DP_SenderOwns( This, header, msg->hostId, "IAMNAMESERVER" ) )
            ;   /* refused and logged inside DP_SenderOwns */
#ifndef DPLAYX_TEST_SKIP_AUTH
        else if ( This->dp2->nameServerId && find_player( This, This->dp2->nameServerId ) )
            DP_NOTE( "IAMNAMESERVER from 0x%08lx ignored: the host 0x%08lx is still here\n", msg->hostId,
                     This->dp2->nameServerId );   /* a second guard, not a test control: dropped under SKIP_AUTH */
#endif
        else
        {
            This->dp2->bHostInterface = FALSE;
            become_or_follow_new_host( This, msg->hostId );
        }
    }
    LeaveCriticalSection( &This->lock );
    return DP_OK;
}

/* TEST ONLY (OKP_TEST_ROGUE=deletehost|iamhost|sessiondesc): once, a few seconds into the session, this
 * machine - a legitimate, already-joined member - sends a system message about a player it does not own,
 * exactly as a griefer inside the game would. On a fixed build every other machine must refuse it
 * (DP_SenderOwns) and the game go on under the real host; on a -DDPLAYX_TEST_SKIP_AUTH build the abuse
 * takes effect, which is how run_local.sh `rogue` proves the check really fires. Never sends anything the
 * game couldn't already send itself - it only puts another machine's id in the "whose" field. */
static void rogue_fire( IDirectPlayImpl *This, const char *what )
{
    EnterCriticalSection( &This->lock );
    if ( !This->dp2->bConnectionOpen || !This->dp2->lpSysGroup || !This->dp2->lpSessionDesc ||
         This->dp2->bHostInterface )   /* only a non-host member plays the griefer */
    {
        LeaveCriticalSection( &This->lock );
        return;
    }
    if ( !strcmp( what, "deletehost" ) )
    {
        DPSP_MSG_DELETEPLAYER msg = { 0 };
        msg.envelope.dwMagic = DPMSGMAGIC_DPLAYMSG;
        msg.envelope.wCommandId = DPMSGCMD_DELETEPLAYER;
        msg.envelope.wVersion = DPMSGVER_DP6;
        msg.playerId = This->dp2->nameServerId;   /* "the host has left" - a lie */
        DP_SendToAllMachines( This, &msg, sizeof(msg), NULL, 0 );
    }
    else if ( !strcmp( what, "iamhost" ) )
    {
        DPSP_MSG_IAMNAMESERVER msg = { 0 };
        msg.envelope.dwMagic = DPMSGMAGIC_DPLAYMSG;
        msg.envelope.wCommandId = DPMSGCMD_IAMNAMESERVER;
        msg.envelope.wVersion = DPMSGVER_DP6;
        msg.hostId = This->dp2->systemPlayerId;    /* "I am the host now" - while the real one is here */
        msg.flags = DPLAYI_PLAYER_SYSPLAYER | DPLAYI_PLAYER_NAMESRVR;
        DP_SendToAllMachines( This, &msg, sizeof(msg), NULL, 0 );
    }
    else if ( !strcmp( what, "sessiondesc" ) )
    {
        static const WCHAR name[] = L"ROGUE";
        DWORD size = sizeof(DPSP_MSG_SESSIONDESCCHANGED) + sizeof(name);
        DPSP_MSG_SESSIONDESCCHANGED *m = calloc( 1, size );
        if ( m )
        {
            m->envelope.dwMagic = DPMSGMAGIC_DPLAYMSG;
            m->envelope.wCommandId = DPMSGCMD_SESSIONDESCCHANGED;
            m->envelope.wVersion = DPMSGVER_DP6;
            m->desc = *This->dp2->lpSessionDesc;
            m->desc.lpszSessionName = NULL;
            m->desc.lpszPassword = NULL;
            m->desc.dwMaxPlayers = 1;              /* "the room is changed" - only the host may say so */
            m->sessionNameOffset = sizeof(*m);
            memcpy( m + 1, name, sizeof(name) );
            DP_SendToAllMachines( This, m, size, NULL, 0 );
            free( m );
        }
    }
    DP_NOTE( "TEST ROGUE: this machine sent a forged %s to every other machine\n", what );
    LeaveCriticalSection( &This->lock );
}

/* one round of the keepalive: pings to quiet machines, drops of silent ones (every 500 ms) */
static void keepalive_once( IDirectPlayImpl *This )
{
        struct PlayerList *plist;
        DPID dead[ 16 ];
        int ndead = 0, i;
        DWORD now = GetTickCount();

        EnterCriticalSection( &This->lock );
        if ( This->dp2->bConnectionOpen && This->dp2->lpSysGroup )
        {
            for ( plist = DPQ_FIRST( This->dp2->lpSysGroup->players ); plist; plist = DPQ_NEXT( plist->players ) )
            {
                struct PlayerData *p = plist->lpPData;
                if ( !(p->dwFlags & DPLAYI_PLAYER_SYSPLAYER) || (p->dwFlags & DPLAYI_PLAYER_PLAYERLOCAL) )
                    continue;
                if ( !p->lastHeard ) p->lastHeard = now;
                if ( now - p->lastHeard > KEEPALIVE_DEAD_NOW() )
                {
                    if ( ndead < ARRAYSIZE( dead ) ) dead[ ndead++ ] = p->dpid;
                }
                /* only a machine that has gone quiet: while it talks it is alive, and Windows sends
                 * no PING then (S3 tap, 2026-10-16: none in a 14 s game with traffic, ours sent one
                 * every 1.5 s regardless) */
                else if ( now - p->lastHeard >= KEEPALIVE_PING_MS && now - p->lastPing >= KEEPALIVE_PING_MS )
                {
                    DPSP_MSG_PING ping;
                    ping.envelope.dwMagic = DPMSGMAGIC_DPLAYMSG;
                    ping.envelope.wCommandId = DPMSGCMD_PING;
                    ping.envelope.wVersion = DPMSGVER_DP6;
                    ping.fromId = This->dp2->systemPlayerId;
                    ping.tickCount = now;
                    send_system( This, p->dpid, &ping, sizeof(ping), NULL, 0, 0 );
                    p->lastPing = now;
                }
            }
        }
        LeaveCriticalSection( &This->lock );
        for ( i = 0; i < ndead; i++ )
            DP_LIFE_MachineGone( This, dead[ i ], "no reply for drop_after= seconds" );
}

static DWORD WINAPI keepalive_thread( void *arg )
{
    IDirectPlayImpl *This = arg;
    HANDLE stop = This->dp2->hKeepAliveStop;
    const char *rogue = getenv( "OKP_TEST_ROGUE" );
    DWORD started = GetTickCount();
    BOOL rogue_done = FALSE;

    while ( WaitForSingleObject( stop, 500 ) == WAIT_TIMEOUT )
    {
        if ( rogue && *rogue && !rogue_done && GetTickCount() - started > 4000 )
        {
            rogue_done = TRUE;
            rogue_fire( This, rogue );
        }
        keepalive_once( This );
    }
    return 0;
}

#ifdef OKWEB_DPLAY
/* the browser build has no threads: the same round runs as a task of the page's pump (okweb/dplay/winweb.c) */
void winweb_every( DWORD ms, void (*fn)( void * ), void *arg );
static void keepalive_task( void *arg ) { keepalive_once( arg ); }
#endif

/* the session is open (hosting or joined) */
void DP_LIFE_Start( IDirectPlayImpl *This )
{
    if ( This->dp2->bHostInterface )
        This->dp2->nameServerId = This->dp2->systemPlayerId;
    if ( !(This->dp2->lpSessionDesc->dwFlags & DPSESSION_KEEPALIVE) || This->dp2->hKeepAliveThread )
        return;
#ifdef OKWEB_DPLAY
    winweb_every( 500, keepalive_task, This );
    This->dp2->hKeepAliveThread = (HANDLE)1;   /* "running" */
    return;
#endif
    This->dp2->hKeepAliveStop = CreateEventW( NULL, TRUE, FALSE, NULL );
    This->dp2->hKeepAliveThread = CreateThread( NULL, 0, keepalive_thread, This, 0, &This->dp2->dwKeepAliveThreadId );
}

void DP_LIFE_Stop( IDirectPlayImpl *This )
{
    if ( !This->dp2->hKeepAliveThread ) return;
#ifdef OKWEB_DPLAY
    winweb_every( 0, keepalive_task, This );
    This->dp2->hKeepAliveThread = NULL;
    return;
#endif
    SetEvent( This->dp2->hKeepAliveStop );
    if ( GetCurrentThreadId() != This->dp2->dwKeepAliveThreadId )
        WaitForSingleObject( This->dp2->hKeepAliveThread, 5000 );
    CloseHandle( This->dp2->hKeepAliveThread );
    CloseHandle( This->dp2->hKeepAliveStop );
    This->dp2->hKeepAliveThread = NULL;
    This->dp2->hKeepAliveStop = NULL;
}
