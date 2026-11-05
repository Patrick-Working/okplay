/* udpconn: the "connected UDP" pattern Road Rash uses, outside the game.
 *   udpconn recv LOCALIP PORT REMOTEIP PORT [ws1|ws2]
 *   udpconn send LOCALIP PORT REMOTEIP PORT [ws1|ws2]
 * Both: socket(UDP), FIONBIO 1, bind(LOCAL), connect(REMOTE). send: send() 20 times, 50 ms apart.
 * recv: poll ioctlsocket(FIONREAD)/recv() for 15 s. ws1 = Windows' wsock32.dll, ws2 = ws2_32.dll.
 * Writes C:/UDPDUMP/udpconn.txt. Windows 95 and later. */
#define WIN32_LEAN_AND_MEAN
#include <winsock.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (WINAPI *startup_t)(WORD, LPWSADATA);
typedef SOCKET (WINAPI *socket_t)(int, int, int);
typedef int (WINAPI *bind_t)(SOCKET, const struct sockaddr *, int);
typedef int (WINAPI *ioctl_t)(SOCKET, long, u_long *);
typedef int (WINAPI *send_t)(SOCKET, const char *, int, int);
typedef int (WINAPI *recv_t)(SOCKET, char *, int, int);
typedef int (WINAPI *err_t)(void);
typedef unsigned long (WINAPI *addr_t)(const char *);
typedef u_short (WINAPI *htons_t)(u_short);

int main(int argc, char **argv)
{
    if (argc < 6) return 2;
    int sender = !strcmp(argv[1], "send");
    HMODULE m = LoadLibraryA(argc > 6 && !strcmp(argv[6], "ws1") ? "wsock32.dll" : "ws2_32.dll");
    FILE *f = fopen("C:/UDPDUMP/udpconn.txt", "a");
    if (!m || !f) return 1;
    startup_t pStartup = (startup_t)GetProcAddress(m, "WSAStartup");
    socket_t pSocket = (socket_t)GetProcAddress(m, "socket");
    bind_t pBind = (bind_t)GetProcAddress(m, "bind"), pConnect = (bind_t)GetProcAddress(m, "connect");
    ioctl_t pIoctl = (ioctl_t)GetProcAddress(m, "ioctlsocket");
    send_t pSend = (send_t)GetProcAddress(m, "send");
    recv_t pRecv = (recv_t)GetProcAddress(m, "recv");
    err_t pErr = (err_t)GetProcAddress(m, "WSAGetLastError");
    addr_t pAddr = (addr_t)GetProcAddress(m, "inet_addr");
    htons_t pHtons = (htons_t)GetProcAddress(m, "htons");
    WSADATA w;
    pStartup(0x0101, &w);
    struct sockaddr_in l, r;
    memset(&l, 0, sizeof(l));
    memset(&r, 0, sizeof(r));
    l.sin_family = r.sin_family = AF_INET;
    l.sin_addr.s_addr = pAddr(argv[2]);
    l.sin_port = pHtons((u_short)atoi(argv[3]));
    r.sin_addr.s_addr = pAddr(argv[4]);
    r.sin_port = pHtons((u_short)atoi(argv[5]));
    SOCKET s = pSocket(AF_INET, SOCK_DGRAM, 0);
    u_long nb = 1;
    pIoctl(s, FIONBIO, &nb);
    int b = pBind(s, (struct sockaddr *)&l, sizeof(l));
    int c = pConnect(s, (struct sockaddr *)&r, sizeof(r));
    fprintf(f, "---- %s %s:%s -> %s:%s via %s: bind %d, connect %d (err %d)\n", argv[1], argv[2], argv[3], argv[4], argv[5],
            argc > 6 ? argv[6] : "ws2", b, c, pErr());
    fflush(f);
    char buf[2048];
    if (sender) {
        for (int i = 0; i < 20; i++) {
            int k = pSend(s, "udpconn hello", 13, 0);
            fprintf(f, "send -> %d (err %d)\n", k, k < 0 ? pErr() : 0);
            Sleep(50);
        }
    } else {
        DWORD t0 = GetTickCount();
        int got = 0;
        while (GetTickCount() - t0 < 15000) {
            u_long n = 0;
            pIoctl(s, FIONREAD, &n);
            if (n) {
                int k = pRecv(s, buf, sizeof(buf), 0);
                fprintf(f, "FIONREAD %lu, recv -> %d\n", n, k);
                got++;
            }
            Sleep(10);
        }
        fprintf(f, "received %d datagram(s) in 15 s\n", got);
    }
    fclose(f);
    return 0;
}
