"""Apply the lobby-changes work to port/dlls/dplayx: player names/data, session description and chat
reach every machine; the session description is always stored ANSI. Run once."""
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


edit("dplayx_messages.h", [
    ("#define DPMSGCMD_IAMNAMESERVER        53 /* host migration: the sender is the new host */\n",
     "#define DPMSGCMD_IAMNAMESERVER        53 /* host migration: the sender is the new host */\n"
     "#define DPMSGCMD_PLAYERDATACHANGED    15\n"
     "#define DPMSGCMD_PLAYERNAMECHANGED    16\n"
     "#define DPMSGCMD_SESSIONDESCCHANGED   26\n"
     "#define DPMSGCMD_CHAT                 45\n", 1),
    ("  DWORD spDataSize; /* followed by the host's SP data */\n} DPSP_MSG_IAMNAMESERVER;\n",
     "  DWORD spDataSize; /* followed by the host's SP data */\n} DPSP_MSG_IAMNAMESERVER;\n\n"
     "/* offsets are from the start of the message (the envelope); strings are WCHAR, NUL-terminated */\n"
     "typedef struct\n{\n  DPMSG_SENDENVELOPE envelope;\n  DPID toId;\n  DPID playerId;\n  DWORD dataSize;\n"
     "  DWORD dataOffset;\n} DPSP_MSG_PLAYERDATACHANGED;\n\n"
     "typedef struct\n{\n  DPMSG_SENDENVELOPE envelope;\n  DPID toId;\n  DPID playerId;\n  DWORD shortNameOffset;\n"
     "  DWORD longNameOffset;\n} DPSP_MSG_PLAYERNAMECHANGED;\n\n"
     "typedef struct\n{\n  DPMSG_SENDENVELOPE envelope;\n  DPID toId;\n  DWORD sessionNameOffset;\n"
     "  DWORD passwordOffset;\n  DPSESSIONDESC2 desc;\n} DPSP_MSG_SESSIONDESCCHANGED;\n\n"
     "typedef struct\n{\n  DPMSG_SENDENVELOPE envelope;\n  DPID fromId;\n  DPID toId;\n  DWORD flags;\n"
     "  DWORD messageOffset;\n} DPSP_MSG_CHAT;\n", 1),
])

edit("dplay_global.h", [
    ("HRESULT DP_MSG_SendDeletePlayer( IDirectPlayImpl *This, DPID playerId );\n",
     "HRESULT DP_MSG_SendDeletePlayer( IDirectPlayImpl *This, DPID playerId );\n"
     "void DP_SendToAllMachines( IDirectPlayImpl *This, void *msg, DWORD size, const void *extra, DWORD extraSize );\n"
     "\n/* dplay.c, Windows port: lobby changes reach every machine */\n"
     "void DP_LOBBY_PlayerNameChanged( IDirectPlayImpl *This, DPID id );\n"
     "void DP_LOBBY_PlayerDataChanged( IDirectPlayImpl *This, DPID id );\n"
     "void DP_LOBBY_SessionDescChanged( IDirectPlayImpl *This );\n"
     "HRESULT DP_LOBBY_SendChat( IDirectPlayImpl *This, DPID from, DPID to, DWORD flags, DPCHAT *chat, BOOL ansi );\n"
     "HRESULT DP_LOBBY_HandlePlayerNameChanged( IDirectPlayImpl *This, const void *body, DWORD size );\n"
     "HRESULT DP_LOBBY_HandlePlayerDataChanged( IDirectPlayImpl *This, const void *body, DWORD size );\n"
     "HRESULT DP_LOBBY_HandleSessionDescChanged( IDirectPlayImpl *This, const void *body, DWORD size );\n"
     "HRESULT DP_LOBBY_HandleChat( IDirectPlayImpl *This, const void *body, DWORD size );\n", 1),
])

edit("lifecycle.c", [
    ("/* one message to every other machine */\nstatic void send_to_all_machines( IDirectPlayImpl *This, void *msg, DWORD size, const void *extra, DWORD extraSize )\n{",
     "/* one message to every other machine */\nvoid DP_SendToAllMachines( IDirectPlayImpl *This, void *msg, DWORD size, const void *extra, DWORD extraSize )\n{", 1),
    ("send_to_all_machines(", "DP_SendToAllMachines(", 2),
])

edit("dplay.c", [
    # session description: stored ANSI, preserved fields, propagated
    ("  lpTempSessDesc = DP_DuplicateSessionDesc( lpSessDesc, bAnsi, bAnsi );\n",
     "  /* stored ANSI (what the enum reply and AoE2 use); GetSessionDesc converts per caller */\n"
     "  lpTempSessDesc = DP_DuplicateSessionDesc( lpSessDesc, TRUE, bAnsi );\n", 1),
    ("  /* Free the old */\n  free( This->dp2->lpSessionDesc );\n\n  This->dp2->lpSessionDesc = lpTempSessDesc;\n",
     "  /* the application can't change these (Windows keeps its own player count) */\n"
     "  if( !bInitial && This->dp2->lpSessionDesc )\n  {\n"
     "    lpTempSessDesc->dwCurrentPlayers = This->dp2->lpSessionDesc->dwCurrentPlayers;\n"
     "    lpTempSessDesc->guidInstance     = This->dp2->lpSessionDesc->guidInstance;\n"
     "    lpTempSessDesc->guidApplication  = This->dp2->lpSessionDesc->guidApplication;\n  }\n\n"
     "  /* Free the old */\n  free( This->dp2->lpSessionDesc );\n\n  This->dp2->lpSessionDesc = lpTempSessDesc;\n", 1),
    ('    FIXME( "Need to send a DPMSG_SETSESSIONDESC msg to everyone' + BS + 'n" );\n',
     "    DP_LOBBY_SessionDescChanged( This );   /* our players and every machine hear about it */\n", 1),
    ("    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );\n    return DP_SetSessionDesc( This, lpSessDesc, dwFlags, FALSE, TRUE );\n",
     "    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );\n    return DP_SetSessionDesc( This, lpSessDesc, dwFlags, FALSE, FALSE );\n", 1),
    ("  dwRequiredSize = DP_CopySessionDesc( NULL, This->dp2->lpSessionDesc, bAnsi, bAnsi );\n",
     "  dwRequiredSize = DP_CopySessionDesc( NULL, This->dp2->lpSessionDesc, bAnsi, TRUE );\n", 1),
    ("  DP_CopySessionDesc( lpData, This->dp2->lpSessionDesc, bAnsi, bAnsi );\n",
     "  DP_CopySessionDesc( lpData, This->dp2->lpSessionDesc, bAnsi, TRUE );\n", 1),
    ("    This->dp2->lpSessionDesc = DP_DuplicateSessionDesc( sessionDesc, bAnsi, bAnsi );\n",
     "    This->dp2->lpSessionDesc = DP_DuplicateSessionDesc( sessionDesc, TRUE, bAnsi );\n", 1),
    # player names
    ("  lpPList->lpPData->name = name;\n  lpPList->lpPData->nameA = nameA;\n\n  /* Should send a DPMSG_SETPLAYERORGROUPNAME message */\n"
     '  FIXME( "Message not sent and dwFlags ignored' + BS + 'n" );\n',
     "  free( lpPList->lpPData->name );\n  free( lpPList->lpPData->nameA );\n"
     "  lpPList->lpPData->name = name;\n  lpPList->lpPData->nameA = nameA;\n\n"
     "  DP_LOBBY_PlayerNameChanged( This, idPlayer );\n", 1),
    # player data
    ("    DP_SetPlayerData( plist->lpPData, flags, data, size );\n\n    if ( !(flags & DPSET_LOCAL) )\n"
     '        FIXME( "Send msg?' + BS + 'n" );\n',
     "    DP_SetPlayerData( plist->lpPData, flags, data, size );\n\n    if ( !(flags & DPSET_LOCAL) )\n"
     "        DP_LOBBY_PlayerDataChanged( This, player );\n", 1),
    # chat
    ("    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );\n"
     '    FIXME("(%p)->(0x%08lx,0x%08lx,0x%08lx,%p): stub' + BS + 'n", This, from, to, flags, chatmsg );\n    return DP_OK;\n',
     "    IDirectPlayImpl *This = impl_from_IDirectPlay4A( iface );\n"
     "    return DP_LOBBY_SendChat( This, from, to, flags, chatmsg, TRUE );\n", 1),
    ("    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );\n"
     '    FIXME( "(%p)->(0x%08lx,0x%08lx,0x%08lx,%p): stub' + BS + 'n", This, from, to, flags, chatmsg );\n    return DP_OK;\n',
     "    IDirectPlayImpl *This = impl_from_IDirectPlay4( iface );\n"
     "    return DP_LOBBY_SendChat( This, from, to, flags, chatmsg, FALSE );\n", 1),
    # incoming
    ("    case DPMSGCMD_DELETEPLAYER:\n",
     "    case DPMSGCMD_PLAYERNAMECHANGED:\n      return DP_LOBBY_HandlePlayerNameChanged( This, messageBody, dwMessageBodySize );\n\n"
     "    case DPMSGCMD_PLAYERDATACHANGED:\n      return DP_LOBBY_HandlePlayerDataChanged( This, messageBody, dwMessageBodySize );\n\n"
     "    case DPMSGCMD_SESSIONDESCCHANGED:\n      return DP_LOBBY_HandleSessionDescChanged( This, messageBody, dwMessageBodySize );\n\n"
     "    case DPMSGCMD_CHAT:\n      return DP_LOBBY_HandleChat( This, messageBody, dwMessageBodySize );\n\n"
     "    case DPMSGCMD_DELETEPLAYER:\n", 1),
])

LOBBY = r'''
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
static const WCHAR *DP_WireString( const void *body, DWORD size, DWORD offset )
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

static void DP_QueueNameChanged( IDirectPlayImpl *This, struct PlayerData *p )
{
    DPMSG_SETPLAYERORGROUPNAME msg;

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
    DPSESSIONDESC2 *w;

    EnterCriticalSection( &This->lock );
    if ( !This->dp2->bConnectionOpen || !This->dp2->lpSessionDesc ||
         !(w = DP_DuplicateSessionDesc( This->dp2->lpSessionDesc, FALSE, TRUE )) )
    {
        LeaveCriticalSection( &This->lock );
        return;
    }
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
    if ( !This->dp2->bConnectionOpen )
    {
        LeaveCriticalSection( &This->lock );
        return DPERR_NOSESSIONS;
    }
    plist = DP_FindPlayer( This, from );
    if ( !plist || !(plist->lpPData->dwFlags & DPLAYI_PLAYER_PLAYERLOCAL) )
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

HRESULT DP_LOBBY_HandlePlayerNameChanged( IDirectPlayImpl *This, const void *body, DWORD size )
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
    if ( This->dp2->bConnectionOpen && plist && !(plist->lpPData->dwFlags & DPLAYI_PLAYER_PLAYERLOCAL) )
    {
        w = DP_DuplicateName( &name, FALSE, FALSE );
        a = DP_DuplicateName( &name, TRUE, FALSE );
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

HRESULT DP_LOBBY_HandlePlayerDataChanged( IDirectPlayImpl *This, const void *body, DWORD size )
{
    const DPSP_MSG_PLAYERDATACHANGED *m = body;
    struct PlayerList *plist;

    if ( size < sizeof(*m) || m->dataOffset > size || size - m->dataOffset < m->dataSize )
        return DPERR_GENERIC;
    EnterCriticalSection( &This->lock );
    plist = DP_FindPlayer( This, m->playerId );
    if ( This->dp2->bConnectionOpen && plist && !(plist->lpPData->dwFlags & DPLAYI_PLAYER_PLAYERLOCAL) )
    {
        DP_SetPlayerData( plist->lpPData, DPSET_REMOTE, (char *) body + m->dataOffset, m->dataSize );
        DP_QueueDataChanged( This, plist->lpPData );
    }
    LeaveCriticalSection( &This->lock );
    return DP_OK;
}

HRESULT DP_LOBBY_HandleSessionDescChanged( IDirectPlayImpl *This, const void *body, DWORD size )
{
    const DPSP_MSG_SESSIONDESCCHANGED *m = body;
    DPSESSIONDESC2 desc, *stored;

    if ( size < sizeof(*m) ) return DPERR_GENERIC;
    EnterCriticalSection( &This->lock );
    if ( !This->dp2->bConnectionOpen || This->dp2->bHostInterface || !This->dp2->lpSessionDesc )
    {
        LeaveCriticalSection( &This->lock );
        return DP_OK;
    }
    desc = m->desc;
    desc.dwSize = sizeof(desc);
    desc.lpszSessionName = (WCHAR *) DP_WireString( body, size, m->sessionNameOffset );
    desc.lpszPassword = (WCHAR *) DP_WireString( body, size, m->passwordOffset );
    desc.dwCurrentPlayers = This->dp2->lpSessionDesc->dwCurrentPlayers;   /* ours is counted here */
    if ( (stored = DP_DuplicateSessionDesc( &desc, TRUE, FALSE )) )
    {
        DPMSG_SETSESSIONDESC msg;
        free( This->dp2->lpSessionDesc );
        This->dp2->lpSessionDesc = stored;
        memset( &msg, 0, sizeof(msg) );
        msg.dwType = DPSYS_SETSESSIONDESC;
        msg.dpDesc = desc;
        DP_QueueMessage( This, DPID_SYSMSG, DPID_ALLPLAYERS, 0, &msg, DP_CopySetSessionDescMsg, 0 );
    }
    LeaveCriticalSection( &This->lock );
    return DP_OK;
}

HRESULT DP_LOBBY_HandleChat( IDirectPlayImpl *This, const void *body, DWORD size )
{
    const DPSP_MSG_CHAT *m = body;
    const WCHAR *text;

    if ( size < sizeof(*m) || !(text = DP_WireString( body, size, m->messageOffset )) )
        return DPERR_GENERIC;
    EnterCriticalSection( &This->lock );
    if ( This->dp2->bConnectionOpen )
        DP_QueueChat( This, m->fromId, m->toId, m->flags, text );
    LeaveCriticalSection( &This->lock );
    return DP_OK;
}
'''
s = open(d + "dplay.c", encoding="utf-8", newline="").read()
if "Windows port: lobby changes" not in s:
    open(d + "dplay.c", "w", encoding="utf-8", newline="").write(s + LOBBY)

# host.c: the stored session description is ANSI
edit("host.c", [
    ("static void add_wstr( struct buf *b, const WCHAR *s )\n{\n    buf_add( b, s, (lstrlenW( s ) + 1) * sizeof(WCHAR) );\n}\n",
     "static void add_wstr( struct buf *b, const WCHAR *s )\n{\n    buf_add( b, s, (lstrlenW( s ) + 1) * sizeof(WCHAR) );\n}\n\n"
     "/* the session description is stored ANSI; the wire wants WCHAR */\n"
     "static void add_astr_as_wstr( struct buf *b, const char *s )\n{\n"
     "    int n = MultiByteToWideChar( CP_ACP, 0, s ? s : \"\", -1, NULL, 0 );\n"
     "    WCHAR *w = buf_add( b, NULL, n * sizeof(WCHAR) );\n"
     "    if ( w ) MultiByteToWideChar( CP_ACP, 0, s ? s : \"\", -1, w, n );\n}\n", 1),
    ("    add_wstr( &b, sd->lpszSessionName ? sd->lpszSessionName : L\"\" );\n",
     "    add_astr_as_wstr( &b, sd->lpszSessionNameA );\n", 1),
    ("    if ( sd->lpszPassword && sd->lpszPassword[ 0 ] && (!password || lstrcmpW( password, sd->lpszPassword )) )\n",
     "    if ( sd->lpszPasswordA && sd->lpszPasswordA[ 0 ] && !password_matches( sd->lpszPasswordA, password ) )\n", 1),
    ("static HRESULT nack( IDirectPlayImpl *This, HRESULT error, void **reply, DWORD *replySize )\n",
     "static BOOL password_matches( const char *stored, const WCHAR *given )\n{\n"
     "    WCHAR w[ 256 ];\n"
     "    if ( !given || !MultiByteToWideChar( CP_ACP, 0, stored, -1, w, ARRAYSIZE( w ) ) ) return FALSE;\n"
     "    return !lstrcmpW( w, given );\n}\n\n"
     "static HRESULT nack( IDirectPlayImpl *This, HRESULT error, void **reply, DWORD *replySize )\n", 1),
])
print("ok")
