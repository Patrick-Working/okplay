/* sptap - S3 wire tap (docs/DPLAY_CONFORMANCE.md section 4): a dpwsockx.dll that loads Windows' real
 * TCP/IP service provider and passes every call through unchanged, logging what DirectPlay hands
 * the provider (Send/SendEx/Reply/Open/EnumSessions...) and what the provider hands back
 * (IDirectPlaySP::HandleMessage, Get/SetSPData, Get/SetSPPlayerData).
 *
 * Black box only: it sits on the documented service-provider interface (dplaysp.h); nothing of
 * Microsoft's code is read. Copy it as dpwsockx.dll next to the test exe (Windows' dplayx and ours
 * both load the TCP/IP provider by that file name, exe folder first) and run the same scenario once
 * with Windows' dplayx and once with ours; diff the two sptap-<pid>.log files.
 *
 *   build: conformance/sptap/build.sh  ->  conformance/sptap/bin/dpwsockx.dll
 *   env SPTAP_LOG=<file> overrides the log path; SPTAP_REAL=<dll> the real provider
 */
#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include "dplay.h"
#include "dplobby.h"
#include "wine/dplaysp.h"

static FILE *g_log;
static CRITICAL_SECTION g_cs;
static DWORD g_t0;
static DWORD g_hdr;               /* dwSPHeaderSize the real provider asked for */
static DPSP_SPCALLBACKS g_real;   /* the real provider's callbacks */

static void tlog(const char *fmt, ...)
{
    va_list ap;
    if (!g_log) return;
    EnterCriticalSection(&g_cs);
    fprintf(g_log, "[%7lu] [%04lx] ", (unsigned long)(GetTickCount() - g_t0), (unsigned long)GetCurrentThreadId());
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_cs);
}

static void hexf(const char *tag, const void *p, DWORD n)
{
    const unsigned char *b = p;
    char line[3 * 32 + 1];
    if (!g_log) return;
    if (!p) { tlog("  %s: (NULL, %lu bytes)", tag, (unsigned long)n); return; }
    DWORD shown = n > 2048 ? 2048 : n;   /* (S3 compares whole messages) */
    for (DWORD off = 0; off < shown; off += 32) {
        int k = 0;
        for (DWORD i = off; i < off + 32 && i < shown; i++) k += sprintf(line + k, "%02x ", b[i]);
        tlog("  %s %04lx: %s", tag, (unsigned long)off, line);
    }
    if (shown < n) tlog("  %s ... %lu more bytes", tag, (unsigned long)(n - shown));
}

/* "play" + version + command at the start of a DirectPlay system message */
static void decode(const char *tag, const void *p, DWORD n)
{
    const unsigned char *b = p;
    if (p && n >= 8 && !memcmp(b, "play", 4))
        tlog("  %s: system message cmd 0x%02x version %u", tag, b[4] | b[5] << 8, b[6] | b[7] << 8);
    else if (p && n >= 8)
        tlog("  %s: not a system message (first dwords %08lx %08lx)", tag, *(DWORD *)b, *(DWORD *)(b + 4));
}

/* ---------------------------------------------------------------- IDirectPlaySP proxy */

typedef struct { IDirectPlaySP iface; IDirectPlaySP *real; } proxy_t;
static proxy_t g_proxy;
#define REAL (g_proxy.real)

static HRESULT WINAPI p_QueryInterface(IDirectPlaySP *t, REFIID r, void **o) { return IDirectPlaySP_QueryInterface(REAL, r, o); }
static ULONG WINAPI p_AddRef(IDirectPlaySP *t) { return IDirectPlaySP_AddRef(REAL); }
static ULONG WINAPI p_Release(IDirectPlaySP *t) { return IDirectPlaySP_Release(REAL); }
static HRESULT WINAPI p_AddMRUEntry(IDirectPlaySP *t, LPCWSTR s, LPCWSTR k, LPCVOID d, DWORD n, DWORD m)
{ return IDirectPlaySP_AddMRUEntry(REAL, s, k, d, n, m); }
static HRESULT WINAPI p_CreateAddress(IDirectPlaySP *t, REFGUID sp, REFGUID ty, LPCVOID d, DWORD n, LPVOID a, LPDWORD as)
{ return IDirectPlaySP_CreateAddress(REAL, sp, ty, d, n, a, as); }
static HRESULT WINAPI p_EnumAddress(IDirectPlaySP *t, LPDPENUMADDRESSCALLBACK cb, LPCVOID a, DWORD n, LPVOID c)
{ return IDirectPlaySP_EnumAddress(REAL, cb, a, n, c); }
static HRESULT WINAPI p_EnumMRUEntries(IDirectPlaySP *t, LPCWSTR s, LPCWSTR k, LPENUMMRUCALLBACK cb, LPVOID c)
{ return IDirectPlaySP_EnumMRUEntries(REAL, s, k, cb, c); }
static HRESULT WINAPI p_GetPlayerFlags(IDirectPlaySP *t, DPID id, LPDWORD f)
{
    HRESULT hr = IDirectPlaySP_GetPlayerFlags(REAL, id, f);
    tlog("GetPlayerFlags(0x%08lx) -> 0x%08lx flags 0x%08lx", (unsigned long)id, (unsigned long)hr, f ? (unsigned long)*f : 0ul);
    return hr;
}
static HRESULT WINAPI p_GetSPPlayerData(IDirectPlaySP *t, DPID id, LPVOID *d, LPDWORD n, DWORD f)
{
    HRESULT hr = IDirectPlaySP_GetSPPlayerData(REAL, id, d, n, f);
    tlog("GetSPPlayerData(0x%08lx, flags %lu) -> 0x%08lx, %lu bytes at %p", (unsigned long)id, (unsigned long)f,
         (unsigned long)hr, n ? (unsigned long)*n : 0ul, d ? *d : NULL);
    if (SUCCEEDED(hr) && d && *d && n) hexf("spdata", *d, *n);
    return hr;
}
static HRESULT WINAPI p_HandleMessage(IDirectPlaySP *t, LPVOID body, DWORD size, LPVOID header)
{
    tlog("HandleMessage <- %lu bytes, header %p", (unsigned long)size, header);
    decode("in", body, size);
    hexf("in", body, size);
    if (header && g_hdr) hexf("in-hdr", header, g_hdr);
    HRESULT hr = IDirectPlaySP_HandleMessage(REAL, body, size, header);
    tlog("HandleMessage -> 0x%08lx", (unsigned long)hr);
    return hr;
}
static HRESULT WINAPI p_SetSPPlayerData(IDirectPlaySP *t, DPID id, LPVOID d, DWORD n, DWORD f)
{
    tlog("SetSPPlayerData(0x%08lx, %lu bytes, flags %lu)", (unsigned long)id, (unsigned long)n, (unsigned long)f);
    hexf("spdata", d, n);
    return IDirectPlaySP_SetSPPlayerData(REAL, id, d, n, f);
}
static HRESULT WINAPI p_CreateCompoundAddress(IDirectPlaySP *t, LPCDPCOMPOUNDADDRESSELEMENT e, DWORD c, LPVOID a, LPDWORD n)
{ return IDirectPlaySP_CreateCompoundAddress(REAL, e, c, a, n); }
static HRESULT WINAPI p_GetSPData(IDirectPlaySP *t, LPVOID *d, LPDWORD n, DWORD f)
{
    HRESULT hr = IDirectPlaySP_GetSPData(REAL, d, n, f);
    tlog("GetSPData(flags %lu) -> 0x%08lx, %lu bytes at %p", (unsigned long)f, (unsigned long)hr,
         n ? (unsigned long)*n : 0ul, d ? *d : NULL);
    return hr;
}
static HRESULT WINAPI p_SetSPData(IDirectPlaySP *t, LPVOID d, DWORD n, DWORD f)
{
    tlog("SetSPData(%lu bytes, flags %lu)", (unsigned long)n, (unsigned long)f);
    return IDirectPlaySP_SetSPData(REAL, d, n, f);
}
static VOID WINAPI p_SendComplete(IDirectPlaySP *t, LPVOID c, DWORD r)
{
    tlog("SendComplete(%p, 0x%08lx)", c, (unsigned long)r);
    IDirectPlaySP_SendComplete(REAL, c, r);
}

static IDirectPlaySPVtbl g_vtbl = {
    p_QueryInterface, p_AddRef, p_Release, p_AddMRUEntry, p_CreateAddress, p_EnumAddress, p_EnumMRUEntries,
    p_GetPlayerFlags, p_GetSPPlayerData, p_HandleMessage, p_SetSPPlayerData, p_CreateCompoundAddress,
    p_GetSPData, p_SetSPData, p_SendComplete,
};

/* ---------------------------------------------------------------- callback wrappers */

static HRESULT WINAPI w_EnumSessions(LPDPSP_ENUMSESSIONSDATA d)
{
    tlog("EnumSessions(%lu bytes, return status %d)", (unsigned long)d->dwMessageSize, d->bReturnStatus);
    hexf("out", d->lpMessage, d->dwMessageSize);
    IDirectPlaySP *keep = d->lpISP; d->lpISP = &g_proxy.iface;
    HRESULT hr = g_real.EnumSessions(d);
    d->lpISP = keep;
    tlog("EnumSessions -> 0x%08lx", (unsigned long)hr);
    return hr;
}
static HRESULT WINAPI w_Reply(LPDPSP_REPLYDATA d)
{
    tlog("Reply(%lu bytes, name server 0x%08lx, header %p)", (unsigned long)d->dwMessageSize,
         (unsigned long)d->idNameServer, d->lpSPMessageHeader);
    if (g_hdr && d->dwMessageSize >= g_hdr) decode("out", (char *)d->lpMessage + g_hdr, d->dwMessageSize - g_hdr);
    hexf("out", d->lpMessage, d->dwMessageSize);
    if (d->lpSPMessageHeader && g_hdr) hexf("reply-to-hdr", d->lpSPMessageHeader, g_hdr);
    IDirectPlaySP *keep = d->lpISP; d->lpISP = &g_proxy.iface;
    HRESULT hr = g_real.Reply(d);
    d->lpISP = keep;
    tlog("Reply -> 0x%08lx", (unsigned long)hr);
    return hr;
}
static HRESULT WINAPI w_Send(LPDPSP_SENDDATA d)
{
    tlog("Send(flags 0x%08lx, to 0x%08lx, from 0x%08lx, %lu bytes, system %d)", (unsigned long)d->dwFlags,
         (unsigned long)d->idPlayerTo, (unsigned long)d->idPlayerFrom, (unsigned long)d->dwMessageSize, d->bSystemMessage);
    if (g_hdr && d->dwMessageSize >= g_hdr) decode("out", (char *)d->lpMessage + g_hdr, d->dwMessageSize - g_hdr);
    hexf("out", d->lpMessage, d->dwMessageSize);
    IDirectPlaySP *keep = d->lpISP; d->lpISP = &g_proxy.iface;
    HRESULT hr = g_real.Send(d);
    d->lpISP = keep;
    tlog("Send -> 0x%08lx", (unsigned long)hr);
    return hr;
}
static HRESULT WINAPI w_SendEx(LPDPSP_SENDEXDATA d)
{
    tlog("SendEx(flags 0x%08lx, to 0x%08lx, from 0x%08lx, %lu bytes in %lu buffers, system %d, prio %lu, timeout %lu, ctx %p)",
         (unsigned long)d->dwFlags, (unsigned long)d->idPlayerTo, (unsigned long)d->idPlayerFrom,
         (unsigned long)d->dwMessageSize, (unsigned long)d->cBuffers, d->bSystemMessage, (unsigned long)d->dwPriority,
         (unsigned long)d->dwTimeout, d->lpDPContext);
    for (DWORD i = 0; i < d->cBuffers; i++) {
        char tag[16];
        sprintf(tag, "buf%lu", (unsigned long)i);
        tlog("  %s: %lu bytes at %p", tag, (unsigned long)d->lpSendBuffers[i].len, d->lpSendBuffers[i].pData);
        if (i == 1 || (i == 0 && d->cBuffers == 1)) decode(tag, d->lpSendBuffers[i].pData, d->lpSendBuffers[i].len);
        hexf(tag, d->lpSendBuffers[i].pData, d->lpSendBuffers[i].len);
    }
    IDirectPlaySP *keep = d->lpISP; d->lpISP = &g_proxy.iface;
    HRESULT hr = g_real.SendEx(d);
    d->lpISP = keep;
    tlog("SendEx -> 0x%08lx", (unsigned long)hr);
    return hr;
}
static HRESULT WINAPI w_SendToGroupEx(LPDPSP_SENDTOGROUPEXDATA d)
{
    tlog("SendToGroupEx(flags 0x%08lx, group 0x%08lx, from 0x%08lx, %lu bytes in %lu buffers)", (unsigned long)d->dwFlags,
         (unsigned long)d->idGroupTo, (unsigned long)d->idPlayerFrom, (unsigned long)d->dwMessageSize,
         (unsigned long)d->cBuffers);
    for (DWORD i = 0; i < d->cBuffers; i++) {
        char tag[16];
        sprintf(tag, "buf%lu", (unsigned long)i);
        hexf(tag, d->lpSendBuffers[i].pData, d->lpSendBuffers[i].len);
    }
    IDirectPlaySP *keep = d->lpISP; d->lpISP = &g_proxy.iface;
    HRESULT hr = g_real.SendToGroupEx(d);
    d->lpISP = keep;
    tlog("SendToGroupEx -> 0x%08lx", (unsigned long)hr);
    return hr;
}
static HRESULT WINAPI w_Open(LPDPSP_OPENDATA d)
{
    tlog("Open(create %d, header %p, return status %d, open flags 0x%08lx, session flags 0x%08lx)", d->bCreate,
         d->lpSPMessageHeader, d->bReturnStatus, (unsigned long)d->dwOpenFlags, (unsigned long)d->dwSessionFlags);
    if (d->lpSPMessageHeader && g_hdr) hexf("open-hdr", d->lpSPMessageHeader, g_hdr);
    IDirectPlaySP *keep = d->lpISP; d->lpISP = &g_proxy.iface;
    HRESULT hr = g_real.Open(d);
    d->lpISP = keep;
    tlog("Open -> 0x%08lx", (unsigned long)hr);
    return hr;
}
static HRESULT WINAPI w_CreatePlayer(LPDPSP_CREATEPLAYERDATA d)
{
    tlog("CreatePlayer(0x%08lx, flags 0x%08lx, header %p)", (unsigned long)d->idPlayer, (unsigned long)d->dwFlags,
         d->lpSPMessageHeader);
    if (d->lpSPMessageHeader && g_hdr) hexf("cp-hdr", d->lpSPMessageHeader, g_hdr);
    IDirectPlaySP *keep = d->lpISP; d->lpISP = &g_proxy.iface;
    HRESULT hr = g_real.CreatePlayer(d);
    d->lpISP = keep;
    tlog("CreatePlayer -> 0x%08lx", (unsigned long)hr);
    return hr;
}
static HRESULT WINAPI w_DeletePlayer(LPDPSP_DELETEPLAYERDATA d)
{
    tlog("DeletePlayer(0x%08lx, flags 0x%08lx)", (unsigned long)d->idPlayer, (unsigned long)d->dwFlags);
    IDirectPlaySP *keep = d->lpISP; d->lpISP = &g_proxy.iface;
    HRESULT hr = g_real.DeletePlayer(d);
    d->lpISP = keep;
    return hr;
}
static HRESULT WINAPI w_GetCaps(LPDPSP_GETCAPSDATA d)
{
    IDirectPlaySP *keep = d->lpISP; d->lpISP = &g_proxy.iface;
    HRESULT hr = g_real.GetCaps(d);
    d->lpISP = keep;
    tlog("GetCaps(player 0x%08lx, flags 0x%08lx) -> 0x%08lx", (unsigned long)d->idPlayer, (unsigned long)d->dwFlags,
         (unsigned long)hr);
    return hr;
}
static HRESULT WINAPI w_CloseEx(LPDPSP_CLOSEDATA d) { tlog("CloseEx"); return g_real.CloseEx(d); }
static HRESULT WINAPI w_ShutdownEx(LPDPSP_SHUTDOWNDATA d) { tlog("ShutdownEx"); return g_real.ShutdownEx(d); }

/* ---------------------------------------------------------------- SPInit */

__declspec(dllexport) HRESULT WINAPI SPInit(LPSPINITDATA d)
{
    typedef HRESULT (WINAPI *init_t)(LPSPINITDATA);
    char path[MAX_PATH], logp[MAX_PATH];
    if (!GetEnvironmentVariableA("SPTAP_REAL", path, sizeof(path))) {
        GetSystemDirectoryA(path, sizeof(path));
        strcat(path, "\\dpwsockx.dll");
    }
    if (!g_log) {
        InitializeCriticalSection(&g_cs);
        g_t0 = GetTickCount();
        if (!GetEnvironmentVariableA("SPTAP_LOG", logp, sizeof(logp)))
            sprintf(logp, "sptap-%lu.log", (unsigned long)GetCurrentProcessId());
        g_log = fopen(logp, "a");
    }
    HMODULE real = LoadLibraryA(path);
    init_t init = real ? (init_t)GetProcAddress(real, "SPInit") : NULL;
    {
        char dp[MAX_PATH] = "?";
        GetModuleFileNameA(GetModuleHandleA("dplayx.dll"), dp, sizeof(dp));
        tlog("SPInit: real provider %s (%p), dplayx %s", path, real, dp);
    }
    if (!init) return DPERR_UNAVAILABLE;
    tlog("SPInit in: name %ls, guid %08lx, reserved1 0x%08lx, reserved2 0x%08lx, header %lu, address %p (%lu bytes), "
         "version 0x%08lx, callbacks size %lu version 0x%08lx", d->lpszName ? d->lpszName : L"(null)",
         d->lpGuid ? (unsigned long)d->lpGuid->Data1 : 0ul, (unsigned long)d->dwReserved1,
         (unsigned long)d->dwReserved2, (unsigned long)d->dwSPHeaderSize, d->lpAddress, (unsigned long)d->dwAddressSize,
         (unsigned long)d->dwSPVersion, d->lpCB ? (unsigned long)d->lpCB->dwSize : 0ul,
         d->lpCB ? (unsigned long)d->lpCB->dwVersion : 0ul);
    if (d->lpAddress && d->dwAddressSize) hexf("init-addr", d->lpAddress, d->dwAddressSize);
    g_proxy.iface.lpVtbl = &g_vtbl;
    g_proxy.real = d->lpISP;
    SPINITDATA copy = *d;
    copy.lpISP = &g_proxy.iface;
    HRESULT hr = init(&copy);
    d->dwSPHeaderSize = copy.dwSPHeaderSize;
    d->dwSPVersion = copy.dwSPVersion;
    g_hdr = copy.dwSPHeaderSize;
    tlog("SPInit -> 0x%08lx, header %lu bytes, version 0x%08lx, callbacks size %lu", (unsigned long)hr,
         (unsigned long)copy.dwSPHeaderSize, (unsigned long)copy.dwSPVersion, (unsigned long)d->lpCB->dwSize);
    if (FAILED(hr)) return hr;
    g_real = *d->lpCB;
#define WRAP(f) if (d->lpCB->f) d->lpCB->f = w_##f
    WRAP(EnumSessions); WRAP(Reply); WRAP(Send); WRAP(SendEx); WRAP(SendToGroupEx); WRAP(Open);
    WRAP(CreatePlayer); WRAP(DeletePlayer); WRAP(GetCaps); WRAP(CloseEx); WRAP(ShutdownEx);
#undef WRAP
    return hr;
}
