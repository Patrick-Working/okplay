/* udpsend ADDR PORT [TEXT]: send one UDP datagram (broadcast allowed). Diagnostics for udpdump. */
#define WIN32_LEAN_AND_MEAN
#include <winsock.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv)
{
    WSADATA w;
    struct sockaddr_in a;
    int on = 1;
    if (argc < 3 || WSAStartup(0x0101, &w)) return 2;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (char *)&on, sizeof(on));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr(argv[1]);
    a.sin_port = htons((u_short)atoi(argv[2]));
    const char *t = argc > 3 ? argv[3] : "udpsend";
    return sendto(s, t, (int)strlen(t), 0, (struct sockaddr *)&a, sizeof(a)) > 0 ? 0 : 1;
}
