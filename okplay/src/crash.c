/* okplay - diagnostics: session log header, crash capture (log + minidump).
 *
 * The provider runs inside the game process, so when something goes wrong we
 * want the log to say where - even when the crash is in the game or another
 * DLL (cnc-ddraw, UserPatch) rather than in okplay itself.
 */
#include "okp.h"
#include <stdarg.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <stdio.h>
#include <string.h>

static HMODULE g_self;
static uintptr_t g_base, g_end;
static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_uef;
static volatile LONG g_in_report, g_dumped, g_firstchance;

/* ------------------------------------------------------------ helpers */

static const char *basename_of(const char *p)
{
    const char *s = strrchr(p, '\\');
    return s ? s + 1 : p;
}

/* "module+0xoffset" for any address, or "0xaddr" if it is in no module. */
static const char *where(uintptr_t a, char *buf, int n)
{
    HMODULE hm = okp_module_of((const void *)a);   /* compat.c: no GetModuleHandleEx on 9x */
    char path[MAX_PATH];
    if (hm && GetModuleFileNameA(hm, path, sizeof(path))) {
        snprintf(buf, n, "%s+0x%lx", basename_of(path), (unsigned long)(a - (uintptr_t)hm));
    } else {
        snprintf(buf, n, "0x%08lx", (unsigned long)a);
    }
    return buf;
}

static int is_code(uintptr_t a)
{
    MEMORY_BASIC_INFORMATION mbi;
    char name[8];
    if (!VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) return 1;
    /* Windows 9x reports code pages as plain read-only/read-write (no execute bit before NX):
     * there, anywhere inside a loaded module counts (a data address may slip in - a hint only) */
    return okp_win9x_paths() && mbi.AllocationBase &&
           GetModuleFileNameA((HMODULE)mbi.AllocationBase, name, sizeof(name)) > 0;
}

static int is_readable(uintptr_t a, size_t len)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return a + len <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

static void file_version(const char *path, char *out, int n)
{
    lstrcpynA(out, "?", n);
    DWORD h = 0, sz = GetFileVersionInfoSizeA(path, &h);
    if (!sz) return;
    void *buf = malloc(sz);
    VS_FIXEDFILEINFO *fi = NULL;
    UINT fl = 0;
    if (buf && GetFileVersionInfoA(path, 0, sz, buf) && VerQueryValueA(buf, "\\", (void **)&fi, &fl) && fi)
        snprintf(out, n, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                 HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
    free(buf);
}

static void log_module(const char *label, HMODULE hm)
{
    char path[MAX_PATH], ver[64];
    if (!hm || !GetModuleFileNameA(hm, path, sizeof(path))) {
        okp_log(1, "  %-10s (not loaded)", label);
        return;
    }
    file_version(path, ver, sizeof(ver));
    WIN32_FIND_DATAA fd;   /* (GetFileAttributesEx is Windows 98+) */
    HANDLE fh = FindFirstFileA(path, &fd);
    DWORD size = 0;
    if (fh != INVALID_HANDLE_VALUE) { size = fd.nFileSizeLow; FindClose(fh); }
    okp_log(1, "  %-10s %s  v%s  %lu bytes", label, path, ver, (unsigned long)size);
}

/* ------------------------------------------------------- session header */

void okp_log_header(void)
{
    static LONG done;
    if (InterlockedExchange(&done, 1)) return;
    extern int okp_log_reused;
    if (okp_log_reused) {
        okp_log(1, "---- okplay %s loaded again by the same game process ----", OKP_VERSION_STR);
        return;
    }

    okp_sys_init();   /* compat.c: the real version on NT (RtlGetVersion), GetVersionEx on 9x */

    SYSTEMTIME st;
    GetLocalTime(&st);
    okp_log(1, "==================== okplay session ====================");
    okp_log(1, "okplay %s %s (built %s %s)  log %s", OKP_VERSION_STR, OKP_EDITION, __DATE__, __TIME__, okp_log_path());
    okp_log(1, "date %04d-%02d-%02d  windows %lu.%lu.%lu  pid %lu", st.wYear, st.wMonth, st.wDay,
            (unsigned long)okp_sys.major, (unsigned long)okp_sys.minor, (unsigned long)okp_sys.build,
            (unsigned long)GetCurrentProcessId());
    {   /* (which functions are in use is logged at the first SPInit: see okp_log_sysinfo) */
        char sysname[80];
        okp_log(1, "system %s%s", okp_sys_name(sysname, sizeof(sysname)),
                okp_sys.emulate9x ? " (OKP_EMULATE_9X: taking the Windows 9x paths)" : "");
    }
    log_module("process", GetModuleHandleA(NULL));
#ifdef OKP_BUILTIN
    log_module("dplayx+okplay", g_self);
    log_module("okindeo", GetModuleHandleA("okindeo.dll"));
#else
    log_module("okplay", g_self);
    log_module("dplayx", GetModuleHandleA("dplayx.dll"));
#endif
    log_module("ddraw", GetModuleHandleA("ddraw.dll"));
    log_module("wsock32", GetModuleHandleA("wsock32.dll"));
    okp_log(1, "config server=%s port=%d stun=%s family=%d drop_pct=%d loglevel=%d stats_ms=%d crashdump=%d transport=%s drop_after=%ds",
            okp_cfg.server, okp_cfg.port, okp_cfg.stun, okp_cfg.family, okp_cfg.drop_pct, okp_loglevel,
            okp_cfg.stats_ms, okp_cfg.crashdump, okp_transport_name(okp_cfg.transport), okp_drop_after_ms() / 1000);
    okp_log(1, "=========================================================");
}

/* outside DllMain (the first SPInit): load the optional DLLs and say which paths this Windows takes */
void okp_log_sysinfo(void)
{
    static LONG done;
    if (InterlockedExchange(&done, 1)) return;
    okp_sys_load();
    okp_log(1, "system paths: %s, %s, %s, %s",
            okp_sys.has_winhttp ? "HTTPS (WinHTTP)" : "plain HTTP",
            okp_sys.getaddrinfo ? "getaddrinfo" : "gethostbyname",
            okp_sys.GetAdaptersAddresses ? "GetAdaptersAddresses" : okp_sys.GetAdaptersInfo ? "GetAdaptersInfo" : "no iphlpapi",
            okp_sys.BCryptGenRandom ? "BCryptGenRandom" : okp_sys.CryptGenRandom ? "CryptGenRandom" : "hashed-entropy RNG");
}

/* ------------------------------------------------------------ crash path */

static const char *exc_name(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
    case 0xC0000374:                      return "HEAP_CORRUPTION";
    case 0xC0000409:                      return "STACK_BUFFER_OVERRUN";
    default:                              return NULL;
    }
}

static void write_dump(EXCEPTION_POINTERS *ep)
{
    if (!okp_cfg.crashdump || InterlockedExchange(&g_dumped, 1)) return;
    typedef BOOL (WINAPI *MDWD_t)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                  PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    HMODULE dbg = LoadLibraryA("dbghelp.dll");
    MDWD_t mdwd = dbg ? (MDWD_t)(void *)GetProcAddress(dbg, "MiniDumpWriteDump") : NULL;
    if (!mdwd) { okp_log(0, "crash: dbghelp unavailable, no dump written"); return; }

    SYSTEMTIME st;
    GetLocalTime(&st);
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sokplay-crash-%04d%02d%02d-%02d%02d%02d-%lu.dmp", okp_log_dir(), st.wYear,
             st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, (unsigned long)GetCurrentProcessId());
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { okp_log(0, "crash: cannot create %s", path); return; }
    MINIDUMP_EXCEPTION_INFORMATION mei;
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    BOOL ok = mdwd(GetCurrentProcess(), GetCurrentProcessId(), f,
                   (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory |
                                   MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
                   &mei, NULL, NULL);
    CloseHandle(f);
    okp_log(0, "crash: minidump %s %s", ok ? "written to" : "FAILED for", path);
}

static void report(EXCEPTION_POINTERS *ep, const char *kind, int dump)
{
    if (InterlockedExchange(&g_in_report, 1)) return;   /* one full report per process */
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *cx = ep->ContextRecord;
    char w[160];
    const char *nm = exc_name(er->ExceptionCode);

    okp_report_crash_begin();   /* report.c: the log's last lines, before this block */
    okp_log(0, "======================== CRASH ========================");
    okp_log(0, "%s: %s (0x%08lx) at %s  thread %lu", kind, nm ? nm : "?", (unsigned long)er->ExceptionCode,
            where((uintptr_t)er->ExceptionAddress, w, sizeof(w)), (unsigned long)GetCurrentThreadId());
    /* the crash report (asked before it is sent): where, without registers or memory */
    okp_report_crash_add("sig=%s at %s", nm ? nm : "?", w);
    okp_report_crash_add("kind=%s", kind);
    okp_report_crash_add("code=0x%08lx", (unsigned long)er->ExceptionCode);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        okp_log(0, "  %s of address 0x%08lx",
                er->ExceptionInformation[0] == 0 ? "read" : er->ExceptionInformation[0] == 1 ? "write" : "execute",
                (unsigned long)er->ExceptionInformation[1]);
        okp_report_crash_add("access=%s of 0x%08lx",
                er->ExceptionInformation[0] == 0 ? "read" : er->ExceptionInformation[0] == 1 ? "write" : "execute",
                (unsigned long)er->ExceptionInformation[1]);
    }
#ifdef _M_IX86
    okp_log(0, "  eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx", cx->Eax, cx->Ebx, cx->Ecx, cx->Edx,
            cx->Esi, cx->Edi);
    okp_log(0, "  eip=%08lx esp=%08lx ebp=%08lx efl=%08lx", cx->Eip, cx->Esp, cx->Ebp, cx->EFlags);

    /* frame-pointer chain (okplay is built with frame pointers) */
    uintptr_t bp = cx->Ebp;
    for (int i = 0; i < 24 && bp && is_readable(bp, 8); i++) {
        uintptr_t ret = ((uintptr_t *)bp)[1];
        uintptr_t next = ((uintptr_t *)bp)[0];
        if (!is_code(ret)) break;
        okp_log(0, "  frame %2d: %s", i, where(ret, w, sizeof(w)));
        okp_report_crash_add("frame=%s", w);
        if (next <= bp) break;
        bp = next;
    }
    /* code addresses on the stack - who called whom, when there is no frame chain (Windows 9x's
     * kernel32 keeps none, and 9x has no dbghelp): the thread's owner is usually among them */
    {
        uintptr_t sp = cx->Esp;
        int shown = 0;
        for (int i = 0; i < 1024 && shown < 16 && is_readable(sp + i * 4, 4); i++) {
            uintptr_t v = ((uintptr_t *)sp)[i];
            if (!is_code(v)) continue;
            okp_log(0, "  stack +%03x: %s", i * 4, where(v, w, sizeof(w)));
            shown++;
        }
    }
    /* raw stack scan: anything that looks like a return address into code */
    uintptr_t sp = cx->Esp;
    int shown = 0;
    for (int i = 0; i < 512 && shown < 32; i++) {
        uintptr_t a = sp + i * 4;
        if (!is_readable(a, 4)) break;
        uintptr_t v = *(uintptr_t *)a;
        if (v > 0x10000 && is_code(v)) {
            okp_log(0, "  stack esp+%03x: %s", i * 4, where(v, w, sizeof(w)));
            if (shown < 16) okp_report_crash_add("stack=%s", w);
            shown++;
        }
    }
#endif
    /* loaded modules, so offsets can be mapped back to files */
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32 me;
        me.dwSize = sizeof(me);
        for (BOOL ok = Module32First(snap, &me); ok; ok = Module32Next(snap, &me)) {
            okp_log(0, "  module %08lx %8lx %s", (unsigned long)(uintptr_t)me.modBaseAddr,
                    (unsigned long)me.modBaseSize, me.szExePath);
            okp_report_crash_add("module=%s", me.szModule);   /* file name only, never the folder */
        }
        CloseHandle(snap);
    }
    if (dump) write_dump(ep);
    okp_log(0, "=======================================================");
    okp_report_crash_end();
    InterlockedExchange(&g_in_report, 0);
}

static LONG CALLBACK okp_veh(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (!exc_name(code)) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t addr = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    if (addr >= g_base && addr < g_end) {
        /* okplay never relies on SEH, so any fault inside us is a real bug */
        report(ep, "fault inside " OKP_MODULE_DESC, 1);
    } else if (InterlockedIncrement(&g_firstchance) <= 10) {
        /* elsewhere it may still be handled by the game; note it, don't dump */
        char w[160];
        okp_log(1, "first-chance %s at %s (may be handled by the game)", exc_name(code), where(addr, w, sizeof(w)));
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI okp_uef(EXCEPTION_POINTERS *ep)
{
    report(ep, "UNHANDLED - process is crashing", 1);
    return g_prev_uef ? g_prev_uef(ep) : EXCEPTION_CONTINUE_SEARCH;
}

void okp_crash_install(HMODULE self)
{
    static LONG done;
    if (InterlockedExchange(&done, 1)) return;
    g_self = self;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)self;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((uint8_t *)self + dos->e_lfanew);
    g_base = (uintptr_t)self;
    g_end = g_base + nt->OptionalHeader.SizeOfImage;
    okp_sys_init();
    if (okp_sys.AddVectoredExceptionHandler) okp_sys.AddVectoredExceptionHandler(1, okp_veh);   /* XP+ */
    g_prev_uef = SetUnhandledExceptionFilter(okp_uef);
    okp_log(2, "crash capture installed (okplay at %08lx-%08lx)", (unsigned long)g_base, (unsigned long)g_end);
}

/* ============================================================ crash report
 * What a crash leaves for report.c to offer the next time Multiplayer opens (asked first, see there):
 * where it happened, the modules' file names and okplay's last error/warning/info lines with every
 * address and folder taken out. Written to OKP_CRASH_REPORT_FILE next to the logs. */

#define TAIL_LINES 30
static char g_crash[12288];
static int g_crash_len;
static char g_tail[6144];

static int report_path(const char *name, char *out, int n)
{
    const char *d = okp_log_dir();
    if (!d || !d[0]) return 0;   /* logging off (loglevel=0): nowhere to keep anything */
    snprintf(out, n, "%s%s", d, name);
    return 1;
}

static void ini_get(const char *key, const char *def, char *out, int n)
{
    char ini[MAX_PATH];
    snprintf(ini, sizeof(ini), "%sokplay.ini", okp_dll_dir());
    GetPrivateProfileStringA("okplay", key, def, out, n, ini);
}

/* ------------------------------------------------------------------ scrub */

static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

/* one log line -> the same line without addresses and folders:
 *   C:\Users\Name\Games\AoE2\age2_x1.exe  ->  ...\age2_x1.exe   (a path ends at a double space, a quote,
 *                                            " (", " -", ", " or the end of the line)
 *   192.168.1.20 -> x.x.x.x  (four numbers up to 255; a version like v5.1.2600.0 stays)
 *   any word of hex digits with 2+ colons (IPv6, "[2001:db8::1]:2300") -> [v6]   (not a hh:mm:ss time) */
void okp_scrub(const char *in, char *out, int n)
{
    int o = 0;
    const char *p = in;
    while (*p && o < n - 1) {
        /* a path: X:\ or \\server */
        if ((((*p | 0x20) >= 'a' && (*p | 0x20) <= 'z') && p[1] == ':' && p[2] == '\\') || (p[0] == '\\' && p[1] == '\\')) {
            const char *e = p, *last = NULL;
            while (*e && *e != '"' && *e != '\'' && *e != '\r' && *e != '\n' &&
                   !(e[0] == ' ' && (e[1] == ' ' || e[1] == '(' || e[1] == '-')) && !(e[0] == ',' && e[1] == ' ')) {
                if (*e == '\\') last = e;
                e++;
            }
            const char *keep = last ? last + 1 : e;
            o += snprintf(out + o, n - o, "...\\");
            if (o >= n - 1) break;
            while (keep < e && o < n - 1) out[o++] = *keep++;
            p = e;
            continue;
        }
        /* IPv4 */
        if (is_digit(*p) && (p == in || (!is_digit(p[-1]) && p[-1] != '.' && p[-1] != 'v'))) {
            const char *q = p;
            int parts = 0, ok = 1;
            while (parts < 4) {
                int v = 0, d = 0;
                while (is_digit(*q) && d < 4) { v = v * 10 + (*q - '0'); q++; d++; }
                if (!d || v > 255) { ok = 0; break; }
                parts++;
                if (parts < 4) { if (*q != '.') { ok = 0; break; } q++; }
            }
            if (ok && parts == 4 && !is_digit(*q) && !(*q == '.' && is_digit(q[1]))) {
                o += snprintf(out + o, n - o, "x.x.x.x");
                p = q;
                continue;
            }
        }
        /* IPv6: a word of hex digits, colons, dots and brackets with at least two colons */
        if ((is_hex(*p) || *p == ':' || *p == '[') && (p == in || !(is_hex(p[-1]) || p[-1] == ':' || p[-1] == '.'))) {
            const char *q = p;
            int colons = 0, hex = 0, digits_only = 1;
            while (is_hex(*q) || *q == ':' || *q == '.' || *q == '[' || *q == ']') {
                if (*q == ':') colons++;
                if (is_hex(*q)) hex++;
                if (!(is_digit(*q) || *q == ':' || *q == '.')) digits_only = 0;
                q++;
            }
            /* "12:34:56.789" (a time) is digits only with colons at 2 and 5: leave it */
            int is_time = digits_only && q - p >= 8 && p[2] == ':' && p[5] == ':';
            if (colons >= 2 && hex && !is_time) {
                o += snprintf(out + o, n - o, "[v6]");
                p = q;
                continue;
            }
            while (p < q && o < n - 1) out[o++] = *p++;   /* not an address: copy the whole word */
            continue;
        }
        out[o++] = *p++;
    }
    out[o < n ? o : n - 1] = 0;
}

/* the log's last error/warning/info lines (not debug), scrubbed, into g_tail */
static void grab_tail(void)
{
    static char buf[16384];
    const char *lp = okp_log_path();
    g_tail[0] = 0;
    if (!lp || !lp[0]) return;
    HANDLE f = CreateFileA(lp, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(f, NULL), got = 0;
    if (size != INVALID_FILE_SIZE && size > sizeof(buf) - 1) SetFilePointer(f, (LONG)(size - (sizeof(buf) - 1)), NULL, FILE_BEGIN);
    ReadFile(f, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(f);
    buf[got] = 0;
    /* split into lines (the first may be cut off by the seek: skip it), keep the last TAIL_LINES of
     * level E/W/I - "hh:mm:ss.mmm [ pid] L text" */
    const char *keep[TAIL_LINES];
    int n = 0;
    char *s = buf;
    if (size != INVALID_FILE_SIZE && size > sizeof(buf) - 1) { s = strchr(buf, '\n'); s = s ? s + 1 : buf + got; }
    while (*s) {
        char *e = strchr(s, '\n');
        if (e) *e = 0;
        if (e && e > s && e[-1] == '\r') e[-1] = 0;
        char *br = strstr(s, "] ");
        if (br && (br[2] == 'E' || br[2] == 'W' || br[2] == 'I')) {
            if (n == TAIL_LINES) { memmove(keep, keep + 1, sizeof(keep[0]) * (TAIL_LINES - 1)); n--; }
            keep[n++] = s;
        }
        if (!e) break;
        s = e + 1;
    }
    int o = 0;
    for (int i = 0; i < n && o < (int)sizeof(g_tail) - 300; i++) {
        okp_scrub(keep[i], g_tail + o, (int)sizeof(g_tail) - o - 2);
        o += (int)strlen(g_tail + o);
        g_tail[o++] = '\n';
        g_tail[o] = 0;
    }
}

/* ------------------------------------------------------------------ crash (crash.c) */

/* one report per process: a fault inside okplay is reported by the vectored handler and then again by
 * the unhandled-exception filter - the first is the one that says where, the second would carry the
 * first's crash block as its "last log lines" */
static volatile LONG g_crash_taken;
static int g_crash_skip;

void okp_report_crash_begin(void)
{
    g_crash_skip = InterlockedExchange(&g_crash_taken, 1) != 0;
    if (g_crash_skip) return;
    g_crash_len = 0;
    g_crash[0] = 0;
    grab_tail();   /* before the crash block goes into the log */
}

void okp_report_crash_add(const char *fmt, ...)
{
    va_list ap;
    if (g_crash_skip || g_crash_len >= (int)sizeof(g_crash) - 2) return;
    va_start(ap, fmt);
    int m = vsnprintf(g_crash + g_crash_len, sizeof(g_crash) - g_crash_len - 1, fmt, ap);
    va_end(ap);
    if (m < 0) return;
    g_crash_len += m;
    if (g_crash_len > (int)sizeof(g_crash) - 2) g_crash_len = sizeof(g_crash) - 2;
    g_crash[g_crash_len++] = '\n';
    g_crash[g_crash_len] = 0;
}

void okp_exe_name(char *out, int n)
{
    char path[MAX_PATH], *base;
    out[0] = 0;
    if (!GetModuleFileNameA(NULL, path, sizeof(path))) return;
    base = strrchr(path, '\\');
    lstrcpynA(out, base ? base + 1 : path, n);
}

static void common_head(char *out, int n)
{
    char sysname[80], exe[64];
    okp_exe_name(exe, sizeof(exe));
    snprintf(out, n, "ver=%s %s\nwin=%lu.%lu.%lu %s\nexe=%s\n", OKP_VERSION_STR, OKP_EDITION,
             (unsigned long)okp_sys.major, (unsigned long)okp_sys.minor, (unsigned long)okp_sys.build,
             okp_sys_name(sysname, sizeof(sysname)), exe);
}

void okp_report_crash_end(void)
{
    char ask[16], path[MAX_PATH], head[300];
    if (g_crash_skip) return;
    ini_get("crash_reports", "ask", ask, sizeof(ask));
    if (!_stricmp(ask, "never") || !report_path(OKP_CRASH_REPORT_FILE, path, sizeof(path))) return;
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st;
    GetSystemTime(&st);
    char top[400];
    common_head(head, sizeof(head));
    int tn = snprintf(top, sizeof(top), "okplay crash report\ntime=%04d-%02d-%02d %02d:%02d UTC\n%s", st.wYear, st.wMonth,
                      st.wDay, st.wHour, st.wMinute, head);
    DWORD w;
    WriteFile(f, top, tn, &w, NULL);
    WriteFile(f, g_crash, g_crash_len, &w, NULL);
    WriteFile(f, "last log lines:\n", 16, &w, NULL);
    WriteFile(f, g_tail, (DWORD)strlen(g_tail), &w, NULL);
    CloseHandle(f);
    okp_log(0, "crash: report kept for sending (asked first): %s", path);
}
