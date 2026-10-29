/* the drop-in's entry-point plumbing: mode.c (which DirectPlay) and switch.c (mode=all's mixed list) */
#ifndef OKP_SWITCH_H
#define OKP_SWITCH_H

typedef HRESULT (WINAPI *create_t)( LPGUID, LPDIRECTPLAY *, IUnknown * );
typedef HRESULT (WINAPI *enum_a_t)( LPDPENUMDPCALLBACKA, LPVOID );
typedef HRESULT (WINAPI *enum_w_t)( LPDPENUMDPCALLBACKW, LPVOID );

void SW_SetWindows( create_t create );
void SW_SetTrace( BOOL on );
BOOL SW_Tracing( void );       /* trace=1 is on */   /* okplay.ini trace=1: log every DirectPlay call and its result */
HRESULT SW_Create( REFIID riid, void **out );
HRESULT SW_Wrap( IDirectPlay *dp, REFIID riid, void **out );   /* takes over dp's reference */
HRESULT SW_EnumerateA( LPDPENUMDPCALLBACKA cb, LPVOID ctx, enum_a_t windows );
HRESULT SW_EnumerateW( LPDPENUMDPCALLBACKW cb, LPVOID ctx, enum_w_t windows );
HRESULT SW_GetClassObject( REFIID riid, void **out );

#endif
