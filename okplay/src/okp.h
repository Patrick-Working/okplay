/* okplay - "Oldkid.net Online Play" / "Oldkid.net LAN Play" DirectPlay service provider.
 *
 * One UDP socket per game, peers addressed by random 64-bit peer IDs instead
 * of IP addresses. The same DLL is registered twice:
 *   Online Play - rooms via a small HTTP rendezvous (PHP on oldkid.net, or
 *                 server/rdv_standin.py for local tests) + STUN + hole punching
 *   LAN Play    - rooms via UDP broadcast on the local network, no internet at all
 */
#ifndef OKP_H
#define OKP_H

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <iphlpapi.h>
#include <stdint.h>
#include <stddef.h>
#include <dplay.h>
#include "dplaysp.h"

#include "version.h"

#ifdef OKP_BUILTIN
#define OKP_MODULE_DESC "dplayx.dll (Wine DirectPlay + okplay)"
#else
#define OKP_MODULE_DESC "okplay.dll"
#endif

/* {66D8DC59-28E8-4D3E-BA93-2E98D251C0F3} */
DEFINE_GUID(OKP_SPGUID, 0x66d8dc59, 0x28e8, 0x4d3e, 0xba, 0x93, 0x2e, 0x98, 0xd2, 0x51, 0xc0, 0xf3);
/* {6BCFF704-CE20-4150-98B4-5A7198175D7D} */
DEFINE_GUID(OKP_LANGUID, 0x6bcff704, 0xce20, 0x4150, 0x98, 0xb4, 0x5a, 0x71, 0x98, 0x17, 0x5d, 0x7d);

enum { OKP_MODE_ONLINE = 0, OKP_MODE_LAN = 1 };
#define OKP_LAN_PORT     47632   /* LAN Play discovery (UDP broadcast) */
#define OKP_LAN_ROOM_TTL 6000    /* ms a LAN offer stays in the list without a refresh */

#define OKP_MAX_PEERS   16
#define OKP_MAX_CANDS   8     /* per peer / local */
#define OKP_PDATA_CANDS 3     /* candidates carried in DirectPlay player data */
#define OKP_MAX_ROOMS   32
#define OKP_QMAX        64    /* queued DATA packets per peer while no path */
#define OKP_MAX_PKT     65000

/* ---- SP header: what DirectPlay stores for us and hands back in Reply/Open.
 * dplayx copies exactly dwSPHeaderSize bytes, so this struct IS that size. */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;          /* OKP_HDR_MAGIC */
    uint64_t peer;           /* sender / session host peer id */
    uint32_t room;
    uint32_t flags;          /* OKP_HF_* */
} okp_sphdr;
#pragma pack(pop)
#define OKP_HDR_SIZE   ((DWORD)sizeof(okp_sphdr))   /* 20 */
#define OKP_HDR_MAGIC  0x50504B4Fu                  /* "OKPP" */
#define OKP_HF_SELFPROBE 0x1u                       /* host's own enum probe */

/* ---- DirectPlay player data (shared, replicated by dplayx to all peers). */
#pragma pack(push, 1)
typedef struct {
    uint8_t  fam;            /* 4 or 6 */
    uint16_t port;           /* network order */
    uint8_t  addr[16];       /* v4 uses first 4 */
} okp_ccand;
typedef struct {
    uint32_t magic;          /* OKP_PD_MAGIC */
    uint64_t peer;
    uint8_t  ncands;
    okp_ccand cands[OKP_PDATA_CANDS];
} okp_pdata;
#pragma pack(pop)
#define OKP_PD_MAGIC 0x44504B4Fu /* "OKPD" */

/* ---- Wire format (our UDP packets). */
#pragma pack(push, 1)
typedef struct {
    uint8_t  m0, m1;         /* 'O','K' */
    uint8_t  ver;            /* OKP_WIRE_VER */
    uint8_t  type;           /* OKP_T_* */
    uint32_t room;
    uint64_t from;
    uint64_t to;
    uint64_t token;          /* 4+: the receiver's token for this sender (okp_peer); 0 until learned */
} okp_wire;
typedef struct {
    uint32_t nonce;
    uint32_t t_send;         /* sender's tick */
    uint32_t epoch;          /* 0.5.0+: the sender's attempt id for this peer (older probes stop
                                at t_send, OKP_PROBE_MIN bytes - both sizes are accepted) */
    uint64_t token;          /* 4+: "put this in your packets to me" (0 = none: the probe was not trusted) */
    uint8_t  host_mode;      /* 0.8.1+: from a host, 1 + its transport (OKP_TRANSPORT_*); 0 = not a host.
                                Older peers stop reading at token (they only check a minimum length). */
} okp_probe;
#define OKP_PROBE_MIN 8
#define OKP_PROBE_V4  ((int)offsetof(okp_probe, host_mode))       /* 20: up to and including token, what 0.8.0 sends */
typedef struct {
    uint32_t nonce;
    uint32_t t_echo;
    uint8_t  obs_fam;        /* observed address of the prober */
    uint16_t obs_port;
    uint8_t  obs_addr[16];
    uint64_t token;          /* 4+: the acker's token for the prober (0: the probe was not trusted) */
    uint8_t  host_mode;      /* 0.8.1+: as in okp_probe */
} okp_probe_ack;
#define OKP_PROBE_ACK_V4 ((int)offsetof(okp_probe_ack, host_mode)) /* 35: up to and including token, what 0.8.0 sends */
#pragma pack(pop)
/* v2 (okplay 0.2): reliable lane. Older peers drop v2 packets, so mixed versions fail fast. */
/* 2: okplay 0.2 on Windows' DirectPlay. 3: the drop-in (Wine's DirectPlay): same transport, but
 * its DirectPlay layer can't talk to Windows' (no DPLAY protocol wrapping), so the two must never
 * meet - every packet, LAN offer and server listing carries this number and mismatches are ignored.
 * 4 (0.8.0): every packet to a peer carries the token that peer gave us (okp_peer my_token /
 * their_token), so an outsider who read the room list can't speak as a player any more. */
#define OKP_WIRE_VER 4
enum { OKP_T_PROBE = 1, OKP_T_PROBE_ACK = 2, OKP_T_DATA = 3,
       OKP_T_LAN_DISCOVER = 4,   /* joiner -> broadcast: "any games here?" */
       OKP_T_LAN_OFFER = 5,      /* host -> joiner: room, enum reply, candidates */
       OKP_T_RDATA = 6,          /* reliable, in-order: dplayx's own "play" messages */
       OKP_T_RACK = 7,           /* ack for RDATA/RFRAG: cumulative + the specific seq */
       OKP_T_RFRAG = 8 };        /* reliable, in-order: one piece of a bigger message; the last piece is an RDATA */

/* Big messages are split by us, never by dplayx: Windows dplayx's own packetizer (used when a
 * message exceeds the SP's dwMaxBufferSize) builds a corrupt second packet for the ~1.1 KB player
 * list of a 6-player game, and the joiner never acks it (found by the 8-player test). So we
 * advertise OKP_CAPS_MAXBUF and cut anything over OKP_FRAG_MAX into reliable in-order pieces. */
#define OKP_FRAG_MAX    1200     /* payload bytes per datagram - stays under internet MTUs */
#define OKP_CAPS_MAXBUF 16384    /* what GetCaps tells dplayx */
#define OKP_MSG_MAX     65536    /* reassembly cap */

#define OKP_RTX_MAX 128          /* unacked reliable messages per peer (the send window) */
/* Reliable pieces that don't fit the window wait in a per-peer queue, in order, and go out as acks
 * free slots (a guaranteed message is never dropped for being one too many, as Windows' TCP never
 * drops one for a full send buffer). The cap only stops a peer that never acks from eating memory. */
#define OKP_PEND_MAX_BYTES (4 * 1024 * 1024)
typedef struct okp_pend {
    struct okp_pend *next;
    int type;                    /* OKP_T_RDATA or OKP_T_RFRAG */
    int len;
    uint8_t data[1];             /* len bytes */
} okp_pend;
typedef struct {
    uint32_t seq;
    int len;
    uint8_t *pkt;                /* full wire packet, ready to resend */
    DWORD first, last;
    int tries;
} okp_rtx;
typedef struct {
    uint32_t seq;
    int len;
    int frag;                    /* 1 = RFRAG piece (more follow) */
    uint8_t *data;               /* payload waiting for an earlier seq */
} okp_rbuf;

typedef struct {
    int len;
    DWORD t;
    uint8_t *data;
} okp_qent;

typedef struct {
    int used;
    uint64_t id;
    SOCKADDR_IN6 cands[OKP_MAX_CANDS];
    int ncands;
    int have_path;
    SOCKADDR_IN6 path;
    DWORD last_rx, last_path_rx, last_probe, probe_start, path_since;
    DWORD rtt;
    okp_qent q[OKP_QMAX];
    int qn;
    uint32_t tx_pkts, rx_pkts, dropped_q;
    uint32_t tx_bytes, rx_bytes, rx_sys, rx_game;
    uint32_t path_changes;
    /* reliable lane */
    uint32_t r_tx_next, r_rx_next;
    okp_rtx rtx[OKP_RTX_MAX];
    int nrtx;
    okp_rbuf rbuf[OKP_RTX_MAX];
    int nrbuf;
    uint32_t r_sent, r_retrans, r_giveup, r_dups;
    okp_pend *pend, *pend_tail;    /* waiting for window space, oldest first */
    int npend, pend_bytes, pend_peak;
    uint32_t r_queued;             /* pieces that had to wait */
    uint8_t *reasm;                /* pieces of the message being reassembled */
    int reasm_len, reasm_bad;
    /* each side's attempt id for this peer (sent in probes): a new one from the other side means it
     * threw its state away (e.g. retrying a join after a refusal) - our reliable lane restarts too */
    uint32_t my_epoch, their_epoch;
    /* 4+: a packet from this peer must carry my_token (ours for it, told in our probes and acks); ours
     * to it carry their_token (learned from its probes and acks). A probe without our token is believed -
     * and told the token - only from an address we already know for the peer, or as the id's very first
     * contact; otherwise it is answered without one, so nobody is told the token for an established peer */
    uint64_t my_token, their_token;
    uint32_t rx_badtoken, rx_untrusted_probe;

    int nplayers;                  /* DirectPlay players living on this peer */
    DWORD keep_until;              /* retain without players until this tick */
    DWORD last_nopath_log;
} okp_peer;

typedef struct {
    uint32_t room;
    uint64_t host;
    SOCKADDR_IN6 cands[OKP_MAX_CANDS];
    int ncands;
    uint8_t enumblob[1024];
    int enumlen;
    char name[128];
    DWORD seen;                    /* LAN: tick of the last offer */
    char build[17];                /* the host's game exe fingerprint (okp_exe_build), "" = not told */
    char exe[64];                  /* the host's game exe file name (UTF-8), for the warning */
} okp_room;

typedef struct {
    IDirectPlaySP *isp;
    CRITICAL_SECTION lock;
    int mode;                      /* OKP_MODE_* (chosen by the GUID dplayx loaded us for) */
    const char *mode_name;

    SOCKET sock;
    int v4only;        /* no IPv6 on this system: sock is AF_INET (see compat.c) */
    uint16_t port;                 /* host order */
    uint64_t self;
    uint32_t room;
    uint64_t host_peer;            /* name server peer (when joined) */
    int is_host;
    int host_transport;            /* the host's transport, told in its probes (-1 = not known) */
    /* the room's state for the room list (rdv.c room_state): lobby / playing / between games */
    int room_state, state_hi, state_lo;
    int state_rate;                /* smoothed messages/s per player's machine, in tenths */
    int state_base, state_base_n;  /* the room's own lobby rate (tenths) and how many looks made it */
    int state_play;                /* the smoothed rate while in a game (tenths) */
    uint64_t state_msgs;           /* messages delivered from players' machines at the last look */
    DWORD state_tick;
    int session_open;
    char tok[24];                  /* host announce token */

    SOCKADDR_IN6 cands[OKP_MAX_CANDS];
    int ncands;
    SOCKADDR_IN6 srflx;
    int have_srflx;
    const char *srflx_src;         /* "STUN" or "peer" */
    SOCKADDR_IN6 guess;            /* fallback: public IP seen by oldkid.net + our local port */
    int have_guess;
    char public_ip[64];            /* as seen by the rendezvous */

    okp_peer peers[OKP_MAX_PEERS];

    /* host: latest enum reply captured from our own dplayx */
    uint8_t enum_blob[1024];
    int enum_len;
    uint16_t dp_version;           /* envelope version seen from local dplayx */

    /* joiner: rooms from the last listing */
    okp_room rooms[OKP_MAX_ROOMS];
    int nrooms;
    int enum_pending;
    GUID enum_app;
    DWORD enum_flags;

    HANDLE net_thread, rdv_thread, rdv_wake;
    volatile LONG running;
    volatile LONG closing;         /* session closed: host should withdraw */

    DWORD last_stats;

    /* LAN Play */
    SOCKET lan_q;                  /* joiner: ephemeral IPv4 socket for DISCOVER/OFFER */
    SOCKET lan_listen;             /* host: bound to OKP_LAN_PORT while hosting */
    DWORD last_discover;

    /* STUN */
    uint8_t stun_txid[12];
    SOCKADDR_IN6 stun_addr[8];     /* STUN servers, looked up once by the rdv thread (not the net thread) */
    int nstun;
    volatile int stun_resolved;
    DWORD stun_sent;
    int stun_tries;

    /* this game, for the connection report (report.c) */
    DWORD sess_t0;
    int sess_role;                 /* 0 = no game, 1 = host, 2 = joiner */
    int sess_peers;
    char sess_result[32];          /* "ok" or how the join failed */
} okp_ctx;

/* util.c */
void okp_log(int level, const char *fmt, ...);
void okp_log_init(HMODULE self);
extern int okp_loglevel;
typedef struct {
    char server[256];
    char key[128];
    int port;
    char stun[256];
    int drop_pct;
    int announce_ms;
    int family;        /* test knob: 0 = all, 4 = advertise IPv4 only, 6 = IPv6 only */
    int crashdump;     /* write a minidump on crash (default on) */
    int stats_ms;      /* periodic per-peer stats line interval, 0 = off */
    int trace;         /* 1 = hex-trace every dplayx system message sent/received (debugging) */
    int transport;     /* OKP_TRANSPORT_*: which sends go on the reliable lane (okplay.ini transport=) */
    int drop_after_ms; /* okplay.ini drop_after= (s): silence before a machine's players are dropped */
} okp_config;
/* drop_after= : how long a machine may go silent (an internet outage, a Wi-Fi reconnect) before
 * DirectPlay drops its players. 15 s = what Windows' DirectPlay does (the default); up to 120 s keeps
 * a player through longer outages - the game waits meanwhile. Each PC applies its own value to the
 * others it hears from. The reliable lane keeps resending at least that long (net.c). */
#define OKP_DROP_AFTER_DEFAULT_MS 15000
int okp_drop_after_ms(void);
/* The game's build: the first 16 hex digits of the SHA-256 of its exe, and the exe's file name
 * (UTF-8). Rooms carry the host's; a joiner on a different build is warned (build_check=). */
void okp_exe_build(char build[17], char *exe, int exesz);
/* the host build of the room whose session is `instance` (as dplayx joins it): 1 = known */
int okp_room_build(IDirectPlaySP *isp, const GUID *instance, char build[17], char *exe, int exesz);
/* Transport: which sends ride the reliable lane. A HOST setting: the host's choice travels in its
 * probes (okp_probe.host_mode) and every joiner in that game sends the same way.
 *   transport=       the mode when this PC hosts (okplay.ini; env OKP_TRANSPORT overrides; re-read
 *                    at every session start, so a test tool can switch it between games)
 *   transport_force= this PC's own sends always this way, whatever the host says (advanced;
 *                    env OKP_TRANSPORT_FORCE)
 * Values (older names still accepted):
 *   udp            "Fast": everything as datagrams; only dplayx's own messages and > OKP_FRAG_MAX
 *                  ride the reliable lane (the behaviour of 0.8.0 and older)
 *   udp+reliable   "DirectPlay-style" (old name udp+tcp), the default since 0.8.1: DPSEND_GUARANTEED
 *                  messages reliable and in order, the rest as datagrams - what Windows' TCP/IP
 *                  provider does with TCP+UDP. A 0.8.0 joiner ignores the host's choice and sends Fast.
 *   reliable       "All reliable" (old name tcp): every message reliable and in order
 * The reliable lane is okplay's acknowledged, resent, ordered stream over the same hole-punched UDP
 * path: TCP's guarantees without a TCP connection (TCP hole punching works through fewer routers). */
#define OKP_TRANSPORT_UDP     0
#define OKP_TRANSPORT_UDP_TCP 1
#define OKP_TRANSPORT_TCP     2
void okp_transport_refresh(void);
const char *okp_transport_name(int t);
int okp_transport_force(void);                 /* -1 = follow the host */
int okp_effective_transport(const okp_ctx *c); /* what this PC's sends use now */
#define OKP_SEND_GUARANTEED 1   /* okp_send_data flags */
const char *okp_log_dir(void);
const char *okp_log_path(void);
const char *okp_dll_dir(void);   /* this DLL's folder (okplay.ini), with a trailing backslash */
void okp_log_header(void);
void okp_log_sysinfo(void);   /* crash.c: once, outside DllMain */
void okp_trace(const char *dir, uint64_t peer, const void *msg, int len);

/* crash.c */
void okp_crash_install(HMODULE self);
extern okp_config okp_cfg;
void okp_config_load(void);
uint64_t okp_rand64(void);
const char *okp_addr_str(const SOCKADDR_IN6 *a, char *buf, int n);
int okp_addr_parse(const char *s, SOCKADDR_IN6 *out);
int okp_addr_eq(const SOCKADDR_IN6 *a, const SOCKADDR_IN6 *b);
void okp_addr_from_v4(SOCKADDR_IN6 *out, const void *v4, uint16_t port_n);
int okp_addr_is_v4(const SOCKADDR_IN6 *a);
int okp_addr_is_public_v4(const SOCKADDR_IN6 *a);
int okp_b64_encode(const uint8_t *in, int n, char *out, int outsz);
int okp_b64_decode(const char *in, uint8_t *out, int outsz);
int okp_urlenc(const char *in, char *out, int outsz);
int okp_urldec(const char *in, char *out, int outsz);
char *okp_tok(char **state, const char *delims);
DWORD okp_now(void);

/* compat.c - one DLL for Windows 95/98 .. 11: newer functions looked up at run time */
typedef INT (WSAAPI *okp_getaddrinfo_t)(PCSTR, PCSTR, const ADDRINFOA *, PADDRINFOA *);
typedef VOID (WSAAPI *okp_freeaddrinfo_t)(PADDRINFOA);
typedef ULONG (WINAPI *okp_gaa_t)(ULONG, ULONG, PVOID, PIP_ADAPTER_ADDRESSES, PULONG);
typedef DWORD (WINAPI *okp_gai_t)(PIP_ADAPTER_INFO, PULONG);
typedef PVOID (WINAPI *okp_aveh_t)(ULONG, PVECTORED_EXCEPTION_HANDLER);
typedef LONG (WINAPI *okp_bgr_t)(PVOID, PUCHAR, ULONG, ULONG);
typedef BOOL (WINAPI *okp_cac_t)(HCRYPTPROV *, LPCSTR, LPCSTR, DWORD, DWORD);
typedef BOOL (WINAPI *okp_cgr_t)(HCRYPTPROV, DWORD, BYTE *);
typedef int (WSAAPI *okp_wsaioctl_t)(SOCKET, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPWSAOVERLAPPED,
                                     LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef struct {
    int win9x;                          /* Windows 95/98/ME */
    DWORD major, minor, build;
    okp_getaddrinfo_t getaddrinfo;      /* NULL on 9x (and 2000 without the IPv6 pack) */
    okp_freeaddrinfo_t freeaddrinfo;
    okp_gaa_t GetAdaptersAddresses;     /* XP+ */
    okp_gai_t GetAdaptersInfo;          /* 98+ */
    okp_aveh_t AddVectoredExceptionHandler;
    okp_bgr_t BCryptGenRandom;          /* Vista+ */
    okp_cac_t CryptAcquireContextA;
    okp_cgr_t CryptGenRandom;
    okp_wsaioctl_t WSAIoctl;            /* Winsock 2 (ws2_32): not on a stock Windows 95 */
    int has_winhttp;                    /* 2000 SP3+: HTTPS; else plain HTTP */
    int emulate9x;                      /* test knob OKP_EMULATE_9X=1: all of the above "missing", no IPv6 */
} okp_sys_t;
extern okp_sys_t okp_sys;
void okp_sys_init(void);   /* safe in DllMain: version + already-loaded DLLs */
void okp_sys_load(void);   /* not in DllMain: loads the optional DLLs (ws2_32, iphlpapi, bcrypt, winhttp) */
const char *okp_sys_name(char *buf, int n);
HMODULE okp_module_of(const void *addr);
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t fill; } okp_sha256_ctx;
void okp_sha256_init(okp_sha256_ctx *s);
void okp_sha256_update(okp_sha256_ctx *s, const void *data, size_t n);
void okp_sha256_final(okp_sha256_ctx *s, uint8_t out[32]);
void okp_random(void *out, int n);
int okp_ntop(int af, const void *src, char *dst, int size);
int okp_pton(int af, const char *src, void *dst);
int okp_sendto6(SOCKET s, int v4only, const void *buf, int len, const SOCKADDR_IN6 *to);
int okp_recvfrom6(SOCKET s, int v4only, void *buf, int cap, SOCKADDR_IN6 *from);
int okp_resolve(const char *host, uint16_t port_n, int family, SOCKADDR_IN6 *out, int max);
void okp_udp_no_connreset(SOCKET s);
#ifdef OKP_BUILTIN
void DP_BUILTIN_OfferUpdate(const char *latest, const char *note_utf8);   /* builtin.c: the update box */
void DP_BUILTIN_NetworkUnavailable(int err);   /* builtin.c: the "TCP/IP isn't working" box */
void DP_BUILTIN_ServerUnreachable(const char *server, const char *why);   /* builtin.c: the "can't reach the server" box */
#endif
int okp_win9x_paths(void);
void okp_sha256_parts(const void *const *parts, const size_t *lens, int n, uint8_t out[32]);
int okp_utf16_to_utf8(const WCHAR *in, char *out, int outsz);

/* net.c */
int  okp_net_start(okp_ctx *c);
void okp_net_stop(okp_ctx *c);
void okp_gather_cands(okp_ctx *c);
void okp_stun_resolve(okp_ctx *c);   /* rdv thread: looks the STUN servers up once */
int  okp_cands_to_str(const SOCKADDR_IN6 *cands, int n, char *out, int outsz);
int  okp_cands_from_str(const char *s, SOCKADDR_IN6 *cands, int max);
okp_peer *okp_peer_get(okp_ctx *c, uint64_t id, int create);   /* lock held */
void okp_peer_add_cands(okp_ctx *c, okp_peer *p, const SOCKADDR_IN6 *cands, int n); /* lock held; drops unusable addresses */
int  okp_cand_ok(const okp_ctx *c, const SOCKADDR_IN6 *a);
int  okp_pdata_parse(const void *buf, DWORD sz, uint64_t *peer, SOCKADDR_IN6 *out, int max);
int  okp_enum_blob_ok(const uint8_t *b, int n);
void okp_peer_free_bufs(okp_peer *p);                                   /* lock held */
int  okp_send_data(okp_ctx *c, uint64_t to, const void *buf, int len, int flags);
int  okp_wait_path(okp_ctx *c, uint64_t peer, DWORD timeout_ms);
void okp_local_cands(okp_ctx *c, SOCKADDR_IN6 *out, int *n, int max); /* incl. srflx */

/* rdv.c */
int  okp_rdv_start(okp_ctx *c);
void okp_rdv_stop(okp_ctx *c);
void okp_rdv_wake(okp_ctx *c);
int  okp_rdv_knock(okp_ctx *c, uint32_t room);
int  okp_rdv_close(okp_ctx *c);

int  okp_rdv_post_simple(const char *body);   /* one request outside any game, no boxes: 1 = "ok" */

/* crash.c: the crash report a crash leaves for report.c to offer (0.8.3) */
#define OKP_CRASH_REPORT_FILE "okplay-crash-report.txt"   /* in okp_log_dir() */
void okp_scrub(const char *in, char *out, int n);   /* a log line without addresses or folders */
void okp_report_crash_begin(void);                  /* start of a crash */
void okp_report_crash_add(const char *fmt, ...);    /* one line for the report */
void okp_report_crash_end(void);                    /* keep the report to offer next time */
void okp_exe_name(char *out, int n);                /* the game exe's file name (no folder) */

/* report.c - opt-in crash and connection reports (0.8.3) */
void okp_reports_kick(void);                        /* SPInit: offer/send what is waiting (own thread) */
void okp_report_session_begin(okp_ctx *c, int host);
void okp_report_session_result(okp_ctx *c, const char *result);
void okp_report_session_end(okp_ctx *c);
void okp_report_set_ini(const char *key, const char *value);

/* lan.c */
int  okp_lan_open_query(okp_ctx *c);
int  okp_lan_open_listen(okp_ctx *c);
void okp_lan_close(okp_ctx *c);
void okp_lan_discover(okp_ctx *c);
void okp_lan_readable(okp_ctx *c, SOCKET s);

/* sp.c */
void okp_capture_enum_reply(okp_ctx *c, const uint8_t *msg, int len);
void okp_inject_rooms(okp_ctx *c);
void okp_self_probe(okp_ctx *c);
#ifdef OKP_BUILTIN
/* for dplay.c: who sent a message - the peer named in our SP header (0: not our header) - and the peer
 * id a machine's system player carries in its SP data (0: not okplay's data) */
uint64_t okp_header_peer(const void *hdr, DWORD hdrsize);
uint64_t okp_spdata_peer(const void *data, DWORD size);
#endif

#endif
