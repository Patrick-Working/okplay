/* Wine's debug channel runtime (normally in ntdll) for Wine's dplayx built as a Windows DLL.
 *
 *   DPLAYX_DEBUG   like WINEDEBUG: "err+fixme" (default), "+dplay" (trace a channel), "+all", "-all"
 *
 * Plain build: lines go to DPLAYX_LOG, or dplayx-<pid>.log next to the game exe.
 *
 * Drop-in build (OKP_BUILTIN): lines go into okplay's session log (one file per game run), and
 *  - each fixme/warn is logged at most 3 times per function (the rest are counted);
 *  - every DirectPlay function the game calls is counted - "census" lines in the log list what the
 *    game used and which unfinished parts (fixme) it reached. Written when a DirectPlay object is
 *    released and when the game exits.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wine/debug.h"

static CRITICAL_SECTION log_cs;
static volatile LONG cfg_state;   /* 0 new, 1 initializing, 2 ready */
static unsigned char default_flags = (1 << __WINE_DBCL_ERR) | (1 << __WINE_DBCL_FIXME);
static char spec[256];            /* the raw DPLAYX_DEBUG value */

static void config(void)
{
    if (cfg_state == 2) return;
    if (InterlockedCompareExchange(&cfg_state, 1, 0) == 0) {
        InitializeCriticalSection(&log_cs);
        DWORD n = GetEnvironmentVariableA("DPLAYX_DEBUG", spec, sizeof(spec));
        if (!n || n >= sizeof(spec)) spec[0] = 0;
        if (strstr(spec, "+all")) default_flags = 0x0f;
        if (strstr(spec, "-all")) default_flags = 0;
        cfg_state = 2;
    }
    while (cfg_state != 2) Sleep(0);
}

/* "+name" / "-name" / "trace+name" etc. for one channel, else the default */
static unsigned char flags_for(const char *name)
{
    unsigned char f = default_flags;
    const char *p = spec;
    size_t len = strlen(name);
    while ((p = strstr(p, name))) {
        int whole = (p[len] == 0 || p[len] == ',') && p > spec && (p[-1] == '+' || p[-1] == '-');
        if (whole) {
            unsigned char bits = 0x0f;   /* bare +name: every class */
            const char *q = p - 1;
            if (q > spec && q[-1] != ',') {   /* "trace+name", "warn+name" ... */
                const char *s = q;
                while (s > spec && s[-1] != ',') s--;
                if (!strncmp(s, "err", 3)) bits = 1 << __WINE_DBCL_ERR;
                else if (!strncmp(s, "fixme", 5)) bits = 1 << __WINE_DBCL_FIXME;
                else if (!strncmp(s, "warn", 4)) bits = 1 << __WINE_DBCL_WARN;
                else if (!strncmp(s, "trace", 5)) bits = 1 << __WINE_DBCL_TRACE;
            }
            if (*q == '+') f |= bits; else f &= ~bits;
        }
        p += len;
    }
    return f;
}

/* Per-thread state: TlsAlloc, not __thread. The compiler's TLS (a .tls section) works only in a
 * DLL loaded with the program on Windows 95/98/ME (and XP); one loaded later with LoadLibrary gets
 * no slot and the first access reads through an invalid pointer. DirectX 3-era games load us just
 * like that (DPLAY.DLL -> LoadLibrary dplayx.dll at the first call): Necrodome crashed on Windows
 * 98 in __wine_dbg_strdup. Windows Vista and later handle both, so it never showed there. */
struct dbg_thread {
    char ring[8192];   /* strings returned by debugstr_*, like Wine's per-thread ring */
    unsigned pos;
    char line[1200];   /* the drop-in: the log line being put together */
    int line_len, line_cls;
};
static DWORD dbg_tls = TLS_OUT_OF_INDEXES;
static volatile LONG dbg_tls_state;   /* 0 new, 1 being made, 2 ready */
static struct dbg_thread dbg_fallback = { .line_cls = -1 };   /* no TLS slot or no memory: shared */

static struct dbg_thread *dbg_thread(void)
{
    struct dbg_thread *t;
    DWORD err = GetLastError();   /* TlsGetValue clears it; TRACE must not change what callers see */
    if (dbg_tls_state != 2) {
        if (InterlockedCompareExchange(&dbg_tls_state, 1, 0) == 0) { dbg_tls = TlsAlloc(); dbg_tls_state = 2; }
        while (dbg_tls_state != 2) Sleep(0);
    }
    if (dbg_tls == TLS_OUT_OF_INDEXES) { SetLastError(err); return &dbg_fallback; }
    t = TlsGetValue(dbg_tls);
    if (!t) {
        t = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*t));
        if (!t) { SetLastError(err); return &dbg_fallback; }
        t->line_cls = -1;
        TlsSetValue(dbg_tls, t);
    }
    SetLastError(err);
    return t;
}

const char * __cdecl __wine_dbg_strdup(const char *str)
{
    struct dbg_thread *t = dbg_thread();
    size_t n = strlen(str) + 1;
    if (n > sizeof(t->ring) / 4) n = sizeof(t->ring) / 4;
    if (t->pos + n > sizeof(t->ring)) t->pos = 0;
    char *d = t->ring + t->pos;
    memcpy(d, str, n - 1);
    d[n - 1] = 0;
    t->pos += n;
    return d;
}

static const char *const class_names[] = { "fixme", "err", "warn", "trace" };

#ifdef OKP_BUILTIN
/* ------------------------------------------------------------------ drop-in: okplay's log */
extern void okp_log(int level, const char *fmt, ...);

static unsigned char user_flags(const char *name) { return flags_for(name); }

unsigned char __cdecl __wine_dbg_get_channel_flags(struct __wine_debug_channel *channel)
{
    config();
    /* trace is always on for the census; header() drops what wasn't asked for */
    return user_flags(channel->name) | (1 << __WINE_DBCL_TRACE);
}

/* census: calls per function (keyed by the function-name pointer: one per call site's function) */
#define CENSUS_SLOTS 1024
static struct { const char *fn; volatile LONG traces, fixmes, warns, errs; } census[CENSUS_SLOTS];
static CRITICAL_SECTION census_cs;
static volatile LONG census_init;

static int census_slot(const char *fn)
{
    unsigned h = (unsigned)(((uintptr_t)fn >> 3) * 2654435761u) % CENSUS_SLOTS;
    for (int i = 0; i < CENSUS_SLOTS; i++, h = (h + 1) % CENSUS_SLOTS) {
        const char *cur = census[h].fn;
        if (cur == fn) return (int)h;
        if (!cur) {
            if (InterlockedCompareExchange(&census_init, 1, 0) == 0) InitializeCriticalSection(&census_cs), census_init = 2;
            while (census_init != 2) Sleep(0);
            EnterCriticalSection(&census_cs);
            if (!census[h].fn) census[h].fn = fn;
            LeaveCriticalSection(&census_cs);
            if (census[h].fn == fn) return (int)h;
        }
    }
    return -1;   /* table full: not counted */
}

int __cdecl __wine_dbg_header(enum __wine_debug_class cls, struct __wine_debug_channel *channel, const char *function)
{
    const char *fn = function ? function : "?";
    /* Wine's TRACE/FIXME macros only test channel->flags (all on) - they never call
     * __wine_dbg_get_channel_flags, so DPLAYX_DEBUG must be read here */
    config();
    int s = census_slot(fn);
    LONG seen = 0;
    if (s >= 0) {
        volatile LONG *counter = cls == __WINE_DBCL_TRACE ? &census[s].traces : cls == __WINE_DBCL_FIXME ? &census[s].fixmes
                               : cls == __WINE_DBCL_WARN ? &census[s].warns : &census[s].errs;
        seen = InterlockedIncrement(counter);
    }
    if (!(user_flags(channel->name) & (1 << cls))) return -1;           /* counted only */
    if ((cls == __WINE_DBCL_FIXME || cls == __WINE_DBCL_WARN) && seen > 3) return -1;
    if (cls == __WINE_DBCL_ERR && seen > 20) return -1;
    struct dbg_thread *t = dbg_thread();
    t->line_cls = cls;
    t->line_len = snprintf(t->line, sizeof(t->line), "wine %s:%s: ", class_names[cls < 4 ? cls : 0], fn);
    if (t->line_len < 0 || t->line_len >= (int)sizeof(t->line)) t->line_len = 0;
    return t->line_len;
}

int __cdecl __wine_dbg_output(const char *str)
{
    struct dbg_thread *t = dbg_thread();
    int n = (int)strlen(str);
    if (t->line_cls < 0) return n;   /* output without a header (wine_dbg_printf): ignore */
    for (const char *p = str; *p; p++) {
        if (*p == '\n' || t->line_len >= (int)sizeof(t->line) - 1) {
            t->line[t->line_len] = 0;
            /* err -> E, fixme -> W, warn -> I, trace -> D (okplay's levels) */
            int level = t->line_cls == __WINE_DBCL_ERR ? 0 : t->line_cls == __WINE_DBCL_FIXME ? 1
                      : t->line_cls == __WINE_DBCL_WARN ? 2 : 3;
            okp_log(level, "%s", t->line);
            t->line_len = 0;
            if (*p == '\n') { t->line_cls = -1; break; }
        } else {
            t->line[t->line_len++] = *p;
        }
    }
    return n;
}

static int by_count(const void *a, const void *b)
{
    const int *x = a, *y = b;
    LONG ca = census[*x].traces + census[*x].fixmes, cb = census[*y].traces + census[*y].fixmes;
    return ca < cb ? 1 : ca > cb ? -1 : 0;
}

/* the census, into the session log */
void __wine_dbg_census(const char *why)
{
    static int idx[CENSUS_SLOTS];
    int n = 0;
    char buf[1100];
    int len = 0;
    for (int i = 0; i < CENSUS_SLOTS; i++) if (census[i].fn) idx[n++] = i;
    qsort(idx, n, sizeof(int), by_count);
    okp_log(1, "census (%s): DirectPlay functions used, most first", why);
    /* the unfinished parts the game reached: the list to fix */
    len = snprintf(buf, sizeof(buf), "census: unfinished (fixme) reached:");
    int any = 0;
    for (int k = 0; k < n; k++) {
        int i = idx[k];
        if (!census[i].fixmes) continue;
        any = 1;
        int w = snprintf(buf + len, sizeof(buf) - len, " %s x%ld", census[i].fn, census[i].fixmes);
        if (w < 0 || len + w >= (int)sizeof(buf) - 1) { okp_log(1, "%s", buf); len = snprintf(buf, sizeof(buf), "census:  ..."); w = snprintf(buf + len, sizeof(buf) - len, " %s x%ld", census[i].fn, census[i].fixmes); }
        len += w;
    }
    okp_log(1, "%s%s", buf, any ? "" : " none");
    len = snprintf(buf, sizeof(buf), "census: calls:");
    for (int k = 0; k < n; k++) {
        int i = idx[k];
        if (!census[i].traces && !census[i].errs) continue;
        int w = snprintf(buf + len, sizeof(buf) - len, " %s x%ld%s", census[i].fn, census[i].traces,
                         census[i].errs ? "(err)" : "");
        if (w < 0 || len + w >= (int)sizeof(buf) - 1) {
            okp_log(1, "%s", buf);
            len = snprintf(buf, sizeof(buf), "census: calls:");
            w = snprintf(buf + len, sizeof(buf) - len, " %s x%ld", census[i].fn, census[i].traces);
        }
        len += w;
    }
    okp_log(1, "%s", buf);
}

#else
/* ------------------------------------------------------------------ plain build: own file */
static FILE *log_file;
static int opened;

unsigned char __cdecl __wine_dbg_get_channel_flags(struct __wine_debug_channel *channel)
{
    config();
    return flags_for(channel->name);
}

static void open_log(void)
{
    char path[MAX_PATH + 64];
    opened = 1;
#ifdef OKWEB_DPLAY
    log_file = stderr;   /* the browser build (okweb/dplay): the page's console */
    return;
#endif
    DWORD n = GetEnvironmentVariableA("DPLAYX_LOG", path, MAX_PATH);
    if (!n || n >= MAX_PATH) {
        n = GetModuleFileNameA(NULL, path, MAX_PATH);
        char *slash = strrchr(path, '\\');
        if (!n || !slash) return;
        sprintf(slash + 1, "dplayx-%lu.log", GetCurrentProcessId());
    }
    log_file = fopen(path, "a");
}

int __cdecl __wine_dbg_output(const char *str)
{
    config();
    EnterCriticalSection(&log_cs);
    if (!opened) open_log();
    int n = (int)strlen(str);
    if (log_file) { fputs(str, log_file); if (n && str[n - 1] == '\n') fflush(log_file); }
    LeaveCriticalSection(&log_cs);
    return n;
}

int __cdecl __wine_dbg_header(enum __wine_debug_class cls, struct __wine_debug_channel *channel, const char *function)
{
    char buf[160];
    config();
    if (!(flags_for(channel->name) & (1 << cls))) return -1;   /* the macros don't filter (see above) */
    SYSTEMTIME t;
    GetLocalTime(&t);
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u %04lx:%s:%s:%s ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
             GetCurrentThreadId(), cls < 4 ? class_names[cls] : "?", channel->name, function ? function : "");
    return __wine_dbg_output(buf);
}

void __wine_dbg_census(const char *why) { (void)why; }
#endif

int WINAPI __wine_dbg_write(const char *str, unsigned int len)
{
    char tmp[1024];
    if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
    memcpy(tmp, str, len);
    tmp[len] = 0;
    return __wine_dbg_output(tmp);
}
