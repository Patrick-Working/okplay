/* okweb_password.c - game passwords in the browser (the drop-in draws its own boxes: builtin.c).
 *
 * AoE2 has no password box, but DirectPlay has passwords and the host checks them on every join.
 * As in the drop-in, Online Play asks: the host when it creates a game (an optional password), a
 * joiner when the game it picked needs one (dplay.c calls these at Open). Here the page asks
 * (okweb-sp.js Module.okwebSP.askPassword: a box styled like the drop-in's, or the page's options).
 *
 * What DirectPlay gets - and what goes on the wire - is never the typed text: the same value as the
 * drop-in's, "okp1-" + the first 20 bytes of SHA-256("okplay game password v1|" + the text in UTF-8 +
 * the session's instance GUID) in hex. A sniffed value is useless for any other game and doesn't
 * reveal the password. The box waits like DirectPlay's own waits (Asyncify; winweb.c keeps pumping). */
#include "winweb.h"
#include <emscripten.h>
#include "dplay.h"

BOOL DP_BUILTIN_AskHostPassword( const GUID *instance, WCHAR *hashed, DWORD size );
HRESULT DP_BUILTIN_AskJoinPassword( const GUID *instance, WCHAR *hashed, DWORD size );
void DP_BUILTIN_JoinFailed( HRESULT hr );

/* kind 0 = host, 1 = join. 1 = a password (its hash in out), 0 = none (host), -1 = Cancel (join) */
EM_ASYNC_JS(int, okweb_js_ask_password, (int kind, const void *guid, char *out, int outsz), {
    const sp = Module.okwebSP;
    const g = HEAPU8.slice(guid, guid + 16);   /* (before awaiting: the memory may grow meanwhile) */
    let typed;
    try { typed = sp && sp.askPassword ? await sp.askPassword(kind === 0 ? 'host' : 'join') : null; } catch (e) { typed = null; }
    if (typed === null || typed === undefined) return kind === 0 ? 0 : -1;
    if (kind === 0 && typed === '') return 0;
    const enc = new TextEncoder();
    const salt = enc.encode('okplay game password v1|'), text = enc.encode(String(typed));
    const all = new Uint8Array(salt.length + text.length + 16);
    all.set(salt); all.set(text, salt.length); all.set(g, salt.length + text.length);
    const d = new Uint8Array(await crypto.subtle.digest('SHA-256', all));
    let s = 'okp1-';
    for (let i = 0; i < 20; i++) s += d[i].toString(16).padStart(2, '0');
    for (let i = 0; i < s.length && i + 1 < outsz; i++) HEAPU8[out + i] = s.charCodeAt(i);
    HEAPU8[out + Math.min(s.length, outsz - 1)] = 0;
    return 1;
});

EM_JS(void, okweb_js_wrong_password, (void), {
    const sp = Module.okwebSP;
    if (sp && sp.wrongPassword) sp.wrongPassword();
});

static int ask( int kind, const GUID *instance, WCHAR *hashed, DWORD size )
{
    char a[ 64 ];
    int r, i;

    if ( !winweb_wait_begin() ) return kind == 0 ? 0 : -1;   /* (can't wait here: an open game / no join) */
    r = okweb_js_ask_password( kind, instance, a, sizeof(a) );
    winweb_wait_end();
    if ( r != 1 ) return r;
    for ( i = 0; a[ i ] && i + 1 < (int)size; i++ ) hashed[ i ] = (WCHAR)(unsigned char)a[ i ];
    hashed[ i ] = 0;
    return 1;
}

BOOL DP_BUILTIN_AskHostPassword( const GUID *instance, WCHAR *hashed, DWORD size )
{
    return ask( 0, instance, hashed, size ) == 1;
}

HRESULT DP_BUILTIN_AskJoinPassword( const GUID *instance, WCHAR *hashed, DWORD size )
{
    return ask( 1, instance, hashed, size ) == 1 ? DP_OK : DPERR_USERCANCEL;
}

void DP_BUILTIN_JoinFailed( HRESULT hr )
{
    if ( hr == DPERR_INVALIDPASSWORD ) okweb_js_wrong_password();
}
