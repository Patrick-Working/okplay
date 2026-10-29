/* winweb.c - the Windows functions our DirectPlay uses, for a single-threaded browser page.
 *
 * The model: the page has one thread. DirectPlay's background loops (keepalive, async session
 * search) run as timer tasks from winweb_pump() (dplay.c / lifecycle.c hook them in under
 * OKWEB_DPLAY). A wait (WaitForSingleObject, Sleep) runs the pump - which also hands the provider's
 * incoming messages to DirectPlay - and, when the program is built with Asyncify, yields to the
 * browser between tries, so the network can deliver the reply being waited for. Without Asyncify a
 * wait that isn't satisfied at once times out. */
#include "winweb.h"
#include <emscripten.h>
#include <emscripten/eventloop.h>
#include <time.h>
#include <sys/time.h>

__attribute__((weak)) const GUID GUID_NULL;   /* (weak: a game may define these too) */
__attribute__((weak)) const GUID IID_IUnknown = { 0x00000000, 0x0000, 0x0000, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
__attribute__((weak)) const GUID IID_IClassFactory = { 0x00000001, 0x0000, 0x0000, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };

static DWORD g_last_error;
DWORD WINAPI GetLastError(void) { return g_last_error; }
void WINAPI SetLastError(DWORD e) { g_last_error = e; }

/* ------------------------------------------------------------------ time */

DWORD WINAPI GetTickCount(void) { return (DWORD)emscripten_get_now(); }
DWORD WINAPI timeGetTime(void) { return (DWORD)emscripten_get_now(); }

void WINAPI GetLocalTime(LPSYSTEMTIME st)
{
    struct timeval tv;
    struct tm tm;
    gettimeofday(&tv, NULL);
    time_t t = tv.tv_sec;
    localtime_r(&t, &tm);
    st->wYear = tm.tm_year + 1900; st->wMonth = tm.tm_mon + 1; st->wDayOfWeek = tm.tm_wday;
    st->wDay = tm.tm_mday; st->wHour = tm.tm_hour; st->wMinute = tm.tm_min; st->wSecond = tm.tm_sec;
    st->wMilliseconds = tv.tv_usec / 1000;
}

/* ------------------------------------------------------------------ handles */

enum { O_FREE, O_EVENT, O_SEMAPHORE, O_MAPPING, O_THREAD, O_MODULE };
typedef struct { int type, refs; BOOL manual, signaled; LONG count, max; void *mem; SIZE_T size; } wobj;
#define MAXOBJ 256
#define HBASE 0x1000
static wobj objs[MAXOBJ];

static HANDLE alloc_obj(int type)
{
    for (int i = 0; i < MAXOBJ; i++)
        if (objs[i].type == O_FREE) {
            memset(&objs[i], 0, sizeof(objs[i]));
            objs[i].type = type;
            objs[i].refs = 1;
            return (HANDLE)(intptr_t)(HBASE + i);
        }
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return NULL;
}

static wobj *get_obj(HANDLE h)
{
    intptr_t i = (intptr_t)h - HBASE;
    return i >= 0 && i < MAXOBJ && objs[i].type != O_FREE ? &objs[i] : NULL;
}

BOOL WINAPI CloseHandle(HANDLE h)
{
    wobj *o = get_obj(h);
    if (!o) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (--o->refs <= 0) {
        if (o->type == O_MAPPING) free(o->mem);
        o->type = O_FREE;
    }
    return TRUE;
}

/* the one handle DirectPlay duplicates is the "stop" event of its async session search */
BOOL WINAPI DuplicateHandle(HANDLE src_proc, HANDLE src, HANDLE dst_proc, LPHANDLE dst, DWORD access,
                            BOOL inherit, DWORD opts)
{
    wobj *o = get_obj(src);
    if (!o || !dst) return FALSE;
    o->refs++;
    *dst = src;
    return TRUE;
}

HANDLE WINAPI CreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCWSTR name)
{
    HANDLE h = alloc_obj(O_EVENT);
    wobj *o = get_obj(h);
    if (o) { o->manual = manual; o->signaled = initial; SetLastError(0); }
    return h;
}

HANDLE WINAPI CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCSTR name)
{
    return CreateEventW(sa, manual, initial, NULL);
}

BOOL WINAPI SetEvent(HANDLE h) { wobj *o = get_obj(h); if (!o) return FALSE; o->signaled = TRUE; return TRUE; }
BOOL WINAPI ResetEvent(HANDLE h) { wobj *o = get_obj(h); if (!o) return FALSE; o->signaled = FALSE; return TRUE; }

/* (named semaphores are only the lobby's cross-process lock: one page, no other process) */
HANDLE WINAPI CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG max, LPCSTR name)
{
    HANDLE h = alloc_obj(O_SEMAPHORE);
    wobj *o = get_obj(h);
    if (o) { o->count = initial; o->max = max; SetLastError(0); }   /* never "already exists": one page */
    return h;
}

BOOL WINAPI ReleaseSemaphore(HANDLE h, LONG n, LPLONG prev)
{
    wobj *o = get_obj(h);
    if (!o || o->type != O_SEMAPHORE) return FALSE;
    if (prev) *prev = o->count;
    o->count += n;
    if (o->count > o->max) o->count = o->max;
    return TRUE;
}

/* ------------------------------------------------------------------ the pump */

typedef struct { void (*fn)(void *); void *arg; DWORD every, next; int used; } wtask;
#define MAX_TASKS 64   /* a game makes a DirectPlay object per session search: each holds one for a while */
static wtask tasks[MAX_TASKS];
static void (*g_pump_hook)(void);
static int g_in_pump;
static int g_yielding;   /* a wait has given the browser a turn: it pumps for itself */

/* a background loop of DirectPlay's, every `ms` (0 = remove the task with this fn+arg) */
void winweb_every(DWORD ms, void (*fn)(void *), void *arg)
{
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].used && tasks[i].fn == fn && tasks[i].arg == arg) {
            if (!ms) tasks[i].used = 0; else tasks[i].every = ms;
            return;
        }
    if (!ms) return;
    for (int i = 0; i < MAX_TASKS; i++)
        if (!tasks[i].used) {
            tasks[i] = (wtask){ fn, arg, ms, GetTickCount() + ms, 1 };
            return;
        }
    fprintf(stderr, "winweb: no room for another background task (%d running) - one of DirectPlay's loops won't run\n", MAX_TASKS);
}

/* diagnostics: Module.winwebTasks() - DirectPlay's background loops now running */
EMSCRIPTEN_KEEPALIVE uint32_t *winweb_task_info(int i)
{
    static uint32_t out[4];
    if (i < 0 || i >= MAX_TASKS || !tasks[i].used) return NULL;
    out[0] = (uint32_t)(uintptr_t)tasks[i].fn;
    out[1] = (uint32_t)(uintptr_t)tasks[i].arg;
    out[2] = tasks[i].every;
    out[3] = tasks[i].next - GetTickCount();
    return out;
}
EM_JS(void, winweb_js_install_debug, (int max), {
    Module.winwebTasks = () => {
        const out = [];
        for (let i = 0; i < max; i++) {
            const p = _winweb_task_info(i) >> 2;
            if (p) out.push({ fn: HEAPU32[p], arg: HEAPU32[p + 1], every: HEAPU32[p + 2], dueIn: HEAP32[p + 3] });
        }
        return out;
    };
});

/* the provider's delivery of incoming messages, run first in every pump */
/* the pump also runs on its own every 30 ms, so DirectPlay's background work goes on while the
 * program isn't calling it (a game in its menus) - except while a wait is pumping for itself */
static void pump_timer(void *unused)
{
    (void)unused;
    if (!g_yielding) winweb_pump();
}

/* okweb.js's clock (Module.okwebClock, set by okweb-sp.js), whose timers go on in a hidden tab;
 * without it, Emscripten's own (which a hidden tab slows to once a second or less) */
EM_JS(int, winweb_js_has_clock, (void), { return Module.okwebClock ? 1 : 0; });
EM_JS(void, winweb_js_clock_every, (int ms), { Module.okwebClock.setInterval(() => _winweb_clock_tick(), ms); });
EM_ASYNC_JS(void, winweb_js_clock_sleep, (int ms), { await Module.okwebClock.sleep(ms); });
EMSCRIPTEN_KEEPALIVE void winweb_clock_tick(void) { pump_timer(NULL); }

void winweb_set_pump_hook(void (*fn)(void))
{
    static int timer;
    g_pump_hook = fn;
    if (timer) return;
    timer = 1;
    winweb_js_install_debug(MAX_TASKS);
    if (winweb_js_has_clock()) winweb_js_clock_every(30);
    else emscripten_set_interval(pump_timer, 30, NULL);
}

void winweb_pump(void)
{
    if (g_in_pump) return;   /* a task or a delivered message that waits: no nested pumps */
    g_in_pump = 1;
    if (g_pump_hook) g_pump_hook();
    DWORD now = GetTickCount();
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].used && (LONG)(now - tasks[i].next) >= 0) {
            tasks[i].next = now + tasks[i].every;
            tasks[i].fn(tasks[i].arg);
        }
    g_in_pump = 0;
}

/* can this call stack give the browser a turn? Only with Asyncify, and never from inside the pump */
EM_JS(int, winweb_js_can_yield, (void), { return typeof Asyncify !== 'undefined' ? 1 : 0; });

/* a wait for the page (okweb_password.c's box): the same rule as a yielding wait, and meanwhile the
   clock pumps (other players' messages keep coming) */
int winweb_wait_begin(void)
{
    if (g_in_pump || g_yielding || !winweb_js_can_yield()) return 0;
    g_yielding = 1;
    return 1;
}

void winweb_wait_end(void)
{
    g_yielding = 0;
}

static int yield(DWORD ms)
{
    if (g_in_pump || g_yielding || !winweb_js_can_yield()) return 0;
    g_yielding = 1;
    if (winweb_js_has_clock()) winweb_js_clock_sleep((int)ms); else emscripten_sleep(ms);
    g_yielding = 0;
    return 1;
}

static BOOL take(wobj *o)
{
    switch (o->type) {
    case O_EVENT: if (!o->signaled) return FALSE; if (!o->manual) o->signaled = FALSE; return TRUE;
    case O_SEMAPHORE: if (o->count <= 0) return FALSE; o->count--; return TRUE;
    case O_THREAD: return TRUE;   /* our "threads" are tasks: never running when waited for */
    default: return TRUE;
    }
}

DWORD WINAPI WaitForSingleObject(HANDLE h, DWORD ms)
{
    DWORD start = GetTickCount();
    for (;;) {
        wobj *o = get_obj(h);
        if (!o) { SetLastError(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
        if (take(o)) return WAIT_OBJECT_0;
        winweb_pump();
        if ((o = get_obj(h)) && take(o)) return WAIT_OBJECT_0;
        DWORD spent = GetTickCount() - start;
        if (ms != INFINITE && spent >= ms) return WAIT_TIMEOUT;
        DWORD left = ms == INFINITE ? 10 : ms - spent;
        if (!yield(left < 10 ? left : 10))
            return ms == INFINITE ? WAIT_FAILED : WAIT_TIMEOUT;   /* nothing can change while we hold the page */
    }
}

void WINAPI Sleep(DWORD ms)
{
    DWORD start = GetTickCount();
    do { winweb_pump(); } while (GetTickCount() - start < ms && yield(10));
}

DWORD WINAPI SleepEx(DWORD ms, BOOL alertable) { Sleep(ms); return 0; }

/* ------------------------------------------------------------------ threads */

/* DirectPlay's own loops don't come here in the browser build (they're tasks: OKWEB_DPLAY hooks).
 * What's left is the lobby's message thread, which only serves programs launched by a lobby. */
HANDLE WINAPI CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg,
                           DWORD flags, LPDWORD id)
{
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return NULL;
}

DWORD WINAPI ResumeThread(HANDLE h) { return 0; }
BOOL WINAPI TerminateThread(HANDLE h, DWORD code) { return TRUE; }
DWORD WINAPI GetCurrentThreadId(void) { return 1; }

/* thread-local slots: a page has one thread, so a slot is just a value */
#define WINWEB_TLS_SLOTS 16
static LPVOID g_tls[WINWEB_TLS_SLOTS];
static DWORD g_tls_used;
DWORD WINAPI TlsAlloc(void) { return g_tls_used < WINWEB_TLS_SLOTS ? g_tls_used++ : TLS_OUT_OF_INDEXES; }
LPVOID WINAPI TlsGetValue(DWORD index)
{
    if (index >= g_tls_used) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    SetLastError(ERROR_SUCCESS);
    return g_tls[index];
}
BOOL WINAPI TlsSetValue(DWORD index, LPVOID value)
{
    if (index >= g_tls_used) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    g_tls[index] = value;
    return TRUE;
}
DWORD WINAPI GetCurrentProcessId(void) { return 1; }
HANDLE WINAPI GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }
BOOL WINAPI DisableThreadLibraryCalls(HMODULE m) { return TRUE; }

void WINAPI InitializeCriticalSection(LPCRITICAL_SECTION cs)
{
    memset(cs, 0, sizeof(*cs));
    cs->DebugInfo = &cs->debug_storage;   /* Wine code names its locks through DebugInfo->Spare */
    cs->LockCount = -1;
}
void WINAPI DeleteCriticalSection(LPCRITICAL_SECTION cs) { cs->DebugInfo = NULL; }
void WINAPI EnterCriticalSection(LPCRITICAL_SECTION cs) { cs->RecursionCount++; }
void WINAPI LeaveCriticalSection(LPCRITICAL_SECTION cs) { cs->RecursionCount--; }

BOOL WINAPI InitOnceExecuteOnce(PINIT_ONCE once, PINIT_ONCE_FN fn, PVOID param, PVOID *ctx)
{
    if (once->Ptr) return TRUE;
    once->Ptr = (void *)1;
    return fn(once, param, ctx);
}

/* ------------------------------------------------------------------ memory */

HANDLE WINAPI CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect, DWORD size_hi, DWORD size_lo,
                                 LPCSTR name)
{
    HANDLE h = alloc_obj(O_MAPPING);
    wobj *o = get_obj(h);
    if (o && !(o->mem = calloc(1, size_lo))) { CloseHandle(h); return NULL; }
    if (o) o->size = size_lo;
    SetLastError(0);   /* never "already exists": one page, so this process creates it */
    return h;
}

LPVOID WINAPI MapViewOfFileEx(HANDLE map, DWORD access, DWORD off_hi, DWORD off_lo, SIZE_T size, LPVOID base)
{
    wobj *o = get_obj(map);
    return o && o->type == O_MAPPING ? (char *)o->mem + off_lo : NULL;
}
LPVOID WINAPI MapViewOfFile(HANDLE map, DWORD access, DWORD off_hi, DWORD off_lo, SIZE_T size)
{
    return MapViewOfFileEx(map, access, off_hi, off_lo, size, NULL);
}
BOOL WINAPI UnmapViewOfFile(LPCVOID base) { return TRUE; }
BOOL WINAPI FlushViewOfFile(LPCVOID base, SIZE_T size) { return TRUE; }
BOOL WINAPI VirtualProtect(LPVOID addr, SIZE_T size, DWORD prot, PDWORD old) { if (old) *old = PAGE_READWRITE; return TRUE; }
SIZE_T WINAPI VirtualQuery(LPCVOID addr, PMEMORY_BASIC_INFORMATION mbi, SIZE_T len)
{
    if (!mbi || len < sizeof(*mbi)) return 0;
    memset(mbi, 0, sizeof(*mbi));
    mbi->BaseAddress = (PVOID)addr;
    mbi->RegionSize = 4096;
    mbi->State = MEM_COMMIT;
    mbi->Protect = PAGE_READWRITE;
    return sizeof(*mbi);
}
BOOL WINAPI IsBadStringPtrA(LPCSTR s, UINT_PTR max) { return s == NULL; }
BOOL WINAPI IsBadStringPtrW(LPCWSTR s, UINT_PTR max) { return s == NULL; }

/* ------------------------------------------------------------------ modules: the okweb provider */

#define OKWEB_MODULE ((HMODULE)(intptr_t)0x0b0b0001)
extern HRESULT WINAPI okweb_SPInit(void *data);   /* okweb_sp.c */

HMODULE WINAPI GetModuleHandleA(LPCSTR name) { return name ? NULL : (HMODULE)(intptr_t)0x0b0b0000; }
DWORD WINAPI GetModuleFileNameA(HMODULE m, LPSTR out, DWORD size) { lstrcpynA(out, "dplayx.wasm", size); return lstrlenA(out); }
HMODULE WINAPI LoadLibraryA(LPCSTR name)
{
    if (name && !strcasecmp(name, "okweb.dll")) return OKWEB_MODULE;
    SetLastError(ERROR_FILE_NOT_FOUND);
    return NULL;
}
BOOL WINAPI FreeLibrary(HMODULE m) { return TRUE; }
FARPROC WINAPI GetProcAddress(HMODULE m, LPCSTR name)
{
    if (m == OKWEB_MODULE && !strcmp(name, "SPInit")) return (FARPROC)okweb_SPInit;
    return NULL;
}

BOOL WINAPI CreateProcessA(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit,
                           DWORD flags, LPVOID env, LPCSTR dir, LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi)
{
    SetLastError(ERROR_FILE_NOT_FOUND);   /* lobby launches: no programs to start in a page */
    return FALSE;
}

DWORD WINAPI GetEnvironmentVariableA(LPCSTR name, LPSTR out, DWORD size)
{
    const char *v = getenv(name);
    if (!v) { SetLastError(ERROR_FILE_NOT_FOUND); return 0; }
    DWORD n = (DWORD)strlen(v);
    if (n + 1 > size) return n + 1;
    memcpy(out, v, n + 1);
    return n;
}
DWORD WINAPI GetVersion(void) { return 0x0a000000 | 0x0a; /* "Windows 10": dplayx only branches on 9x */ }
void WINAPI DebugBreak(void) { emscripten_debugger(); }

/* ------------------------------------------------------------------ registry: one provider */

/* HKLM\SOFTWARE\Microsoft\DirectPlay\Service Providers\Oldkid.net Online Play (as the Windows
 * installer of okplay registers it, with okweb.dll as the path: LoadLibraryA above) */
#define K_SP_ROOT ((HKEY)(intptr_t)0x0c0c0001)
#define K_SP_OKWEB ((HKEY)(intptr_t)0x0c0c0002)
static const char SP_ROOT_PATH[] = "SOFTWARE\\Microsoft\\DirectPlay\\Service Providers";
static const char SP_NAME[] = "Oldkid.net Online Play";
static const struct { const char *name, *value; } sp_values[] = {
    { "Guid", "{66D8DC59-28E8-4D3E-BA93-2E98D251C0F3}" },   /* okplay's OKP_SPGUID */
    { "DescriptionA", "Oldkid.net Online Play" },
    { "DescriptionW", "Oldkid.net Online Play" },
    { "Path", "okweb.dll" },
    { "dwReserved1", "500" },
    { "dwReserved2", "0" },
};

LONG WINAPI RegOpenKeyExA(HKEY key, LPCSTR sub, DWORD opts, REGSAM sam, PHKEY out)
{
    if (key == HKEY_LOCAL_MACHINE && sub && !strcasecmp(sub, SP_ROOT_PATH)) { *out = K_SP_ROOT; return ERROR_SUCCESS; }
    if (key == K_SP_ROOT && sub && !strcasecmp(sub, SP_NAME)) { *out = K_SP_OKWEB; return ERROR_SUCCESS; }
    return ERROR_FILE_NOT_FOUND;   /* no lobby providers, no lobbyable applications */
}

LONG WINAPI RegCloseKey(HKEY key) { return ERROR_SUCCESS; }

LONG WINAPI RegEnumKeyExA(HKEY key, DWORD index, LPSTR name, LPDWORD name_len, LPDWORD reserved,
                          LPSTR cls, LPDWORD cls_len, LPFILETIME ft)
{
    if (key != K_SP_ROOT || index > 0) return ERROR_NO_MORE_ITEMS;
    if (*name_len < sizeof(SP_NAME)) return ERROR_MORE_DATA;
    strcpy(name, SP_NAME);
    *name_len = sizeof(SP_NAME) - 1;
    if (ft) memset(ft, 0, sizeof(*ft));
    return ERROR_SUCCESS;
}

static const char *sp_value(HKEY key, const char *name)
{
    if (key != K_SP_OKWEB || !name) return NULL;
    for (size_t i = 0; i < ARRAY_SIZE(sp_values); i++)
        if (!strcasecmp(sp_values[i].name, name)) return sp_values[i].value;
    return NULL;
}

static LONG put_value(const char *v, DWORD flags, LPDWORD type, void *data, LPDWORD len, BOOL wide)
{
    if (!v) return ERROR_FILE_NOT_FOUND;
    DWORD need = (DWORD)(strlen(v) + 1) * (wide ? sizeof(WCHAR) : 1);
    if ((flags & RRF_RT_DWORD) && !(flags & RRF_RT_REG_SZ)) {
        /* asked for a number, but our values are text: as Windows, say so with the size the text needs
         * (dplayx then reads it as text: DP_GetRegDword) */
        *len = need;
        return 1630L;   /* ERROR_UNSUPPORTED_TYPE */
    }
    if (type) *type = REG_SZ;
    if (!data) { *len = need; return ERROR_SUCCESS; }
    if (*len < need) { *len = need; return ERROR_MORE_DATA; }
    if (wide) MultiByteToWideChar(CP_ACP, 0, v, -1, data, (int)(need / sizeof(WCHAR)));
    else memcpy(data, v, need);
    *len = need;
    return ERROR_SUCCESS;
}

LONG WINAPI RegGetValueA(HKEY key, LPCSTR sub, LPCSTR value, DWORD flags, LPDWORD type, PVOID data, LPDWORD len)
{
    return put_value(sp_value(key, value), flags, type, data, len, FALSE);
}

LONG WINAPI RegGetValueW(HKEY key, LPCWSTR sub, LPCWSTR value, DWORD flags, LPDWORD type, PVOID data, LPDWORD len)
{
    char name[64];
    WideCharToMultiByte(CP_ACP, 0, value, -1, name, sizeof(name), NULL, NULL);
    return put_value(sp_value(key, name), flags, type, data, len, TRUE);
}

LONG WINAPI RegQueryValueExA(HKEY key, LPCSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD len)
{
    return put_value(sp_value(key, name), RRF_RT_REG_SZ, type, data, len, FALSE);
}

/* ------------------------------------------------------------------ strings (WCHAR = 16 bits) */

/* code pages: everything is Latin-1 - byte for byte, as old games' names travel */
int WINAPI MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR src, int srclen, LPWSTR dst, int dstlen)
{
    if (srclen < 0) srclen = (int)strlen(src) + 1;
    if (!dstlen) return srclen;
    if (dstlen < srclen) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    for (int i = 0; i < srclen; i++) dst[i] = (unsigned char)src[i];
    return srclen;
}

int WINAPI WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR src, int srclen, LPSTR dst, int dstlen,
                               LPCSTR defchar, LPBOOL used)
{
    if (srclen < 0) srclen = lstrlenW(src) + 1;
    if (!dstlen) return srclen;
    if (dstlen < srclen) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    for (int i = 0; i < srclen; i++) dst[i] = src[i] < 256 ? (char)src[i] : '?';
    if (used) *used = FALSE;
    return srclen;
}

int WINAPI lstrlenW(LPCWSTR s) { int n = 0; if (s) while (s[n]) n++; return n; }
int WINAPI lstrlenA(LPCSTR s) { return s ? (int)strlen(s) : 0; }
int WINAPI lstrcmpW(LPCWSTR a, LPCWSTR b)
{
    while (*a && *a == *b) { a++; b++; }
    return (*a > *b) - (*a < *b);
}
LPWSTR WINAPI lstrcpyW(LPWSTR d, LPCWSTR s) { LPWSTR r = d; while ((*d++ = *s++)) continue; return r; }
LPSTR WINAPI lstrcpynA(LPSTR d, LPCSTR s, int n)
{
    if (n <= 0) return d;
    int i = 0;
    for (; i < n - 1 && s[i]; i++) d[i] = s[i];
    d[i] = 0;
    return d;
}
LPWSTR WINAPI lstrcpynW(LPWSTR d, LPCWSTR s, int n)
{
    if (n <= 0) return d;
    int i = 0;
    for (; i < n - 1 && s[i]; i++) d[i] = s[i];
    d[i] = 0;
    return d;
}
size_t winweb_wcslen(const WCHAR *s) { return (size_t)lstrlenW(s); }
size_t winweb_wcsnlen(const WCHAR *s, size_t max) { size_t n = 0; while (n < max && s[n]) n++; return n; }
int winweb_wcscmp(const WCHAR *a, const WCHAR *b) { return lstrcmpW(a, b); }
int winweb_swscanf_lu(const WCHAR *s, unsigned long *out)
{
    unsigned long v = 0;
    int any = 0;
    while (*s == ' ') s++;
    for (; *s >= '0' && *s <= '9'; s++, any = 1) v = v * 10 + (*s - '0');
    if (!any) return 0;
    *out = v;
    return 1;
}

int WINAPIV wsprintfA(LPSTR out, LPCSTR fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, 1024, fmt, ap);   /* wsprintf's own limit */
    va_end(ap);
    return n;
}

/* ------------------------------------------------------------------ COM helpers */

static int hexval(WCHAR c) { return c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : -1; }

HRESULT WINAPI CLSIDFromString(LPCWSTR s, LPCLSID out)
{
    /* {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX} */
    static const int pos[16] = { 1, 3, 5, 7, 10, 12, 15, 17, 20, 22, 25, 27, 29, 31, 33, 35 };
    BYTE b[16];
    if (!s || lstrlenW(s) != 38 || s[0] != '{' || s[37] != '}') {
        memset(out, 0, sizeof(*out)); return E_INVALIDARG; }
    for (int i = 0; i < 16; i++) {
        int h = hexval(s[pos[i]]), l = hexval(s[pos[i] + 1]);
        if (h < 0 || l < 0) { memset(out, 0, sizeof(*out)); return E_INVALIDARG; }
        b[i] = (BYTE)(h << 4 | l);
    }
    out->Data1 = (DWORD)b[0] << 24 | (DWORD)b[1] << 16 | (DWORD)b[2] << 8 | b[3];
    out->Data2 = (WORD)(b[4] << 8 | b[5]);
    out->Data3 = (WORD)(b[6] << 8 | b[7]);
    memcpy(out->Data4, b + 8, 8);
    return S_OK;
}

EM_JS(void, winweb_js_random, (void *p, int n), { crypto.getRandomValues(HEAPU8.subarray(p, p + n)); });

HRESULT WINAPI CoCreateGuid(GUID *out)
{
    winweb_js_random(out, sizeof(*out));
    out->Data3 = (out->Data3 & 0x0fff) | 0x4000;
    out->Data4[0] = (out->Data4[0] & 0x3f) | 0x80;
    return S_OK;
}

/* the lobby thread's message pump: never reached (no lobby thread in a page) */
BOOL WINAPI GetMessageW(LPMSG msg, HWND w, UINT lo, UINT hi) { return FALSE; }

/* ------------------------------------------------------------------ start-up */

/* Windows calls dplayx's DllMain when the DLL loads; a static library runs this at page start instead */
extern BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved);
__attribute__((constructor)) static void winweb_dplayx_attach(void)
{
    DllMain((HINSTANCE)(intptr_t)0x0b0b0000, DLL_PROCESS_ATTACH, NULL);
}
