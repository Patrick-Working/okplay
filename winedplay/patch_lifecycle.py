"""Apply the lifecycle wiring (leaving, keepalive, host migration) to port/dlls/dplayx. Run once."""
import sys

d = r"D:\aoe2tool\winedplay\port\dlls\dplayx" + "\\"
BS = "\\"


def edit(name, pairs):
    s = open(d + name, encoding="utf-8", newline="").read()
    for old, new, n in pairs:
        c = s.count(old)
        if c != n:
            sys.exit(f"{name}: expected {n} match(es), found {c}: {old[:90]!r}")
        s = s.replace(old, new)
    open(d + name, "w", encoding="utf-8", newline="").write(s)


edit("dplay.c", [
    # no DebugBreak inside a game
    ('    FIXME( "Why is this not the last reference to player?' + BS + 'n" );\n    DebugBreak();\n',
     '    ERR( "player 0x%08lx still has %lu references - removing it anyway' + BS + 'n", dpid, lpPList->lpPData->uRef );\n', 1),
    # DestroyPlayer: the full removal, including telling the other machines
    ("  /* FIXME: If the player is remote, we must be the host to delete this */\n\n  cbContext.This     = This;\n",
     "  /* FIXME: If the player is remote, we must be the host to delete this */\n\n"
     "  DP_RemovePlayer( This, idPlayer, TRUE );\n  LeaveCriticalSection( &This->lock );\n  return DP_OK;\n\n"
     "  cbContext.This     = This;\n", 1),
    # Close: say goodbye, stop the keepalive
    ('    TRACE( "(%p)' + BS + 'n", This );\n\n    EnterCriticalSection( &This->lock );\n\n    This->dp2->bConnectionOpen = FALSE;\n',
     '    TRACE( "(%p)' + BS + 'n", This );\n\n    DP_LIFE_Leaving( This );\n    DP_LIFE_Stop( This );\n\n'
     '    EnterCriticalSection( &This->lock );\n\n    This->dp2->bConnectionOpen = FALSE;\n', 1),
    # object destroyed
    ('     __wine_dbg_census( "DirectPlay object released" );\n#endif\n     DP_DestroyDirectPlay2( obj );\n',
     '     __wine_dbg_census( "DirectPlay object released" );\n#endif\n     DP_LIFE_Stop( obj );\n     DP_DestroyDirectPlay2( obj );\n', 1),
    # messages
    ("    case DPMSGCMD_ADDFORWARDACK:\n",
     "    case DPMSGCMD_DELETEPLAYER:\n      return DP_LIFE_HandleDeletePlayer( This, messageBody, dwMessageBodySize );\n\n"
     "    case DPMSGCMD_IAMNAMESERVER:\n      return DP_LIFE_HandleIAmNameServer( This, messageBody, dwMessageBodySize );\n\n"
     "    case DPMSGCMD_PINGREPLY:\n      if( dwMessageBodySize >= sizeof( DPSP_MSG_PING ) )\n"
     "        DP_LIFE_Heard( This, ((const DPSP_MSG_PING *)messageBody)->fromId );\n      break;\n\n"
     "    case DPMSGCMD_ADDFORWARDACK:\n", 1),
    ("      msg = (const DPSP_MSG_PING *)messageBody;\n\n      EnterCriticalSection( &This->lock );\n",
     "      msg = (const DPSP_MSG_PING *)messageBody;\n      DP_LIFE_Heard( This, msg->fromId );\n\n      EnterCriticalSection( &This->lock );\n", 1),
    ("  EnterCriticalSection( &This->lock );\n\n  hr = DP_QueueMessage( This, fromId, toId, 0, msg, DP_CopyGeneric, msgSize );\n",
     "  DP_LIFE_Heard( This, fromId );\n\n  EnterCriticalSection( &This->lock );\n\n"
     "  hr = DP_QueueMessage( This, fromId, toId, 0, msg, DP_CopyGeneric, msgSize );\n", 1),
    # session open -> keepalive, name server
    ("  EnterCriticalSection( &This->lock );\n\n  This->dp2->bConnectionOpen = TRUE;\n\n  LeaveCriticalSection( &This->lock );\n\n  return DP_OK;\n}",
     "  EnterCriticalSection( &This->lock );\n\n  This->dp2->bConnectionOpen = TRUE;\n  DP_LIFE_Start( This );\n\n"
     "  LeaveCriticalSection( &This->lock );\n\n  return DP_OK;\n}", 1),
])

HELPERS = r'''
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

/* Remove a player completely: the application hears DPSYS_DESTROYPLAYERORGROUP, the player
 * leaves its groups and the SP forgets it. tellOthers: it's ours - tell the other machines. */
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

    if ( !(p->dwFlags & DPLAYI_PLAYER_SYSPLAYER) )
    {
        DPMSG_DESTROYPLAYERORGROUP msg;

        memset( &msg, 0, sizeof(msg) );
        msg.dwType = DPSYS_DESTROYPLAYERORGROUP;
        msg.dwPlayerType = DPPLAYERTYPE_PLAYER;
        msg.dpId = idPlayer;
        if ( p->name ) msg.dpnName = *p->name;
        msg.dwFlags = p->dwFlags & ~(DPLAYI_PLAYER_PLAYERLOCAL | DPLAYI_PLAYER_SYSPLAYER);
        DP_QueueMessage( This, DPID_SYSMSG, DPID_ALLPLAYERS, idPlayer, &msg, DP_CopyDestroyPlayerOrGroup, 0 );
        if ( This->dp2->lpSessionDesc && This->dp2->lpSessionDesc->dwCurrentPlayers )
            This->dp2->lpSessionDesc->dwCurrentPlayers--;
    }

    if ( tellOthers && (p->dwFlags & DPLAYI_PLAYER_PLAYERLOCAL) && This->dp2->bConnectionOpen )
        DP_MSG_SendDeletePlayer( This, idPlayer );

    cbContext.This     = This;
    cbContext.idPlayer = idPlayer;
    cbContext.bAnsi    = FALSE;
    IDirectPlayX_EnumGroups( &This->IDirectPlay4_iface, NULL, cbDeletePlayerFromAllGroups, &cbContext,
                             DPENUMGROUPS_ALL );

    DP_DeletePlayer( This, idPlayer );

    if ( This->dp2->spData.lpCB->DeletePlayer )
    {
        DPSP_DELETEPLAYERDATA data;
        data.idPlayer = idPlayer;
        data.dwFlags = 0;
        data.lpISP = This->dp2->spData.lpISP;
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
'''
s = open(d + "dplay.c", encoding="utf-8", newline="").read()
if "Windows port: lifecycle helpers" not in s:
    open(d + "dplay.c", "w", encoding="utf-8", newline="").write(s + HELPERS)

edit("dplayx_messages.c", [
    ("        DP_SetPlayerSystemId( This, playerInfo.id, playerInfo.systemPlayerId );\n      }",
     "        DP_SetPlayerSystemId( This, playerInfo.id, playerInfo.systemPlayerId );\n"
     "        if ( playerInfo.flags & DPLAYI_PLAYER_NAMESRVR )\n"
     "          This->dp2->nameServerId = playerInfo.id;   /* the host's machine */\n      }", 1),
])

edit("sp_compat.c", [
    ("    HRESULT hr;\n    void *flat;\n\n    if ( This->dp2->spData.lpCB->SendEx )\n        return This->dp2->spData.lpCB->SendEx( data );\n",
     "    HRESULT hr;\n    void *flat;\n\n"
     "    /* after a host migration \"the name server\" (player 0) is the new host's machine; the SP\n"
     "     * only knows the original host, so address it by its system player */\n"
     "    if ( !data->idPlayerTo && This->dp2->hostMigrated && !This->dp2->bHostInterface &&\n"
     "         This->dp2->nameServerId )\n"
     "        data->idPlayerTo = This->dp2->nameServerId;\n\n"
     "    if ( This->dp2->spData.lpCB->SendEx )\n        return This->dp2->spData.lpCB->SendEx( data );\n", 1),
])
print("ok")
