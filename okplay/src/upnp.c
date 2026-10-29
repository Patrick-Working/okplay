/* upnp.c - see upnp.h. */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "upnp.h"

#define SSDP_ADDR "239.255.255.250"
#define SSDP_PORT 1900

static const char *istrstr(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!_strnicmp(hay, needle, n)) return hay;
    return NULL;
}

/* the text of <tag>...</tag> after `from` (first match), into out */
static int xml_value(const char *from, const char *tag, char *out, int n)
{
    char open[64], close[64];
    _snprintf(open, sizeof(open), "<%s>", tag);
    _snprintf(close, sizeof(close), "</%s>", tag);
    const char *a = istrstr(from, open);
    if (!a) {
        /* a namespace prefix: <u:NewExternalIPAddress> etc. */
        _snprintf(open, sizeof(open), ":%s>", tag);
        a = istrstr(from, open);
        if (!a) return 0;
    }
    a += strlen(open);
    const char *b = istrstr(a, "</");
    if (!b) return 0;
    int len = (int)(b - a);
    if (len >= n) len = n - 1;
    memcpy(out, a, len);
    out[len] = 0;
    return 1;
}

static int split_http_url(const char *url, char *host, int hostsz, unsigned short *port, char *path, int pathsz)
{
    if (_strnicmp(url, "http://", 7)) return 0;
    url += 7;
    const char *slash = strchr(url, '/');
    int hl = slash ? (int)(slash - url) : (int)strlen(url);
    if (hl <= 0 || hl >= hostsz) return 0;
    memcpy(host, url, hl);
    host[hl] = 0;
    *port = 80;
    char *c = strchr(host, ':');
    if (c) { *c = 0; *port = (unsigned short)atoi(c + 1); }
    lstrcpynA(path, slash ? slash : "/", pathsz);
    return 1;
}

static int wait_sock(SOCKET s, int write, int ms)
{
    fd_set f;
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    FD_ZERO(&f);
    FD_SET(s, &f);
    return select(0, write ? NULL : &f, write ? &f : NULL, NULL, &tv) > 0;
}

/* one HTTP request to host:port; returns a malloc'd response body (NUL-terminated) or NULL */
static char *http(const char *host, unsigned short port, const char *req, int reqlen, char *local_ip, int lipsz)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = inet_addr(host);
    if (a.sin_addr.s_addr == INADDR_NONE) return NULL;
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return NULL;
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    connect(s, (struct sockaddr *)&a, sizeof(a));
    if (!wait_sock(s, 1, 3000)) { closesocket(s); return NULL; }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    if (local_ip) {   /* our address towards the router: what a mapping must point at */
        struct sockaddr_in me;
        int ml = sizeof(me);
        if (!getsockname(s, (struct sockaddr *)&me, &ml)) lstrcpynA(local_ip, inet_ntoa(me.sin_addr), lipsz);
    }
    if (send(s, req, reqlen, 0) != reqlen) { closesocket(s); return NULL; }
    size_t len = 0, cap = 8192;
    char *buf = (char *)malloc(cap);
    while (buf && wait_sock(s, 0, 3000)) {
        if (len + 4096 + 1 > cap) {
            char *nbuf = (char *)realloc(buf, cap * 2);
            if (!nbuf) break;
            buf = nbuf;
            cap *= 2;
        }
        int got = recv(s, buf + len, 4096, 0);
        if (got <= 0) break;
        len += got;
    }
    closesocket(s);
    if (!buf) return NULL;
    buf[len] = 0;
    if (strncmp(buf, "HTTP/", 5)) { free(buf); return NULL; }
    int status = 0;
    sscanf(buf, "HTTP/%*s %d", &status);
    char *body = strstr(buf, "\r\n\r\n");
    if (status != 200 || !body) {
        if (body && status == 500) { memmove(buf, body + 4, strlen(body + 4) + 1); return buf; }   /* a SOAP fault */
        free(buf);
        return NULL;
    }
    memmove(buf, body + 4, strlen(body + 4) + 1);
    return buf;
}

static int fetch_description(upnp_igd *igd, const char *location)
{
    char host[64], path[256], req[600];
    unsigned short port;
    if (!split_http_url(location, host, sizeof(host), &port, path, sizeof(path))) return 0;
    int n = _snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s:%u\r\nConnection: close\r\n\r\n", path, host, port);
    char *xml = http(host, port, req, n, igd->lan_ip, sizeof(igd->lan_ip));
    if (!xml) return 0;
    /* the first WANIPConnection (or WANPPPConnection) service and its control URL */
    static const char *kinds[] = {"WANIPConnection:", "WANPPPConnection:"};
    int ok = 0;
    for (int k = 0; k < 2 && !ok; k++) {
        const char *p = xml;
        while ((p = istrstr(p, "<serviceType>")) != NULL) {
            char type[128], ctl[256];
            if (xml_value(p, "serviceType", type, sizeof(type)) && istrstr(type, kinds[k]) &&
                xml_value(p, "controlURL", ctl, sizeof(ctl))) {
                lstrcpynA(igd->service, type, sizeof(igd->service));
                if (!_strnicmp(ctl, "http://", 7)) {
                    char h2[64];
                    unsigned short p2;
                    if (split_http_url(ctl, h2, sizeof(h2), &p2, igd->control, sizeof(igd->control))) {
                        lstrcpynA(igd->host, h2, sizeof(igd->host));
                        igd->port = p2;
                        ok = 1;
                    }
                } else {
                    lstrcpynA(igd->host, host, sizeof(igd->host));
                    igd->port = port;
                    if (ctl[0] == '/') lstrcpynA(igd->control, ctl, sizeof(igd->control));
                    else _snprintf(igd->control, sizeof(igd->control), "/%s", ctl);
                    ok = 1;
                }
                break;
            }
            p += 13;
        }
    }
    free(xml);
    if (ok) lstrcpynA(igd->location, location, sizeof(igd->location));
    return ok;
}

int upnp_discover(upnp_igd *igd, int timeout_ms)
{
    static const char *targets[] = {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                                    "urn:schemas-upnp-org:service:WANIPConnection:1",
                                    "urn:schemas-upnp-org:service:WANPPPConnection:1"};
    memset(igd, 0, sizeof(*igd));
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return 0;
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(SSDP_PORT);
    to.sin_addr.s_addr = inet_addr(SSDP_ADDR);
    for (int t = 0; t < 3; t++) {
        char msg[400];
        int n = _snprintf(msg, sizeof(msg), "M-SEARCH * HTTP/1.1\r\nHOST: %s:%d\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n"
                          "ST: %s\r\n\r\n", SSDP_ADDR, SSDP_PORT, targets[t]);
        sendto(s, msg, n, 0, (struct sockaddr *)&to, sizeof(to));
    }
    DWORD end = GetTickCount() + (DWORD)timeout_ms;
    int found = 0;
    while (!found && GetTickCount() < end) {
        char buf[1500];
        if (!wait_sock(s, 0, 200)) continue;
        int got = recv(s, buf, sizeof(buf) - 1, 0);
        if (got <= 0) continue;
        buf[got] = 0;
        const char *loc = istrstr(buf, "\nLOCATION:");
        if (!loc) continue;
        loc += 10;
        while (*loc == ' ') loc++;
        char url[300];
        int k = 0;
        while (loc[k] && loc[k] != '\r' && loc[k] != '\n' && k < (int)sizeof(url) - 1) { url[k] = loc[k]; k++; }
        url[k] = 0;
        found = fetch_description(igd, url);
    }
    closesocket(s);
    return found;
}

/* a SOAP action; returns the malloc'd response (or fault) body */
static char *soap(const upnp_igd *igd, const char *action, const char *args)
{
    char body[2048], req[3072];
    int bl = _snprintf(body, sizeof(body),
                       "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                       "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
                       "<u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>\r\n",
                       action, igd->service, args ? args : "", action);
    if (bl < 0) return NULL;
    int rl = _snprintf(req, sizeof(req),
                       "POST %s HTTP/1.0\r\nHost: %s:%u\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
                       "Content-Length: %d\r\nSOAPAction: \"%s#%s\"\r\nConnection: close\r\n\r\n%s",
                       igd->control, igd->host, igd->port, bl, igd->service, action, body);
    if (rl < 0) return NULL;
    return http(igd->host, igd->port, req, rl, NULL, 0);
}

int upnp_external_ip(const upnp_igd *igd, char *out, int n)
{
    char *r = soap(igd, "GetExternalIPAddress", NULL);
    int ok = r && xml_value(r, "NewExternalIPAddress", out, n) && out[0];
    free(r);
    return ok;
}

int upnp_get_mapping(const upnp_igd *igd, int ext_port, const char *proto, char *client, int clientsz,
                     int *int_port, char *desc, int descsz)
{
    char args[256], v[32];
    _snprintf(args, sizeof(args), "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort>"
              "<NewProtocol>%s</NewProtocol>", ext_port, proto);
    char *r = soap(igd, "GetSpecificPortMappingEntry", args);
    int ok = r && !istrstr(r, "Fault>") && xml_value(r, "NewInternalClient", client, clientsz) &&
             xml_value(r, "NewInternalPort", v, sizeof(v));
    if (ok) {
        *int_port = atoi(v);
        if (desc && !xml_value(r, "NewPortMappingDescription", desc, descsz)) desc[0] = 0;
    }
    free(r);
    return ok;
}

int upnp_get_mapping_at(const upnp_igd *igd, int index, int *ext_port, char *proto, int protosz, char *client,
                        int clientsz, int *int_port, char *desc, int descsz)
{
    char args[96], v[32], e[32];
    _snprintf(args, sizeof(args), "<NewPortMappingIndex>%d</NewPortMappingIndex>", index);
    char *r = soap(igd, "GetGenericPortMappingEntry", args);
    int ok = r && !istrstr(r, "Fault>") && xml_value(r, "NewExternalPort", e, sizeof(e)) &&
             xml_value(r, "NewProtocol", proto, protosz) && xml_value(r, "NewInternalClient", client, clientsz) &&
             xml_value(r, "NewInternalPort", v, sizeof(v));
    if (ok) {
        *ext_port = atoi(e);
        *int_port = atoi(v);
        if (desc && !xml_value(r, "NewPortMappingDescription", desc, descsz)) desc[0] = 0;
    }
    free(r);
    return ok;
}

int upnp_add_mapping(const upnp_igd *igd, int ext_port, const char *proto, int int_port, const char *desc,
                     int lease_s)
{
    char args[600];
    _snprintf(args, sizeof(args), "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort>"
              "<NewProtocol>%s</NewProtocol><NewInternalPort>%d</NewInternalPort>"
              "<NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled>"
              "<NewPortMappingDescription>%s</NewPortMappingDescription><NewLeaseDuration>%d</NewLeaseDuration>",
              ext_port, proto, int_port, igd->lan_ip, desc ? desc : "", lease_s);
    char *r = soap(igd, "AddPortMapping", args);
    int ok = r && !istrstr(r, "Fault>");
    free(r);
    return ok;
}

int upnp_delete_mapping(const upnp_igd *igd, int ext_port, const char *proto)
{
    char args[256];
    _snprintf(args, sizeof(args), "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort>"
              "<NewProtocol>%s</NewProtocol>", ext_port, proto);
    char *r = soap(igd, "DeletePortMapping", args);
    int ok = r && !istrstr(r, "Fault>");
    free(r);
    return ok;
}
