/* Internet TCP/IP and IPX Connection For DirectPlay
 *
 * Copyright 2008 Ismael Barros Barros
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


#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winsock2.h"
#include "dpwsockx_dll.h"
#include "wine/debug.h"
#include "dplay.h"
#include "wine/dplaysp.h"
#include "upnp.h"

WINE_DEFAULT_DEBUG_CHANNEL(dplay);

#define DPWS_PORT           47624
#define DPWS_START_TCP_PORT 2300
#define DPWS_END_TCP_PORT   2350
#define DPWS_START_UDP_PORT 2350
#define DPWS_END_UDP_PORT   2400

static void DPWS_MessageBodyReceiveCompleted( DPWS_IN_CONNECTION *connection );

static int DPWS_CompareConnections( const void *key, const struct rb_entry *entry )
{
    DPWS_OUT_CONNECTION *connection = RB_ENTRY_VALUE( entry, DPWS_OUT_CONNECTION, entry );
    DPWS_CONNECTION_KEY *connection_key = (DPWS_CONNECTION_KEY *)key;

    if ( connection_key->addr.sin_port < connection->key.addr.sin_port )
        return -1;
    if ( connection_key->addr.sin_port > connection->key.addr.sin_port )
        return 1;

    if ( connection_key->addr.sin_addr.s_addr < connection->key.addr.sin_addr.s_addr )
        return -1;
    if ( connection_key->addr.sin_addr.s_addr > connection->key.addr.sin_addr.s_addr )
        return 1;

    if ( connection_key->guaranteed < connection->key.guaranteed )
        return -1;
    if ( connection_key->guaranteed > connection->key.guaranteed )
        return 1;

    return 0;
}

static int DPWS_CompareConnectionRefs( const void *key, const struct rb_entry *entry )
{
    DPWS_CONNECTION_REF *connectionRef = RB_ENTRY_VALUE( entry, DPWS_CONNECTION_REF, entry );

    return DPWS_CompareConnections( key, &connectionRef->connection->entry );
}

static HRESULT DPWS_BindToFreePort( SOCKET sock, SOCKADDR_IN *addr, int startPort, int endPort )
{
    int port;

    memset( addr, 0, sizeof( *addr ) );
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = htonl( INADDR_ANY );

    for ( port = startPort; port < endPort; ++port )
    {
        addr->sin_port = htons( port );

        if ( SOCKET_ERROR != bind( sock, (SOCKADDR *) addr, sizeof( *addr ) ) )
            return DP_OK;

        if ( WSAGetLastError() == WSAEADDRINUSE )
            continue;

        ERR( "bind() failed\n" );
        return DPERR_UNAVAILABLE;
    }

    ERR( "no free ports\n" );
    return DPERR_UNAVAILABLE;
}

static void DPWS_RemoveInConnection( DPWS_IN_CONNECTION *connection )
{
    list_remove( &connection->entry );
    closesocket( connection->tcpSock );
    free( connection->buffer );
    free( connection );
}

static void WINAPI DPWS_TcpReceiveCompleted( DWORD error, DWORD transferred,
                                             WSAOVERLAPPED *overlapped, DWORD flags )
{
    DPWS_IN_CONNECTION *connection = (DPWS_IN_CONNECTION *)overlapped->hEvent;

    if ( error != ERROR_SUCCESS )
    {
        ERR( "WSARecv() failed\n" );
        DPWS_RemoveInConnection( connection );
        return;
    }

    if ( !transferred )
    {
        DPWS_RemoveInConnection( connection );
        return;
    }

    if ( transferred < connection->wsaBuffer.len )
    {
        connection->wsaBuffer.len -= transferred;
        connection->wsaBuffer.buf += transferred;

        if ( SOCKET_ERROR == WSARecv( connection->tcpSock, &connection->wsaBuffer, 1, &transferred,
                                      &flags, &connection->overlapped, DPWS_TcpReceiveCompleted ) )
        {
            if ( WSAGetLastError() != WSA_IO_PENDING )
            {
                ERR( "WSARecv() failed\n" );
                DPWS_RemoveInConnection( connection );
                return;
            }
        }
        return;
    }

    connection->completionRoutine( connection );
}

static HRESULT DPWS_TcpReceive( DPWS_IN_CONNECTION *connection, void *data, DWORD size,
                                DPWS_COMPLETION_ROUTINE *completionRoutine )
{
    DWORD transferred;
    DWORD flags = 0;

    connection->wsaBuffer.len = size;
    connection->wsaBuffer.buf = data;

    connection->completionRoutine = completionRoutine;

    if ( SOCKET_ERROR == WSARecv( connection->tcpSock, &connection->wsaBuffer, 1, &transferred,
                                  &flags, &connection->overlapped, DPWS_TcpReceiveCompleted ) )
    {
        if ( WSAGetLastError() != WSA_IO_PENDING )
        {
            ERR( "WSARecv() failed\n" );
            return DPERR_UNAVAILABLE;
        }
    }

    return DP_OK;
}

static void DPWS_HeaderReceiveCompleted( DPWS_IN_CONNECTION *connection )
{
    int messageBodySize;
    int messageSize;

    messageSize = DPSP_MSG_SIZE( connection->header.mixed );
    if ( messageSize < sizeof( DPSP_MSG_HEADER ))
    {
        ERR( "message is too short: %d\n", messageSize );
        DPWS_RemoveInConnection( connection );
        return;
    }
    messageBodySize = messageSize - sizeof( DPSP_MSG_HEADER );

    if ( messageBodySize > DPWS_GUARANTEED_MAXBUFFERSIZE )
    {
        ERR( "message is too long: %d\n", messageSize );
        DPWS_RemoveInConnection( connection );
        return;
    }

    if ( connection->bufferSize < messageBodySize )
    {
        int newSize = max( connection->bufferSize * 2, messageBodySize );
        char *newBuffer = malloc( newSize );
        if ( !newBuffer )
        {
            ERR( "failed to allocate required memory.\n" );
            DPWS_RemoveInConnection( connection );
            return;
        }
        free( connection->buffer );
        connection->buffer = newBuffer;
        connection->bufferSize = newSize;
    }

    if ( FAILED( DPWS_TcpReceive( connection, connection->buffer, messageBodySize,
                                  DPWS_MessageBodyReceiveCompleted ) ) )
    {
        DPWS_RemoveInConnection( connection );
        return;
    }
}

static void DPWS_MessageBodyReceiveCompleted( DPWS_IN_CONNECTION *connection )
{
    int messageBodySize;
    int messageSize;

    if ( connection->header.SockAddr.sin_addr.s_addr == INADDR_ANY )
        connection->header.SockAddr.sin_addr = connection->addr.sin_addr;

    messageSize = DPSP_MSG_SIZE( connection->header.mixed );
    messageBodySize = messageSize - sizeof( DPSP_MSG_HEADER );

    IDirectPlaySP_HandleMessage( connection->sp, connection->buffer, messageBodySize,
                                 &connection->header );

    if ( FAILED( DPWS_TcpReceive( connection, &connection->header, sizeof( DPSP_MSG_HEADER ),
                                  DPWS_HeaderReceiveCompleted ) ) )
    {
        DPWS_RemoveInConnection( connection );
        return;
    }
}

static void WINAPI DPWS_UdpReceiveCompleted( DWORD error, DWORD transferred,
                                             WSAOVERLAPPED *overlapped, DWORD flags )
{
    DPWS_DATA *dpwsData = (DPWS_DATA *)overlapped->hEvent;
    DPSP_MSG_HEADER *header;
    int messageBodySize;
    char *messageBody;

    if ( error != ERROR_SUCCESS )
    {
        ERR( "WSARecv() failed\n" );
        return;
    }

    messageBody = dpwsData->buffer;
    messageBodySize = transferred;
    header = NULL;

    if ( transferred >= sizeof( DPSP_MSG_HEADER ) + sizeof( DWORD ) )
    {
        DWORD signature = *(DWORD *)&dpwsData->buffer[ sizeof( DPSP_MSG_HEADER ) ];
        if ( signature == 0x79616c70 )
        {
            int messageSize;

            header = (DPSP_MSG_HEADER *)dpwsData->buffer;
            messageSize = DPSP_MSG_SIZE( header->mixed );

            if ( messageSize < sizeof( DPSP_MSG_HEADER ))
            {
                ERR( "message is too short: %d\n", messageSize );
                return;
            }
            if ( messageSize > transferred )
            {
                ERR( "truncated message\n" );
                return;
            }

            messageBody = dpwsData->buffer + sizeof( DPSP_MSG_HEADER );
            messageBodySize = messageSize - sizeof( DPSP_MSG_HEADER );
        }
    }

    IDirectPlaySP_HandleMessage( dpwsData->lpISP, messageBody, messageBodySize, header );

    if ( SOCKET_ERROR == WSARecv( dpwsData->udpSock, &dpwsData->wsaBuffer, 1, &transferred, &flags,
                                  &dpwsData->overlapped, DPWS_UdpReceiveCompleted ) )
    {
        if ( WSAGetLastError() != WSA_IO_PENDING )
        {
            ERR( "WSARecv() failed\n" );
            return;
        }
    }
}

/* Hosting: a session search ([MC-DPL4CS] 3.2.5.3) arrives on DPWS_PORT. Its header names the
 * searcher's TCP port; an address of 0.0.0.0 means "where it came from". DirectPlay answers through
 * DPWSCB_Reply, which sends over TCP to that address. */
static void DPWS_EnumArm( DPWS_DATA *dpwsData );

static void WINAPI DPWS_EnumUdpReceiveCompleted( DWORD error, DWORD transferred,
                                                 WSAOVERLAPPED *overlapped, DWORD flags )
{
    DPWS_DATA *dpwsData = (DPWS_DATA *)overlapped->hEvent;

    if ( error == ERROR_SUCCESS && transferred >= sizeof( DPSP_MSG_HEADER ) + 8 &&
         *(DWORD *)&dpwsData->enumBuffer[ sizeof( DPSP_MSG_HEADER ) ] == 0x79616c70 )
    {
        DPSP_MSG_HEADER header;
        int messageSize;

        memcpy( &header, dpwsData->enumBuffer, sizeof( header ) );
        messageSize = DPSP_MSG_SIZE( header.mixed );
        if ( messageSize >= sizeof( DPSP_MSG_HEADER ) && messageSize <= transferred )
        {
            if ( header.SockAddr.sin_addr.s_addr == INADDR_ANY )
                header.SockAddr.sin_addr = dpwsData->enumFrom.sin_addr;
            IDirectPlaySP_HandleMessage( dpwsData->lpISP, dpwsData->enumBuffer + sizeof( header ),
                                         messageSize - sizeof( header ), &header );
        }
    }
    if ( dpwsData->hosting )
        DPWS_EnumArm( dpwsData );   /* also after an error: a UDP "port unreachable" reset is no reason to stop */
}

/* must run on the provider's thread: completion routines run on the thread that started the read */
static void DPWS_EnumArm( DPWS_DATA *dpwsData )
{
    DWORD transferred, flags = 0;

    dpwsData->enumOverlapped.hEvent = (HANDLE)dpwsData;
    dpwsData->enumWsaBuffer.len = sizeof( dpwsData->enumBuffer );
    dpwsData->enumWsaBuffer.buf = dpwsData->enumBuffer;
    dpwsData->enumFromLen = sizeof( dpwsData->enumFrom );
    if ( SOCKET_ERROR == WSARecvFrom( dpwsData->enumUdpSock, &dpwsData->enumWsaBuffer, 1, &transferred, &flags,
                                      (SOCKADDR *)&dpwsData->enumFrom, &dpwsData->enumFromLen,
                                      &dpwsData->enumOverlapped, DPWS_EnumUdpReceiveCompleted ) &&
         WSAGetLastError() != WSA_IO_PENDING )
        ERR( "WSARecvFrom() on the session-search port failed: %d\n", WSAGetLastError() );
}

static DWORD WINAPI DPWS_ThreadProc( void *param )
{
    DPWS_DATA *dpwsData = (DPWS_DATA *)param;
    DWORD transferred;
    DWORD flags = 0;

    SetThreadDescription( GetCurrentThread(), L"dpwsockx" );

    if ( SOCKET_ERROR == WSARecv( dpwsData->udpSock, &dpwsData->wsaBuffer, 1, &transferred, &flags,
                                  &dpwsData->overlapped, DPWS_UdpReceiveCompleted ) )
    {
        if ( WSAGetLastError() != WSA_IO_PENDING )
        {
            ERR( "WSARecv() failed\n" );
            return 0;
        }
    }

    for ( ;; )
    {
        DPWS_IN_CONNECTION *connection;
        WSANETWORKEVENTS networkEvents;
        WSAEVENT events[4];
        SOCKADDR_IN addr;
        DWORD waitResult;
        int addrSize;
        SOCKET sock;

        SOCKET listenSock = dpwsData->tcpSock;
        WSAEVENT listenEvent = dpwsData->acceptEvent;
        DWORD nevents = 3;

        events[ 0 ] = dpwsData->stopEvent;
        events[ 1 ] = dpwsData->acceptEvent;
        events[ 2 ] = dpwsData->wakeEvent;
        if ( dpwsData->enumAcceptEvent )
            events[ nevents++ ] = dpwsData->enumAcceptEvent;
        waitResult = WSAWaitForMultipleEvents( nevents, events, FALSE, WSA_INFINITE, TRUE );
        if ( waitResult == WSA_WAIT_FAILED )
        {
            ERR( "WSAWaitForMultipleEvents() failed\n" );
            break;
        }
        if ( waitResult == WSA_WAIT_IO_COMPLETION )
            continue;
        if ( waitResult == WSA_WAIT_EVENT_0 )
            break;
        if ( waitResult == WSA_WAIT_EVENT_0 + 2 )   /* hosting started: arm the session-search port */
        {
            WSAResetEvent( dpwsData->wakeEvent );
            if ( dpwsData->hosting && !dpwsData->enumArmed && dpwsData->enumUdpSock != INVALID_SOCKET )
            {
                dpwsData->enumArmed = TRUE;
                DPWS_EnumArm( dpwsData );
            }
            continue;
        }
        if ( waitResult == WSA_WAIT_EVENT_0 + 3 )
        {
            listenSock = dpwsData->enumTcpSock;
            listenEvent = dpwsData->enumAcceptEvent;
        }

        if ( SOCKET_ERROR == WSAEnumNetworkEvents( listenSock, listenEvent, &networkEvents ) )
        {
            ERR( "WSAEnumNetworkEvents() failed\n" );
            break;
        }

        addrSize = sizeof( addr );
        sock = accept( listenSock, (SOCKADDR *)&addr, &addrSize );
        if ( sock == INVALID_SOCKET )
        {
            if ( WSAGetLastError() == WSAEWOULDBLOCK )
                continue;
            ERR( "accept() failed\n" );
            break;
        }

        connection = calloc( 1, sizeof( DPWS_IN_CONNECTION ) );
        if ( !connection )
        {
            ERR( "failed to allocate required memory.\n" );
            closesocket( sock );
            continue;
        }

        connection->addr = addr;
        connection->tcpSock = sock;
        connection->overlapped.hEvent = connection;
        connection->sp = dpwsData->lpISP;

        list_add_tail( &dpwsData->inConnections, &connection->entry );

        if ( FAILED( DPWS_TcpReceive( connection, &connection->header, sizeof( DPSP_MSG_HEADER ),
                                      DPWS_HeaderReceiveCompleted ) ) )
        {
            DPWS_RemoveInConnection( connection );
            continue;
        }
    }

    return 0;
}

static HRESULT DPWS_Start( DPWS_DATA *dpwsData )
{
    HRESULT hr;

    if ( dpwsData->started )
        return S_OK;

    dpwsData->tcpSock = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
    if ( dpwsData->tcpSock == INVALID_SOCKET )
    {
        ERR( "socket() failed\n" );
        return DPERR_UNAVAILABLE;
    }

    hr = DPWS_BindToFreePort( dpwsData->tcpSock, &dpwsData->tcpAddr, DPWS_START_TCP_PORT,
                              DPWS_END_TCP_PORT );
    if ( FAILED( hr ) )
    {
        closesocket( dpwsData->tcpSock );
        return hr;
    }

    if ( SOCKET_ERROR == listen( dpwsData->tcpSock, SOMAXCONN ) )
    {
        ERR( "listen() failed\n" );
        closesocket( dpwsData->tcpSock );
        return DPERR_UNAVAILABLE;
    }

    dpwsData->acceptEvent = WSACreateEvent();
    if ( !dpwsData->acceptEvent )
    {
        ERR( "WSACreateEvent() failed\n" );
        closesocket( dpwsData->tcpSock );
        return DPERR_UNAVAILABLE;
    }

    if ( SOCKET_ERROR == WSAEventSelect( dpwsData->tcpSock, dpwsData->acceptEvent, FD_ACCEPT ) )
    {
        ERR( "WSAEventSelect() failed\n" );
        WSACloseEvent( dpwsData->acceptEvent );
        closesocket( dpwsData->tcpSock );
        return DPERR_UNAVAILABLE;
    }

    list_init( &dpwsData->inConnections );

    dpwsData->udpSock = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    if ( dpwsData->udpSock == INVALID_SOCKET )
    {
        ERR( "socket() failed\n" );
        WSACloseEvent( dpwsData->acceptEvent );
        closesocket( dpwsData->tcpSock );
        return DPERR_UNAVAILABLE;
    }

    hr = DPWS_BindToFreePort( dpwsData->udpSock, &dpwsData->udpAddr, DPWS_START_UDP_PORT,
                              DPWS_END_UDP_PORT );
    if ( FAILED( hr ) )
    {
        closesocket( dpwsData->udpSock );
        WSACloseEvent( dpwsData->acceptEvent );
        closesocket( dpwsData->tcpSock );
        return hr;
    }

    dpwsData->overlapped.hEvent = (HANDLE)dpwsData;
    dpwsData->wsaBuffer.len = sizeof( dpwsData->buffer );
    dpwsData->wsaBuffer.buf = dpwsData->buffer;

    rb_init( &dpwsData->connections, DPWS_CompareConnections );

    dpwsData->stopEvent = WSACreateEvent();
    if ( !dpwsData->stopEvent )
    {
        ERR( "WSACreateEvent() failed\n" );
        closesocket( dpwsData->udpSock );
        WSACloseEvent( dpwsData->acceptEvent );
        closesocket( dpwsData->tcpSock );
        return DPERR_UNAVAILABLE;
    }

    InitializeCriticalSection( &dpwsData->sendCs );

    dpwsData->enumUdpSock = INVALID_SOCKET;
    dpwsData->enumTcpSock = INVALID_SOCKET;
    dpwsData->enumAcceptEvent = NULL;
    dpwsData->enumArmed = FALSE;
    dpwsData->hosting = FALSE;
    dpwsData->wakeEvent = WSACreateEvent();

    dpwsData->thread = CreateThread( NULL, 0, DPWS_ThreadProc, dpwsData, 0, NULL );
    if ( !dpwsData->thread )
    {
        ERR( "CreateThread() failed\n" );
        DeleteCriticalSection( &dpwsData->sendCs );
        WSACloseEvent( dpwsData->stopEvent );
        closesocket( dpwsData->udpSock );
        WSACloseEvent( dpwsData->acceptEvent );
        closesocket( dpwsData->tcpSock );
        return DPERR_UNAVAILABLE;
    }

    dpwsData->started = TRUE;

    return S_OK;
}

static HRESULT DPWS_GetConnection( DPWS_DATA *dpwsData, SOCKADDR_IN *addr, BOOL guaranteed,
                                   DPWS_OUT_CONNECTION **connection );
static BOOL DPWS_SameRouter( SOCKADDR_IN *addr, BOOL tcp );

/* Hosting: listen for session searches on DPWS_PORT (UDP: LAN broadcasts and direct searches;
 * TCP: Windows hosts listen there too). Another DirectPlay host on this PC may own the port: the
 * game is then hosted but not found by searches - said in the log, not an error. */
static HRESULT DPWS_StartHost( DPWS_DATA *dpwsData )
{
    SOCKADDR_IN addr;

    if ( dpwsData->hosting )
        return DP_OK;

    memset( &addr, 0, sizeof( addr ) );
    addr.sin_family = AF_INET;
    addr.sin_port = htons( DPWS_PORT );

    dpwsData->enumUdpSock = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    if ( dpwsData->enumUdpSock != INVALID_SOCKET &&
         SOCKET_ERROR == bind( dpwsData->enumUdpSock, (SOCKADDR *)&addr, sizeof( addr ) ) )
    {
        WARN( "UDP port %d is in use (another DirectPlay host on this PC?): searches won't find this game\n", DPWS_PORT );
        closesocket( dpwsData->enumUdpSock );
        dpwsData->enumUdpSock = INVALID_SOCKET;
    }

    dpwsData->enumTcpSock = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
    if ( dpwsData->enumTcpSock != INVALID_SOCKET &&
         (SOCKET_ERROR == bind( dpwsData->enumTcpSock, (SOCKADDR *)&addr, sizeof( addr ) ) ||
          SOCKET_ERROR == listen( dpwsData->enumTcpSock, SOMAXCONN ) ||
          !(dpwsData->enumAcceptEvent = WSACreateEvent()) ||
          SOCKET_ERROR == WSAEventSelect( dpwsData->enumTcpSock, dpwsData->enumAcceptEvent, FD_ACCEPT )) )
    {
        WARN( "TCP port %d is in use: TCP session searches won't reach this game\n", DPWS_PORT );
        closesocket( dpwsData->enumTcpSock );
        dpwsData->enumTcpSock = INVALID_SOCKET;
        if ( dpwsData->enumAcceptEvent ) WSACloseEvent( dpwsData->enumAcceptEvent );
        dpwsData->enumAcceptEvent = NULL;
    }

    dpwsData->hosting = TRUE;
    WSASetEvent( dpwsData->wakeEvent );
    TRACE( "hosting: session searches on port %d (UDP %s, TCP %s)\n", DPWS_PORT,
           dpwsData->enumUdpSock != INVALID_SOCKET ? "yes" : "no", dpwsData->enumTcpSock != INVALID_SOCKET ? "yes" : "no" );
    return DP_OK;
}

/* one whole message over TCP to an address (Reply: the searcher / requester named in its header) */
static HRESULT DPWS_SendTcpTo( DPWS_DATA *dpwsData, SOCKADDR_IN *addr, const void *data, DWORD size )
{
    DPWS_OUT_CONNECTION *connection;
    const char *p = data;
    HRESULT hr;

    EnterCriticalSection( &dpwsData->sendCs );
    hr = DPWS_GetConnection( dpwsData, addr, TRUE, &connection );
    if ( FAILED( hr ) )
    {
        LeaveCriticalSection( &dpwsData->sendCs );
        return hr;
    }
    if ( connection->tcpSock == INVALID_SOCKET )
    {
        connection->tcpSock = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
        if ( connection->tcpSock == INVALID_SOCKET ||
             SOCKET_ERROR == connect( connection->tcpSock, (SOCKADDR *)&connection->key.addr, sizeof( *addr ) ) )
        {
            SOCKADDR_IN lan = connection->key.addr;
            if ( connection->tcpSock != INVALID_SOCKET ) closesocket( connection->tcpSock );
            connection->tcpSock = INVALID_SOCKET;
            if ( DPWS_SameRouter( &lan, TRUE ) &&
                 (connection->tcpSock = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP )) != INVALID_SOCKET &&
                 SOCKET_ERROR != connect( connection->tcpSock, (SOCKADDR *)&lan, sizeof( lan ) ) )
            {
                connection->route = lan;
                connection->routed = TRUE;
            }
            else
            {
                if ( connection->tcpSock != INVALID_SOCKET ) closesocket( connection->tcpSock );
                connection->tcpSock = INVALID_SOCKET;
                ERR( "connect() for a reply failed\n" );
                LeaveCriticalSection( &dpwsData->sendCs );
                return DPERR_UNAVAILABLE;
            }
        }
    }
    while ( size )
    {
        int sent = send( connection->tcpSock, p, size, 0 );
        if ( sent <= 0 )
        {
            ERR( "send() of a reply failed: %d\n", WSAGetLastError() );
            closesocket( connection->tcpSock );
            connection->tcpSock = INVALID_SOCKET;
            LeaveCriticalSection( &dpwsData->sendCs );
            return DPERR_UNAVAILABLE;
        }
        p += sent;
        size -= sent;
    }
    LeaveCriticalSection( &dpwsData->sendCs );
    return DP_OK;
}

static void DPWS_Stop( DPWS_DATA *dpwsData )
{
    DPWS_OUT_CONNECTION *outConnection2;
    DPWS_OUT_CONNECTION *outConnection;
    DPWS_IN_CONNECTION *inConnection2;
    DPWS_IN_CONNECTION *inConnection;

    if ( !dpwsData->started )
        return;

    dpwsData->started = FALSE;
    dpwsData->hosting = FALSE;

    WSASetEvent( dpwsData->stopEvent );
    WaitForSingleObject( dpwsData->thread, INFINITE );
    CloseHandle( dpwsData->thread );

    if ( dpwsData->enumUdpSock != INVALID_SOCKET ) closesocket( dpwsData->enumUdpSock );
    if ( dpwsData->enumTcpSock != INVALID_SOCKET ) closesocket( dpwsData->enumTcpSock );
    if ( dpwsData->enumAcceptEvent ) WSACloseEvent( dpwsData->enumAcceptEvent );
    if ( dpwsData->wakeEvent ) WSACloseEvent( dpwsData->wakeEvent );
    dpwsData->enumUdpSock = dpwsData->enumTcpSock = INVALID_SOCKET;
    dpwsData->enumAcceptEvent = dpwsData->wakeEvent = NULL;

    RB_FOR_EACH_ENTRY_DESTRUCTOR( outConnection, outConnection2, &dpwsData->connections,
                                  DPWS_OUT_CONNECTION, entry )
    {
        rb_remove( &dpwsData->connections, &outConnection->entry );
        if ( outConnection->tcpSock != INVALID_SOCKET )
            closesocket( outConnection->tcpSock );
        free( outConnection );
    }

    LIST_FOR_EACH_ENTRY_SAFE( inConnection, inConnection2, &dpwsData->inConnections,
                              DPWS_IN_CONNECTION, entry )
        DPWS_RemoveInConnection( inConnection );

    DeleteCriticalSection( &dpwsData->sendCs );
    WSACloseEvent( dpwsData->stopEvent );
    closesocket( dpwsData->udpSock );
    WSACloseEvent( dpwsData->acceptEvent );
    closesocket( dpwsData->tcpSock );
}

static HRESULT WINAPI DPWSCB_EnumSessions( LPDPSP_ENUMSESSIONSDATA data )
{
    DPSP_MSG_HEADER *header = (DPSP_MSG_HEADER *) data->lpMessage;
    DPWS_DATA *dpwsData;
    DWORD dpwsDataSize;
    SOCKADDR_IN addr;
    BOOL trueflag = TRUE;
    SOCKET sock;
    HRESULT hr;

    TRACE( "(%p,%ld,%p,%u)\n",
           data->lpMessage, data->dwMessageSize,
           data->lpISP, data->bReturnStatus );

    hr = IDirectPlaySP_GetSPData( data->lpISP, (void **) &dpwsData, &dpwsDataSize, DPSET_LOCAL );
    if ( FAILED( hr ) )
        return hr;

    hr = DPWS_Start( dpwsData );
    if ( FAILED (hr) )
        return hr;

    sock = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    if ( sock == INVALID_SOCKET )
    {
        ERR( "socket() failed\n" );
        return DPERR_UNAVAILABLE;
    }

    if ( SOCKET_ERROR == setsockopt( sock, SOL_SOCKET, SO_BROADCAST,
                                     (const char *) &trueflag,
                                     sizeof( trueflag ) ) )
    {
        ERR( "setsockopt() failed\n" );
        closesocket( sock );
        return DPERR_UNAVAILABLE;
    }

    memset( header, 0, sizeof( DPSP_MSG_HEADER ) );
    header->mixed = DPSP_MSG_MAKE_MIXED( data->dwMessageSize, DPSP_MSG_TOKEN_REMOTE );
    header->SockAddr.sin_family = AF_INET;
    header->SockAddr.sin_port = dpwsData->tcpAddr.sin_port;

    memset( &addr, 0, sizeof( addr ) );
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl( INADDR_BROADCAST );
    addr.sin_port = htons( DPWS_PORT );
    /* an address in the connection (the game's "IP address" box): ask that host only, as
       Windows' provider does - a broadcast never leaves the local network, and on a machine with
       two network cards it goes out of one of them only */
    if ( dpwsData->searchDirected )
        addr = dpwsData->searchTo;

    if ( SOCKET_ERROR == sendto( sock, data->lpMessage, data->dwMessageSize, 0, (SOCKADDR *) &addr,
                                 sizeof( addr ) ) )
    {
        ERR( "sendto() failed\n" );
        closesocket( sock );
        return DPERR_UNAVAILABLE;
    }

    closesocket( sock );

    return DP_OK;
}

/* DirectPlay's answer to a request (a session search, a player-id request, a join...): over TCP to
 * the address in the request's header (its TCP port; the source address when it said 0.0.0.0, which
 * the receive paths filled in), with our header in front: size, the remote token, our TCP port. */
static HRESULT WINAPI DPWSCB_Reply( LPDPSP_REPLYDATA data )
{
    DPSP_MSG_HEADER *to = data->lpSPMessageHeader, *header = data->lpMessage;
    DPWS_DATA *dpwsData;
    DWORD dpwsDataSize;
    SOCKADDR_IN addr;
    HRESULT hr;

    TRACE( "(%p,%p,%ld,%ld,%p)\n", data->lpSPMessageHeader, data->lpMessage, data->dwMessageSize,
           data->idNameServer, data->lpISP );

    if ( !to || !header || data->dwMessageSize < sizeof( DPSP_MSG_HEADER ) )
        return DPERR_INVALIDPARAMS;
    hr = IDirectPlaySP_GetSPData( data->lpISP, (void **)&dpwsData, &dpwsDataSize, DPSET_LOCAL );
    if ( FAILED( hr ) )
        return hr;
    hr = DPWS_Start( dpwsData );
    if ( FAILED( hr ) )
        return hr;

    memset( header, 0, sizeof( *header ) );
    header->mixed = DPSP_MSG_MAKE_MIXED( data->dwMessageSize, DPSP_MSG_TOKEN_REMOTE );
    header->SockAddr.sin_family = AF_INET;
    header->SockAddr.sin_port = dpwsData->tcpAddr.sin_port;

    memset( &addr, 0, sizeof( addr ) );
    addr.sin_family = AF_INET;
    addr.sin_addr = to->SockAddr.sin_addr;
    addr.sin_port = to->SockAddr.sin_port;
    return DPWS_SendTcpTo( dpwsData, &addr, data->lpMessage, data->dwMessageSize );
}

static HRESULT WINAPI DPWSCB_SendEx( LPDPSP_SENDEXDATA data );

/* DirectX 5 style: one buffer with the provider header space in front (this is what Windows'
 * dplayx uses, S3 wire tap 2026-10-14). Guaranteed and system messages keep the header; a game
 * datagram goes without it, as Windows sends those. */
static HRESULT WINAPI DPWSCB_Send( LPDPSP_SENDDATA data )
{
    DPSP_SENDEXDATA ex;
    SGBUFFER b[ 2 ];

    if ( data->dwMessageSize < sizeof( DPSP_MSG_HEADER ) )
        return DPERR_INVALIDPARAMS;
    b[ 0 ].len = sizeof( DPSP_MSG_HEADER );
    b[ 0 ].pData = data->lpMessage;
    b[ 1 ].len = data->dwMessageSize - sizeof( DPSP_MSG_HEADER );
    b[ 1 ].pData = (UCHAR *)data->lpMessage + sizeof( DPSP_MSG_HEADER );

    memset( &ex, 0, sizeof( ex ) );
    ex.lpISP = data->lpISP;
    ex.dwFlags = data->dwFlags;
    ex.idPlayerTo = data->idPlayerTo;
    ex.idPlayerFrom = data->idPlayerFrom;
    ex.bSystemMessage = data->bSystemMessage;
    if ( (data->dwFlags & DPSEND_GUARANTEED) || data->bSystemMessage )
    {
        ex.lpSendBuffers = b;
        ex.cBuffers = 2;
        ex.dwMessageSize = data->dwMessageSize;
    }
    else
    {
        ex.lpSendBuffers = &b[ 1 ];
        ex.cBuffers = 1;
        ex.dwMessageSize = b[ 1 ].len;
    }
    return DPWSCB_SendEx( &ex );
}

static HRESULT WINAPI DPWSCB_AddPlayerToGroup( DPSP_ADDPLAYERTOGROUPDATA *data )
{
    DPWS_CONNECTION_REF *playerConnectionRef;
    DPWS_CONNECTION_REF *groupConnectionRef;
    struct rb_entry *entry;
    DPWS_PLAYER *player;
    DPWS_PLAYER *group;
    DWORD playerSize;
    DWORD groupSize;
    HRESULT hr;

    TRACE( "(%ld,%ld,%p)\n", data->idPlayer, data->idGroup, data->lpISP );

    hr = IDirectPlaySP_GetSPPlayerData( data->lpISP, data->idPlayer, (void **) &player, &playerSize,
                                        DPSET_LOCAL );
    if ( FAILED( hr ) )
        return hr;

    hr = IDirectPlaySP_GetSPPlayerData( data->lpISP, data->idGroup, (void **) &group, &groupSize,
                                        DPSET_LOCAL );
    if ( FAILED( hr ) )
        return hr;

    RB_FOR_EACH_ENTRY( playerConnectionRef, &player->connectionRefs, DPWS_CONNECTION_REF, entry )
    {
        entry = rb_get( &group->connectionRefs, &playerConnectionRef->connection->key );
        if ( entry )
        {
            groupConnectionRef = RB_ENTRY_VALUE( entry, DPWS_CONNECTION_REF, entry );
            ++groupConnectionRef->ref;
            continue;
        }
        groupConnectionRef = calloc( 1, sizeof( DPWS_CONNECTION_REF ) );
        if ( !groupConnectionRef )
            return DPERR_OUTOFMEMORY;
        groupConnectionRef->ref = 1;
        groupConnectionRef->connection = playerConnectionRef->connection;
        rb_put( &group->connectionRefs, &groupConnectionRef->connection->key,
                &groupConnectionRef->entry );
    }

    return DP_OK;
}

static HRESULT WINAPI DPWSCB_CreateGroup( DPSP_CREATEGROUPDATA *data )
{
    DPWS_PLAYER groupPlaceholder = { 0 };
    DPWS_PLAYER *group;
    DWORD groupSize;
    HRESULT hr;

    TRACE( "(%ld,0x%08lx,%p,%p)\n", data->idGroup, data->dwFlags, data->lpSPMessageHeader,
           data->lpISP );

    hr = IDirectPlaySP_SetSPPlayerData( data->lpISP, data->idGroup, &groupPlaceholder,
                                        sizeof( groupPlaceholder ), DPSET_LOCAL );
    if ( FAILED( hr ) )
        return hr;

    hr = IDirectPlaySP_GetSPPlayerData( data->lpISP, data->idGroup, (void **) &group, &groupSize,
                                        DPSET_LOCAL );
    if ( FAILED( hr ) )
        return hr;

    rb_init( &group->connectionRefs, DPWS_CompareConnectionRefs );

    return DP_OK;
}

/* "Same router": a Windows host whose DirectPlay NAT helper mapped its port on a UPnP router
 * advertises the router's public address and port (203.0.113.24:58057 for a host listening on
 * 192.168.1.182:2300, S4 2026-10-14). From the same LAN that address only works on routers with
 * hairpin NAT; Windows' joiner asks the router instead. So do we: when an address is the router's
 * own public one, ask the router which LAN machine/port it forwards that port to (UPnP
 * GetSpecificPortMappingEntry, read-only) and use that. A last resort: only after a TCP connect to
 * the advertised address failed, and for UDP only towards a host whose TCP address needed it
 * (UDP sends can't fail visibly). Private addresses are never looked up. OKP_UPNP=0 turns it off.
 * Returns TRUE when *addr was replaced. */
static ULONG same_router_hosts[ 16 ];   /* public addresses whose TCP port turned out to be on our LAN */
static int same_router_nhosts;

static BOOL DPWS_SameRouterKnown( ULONG ip )
{
    int i;
    for ( i = 0; i < same_router_nhosts; i++ )
        if ( same_router_hosts[ i ] == ip ) return TRUE;
    return FALSE;
}

static BOOL DPWS_SameRouter( SOCKADDR_IN *addr, BOOL tcp )
{
    static CRITICAL_SECTION cs;
    static LONG claimed;
    static volatile LONG ready;
    static int state;            /* 0 not tried, 1 router found, 2 none */
    static upnp_igd igd;
    static ULONG pub;
    static struct { ULONG ip; USHORT port; BOOL tcp; ULONG to_ip; USHORT to_port; BOOL ok; } cache[16];
    static int ncache;
    const BYTE *b = (const BYTE *)&addr->sin_addr.s_addr;
    char v[8];
    int i;

    BOOL replaced = FALSE;
    if ( !b[0] || b[0] == 10 || b[0] == 127 || (b[0] == 172 && (b[1] & 0xf0) == 16) || (b[0] == 192 && b[1] == 168) ||
         (b[0] == 169 && b[1] == 254) )
        return FALSE;
    if ( GetEnvironmentVariableA( "OKP_UPNP", v, sizeof(v) ) && v[0] == '0' )
        return FALSE;
    /* one-time lock set-up with InterlockedExchange only (Windows 95 has no InterlockedCompareExchange) */
    if ( InterlockedExchange( &claimed, 1 ) == 0 )
    {
        InitializeCriticalSection( &cs );
        ready = 1;
    }
    else
        while ( !ready ) Sleep( 0 );
    EnterCriticalSection( &cs );
    if ( state == 0 )
    {
        char ip[32];
        state = 2;
        if ( upnp_discover( &igd, 1500 ) && upnp_external_ip( &igd, ip, sizeof(ip) ) )
        {
            pub = inet_addr( ip );
            state = 1;
            TRACE( "same-router: UPnP router at %s, public address %s\n", igd.host, ip );
        }
    }
    if ( state == 1 && addr->sin_addr.s_addr == pub )
    {
        for ( i = 0; i < ncache; i++ )
            if ( cache[ i ].ip == pub && cache[ i ].port == addr->sin_port && cache[ i ].tcp == tcp )
                break;
        if ( i == ncache && ncache < ARRAY_SIZE( cache ) )
        {
            char client[64];
            int intport = 0;
            cache[ i ].ip = pub;
            cache[ i ].port = addr->sin_port;
            cache[ i ].tcp = tcp;
            cache[ i ].ok = upnp_get_mapping( &igd, ntohs( addr->sin_port ), tcp ? "TCP" : "UDP", client, sizeof(client),
                                              &intport, NULL, 0 );
            if ( cache[ i ].ok )
            {
                cache[ i ].to_ip = inet_addr( client );
                cache[ i ].to_port = htons( (USHORT)intport );
            }
            ncache++;
        }
        if ( i < ncache && cache[ i ].ok )
        {
            TRACE( "same-router: %s port %u is %s port %u on our LAN\n", inet_ntoa( addr->sin_addr ),
                   ntohs( addr->sin_port ), inet_ntoa( *(struct in_addr *)&cache[ i ].to_ip ), ntohs( cache[ i ].to_port ) );
            if ( tcp && !DPWS_SameRouterKnown( pub ) && same_router_nhosts < ARRAY_SIZE( same_router_hosts ) )
                same_router_hosts[ same_router_nhosts++ ] = pub;
            addr->sin_addr.s_addr = cache[ i ].to_ip;
            addr->sin_port = cache[ i ].to_port;
            replaced = TRUE;
        }
    }
    LeaveCriticalSection( &cs );
    return replaced;
}

static HRESULT DPWS_GetConnection( DPWS_DATA *dpwsData, SOCKADDR_IN *addr, BOOL guaranteed,
                                   DPWS_OUT_CONNECTION **connection )
{
    DPWS_CONNECTION_KEY key;
    struct rb_entry *entry;

    key.addr = *addr;
    key.guaranteed = guaranteed;

    entry = rb_get( &dpwsData->connections, &key );
    if ( entry )
    {
        *connection = RB_ENTRY_VALUE( entry, DPWS_OUT_CONNECTION, entry );
        return DP_OK;
    }

    *connection = calloc( 1, sizeof( DPWS_OUT_CONNECTION ) );
    if ( !*connection )
        return DPERR_OUTOFMEMORY;

    (*connection)->key = key;
    (*connection)->tcpSock = INVALID_SOCKET;

    rb_put( &dpwsData->connections, &(*connection)->key, &(*connection)->entry );

    return DP_OK;
}

static HRESULT WINAPI DPWSCB_CreatePlayer( LPDPSP_CREATEPLAYERDATA data )
{
    DPWS_CONNECTION_REF *tcpConnectionRef;
    DPWS_CONNECTION_REF *udpConnectionRef;
    DPWS_PLAYER playerPlaceholder = { 0 };
    DPWS_PLAYERDATA *playerData;
    DWORD playerDataSize;
    DPWS_PLAYER *player;
    DPWS_DATA *dpwsData;
    DWORD dpwsDataSize;
    DWORD playerSize;
    HRESULT hr;

    TRACE( "(%ld,0x%08lx,%p,%p)\n",
           data->idPlayer, data->dwFlags,
           data->lpSPMessageHeader, data->lpISP );

    hr = IDirectPlaySP_GetSPData( data->lpISP, (void **) &dpwsData, &dpwsDataSize, DPSET_LOCAL );
    if ( FAILED( hr ) )
        return hr;

    if ( !data->lpSPMessageHeader )
    {
        DPWS_PLAYERDATA playerDataPlaceholder = { 0 };
        hr = IDirectPlaySP_SetSPPlayerData( data->lpISP, data->idPlayer,
                                            (void *) &playerDataPlaceholder,
                                            sizeof( playerDataPlaceholder ), DPSET_REMOTE );
        if ( FAILED( hr ) )
            return hr;
    }

    hr = IDirectPlaySP_GetSPPlayerData( data->lpISP, data->idPlayer, (void **) &playerData,
                                        &playerDataSize, DPSET_REMOTE );
    if ( FAILED( hr ) )
        return hr;
    if ( playerDataSize != sizeof( DPWS_PLAYERDATA ) )
        return DPERR_GENERIC;

    if ( data->lpSPMessageHeader )
    {
        DPSP_MSG_HEADER *header = data->lpSPMessageHeader;
        if ( !playerData->tcpAddr.sin_addr.s_addr )
            playerData->tcpAddr.sin_addr = header->SockAddr.sin_addr;
        if ( !playerData->udpAddr.sin_addr.s_addr )
            playerData->udpAddr.sin_addr = header->SockAddr.sin_addr;
    }
    else
    {
        playerData->tcpAddr.sin_family = AF_INET;
        playerData->tcpAddr.sin_port = dpwsData->tcpAddr.sin_port;
        playerData->udpAddr.sin_family = AF_INET;
        playerData->udpAddr.sin_port = dpwsData->udpAddr.sin_port;
    }

    hr = IDirectPlaySP_SetSPPlayerData( data->lpISP, data->idPlayer, (void *) &playerPlaceholder,
                                        sizeof( playerPlaceholder ), DPSET_LOCAL );
    if ( FAILED( hr ) )
        return hr;

    hr = IDirectPlaySP_GetSPPlayerData( data->lpISP, data->idPlayer,
                                        (void **) &player, &playerSize, DPSET_LOCAL );
    if ( FAILED( hr ) )
        return hr;

    rb_init( &player->connectionRefs, DPWS_CompareConnectionRefs );

    if ( data->dwFlags & DPLAYI_PLAYER_PLAYERLOCAL )
        return DP_OK;

    tcpConnectionRef = calloc( 1, sizeof( DPWS_CONNECTION_REF ) );
    if ( !tcpConnectionRef )
    {
        free( player );
        return DPERR_OUTOFMEMORY;
    }

    tcpConnectionRef->ref = 1;

    hr = DPWS_GetConnection( dpwsData, &playerData->tcpAddr, TRUE, &tcpConnectionRef->connection );
    if ( FAILED( hr ) )
    {
        free( tcpConnectionRef );
        free( player );
        return hr;
    }

    udpConnectionRef = calloc( 1, sizeof( DPWS_CONNECTION_REF ) );
    if ( !udpConnectionRef )
    {
        free( tcpConnectionRef );
        free( player );
        return DPERR_OUTOFMEMORY;
    }

    udpConnectionRef->ref = 1;

    hr = DPWS_GetConnection( dpwsData, &playerData->udpAddr, FALSE, &udpConnectionRef->connection );
    if ( FAILED( hr ) )
    {
        free( udpConnectionRef );
        free( tcpConnectionRef );
        free( player );
        return hr;
    }

    rb_put( &player->connectionRefs, &tcpConnectionRef->connection->key, &tcpConnectionRef->entry );
    rb_put( &player->connectionRefs, &udpConnectionRef->connection->key, &udpConnectionRef->entry );

    return DP_OK;
}

static HRESULT WINAPI DPWSCB_DeleteGroup( DPSP_DELETEGROUPDATA *data )
{
    DPWS_CONNECTION_REF *connectionRef2;
    DPWS_CONNECTION_REF *connectionRef;
    DPWS_PLAYER *group;
    DWORD groupSize;
    HRESULT hr;

    TRACE( "(%ld,0x%08lx,%p)\n", data->idGroup, data->dwFlags, data->lpISP );

    hr = IDirectPlaySP_GetSPPlayerData( data->lpISP, data->idGroup, (void **) &group, &groupSize,
                                        DPSET_LOCAL );
    if ( FAILED( hr ) )
        return hr;

    RB_FOR_EACH_ENTRY_DESTRUCTOR( connectionRef, connectionRef2, &group->connectionRefs,
                                  DPWS_CONNECTION_REF, entry )
    {
        rb_remove( &group->connectionRefs, &connectionRef->entry );
        free( connectionRef );
    }

    return DP_OK;
}

static HRESULT WINAPI DPWSCB_DeletePlayer( LPDPSP_DELETEPLAYERDATA data )
{
    DPWS_CONNECTION_REF *connectionRef2;
    DPWS_CONNECTION_REF *connectionRef;
    DPWS_PLAYER *player;
    DWORD playerSize;
    HRESULT hr;

    TRACE( "(%ld,0x%08lx,%p)\n",
           data->idPlayer, data->dwFlags, data->lpISP );

    hr = IDirectPlaySP_GetSPPlayerData( data->lpISP, data->idPlayer, (void **) &player, &playerSize,
                                        DPSET_LOCAL );
    if ( FAILED( hr ) )
        return hr;

    RB_FOR_EACH_ENTRY_DESTRUCTOR( connectionRef, connectionRef2, &player->connectionRefs,
                                  DPWS_CONNECTION_REF, entry )
    {
        rb_remove( &player->connectionRefs, &connectionRef->entry );
        free( connectionRef );
    }

    return DP_OK;
}

static HRESULT WINAPI DPWSCB_GetAddress( LPDPSP_GETADDRESSDATA data )
{
    FIXME( "(%ld,0x%08lx,%p,%p,%p) stub\n",
           data->idPlayer, data->dwFlags, data->lpAddress,
           data->lpdwAddressSize, data->lpISP );
    return DPERR_UNSUPPORTED;
}

static HRESULT WINAPI DPWSCB_GetCaps( LPDPSP_GETCAPSDATA data )
{
    TRACE( "(%ld,%p,0x%08lx,%p)\n",
           data->idPlayer, data->lpCaps, data->dwFlags, data->lpISP );

    data->lpCaps->dwFlags = ( DPCAPS_ASYNCSUPPORTED |
                              DPCAPS_GUARANTEEDOPTIMIZED |
                              DPCAPS_GUARANTEEDSUPPORTED );

    data->lpCaps->dwMaxQueueSize    = DPWS_MAXQUEUESIZE;
    data->lpCaps->dwHundredBaud     = DPWS_HUNDREDBAUD;
    data->lpCaps->dwLatency         = DPWS_LATENCY;
    data->lpCaps->dwMaxLocalPlayers = DPWS_MAXLOCALPLAYERS;
    data->lpCaps->dwHeaderLength    = sizeof(DPSP_MSG_HEADER);
    data->lpCaps->dwTimeout         = DPWS_TIMEOUT;

    if ( data->dwFlags & DPGETCAPS_GUARANTEED )
    {
        data->lpCaps->dwMaxBufferSize = DPWS_GUARANTEED_MAXBUFFERSIZE;
        data->lpCaps->dwMaxPlayers    = DPWS_GUARANTEED_MAXPLAYERS;
    }
    else
    {
        data->lpCaps->dwMaxBufferSize = DPWS_MAXBUFFERSIZE;
        data->lpCaps->dwMaxPlayers    = DPWS_MAXPLAYERS;
    }

    return DP_OK;
}

static HRESULT WINAPI DPWSCB_Open( LPDPSP_OPENDATA data )
{
    DPSP_MSG_HEADER *header = (DPSP_MSG_HEADER *) data->lpSPMessageHeader;
    SOCKADDR_IN nameserverAddr;
    DPWS_DATA *dpwsData;
    DWORD dpwsDataSize;
    HRESULT hr;

    TRACE( "(%u,%p,%p,%u,0x%08lx,0x%08lx)\n",
           data->bCreate, data->lpSPMessageHeader, data->lpISP,
           data->bReturnStatus, data->dwOpenFlags, data->dwSessionFlags );

    if ( data->bCreate )
    {
        /* hosting: our sockets plus the session-search port */
        hr = IDirectPlaySP_GetSPData( data->lpISP, (void **)&dpwsData, &dpwsDataSize, DPSET_LOCAL );
        if ( FAILED( hr ) )
            return hr;
        hr = DPWS_Start( dpwsData );
        if ( FAILED( hr ) )
            return hr;
        return DPWS_StartHost( dpwsData );
    }

    hr = IDirectPlaySP_GetSPData( data->lpISP, (void **)&dpwsData, &dpwsDataSize, DPSET_LOCAL );
    if ( FAILED( hr ) )
        return hr;

    hr = DPWS_Start( dpwsData );
    if ( FAILED (hr) )
        return hr;

    memset( &nameserverAddr, 0, sizeof( nameserverAddr ) );
    nameserverAddr.sin_family = AF_INET;
    nameserverAddr.sin_addr = header->SockAddr.sin_addr;
    nameserverAddr.sin_port = header->SockAddr.sin_port;

    rb_init( &dpwsData->nameserver.connectionRefs, DPWS_CompareConnectionRefs );

    hr = DPWS_GetConnection( dpwsData, &nameserverAddr, TRUE,
                             &dpwsData->nameserverConnectionRef.connection );
    if ( FAILED( hr ) )
        return hr;

    rb_put( &dpwsData->nameserver.connectionRefs,
            &dpwsData->nameserverConnectionRef.connection->key,
            &dpwsData->nameserverConnectionRef.entry );

    return DP_OK;
}

static HRESULT WINAPI DPWSCB_RemovePlayerFromGroup( DPSP_REMOVEPLAYERFROMGROUPDATA *data )
{
    DPWS_CONNECTION_REF *playerConnectionRef;
    DPWS_CONNECTION_REF *groupConnectionRef;
    struct rb_entry *entry;
    DPWS_PLAYER *player;
    DPWS_PLAYER *group;
    DWORD playerSize;
    DWORD groupSize;
    HRESULT hr;

    TRACE( "(%ld,%ld,%p)\n", data->idPlayer, data->idGroup, data->lpISP );

    hr = IDirectPlaySP_GetSPPlayerData( data->lpISP, data->idPlayer, (void **) &player, &playerSize,
                                        DPSET_LOCAL );
    if ( FAILED( hr ) )
        return hr;

    hr = IDirectPlaySP_GetSPPlayerData( data->lpISP, data->idGroup, (void **) &group, &groupSize,
                                        DPSET_LOCAL );
    if ( FAILED( hr ) )
        return hr;

    RB_FOR_EACH_ENTRY( playerConnectionRef, &player->connectionRefs, DPWS_CONNECTION_REF, entry )
    {
        entry = rb_get( &group->connectionRefs, &playerConnectionRef->connection->key );
        if ( !entry )
            continue;
        groupConnectionRef = RB_ENTRY_VALUE( entry, DPWS_CONNECTION_REF, entry );
        --groupConnectionRef->ref;
        if ( groupConnectionRef->ref )
            continue;
        rb_remove( &player->connectionRefs, &groupConnectionRef->entry );
        free( groupConnectionRef );
    }

    return DP_OK;
}

static HRESULT WINAPI DPWSCB_CloseEx( LPDPSP_CLOSEDATA data )
{
    DPWS_DATA *dpwsData;
    DWORD dpwsDataSize;
    HRESULT hr;

    TRACE( "(%p)\n", data->lpISP );

    hr = IDirectPlaySP_GetSPData( data->lpISP, (void **) &dpwsData, &dpwsDataSize, DPSET_LOCAL );
    if ( FAILED( hr ) )
        return hr;

    DPWS_Stop( dpwsData );

    return DP_OK;
}

static HRESULT WINAPI DPWSCB_ShutdownEx( LPDPSP_SHUTDOWNDATA data )
{
    DPWS_DATA *dpwsData;
    DWORD dpwsDataSize;
    HRESULT hr;

    TRACE( "(%p)\n", data->lpISP );

    hr = IDirectPlaySP_GetSPData( data->lpISP, (void **) &dpwsData, &dpwsDataSize, DPSET_LOCAL );
    if ( FAILED( hr ) )
        return hr;

    DPWS_Stop( dpwsData );

    WSACleanup();

    return DP_OK;
}

static HRESULT WINAPI DPWSCB_GetAddressChoices( LPDPSP_GETADDRESSCHOICESDATA data )
{
    FIXME( "(%p,%p,%p) stub\n",
           data->lpAddress, data->lpdwAddressSize, data->lpISP );
    return DPERR_UNSUPPORTED;
}

static HRESULT DPWS_SendImpl( DPWS_DATA *dpwsData, DPWS_PLAYER *player, SGBUFFER *buffers,
                              DWORD bufferCount, DWORD messageSize, BOOL guaranteed, BOOL system )
{
    DPWS_CONNECTION_REF *connectionRef;
    DPSP_MSG_HEADER header = { 0 };
    HRESULT sendResult;

    if ( guaranteed || system )
    {
        header.mixed = DPSP_MSG_MAKE_MIXED( messageSize, DPSP_MSG_TOKEN_REMOTE );
        header.SockAddr.sin_family = AF_INET;
        header.SockAddr.sin_port = dpwsData->tcpAddr.sin_port;

        buffers[ 0 ].pData = (unsigned char *) &header;
    }

    EnterCriticalSection( &dpwsData->sendCs );

    if ( guaranteed )
    {
        RB_FOR_EACH_ENTRY( connectionRef, &player->connectionRefs, DPWS_CONNECTION_REF, entry )
        {
            DPWS_OUT_CONNECTION *connection = connectionRef->connection;

            if ( !connection->key.guaranteed )
                continue;
            if ( connection->tcpSock != INVALID_SOCKET )
                continue;

            connection->tcpSock = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
            if ( connection->tcpSock == INVALID_SOCKET )
            {
                ERR( "socket() failed\n" );
                LeaveCriticalSection( &dpwsData->sendCs );
                return DPERR_UNAVAILABLE;
            }

            if ( SOCKET_ERROR == connect( connection->tcpSock, (SOCKADDR *) &connection->key.addr,
                                          sizeof( connection->key.addr ) ) )
            {
                SOCKADDR_IN lan = connection->key.addr;
                closesocket( connection->tcpSock );
                connection->tcpSock = INVALID_SOCKET;
                /* last resort: the advertised address may be our own router's public one */
                if ( DPWS_SameRouter( &lan, TRUE ) &&
                     (connection->tcpSock = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP )) != INVALID_SOCKET )
                {
                    if ( SOCKET_ERROR != connect( connection->tcpSock, (SOCKADDR *) &lan, sizeof( lan ) ) )
                    {
                        connection->route = lan;
                        connection->routed = TRUE;
                        continue;
                    }
                    closesocket( connection->tcpSock );
                    connection->tcpSock = INVALID_SOCKET;
                }
                ERR( "connect() failed\n" );
                LeaveCriticalSection( &dpwsData->sendCs );
                return DPERR_UNAVAILABLE;
            }
        }
    }

    sendResult = DP_OK;

    RB_FOR_EACH_ENTRY( connectionRef, &player->connectionRefs, DPWS_CONNECTION_REF, entry )
    {
        DPWS_OUT_CONNECTION *connection = connectionRef->connection;
        DWORD transferred;

        if ( connection->key.guaranteed != guaranteed )
            continue;

        if ( guaranteed )
        {
            if ( SOCKET_ERROR == WSASend( connection->tcpSock, (WSABUF *) buffers, bufferCount,
                                          &transferred, 0, &connection->overlapped, NULL ) )
            {
                if ( WSAGetLastError() != WSA_IO_PENDING )
                {
                    ERR( "WSASend() failed\n" );
                    LeaveCriticalSection( &dpwsData->sendCs );
                    return DPERR_UNAVAILABLE;
                }
            }
        }
        else
        {
            if ( !connection->routed && DPWS_SameRouterKnown( connection->key.addr.sin_addr.s_addr ) )
            {
                SOCKADDR_IN lan = connection->key.addr;
                if ( DPWS_SameRouter( &lan, FALSE ) )
                {
                    connection->route = lan;
                    connection->routed = TRUE;
                }
            }
            if ( SOCKET_ERROR == WSASendTo( dpwsData->udpSock, (WSABUF *) buffers, bufferCount,
                                            &transferred, 0,
                                            (SOCKADDR *) (connection->routed ? &connection->route : &connection->key.addr),
                                            sizeof( connection->key.addr ), &connection->overlapped,
                                            NULL ) )
            {
                if ( WSAGetLastError() != WSA_IO_PENDING )
                {
                    ERR( "WSASendTo() failed\n" );
                    LeaveCriticalSection( &dpwsData->sendCs );
                    return DPERR_UNAVAILABLE;
                }
            }
        }
    }

    RB_FOR_EACH_ENTRY( connectionRef, &player->connectionRefs, DPWS_CONNECTION_REF, entry )
    {
        DPWS_OUT_CONNECTION *connection = connectionRef->connection;
        DWORD transferred;
        SOCKET sock;

        if ( connection->key.guaranteed != guaranteed )
            continue;

        DWORD wsaFlags = 0;
        sock = guaranteed ? connection->tcpSock : dpwsData->udpSock;

        /* lpdwFlags must be a real pointer on Windows: Wine's ws2_32 tolerates NULL, Windows fails the
         * call and with it every guaranteed send (found running this provider on Windows 10) */
        if ( !WSAGetOverlappedResult( sock, &connection->overlapped, &transferred, TRUE, &wsaFlags ) )
        {
            ERR( "WSAGetOverlappedResult() failed: %d\n", WSAGetLastError() );
            sendResult = DPERR_UNAVAILABLE;
            continue;
        }

        if ( guaranteed && transferred < messageSize )
        {
            ERR( "lost connection\n" );
            closesocket( connection->tcpSock );
            connection->tcpSock = INVALID_SOCKET;
            sendResult = DPERR_CONNECTIONLOST;
            continue;
        }
    }

    LeaveCriticalSection( &dpwsData->sendCs );

    return sendResult;
}

/* Async sends, as Windows' TCP/IP provider answers them (S3 wire tap, 2026-10-14, a
 * DPSESSION_DIRECTPLAYPROTOCOL session with DPSEND_ASYNC): the call returns E_PENDING and the
 * provider reports IDirectPlaySP::SendComplete(context, hr) later from its own thread. The send
 * itself is done at once; the completion goes to our thread as an APC (it waits alertably). */
typedef struct
{
    IDirectPlaySP *sp;
    void          *context;
    HRESULT        hr;
} DPWS_COMPLETION;

static void CALLBACK DPWS_CompleteApc( ULONG_PTR param )
{
    DPWS_COMPLETION *c = (DPWS_COMPLETION *)param;
    IDirectPlaySP_SendComplete( c->sp, c->context, c->hr );
    free( c );
}

static HRESULT DPWS_FinishAsync( IDirectPlaySP *sp, void *context, HRESULT hr )
{
    DPWS_DATA *dpwsData;
    DWORD dpwsDataSize;
    DPWS_COMPLETION *c;

    if ( FAILED( hr ) )
        return hr;
    if ( FAILED( IDirectPlaySP_GetSPData( sp, (void **)&dpwsData, &dpwsDataSize, DPSET_LOCAL ) ) ||
         !dpwsData->started || !dpwsData->thread || !(c = malloc( sizeof( *c ) )) )
        return hr;   /* nowhere to report from: it is done, say so now */
    c->sp = sp;
    c->context = context;
    c->hr = hr;
    if ( !QueueUserAPC( DPWS_CompleteApc, dpwsData->thread, (ULONG_PTR)c ) )
    {
        free( c );
        return hr;
    }
    return E_PENDING;
}

static HRESULT WINAPI DPWSCB_SendEx( LPDPSP_SENDEXDATA data )
{
    DPWS_PLAYER *player;
    DPWS_DATA *dpwsData;
    DWORD dpwsDataSize;
    HRESULT hr;

    TRACE( "(%p,0x%08lx,%ld,%ld,%p,%ld,%ld,%ld,%ld,%p,%p,%u)\n",
           data->lpISP, data->dwFlags, data->idPlayerTo, data->idPlayerFrom,
           data->lpSendBuffers, data->cBuffers, data->dwMessageSize,
           data->dwPriority, data->dwTimeout, data->lpDPContext,
           data->lpdwSPMsgID, data->bSystemMessage );

    if ( data->dwFlags & DPSEND_ASYNC )
    {
        DPSP_SENDEXDATA sync = *data;
        sync.dwFlags &= ~DPSEND_ASYNC;
        return DPWS_FinishAsync( data->lpISP, data->lpDPContext, DPWSCB_SendEx( &sync ) );
    }

    hr = IDirectPlaySP_GetSPData( data->lpISP, (void **) &dpwsData, &dpwsDataSize, DPSET_LOCAL );
    if ( FAILED( hr ) )
        return hr;

    if ( data->idPlayerTo )
    {
        DWORD playerSize;
        hr = IDirectPlaySP_GetSPPlayerData( data->lpISP, data->idPlayerTo, (void **) &player,
                                            &playerSize, DPSET_LOCAL );
        if ( FAILED( hr ) )
            return hr;
    }
    else
    {
        player = &dpwsData->nameserver;
    }

    return DPWS_SendImpl( dpwsData, player, data->lpSendBuffers, data->cBuffers,
                          data->dwMessageSize, !!( data->dwFlags & DPSEND_GUARANTEED ),
                          data->bSystemMessage );
}

static HRESULT WINAPI DPWSCB_SendToGroupEx( LPDPSP_SENDTOGROUPEXDATA data )
{
    DPWS_DATA *dpwsData;
    DPWS_PLAYER *group;
    DWORD dpwsDataSize;
    DWORD groupSize;
    HRESULT hr;

    TRACE( "(%p,0x%08lx,%ld,%ld,%p,%ld,%ld,%ld,%ld,%p,%p)\n",
           data->lpISP, data->dwFlags, data->idGroupTo, data->idPlayerFrom,
           data->lpSendBuffers, data->cBuffers, data->dwMessageSize,
           data->dwPriority, data->dwTimeout, data->lpDPContext,
           data->lpdwSPMsgID );

    if ( data->dwFlags & DPSEND_ASYNC )
    {
        DPSP_SENDTOGROUPEXDATA sync = *data;
        sync.dwFlags &= ~DPSEND_ASYNC;
        return DPWS_FinishAsync( data->lpISP, data->lpDPContext, DPWSCB_SendToGroupEx( &sync ) );
    }

    hr = IDirectPlaySP_GetSPData( data->lpISP, (void **) &dpwsData, &dpwsDataSize, DPSET_LOCAL );
    if ( FAILED( hr ) )
        return hr;

    hr = IDirectPlaySP_GetSPPlayerData( data->lpISP, data->idGroupTo, (void **) &group, &groupSize,
                                        DPSET_LOCAL );
    if ( FAILED( hr ) )
        return hr;

    return DPWS_SendImpl( dpwsData, group, data->lpSendBuffers, data->cBuffers, data->dwMessageSize,
                          !!( data->dwFlags & DPSEND_GUARANTEED ), FALSE );
}

static HRESULT WINAPI DPWSCB_Cancel( LPDPSP_CANCELDATA data )
{
    FIXME( "(%p,0x%08lx,%p,%ld,0x%08lx,0x%08lx) stub\n",
           data->lpISP, data->dwFlags, data->lprglpvSPMsgID, data->cSPMsgID,
           data->dwMinPriority, data->dwMaxPriority );
    return DPERR_UNSUPPORTED;
}

static HRESULT WINAPI DPWSCB_GetMessageQueue( LPDPSP_GETMESSAGEQUEUEDATA data )
{
    FIXME( "(%p,0x%08lx,%ld,%ld,%p,%p) stub\n",
           data->lpISP, data->dwFlags, data->idFrom, data->idTo,
           data->lpdwNumMsgs, data->lpdwNumBytes );
    return DPERR_UNSUPPORTED;
}

static void setup_callbacks( LPDPSP_SPCALLBACKS lpCB )
{
    lpCB->EnumSessions           = DPWSCB_EnumSessions;
    lpCB->Reply                  = DPWSCB_Reply;
    lpCB->Send                   = DPWSCB_Send;
    lpCB->AddPlayerToGroup       = DPWSCB_AddPlayerToGroup;
    lpCB->CreateGroup            = DPWSCB_CreateGroup;
    lpCB->CreatePlayer           = DPWSCB_CreatePlayer;
    lpCB->DeleteGroup            = DPWSCB_DeleteGroup;
    lpCB->DeletePlayer           = DPWSCB_DeletePlayer;
    lpCB->GetAddress             = DPWSCB_GetAddress;
    lpCB->GetCaps                = DPWSCB_GetCaps;
    lpCB->Open                   = DPWSCB_Open;
    lpCB->RemovePlayerFromGroup  = DPWSCB_RemovePlayerFromGroup;
    lpCB->CloseEx                = DPWSCB_CloseEx;
    lpCB->ShutdownEx             = DPWSCB_ShutdownEx;
    lpCB->GetAddressChoices      = DPWSCB_GetAddressChoices;
    lpCB->SendEx                 = DPWSCB_SendEx;
    lpCB->SendToGroupEx          = DPWSCB_SendToGroupEx;
    lpCB->Cancel                 = DPWSCB_Cancel;
    lpCB->GetMessageQueue        = DPWSCB_GetMessageQueue;

    lpCB->Close                  = NULL;
    lpCB->SendToGroup            = NULL;
    lpCB->Shutdown               = NULL;
}



/* the connection's host address, for session searches: DPAID_INet (ANSI) or DPAID_INetW, a dotted
   address or a name, and DPAID_INetPort (default: the session-search port) */
struct search_ctx { char host[ 256 ]; WORD port; };

static BOOL CALLBACK search_address_part( REFGUID type, DWORD size, LPCVOID data, LPVOID context )
{
    struct search_ctx *ctx = context;
    if ( IsEqualGUID( type, &DPAID_INet ) && size )
        lstrcpynA( ctx->host, data, min( size + 1, sizeof(ctx->host) ) );
    else if ( IsEqualGUID( type, &DPAID_INetW ) && size )
        WideCharToMultiByte( CP_ACP, 0, data, size / sizeof(WCHAR), ctx->host, sizeof(ctx->host) - 1, NULL, NULL );
    else if ( IsEqualGUID( type, &DPAID_INetPort ) && size >= sizeof(WORD) )
        ctx->port = *(const WORD *)data;
    return TRUE;
}

static void search_address( IDirectPlaySP *isp, const void *address, DWORD size, DPWS_DATA *dpwsData )
{
    struct search_ctx ctx;
    struct hostent *he;
    unsigned long ip;

    memset( &ctx, 0, sizeof(ctx) );
    IDirectPlaySP_EnumAddress( isp, search_address_part, address, size, &ctx );
    if ( !ctx.host[ 0 ] ) return;
    ip = inet_addr( ctx.host );
    if ( ip == INADDR_NONE && (he = gethostbyname( ctx.host )) && he->h_addrtype == AF_INET )
        memcpy( &ip, he->h_addr_list[ 0 ], sizeof(ip) );
    if ( ip == INADDR_NONE || ip == htonl( INADDR_BROADCAST ) || ip == INADDR_ANY )
    {
        TRACE( "host %s: broadcasting the session search\n", debugstr_a( ctx.host ) );
        return;
    }
    dpwsData->searchTo.sin_family = AF_INET;
    dpwsData->searchTo.sin_addr.s_addr = ip;
    dpwsData->searchTo.sin_port = htons( ctx.port ? ctx.port : DPWS_PORT );
    dpwsData->searchDirected = TRUE;
    TRACE( "session searches go to %s:%u\n", debugstr_a( ctx.host ), ntohs( dpwsData->searchTo.sin_port ) );
}

/******************************************************************
 *		SPInit (DPWSOCKX.1)
 */
HRESULT WINAPI SPInit( LPSPINITDATA lpspData )
{
    WSADATA wsaData;
    DPWS_DATA dpwsData;

    TRACE( "Initializing library for %s (%s)\n",
           wine_dbgstr_guid(lpspData->lpGuid), debugstr_w(lpspData->lpszName) );

    /* We only support TCP/IP service */
    if ( !IsEqualGUID(lpspData->lpGuid, &DPSPGUID_TCPIP) )
    {
        return DPERR_UNAVAILABLE;
    }

    /* Assign callback functions */
    setup_callbacks( lpspData->lpCB );

    /* Load Winsock 2.0 DLL */
    if ( WSAStartup( MAKEWORD(2, 0), &wsaData ) != 0 )
    {
        ERR( "WSAStartup() failed\n" );
        return DPERR_UNAVAILABLE;
    }

    /* Initialize internal data */
    memset( &dpwsData, 0, sizeof(DPWS_DATA) );
    dpwsData.lpISP = lpspData->lpISP;
    if ( lpspData->lpAddress && lpspData->dwAddressSize )
        search_address( lpspData->lpISP, lpspData->lpAddress, lpspData->dwAddressSize, &dpwsData );
    IDirectPlaySP_SetSPData( lpspData->lpISP, &dpwsData, sizeof(DPWS_DATA),
                             DPSET_LOCAL );

    /* dplay needs to know the size of the header */
    lpspData->dwSPHeaderSize = sizeof(DPSP_MSG_HEADER);
    /* and our version: Windows' dplayx refuses a provider that leaves it at the 0 it passed in
       (InitializeConnection -> DPERR_UNAVAILABLE); Windows' own TCP/IP provider says 0x60000 */
    lpspData->dwSPVersion = DPSP_MAJORVERSIONMASK & DPSP_MAJORVERSION;

    return DP_OK;
}
