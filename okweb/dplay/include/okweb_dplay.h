/* okweb_dplay.h - our DirectPlay in the browser, for a game's own code.
 *
 * A game built for the browser usually can't include Windows' or Wine's DirectPlay headers next to
 * its own (rogan2 has stand-in Windows headers). So this header uses no Windows types: handles are
 * void *, results are int32_t HRESULTs, and the struct pointers are the game's own DPSESSIONDESC2,
 * DPNAME, DPCAPS... - binary-identical to DirectPlay's (wasm32: 4-byte pointers and DWORDs).
 * Each call is the IDirectPlay4A / IDirectPlayLobby3A method of the same name.
 *
 * The provider is "Oldkid.net Online Play" (okweb): the page must install sdk/v1/okweb-sp.js on
 * the Emscripten Module before the game creates DirectPlay, and the game must be linked with
 * -sASYNCIFY (DirectPlay waits for replies while joining). docs/WEB_NETPLAY.md, okweb/README.md. */
#ifndef OKWEB_DPLAY_H
#define OKWEB_DPLAY_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* the provider as DirectPlayEnumerate lists it */
void okweb_dp_provider(void *guid_out16, const char **name);
int okweb_dp_is_provider(const void *guid16);   /* 1 = this GUID is ours */

/* DirectPlayCreate(our provider) + QueryInterface(IID_IDirectPlay4A); NULL on failure */
void *okweb_dp_create(void);
uint32_t okweb_dp_addref(void *dp);
uint32_t okweb_dp_release(void *dp);

typedef int (*okweb_enum_sessions_cb)(void *sessiondesc, uint32_t *timeout, uint32_t flags, void *ctx);
typedef int (*okweb_enum_players_cb)(uint32_t id, uint32_t type, void *dpname, uint32_t flags, void *ctx);

int32_t okweb_dp_enum_sessions(void *dp, void *sessiondesc, uint32_t timeout, okweb_enum_sessions_cb cb, void *ctx,
                               uint32_t flags);
int32_t okweb_dp_open(void *dp, void *sessiondesc, uint32_t flags);
int32_t okweb_dp_close(void *dp);
int32_t okweb_dp_create_player(void *dp, uint32_t *id, void *dpname, void *event, void *data, uint32_t size,
                               uint32_t flags);
int32_t okweb_dp_destroy_player(void *dp, uint32_t id);
int32_t okweb_dp_enum_players(void *dp, void *instance_guid, okweb_enum_players_cb cb, void *ctx, uint32_t flags);
int32_t okweb_dp_get_caps(void *dp, void *caps, uint32_t flags);
int32_t okweb_dp_get_player_caps(void *dp, uint32_t id, void *caps, uint32_t flags);
int32_t okweb_dp_get_player_name(void *dp, uint32_t id, void *data, uint32_t *size);
int32_t okweb_dp_get_session_desc(void *dp, void *data, uint32_t *size);
int32_t okweb_dp_get_message_count(void *dp, uint32_t id, uint32_t *count);
int32_t okweb_dp_receive(void *dp, uint32_t *from, uint32_t *to, uint32_t flags, void *data, uint32_t *size);
int32_t okweb_dp_send(void *dp, uint32_t from, uint32_t to, uint32_t flags, const void *data, uint32_t size);
int32_t okweb_dp_cancel_message(void *dp, uint32_t msg_id, uint32_t flags);
int32_t okweb_dp_enable_new_players(void *dp, int enable);

/* the lobby object, for CreateAddress (the TCP/IP address a game builds before choosing a provider) */
void *okweb_dpl_create(void);
uint32_t okweb_dpl_release(void *dpl);
int32_t okweb_dpl_create_address(void *dpl, const void *sp_guid, const void *type_guid, const void *data, uint32_t size,
                                 void *address, uint32_t *address_size);

/* DirectPlay's background work and incoming messages; the receive calls run it too, and a timer
 * runs it while the game isn't calling - a game may call it from its own loop as well */
void okweb_dp_pump(void);

#ifdef __cplusplus
}
#endif
#endif
