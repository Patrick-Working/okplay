/* winweb.h - the Windows types, constants and COM machinery our DirectPlay (Wine's dplayx) needs,
 * for building it with Emscripten (wasm32). Not a Windows SDK: only what dplayx uses, written from
 * the public Win32 documentation. The functions are in ../winweb.c (one thread, no OS underneath).
 *
 * Built with -fshort-wchar: WCHAR is 16-bit as on Windows, so L"..." literals match; never call the
 * C library's wcs* functions on them (they assume 32-bit wchar_t) - winweb.c has its own. */
#include "winweb_names.h"   /* first: private names for the library's symbols */
#ifndef WINWEB_H
#define WINWEB_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <limits.h>
#include <wchar.h>   /* first, so the wcs* renames in winweb_api.h never touch its prototypes */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- calling conventions and decorations: none on wasm32 */
#define WINAPI
#define WINAPIV
#define CALLBACK
#define APIENTRY
#define STDMETHODCALLTYPE
#define STDMETHODVCALLTYPE
#define STDAPICALLTYPE
#define CDECL
#define PASCAL
#define FAR
#define NEAR
#define IN
#define OUT
#define OPTIONAL
#define CONST const
#define DECLSPEC_HIDDEN
#define DECLSPEC_EXPORT
#define DECLSPEC_IMPORT
#define DECLSPEC_NORETURN __attribute__((noreturn))
#define DECLSPEC_ALIGN(x) __attribute__((aligned(x)))
#define WINBASEAPI
#define WINADVAPI
#define WINUSERAPI
#define WINGDIAPI
#define WINOLEAPI HRESULT
#define WINOLEAPI_(t) t
#define STDAPI HRESULT
#define STDAPI_(t) t
#define __WINE_ALLOC_SIZE(...)
#define __WINE_DEALLOC(...)
#define __WINE_MALLOC
#define __WINE_CDECL
#define __WINESRC__ 1
#define __ms_va_list va_list
#define __ms_va_start va_start
#define __ms_va_end va_end
#define NTSYSAPI
#define C_ASSERT(e) _Static_assert(e, #e)
#define NTAPI
#define UNREFERENCED_PARAMETER(x) ((void)(x))
/* Wine headers' unions and structs without member names (anonymous: C11) */
#define DUMMYUNIONNAME
#define DUMMYUNIONNAME1
#define DUMMYUNIONNAME2
#define DUMMYUNIONNAME3
#define DUMMYUNIONNAME4
#define DUMMYUNIONNAME5
#define DUMMYSTRUCTNAME
#define DUMMYSTRUCTNAME1
#define DUMMYSTRUCTNAME2
#define CONTAINING_RECORD(addr, type, field) ((type *)((char *)(addr) - offsetof(type, field)))
#define FIELD_OFFSET(type, field) ((LONG)offsetof(type, field))
#define CopyMemory(d, s, n) memcpy((d), (s), (n))
#define MoveMemory(d, s, n) memmove((d), (s), (n))
#define ZeroMemory(d, n) memset((d), 0, (n))
#define FillMemory(d, n, c) memset((d), (c), (n))
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif
#define ARRAYSIZE(x) ARRAY_SIZE(x)

/* ---- integer types (Windows LLP64-style widths; wasm32 long is 32 bits like Windows') */
typedef void VOID, *PVOID, *LPVOID;
typedef const void *LPCVOID, *PCVOID;
typedef unsigned char BYTE, UCHAR, *LPBYTE, *PBYTE, BOOLEAN, *PUCHAR;
typedef unsigned short WORD, USHORT, *LPWORD, *PWORD;
typedef unsigned long DWORD, ULONG, *LPDWORD, *PDWORD, *PULONG;
typedef long LONG, *LPLONG, *PLONG;
typedef int INT, BOOL, *LPBOOL, *PBOOL, *LPINT;
typedef unsigned int UINT, *PUINT;
typedef short SHORT;
typedef char CHAR, *LPSTR, *PSTR;
typedef const char *LPCSTR, *PCSTR;
typedef unsigned short WCHAR, *LPWSTR, *PWSTR;
typedef const WCHAR *LPCWSTR, *PCWSTR;
typedef CHAR TCHAR;
typedef long long LONGLONG;
typedef unsigned long long ULONGLONG, DWORD64;
typedef intptr_t INT_PTR, LONG_PTR, LPARAM, LRESULT;
typedef uintptr_t UINT_PTR, ULONG_PTR, DWORD_PTR, WPARAM, SIZE_T, *PULONG_PTR;
typedef LONG HRESULT;
typedef LONG LSTATUS;
typedef DWORD LCID;
typedef WORD ATOM;
typedef float FLOAT;
typedef double DOUBLE;

#define TRUE 1
#define FALSE 0
#ifndef NULL
#define NULL ((void *)0)
#endif
#define MAX_PATH 260
#define INFINITE 0xffffffffu

/* ---- handles */
typedef void *HANDLE, **LPHANDLE, **PHANDLE;
#define DECLARE_HANDLE(n) typedef struct n##__ { int unused; } *n
DECLARE_HANDLE(HINSTANCE);
DECLARE_HANDLE(HWND);
DECLARE_HANDLE(HKEY);
DECLARE_HANDLE(HDC);
DECLARE_HANDLE(HICON);
DECLARE_HANDLE(HMENU);
DECLARE_HANDLE(HBITMAP);
DECLARE_HANDLE(HBRUSH);
DECLARE_HANDLE(HFONT);
DECLARE_HANDLE(HGLOBAL_);
typedef HINSTANCE HMODULE;
typedef HANDLE HGLOBAL, HLOCAL, HDRVR, HMMIO;
typedef HKEY *PHKEY;
typedef INT_PTR (WINAPI *FARPROC)(void);
typedef DWORD (WINAPI *LPTHREAD_START_ROUTINE)(LPVOID);
#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR)-1)

/* ---- GUIDs */
typedef struct _GUID { DWORD Data1; WORD Data2; WORD Data3; BYTE Data4[8]; } GUID, *LPGUID, *PGUID;
typedef GUID IID, CLSID, FMTID, *LPIID, *LPCLSID;
typedef const GUID *LPCGUID, *REFGUID, *REFIID, *REFCLSID, *REFFMTID;
#define GUID_DEFINED
#define CLSID_DEFINED
#define IID_DEFINED
static inline BOOL IsEqualGUID(REFGUID a, REFGUID b) { return !memcmp(a, b, sizeof(GUID)); }
#define IsEqualIID(a, b) IsEqualGUID(a, b)
#define IsEqualCLSID(a, b) IsEqualGUID(a, b)
#undef DEFINE_GUID
#ifdef INITGUID
#define DEFINE_GUID(n, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    const GUID n = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
#else
#define DEFINE_GUID(n, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) extern const GUID n
#endif
extern const GUID GUID_NULL;
#define IID_NULL GUID_NULL
#define CLSID_NULL GUID_NULL

/* ---- HRESULT and errors */
#define SUCCEEDED(hr) ((HRESULT)(hr) >= 0)
#define FAILED(hr) ((HRESULT)(hr) < 0)
#define MAKE_HRESULT(sev, fac, code) ((HRESULT)(((unsigned long)(sev) << 31) | ((unsigned long)(fac) << 16) | ((unsigned long)(code))))
#define HRESULT_FROM_WIN32(x) ((HRESULT)(x) <= 0 ? (HRESULT)(x) : (HRESULT)(((x) & 0x0000ffff) | (7 << 16) | 0x80000000))
#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_NOTIMPL ((HRESULT)0x80004001L)
#define E_PENDING ((HRESULT)0x8000000aL)
#define E_NOINTERFACE ((HRESULT)0x80004002L)
#define E_POINTER ((HRESULT)0x80004003L)
#define E_ABORT ((HRESULT)0x80004004L)
#define E_FAIL ((HRESULT)0x80004005L)
#define E_UNEXPECTED ((HRESULT)0x8000ffffL)
#define E_ACCESSDENIED ((HRESULT)0x80070005L)
#define E_HANDLE ((HRESULT)0x80070006L)
#define E_OUTOFMEMORY ((HRESULT)0x8007000eL)
#define E_INVALIDARG ((HRESULT)0x80070057L)
#define CLASS_E_NOAGGREGATION ((HRESULT)0x80040110L)
#define CLASS_E_CLASSNOTAVAILABLE ((HRESULT)0x80040111L)
#define CO_E_NOTINITIALIZED ((HRESULT)0x800401f0L)
#define REGDB_E_CLASSNOTREG ((HRESULT)0x80040154L)
#define ERROR_SUCCESS 0L
#define NO_ERROR 0L
#define ERROR_FILE_NOT_FOUND 2L
#define ERROR_ACCESS_DENIED 5L
#define ERROR_INVALID_HANDLE 6L
#define ERROR_NOT_ENOUGH_MEMORY 8L
#define ERROR_OUTOFMEMORY 14L
#define ERROR_INVALID_PARAMETER 87L
#define ERROR_INSUFFICIENT_BUFFER 122L
#define ERROR_MORE_DATA 234L
#define ERROR_NO_MORE_ITEMS 259L
#define ERROR_ALREADY_EXISTS 183L
#define ERROR_TIMEOUT 1460L
#define ERROR_INVALID_ADDRESS 487L

/* ---- waits, threads, synchronisation */
#define WAIT_OBJECT_0 0x00000000u
#define WAIT_ABANDONED 0x00000080u
#define WAIT_TIMEOUT 0x00000102u
#define WAIT_FAILED 0xffffffffu
#define CREATE_SUSPENDED 0x00000004
#define DUPLICATE_SAME_ACCESS 0x00000002
#define DUPLICATE_CLOSE_SOURCE 0x00000001
#define DLL_PROCESS_DETACH 0
#define DLL_PROCESS_ATTACH 1
#define DLL_THREAD_ATTACH 2
#define DLL_THREAD_DETACH 3
typedef struct _SECURITY_ATTRIBUTES { DWORD nLength; LPVOID lpSecurityDescriptor; BOOL bInheritHandle; }
    SECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES, *PSECURITY_ATTRIBUTES;
typedef struct _RTL_CRITICAL_SECTION_DEBUG {
    WORD Type; WORD CreatorBackTraceIndex; struct _RTL_CRITICAL_SECTION *CriticalSection;
    void *ProcessLocksList[2]; DWORD EntryCount; DWORD ContentionCount; DWORD_PTR Spare[8];
} RTL_CRITICAL_SECTION_DEBUG, *PRTL_CRITICAL_SECTION_DEBUG, CRITICAL_SECTION_DEBUG;
typedef struct _RTL_CRITICAL_SECTION {
    PRTL_CRITICAL_SECTION_DEBUG DebugInfo; LONG LockCount; LONG RecursionCount;
    HANDLE OwningThread; HANDLE LockSemaphore; ULONG_PTR SpinCount;
    RTL_CRITICAL_SECTION_DEBUG debug_storage;   /* winweb: DebugInfo points here */
} CRITICAL_SECTION, *LPCRITICAL_SECTION, *PCRITICAL_SECTION, RTL_CRITICAL_SECTION;
typedef union { void *Ptr; } INIT_ONCE, *PINIT_ONCE, *LPINIT_ONCE;
#define INIT_ONCE_STATIC_INIT { 0 }
typedef BOOL (WINAPI *PINIT_ONCE_FN)(PINIT_ONCE, PVOID, PVOID *);
#define RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO 0

/* the one-thread page: interlocked operations are plain ones */
static inline LONG InterlockedIncrement(LONG volatile *p) { return ++*p; }
static inline LONG InterlockedDecrement(LONG volatile *p) { return --*p; }
static inline LONG InterlockedExchange(LONG volatile *p, LONG v) { LONG o = *p; *p = v; return o; }
static inline LONG InterlockedExchangeAdd(LONG volatile *p, LONG v) { LONG o = *p; *p += v; return o; }
static inline LONG InterlockedCompareExchange(LONG volatile *p, LONG x, LONG c) { LONG o = *p; if (o == c) *p = x; return o; }
static inline void *InterlockedExchangePointer(void *volatile *p, void *v) { void *o = *p; *p = v; return o; }
static inline void *InterlockedCompareExchangePointer(void *volatile *p, void *x, void *c) { void *o = *p; if (o == c) *p = x; return o; }

/* ---- time */
typedef struct _SYSTEMTIME { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; }
    SYSTEMTIME, *LPSYSTEMTIME, *PSYSTEMTIME;
typedef struct _FILETIME { DWORD dwLowDateTime, dwHighDateTime; } FILETIME, *LPFILETIME;
typedef union _LARGE_INTEGER { struct { DWORD LowPart; LONG HighPart; } u; LONGLONG QuadPart; } LARGE_INTEGER, *PLARGE_INTEGER;

/* ---- registry */
#define HKEY_CLASSES_ROOT ((HKEY)(ULONG_PTR)0x80000000)
#define HKEY_CURRENT_USER ((HKEY)(ULONG_PTR)0x80000001)
#define HKEY_LOCAL_MACHINE ((HKEY)(ULONG_PTR)0x80000002)
typedef DWORD REGSAM, ACCESS_MASK;
#define KEY_READ 0x20019
#define KEY_WRITE 0x20006
#define KEY_ALL_ACCESS 0xf003f
#define KEY_ENUMERATE_SUB_KEYS 0x0008
#define KEY_QUERY_VALUE 0x0001
#define REG_NONE 0
#define REG_SZ 1
#define REG_EXPAND_SZ 2
#define REG_BINARY 3
#define REG_DWORD 4
#define RRF_RT_REG_SZ 0x00000002
#define RRF_RT_REG_EXPAND_SZ 0x00000004
#define RRF_RT_REG_BINARY 0x00000008
#define RRF_RT_REG_DWORD 0x00000010
#define RRF_RT_DWORD 0x00000018
#define RRF_RT_ANY 0x0000ffff
#define RRF_NOEXPAND 0x10000000
#define RRF_ZEROONFAILURE 0x20000000

/* ---- memory mappings, processes, misc */
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_EXECUTE_READWRITE 0x40
#define SEC_COMMIT 0x08000000
#define FILE_MAP_WRITE 0x0002
#define FILE_MAP_READ 0x0004
#define FILE_MAP_ALL_ACCESS 0xf001f
#define STANDARD_RIGHTS_REQUIRED 0x000f0000
#define SYNCHRONIZE 0x00100000
#define EVENT_ALL_ACCESS 0x1f0003
#define SEMAPHORE_ALL_ACCESS 0x1f0003
#define NORMAL_PRIORITY_CLASS 0x20
#define CREATE_NEW_CONSOLE 0x00000010
#define CREATE_DEFAULT_ERROR_MODE 0x04000000
#define CP_ACP 0
#define CP_OEMCP 1
#define CP_UTF8 65001
#define MB_PRECOMPOSED 0x1
typedef struct _MEMORY_BASIC_INFORMATION {
    PVOID BaseAddress; PVOID AllocationBase; DWORD AllocationProtect; SIZE_T RegionSize;
    DWORD State; DWORD Protect; DWORD Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;
#define MEM_COMMIT 0x1000
typedef struct _STARTUPINFOA {
    DWORD cb; LPSTR lpReserved, lpDesktop, lpTitle; DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars,
    dwYCountChars, dwFillAttribute, dwFlags; WORD wShowWindow, cbReserved2; LPBYTE lpReserved2;
    HANDLE hStdInput, hStdOutput, hStdError;
} STARTUPINFOA, *LPSTARTUPINFOA;
typedef struct _PROCESS_INFORMATION { HANDLE hProcess, hThread; DWORD dwProcessId, dwThreadId; }
    PROCESS_INFORMATION, *LPPROCESS_INFORMATION;
typedef struct tagPOINT { LONG x, y; } POINT;
typedef struct tagRECT { LONG left, top, right, bottom; } RECT, *LPRECT;
typedef struct tagSIZE { LONG cx, cy; } SIZE;
typedef struct tagMSG { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt; } MSG, *LPMSG;
#define LOWORD(l) ((WORD)((DWORD_PTR)(l) & 0xffff))
#define HIWORD(l) ((WORD)((DWORD_PTR)(l) >> 16))
#define MAKELONG(a, b) ((LONG)(((WORD)(a)) | ((DWORD)((WORD)(b))) << 16))
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))

/* ---- COM, C style (CINTERFACE) */
#define interface struct
#define CONST_VTBL const
#define BEGIN_INTERFACE
#define END_INTERFACE
#define PURE
#define THIS_ INTERFACE *This,
#define THIS INTERFACE *This
#define STDMETHOD(m) HRESULT (STDMETHODCALLTYPE *m)
#define STDMETHOD_(t, m) t (STDMETHODCALLTYPE *m)
#define DECLARE_INTERFACE(i) \
    typedef interface i { const struct i##Vtbl *lpVtbl; } i; typedef const struct i##Vtbl i##Vtbl; struct i##Vtbl
#define DECLARE_INTERFACE_(i, b) DECLARE_INTERFACE(i)
#ifndef CINTERFACE
#define CINTERFACE 1
#endif
#ifndef COBJMACROS
#define COBJMACROS 1
#endif

typedef interface IUnknown IUnknown, *LPUNKNOWN;
typedef struct IUnknownVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IUnknown *This, REFIID riid, void **ppv);
    ULONG (STDMETHODCALLTYPE *AddRef)(IUnknown *This);
    ULONG (STDMETHODCALLTYPE *Release)(IUnknown *This);
} IUnknownVtbl;
interface IUnknown { CONST_VTBL IUnknownVtbl *lpVtbl; };
#define IUnknown_QueryInterface(p, a, b) (p)->lpVtbl->QueryInterface(p, a, b)
#define IUnknown_AddRef(p) (p)->lpVtbl->AddRef(p)
#define IUnknown_Release(p) (p)->lpVtbl->Release(p)

typedef interface IClassFactory IClassFactory, *LPCLASSFACTORY;
typedef struct IClassFactoryVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IClassFactory *This, REFIID riid, void **ppv);
    ULONG (STDMETHODCALLTYPE *AddRef)(IClassFactory *This);
    ULONG (STDMETHODCALLTYPE *Release)(IClassFactory *This);
    HRESULT (STDMETHODCALLTYPE *CreateInstance)(IClassFactory *This, IUnknown *outer, REFIID riid, void **ppv);
    HRESULT (STDMETHODCALLTYPE *LockServer)(IClassFactory *This, BOOL lock);
} IClassFactoryVtbl;
interface IClassFactory { CONST_VTBL IClassFactoryVtbl *lpVtbl; };
extern const GUID IID_IUnknown, IID_IClassFactory;
#define IClassFactory_QueryInterface(p, a, b) (p)->lpVtbl->QueryInterface(p, a, b)
#define IClassFactory_AddRef(p) (p)->lpVtbl->AddRef(p)
#define IClassFactory_Release(p) (p)->lpVtbl->Release(p)
#define IClassFactory_CreateInstance(p, a, b, c) (p)->lpVtbl->CreateInstance(p, a, b, c)
#define IClassFactory_LockServer(p, a) (p)->lpVtbl->LockServer(p, a)

/* heaps: the C heap */
#define HEAP_ZERO_MEMORY 0x00000008
#define GetProcessHeap() ((HANDLE)1)
#define HeapAlloc(h, f, n) (((f) & HEAP_ZERO_MEMORY) ? calloc(1, (n)) : malloc(n))
#define HeapReAlloc(h, f, p, n) realloc((p), (n))
#define HeapFree(h, f, p) (free(p), TRUE)
#define CLSCTX_INPROC_SERVER 0x1
#define CLSCTX_ALL 0x17
#define COINIT_APARTMENTTHREADED 0x2

#ifdef __cplusplus
}
#endif

#include "winweb_api.h"
#endif
