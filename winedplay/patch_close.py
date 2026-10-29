"""Close() tears the session down (so the object can host/join again), and calls made with no
session answer like Windows (apistate.exe measures this). Run once."""
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


edit("dplay_global.h", [
    ("void DP_QueueSystemMessage( IDirectPlayImpl *This, DWORD type );\n",
     "void DP_QueueSystemMessage( IDirectPlayImpl *This, DWORD type );\n"
     "void DP_ResetSession( IDirectPlayImpl *This );\n", 1),
])

edit("dplay.c", [
    # no DebugBreak inside a game (groups)
    ('    FIXME( "Why is this not the last reference to group?' + BS + 'n" );\n    DebugBreak();\n',
     '    ERR( "group 0x%08lx still has %lu references - removing it anyway' + BS + 'n", dpid, lpGList->lpGData->uRef );\n', 1),
    # Close: like Windows when there is nothing to close; forget the session afterwards
    ('    TRACE( "(%p)' + BS + 'n", This );\n\n    DP_LIFE_Leaving( This );\n    DP_LIFE_Stop( This );\n',
     '    TRACE( "(%p)' + BS + 'n", This );\n\n'
     "    if ( This->dp2->connectionInitialized == NO_PROVIDER )\n        return DPERR_UNINITIALIZED;\n"
     "    if ( !This->dp2->bConnectionOpen )\n        return DPERR_NOSESSIONS;\n\n"
     "    DP_LIFE_Leaving( This );\n    DP_LIFE_Stop( This );\n", 1),
    ('        TRACE( "Calling SP Close (obsolete interface)' + BS + 'n" );\n        hr = (*This->dp2->spData.lpCB->Close)();\n    }\n\n    return hr;\n}',
     '        TRACE( "Calling SP Close (obsolete interface)' + BS + 'n" );\n        hr = (*This->dp2->spData.lpCB->Close)();\n    }\n\n'
     "    DP_ResetSession( This );   /* a later Open starts clean */\n\n    return hr;\n}", 1),
    # no session: NOSESSIONS, like Windows
    ("  if( ( lpData == NULL ) && ( lpdwDataSize == NULL ) )\n  {\n    return DPERR_INVALIDPARAMS;\n  }\n\n  EnterCriticalSection( &This->lock );\n\n  dwRequiredSize",
     "  if( ( lpData == NULL ) && ( lpdwDataSize == NULL ) )\n  {\n    return DPERR_INVALIDPARAMS;\n  }\n\n"
     "  if( !This->dp2->bConnectionOpen )\n  {\n    return DPERR_NOSESSIONS;\n  }\n\n"
     "  EnterCriticalSection( &This->lock );\n\n  dwRequiredSize", 1),
    ("  if( dwFlags )\n  {\n    return DPERR_INVALIDPARAMS;\n  }\n\n  /* Only the host is allowed to update the session desc */\n",
     "  if( dwFlags )\n  {\n    return DPERR_INVALIDPARAMS;\n  }\n\n"
     "  if( !bInitial && !This->dp2->bConnectionOpen )\n  {\n    return DPERR_NOSESSIONS;\n  }\n\n"
     "  /* Only the host is allowed to update the session desc */\n", 1),
    ("static HRESULT WINAPI IDirectPlay4AImpl_EnumPlayers( IDirectPlay4A *iface, GUID *instance,\n        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )\n{\n",
     "static HRESULT WINAPI IDirectPlay4AImpl_EnumPlayers( IDirectPlay4A *iface, GUID *instance,\n        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )\n{\n"
     "    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );\n"
     "    if ( This->dp2->connectionInitialized != NO_PROVIDER && !This->dp2->bConnectionOpen &&\n"
     "         !(flags & DPENUMPLAYERS_SESSION) )\n        return DPERR_NOSESSIONS;\n", 1),
    ("static HRESULT WINAPI IDirectPlay4Impl_EnumPlayers( IDirectPlay4 *iface, GUID *instance,\n        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )\n{\n",
     "static HRESULT WINAPI IDirectPlay4Impl_EnumPlayers( IDirectPlay4 *iface, GUID *instance,\n        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )\n{\n"
     "    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );\n"
     "    if ( This->dp2->connectionInitialized != NO_PROVIDER && !This->dp2->bConnectionOpen &&\n"
     "         !(flags & DPENUMPLAYERS_SESSION) )\n        return DPERR_NOSESSIONS;\n", 1),
    ("static HRESULT WINAPI IDirectPlay4Impl_EnumGroups ( IDirectPlay4 *iface, GUID *instance,\n        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )\n{\n",
     "static HRESULT WINAPI IDirectPlay4Impl_EnumGroups ( IDirectPlay4 *iface, GUID *instance,\n        LPDPENUMPLAYERSCALLBACK2 enumplayercb, void *context, DWORD flags )\n{\n"
     "    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );\n"
     "    if ( This->dp2->connectionInitialized != NO_PROVIDER && !This->dp2->bConnectionOpen &&\n"
     "         !(flags & DPENUMGROUPS_SESSION) )\n        return DPERR_NOSESSIONS;\n", 1),
    # unknown players
    ("    if( caps->dwSize != sizeof(DPCAPS) )\n        return DPERR_INVALIDPARAMS;\n\n    /* Query the service provider */\n",
     "    if( caps->dwSize != sizeof(DPCAPS) )\n        return DPERR_INVALIDPARAMS;\n\n"
     "    /* player 0 = the session in general (dplayx asks that itself) */\n"
     "    if ( player && !DP_PlayerExists( This, player ) )\n        return DPERR_INVALIDPLAYER;\n\n"
     "    /* Query the service provider */\n", 1),
    ("  if( ( lpPList = DP_FindPlayer( This, idPlayer ) ) == NULL )\n  {\n    LeaveCriticalSection( &This->lock );\n    return DPERR_INVALIDGROUP;\n  }\n\n  name = DP_DuplicateName( lpPlayerName, FALSE, bAnsi );",
     "  if( ( lpPList = DP_FindPlayer( This, idPlayer ) ) == NULL )\n  {\n    LeaveCriticalSection( &This->lock );\n    return DPERR_INVALIDPLAYER;\n  }\n\n  name = DP_DuplicateName( lpPlayerName, FALSE, bAnsi );", 1),
])

RESET = r'''
/* ------------------------------------------------------------------ Windows port: Close() */
/* Forget the session - players, groups, who hosted it - so the object can host or join again
 * cleanly. (Wine kept them: a second Open inherited the first session's players.) */
void DP_ResetSession( IDirectPlayImpl *This )
{
    lpGroupData sys;
    struct GroupList *glist;
    struct PlayerList *plist;

    EnterCriticalSection( &This->lock );
    sys = This->dp2->lpSysGroup;
    if ( sys )
    {
        /* groups first (they hold references to players) */
        while ( (glist = DPQ_FIRST( sys->groups )) )
        {
            struct GroupData *g = glist->lpGData;
            struct GroupList *sub;
            struct PlayerList *member;

            DPQ_REMOVE( sys->groups, glist, groups );
            while ( (member = DPQ_FIRST( g->players )) )
            {
                DPQ_REMOVE( g->players, member, players );
                if ( member->lpPData->uRef > 1 ) member->lpPData->uRef--;
                free( member );
            }
            while ( (sub = DPQ_FIRST( g->groups )) )   /* shortcuts to other groups */
            {
                DPQ_REMOVE( g->groups, sub, groups );
                free( sub );
            }
            free( g->lpSPPlayerData );
            free( g->lpRemoteData );
            free( g->nameA );
            free( g->name );
            free( g );
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
'''
s = open(d + "dplay.c", encoding="utf-8", newline="").read()
if "Windows port: Close()" not in s:
    open(d + "dplay.c", "w", encoding="utf-8", newline="").write(s + RESET)
print("ok")
