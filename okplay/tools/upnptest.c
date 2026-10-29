/* upnptest - try the drop-in's UPnP client (src/upnp.c) against the local router.
 *   upnptest                      find the router, its public address, list its port mappings (read-only)
 *   upnptest lookup PORT [TCP|UDP] what the router forwards that public port to (read-only)
 *   upnptest add PORT TCP|UDP INTPORT   ask the router to forward PORT to this PC  (CHANGES THE ROUTER)
 *   upnptest delete PORT TCP|UDP        remove that mapping                         (CHANGES THE ROUTER)
 */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "upnp.h"

int main(int argc, char **argv)
{
    WSADATA wd;
    WSAStartup(MAKEWORD(1, 1), &wd);
    upnp_igd igd;
    DWORD t = GetTickCount();
    if (!upnp_discover(&igd, 3000)) { printf("no UPnP router answered in 3 s\nRESULT fail\n"); return 1; }
    printf("router: %s (control http://%s:%u%s, %s), found in %lu ms\n", igd.location, igd.host, igd.port, igd.control,
           igd.service, (unsigned long)(GetTickCount() - t));
    printf("this PC towards the router: %s\n", igd.lan_ip);
    char ip[64] = "";
    if (upnp_external_ip(&igd, ip, sizeof(ip))) printf("public address: %s\n", ip);
    else printf("public address: (the router didn't say)\n");
    if (argc >= 3 && !strcmp(argv[1], "lookup")) {
        const char *proto = argc >= 4 ? argv[3] : "TCP";
        char client[64], desc[128];
        int ip2;
        if (upnp_get_mapping(&igd, atoi(argv[2]), proto, client, sizeof(client), &ip2, desc, sizeof(desc)))
            printf("%s %s -> %s:%d (\"%s\")\nRESULT ok\n", argv[2], proto, client, ip2, desc);
        else
            printf("%s %s: no such mapping\nRESULT fail\n", argv[2], proto);
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "add")) {
        int ok = upnp_add_mapping(&igd, atoi(argv[2]), argv[3], atoi(argv[4]), "okplay test", 600);
        printf("add %s %s -> %s:%s: %s\nRESULT %s\n", argv[2], argv[3], igd.lan_ip, argv[4], ok ? "ok" : "refused",
               ok ? "ok" : "fail");
        return !ok;
    }
    if (argc >= 4 && !strcmp(argv[1], "delete")) {
        int ok = upnp_delete_mapping(&igd, atoi(argv[2]), argv[3]);
        printf("delete %s %s: %s\nRESULT %s\n", argv[2], argv[3], ok ? "ok" : "refused", ok ? "ok" : "fail");
        return !ok;
    }
    int n = 0;
    for (int i = 0; i < 64; i++) {
        int ext, intp;
        char proto[8], client[64], desc[128];
        if (!upnp_get_mapping_at(&igd, i, &ext, proto, sizeof(proto), client, sizeof(client), &intp, desc, sizeof(desc)))
            break;
        printf("mapping %d: %d %s -> %s:%d (\"%s\")\n", i, ext, proto, client, intp, desc);
        n++;
    }
    printf("%d mapping(s)\nRESULT ok\n", n);
    return 0;
}
