/* Direct Play 2,3,4 Implementation
 *
 * Copyright 1998,1999,2000,2001 - Peter Hunnisett
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define COBJMACROS

#include <stdarg.h>
#include <string.h>
#include <limits.h>

#include "windef.h"
#include "winerror.h"
#include "winbase.h"
#include "winnt.h"
#include "winreg.h"
#include "winnls.h"
#include "wine/debug.h"

#include "dplayx_global.h"
#include "name_server.h"
#include "dplayx_queue.h"
#include "wine/dplaysp.h"
#include "dplay_global.h"

WINE_DEFAULT_DEBUG_CHANNEL(dplay);

#ifdef OKP_BUILTIN
void okp_join_failed( IDirectPlaySP *isp, const char *step, HRESULT hr );   /* okplay/src/sp.c */
#endif

/* FIXME: Should this be externed? */
extern HRESULT DPL_CreateCompoundAddress
( LPCDPCOMPOUNDADDRESSELEMENT lpElements, DWORD dwElementCount,
  LPVOID lpAddress, LPDWORD lpdwAddressSize, BOOL bAnsiInterface );


/* Local function prototypes */
static lpPlayerList DP_FindPlayer( IDirectPlayImpl *This, DPID dpid );
static DPNAME *DP_DuplicateName( const DPNAME *src, BOOL dstAnsi, BOOL srcAnsi, BOOL fill );
static void DP_SetGroupData( lpGroupData lpGData, DWORD dwFlags,
                             LPVOID lpData, DWORD dwDataSize );
static BOOL CALLBACK cbDeletePlayerFromAllGroups( DPID dpId,
                                                  DWORD dwPlayerType,
                                                  LPCDPNAME lpName,
                                                  DWORD dwFlags,
                                                  LPVOID lpContext );
lpGroupData DP_FindAnyGroup( IDirectPlayImpl *This, DPID dpid );
/* a group a game named: never the system group - Windows answers DPERR_INVALIDGROUP for id 0
 * (DPID_SYSTEM_GROUP) in every group call (black box, 2026-10-22) */
#define DP_FindGameGroup( This, dpid ) ( (dpid) == DPID_SYSTEM_GROUP ? NULL : DP_FindAnyGroup( (This), (dpid) ) )

/* Helper methods for player/group interfaces */
static HRESULT DP_SetSessionDesc( IDirectPlayImpl *This, const DPSESSIONDESC2 *lpSessDesc,
        DWORD dwFlags, BOOL bInitial, BOOL bAnsi );
static void DP_SetPlayerData( lpPlayerData lpPData, DWORD dwFlags,
        LPVOID lpData, DWORD dwDataSize );
static BOOL DP_BuildSPCompoundAddr( LPGUID lpcSpGuid, LPVOID* lplpAddrBuf,
                                    LPDWORD lpdwBufSize );

static DPID DP_GetRemoteNextObjectId(void);

static DWORD DP_CopySessionDesc( LPDPSESSIONDESC2 destSessionDesc,
                                 LPCDPSESSIONDESC2 srcSessDesc, BOOL dstAnsi, BOOL srcAnsi );


#undef DPID_NOPARENT_GROUP
#define DPID_NOPARENT_GROUP 0 /* Magic number to indicate no parent of group */
/* DPID_SYSTEM_GROUP (dplay_global.h): if system group is supported
                                                 we don't have to change much */
#define DPID_NAME_SERVER 0x19a9d65b  /* Don't ask me why */

/* Strip out dwFlag values which cannot be sent in the CREATEGROUP msg */
#define DPMSG_CREATEGROUP_DWFLAGS(x) ( (x) & DPGROUP_HIDDEN )

/* Strip out all dwFlags values for CREATEPLAYER msg */
#define DPMSG_CREATEPLAYER_DWFLAGS(x) 0

static LONG kludgePlayerGroupId = 1000;
static LONG idLimit;   /* 0, or the highest id a game fix allows (DP_LimitObjectIds) */


static inline IDirectPlayImpl *impl_from_IDirectPlay( IDirectPlay *iface )
{
    return CONTAINING_RECORD( iface, IDirectPlayImpl, IDirectPlay_iface );
}

static inline IDirectPlayImpl *impl_from_IDirectPlay2( IDirectPlay2 *iface )
{
    return CONTAINING_RECORD( iface, IDirectPlayImpl, IDirectPlay2_iface );
}

static inline IDirectPlayImpl *impl_from_IDirectPlay2A( IDirectPlay2A *iface )
{
    return CONTAINING_RECORD( iface, IDirectPlayImpl, IDirectPlay2A_iface );
}

static inline IDirectPlayImpl *impl_from_IDirectPlay3A( IDirectPlay3A *iface )
{
    return CONTAINING_RECORD( iface, IDirectPlayImpl, IDirectPlay3A_iface );
}

static inline IDirectPlayImpl *impl_from_IDirectPlay3( IDirectPlay3 *iface )
{
    return CONTAINING_RECORD( iface, IDirectPlayImpl, IDirectPlay3_iface );
}

static inline IDirectPlayImpl *impl_from_IDirectPlay4A( IDirectPlay4A *iface )
{
    return CONTAINING_RECORD( iface, IDirectPlayImpl, IDirectPlay4A_iface );
}

static inline IDirectPlayImpl *impl_from_IDirectPlay4( IDirectPlay4 *iface )
{
    return CONTAINING_RECORD( iface, IDirectPlayImpl, IDirectPlay4_iface );
}

static BOOL DP_CreateDirectPlay2( LPVOID lpDP )
{
  IDirectPlayImpl *This = lpDP;

  This->dp2 = calloc( 1, sizeof( *(This->dp2) ) );
  if ( This->dp2 == NULL )
  {
    return FALSE;
  }

  This->dp2->bConnectionOpen = FALSE;

  This->dp2->hEnumSessionThread = INVALID_HANDLE_VALUE;
  This->dp2->dwEnumSessionLock = 0;

  This->dp2->bHostInterface = FALSE;

  DPQ_INIT(This->dp2->receiveMsgs);
  DPQ_INIT(This->dp2->repliesExpected);

  if( !NS_InitializeSessionCache( &This->dp2->lpNameServerData ) )
  {
    /* FIXME: Memory leak */
    return FALSE;
  }

  /* Provide an initial session desc with nothing in it */
  This->dp2->lpSessionDesc = calloc( 1, sizeof( *This->dp2->lpSessionDesc ) );
  if( This->dp2->lpSessionDesc == NULL )
  {
    /* FIXME: Memory leak */
    return FALSE;
  }
  This->dp2->lpSessionDesc->dwSize = sizeof( *This->dp2->lpSessionDesc );

  /* the provider reports its version in SPInit; Windows' dplayx hands it 0 there (S3 wire tap,
   * 2026-10-14). The callback table's dwVersion still says what we are (DirectPlay 6). */
  This->dp2->spData.dwSPVersion = 0;

  This->dp2->spData.lpCB = calloc( 1, sizeof( *This->dp2->spData.lpCB ) );
  This->dp2->spData.lpCB->dwSize = sizeof( *This->dp2->spData.lpCB );
  This->dp2->spData.lpCB->dwVersion = DPSP_MAJORVERSION;

  /* This is the pointer to the service provider */
  if ( FAILED( dplaysp_create( &IID_IDirectPlaySP, (void**)&This->dp2->spData.lpISP, This ) ) )
  {
    /* FIXME: Memory leak */
    return FALSE;
  }

  /* Setup lobby provider information */
  This->dp2->dplspData.dwSPVersion = DPSP_MAJORVERSION;
  This->dp2->dplspData.lpCB = calloc( 1, sizeof( *This->dp2->dplspData.lpCB ) );
  This->dp2->dplspData.lpCB->dwSize = sizeof(  *This->dp2->dplspData.lpCB );

  if( FAILED( dplobbysp_create( &IID_IDPLobbySP, (void**)&This->dp2->dplspData.lpISP, This ) )
    )
  {
    /* FIXME: Memory leak */
    return FALSE;
  }

  return TRUE;
}

/* Definition of the global function in dplayx_queue.h. #
 * FIXME: Would it be better to have a dplayx_queue.c for this function? */
DPQ_DECL_DELETECB( cbDeleteElemFromHeap, LPVOID )
{
  free( elem );
}

static BOOL DP_DestroyDirectPlay2( LPVOID lpDP )
{
  IDirectPlayImpl *This = lpDP;

  if( This->dp2->hEnumSessionThread != INVALID_HANDLE_VALUE )
  {
#ifdef OKWEB_DPLAY
    SetEvent( This->dp2->hKillEnumSessionThreadEvent );   /* the task frees itself on its next round */
#else
    TerminateThread( This->dp2->hEnumSessionThread, 0 );
    CloseHandle( This->dp2->hEnumSessionThread );
#endif
  }

  /* Finish with the SP - have it shutdown */
  if( This->dp2->spData.lpCB->ShutdownEx )
  {
    DPSP_SHUTDOWNDATA data;

    TRACE( "Calling SP ShutdownEx\n" );

    data.lpISP = This->dp2->spData.lpISP;

    (*This->dp2->spData.lpCB->ShutdownEx)( &data );
  }
  else if (This->dp2->spData.lpCB->Shutdown ) /* obsolete interface */
  {
    TRACE( "Calling obsolete SP Shutdown\n" );
    (*This->dp2->spData.lpCB->Shutdown)();
  }

  /* Unload the SP (if it exists) */
  if( This->dp2->hServiceProvider != 0 )
  {
    FreeLibrary( This->dp2->hServiceProvider );
  }

  /* Unload the Lobby Provider (if it exists) */
  if( This->dp2->hDPLobbyProvider != 0 )
  {
    FreeLibrary( This->dp2->hDPLobbyProvider );
  }

  /* FIXME: Need to delete receive and send msgs queue contents */

  NS_DeleteSessionCache( This->dp2->lpNameServerData );

  free( This->dp2->dplspData.lpCB);
  free( This->dp2->lpSessionDesc );

  IDirectPlaySP_Release( This->dp2->spData.lpISP );

  /* Delete the contents */
  free( This->dp2 );

  return TRUE;
}

static void dplay_destroy(IDirectPlayImpl *obj)
{
#ifdef OKP_BUILTIN
     __wine_dbg_census( "DirectPlay object released" );
#endif
     DP_LIFE_Stop( obj );
     DP_DestroyDirectPlay2( obj );
     DeleteCriticalSection( &obj->lock );
     free( obj );
}

static inline DPID DP_NextObjectId(void)
{
  LONG id = InterlockedIncrement( &kludgePlayerGroupId );
  if ( idLimit && id > idLimit )
  {
    /* start again at 1; only a session that has made more objects than the limit gets here */
    ERR( "more than %ld player and group ids made in this run; numbering again from 1\n", idLimit );
    InterlockedCompareExchange( &kludgePlayerGroupId, 1, id );
    id = 1;
  }
  return (DPID)id;
}

/* A game fix (shim/gamefix.c) for a game that indexes its own tables by player id: ids from 1 to
 * limit. Windows hands out large ids (0x12cd979f on Windows 10, 2026-10-22), so this is never the
 * default. Called once, while the DLL loads, before any object exists. */
void DP_LimitObjectIds( DWORD limit )
{
  idLimit = (LONG)limit;
  kludgePlayerGroupId = 0;
}

/* dplay.dll (shim/dplay_forward.c) calls this before its first DirectPlayCreate. The DirectPlay 1.1
 * dplay.dll it stands in for numbers players 1, 2, 3... (GOG's Necrodome ships one, 5.00.2134.1:
 * ids 1 and 2, okplay/tests/dp1ids.c, 2026-10-22), and games of that time index their own tables
 * by id - Necrodome does. dplayx's own DirectPlayCreate keeps Windows' dplayx numbering (large
 * ids there, ours from 1000). Only while this process has made no player or group yet. */
void WINAPI OkpDplay11Numbering( void )
{
  if ( InterlockedCompareExchange( &kludgePlayerGroupId, 0, 1000 ) == 1000 )
    TRACE( "players and groups numbered from 1, as DirectPlay 1.1 does\n" );
}

static DWORD DP_CopyString( char **dst, const void *src, BOOL dstAnsi, BOOL srcAnsi, void *buffer, DWORD offset )
{
    void *dstPtr = NULL;
    DWORD size;

    if ( !src )
    {
        if ( buffer )
            *dst = NULL;

        return 0;
    }

    if ( buffer )
    {
        dstPtr = (char *) buffer + offset;
        *dst = dstPtr;
    }

    if ( dstAnsi == srcAnsi )
    {
        if ( srcAnsi )
            size = strlen( src ) + 1;
        else
            size = (wcslen( src ) + 1) * sizeof( WCHAR );

        if ( dstPtr )
            memcpy( dstPtr, src, size );
    }
    else
    {
        /* ask for the size first and pass exactly that: Windows 9x checks the whole output range it
         * is given (INT_MAX = "bad pointer" -> 0), so every name an ANSI game (MTM2) handed over
         * arrived empty - its hosted games listed with no name and never showed on Windows 11 */
        if ( srcAnsi )
        {
            size = MultiByteToWideChar( CP_ACP, 0, src, -1, NULL, 0 );
            if ( dstPtr && size ) MultiByteToWideChar( CP_ACP, 0, src, -1, dstPtr, size );
            size *= sizeof( WCHAR );
        }
        else
        {
            size = WideCharToMultiByte( CP_ACP, 0, src, -1, NULL, 0, NULL, NULL );
            if ( dstPtr && size ) WideCharToMultiByte( CP_ACP, 0, src, -1, dstPtr, size, NULL, NULL );
        }
    }

    return size;
}

static void *DP_DuplicateString( const void *src, BOOL dstAnsi, BOOL srcAnsi )
{
    DWORD size;
    char *dst;

    if ( !src )
        return NULL;

    size = DP_CopyString( NULL, src, dstAnsi, srcAnsi, NULL, 0 );

    dst = malloc( size );
    if ( !dst )
        return NULL;

    DP_CopyString( &dst, src, dstAnsi, srcAnsi, dst, 0 );

    return dst;
}

static HRESULT DP_QueuePlayerMessage( IDirectPlayImpl *This, DPID fromId, struct PlayerData *player,
                                      DPID excludeId, void *msg, FN_COPY_MESSAGE *copyMessage,
                                      DWORD genericSize )
{
    struct DPMSG *elem;
    DWORD msgSize;

    if ( !( player->dwFlags & DPLAYI_PLAYER_PLAYERLOCAL ) )
        return DP_OK;
    if ( player->dwFlags & DPLAYI_PLAYER_SYSPLAYER )
        return DP_OK;
    if ( player->dpid == excludeId )
        return DP_OK;

    elem = calloc( 1, sizeof( struct DPMSG ) );
    if( !elem )
        return DPERR_OUTOFMEMORY;

    msgSize = copyMessage( NULL, msg, genericSize, FALSE );
    elem->msg = malloc( msgSize );
    if ( !elem->msg )
    {
        free( elem );
        return DPERR_OUTOFMEMORY;
    }
    copyMessage( elem->msg, msg, genericSize, FALSE );

    elem->fromId = fromId;
    elem->toId = player->dpid;
    elem->copyMessage = copyMessage;
    elem->genericSize = genericSize;

    DPQ_INSERT_IN_TAIL( This->dp2->receiveMsgs, elem, msgs );

    if( player->hEvent )
        SetEvent( player->hEvent );

    return DP_OK;
}

/* A group's players in the order the game sees them: the system group's (every player) newest
 * first, as Windows enumerates them and hands out its system messages (apistate_dp1.c,
 * 2026-10-22); other groups in the order players were added. A snapshot (free it), so a callback
 * that creates or destroys players doesn't walk a changing list. The list itself stays oldest
 * first: host.c and lifecycle.c build Windows' wire order from that. */
static struct PlayerData **DP_GroupPlayersInOrder( IDirectPlayImpl *This, struct GroupData *group,
                                                   int *count )
{
    struct PlayerList *plist;
    struct PlayerData **list;
    int n = 0, i;

    for ( plist = DPQ_FIRST( group->players ); plist; plist = DPQ_NEXT( plist->players ) ) n++;
    *count = 0;
    if ( !(list = malloc( (n ? n : 1) * sizeof(*list) )) ) return NULL;
    i = 0;
    for ( plist = DPQ_FIRST( group->players ); plist; plist = DPQ_NEXT( plist->players ) )
        list[ group == This->dp2->lpSysGroup ? n - 1 - i++ : i++ ] = plist->lpPData;
    *count = n;
    return list;
}

/* DPSESSION_NODATAMESSAGES: Windows then tells its players of no name, data or session desc change
 * (DPSYS_SETPLAYERORGROUPNAME/DATA, DPSYS_SETSESSIONDESC; black box, 2026-10-22) */
static BOOL DP_DataMessagesOff( IDirectPlayImpl *This )
{
    return This->dp2->lpSessionDesc && (This->dp2->lpSessionDesc->dwFlags & DPSESSION_NODATAMESSAGES);
}

static HRESULT DP_QueueMessage( IDirectPlayImpl *This, DPID fromId, DPID toId, DPID excludeId,
                                void *msg, FN_COPY_MESSAGE *copyMessage, DWORD genericSize )
{
    struct PlayerList *plist;
    struct GroupData *group;
    HRESULT hr;

    plist = DP_FindPlayer( This, toId );
    if ( plist )
        return DP_QueuePlayerMessage( This, fromId, plist->lpPData, excludeId, msg, copyMessage,
                                      genericSize );

    group = DP_FindAnyGroup( This, toId );
    if( group )
    {
        struct PlayerData **players;
        int n, i;

        if ( !(players = DP_GroupPlayersInOrder( This, group, &n )) )
            return DPERR_OUTOFMEMORY;
        for( i = 0; i < n; i++ )
        {
            hr = DP_QueuePlayerMessage( This, fromId, players[ i ], excludeId, msg, copyMessage,
                                        genericSize );
            if ( FAILED( hr ) )
            {
                free( players );
                return hr;
            }
        }
        free( players );
        return DP_OK;
    }

    return DPERR_INVALIDPLAYER;
}

static DWORD DP_CopyGeneric( DPMSG_GENERIC *genericDst, DPMSG_GENERIC *genericSrc,
                             DWORD genericSize, BOOL ansi )
{
  if ( !genericDst )
    return genericSize;

  memcpy( genericDst, genericSrc, genericSize );

  return genericSize;
}

static DWORD DP_CopyCreatePlayerOrGroup( DPMSG_GENERIC *genericDst, DPMSG_GENERIC *genericSrc,
                                         DWORD genericSize, BOOL ansi )
{
    DPMSG_CREATEPLAYERORGROUP *src = (DPMSG_CREATEPLAYERORGROUP *) genericSrc;
    DPMSG_CREATEPLAYERORGROUP *dst = (DPMSG_CREATEPLAYERORGROUP *) genericDst;
    DWORD offset = sizeof( DPMSG_CREATEPLAYERORGROUP );

    if ( dst )
        *dst = *src;

    if ( src->lpData )
    {
        if ( dst )
        {
            dst->lpData = (char *) dst + offset;
            memcpy( dst->lpData, src->lpData, src->dwDataSize );
        }
        offset += src->dwDataSize;
    }

    offset += DP_CopyString( &dst->dpnName.lpszShortNameA, src->dpnName.lpszShortNameA, ansi, FALSE,
                             genericDst, offset );
    offset += DP_CopyString( &dst->dpnName.lpszLongNameA, src->dpnName.lpszLongNameA, ansi, FALSE,
                             genericDst, offset );

    return offset;
}

static DWORD DP_CopySetPlayerOrGroupData( DPMSG_GENERIC *genericDst, DPMSG_GENERIC *genericSrc,
                                          DWORD genericSize, BOOL ansi )
{
    DPMSG_SETPLAYERORGROUPDATA *src = (DPMSG_SETPLAYERORGROUPDATA *) genericSrc;
    DPMSG_SETPLAYERORGROUPDATA *dst = (DPMSG_SETPLAYERORGROUPDATA *) genericDst;
    DWORD offset = sizeof( DPMSG_SETPLAYERORGROUPDATA );

    if ( dst )
        *dst = *src;

    if ( src->lpData )
    {
        if ( dst )
        {
            dst->lpData = (char *) dst + offset;
            memcpy( dst->lpData, src->lpData, src->dwDataSize );
        }
        offset += src->dwDataSize;
    }

    return offset;
}

/* *lplpReply will be non NULL iff there is something to reply */
HRESULT DP_HandleMessage( IDirectPlayImpl *This, void *messageBody,
        DWORD dwMessageBodySize, void *messageHeader, WORD wCommandId, WORD wVersion,
        void **lplpReply, DWORD *lpdwMsgSize )
{
  TRACE( "(%p)->(%p,0x%08lx,%p,%u,%u)\n",
         This, messageBody, dwMessageBodySize, messageHeader, wCommandId,
         wVersion );

  switch( wCommandId )
  {
    /* Name server needs to handle this request */
    case DPMSGCMD_ENUMSESSIONSREQUEST:
      /* Reply expected */
      NS_ReplyToEnumSessionsRequest( messageBody, dwMessageBodySize, lplpReply, lpdwMsgSize, This );
      break;

    /* Name server needs to handle this request */
    case DPMSGCMD_ENUMSESSIONSREPLY:
      EnterCriticalSection( &This->lock );

      /* No reply expected */
      NS_AddRemoteComputerAsNameServer( messageHeader,
                                        This->dp2->spData.dwSPHeaderSize,
                                        messageBody,
                                        dwMessageBodySize,
                                        This->dp2->lpNameServerData );

      LeaveCriticalSection( &This->lock );
      break;

    case DPMSGCMD_REQUESTNEWPLAYERID:
    case DPMSGCMD_REQUESTGROUPID:   /* a joiner's group gets its id from the host too */
    {
      LPCDPMSG_REQUESTNEWPLAYERID lpcMsg = messageBody;

      LPDPMSG_NEWPLAYERIDREPLY lpReply;

      *lpdwMsgSize = This->dp2->spData.dwSPHeaderSize + sizeof( *lpReply );

      *lplpReply = calloc( 1, *lpdwMsgSize );

      FIXME( "Ignoring dwFlags 0x%08lx in request msg\n",
             lpcMsg->dwFlags );

      /* Setup the reply */
      lpReply = (LPDPMSG_NEWPLAYERIDREPLY)( (BYTE*)(*lplpReply) +
                                            This->dp2->spData.dwSPHeaderSize );

      lpReply->envelope.dwMagic    = DPMSGMAGIC_DPLAYMSG;
      lpReply->envelope.wCommandId = DPMSGCMD_NEWPLAYERIDREPLY;
      lpReply->envelope.wVersion   = DPMSGVER_DP6;

      lpReply->dpidNewPlayerId = DP_NextObjectId();

      TRACE( "Allocating new playerid 0x%08lx from remote request\n",
             lpReply->dpidNewPlayerId );
      break;
    }

    case DPMSGCMD_CREATESESSION: {
      DPMSG_CREATESESSION *msg;
      DPPLAYERINFO playerInfo;
      DWORD offset = 0;
      HRESULT hr;

      if( dwMessageBodySize < sizeof( DPMSG_CREATESESSION ) )
        return DPERR_GENERIC;
      msg = (DPMSG_CREATESESSION *)messageBody;
      offset += sizeof( DPMSG_CREATESESSION );

      hr = DP_MSG_ReadPackedPlayer( (char *)messageBody, &offset, dwMessageBodySize, &playerInfo );
      if ( FAILED( hr ) )
        return hr;

      if ( dwMessageBodySize - offset < 6 )
        return DPERR_GENERIC;

      /* a machine announces players on itself only */
      if ( playerInfo.systemPlayerId &&
           !DP_SenderOwns( This, messageHeader, playerInfo.systemPlayerId, "CREATESESSION" ) )
        return DP_OK;

      EnterCriticalSection( &This->lock );

      if ( !This->dp2->bConnectionOpen )
      {
        LeaveCriticalSection( &This->lock );
        return DP_OK;
      }

      hr = DP_CreatePlayer( This, messageHeader, &msg->playerId, &playerInfo.name,
                            playerInfo.playerData, playerInfo.playerDataLength, playerInfo.spData,
                            playerInfo.spDataLength, playerInfo.flags & ~DPLAYI_PLAYER_PLAYERLOCAL,
                            NULL, NULL, FALSE );

      if ( FAILED( hr ) )
      {
        LeaveCriticalSection( &This->lock );
        return hr;
      }
      DP_SetPlayerSystemId( This, msg->playerId, playerInfo.systemPlayerId );

      LeaveCriticalSection( &This->lock );

      break;
    }

    case DPMSGCMD_GETNAMETABLEREPLY:
    case DPMSGCMD_NEWPLAYERIDREPLY:
    case DPMSGCMD_FORWARDADDPLAYERNACK:
    case DPMSGCMD_SUPERENUMPLAYERSREPLY:
      DP_MSG_ReplyReceived( This, wCommandId, messageBody, dwMessageBodySize, messageHeader );
      break;

    case DPMSGCMD_GROUPDATACHANGED: {
      DPMSG_SETPLAYERORGROUPDATA setPlayerOrGroupDataMsg;
      DPSP_MSG_GROUPDATACHANGED *msg;
      struct GroupData *group;
      HRESULT hr;
      void *data;

      if( dwMessageBodySize < sizeof( DPSP_MSG_GROUPDATACHANGED ) )
        return DPERR_GENERIC;
      msg = (DPSP_MSG_GROUPDATACHANGED *)messageBody;

      if( dwMessageBodySize < msg->dataOffset )
        return DPERR_GENERIC;
      if( dwMessageBodySize - msg->dataOffset < msg->dataSize )
        return DPERR_GENERIC;
      data = (char *)messageBody + msg->dataOffset;

      EnterCriticalSection( &This->lock );

      if( !This->dp2->bConnectionOpen )
      {
        LeaveCriticalSection( &This->lock );
        return DP_OK;
      }

      group = DP_FindAnyGroup( This, msg->groupId );
      if( !group )
      {
        LeaveCriticalSection( &This->lock );
        return DPERR_GENERIC;
      }

      DP_SetGroupData( group, DPSET_REMOTE, data, msg->dataSize );
      if( DP_DataMessagesOff( This ) )
      {
        LeaveCriticalSection( &This->lock );
        break;
      }

      setPlayerOrGroupDataMsg.dwType = DPSYS_SETPLAYERORGROUPDATA;
      setPlayerOrGroupDataMsg.dwPlayerType = DPPLAYERTYPE_GROUP;
      setPlayerOrGroupDataMsg.dpId = msg->groupId;
      setPlayerOrGroupDataMsg.lpData = data;
      setPlayerOrGroupDataMsg.dwDataSize = msg->dataSize;

      hr = DP_QueueMessage( This, DPID_SYSMSG, DPID_ALLPLAYERS, 0, &setPlayerOrGroupDataMsg,
                            DP_CopySetPlayerOrGroupData, 0 );
      if ( FAILED( hr ) )
      {
        LeaveCriticalSection( &This->lock );
        return hr;
      }

      LeaveCriticalSection( &This->lock );

      break;
    }

    case DPMSGCMD_CREATEGROUP:
      return DP_GRP_HandleCreate( This, messageBody, dwMessageBodySize, messageHeader );

    case DPMSGCMD_ADDPLAYERTOGROUP:
    case DPMSGCMD_DELETEPLAYERFROMGROUP:
    case DPMSGCMD_DELETEGROUP:
    case DPMSGCMD_ADDSHORTCUTTOGROUP:
    case DPMSGCMD_DELETEGROUPFROMGROUP:
      return DP_GRP_HandleIds( This, wCommandId, messageBody, dwMessageBodySize, messageHeader );

    case DPMSGCMD_GROUPNAMECHANGED:
      return DP_GRP_HandleName( This, messageBody, dwMessageBodySize, messageHeader );

    case DPMSGCMD_JUSTENVELOPE:
      TRACE( "GOT THE SELF MESSAGE: %p -> 0x%08lx\n", messageHeader, ((const DWORD *)messageHeader)[1] );
      NS_SetLocalAddr( This->dp2->lpNameServerData, messageHeader, 20 );
      DP_MSG_ReplyReceived( This, wCommandId, messageBody, dwMessageBodySize, messageHeader );
      break;

    case DPMSGCMD_FORWARDADDPLAYER:
      return DP_HOST_HandleForwardAddPlayer( This, messageBody, dwMessageBodySize, messageHeader,
                                             lplpReply, lpdwMsgSize );

    case DPMSGCMD_PLAYERNAMECHANGED:
      return DP_LOBBY_HandlePlayerNameChanged( This, messageBody, dwMessageBodySize, messageHeader );

    case DPMSGCMD_PLAYERDATACHANGED:
      return DP_LOBBY_HandlePlayerDataChanged( This, messageBody, dwMessageBodySize, messageHeader );

    case DPMSGCMD_SESSIONDESCCHANGED:
      return DP_LOBBY_HandleSessionDescChanged( This, messageBody, dwMessageBodySize, messageHeader );

    case DPMSGCMD_CHAT:
      return DP_LOBBY_HandleChat( This, messageBody, dwMessageBodySize, messageHeader );

    case DPMSGCMD_DELETEPLAYER:
      return DP_LIFE_HandleDeletePlayer( This, messageBody, dwMessageBodySize, messageHeader );

    case DPMSGCMD_IAMNAMESERVER:
      return DP_LIFE_HandleIAmNameServer( This, messageBody, dwMessageBodySize, messageHeader );

    case DPMSGCMD_PINGREPLY:
      /* only keeps the sender's own machine alive: a forged reply must not revive a machine that left */
      if( dwMessageBodySize >= sizeof( DPSP_MSG_PING ) &&
          DP_SenderOwns( This, messageHeader, ((const DPSP_MSG_PING *)messageBody)->fromId, "PINGREPLY" ) )
        DP_LIFE_Heard( This, ((const DPSP_MSG_PING *)messageBody)->fromId );
      break;

    case DPMSGCMD_ADDFORWARDACK:
      /* another machine has created the newcomer's system player */
      TRACE( "ADDFORWARDACK\n" );
      break;

    case DPMSGCMD_PING: {
      const DPSP_MSG_PING *msg;

      if( dwMessageBodySize < sizeof( DPSP_MSG_PING ) )
        return DPERR_GENERIC;
      msg = (const DPSP_MSG_PING *)messageBody;
      if( DP_SenderOwns( This, messageHeader, msg->fromId, "PING" ) )   /* a forged ping keeps nobody alive */
        DP_LIFE_Heard( This, msg->fromId );

      EnterCriticalSection( &This->lock );

      if ( !This->dp2->bConnectionOpen )
      {
        LeaveCriticalSection( &This->lock );
        return DP_OK;
      }

      LeaveCriticalSection( &This->lock );

      DP_MSG_SendPingReply( This, msg->fromId, This->dp2->systemPlayerId, msg->tickCount );

      break;
    }

    case DPMSGCMD_ADDFORWARD: {
      DPSP_MSG_ADDFORWARD *msg;
      DPPLAYERINFO playerInfo;
      DWORD offset = 0;
      HRESULT hr;

      if( dwMessageBodySize < sizeof( DPSP_MSG_ADDFORWARD ) )
        return DPERR_GENERIC;
      msg = (DPSP_MSG_ADDFORWARD *) messageBody;
      offset += sizeof( DPSP_MSG_ADDFORWARD );

      hr = DP_MSG_ReadPackedPlayer( (char *) messageBody, &offset, dwMessageBodySize, &playerInfo );
      if ( FAILED( hr ) )
        return hr;

      /* only the host introduces newcomers to the rest of the session */
      if ( !DP_SenderOwns( This, messageHeader, This->dp2->nameServerId, "ADDFORWARD" ) )
        return DP_OK;

      EnterCriticalSection( &This->lock );

      if ( !This->dp2->bConnectionOpen )
      {
        LeaveCriticalSection( &This->lock );
        return DP_OK;
      }

      hr = DP_CreatePlayer( This, messageHeader, &msg->playerId, &playerInfo.name,
                            playerInfo.playerData, playerInfo.playerDataLength, playerInfo.spData,
                            playerInfo.spDataLength, playerInfo.flags & ~DPLAYI_PLAYER_PLAYERLOCAL,
                            NULL, NULL, FALSE );
      if ( FAILED( hr ) )
      {
        LeaveCriticalSection( &This->lock );
        return hr;
      }
      DP_SetPlayerSystemId( This, msg->playerId, playerInfo.systemPlayerId );

      LeaveCriticalSection( &This->lock );

      DP_MSG_SendAddForwardAck( This, msg->playerId );

      break;
    }

    default:
      FIXME( "Unknown wCommandId %u. Ignoring message\n", wCommandId );
      break;
  }

  /* FIXME: There is code in dplaysp.c to handle dplay commands. Move to here. */

  return DP_OK;
}

HRESULT DP_HandleGameMessage( IDirectPlayImpl *This, void *messageBody, DWORD messageBodySize,
                              DPID fromId, DPID toId )
{
  DPMSG_GENERIC *msg;
  DWORD msgSize;
  HRESULT hr;

  TRACE( "(%p)->(%p,0x%08lx,0x%08lx,0x%08lx)\n", This, messageBody, messageBodySize, fromId, toId );

  msg = (DPMSG_GENERIC *) ((char *) messageBody + sizeof( DPMSG_SYSMSGENVELOPE ));
  msgSize = messageBodySize - sizeof( DPMSG_SYSMSGENVELOPE );

  DP_LIFE_Heard( This, fromId );

  EnterCriticalSection( &This->lock );

  hr = DP_QueueMessage( This, fromId, toId, 0, msg, DP_CopyGeneric, msgSize );

  LeaveCriticalSection( &This->lock );

  return hr;
}

static HRESULT WINAPI IDirectPlayImpl_QueryInterface( IDirectPlay *iface, REFIID riid, void **ppv )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    return IDirectPlayX_QueryInterface( &This->IDirectPlay4_iface, riid, ppv );
}

static ULONG WINAPI IDirectPlayImpl_AddRef( IDirectPlay *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    ULONG ref = InterlockedIncrement( &This->ref );

    TRACE( "(%p) ref=%ld\n", This, ref );

    return ref;
}

static ULONG WINAPI IDirectPlayImpl_Release( IDirectPlay *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    ULONG ref = InterlockedDecrement( &This->ref );

    TRACE( "(%p) ref=%ld\n", This, ref );

    if ( !ref )
        dplay_destroy( This );

    return ref;
}

/* ------------------------------------------------------------------ IDirectPlay (DirectPlay 1)
 *
 * The DirectX 1-3 interface (DPLAY.DLL games: Necrodome), answering as Windows' dplayx does - learned
 * black-box only (okplay/tests/apistate_dp1.c through apidiff.sh, 2026-10-22; docs/DPLAY_CONFORMANCE.md).
 * Each method goes to the DirectPlay 2+ code of the same object, with:
 *   - char* names (the A side) and DPSESSIONDESC sessions, which DirectPlay 1 names by a DWORD
 *     (dp2->dp1Sessions: the instance GUIDs its EnumSessions has shown, numbered from 1);
 *   - the DirectX 1-3 DPCAPS sizes (see dp1_caps);
 *   - Receive: the create/destroy system messages in their DirectPlay 1 forms, the rest as the
 *     Unicode DirectPlay 2 messages Windows passes through.
 */

static HRESULT DP_IF_Receive( IDirectPlayImpl *This, DPID *lpidFrom, DPID *lpidTo, DWORD dwFlags,
        void *lpData, DWORD *lpdwDataSize, BOOL bAnsi );
static HRESULT DP_IF_GetPlayerName( IDirectPlayImpl *This, DPID idPlayer, void *lpData,
        DWORD *lpdwDataSize, BOOL bAnsi );

#define DP1_CAPS_V1   24   /* DirectX 1/2's DPCAPS (no dwLatency): Windows takes it */
#define DP1_CAPS_DX3  28   /* DirectX 3's (dwLatency added): Windows 98 SE and 10 refuse it */

/* DirectPlay 1's system messages for "created" and "destroyed" (DirectX 3 dplay.h layouts) */
#define DP1SYS_ADDPLAYER    0x0003
#define DP1SYS_DELETEPLAYER 0x0005
#define DP1SYS_DELETEGROUP  0x0020
typedef struct
{
    DWORD dwType;            /* DP1SYS_ADDPLAYER */
    DWORD dwPlayerType;      /* 1 player, 0 group */
    DPID  dpId;
    char  szLongName[ DPLONGNAMELEN ];
    char  szShortName[ DPSHORTNAMELEN ];
    DWORD dwCurrentPlayers;
} DP1MSG_ADDPLAYER;          /* 88 bytes */
typedef struct
{
    DWORD dwType;            /* DP1SYS_DELETEPLAYER (a player) or DP1SYS_DELETEGROUP */
    DPID  dpId;
} DP1MSG_DELETEPLAYER;

/* GetCaps / GetPlayerCaps: Windows takes dwSize 24 or 40 only, reads the caps' dwFlags as the
 * DirectPlay 2 flags argument (anything but DPGETCAPS_GUARANTEED: INVALIDPARAMS), fills that many
 * bytes and writes dwSize = 40 back - into a 24-byte one too. DirectX 3's 28-byte DPCAPS, which
 * Windows refuses (so Necrodome stops at "No capabilities available for service" there), is taken
 * here as DirectX 3 did: 28 bytes filled, nothing written past them (Necrodome keeps its own data
 * right after the struct). The one place we answer differently from Windows, on purpose. */
static HRESULT dp1_caps( IDirectPlayImpl *This, DPID player, DPCAPS *caps )
{
    DPCAPS full;
    DWORD size, flags;
    HRESULT hr;

    if ( !caps ) return DPERR_INVALIDPARAMS;
    size = caps->dwSize;
    /* DirectX 3's GetCaps took no flags: whatever its dwFlags holds is ignored (Necrodome hands the
     * same struct back with the provider's flags from the last call still in it) */
    flags = size == DP1_CAPS_DX3 ? 0 : caps->dwFlags;
    if ( (size != DP1_CAPS_V1 && size != DP1_CAPS_DX3 && size != sizeof(DPCAPS)) ||
         (flags & ~DPGETCAPS_GUARANTEED) )
        return DPERR_INVALIDPARAMS;
    memset( &full, 0, sizeof(full) );
    full.dwSize = sizeof(full);
    hr = IDirectPlayX_GetPlayerCaps( &This->IDirectPlay4A_iface, player, &full, flags );
    if ( FAILED(hr) ) return hr;
    memcpy( caps, &full, size );
    caps->dwSize = size == DP1_CAPS_DX3 ? DP1_CAPS_DX3 : sizeof(DPCAPS);
    return DP_OK;
}

static void dp1_name( DPNAME *dpn, LPSTR name, LPSTR fullname )
{
    memset( dpn, 0, sizeof(*dpn) );
    dpn->dwSize = sizeof(*dpn);
    dpn->lpszShortNameA = name;
    dpn->lpszLongNameA = fullname;
}

/* a DirectPlay 1 session number for an instance GUID (0: no room left) */
static DWORD dp1_session_number( IDirectPlayImpl *This, const GUID *instance )
{
    DirectPlay2Data *d = This->dp2;
    DWORD i;
    for ( i = 0; i < d->dp1SessionCount; i++ )
        if ( IsEqualGUID( &d->dp1Sessions[ i ], instance ) ) return i + 1;
    if ( d->dp1SessionCount >= ARRAY_SIZE( d->dp1Sessions ) ) return 0;
    d->dp1Sessions[ d->dp1SessionCount++ ] = *instance;
    return d->dp1SessionCount;
}

static const GUID *dp1_session_guid( IDirectPlayImpl *This, DWORD number )
{
    DirectPlay2Data *d = This->dp2;
    return number && number <= d->dp1SessionCount ? &d->dp1Sessions[ number - 1 ] : NULL;
}

static void dp1_to_sd2( const DPSESSIONDESC *sd, DPSESSIONDESC2 *sd2, char *name, char *password )
{
    memset( sd2, 0, sizeof(*sd2) );
    sd2->dwSize = sizeof(*sd2);
    /* Windows keeps DirectPlay 1's dwFlags (the DPOPEN_* bits) as the session's flags too:
     * a session hosted through IDirectPlay::Open(create) shows dwFlags 2 to GetSessionDesc */
    sd2->dwFlags = sd->dwFlags;
    sd2->guidApplication = sd->guidSession;   /* DirectPlay 1's "session GUID" is the game's */
    sd2->dwMaxPlayers = sd->dwMaxPlayers;
    sd2->dwCurrentPlayers = sd->dwCurrentPlayers;
    lstrcpynA( name, sd->szSessionName, DPSESSIONNAMELEN + 1 );
    lstrcpynA( password, sd->szPassword, DPPASSWORDLEN + 1 );
    sd2->lpszSessionNameA = name;
    sd2->lpszPasswordA = password[ 0 ] ? password : NULL;
    sd2->dwReserved1 = sd->dwReserved1;
    sd2->dwReserved2 = sd->dwReserved2;
    sd2->dwUser1 = sd->dwUser1;
    sd2->dwUser2 = sd->dwUser2;
    sd2->dwUser3 = sd->dwUser3;
    sd2->dwUser4 = sd->dwUser4;
}

static HRESULT WINAPI IDirectPlayImpl_AddPlayerToGroup( IDirectPlay *iface, DPID group,
        DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    TRACE( "(%p)->(0x%08lx,0x%08lx)\n", This, group, player );
    return IDirectPlayX_AddPlayerToGroup( &This->IDirectPlay4A_iface, group, player );
}

static HRESULT WINAPI IDirectPlayImpl_Close( IDirectPlay *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    TRACE( "(%p)\n", This );
    return IDirectPlayX_Close( &This->IDirectPlay4A_iface );
}

static HRESULT WINAPI IDirectPlayImpl_CreatePlayer( IDirectPlay *iface, DPID *player,
        LPSTR name, LPSTR fullname, HANDLE *event )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    DPNAME dpn;
    HANDLE h = NULL;
    HRESULT hr;

    TRACE( "(%p)->(%p,%s,%s,%p)\n", This, player, debugstr_a( name ), debugstr_a( fullname ), event );
    if ( !player ) return DPERR_INVALIDPARAMS;
    dp1_name( &dpn, name, fullname );
    /* DirectPlay 1 makes the player's event itself and hands it back - whatever *event held
     * (Windows: a manual-reset event, set when a message for the player arrives) */
    if ( event && !(h = CreateEventW( NULL, TRUE, FALSE, NULL )) ) return DPERR_OUTOFMEMORY;
    hr = IDirectPlayX_CreatePlayer( &This->IDirectPlay4A_iface, player, &dpn, h, NULL, 0, 0 );
    if ( FAILED(hr) )
    {
        if ( h ) CloseHandle( h );
        return hr;
    }
    if ( event ) *event = h;
    return hr;
}

static HRESULT WINAPI IDirectPlayImpl_CreateGroup( IDirectPlay *iface, DPID *group, LPSTR name,
        LPSTR fullname )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    DPNAME dpn;
    TRACE( "(%p)->(%p,%s,%s)\n", This, group, debugstr_a( name ), debugstr_a( fullname ) );
    dp1_name( &dpn, name, fullname );
    return IDirectPlayX_CreateGroup( &This->IDirectPlay4A_iface, group, &dpn, NULL, 0, 0 );
}

static HRESULT WINAPI IDirectPlayImpl_DeletePlayerFromGroup( IDirectPlay *iface, DPID group,
        DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    TRACE( "(%p)->(0x%08lx,0x%08lx)\n", This, group, player );
    return IDirectPlayX_DeletePlayerFromGroup( &This->IDirectPlay4A_iface, group, player );
}

static HRESULT WINAPI IDirectPlayImpl_DestroyPlayer( IDirectPlay *iface, DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    TRACE( "(%p)->(0x%08lx)\n", This, player );
    return IDirectPlayX_DestroyPlayer( &This->IDirectPlay4A_iface, player );
}

static HRESULT WINAPI IDirectPlayImpl_DestroyGroup( IDirectPlay *iface, DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    TRACE( "(%p)->(0x%08lx)\n", This, group );
    return IDirectPlayX_DestroyGroup( &This->IDirectPlay4A_iface, group );
}

static HRESULT WINAPI IDirectPlayImpl_EnableNewPlayers( IDirectPlay *iface, BOOL enable )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );

    TRACE( "(%p)->(%d)\n", This, enable );

    /* as Windows answers (apistate, 2026-10-21): UNINITIALIZED without a connection, NOSESSIONS
     * without an open session (also after Close or a failed join); hosting, it sets or clears
     * DPSESSION_NEWPLAYERSDISABLED in the session desc. AoE2 calls it with FALSE as a game starts.
     * [UNV] a joiner's answer, and whether Windows announces the change as SetSessionDesc does. */
    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;
    if ( !This->dp2->bConnectionOpen || !This->dp2->lpSessionDesc )
        return DPERR_NOSESSIONS;

    /* not through SetSessionDesc, and nothing goes out: Windows queues no DPSYS_SETSESSIONDESC for
     * its own players (apistate, 2026-10-22) and sends the other machines nothing either - a joiner
     * reading through IDirectPlay or IDirectPlay4 got no message for four calls that changed nothing
     * and two that did (okplay/tests/dp1drop.c hostenp, 2026-10-22). Necrodome's host calls it four
     * times a second in its lobby; ours used to send each joiner a SESSIONDESCCHANGED every time.
     * Only the host's own session changes: new players are refused, and the room list (okplay reads
     * the host's enum reply) shows it. */
    if ( !This->dp2->bHostInterface )
        return DPERR_ACCESSDENIED;   /* [UNV] a joiner's answer */
    EnterCriticalSection( &This->lock );
    if ( enable )
        This->dp2->lpSessionDesc->dwFlags &= ~DPSESSION_NEWPLAYERSDISABLED;
    else
        This->dp2->lpSessionDesc->dwFlags |= DPSESSION_NEWPLAYERSDISABLED;
    LeaveCriticalSection( &This->lock );
    return DP_OK;
}

/* DirectPlay 2's player callbacks, handed on as DirectPlay 1's (both names as char*, may be NULL) */
struct dp1_enum_players
{
    LPDPENUMPLAYERSCALLBACK cb;
    void *context;
};

static BOOL CALLBACK dp1_enum_players_cb( DPID id, DWORD type, const DPNAME *name, DWORD flags,
        void *context )
{
    struct dp1_enum_players *e = context;
    return e->cb( id, name ? name->lpszShortNameA : NULL, name ? name->lpszLongNameA : NULL, flags,
                  e->context );
}

static HRESULT WINAPI IDirectPlayImpl_EnumGroupPlayers( IDirectPlay *iface, DPID group,
        LPDPENUMPLAYERSCALLBACK enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    struct dp1_enum_players e = { enumplayercb, context };
    TRACE( "(%p)->(0x%08lx,%p,%p,0x%08lx)\n", This, group, enumplayercb, context, flags );
    if ( !enumplayercb ) return DPERR_INVALIDPARAMS;
    return IDirectPlayX_EnumGroupPlayers( &This->IDirectPlay4A_iface, group, NULL, dp1_enum_players_cb,
                                          &e, flags );
}

/* DirectPlay 1's EnumGroups/EnumPlayers take a session number: used with the _SESSION flag */
static HRESULT WINAPI IDirectPlayImpl_EnumGroups( IDirectPlay *iface, DWORD session,
        LPDPENUMPLAYERSCALLBACK enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    struct dp1_enum_players e = { enumplayercb, context };
    const GUID *instance = NULL;
    TRACE( "(%p)->(0x%08lx,%p,%p,0x%08lx)\n", This, session, enumplayercb, context, flags );
    if ( !enumplayercb ) return DPERR_INVALIDPARAMS;
    if ( flags & DPENUMGROUPS_SESSION )
    {
        if ( !(instance = dp1_session_guid( This, session )) ) return DPERR_INVALIDPARAMS;
    }
    return IDirectPlayX_EnumGroups( &This->IDirectPlay4A_iface, (GUID *)instance, dp1_enum_players_cb,
                                    &e, flags );
}

static HRESULT WINAPI IDirectPlayImpl_EnumPlayers( IDirectPlay *iface, DWORD session,
        LPDPENUMPLAYERSCALLBACK enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    struct dp1_enum_players e = { enumplayercb, context };
    const GUID *instance = NULL;
    TRACE( "(%p)->(0x%08lx,%p,%p,0x%08lx)\n", This, session, enumplayercb, context, flags );
    if ( !enumplayercb ) return DPERR_INVALIDPARAMS;
    if ( flags & DPENUMPLAYERS_SESSION )
    {
        if ( !(instance = dp1_session_guid( This, session )) ) return DPERR_INVALIDPARAMS;
    }
    return IDirectPlayX_EnumPlayers( &This->IDirectPlay4A_iface, (GUID *)instance, dp1_enum_players_cb,
                                     &e, flags );
}

/* DirectPlay 2's session callbacks, handed on as DirectPlay 1's DPSESSIONDESC */
struct dp1_enum_sessions
{
    IDirectPlayImpl *This;
    LPDPENUMSESSIONSCALLBACK cb;
    void *context;
};

static BOOL CALLBACK dp1_enum_sessions_cb( const DPSESSIONDESC2 *sd2, DWORD *timeout, DWORD flags,
        void *context )
{
    struct dp1_enum_sessions *e = context;
    DPSESSIONDESC sd;

    if ( !sd2 ) return e->cb( NULL, e->context, timeout, flags );   /* DPESC_TIMEDOUT */
    memset( &sd, 0, sizeof(sd) );
    sd.dwSize = sizeof(sd);
    sd.guidSession = sd2->guidApplication;
    sd.dwSession = dp1_session_number( e->This, &sd2->guidInstance );
    sd.dwMaxPlayers = sd2->dwMaxPlayers;
    sd.dwCurrentPlayers = sd2->dwCurrentPlayers;
    sd.dwFlags = sd2->dwFlags;
    if ( sd2->lpszSessionNameA ) lstrcpynA( sd.szSessionName, sd2->lpszSessionNameA, DPSESSIONNAMELEN );
    if ( sd2->lpszPasswordA ) lstrcpynA( sd.szPassword, sd2->lpszPasswordA, DPPASSWORDLEN );
    sd.dwReserved1 = sd2->dwReserved1;
    sd.dwReserved2 = sd2->dwReserved2;
    sd.dwUser1 = sd2->dwUser1;
    sd.dwUser2 = sd2->dwUser2;
    sd.dwUser3 = sd2->dwUser3;
    sd.dwUser4 = sd2->dwUser4;
    return e->cb( &sd, e->context, timeout, flags );
}

static HRESULT WINAPI IDirectPlayImpl_EnumSessions( IDirectPlay *iface, DPSESSIONDESC *sdesc,
        DWORD timeout, LPDPENUMSESSIONSCALLBACK enumsessioncb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    struct dp1_enum_sessions e = { This, enumsessioncb, context };
    DPSESSIONDESC2 sd2;
    char name[ DPSESSIONNAMELEN + 1 ], password[ DPPASSWORDLEN + 1 ];

    TRACE( "(%p)->(%p,%lu,%p,%p,0x%08lx)\n", This, sdesc, timeout, enumsessioncb, context, flags );
    if ( !sdesc || !enumsessioncb ) return DPERR_INVALIDPARAMS;
    dp1_to_sd2( sdesc, &sd2, name, password );
    sd2.dwFlags = 0;   /* (asked for: the game and the password; DirectPlay 1 has no async enumeration) */
    return IDirectPlayX_EnumSessions( &This->IDirectPlay4A_iface, &sd2, timeout, dp1_enum_sessions_cb,
                                      &e, flags );
}

static HRESULT WINAPI IDirectPlayImpl_GetCaps( IDirectPlay *iface, DPCAPS *caps )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    TRACE( "(%p)->(%p)\n", This, caps );
    return dp1_caps( This, DPID_ALLPLAYERS, caps );
}

static HRESULT WINAPI IDirectPlayImpl_GetMessageCount( IDirectPlay *iface, DPID player,
        DWORD *count )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    TRACE( "(%p)->(0x%08lx,%p)\n", This, player, count );
    return IDirectPlayX_GetMessageCount( &This->IDirectPlay4A_iface, player, count );
}

static HRESULT WINAPI IDirectPlayImpl_GetPlayerCaps( IDirectPlay *iface, DPID player, DPCAPS *caps )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    TRACE( "(%p)->(0x%08lx,%p)\n", This, player, caps );
    return dp1_caps( This, player, caps );
}

/* The two names into the caller's two buffers. As Windows answers: an unknown player -
 * INVALIDPLAYER, sizes untouched; a buffer too small (or NULL with size 0) - BUFFERTOOSMALL with
 * both sizes needed (terminating 0 counted) and nothing copied; a name not set - size 0. */
static HRESULT WINAPI IDirectPlayImpl_GetPlayerName( IDirectPlay *iface, DPID player, LPSTR name,
        DWORD *size_name, LPSTR fullname, DWORD *size_fullname )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    BYTE stack[ 256 ], *buf = stack;
    DWORD size = sizeof(stack), need_name, need_full;
    const DPNAME *dpn;
    HRESULT hr;

    TRACE( "(%p)->(0x%08lx,%p,%p,%p,%p)\n", This, player, name, size_name, fullname, size_fullname );
    if ( !size_name || !size_fullname ) return DPERR_INVALIDPARAMS;
    hr = DP_IF_GetPlayerName( This, player, buf, &size, TRUE );
    if ( hr == DPERR_BUFFERTOOSMALL )
    {
        if ( !(buf = malloc( size )) ) return DPERR_OUTOFMEMORY;
        hr = DP_IF_GetPlayerName( This, player, buf, &size, TRUE );
    }
    if ( FAILED(hr) )
    {
        if ( buf != stack ) free( buf );
        return hr;
    }
    dpn = (const DPNAME *)buf;
    need_name = dpn->lpszShortNameA ? strlen( dpn->lpszShortNameA ) + 1 : 0;
    need_full = dpn->lpszLongNameA ? strlen( dpn->lpszLongNameA ) + 1 : 0;
    if ( *size_name < need_name || *size_fullname < need_full ||
         (need_name && !name) || (need_full && !fullname) )
        hr = DPERR_BUFFERTOOSMALL;
    else
    {
        if ( need_name ) memcpy( name, dpn->lpszShortNameA, need_name );
        if ( need_full ) memcpy( fullname, dpn->lpszLongNameA, need_full );
    }
    *size_name = need_name;
    *size_fullname = need_full;
    if ( buf != stack ) free( buf );
    return hr;
}

static HRESULT WINAPI IDirectPlayImpl_Initialize( IDirectPlay *iface, GUID *guid )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    TRACE( "(%p)->(%s)\n", This, debugstr_guid( guid ) );
    return IDirectPlayX_Initialize( &This->IDirectPlay4A_iface, guid );
}

/* DirectPlay 1's Open: DPOPEN_CREATESESSION/OPENSESSION (= DPOPEN_CREATE/JOIN) in sdesc->dwFlags;
 * a session to join is named by the number its EnumSessions gave it */
static HRESULT WINAPI IDirectPlayImpl_Open( IDirectPlay *iface, DPSESSIONDESC *sdesc )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    DPSESSIONDESC2 sd2;
    char name[ DPSESSIONNAMELEN + 1 ], password[ DPPASSWORDLEN + 1 ];
    const GUID *instance;

    TRACE( "(%p)->(%p)\n", This, sdesc );
    if ( !sdesc || sdesc->dwSize != sizeof(*sdesc) ) return DPERR_INVALIDPARAMS;
    dp1_to_sd2( sdesc, &sd2, name, password );
    if ( sdesc->dwFlags & DPOPEN_JOIN )
    {
        if ( !(instance = dp1_session_guid( This, sdesc->dwSession )) ) return DPERR_INVALIDPARAMS;
        sd2.guidInstance = *instance;
    }
    else
        sd2.lpszPasswordA = password;   /* a session made here keeps DirectPlay 1's char[] as a
                                         * string, "" when empty, as Windows' GetSessionDesc shows */
    return IDirectPlayX_Open( &This->IDirectPlay4A_iface, &sd2, sdesc->dwFlags & (DPOPEN_JOIN | DPOPEN_CREATE) );
}

/* Receive: the next message as DirectPlay 1 delivers it. DirectPlay 2's "created" and "destroyed"
 * become DirectPlay 1's DPMSG_ADDPLAYER (names as char[], long name first) and an id-only delete
 * message (DELETEPLAYER or DELETEGROUP); everything else is DirectPlay 2's Unicode message, as Windows
 * passes it on (names in DPNAME pointing into the caller's buffer). */
static HRESULT WINAPI IDirectPlayImpl_Receive( IDirectPlay *iface, DPID *from, DPID *to,
        DWORD flags, void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    BYTE stack[ 512 ], *buf = stack;
    DWORD bufsize = sizeof(stack), need;
    DPID f = 0, t = 0;
    HRESULT hr;
    DWORD type;

    TRACE( "(%p)->(%p,%p,0x%08lx,%p,%p)\n", This, from, to, flags, data, size );
    if ( !from || !to || !size ) return DPERR_INVALIDPARAMS;
    /* look first: is it one of the two that change shape? (only those are copied through here) */
    for ( ;; )
    {
        DWORD got = bufsize;
        /* *from / *to are inputs too: DPRECEIVE_FROMPLAYER / TOPLAYER pick by them (Necrodome
         * receives with TOPLAYER and its player's id) */
        f = *from;
        t = *to;
        hr = DP_IF_Receive( This, &f, &t, flags | DPRECEIVE_PEEK, buf, &got, FALSE );
        if ( hr != DPERR_BUFFERTOOSMALL ) break;
        if ( buf != stack ) free( buf );
        if ( !(buf = malloc( got )) ) return DPERR_OUTOFMEMORY;
        bufsize = got;
    }
    if ( FAILED(hr) || f != DPID_SYSMSG ||
         ((type = ((DPMSG_GENERIC *)buf)->dwType) != DPSYS_CREATEPLAYERORGROUP && type != DPSYS_DESTROYPLAYERORGROUP) )
    {
        if ( buf != stack ) free( buf );
        if ( FAILED(hr) ) return hr;
        hr = DP_IF_Receive( This, from, to, flags, data, size, FALSE );   /* as it is, into the caller's buffer */
        if ( SUCCEEDED(hr) && TRACE_ON( dplay ) )
        {
            const BYTE *b = data;
            DWORD n = *size < 16 ? *size : 16, i;
            char hex[ 64 ];
            for ( i = 0; i < n; i++ ) sprintf( hex + i * 3, "%02x ", b[ i ] );
            hex[ n * 3 ] = 0;
            TRACE( "dp1 received 0x%08lx -> 0x%08lx, %lu bytes: %s\n", *from, *to, *size, hex );
        }
        return hr;
    }
    need = type == DPSYS_CREATEPLAYERORGROUP ? sizeof(DP1MSG_ADDPLAYER) : sizeof(DP1MSG_DELETEPLAYER);
    if ( !data || *size < need )
    {
        *size = need;
        if ( buf != stack ) free( buf );
        return DPERR_BUFFERTOOSMALL;
    }
    if ( !(flags & DPRECEIVE_PEEK) )
    {
        DWORD got = bufsize;
        f = *from;
        t = *to;
        hr = DP_IF_Receive( This, &f, &t, flags, buf, &got, FALSE );   /* take it off the queue */
        if ( FAILED(hr) )
        {
            if ( buf != stack ) free( buf );
            return hr;
        }
    }
    if ( type == DPSYS_CREATEPLAYERORGROUP )
    {
        const DPMSG_CREATEPLAYERORGROUP *c = (const DPMSG_CREATEPLAYERORGROUP *)buf;
        DP1MSG_ADDPLAYER m;
        memset( &m, 0, sizeof(m) );
        m.dwType = DP1SYS_ADDPLAYER;
        m.dwPlayerType = c->dwPlayerType == DPPLAYERTYPE_PLAYER ? 1 : 0;
        m.dpId = c->dpId;
        if ( c->dpnName.lpszLongName )
            WideCharToMultiByte( CP_ACP, 0, c->dpnName.lpszLongName, -1, m.szLongName, DPLONGNAMELEN, NULL, NULL );
        if ( c->dpnName.lpszShortName )
            WideCharToMultiByte( CP_ACP, 0, c->dpnName.lpszShortName, -1, m.szShortName, DPSHORTNAMELEN, NULL, NULL );
        m.szLongName[ DPLONGNAMELEN - 1 ] = m.szShortName[ DPSHORTNAMELEN - 1 ] = 0;
        m.dwCurrentPlayers = c->dwCurrentPlayers;
        memcpy( data, &m, sizeof(m) );
    }
    else
    {
        const DPMSG_DESTROYPLAYERORGROUP *d = (const DPMSG_DESTROYPLAYERORGROUP *)buf;
        DP1MSG_DELETEPLAYER m;
        m.dwType = d->dwPlayerType == DPPLAYERTYPE_PLAYER ? DP1SYS_DELETEPLAYER : DP1SYS_DELETEGROUP;
        m.dpId = d->dpId;
        memcpy( data, &m, sizeof(m) );
    }
    *size = need;
    *from = f;
    *to = t;
    if ( buf != stack ) free( buf );
    return DP_OK;
}

static HRESULT WINAPI IDirectPlayImpl_SaveSession( IDirectPlay *iface, LPSTR reserved )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    TRACE( "(%p)->(%p)\n", This, reserved );
    return E_NOTIMPL;   /* as Windows answers */
}

static HRESULT WINAPI IDirectPlayImpl_Send( IDirectPlay *iface, DPID from, DPID to, DWORD flags,
        void *data, DWORD size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    TRACE( "(%p)->(0x%08lx,0x%08lx,0x%08lx,%p,%lu)\n", This, from, to, flags, data, size );
    HRESULT hr = IDirectPlayX_Send( &This->IDirectPlay4A_iface, from, to, flags, data, size );
    if ( TRACE_ON( dplay ) && data )
    {
        const BYTE *b = data;
        DWORD n = size < 16 ? size : 16, i;
        char hex[ 64 ];
        for ( i = 0; i < n; i++ ) sprintf( hex + i * 3, "%02x ", b[ i ] );
        hex[ n * 3 ] = 0;
        TRACE( "dp1 sent 0x%08lx -> 0x%08lx flags 0x%lx, %lu bytes: %s -> 0x%08lx\n", from, to, flags, size, hex, hr );
    }
    return hr;
}

static HRESULT WINAPI IDirectPlayImpl_SetPlayerName( IDirectPlay *iface, DPID player, LPSTR name,
        LPSTR fullname )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay( iface );
    DPNAME dpn;
    TRACE( "(%p)->(0x%08lx,%s,%s)\n", This, player, debugstr_a( name ), debugstr_a( fullname ) );
    dp1_name( &dpn, name, fullname );
    return IDirectPlayX_SetPlayerName( &This->IDirectPlay4A_iface, player, &dpn, 0 );
}

static const IDirectPlayVtbl dp_vt =
{
    IDirectPlayImpl_QueryInterface,
    IDirectPlayImpl_AddRef,
    IDirectPlayImpl_Release,
    IDirectPlayImpl_AddPlayerToGroup,
    IDirectPlayImpl_Close,
    IDirectPlayImpl_CreatePlayer,
    IDirectPlayImpl_CreateGroup,
    IDirectPlayImpl_DeletePlayerFromGroup,
    IDirectPlayImpl_DestroyPlayer,
    IDirectPlayImpl_DestroyGroup,
    IDirectPlayImpl_EnableNewPlayers,
    IDirectPlayImpl_EnumGroupPlayers,
    IDirectPlayImpl_EnumGroups,
    IDirectPlayImpl_EnumPlayers,
    IDirectPlayImpl_EnumSessions,
    IDirectPlayImpl_GetCaps,
    IDirectPlayImpl_GetMessageCount,
    IDirectPlayImpl_GetPlayerCaps,
    IDirectPlayImpl_GetPlayerName,
    IDirectPlayImpl_Initialize,
    IDirectPlayImpl_Open,
    IDirectPlayImpl_Receive,
    IDirectPlayImpl_SaveSession,
    IDirectPlayImpl_Send,
    IDirectPlayImpl_SetPlayerName,
};


static HRESULT WINAPI IDirectPlay2AImpl_QueryInterface( IDirectPlay2A *iface, REFIID riid,
        void **ppv )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_QueryInterface( &This->IDirectPlay4_iface, riid, ppv );
}

static HRESULT WINAPI IDirectPlay2Impl_QueryInterface( IDirectPlay2 *iface, REFIID riid,
        void **ppv )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_QueryInterface( &This->IDirectPlay4_iface, riid, ppv );
}

static HRESULT WINAPI IDirectPlay3AImpl_QueryInterface( IDirectPlay3A *iface, REFIID riid,
        void **ppv )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_QueryInterface( &This->IDirectPlay4A_iface, riid, ppv );
}

static HRESULT WINAPI IDirectPlay3Impl_QueryInterface( IDirectPlay3 *iface, REFIID riid,
        void **ppv )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_QueryInterface( &This->IDirectPlay4_iface, riid, ppv );
}

static HRESULT WINAPI IDirectPlay4AImpl_QueryInterface( IDirectPlay4A *iface, REFIID riid,
        void **ppv )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_QueryInterface( &This->IDirectPlay4_iface, riid, ppv );
}

static HRESULT WINAPI IDirectPlay4Impl_QueryInterface( IDirectPlay4 *iface, REFIID riid,
        void **ppv )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );

    if ( IsEqualGUID( &IID_IUnknown, riid ) )
    {
        TRACE( "(%p)->(IID_IUnknown %p)\n", This, ppv );
        *ppv = &This->IDirectPlay_iface;
    }
    else if ( IsEqualGUID( &IID_IDirectPlay, riid ) )
    {
        TRACE( "(%p)->(IID_IDirectPlay %p)\n", This, ppv );
        *ppv = &This->IDirectPlay_iface;
    }
    else if ( IsEqualGUID( &IID_IDirectPlay2A, riid ) )
    {
        TRACE( "(%p)->(IID_IDirectPlay2A %p)\n", This, ppv );
        *ppv = &This->IDirectPlay2A_iface;
    }
    else if ( IsEqualGUID( &IID_IDirectPlay2, riid ) )
    {
        TRACE( "(%p)->(IID_IDirectPlay2 %p)\n", This, ppv );
        *ppv = &This->IDirectPlay2_iface;
    }
    else if ( IsEqualGUID( &IID_IDirectPlay3A, riid ) )
    {
        TRACE( "(%p)->(IID_IDirectPlay3A %p)\n", This, ppv );
        *ppv = &This->IDirectPlay3A_iface;
    }
    else if ( IsEqualGUID( &IID_IDirectPlay3, riid ) )
    {
        TRACE( "(%p)->(IID_IDirectPlay3 %p)\n", This, ppv );
        *ppv = &This->IDirectPlay3_iface;
    }
    else if ( IsEqualGUID( &IID_IDirectPlay4A, riid ) )
    {
        TRACE( "(%p)->(IID_IDirectPlay4A %p)\n", This, ppv );
        *ppv = &This->IDirectPlay4A_iface;
    }
    else if ( IsEqualGUID( &IID_IDirectPlay4, riid ) )
    {
        TRACE( "(%p)->(IID_IDirectPlay4 %p)\n", This, ppv );
        *ppv = &This->IDirectPlay4_iface;
    }
    else
    {
        WARN("(%p)->(%s %p)\n", This, debugstr_guid(riid), ppv);
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown*)*ppv);
    return S_OK;
}

static ULONG WINAPI IDirectPlay2AImpl_AddRef( IDirectPlay2A *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    ULONG ref = InterlockedIncrement( &This->ref );

    TRACE( "(%p) ref=%ld\n", This, ref );

    return ref;
}

static ULONG WINAPI IDirectPlay2Impl_AddRef( IDirectPlay2 *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    ULONG ref = InterlockedIncrement( &This->ref );

    TRACE( "(%p) ref=%ld\n", This, ref );

    return ref;
}

static ULONG WINAPI IDirectPlay3AImpl_AddRef( IDirectPlay3A *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    ULONG ref = InterlockedIncrement( &This->ref );

    TRACE( "(%p) ref=%ld\n", This, ref );

    return ref;
}

static ULONG WINAPI IDirectPlay3Impl_AddRef( IDirectPlay3 *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    ULONG ref = InterlockedIncrement( &This->ref );

    TRACE( "(%p) ref=%ld\n", This, ref );

    return ref;
}

static ULONG WINAPI IDirectPlay4AImpl_AddRef(IDirectPlay4A *iface)
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    ULONG ref = InterlockedIncrement( &This->ref );

    TRACE( "(%p) ref=%ld\n", This, ref );

    return ref;
}

static ULONG WINAPI IDirectPlay4Impl_AddRef(IDirectPlay4 *iface)
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    ULONG ref = InterlockedIncrement( &This->ref );

    TRACE( "(%p) ref=%ld\n", This, ref );

    return ref;
}

static ULONG WINAPI IDirectPlay2AImpl_Release( IDirectPlay2A *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    ULONG ref = InterlockedDecrement( &This->ref );

    TRACE( "(%p) ref=%ld\n", This, ref );

    if ( !ref )
        dplay_destroy( This );

    return ref;
}

static ULONG WINAPI IDirectPlay2Impl_Release( IDirectPlay2 *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    ULONG ref = InterlockedDecrement( &This->ref );

    TRACE( "(%p) ref=%ld\n", This, ref );

    if ( !ref  )
        dplay_destroy( This );

    return ref;
}

static ULONG WINAPI IDirectPlay3AImpl_Release( IDirectPlay3A *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    ULONG ref = InterlockedDecrement( &This->ref );

    TRACE( "(%p) ref=%ld\n", This, ref );

    if ( !ref )
        dplay_destroy( This );

    return ref;
}

static ULONG WINAPI IDirectPlay3Impl_Release( IDirectPlay3 *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    ULONG ref = InterlockedDecrement( &This->ref );

    TRACE( "(%p) ref=%ld\n", This, ref );

    if ( !ref )
        dplay_destroy( This );

    return ref;
}

static ULONG WINAPI IDirectPlay4AImpl_Release(IDirectPlay4A *iface)
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    ULONG ref = InterlockedDecrement( &This->ref );

    TRACE( "(%p) ref=%ld\n", This, ref );

    if ( !ref )
        dplay_destroy( This );

    return ref;
}

static ULONG WINAPI IDirectPlay4Impl_Release(IDirectPlay4 *iface)
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    ULONG ref = InterlockedDecrement( &This->ref );

    TRACE( "(%p) ref=%ld\n", This, ref );

    if ( !ref )
        dplay_destroy( This );

    return ref;
}

static HRESULT WINAPI IDirectPlay2AImpl_AddPlayerToGroup( IDirectPlay2A *iface, DPID group,
        DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_AddPlayerToGroup( &This->IDirectPlay4A_iface, group, player );
}

static HRESULT WINAPI IDirectPlay2Impl_AddPlayerToGroup( IDirectPlay2 *iface, DPID group,
        DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_AddPlayerToGroup( &This->IDirectPlay4_iface, group, player );
}

static HRESULT WINAPI IDirectPlay3AImpl_AddPlayerToGroup( IDirectPlay3A *iface, DPID group,
        DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_AddPlayerToGroup( &This->IDirectPlay4A_iface, group, player );
}

static HRESULT WINAPI IDirectPlay3Impl_AddPlayerToGroup( IDirectPlay3 *iface, DPID group,
        DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_AddPlayerToGroup( &This->IDirectPlay4_iface, group, player );
}

static HRESULT WINAPI IDirectPlay4AImpl_AddPlayerToGroup( IDirectPlay4A *iface, DPID group,
        DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_AddPlayerToGroup( &This->IDirectPlay4_iface, group, player );
}

HRESULT DP_AddPlayerToGroup( IDirectPlayImpl *This, DPID group, DPID player )
{
    DPMSG_ADDPLAYERTOGROUP addPlayerToGroupMsg;
    lpGroupData  gdata;
    lpPlayerList plist;
    lpPlayerList newplist;
    HRESULT hr;

    /* Find the group */
    if ( ( gdata = DP_FindGameGroup( This, group ) ) == NULL )
        return DPERR_INVALIDGROUP;

    /* Find the player */
    if ( ( plist = DP_FindPlayer( This, player ) ) == NULL )
        return DPERR_INVALIDPLAYER;

    /* Create a player list (ie "shortcut" ) */
    newplist = calloc( 1, sizeof( *newplist ) );
    if ( !newplist )
        return DPERR_CANTADDPLAYER;

    /* Add the shortcut */
    plist->lpPData->uRef++;
    newplist->lpPData = plist->lpPData;

    /* Add the player to the list of players for this group */
    DPQ_INSERT(gdata->players, newplist, players);

    /* Let the SP know that we've added a player to the group */
    if ( This->dp2->spData.lpCB->AddPlayerToGroup )
    {
        DPSP_ADDPLAYERTOGROUPDATA data;

        TRACE( "Calling SP AddPlayerToGroup\n" );

        data.idPlayer = player;
        data.idGroup  = group;
        data.lpISP    = This->dp2->spData.lpISP;

        hr = (*This->dp2->spData.lpCB->AddPlayerToGroup)( &data );
        if ( FAILED( hr ) )
        {
            DPQ_REMOVE( gdata->players, newplist, players );
            --plist->lpPData->uRef;
            free( newplist );
            return hr;
        }
    }

    addPlayerToGroupMsg.dwType = DPSYS_ADDPLAYERTOGROUP;
    addPlayerToGroupMsg.dpIdGroup = group;
    addPlayerToGroupMsg.dpIdPlayer = player;

    hr = DP_QueueMessage( This, DPID_SYSMSG, DPID_ALLPLAYERS, 0, &addPlayerToGroupMsg,
                          DP_CopyGeneric, sizeof( DPMSG_ADDPLAYERTOGROUP ) );
    if ( FAILED( hr ) )
    {
        if ( This->dp2->spData.lpCB->RemovePlayerFromGroup )
        {
            DPSP_REMOVEPLAYERFROMGROUPDATA data;
            data.idPlayer = player;
            data.idGroup = group;
            data.lpISP = This->dp2->spData.lpISP;
            This->dp2->spData.lpCB->RemovePlayerFromGroup( &data );
        }
        DPQ_REMOVE( gdata->players, newplist, players );
        --plist->lpPData->uRef;
        free( newplist );
        return hr;
    }

    return DP_OK;
}

static HRESULT WINAPI IDirectPlay4Impl_AddPlayerToGroup( IDirectPlay4 *iface, DPID group,
        DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    HRESULT hr;

    TRACE( "(%p)->(0x%08lx,0x%08lx)\n", This, group, player );

    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;

    EnterCriticalSection( &This->lock );

    hr = DP_AddPlayerToGroup( This, group, player );
    if ( FAILED( hr ) )
    {
        LeaveCriticalSection( &This->lock );
        return hr;
    }

    /* Inform all other peers of the addition of player to the group. If there are
     * no peers keep this event quiet.
     * Also, if this event was the result of another machine sending it to us,
     * don't bother rebroadcasting it.
     */
    hr = DP_MSG_SendAddPlayerToGroup( This, DPID_ALLPLAYERS, player, group );
    if( FAILED( hr ) )
    {
        LeaveCriticalSection( &This->lock );
        return hr;
    }

    LeaveCriticalSection( &This->lock );

    return DP_OK;
}

static HRESULT WINAPI IDirectPlay2AImpl_Close( IDirectPlay2A *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_Close( &This->IDirectPlay4A_iface );
}

static HRESULT WINAPI IDirectPlay2Impl_Close( IDirectPlay2 *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_Close( &This->IDirectPlay4_iface );
}

static HRESULT WINAPI IDirectPlay3AImpl_Close( IDirectPlay3A *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_Close( &This->IDirectPlay4A_iface );
}

static HRESULT WINAPI IDirectPlay3Impl_Close( IDirectPlay3 *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_Close( &This->IDirectPlay4_iface );
}

static HRESULT WINAPI IDirectPlay4AImpl_Close( IDirectPlay4A *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_Close( &This->IDirectPlay4_iface);
}

static void DP_SPDeleteAllPlayers( IDirectPlayImpl *This );

static HRESULT WINAPI IDirectPlay4Impl_Close( IDirectPlay4 *iface )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    HRESULT hr = DP_OK;

    TRACE( "(%p)\n", This );

    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;
    if ( !This->dp2->bConnectionOpen )
        return DPERR_NOSESSIONS;

    DP_LIFE_Leaving( This );
    DP_LIFE_Stop( This );

    EnterCriticalSection( &This->lock );

    This->dp2->bConnectionOpen = FALSE;

    LeaveCriticalSection( &This->lock );

    /* FIXME: Need to find a new host I assume (how?) */
    /* FIXME: Need to destroy all local groups */
    /* FIXME: Need to migrate all remotely visible players to the new host */

    DP_SPDeleteAllPlayers( This );

    /* Invoke the SP callback to inform of session close */
    if( This->dp2->spData.lpCB->CloseEx )
    {
        DPSP_CLOSEDATA data;

        TRACE( "Calling SP CloseEx\n" );
        data.lpISP = This->dp2->spData.lpISP;
        hr = (*This->dp2->spData.lpCB->CloseEx)( &data );
    }
    else if ( This->dp2->spData.lpCB->Close ) /* Try obsolete version */
    {
        TRACE( "Calling SP Close (obsolete interface)\n" );
        hr = (*This->dp2->spData.lpCB->Close)();
    }

    DP_ResetSession( This );   /* a later Open starts clean */

    return hr;
}

HRESULT DP_CreateGroup( IDirectPlayImpl *This, void *msgHeader, const DPID *lpid,
        const DPNAME *lpName, void *data, DWORD dataSize, DWORD dwFlags, DPID idParent,
        BOOL bAnsi )
{
  struct GroupList *groupList = NULL;
  struct GroupData *parent = NULL;
  lpGroupData lpGData;
  HRESULT hr;

  if( DPID_SYSTEM_GROUP != *lpid )
  {
    parent = DP_FindAnyGroup( This, idParent );
    if( !parent )
      return DPERR_INVALIDGROUP;
  }

  /* Allocate the new space and add to end of high level group list */
  lpGData = calloc( 1, sizeof( *lpGData ) );

  if( lpGData == NULL )
  {
    return DPERR_OUTOFMEMORY;
  }

  DPQ_INIT(lpGData->groups);
  DPQ_INIT(lpGData->players);

  /* Set the desired player ID - no sanity checking to see if it exists */
  lpGData->dpid = *lpid;

  lpGData->name = DP_DuplicateName( lpName, FALSE, bAnsi, msgHeader != NULL );
  if ( !lpGData->name )
  {
    free( lpGData );
    return DPERR_OUTOFMEMORY;
  }

  lpGData->nameA = DP_DuplicateName( lpName, TRUE, bAnsi, msgHeader != NULL );
  if ( !lpGData->nameA )
  {
    free( lpGData->name );
    free( lpGData );
    return DPERR_OUTOFMEMORY;
  }

  lpGData->parent = idParent;

  /* FIXME: Should we validate the dwFlags? */
  lpGData->dwFlags = dwFlags;
  lpGData->bLocal = msgHeader == NULL && *lpid != DPID_SYSTEM_GROUP;

  /* Initialize the SP data section */
  lpGData->lpSPPlayerData = DPSP_CreateSPPlayerData();
  if ( !lpGData->lpSPPlayerData )
  {
    free( lpGData->nameA );
    free( lpGData->name );
    free( lpGData );
    return DPERR_OUTOFMEMORY;
  }

  if( DPID_SYSTEM_GROUP == *lpid )
  {
    This->dp2->lpSysGroup = lpGData;
    TRACE( "Inserting system group\n" );
  }
  else
  {
    /* Insert into the parent group */
    groupList = calloc( 1, sizeof( *groupList ) );
    if( !groupList )
    {
      free( lpGData->nameA );
      free( lpGData->name );
      free( lpGData );
      return DPERR_OUTOFMEMORY;
    }
    groupList->lpGData = lpGData;

    DPQ_INSERT( parent->groups, groupList, groups );
  }

  /* Something is now referencing this data */
  lpGData->uRef++;

  DP_SetGroupData( lpGData, DPSET_REMOTE, data, dataSize );

  /* FIXME: We should only create the system group if GetCaps returns
   *        DPCAPS_GROUPOPTIMIZED.
   */

  /* Let the SP know that we've created this group */
  if( This->dp2->spData.lpCB->CreateGroup )
  {
    DPSP_CREATEGROUPDATA data;
    DWORD dwCreateFlags = 0;

    TRACE( "Calling SP CreateGroup\n" );

    if( !parent )
      dwCreateFlags |= DPLAYI_GROUP_SYSGROUP;

    if( !msgHeader )
      dwCreateFlags |= DPLAYI_PLAYER_PLAYERLOCAL;

    if( dwFlags & DPGROUP_HIDDEN )
      dwCreateFlags |= DPLAYI_GROUP_HIDDEN;

    data.idGroup           = *lpid;
    data.dwFlags           = dwCreateFlags;
    data.lpSPMessageHeader = msgHeader;
    data.lpISP             = This->dp2->spData.lpISP;

    hr = (*This->dp2->spData.lpCB->CreateGroup)( &data );
    if( FAILED( hr ) )
    {
      if( groupList )
      {
        DPQ_REMOVE( parent->groups, groupList, groups );
        free( groupList );
      }
      else
      {
        This->dp2->lpSysGroup = NULL;
      }
      free( lpGData->nameA );
      free( lpGData->name );
      free( lpGData );
      return hr;
    }
  }

  TRACE( "Created group id 0x%08lx\n", *lpid );

  return DP_OK;
}

/* This method assumes that all links to it are already deleted */
static void DP_DeleteGroup( IDirectPlayImpl *This, DPID dpid )
{
  lpGroupList lpGList;

  TRACE( "(%p)->(0x%08lx)\n", This, dpid );

  DPQ_REMOVE_ENTRY( This->dp2->lpSysGroup->groups, groups, lpGData->dpid, ==, dpid, lpGList );

  if( lpGList == NULL )
  {
    ERR( "DPID 0x%08lx not found\n", dpid );
    return;
  }

  if( --(lpGList->lpGData->uRef) )
  {
    ERR( "group 0x%08lx still has %lu references - removing it anyway\n", dpid, lpGList->lpGData->uRef );
  }

  /* Delete player */
  free( lpGList->lpGData->lpSPPlayerData );
  free( lpGList->lpGData->nameA );
  free( lpGList->lpGData->name );
  free( lpGList->lpGData );

  /* Remove and Delete Player List object */
  free( lpGList );

}

/* a group's flags as Windows reports them (GetGroupFlags) and puts them in its create/destroy
 * messages: staging area and hidden as created, and DPGROUP_LOCAL for one made on this machine
 * (black box, 2026-10-22) */
#define DP_GROUP_FLAGS( g ) ( ((g)->dwFlags & (DPGROUP_STAGINGAREA | DPGROUP_HIDDEN)) | \
                              ((g)->bLocal ? DPGROUP_LOCAL : 0) )

/* is this entry of a group's list the group's own child (CreateGroupInGroup, or a top-level group
 * in the system group) rather than a shortcut (AddGroupToGroup)? */
#define DP_IS_CHILD_LINK( where, link ) ( (link)->lpGData->parent == (where)->dpid )

/* depth first through the groups in a group: a group created inside another (CreateGroupInGroup)
 * is only in its parent's list, and "any group" means any (apidiff: AddGroupToGroup of such a
 * group answered "invalid group"). Only into children: shortcuts can make loops (a parent can be
 * added to its own child, as Windows allows), and every group is some group's child. */
static lpGroupData DP_FindGroupIn( lpGroupData where, DPID dpid, int depth )
{
  lpGroupList lpGroups;
  lpGroupData found;

  if( !where || depth > 64 )
    return NULL;

  DPQ_FIND_ENTRY( where->groups, groups, lpGData->dpid, ==, dpid, lpGroups );
  if( lpGroups )
    return lpGroups->lpGData;

  for( lpGroups = DPQ_FIRST( where->groups ); lpGroups; lpGroups = DPQ_NEXT( lpGroups->groups ) )
  {
    if( !DP_IS_CHILD_LINK( where, lpGroups ) )
      continue;
    found = DP_FindGroupIn( lpGroups->lpGData, dpid, depth + 1 );
    if( found )
      return found;
  }

  return NULL;
}

lpGroupData DP_FindAnyGroup( IDirectPlayImpl *This, DPID dpid )
{
  TRACE( "(%p)->(0x%08lx)\n", This, dpid );

  /* no session (never opened, a failed Open, or closed): nothing to find - and nothing to
   * dereference (DP_FindPlayer had this check, this one didn't) */
  if( !This->dp2->lpSysGroup )
    return NULL;

  if( dpid == DPID_SYSTEM_GROUP )
    return This->dp2->lpSysGroup;

  return DP_FindGroupIn( This->dp2->lpSysGroup, dpid, 0 );
}

/* every player on this machine hears of a group made here, as of a player (Windows,
 * apistate_dp1.c: dwCurrentPlayers is the session's player count) - a group made inside another
 * one too, with its parent (CreateGroupInGroup, 2026-10-22) */
static void DP_QueueGroupCreated( IDirectPlayImpl *This, DPID id, void *data, DWORD dataSize,
                                  DWORD flags, DPID parent )
{
    lpGroupData gdata = DP_FindAnyGroup( This, id );
    DPMSG_CREATEPLAYERORGROUP msg;

    memset( &msg, 0, sizeof(msg) );
    msg.dwType           = DPSYS_CREATEPLAYERORGROUP;
    msg.dwPlayerType     = DPPLAYERTYPE_GROUP;
    msg.dpId             = id;
    msg.dwCurrentPlayers = This->dp2->lpSessionDesc ? This->dp2->lpSessionDesc->dwCurrentPlayers : 0;
    msg.lpData           = data;
    msg.dwDataSize       = dataSize;
    if( gdata && gdata->name ) msg.dpnName = *gdata->name;
    msg.dpIdParent       = parent;
    msg.dwFlags          = gdata ? DP_GROUP_FLAGS( gdata ) : DPMSG_CREATEGROUP_DWFLAGS( flags );
    DP_QueueMessage( This, DPID_SYSMSG, DPID_ALLPLAYERS, 0, &msg, DP_CopyCreatePlayerOrGroup, 0 );
}

void DP_QueueGroupCreatedFromWire( IDirectPlayImpl *This, DPID id, void *data, DWORD dataSize, DWORD flags,
                                   DPID parent )
{
    DP_QueueGroupCreated( This, id, data, dataSize, flags, parent );
}

/* a joiner's new group: the host hands out the id (REQUESTGROUPID, groups.c); asked before the lock is
 * taken, as CreatePlayer does, since the reply comes in on the provider's thread */
static HRESULT DP_GroupIdFromHost( IDirectPlayImpl *This, void *lpMsgHdr, DPID *lpidGroup )
{
  HRESULT hr;
  if( lpMsgHdr || DPID_UNKNOWN != *lpidGroup || This->dp2->bHostInterface || !This->dp2->bConnectionOpen )
    return DP_OK;
  hr = DP_MSG_SendRequestGroupId( This, DPLAYI_PLAYER_PLAYERLOCAL, lpidGroup );
  if( FAILED( hr ) )
    ERR( "group id from the host: %s\n", DPLAYX_HresultToString( hr ) );
  return hr;
}

static HRESULT DP_IF_CreateGroup( IDirectPlayImpl *This, void *lpMsgHdr, DPID *lpidGroup,
        DPNAME *lpGroupName, void *lpData, DWORD dwDataSize, DWORD dwFlags, BOOL bAnsi )
{
  HRESULT hr;

  TRACE( "(%p)->(%p,%p,%p,%p,0x%08lx,0x%08lx,%u)\n",
         This, lpMsgHdr, lpidGroup, lpGroupName, lpData, dwDataSize,
         dwFlags, bAnsi );

  if( This->dp2->connectionInitialized == NO_PROVIDER )
  {
    return DPERR_UNINITIALIZED;
  }

  /* a game can ask for a staging area and hidden, nothing else (Windows: DPGROUP_LOCAL or an
   * unknown bit is DPERR_INVALIDFLAGS, black box 2026-10-22) */
  if( lpMsgHdr == NULL && (dwFlags & ~(DPGROUP_STAGINGAREA | DPGROUP_HIDDEN)) )
  {
    return DPERR_INVALIDFLAGS;
  }

  if( FAILED( hr = DP_GroupIdFromHost( This, lpMsgHdr, lpidGroup ) ) )
    return hr;

  EnterCriticalSection( &This->lock );

  /* If the name is not specified, we must provide one */
  if( DPID_UNKNOWN == *lpidGroup )
  {
    /* If we are the name server, we decide on the group ids. If not, we
     * must ask for one before attempting a creation.
     */
    if( This->dp2->bHostInterface )
    {
      *lpidGroup = DP_NextObjectId();
    }
    else
    {
      *lpidGroup = DP_GetRemoteNextObjectId();
    }
  }

  hr = DP_CreateGroup( This, lpMsgHdr, lpidGroup, lpGroupName, lpData, dwDataSize, dwFlags,
                       DPID_NOPARENT_GROUP, bAnsi );

  if( FAILED( hr ) )
  {
    LeaveCriticalSection( &This->lock );
    return hr;
  }

  if( lpMsgHdr == NULL )
  {
    lpGroupData g = DP_FindAnyGroup( This, *lpidGroup );
    DP_QueueGroupCreated( This, *lpidGroup, lpData, dwDataSize, dwFlags, DPID_NOPARENT_GROUP );
    DP_GRP_SendCreate( This, *lpidGroup, DPID_NOPARENT_GROUP, dwFlags, g ? g->name : NULL, lpData, dwDataSize );
  }

  /* Inform all other peers of the creation of a new group. If there are
   * no peers keep this event quiet.
   * Also if this message was sent to us, don't rebroadcast.
   */
  if( ( lpMsgHdr == NULL ) &&
      This->dp2->lpSessionDesc &&
      ( This->dp2->lpSessionDesc->dwFlags & DPSESSION_MULTICASTSERVER ) )
  {
    DPMSG_CREATEPLAYERORGROUP msg;
    msg.dwType = DPSYS_CREATEPLAYERORGROUP;

    msg.dwPlayerType     = DPPLAYERTYPE_GROUP;
    msg.dpId             = *lpidGroup;
    msg.dwCurrentPlayers = 0; /* FIXME: Incorrect? */
    msg.lpData           = lpData;
    msg.dwDataSize       = dwDataSize;
    msg.dpnName          = *lpGroupName;
    msg.dpIdParent       = DPID_NOPARENT_GROUP;
    msg.dwFlags          = DPMSG_CREATEGROUP_DWFLAGS( dwFlags );

    /* FIXME: Correct to just use send effectively? */
    /* FIXME: Should size include data w/ message or just message "header" */
    /* FIXME: Check return code */
    IDirectPlayX_SendEx( &This->IDirectPlay4_iface, DPID_SERVERPLAYER, DPID_ALLPLAYERS, 0, &msg,
            sizeof( msg ), 0, 0, NULL, NULL );
  }

  LeaveCriticalSection( &This->lock );

  return DP_OK;
}

static HRESULT WINAPI IDirectPlay2AImpl_CreateGroup( IDirectPlay2A *iface, DPID *lpidGroup,
        DPNAME *name, void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_CreateGroup( &This->IDirectPlay4A_iface, lpidGroup, name, data, size,
            flags );
}

static HRESULT WINAPI IDirectPlay2Impl_CreateGroup( IDirectPlay2 *iface, DPID *lpidGroup,
        DPNAME *name, void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_CreateGroup( &This->IDirectPlay4_iface, lpidGroup, name, data, size,
            flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_CreateGroup( IDirectPlay3A *iface, DPID *group,
        DPNAME *name, void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_CreateGroup( &This->IDirectPlay4A_iface, group, name, data, size,
            flags );
}

static HRESULT WINAPI IDirectPlay3Impl_CreateGroup( IDirectPlay3 *iface, DPID *lpidGroup,
        DPNAME *name, void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_CreateGroup( &This->IDirectPlay4_iface, lpidGroup, name, data, size,
            flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_CreateGroup( IDirectPlay4A *iface, DPID *lpidGroup,
        DPNAME *lpGroupName, void *lpData, DWORD dwDataSize, DWORD dwFlags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );

    /* no provider yet: Windows says so before anything else (apidiff, fresh state) */
    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;
    /* no session to create it in: Windows refuses the call outright (apidiff) */
    if ( !This->dp2->bConnectionOpen )
        return DPERR_INVALIDPARAMS;

    *lpidGroup = DPID_UNKNOWN;

    return DP_IF_CreateGroup( This, NULL, lpidGroup, lpGroupName, lpData, dwDataSize, dwFlags,
            TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_CreateGroup( IDirectPlay4 *iface, DPID *lpidGroup,
        DPNAME *lpGroupName, void *lpData, DWORD dwDataSize, DWORD dwFlags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );

    /* no provider yet: Windows says so before anything else (apidiff, fresh state) */
    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;
    /* no session to create it in: Windows refuses the call outright (apidiff) */
    if ( !This->dp2->bConnectionOpen )
        return DPERR_INVALIDPARAMS;

    *lpidGroup = DPID_UNKNOWN;

    return DP_IF_CreateGroup( This, NULL, lpidGroup, lpGroupName, lpData, dwDataSize, dwFlags,
            FALSE );
}


static void
DP_SetGroupData( lpGroupData lpGData, DWORD dwFlags,
                 LPVOID lpData, DWORD dwDataSize )
{
  /* Clear out the data with this player */
  if( dwFlags & DPSET_LOCAL )
  {
    if ( lpGData->dwLocalDataSize != 0 )
    {
      free( lpGData->lpLocalData );
      lpGData->lpLocalData = NULL;
      lpGData->dwLocalDataSize = 0;
    }
  }
  else
  {
    if( lpGData->dwRemoteDataSize != 0 )
    {
      free( lpGData->lpRemoteData );
      lpGData->lpRemoteData = NULL;
      lpGData->dwRemoteDataSize = 0;
    }
  }

  /* Reallocate for new data */
  if( lpData != NULL )
  {
    if( dwFlags & DPSET_LOCAL )
    {
      lpGData->lpLocalData     = lpData;
      lpGData->dwLocalDataSize = dwDataSize;
    }
    else
    {
      lpGData->lpRemoteData = malloc( dwDataSize );
      CopyMemory( lpGData->lpRemoteData, lpData, dwDataSize );
      lpGData->dwRemoteDataSize = dwDataSize;
    }
  }

}

static HRESULT DP_CreateSPPlayer( IDirectPlayImpl *This, DPID dpid, DWORD flags, void *msgHeader )
{
  HRESULT hr;

  /* Let the SP know that we've created this player */
  if( This->dp2->spData.lpCB->CreatePlayer )
  {
    DPSP_CREATEPLAYERDATA data;

    data.idPlayer = dpid;
    data.dwFlags = flags;
    data.lpSPMessageHeader = msgHeader;
    data.lpISP = This->dp2->spData.lpISP;

    TRACE( "Calling SP CreatePlayer 0x%08lx: dwFlags: 0x%08lx lpMsgHdr: %p\n", dpid, flags,
           msgHeader );

    hr = (*This->dp2->spData.lpCB->CreatePlayer)( &data );
    if( FAILED( hr ) )
    {
      ERR( "Failed to create player with sp: %s\n", DPLAYX_HresultToString( hr ) );
      return hr;
    }
  }

  /* Now let the SP know that this player is a member of the system group */
  if( This->dp2->spData.lpCB->AddPlayerToGroup )
  {
    DPSP_ADDPLAYERTOGROUPDATA data;

    data.idPlayer = dpid;
    data.idGroup = DPID_SYSTEM_GROUP;
    data.lpISP = This->dp2->spData.lpISP;

    TRACE( "Calling SP AddPlayerToGroup (sys group)\n" );

    hr = (*This->dp2->spData.lpCB->AddPlayerToGroup)( &data );
    if( FAILED( hr ) )
    {
      ERR( "Failed to add player to sys group with sp: %s\n", DPLAYX_HresultToString( hr ) );
      if ( This->dp2->spData.lpCB->DeletePlayer )
      {
        DPSP_DELETEPLAYERDATA data;
        data.idPlayer = dpid;
        data.dwFlags = 0;
        data.lpISP = This->dp2->spData.lpISP;
        This->dp2->spData.lpCB->DeletePlayer( &data );
      }
      return hr;
    }
  }

  return DP_OK;
}

/* the flags of a provider DeletePlayer: the player's own, without "in group", and 0x10000 - a bit
 * no header we have names; Windows always sets it there (S3 tap, 2026-10-16: 0x10000, 0x10001,
 * 0x10003, 0x10008, 0x10009, 0x1000b for players created with 0, 1, 7, 8, 9, 0xb) */
#define DP_SPDELETE_BIT 0x00010000
DWORD DP_SPDeleteFlags( DWORD playerFlags )
{
    return DP_SPDELETE_BIT | (playerFlags & ~DPLAYI_PLAYER_PLAYERINGROUP);
}

static void DP_DeleteSPPlayer( IDirectPlayImpl *This, DPID dpid )
{
    if ( This->dp2->spData.lpCB->RemovePlayerFromGroup )
    {
        DPSP_REMOVEPLAYERFROMGROUPDATA data;
        data.idPlayer = dpid;
        data.idGroup = DPID_SYSTEM_GROUP;
        data.lpISP = This->dp2->spData.lpISP;
        This->dp2->spData.lpCB->RemovePlayerFromGroup( &data );
    }
    if ( This->dp2->spData.lpCB->DeletePlayer )
    {
        DPSP_DELETEPLAYERDATA data;
        data.idPlayer = dpid;
        data.dwFlags = 0;
        data.lpISP = This->dp2->spData.lpISP;
        This->dp2->spData.lpCB->DeletePlayer( &data );
    }
}

BOOL DP_PlayerExists( IDirectPlayImpl *This, DPID id )
{
  return DP_FindPlayer( This, id ) != NULL;
}

void DP_SetPlayerSystemId( IDirectPlayImpl *This, DPID id, DPID systemPlayerId )
{
  struct PlayerList *plist = DP_FindPlayer( This, id );
  if ( plist && systemPlayerId )
    plist->lpPData->systemPlayerId = systemPlayerId;
}

HRESULT DP_CreatePlayer( IDirectPlayImpl *This, void *msgHeader, DPID *lpid, DPNAME *lpName,
        void *data, DWORD dataSize, void *spData, DWORD spDataSize, DWORD dwFlags, HANDLE hEvent,
        struct PlayerData **playerData, BOOL bAnsi )
{
  lpPlayerData lpPData;
  lpPlayerList lpPList;
  HRESULT hr;

  TRACE( "(%p)->(%p,%p,%u)\n", This, lpid, lpName, bAnsi );

  /* Allocate the storage for the player and associate it with list element */
  lpPData = calloc( 1, sizeof( *lpPData ) );
  if( lpPData == NULL )
    return DPERR_OUTOFMEMORY;

  /* Set the desired player ID */
  lpPData->dpid = *lpid;

  lpPData->name = DP_DuplicateName( lpName, FALSE, bAnsi, msgHeader != NULL );
  if ( !lpPData->name )
  {
    free( lpPData );
    return DPERR_OUTOFMEMORY;
  }

  lpPData->nameA = DP_DuplicateName( lpName, TRUE, bAnsi, msgHeader != NULL );
  if ( !lpPData->nameA )
  {
    free( lpPData->name );
    free( lpPData );
    return DPERR_OUTOFMEMORY;
  }

  lpPData->dwFlags = dwFlags;
  /* which machine owns the player (the host lists it for joiners); remote players are updated
   * by the message that announced them */
  lpPData->systemPlayerId = ( dwFlags & DPLAYI_PLAYER_SYSPLAYER ) ? *lpid
                          : ( dwFlags & DPLAYI_PLAYER_PLAYERLOCAL ) ? This->dp2->systemPlayerId : 0;

  /* If we were given an event handle, duplicate it */
  if( hEvent != 0 )
  {
    if( !DuplicateHandle( GetCurrentProcess(), hEvent,
                          GetCurrentProcess(), &lpPData->hEvent,
                          0, FALSE, DUPLICATE_SAME_ACCESS )
      )
    {
      /* FIXME: Memory leak */
      ERR( "Can't duplicate player msg handle %p\n", hEvent );
    }
  }

  /* Initialize the SP data section */
  lpPData->lpSPPlayerData = DPSP_CreateSPPlayerData();

  /* Create the list object and link it in */
  lpPList = calloc( 1, sizeof( *lpPList ) );
  if( !lpPList )
  {
    free( lpPData->lpSPPlayerData );
    CloseHandle( lpPData->hEvent );
    free( lpPData->nameA );
    free( lpPData->name );
    free( lpPData );
    return DPERR_OUTOFMEMORY;
  }

  lpPData->uRef = 1;
  lpPList->lpPData = lpPData;

  /* Add the player to the system group */
  DPQ_INSERT( This->dp2->lpSysGroup->players, lpPList, players );

  DP_SetPlayerData( lpPData, DPSET_REMOTE, data, dataSize );

  hr = IDirectPlaySP_SetSPPlayerData( This->dp2->spData.lpISP, *lpid, spData, spDataSize,
                                      DPSET_REMOTE );
  if ( FAILED( hr ) )
  {
    free( lpPData->lpRemoteData );
    DPQ_REMOVE( This->dp2->lpSysGroup->players, lpPList, players );
    free( lpPList );
    free( lpPData->lpSPPlayerData );
    CloseHandle( lpPData->hEvent );
    free( lpPData->nameA );
    free( lpPData->name );
    free( lpPData );
    return hr;
  }

  hr = DP_CreateSPPlayer( This, *lpid, dwFlags, msgHeader );
  if ( FAILED( hr ) )
  {
    free( lpPData->lpRemoteData );
    DPQ_REMOVE( This->dp2->lpSysGroup->players, lpPList, players );
    free( lpPList );
    free( lpPData->lpSPPlayerData );
    CloseHandle( lpPData->hEvent );
    free( lpPData->nameA );
    free( lpPData->name );
    free( lpPData );
    return hr;
  }

  if( ~dwFlags & DPLAYI_PLAYER_SYSPLAYER )
  {
    DPMSG_CREATEPLAYERORGROUP msg;
    DWORD currentPlayers;

    currentPlayers = This->dp2->lpSessionDesc->dwCurrentPlayers;
    if ( ~dwFlags & DPLAYI_PLAYER_PLAYERLOCAL )
        ++currentPlayers;

    msg.dwType = DPSYS_CREATEPLAYERORGROUP;
    msg.dwPlayerType = DPPLAYERTYPE_PLAYER;
    msg.dpId = *lpid;
    msg.dwCurrentPlayers = currentPlayers;
    msg.lpData = data;
    msg.dwDataSize = dataSize;
    msg.dpnName = *lpPData->name;
    msg.dpIdParent = 0;
    msg.dwFlags = dwFlags;

    hr = DP_QueueMessage( This, DPID_SYSMSG, DPID_ALLPLAYERS, *lpid, &msg,
                          DP_CopyCreatePlayerOrGroup, 0 );
    if ( FAILED( hr ) )
    {
        DP_DeleteSPPlayer( This, *lpid );
        free( lpPData->lpLocalData );
        free( lpPData->lpRemoteData );
        DPQ_REMOVE( This->dp2->lpSysGroup->players, lpPList, players );
        free( lpPList );
        CloseHandle( lpPData->hEvent );
        free( lpPData->nameA );
        free( lpPData->name );
        free( lpPData );
        return hr;
    }

    This->dp2->lpSessionDesc->dwCurrentPlayers++;
  }

  TRACE( "Created player id 0x%08lx\n", *lpid );

  if( playerData )
    *playerData = lpPData;

  return DP_OK;
}

/* This method assumes that all links to it are already deleted */
static void DP_DeletePlayer( IDirectPlayImpl *This, DPID dpid )
{
  lpPlayerList lpPList;

  TRACE( "(%p)->(0x%08lx)\n", This, dpid );

  DPQ_REMOVE_ENTRY( This->dp2->lpSysGroup->players, players, lpPData->dpid, ==, dpid, lpPList );

  if( lpPList == NULL )
  {
    ERR( "DPID 0x%08lx not found\n", dpid );
    return;
  }

  /* Verify that this is the last reference to the data */
  if( --(lpPList->lpPData->uRef) )
  {
    ERR( "player 0x%08lx still has %lu references - removing it anyway\n", dpid, lpPList->lpPData->uRef );
  }

  /* Delete player */
  free( lpPList->lpPData->nameA );
  free( lpPList->lpPData->name );

  CloseHandle( lpPList->lpPData->hEvent );
  free( lpPList->lpPData );

  /* Delete Player List object */
  free( lpPList );
}

static lpPlayerList DP_FindPlayer( IDirectPlayImpl *This, DPID dpid )
{
  lpPlayerList lpPlayers;

  TRACE( "(%p)->(0x%08lx)\n", This, dpid );

  if(This->dp2->lpSysGroup == NULL)
    return NULL;

  DPQ_FIND_ENTRY( This->dp2->lpSysGroup->players, players, lpPData->dpid, ==, dpid, lpPlayers );

  return lpPlayers;
}

static DWORD DP_CopyName( DPNAME *dst, const DPNAME *src, BOOL dstAnsi, BOOL srcAnsi, BOOL fill )
{
    static const WCHAR emptyW[] = { 0 };
    static const char emptyA[] = "";
    const void *empty = fill ? (srcAnsi ? (const void *) emptyA : (const void *) emptyW) : NULL;
    const void *shortName = src && src->lpszShortNameA ? (const void *) src->lpszShortNameA : empty;
    const void *longName = src && src->lpszLongNameA ? (const void *) src->lpszLongNameA : empty;
    DWORD offset = sizeof( DPNAME );

    /* fill: a missing name becomes "" - for names from other machines. AoE2 copies both names
     * with no NULL check, and a joiner without a long name crashed a real AoE2 host
     * (age2_x1.5.exe+0x1db37f); the wire carries no empty names (host.c), so a remote player's
     * missing name arrives as none. A name set here stays as given, NULL included, as Windows
     * keeps it (apistate_dp1.c: GetPlayerName sizes 0, a 28-byte name message). */
    if ( dst )
    {
        if ( src )
            *dst = *src;
        else
            memset( dst, 0, sizeof( DPNAME ) );
        dst->dwSize = sizeof( DPNAME );
    }

    offset += DP_CopyString( &dst->lpszShortNameA, shortName, dstAnsi, srcAnsi, dst, offset );
    offset += DP_CopyString( &dst->lpszLongNameA, longName, dstAnsi, srcAnsi, dst, offset );

    return offset;
}

static DPNAME *DP_DuplicateName( const DPNAME *src, BOOL dstAnsi, BOOL srcAnsi, BOOL fill )
{
    DPNAME *dst;
    DWORD size;

    size = DP_CopyName( NULL, src, dstAnsi, srcAnsi, fill );

    dst = malloc( size );
    if ( !dst )
        return NULL;

    DP_CopyName( dst, src, dstAnsi, srcAnsi, fill );

    return dst;
}

static void
DP_SetPlayerData( lpPlayerData lpPData, DWORD dwFlags,
                  LPVOID lpData, DWORD dwDataSize )
{
  /* Clear out the data with this player */
  if( dwFlags & DPSET_LOCAL )
  {
    if ( lpPData->dwLocalDataSize != 0 )
    {
      free( lpPData->lpLocalData );
      lpPData->lpLocalData = NULL;
      lpPData->dwLocalDataSize = 0;
    }
  }
  else
  {
    if( lpPData->dwRemoteDataSize != 0 )
    {
      free( lpPData->lpRemoteData );
      lpPData->lpRemoteData = NULL;
      lpPData->dwRemoteDataSize = 0;
    }
  }

  /* Reallocate for new data */
  if( lpData != NULL )
  {

    if( dwFlags & DPSET_LOCAL )
    {
      lpPData->lpLocalData     = lpData;
      lpPData->dwLocalDataSize = dwDataSize;
    }
    else
    {
      lpPData->lpRemoteData = malloc( dwDataSize );
      CopyMemory( lpPData->lpRemoteData, lpData, dwDataSize );
      lpPData->dwRemoteDataSize = dwDataSize;
    }
  }

}

static HRESULT DP_IF_CreatePlayer( IDirectPlayImpl *This, DPID *lpidPlayer,
        DPNAME *lpPlayerName, HANDLE hEvent, void *lpData, DWORD dwDataSize, DWORD dwFlags,
        BOOL bAnsi )
{
  struct PlayerData *player;
  HRESULT hr = DP_OK;
  DWORD dwCreateFlags;

  TRACE( "(%p)->(%p,%p,%p,%p,0x%08lx,0x%08lx,%u)\n",
         This, lpidPlayer, lpPlayerName, hEvent, lpData,
         dwDataSize, dwFlags, bAnsi );
  if( This->dp2->connectionInitialized == NO_PROVIDER )
  {
    return DPERR_UNINITIALIZED;
  }

  EnterCriticalSection( &This->lock );
  if( !This->dp2->bConnectionOpen )
  {
    LeaveCriticalSection( &This->lock );
    return DPERR_INVALIDPARAM;
  }
  LeaveCriticalSection( &This->lock );

  if( lpidPlayer == NULL )
  {
    return DPERR_INVALIDPARAMS;
  }


  /* Determine the creation flags for the player. These will be passed
   * to the name server if requesting a player id and to the SP when
   * informing it of the player creation
   */
  dwCreateFlags = dwFlags | DPLAYI_PLAYER_PLAYERLOCAL;

  /* Verify we know how to handle all the flags */
  if( dwFlags & ~( DPPLAYER_SERVERPLAYER | DPPLAYER_SPECTATOR ) )
  {
    /* Assume non fatal failure */
    ERR( "unknown dwFlags = 0x%08lx\n", dwFlags );
  }

  /* If the name is not specified, we must provide one */
  if( !(dwFlags & DPPLAYER_SERVERPLAYER) )
  {
    /* If we are the session master, we dish out the group/player ids */
    if( This->dp2->bHostInterface )
    {
      *lpidPlayer = DP_NextObjectId();
    }
    else
    {
      hr = DP_MSG_SendRequestPlayerId( This, dwCreateFlags, lpidPlayer );

      if( FAILED(hr) )
      {
        ERR( "Request for ID failed: %s\n", DPLAYX_HresultToString( hr ) );
        return hr;
      }
    }
  }
  else
  {
    /* FIXME: Would be nice to perhaps verify that we don't already have
     *        this player.
     */
    *lpidPlayer = DPID_SERVERPLAYER;
  }

  EnterCriticalSection( &This->lock );

  /* We pass creation flags, so we can distinguish sysplayers and not count them in the current
     player total */
  hr = DP_CreatePlayer( This, NULL, lpidPlayer, lpPlayerName, lpData, dwDataSize, NULL, 0,
                        dwCreateFlags, hEvent, &player, bAnsi );
  if( FAILED( hr ) )
  {
    LeaveCriticalSection( &This->lock );
    return hr;
  }

  hr = DP_MSG_SendCreatePlayer( This, DPID_ALLPLAYERS, *lpidPlayer, dwCreateFlags, player->name,
                                lpData, dwDataSize, This->dp2->systemPlayerId );
  if( FAILED( hr ) )
  {
    LeaveCriticalSection( &This->lock );
    return hr;
  }

  LeaveCriticalSection( &This->lock );

  return hr;
}

static HRESULT WINAPI IDirectPlay2AImpl_CreatePlayer( IDirectPlay2A *iface, DPID *lpidPlayer,
        DPNAME *name, HANDLE event, void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_CreatePlayer( &This->IDirectPlay4A_iface, lpidPlayer, name, event, data,
            size, flags );
}

static HRESULT WINAPI IDirectPlay2Impl_CreatePlayer( IDirectPlay2 *iface, DPID *lpidPlayer,
        DPNAME *name, HANDLE event, void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_CreatePlayer( &This->IDirectPlay4_iface, lpidPlayer, name, event, data,
            size, flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_CreatePlayer( IDirectPlay3A *iface, DPID *lpidPlayer,
        DPNAME *name, HANDLE event, void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_CreatePlayer( &This->IDirectPlay4A_iface, lpidPlayer, name, event, data,
            size, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_CreatePlayer( IDirectPlay3 *iface, DPID *lpidPlayer,
        DPNAME *name, HANDLE event, void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_CreatePlayer( &This->IDirectPlay4_iface, lpidPlayer, name, event, data,
            size, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_CreatePlayer( IDirectPlay4A *iface, DPID *lpidPlayer,
        DPNAME *lpPlayerName, HANDLE hEvent, void *lpData, DWORD dwDataSize, DWORD dwFlags )
{
  IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
  return DP_IF_CreatePlayer( This, lpidPlayer, lpPlayerName, hEvent,
                           lpData, dwDataSize, dwFlags, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_CreatePlayer( IDirectPlay4 *iface, DPID *lpidPlayer,
        DPNAME *lpPlayerName, HANDLE hEvent, void *lpData, DWORD dwDataSize, DWORD dwFlags )
{
  IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
  return DP_IF_CreatePlayer( This, lpidPlayer, lpPlayerName, hEvent,
                           lpData, dwDataSize, dwFlags, FALSE );
}

static DPID DP_GetRemoteNextObjectId(void)
{
  FIXME( ":stub\n" );

  /* Hack solution */
  return DP_NextObjectId();
}

static HRESULT WINAPI IDirectPlay2AImpl_DeletePlayerFromGroup( IDirectPlay2A *iface, DPID group,
        DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_DeletePlayerFromGroup( &This->IDirectPlay4A_iface, group, player );
}

static HRESULT WINAPI IDirectPlay2Impl_DeletePlayerFromGroup( IDirectPlay2 *iface, DPID group,
        DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_DeletePlayerFromGroup( &This->IDirectPlay4_iface, group, player );
}

static HRESULT WINAPI IDirectPlay3AImpl_DeletePlayerFromGroup( IDirectPlay3A *iface, DPID group,
        DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_DeletePlayerFromGroup( &This->IDirectPlay4A_iface, group, player );
}

static HRESULT WINAPI IDirectPlay3Impl_DeletePlayerFromGroup( IDirectPlay3 *iface, DPID group,
        DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_DeletePlayerFromGroup( &This->IDirectPlay4_iface, group, player );
}

static HRESULT WINAPI IDirectPlay4AImpl_DeletePlayerFromGroup( IDirectPlay4A *iface, DPID group,
        DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_DeletePlayerFromGroup( &This->IDirectPlay4_iface, group, player );
}

static HRESULT WINAPI IDirectPlay4Impl_DeletePlayerFromGroup( IDirectPlay4 *iface, DPID group,
        DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    HRESULT hr = DP_OK;

    lpGroupData  gdata;
    lpPlayerList plist;

    TRACE( "(%p)->(0x%08lx,0x%08lx)\n", This, group, player );

    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;

    EnterCriticalSection( &This->lock );

    /* Find the group */
    if ( ( gdata = DP_FindGameGroup( This, group ) ) == NULL )
    {
        LeaveCriticalSection( &This->lock );
        return DPERR_INVALIDGROUP;
    }

    /* Find the player */
    if ( DP_FindPlayer( This, player ) == NULL )
    {
        LeaveCriticalSection( &This->lock );
        return DPERR_INVALIDPLAYER;
    }

    /* Remove the player shortcut from the group */
    DPQ_REMOVE_ENTRY( gdata->players, players, lpPData->dpid, ==, player, plist );

    if ( !plist )
    {
        LeaveCriticalSection( &This->lock );
        return DPERR_INVALIDPLAYER;
    }

    /* One less reference */
    plist->lpPData->uRef--;

    /* Delete the Player List element */
    free( plist );

    /* Inform the SP if they care */
    if ( This->dp2->spData.lpCB->RemovePlayerFromGroup )
    {
        DPSP_REMOVEPLAYERFROMGROUPDATA data;

        TRACE( "Calling SP RemovePlayerFromGroup\n" );
        data.idPlayer = player;
        data.idGroup = group;
        data.lpISP = This->dp2->spData.lpISP;
        hr = (*This->dp2->spData.lpCB->RemovePlayerFromGroup)( &data );
    }

    /* every player on this machine hears of it, the one taken out too (Windows, apistate_dp1.c) */
    if ( SUCCEEDED( hr ) )
    {
        DPMSG_DELETEPLAYERFROMGROUP msg;
        msg.dwType = DPSYS_DELETEPLAYERFROMGROUP;
        msg.dpIdGroup = group;
        msg.dpIdPlayer = player;
        DP_QueueMessage( This, DPID_SYSMSG, DPID_ALLPLAYERS, 0, &msg, DP_CopyGeneric, sizeof( msg ) );
        DP_GRP_SendIds( This, DPMSGCMD_DELETEPLAYERFROMGROUP, player, group );   /* every machine (Windows) */
    }

    LeaveCriticalSection( &This->lock );

    return hr;
}

static DWORD DP_CopyDestroyPlayerOrGroup( DPMSG_GENERIC *genericDst, DPMSG_GENERIC *genericSrc,
                                          DWORD genericSize, BOOL ansi );
static DWORD DP_CopySetPlayerOrGroupName( DPMSG_GENERIC *genericDst, DPMSG_GENERIC *genericSrc,
                                          DWORD genericSize, BOOL ansi );

/* take out every shortcut to g (AddGroupToGroup links in other groups), walking the groups through
 * their children only; each one as DeleteGroupFromGroup announces it */
static void DP_UnlinkShortcutsTo( IDirectPlayImpl *This, lpGroupData where, lpGroupData g, int depth )
{
  lpGroupList link, next;

  if( !where || depth > 64 )
    return;
  for( link = DPQ_FIRST( where->groups ); link; link = next )
  {
    next = DPQ_NEXT( link->groups );
    if( DP_IS_CHILD_LINK( where, link ) )
      DP_UnlinkShortcutsTo( This, link->lpGData, g, depth + 1 );
    else if( link->lpGData == g )
    {
      DPMSG_DELETEGROUPFROMGROUP msg;
      DPQ_REMOVE( where->groups, link, groups );
      g->uRef--;
      free( link );
      msg.dwType = DPSYS_DELETEGROUPFROMGROUP;
      msg.dpIdParentGroup = where->dpid;
      msg.dpIdGroup = g->dpid;
      DP_QueueMessage( This, DPID_SYSMSG, DPID_ALLPLAYERS, 0, &msg, DP_CopyGeneric, sizeof( msg ) );
    }
  }
}

/* DestroyGroup, in Windows' order (black box, 2026-10-22: a group with players, shortcuts in and
 * out and three children, one with a child of its own):
 *   1. the shortcuts other groups hold to it are taken out (DPSYS_DELETEGROUPFROMGROUP each)
 *   2. its players leave it, the last one added first (DPSYS_DELETEPLAYERFROMGROUP each)
 *   3. the shortcuts it holds are taken out, the last one added first
 *   4. its children are destroyed the same way, the newest first
 *   5. DPSYS_DESTROYPLAYERORGROUP for the group itself, with its parent and flags
 * then it is gone from its parent (or the system group) and from the provider. */
static void DP_DestroyGroupTree( IDirectPlayImpl *This, lpGroupData g, int depth )
{
  lpGroupList link;
  lpPlayerList plist;
  lpGroupData container;
  DPID id = g->dpid, parent = g->parent, *ids = NULL;
  lpGroupData *children = NULL;
  int n = 0, i;

  if( depth > 64 )
    return;

  DP_UnlinkShortcutsTo( This, This->dp2->lpSysGroup, g, 0 );

  for( plist = DPQ_FIRST( g->players ); plist; plist = DPQ_NEXT( plist->players ) ) n++;
  if( n && (ids = malloc( n * sizeof(*ids) )) )
  {
    i = 0;
    for( plist = DPQ_FIRST( g->players ); plist; plist = DPQ_NEXT( plist->players ) )
      ids[ i++ ] = plist->lpPData->dpid;
    for( i = n - 1; i >= 0; i-- )
      IDirectPlayX_DeletePlayerFromGroup( &This->IDirectPlay4_iface, id, ids[ i ] );
  }
  free( ids );
  ids = NULL;

  n = 0;
  for( link = DPQ_FIRST( g->groups ); link; link = DPQ_NEXT( link->groups ) ) n++;
  if( n && (ids = malloc( n * sizeof(*ids) )) && (children = malloc( n * sizeof(*children) )) )
  {
    int ns = 0, nc = 0;
    for( link = DPQ_FIRST( g->groups ); link; link = DPQ_NEXT( link->groups ) )
    {
      if( DP_IS_CHILD_LINK( g, link ) )
        children[ nc++ ] = link->lpGData;
      else
        ids[ ns++ ] = link->lpGData->dpid;
    }
    for( i = ns - 1; i >= 0; i-- )
      IDirectPlayX_DeleteGroupFromGroup( &This->IDirectPlay4_iface, id, ids[ i ] );
    for( i = nc - 1; i >= 0; i-- )
      DP_DestroyGroupTree( This, children[ i ], depth + 1 );
  }
  free( ids );
  free( children );

  {
    DPMSG_DESTROYPLAYERORGROUP msg;
    memset( &msg, 0, sizeof(msg) );
    msg.dwType       = DPSYS_DESTROYPLAYERORGROUP;
    msg.dwPlayerType = DPPLAYERTYPE_GROUP;
    msg.dpId         = id;
    if( g->name ) msg.dpnName = *g->name;
    msg.dpIdParent   = parent;
    msg.dwFlags      = DP_GROUP_FLAGS( g );
    DP_QueueMessage( This, DPID_SYSMSG, DPID_ALLPLAYERS, 0, &msg, DP_CopyDestroyPlayerOrGroup, 0 );
  }
  DP_GRP_SendIds( This, DPMSGCMD_DELETEGROUP, 0, id );   /* after DELETEPLAYERFROMGROUP for its players (Windows) */

  /* out of its parent's list (a top-level group's parent is the system group) */
  container = parent == DPID_NOPARENT_GROUP ? This->dp2->lpSysGroup : DP_FindAnyGroup( This, parent );
  if( container )
  {
    DPQ_REMOVE_ENTRY( container->groups, groups, lpGData->dpid, ==, id, link );
    if( link )
      free( link );
    else
      ERR( "group 0x%08lx not in its parent 0x%08lx\n", id, parent );
  }

  /* what's left of the group itself: anything still in it was taken out above */
  while( (plist = DPQ_FIRST( g->players )) )
  {
    DPQ_REMOVE( g->players, plist, players );
    plist->lpPData->uRef--;
    free( plist );
  }
  while( (link = DPQ_FIRST( g->groups )) )
  {
    DPQ_REMOVE( g->groups, link, groups );
    link->lpGData->uRef--;
    free( link );
  }
  free( g->lpSPPlayerData );
  free( g->nameA );
  free( g->name );
  free( g );

  if( This->dp2->spData.lpCB->DeleteGroup )
  {
    DPSP_DELETEGROUPDATA data;
    data.idGroup = id;
    data.dwFlags = 0;
    data.lpISP   = This->dp2->spData.lpISP;
    (*This->dp2->spData.lpCB->DeleteGroup)( &data );
  }
}

static HRESULT DP_IF_DestroyGroup( IDirectPlayImpl *This, void *lpMsgHdr, DPID idGroup, BOOL bAnsi )
{
  lpGroupData lpGData;

  TRACE( "(%p)->(%p,0x%08lx,%u)\n", This, lpMsgHdr, idGroup, bAnsi );

  /* no provider yet: Windows says so before looking at the id (apidiff, fresh state) */
  if( This->dp2->connectionInitialized == NO_PROVIDER )
    return DPERR_UNINITIALIZED;

  EnterCriticalSection( &This->lock );

  /* Find the group */
  if( ( lpGData = DP_FindAnyGroup( This, idGroup ) ) == NULL )
  {
    LeaveCriticalSection( &This->lock );
    return DPERR_INVALIDGROUP;   /* Windows: a group id nobody has is an invalid group */
  }

  if( idGroup == DPID_SYSTEM_GROUP )
  {
    LeaveCriticalSection( &This->lock );
    return DPERR_INVALIDGROUP;   /* [UNV] Windows' answer; the system group is never destroyed */
  }

  DP_DestroyGroupTree( This, lpGData, 0 );

  LeaveCriticalSection( &This->lock );

  return DP_OK;
}

static HRESULT WINAPI IDirectPlay2AImpl_DestroyGroup( IDirectPlay2A *iface, DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_DestroyGroup( &This->IDirectPlay4A_iface, group );
}

static HRESULT WINAPI IDirectPlay2Impl_DestroyGroup( IDirectPlay2 *iface, DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_DestroyGroup( &This->IDirectPlay4_iface, group );
}

static HRESULT WINAPI IDirectPlay3AImpl_DestroyGroup( IDirectPlay3A *iface, DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_DestroyGroup( &This->IDirectPlay4A_iface, group );
}

static HRESULT WINAPI IDirectPlay3Impl_DestroyGroup( IDirectPlay3 *iface, DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_DestroyGroup( &This->IDirectPlay4_iface, group );
}

static HRESULT WINAPI IDirectPlay4AImpl_DestroyGroup( IDirectPlay4A *iface, DPID idGroup )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return DP_IF_DestroyGroup( This, NULL, idGroup, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_DestroyGroup( IDirectPlay4 *iface, DPID idGroup )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    return DP_IF_DestroyGroup( This, NULL, idGroup, FALSE );
}

typedef struct _DPFAGContext
{
  IDirectPlayImpl   *This;
  DPID              idPlayer;
  BOOL              bAnsi;
} DPFAGContext, *lpDPFAGContext;

static HRESULT DP_IF_DestroyPlayer( IDirectPlayImpl *This, void *lpMsgHdr, DPID idPlayer,
        BOOL bAnsi )
{
  DPFAGContext cbContext;

  FIXME( "(%p)->(%p,0x%08lx,%u): semi stub\n",
         This, lpMsgHdr, idPlayer, bAnsi );

  if( This->dp2->connectionInitialized == NO_PROVIDER )
  {
    return DPERR_UNINITIALIZED;
  }

  EnterCriticalSection( &This->lock );

  {
    struct PlayerList *plist = DP_FindPlayer( This, idPlayer );
    if( plist == NULL )
    {
      LeaveCriticalSection( &This->lock );
      return DPERR_INVALIDPLAYER;
    }
    /* the game ended its own player's part: resign, quit or back to the menu (which of these is
       inside the game's own messages - DirectPlay can't tell) */
    if( plist->lpPData->dwFlags & DPLAYI_PLAYER_PLAYERLOCAL )
      DP_NOTE( "this PC's player 0x%08lx left the game (the game removed it: resigned, quit or "
               "went back to the menu)\n", idPlayer );
    else
      DP_NOTE( "the game removed player 0x%08lx (on another PC)\n", idPlayer );
  }

  /* FIXME: If the player is remote, we must be the host to delete this */

  DP_RemovePlayer( This, idPlayer, TRUE );
  LeaveCriticalSection( &This->lock );
  return DP_OK;

  cbContext.This     = This;
  cbContext.idPlayer = idPlayer;
  cbContext.bAnsi    = bAnsi;

  /* Find each group and call DeletePlayerFromGroup if the player is a
     member of the group */
  IDirectPlayX_EnumGroups( &This->IDirectPlay4_iface, NULL, cbDeletePlayerFromAllGroups, &cbContext,
          DPENUMGROUPS_ALL );

  /* Now delete player and player list from the sys group */
  DP_DeletePlayer( This, idPlayer );

  /* Let the SP know that we've destroyed this group */
  if( This->dp2->spData.lpCB->DeletePlayer )
  {
    DPSP_DELETEPLAYERDATA data;

    FIXME( "data.dwFlags is incorrect\n" );

    data.idPlayer = idPlayer;
    data.dwFlags = 0;
    data.lpISP   = This->dp2->spData.lpISP;

    (*This->dp2->spData.lpCB->DeletePlayer)( &data );
  }

  FIXME( "Send a DELETEPLAYERORGROUP msg\n" );

  LeaveCriticalSection( &This->lock );

  return DP_OK;
}

static BOOL CALLBACK
cbDeletePlayerFromAllGroups(
    DPID            dpId,
    DWORD           dwPlayerType,
    LPCDPNAME       lpName,
    DWORD           dwFlags,
    LPVOID          lpContext )
{
  lpDPFAGContext lpCtxt = (lpDPFAGContext)lpContext;

  if( dwPlayerType == DPPLAYERTYPE_GROUP )
  {
    IDirectPlayX_DeletePlayerFromGroup( &lpCtxt->This->IDirectPlay4_iface, dpId, lpCtxt->idPlayer );

    /* Enumerate all groups in this group since this will normally only
     * be called for top level groups
     */
    IDirectPlayX_EnumGroupsInGroup( &lpCtxt->This->IDirectPlay4_iface, dpId, NULL,
            cbDeletePlayerFromAllGroups, lpContext, DPENUMGROUPS_ALL);

  }
  else
  {
    ERR( "Group callback has dwPlayerType = 0x%08lx\n", dwPlayerType );
  }

  return TRUE;
}

static HRESULT WINAPI IDirectPlay2AImpl_DestroyPlayer( IDirectPlay2A *iface, DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_DestroyPlayer( &This->IDirectPlay4A_iface, player );
}

static HRESULT WINAPI IDirectPlay2Impl_DestroyPlayer( IDirectPlay2 *iface, DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_DestroyPlayer( &This->IDirectPlay4_iface, player );
}

static HRESULT WINAPI IDirectPlay3AImpl_DestroyPlayer( IDirectPlay3A *iface, DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_DestroyPlayer( &This->IDirectPlay4A_iface, player );
}

static HRESULT WINAPI IDirectPlay3Impl_DestroyPlayer( IDirectPlay3 *iface, DPID player )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_DestroyPlayer( &This->IDirectPlay4_iface, player );
}

static HRESULT WINAPI IDirectPlay4AImpl_DestroyPlayer( IDirectPlay4A *iface, DPID idPlayer )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return DP_IF_DestroyPlayer( This, NULL, idPlayer, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_DestroyPlayer( IDirectPlay4 *iface, DPID idPlayer )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    return DP_IF_DestroyPlayer( This, NULL, idPlayer, FALSE );
}

static HRESULT WINAPI IDirectPlay2AImpl_EnumGroupPlayers( IDirectPlay2A *iface, DPID group,
        GUID *instance, LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_EnumGroupPlayers( &This->IDirectPlay4A_iface, group, instance,
            enumplayercb, context, flags );
}

static HRESULT WINAPI IDirectPlay2Impl_EnumGroupPlayers( IDirectPlay2 *iface, DPID group,
        GUID *instance, LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_EnumGroupPlayers( &This->IDirectPlay4_iface, group, instance,
            enumplayercb, context, flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_EnumGroupPlayers( IDirectPlay3A *iface, DPID group,
        GUID *instance, LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_EnumGroupPlayers( &This->IDirectPlay4A_iface, group, instance,
            enumplayercb, context, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_EnumGroupPlayers( IDirectPlay3 *iface, DPID group,
        GUID *instance, LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_EnumGroupPlayers( &This->IDirectPlay4_iface, group, instance,
            enumplayercb, context, flags );
}

static HRESULT DP_IF_EnumGroupPlayers( IDirectPlayImpl *This, DPID group, GUID *instance,
        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags, BOOL ansi )
{
    lpGroupData  gdata;
    lpPlayerList plist;

    FIXME( "(%p)->(0x%08lx,%p,%p,%p,0x%08lx): semi stub\n", This, group, instance, enumplayercb,
           context, flags );

    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;

    EnterCriticalSection( &This->lock );

    /* Find the group */
    if ( ( gdata = DP_FindAnyGroup( This, group ) ) == NULL )
    {
        LeaveCriticalSection( &This->lock );
        return DPERR_INVALIDGROUP;
    }

    /* Walk the players in this group (the system group's newest first, as Windows does) */
    {
    struct PlayerData **players;
    int n, i;

    if ( !(players = DP_GroupPlayersInOrder( This, gdata, &n )) )
    {
        LeaveCriticalSection( &This->lock );
        return DPERR_OUTOFMEMORY;
    }
    for( i = 0; i < n; i++ )
    {
        struct PlayerData *p = players[ i ];
        DWORD playerFlags;

        /* We do not enum the name server or app server as they are of no
         * consequence to the end user.
         */
        if ( p->dwFlags & DPLAYI_PLAYER_SYSPLAYER )
            continue;

        if ( (p->dwFlags & flags) != (flags & ~DPENUMPLAYERS_REMOTE) )
            continue;
        if ( (p->dwFlags & DPENUMPLAYERS_LOCAL) && (flags & DPENUMPLAYERS_REMOTE) )
            continue;

        playerFlags = p->dwFlags;
        playerFlags &= ~(DPENUMPLAYERS_GROUP | DPENUMPLAYERS_LOCAL | DPENUMPLAYERS_OWNER);
        playerFlags |= flags;

        if ( !enumplayercb( p->dpid, DPPLAYERTYPE_PLAYER, ansi ? p->nameA : p->name,
                    playerFlags, context ) )
            /* User requested break */
            break;
    }
    free( players );
    }

    LeaveCriticalSection( &This->lock );

    return DP_OK;
}

/* (EnumPlayers is the system group's list; a game asking for group 0 gets INVALIDGROUP, as from
 * Windows - black box, 2026-10-22) */
static HRESULT WINAPI IDirectPlay4AImpl_EnumGroupPlayers( IDirectPlay4A *iface, DPID group,
        GUID *instance, LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    if ( group == DPID_SYSTEM_GROUP && This->dp2->connectionInitialized != NO_PROVIDER )
        return DPERR_INVALIDGROUP;
    return DP_IF_EnumGroupPlayers( This, group, instance, enumplayercb, context, flags, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_EnumGroupPlayers( IDirectPlay4 *iface, DPID group,
        GUID *instance, LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    if ( group == DPID_SYSTEM_GROUP && This->dp2->connectionInitialized != NO_PROVIDER )
        return DPERR_INVALIDGROUP;
    return DP_IF_EnumGroupPlayers( This, group, instance, enumplayercb, context, flags, FALSE );
}

/* NOTE: This only enumerates top level groups (created with CreateGroup) */
static HRESULT WINAPI IDirectPlay2AImpl_EnumGroups( IDirectPlay2A *iface, GUID *instance,
        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_EnumGroups( &This->IDirectPlay4A_iface, instance, enumplayercb, context,
            flags );
}

static HRESULT WINAPI IDirectPlay2Impl_EnumGroups( IDirectPlay2 *iface, GUID *instance,
        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_EnumGroups( &This->IDirectPlay4_iface, instance, enumplayercb, context,
            flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_EnumGroups( IDirectPlay3A *iface, GUID *instance,
        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_EnumGroups( &This->IDirectPlay4A_iface, instance, enumplayercb, context,
            flags );
}

static HRESULT WINAPI IDirectPlay3Impl_EnumGroups( IDirectPlay3 *iface, GUID *instance,
        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_EnumGroups( &This->IDirectPlay4_iface, instance, enumplayercb, context,
            flags );
}

static HRESULT DP_IF_EnumGroupsInGroup( IDirectPlayImpl *This, DPID group, GUID *instance,
        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags, BOOL ansi );

static HRESULT WINAPI IDirectPlay4AImpl_EnumGroups( IDirectPlay4A *iface, GUID *instance,
        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    if ( This->dp2->connectionInitialized != NO_PROVIDER && !This->dp2->bConnectionOpen &&
         !(flags & DPENUMGROUPS_SESSION) )
        return DPERR_NOSESSIONS;
    return DP_IF_EnumGroupsInGroup( This, DPID_SYSTEM_GROUP, instance, enumplayercb,
            context, flags, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_EnumGroups ( IDirectPlay4 *iface, GUID *instance,
        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    if ( This->dp2->connectionInitialized != NO_PROVIDER && !This->dp2->bConnectionOpen &&
         !(flags & DPENUMGROUPS_SESSION) )
        return DPERR_NOSESSIONS;
    return DP_IF_EnumGroupsInGroup( This, DPID_SYSTEM_GROUP, instance, enumplayercb,
            context, flags, FALSE );
}

static HRESULT WINAPI IDirectPlay2AImpl_EnumPlayers( IDirectPlay2A *iface, GUID *instance,
        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_EnumPlayers( &This->IDirectPlay4A_iface, instance, enumplayercb, context,
            flags );
}

static HRESULT WINAPI IDirectPlay2Impl_EnumPlayers( IDirectPlay2 *iface, GUID *instance,
        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_EnumPlayers( &This->IDirectPlay4_iface, instance, enumplayercb, context,
            flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_EnumPlayers( IDirectPlay3A *iface, GUID *instance,
        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_EnumPlayers( &This->IDirectPlay4A_iface, instance, enumplayercb, context,
            flags );
}

static HRESULT WINAPI IDirectPlay3Impl_EnumPlayers( IDirectPlay3 *iface, GUID *instance,
        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_EnumPlayers( &This->IDirectPlay4_iface, instance, enumplayercb, context,
            flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_EnumPlayers( IDirectPlay4A *iface, GUID *instance,
        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    if ( This->dp2->connectionInitialized != NO_PROVIDER && !This->dp2->bConnectionOpen &&
         !(flags & DPENUMPLAYERS_SESSION) )
        return DPERR_NOSESSIONS;
    return DP_IF_EnumGroupPlayers( This, DPID_SYSTEM_GROUP, instance, enumplayercb,
            context, flags, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_EnumPlayers( IDirectPlay4 *iface, GUID *instance,
        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    if ( This->dp2->connectionInitialized != NO_PROVIDER && !This->dp2->bConnectionOpen &&
         !(flags & DPENUMPLAYERS_SESSION) )
        return DPERR_NOSESSIONS;
    return DP_IF_EnumGroupPlayers( This, DPID_SYSTEM_GROUP, instance, enumplayercb,
            context, flags, FALSE );
}

/* This function should call the registered callback function that the user
   passed into EnumSessions for each entry available.
 */
static BOOL DP_InvokeEnumSessionCallbacks
       ( LPDPENUMSESSIONSCALLBACK2 lpEnumSessionsCallback2,
         LPVOID lpNSInfo,
         DWORD *timeout,
         LPVOID lpContext,
         BOOL ansi )
{
  LPDPSESSIONDESC2 lpSessionDesc;

  FIXME( ": not checking for conditions\n" );

  /* Enumerate all sessions */
  /* FIXME: Need to indicate ANSI */
  while( (lpSessionDesc = NS_WalkSessions( lpNSInfo, NULL, ansi ) ) != NULL )
  {
    TRACE( "EnumSessionsCallback2 invoked\n" );
    if( !lpEnumSessionsCallback2( lpSessionDesc, timeout, 0, lpContext ) )
      return FALSE;
  }

  /* Invoke one last time to indicate that there is no more to come */
  return lpEnumSessionsCallback2( NULL, timeout, DPESC_TIMEDOUT, lpContext );
}

static DWORD CALLBACK DP_EnumSessionsSendAsyncRequestThread( LPVOID lpContext )
{
  EnumSessionAsyncCallbackData* data = lpContext;
  HANDLE hSuicideRequest = data->hSuicideRequest;
  DWORD dwTimeout = data->dwTimeout;

  TRACE( "Thread started with timeout = 0x%08lx\n", dwTimeout );

  for( ;; )
  {
    HRESULT hr;

    /* Sleep up to dwTimeout waiting for request to terminate thread */
    if( WaitForSingleObject( hSuicideRequest, dwTimeout ) == WAIT_OBJECT_0 )
    {
      TRACE( "Thread terminating on terminate request\n" );
      break;
    }

    /* Now resend the enum request */
    hr = NS_SendSessionRequestBroadcast( &data->requestGuid,
                                         data->dwEnumSessionFlags,
                                         data->password,
                                         data->lpSpData );

    if( FAILED(hr) )
    {
      ERR( "Enum broadcase request failed: %s\n", DPLAYX_HresultToString(hr) );
      /* FIXME: Should we kill this thread? How to inform the main thread? */
    }

  }

  TRACE( "Thread terminating\n" );

  /* Clean up the thread data */
  CloseHandle( hSuicideRequest );
  free( data->password );
  free( lpContext );

  /* FIXME: Need to have some notification to main app thread that this is
   *        dead. It would serve two purposes. 1) allow sync on termination
   *        so that we don't actually send something to ourselves when we
   *        become name server (race condition) and 2) so that if we die
   *        abnormally something else will be able to tell.
   */

  return 1;
}

#ifdef OKWEB_DPLAY
/* the same loop for the browser build (okweb/dplay/winweb.c): one round per call, every dwTimeout */
void winweb_every( DWORD ms, void (*fn)( void * ), void *arg );
static void DP_EnumSessionsSendAsyncRequestTask( void *lpContext )
{
  EnumSessionAsyncCallbackData *data = lpContext;
  HRESULT hr;

  if( WaitForSingleObject( data->hSuicideRequest, 0 ) == WAIT_OBJECT_0 )
  {
    winweb_every( 0, DP_EnumSessionsSendAsyncRequestTask, data );
    CloseHandle( data->hSuicideRequest );
    free( data->password );
    free( data );
    return;
  }
  hr = NS_SendSessionRequestBroadcast( &data->requestGuid, data->dwEnumSessionFlags, data->password,
                                       data->lpSpData );
  if( FAILED(hr) )
    ERR( "Enum broadcast request failed: %s\n", DPLAYX_HresultToString(hr) );
}
#endif

static void DP_KillEnumSessionThread( IDirectPlayImpl *This )
{
  /* Does a thread exist? If so we were doing an async enum session */
  if( This->dp2->hEnumSessionThread != INVALID_HANDLE_VALUE )
  {
    TRACE( "Killing EnumSession thread %p\n",
           This->dp2->hEnumSessionThread );

    /* Request that the thread kill itself nicely */
    SetEvent( This->dp2->hKillEnumSessionThreadEvent );
    CloseHandle( This->dp2->hKillEnumSessionThreadEvent );

    /* We no longer need to know about the thread */
    CloseHandle( This->dp2->hEnumSessionThread );

    This->dp2->hEnumSessionThread = INVALID_HANDLE_VALUE;
  }
}

static HRESULT WINAPI IDirectPlay2AImpl_EnumSessions( IDirectPlay2A *iface, DPSESSIONDESC2 *sdesc,
        DWORD timeout, LPDPENUMSESSIONSCALLBACK2 enumsessioncb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_EnumSessions( &This->IDirectPlay4A_iface, sdesc, timeout, enumsessioncb,
            context, flags );
}

static HRESULT WINAPI IDirectPlay2Impl_EnumSessions( IDirectPlay2 *iface, DPSESSIONDESC2 *sdesc,
        DWORD timeout, LPDPENUMSESSIONSCALLBACK2 enumsessioncb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_EnumSessions( &This->IDirectPlay4_iface, sdesc, timeout, enumsessioncb,
            context, flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_EnumSessions( IDirectPlay3A *iface, DPSESSIONDESC2 *sdesc,
        DWORD timeout, LPDPENUMSESSIONSCALLBACK2 enumsessioncb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_EnumSessions( &This->IDirectPlay4A_iface, sdesc, timeout, enumsessioncb,
            context, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_EnumSessions( IDirectPlay3 *iface, DPSESSIONDESC2 *sdesc,
        DWORD timeout, LPDPENUMSESSIONSCALLBACK2 enumsessioncb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_EnumSessions( &This->IDirectPlay4_iface, sdesc, timeout, enumsessioncb,
            context, flags );
}

static HRESULT DP_IF_EnumSessionsHeld( IDirectPlayImpl *This, DPSESSIONDESC2 *sdesc,
        DWORD timeout, LPDPENUMSESSIONSCALLBACK2 enumsessioncb, void *context, DWORD flags,
        BOOL ansi );

/* A synchronous enumeration waits (timeout) on the caller's thread. Some games enumerate on a
 * thread of their own and release the object from another meanwhile - Necrodome does, when the
 * player leaves its session list: the waiting call then woke up in freed memory (a Windows 98
 * crash in EnterCriticalSection, 2026-10-22). The call holds a reference while it runs; the last
 * Release, the caller's or this one, frees the object. */
static HRESULT DP_IF_EnumSessions( IDirectPlayImpl *This, DPSESSIONDESC2 *sdesc,
        DWORD timeout, LPDPENUMSESSIONSCALLBACK2 enumsessioncb, void *context, DWORD flags,
        BOOL ansi )
{
    HRESULT hr;
    InterlockedIncrement( &This->ref );
    hr = DP_IF_EnumSessionsHeld( This, sdesc, timeout, enumsessioncb, context, flags, ansi );
    IDirectPlayX_Release( &This->IDirectPlay4_iface );
    return hr;
}

static HRESULT DP_IF_EnumSessionsHeld( IDirectPlayImpl *This, DPSESSIONDESC2 *sdesc,
        DWORD timeout, LPDPENUMSESSIONSCALLBACK2 enumsessioncb, void *context, DWORD flags,
        BOOL ansi )
{
    EnumSessionAsyncCallbackData *data;
    WCHAR *password = NULL;
    DWORD defaultTimeout;
    void *connection;
    DPCAPS caps;
    DWORD  size;
    HRESULT hr;
    DWORD tid;

    TRACE( "(%p)->(%p,0x%08lx,%p,%p,0x%08lx)\n", This, sdesc, timeout, enumsessioncb,
            context, flags );

    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;

    if ( !sdesc )
        return DPERR_INVALIDPARAM;

    if ( sdesc->dwSize != sizeof( DPSESSIONDESC2 ) )
        return DPERR_INVALIDPARAM;

    /* Windows refuses flag bits outside the documented ones (tests/dplayx.c test_EnumSessions, flags -1) */
    if ( flags & ~(DPENUMSESSIONS_AVAILABLE | DPENUMSESSIONS_ALL | DPENUMSESSIONS_ASYNC |
                   DPENUMSESSIONS_STOPASYNC | DPENUMSESSIONS_PASSWORDREQUIRED | DPENUMSESSIONS_RETURNSTATUS) )
        return DPERR_INVALIDPARAMS;

    /* Can't enumerate if the interface is already open */
    EnterCriticalSection( &This->lock );
    if ( This->dp2->bConnectionOpen )
    {
        LeaveCriticalSection( &This->lock );
        return DPERR_GENERIC;
    }
    LeaveCriticalSection( &This->lock );

    /* The loading of a lobby provider _seems_ to require a backdoor loading
     * of the service provider to also associate with this DP object. This is
     * because the app doesn't seem to have to call EnumConnections and
     * InitializeConnection for the SP before calling this method. As such
     * we'll do their dirty work for them with a quick hack so as to always
     * load the TCP/IP service provider.
     *
     * The correct solution would seem to involve creating a dialog box which
     * contains the possible SPs. These dialog boxes most likely follow SDK
     * examples.
     */
    if ( This->dp2->bDPLSPInitialized && !This->dp2->bSPInitialized )
    {
        WARN( "Hack providing TCP/IP SP for lobby provider activated\n" );

        if ( !DP_BuildSPCompoundAddr( (GUID*)&DPSPGUID_TCPIP, &connection, &size ) )
        {
            ERR( "Can't build compound addr\n" );
            return DPERR_GENERIC;
        }

        hr = IDirectPlayX_InitializeConnection( &This->IDirectPlay4_iface, connection, 0 );
        if ( FAILED(hr) )
            return hr;

        free( connection );
        This->dp2->bSPInitialized = TRUE;
    }

    caps.dwSize = sizeof( caps );
    IDirectPlayX_GetCaps( &This->IDirectPlay4_iface, &caps, 0 );
    defaultTimeout = caps.dwTimeout;
    if ( !defaultTimeout )
        defaultTimeout = DPMSG_WAIT_5_SECS; /* Provide the TCP/IP default */

    if ( flags & DPENUMSESSIONS_STOPASYNC )
    {
        /* nothing to stop: Windows reports a generic failure (apidiff) */
        if ( This->dp2->hEnumSessionThread == INVALID_HANDLE_VALUE )
            return DPERR_GENERIC;
        DP_KillEnumSessionThread( This );
        /* Windows tells the callback the enumeration is over: one DPESC_TIMEDOUT call and no
         * sessions (tests/dplayx.c check_EnumSessions_async, its last step) */
        if ( enumsessioncb )
            enumsessioncb( NULL, &timeout, DPESC_TIMEDOUT, context );
        return DP_OK;
    }

    password = DP_DuplicateString( sdesc->lpszPassword, FALSE, ansi );
    if ( !password && sdesc->lpszPassword )
        return DPERR_OUTOFMEMORY;

    for ( ;; )
    {
        if ( !(flags & DPENUMSESSIONS_ASYNC) )
        {
            EnterCriticalSection( &This->lock );

            /* Invalidate the session cache for the interface */
            NS_InvalidateSessionCache( This->dp2->lpNameServerData );

            LeaveCriticalSection( &This->lock );

            /* Send the broadcast for session enumeration */
            hr = NS_SendSessionRequestBroadcast( &sdesc->guidApplication, flags, password,
                                                 &This->dp2->spData );
            if ( FAILED( hr ) )
            {
                free( password );
                return hr;
            }
            SleepEx( timeout ? timeout : defaultTimeout, FALSE );
        }

        EnterCriticalSection( &This->lock );

        NS_PruneSessionCache( This->dp2->lpNameServerData );
        NS_ResetSessionEnumeration( This->dp2->lpNameServerData );

        LeaveCriticalSection( &This->lock );

        if ( !DP_InvokeEnumSessionCallbacks( enumsessioncb, This->dp2->lpNameServerData, &timeout,
                                             context, ansi ) )
            break;
    }

    if ( !(flags & DPENUMSESSIONS_ASYNC) )
    {
        free( password );
        return DP_OK;
    }

    /* Async enumeration */

    if ( This->dp2->dwEnumSessionLock )
    {
        free( password );
        return DPERR_CONNECTING;
    }

    /* See if we've already created a thread to service this interface */
    if ( This->dp2->hEnumSessionThread != INVALID_HANDLE_VALUE )
    {
        free( password );
        return DP_OK;
    }

    This->dp2->dwEnumSessionLock++;

    /* Send the first enum request inline since the user may cancel a dialog
     * if one is presented. Also, may also have a connecting return code.
     */
    hr = NS_SendSessionRequestBroadcast( &sdesc->guidApplication, flags, password,
                                         &This->dp2->spData );
    if ( FAILED( hr ) )
    {
        This->dp2->dwEnumSessionLock--;
        free( password );
        return hr;
    }

    data = calloc( 1, sizeof( *data ) );
    /* FIXME: need to kill the thread on object deletion */
    data->lpSpData  = &This->dp2->spData;
    data->requestGuid = sdesc->guidApplication;
    data->dwEnumSessionFlags = flags;
    data->dwTimeout = timeout ? timeout : defaultTimeout;
    data->password = password;

    This->dp2->hKillEnumSessionThreadEvent = CreateEventW( NULL, TRUE, FALSE, NULL );
    if ( !DuplicateHandle( GetCurrentProcess(), This->dp2->hKillEnumSessionThreadEvent,
                           GetCurrentProcess(), &data->hSuicideRequest, 0, FALSE,
                           DUPLICATE_SAME_ACCESS ) )
        ERR( "Can't duplicate thread killing handle\n" );

    TRACE( ": creating EnumSessionsRequest thread\n" );
#ifdef OKWEB_DPLAY
    /* the browser build has no threads: the resend loop runs as a task of the page's pump */
    winweb_every( data->dwTimeout, DP_EnumSessionsSendAsyncRequestTask, data );
    This->dp2->hEnumSessionThread = (HANDLE)1;   /* "running" (the stop event ends the task) */
#else
    This->dp2->hEnumSessionThread = CreateThread( NULL, 0, DP_EnumSessionsSendAsyncRequestThread,
                                                  data, 0, &tid );
#endif
    if ( !This->dp2->hEnumSessionThread )
        free( password );

    This->dp2->dwEnumSessionLock--;

    return DP_OK;
}

static HRESULT WINAPI IDirectPlay4AImpl_EnumSessions( IDirectPlay4A *iface, DPSESSIONDESC2 *sdesc,
        DWORD timeout, LPDPENUMSESSIONSCALLBACK2 enumsessioncb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return DP_IF_EnumSessions( This, sdesc, timeout, enumsessioncb, context, flags, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_EnumSessions( IDirectPlay4 *iface, DPSESSIONDESC2 *sdesc,
        DWORD timeout, LPDPENUMSESSIONSCALLBACK2 enumsessioncb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    return DP_IF_EnumSessions( This, sdesc, timeout, enumsessioncb, context, flags, FALSE );
}

static HRESULT WINAPI IDirectPlay2AImpl_GetCaps( IDirectPlay2A *iface, DPCAPS *caps, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_GetCaps( &This->IDirectPlay4A_iface, caps, flags );
}

static HRESULT WINAPI IDirectPlay2Impl_GetCaps( IDirectPlay2 *iface, DPCAPS *caps, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_GetCaps( &This->IDirectPlay4_iface, caps, flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_GetCaps( IDirectPlay3A *iface, DPCAPS *caps, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_GetCaps( &This->IDirectPlay4A_iface, caps, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_GetCaps( IDirectPlay3 *iface, DPCAPS *caps, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_GetCaps( &This->IDirectPlay4_iface, caps, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_GetCaps( IDirectPlay4A *iface, DPCAPS *caps, DWORD flags )
{
    return IDirectPlayX_GetPlayerCaps( iface, DPID_ALLPLAYERS, caps, flags );
}

static HRESULT WINAPI IDirectPlay4Impl_GetCaps( IDirectPlay4 *iface, DPCAPS *caps, DWORD flags )
{
    return IDirectPlayX_GetPlayerCaps( iface, DPID_ALLPLAYERS, caps, flags );
}

static HRESULT WINAPI IDirectPlay2AImpl_GetGroupData( IDirectPlay2A *iface, DPID group, void *data,
        DWORD *size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_GetGroupData( &This->IDirectPlay4A_iface, group, data, size, flags );
}

static HRESULT WINAPI IDirectPlay2Impl_GetGroupData( IDirectPlay2 *iface, DPID group, void *data,
        DWORD *size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_GetGroupData( &This->IDirectPlay4_iface, group, data, size, flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_GetGroupData( IDirectPlay3A *iface, DPID group, void *data,
        DWORD *size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_GetGroupData( &This->IDirectPlay4A_iface, group, data, size, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_GetGroupData( IDirectPlay3 *iface, DPID group, void *data,
        DWORD *size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_GetGroupData( &This->IDirectPlay4_iface, group, data, size, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_GetGroupData( IDirectPlay4A *iface, DPID group,
        void *data, DWORD *size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_GetGroupData( &This->IDirectPlay4_iface, group, data, size, flags );
}

static HRESULT WINAPI IDirectPlay4Impl_GetGroupData( IDirectPlay4 *iface, DPID group,
        void *data, DWORD *size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    lpGroupData gdata;
    DWORD bufsize;
    void *src;

    TRACE( "(%p)->(0x%08lx,%p,%p,0x%08lx)\n", This, group, data, size, flags );

    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;

    EnterCriticalSection( &This->lock );

    if ( ( gdata = DP_FindGameGroup( This, group ) ) == NULL )
    {
        LeaveCriticalSection( &This->lock );
        return DPERR_INVALIDGROUP;
    }

    /* How much buffer is required? */
    if ( flags & DPSET_LOCAL )
    {
        bufsize = gdata->dwLocalDataSize;
        src = gdata->lpLocalData;
    }
    else
    {
        bufsize = gdata->dwRemoteDataSize;
        src = gdata->lpRemoteData;
    }

    *size = bufsize;

    /* Is the user requesting to know how big a buffer is required? */
    if ( !data || *size < bufsize )
    {
        LeaveCriticalSection( &This->lock );
        return DPERR_BUFFERTOOSMALL;
    }

    CopyMemory( data, src, bufsize );

    LeaveCriticalSection( &This->lock );

    return DP_OK;
}

static HRESULT DP_IF_GetGroupName( IDirectPlayImpl *This, DPID idGroup, void *lpData,
        DWORD *lpdwDataSize, BOOL bAnsi )
{
  lpGroupData lpGData;
  DWORD       dwRequiredDataSize;

  FIXME("(%p)->(0x%08lx,%p,%p,%u) ANSI ignored\n",
          This, idGroup, lpData, lpdwDataSize, bAnsi );

  EnterCriticalSection( &This->lock );

  if( ( lpGData = DP_FindGameGroup( This, idGroup ) ) == NULL )
  {
    LeaveCriticalSection( &This->lock );
    return DPERR_INVALIDGROUP;
  }

  dwRequiredDataSize = DP_CopyName( NULL, bAnsi ? lpGData->nameA : lpGData->name, bAnsi, bAnsi, FALSE );

  if( ( lpData == NULL ) ||
      ( *lpdwDataSize < dwRequiredDataSize )
    )
  {
    *lpdwDataSize = dwRequiredDataSize;
    LeaveCriticalSection( &This->lock );
    return DPERR_BUFFERTOOSMALL;
  }

  DP_CopyName( lpData, bAnsi ? lpGData->nameA : lpGData->name, bAnsi, bAnsi, FALSE );

  LeaveCriticalSection( &This->lock );

  return DP_OK;
}

static HRESULT WINAPI IDirectPlay2AImpl_GetGroupName( IDirectPlay2A *iface, DPID group, void *data,
        DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_GetGroupName( &This->IDirectPlay4A_iface, group, data, size );
}

static HRESULT WINAPI IDirectPlay2Impl_GetGroupName( IDirectPlay2 *iface, DPID group, void *data,
        DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_GetGroupName( &This->IDirectPlay4_iface, group, data, size );
}

static HRESULT WINAPI IDirectPlay3AImpl_GetGroupName( IDirectPlay3A *iface, DPID group, void *data,
        DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_GetGroupName( &This->IDirectPlay4A_iface, group, data, size );
}

static HRESULT WINAPI IDirectPlay3Impl_GetGroupName( IDirectPlay3 *iface, DPID group, void *data,
        DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_GetGroupName( &This->IDirectPlay4_iface, group, data, size );
}

static HRESULT WINAPI IDirectPlay4AImpl_GetGroupName( IDirectPlay4A *iface, DPID idGroup,
        void *lpData, DWORD *lpdwDataSize )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;
    return DP_IF_GetGroupName( This, idGroup, lpData, lpdwDataSize, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_GetGroupName( IDirectPlay4 *iface, DPID idGroup,
        void *lpData, DWORD *lpdwDataSize )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;
    return DP_IF_GetGroupName( This, idGroup, lpData, lpdwDataSize, FALSE );
}

static HRESULT WINAPI IDirectPlay2AImpl_GetMessageCount( IDirectPlay2A *iface, DPID player,
        DWORD *count )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_GetMessageCount( &This->IDirectPlay4A_iface, player, count );
}

static HRESULT WINAPI IDirectPlay2Impl_GetMessageCount( IDirectPlay2 *iface, DPID player,
        DWORD *count )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_GetMessageCount( &This->IDirectPlay4_iface, player, count );
}

static HRESULT WINAPI IDirectPlay3AImpl_GetMessageCount( IDirectPlay3A *iface, DPID player,
        DWORD *count )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_GetMessageCount( &This->IDirectPlay4A_iface, player, count );
}

static HRESULT WINAPI IDirectPlay3Impl_GetMessageCount( IDirectPlay3 *iface, DPID player,
        DWORD *count )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_GetMessageCount( &This->IDirectPlay4_iface, player, count );
}

static HRESULT WINAPI IDirectPlay4AImpl_GetMessageCount( IDirectPlay4A *iface, DPID player,
        DWORD *count )
{
    return IDirectPlayX_GetMessageQueue( iface, 0, player, DPMESSAGEQUEUE_RECEIVE, count, NULL );
}

static HRESULT WINAPI IDirectPlay4Impl_GetMessageCount( IDirectPlay4 *iface, DPID player,
        DWORD *count )
{
    return IDirectPlayX_GetMessageQueue( iface, 0, player, DPMESSAGEQUEUE_RECEIVE, count, NULL );
}

static HRESULT WINAPI IDirectPlay2AImpl_GetPlayerAddress( IDirectPlay2A *iface, DPID player,
        void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_GetPlayerAddress( &This->IDirectPlay4A_iface, player, data, size );
}

static HRESULT WINAPI IDirectPlay2Impl_GetPlayerAddress( IDirectPlay2 *iface, DPID player,
        void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_GetPlayerAddress( &This->IDirectPlay4_iface, player, data, size );
}

static HRESULT WINAPI IDirectPlay3AImpl_GetPlayerAddress( IDirectPlay3A *iface, DPID player,
        void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_GetPlayerAddress( &This->IDirectPlay4A_iface, player, data, size );
}

static HRESULT WINAPI IDirectPlay3Impl_GetPlayerAddress( IDirectPlay3 *iface, DPID player,
        void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_GetPlayerAddress( &This->IDirectPlay4_iface, player, data, size );
}

static HRESULT WINAPI IDirectPlay4AImpl_GetPlayerAddress( IDirectPlay4A *iface, DPID player,
        void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_GetPlayerAddress( &This->IDirectPlay4_iface, player, data, size );
}

static HRESULT WINAPI IDirectPlay4Impl_GetPlayerAddress( IDirectPlay4 *iface, DPID player,
        void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    lpPlayerList plist;
    struct { DPADDRESS h1; DWORD total; DPADDRESS h2; GUID sp; } addr;

    TRACE( "(%p)->(0x%08lx,%p,%p)\n", This, player, data, size );
    /* as Windows' DirectPlay answers (tests/gpa.c): id 0 is not supported in any state, an unknown
     * player is invalid, a known one gets a compound address. Windows' TCP/IP provider adds its
     * DPAID_INet elements; ours says only which provider it is (a joiner reaches peers by id). */
    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;
    if ( !player )
        return DPERR_UNSUPPORTED;
    if ( !size )
        return DPERR_INVALIDPARAMS;
    EnterCriticalSection( &This->lock );
    plist = DP_FindPlayer( This, player );
    LeaveCriticalSection( &This->lock );
    if ( !plist )
        return DPERR_INVALIDPLAYER;
    addr.h1.guidDataType = DPAID_TotalSize; addr.h1.dwDataSize = sizeof(DWORD); addr.total = sizeof(addr);
    addr.h2.guidDataType = DPAID_ServiceProvider; addr.h2.dwDataSize = sizeof(GUID);
    addr.sp = This->dp2->spData.lpGuid ? *This->dp2->spData.lpGuid : GUID_NULL;
    if ( !data || *size < sizeof(addr) )
    {
        *size = sizeof(addr);
        return DPERR_BUFFERTOOSMALL;
    }
    memcpy( data, &addr, sizeof(addr) );
    *size = sizeof(addr);
    return DP_OK;
}

static HRESULT WINAPI IDirectPlay2AImpl_GetPlayerCaps( IDirectPlay2A *iface, DPID player,
        DPCAPS *caps, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_GetPlayerCaps( &This->IDirectPlay4A_iface, player, caps, flags );
}

static HRESULT WINAPI IDirectPlay2Impl_GetPlayerCaps( IDirectPlay2 *iface, DPID player,
        DPCAPS *caps, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_GetPlayerCaps( &This->IDirectPlay4_iface, player, caps, flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_GetPlayerCaps( IDirectPlay3A *iface, DPID player,
        DPCAPS *caps, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_GetPlayerCaps( &This->IDirectPlay4A_iface, player, caps, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_GetPlayerCaps( IDirectPlay3 *iface, DPID player,
        DPCAPS *caps, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_GetPlayerCaps( &This->IDirectPlay4_iface, player, caps, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_GetPlayerCaps( IDirectPlay4A *iface, DPID player,
        DPCAPS *caps, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_GetPlayerCaps( &This->IDirectPlay4_iface, player, caps, flags );
}

static HRESULT WINAPI IDirectPlay4Impl_GetPlayerCaps( IDirectPlay4 *iface, DPID player,
        DPCAPS *caps, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    DPSP_GETCAPSDATA data;
    lpPlayerList plist;
    HRESULT hr;

    TRACE( "(%p)->(0x%08lx,%p,0x%08lx)\n", This, player, caps, flags);

    if ( !caps )
        return DPERR_INVALIDPARAMS;

    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;

    if( caps->dwSize != sizeof(DPCAPS) )
        return DPERR_INVALIDPARAMS;

    /* player 0 = the session in general (dplayx asks that itself) */
    if ( player && !DP_PlayerExists( This, player ) )
        return DPERR_INVALIDPLAYER;

    /* Query the service provider */
    data.idPlayer = player;
    data.dwFlags = flags;
    data.lpCaps = caps;
    data.lpISP = This->dp2->spData.lpISP;

    hr = (*This->dp2->spData.lpCB->GetCaps)( &data );
    if ( FAILED( hr ) )
        return hr;

    /* what DirectPlay itself adds to the provider's answer, as Windows' does (tests/gpa.c compares
     * the two over the same TCP/IP provider): the host bit, "guaranteed supported" when the provider
     * delivers guaranteed messages itself, the local bit for a player on this machine, and "no
     * limit" (65536) where the provider leaves the player counts at 0 */
    EnterCriticalSection( &This->lock );
    if ( This->dp2->bConnectionOpen && This->dp2->bHostInterface )
        caps->dwFlags |= DPCAPS_ISHOST;
    if ( player && (plist = DP_FindPlayer( This, player )) && (plist->lpPData->dwFlags & DPPLAYER_LOCAL) )
        caps->dwFlags |= DPPLAYERCAPS_LOCAL;
    LeaveCriticalSection( &This->lock );
    if ( caps->dwFlags & DPCAPS_GUARANTEEDOPTIMIZED )
        caps->dwFlags |= DPCAPS_GUARANTEEDSUPPORTED;
    if ( !caps->dwMaxPlayers ) caps->dwMaxPlayers = 65536;
    if ( !caps->dwMaxLocalPlayers ) caps->dwMaxLocalPlayers = 65536;
    /* Windows reports 8 bytes less than the provider can carry, for unguaranteed and guaranteed
     * sends alike (S1 2026-10-14 over Windows' TCP/IP provider: 65479 / 1048547 where the provider
     * leaves 65487 / 1048555). 8 = the idFrom/idTo pair of a player message, [MC-DPL4CS] 2.2.45 [INF] */
    if ( caps->dwMaxBufferSize > 8 ) caps->dwMaxBufferSize -= 8;
    return DP_OK;
}

static HRESULT WINAPI IDirectPlay2AImpl_GetPlayerData( IDirectPlay2A *iface, DPID player,
        void *data, DWORD *size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_GetPlayerData( &This->IDirectPlay4A_iface, player, data, size, flags );
}

static HRESULT WINAPI IDirectPlay2Impl_GetPlayerData( IDirectPlay2 *iface, DPID player,
        void *data, DWORD *size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_GetPlayerData( &This->IDirectPlay4_iface, player, data, size, flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_GetPlayerData( IDirectPlay3A *iface, DPID player,
        void *data, DWORD *size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_GetPlayerData( &This->IDirectPlay4A_iface, player, data, size, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_GetPlayerData( IDirectPlay3 *iface, DPID player,
        void *data, DWORD *size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_GetPlayerData( &This->IDirectPlay4_iface, player, data, size, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_GetPlayerData( IDirectPlay4A *iface, DPID player,
        void *data, DWORD *size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_GetPlayerData( &This->IDirectPlay4_iface, player, data, size, flags );
}

static HRESULT WINAPI IDirectPlay4Impl_GetPlayerData( IDirectPlay4 *iface, DPID player,
        void *data, DWORD *size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    lpPlayerList plist;
    DWORD bufsize;
    void *src;

    TRACE( "(%p)->(0x%08lx,%p,%p,0x%08lx)\n", This, player, data, size, flags );

    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;

    EnterCriticalSection( &This->lock );

    if ( ( plist = DP_FindPlayer( This, player ) ) == NULL )
    {
        LeaveCriticalSection( &This->lock );
        return DPERR_INVALIDPLAYER;
    }

    if ( flags & DPSET_LOCAL )
    {
        bufsize = plist->lpPData->dwLocalDataSize;
        src = plist->lpPData->lpLocalData;
    }
    else
    {
        bufsize = plist->lpPData->dwRemoteDataSize;
        src = plist->lpPData->lpRemoteData;
    }

    *size = bufsize;

    /* Is the user requesting to know how big a buffer is required? */
    if ( !data || *size < bufsize )
    {
        LeaveCriticalSection( &This->lock );
        return DPERR_BUFFERTOOSMALL;
    }

    CopyMemory( data, src, bufsize );

    LeaveCriticalSection( &This->lock );

    return DP_OK;
}

static HRESULT DP_IF_GetPlayerName( IDirectPlayImpl *This, DPID idPlayer, void *lpData,
        DWORD *lpdwDataSize, BOOL bAnsi )
{
  lpPlayerList lpPList;
  DWORD       dwRequiredDataSize;

  FIXME( "(%p)->(0x%08lx,%p,%p,%u): ANSI\n",
         This, idPlayer, lpData, lpdwDataSize, bAnsi );

  if( This->dp2->connectionInitialized == NO_PROVIDER )
  {
    return DPERR_UNINITIALIZED;
  }

  EnterCriticalSection( &This->lock );

  if( ( lpPList = DP_FindPlayer( This, idPlayer ) ) == NULL )
  {
    LeaveCriticalSection( &This->lock );
    return DPERR_INVALIDPLAYER;
  }

  dwRequiredDataSize = DP_CopyName( NULL, bAnsi ? lpPList->lpPData->nameA : lpPList->lpPData->name,
                                    bAnsi, bAnsi, FALSE );

  if( ( lpData == NULL ) ||
      ( *lpdwDataSize < dwRequiredDataSize )
    )
  {
    *lpdwDataSize = dwRequiredDataSize;
    LeaveCriticalSection( &This->lock );
    return DPERR_BUFFERTOOSMALL;
  }

  DP_CopyName( lpData, bAnsi ? lpPList->lpPData->nameA : lpPList->lpPData->name, bAnsi, bAnsi, FALSE );

  LeaveCriticalSection( &This->lock );

  return DP_OK;
}

static HRESULT WINAPI IDirectPlay2AImpl_GetPlayerName( IDirectPlay2A *iface, DPID player,
        void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_GetPlayerName( &This->IDirectPlay4A_iface, player, data, size );
}

static HRESULT WINAPI IDirectPlay2Impl_GetPlayerName( IDirectPlay2 *iface, DPID player,
        void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_GetPlayerName( &This->IDirectPlay4_iface, player, data, size );
}

static HRESULT WINAPI IDirectPlay3AImpl_GetPlayerName( IDirectPlay3A *iface, DPID player,
        void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_GetPlayerName( &This->IDirectPlay4A_iface, player, data, size );
}

static HRESULT WINAPI IDirectPlay3Impl_GetPlayerName( IDirectPlay3 *iface, DPID player,
        void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_GetPlayerName( &This->IDirectPlay4_iface, player, data, size );
}

static HRESULT WINAPI IDirectPlay4AImpl_GetPlayerName( IDirectPlay4A *iface, DPID idPlayer,
        void *lpData, DWORD *lpdwDataSize )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return DP_IF_GetPlayerName( This, idPlayer, lpData, lpdwDataSize, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_GetPlayerName( IDirectPlay4 *iface, DPID idPlayer,
        void *lpData, DWORD *lpdwDataSize )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    return DP_IF_GetPlayerName( This, idPlayer, lpData, lpdwDataSize, FALSE );
}

static HRESULT DP_GetSessionDesc( IDirectPlayImpl *This, void *lpData, DWORD *lpdwDataSize,
        BOOL bAnsi )
{
  DWORD dwRequiredSize;

  TRACE( "(%p)->(%p,%p,%u)\n", This, lpData, lpdwDataSize, bAnsi );

  if( This->dp2->connectionInitialized == NO_PROVIDER )
  {
    return DPERR_UNINITIALIZED;
  }

  if( ( lpData == NULL ) && ( lpdwDataSize == NULL ) )
  {
    return DPERR_INVALIDPARAMS;
  }

  if( !This->dp2->bConnectionOpen )
  {
    return DPERR_NOSESSIONS;
  }

  EnterCriticalSection( &This->lock );

  dwRequiredSize = DP_CopySessionDesc( NULL, This->dp2->lpSessionDesc, bAnsi, FALSE );   /* stored WCHAR */

  if ( ( lpData == NULL ) ||
       ( *lpdwDataSize < dwRequiredSize )
     )
  {
    *lpdwDataSize = dwRequiredSize;
    LeaveCriticalSection( &This->lock );
    return DPERR_BUFFERTOOSMALL;
  }

  DP_CopySessionDesc( lpData, This->dp2->lpSessionDesc, bAnsi, FALSE );

  LeaveCriticalSection( &This->lock );

  return DP_OK;
}

static HRESULT WINAPI IDirectPlay2AImpl_GetSessionDesc( IDirectPlay2A *iface, void *data,
        DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_GetSessionDesc( &This->IDirectPlay4A_iface, data, size );
}

static HRESULT WINAPI IDirectPlay2Impl_GetSessionDesc( IDirectPlay2 *iface, void *data,
        DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_GetSessionDesc( &This->IDirectPlay4_iface, data, size );
}

static HRESULT WINAPI IDirectPlay3AImpl_GetSessionDesc( IDirectPlay3A *iface, void *data,
        DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_GetSessionDesc( &This->IDirectPlay4A_iface, data, size );
}

static HRESULT WINAPI IDirectPlay3Impl_GetSessionDesc( IDirectPlay3 *iface, void *data,
        DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_GetSessionDesc( &This->IDirectPlay4_iface, data, size );
}

static HRESULT WINAPI IDirectPlay4AImpl_GetSessionDesc( IDirectPlay4A *iface, void *lpData,
        DWORD *lpdwDataSize )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return DP_GetSessionDesc( This, lpData, lpdwDataSize, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_GetSessionDesc( IDirectPlay4 *iface, void *lpData,
        DWORD *lpdwDataSize )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    return DP_GetSessionDesc( This, lpData, lpdwDataSize, TRUE );
}

static HRESULT WINAPI IDirectPlay2AImpl_Initialize( IDirectPlay2A *iface, GUID *guid )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_Initialize( &This->IDirectPlay4A_iface, guid );
}

static HRESULT WINAPI IDirectPlay2Impl_Initialize( IDirectPlay2 *iface, GUID *guid )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_Initialize( &This->IDirectPlay4_iface, guid );
}

static HRESULT WINAPI IDirectPlay3AImpl_Initialize( IDirectPlay3A *iface, GUID *guid )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_Initialize( &This->IDirectPlay4A_iface, guid );
}

static HRESULT WINAPI IDirectPlay3Impl_Initialize( IDirectPlay3 *iface, GUID *guid )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_Initialize( &This->IDirectPlay4_iface, guid );
}

/* Intended only for COM compatibility. Always returns an error. */
static HRESULT WINAPI IDirectPlay4AImpl_Initialize( IDirectPlay4A *iface, GUID *guid )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    TRACE("(%p)->(%p): no-op\n", This, guid );
    return DPERR_ALREADYINITIALIZED;
}

static HRESULT WINAPI IDirectPlay4Impl_Initialize( IDirectPlay4 *iface, GUID *guid )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    TRACE( "(%p)->(%p): no-op\n", This, guid );
    return DPERR_ALREADYINITIALIZED;
}


static HRESULT DP_SecureOpen( IDirectPlayImpl *This, const DPSESSIONDESC2 *lpsd, DWORD dwFlags,
        const DPSECURITYDESC *lpSecurity, const DPCREDENTIALS *lpCredentials, BOOL bAnsi )
{
  void *spMessageHeader = NULL;
  HRESULT hr;
  /* the caller gave a password of its own (AoE2 never does: it has no password box) */
  BOOL appPassword = lpsd->lpszPassword && ((const BYTE *)lpsd->lpszPassword)[ 0 ];
#if defined(OKP_BUILTIN) || defined(OKWEB_DPLAY)
  WCHAR okpPassword[ 80 ];   /* joining a password game: the hash of what the player typed */
  BOOL useOkpPassword = FALSE;
#endif

  FIXME( "(%p)->(%p,0x%08lx,%p,%p): partial stub\n",
         This, lpsd, dwFlags, lpSecurity, lpCredentials );

  if( lpsd->dwSize != sizeof(DPSESSIONDESC2) )
  {
    TRACE( ": rejecting invalid dpsd size (%ld).\n", lpsd->dwSize );
    return DPERR_INVALIDPARAMS;
  }
  if( This->dp2->connectionInitialized == NO_PROVIDER )
  {
    return DPERR_UNINITIALIZED;
  }

  EnterCriticalSection( &This->lock );
  if( This->dp2->bConnectionOpen )
  {
    TRACE( ": rejecting already open connection.\n" );
    LeaveCriticalSection( &This->lock );
    return DPERR_ALREADYINITIALIZED;
  }
  LeaveCriticalSection( &This->lock );

  /* unknown flag bits are refused before a missing provider, as Windows does (apidiff) */
  if( dwFlags & ~(DPOPEN_JOIN | DPOPEN_CREATE | DPOPEN_RETURNSTATUS) )
  {
    TRACE( ": rejecting unknown open flags 0x%08lx.\n", dwFlags );
    return DPERR_INVALIDPARAMS;
  }

  /* If we're enumerating, kill the thread */
  DP_KillEnumSessionThread( This );

  if( dwFlags & DPOPEN_CREATE )
  {
    /* Rightoo - this computer is the host and the local computer needs to be
       the name server so that others can join this session */
    NS_SetLocalComputerAsNameServer( lpsd, This->dp2->lpNameServerData );

    This->dp2->bHostInterface = TRUE;

    hr = DP_SetSessionDesc( This, lpsd, 0, TRUE, bAnsi );
    if( FAILED( hr ) )
    {
      ERR( "Unable to set session desc: %s\n", DPLAYX_HresultToString( hr ) );
      return hr;
    }
    /* "password required" without a password (Midtown Madness sets the flag with an empty one):
       Windows treats that as no password, and our listing would show it as [PW] */
    if( !appPassword && (This->dp2->lpSessionDesc->dwFlags & DPSESSION_PASSWORDREQUIRED) )
      This->dp2->lpSessionDesc->dwFlags &= ~DPSESSION_PASSWORDREQUIRED;
#if defined(OKP_BUILTIN) || defined(OKWEB_DPLAY)
    /* Online/LAN Play: ask the host for an optional password (the session now has its GUID); in the
       browser the page asks (okweb/dplay/okweb_password.c) */
    if( This->dp2->okpConnection && !appPassword )
    {
      WCHAR hashed[ 80 ];
      if ( DP_BUILTIN_AskHostPassword( &This->dp2->lpSessionDesc->guidInstance, hashed, ARRAY_SIZE( hashed ) ) )
      {
        DPSESSIONDESC2 copy = *This->dp2->lpSessionDesc, *locked;
        copy.lpszPassword = hashed;
        copy.dwFlags |= DPSESSION_PASSWORDREQUIRED;
        if ( (locked = DP_DuplicateSessionDesc( &copy, FALSE, FALSE )) )
        {
          free( This->dp2->lpSessionDesc );
          This->dp2->lpSessionDesc = locked;
        }
      }
    }
#endif
  }
  else
  {
    DPSESSIONDESC2 *sessionDesc;

    EnterCriticalSection( &This->lock );

    NS_ResetSessionEnumeration( This->dp2->lpNameServerData );

    LeaveCriticalSection( &This->lock );

    for ( ;; )
    {
      sessionDesc = NS_WalkSessions( This->dp2->lpNameServerData, &spMessageHeader, bAnsi );
      if ( !sessionDesc )
        return DPERR_NOSESSIONS;
      if ( IsEqualGUID( &sessionDesc->guidInstance, &lpsd->guidInstance ) )
        break;
    }

    /* No need to enter the critical section here as the messaging thread won't access the data
     * while bConnectionOpen is FALSE. */

    free( This->dp2->lpSessionDesc );
    This->dp2->lpSessionDesc = DP_DuplicateSessionDesc( sessionDesc, FALSE, bAnsi );   /* stored WCHAR */
    if ( !This->dp2->lpSessionDesc )
      return DPERR_OUTOFMEMORY;
#ifdef OKP_BUILTIN
    /* the host on another build of the game (exe fingerprint): warn before joining (Cancel = no join) */
    if ( (dwFlags & DPOPEN_JOIN) && This->dp2->okpConnection )
    {
      hr = DP_BUILTIN_CheckBuild( This->dp2->spData.lpISP, &This->dp2->lpSessionDesc->guidInstance );
      if ( FAILED( hr ) )
        return hr;
    }
#endif
#if defined(OKP_BUILTIN) || defined(OKWEB_DPLAY)
    /* a password game on Online/LAN Play: ask now, before anything is opened (Cancel = no join) */
    if ( (dwFlags & DPOPEN_JOIN) && This->dp2->okpConnection && !appPassword &&
         (This->dp2->lpSessionDesc->dwFlags & DPSESSION_PASSWORDREQUIRED) )
    {
      hr = DP_BUILTIN_AskJoinPassword( &This->dp2->lpSessionDesc->guidInstance, okpPassword,
                                       ARRAY_SIZE( okpPassword ) );
      if ( FAILED( hr ) )
        return hr;
      useOkpPassword = TRUE;
    }
#endif
  }

  /* Invoke the conditional callback for the service provider */
  if( This->dp2->spData.lpCB->Open )
  {
    DPSP_OPENDATA data;

    FIXME( "Not all data fields are correct. Need new parameter\n" );

    data.bCreate           = (dwFlags & DPOPEN_CREATE ) != 0;
    data.lpSPMessageHeader = spMessageHeader;
    data.lpISP             = This->dp2->spData.lpISP;
    data.bReturnStatus     = (dwFlags & DPOPEN_RETURNSTATUS) != 0;
    data.dwOpenFlags       = dwFlags;
    data.dwSessionFlags    = This->dp2->lpSessionDesc->dwFlags;

    hr = (*This->dp2->spData.lpCB->Open)(&data);
    if( SUCCEEDED( hr ) )
      DP_QuerySPCaps( This );
    if( FAILED( hr ) )
    {
      ERR( "Unable to open session: %s\n", DPLAYX_HresultToString( hr ) );
      return hr;
    }
  }

  {
    /* Create the system group of which everything is a part of */
    DPID systemGroup = DPID_SYSTEM_GROUP;

    hr = DP_IF_CreateGroup( This, NULL, &systemGroup, NULL,
                            NULL, 0, 0, TRUE );
    if ( FAILED( hr ) )
    {
      if( This->dp2->spData.lpCB->CloseEx )
      {
        DPSP_CLOSEDATA data;
        data.lpISP = This->dp2->spData.lpISP;
        (*This->dp2->spData.lpCB->CloseEx)( &data );
      }
    }
  }

  if( dwFlags & DPOPEN_JOIN )
  {
    DWORD createFlags = DPLAYI_PLAYER_SYSPLAYER | DPLAYI_PLAYER_PLAYERLOCAL;
    WCHAR *password;

#if defined(OKP_BUILTIN) || defined(OKWEB_DPLAY)
    if ( useOkpPassword )
      password = DP_DuplicateString( okpPassword, FALSE, FALSE );
    else
#endif
    password = DP_DuplicateString( lpsd->lpszPassword, FALSE, bAnsi );
    if ( !password && lpsd->lpszPassword )
    {
      DP_IF_DestroyGroup( This, NULL, DPID_SYSTEM_GROUP, TRUE );
      if( This->dp2->spData.lpCB->CloseEx )
      {
        DPSP_CLOSEDATA data;
        data.lpISP = This->dp2->spData.lpISP;
        (*This->dp2->spData.lpCB->CloseEx)( &data );
      }
      return DPERR_OUTOFMEMORY;
    }

    /* Create the server player for this interface. This way we can receive
     * messages for this session.
     */
    /* FIXME: I suppose that we should be setting an event for a receive
     *        type of thing. That way the messaging thread could know to wake
     *        up. DPlay would then trigger the hEvent for the player the
     *        message is directed to.
     */
    hr = DP_MSG_SendRequestPlayerId( This, createFlags, &This->dp2->systemPlayerId );
    if( FAILED( hr ) )
    {
      ERR( "Request for ID failed: %s\n", DPLAYX_HresultToString( hr ) );
#ifdef OKP_BUILTIN
      okp_join_failed( This->dp2->spData.lpISP, "request_id", hr );   /* the connection report */
#endif
      free( password );
      DP_IF_DestroyGroup( This, NULL, DPID_SYSTEM_GROUP, TRUE );
      if( This->dp2->spData.lpCB->CloseEx )
      {
        DPSP_CLOSEDATA data;
        data.lpISP = This->dp2->spData.lpISP;
        (*This->dp2->spData.lpCB->CloseEx)( &data );
      }
      return hr;
    }

    /* No need to enter the critical section here as the messaging thread won't access the data
     * while bConnectionOpen is FALSE. */

    hr = DP_CreatePlayer( This, NULL, &This->dp2->systemPlayerId, NULL, NULL, 0, NULL, 0,
                          createFlags, NULL, NULL, bAnsi );
    if( FAILED( hr ) )
    {
      free( password );
      DP_IF_DestroyGroup( This, NULL, DPID_SYSTEM_GROUP, TRUE );
      if( This->dp2->spData.lpCB->CloseEx )
      {
        DPSP_CLOSEDATA data;
        data.lpISP = This->dp2->spData.lpISP;
        (*This->dp2->spData.lpCB->CloseEx)( &data );
      }
      return hr;
    }

    hr = DP_MSG_ForwardPlayerCreation( This, This->dp2->systemPlayerId, password );
    free( password );
    if( FAILED( hr ) )
    {
#if defined(OKP_BUILTIN) || defined(OKWEB_DPLAY)
      if ( useOkpPassword ) DP_BUILTIN_JoinFailed( hr );   /* "wrong password" for the player */
#endif
#ifdef OKP_BUILTIN
      okp_join_failed( This->dp2->spData.lpISP, "refused", hr );   /* the connection report */
#endif
      DP_DeletePlayer( This, This->dp2->systemPlayerId );
      DP_IF_DestroyGroup( This, NULL, DPID_SYSTEM_GROUP, TRUE );
      if( This->dp2->spData.lpCB->CloseEx )
      {
        DPSP_CLOSEDATA data;
        data.lpISP = This->dp2->spData.lpISP;
        (*This->dp2->spData.lpCB->CloseEx)( &data );
      }
      return hr;
    }
  }
  else if( dwFlags & DPOPEN_CREATE )
  {
    /* the host's system player, as Windows creates it (flags 0xb): a system player that is also
     * the name server. As an app-server player (0x88) it was counted and messaged like a normal
     * player, and joiners did not see a machine to send to. */
    DWORD createFlags = DPLAYI_PLAYER_SYSPLAYER | DPLAYI_PLAYER_NAMESRVR | DPLAYI_PLAYER_PLAYERLOCAL;

    This->dp2->systemPlayerId = DP_NextObjectId();

    hr = DP_CreatePlayer( This, NULL, &This->dp2->systemPlayerId, NULL, NULL, 0, NULL, 0,
                          createFlags, NULL, NULL, bAnsi );
    if( FAILED( hr ) )
    {
      DP_IF_DestroyGroup( This, NULL, DPID_SYSTEM_GROUP, TRUE );
      if( This->dp2->spData.lpCB->CloseEx )
      {
        DPSP_CLOSEDATA data;
        data.lpISP = This->dp2->spData.lpISP;
        (*This->dp2->spData.lpCB->CloseEx)( &data );
      }
      return hr;
    }
  }

  EnterCriticalSection( &This->lock );

  This->dp2->bConnectionOpen = TRUE;
  DP_LIFE_Start( This );

  LeaveCriticalSection( &This->lock );

  return DP_OK;
}

static HRESULT WINAPI IDirectPlay2AImpl_Open( IDirectPlay2A *iface, DPSESSIONDESC2 *sdesc,
        DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_Open( &This->IDirectPlay4A_iface, sdesc, flags );
}

static HRESULT WINAPI IDirectPlay2Impl_Open( IDirectPlay2 *iface, DPSESSIONDESC2 *sdesc,
        DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_Open( &This->IDirectPlay4_iface, sdesc, flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_Open( IDirectPlay3A *iface, DPSESSIONDESC2 *sdesc,
        DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_Open( &This->IDirectPlay4A_iface, sdesc, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_Open( IDirectPlay3 *iface, DPSESSIONDESC2 *sdesc,
        DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_Open( &This->IDirectPlay4_iface, sdesc, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_Open( IDirectPlay4A *iface, DPSESSIONDESC2 *sdesc,
        DWORD flags )
{
    return IDirectPlayX_SecureOpen( iface, sdesc, flags, NULL, NULL );
}

static HRESULT WINAPI IDirectPlay4Impl_Open( IDirectPlay4 *iface, DPSESSIONDESC2 *sdesc,
        DWORD flags )
{
    return IDirectPlayX_SecureOpen( iface, sdesc, flags, NULL, NULL );
}

static HRESULT DP_IF_Receive( IDirectPlayImpl *This, DPID *lpidFrom, DPID *lpidTo, DWORD dwFlags,
        void *lpData, DWORD *lpdwDataSize, BOOL bAnsi )
{
  LPDPMSG lpMsg = NULL;
  DWORD msgSize;

  TRACE( "(%p)->(%p,%p,0x%08lx,%p,%p,%u)\n",
         This, lpidFrom, lpidTo, dwFlags, lpData, lpdwDataSize, bAnsi );

  if( This->dp2->connectionInitialized == NO_PROVIDER )
  {
    return DPERR_UNINITIALIZED;
  }

  EnterCriticalSection( &This->lock );

  for( lpMsg = DPQ_FIRST( This->dp2->receiveMsgs ); lpMsg; lpMsg = DPQ_NEXT( lpMsg->msgs ) )
  {
    if( ( dwFlags & DPRECEIVE_TOPLAYER ) && ( lpMsg->toId != *lpidTo ) )
      continue;
    if( ( dwFlags & DPRECEIVE_FROMPLAYER ) && ( lpMsg->fromId != *lpidFrom ) )
      continue;
    break;
  }

  if( lpMsg == NULL )
  {
    LeaveCriticalSection( &This->lock );
    return DPERR_NOMESSAGES;
  }

  msgSize = lpMsg->copyMessage( NULL, lpMsg->msg, lpMsg->genericSize, bAnsi );

  if( *lpdwDataSize < msgSize || !lpData )
  {
    *lpdwDataSize = msgSize;
    LeaveCriticalSection( &This->lock );
    return DPERR_BUFFERTOOSMALL;
  }

  *lpidFrom = lpMsg->fromId;
  *lpidTo = lpMsg->toId;
  *lpdwDataSize = msgSize;

  /* Copy into the provided buffer */
  lpMsg->copyMessage( lpData, lpMsg->msg, lpMsg->genericSize, bAnsi );

  if( !( dwFlags & DPRECEIVE_PEEK ) )
  {
    DPQ_REMOVE( This->dp2->receiveMsgs, lpMsg, msgs );
    free( lpMsg->msg );
    free( lpMsg );
  }

  LeaveCriticalSection( &This->lock );

  return DP_OK;
}

static HRESULT WINAPI IDirectPlay2AImpl_Receive( IDirectPlay2A *iface, DPID *from, DPID *to,
        DWORD flags, void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_Receive( &This->IDirectPlay4A_iface, from, to, flags, data, size );
}

static HRESULT WINAPI IDirectPlay2Impl_Receive( IDirectPlay2 *iface, DPID *from, DPID *to,
        DWORD flags, void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_Receive( &This->IDirectPlay4_iface, from, to, flags, data, size );
}

static HRESULT WINAPI IDirectPlay3AImpl_Receive( IDirectPlay3A *iface, DPID *from, DPID *to,
        DWORD flags, void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_Receive( &This->IDirectPlay4A_iface, from, to, flags, data, size );
}

static HRESULT WINAPI IDirectPlay3Impl_Receive( IDirectPlay3 *iface, DPID *from, DPID *to,
        DWORD flags, void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_Receive( &This->IDirectPlay4_iface, from, to, flags, data, size );
}

static HRESULT WINAPI IDirectPlay4AImpl_Receive( IDirectPlay4A *iface, DPID *lpidFrom,
        DPID *lpidTo, DWORD dwFlags, void *lpData, DWORD *lpdwDataSize )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return DP_IF_Receive( This, lpidFrom, lpidTo, dwFlags, lpData, lpdwDataSize, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_Receive( IDirectPlay4 *iface, DPID *lpidFrom,
        DPID *lpidTo, DWORD dwFlags, void *lpData, DWORD *lpdwDataSize )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    return DP_IF_Receive( This, lpidFrom, lpidTo, dwFlags, lpData, lpdwDataSize, FALSE );
}

static HRESULT WINAPI IDirectPlay2AImpl_Send( IDirectPlay2A *iface, DPID from, DPID to,
        DWORD flags, void *data, DWORD size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_Send( &This->IDirectPlay4A_iface, from, to, flags, data, size );
}

static HRESULT WINAPI IDirectPlay2Impl_Send( IDirectPlay2 *iface, DPID from, DPID to,
        DWORD flags, void *data, DWORD size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_Send( &This->IDirectPlay4_iface, from, to, flags, data, size );
}

static HRESULT WINAPI IDirectPlay3AImpl_Send( IDirectPlay3A *iface, DPID from, DPID to,
        DWORD flags, void *data, DWORD size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_Send( &This->IDirectPlay4A_iface, from, to, flags, data, size );
}

static HRESULT WINAPI IDirectPlay3Impl_Send( IDirectPlay3 *iface, DPID from, DPID to,
        DWORD flags, void *data, DWORD size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_Send( &This->IDirectPlay4_iface, from, to, flags, data, size );
}

static HRESULT DP_SendExCore( IDirectPlay4 *iface, DPID from, DPID to, DWORD flags, void *data,
        DWORD size, DWORD priority, DWORD timeout, void *context, DWORD *msgid );

/* Send: SendEx's way out, but without a message id (Windows' next SendEx id shows none was taken).
 * DPSEND_ASYNC still means SendEx's asynchronous send [UNV whether Send takes it]. */
static HRESULT WINAPI IDirectPlay4AImpl_Send( IDirectPlay4A *iface, DPID from, DPID to,
        DWORD flags, void *data, DWORD size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_Send( &This->IDirectPlay4_iface, from, to, flags, data, size );
}

static HRESULT WINAPI IDirectPlay4Impl_Send( IDirectPlay4 *iface, DPID from, DPID to,
        DWORD flags, void *data, DWORD size )
{
    if ( flags & DPSEND_ASYNC )
        return IDirectPlayX_SendEx( iface, from, to, flags, data, size, 0, 0, NULL, NULL );
    return DP_SendExCore( iface, from, to, flags, data, size, 0, 0, NULL, NULL );
}

static HRESULT WINAPI IDirectPlay2AImpl_SetGroupData( IDirectPlay2A *iface, DPID group, void *data,
        DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_SetGroupData( &This->IDirectPlay4A_iface, group, data, size, flags );
}

static HRESULT WINAPI IDirectPlay2Impl_SetGroupData( IDirectPlay2 *iface, DPID group, void *data,
        DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_SetGroupData( &This->IDirectPlay4_iface, group, data, size, flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_SetGroupData( IDirectPlay3A *iface, DPID group, void *data,
        DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_SetGroupData( &This->IDirectPlay4A_iface, group, data, size, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_SetGroupData( IDirectPlay3 *iface, DPID group, void *data,
        DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_SetGroupData( &This->IDirectPlay4_iface, group, data, size, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_SetGroupData( IDirectPlay4A *iface, DPID group, void *data,
        DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_SetGroupData( &This->IDirectPlay4_iface, group, data, size, flags );
}

static HRESULT WINAPI IDirectPlay4Impl_SetGroupData( IDirectPlay4 *iface, DPID group, void *data,
        DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    lpGroupData gdata;

    TRACE( "(%p)->(0x%08lx,%p,0x%08lx,0x%08lx)\n", This, group, data, size, flags );

    /* Parameter check */
    if ( !data && size )
        return DPERR_INVALIDPARAMS;

    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;

    EnterCriticalSection( &This->lock );

    /* Find the pointer to the data for this group */
    if ( ( gdata = DP_FindGameGroup( This, group ) ) == NULL )
    {
        LeaveCriticalSection( &This->lock );
        return DPERR_INVALIDGROUP;   /* as Windows answers (apidiff) */
    }

    if ( !(flags & DPSET_LOCAL) )
    {
        FIXME( "Was this group created by this interface?\n" );
        /* FIXME: If this is a remote update need to allow it but not
         *        send a message.
         */
    }

    DP_SetGroupData( gdata, flags, data, size );

    /* remote data: every player on this machine hears of it, as of a player's (Windows, black box
     * 2026-10-22; guaranteed or not). [UNV] other machines: groups don't travel between machines
     * here so far */
    if ( !(flags & DPSET_LOCAL) && !DP_DataMessagesOff( This ) )
    {
        DPMSG_SETPLAYERORGROUPDATA msg;
        memset( &msg, 0, sizeof(msg) );
        msg.dwType = DPSYS_SETPLAYERORGROUPDATA;
        msg.dwPlayerType = DPPLAYERTYPE_GROUP;
        msg.dpId = group;
        msg.lpData = gdata->lpRemoteData;
        msg.dwDataSize = gdata->dwRemoteDataSize;
        DP_QueueMessage( This, DPID_SYSMSG, DPID_ALLPLAYERS, 0, &msg, DP_CopySetPlayerOrGroupData, 0 );
    }
    if ( !(flags & DPSET_LOCAL) )   /* and every machine: GROUPDATACHANGED (Windows, S3 tap 2026-10-28) */
        DP_GRP_SendData( This, group, gdata->lpRemoteData, gdata->dwRemoteDataSize );

    LeaveCriticalSection( &This->lock );

    return DP_OK;
}

static HRESULT DP_IF_SetGroupName( IDirectPlayImpl *This, DPID idGroup, DPNAME *lpGroupName,
        DWORD dwFlags, BOOL bAnsi )
{
  lpGroupData lpGData;
  DPNAME *nameA;
  DPNAME *name;

  TRACE( "(%p)->(0x%08lx,%p,0x%08lx,%u)\n", This, idGroup,
         lpGroupName, dwFlags, bAnsi );

  EnterCriticalSection( &This->lock );

  if( ( lpGData = DP_FindGameGroup( This, idGroup ) ) == NULL )
  {
    LeaveCriticalSection( &This->lock );
    return DPERR_INVALIDGROUP;
  }

  name = DP_DuplicateName( lpGroupName, FALSE, bAnsi, FALSE );
  if ( !name )
  {
    LeaveCriticalSection( &This->lock );
    return DPERR_OUTOFMEMORY;
  }

  nameA = DP_DuplicateName( lpGroupName, TRUE, bAnsi, FALSE );
  if ( !nameA )
  {
    free( name );
    LeaveCriticalSection( &This->lock );
    return DPERR_OUTOFMEMORY;
  }

  free( lpGData->name );
  free( lpGData->nameA );
  lpGData->name = name;
  lpGData->nameA = nameA;

  /* every player on this machine hears of it (Windows, apistate: SetGroupName in state open).
   * [UNV] other machines: not sent yet - groups don't travel between machines here so far */
  if( !DP_DataMessagesOff( This ) )
  {
    DPMSG_SETPLAYERORGROUPNAME msg;
    memset( &msg, 0, sizeof(msg) );
    msg.dwType = DPSYS_SETPLAYERORGROUPNAME;
    msg.dwPlayerType = DPPLAYERTYPE_GROUP;
    msg.dpId = idGroup;
    msg.dpnName = *name;
    DP_QueueMessage( This, DPID_SYSMSG, DPID_ALLPLAYERS, 0, &msg, DP_CopySetPlayerOrGroupName, 0 );
  }
  if( !(dwFlags & DPSET_LOCAL) )   /* and every machine: GROUPNAMECHANGED (Windows, S3 tap 2026-10-28) */
    DP_GRP_SendName( This, idGroup, name );

  LeaveCriticalSection( &This->lock );

  return DP_OK;
}

static HRESULT WINAPI IDirectPlay2AImpl_SetGroupName( IDirectPlay2A *iface, DPID group,
        DPNAME *name, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_SetGroupName( &This->IDirectPlay4A_iface, group, name, flags );
}

static HRESULT WINAPI IDirectPlay2Impl_SetGroupName( IDirectPlay2 *iface, DPID group,
        DPNAME *name, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_SetGroupName( &This->IDirectPlay4_iface, group, name, flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_SetGroupName( IDirectPlay3A *iface, DPID group,
        DPNAME *name, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_SetGroupName( &This->IDirectPlay4A_iface, group, name, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_SetGroupName( IDirectPlay3 *iface, DPID group,
        DPNAME *name, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_SetGroupName( &This->IDirectPlay4_iface, group, name, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_SetGroupName( IDirectPlay4A *iface, DPID idGroup,
        DPNAME *lpGroupName, DWORD dwFlags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;
    return DP_IF_SetGroupName( This, idGroup, lpGroupName, dwFlags, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_SetGroupName( IDirectPlay4 *iface, DPID idGroup,
        DPNAME *lpGroupName, DWORD dwFlags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;
    return DP_IF_SetGroupName( This, idGroup, lpGroupName, dwFlags, FALSE );
}

static HRESULT WINAPI IDirectPlay2AImpl_SetPlayerData( IDirectPlay2A *iface, DPID player,
        void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_SetPlayerData( &This->IDirectPlay4A_iface, player, data, size, flags );
}

static HRESULT WINAPI IDirectPlay2Impl_SetPlayerData( IDirectPlay2 *iface, DPID player,
        void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_SetPlayerData( &This->IDirectPlay4_iface, player, data, size, flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_SetPlayerData( IDirectPlay3A *iface, DPID player,
        void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_SetPlayerData( &This->IDirectPlay4A_iface, player, data, size, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_SetPlayerData( IDirectPlay3 *iface, DPID player,
        void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_SetPlayerData( &This->IDirectPlay4_iface, player, data, size, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_SetPlayerData( IDirectPlay4A *iface, DPID player,
        void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_SetPlayerData( &This->IDirectPlay4_iface, player, data, size, flags );
}

static HRESULT WINAPI IDirectPlay4Impl_SetPlayerData( IDirectPlay4 *iface, DPID player,
        void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    lpPlayerList plist;

    TRACE( "(%p)->(0x%08lx,%p,0x%08lx,0x%08lx)\n", This, player, data, size, flags );

    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;

    /* Parameter check */
    if ( !data && size )
        return DPERR_INVALIDPARAMS;

    EnterCriticalSection( &This->lock );

    /* Find the pointer to the data for this player */
    if ( (plist = DP_FindPlayer( This, player )) == NULL )
    {
        LeaveCriticalSection( &This->lock );
        return DPERR_INVALIDPLAYER;
    }

    if ( !(flags & DPSET_LOCAL) )
    {
        FIXME( "Was this group created by this interface?\n" );
        /* FIXME: If this is a remote update need to allow it but not
         *        send a message.
         */
    }

    DP_SetPlayerData( plist->lpPData, flags, data, size );

    if ( !(flags & DPSET_LOCAL) )
        DP_LOBBY_PlayerDataChanged( This, player );

    LeaveCriticalSection( &This->lock );

    return DP_OK;
}

static HRESULT DP_IF_SetPlayerName( IDirectPlayImpl *This, DPID idPlayer, DPNAME *lpPlayerName,
        DWORD dwFlags, BOOL bAnsi )
{
  lpPlayerList lpPList;
  DPNAME *nameA;
  DPNAME *name;

  TRACE( "(%p)->(0x%08lx,%p,0x%08lx,%u)\n",
         This, idPlayer, lpPlayerName, dwFlags, bAnsi );

  if( This->dp2->connectionInitialized == NO_PROVIDER )
  {
    return DPERR_UNINITIALIZED;
  }

  EnterCriticalSection( &This->lock );

  if( ( lpPList = DP_FindPlayer( This, idPlayer ) ) == NULL )
  {
    LeaveCriticalSection( &This->lock );
    return DPERR_INVALIDPLAYER;
  }

  name = DP_DuplicateName( lpPlayerName, FALSE, bAnsi, FALSE );
  if ( !name )
  {
    LeaveCriticalSection( &This->lock );
    return DPERR_OUTOFMEMORY;
  }

  nameA = DP_DuplicateName( lpPlayerName, TRUE, bAnsi, FALSE );
  if ( !nameA )
  {
    free( name );
    LeaveCriticalSection( &This->lock );
    return DPERR_OUTOFMEMORY;
  }

  free( lpPList->lpPData->name );
  free( lpPList->lpPData->nameA );
  lpPList->lpPData->name = name;
  lpPList->lpPData->nameA = nameA;

  DP_LOBBY_PlayerNameChanged( This, idPlayer );

  LeaveCriticalSection( &This->lock );

  return DP_OK;
}

static HRESULT WINAPI IDirectPlay2AImpl_SetPlayerName( IDirectPlay2A *iface, DPID player,
        DPNAME *name, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_SetPlayerName( &This->IDirectPlay4A_iface, player, name, flags );
}

static HRESULT WINAPI IDirectPlay2Impl_SetPlayerName( IDirectPlay2 *iface, DPID player,
        DPNAME *name, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_SetPlayerName( &This->IDirectPlay4_iface, player, name, flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_SetPlayerName( IDirectPlay3A *iface, DPID player,
        DPNAME *name, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_SetPlayerName( &This->IDirectPlay4A_iface, player, name, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_SetPlayerName( IDirectPlay3 *iface, DPID player,
        DPNAME *name, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_SetPlayerName( &This->IDirectPlay4_iface, player, name, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_SetPlayerName( IDirectPlay4A *iface, DPID idPlayer,
        DPNAME *lpPlayerName, DWORD dwFlags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return DP_IF_SetPlayerName( This, idPlayer, lpPlayerName, dwFlags, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_SetPlayerName( IDirectPlay4 *iface, DPID idPlayer,
        DPNAME *lpPlayerName, DWORD dwFlags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    return DP_IF_SetPlayerName( This, idPlayer, lpPlayerName, dwFlags, FALSE );
}

/* SetSessionDesc and the session flags, as Windows' dplayx takes them (black box, 2026-10-22):
 * bits 0-17 are accepted (any other: DPERR_INVALIDFLAGS); these are fixed once the session exists
 * (a change is DPERR_INVALIDPARAMS) - the rest (new players/join disabled, no data messages,
 * private, no preserve order, 0x2, 0x10, 0x10000, 0x20000) can change */
#define DPSESSION_SETTABLEBITS 0x0003ffff
#define DPSESSION_FIXEDFLAGS   ( DPSESSION_MIGRATEHOST | DPSESSION_NOMESSAGEID | DPSESSION_KEEPALIVE | \
                                 DPSESSION_SECURESERVER | DPSESSION_MULTICASTSERVER | \
                                 DPSESSION_CLIENTSERVER | DPSESSION_DIRECTPLAYPROTOCOL | \
                                 DPSESSION_OPTIMIZELATENCY )
#define DPSESSION_STICKYBIT    0x00010000

static HRESULT DP_SetSessionDesc( IDirectPlayImpl *This, const DPSESSIONDESC2 *lpSessDesc,
        DWORD dwFlags, BOOL bInitial, BOOL bAnsi  )
{
  LPDPSESSIONDESC2 lpTempSessDesc;

  TRACE( "(%p)->(%p,0x%08lx,%u,%u)\n",
         This, lpSessDesc, dwFlags, bInitial, bAnsi );

  if( This->dp2->connectionInitialized == NO_PROVIDER )
  {
    return DPERR_UNINITIALIZED;
  }

  if( !bInitial )
  {
    /* Windows' checks, in its order (black box, 2026-10-22): no session first, then the desc
     * itself (NULL or a wrong dwSize), flag bits it doesn't know (INVALIDFLAGS), the call's own
     * flags, and last a change to a flag that is fixed when the session is created */
    if( !This->dp2->bConnectionOpen || !This->dp2->lpSessionDesc )
      return DPERR_NOSESSIONS;
    if( !lpSessDesc || lpSessDesc->dwSize != sizeof( DPSESSIONDESC2 ) )
      return DPERR_INVALIDPARAMS;
    if( lpSessDesc->dwFlags & ~DPSESSION_SETTABLEBITS )
      return DPERR_INVALIDFLAGS;
    if( dwFlags )
      return DPERR_INVALIDPARAMS;
    if( (lpSessDesc->dwFlags ^ This->dp2->lpSessionDesc->dwFlags) & DPSESSION_FIXEDFLAGS )
      return DPERR_INVALIDPARAMS;
  }
  else if( dwFlags )
  {
    return DPERR_INVALIDPARAMS;
  }

  /* Only the host is allowed to update the session desc */
  if( !This->dp2->bHostInterface )
  {
    return DPERR_ACCESSDENIED;
  }

  EnterCriticalSection( &This->lock );

  /* Stored WCHAR, like Windows: a WCHAR caller's strings are kept byte for byte (AoE2 calls the
   * WCHAR interface with ANSI bytes and relies on them arriving unchanged - converting them turned
   * every name into "?"); an ANSI caller's are converted. GetSessionDesc converts per caller. */
  lpTempSessDesc = DP_DuplicateSessionDesc( lpSessDesc, FALSE, bAnsi );

  if( lpTempSessDesc == NULL )
  {
    LeaveCriticalSection( &This->lock );
    return DPERR_OUTOFMEMORY;
  }

  /* the application can't change these (Windows keeps its own player count). A new session starts
   * empty whatever the caller says: AoE2 hosts with dwCurrentPlayers = 1, which made every game
   * count one player too many (a solo host listed 2/8) */
  if( bInitial )
  {
    lpTempSessDesc->dwCurrentPlayers = 0;
    /* Windows gives every new session its own instance GUID (whatever the caller put there).
     * Without it every AoE2 game had GUID_NULL: joiners' session lists key on this GUID, so two
     * games at once overwrote each other (only one ever listed) and a join could reach the wrong
     * one. */
    CoCreateGuid( &lpTempSessDesc->guidInstance );
    /* and a random dwReserved1, which goes out in every session description and comes back in
     * each join request (S3 tap, 2026-10-16: Windows' host sends a fresh value per session, ours
     * sent the caller's 0) */
    {
      GUID r;
      CoCreateGuid( &r );
      lpTempSessDesc->dwReserved1 = r.Data1 ? r.Data1 : 0x2b5e3c71;
    }
  }
  else if( This->dp2->lpSessionDesc )
  {
    const DPSESSIONDESC2 *cur = This->dp2->lpSessionDesc;
    lpTempSessDesc->dwCurrentPlayers = cur->dwCurrentPlayers;
    lpTempSessDesc->guidInstance     = cur->guidInstance;
    lpTempSessDesc->guidApplication  = cur->guidApplication;
    /* as Windows keeps them (black box, 2026-10-22): the session key and dwReserved2 are the
     * session's own (the caller's are ignored - a 0 here sent every later session description
     * out keyless); "password required" follows the password (set when there is a non-empty
     * one, whatever the caller's flags say); and 0x10000, once set, stays */
    lpTempSessDesc->dwReserved1      = cur->dwReserved1;
    lpTempSessDesc->dwReserved2      = cur->dwReserved2;
    lpTempSessDesc->dwFlags = (lpTempSessDesc->dwFlags & ~DPSESSION_PASSWORDREQUIRED) |
                              (cur->dwFlags & DPSESSION_STICKYBIT);
    if( lpTempSessDesc->lpszPassword && lpTempSessDesc->lpszPassword[ 0 ] )
      lpTempSessDesc->dwFlags |= DPSESSION_PASSWORDREQUIRED;
  }

  /* no DPSYS_SETSESSIONDESC for our own players when the session had or now has
   * DPSESSION_NODATAMESSAGES (Windows: neither setting it nor clearing it is announced) */
  {
    BOOL quiet = ( (This->dp2->lpSessionDesc ? This->dp2->lpSessionDesc->dwFlags : 0) |
                   lpTempSessDesc->dwFlags ) & DPSESSION_NODATAMESSAGES;

    /* Free the old */
    free( This->dp2->lpSessionDesc );

    This->dp2->lpSessionDesc = lpTempSessDesc;

    /* If this is an external invocation of the interface, we should be
     * letting everyone know that things have changed. Otherwise this is
     * just an initialization and it doesn't need to be propagated.
     */
    if( !bInitial )
    {
      DP_LOBBY_SessionDescChangedEx( This, !quiet );   /* our players and every machine hear about it */
    }
  }

  LeaveCriticalSection( &This->lock );

  return DP_OK;
}

static HRESULT WINAPI IDirectPlay2AImpl_SetSessionDesc( IDirectPlay2A *iface, DPSESSIONDESC2 *sdesc,
        DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2A( iface );
    return IDirectPlayX_SetSessionDesc( &This->IDirectPlay4A_iface, sdesc, flags );
}

static HRESULT WINAPI IDirectPlay2Impl_SetSessionDesc( IDirectPlay2 *iface, DPSESSIONDESC2 *sdesc,
        DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay2( iface );
    return IDirectPlayX_SetSessionDesc( &This->IDirectPlay4_iface, sdesc, flags );
}

static HRESULT WINAPI IDirectPlay3AImpl_SetSessionDesc( IDirectPlay3A *iface, DPSESSIONDESC2 *sdesc,
        DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_SetSessionDesc( &This->IDirectPlay4A_iface, sdesc, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_SetSessionDesc( IDirectPlay3 *iface, DPSESSIONDESC2 *sdesc,
        DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_SetSessionDesc( &This->IDirectPlay4_iface, sdesc, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_SetSessionDesc( IDirectPlay4A *iface,
        DPSESSIONDESC2 *lpSessDesc, DWORD dwFlags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return DP_SetSessionDesc( This, lpSessDesc, dwFlags, FALSE, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_SetSessionDesc( IDirectPlay4 *iface,
        DPSESSIONDESC2 *lpSessDesc, DWORD dwFlags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    return DP_SetSessionDesc( This, lpSessDesc, dwFlags, FALSE, FALSE );
}

static DWORD DP_CopySessionDesc( LPDPSESSIONDESC2 lpSessionDest,
                                 LPCDPSESSIONDESC2 lpSessionSrc, BOOL dstAnsi, BOOL srcAnsi )
{
  DWORD offset = sizeof( DPSESSIONDESC2 );

  if( lpSessionDest )
    CopyMemory( lpSessionDest, lpSessionSrc, sizeof( *lpSessionSrc ) );

  offset += DP_CopyString( &lpSessionDest->lpszSessionNameA, lpSessionSrc->lpszSessionNameA,
                           dstAnsi, srcAnsi, lpSessionDest, offset );
  offset += DP_CopyString( &lpSessionDest->lpszPasswordA, lpSessionSrc->lpszPasswordA,
                           dstAnsi, srcAnsi, lpSessionDest, offset );

  return offset;
}

DPSESSIONDESC2 *DP_DuplicateSessionDesc( const DPSESSIONDESC2 *src, BOOL dstAnsi, BOOL srcAnsi )
{
    DPSESSIONDESC2 *dst;
    DWORD size;

    size = DP_CopySessionDesc( NULL, src, dstAnsi, srcAnsi );

    dst = malloc( size );
    if ( !dst )
        return NULL;

    DP_CopySessionDesc( dst, src, dstAnsi, srcAnsi );

    return dst;
}

static HRESULT WINAPI IDirectPlay3AImpl_AddGroupToGroup( IDirectPlay3A *iface, DPID parent,
        DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_AddGroupToGroup( &This->IDirectPlay4A_iface, parent, group );
}

static HRESULT WINAPI IDirectPlay3Impl_AddGroupToGroup( IDirectPlay3 *iface, DPID parent,
        DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_AddGroupToGroup( &This->IDirectPlay4_iface, parent, group );
}

static HRESULT WINAPI IDirectPlay4AImpl_AddGroupToGroup( IDirectPlay4A *iface, DPID parent,
        DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_AddGroupToGroup( &This->IDirectPlay4_iface, parent, group );
}

static HRESULT WINAPI IDirectPlay4Impl_AddGroupToGroup( IDirectPlay4 *iface, DPID parent,
        DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    lpGroupData gdata, gparent;
    lpGroupList glist;

    TRACE( "(%p)->(0x%08lx,0x%08lx)\n", This, parent, group );

    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;

    EnterCriticalSection( &This->lock );

    if ( ( gparent = DP_FindGameGroup(This, parent ) ) == NULL )
    {
        LeaveCriticalSection( &This->lock );
        return DPERR_INVALIDGROUP;
    }

    if ( ( gdata = DP_FindGameGroup(This, group ) ) == NULL )
    {
        LeaveCriticalSection( &This->lock );
        return DPERR_INVALIDGROUP;
    }

    /* a group in itself: Windows refuses it as an invalid group (black box, 2026-10-22) */
    if ( gdata == gparent )
    {
        LeaveCriticalSection( &This->lock );
        return DPERR_INVALIDGROUP;
    }

    /* already in there - its own child, or a shortcut added before: Windows answers DP_OK and
     * adds nothing (apidiff: AddGroupToGroup of a group created in that group) */
    DPQ_FIND_ENTRY( gparent->groups, groups, lpGData->dpid, ==, group, glist );
    if ( glist )
    {
        LeaveCriticalSection( &This->lock );
        return DP_OK;
    }

    /* Create a player list (ie "shortcut" ) */
    glist = calloc( 1, sizeof( *glist ) );
    if ( !glist )
    {
        LeaveCriticalSection( &This->lock );
        return DPERR_CANTADDPLAYER;
    }

    /* Add the shortcut */
    gdata->uRef++;
    glist->lpGData = gdata;

    /* into the parent's list (Wine put it in the group's own list: the group then held itself
     * and DeleteGroupFromGroup never found the shortcut) */
    DPQ_INSERT( gparent->groups, glist, groups );

    /* every player on this machine hears of it, as Windows sends it (black box, 2026-10-22) */
    {
        DPMSG_ADDGROUPTOGROUP msg;
        msg.dwType = DPSYS_ADDGROUPTOGROUP;
        msg.dpIdParentGroup = parent;
        msg.dpIdGroup = group;
        DP_QueueMessage( This, DPID_SYSMSG, DPID_ALLPLAYERS, 0, &msg, DP_CopyGeneric, sizeof( msg ) );
    }
    DP_GRP_SendIds( This, DPMSGCMD_ADDSHORTCUTTOGROUP, group, parent );   /* every machine (Windows) */

    LeaveCriticalSection( &This->lock );

    return DP_OK;
}

static HRESULT DP_IF_CreateGroupInGroup( IDirectPlayImpl *This, void *lpMsgHdr, DPID idParentGroup,
        DPID *lpidGroup, DPNAME *lpGroupName, void *lpData, DWORD dwDataSize, DWORD dwFlags,
        BOOL bAnsi )
{
  HRESULT hr;

  TRACE( "(%p)->(0x%08lx,%p,%p,%p,0x%08lx,0x%08lx,%u)\n",
         This, idParentGroup, lpidGroup, lpGroupName, lpData,
         dwDataSize, dwFlags, bAnsi );

  if( This->dp2->connectionInitialized == NO_PROVIDER )
  {
    return DPERR_UNINITIALIZED;
  }

  if( lpMsgHdr == NULL && (dwFlags & ~(DPGROUP_STAGINGAREA | DPGROUP_HIDDEN)) )
  {
    return DPERR_INVALIDFLAGS;   /* as CreateGroup */
  }

  if( lpMsgHdr == NULL && idParentGroup == DPID_SYSTEM_GROUP )
  {
    return DPERR_INVALIDGROUP;   /* group 0 is no game's group (Windows) - CreateGroup makes top-level ones */
  }

  if( FAILED( hr = DP_GroupIdFromHost( This, lpMsgHdr, lpidGroup ) ) )
    return hr;

  EnterCriticalSection( &This->lock );

  /* a group of its own id, as CreateGroup gives one (every group made in a group got
   * DPID_UNKNOWN: two of them were the same group to every later call) */
  if( DPID_UNKNOWN == *lpidGroup )
  {
    if( This->dp2->bHostInterface )
      *lpidGroup = DP_NextObjectId();
    else
      *lpidGroup = DP_GetRemoteNextObjectId();
  }

  hr = DP_CreateGroup(This, lpMsgHdr, lpidGroup, lpGroupName, lpData, dwDataSize, dwFlags,
                      idParentGroup, bAnsi );

  if( FAILED( hr ) )
  {
    LeaveCriticalSection( &This->lock );
    return hr;
  }

  if( lpMsgHdr == NULL )
  {
    lpGroupData g = DP_FindAnyGroup( This, *lpidGroup );
    DP_QueueGroupCreated( This, *lpidGroup, lpData, dwDataSize, dwFlags, idParentGroup );
    DP_GRP_SendCreate( This, *lpidGroup, idParentGroup, dwFlags, g ? g->name : NULL, lpData, dwDataSize );
  }

  /* Inform all other peers of the creation of a new group. If there are
   * no peers keep this quiet.
   */
  if( This->dp2->lpSessionDesc &&
      ( This->dp2->lpSessionDesc->dwFlags & DPSESSION_MULTICASTSERVER ) )
  {
    DPMSG_CREATEPLAYERORGROUP msg;

    msg.dwType = DPSYS_CREATEPLAYERORGROUP;
    msg.dwPlayerType = DPPLAYERTYPE_GROUP;
    msg.dpId = *lpidGroup;
    msg.dwCurrentPlayers = idParentGroup; /* FIXME: Incorrect? */
    msg.lpData = lpData;
    msg.dwDataSize = dwDataSize;
    msg.dpnName = *lpGroupName;

    /* FIXME: Correct to just use send effectively? */
    /* FIXME: Should size include data w/ message or just message "header" */
    /* FIXME: Check return code */
    IDirectPlayX_SendEx( &This->IDirectPlay4_iface, DPID_SERVERPLAYER, DPID_ALLPLAYERS, 0, &msg,
            sizeof( msg ), 0, 0, NULL, NULL );
  }

  LeaveCriticalSection( &This->lock );

  return DP_OK;
}

static HRESULT WINAPI IDirectPlay3AImpl_CreateGroupInGroup( IDirectPlay3A *iface, DPID parent,
        DPID *group, DPNAME *name, void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_CreateGroupInGroup( &This->IDirectPlay4A_iface, parent, group, name,
            data, size, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_CreateGroupInGroup( IDirectPlay3 *iface, DPID parent,
        DPID *group, DPNAME *name, void *data, DWORD size, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_CreateGroupInGroup( &This->IDirectPlay4_iface, parent, group, name,
            data, size, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_CreateGroupInGroup( IDirectPlay4A *iface,
        DPID idParentGroup, DPID *lpidGroup, DPNAME *lpGroupName, void *lpData, DWORD dwDataSize,
        DWORD dwFlags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );

    *lpidGroup = DPID_UNKNOWN;

    return DP_IF_CreateGroupInGroup( This, NULL, idParentGroup, lpidGroup, lpGroupName, lpData,
            dwDataSize, dwFlags, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_CreateGroupInGroup( IDirectPlay4 *iface, DPID idParentGroup,
        DPID *lpidGroup, DPNAME *lpGroupName, void *lpData, DWORD dwDataSize, DWORD dwFlags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );

    *lpidGroup = DPID_UNKNOWN;

    return DP_IF_CreateGroupInGroup( This, NULL, idParentGroup, lpidGroup, lpGroupName, lpData,
            dwDataSize, dwFlags, FALSE );
}

static HRESULT WINAPI IDirectPlay3AImpl_DeleteGroupFromGroup( IDirectPlay3A *iface, DPID parent,
        DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_DeleteGroupFromGroup( &This->IDirectPlay4A_iface, parent, group );
}

static HRESULT WINAPI IDirectPlay3Impl_DeleteGroupFromGroup( IDirectPlay3 *iface, DPID parent,
        DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_DeleteGroupFromGroup( &This->IDirectPlay4_iface, parent, group );
}

static HRESULT WINAPI IDirectPlay4AImpl_DeleteGroupFromGroup( IDirectPlay4A *iface, DPID parent,
        DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_DeleteGroupFromGroup( &This->IDirectPlay4_iface, parent, group );
}

static HRESULT WINAPI IDirectPlay4Impl_DeleteGroupFromGroup( IDirectPlay4 *iface, DPID parent,
        DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    lpGroupList glist;
    lpGroupData parentdata;

    TRACE("(%p)->(0x%08lx,0x%08lx)\n", This, parent, group );

    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;

    EnterCriticalSection( &This->lock );

    /* Is the parent group valid? */
    if ( ( parentdata = DP_FindAnyGroup(This, parent ) ) == NULL )
    {
        LeaveCriticalSection( &This->lock );
        return DPERR_INVALIDGROUP;
    }

    DPQ_FIND_ENTRY( parentdata->groups, groups, lpGData->dpid, ==, group, glist );
    if ( glist == NULL )
    {
        LeaveCriticalSection( &This->lock );
        return DPERR_INVALIDGROUP;
    }

    /* the group's own child (CreateGroupInGroup) is not a shortcut: Windows refuses to take it
     * out this way - DestroyGroup does that (apidiff) */
    if ( glist->lpGData->parent == parent )
    {
        LeaveCriticalSection( &This->lock );
        return DPERR_ACCESSDENIED;
    }

    /* Remove the group from the parent group queue */
    DPQ_REMOVE_ENTRY( parentdata->groups, groups, lpGData->dpid, ==, group, glist );

    /* Decrement the ref count */
    glist->lpGData->uRef--;

    /* Free up the list item */
    free( glist );

    /* every player on this machine hears of it (Windows, black box 2026-10-22) */
    {
        DPMSG_DELETEGROUPFROMGROUP msg;
        msg.dwType = DPSYS_DELETEGROUPFROMGROUP;
        msg.dpIdParentGroup = parent;
        msg.dpIdGroup = group;
        DP_QueueMessage( This, DPID_SYSMSG, DPID_ALLPLAYERS, 0, &msg, DP_CopyGeneric, sizeof( msg ) );
    }
    DP_GRP_SendIds( This, DPMSGCMD_DELETEGROUPFROMGROUP, group, parent );   /* every machine (Windows) */

    LeaveCriticalSection( &This->lock );

    return DP_OK;
}

/* Windows ends the address of every connection it lists with an empty element (GUID_NULL, size 0):
 * 80 bytes for a provider where the plain compound address is 60 (tests/dplayx.c EnumAddress_cb2
 * passes on Windows, 2026-10-14 conformance run S1). Append it and fix the total-size element. */
BOOL DP_AddAddressTerminator( void **address, DWORD *size )
{
    DPADDRESS *end;
    BYTE *p = realloc( *address, *size + sizeof(DPADDRESS) );

    if ( !p ) return FALSE;
    end = (DPADDRESS *)(p + *size);
    end->guidDataType = GUID_NULL;
    end->dwDataSize = 0;
    *size += sizeof(DPADDRESS);
    if ( IsEqualGUID( &((DPADDRESS *)p)->guidDataType, &DPAID_TotalSize ) )
        *(DWORD *)(p + sizeof(DPADDRESS)) = *size;
    *address = p;
    return TRUE;
}

static BOOL DP_BuildSPCompoundAddr( LPGUID lpcSpGuid, LPVOID* lplpAddrBuf,
                                    LPDWORD lpdwBufSize )
{
  DPCOMPOUNDADDRESSELEMENT dpCompoundAddress;
  HRESULT                  hr;

  dpCompoundAddress.dwDataSize = sizeof( GUID );
  dpCompoundAddress.guidDataType = DPAID_ServiceProvider;
  dpCompoundAddress.lpData = lpcSpGuid;

  *lplpAddrBuf = NULL;
  *lpdwBufSize = 0;

  hr = DPL_CreateCompoundAddress( &dpCompoundAddress, 1, *lplpAddrBuf,
                                  lpdwBufSize, TRUE );

  if( hr != DPERR_BUFFERTOOSMALL )
  {
    ERR( "can't get buffer size: %s\n", DPLAYX_HresultToString( hr ) );
    return FALSE;
  }

  /* Now allocate the buffer */
  *lplpAddrBuf = calloc( 1, *lpdwBufSize );

  hr = DPL_CreateCompoundAddress( &dpCompoundAddress, 1, *lplpAddrBuf,
                                  lpdwBufSize, TRUE );
  if( FAILED(hr) )
  {
    ERR( "can't create address: %s\n", DPLAYX_HresultToString( hr ) );
    free( *lplpAddrBuf );
    return FALSE;
  }

  return TRUE;
}

static HRESULT WINAPI IDirectPlay3AImpl_EnumConnections( IDirectPlay3A *iface,
        const GUID *application, LPDPENUMCONNECTIONSCALLBACK enumcb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_EnumConnections( &This->IDirectPlay4A_iface, application, enumcb, context,
            flags );
}

static HRESULT WINAPI IDirectPlay3Impl_EnumConnections( IDirectPlay3 *iface,
        const GUID *application, LPDPENUMCONNECTIONSCALLBACK enumcb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_EnumConnections( &This->IDirectPlay4_iface, application, enumcb, context,
            flags );
}

static BOOL DP_GetRegString( HKEY key, const void *name, const char *defaultValue, void **outValue,
                             BOOL ansi )
{
    DWORD size = 0;
    LSTATUS status;
    void *value;

    if ( ansi )
        status = RegGetValueA( key, NULL, name, RRF_RT_REG_SZ, NULL, NULL, &size );
    else
        status = RegGetValueW( key, NULL, name, RRF_RT_REG_SZ, NULL, NULL, &size );

    if ( status == ERROR_SUCCESS )
    {
        value = malloc( size );
        if ( !value )
            return FALSE;

        if ( ansi )
            status = RegGetValueA( key, NULL, name, RRF_RT_REG_SZ, NULL, value, &size );
        else
            status = RegGetValueW( key, NULL, name, RRF_RT_REG_SZ, NULL, value, &size );

        if ( status == ERROR_SUCCESS )
        {
            *outValue = value;
            return TRUE;
        }

        free( value );
    }

    value = DP_DuplicateString( defaultValue, ansi, TRUE );
    if ( !value )
        return FALSE;

    *outValue = value;
    return TRUE;
}

static BOOL DP_GetRegStringW( HKEY key, const WCHAR *name, const char *defaultValue, WCHAR **outValue )
{
    return DP_GetRegString( key, name, defaultValue, (void **) outValue, FALSE );
}

static BOOL DP_GetRegStringA( HKEY key, const char *name, const char *defaultValue, char **outValue )
{
    return DP_GetRegString( key, name, defaultValue, (void **) outValue, TRUE );
}

/* Windows 10 registers provider names as MUI references ("@%SystemRoot%\system32\dpwsockx.dll,-3000");
 * Windows' dplayx lists and passes the resolved text ("Internet TCP/IP Connection For DirectPlay",
 * SPInit lpszName in the S3 wire tap, 2026-10-14). RegLoadMUIStringW is Vista+, so it is looked
 * up at run time; Windows 9x registers plain names and keeps them. */
static void DP_ResolveMUIName( HKEY key, const WCHAR *valueName, WCHAR **nameW, char **nameA )
{
    typedef LONG (WINAPI *mui_t)( HKEY, LPCWSTR, LPWSTR, DWORD, LPDWORD, DWORD, LPCWSTR );
    static mui_t mui;
    static BOOL tried;
    WCHAR buf[ 256 ], *w;
    DWORD len = 0;
    char *a;
    int n;

    if ( !*nameW || (*nameW)[ 0 ] != '@' )
        return;
    if ( !tried )
    {
        HMODULE m = GetModuleHandleA( "advapi32.dll" );
        mui = m ? (mui_t)GetProcAddress( m, "RegLoadMUIStringW" ) : NULL;
        tried = TRUE;
    }
    if ( !mui || mui( key, valueName, buf, sizeof(buf), &len, 0, NULL ) != ERROR_SUCCESS )
        return;
    buf[ ARRAY_SIZE( buf ) - 1 ] = 0;
    n = WideCharToMultiByte( CP_ACP, 0, buf, -1, NULL, 0, NULL, NULL );
    if ( n <= 0 || !(a = malloc( n )) )
        return;
    if ( !(w = malloc( (lstrlenW( buf ) + 1) * sizeof(WCHAR) )) )
    {
        free( a );
        return;
    }
    WideCharToMultiByte( CP_ACP, 0, buf, -1, a, n, NULL, NULL );
    lstrcpyW( w, buf );
    free( *nameW );
    *nameW = w;
    free( *nameA );
    *nameA = a;
}

static BOOL DP_GetRegDword( HKEY key, const WCHAR *name, DWORD *value )
{
    DWORD size = sizeof( DWORD );
    WCHAR *buffer;

    if ( ERROR_SUCCESS == RegGetValueW( key, NULL, name, RRF_RT_DWORD, NULL, value, &size ) )
        return TRUE;

    buffer = malloc( size );
    if ( !buffer )
        return FALSE;

    if ( ERROR_SUCCESS != RegGetValueW( key, NULL, name, RRF_RT_REG_SZ, NULL, buffer, &size ) )
    {
        free( buffer );
        return FALSE;
    }

    if ( 1 != swscanf( buffer, L"%lu", value ) )
    {
        free( buffer );
        return FALSE;
    }

    free( buffer );

    return TRUE;
}

static void DP_ReadConnections( const char *searchSubKey, DWORD dwFlags,
                                const GUID *addressDataType, struct list *connections )
{
  HKEY hkResult;
  char subKeyName[51];
  DWORD dwIndex, sizeOfSubKeyName=50;
  FILETIME filetime;

  /* Need to loop over the service providers in the registry */
  if( RegOpenKeyExA( HKEY_LOCAL_MACHINE, searchSubKey,
                     0, KEY_READ, &hkResult ) != ERROR_SUCCESS )
  {
    /* Hmmm. Does this mean that there are no service providers? */
    ERR(": no service providers?\n");
    return;
  }


  /* Traverse all the service providers we have available */
  for( dwIndex=0;
       RegEnumKeyExA( hkResult, dwIndex, subKeyName, &sizeOfSubKeyName,
                      NULL, NULL, NULL, &filetime ) != ERROR_NO_MORE_ITEMS;
       ++dwIndex, sizeOfSubKeyName=51 )
  {

    HKEY     hkServiceProvider;
    WCHAR    *spGuid;
    HRESULT  hr;

    DPCOMPOUNDADDRESSELEMENT dpCompoundAddress;
    DPCONNECTION *connection;

    TRACE(" this time through: %s\n", subKeyName );

    /* Get a handle for this particular service provider */
    if( RegOpenKeyExA( hkResult, subKeyName, 0, KEY_READ,
                       &hkServiceProvider ) != ERROR_SUCCESS )
    {
      ERR(": what the heck is going on?\n" );
      continue;
    }

    connection = calloc( 1, sizeof( DPCONNECTION) );
    if( !connection )
    {
      RegCloseKey( hkServiceProvider );
      continue;
    }

    connection->flags = dwFlags;

    if( !DP_GetRegStringW( hkServiceProvider, L"Guid", NULL, &spGuid ) )
    {
      ERR(": missing GUID registry data members\n" );
      free( connection );
      RegCloseKey(hkServiceProvider);
      continue;
    }
    CLSIDFromString( spGuid, &connection->spGuid );
    free( spGuid );
#ifdef OKP_BUILTIN
    if ( DP_BUILTIN_IsOurs( &connection->spGuid ) )
    {
      /* registered by okplay's installer too: the built-in copy is already listed */
      free( connection );
      RegCloseKey( hkServiceProvider );
      continue;
    }
#endif

    /* Fill in the DPNAME struct for the service provider */

    connection->name.dwSize = sizeof( DPNAME );
    if( !DP_GetRegStringW( hkServiceProvider, L"DescriptionW", subKeyName,
                           &connection->name.lpszShortName ) )
    {
      free( connection );
      RegCloseKey( hkServiceProvider );
      continue;
    }

    connection->nameA.dwSize = sizeof( DPNAME );
    if( !DP_GetRegStringA( hkServiceProvider, "DescriptionA", subKeyName,
                           &connection->nameA.lpszShortNameA ) )
    {
      free( connection->name.lpszShortName );
      free( connection );
      RegCloseKey( hkServiceProvider );
      continue;
    }

    DP_ResolveMUIName( hkServiceProvider, L"DescriptionW", &connection->name.lpszShortName,
                       &connection->nameA.lpszShortNameA );

    if( !DP_GetRegDword( hkServiceProvider, L"dwReserved1", &connection->reserved1 ) )
    {
      free( connection->nameA.lpszShortNameA );
      free( connection->name.lpszShortName );
      free( connection );
      RegCloseKey( hkServiceProvider );
      continue;
    }

    if( !DP_GetRegDword( hkServiceProvider, L"dwReserved2", &connection->reserved2 ) )
    {
      free( connection->nameA.lpszShortNameA );
      free( connection->name.lpszShortName );
      free( connection );
      RegCloseKey( hkServiceProvider );
      continue;
    }

    if( !DP_GetRegStringA( hkServiceProvider, "Path", NULL, &connection->path ) )
    {
      free( connection->nameA.lpszShortNameA );
      free( connection->name.lpszShortName );
      free( connection );
      RegCloseKey( hkServiceProvider );
      continue;
    }

    RegCloseKey( hkServiceProvider );

    /* Create the compound address for the service provider.
     * NOTE: This is a gruesome architectural scar right now.  DP
     * uses DPL and DPL uses DP.  Nasty stuff. This may be why the
     * native dll just gets around this little bit by allocating an
     * 80 byte buffer which isn't even filled with a valid compound
     * address. Oh well. Creating a proper compound address is the
     * way to go anyways despite this method taking slightly more
     * heap space and realtime :) */

    dpCompoundAddress.guidDataType = *addressDataType;
    dpCompoundAddress.dwDataSize   = sizeof( GUID );
    dpCompoundAddress.lpData       = &connection->spGuid;

    if( ( hr = DPL_CreateCompoundAddress( &dpCompoundAddress, 1, connection->address,
                                          &connection->addressSize, TRUE ) ) != DPERR_BUFFERTOOSMALL )
    {
      ERR( "can't get buffer size: %s\n", DPLAYX_HresultToString( hr ) );
      free( connection->path );
      free( connection->nameA.lpszShortNameA );
      free( connection->name.lpszShortName );
      free( connection );
      continue;
    }

    /* Now allocate the buffer */
    connection->address = calloc( 1, connection->addressSize );

    if( ( hr = DPL_CreateCompoundAddress( &dpCompoundAddress, 1, connection->address,
                                          &connection->addressSize, TRUE ) ) != DP_OK )
    {
      ERR( "can't create address: %s\n", DPLAYX_HresultToString( hr ) );
      free( connection->address );
      free( connection->path );
      free( connection->nameA.lpszShortNameA );
      free( connection->name.lpszShortName );
      free( connection );
      continue;
    }

    DP_AddAddressTerminator( &connection->address, &connection->addressSize );
    list_add_tail( connections, &connection->entry );
  }
}

static BOOL connectionsInitialized;

static BOOL WINAPI DP_InitConnections( INIT_ONCE *initOnce, void *param, void **context )
{
    struct list *connections = param;

#ifdef OKP_BUILTIN
    /* the drop-in: our two connections first; Windows' own only if asked for */
    DP_BUILTIN_AddConnections( connections );
    if ( DP_BUILTIN_ShowWindowsConnections() )
#endif
    DP_ReadConnections( "SOFTWARE\\Microsoft\\DirectPlay\\Service Providers",
                        DPCONNECTION_DIRECTPLAY, &DPAID_ServiceProvider, connections );

    DP_ReadConnections( "SOFTWARE\\Microsoft\\DirectPlay\\Lobby Providers",
                        DPCONNECTION_DIRECTPLAYLOBBY, &DPAID_LobbyProvider, connections );

    connectionsInitialized = TRUE;

    return TRUE;
}

static struct list *DP_GetConnections(void)
{
    static struct list connections = LIST_INIT( connections );
    static INIT_ONCE initOnce = INIT_ONCE_STATIC_INIT;

    InitOnceExecuteOnce( &initOnce, DP_InitConnections, &connections, NULL );

    return &connections;
}

void DP_FreeConnections(void)
{
    struct list *connections;

    if ( !connectionsInitialized )
        return;

    connections = DP_GetConnections();

    while ( !list_empty( connections ) )
    {
        DPCONNECTION *connection = LIST_ENTRY( list_tail( connections ), DPCONNECTION, entry );

        list_remove( &connection->entry );
        free( connection->nameA.lpszShortNameA );
        free( connection->name.lpszShortName );
        free( connection->path );
        free( connection->address );
        free( connection );
    }
}

static HRESULT DP_IF_EnumConnections( IDirectPlayImpl *This,
        const GUID *lpguidApplication, LPDPENUMCONNECTIONSCALLBACK lpEnumCallback, void *lpContext,
        DWORD dwFlags, BOOL ansi )
{
  DPCONNECTION *connection;
  struct list *connections;

  TRACE("(%p)->(%p,%p,%p,0x%08lx)\n", This, lpguidApplication, lpEnumCallback, lpContext, dwFlags );

  /* A default dwFlags (0) is backwards compatible -- DPCONNECTION_DIRECTPLAY */
  if( dwFlags == 0 )
  {
    dwFlags = DPCONNECTION_DIRECTPLAY;
  }

  if( ! ( ( dwFlags & DPCONNECTION_DIRECTPLAY ) ||
          ( dwFlags & DPCONNECTION_DIRECTPLAYLOBBY ) )
    )
  {
    return DPERR_INVALIDFLAGS;
  }

  if( !lpEnumCallback )
  {
     return DPERR_INVALIDPARAMS;
  }

  connections = DP_GetConnections();

  LIST_FOR_EACH_ENTRY( connection, connections, DPCONNECTION, entry )
  {
    if ( !(connection->flags & dwFlags) )
      continue;

    if ( !lpEnumCallback( &connection->spGuid, connection->address, connection->addressSize,
                          ansi ? &connection->nameA : &connection->name, dwFlags, lpContext ) )
        break;
  }

  return DP_OK;
}

static HRESULT WINAPI IDirectPlay4AImpl_EnumConnections( IDirectPlay4A *iface,
        const GUID *lpguidApplication, LPDPENUMCONNECTIONSCALLBACK lpEnumCallback, void *lpContext,
        DWORD dwFlags )
{
  IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
  return DP_IF_EnumConnections( This, lpguidApplication, lpEnumCallback, lpContext, dwFlags, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_EnumConnections( IDirectPlay4 *iface,
        const GUID *application, LPDPENUMCONNECTIONSCALLBACK enumcb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    return DP_IF_EnumConnections( This, application, enumcb, context, flags, FALSE );
}

static HRESULT WINAPI IDirectPlay3AImpl_EnumGroupsInGroup( IDirectPlay3A *iface, DPID group,
        GUID *instance, LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_EnumGroupsInGroup( &This->IDirectPlay4A_iface, group, instance,
            enumplayercb, context, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_EnumGroupsInGroup( IDirectPlay3 *iface, DPID group,
        GUID *instance, LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_EnumGroupsInGroup( &This->IDirectPlay4_iface, group, instance,
            enumplayercb, context, flags );
}

static HRESULT DP_IF_EnumGroupsInGroup( IDirectPlayImpl *This, DPID group, GUID *instance,
        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags, BOOL ansi )
{
    lpGroupList glist;
    lpGroupData gdata;

    FIXME( "(%p)->(0x%08lx,%p,%p,%p,0x%08lx): semi stub\n", This, group, instance, enumplayercb,
            context, flags );

    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;

    EnterCriticalSection( &This->lock );

    if ( ( gdata = DP_FindAnyGroup(This, group ) ) == NULL )
    {
        LeaveCriticalSection( &This->lock );
        return DPERR_INVALIDGROUP;
    }

    for( glist = DPQ_FIRST( gdata->groups ); glist; glist = DPQ_NEXT( glist->groups ) )
    {
        DWORD groupFlags;

        if ( (glist->lpGData->dwFlags & flags) != (flags & ~DPENUMGROUPS_REMOTE) )
            continue;
        if ( (glist->lpGData->dwFlags & DPENUMGROUPS_LOCAL) && (flags & DPENUMGROUPS_REMOTE) )
            continue;

        groupFlags = glist->lpGData->dwFlags;
        groupFlags &= ~(DPENUMGROUPS_LOCAL | DPLAYI_GROUP_DPLAYOWNS);
        groupFlags |= flags;

        if ( !(*enumplayercb)( glist->lpGData->dpid, DPPLAYERTYPE_GROUP,
                    ansi ? glist->lpGData->nameA : glist->lpGData->name, groupFlags, context ) )
            break; /* User requested break */
    }

    LeaveCriticalSection( &This->lock );

    return DP_OK;
}

static HRESULT WINAPI IDirectPlay4AImpl_EnumGroupsInGroup( IDirectPlay4A *iface, DPID group,
        GUID *instance, LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    if ( group == DPID_SYSTEM_GROUP && This->dp2->connectionInitialized != NO_PROVIDER )
        return DPERR_INVALIDGROUP;   /* as EnumGroupPlayers: group 0 is no game's group */
    return DP_IF_EnumGroupsInGroup( This, group, instance, enumplayercb, context, flags, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_EnumGroupsInGroup( IDirectPlay4 *iface, DPID group,
        GUID *instance, LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    if ( group == DPID_SYSTEM_GROUP && This->dp2->connectionInitialized != NO_PROVIDER )
        return DPERR_INVALIDGROUP;
    return DP_IF_EnumGroupsInGroup( This, group, instance, enumplayercb, context, flags, FALSE );
}

static HRESULT WINAPI IDirectPlay3AImpl_GetGroupConnectionSettings( IDirectPlay3A *iface,
        DWORD flags, DPID group, void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_GetGroupConnectionSettings( &This->IDirectPlay4A_iface, flags, group,
            data, size );
}

static HRESULT WINAPI IDirectPlay3Impl_GetGroupConnectionSettings( IDirectPlay3 *iface,
        DWORD flags, DPID group, void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_GetGroupConnectionSettings( &This->IDirectPlay4_iface, flags, group,
            data, size );
}

/* Staging-area groups (lobby-launched sessions) are not implemented: as Windows' DirectPlay answers
 * with its own TCP/IP provider (apidiff.sh, 2026-10-04) - DPERR_UNINITIALIZED before a provider,
 * DPERR_UNSUPPORTED after - rather than DP_OK for something that did not happen. */
static HRESULT WINAPI IDirectPlay4AImpl_GetGroupConnectionSettings( IDirectPlay4A *iface,
        DWORD flags, DPID group, void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_GetGroupConnectionSettings( &This->IDirectPlay4_iface, flags, group, data, size );
}

static HRESULT WINAPI IDirectPlay4Impl_GetGroupConnectionSettings( IDirectPlay4 *iface, DWORD flags,
        DPID group, void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    TRACE( "(%p)->(0x%08lx,0x%08lx,%p,%p): not supported\n", This, flags, group, data, size );
    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;
    return DPERR_UNSUPPORTED;
}

static BOOL CALLBACK DP_GetSpLpGuidFromCompoundAddress(
    REFGUID         guidDataType,
    DWORD           dwDataSize,
    LPCVOID         lpData,
    LPVOID          lpContext )
{
  /* Looking for the GUID of the provider to load */
  if( ( IsEqualGUID( guidDataType, &DPAID_ServiceProvider ) ) ||
      ( IsEqualGUID( guidDataType, &DPAID_LobbyProvider ) )
    )
  {
    TRACE( "Found SP/LP (%s) %s (data size = 0x%08lx)\n",
           debugstr_guid( guidDataType ), debugstr_guid( lpData ), dwDataSize );

    if( dwDataSize != sizeof( GUID ) )
    {
      ERR( "Invalid sp/lp guid size 0x%08lx\n", dwDataSize );
    }

    memcpy( lpContext, lpData, dwDataSize );

    /* There shouldn't be more than 1 GUID/compound address */
    return FALSE;
  }

  /* Still waiting for what we want */
  return TRUE;
}


/* Find and perform a LoadLibrary on the requested SP or LP GUID */
static HMODULE DP_LoadSP( LPCGUID lpcGuid, LPSPINITDATA lpSpData, LPBOOL lpbIsDpSp )
{
  DPCONNECTION *connection;
  struct list *connections;

  TRACE( " request to load %s\n", debugstr_guid( lpcGuid ) );

  connections = DP_GetConnections();

  LIST_FOR_EACH_ENTRY( connection, connections, DPCONNECTION, entry )
  {
    /* Determine if this is the Service Provider that the user asked for */
    if( !IsEqualGUID( &connection->spGuid, lpcGuid ) )
    {
      continue;
    }

    if( connection->flags & DPCONNECTION_DIRECTPLAY )
    {
      lpSpData->lpszName = connection->name.lpszShortName;
      lpSpData->dwReserved1 = connection->reserved1;
      lpSpData->dwReserved2 = connection->reserved2;
    }

    *lpbIsDpSp = !!(connection->flags & DPCONNECTION_DIRECTPLAY);

    TRACE( "Loading %s\n", connection->path );
    return LoadLibraryA( connection->path );
  }

  return 0;
}

static HRESULT DP_InitializeDPSP( IDirectPlayImpl *This, HMODULE hServiceProvider )
{
  HRESULT hr;
  LPDPSP_SPINIT SPInit;

  /* Initialize the service provider by calling SPInit */
  SPInit = (LPDPSP_SPINIT)GetProcAddress( hServiceProvider, "SPInit" );

  if( SPInit == NULL )
  {
    ERR( "Service provider doesn't provide SPInit interface?\n" );
    FreeLibrary( hServiceProvider );
    return DPERR_UNAVAILABLE;
  }

  TRACE( "Calling SPInit (DP SP entry point)\n" );

  hr = (*SPInit)( &This->dp2->spData );

  if( FAILED(hr) )
  {
    ERR( "DP SP Initialization failed: %s\n", DPLAYX_HresultToString(hr) );
    FreeLibrary( hServiceProvider );
    return hr;
  }

  /* FIXME: Need to verify the sanity of the returned callback table
   *        using IsBadCodePtr */
  This->dp2->bSPInitialized = TRUE;

  /* This interface is now initialized as a DP object */
  This->dp2->connectionInitialized = DP_SERVICE_PROVIDER;

  /* Store the handle of the module so that we can unload it later */
  This->dp2->hServiceProvider = hServiceProvider;

  return hr;
}

static HRESULT DP_InitializeDPLSP( IDirectPlayImpl *This, HMODULE hLobbyProvider )
{
  HRESULT hr;
  LPSP_INIT DPLSPInit;

  /* Initialize the service provider by calling SPInit */
  DPLSPInit = (LPSP_INIT)GetProcAddress( hLobbyProvider, "DPLSPInit" );

  if( DPLSPInit == NULL )
  {
    ERR( "Service provider doesn't provide DPLSPInit interface?\n" );
    FreeLibrary( hLobbyProvider );
    return DPERR_UNAVAILABLE;
  }

  TRACE( "Calling DPLSPInit (DPL SP entry point)\n" );

  hr = (*DPLSPInit)( &This->dp2->dplspData );

  if( FAILED(hr) )
  {
    ERR( "DPL SP Initialization failed: %s\n", DPLAYX_HresultToString(hr) );
    FreeLibrary( hLobbyProvider );
    return hr;
  }

  /* FIXME: Need to verify the sanity of the returned callback table
   *        using IsBadCodePtr */

  This->dp2->bDPLSPInitialized = TRUE;

  /* This interface is now initialized as a lobby object */
  This->dp2->connectionInitialized = DP_LOBBY_PROVIDER;

  /* Store the handle of the module so that we can unload it later */
  This->dp2->hDPLobbyProvider = hLobbyProvider;

  return hr;
}

static HRESULT WINAPI IDirectPlay3AImpl_InitializeConnection( IDirectPlay3A *iface,
        void *connection, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_InitializeConnection( &This->IDirectPlay4A_iface, connection, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_InitializeConnection( IDirectPlay3 *iface,
        void *connection, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_InitializeConnection( &This->IDirectPlay4_iface, connection, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_InitializeConnection( IDirectPlay4A *iface,
        void *connection, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_InitializeConnection( &This->IDirectPlay4_iface, connection, flags );
}

/* Windows' dplayx asks the provider for its plain and its guaranteed capabilities after SPInit and
 * after Open (S3 wire tap, 2026-10-14: GetCaps(0) and GetCaps(DPGETCAPS_GUARANTEED) at both
 * points); Wine asked only the plain ones, later. Ask both, the same way. */
void DP_QuerySPCaps( IDirectPlayImpl *This )
{
    DPSP_GETCAPSDATA data;
    DPCAPS caps;
    DWORD flags[ 2 ] = { 0, DPGETCAPS_GUARANTEED };
    int i;

    if ( !This->dp2->spData.lpCB->GetCaps )
        return;
    for ( i = 0; i < 2; i++ )
    {
        memset( &caps, 0, sizeof(caps) );
        caps.dwSize = sizeof(caps);
        data.idPlayer = 0;
        data.lpCaps = &caps;
        data.dwFlags = flags[ i ];
        data.lpISP = This->dp2->spData.lpISP;
        This->dp2->spData.lpCB->GetCaps( &data );
    }
}

static HRESULT WINAPI IDirectPlay4Impl_InitializeConnection( IDirectPlay4 *iface,
        void *connection, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    HMODULE servprov;
    GUID sp;
    DWORD size = 80; /* when the address has no total-size element (see below) */
    BOOL is_dp_sp; /* TRUE if Direct Play SP, FALSE if Direct Play Lobby SP */
    HRESULT hr;
#ifdef OKP_BUILTIN
    void *mapped = NULL;
#endif

    TRACE( "(%p)->(%p,0x%08lx)\n", This, connection, flags );

    if ( !connection )
        return DPERR_INVALIDPARAMS;

    if ( flags )
        return DPERR_INVALIDFLAGS;

    if ( This->dp2->connectionInitialized != NO_PROVIDER )
        return DPERR_ALREADYINITIALIZED;

    /* The whole address goes to the provider, as Windows does: its total-size element says how
     * long it is. A fixed 80 bytes cut off the DPAID_INet host name a game adds to the TCP/IP
     * address (90 bytes for "127.0.0.1"), and Windows' TCP/IP provider then handled the session
     * differently and refused our joins (S3 wire tap, 2026-10-14). */
    {
        const DPADDRESS *first = connection;
        if ( IsEqualGUID( &first->guidDataType, &DPAID_TotalSize ) && first->dwDataSize == sizeof(DWORD) )
        {
            DWORD total = *(const DWORD *)(first + 1);
            if ( total >= sizeof(DPADDRESS) + sizeof(DWORD) && total <= 65536 )
                size = total;
        }
    }

    /* Find out what the requested SP is and how large this buffer is */
    hr = DPL_EnumAddress( DP_GetSpLpGuidFromCompoundAddress, connection, size, &sp );

    if ( FAILED(hr) )
    {
        ERR( "Invalid compound address?\n" );
        return DPERR_UNAVAILABLE;
    }
#ifdef OKP_BUILTIN
    /* a classic provider the ini hands to ours (fixed connection lists): our address from here on */
    if ( DP_BUILTIN_Remap( &sp, &mapped, &size ) && mapped )
        connection = mapped;   /* kept for the object's life, as the caller's buffer would be */
#endif

    /* Load the service provider */
    servprov = DP_LoadSP( &sp, &This->dp2->spData, &is_dp_sp );

    if ( !servprov )
    {
        ERR( "Unable to load service provider %s\n", debugstr_guid(&sp) );
        return DPERR_UNAVAILABLE;
    }

    if ( is_dp_sp )
    {
         /* Fill in what we can of the Service Provider required information.
          * The rest was be done in DP_LoadSP
          */
         This->dp2->spData.lpAddress = connection;
         This->dp2->spData.dwAddressSize = size;
         This->dp2->spGuid = sp;
         This->dp2->spData.lpGuid = &This->dp2->spGuid;
#ifdef OKP_BUILTIN
         This->dp2->okpConnection = DP_BUILTIN_IsOurs( &sp );
#elif defined(OKWEB_DPLAY)
         This->dp2->okpConnection = TRUE;   /* the browser build's only connection: Oldkid.net Online Play */
#endif
         hr = DP_InitializeDPSP( This, servprov );
         if ( SUCCEEDED( hr ) )
             DP_QuerySPCaps( This );
    }
    else
    {
         This->dp2->dplspData.lpAddress = connection;
         hr = DP_InitializeDPLSP( This, servprov );
    }

    if ( FAILED(hr) )
        return hr;

    return DP_OK;
}

static HRESULT WINAPI IDirectPlay3AImpl_SecureOpen( IDirectPlay3A *iface,
        const DPSESSIONDESC2 *sdesc, DWORD flags, const DPSECURITYDESC *security,
        const DPCREDENTIALS *credentials )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_SecureOpen( &This->IDirectPlay4A_iface, sdesc, flags, security,
            credentials );
}

static HRESULT WINAPI IDirectPlay3Impl_SecureOpen( IDirectPlay3 *iface,
        const DPSESSIONDESC2 *sdesc, DWORD flags, const DPSECURITYDESC *security,
        const DPCREDENTIALS *credentials )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_SecureOpen( &This->IDirectPlay4_iface, sdesc, flags, security,
            credentials );
}

static HRESULT WINAPI IDirectPlay4AImpl_SecureOpen( IDirectPlay4A *iface,
        const DPSESSIONDESC2 *lpsd, DWORD dwFlags, const DPSECURITYDESC *lpSecurity,
        const DPCREDENTIALS *lpCredentials )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return DP_SecureOpen( This, lpsd, dwFlags, lpSecurity, lpCredentials, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_SecureOpen( IDirectPlay4 *iface,
        const DPSESSIONDESC2 *lpsd, DWORD dwFlags, const DPSECURITYDESC *lpSecurity,
        const DPCREDENTIALS *lpCredentials )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    return DP_SecureOpen( This, lpsd, dwFlags, lpSecurity, lpCredentials, FALSE );
}

static HRESULT WINAPI IDirectPlay3AImpl_SendChatMessage( IDirectPlay3A *iface, DPID from, DPID to,
        DWORD flags, DPCHAT *chatmsg )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_SendChatMessage( &This->IDirectPlay4A_iface, from, to, flags, chatmsg );
}

static HRESULT WINAPI IDirectPlay3Impl_SendChatMessage( IDirectPlay3 *iface, DPID from, DPID to,
        DWORD flags, DPCHAT *chatmsg )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_SendChatMessage( &This->IDirectPlay4_iface, from, to, flags, chatmsg );
}

static HRESULT WINAPI IDirectPlay4AImpl_SendChatMessage( IDirectPlay4A *iface, DPID from,
        DPID to, DWORD flags, DPCHAT *chatmsg )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return DP_LOBBY_SendChat( This, from, to, flags, chatmsg, TRUE );
}

static HRESULT WINAPI IDirectPlay4Impl_SendChatMessage( IDirectPlay4 *iface, DPID from, DPID to,
        DWORD flags, DPCHAT *chatmsg )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    return DP_LOBBY_SendChat( This, from, to, flags, chatmsg, FALSE );
}

static HRESULT WINAPI IDirectPlay3AImpl_SetGroupConnectionSettings( IDirectPlay3A *iface,
        DWORD flags, DPID group, DPLCONNECTION *connection )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_SetGroupConnectionSettings( &This->IDirectPlay4A_iface, flags, group,
            connection );
}

static HRESULT WINAPI IDirectPlay3Impl_SetGroupConnectionSettings( IDirectPlay3 *iface,
        DWORD flags, DPID group, DPLCONNECTION *connection )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_SetGroupConnectionSettings( &This->IDirectPlay4_iface, flags, group,
            connection );
}

static HRESULT WINAPI IDirectPlay4AImpl_SetGroupConnectionSettings( IDirectPlay4A *iface,
        DWORD flags, DPID group, DPLCONNECTION *connection )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_SetGroupConnectionSettings( &This->IDirectPlay4_iface, flags, group, connection );
}

static HRESULT WINAPI IDirectPlay4Impl_SetGroupConnectionSettings( IDirectPlay4 *iface, DWORD flags,
        DPID group, DPLCONNECTION *connection )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    TRACE( "(%p)->(0x%08lx,0x%08lx,%p): not supported\n", This, flags, group, connection );
    /* Windows checks the connection first: one without a session description is invalid */
    if ( !connection || connection->dwSize != sizeof(*connection) || !connection->lpSessionDesc )
        return DPERR_INVALIDPARAMS;
    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;
    return DPERR_UNSUPPORTED;
}

static HRESULT WINAPI IDirectPlay3AImpl_StartSession( IDirectPlay3A *iface, DWORD flags,
        DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_StartSession( &This->IDirectPlay4A_iface, flags, group );
}

static HRESULT WINAPI IDirectPlay3Impl_StartSession( IDirectPlay3 *iface, DWORD flags,
        DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_StartSession( &This->IDirectPlay4_iface, flags, group );
}

static HRESULT WINAPI IDirectPlay4AImpl_StartSession( IDirectPlay4A *iface, DWORD flags,
        DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_StartSession( &This->IDirectPlay4_iface, flags, group );
}

static HRESULT WINAPI IDirectPlay4Impl_StartSession( IDirectPlay4 *iface, DWORD flags, DPID group )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    TRACE( "(%p)->(0x%08lx,0x%08lx): not supported\n", This, flags, group );
    /* a DP_OK here would leave a game waiting for a DPSYS_STARTSESSION that never comes */
    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;
    return DPERR_UNSUPPORTED;
}

static HRESULT WINAPI IDirectPlay3AImpl_GetGroupFlags( IDirectPlay3A *iface, DPID group,
        DWORD *flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_GetGroupFlags( &This->IDirectPlay4A_iface, group, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_GetGroupFlags( IDirectPlay3 *iface, DPID group,
        DWORD *flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_GetGroupFlags( &This->IDirectPlay4_iface, group, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_GetGroupFlags( IDirectPlay4A *iface, DPID group,
        DWORD *flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_GetGroupFlags( &This->IDirectPlay4_iface, group, flags );
}

static HRESULT WINAPI IDirectPlay4Impl_GetGroupFlags( IDirectPlay4 *iface, DPID group,
        DWORD *flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    lpGroupData gdata;

    TRACE( "(%p)->(0x%08lx,%p)\n", This, group, flags );

    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;
    if ( !flags )
        return DPERR_INVALIDPARAMS;

    EnterCriticalSection( &This->lock );
    if ( !(gdata = DP_FindGameGroup( This, group )) )
    {
        LeaveCriticalSection( &This->lock );
        return DPERR_INVALIDGROUP;
    }
    *flags = DP_GROUP_FLAGS( gdata );
    LeaveCriticalSection( &This->lock );
    return DP_OK;
}

static HRESULT WINAPI IDirectPlay3AImpl_GetGroupParent( IDirectPlay3A *iface, DPID group,
        DPID *parent )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_GetGroupParent( &This->IDirectPlay4A_iface, group, parent );
}

static HRESULT WINAPI IDirectPlay3Impl_GetGroupParent( IDirectPlay3 *iface, DPID group,
        DPID *parent )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_GetGroupParent( &This->IDirectPlay4_iface, group, parent );
}

static HRESULT WINAPI IDirectPlay4AImpl_GetGroupParent( IDirectPlay4A *iface, DPID group,
        DPID *parent )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_GetGroupParent( &This->IDirectPlay4_iface, group, parent );
}

static HRESULT WINAPI IDirectPlay4Impl_GetGroupParent( IDirectPlay4 *iface, DPID group,
        DPID *parent )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    lpGroupData gdata;

    TRACE( "(%p)->(0x%08lx,%p)\n", This, group, parent );

    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;

    EnterCriticalSection( &This->lock );

    if ( ( gdata = DP_FindGameGroup( This, group ) ) == NULL )
    {
        LeaveCriticalSection( &This->lock );
        return DPERR_INVALIDGROUP;
    }

    *parent = gdata->dpid;

    LeaveCriticalSection( &This->lock );

    return DP_OK;
}

static HRESULT WINAPI IDirectPlay3AImpl_GetPlayerAccount( IDirectPlay3A *iface, DPID player,
        DWORD flags, void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_GetPlayerAccount( &This->IDirectPlay4A_iface, player, flags, data, size );
}

static HRESULT WINAPI IDirectPlay3Impl_GetPlayerAccount( IDirectPlay3 *iface, DPID player,
        DWORD flags, void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_GetPlayerAccount( &This->IDirectPlay4_iface, player, flags, data, size );
}

static HRESULT WINAPI IDirectPlay4AImpl_GetPlayerAccount( IDirectPlay4A *iface, DPID player,
        DWORD flags, void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_GetPlayerAccount( &This->IDirectPlay4_iface, player, flags, data, size );
}

static HRESULT WINAPI IDirectPlay4Impl_GetPlayerAccount( IDirectPlay4 *iface, DPID player,
        DWORD flags, void *data, DWORD *size )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    TRACE( "(%p)->(0x%08lx,0x%08lx,%p,%p): no secure sessions\n", This, player, flags, data, size );
    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;
    if ( !This->dp2->bConnectionOpen )
        return DPERR_NOSESSIONS;
    return DPERR_UNSUPPORTED;   /* account information exists only in secure (authenticated) sessions */
}

static HRESULT WINAPI IDirectPlay3AImpl_GetPlayerFlags( IDirectPlay3A *iface, DPID player,
        DWORD *flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3A( iface );
    return IDirectPlayX_GetPlayerFlags( &This->IDirectPlay4A_iface, player, flags );
}

static HRESULT WINAPI IDirectPlay3Impl_GetPlayerFlags( IDirectPlay3 *iface, DPID player,
        DWORD *flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay3( iface );
    return IDirectPlayX_GetPlayerFlags( &This->IDirectPlay4_iface, player, flags );
}

static HRESULT WINAPI IDirectPlay4AImpl_GetPlayerFlags( IDirectPlay4A *iface, DPID player,
        DWORD *flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_GetPlayerFlags( &This->IDirectPlay4_iface, player, flags );
}

static HRESULT WINAPI IDirectPlay4Impl_GetPlayerFlags( IDirectPlay4 *iface, DPID player,
        DWORD *flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    lpPlayerList plist;

    TRACE( "(%p)->(0x%08lx,%p)\n", This, player, flags );

    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;
    if ( !flags )
        return DPERR_INVALIDPARAMS;

    EnterCriticalSection( &This->lock );
    if ( !(plist = DP_FindPlayer( This, player )) )
    {
        LeaveCriticalSection( &This->lock );
        return DPERR_INVALIDPLAYER;
    }
    *flags = plist->lpPData->dwFlags & (DPPLAYER_SERVERPLAYER | DPPLAYER_SPECTATOR | DPPLAYER_LOCAL | DPPLAYER_OWNER);
    LeaveCriticalSection( &This->lock );
    return DP_OK;
}

static HRESULT WINAPI IDirectPlay4AImpl_GetGroupOwner( IDirectPlay4A *iface, DPID group,
        DPID *owner )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_GetGroupOwner( &This->IDirectPlay4_iface, group, owner );
}

static HRESULT WINAPI IDirectPlay4Impl_GetGroupOwner( IDirectPlay4 *iface, DPID group,
        DPID *owner )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    BOOL known;

    TRACE( "(%p)->(0x%08lx,%p): not supported\n", This, group, owner );
    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;
    EnterCriticalSection( &This->lock );
    known = DP_FindGameGroup( This, group ) != NULL;
    LeaveCriticalSection( &This->lock );
    /* group ownership is a lobby-session feature; Windows answers the same way */
    return known ? DPERR_UNSUPPORTED : DPERR_INVALIDGROUP;
}

static HRESULT WINAPI IDirectPlay4AImpl_SetGroupOwner( IDirectPlay4A *iface, DPID group,
        DPID owner )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_SetGroupOwner( &This->IDirectPlay4_iface, group, owner );
}

static HRESULT WINAPI IDirectPlay4Impl_SetGroupOwner( IDirectPlay4 *iface, DPID group ,
        DPID owner )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    BOOL known;

    TRACE( "(%p)->(0x%08lx,0x%08lx): not supported\n", This, group, owner );
    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;
    EnterCriticalSection( &This->lock );
    known = DP_FindGameGroup( This, group ) != NULL;
    LeaveCriticalSection( &This->lock );
    return known ? DPERR_UNSUPPORTED : DPERR_INVALIDGROUP;
}

static HRESULT WINAPI IDirectPlay4AImpl_SendEx( IDirectPlay4A *iface, DPID from, DPID to,
        DWORD flags, void *data, DWORD size, DWORD priority, DWORD timeout, void *context,
        DWORD *msgid )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_SendEx( &This->IDirectPlay4_iface, from, to, flags, data, size, priority,
            timeout, context, msgid );
}

/* does a send to this player/group reach another machine? */
static BOOL DP_HasRemoteRecipient( IDirectPlayImpl *This, DPID to )
{
    struct PlayerList *plist;
    struct GroupData *group;

    if ( (plist = DP_FindPlayer( This, to )) )
        return !(plist->lpPData->dwFlags & DPPLAYER_LOCAL);
    if ( (group = DP_FindAnyGroup( This, to )) )
        for ( plist = DPQ_FIRST( group->players ); plist; plist = DPQ_NEXT( plist->players ) )
            if ( !(plist->lpPData->dwFlags & DPPLAYER_LOCAL) )
                return TRUE;
    return FALSE;
}

/* Message ids, as Windows hands them out (black box, 2026-10-22): n << 16, one for every SendEx
 * that goes out - synchronous ones too - and none for a refused SendEx, a plain Send or a chat
 * message; the count goes on across Close and Open. */
static LONG dp_msgid_counter;

static HRESULT WINAPI IDirectPlay4Impl_SendEx( IDirectPlay4 *iface, DPID from, DPID to,
        DWORD flags, void *data, DWORD size, DWORD priority, DWORD timeout, void *context,
        DWORD *msgid )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    HRESULT hr;

    TRACE( "(%p)->(0x%08lx,0x%08lx,0x%08lx,%p,0x%08lx,0x%08lx,0x%08lx,%p,%p)\n",
            This, from, to, flags, data, size, priority, timeout, context, msgid );

    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;

    /* DPSEND_ASYNC, as Windows' dplayx answers it (dptest --async over Windows' TCP/IP provider,
     * 2026-10-14): a message id of n << 16, DPERR_PENDING when another machine is involved
     * (DP_OK when not), and one DPSYS_SENDCOMPLETE per send for the sending player with the
     * caller's context, the flags as given, hr DP_OK and dwSendTime 0. Done here: the send itself
     * goes to the provider synchronously. */
    if ( flags & DPSEND_ASYNC )
    {
        DPMSG_SENDCOMPLETE done;
        DWORD id;
        BOOL remote;

        EnterCriticalSection( &This->lock );
        remote = DP_HasRemoteRecipient( This, to );
        LeaveCriticalSection( &This->lock );
        hr = DP_SendExCore( iface, from, to, flags & ~DPSEND_ASYNC, data, size, priority, timeout,
                            NULL, NULL );
        if ( FAILED( hr ) )
            return hr;
        id = (DWORD)InterlockedIncrement( &dp_msgid_counter ) << 16;
        /* the id goes back only for a send still under way (Windows leaves *msgid as it was when
         * the send was done at once, to players on this machine only) */
        if ( msgid && remote ) *msgid = id;
        memset( &done, 0, sizeof(done) );
        done.dwType = DPSYS_SENDCOMPLETE;
        done.idFrom = from;
        done.idTo = to;
        done.dwFlags = flags;
        done.dwPriority = priority;
        done.dwTimeout = timeout;
        done.lpvContext = context;
        done.dwMsgID = id;
        done.hr = DP_OK;
        done.dwSendTime = 0;
        EnterCriticalSection( &This->lock );
        if ( DP_FindPlayer( This, from ) )
            DP_QueueMessage( This, DPID_SYSMSG, from, 0, &done, DP_CopyGeneric, sizeof(done) );
        LeaveCriticalSection( &This->lock );
        return remote ? DPERR_PENDING : DP_OK;
    }

    hr = DP_SendExCore( iface, from, to, flags, data, size, priority, timeout, context, msgid );
    if ( SUCCEEDED( hr ) )
        InterlockedIncrement( &dp_msgid_counter );
    return hr;
}

/* the send itself (Send, and SendEx without DPSEND_ASYNC) */
static HRESULT DP_SendExCore( IDirectPlay4 *iface, DPID from, DPID to,
        DWORD flags, void *data, DWORD size, DWORD priority, DWORD timeout, void *context,
        DWORD *msgid )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    DPMSG_SYSMSGENVELOPE envelope;
    SGBUFFER buffers[ 3 ] = { 0 };
    lpPlayerList playerTo;
    HRESULT hr;

    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;

    /* FIXME: Add parameter checking */
    /* FIXME: First call to this needs to acquire a message id which will be
     *        used for multiple sends
     */

    /* NOTE: Can't send messages to yourself - this will be trapped in receive */

    EnterCriticalSection( &This->lock );

    /* Verify that the message is being sent from a valid local player. The
     * from player may be anonymous DPID_UNKNOWN
     */
    if ( from != DPID_UNKNOWN && !DP_FindPlayer( This, from ) )
    {
        WARN( "INFO: Invalid from player 0x%08lx\n", from );
        LeaveCriticalSection( &This->lock );
        return DPERR_INVALIDPLAYER;
    }

    hr = DP_QueueMessage( This, from, to, from, data, DP_CopyGeneric, size );
    if ( FAILED( hr ) )
    {
        LeaveCriticalSection( &This->lock );
        return hr;
    }

    envelope.dwPlayerFrom = from;
    envelope.dwPlayerTo = to;

    buffers[ 0 ].len = This->dp2->spData.dwSPHeaderSize;
    buffers[ 0 ].pData = NULL;
    buffers[ 1 ].len = sizeof( envelope );
    buffers[ 1 ].pData = (UCHAR *)&envelope;
    buffers[ 2 ].len = size;
    buffers[ 2 ].pData = data;

    /* Verify that the message is being sent to a valid player, group or to
     * everyone. If it's valid, send it to those players.
     */
    if ( (playerTo = DP_FindPlayer( This, to )) )
    {
        /* a player on this machine already has the message (DP_QueueMessage above): the provider
         * knows no route to it and would fail the send - Windows answers DP_OK (apidiff) */
        if ( playerTo->lpPData->dwFlags & DPPLAYER_LOCAL )
        {
            LeaveCriticalSection( &This->lock );
            return DP_OK;
        }
        /* DP_SP_SendEx falls back to the SP's Send when it has no SendEx */
        {
            DPSP_SENDEXDATA sendData;

            sendData.lpISP = This->dp2->spData.lpISP;
            sendData.dwFlags = flags;
            sendData.idPlayerTo = to;
            sendData.idPlayerFrom = from;
            sendData.lpSendBuffers = ( flags & DPSEND_GUARANTEED ) ? buffers : &buffers[ 1 ];
            sendData.cBuffers = ( flags & DPSEND_GUARANTEED ) ? 3 : 2;
            sendData.dwMessageSize = DP_MSG_ComputeMessageSize( sendData.lpSendBuffers,
                                                                sendData.cBuffers );
            sendData.dwPriority = priority;
            sendData.dwTimeout = timeout;
            sendData.lpDPContext = context;
            sendData.lpdwSPMsgID = msgid;
            sendData.bSystemMessage = FALSE;

            hr = DP_SP_SendEx( This, &sendData );

            LeaveCriticalSection( &This->lock );

            return hr;
        }
    }
    else if ( DP_FindAnyGroup( This, to ) )
    {
        /* DP_SP_SendToGroupEx multicasts if the SP can, else sends to each remote machine */
        {
            DPSP_SENDTOGROUPEXDATA sendData;

            sendData.lpISP = This->dp2->spData.lpISP;
            sendData.dwFlags = flags;
            sendData.idGroupTo = to;
            sendData.idPlayerFrom = from;
            sendData.lpSendBuffers = ( flags & DPSEND_GUARANTEED ) ? buffers : &buffers[ 1 ];
            sendData.cBuffers = ( flags & DPSEND_GUARANTEED ) ? 3 : 2;
            sendData.dwMessageSize = DP_MSG_ComputeMessageSize( sendData.lpSendBuffers,
                                                                sendData.cBuffers );
            sendData.dwPriority = priority;
            sendData.dwTimeout = timeout;
            sendData.lpDPContext = context;
            sendData.lpdwSPMsgID = msgid;

            hr = DP_SP_SendToGroupEx( This, &sendData );

            LeaveCriticalSection( &This->lock );

            return hr;
        }

    }
    else
    {
        LeaveCriticalSection( &This->lock );
        return DPERR_INVALIDPLAYER;
    }

    LeaveCriticalSection( &This->lock );

    return DP_OK;
}

static HRESULT WINAPI IDirectPlay4AImpl_GetMessageQueue( IDirectPlay4A *iface, DPID from, DPID to,
        DWORD flags, DWORD *msgs, DWORD *bytes )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_GetMessageQueue( &This->IDirectPlay4_iface, from, to, flags, msgs, bytes );
}

static HRESULT WINAPI IDirectPlay4Impl_GetMessageQueue( IDirectPlay4 *iface, DPID from, DPID to,
        DWORD flags, DWORD *msgs, DWORD *bytes )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );
    struct PlayerList *playerFrom = NULL;
    struct PlayerList *playerTo = NULL;
    HRESULT hr = DP_OK;

    TRACE( "(%p)->(0x%08lx,0x%08lx,0x%08lx,%p,%p)\n", This, from, to, flags, msgs, bytes );

    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;

    if ( !flags )
        flags = DPMESSAGEQUEUE_SEND;

    if ( flags != DPMESSAGEQUEUE_SEND && flags != DPMESSAGEQUEUE_RECEIVE )
        return DPERR_INVALIDFLAGS;

    EnterCriticalSection( &This->lock );

    if ( to )
    {
        playerTo = DP_FindPlayer( This, to );
        if ( !playerTo )
        {
            LeaveCriticalSection( &This->lock );
            return DPERR_INVALIDPLAYER;
        }
    }

    if ( from )
    {
        playerFrom = DP_FindPlayer( This, from );
        if ( !playerFrom )
        {
            LeaveCriticalSection( &This->lock );
            return DPERR_INVALIDPLAYER;
        }
    }

    if ( flags & DPMESSAGEQUEUE_RECEIVE )
    {
        DWORD byteCount = 0;
        DWORD msgCount = 0;
        struct DPMSG *msg;

        if ( playerTo && !(playerTo->lpPData->dwFlags & DPPLAYER_LOCAL) )
        {
            LeaveCriticalSection( &This->lock );
            return DPERR_INVALIDPLAYER;
        }

        for ( msg = DPQ_FIRST( This->dp2->receiveMsgs ); msg; msg = DPQ_NEXT( msg->msgs ) )
        {
            if( from && msg->fromId != from )
                continue;
            if( to && msg->toId != to )
                continue;

            ++msgCount;
            byteCount += msg->copyMessage( NULL, msg->msg, msg->genericSize, FALSE );
        }

        if ( msgs )
            *msgs = msgCount;
        if ( bytes )
            *bytes = byteCount;

        LeaveCriticalSection( &This->lock );

        return DP_OK;
    }

    /* FIXME: Do we need to do from and to sanity checking here? */
    /* FIXME: What about sends which are not immediate? */

    if ( This->dp2->spData.lpCB->GetMessageQueue )
    {
        DPSP_GETMESSAGEQUEUEDATA data;

        FIXME( "Calling SP GetMessageQueue - is it right?\n" );

        /* FIXME: None of this is documented :( */
        data.lpISP        = This->dp2->spData.lpISP;
        data.dwFlags      = flags;
        data.idFrom       = from;
        data.idTo         = to;
        data.lpdwNumMsgs  = msgs;
        data.lpdwNumBytes = bytes;

        hr = (*This->dp2->spData.lpCB->GetMessageQueue)( &data );
    }
    else
        FIXME( "No SP for GetMessageQueue - fake some data\n" );

    LeaveCriticalSection( &This->lock );

    return hr;
}

static HRESULT dplay_cancelmsg ( IDirectPlayImpl* This, DWORD msgid, DWORD flags, DWORD minprio,
        DWORD maxprio )
{
    HRESULT hr = DP_OK;

    TRACE( "(%p)->(0x%08lx,0x%08lx)\n", This, msgid, flags );

    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;

    if ( This->dp2->spData.lpCB->Cancel )
    {
        DPSP_CANCELDATA data;

        TRACE( "Calling SP Cancel\n" );

        /* FIXME: Undocumented callback */

        data.lpISP          = This->dp2->spData.lpISP;
        data.dwFlags        = flags;
        data.lprglpvSPMsgID = NULL;
        data.cSPMsgID       = msgid;
        data.dwMinPriority  = minprio;
        data.dwMaxPriority  = maxprio;

        hr = (*This->dp2->spData.lpCB->Cancel)( &data );
    }
    else
    {
        /* no asynchronous sends to cancel with this provider: Windows' TCP/IP answers the same */
        TRACE( "the provider has no Cancel - not supported\n" );
        hr = DPERR_UNSUPPORTED;
    }

    return hr;
}

static HRESULT WINAPI IDirectPlay4AImpl_CancelMessage( IDirectPlay4A *iface, DWORD msgid,
        DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_CancelMessage( &This->IDirectPlay4_iface, msgid, flags );
}

static HRESULT WINAPI IDirectPlay4Impl_CancelMessage( IDirectPlay4 *iface, DWORD msgid,
        DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );

    if ( flags != 0 )
      return DPERR_INVALIDFLAGS;

    if ( msgid == 0 )
      flags |= DPCANCELSEND_ALL;

    return dplay_cancelmsg( This, msgid, flags, 0, 0 );
}

static HRESULT WINAPI IDirectPlay4AImpl_CancelPriority( IDirectPlay4A *iface, DWORD minprio,
        DWORD maxprio, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );
    return IDirectPlayX_CancelPriority( &This->IDirectPlay4_iface, minprio, maxprio, flags );
}

static HRESULT WINAPI IDirectPlay4Impl_CancelPriority( IDirectPlay4 *iface, DWORD minprio,
        DWORD maxprio, DWORD flags )
{
    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );

    if ( flags != 0 )
        return DPERR_INVALIDFLAGS;

    return dplay_cancelmsg( This, 0, DPCANCELSEND_PRIORITY, minprio, maxprio );
}

static const IDirectPlay2Vtbl dp2_vt =
{
    IDirectPlay2Impl_QueryInterface,
    IDirectPlay2Impl_AddRef,
    IDirectPlay2Impl_Release,
    IDirectPlay2Impl_AddPlayerToGroup,
    IDirectPlay2Impl_Close,
    IDirectPlay2Impl_CreateGroup,
    IDirectPlay2Impl_CreatePlayer,
    IDirectPlay2Impl_DeletePlayerFromGroup,
    IDirectPlay2Impl_DestroyGroup,
    IDirectPlay2Impl_DestroyPlayer,
    IDirectPlay2Impl_EnumGroupPlayers,
    IDirectPlay2Impl_EnumGroups,
    IDirectPlay2Impl_EnumPlayers,
    IDirectPlay2Impl_EnumSessions,
    IDirectPlay2Impl_GetCaps,
    IDirectPlay2Impl_GetGroupData,
    IDirectPlay2Impl_GetGroupName,
    IDirectPlay2Impl_GetMessageCount,
    IDirectPlay2Impl_GetPlayerAddress,
    IDirectPlay2Impl_GetPlayerCaps,
    IDirectPlay2Impl_GetPlayerData,
    IDirectPlay2Impl_GetPlayerName,
    IDirectPlay2Impl_GetSessionDesc,
    IDirectPlay2Impl_Initialize,
    IDirectPlay2Impl_Open,
    IDirectPlay2Impl_Receive,
    IDirectPlay2Impl_Send,
    IDirectPlay2Impl_SetGroupData,
    IDirectPlay2Impl_SetGroupName,
    IDirectPlay2Impl_SetPlayerData,
    IDirectPlay2Impl_SetPlayerName,
    IDirectPlay2Impl_SetSessionDesc
};

static const IDirectPlay2Vtbl dp2A_vt =
{
    IDirectPlay2AImpl_QueryInterface,
    IDirectPlay2AImpl_AddRef,
    IDirectPlay2AImpl_Release,
    IDirectPlay2AImpl_AddPlayerToGroup,
    IDirectPlay2AImpl_Close,
    IDirectPlay2AImpl_CreateGroup,
    IDirectPlay2AImpl_CreatePlayer,
    IDirectPlay2AImpl_DeletePlayerFromGroup,
    IDirectPlay2AImpl_DestroyGroup,
    IDirectPlay2AImpl_DestroyPlayer,
    IDirectPlay2AImpl_EnumGroupPlayers,
    IDirectPlay2AImpl_EnumGroups,
    IDirectPlay2AImpl_EnumPlayers,
    IDirectPlay2AImpl_EnumSessions,
    IDirectPlay2AImpl_GetCaps,
    IDirectPlay2AImpl_GetGroupData,
    IDirectPlay2AImpl_GetGroupName,
    IDirectPlay2AImpl_GetMessageCount,
    IDirectPlay2AImpl_GetPlayerAddress,
    IDirectPlay2AImpl_GetPlayerCaps,
    IDirectPlay2AImpl_GetPlayerData,
    IDirectPlay2AImpl_GetPlayerName,
    IDirectPlay2AImpl_GetSessionDesc,
    IDirectPlay2AImpl_Initialize,
    IDirectPlay2AImpl_Open,
    IDirectPlay2AImpl_Receive,
    IDirectPlay2AImpl_Send,
    IDirectPlay2AImpl_SetGroupData,
    IDirectPlay2AImpl_SetGroupName,
    IDirectPlay2AImpl_SetPlayerData,
    IDirectPlay2AImpl_SetPlayerName,
    IDirectPlay2AImpl_SetSessionDesc
};

static const IDirectPlay3Vtbl dp3_vt =
{
    IDirectPlay3Impl_QueryInterface,
    IDirectPlay3Impl_AddRef,
    IDirectPlay3Impl_Release,
    IDirectPlay3Impl_AddPlayerToGroup,
    IDirectPlay3Impl_Close,
    IDirectPlay3Impl_CreateGroup,
    IDirectPlay3Impl_CreatePlayer,
    IDirectPlay3Impl_DeletePlayerFromGroup,
    IDirectPlay3Impl_DestroyGroup,
    IDirectPlay3Impl_DestroyPlayer,
    IDirectPlay3Impl_EnumGroupPlayers,
    IDirectPlay3Impl_EnumGroups,
    IDirectPlay3Impl_EnumPlayers,
    IDirectPlay3Impl_EnumSessions,
    IDirectPlay3Impl_GetCaps,
    IDirectPlay3Impl_GetGroupData,
    IDirectPlay3Impl_GetGroupName,
    IDirectPlay3Impl_GetMessageCount,
    IDirectPlay3Impl_GetPlayerAddress,
    IDirectPlay3Impl_GetPlayerCaps,
    IDirectPlay3Impl_GetPlayerData,
    IDirectPlay3Impl_GetPlayerName,
    IDirectPlay3Impl_GetSessionDesc,
    IDirectPlay3Impl_Initialize,
    IDirectPlay3Impl_Open,
    IDirectPlay3Impl_Receive,
    IDirectPlay3Impl_Send,
    IDirectPlay3Impl_SetGroupData,
    IDirectPlay3Impl_SetGroupName,
    IDirectPlay3Impl_SetPlayerData,
    IDirectPlay3Impl_SetPlayerName,
    IDirectPlay3Impl_SetSessionDesc,
    IDirectPlay3Impl_AddGroupToGroup,
    IDirectPlay3Impl_CreateGroupInGroup,
    IDirectPlay3Impl_DeleteGroupFromGroup,
    IDirectPlay3Impl_EnumConnections,
    IDirectPlay3Impl_EnumGroupsInGroup,
    IDirectPlay3Impl_GetGroupConnectionSettings,
    IDirectPlay3Impl_InitializeConnection,
    IDirectPlay3Impl_SecureOpen,
    IDirectPlay3Impl_SendChatMessage,
    IDirectPlay3Impl_SetGroupConnectionSettings,
    IDirectPlay3Impl_StartSession,
    IDirectPlay3Impl_GetGroupFlags,
    IDirectPlay3Impl_GetGroupParent,
    IDirectPlay3Impl_GetPlayerAccount,
    IDirectPlay3Impl_GetPlayerFlags
};

static const IDirectPlay3Vtbl dp3A_vt =
{
    IDirectPlay3AImpl_QueryInterface,
    IDirectPlay3AImpl_AddRef,
    IDirectPlay3AImpl_Release,
    IDirectPlay3AImpl_AddPlayerToGroup,
    IDirectPlay3AImpl_Close,
    IDirectPlay3AImpl_CreateGroup,
    IDirectPlay3AImpl_CreatePlayer,
    IDirectPlay3AImpl_DeletePlayerFromGroup,
    IDirectPlay3AImpl_DestroyGroup,
    IDirectPlay3AImpl_DestroyPlayer,
    IDirectPlay3AImpl_EnumGroupPlayers,
    IDirectPlay3AImpl_EnumGroups,
    IDirectPlay3AImpl_EnumPlayers,
    IDirectPlay3AImpl_EnumSessions,
    IDirectPlay3AImpl_GetCaps,
    IDirectPlay3AImpl_GetGroupData,
    IDirectPlay3AImpl_GetGroupName,
    IDirectPlay3AImpl_GetMessageCount,
    IDirectPlay3AImpl_GetPlayerAddress,
    IDirectPlay3AImpl_GetPlayerCaps,
    IDirectPlay3AImpl_GetPlayerData,
    IDirectPlay3AImpl_GetPlayerName,
    IDirectPlay3AImpl_GetSessionDesc,
    IDirectPlay3AImpl_Initialize,
    IDirectPlay3AImpl_Open,
    IDirectPlay3AImpl_Receive,
    IDirectPlay3AImpl_Send,
    IDirectPlay3AImpl_SetGroupData,
    IDirectPlay3AImpl_SetGroupName,
    IDirectPlay3AImpl_SetPlayerData,
    IDirectPlay3AImpl_SetPlayerName,
    IDirectPlay3AImpl_SetSessionDesc,
    IDirectPlay3AImpl_AddGroupToGroup,
    IDirectPlay3AImpl_CreateGroupInGroup,
    IDirectPlay3AImpl_DeleteGroupFromGroup,
    IDirectPlay3AImpl_EnumConnections,
    IDirectPlay3AImpl_EnumGroupsInGroup,
    IDirectPlay3AImpl_GetGroupConnectionSettings,
    IDirectPlay3AImpl_InitializeConnection,
    IDirectPlay3AImpl_SecureOpen,
    IDirectPlay3AImpl_SendChatMessage,
    IDirectPlay3AImpl_SetGroupConnectionSettings,
    IDirectPlay3AImpl_StartSession,
    IDirectPlay3AImpl_GetGroupFlags,
    IDirectPlay3AImpl_GetGroupParent,
    IDirectPlay3AImpl_GetPlayerAccount,
    IDirectPlay3AImpl_GetPlayerFlags
};

static const IDirectPlay4Vtbl dp4_vt =
{
    IDirectPlay4Impl_QueryInterface,
    IDirectPlay4Impl_AddRef,
    IDirectPlay4Impl_Release,
    IDirectPlay4Impl_AddPlayerToGroup,
    IDirectPlay4Impl_Close,
    IDirectPlay4Impl_CreateGroup,
    IDirectPlay4Impl_CreatePlayer,
    IDirectPlay4Impl_DeletePlayerFromGroup,
    IDirectPlay4Impl_DestroyGroup,
    IDirectPlay4Impl_DestroyPlayer,
    IDirectPlay4Impl_EnumGroupPlayers,
    IDirectPlay4Impl_EnumGroups,
    IDirectPlay4Impl_EnumPlayers,
    IDirectPlay4Impl_EnumSessions,
    IDirectPlay4Impl_GetCaps,
    IDirectPlay4Impl_GetGroupData,
    IDirectPlay4Impl_GetGroupName,
    IDirectPlay4Impl_GetMessageCount,
    IDirectPlay4Impl_GetPlayerAddress,
    IDirectPlay4Impl_GetPlayerCaps,
    IDirectPlay4Impl_GetPlayerData,
    IDirectPlay4Impl_GetPlayerName,
    IDirectPlay4Impl_GetSessionDesc,
    IDirectPlay4Impl_Initialize,
    IDirectPlay4Impl_Open,
    IDirectPlay4Impl_Receive,
    IDirectPlay4Impl_Send,
    IDirectPlay4Impl_SetGroupData,
    IDirectPlay4Impl_SetGroupName,
    IDirectPlay4Impl_SetPlayerData,
    IDirectPlay4Impl_SetPlayerName,
    IDirectPlay4Impl_SetSessionDesc,
    IDirectPlay4Impl_AddGroupToGroup,
    IDirectPlay4Impl_CreateGroupInGroup,
    IDirectPlay4Impl_DeleteGroupFromGroup,
    IDirectPlay4Impl_EnumConnections,
    IDirectPlay4Impl_EnumGroupsInGroup,
    IDirectPlay4Impl_GetGroupConnectionSettings,
    IDirectPlay4Impl_InitializeConnection,
    IDirectPlay4Impl_SecureOpen,
    IDirectPlay4Impl_SendChatMessage,
    IDirectPlay4Impl_SetGroupConnectionSettings,
    IDirectPlay4Impl_StartSession,
    IDirectPlay4Impl_GetGroupFlags,
    IDirectPlay4Impl_GetGroupParent,
    IDirectPlay4Impl_GetPlayerAccount,
    IDirectPlay4Impl_GetPlayerFlags,
    IDirectPlay4Impl_GetGroupOwner,
    IDirectPlay4Impl_SetGroupOwner,
    IDirectPlay4Impl_SendEx,
    IDirectPlay4Impl_GetMessageQueue,
    IDirectPlay4Impl_CancelMessage,
    IDirectPlay4Impl_CancelPriority
};

static const IDirectPlay4Vtbl dp4A_vt =
{
    IDirectPlay4AImpl_QueryInterface,
    IDirectPlay4AImpl_AddRef,
    IDirectPlay4AImpl_Release,
    IDirectPlay4AImpl_AddPlayerToGroup,
    IDirectPlay4AImpl_Close,
    IDirectPlay4AImpl_CreateGroup,
    IDirectPlay4AImpl_CreatePlayer,
    IDirectPlay4AImpl_DeletePlayerFromGroup,
    IDirectPlay4AImpl_DestroyGroup,
    IDirectPlay4AImpl_DestroyPlayer,
    IDirectPlay4AImpl_EnumGroupPlayers,
    IDirectPlay4AImpl_EnumGroups,
    IDirectPlay4AImpl_EnumPlayers,
    IDirectPlay4AImpl_EnumSessions,
    IDirectPlay4AImpl_GetCaps,
    IDirectPlay4AImpl_GetGroupData,
    IDirectPlay4AImpl_GetGroupName,
    IDirectPlay4AImpl_GetMessageCount,
    IDirectPlay4AImpl_GetPlayerAddress,
    IDirectPlay4AImpl_GetPlayerCaps,
    IDirectPlay4AImpl_GetPlayerData,
    IDirectPlay4AImpl_GetPlayerName,
    IDirectPlay4AImpl_GetSessionDesc,
    IDirectPlay4AImpl_Initialize,
    IDirectPlay4AImpl_Open,
    IDirectPlay4AImpl_Receive,
    IDirectPlay4AImpl_Send,
    IDirectPlay4AImpl_SetGroupData,
    IDirectPlay4AImpl_SetGroupName,
    IDirectPlay4AImpl_SetPlayerData,
    IDirectPlay4AImpl_SetPlayerName,
    IDirectPlay4AImpl_SetSessionDesc,
    IDirectPlay4AImpl_AddGroupToGroup,
    IDirectPlay4AImpl_CreateGroupInGroup,
    IDirectPlay4AImpl_DeleteGroupFromGroup,
    IDirectPlay4AImpl_EnumConnections,
    IDirectPlay4AImpl_EnumGroupsInGroup,
    IDirectPlay4AImpl_GetGroupConnectionSettings,
    IDirectPlay4AImpl_InitializeConnection,
    IDirectPlay4AImpl_SecureOpen,
    IDirectPlay4AImpl_SendChatMessage,
    IDirectPlay4AImpl_SetGroupConnectionSettings,
    IDirectPlay4AImpl_StartSession,
    IDirectPlay4AImpl_GetGroupFlags,
    IDirectPlay4AImpl_GetGroupParent,
    IDirectPlay4AImpl_GetPlayerAccount,
    IDirectPlay4AImpl_GetPlayerFlags,
    IDirectPlay4AImpl_GetGroupOwner,
    IDirectPlay4AImpl_SetGroupOwner,
    IDirectPlay4AImpl_SendEx,
    IDirectPlay4AImpl_GetMessageQueue,
    IDirectPlay4AImpl_CancelMessage,
    IDirectPlay4AImpl_CancelPriority
};

HRESULT dplay_create( REFIID riid, void **ppv )
{
    IDirectPlayImpl *obj;
    HRESULT hr;

    TRACE( "(%s, %p)\n", debugstr_guid( riid ), ppv );

    *ppv = NULL;
    obj = malloc( sizeof( *obj ) );
    if ( !obj )
        return DPERR_OUTOFMEMORY;

    obj->IDirectPlay_iface.lpVtbl = &dp_vt;
    obj->IDirectPlay2A_iface.lpVtbl = &dp2A_vt;
    obj->IDirectPlay2_iface.lpVtbl = &dp2_vt;
    obj->IDirectPlay3A_iface.lpVtbl = &dp3A_vt;
    obj->IDirectPlay3_iface.lpVtbl = &dp3_vt;
    obj->IDirectPlay4A_iface.lpVtbl = &dp4A_vt;
    obj->IDirectPlay4_iface.lpVtbl = &dp4_vt;
    obj->ref = 1;

    InitializeCriticalSection( &obj->lock );

    if ( DP_CreateDirectPlay2( obj ) )
        hr = IDirectPlayX_QueryInterface( &obj->IDirectPlay4_iface, riid, ppv );
    else
        hr = DPERR_NOMEMORY;
    IDirectPlayX_Release( &obj->IDirectPlay4_iface );

    return hr;
}


HRESULT DP_GetSPPlayerData( IDirectPlayImpl *lpDP, DPID idPlayer, void **lplpData )
{
  struct GroupData *group;
  lpPlayerList lpPlayer;

  EnterCriticalSection( &lpDP->lock );

  lpPlayer = DP_FindPlayer( lpDP, idPlayer );
  if( lpPlayer )
  {
    *lplpData = lpPlayer->lpPData->lpSPPlayerData;
    LeaveCriticalSection( &lpDP->lock );
    return DP_OK;
  }

  group = DP_FindAnyGroup( lpDP, idPlayer );
  if( group )
  {
    *lplpData = group->lpSPPlayerData;
    LeaveCriticalSection( &lpDP->lock );
    return DP_OK;
  }

  LeaveCriticalSection( &lpDP->lock );

  return DPERR_INVALIDPLAYER;
}

HRESULT DP_SetSPPlayerData( IDirectPlayImpl *lpDP, DPID idPlayer, void *lpData )
{
  struct GroupData *group;
  lpPlayerList lpPlayer;

  EnterCriticalSection( &lpDP->lock );

  lpPlayer = DP_FindPlayer( lpDP, idPlayer );
  if( lpPlayer )
  {
    lpPlayer->lpPData->lpSPPlayerData = lpData;
    LeaveCriticalSection( &lpDP->lock );
    return DP_OK;
  }

  group = DP_FindAnyGroup( lpDP, idPlayer );
  if( group )
  {
    group->lpSPPlayerData = lpData;
    LeaveCriticalSection( &lpDP->lock );
    return DP_OK;
  }

  LeaveCriticalSection( &lpDP->lock );

  return DPERR_INVALIDPLAYER;
}

/***************************************************************************
 *  DirectPlayEnumerateAW
 *
 *  The pointer to the structure lpContext will be filled with the
 *  appropriate data for each service offered by the OS. These services are
 *  not necessarily available on this particular machine but are defined
 *  as simple service providers under the "Service Providers" registry key.
 *  This structure is then passed to lpEnumCallback for each of the different
 *  services.
 *
 *  This API is useful only for applications written using DirectX3 or
 *  worse. It is superseded by IDirectPlay3::EnumConnections which also
 *  gives information on the actual connections.
 *
 * defn of a service provider:
 * A dynamic-link library used by DirectPlay to communicate over a network.
 * The service provider contains all the network-specific code required
 * to send and receive messages. Online services and network operators can
 * supply service providers to use specialized hardware, protocols, communications
 * media, and network resources.
 *
 */
static HRESULT DirectPlayEnumerateAW(LPDPENUMDPCALLBACKA lpEnumCallbackA,
                                     LPDPENUMDPCALLBACKW lpEnumCallbackW,
                                     LPVOID lpContext)
{
    DPCONNECTION *connection;
    struct list *connections;

    if (!lpEnumCallbackA && !lpEnumCallbackW)
    {
	return DPERR_INVALIDPARAMS;
    }

    connections = DP_GetConnections();

    LIST_FOR_EACH_ENTRY(connection, connections, DPCONNECTION, entry)
    {
	if (!(connection->flags & DPCONNECTION_DIRECTPLAY))
	    continue;

	/* The enumeration will return FALSE if we are not to continue.
	 *
	 * Note: on my windows box, major / minor version is 6 / 0 for all service providers
	 *       and have no relation to any of the two dwReserved1 and dwReserved2 keys.
	 *       I think that it simply means that they are in-line with DirectX 6.0
	 */
	if (lpEnumCallbackA)
	{
	    if (!lpEnumCallbackA(&connection->spGuid, connection->nameA.lpszShortNameA, 6, 0,
				 lpContext))
		break;
	}
	else
	{
	    if (!lpEnumCallbackW(&connection->spGuid, connection->name.lpszShortName, 6, 0,
				 lpContext))
		break;
	}
    }

    return DP_OK;
}

/***************************************************************************
 *  DirectPlayEnumerate  [DPLAYX.9]
 *  DirectPlayEnumerateA [DPLAYX.2]
 */
HRESULT WINAPI DirectPlayEnumerateA(LPDPENUMDPCALLBACKA lpEnumCallback, LPVOID lpContext )
{
    TRACE("(%p,%p)\n", lpEnumCallback, lpContext);
    
    return DirectPlayEnumerateAW(lpEnumCallback, NULL, lpContext);
}

/***************************************************************************
 *  DirectPlayEnumerateW [DPLAYX.3]
 */
HRESULT WINAPI DirectPlayEnumerateW(LPDPENUMDPCALLBACKW lpEnumCallback, LPVOID lpContext )
{
    TRACE("(%p,%p)\n", lpEnumCallback, lpContext);
    
    return DirectPlayEnumerateAW(NULL, lpEnumCallback, lpContext);
}

typedef struct tagCreateEnum
{
  LPVOID  lpConn;
  LPCGUID lpGuid;
} CreateEnumData, *lpCreateEnumData;

/* Find and copy the matching connection for the SP guid */
static BOOL CALLBACK cbDPCreateEnumConnections(
    LPCGUID     lpguidSP,
    LPVOID      lpConnection,
    DWORD       dwConnectionSize,
    LPCDPNAME   lpName,
    DWORD       dwFlags,
    LPVOID      lpContext)
{
  lpCreateEnumData lpData = (lpCreateEnumData)lpContext;

  if( IsEqualGUID( lpguidSP, lpData->lpGuid ) )
  {
    TRACE( "Found SP entry with guid %s\n", debugstr_guid(lpData->lpGuid) );

    lpData->lpConn = malloc( dwConnectionSize );
    CopyMemory( lpData->lpConn, lpConnection, dwConnectionSize );

    /* Found the record that we were looking for */
    return FALSE;
  }

  /* Haven't found what were looking for yet */
  return TRUE;
}


/***************************************************************************
 *  DirectPlayCreate [DPLAYX.1]
 *
 */
HRESULT WINAPI DirectPlayCreate
( LPGUID lpGUID, LPDIRECTPLAY *lplpDP, IUnknown *pUnk )
{
  HRESULT hr;
  LPDIRECTPLAY3A lpDP3A;
  CreateEnumData cbData;

  TRACE( "lpGUID=%s lplpDP=%p pUnk=%p\n", debugstr_guid(lpGUID), lplpDP, pUnk );

  if( pUnk != NULL )
  {
    return CLASS_E_NOAGGREGATION;
  }

  if( (lplpDP == NULL) || (lpGUID == NULL) )
  {
    return DPERR_INVALIDPARAMS;
  }

  if ( dplay_create( &IID_IDirectPlay, (void**)lplpDP ) != DP_OK )
    return DPERR_UNAVAILABLE;

  if( IsEqualGUID( &GUID_NULL, lpGUID ) )
  {
    /* The GUID_NULL means don't bind a service provider. Just return the
       interface as is */
    return DP_OK;
  }

  /* Bind the desired service provider since lpGUID is non NULL */
  TRACE( "Service Provider binding for %s\n", debugstr_guid(lpGUID) );

  /* We're going to use a DP3 interface */
  hr = IDirectPlayX_QueryInterface( *lplpDP, &IID_IDirectPlay3A,
                                    (LPVOID*)&lpDP3A );
  if( FAILED(hr) )
  {
    ERR( "Failed to get DP3 interface: %s\n", DPLAYX_HresultToString(hr) );
    return hr;
  }

  cbData.lpConn = NULL;
  cbData.lpGuid = lpGUID;

  /* We were given a service provider, find info about it... */
  hr = IDirectPlayX_EnumConnections( lpDP3A, NULL, cbDPCreateEnumConnections,
                                     &cbData, DPCONNECTION_DIRECTPLAY );
  if( ( FAILED(hr) ) ||
      ( cbData.lpConn == NULL )
    )
  {
    ERR( "Failed to get Enum for SP: %s\n", DPLAYX_HresultToString(hr) );
    IDirectPlayX_Release( lpDP3A );
    return DPERR_UNAVAILABLE;
  }

  /* Initialize the service provider */
  hr = IDirectPlayX_InitializeConnection( lpDP3A, cbData.lpConn, 0 );
  if( FAILED(hr) )
  {
    ERR( "Failed to Initialize SP: %s\n", DPLAYX_HresultToString(hr) );
    free( cbData.lpConn );
    IDirectPlayX_Release( lpDP3A );
    return hr;
  }

  /* Release our version of the interface now that we're done with it */
  IDirectPlayX_Release( lpDP3A );
  free( cbData.lpConn );

  return DP_OK;
}

/* ------------------------------------------------------------------ Windows port: lifecycle helpers */

static DWORD DP_CopyDestroyPlayerOrGroup( DPMSG_GENERIC *genericDst, DPMSG_GENERIC *genericSrc,
                                          DWORD genericSize, BOOL ansi )
{
    DPMSG_DESTROYPLAYERORGROUP *src = (DPMSG_DESTROYPLAYERORGROUP *) genericSrc;
    DPMSG_DESTROYPLAYERORGROUP *dst = (DPMSG_DESTROYPLAYERORGROUP *) genericDst;
    DWORD offset = sizeof( DPMSG_DESTROYPLAYERORGROUP );

    if ( dst )
    {
        *dst = *src;
        dst->lpLocalData = dst->lpRemoteData = NULL;
        dst->dwLocalDataSize = dst->dwRemoteDataSize = 0;
    }
    offset += DP_CopyString( &dst->dpnName.lpszShortNameA, src->dpnName.lpszShortNameA, ansi, FALSE,
                             genericDst, offset );
    offset += DP_CopyString( &dst->dpnName.lpszLongNameA, src->dpnName.lpszLongNameA, ansi, FALSE,
                             genericDst, offset );
    return offset;
}

/* the player out of every group it is in: the groups in creation order, each before its own
 * children (Windows: three groups, the player added to them in another order - they came in
 * creation order) */
static void DP_RemovePlayerFromGroups( IDirectPlayImpl *This, lpGroupData where, DPID idPlayer, int depth )
{
    lpGroupList link;
    lpPlayerList member;

    if ( !where || depth > 64 )
        return;
    for ( link = DPQ_FIRST( where->groups ); link; link = DPQ_NEXT( link->groups ) )
    {
        if ( !DP_IS_CHILD_LINK( where, link ) )
            continue;
        DPQ_FIND_ENTRY( link->lpGData->players, players, lpPData->dpid, ==, idPlayer, member );
        if ( member )
            IDirectPlayX_DeletePlayerFromGroup( &This->IDirectPlay4_iface, link->lpGData->dpid, idPlayer );
        DP_RemovePlayerFromGroups( This, link->lpGData, idPlayer, depth + 1 );
    }
}

/* Remove a player completely: the player leaves its groups, the application hears
 * DPSYS_DESTROYPLAYERORGROUP and the SP forgets it. tellOthers: it's ours - tell the other machines. */
void DP_RemovePlayer( IDirectPlayImpl *This, DPID idPlayer, BOOL tellOthers )
{
    struct PlayerList *plist;
    struct PlayerData *p;
    DPFAGContext cbContext;

    EnterCriticalSection( &This->lock );
    plist = DP_FindPlayer( This, idPlayer );
    if ( !plist )
    {
        LeaveCriticalSection( &This->lock );
        return;
    }
    p = plist->lpPData;
    (void)cbContext;

    /* first out of every group it is in, then gone - Windows' order (black box, 2026-10-22: each
     * group's DPSYS_DELETEPLAYERFROMGROUP, the player itself hearing it too, then the
     * DPSYS_DESTROYPLAYERORGROUP to the others). (Through the groups' children only: Wine's walk
     * followed shortcuts too, and a group added to its own child made it recurse until the stack
     * ran out.) */
    DP_RemovePlayerFromGroups( This, This->dp2->lpSysGroup, idPlayer, 0 );

    if ( !(p->dwFlags & DPLAYI_PLAYER_SYSPLAYER) )
    {
        DPMSG_DESTROYPLAYERORGROUP msg;

        memset( &msg, 0, sizeof(msg) );
        msg.dwType = DPSYS_DESTROYPLAYERORGROUP;
        msg.dwPlayerType = DPPLAYERTYPE_PLAYER;
        msg.dpId = idPlayer;
        if ( p->name ) msg.dpnName = *p->name;
        /* DPPLAYER_LOCAL (the same bit as DPLAYI_PLAYER_PLAYERLOCAL) for a player of this machine,
         * as Windows sends it; the internal bits stay out */
        msg.dwFlags = p->dwFlags & ~(DPLAYI_PLAYER_SYSPLAYER | DPLAYI_PLAYER_NAMESRVR | DPLAYI_PLAYER_PLAYERINGROUP);
        DP_QueueMessage( This, DPID_SYSMSG, DPID_ALLPLAYERS, idPlayer, &msg, DP_CopyDestroyPlayerOrGroup, 0 );
        if ( This->dp2->lpSessionDesc && This->dp2->lpSessionDesc->dwCurrentPlayers )
            This->dp2->lpSessionDesc->dwCurrentPlayers--;
    }

    if ( tellOthers && (p->dwFlags & DPLAYI_PLAYER_PLAYERLOCAL) && This->dp2->bConnectionOpen )
        DP_MSG_SendDeletePlayer( This, idPlayer );

    {
        DWORD spFlags = DP_SPDeleteFlags( p->dwFlags );   /* (p goes with DP_DeletePlayer) */

        DP_DeletePlayer( This, idPlayer );

        if ( This->dp2->spData.lpCB->DeletePlayer )
        {
            DPSP_DELETEPLAYERDATA data;
            data.idPlayer = idPlayer;
            data.dwFlags = spFlags;
            data.lpISP = This->dp2->spData.lpISP;
            This->dp2->spData.lpCB->DeletePlayer( &data );
        }
    }
    LeaveCriticalSection( &This->lock );
}

/* Close: the provider hears about every player before CloseEx, in Windows' order - this machine's
 * game players newest first, its system player, then the other machines' players in list order
 * (S3 tap, 2026-10-16; ours told it nothing and went straight to CloseEx) */
static void DP_SPDeleteAllPlayers( IDirectPlayImpl *This )
{
    struct PlayerList *plist;
    struct PlayerData *order[ 256 ];
    DPSP_DELETEPLAYERDATA data;
    int count = 0, pass, k;

    if ( !This->dp2->spData.lpCB->DeletePlayer || !This->dp2->lpSysGroup )
        return;
    EnterCriticalSection( &This->lock );
    for ( plist = DPQ_FIRST( This->dp2->lpSysGroup->players ); plist && count < ARRAYSIZE( order );
          plist = DPQ_NEXT( plist->players ) )
        order[ count++ ] = plist->lpPData;
    data.lpISP = This->dp2->spData.lpISP;
    for ( pass = 0; pass < 3; pass++ )
        for ( k = 0; k < count; k++ )
        {
            struct PlayerData *p = order[ pass == 2 ? k : count - 1 - k ];
            BOOL local = !!(p->dwFlags & DPLAYI_PLAYER_PLAYERLOCAL), sys = !!(p->dwFlags & DPLAYI_PLAYER_SYSPLAYER);
            if ( pass == 0 ? !(local && !sys) : pass == 1 ? !(local && sys) : local )
                continue;
            data.idPlayer = p->dpid;
            data.dwFlags = DP_SPDeleteFlags( p->dwFlags );
            This->dp2->spData.lpCB->DeletePlayer( &data );
        }
    LeaveCriticalSection( &This->lock );
}

/* a new host continues numbering above every id already in the session */
void DP_BumpObjectIds( DPID above )
{
    LONG cur;
    while ( (cur = kludgePlayerGroupId) < (LONG)above )
        InterlockedCompareExchange( &kludgePlayerGroupId, (LONG)above, cur );
}

void DP_QueueSystemMessage( IDirectPlayImpl *This, DWORD type )
{
    DPMSG_GENERIC msg;
    msg.dwType = type;
    EnterCriticalSection( &This->lock );
    DP_QueueMessage( This, DPID_SYSMSG, DPID_ALLPLAYERS, 0, &msg, DP_CopyGeneric, sizeof(msg) );
    LeaveCriticalSection( &This->lock );
}

/* ------------------------------------------------------------------ Windows port: lobby changes */
/* Player names and data, the session description and chat now reach every machine (Wine kept them
 * local). Commands as Windows numbers them: 15 PLAYERDATACHANGED, 16 PLAYERNAMECHANGED,
 * 26 SESSIONDESCCHANGED, 45 CHAT. Like Windows, our own players hear about name, data and session
 * changes too; chat goes to everyone but the sender. */

static DWORD DP_CopySetPlayerOrGroupName( DPMSG_GENERIC *genericDst, DPMSG_GENERIC *genericSrc,
                                          DWORD genericSize, BOOL ansi )
{
    DPMSG_SETPLAYERORGROUPNAME *src = (DPMSG_SETPLAYERORGROUPNAME *) genericSrc;
    DPMSG_SETPLAYERORGROUPNAME *dst = (DPMSG_SETPLAYERORGROUPNAME *) genericDst;
    DWORD offset = sizeof( DPMSG_SETPLAYERORGROUPNAME );

    if ( dst ) *dst = *src;
    offset += DP_CopyString( &dst->dpnName.lpszShortNameA, src->dpnName.lpszShortNameA, ansi, FALSE,
                             genericDst, offset );
    offset += DP_CopyString( &dst->dpnName.lpszLongNameA, src->dpnName.lpszLongNameA, ansi, FALSE,
                             genericDst, offset );
    return offset;
}

static DWORD DP_CopySetSessionDescMsg( DPMSG_GENERIC *genericDst, DPMSG_GENERIC *genericSrc,
                                       DWORD genericSize, BOOL ansi )
{
    DPMSG_SETSESSIONDESC *src = (DPMSG_SETSESSIONDESC *) genericSrc;
    DPMSG_SETSESSIONDESC *dst = (DPMSG_SETSESSIONDESC *) genericDst;
    DWORD offset = sizeof( DPMSG_SETSESSIONDESC );

    if ( dst ) *dst = *src;
    offset += DP_CopyString( &dst->dpDesc.lpszSessionNameA, src->dpDesc.lpszSessionNameA, ansi, FALSE,
                             genericDst, offset );
    offset += DP_CopyString( &dst->dpDesc.lpszPasswordA, src->dpDesc.lpszPasswordA, ansi, FALSE,
                             genericDst, offset );
    return offset;
}

static DWORD DP_CopyChat( DPMSG_GENERIC *genericDst, DPMSG_GENERIC *genericSrc, DWORD genericSize, BOOL ansi )
{
    DPMSG_CHAT *src = (DPMSG_CHAT *) genericSrc;
    DPMSG_CHAT *dst = (DPMSG_CHAT *) genericDst;
    DPCHAT *chat = dst ? (DPCHAT *)((char *) dst + sizeof( DPMSG_CHAT )) : NULL;
    DWORD offset = sizeof( DPMSG_CHAT ) + sizeof( DPCHAT );

    if ( dst )
    {
        *dst = *src;
        *chat = *src->lpChat;
        dst->lpChat = chat;
    }
    offset += DP_CopyString( chat ? &chat->lpszMessageA : NULL, src->lpChat->lpszMessageA, ansi, FALSE,
                             genericDst, offset );
    return offset;
}

/* a NUL-terminated WCHAR string inside a received message, or NULL */
const WCHAR *DP_WireString( const void *body, DWORD size, DWORD offset )
{
    const WCHAR *s;
    DWORD n, i;

    if ( !offset || offset >= size || size - offset < sizeof( WCHAR ) )
        return NULL;
    s = (const WCHAR *)((const char *) body + offset);
    n = (size - offset) / sizeof( WCHAR );
    for ( i = 0; i < n; i++ )
        if ( !s[ i ] ) return s;
    return NULL;
}

static DWORD DP_WireStringSize( const WCHAR *s )
{
    return s ? (lstrlenW( s ) + 1) * sizeof( WCHAR ) : 0;
}

/* ------------------------------------------------------------------ Windows port: who sent this? */
/* Online/LAN Play attaches an SP header naming the sending machine by its peer id, and every machine's
 * system player carries that peer id in its SP data; the transport delivers only packets that prove the
 * sender holds that peer's token (okplay net.c). So a system message that acts on a player is honoured
 * only when it came from that player's own machine, and a host-only message only from the host. With
 * any other service provider the header carries no peer id, DP_SenderMachine reports "not known", and
 * nothing is refused - classic connections behave exactly as before. */
#ifdef OKP_BUILTIN
extern unsigned long long okp_header_peer( const void *hdr, DWORD hdrsize );
extern unsigned long long okp_spdata_peer( const void *data, DWORD size );
#endif

DPID DP_SenderMachine( IDirectPlayImpl *This, const void *header, BOOL *known )
{
    *known = FALSE;
#ifdef OKP_BUILTIN
    {
        unsigned long long peer = okp_header_peer( header, This->dp2->spData.dwSPHeaderSize );
        struct PlayerList *plist;

        if ( !peer || !This->dp2->lpSysGroup ) return 0;
        *known = TRUE;
        for ( plist = DPQ_FIRST( This->dp2->lpSysGroup->players ); plist; plist = DPQ_NEXT( plist->players ) )
        {
            struct PlayerData *p = plist->lpPData;
            void *data = NULL;
            DWORD size = 0;
            if ( !(p->dwFlags & DPLAYI_PLAYER_SYSPLAYER) ) continue;
            if ( SUCCEEDED( IDirectPlaySP_GetSPPlayerData( This->dp2->spData.lpISP, p->dpid, &data, &size,
                                                           DPSET_REMOTE ) ) && okp_spdata_peer( data, size ) == peer )
                return p->dpid;
        }
    }
#else
    (void)This; (void)header;
#endif
    return 0;
}

BOOL DP_SenderOwns( IDirectPlayImpl *This, const void *header, DPID owner, const char *what )
{
    BOOL known;
    DPID sender;
    struct PlayerList *plist;

#ifdef DPLAYX_TEST_SKIP_AUTH   /* negative control for tests only: trust every sender, as 0.7 did */
    return TRUE;
#endif
    if ( !owner ) return TRUE;   /* nobody in particular (e.g. the host before we know who that is) */
    EnterCriticalSection( &This->lock );
    sender = DP_SenderMachine( This, header, &known );
    if ( !known || (sender && owner == sender) )   /* no identity (classic SP), or the owner itself */
    {
        LeaveCriticalSection( &This->lock );
        return TRUE;
    }
    plist = DP_FindPlayer( This, owner );
    if ( sender && plist && plist->lpPData->systemPlayerId == sender )   /* one of the sender's own players */
    {
        LeaveCriticalSection( &This->lock );
        return TRUE;
    }
    LeaveCriticalSection( &This->lock );
    DP_NOTE( "%s for 0x%08lx ignored: sent by machine 0x%08lx, not the owner's\n", what, owner, sender );
    return FALSE;
}

static void DP_QueueNameChanged( IDirectPlayImpl *This, struct PlayerData *p )
{
    DPMSG_SETPLAYERORGROUPNAME msg;

    if ( DP_DataMessagesOff( This ) ) return;
    memset( &msg, 0, sizeof(msg) );
    msg.dwType = DPSYS_SETPLAYERORGROUPNAME;
    msg.dwPlayerType = DPPLAYERTYPE_PLAYER;
    msg.dpId = p->dpid;
    if ( p->name ) msg.dpnName = *p->name;
    DP_QueueMessage( This, DPID_SYSMSG, DPID_ALLPLAYERS, 0, &msg, DP_CopySetPlayerOrGroupName, 0 );
}

static void DP_QueueDataChanged( IDirectPlayImpl *This, struct PlayerData *p )
{
    DPMSG_SETPLAYERORGROUPDATA msg;

    if ( DP_DataMessagesOff( This ) ) return;
    memset( &msg, 0, sizeof(msg) );
    msg.dwType = DPSYS_SETPLAYERORGROUPDATA;
    msg.dwPlayerType = DPPLAYERTYPE_PLAYER;
    msg.dpId = p->dpid;
    msg.lpData = p->lpRemoteData;
    msg.dwDataSize = p->dwRemoteDataSize;
    DP_QueueMessage( This, DPID_SYSMSG, DPID_ALLPLAYERS, 0, &msg, DP_CopySetPlayerOrGroupData, 0 );
}

static void DP_QueueChat( IDirectPlayImpl *This, DPID from, DPID to, DWORD flags, const WCHAR *text )
{
    DPMSG_CHAT msg;
    DPCHAT chat;
    BOOL toPlayer = to && DP_FindPlayer( This, to ) != NULL;

    memset( &chat, 0, sizeof(chat) );
    chat.dwSize = sizeof(chat);
    chat.lpszMessage = (WCHAR *) text;
    memset( &msg, 0, sizeof(msg) );
    msg.dwType = DPSYS_CHAT;
    msg.dwFlags = flags;
    msg.idFromPlayer = from;
    msg.idToPlayer = toPlayer ? to : 0;
    msg.idToGroup = toPlayer ? 0 : to;
    msg.lpChat = &chat;
    DP_QueueMessage( This, DPID_SYSMSG, to, from, &msg, DP_CopyChat, 0 );
}

void DP_LOBBY_PlayerNameChanged( IDirectPlayImpl *This, DPID id )
{
    struct PlayerList *plist;
    struct PlayerData *p;

    EnterCriticalSection( &This->lock );
    plist = DP_FindPlayer( This, id );
    if ( !plist || !This->dp2->bConnectionOpen )
    {
        LeaveCriticalSection( &This->lock );
        return;
    }
    p = plist->lpPData;
    DP_QueueNameChanged( This, p );
    if ( p->dwFlags & DPLAYI_PLAYER_PLAYERLOCAL )
    {
        const WCHAR *s = p->name ? p->name->lpszShortName : NULL, *l = p->name ? p->name->lpszLongName : NULL;
        DWORD ss = DP_WireStringSize( s ), ls = DP_WireStringSize( l ), size = sizeof( DPSP_MSG_PLAYERNAMECHANGED ) + ss + ls;
        DPSP_MSG_PLAYERNAMECHANGED *m = calloc( 1, size );
        if ( m )
        {
            m->envelope.dwMagic = DPMSGMAGIC_DPLAYMSG;
            m->envelope.wCommandId = DPMSGCMD_PLAYERNAMECHANGED;
            m->envelope.wVersion = DPMSGVER_DP6;
            m->playerId = id;
            m->shortNameOffset = ss ? sizeof(*m) : 0;
            m->longNameOffset = ls ? sizeof(*m) + ss : 0;
            if ( ss ) memcpy( (char *) m + sizeof(*m), s, ss );
            if ( ls ) memcpy( (char *) m + sizeof(*m) + ss, l, ls );
            DP_SendToAllMachines( This, m, size, NULL, 0 );
            free( m );
        }
    }
    LeaveCriticalSection( &This->lock );
}

void DP_LOBBY_PlayerDataChanged( IDirectPlayImpl *This, DPID id )
{
    struct PlayerList *plist;
    struct PlayerData *p;

    EnterCriticalSection( &This->lock );
    plist = DP_FindPlayer( This, id );
    if ( !plist || !This->dp2->bConnectionOpen )
    {
        LeaveCriticalSection( &This->lock );
        return;
    }
    p = plist->lpPData;
    DP_QueueDataChanged( This, p );
    if ( p->dwFlags & DPLAYI_PLAYER_PLAYERLOCAL )
    {
        DWORD size = sizeof( DPSP_MSG_PLAYERDATACHANGED ) + p->dwRemoteDataSize;
        DPSP_MSG_PLAYERDATACHANGED *m = calloc( 1, size );
        if ( m )
        {
            m->envelope.dwMagic = DPMSGMAGIC_DPLAYMSG;
            m->envelope.wCommandId = DPMSGCMD_PLAYERDATACHANGED;
            m->envelope.wVersion = DPMSGVER_DP6;
            m->playerId = id;
            m->dataSize = p->dwRemoteDataSize;
            m->dataOffset = sizeof(*m);
            if ( p->dwRemoteDataSize ) memcpy( (char *) m + sizeof(*m), p->lpRemoteData, p->dwRemoteDataSize );
            DP_SendToAllMachines( This, m, size, NULL, 0 );
            free( m );
        }
    }
    LeaveCriticalSection( &This->lock );
}

void DP_LOBBY_SessionDescChanged( IDirectPlayImpl *This )
{
    DP_LOBBY_SessionDescChangedEx( This, TRUE );
}

/* tellLocalPlayers FALSE: only the other machines hear it (IDirectPlay::EnableNewPlayers - Windows
 * changes the flags without a DPSYS_SETSESSIONDESC to its own players, apistate 2026-10-22) */
void DP_LOBBY_SessionDescChangedEx( IDirectPlayImpl *This, BOOL tellLocalPlayers )
{
    DPSESSIONDESC2 *w;

    EnterCriticalSection( &This->lock );
    if ( !This->dp2->bConnectionOpen || !This->dp2->lpSessionDesc ||
         !(w = DP_DuplicateSessionDesc( This->dp2->lpSessionDesc, FALSE, FALSE )) )
    {
        LeaveCriticalSection( &This->lock );
        return;
    }
    if ( tellLocalPlayers )
    {
        DPMSG_SETSESSIONDESC msg;
        memset( &msg, 0, sizeof(msg) );
        msg.dwType = DPSYS_SETSESSIONDESC;
        msg.dpDesc = *w;
        DP_QueueMessage( This, DPID_SYSMSG, DPID_ALLPLAYERS, 0, &msg, DP_CopySetSessionDescMsg, 0 );
    }
    if ( This->dp2->bHostInterface )
    {
        DWORD ns = DP_WireStringSize( w->lpszSessionName ), ps = DP_WireStringSize( w->lpszPassword );
        DWORD size = sizeof( DPSP_MSG_SESSIONDESCCHANGED ) + ns + ps;
        DPSP_MSG_SESSIONDESCCHANGED *m = calloc( 1, size );
        if ( m )
        {
            m->envelope.dwMagic = DPMSGMAGIC_DPLAYMSG;
            m->envelope.wCommandId = DPMSGCMD_SESSIONDESCCHANGED;
            m->envelope.wVersion = DPMSGVER_DP6;
            m->desc = *w;
            m->desc.lpszSessionName = NULL;
            m->desc.lpszPassword = NULL;
            m->sessionNameOffset = ns ? sizeof(*m) : 0;
            m->passwordOffset = ps ? sizeof(*m) + ns : 0;
            if ( ns ) memcpy( (char *) m + sizeof(*m), w->lpszSessionName, ns );
            if ( ps ) memcpy( (char *) m + sizeof(*m) + ns, w->lpszPassword, ps );
            DP_SendToAllMachines( This, m, size, NULL, 0 );
            free( m );
        }
    }
    free( w );
    LeaveCriticalSection( &This->lock );
}

HRESULT DP_LOBBY_SendChat( IDirectPlayImpl *This, DPID from, DPID to, DWORD flags, DPCHAT *chat, BOOL ansi )
{
    struct PlayerList *plist;
    WCHAR *text;

    TRACE( "(%p)->(0x%08lx,0x%08lx,0x%08lx,%p,%u)\n", This, from, to, flags, chat, ansi );
    if ( This->dp2->connectionInitialized == NO_PROVIDER )
        return DPERR_UNINITIALIZED;
    if ( !chat || !chat->lpszMessage )
        return DPERR_INVALIDPARAMS;

    EnterCriticalSection( &This->lock );
    plist = DP_FindPlayer( This, from );   /* no session means no players: INVALIDPLAYER, like Windows */
    if ( !This->dp2->bConnectionOpen || !plist || !(plist->lpPData->dwFlags & DPLAYI_PLAYER_PLAYERLOCAL) )
    {
        LeaveCriticalSection( &This->lock );
        return DPERR_INVALIDPLAYER;
    }
    if ( to != DPID_ALLPLAYERS && !DP_FindPlayer( This, to ) && !DP_FindAnyGroup( This, to ) )
    {
        LeaveCriticalSection( &This->lock );
        return DPERR_INVALIDPLAYER;
    }
    text = DP_DuplicateString( chat->lpszMessage, FALSE, ansi );
    if ( !text )
    {
        LeaveCriticalSection( &This->lock );
        return DPERR_OUTOFMEMORY;
    }

    DP_QueueChat( This, from, to, flags, text );   /* other players on this machine */
    {
        DWORD ts = DP_WireStringSize( text ), size = sizeof( DPSP_MSG_CHAT ) + ts;
        DPSP_MSG_CHAT *m = calloc( 1, size );
        if ( m )
        {
            m->envelope.dwMagic = DPMSGMAGIC_DPLAYMSG;
            m->envelope.wCommandId = DPMSGCMD_CHAT;
            m->envelope.wVersion = DPMSGVER_DP6;
            m->fromId = from;
            m->toId = to;
            m->flags = flags;
            m->messageOffset = sizeof(*m);
            memcpy( (char *) m + sizeof(*m), text, ts );
            DP_SendToAllMachines( This, m, size, NULL, 0 );
            free( m );
        }
    }
    free( text );
    LeaveCriticalSection( &This->lock );
    return DP_OK;
}

HRESULT DP_LOBBY_HandlePlayerNameChanged( IDirectPlayImpl *This, const void *body, DWORD size, const void *header )
{
    const DPSP_MSG_PLAYERNAMECHANGED *m = body;
    struct PlayerList *plist;
    DPNAME name, *w, *a;

    if ( size < sizeof(*m) ) return DPERR_GENERIC;
    memset( &name, 0, sizeof(name) );
    name.dwSize = sizeof(name);
    name.lpszShortName = (WCHAR *) DP_WireString( body, size, m->shortNameOffset );
    name.lpszLongName = (WCHAR *) DP_WireString( body, size, m->longNameOffset );

    EnterCriticalSection( &This->lock );
    plist = DP_FindPlayer( This, m->playerId );
    if ( This->dp2->bConnectionOpen && plist && !(plist->lpPData->dwFlags & DPLAYI_PLAYER_PLAYERLOCAL) &&
         DP_SenderOwns( This, header, m->playerId, "PLAYERNAMECHANGED" ) )   /* a player renames itself */
    {
        w = DP_DuplicateName( &name, FALSE, FALSE, TRUE );
        a = DP_DuplicateName( &name, TRUE, FALSE, TRUE );
        if ( w && a )
        {
            free( plist->lpPData->name );
            free( plist->lpPData->nameA );
            plist->lpPData->name = w;
            plist->lpPData->nameA = a;
            DP_QueueNameChanged( This, plist->lpPData );
        }
        else
        {
            free( w );
            free( a );
        }
    }
    LeaveCriticalSection( &This->lock );
    return DP_OK;
}

HRESULT DP_LOBBY_HandlePlayerDataChanged( IDirectPlayImpl *This, const void *body, DWORD size, const void *header )
{
    const DPSP_MSG_PLAYERDATACHANGED *m = body;
    struct PlayerList *plist;

    if ( size < sizeof(*m) || m->dataOffset > size || size - m->dataOffset < m->dataSize )
        return DPERR_GENERIC;
    EnterCriticalSection( &This->lock );
    plist = DP_FindPlayer( This, m->playerId );
    if ( This->dp2->bConnectionOpen && plist && !(plist->lpPData->dwFlags & DPLAYI_PLAYER_PLAYERLOCAL) &&
         DP_SenderOwns( This, header, m->playerId, "PLAYERDATACHANGED" ) )   /* a player sets its own data */
    {
        DP_SetPlayerData( plist->lpPData, DPSET_REMOTE, (char *) body + m->dataOffset, m->dataSize );
        DP_QueueDataChanged( This, plist->lpPData );
    }
    LeaveCriticalSection( &This->lock );
    return DP_OK;
}

HRESULT DP_LOBBY_HandleSessionDescChanged( IDirectPlayImpl *This, const void *body, DWORD size, const void *header )
{
    const DPSP_MSG_SESSIONDESCCHANGED *m = body;
    DPSESSIONDESC2 desc, *stored;

    if ( size < sizeof(*m) ) return DPERR_GENERIC;
    EnterCriticalSection( &This->lock );
    /* the session description is the host's alone */
    if ( !This->dp2->bConnectionOpen || This->dp2->bHostInterface || !This->dp2->lpSessionDesc ||
         !DP_SenderOwns( This, header, This->dp2->nameServerId, "SESSIONDESCCHANGED" ) )
    {
        LeaveCriticalSection( &This->lock );
        return DP_OK;
    }
    desc = m->desc;
    desc.dwSize = sizeof(desc);
    desc.lpszSessionName = (WCHAR *) DP_WireString( body, size, m->sessionNameOffset );
    desc.lpszPassword = (WCHAR *) DP_WireString( body, size, m->passwordOffset );
    desc.dwCurrentPlayers = This->dp2->lpSessionDesc->dwCurrentPlayers;   /* ours is counted here */
    if ( (stored = DP_DuplicateSessionDesc( &desc, FALSE, FALSE )) )   /* stored WCHAR */
    {
        DPMSG_SETSESSIONDESC msg;
        /* as on the host: none while the session had or now has DPSESSION_NODATAMESSAGES [INF] */
        BOOL quiet = ( This->dp2->lpSessionDesc->dwFlags | stored->dwFlags ) & DPSESSION_NODATAMESSAGES;
        free( This->dp2->lpSessionDesc );
        This->dp2->lpSessionDesc = stored;
        memset( &msg, 0, sizeof(msg) );
        msg.dwType = DPSYS_SETSESSIONDESC;
        msg.dpDesc = desc;
        if ( !quiet )
            DP_QueueMessage( This, DPID_SYSMSG, DPID_ALLPLAYERS, 0, &msg, DP_CopySetSessionDescMsg, 0 );
        DP_NOTE( "SESSIONDESCCHANGED applied: the host set max %lu players\n", desc.dwMaxPlayers );
    }
    LeaveCriticalSection( &This->lock );
    return DP_OK;
}

HRESULT DP_LOBBY_HandleChat( IDirectPlayImpl *This, const void *body, DWORD size, const void *header )
{
    const DPSP_MSG_CHAT *m = body;
    const WCHAR *text;

    if ( size < sizeof(*m) || !(text = DP_WireString( body, size, m->messageOffset )) )
        return DPERR_GENERIC;
    EnterCriticalSection( &This->lock );
    if ( This->dp2->bConnectionOpen && DP_SenderOwns( This, header, m->fromId, "CHAT" ) )   /* no chat under another's name */
        DP_QueueChat( This, m->fromId, m->toId, m->flags, text );
    LeaveCriticalSection( &This->lock );
    return DP_OK;
}

/* ------------------------------------------------------------------ Windows port: Close() */
/* Forget the session - players, groups, who hosted it - so the object can host or join again
 * cleanly. (Wine kept them: a second Open inherited the first session's players.) */
/* the shortcut links (AddGroupToGroup) out of every group, while all groups still exist */
static void DP_DropShortcuts( struct GroupData *g, int depth )
{
    struct GroupList *sub, *next;

    for ( sub = DPQ_FIRST( g->groups ); sub; sub = next )
    {
        next = DPQ_NEXT( sub->groups );
        if ( DP_IS_CHILD_LINK( g, sub ) )
        {
            if ( depth < 64 ) DP_DropShortcuts( sub->lpGData, depth + 1 );
        }
        else
        {
            DPQ_REMOVE( g->groups, sub, groups );
            free( sub );
        }
    }
}

/* free a group and the groups made inside it (DP_DropShortcuts first: then every link left is a
 * child, and each group is exactly one group's child) */
static void DP_FreeGroupTree( struct GroupData *g, int depth )
{
    struct GroupList *sub;
    struct PlayerList *member;

    while ( (member = DPQ_FIRST( g->players )) )
    {
        DPQ_REMOVE( g->players, member, players );
        if ( member->lpPData->uRef > 1 ) member->lpPData->uRef--;
        free( member );
    }
    while ( (sub = DPQ_FIRST( g->groups )) )
    {
        DPQ_REMOVE( g->groups, sub, groups );
        if ( depth < 64 )
            DP_FreeGroupTree( sub->lpGData, depth + 1 );
        free( sub );
    }
    free( g->lpSPPlayerData );
    free( g->lpRemoteData );
    free( g->nameA );
    free( g->name );
    free( g );
}

void DP_ResetSession( IDirectPlayImpl *This )
{
    lpGroupData sys;
    struct GroupList *glist;
    struct PlayerList *plist;

#ifdef DPLAYX_TEST_SKIP_RESET   /* negative control for tests only: Wine's old behaviour */
    return;
#endif
    EnterCriticalSection( &This->lock );
    sys = This->dp2->lpSysGroup;
    if ( sys )
    {
        /* groups first (they hold references to players), each with the groups made inside it */
        DP_DropShortcuts( sys, 0 );
        while ( (glist = DPQ_FIRST( sys->groups )) )
        {
            DPQ_REMOVE( sys->groups, glist, groups );
            DP_FreeGroupTree( glist->lpGData, 0 );
            free( glist );
        }
        while ( (plist = DPQ_FIRST( sys->players )) )
        {
            plist->lpPData->uRef = 1;
            free( plist->lpPData->lpRemoteData );
            plist->lpPData->lpRemoteData = NULL;
            DP_DeletePlayer( This, plist->lpPData->dpid );
        }
        free( sys->lpSPPlayerData );
        free( sys->nameA );
        free( sys->name );
        free( sys );
        This->dp2->lpSysGroup = NULL;
    }
    This->dp2->bHostInterface = FALSE;
    This->dp2->systemPlayerId = 0;
    This->dp2->nameServerId = 0;
    This->dp2->hostMigrated = FALSE;
    if ( This->dp2->lpSessionDesc )
        This->dp2->lpSessionDesc->dwCurrentPlayers = 0;
    LeaveCriticalSection( &This->lock );
}
