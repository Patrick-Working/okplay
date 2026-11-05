/* udpdump: listen on UDP ports and write every datagram (time, sender, full hex) to udpdump.txt
 * beside the exe. For seeing what a game broadcasts on a LAN when its own process can't be
 * logged (Windows 9x shares one wsock32 between all programs). Winsock 1.1, Windows 95 and later.
 *
 *   udpdump 6995 6996        (default: 6995 6996)
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    WSADATA w;
    SOCKET s[8];
    int port[8], n = 0;
    char path[MAX_PATH];
    DWORD l = GetModuleFileNameA(NULL, path, sizeof(path));
    while (l && path[l - 1] != '\\') l--;
    path[l] = 0;
    strcat(path, "udpdump.txt");
    FILE *f = fopen(path, "a");
    if (!f) return 1;
    if (WSAStartup(0x0101, &w)) return 1;
    for (int i = 1; i < argc && n < 8; i++) port[n++] = atoi(argv[i]);
    if (!n) { port[0] = 6995; port[1] = 6996; n = 2; }
    DWORD t0 = GetTickCount();
    for (int i = 0; i < n; i++) {
        struct sockaddr_in a;
        int on = 1;
        s[i] = socket(AF_INET, SOCK_DGRAM, 0);
        setsockopt(s[i], SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on));
        setsockopt(s[i], SOL_SOCKET, SO_BROADCAST, (char *)&on, sizeof(on));
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons((u_short)port[i]);
        int r = bind(s[i], (struct sockaddr *)&a, sizeof(a));
        fprintf(f, "---- udpdump port %d: bind %s (%d)\n", port[i], r ? "FAILED" : "ok", r ? WSAGetLastError() : 0);
    }
    fflush(f);
    static unsigned char buf[65536];
    for (;;) {
        fd_set rs;
        FD_ZERO(&rs);
        for (int i = 0; i < n; i++) FD_SET(s[i], &rs);
        if (select(0, &rs, NULL, NULL, NULL) <= 0) { Sleep(10); continue; }
        for (int i = 0; i < n; i++) {
            if (!FD_ISSET(s[i], &rs)) continue;
            struct sockaddr_in from;
            int fl = sizeof(from);
            int k = recvfrom(s[i], (char *)buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
            if (k < 0) continue;
            unsigned char *ip = (unsigned char *)&from.sin_addr;
            fprintf(f, "%8lu port %d from %u.%u.%u.%u:%u len %d ", (unsigned long)(GetTickCount() - t0), port[i], ip[0], ip[1],
                    ip[2], ip[3], ntohs(from.sin_port), k);
            for (int j = 0; j < k; j++) fprintf(f, "%02x", buf[j]);
            fprintf(f, "\n");
            fflush(f);
        }
    }
}
