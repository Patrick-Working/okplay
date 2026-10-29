/*
 * One DLL for Windows 95/98 up to 11: everything newer than Windows 98 that okplay uses is looked up
 * at run time here, with a fallback that works on the old system. Nothing in this file (or anywhere
 * in the drop-in) may import a function Windows 98 doesn't export - winedplay/check9x.sh checks the
 * built DLL against 98's real export tables.
 *
 *   newer Windows                          Windows 98 (and 95 with the Winsock 2 update)
 *   IPv6 + IPv4 on one socket              IPv4 socket; addresses stay v4-mapped inside okplay,
 *                                          converted at sendto/recvfrom (okp_sendto6/okp_recvfrom6)
 *   getaddrinfo                            gethostbyname (IPv4 only)
 *   inet_ntop / inet_pton                  our own (okp_ntop / okp_pton), used everywhere
 *   GetAdaptersAddresses                   GetAdaptersInfo (IPv4), in net.c/lan.c
 *   BCryptGenRandom                        CryptGenRandom, else a SHA-256 of whatever varies
 *   SHA-256 (bcrypt)                       our own (okp_sha256), used everywhere: same hash on all
 *   AddVectoredExceptionHandler            SetUnhandledExceptionFilter only (crash.c)
 *   ws2_32.dll (Winsock 2)                 wsock32.dll (1.1) for everything but WSAIoctl, which is
 *                                          looked up (a stock Windows 95 has no Winsock 2)
 *   GetModuleHandleEx(FROM_ADDRESS)        VirtualQuery's AllocationBase (okp_module_of)
 *   WinHTTP (HTTPS)                        plain HTTP over a socket (rdv.c)
 */

#include "okp.h"
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------ system */

static volatile LONG sys_state;    /* okp_sys_init: 0 new, 1 busy, 2 done */
static volatile LONG load_state;   /* okp_sys_load: the same */
okp_sys_t okp_sys;

/* a function from a DLL that is already loaded (nothing is loaded: safe inside DllMain) */
static FARPROC sym_loaded(const char *dll, const char *name)
{
    HMODULE m = GetModuleHandleA(dll);
    return m ? GetProcAddress(m, name) : NULL;
}

/* a function from a DLL, loading it if needed - never from DllMain (the loader lock) */
static FARPROC sym(const char *dll, const char *name)
{
    HMODULE m = GetModuleHandleA(dll);
    if (!m) m = LoadLibraryA(dll);
    return m ? GetProcAddress(m, name) : NULL;
}

static int emulating_9x(void)
{
    char e[8];
    return GetEnvironmentVariableA("OKP_EMULATE_9X", e, sizeof(e)) && e[0] == '1';
}

/* once, from any thread - including DllMain: the Windows version and functions of DLLs that are
 * already loaded. No InitOnce on 9x, and 95 has no InterlockedCompareExchange import - the builtin
 * compiles to lock cmpxchg (i486+). */
void okp_sys_init(void)
{
    if (sys_state == 2) return;
    if (__sync_val_compare_and_swap(&sys_state, 0, 1) == 0) {
        OSVERSIONINFOA v;
        memset(&v, 0, sizeof(v));
        v.dwOSVersionInfoSize = sizeof(v);
        GetVersionExA(&v);
        okp_sys.win9x = v.dwPlatformId != VER_PLATFORM_WIN32_NT;
        if (!okp_sys.win9x) {
            /* GetVersionEx says 6.2 on Windows 8+ to programs without a manifest; ntdll tells the truth */
            typedef LONG (WINAPI *rgv_t)(OSVERSIONINFOW *);
            HMODULE nt = GetModuleHandleA("ntdll.dll");
            rgv_t rgv = nt ? (rgv_t)(void *)GetProcAddress(nt, "RtlGetVersion") : NULL;
            OSVERSIONINFOW w;
            memset(&w, 0, sizeof(w));
            w.dwOSVersionInfoSize = sizeof(w);
            if (rgv && rgv(&w) == 0) {
                v.dwMajorVersion = w.dwMajorVersion;
                v.dwMinorVersion = w.dwMinorVersion;
                v.dwBuildNumber = w.dwBuildNumber;
            }
        }
        okp_sys.major = v.dwMajorVersion;
        okp_sys.minor = v.dwMinorVersion;
        okp_sys.build = v.dwBuildNumber & 0xffff;
        /* test knob: take the Windows 9x paths on a new Windows (every lookup "fails") */
        okp_sys.emulate9x = emulating_9x();
        if (!okp_sys.emulate9x) {
            okp_sys.AddVectoredExceptionHandler = (okp_aveh_t)sym_loaded("kernel32.dll", "AddVectoredExceptionHandler");
            okp_sys.CryptAcquireContextA = (okp_cac_t)sym_loaded("advapi32.dll", "CryptAcquireContextA");
            okp_sys.CryptGenRandom = (okp_cgr_t)sym_loaded("advapi32.dll", "CryptGenRandom");
        }
        sys_state = 2;
    }
    while (sys_state != 2) Sleep(0);
}

/* once, outside DllMain (SPInit, networking start): the optional DLLs - loading them under the loader
 * lock (DllMain) is a classic cause of rare hangs at game start */
void okp_sys_load(void)
{
    okp_sys_init();
    if (load_state == 2) return;
    if (__sync_val_compare_and_swap(&load_state, 0, 1) == 0) {
        if (!okp_sys.emulate9x) {
            okp_sys.getaddrinfo = (okp_getaddrinfo_t)sym("ws2_32.dll", "getaddrinfo");
            okp_sys.freeaddrinfo = (okp_freeaddrinfo_t)sym("ws2_32.dll", "freeaddrinfo");
            okp_sys.WSAIoctl = (okp_wsaioctl_t)sym("ws2_32.dll", "WSAIoctl");
            okp_sys.GetAdaptersAddresses = (okp_gaa_t)sym("iphlpapi.dll", "GetAdaptersAddresses");
            okp_sys.GetAdaptersInfo = (okp_gai_t)sym("iphlpapi.dll", "GetAdaptersInfo");
            okp_sys.BCryptGenRandom = (okp_bgr_t)sym("bcrypt.dll", "BCryptGenRandom");
            okp_sys.has_winhttp = sym("winhttp.dll", "WinHttpOpen") != NULL;
        }
        load_state = 2;
    }
    while (load_state != 2) Sleep(0);
}

const char *okp_sys_name(char *buf, int n)
{
    okp_sys_init();
    if (okp_sys.win9x)
        snprintf(buf, n, "Windows %s (4.%lu.%lu)", okp_sys.minor == 0 ? "95" : okp_sys.minor < 90 ? "98" : "ME",
                 okp_sys.minor, okp_sys.build);
    else
        snprintf(buf, n, "Windows NT %lu.%lu build %lu", okp_sys.major, okp_sys.minor, okp_sys.build);
    return buf;
}

/* the module an address belongs to (GetModuleHandleEx FROM_ADDRESS, without it) */
HMODULE okp_module_of(const void *addr)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(addr, &mbi, sizeof(mbi))) return NULL;
    return (HMODULE)mbi.AllocationBase;
}

/* ------------------------------------------------------------ SHA-256 (FIPS 180-4) */

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(okp_sha256_ctx *s, const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

void okp_sha256_init(okp_sha256_ctx *s)
{
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, iv, sizeof(iv));
    s->len = 0;
    s->fill = 0;
}

void okp_sha256_update(okp_sha256_ctx *s, const void *data, size_t n)
{
    const uint8_t *p = data;
    s->len += n;
    while (n) {
        size_t take = 64 - s->fill;
        if (take > n) take = n;
        memcpy(s->buf + s->fill, p, take);
        s->fill += take;
        p += take;
        n -= take;
        if (s->fill == 64) {
            sha256_block(s, s->buf);
            s->fill = 0;
        }
    }
}

void okp_sha256_final(okp_sha256_ctx *s, uint8_t out[32])
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, zero = 0, lenbe[8];
    int i;
    okp_sha256_update(s, &pad, 1);
    while (s->fill != 56) okp_sha256_update(s, &zero, 1);
    for (i = 0; i < 8; i++) lenbe[i] = (uint8_t)(bits >> (56 - 8 * i));
    okp_sha256_update(s, lenbe, 8);
    for (i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

/* ------------------------------------------------------------ random */

void okp_random(void *out, int n)
{
    static HCRYPTPROV prov;
    static volatile LONG prov_state;   /* 0 untried, 1 trying, 2 done */
    okp_sys_load();
    if (okp_sys.BCryptGenRandom &&
        okp_sys.BCryptGenRandom(NULL, (PUCHAR)out, (ULONG)n, 2 /* BCRYPT_USE_SYSTEM_PREFERRED_RNG */) == 0)
        return;
    if (okp_sys.CryptAcquireContextA && okp_sys.CryptGenRandom) {
        if (__sync_val_compare_and_swap(&prov_state, 0, 1) == 0) {
            if (!okp_sys.CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
                prov = 0;
            prov_state = 2;
        }
        while (prov_state != 2) Sleep(0);
        if (prov && okp_sys.CryptGenRandom(prov, (DWORD)n, (BYTE *)out)) return;
    }
    /* no system generator (a bare Windows 95): hash everything that differs between runs and calls */
    {
        static volatile LONG counter;
        uint8_t *o = out;
        while (n > 0) {
            okp_sha256_ctx s;
            uint8_t d[32];
            LARGE_INTEGER pc;
            SYSTEMTIME st;
            LONG k = InterlockedIncrement(&counter);
            DWORD pid = GetCurrentProcessId(), tid = GetCurrentThreadId(), tick = GetTickCount();
            MEMORYSTATUS ms;
            ms.dwLength = sizeof(ms);
            GlobalMemoryStatus(&ms);
            QueryPerformanceCounter(&pc);
            GetLocalTime(&st);
            okp_sha256_init(&s);
            okp_sha256_update(&s, &k, sizeof(k));
            okp_sha256_update(&s, &pc, sizeof(pc));
            okp_sha256_update(&s, &st, sizeof(st));
            okp_sha256_update(&s, &pid, sizeof(pid));
            okp_sha256_update(&s, &tid, sizeof(tid));
            okp_sha256_update(&s, &tick, sizeof(tick));
            okp_sha256_update(&s, &ms, sizeof(ms));
            okp_sha256_update(&s, &o, sizeof(o));
            okp_sha256_final(&s, d);
            memcpy(o, d, n < 32 ? n : 32);
            o += 32;
            n -= 32;
        }
    }
}

/* ------------------------------------------------------------ address text */

int okp_ntop(int af, const void *src, char *dst, int size)
{
    const uint8_t *b = src;
    if (af == AF_INET) {
        return snprintf(dst, size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]) < size;
    }
    if (af == AF_INET6) {
        /* RFC 5952: lower case, the longest run (2+) of zero groups as "::", v4-mapped as ::ffff:a.b.c.d */
        uint16_t g[8];
        int i, best = -1, bestlen = 0, run = -1, len = 0, pos = 0;
        char out[64];
        for (i = 0; i < 8; i++) g[i] = (uint16_t)(b[2 * i] << 8 | b[2 * i + 1]);
        if (!g[0] && !g[1] && !g[2] && !g[3] && !g[4] && g[5] == 0xffff && g[6])
            return snprintf(dst, size, "::ffff:%u.%u.%u.%u", b[12], b[13], b[14], b[15]) < size;
        if (!g[0] && !g[1] && !g[2] && !g[3] && !g[4] && !g[5] && g[6])   /* as Windows: ::a.b.c.d */
            return snprintf(dst, size, "::%u.%u.%u.%u", b[12], b[13], b[14], b[15]) < size;
        for (i = 0; i < 8; i++) {
            if (!g[i]) {
                if (run < 0) { run = i; len = 0; }
                if (++len > bestlen) { best = run; bestlen = len; }
            } else run = -1;
        }
        if (bestlen < 2) best = -1;
        for (i = 0; i < 8; i++) {
            if (i == best) {
                pos += snprintf(out + pos, sizeof(out) - pos, "::");
                i += bestlen - 1;
                continue;
            }
            pos += snprintf(out + pos, sizeof(out) - pos, "%s%x", pos && out[pos - 1] != ':' ? ":" : "", g[i]);
        }
        if (pos >= size) return 0;
        memcpy(dst, out, pos + 1);
        return 1;
    }
    return 0;
}

static int hexval(int ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static int parse_v4(const char *s, uint8_t out[4])
{
    int i;
    for (i = 0; i < 4; i++) {
        int v = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s++ - '0');
            if (++digits > 3 || v > 255) return 0;
        }
        if (!digits) return 0;
        out[i] = (uint8_t)v;
        if (i < 3 && *s++ != '.') return 0;
    }
    return *s == 0;
}

/* 1 = parsed, 0 = not an address of that family (inet_pton's contract) */
int okp_pton(int af, const char *src, void *dst)
{
    if (af == AF_INET) return parse_v4(src, dst);
    if (af == AF_INET6) {
        uint8_t out[16] = {0};
        int n = 0, gap = -1;
        const char *s = src;
        if (s[0] == ':' && s[1] == ':') { gap = 0; s += 2; }
        while (*s && n < 16) {
            const char *start = s;
            int v = 0, digits = 0, h;
            if (strchr(s, '.') && !strchr(s, ':')) {   /* trailing dotted IPv4 */
                if (n > 12 || !parse_v4(s, out + n)) return 0;
                n += 4;
                s += strlen(s);
                break;
            }
            while ((h = hexval(*s)) >= 0) {
                v = v * 16 + h;
                s++;
                if (++digits > 4) return 0;
            }
            if (!digits) return 0;
            (void)start;
            out[n++] = (uint8_t)(v >> 8);
            out[n++] = (uint8_t)v;
            if (*s == ':') {
                s++;
                if (*s == ':') {
                    if (gap >= 0) return 0;
                    gap = n;
                    s++;
                } else if (!*s) return 0;
            } else if (*s) return 0;
        }
        if (*s) return 0;
        if (gap >= 0) {
            int tail = n - gap;
            if (n == 16) return 0;
            memmove(out + 16 - tail, out + gap, tail);
            memset(out + gap, 0, 16 - tail - gap);
        } else if (n != 16) return 0;
        memcpy(dst, out, 16);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ IPv4-only sockets */

/* okplay keeps every address as SOCKADDR_IN6; an AF_INET socket (no IPv6 on this system) takes
 * v4-mapped ones as plain IPv4. A pure IPv6 destination can't be reached from it. */
int okp_sendto6(SOCKET s, int v4only, const void *buf, int len, const SOCKADDR_IN6 *to)
{
    if (!v4only) return sendto(s, (const char *)buf, len, 0, (const SOCKADDR *)to, sizeof(*to));
    if (!okp_addr_is_v4(to)) {
        WSASetLastError(WSAEAFNOSUPPORT);
        return SOCKET_ERROR;
    }
    SOCKADDR_IN a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = to->sin6_port;
    memcpy(&a.sin_addr, ((const uint8_t *)&to->sin6_addr) + 12, 4);
    return sendto(s, (const char *)buf, len, 0, (const SOCKADDR *)&a, sizeof(a));
}

int okp_recvfrom6(SOCKET s, int v4only, void *buf, int cap, SOCKADDR_IN6 *from)
{
    if (!v4only) {
        int fl = sizeof(*from);
        return recvfrom(s, (char *)buf, cap, 0, (SOCKADDR *)from, &fl);
    }
    SOCKADDR_IN a;
    int fl = sizeof(a), n;
    memset(&a, 0, sizeof(a));
    n = recvfrom(s, (char *)buf, cap, 0, (SOCKADDR *)&a, &fl);
    if (n >= 0) okp_addr_from_v4(from, &a.sin_addr, a.sin_port);
    return n;
}

/* ------------------------------------------------------------ name lookup */

/* host (name or literal) -> up to max addresses (v4-mapped for IPv4); IPv6 only where the system has
 * getaddrinfo. Returns the count. */
int okp_resolve(const char *host, uint16_t port_n, int family, SOCKADDR_IN6 *out, int max)
{
    int n = 0;
    okp_sys_load();
    if (okp_sys.getaddrinfo && okp_sys.freeaddrinfo) {
        struct addrinfo hints, *res = NULL, *ai;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = family == 4 ? AF_INET : family == 6 ? AF_INET6 : AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        if (okp_sys.getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return 0;
        for (ai = res; ai && n < max; ai = ai->ai_next) {
            if (ai->ai_family == AF_INET) {
                okp_addr_from_v4(&out[n++], &((SOCKADDR_IN *)ai->ai_addr)->sin_addr, port_n);
            } else if (ai->ai_family == AF_INET6) {
                memcpy(&out[n], ai->ai_addr, sizeof(SOCKADDR_IN6));
                out[n++].sin6_port = port_n;
            }
        }
        okp_sys.freeaddrinfo(res);
        return n;
    }
    if (family == 6) return 0;
    {
        uint8_t v4[4];
        struct hostent *he;
        if (okp_pton(AF_INET, host, v4)) {
            okp_addr_from_v4(&out[0], v4, port_n);
            return max > 0;
        }
        he = gethostbyname(host);   /* per-thread result on Windows */
        if (!he || he->h_addrtype != AF_INET) return 0;
        for (char **a = he->h_addr_list; *a && n < max; a++) okp_addr_from_v4(&out[n++], *a, port_n);
        return n;
    }
}

/* ------------------------------------------------------------ for the Wine side (builtin.c) */

/* take the Windows 9x code paths? (a real 9x, or the OKP_EMULATE_9X test knob) */
int okp_win9x_paths(void)
{
    okp_sys_init();
    return okp_sys.win9x || okp_sys.emulate9x;
}

/* SHA-256 over several pieces, one call */
void okp_sha256_parts(const void *const *parts, const size_t *lens, int n, uint8_t out[32])
{
    okp_sha256_ctx s;
    int i;
    okp_sha256_init(&s);
    for (i = 0; i < n; i++) okp_sha256_update(&s, parts[i], lens[i]);
    okp_sha256_final(&s, out);
}

/* UTF-16 -> UTF-8 without the system (Windows 95 has no CP_UTF8); an unpaired surrogate becomes
 * U+FFFD, as WideCharToMultiByte does. Returns the length without the NUL, or -1 if out is too small. */
int okp_utf16_to_utf8(const WCHAR *in, char *out, int outsz)
{
    int o = 0;
    while (*in) {
        uint32_t c = *in++;
        if (c >= 0xD800 && c <= 0xDBFF && *in >= 0xDC00 && *in <= 0xDFFF)
            c = 0x10000 + ((c - 0xD800) << 10) + (*in++ - 0xDC00);
        else if (c >= 0xD800 && c <= 0xDFFF)
            c = 0xFFFD;
        if (c < 0x80) {
            if (o + 1 >= outsz) return -1;
            out[o++] = (char)c;
        } else if (c < 0x800) {
            if (o + 2 >= outsz) return -1;
            out[o++] = (char)(0xC0 | c >> 6);
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            if (o + 3 >= outsz) return -1;
            out[o++] = (char)(0xE0 | c >> 12);
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (o + 4 >= outsz) return -1;
            out[o++] = (char)(0xF0 | c >> 18);
            out[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = 0;
    return o;
}

/* Windows 2000+ reports an ICMP "port unreachable" as WSAECONNRESET on the next recvfrom of a UDP
 * socket; switch that off where the system can (Winsock 2's WSAIoctl; 9x doesn't do it anyway) */
void okp_udp_no_connreset(SOCKET s)
{
    BOOL off = FALSE;
    DWORD ret;
    okp_sys_load();
    if (okp_sys.WSAIoctl) okp_sys.WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), NULL, 0, &ret, NULL, NULL);
}
