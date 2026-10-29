/*
 * Service providers without the DirectX 6 send callbacks.
 *
 * Wine's dplayx calls the SP's SendEx/SendToGroupEx directly. Providers written against the
 * DirectX 5 SP interface only implement Send (and maybe SendToGroup); Windows' dplayx supports
 * them by flattening the message and falling back. These helpers do the same:
 *
 *   SendEx       -> Send, with room for the SP header always at the front (DX5 contract; the DX6
 *                   path may omit it for non-guaranteed sends)
 *   SendToGroupEx -> SendToGroup, or, if the SP has no group send at all, one Send per remote
 *                   machine (its system player) for the all-players group, or one per remote
 *                   member for any other group
 *
 * Part of the Windows port for Classic Netplay for AoE2. LGPL 2.1+, like the rest of dplayx.
 */

#include <stdarg.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "wine/debug.h"

#include "dplay_global.h"
#include "dplayx_messages.h"

WINE_DEFAULT_DEBUG_CHANNEL(dplay);

/* the whole message as one block, SP header space first */
static void *flatten( IDirectPlayImpl *This, const SGBUFFER *buffers, DWORD count, DWORD *size )
{
    DWORD header = This->dp2->spData.dwSPHeaderSize, total = 0, i, first = 0;
    char *data, *p;

    /* the header placeholder is a NULL buffer of exactly the SP header size */
    if ( count && !buffers[ 0 ].pData && buffers[ 0 ].len == header )
        first = 1;
    total = header;
    for ( i = first; i < count; ++i )
        total += buffers[ i ].len;
    if ( !(data = calloc( 1, total )) )
        return NULL;
    p = data + header;
    for ( i = first; i < count; ++i )
    {
        if ( buffers[ i ].pData ) memcpy( p, buffers[ i ].pData, buffers[ i ].len );
        p += buffers[ i ].len;
    }
    *size = total;
    return data;
}

static BOOL is_system_message( IDirectPlayImpl *This, const char *data, DWORD size )
{
    DWORD header = This->dp2->spData.dwSPHeaderSize;
    return size >= header + 4 && *(const DWORD *)(data + header) == DPMSGMAGIC_DPLAYMSG;
}

/* A DirectX 6 provider's SendEx/SendToGroupEx gets the provider header space as the first buffer
 * of guaranteed and system messages: real memory it writes its own header into. Wine passed it as
 * a NULL placeholder, and Windows' TCP/IP provider crashed on the first join request (memcpy into
 * NULL, S4 2026-10-14): replace the placeholder with a zeroed buffer. Unguaranteed game messages
 * carry no header space (Wine's own TCP/IP provider sends them as given, and Windows' game
 * datagrams have no provider header - its receive code, S4 full 2026-10-14), so none is added.
 * *buffers is replaced with a malloc'd array the caller frees with *freeme. */
static BOOL with_header_buffer( IDirectPlayImpl *This, SGBUFFER **buffers, DWORD *count, DWORD *msgsize,
                                void **freeme )
{
    DWORD header = This->dp2->spData.dwSPHeaderSize, n = *count, first = 0;
    SGBUFFER *b;

    *freeme = NULL;
    if ( !header ) return TRUE;
    if ( n && !(*buffers)[ 0 ].pData && (*buffers)[ 0 ].len == header )
        first = 1;                       /* the NULL placeholder: replace it */
    else
        return TRUE;                     /* no header space asked for: leave the buffers alone */
    if ( !(b = calloc( 1, (n - first + 1) * sizeof(SGBUFFER) + header )) )
        return FALSE;
    b[ 0 ].len = header;
    b[ 0 ].pData = (UCHAR *)(b + (n - first + 1));   /* zeroed header space after the array */
    memcpy( b + 1, *buffers + first, (n - first) * sizeof(SGBUFFER) );
    if ( !first ) *msgsize += header;
    *buffers = b;
    *count = n - first + 1;
    *freeme = b;
    return TRUE;
}

static HRESULT call_sendex( IDirectPlayImpl *This, DPSP_SENDEXDATA *data )
{
    DPSP_SENDEXDATA copy = *data;
    void *freeme;
    HRESULT hr;
    if ( !with_header_buffer( This, &copy.lpSendBuffers, &copy.cBuffers, &copy.dwMessageSize, &freeme ) )
        return DPERR_OUTOFMEMORY;
    hr = This->dp2->spData.lpCB->SendEx( &copy );
    free( freeme );
    return hr;
}

HRESULT DP_SP_SendEx( IDirectPlayImpl *This, DPSP_SENDEXDATA *data )
{
    DPSP_SENDDATA send;
    DWORD size;
    HRESULT hr;
    void *flat;

    /* after a host migration "the name server" (player 0) is the new host's machine; the SP
     * only knows the original host, so address it by its system player */
    if ( !data->idPlayerTo && This->dp2->hostMigrated && !This->dp2->bHostInterface &&
         This->dp2->nameServerId )
        data->idPlayerTo = This->dp2->nameServerId;

    /* Windows' dplayx calls a provider's plain Send for ordinary sends even when the provider also
     * has SendEx: 137 Send, 0 SendEx in a host+join run over Windows' TCP/IP provider (S3 wire tap,
     * 2026-10-14). SendEx only for what Send can't express (async, priority, timeout) [INF]. */
    if ( This->dp2->spData.lpCB->SendEx &&
         (!This->dp2->spData.lpCB->Send || (data->dwFlags & DPSEND_ASYNC) || data->dwPriority || data->dwTimeout) )
        return call_sendex( This, data );
    if ( !This->dp2->spData.lpCB->Send )
    {
        ERR( "service provider has neither SendEx nor Send\n" );
        return DPERR_UNSUPPORTED;
    }
    if ( !(flat = flatten( This, data->lpSendBuffers, data->cBuffers, &size )) )
        return DPERR_OUTOFMEMORY;

    send.dwFlags = data->dwFlags;
    send.idPlayerTo = data->idPlayerTo;
    send.idPlayerFrom = data->idPlayerFrom;
    send.lpMessage = flat;
    send.dwMessageSize = size;
    send.bSystemMessage = data->bSystemMessage;
    send.lpISP = data->lpISP;
    TRACE( "Send (DX5 SP) to 0x%08lx, %lu bytes, flags 0x%08lx\n", send.idPlayerTo, size, send.dwFlags );
    hr = This->dp2->spData.lpCB->Send( &send );
    free( flat );
    /* a provider whose Send is only a stub (Wine's own TCP/IP provider implements SendEx only) */
    if ( hr == DPERR_UNSUPPORTED && This->dp2->spData.lpCB->SendEx )
        return call_sendex( This, data );
    return hr;
}

HRESULT DP_SP_SendToGroupEx( IDirectPlayImpl *This, DPSP_SENDTOGROUPEXDATA *data )
{
    struct GroupData *group;
    struct PlayerList *plist;
    DPSP_SENDDATA send;
    DWORD size;
    HRESULT hr = DP_OK;
    void *flat;
    int sent = 0, failed = 0;

    /* as above: Windows sends to a group (everyone too) with one Send per remote player */
    if ( This->dp2->spData.lpCB->SendToGroupEx &&
         (!This->dp2->spData.lpCB->Send || (data->dwFlags & DPSEND_ASYNC) || data->dwPriority || data->dwTimeout) )
    {
        DPSP_SENDTOGROUPEXDATA copy = *data;
        void *freeme;
        HRESULT hrx;
        if ( !with_header_buffer( This, &copy.lpSendBuffers, &copy.cBuffers, &copy.dwMessageSize, &freeme ) )
            return DPERR_OUTOFMEMORY;
        hrx = This->dp2->spData.lpCB->SendToGroupEx( &copy );
        free( freeme );
        return hrx;
    }
    if ( !(flat = flatten( This, data->lpSendBuffers, data->cBuffers, &size )) )
        return DPERR_OUTOFMEMORY;

    if ( This->dp2->spData.lpCB->SendToGroup && !This->dp2->spData.lpCB->Send )
    {
        DPSP_SENDTOGROUPDATA gsend;
        gsend.dwFlags = data->dwFlags;
        gsend.idGroupTo = data->idGroupTo;
        gsend.idPlayerFrom = data->idPlayerFrom;
        gsend.lpMessage = flat;
        gsend.dwMessageSize = size;
        gsend.lpISP = data->lpISP;
        hr = This->dp2->spData.lpCB->SendToGroup( &gsend );
        free( flat );
        return hr;
    }
    if ( !This->dp2->spData.lpCB->Send )
    {
        free( flat );
        ERR( "service provider has no send callback at all\n" );
        return DPERR_UNSUPPORTED;
    }

    send.dwFlags = data->dwFlags;
    send.idPlayerFrom = data->idPlayerFrom;
    send.lpMessage = flat;
    send.dwMessageSize = size;
    send.bSystemMessage = is_system_message( This, flat, size );
    send.lpISP = data->lpISP;

    EnterCriticalSection( &This->lock );
    group = data->idGroupTo == DPID_SYSTEM_GROUP ? This->dp2->lpSysGroup : DP_FindAnyGroup( This, data->idGroupTo );
    if ( !group )
    {
        LeaveCriticalSection( &This->lock );
        free( flat );
        return DPERR_INVALIDGROUP;
    }
    for ( plist = DPQ_FIRST( group->players ); plist; plist = DPQ_NEXT( plist->players ) )
    {
        struct PlayerData *p = plist->lpPData;
        if ( p->dwFlags & DPLAYI_PLAYER_PLAYERLOCAL )
            continue;
        /* everyone: once per remote machine (its system player hands the message to all of that
         * machine's players); a real group: once per remote member */
        if ( group == This->dp2->lpSysGroup ? !(p->dwFlags & DPLAYI_PLAYER_SYSPLAYER)
                                            : (p->dwFlags & DPLAYI_PLAYER_SYSPLAYER) )
            continue;
        send.idPlayerTo = p->dpid;
        hr = This->dp2->spData.lpCB->Send( &send );
        if ( hr == DPERR_UNSUPPORTED && This->dp2->spData.lpCB->SendToGroupEx )
        {
            DPSP_SENDTOGROUPEXDATA copy = *data;
            void *freeme;
            LeaveCriticalSection( &This->lock );
            free( flat );
            if ( !with_header_buffer( This, &copy.lpSendBuffers, &copy.cBuffers, &copy.dwMessageSize, &freeme ) )
                return DPERR_OUTOFMEMORY;
            hr = This->dp2->spData.lpCB->SendToGroupEx( &copy );
            free( freeme );
            return hr;
        }
        if ( FAILED( hr ) ) failed++;
        else sent++;
    }
    if ( !sent && !failed && TRACE_ON( dplay ) )   /* nobody reached: who is in the group, and why not */
        for ( plist = DPQ_FIRST( group->players ); plist; plist = DPQ_NEXT( plist->players ) )
            TRACE( "  member 0x%08lx flags 0x%08lx\n", plist->lpPData->dpid, plist->lpPData->dwFlags );
    LeaveCriticalSection( &This->lock );
    free( flat );
    TRACE( "group 0x%08lx fanned out to %d player(s), %d failed\n", data->idGroupTo, sent, failed );
    return failed && !sent ? DPERR_GENERIC : DP_OK;
}
