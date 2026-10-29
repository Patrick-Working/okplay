/* winweb_api.h - the Windows functions our DirectPlay calls, as implemented by ../winweb.c for a
 * single-threaded browser page. Exactly the 64 imports of the Windows build's plain dplayx.dll
 * (objdump -p winedplay/bin/plain/dplayx.dll), plus the few helpers they need. */
#ifndef WINWEB_API_H
#define WINWEB_API_H

#ifdef __cplusplus
extern "C" {
#endif

/* registry: a fixed table (the okweb provider's entry), read only */
LONG WINAPI RegOpenKeyExA(HKEY key, LPCSTR sub, DWORD opts, REGSAM sam, PHKEY out);
LONG WINAPI RegCloseKey(HKEY key);
LONG WINAPI RegEnumKeyExA(HKEY key, DWORD index, LPSTR name, LPDWORD name_len, LPDWORD reserved,
                          LPSTR cls, LPDWORD cls_len, LPFILETIME ft);
LONG WINAPI RegQueryValueExA(HKEY key, LPCSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD len);
LONG WINAPI RegGetValueA(HKEY key, LPCSTR sub, LPCSTR value, DWORD flags, LPDWORD type, PVOID data, LPDWORD len);
LONG WINAPI RegGetValueW(HKEY key, LPCWSTR sub, LPCWSTR value, DWORD flags, LPDWORD type, PVOID data, LPDWORD len);

/* handles, events, semaphores, threads (cooperative: see winweb.c) */
BOOL WINAPI CloseHandle(HANDLE h);
HANDLE WINAPI CreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCWSTR name);
HANDLE WINAPI CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCSTR name);
BOOL WINAPI SetEvent(HANDLE h);
BOOL WINAPI ResetEvent(HANDLE h);
HANDLE WINAPI CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG max, LPCSTR name);
BOOL WINAPI ReleaseSemaphore(HANDLE h, LONG count, LPLONG previous);
DWORD WINAPI WaitForSingleObject(HANDLE h, DWORD ms);
HANDLE WINAPI CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg,
                           DWORD flags, LPDWORD id);
DWORD WINAPI ResumeThread(HANDLE h);
BOOL WINAPI TerminateThread(HANDLE h, DWORD code);
BOOL WINAPI DuplicateHandle(HANDLE src_proc, HANDLE src, HANDLE dst_proc, LPHANDLE dst, DWORD access,
                            BOOL inherit, DWORD opts);
void WINAPI Sleep(DWORD ms);
DWORD WINAPI SleepEx(DWORD ms, BOOL alertable);
void WINAPI InitializeCriticalSection(LPCRITICAL_SECTION cs);
void WINAPI DeleteCriticalSection(LPCRITICAL_SECTION cs);
void WINAPI EnterCriticalSection(LPCRITICAL_SECTION cs);
void WINAPI LeaveCriticalSection(LPCRITICAL_SECTION cs);
BOOL WINAPI InitOnceExecuteOnce(PINIT_ONCE once, PINIT_ONCE_FN fn, PVOID param, PVOID *ctx);

/* process and module */
HANDLE WINAPI GetCurrentProcess(void);
DWORD WINAPI GetCurrentProcessId(void);
DWORD WINAPI GetCurrentThreadId(void);
/* thread-local slots (winedbg_shim.c's per-thread state): one thread in a page, so plain slots */
#define TLS_OUT_OF_INDEXES ((DWORD)0xFFFFFFFF)
DWORD WINAPI TlsAlloc(void);
LPVOID WINAPI TlsGetValue(DWORD index);
BOOL WINAPI TlsSetValue(DWORD index, LPVOID value);
BOOL WINAPI DisableThreadLibraryCalls(HMODULE m);
HMODULE WINAPI GetModuleHandleA(LPCSTR name);
DWORD WINAPI GetModuleFileNameA(HMODULE m, LPSTR out, DWORD size);
HMODULE WINAPI LoadLibraryA(LPCSTR name);
BOOL WINAPI FreeLibrary(HMODULE m);
FARPROC WINAPI GetProcAddress(HMODULE m, LPCSTR name);
BOOL WINAPI CreateProcessA(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit,
                           DWORD flags, LPVOID env, LPCSTR dir, LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi);
DWORD WINAPI GetEnvironmentVariableA(LPCSTR name, LPSTR out, DWORD size);
DWORD WINAPI GetVersion(void);
void WINAPI DebugBreak(void);
DWORD WINAPI GetLastError(void);
void WINAPI SetLastError(DWORD e);

/* memory (lobby shared memory becomes ordinary memory) */
HANDLE WINAPI CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect, DWORD size_hi, DWORD size_lo,
                                 LPCSTR name);
LPVOID WINAPI MapViewOfFile(HANDLE map, DWORD access, DWORD off_hi, DWORD off_lo, SIZE_T size);
LPVOID WINAPI MapViewOfFileEx(HANDLE map, DWORD access, DWORD off_hi, DWORD off_lo, SIZE_T size, LPVOID base);
BOOL WINAPI UnmapViewOfFile(LPCVOID base);
BOOL WINAPI FlushViewOfFile(LPCVOID base, SIZE_T size);
BOOL WINAPI VirtualProtect(LPVOID addr, SIZE_T size, DWORD prot, PDWORD old);
SIZE_T WINAPI VirtualQuery(LPCVOID addr, PMEMORY_BASIC_INFORMATION mbi, SIZE_T len);
BOOL WINAPI IsBadStringPtrA(LPCSTR s, UINT_PTR max);
BOOL WINAPI IsBadStringPtrW(LPCWSTR s, UINT_PTR max);

/* time */
DWORD WINAPI GetTickCount(void);
DWORD WINAPI timeGetTime(void);
void WINAPI GetLocalTime(LPSYSTEMTIME st);

/* strings (16-bit WCHAR) */
int WINAPI MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR src, int srclen, LPWSTR dst, int dstlen);
int WINAPI WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR src, int srclen, LPSTR dst, int dstlen,
                               LPCSTR defchar, LPBOOL used);
int WINAPI lstrlenW(LPCWSTR s);
int WINAPI lstrlenA(LPCSTR s);
int WINAPI lstrcmpW(LPCWSTR a, LPCWSTR b);
LPWSTR WINAPI lstrcpyW(LPWSTR d, LPCWSTR s);
LPSTR WINAPI lstrcpynA(LPSTR d, LPCSTR s, int n);
LPWSTR WINAPI lstrcpynW(LPWSTR d, LPCWSTR s, int n);
int WINAPIV wsprintfA(LPSTR out, LPCSTR fmt, ...);
size_t winweb_wcslen(const WCHAR *s);
int winweb_wcscmp(const WCHAR *a, const WCHAR *b);
size_t winweb_wcsnlen(const WCHAR *s, size_t max);
#define wcsnlen winweb_wcsnlen
int winweb_swscanf_lu(const WCHAR *s, unsigned long *out);   /* the one swscanf use: "%lu" */
#define wcslen winweb_wcslen
#define wcscmp winweb_wcscmp
#define swscanf(s, fmt, out) winweb_swscanf_lu(s, out)

/* COM */
HRESULT WINAPI CLSIDFromString(LPCWSTR s, LPCLSID out);
HRESULT WINAPI CoCreateGuid(GUID *out);

/* messages (the lobby thread's message pump: never used in the browser) */
BOOL WINAPI GetMessageW(LPMSG msg, HWND w, UINT lo, UINT hi);

/* winweb's own: the browser build's pump - DirectPlay's background work (keepalive pings, async
 * session searches) runs here, called from DirectPlay's entry points and the provider */
void winweb_pump(void);
void winweb_every(DWORD ms, void (*fn)(void *), void *arg);   /* a background loop as a task (0 = stop) */
void winweb_set_pump_hook(void (*fn)(void));                  /* the provider's message delivery */
int winweb_wait_begin(void);   /* a wait for the page (a password box): 0 = can't wait here; else the clock pumps until */
void winweb_wait_end(void);    /* ... winweb_wait_end() */

#ifdef __cplusplus
}
#endif
#endif
