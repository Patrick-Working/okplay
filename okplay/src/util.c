/* okplay - logging, config, random, address and encoding helpers. */
#include "okp.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

int okp_loglevel = 2;
okp_config okp_cfg;

static HANDLE g_log = INVALID_HANDLE_VALUE;
/* Windows 9x has no FILE_APPEND_DATA (CreateFile fails with it): there the log is opened for plain
 * writing and each line seeks to the end under this lock. Decided in DllMain, so kernel32 only. */
static int g_log_seek;
static CRITICAL_SECTION g_log_cs;

static int log_9x(void)
{
    char e[8];
    if (GetVersion() & 0x80000000) return 1;   /* Windows 95/98/ME */
    return GetEnvironmentVariableA("OKP_EMULATE_9X", e, sizeof(e)) && e[0] == '1';
}
static char g_dir[MAX_PATH];       /* folder holding okplay.dll + okplay.ini */
static char g_logdir[MAX_PATH];    /* where session logs and crash dumps go */
static char g_logpath[MAX_PATH];

DWORD okp_now(void) { return GetTickCount(); }
const char *okp_log_dir(void) { return g_logdir; }
const char *okp_log_path(void) { return g_logpath; }
const char *okp_dll_dir(void) { return g_dir; }

/* Keep the newest `keep` files matching pattern in g_logdir. */
static void prune_files(const char *pattern, int keep)
{
    char spec[MAX_PATH];
    snprintf(spec, sizeof(spec), "%s%s", g_logdir, pattern);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(spec, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    char names[256][64];
    int n = 0;
    do {
        if (n < 256 && strlen(fd.cFileName) < 64) lstrcpynA(names[n++], fd.cFileName, 64);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    /* names embed YYYYMMDD-HHMMSS, so lexical order is chronological */
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (strcmp(names[i], names[j]) > 0) {
                char t[64];
                memcpy(t, names[i], 64); memcpy(names[i], names[j], 64); memcpy(names[j], t, 64);
            }
    for (int i = 0; i < n - keep; i++) {
        char p[MAX_PATH];
        snprintf(p, sizeof(p), "%s%s", g_logdir, names[i]);
        DeleteFileA(p);
    }
}

int okp_log_reused;   /* this process already had a log file: append, short header */

static int try_logdir(const char *base)
{
    char d[MAX_PATH];
#ifdef OKP_BUILTIN
    snprintf(d, sizeof(d), "%snetplay-logs\\", base);   /* next to the game: say whose logs */
#else
    snprintf(d, sizeof(d), "%slogs\\", base);
#endif
    CreateDirectoryA(d, NULL);
    SYSTEMTIME st;
    GetLocalTime(&st);
    char p[MAX_PATH];
    /* AoE2 loads and unloads the provider every few seconds on its Join screen; keep one file per
     * game run. The file is remembered in the process's own environment, which outlives the DLL
     * but not the process - a process id alone is not enough: Windows 98 gives the next run of
     * the same game the same id (2026-10-16, the 86Box VMs). "<pid> <path>": a child process
     * inherits the variable but not the id. */
    char mark[MAX_PATH + 16], *sp;
    DWORD ml = GetEnvironmentVariableA("OKPLAY_LOG", mark, sizeof(mark));
    if (ml > 0 && ml < sizeof(mark) && (sp = strchr(mark, ' ')) && strtoul(mark, NULL, 10) == GetCurrentProcessId() &&
        !_strnicmp(sp + 1, d, strlen(d)) && GetFileAttributesA(sp + 1) != INVALID_FILE_ATTRIBUTES) {
        lstrcpynA(p, sp + 1, sizeof(p));
        okp_log_reused = 1;
    } else {
        snprintf(p, sizeof(p), "%sokplay-%04d%02d%02d-%02d%02d%02d-%lu.log", d, st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
    }
    int seek = log_9x();
    HANDLE h = CreateFileA(p, seek ? GENERIC_WRITE : FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (seek) {
        InitializeCriticalSection(&g_log_cs);
        g_log_seek = 1;
    }
    g_log = h;
    lstrcpynA(g_logdir, d, sizeof(g_logdir));
    lstrcpynA(g_logpath, p, sizeof(g_logpath));
    snprintf(mark, sizeof(mark), "%lu %s", GetCurrentProcessId(), p);
    SetEnvironmentVariableA("OKPLAY_LOG", mark);
    return 1;
}

/* Called from DllMain: kernel32-only work (no LoadLibrary). */
void okp_log_init(HMODULE self)
{
    GetModuleFileNameA(self, g_dir, sizeof(g_dir));
    char *slash = strrchr(g_dir, '\\');
    if (slash) slash[1] = 0; else g_dir[0] = 0;

    okp_config_load();
    if (okp_loglevel <= 0) return;

    /* next to the DLL if writable, else %LOCALAPPDATA%\okplay\ */
    if (!try_logdir(g_dir)) {
        char base[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof(base));
        if (n && n < sizeof(base) - 10) {
            lstrcatA(base, "\\okplay\\");
            CreateDirectoryA(base, NULL);
            try_logdir(base);
        }
    }
    if (g_log != INVALID_HANDLE_VALUE) {
        prune_files("okplay-*.log", 40);
        prune_files("okplay-crash-*.dmp", 10);
    }
}

void okp_log(int level, const char *fmt, ...)
{
    if (level > okp_loglevel || g_log == INVALID_HANDLE_VALUE) return;
    char line[1400];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [%5lu] %c ",
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                     GetCurrentProcessId(), "EWID"[level < 4 ? level : 3]);
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    n += m;
    if (n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
    line[n++] = '\r';
    line[n++] = '\n';
    DWORD w;
    if (g_log_seek) {   /* Windows 9x: no append access, so seek to the end under the lock */
        EnterCriticalSection(&g_log_cs);
        static DWORD last_flush;
        SetFilePointer(g_log, 0, NULL, FILE_END);
        WriteFile(g_log, line, n, &w, NULL);
        /* 9x only updates the file's size on disk at close/flush: if the game dies with the log
         * open, unflushed lines are lost - exactly when the log matters most. Errors and warnings
         * go to disk at once; the chatty debug lines at most once a second (a disk write per line
         * stuttered slow PCs). The crash handler logs at level 0, so a crash flushes everything. */
        if (level <= 1 || GetTickCount() - last_flush >= 1000) {
            FlushFileBuffers(g_log);
            last_flush = GetTickCount();
        }
        LeaveCriticalSection(&g_log_cs);
        return;
    }
    WriteFile(g_log, line, n, &w, NULL);   /* one write per line: appends stay whole */
}

static void ini_str(const char *ini, const char *k, const char *def, char *out, int n)
{
    GetPrivateProfileStringA("okplay", k, def, out, n, ini);
}

const char *okp_transport_name(int t)
{
    return t == OKP_TRANSPORT_TCP ? "reliable" : t == OKP_TRANSPORT_UDP_TCP ? "udp+reliable" : "udp";
}

static int transport_parse(const char *v)
{
    if (!_stricmp(v, "reliable") || !_stricmp(v, "tcp") || !_stricmp(v, "all")) return OKP_TRANSPORT_TCP;
    if (!_stricmp(v, "udp+reliable") || !_stricmp(v, "udp+tcp") || !_stricmp(v, "directplay")) return OKP_TRANSPORT_UDP_TCP;
    if (!_stricmp(v, "udp") || !_stricmp(v, "fast")) return OKP_TRANSPORT_UDP;
    return -1;
}

static int g_transport_force = -1;
int okp_transport_force(void) { return g_transport_force; }

int okp_effective_transport(const okp_ctx *c)
{
    if (g_transport_force >= 0) return g_transport_force;
    if (c && !c->is_host && c->host_transport >= 0) return c->host_transport;   /* the host decides */
    return okp_cfg.transport;
}

/* transport= (okp.h): the ini, then env OKP_TRANSPORT; called at load and at every session start */
void okp_transport_refresh(void)
{
    char ini[MAX_PATH], v[32];
    snprintf(ini, sizeof(ini), "%sokplay.ini", g_dir);
    GetPrivateProfileStringA("okplay", "transport", "udp+reliable", v, sizeof(v), ini);
    GetEnvironmentVariableA("OKP_TRANSPORT", v, sizeof(v));
    int t = transport_parse(v);
    if (t < 0) t = OKP_TRANSPORT_UDP_TCP;   /* the default: DirectPlay-style (okp.h) */
    if (t != okp_cfg.transport) okp_log(2, "transport when hosting: %s", okp_transport_name(t));   /* changes between games */
    okp_cfg.transport = t;
    v[0] = 0;
    GetPrivateProfileStringA("okplay", "transport_force", "", v, sizeof(v), ini);
    GetEnvironmentVariableA("OKP_TRANSPORT_FORCE", v, sizeof(v));
    int f = v[0] ? transport_parse(v) : -1;
    if (f != g_transport_force && f >= 0) okp_log(2, "transport forced for this PC: %s", okp_transport_name(f));
    g_transport_force = f;
}

void okp_config_load(void)
{
    char ini[MAX_PATH];
    snprintf(ini, sizeof(ini), "%sokplay.ini", g_dir);
    ini_str(ini, "server", "https://oldkid.net/aoe2net/okplay.php", okp_cfg.server, sizeof(okp_cfg.server));
    ini_str(ini, "key", "okp_7Qm2xV9rLs4Hd8Wn", okp_cfg.key, sizeof(okp_cfg.key));
    ini_str(ini, "stun", "stun.cloudflare.com:3478,stun.l.google.com:19302,stun1.l.google.com:19302,stun.nextcloud.com:443",
            okp_cfg.stun, sizeof(okp_cfg.stun));
    okp_cfg.port = GetPrivateProfileIntA("okplay", "port", 0, ini);
#ifdef OKP_BUILTIN
    okp_cfg.drop_pct = 0;   /* the drop-in: packet-loss simulation only via OKP_DROP_PCT (tests) */
#else
    okp_cfg.drop_pct = GetPrivateProfileIntA("okplay", "drop_pct", 0, ini);
#endif
    okp_cfg.announce_ms = GetPrivateProfileIntA("okplay", "announce_ms", 1000, ini);
    okp_cfg.family = GetPrivateProfileIntA("okplay", "family", 0, ini);
    /* 3 = debug: verbose by default while we're testing with the real game */
    okp_loglevel = GetPrivateProfileIntA("okplay", "loglevel", 3, ini);
    okp_cfg.crashdump = GetPrivateProfileIntA("okplay", "crashdump", 1, ini);
    okp_cfg.stats_ms = GetPrivateProfileIntA("okplay", "stats_ms", 10000, ini);
    okp_cfg.drop_after_ms = GetPrivateProfileIntA("okplay", "drop_after", OKP_DROP_AFTER_DEFAULT_MS / 1000, ini) * 1000;

    /* per-process overrides for tests (the ini is shared by every process on the PC) */
    char v[256];
    if (GetEnvironmentVariableA("OKP_SERVER", v, sizeof(v))) lstrcpynA(okp_cfg.server, v, sizeof(okp_cfg.server));
    v[0] = 0;
    SetLastError(0);
    if (GetEnvironmentVariableA("OKP_STUN", v, sizeof(v)) || GetLastError() != ERROR_ENVVAR_NOT_FOUND)
        lstrcpynA(okp_cfg.stun, v, sizeof(okp_cfg.stun));   /* empty value disables STUN */
    if (GetEnvironmentVariableA("OKP_DROP_PCT", v, sizeof(v))) okp_cfg.drop_pct = atoi(v);
    if (GetEnvironmentVariableA("OKP_FAMILY", v, sizeof(v))) okp_cfg.family = atoi(v);
    if (GetEnvironmentVariableA("OKP_LOGLEVEL", v, sizeof(v))) okp_loglevel = atoi(v);
    if (GetEnvironmentVariableA("OKP_STATS_MS", v, sizeof(v))) okp_cfg.stats_ms = atoi(v);
    if (GetEnvironmentVariableA("OKP_DROP_AFTER", v, sizeof(v))) okp_cfg.drop_after_ms = atoi(v) * 1000;
    if (okp_cfg.drop_after_ms < OKP_DROP_AFTER_DEFAULT_MS) okp_cfg.drop_after_ms = OKP_DROP_AFTER_DEFAULT_MS;
    if (okp_cfg.drop_after_ms > 120000) okp_cfg.drop_after_ms = 120000;
    okp_cfg.trace = GetPrivateProfileIntA("okplay", "trace", 0, ini);
    okp_transport_refresh();
    if (GetEnvironmentVariableA("OKP_TRACE", v, sizeof(v))) okp_cfg.trace = atoi(v);
}

void okp_exe_build(char build[17], char *exe, int exesz)
{
    static char cbuild[17], cexe[64];
    static LONG done;
    if (!InterlockedCompareExchange(&done, 1, 0)) {
        char path[MAX_PATH], *base;
        WCHAR w[MAX_PATH];
        HANDLE f;
        okp_sha256_ctx s;
        uint8_t h[32], chunk[16384];
        DWORD got;
        if (GetModuleFileNameA(NULL, path, sizeof(path))) {
            base = strrchr(path, '\\');
            MultiByteToWideChar(CP_ACP, 0, base ? base + 1 : path, -1, w, MAX_PATH);
            WideCharToMultiByte(CP_UTF8, 0, w, -1, cexe, sizeof(cexe), NULL, NULL);
            f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            if (f != INVALID_HANDLE_VALUE) {
                okp_sha256_init(&s);
                while (ReadFile(f, chunk, sizeof(chunk), &got, NULL) && got) okp_sha256_update(&s, chunk, got);
                CloseHandle(f);
                okp_sha256_final(&s, h);
                for (int i = 0; i < 8; i++) snprintf(cbuild + 2 * i, 3, "%02x", h[i]);
            }
        }
        /* tests: pretend to be another build */
        if (GetEnvironmentVariableA("OKP_TEST_EXEBUILD", path, sizeof(path)) && strlen(path) == 16)
            lstrcpynA(cbuild, path, sizeof(cbuild));
        okp_log(2, "game build: %s %s", cexe, cbuild[0] ? cbuild : "(could not read the exe)");
        InterlockedExchange(&done, 2);
    }
    while (done != 2) Sleep(1);
    lstrcpynA(build, cbuild, 17);
    if (exe) lstrcpynA(exe, cexe, exesz);
}

int okp_drop_after_ms(void) { return okp_cfg.drop_after_ms ? okp_cfg.drop_after_ms : OKP_DROP_AFTER_DEFAULT_MS; }

uint64_t okp_rand64(void)
{
    uint64_t v = 0;
    okp_random(&v, sizeof(v));   /* compat.c: the best generator this Windows has */
    if (v == 0) v = ((uint64_t)GetTickCount() << 32) ^ GetCurrentProcessId() ^ (uint64_t)(uintptr_t)&v;
    return v;
}

/* ---- addresses: everything is kept as SOCKADDR_IN6, IPv4 as v4-mapped. */

static const uint8_t v4mapped_prefix[12] = {0,0,0,0,0,0,0,0,0,0,0xff,0xff};

int okp_addr_is_v4(const SOCKADDR_IN6 *a)
{
    return memcmp(&a->sin6_addr, v4mapped_prefix, 12) == 0;
}

int okp_addr_is_public_v4(const SOCKADDR_IN6 *a)
{
    if (!okp_addr_is_v4(a)) return 0;
    const uint8_t *b = ((const uint8_t *)&a->sin6_addr) + 12;
    if (b[0] == 0 || b[0] == 10 || b[0] == 127 || b[0] >= 224) return 0;
    if (b[0] == 169 && b[1] == 254) return 0;
    if (b[0] == 172 && (b[1] & 0xF0) == 16) return 0;
    if (b[0] == 192 && b[1] == 168) return 0;
    if (b[0] == 100 && (b[1] & 0xC0) == 64) return 0;   /* carrier-grade NAT 100.64/10 */
    return 1;
}

void okp_addr_from_v4(SOCKADDR_IN6 *out, const void *v4, uint16_t port_n)
{
    memset(out, 0, sizeof(*out));
    out->sin6_family = AF_INET6;
    out->sin6_port = port_n;
    memcpy(&out->sin6_addr, v4mapped_prefix, 12);
    memcpy(((uint8_t *)&out->sin6_addr) + 12, v4, 4);
}

int okp_addr_eq(const SOCKADDR_IN6 *a, const SOCKADDR_IN6 *b)
{
    return a->sin6_port == b->sin6_port &&
           memcmp(&a->sin6_addr, &b->sin6_addr, 16) == 0;
}

const char *okp_addr_str(const SOCKADDR_IN6 *a, char *buf, int n)
{
    char ip[64];
    if (okp_addr_is_v4(a)) {
        okp_ntop(AF_INET, ((uint8_t *)&a->sin6_addr) + 12, ip, sizeof(ip));
        snprintf(buf, n, "%s:%u", ip, ntohs(a->sin6_port));
    } else {
        okp_ntop(AF_INET6, (void *)&a->sin6_addr, ip, sizeof(ip));
        snprintf(buf, n, "[%s]:%u", ip, ntohs(a->sin6_port));
    }
    return buf;
}

int okp_addr_parse(const char *s, SOCKADDR_IN6 *out)
{
    char host[64];
    const char *portp;
    memset(out, 0, sizeof(*out));
    out->sin6_family = AF_INET6;
    if (*s == '[') {
        const char *e = strchr(s, ']');
        if (!e || e[1] != ':' || e - s - 1 >= (int)sizeof(host)) return 0;
        memcpy(host, s + 1, e - s - 1);
        host[e - s - 1] = 0;
        if (okp_pton(AF_INET6, host, &out->sin6_addr) != 1) return 0;
        portp = e + 2;
    } else {
        const char *e = strrchr(s, ':');
        struct in_addr v4;
        if (!e || e - s >= (int)sizeof(host)) return 0;
        memcpy(host, s, e - s);
        host[e - s] = 0;
        if (okp_pton(AF_INET, host, &v4) != 1) return 0;
        okp_addr_from_v4(out, &v4, 0);
        portp = e + 1;
    }
    int port = atoi(portp);
    if (port <= 0 || port > 65535) return 0;
    out->sin6_port = htons((uint16_t)port);
    return 1;
}

/* ---- base64 */
static const char b64c[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int okp_b64_encode(const uint8_t *in, int n, char *out, int outsz)
{
    int o = 0;
    if (outsz <= 0) return -1;
    for (int i = 0; i < n; i += 3) {
        uint32_t v = in[i] << 16;
        if (i + 1 < n) v |= in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        if (o + 4 >= outsz) { out[o] = 0; return -1; }
        out[o++] = b64c[(v >> 18) & 63];
        out[o++] = b64c[(v >> 12) & 63];
        out[o++] = i + 1 < n ? b64c[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? b64c[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

static int b64v(char ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+' || ch == '-') return 62;
    if (ch == '/' || ch == '_') return 63;
    return -1;
}

int okp_b64_decode(const char *in, uint8_t *out, int outsz)
{
    int o = 0, bits = 0;
    uint32_t acc = 0;
    for (; *in && *in != '='; in++) {
        int v = b64v(*in);
        if (v < 0) continue;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= outsz) return -1;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return o;
}

int okp_urlenc(const char *in, char *out, int outsz)
{
    static const char hx[] = "0123456789ABCDEF";
    int o = 0;
    if (outsz <= 0) return -1;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            if (o + 1 >= outsz) { out[o] = 0; return -1; }
            out[o++] = *p;
        } else {
            if (o + 3 >= outsz) { out[o] = 0; return -1; }
            out[o++] = '%';
            out[o++] = hx[*p >> 4];
            out[o++] = hx[*p & 15];
        }
    }
    out[o] = 0;
    return o;
}

int okp_urldec(const char *in, char *out, int outsz)
{
    int o = 0;
    if (outsz <= 0) return -1;
    out[0] = 0;
    for (; *in; in++) {
        if (o + 1 >= outsz) { out[o] = 0; return -1; }
        if (*in == '%' && in[1] && in[2]) {
            char h[3] = {in[1], in[2], 0};
            out[o++] = (char)strtol(h, NULL, 16);
            in += 2;
        } else if (*in == '+') {
            out[o++] = ' ';
        } else {
            out[o++] = *in;
        }
    }
    out[o] = 0;
    return o;
}

/* Hex trace of a dplayx system message: envelope decoded, then the first 96 bytes
 * (OKP_TRACE=2: the whole message, up to 2 KB). */
void okp_trace(const char *dir, uint64_t peer, const void *msg, int len)
{
    if (!okp_cfg.trace) return;
    const uint8_t *b = (const uint8_t *)msg;
    int max = okp_cfg.trace >= 2 ? 2048 : 96;
    uint16_t cmd = 0;
    if (len >= 6) memcpy(&cmd, b + 4, 2);
    /* 128 bytes per log line (okp_log lines are capped); continuation lines say where they start */
    for (int off = 0; off < len && off < max; off += 128) {
        char hex[3 * 128 + 1];
        int k = 0;
        hex[0] = 0;
        for (int i = off; i < len && i < max && i < off + 128; i++)
            k += snprintf(hex + k, sizeof(hex) - k, "%02x ", b[i]);
        if (off == 0)
            okp_log(0, "TRACE %s peer %016llx cmd %u len %d: %s", dir, (unsigned long long)peer, cmd, len, hex);
        else
            okp_log(0, "TRACE   ...+%d: %s", off, hex);
    }
}

/* Re-entrant tokenizer (strtok is shared across threads). */
char *okp_tok(char **state, const char *delims)
{
    char *s = *state;
    if (!s) return NULL;
    s += strspn(s, delims);
    if (!*s) { *state = NULL; return NULL; }
    char *e = s + strcspn(s, delims);
    if (*e) { *e = 0; *state = e + 1; } else { *state = NULL; }
    return s;
}
