/* okweb_flat.c - include/okweb_dplay.h: our DirectPlay as plain C calls, for games whose own headers
 * can't sit next to DirectPlay's (see the header). Each function is one IDirectPlay4A /
 * IDirectPlayLobby3A method; the struct pointers pass straight through (same layouts). */
#include "winweb.h"
#include "dplay.h"
#include "dplobby.h"
#include "okweb_dplay.h"

static const GUID OKWEB_SPGUID = { 0x66d8dc59, 0x28e8, 0x4d3e, { 0xba, 0x93, 0x2e, 0x98, 0xd2, 0x51, 0xc0, 0xf3 } };

void okweb_dp_provider(void *guid_out16, const char **name)
{
    if (guid_out16) memcpy(guid_out16, &OKWEB_SPGUID, sizeof(GUID));
    if (name) *name = "Oldkid.net Online Play";
}

int okweb_dp_is_provider(const void *guid16) { return guid16 && !memcmp(guid16, &OKWEB_SPGUID, sizeof(GUID)); }

void okweb_dp_pump(void) { winweb_pump(); }

void *okweb_dp_create(void)
{
    LPDIRECTPLAY dp1 = NULL;
    IDirectPlay4A *dp = NULL;
    GUID sp = OKWEB_SPGUID;
    if (DirectPlayCreate(&sp, &dp1, NULL) != DP_OK || !dp1) return NULL;
    IDirectPlay_QueryInterface(dp1, &IID_IDirectPlay4A, (void **)&dp);
    IDirectPlay_Release(dp1);
    return dp;
}

#define DP ((IDirectPlay4A *)dp)
uint32_t okweb_dp_addref(void *dp) { return IDirectPlayX_AddRef(DP); }
uint32_t okweb_dp_release(void *dp) { return IDirectPlayX_Release(DP); }

int32_t okweb_dp_enum_sessions(void *dp, void *sd, uint32_t timeout, okweb_enum_sessions_cb cb, void *ctx, uint32_t flags)
{
    return IDirectPlayX_EnumSessions(DP, sd, timeout, (LPDPENUMSESSIONSCALLBACK2)cb, ctx, flags);
}
int32_t okweb_dp_open(void *dp, void *sd, uint32_t flags) { return IDirectPlayX_Open(DP, sd, flags); }
int32_t okweb_dp_close(void *dp) { return IDirectPlayX_Close(DP); }
int32_t okweb_dp_create_player(void *dp, uint32_t *id, void *name, void *event, void *data, uint32_t size, uint32_t flags)
{
    DPID pid = 0;
    HRESULT hr = IDirectPlayX_CreatePlayer(DP, &pid, name, event, data, size, flags);
    if (id) *id = pid;
    return hr;
}
int32_t okweb_dp_destroy_player(void *dp, uint32_t id) { return IDirectPlayX_DestroyPlayer(DP, id); }
int32_t okweb_dp_enum_players(void *dp, void *instance, okweb_enum_players_cb cb, void *ctx, uint32_t flags)
{
    return IDirectPlayX_EnumPlayers(DP, instance, (LPDPENUMPLAYERSCALLBACK2)cb, ctx, flags);
}
int32_t okweb_dp_get_caps(void *dp, void *caps, uint32_t flags) { return IDirectPlayX_GetCaps(DP, caps, flags); }
int32_t okweb_dp_get_player_caps(void *dp, uint32_t id, void *caps, uint32_t flags)
{
    return IDirectPlayX_GetPlayerCaps(DP, id, caps, flags);
}
int32_t okweb_dp_get_player_name(void *dp, uint32_t id, void *data, uint32_t *size)
{
    DWORD n = size ? *size : 0;
    HRESULT hr = IDirectPlayX_GetPlayerName(DP, id, data, &n);
    if (size) *size = n;
    return hr;
}
int32_t okweb_dp_get_session_desc(void *dp, void *data, uint32_t *size)
{
    DWORD n = size ? *size : 0;
    HRESULT hr = IDirectPlayX_GetSessionDesc(DP, data, &n);
    if (size) *size = n;
    return hr;
}
int32_t okweb_dp_get_message_count(void *dp, uint32_t id, uint32_t *count)
{
    DWORD n = 0;
    winweb_pump();
    HRESULT hr = IDirectPlayX_GetMessageCount(DP, id, &n);
    if (count) *count = n;
    return hr;
}
int32_t okweb_dp_receive(void *dp, uint32_t *from, uint32_t *to, uint32_t flags, void *data, uint32_t *size)
{
    DPID f = 0, t = 0;
    DWORD n = size ? *size : 0;
    winweb_pump();
    HRESULT hr = IDirectPlayX_Receive(DP, &f, &t, flags, data, &n);
    if (from) *from = f;
    if (to) *to = t;
    if (size) *size = n;
    return hr;
}
int32_t okweb_dp_send(void *dp, uint32_t from, uint32_t to, uint32_t flags, const void *data, uint32_t size)
{
    return IDirectPlayX_Send(DP, from, to, flags, (void *)data, size);
}
int32_t okweb_dp_cancel_message(void *dp, uint32_t msg_id, uint32_t flags)
{
    return IDirectPlayX_CancelMessage(DP, msg_id, flags);   /* (0, 0) = every queued message */
}
int32_t okweb_dp_enable_new_players(void *dp, int enable)
{
    IDirectPlay *dp1 = NULL;
    HRESULT hr = IDirectPlayX_QueryInterface(DP, &IID_IDirectPlay, (void **)&dp1);
    if (FAILED(hr)) return hr;
    hr = IDirectPlay_EnableNewPlayers(dp1, enable);
    IDirectPlay_Release(dp1);
    return hr;
}
#undef DP

void *okweb_dpl_create(void)
{
    LPDIRECTPLAYLOBBYA l1 = NULL;
    IDirectPlayLobby3A *l3 = NULL;
    if (DirectPlayLobbyCreateA(NULL, &l1, NULL, NULL, 0) != DP_OK || !l1) return NULL;
    IDirectPlayLobby_QueryInterface(l1, &IID_IDirectPlayLobby3A, (void **)&l3);
    IDirectPlayLobby_Release(l1);
    return l3;
}
uint32_t okweb_dpl_release(void *dpl) { return IDirectPlayLobby_Release((IDirectPlayLobby3A *)dpl); }
int32_t okweb_dpl_create_address(void *dpl, const void *sp, const void *type, const void *data, uint32_t size,
                                 void *address, uint32_t *address_size)
{
    DWORD n = address_size ? *address_size : 0;
    HRESULT hr = IDirectPlayLobby_CreateAddress((IDirectPlayLobby3A *)dpl, sp, type, data, size, address, &n);
    if (address_size) *address_size = n;
    return hr;
}
