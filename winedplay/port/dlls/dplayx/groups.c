/*
 * Groups between machines.
 *
 * Wine's dplayx kept groups on the machine that made them: no other machine heard of a group, its
 * players, its name or data, and a joiner numbered its groups itself (ids that could clash with the
 * host's). Windows sends every change to every machine (S3 wire tap, dptest --groups, 2026-10-28):
 *
 *   CreateGroup / CreateGroupInGroup  a joiner first asks the host for an id (REQUESTGROUPID 6,
 *                     12 bytes, the creation flags; the host answers REQUESTPLAYERREPLY 7), then
 *                     CREATEGROUP 9: CREATEPLAYER's layout, the packed group with flags 0x48
 *                     (DPLAYI_GROUP_DPLAYOWNS | DPLAYI_PLAYER_PLAYERLOCAL), the creating machine's
 *                     system player, the parent for a group in a group, 6 reserved bytes at the end
 *   AddPlayerToGroup / DeletePlayerFromGroup   ADDPLAYERTOGROUP 13 / DELETEPLAYERFROMGROUP 14
 *   DestroyGroup      DELETEPLAYERFROMGROUP for each player in it, then DELETEGROUP 12
 *   AddGroupToGroup / DeleteGroupFromGroup     ADDSHORTCUTTOGROUP 39 / DELETEGROUPFROMGROUP 40,
 *                     the group in the player field and the parent in the group field
 *   SetGroupName / SetGroupData (remote)       GROUPNAMECHANGED 18 / GROUPDATACHANGED 17
 *
 * The fixed ones are 28 bytes: envelope, to (0), player, group, two offsets (0). Receivers apply the
 * change through the same code a local call uses, with This->dp2->groupFromWire set so nothing is
 * sent on again, and their players get the same system messages as for a local change.
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

WINE_DEFAULT_DEBUG_CHANNEL(dplay);

#define GROUP_PACKED_FLAGS  ( DPLAYI_GROUP_DPLAYOWNS | DPLAYI_PLAYER_PLAYERLOCAL )   /* 0x48, Windows */
#define GROUP_STAGING_BIT   0x00000200   /* DPLAYI_GROUP_STAGINGAREA [UNV on the wire: only plain groups captured] */

/* may a group change go out now? (a session with other machines, and not a change that came in) */
static BOOL grp_may_send( IDirectPlayImpl *This )
{
    return This->dp2->bConnectionOpen && This->dp2->lpSysGroup && !This->dp2->groupFromWire;
}

static void grp_envelope( DPMSG_SENDENVELOPE *e, WORD cmd )
{
    e->dwMagic = DPMSGMAGIC_DPLAYMSG;
    e->wCommandId = cmd;
    e->wVersion = DPMSGVER_DP6;
}

/* the 28-byte ones: DELETEGROUP, ADD/DELETEPLAYERTOGROUP, ADDSHORTCUTTOGROUP, DELETEGROUPFROMGROUP */
void DP_GRP_SendIds( IDirectPlayImpl *This, WORD cmd, DPID player, DPID group )
{
    DPSP_MSG_DELETEPLAYER msg = { 0 };   /* the same layout */

    EnterCriticalSection( &This->lock );
    if ( grp_may_send( This ) )
    {
        grp_envelope( &msg.envelope, cmd );
        msg.playerId = player;
        msg.groupId = group;
        TRACE( "group message %u: player 0x%08lx group 0x%08lx\n", cmd, player, group );
        DP_SendToAllMachines( This, &msg, sizeof(msg), NULL, 0 );
    }
    LeaveCriticalSection( &This->lock );
}

static DWORD grp_wstr_size( const WCHAR *s )
{
    return s ? ( lstrlenW( s ) + 1 ) * sizeof(WCHAR) : 0;
}

void DP_GRP_SendCreate( IDirectPlayImpl *This, DPID id, DPID parent, DWORD flags, const DPNAME *name,
                        const void *data, DWORD dataSize )
{
    DPMSG_CREATESESSION *msg;   /* CREATEPLAYER's header (Wine's name for command 8) */
    DPLAYI_PACKEDPLAYER *packed;
    DWORD ns, ls, size;
    BYTE *p;

    EnterCriticalSection( &This->lock );
    if ( !grp_may_send( This ) )
    {
        LeaveCriticalSection( &This->lock );
        return;
    }
    ns = name ? grp_wstr_size( name->lpszShortName ) : 0;
    ls = name ? grp_wstr_size( name->lpszLongName ) : 0;
    size = sizeof(*msg) + sizeof(*packed) + ns + ls + dataSize + 6;
    if ( !(p = calloc( 1, size )) )
    {
        LeaveCriticalSection( &This->lock );
        return;
    }
    msg = (DPMSG_CREATESESSION *)p;
    grp_envelope( &msg->envelope, DPMSGCMD_CREATEGROUP );
    msg->playerId = id;
    msg->createOffset = sizeof(*msg);
    packed = (DPLAYI_PACKEDPLAYER *)(msg + 1);
    packed->size = sizeof(*packed) + ns + ls + dataSize;
    packed->flags = GROUP_PACKED_FLAGS | ((flags & DPGROUP_HIDDEN) ? DPLAYI_GROUP_HIDDEN : 0) |
                    ((flags & DPGROUP_STAGINGAREA) ? GROUP_STAGING_BIT : 0);
    packed->id = id;
    packed->shortNameLength = ns;
    packed->longNameLength = ls;
    packed->playerDataLength = dataSize;
    packed->systemPlayerId = This->dp2->systemPlayerId;
    packed->fixedSize = sizeof(*packed);
    packed->version = DPMSGVER_DP6;
    packed->parentId = parent == DPID_NOPARENT_GROUP ? 0 : parent;
    p = (BYTE *)(packed + 1);
    if ( ns ) memcpy( p, name->lpszShortName, ns );
    p += ns;
    if ( ls ) memcpy( p, name->lpszLongName, ls );
    p += ls;
    if ( dataSize ) memcpy( p, data, dataSize );
    TRACE( "CREATEGROUP 0x%08lx parent 0x%08lx, %lu bytes\n", id, parent, size );
    DP_SendToAllMachines( This, msg, size, NULL, 0 );
    free( msg );
    LeaveCriticalSection( &This->lock );
}

void DP_GRP_SendName( IDirectPlayImpl *This, DPID id, const DPNAME *name )
{
    DPSP_MSG_GROUPNAMECHANGED *msg;
    DWORD ns, ls, size;

    EnterCriticalSection( &This->lock );
    if ( !grp_may_send( This ) )
    {
        LeaveCriticalSection( &This->lock );
        return;
    }
    ns = name ? grp_wstr_size( name->lpszShortName ) : 0;
    ls = name ? grp_wstr_size( name->lpszLongName ) : 0;
    size = sizeof(*msg) + ns + ls;
    if ( (msg = calloc( 1, size )) )
    {
        grp_envelope( &msg->envelope, DPMSGCMD_GROUPNAMECHANGED );
        msg->groupId = id;
        msg->shortNameOffset = ns ? sizeof(*msg) : 0;
        msg->longNameOffset = ls ? sizeof(*msg) + ns : 0;
        if ( ns ) memcpy( (BYTE *)(msg + 1), name->lpszShortName, ns );
        if ( ls ) memcpy( (BYTE *)(msg + 1) + ns, name->lpszLongName, ls );
        DP_SendToAllMachines( This, msg, size, NULL, 0 );
        free( msg );
    }
    LeaveCriticalSection( &This->lock );
}

void DP_GRP_SendData( IDirectPlayImpl *This, DPID id, const void *data, DWORD dataSize )
{
    DPSP_MSG_GROUPDATACHANGED *msg;
    DWORD size;

    EnterCriticalSection( &This->lock );
    if ( !grp_may_send( This ) )
    {
        LeaveCriticalSection( &This->lock );
        return;
    }
    size = sizeof(*msg) + dataSize;
    if ( (msg = calloc( 1, size )) )
    {
        grp_envelope( &msg->envelope, DPMSGCMD_GROUPDATACHANGED );
        msg->groupId = id;
        msg->dataSize = dataSize;
        msg->dataOffset = sizeof(*msg);
        if ( dataSize ) memcpy( msg + 1, data, dataSize );
        DP_SendToAllMachines( This, msg, size, NULL, 0 );
        free( msg );
    }
    LeaveCriticalSection( &This->lock );
}

/* ---- receiving */

HRESULT DP_GRP_HandleCreate( IDirectPlayImpl *This, const void *body, DWORD size, void *header )
{
    const DPMSG_CREATESESSION *msg = body;
    DPPLAYERINFO info;
    DWORD offset = sizeof(*msg), flags = 0;
    DPID id, parent;
    HRESULT hr;

    if ( size < sizeof(*msg) )
        return DPERR_GENERIC;
    memset( &info, 0, sizeof(info) );
    hr = DP_MSG_ReadPackedPlayer( (char *)body, &offset, size, &info );
    if ( FAILED( hr ) )
        return hr;
    /* a machine announces its own groups only */
    if ( info.systemPlayerId && !DP_SenderOwns( This, header, info.systemPlayerId, "CREATEGROUP" ) )
        return DP_OK;
    if ( info.flags & DPLAYI_GROUP_HIDDEN ) flags |= DPGROUP_HIDDEN;
    if ( info.flags & GROUP_STAGING_BIT ) flags |= DPGROUP_STAGINGAREA;
    id = msg->playerId;
    parent = info.parentId ? info.parentId : DPID_NOPARENT_GROUP;

    EnterCriticalSection( &This->lock );
    if ( This->dp2->bConnectionOpen && !DP_FindAnyGroup( This, id ) )
    {
        hr = DP_CreateGroup( This, header, &id, &info.name, info.playerData, info.playerDataLength,
                             flags, parent, FALSE );
        if ( SUCCEEDED( hr ) )
            DP_QueueGroupCreatedFromWire( This, id, info.playerData, info.playerDataLength, flags, parent );
        else
            WARN( "CREATEGROUP 0x%08lx (parent 0x%08lx): 0x%08lx\n", id, parent, hr );
    }
    LeaveCriticalSection( &This->lock );
    return DP_OK;
}

HRESULT DP_GRP_HandleIds( IDirectPlayImpl *This, WORD cmd, const void *body, DWORD size, void *header )
{
    const DPSP_MSG_DELETEPLAYER *msg = body;
    IDirectPlay4 *dp = &This->IDirectPlay4_iface;
    HRESULT hr = DP_OK;

    if ( size < sizeof(*msg) )
        return DPERR_GENERIC;

    EnterCriticalSection( &This->lock );
    if ( !This->dp2->bConnectionOpen )
    {
        LeaveCriticalSection( &This->lock );
        return DP_OK;
    }
    This->dp2->groupFromWire++;
    switch ( cmd )
    {
    case DPMSGCMD_ADDPLAYERTOGROUP:     hr = DP_AddPlayerToGroup( This, msg->groupId, msg->playerId ); break;
    case DPMSGCMD_DELETEPLAYERFROMGROUP: hr = IDirectPlayX_DeletePlayerFromGroup( dp, msg->groupId, msg->playerId ); break;
    case DPMSGCMD_DELETEGROUP:          hr = IDirectPlayX_DestroyGroup( dp, msg->groupId ); break;
    case DPMSGCMD_ADDSHORTCUTTOGROUP:   hr = IDirectPlayX_AddGroupToGroup( dp, msg->groupId, msg->playerId ); break;
    case DPMSGCMD_DELETEGROUPFROMGROUP: hr = IDirectPlayX_DeleteGroupFromGroup( dp, msg->groupId, msg->playerId ); break;
    }
    This->dp2->groupFromWire--;
    LeaveCriticalSection( &This->lock );
    if ( FAILED( hr ) )
        TRACE( "group message %u (player 0x%08lx, group 0x%08lx): 0x%08lx\n", cmd, msg->playerId, msg->groupId, hr );
    return DP_OK;
}

HRESULT DP_GRP_HandleName( IDirectPlayImpl *This, const void *body, DWORD size, void *header )
{
    const DPSP_MSG_GROUPNAMECHANGED *msg = body;
    DPNAME name;

    if ( size < sizeof(*msg) )
        return DPERR_GENERIC;
    memset( &name, 0, sizeof(name) );
    name.dwSize = sizeof(name);
    name.lpszShortName = (WCHAR *)DP_WireString( body, size, msg->shortNameOffset );
    name.lpszLongName = (WCHAR *)DP_WireString( body, size, msg->longNameOffset );

    EnterCriticalSection( &This->lock );
    if ( This->dp2->bConnectionOpen )
    {
        This->dp2->groupFromWire++;
        IDirectPlayX_SetGroupName( &This->IDirectPlay4_iface, msg->groupId, &name, 0 );
        This->dp2->groupFromWire--;
    }
    LeaveCriticalSection( &This->lock );
    return DP_OK;
}
