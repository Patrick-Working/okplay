/* hostip: write this machine's name and every address gethostbyname gives for it to hostip.txt */
#define WIN32_LEAN_AND_MEAN
#include <winsock.h>
#include <stdio.h>
int main(void)
{
    WSADATA w; char n[256]; FILE *f = fopen("C:/UDPDUMP/hostip.txt", "w");
    if (!f || WSAStartup(0x0101, &w)) return 1;
    gethostname(n, sizeof(n));
    struct hostent *h = gethostbyname(n);
    fprintf(f, "name %s\n", n);
    for (int i = 0; h && h->h_addr_list[i]; i++) {
        unsigned char *p = (unsigned char *)h->h_addr_list[i];
        fprintf(f, "addr %d: %u.%u.%u.%u\n", i, p[0], p[1], p[2], p[3]);
    }
    fclose(f);
    return 0;
}
