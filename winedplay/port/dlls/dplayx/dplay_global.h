/*
 * Copyright 2000 Peter Hunnisett
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

#ifndef __WINE_DPLAY_GLOBAL_INCLUDED
#define __WINE_DPLAY_GLOBAL_INCLUDED

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "wine/dplaysp.h"
#include "wine/list.h"
#include "lobbysp.h"
#include "dplayx_queue.h"

extern HRESULT DPL_EnumAddress( LPDPENUMADDRESSCALLBACK lpEnumAddressCallback,
                                LPCVOID lpAddress, DWORD dwAddressSize,
                                LPVOID lpContext );

typedef struct
{
    struct list entry;

    DWORD flags;
    GUID spGuid;
    void *address;
    DWORD addressSize;
    DPNAME name;
    DPNAME nameA;
    DWORD reserved1;
    DWORD reserved2;
    char *path;
} DPCONNECTION;

typedef struct tagEnumSessionAsyncCallbackData
{
  LPSPINITDATA lpSpData;
  GUID         requestGuid;
  DWORD        dwEnumSessionFlags;
  DWORD        dwTimeout;
  WCHAR       *password;
  HANDLE       hSuicideRequest;
} EnumSessionAsyncCallbackData;

typedef struct tagDP_MSG_REPLY_STRUCT
{
  HANDLE hReceipt;
  WORD  *expectedReplies;
  DWORD  expectedReplyCount;
  LPVOID lpReplyMsg;
  DWORD  dwMsgBodySize;
  void  *replyMsgHeader;
} DP_MSG_REPLY_STRUCT, *LPDP_MSG_REPLY_STRUCT;

typedef struct tagDP_MSG_REPLY_STRUCT_LIST
{
  DPQ_ENTRY(tagDP_MSG_REPLY_STRUCT_LIST) repliesExpected;
  DP_MSG_REPLY_STRUCT replyExpected;
} DP_MSG_REPLY_STRUCT_LIST, *LPDP_MSG_REPLY_STRUCT_LIST;

struct PlayerData
{
  /* Individual player information */
  DPID dpid;

  DPNAME *name;
  DPNAME *nameA;
  HANDLE hEvent;

  ULONG  uRef;  /* What is the reference count on this data? */

  /* View of local data */
  LPVOID lpLocalData;
  DWORD  dwLocalDataSize;

  /* View of remote data */
  LPVOID lpRemoteData;
  DWORD  dwRemoteDataSize;

  /* SP data on a per player basis */
  LPVOID lpSPPlayerData;

  DWORD  dwFlags; /* Special remarks about the type of player */

  DPID systemPlayerId; /* the system player of the machine that owns this player */

  /* keepalive (remote system players): last time we heard from the machine, last ping sent */
  DWORD lastHeard, lastPing;
};
typedef struct PlayerData* lpPlayerData;

struct PlayerList
{
  DPQ_ENTRY(PlayerList) players;

  lpPlayerData lpPData;
};
typedef struct PlayerList* lpPlayerList;

struct GroupData
{
  /* Internal information */
  DPID parent; /* If parent == 0 it's a top level group */

  ULONG uRef; /* Reference count */

  DPQ_HEAD(GroupList)  groups;  /* A group has [0..n] groups */
  DPQ_HEAD(PlayerList) players; /* A group has [0..n] players */

  DPID idGroupOwner; /* ID of player who owns the group */

  DWORD dwFlags; /* Flags describing anything special about the group */

  DPID   dpid;
  DPNAME *name;
  DPNAME *nameA;

  /* View of local data */
  LPVOID lpLocalData;
  DWORD  dwLocalDataSize;

  /* View of remote data */
  LPVOID lpRemoteData;
  DWORD  dwRemoteDataSize;

  /* SP data on a per player basis */
  LPVOID lpSPPlayerData;

  /* created on this machine: DPGROUP_LOCAL in GetGroupFlags and the system messages - kept
   * apart from dwFlags, which goes out on the wire in the player list */
  BOOL bLocal;
};
typedef struct GroupData  GroupData;
typedef struct GroupData* lpGroupData;

struct GroupList
{
  DPQ_ENTRY(GroupList) groups;

  lpGroupData lpGData;
};
typedef struct GroupList* lpGroupList;

typedef DWORD FN_COPY_MESSAGE( DPMSG_GENERIC *genericDst, DPMSG_GENERIC *genericSrc,
                               DWORD genericSize, BOOL ansi );

struct DPMSG
{
  DPQ_ENTRY( DPMSG ) msgs;
  DPID fromId;
  DPID toId;
  DPMSG_GENERIC* msg;
  FN_COPY_MESSAGE *copyMessage;
  DWORD genericSize;
};
typedef struct DPMSG* LPDPMSG;

enum SPSTATE
{
  NO_PROVIDER = 0,
  DP_SERVICE_PROVIDER = 1,
  DP_LOBBY_PROVIDER = 2
};

/* Contains all data members. FIXME: Rename me */
typedef struct tagDirectPlay2Data
{
  BOOL   bConnectionOpen;

  /* For async EnumSessions requests */
  HANDLE hEnumSessionThread;
  HANDLE hKillEnumSessionThreadEvent;
  DWORD  dwEnumSessionLock;

  LPVOID lpNameServerData; /* DPlay interface doesn't know contents */

  BOOL bHostInterface; /* Did this interface create the session */

  lpGroupData lpSysGroup; /* System group with _everything_ in it */

  DPID systemPlayerId;

  LPDPSESSIONDESC2 lpSessionDesc;

  /* I/O Msg queues */
  DPQ_HEAD( DPMSG ) receiveMsgs; /* Msg receive queue */

  /* Information about the service provider active on this connection */
  SPINITDATA spData;
  GUID spGuid;   /* the provider spData.lpGuid points at (Wine pointed it at a stack variable) */
  BOOL       bSPInitialized;

  /* Information about the lobby server that's attached to this DP object */
  SPDATA_INIT dplspData;
  BOOL        bDPLSPInitialized;

  /* Our service provider */
  HMODULE hServiceProvider;

  /* Our DP lobby provider */
  HMODULE hDPLobbyProvider;

  enum SPSTATE connectionInitialized;

  /* Expected messages queue */
  DPQ_HEAD( tagDP_MSG_REPLY_STRUCT_LIST ) repliesExpected;

  /* lifecycle.c: who hosts (the name server's system player), keepalive thread */
  DPID   nameServerId;
  BOOL   hostMigrated;     /* the original host left and another machine took over */
  HANDLE hKeepAliveThread;
  DWORD dwKeepAliveThreadId;   /* its id (GetThreadId is Vista+) */
  HANDLE hKeepAliveStop;

  /* builtin.c: the connection in use is one of okplay's (Online/LAN Play) - game passwords apply */
  BOOL   okpConnection;

  /* IDirectPlay (DirectPlay 1) names sessions by a DWORD (DPSESSIONDESC.dwSession), not a GUID:
   * the instance GUIDs its EnumSessions has shown, numbered from 1 */
  GUID   dp1Sessions[ 64 ];
  DWORD  dp1SessionCount;
  int    groupFromWire;   /* applying another machine's group change: send nothing on (groups.c) */
} DirectPlay2Data;

typedef struct IDirectPlayImpl
{
  IDirectPlay IDirectPlay_iface;
  IDirectPlay2A IDirectPlay2A_iface;
  IDirectPlay2 IDirectPlay2_iface;
  IDirectPlay3A IDirectPlay3A_iface;
  IDirectPlay3 IDirectPlay3_iface;
  IDirectPlay4A IDirectPlay4A_iface;
  IDirectPlay4  IDirectPlay4_iface;
  LONG ref;
  CRITICAL_SECTION lock;
  DirectPlay2Data *dp2;
} IDirectPlayImpl;

HRESULT DP_HandleMessage( IDirectPlayImpl *This, void *messageBody,
        DWORD  dwMessageBodySize, void *messageHeader, WORD wCommandId, WORD wVersion,
        void **lplpReply, DWORD *lpdwMsgSize );
DPSESSIONDESC2 *DP_DuplicateSessionDesc( const DPSESSIONDESC2 *src, BOOL dstAnsi, BOOL srcAnsi );
HRESULT DP_HandleGameMessage( IDirectPlayImpl *This, void *messageBody, DWORD messageBodySize,
                              DPID fromId, DPID toId );
HRESULT DP_CreatePlayer( IDirectPlayImpl *This, void *msgHeader, DPID *lpid, DPNAME *lpName,
                         void *data, DWORD dataSize, void *spData, DWORD spDataSize, DWORD dwFlags,
                         HANDLE hEvent, struct PlayerData **playerData, BOOL bAnsi );
HRESULT DP_CreateGroup( IDirectPlayImpl *This, void *msgHeader, const DPID *lpid,
                        const DPNAME *lpName, void *data, DWORD dataSize, DWORD dwFlags,
                        DPID idParent, BOOL bAnsi );
HRESULT DP_AddPlayerToGroup( IDirectPlayImpl *This, DPID group, DPID player );

void DP_FreeConnections(void);

/* DP SP external interfaces into DirectPlay */
extern HRESULT DP_GetSPPlayerData( IDirectPlayImpl *lpDP, DPID idPlayer, void **lplpData );
extern HRESULT DP_SetSPPlayerData( IDirectPlayImpl *lpDP, DPID idPlayer, void *lpData );

/* DP external interfaces to call into DPSP interface */
extern LPVOID DPSP_CreateSPPlayerData(void);

extern HRESULT dplay_create( REFIID riid, void **ppv );
extern HRESULT dplobby_create( REFIID riid, void **ppv );
extern HRESULT dplaysp_create( REFIID riid, void **ppv, IDirectPlayImpl *dp );
extern HRESULT dplobbysp_create( REFIID riid, void **ppv, IDirectPlayImpl *dp );

/* sp_compat.c: send through SPs that lack the DirectX 6 SendEx/SendToGroupEx callbacks */
#ifndef DPID_NOPARENT_GROUP
#define DPID_NOPARENT_GROUP 0
#endif
#define DPID_SYSTEM_GROUP DPID_NOPARENT_GROUP
struct GroupData *DP_FindAnyGroup( IDirectPlayImpl *This, DPID dpid );
HRESULT DP_SP_SendEx( IDirectPlayImpl *This, DPSP_SENDEXDATA *data );
HRESULT DP_SP_SendToGroupEx( IDirectPlayImpl *This, DPSP_SENDTOGROUPEXDATA *data );

/* builtin.c: the drop-in edition (okplay's connections and the intro-video decoder built in) */
void DP_BUILTIN_Attach( HINSTANCE hinst );
void DP_BUILTIN_Detach( void );
void __wine_dbg_census( const char *why );   /* shim: DirectPlay calls made so far -> log */
void DP_BUILTIN_AddConnections( struct list *connections );
BOOL DP_BUILTIN_IsOurs( const GUID *guid );
BOOL DP_BUILTIN_Remap( GUID *sp, void **address, DWORD *size );   /* okplay.ini classic_tcpip/classic_ipx/... */
int  DP_BUILTIN_RemapMode( const GUID *sp );                         /* 0 Windows', 1 Online, 2 LAN, 3 ask (pending) */
BOOL DP_BUILTIN_Address( const GUID *sp, void **address, DWORD *size );
BOOL DP_BUILTIN_OurAddress( BOOL lan, void **address, DWORD *size );
BOOL DP_BUILTIN_IsLan( const GUID *sp );
void DP_BUILTIN_RemapForget( const GUID *sp );
BOOL DP_BUILTIN_ShowWindowsConnections( void );
/* OKP_TEST_LOBBY=host:<session> | join:<session> (tests only): the game is told a lobby launched it,
 * over LAN Play. NULL = not set. A join names the session; its instance GUID is GUID_NULL. */
const DPLCONNECTION *DP_BUILTIN_TestLobby( BOOL unicode );
/* game passwords (okplay connections only): the typed password never goes on the wire - both
 * sides hash it with the session's instance GUID and DirectPlay compares the hashes */
BOOL DP_BUILTIN_AskHostPassword( const GUID *instance, WCHAR *hashed, DWORD size );   /* TRUE = set */
HRESULT DP_BUILTIN_AskJoinPassword( const GUID *instance, WCHAR *hashed, DWORD size ); /* DP_OK/USERCANCEL */
/* the host runs another build of the game (its exe's fingerprint differs): warn, Join anyway / Cancel
 * (okplay.ini build_check=ask|off) - DP_OK or DPERR_USERCANCEL */
HRESULT DP_BUILTIN_CheckBuild( IDirectPlaySP *isp, const GUID *instance );
void DP_BUILTIN_JoinFailed( HRESULT hr );
void DP_BUILTIN_OfferUpdate( const char *latest, const char *note_utf8 );   /* from okplay rdv.c */
void DP_BUILTIN_NetworkUnavailable( int err );   /* from okplay net.c/lan.c: no TCP/IP */
void DP_BUILTIN_ServerUnreachable( const char *server, const char *why );   /* from okplay rdv.c */

/* important session events: always in the drop-in's session log, a trace otherwise */
#ifdef OKP_BUILTIN
extern void okp_log( int level, const char *fmt, ... );
#define DP_NOTE( ... ) okp_log( 1, "dplay: " __VA_ARGS__ )
#else
#define DP_NOTE( ... ) TRACE( __VA_ARGS__ )
#endif

/* dplay.c helpers for the lifecycle code */
void DP_RemovePlayer( IDirectPlayImpl *This, DPID id, BOOL tellOthers );
DWORD DP_SPDeleteFlags( DWORD playerFlags );   /* a provider DeletePlayer's flags, as Windows' */
void DP_BumpObjectIds( DPID above );
void DP_LimitObjectIds( DWORD limit );
void DP_QueueSystemMessage( IDirectPlayImpl *This, DWORD type );
void DP_ResetSession( IDirectPlayImpl *This );

/* lifecycle.c: leaving, keepalive, host migration */
void DP_LIFE_Start( IDirectPlayImpl *This );
void DP_LIFE_Stop( IDirectPlayImpl *This );
void DP_LIFE_Leaving( IDirectPlayImpl *This );
void DP_LIFE_Heard( IDirectPlayImpl *This, DPID anyPlayerOfTheMachine );
void DP_LIFE_MachineGone( IDirectPlayImpl *This, DPID systemPlayerId, const char *why );
HRESULT DP_LIFE_HandleDeletePlayer( IDirectPlayImpl *This, const void *body, DWORD size, const void *header );
HRESULT DP_LIFE_HandleIAmNameServer( IDirectPlayImpl *This, const void *body, DWORD size, const void *header );
HRESULT DP_MSG_SendDeletePlayer( IDirectPlayImpl *This, DPID playerId );
void DP_SendToAllMachines( IDirectPlayImpl *This, void *msg, DWORD size, const void *extra, DWORD extraSize );
const WCHAR *DP_WireString( const void *body, DWORD size, DWORD offset );
void DP_QueueGroupCreatedFromWire( IDirectPlayImpl *This, DPID id, void *data, DWORD dataSize, DWORD flags, DPID parent );
/* groups.c: group changes between machines */
void DP_GRP_SendIds( IDirectPlayImpl *This, WORD cmd, DPID player, DPID group );
void DP_GRP_SendCreate( IDirectPlayImpl *This, DPID id, DPID parent, DWORD flags, const DPNAME *name,
                        const void *data, DWORD dataSize );
void DP_GRP_SendName( IDirectPlayImpl *This, DPID id, const DPNAME *name );
void DP_GRP_SendData( IDirectPlayImpl *This, DPID id, const void *data, DWORD dataSize );
HRESULT DP_GRP_HandleCreate( IDirectPlayImpl *This, const void *body, DWORD size, void *header );
HRESULT DP_GRP_HandleIds( IDirectPlayImpl *This, WORD cmd, const void *body, DWORD size, void *header );
HRESULT DP_GRP_HandleName( IDirectPlayImpl *This, const void *body, DWORD size, void *header );

/* dplay.c, Windows port: lobby changes reach every machine */
void DP_LOBBY_PlayerNameChanged( IDirectPlayImpl *This, DPID id );
void DP_LOBBY_PlayerDataChanged( IDirectPlayImpl *This, DPID id );
void DP_LOBBY_SessionDescChanged( IDirectPlayImpl *This );
void DP_LOBBY_SessionDescChangedEx( IDirectPlayImpl *This, BOOL tellLocalPlayers );
HRESULT DP_LOBBY_SendChat( IDirectPlayImpl *This, DPID from, DPID to, DWORD flags, DPCHAT *chat, BOOL ansi );
HRESULT DP_LOBBY_HandlePlayerNameChanged( IDirectPlayImpl *This, const void *body, DWORD size, const void *header );
HRESULT DP_LOBBY_HandlePlayerDataChanged( IDirectPlayImpl *This, const void *body, DWORD size, const void *header );
HRESULT DP_LOBBY_HandleSessionDescChanged( IDirectPlayImpl *This, const void *body, DWORD size, const void *header );
HRESULT DP_LOBBY_HandleChat( IDirectPlayImpl *This, const void *body, DWORD size, const void *header );

/* dplay.c, Windows port: who sent a received message. The Online/LAN Play SP header names the sending
 * machine by peer id, and the transport delivers only packets that prove the sender holds that peer's
 * token, so a message that acts on a player is honoured only from that player's own machine, and a
 * host-only message only from the host. DP_SenderMachine returns the sender's system player (0 if the
 * SP data doesn't identify it; *known FALSE when the SP tells us nothing, e.g. another provider).
 * DP_SenderOwns: may the sender act for `owner` (the player itself or one of its machine's players)?
 * It returns TRUE when the provider gives us no identity, so classic providers behave exactly as before. */
DPID DP_SenderMachine( IDirectPlayImpl *This, const void *header, BOOL *known );
BOOL DP_SenderOwns( IDirectPlayImpl *This, const void *header, DPID owner, const char *what );

/* host.c: accepting joiners */
HRESULT DP_HOST_HandleForwardAddPlayer( IDirectPlayImpl *This, void *body, DWORD bodySize,
                                        void *header, void **reply, DWORD *replySize );
BOOL DP_PlayerExists( IDirectPlayImpl *This, DPID id );
void DP_SetPlayerSystemId( IDirectPlayImpl *This, DPID id, DPID systemPlayerId );
BOOL DP_AddAddressTerminator( void **address, DWORD *size );
void DP_QuerySPCaps( IDirectPlayImpl *This );

#endif /* __WINE_DPLAY_GLOBAL_INCLUDED */
