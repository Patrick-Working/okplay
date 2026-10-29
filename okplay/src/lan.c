/* okplay - LAN Play: room discovery by UDP broadcast. No internet, no server.
 *
 *   joiner  lan_q (ephemeral IPv4)  --DISCOVER-->  every subnet broadcast :47632
 *   host    lan_listen (:47632)     --OFFER----->  back to the joiner's lan_q
 *
 * An OFFER carries the room id, the host's peer id, the enum reply our own
 * dplayx produced (same trick as Online Play) and the host's candidates. From
 * there joining is identical to Online Play: probe the candidates, done.
 */
#include "okp.h"
#include <iphlpapi.h>
#include <mstcpip.h>
#include <stdio.h>
#include <string.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#define LAN_MAGIC "OKLN"
#define LAN_OFFER_CANDS 6

static SOCKET udp4(uint16_t port, int reuse)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        int e = WSAGetLastError();
        okp_log(0, "LAN: socket: %d", e);
#ifdef OKP_BUILTIN
        DP_BUILTIN_NetworkUnavailable(e);
#endif
        return s;
    }
    BOOL on = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (char *)&on, sizeof(on));
    if (reuse) setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on));
    okp_udp_no_connreset(s);
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    SOCKADDR_IN a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (bind(s, (SOCKADDR *)&a, sizeof(a)) != 0) {
        okp_log(0, "LAN: bind UDP %u failed: %d", port, WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

int okp_lan_open_query(okp_ctx *c)
{
    if (c->lan_q == INVALID_SOCKET) c->lan_q = udp4(0, 0);
    return c->lan_q != INVALID_SOCKET;
}

int okp_lan_open_listen(okp_ctx *c)
{
    if (c->lan_listen == INVALID_SOCKET) {
        c->lan_listen = udp4(OKP_LAN_PORT, 1);
        if (c->lan_listen != INVALID_SOCKET)
            okp_log(1, "LAN: answering discovery on UDP %u", OKP_LAN_PORT);
    }
    return c->lan_listen != INVALID_SOCKET;
}

void okp_lan_close(okp_ctx *c)
{
    if (c->lan_q != INVALID_SOCKET) { closesocket(c->lan_q); c->lan_q = INVALID_SOCKET; }
    if (c->lan_listen != INVALID_SOCKET) { closesocket(c->lan_listen); c->lan_listen = INVALID_SOCKET; }
}

/* Directed broadcast of every up IPv4 interface (so multi-NIC and VPN machines
 * still reach the real LAN), plus 255.255.255.255 and loopback. */
static int bcast_targets(uint32_t *out, int max)
{
    int n = 0;
    ULONG sz = 16 * 1024;
    IP_ADAPTER_ADDRESSES *aa = NULL;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    okp_sys_load();
    if (!okp_sys.GetAdaptersAddresses) {
        /* Windows 98/ME/2000: address + mask from GetAdaptersInfo (95: only the two below) */
        IP_ADAPTER_INFO *ai = NULL;
        DWORD r = ERROR_BUFFER_OVERFLOW;
        for (int tries = 0; okp_sys.GetAdaptersInfo && tries < 3 && r == ERROR_BUFFER_OVERFLOW; tries++) {
            free(ai);
            ai = (IP_ADAPTER_INFO *)malloc(sz);
            r = ai ? okp_sys.GetAdaptersInfo(ai, &sz) : ERROR_NOT_ENOUGH_MEMORY;
        }
        if (ai && r == NO_ERROR)
            for (IP_ADAPTER_INFO *a = ai; a && n < max - 2; a = a->Next)
                for (IP_ADDR_STRING *s = &a->IpAddressList; s && n < max - 2; s = s->Next) {
                    uint32_t ip, mask;
                    if (!okp_pton(AF_INET, s->IpAddress.String, &ip) || !okp_pton(AF_INET, s->IpMask.String, &mask) ||
                        !ip || !mask || mask == 0xFFFFFFFFu) continue;
                    uint32_t bc = ip | ~mask;
                    int dup = 0;
                    for (int k = 0; k < n; k++) if (out[k] == bc) dup = 1;
                    if (!dup) out[n++] = bc;
                }
        free(ai);
        rc = ERROR_NOT_SUPPORTED;
    }
    for (int tries = 0; okp_sys.GetAdaptersAddresses && tries < 3 && rc == ERROR_BUFFER_OVERFLOW; tries++) {
        free(aa);
        aa = (IP_ADAPTER_ADDRESSES *)malloc(sz);
        rc = okp_sys.GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  NULL, aa, &sz);
    }
    if (aa && rc == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES *a = aa; a && n < max - 2; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u && n < max - 2; u = u->Next) {
                if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
                UINT8 len = u->OnLinkPrefixLength;
                if (len == 0 || len > 30) continue;
                uint32_t ip = ntohl(((SOCKADDR_IN *)u->Address.lpSockaddr)->sin_addr.s_addr);
                uint32_t bc = htonl(ip | (0xFFFFFFFFu >> len));
                int dup = 0;
                for (int k = 0; k < n; k++) if (out[k] == bc) dup = 1;
                if (!dup) out[n++] = bc;
            }
        }
    }
    free(aa);
    out[n++] = htonl(INADDR_BROADCAST);
    out[n++] = htonl(INADDR_LOOPBACK);
    return n;
}

/* Only answer discovery from the local network: a spoofed internet request must not make a host
 * reflect offers at someone else (the firewall rule lets the game receive UDP from anywhere). */
static int lan_source(const SOCKADDR_IN *a)
{
    const uint8_t *b = (const uint8_t *)&a->sin_addr;
    SOCKADDR_IN6 m;
    okp_addr_from_v4(&m, b, 0);
    return b[0] == 127 || !okp_addr_is_public_v4(&m);   /* loopback, RFC1918, link-local, CGNAT */
}

void okp_lan_discover(okp_ctx *c)
{
    if (!okp_lan_open_query(c)) return;
    uint8_t pkt[sizeof(okp_wire) + 4];
    okp_wire *w = (okp_wire *)pkt;
    memset(w, 0, sizeof(*w));
    w->m0 = 'O';
    w->m1 = 'K';
    w->ver = OKP_WIRE_VER;
    w->type = OKP_T_LAN_DISCOVER;
    w->from = c->self;
    memcpy(pkt + sizeof(okp_wire), LAN_MAGIC, 4);

    uint32_t t[16];
    int n = bcast_targets(t, 16);
    int sent = 0;
    for (int i = 0; i < n; i++) {
        SOCKADDR_IN to;
        memset(&to, 0, sizeof(to));
        to.sin_family = AF_INET;
        to.sin_port = htons(OKP_LAN_PORT);
        to.sin_addr.s_addr = t[i];
        if (sendto(c->lan_q, (const char *)pkt, sizeof(pkt), 0, (SOCKADDR *)&to, sizeof(to)) > 0) sent++;
    }
    c->last_discover = okp_now();
    okp_log(3, "LAN: discover sent to %d/%d broadcast target(s)", sent, n);
}

static void send_offer(okp_ctx *c, const SOCKADDR_IN *to, uint64_t joiner)
{
    uint8_t pkt[sizeof(okp_wire) + 2 + 1024 + 1 + LAN_OFFER_CANDS * sizeof(okp_ccand) + 1 + 8 + 1 + 64];
    okp_wire *w = (okp_wire *)pkt;
    memset(w, 0, sizeof(*w));
    w->m0 = 'O';
    w->m1 = 'K';
    w->ver = OKP_WIRE_VER;
    w->type = OKP_T_LAN_OFFER;
    w->room = c->room;
    w->from = c->self;
    w->to = joiner;
    uint8_t *p = pkt + sizeof(okp_wire);

    EnterCriticalSection(&c->lock);
    uint16_t elen = (uint16_t)c->enum_len;
    memcpy(p, &elen, 2);
    memcpy(p + 2, c->enum_blob, elen);
    LeaveCriticalSection(&c->lock);
    p += 2 + elen;

    SOCKADDR_IN6 lc[LAN_OFFER_CANDS];
    int n;
    okp_local_cands(c, lc, &n, LAN_OFFER_CANDS);
    *p++ = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        okp_ccand cc;
        memset(&cc, 0, sizeof(cc));
        cc.port = lc[i].sin6_port;
        if (okp_addr_is_v4(&lc[i])) { cc.fam = 4; memcpy(cc.addr, ((uint8_t *)&lc[i].sin6_addr) + 12, 4); }
        else { cc.fam = 6; memcpy(cc.addr, &lc[i].sin6_addr, 16); }
        memcpy(p, &cc, sizeof(cc));
        p += sizeof(cc);
    }
    /* 0.8.1+: 'B', the 8-byte exe fingerprint, the exe name's length and the name (older joiners
     * stop reading after the candidates) */
    {
        char build[17], exe[64];
        size_t el;
        okp_exe_build(build, exe, sizeof(exe));
        el = strlen(exe);
        if (strlen(build) == 16 && (size_t)(p - pkt) + 1 + 8 + 1 + el <= sizeof(pkt)) {
            *p++ = 'B';
            for (int i = 0; i < 8; i++) { unsigned v; sscanf(build + 2 * i, "%2x", &v); *p++ = (uint8_t)v; }
            *p++ = (uint8_t)el;
            memcpy(p, exe, el);
            p += el;
        }
    }
    sendto(c->lan_listen, (const char *)pkt, (int)(p - pkt), 0, (const SOCKADDR *)to, sizeof(*to));
    char ip[32];
    okp_ntop(AF_INET, (void *)&to->sin_addr, ip, sizeof(ip));
    okp_log(3, "LAN: offer room %08x to %s:%u (peer %016llx)", c->room, ip, ntohs(to->sin_port),
            (unsigned long long)joiner);
}

static void take_offer(okp_ctx *c, const okp_wire *w, const uint8_t *body, int blen, const SOCKADDR_IN *from)
{
    if (blen < 3) return;
    uint16_t elen;
    memcpy(&elen, body, 2);
    if (elen < 92 || elen > 1024 || 2 + elen + 1 > blen) return;
    if (!okp_enum_blob_ok(body + 2, elen)) return;
    const uint8_t *p = body + 2 + elen;
    int n = *p++;
    if (n > LAN_OFFER_CANDS || 2 + elen + 1 + n * (int)sizeof(okp_ccand) > blen) return;

    okp_room rm;
    memset(&rm, 0, sizeof(rm));
    rm.room = w->room;
    rm.host = w->from;
    memcpy(rm.enumblob, body + 2, elen);
    rm.enumlen = elen;
    rm.seen = okp_now();
    {   /* 0.8.1+ tail: the host's build */
        const uint8_t *t = p + n * sizeof(okp_ccand), *end = body + blen;
        if (end - t >= 10 && t[0] == 'B' && t[9] < sizeof(rm.exe) && end - t >= 10 + t[9]) {
            for (int i = 0; i < 8; i++) snprintf(rm.build + 2 * i, 3, "%02x", t[1 + i]);
            memcpy(rm.exe, t + 10, t[9]);
            rm.exe[t[9]] = 0;
        }
    }
    for (int i = 0; i < n; i++) {
        okp_ccand cc;
        memcpy(&cc, p + i * sizeof(cc), sizeof(cc));
        SOCKADDR_IN6 *a = &rm.cands[rm.ncands];
        if (cc.fam == 4) okp_addr_from_v4(a, cc.addr, cc.port);
        else { memset(a, 0, sizeof(*a)); a->sin6_family = AF_INET6; a->sin6_port = cc.port; memcpy(&a->sin6_addr, cc.addr, 16); }
        rm.ncands++;
    }
    /* the address the offer came from is the most direct route of all */
    SOCKADDR_IN6 src;
    okp_addr_from_v4(&src, &from->sin_addr, 0);
    for (int i = 0; i < rm.ncands && src.sin6_port == 0; i++)
        if (okp_addr_is_v4(&rm.cands[i])) src.sin6_port = rm.cands[i].sin6_port;
    if (src.sin6_port && rm.ncands < OKP_MAX_CANDS) {
        int dup = 0;
        for (int i = 0; i < rm.ncands; i++) if (okp_addr_eq(&rm.cands[i], &src)) dup = 1;
        if (!dup) { memmove(&rm.cands[1], &rm.cands[0], sizeof(rm.cands[0]) * rm.ncands); rm.cands[0] = src; rm.ncands++; }
    }

    int is_new = 1;
    EnterCriticalSection(&c->lock);
    int slot = -1;
    for (int i = 0; i < c->nrooms; i++)
        if (c->rooms[i].room == rm.room && c->rooms[i].host == rm.host) { slot = i; is_new = 0; }
    if (slot < 0) {
        /* reuse a stale slot before growing */
        for (int i = 0; i < c->nrooms && slot < 0; i++)
            if (okp_now() - c->rooms[i].seen > OKP_LAN_ROOM_TTL) slot = i;
        if (slot < 0 && c->nrooms < OKP_MAX_ROOMS) slot = c->nrooms++;
    }
    if (slot >= 0) c->rooms[slot] = rm;
    LeaveCriticalSection(&c->lock);
    if (is_new) {
        char ip[32];
        okp_ntop(AF_INET, (void *)&from->sin_addr, ip, sizeof(ip));
        okp_log(2, "LAN: found room %08x hosted by %016llx at %s (%d cands)", rm.room, (unsigned long long)rm.host,
                ip, rm.ncands);
    }
}

void okp_lan_readable(okp_ctx *c, SOCKET s)
{
    uint8_t buf[2048];
    int got_offer = 0;
    for (;;) {
        SOCKADDR_IN from;
        int fl = sizeof(from);
        int n = recvfrom(s, (char *)buf, sizeof(buf), 0, (SOCKADDR *)&from, &fl);
        if (n < 0) {
            int e = WSAGetLastError();
            if (e == WSAEMSGSIZE || e == WSAECONNRESET) continue;
            break;
        }
        if (n < (int)sizeof(okp_wire)) continue;
        okp_wire *w = (okp_wire *)buf;
        if (w->m0 != 'O' || w->m1 != 'K' || w->ver != OKP_WIRE_VER || w->from == 0 || w->from == c->self) continue;
        const uint8_t *body = buf + sizeof(okp_wire);
        int blen = n - sizeof(okp_wire);

        if (w->type == OKP_T_LAN_DISCOVER && s == c->lan_listen && blen >= 4 && memcmp(body, LAN_MAGIC, 4) == 0 &&
            lan_source(&from)) {
            if (c->is_host && c->session_open && !c->closing && c->enum_len > 0) send_offer(c, &from, w->from);
        } else if (w->type == OKP_T_LAN_OFFER && s == c->lan_q && w->to == c->self) {
            take_offer(c, w, body, blen, &from);
            got_offer = 1;
        }
    }
    if (got_offer && c->isp) okp_inject_rooms(c);
}
