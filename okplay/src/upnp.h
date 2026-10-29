/* upnp - the little UPnP Internet Gateway Device client the drop-in needs (public UPnP standard:
 * SSDP discovery, the device description, WANIPConnection / WANPPPConnection SOAP actions).
 *
 * Two uses, both what Windows' DirectPlay NAT helper (dpnathlp) does:
 *  - "same router" lookup: a host that advertises its router's public address and port can be
 *    reached from the same LAN at the private address the router maps it to
 *    (GetSpecificPortMappingEntry - read-only);
 *  - hosting: ask the router to forward a port to us (AddPortMapping / DeletePortMapping - this
 *    changes the router's configuration, so callers make it an explicit choice).
 * Plain Winsock 1.1 and blocking sockets with select() timeouts: Windows 95/98 through 11.
 * Call WSAStartup first. LGPL 2.1+, like the rest of the drop-in.
 */
#ifndef OKP_UPNP_H
#define OKP_UPNP_H

typedef struct {
    char host[64];          /* the router's control address */
    unsigned short port;
    char control[256];      /* control URL path */
    char service[96];       /* urn:schemas-upnp-org:service:WANIPConnection:1 (or PPP) */
    char location[300];     /* the device description it came from */
    char lan_ip[16];        /* our address towards the router */
} upnp_igd;

/* find the router; 1 = found */
int upnp_discover(upnp_igd *igd, int timeout_ms);
/* the router's public IPv4 address; 1 = ok */
int upnp_external_ip(const upnp_igd *igd, char *out, int n);
/* what the router forwards external port/proto ("TCP"/"UDP") to; 1 = a mapping exists */
int upnp_get_mapping(const upnp_igd *igd, int ext_port, const char *proto, char *client, int clientsz,
                     int *int_port, char *desc, int descsz);
/* the index-th mapping (GetGenericPortMappingEntry); 1 = ok, 0 = past the end */
int upnp_get_mapping_at(const upnp_igd *igd, int index, int *ext_port, char *proto, int protosz, char *client,
                        int clientsz, int *int_port, char *desc, int descsz);
/* forward ext_port/proto to our LAN address:int_port; lease_s 0 = until removed; 1 = ok */
int upnp_add_mapping(const upnp_igd *igd, int ext_port, const char *proto, int int_port, const char *desc,
                     int lease_s);
int upnp_delete_mapping(const upnp_igd *igd, int ext_port, const char *proto);

#endif
