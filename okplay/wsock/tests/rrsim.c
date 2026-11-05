/* rrsim: a stand-in for a LAN game with Road Rash's network pattern, to test okplay wsock without the game.
 *   rrsim host NAME [secs]   - broadcasts 57-byte adverts to 255.255.255.255:6995 every 500 ms, takes a
 *                              19-byte join request on 6996, opens a connected socket back on the joiner's port
 *   rrsim join [secs] [name] - listens on 6995, takes the first advert (from that host name), sends the join request to host:6996
 *                              from 6996, opens a connected socket on its own port to host:that port
 * Both then exchange hellos on the link; RESULT ok when each heard the other. Plain Winsock 1.1 (wsock32),
 * the calls the game makes (FIONREAD polling, FIONBIO, connected UDP). Windows 95 and later. */
#define WIN32_LEAN_AND_MEAN
#include <winsock.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long my_ip(void)
{
    char n[256];
    gethostname(n, sizeof(n));
    struct hostent *h = gethostbyname(n);
    unsigned long ip = 0;
    if (h && h->h_addr_list[0]) memcpy(&ip, h->h_addr_list[0], 4);
    return ip;
}

static unsigned short learn_port(unsigned long ip)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a;
    int l = sizeof(a);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ip;
    bind(s, (struct sockaddr *)&a, sizeof(a));
    getsockname(s, (struct sockaddr *)&a, &l);
    closesocket(s);
    return ntohs(a.sin_port);
}

static SOCKET udp_bound(unsigned short port)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    bind(s, (struct sockaddr *)&a, sizeof(a));
    return s;
}

static const char *ip_s(unsigned long ip)
{
    static char b[20];
    unsigned char *p = (unsigned char *)&ip;
    sprintf(b, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    return b;
}

/* link: send our hello every 33 ms, read theirs; 1 when heard */
static int link_up(SOCKET s, char role, DWORD secs)
{
    char hello[76], buf[256];
    memset(hello, 0, sizeof(hello));
    hello[0] = 1;
    hello[3] = role;
    memcpy(hello + 4, "IDLE", 4);
    DWORD t0 = GetTickCount();
    int heard = 0, sent = 0;
    while (GetTickCount() - t0 < secs * 1000) {
        send(s, hello, sizeof(hello), 0);
        sent++;
        u_long n = 0;
        ioctlsocket(s, FIONREAD, &n);
        while (n) {
            int k = recv(s, buf, sizeof(buf), 0);
            if (k == 76 && buf[0] == 1 && buf[3] != role) {
                if (!heard) printf("EVENT link up after %lu ms (%d hellos sent)\n", GetTickCount() - t0, sent);
                heard++;
                if (heard >= 10) return 1;
            }
            n = 0;
            ioctlsocket(s, FIONREAD, &n);
        }
        Sleep(33);
    }
    return 0;
}

int main(int argc, char **argv)
{
    WSADATA w;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2 || WSAStartup(0x0101, &w)) return 2;
    int host = !strcmp(argv[1], "host");
    DWORD secs = (DWORD)atoi(argc > (host ? 3 : 2) ? argv[host ? 3 : 2] : "40");
    unsigned long me = my_ip();
    printf("EVENT my address %s\n", ip_s(me));
    DWORD t0 = GetTickCount();
    if (host) {
        unsigned short p = learn_port(me);
        SOCKET ad = udp_bound(6995), jl = udp_bound(6996);
        int on = 1;
        setsockopt(ad, SOL_SOCKET, SO_BROADCAST, (char *)&on, sizeof(on));
        char advert[57];
        memset(advert, 0, sizeof(advert));
        advert[0] = 2;
        memcpy(advert + 2, &(unsigned short){ htons(p) }, 2);
        memcpy(advert + 4, &me, 4);
        advert[17] = 7;
        strncpy(advert + 37, argc > 2 ? argv[2] : "rrsim", 19);
        struct sockaddr_in bc;
        memset(&bc, 0, sizeof(bc));
        bc.sin_family = AF_INET;
        bc.sin_port = htons(6995);
        bc.sin_addr.s_addr = INADDR_BROADCAST;
        DWORD last = 0;
        while (GetTickCount() - t0 < secs * 1000) {
            if (GetTickCount() - last >= 500) {
                sendto(ad, advert, sizeof(advert), 0, (struct sockaddr *)&bc, sizeof(bc));
                last = GetTickCount();
            }
            u_long n = 0;
            ioctlsocket(jl, FIONREAD, &n);
            if (n) {
                char req[64];
                struct sockaddr_in from;
                int fl = sizeof(from);
                int k = recvfrom(jl, req, sizeof(req), 0, (struct sockaddr *)&from, &fl);
                if (k == 19) {
                    unsigned short jp;
                    unsigned long jip;
                    memcpy(&jp, req + 2, 2);
                    memcpy(&jip, req + 4, 4);
                    printf("EVENT join request from %s: joiner %s port %u\n", ip_s(from.sin_addr.s_addr), ip_s(jip), ntohs(jp));
                    SOCKET l = udp_bound(ntohs(jp));
                    u_long nb = 1;
                    ioctlsocket(l, FIONBIO, &nb);
                    struct sockaddr_in to;
                    memset(&to, 0, sizeof(to));
                    to.sin_family = AF_INET;
                    to.sin_port = jp;
                    to.sin_addr.s_addr = jip;
                    connect(l, (struct sockaddr *)&to, sizeof(to));
                    int ok = link_up(l, 'S', secs);
                    printf("RESULT %s\n", ok ? "ok" : "fail");
                    closesocket(l);
                    closesocket(ad);   /* as the game does: its room comes off the list */
                    closesocket(jl);
                    WSACleanup();
                    return ok ? 0 : 1;
                }
            }
            Sleep(33);
        }
        printf("RESULT fail (no join)\n");
        closesocket(ad);
        closesocket(jl);
        WSACleanup();
        return 1;
    }
    SOCKET ad = udp_bound(6995);
    while (GetTickCount() - t0 < secs * 1000) {
        u_long n = 0;
        ioctlsocket(ad, FIONREAD, &n);
        if (n) {
            char a[256];
            struct sockaddr_in from;
            int fl = sizeof(from);
            int k = recvfrom(ad, a, sizeof(a), 0, (struct sockaddr *)&from, &fl);
            if (k == 57 && (argc < 4 || !strncmp(a + 37, argv[3], 19))) {   /* join [secs] [host name] */
                unsigned long hip;
                memcpy(&hip, a + 4, 4);
                printf("EVENT advert from %s: host %s \"%.19s\", %d free\n", ip_s(from.sin_addr.s_addr), ip_s(hip), a + 37, a[17]);
                unsigned short p = learn_port(me);
                SOCKET l = udp_bound(p);
                u_long nb = 1;
                ioctlsocket(l, FIONBIO, &nb);
                struct sockaddr_in to;
                memset(&to, 0, sizeof(to));
                to.sin_family = AF_INET;
                to.sin_port = htons(p);
                to.sin_addr.s_addr = hip;
                connect(l, (struct sockaddr *)&to, sizeof(to));
                SOCKET js = udp_bound(6996);
                char req[19];
                memset(req, 0, sizeof(req));
                req[0] = 2;
                memcpy(req + 2, &(unsigned short){ htons(p) }, 2);
                memcpy(req + 4, &me, 4);
                to.sin_port = htons(6996);
                sendto(js, req, sizeof(req), 0, (struct sockaddr *)&to, sizeof(to));
                closesocket(js);
                closesocket(ad);
                int ok = link_up(l, 'R', secs);
                printf("RESULT %s\n", ok ? "ok" : "fail");
                return ok ? 0 : 1;
            }
        }
        Sleep(100);
    }
    printf("RESULT fail (no advert)\n");
    return 1;
}
