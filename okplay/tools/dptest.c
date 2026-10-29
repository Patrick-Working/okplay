/* dptest - a stand-in for AoE2: drives real dplayx.dll with a chosen service
 * provider so hosting/joining/traffic can be tested without the game.
 *
 *   dptest [--sp okplay|tcpip] [--ip HOST] [--name N] [--secs S] [--expect P] host|join|list
 *          [--dplayx DLL] [--sflags 0xC044] [--guaranteed]
 *
 *   --dplayx DLL    use this dplayx.dll (e.g. Wine's build) instead of Windows' registered one;
 *                   default from env DPTEST_DPLAYX. The DLL actually used is printed.
 *   --sflags X      session flags when hosting (default 0x44; AoE2 uses 0xC044, which adds
 *                   DPSESSION_DIRECTPLAYPROTOCOL); default from env DPTEST_SFLAGS
 *   --guaranteed    send pings/pongs with DPSEND_GUARANTEED (env DPTEST_GUARANTEED=1)
 *   --cycles N      host/join, Close, and again N times with the SAME DirectPlay object
 *                   (leaving one game and joining/hosting another); cycle c's session is
 *                   named "...#c" and joiners look for exactly that one
 *   --aoe2names     call DirectPlay the way AoE2 does: the WCHAR interface fed ANSI bytes (and a
 *                   session opened with dwCurrentPlayers = 1); checks names arrive byte for byte
 *                   and the player count never exceeds --expect (STAT aoe2names ...)
 *   --session NAME  join/list only the session with exactly this name (with several hosts up)
 *   --retry N       after a failed join, list and join again (same DirectPlay object), up to N times
 *   --nolongname    create our player with no long name (NULL), as the first lobby test did - it
 *                   crashed a real AoE2 host; every dptest fails if it is handed a NULL name
 *   --nosend        never send our own messages (pings); with --app aoe2 this is the only way
 *                   'join' is allowed: sit silently in a real AoE2 lobby as an extra player
 *   --extra         also create a second local player right away: a host that "has company" without
 *                   a joiner (MTM2 never locks its session, so the drop-in judges a running game by
 *                   the player count)
 *   --features      also exercise what a lobby does: once everyone is in, rename our player,
 *                   set its remote data and send a chat line; the host also changes the
 *                   session description. Checks the notifications arrive and the Get* calls
 *                   return the new values (STAT features ...).
 *   --groups        also exercise groups (S3: what goes on the wire): once everyone is in, the host
 *                   creates a group, adds players, renames it, sets its data, creates a group in it,
 *                   adds a shortcut and removes it, removes a player and destroys the groups, one
 *                   step every 600 ms; a joiner creates, joins and destroys a group of its own.
 *                   Every group notification is logged (EVENT group ...), STAT groups counts them.
 *
 * Prints machine-readable lines: EVENT ..., STAT ..., RESULT ok|fail.
 */
#define INITGUID
#include <windows.h>
#include <dplay.h>
#include <dplobby.h>
#include <stdio.h>
#include <string.h>

DEFINE_GUID(OKP_SPGUID, 0x66d8dc59, 0x28e8, 0x4d3e, 0xba, 0x93, 0x2e, 0x98, 0xd2, 0x51, 0xc0, 0xf3);
DEFINE_GUID(OKP_LANGUID, 0x6bcff704, 0xce20, 0x4150, 0x98, 0xb4, 0x5a, 0x71, 0x98, 0x17, 0x5d, 0x7d);
/* {B3C1E0A2-6F1D-4B8E-9A2F-5D7C3E1A9B40} test application */
DEFINE_GUID(DPTEST_APP, 0xb3c1e0a2, 0x6f1d, 0x4b8e, 0x9a, 0x2f, 0x5d, 0x7c, 0x3e, 0x1a, 0x9b, 0x40);
/* {5DE93F3F-FC90-4EE1-AE5A-63DAFA055950} Age of Empires II: The Conquerors */
DEFINE_GUID(AOE2TC_APP, 0x5de93f3f, 0xfc90, 0x4ee1, 0xae, 0x5a, 0x63, 0xda, 0xfa, 0x05, 0x59, 0x50);
static GUID g_app;

#define MSG_PING 1
#define MSG_PONG 2
#define MSG_BURST 3
#define MSG_BUSY 4    /* --busy traffic: just counted as received */   /* --burst N: N guaranteed messages sent back to back, checked complete and in order */
typedef struct { DWORD type, seq, tick; } tmsg;
typedef struct { DWORD type, seq, tick; BYTE pad[988]; } bmsg;   /* 1000 bytes: one datagram each */
static int burst;
static int busy_from = -1, busy_secs;   /* --busy START:SECS - match-like traffic (~70 msgs/s) in that window */

typedef struct {
    DPID id;
    char name[64];
    DWORD pings_rx, pongs_rx, rtt_sum, rtt_max;
    DWORD last_pong;
    char orig[64];   /* name when first seen */
    DWORD burst_rx, burst_ooo, burst_last;
} remote;

static IDirectPlay4A *dp;
static int aoe2names;   /* --aoe2names: dp is really the WCHAR interface, fed ANSI bytes (see main) */
static int null_names;  /* player-created messages that carried a NULL short or long name */
static DPID me;
static remote rem[16];
static int nrem;
static DWORD t0, send_flags;

static void out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("[%6lu] ", (unsigned long)(GetTickCount() - t0));
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

static remote *rem_get(DPID id, const char *name)
{
    for (int i = 0; i < nrem; i++) if (rem[i].id == id) return &rem[i];
    if (nrem >= 16) return NULL;
    remote *r = &rem[nrem++];
    memset(r, 0, sizeof(*r));
    r->id = id;
    lstrcpynA(r->name, name ? name : "?", sizeof(r->name));
    if (!name && dp) {   /* already in the session when we joined: ask DirectPlay */
        char buf[160];
        DWORD sz = sizeof(buf);
        if (IDirectPlayX_GetPlayerName(dp, id, buf, &sz) == DP_OK && ((DPNAME *)buf)->lpszShortNameA)
            lstrcpynA(r->name, ((DPNAME *)buf)->lpszShortNameA, sizeof(r->name));
    }
    lstrcpynA(r->orig, r->name, sizeof(r->orig));
    return r;
}

static int fail(const char *what, HRESULT hr)
{
    out("ERROR %s: 0x%08lx", what, (unsigned long)hr);
    out("RESULT fail");
    return 1;
}

typedef struct { int found; DPSESSIONDESC2 sd; char name[128]; int count; } enum_ctx;
static char want_suffix[16];   /* --cycles: only the session of this cycle */
static char want_name[80];     /* --session NAME: only the session with exactly this name */

static BOOL FAR PASCAL enum_cb(LPCDPSESSIONDESC2 sd, LPDWORD timeout, DWORD flags, LPVOID ctx)
{
    (void)timeout;
    enum_ctx *e = (enum_ctx *)ctx;
    if (!(flags & DPESC_TIMEDOUT) && want_suffix[0]) {
        const char *n = sd->lpszSessionNameA ? sd->lpszSessionNameA : "";
        size_t ln = strlen(n), ls = strlen(want_suffix);
        if (ln < ls || strcmp(n + ln - ls, want_suffix) != 0) return TRUE;   /* an older cycle's session */
    }
    if (flags & DPESC_TIMEDOUT) return FALSE;
    e->count++;
    if (want_name[0] && strcmp(sd->lpszSessionNameA ? sd->lpszSessionNameA : "", want_name) != 0) {
        out("EVENT session \"%s\" (not the one we want)", sd->lpszSessionNameA ? sd->lpszSessionNameA : "");
        return TRUE;
    }
    out("EVENT session \"%s\" players %lu/%lu flags 0x%lx", sd->lpszSessionNameA ? sd->lpszSessionNameA : "",
        (unsigned long)sd->dwCurrentPlayers, (unsigned long)sd->dwMaxPlayers, (unsigned long)sd->dwFlags);
    if (!e->found) {
        e->found = 1;
        e->sd = *sd;
        lstrcpynA(e->name, sd->lpszSessionNameA ? sd->lpszSessionNameA : "", sizeof(e->name));
        e->sd.lpszSessionNameA = NULL;
        e->sd.lpszPasswordA = NULL;
    }
    return TRUE;
}

static int found_okplay, found_lan;
static int async_sends, n_complete, n_complete_ok;   /* --async: DPSEND_ASYNC and DPSYS_SENDCOMPLETE */
static DWORD async_hr_first, n_async_pending, n_async_ok, n_async_other;
static int features, n_desc, n_name, n_data, n_chat, desc_value_ok;
#define FEATURE_USER1 0xC0FFEE42
static int groups, n_grp_create, n_grp_destroy, n_grp_add, n_grp_del, n_grp_addgrp, n_grp_delgrp,
           n_grp_name, n_grp_data;

static BOOL FAR PASCAL conn_cb(LPCGUID sp, LPVOID conn, DWORD connsz, LPCDPNAME name, DWORD flags, LPVOID ctx)
{
    (void)conn; (void)connsz; (void)flags; (void)ctx;
    int ours = IsEqualGUID(sp, &OKP_SPGUID), lan = IsEqualGUID(sp, &OKP_LANGUID);
    if (ours) found_okplay = 1;
    if (lan) found_lan = 1;
    out("EVENT connection \"%s\"%s", name && name->lpszShortNameA ? name->lpszShortNameA : "?",
        ours ? " (okplay online)" : lan ? " (okplay lan)" : "");
    return TRUE;
}

static void handle_sys(DPMSG_GENERIC *g)
{
    switch (g->dwType) {
    case DPSYS_CREATEPLAYERORGROUP: {
        DPMSG_CREATEPLAYERORGROUP *m = (DPMSG_CREATEPLAYERORGROUP *)g;
        const char *nm = m->dpnName.lpszShortNameA ? m->dpnName.lpszShortNameA : "?";
        if (m->dwPlayerType == DPPLAYERTYPE_GROUP) {
            n_grp_create++;
            out("EVENT group created id=%lu name=%s parent=%lu flags=0x%lx data=%lu", (unsigned long)m->dpId, nm,
                (unsigned long)m->dpIdParent, (unsigned long)m->dwFlags, (unsigned long)m->dwDataSize);
            break;
        }
        if (!m->dpnName.lpszShortNameA || !m->dpnName.lpszLongNameA) {
            /* AoE2 copies both names with no NULL check: a NULL here crashes a real AoE2 host */
            null_names++;
            out("EVENT null_name id=%lu short=%s long=%s", (unsigned long)m->dpId,
                m->dpnName.lpszShortNameA ? "set" : "NULL", m->dpnName.lpszLongNameA ? "set" : "NULL");
        }
        rem_get(m->dpId, nm);
        out("EVENT player_joined id=%lu name=%s", (unsigned long)m->dpId, nm);
        break;
    }
    case DPSYS_DESTROYPLAYERORGROUP: {
        DPMSG_DESTROYPLAYERORGROUP *m = (DPMSG_DESTROYPLAYERORGROUP *)g;
        if (m->dwPlayerType == DPPLAYERTYPE_GROUP) {
            n_grp_destroy++;
            out("EVENT group destroyed id=%lu parent=%lu", (unsigned long)m->dpId, (unsigned long)m->dpIdParent);
            break;
        }
        out("EVENT player_left id=%lu", (unsigned long)m->dpId);
        break;
    }
    case DPSYS_ADDPLAYERTOGROUP: {
        DPMSG_ADDPLAYERTOGROUP *m = (DPMSG_ADDPLAYERTOGROUP *)g;
        n_grp_add++;
        out("EVENT group add_player group=%lu player=%lu", (unsigned long)m->dpIdGroup, (unsigned long)m->dpIdPlayer);
        break;
    }
    case DPSYS_DELETEPLAYERFROMGROUP: {
        DPMSG_DELETEPLAYERFROMGROUP *m = (DPMSG_DELETEPLAYERFROMGROUP *)g;
        n_grp_del++;
        out("EVENT group delete_player group=%lu player=%lu", (unsigned long)m->dpIdGroup, (unsigned long)m->dpIdPlayer);
        break;
    }
    case DPSYS_ADDGROUPTOGROUP: {
        DPMSG_ADDGROUPTOGROUP *m = (DPMSG_ADDGROUPTOGROUP *)g;
        n_grp_addgrp++;
        out("EVENT group add_group parent=%lu group=%lu", (unsigned long)m->dpIdParentGroup, (unsigned long)m->dpIdGroup);
        break;
    }
    case DPSYS_DELETEGROUPFROMGROUP: {
        DPMSG_DELETEGROUPFROMGROUP *m = (DPMSG_DELETEGROUPFROMGROUP *)g;
        n_grp_delgrp++;
        out("EVENT group delete_group parent=%lu group=%lu", (unsigned long)m->dpIdParentGroup, (unsigned long)m->dpIdGroup);
        break;
    }
    case DPSYS_SETSESSIONDESC: {
        DPMSG_SETSESSIONDESC *m = (DPMSG_SETSESSIONDESC *)g;
        n_desc++;
        if (m->dpDesc.dwUser1 == FEATURE_USER1) desc_value_ok = 1;
        out("EVENT session_desc_changed user1=0x%lx", (unsigned long)m->dpDesc.dwUser1);
        break;
    }
    case DPSYS_SETPLAYERORGROUPNAME: {
        DPMSG_SETPLAYERORGROUPNAME *m = (DPMSG_SETPLAYERORGROUPNAME *)g;
        if (m->dwPlayerType == DPPLAYERTYPE_GROUP) {
            n_grp_name++;
            out("EVENT group name id=%lu name=%s", (unsigned long)m->dpId,
                m->dpnName.lpszShortNameA ? m->dpnName.lpszShortNameA : "?");
            break;
        }
        n_name++;
        out("EVENT name_changed id=%lu name=%s", (unsigned long)m->dpId,
            m->dpnName.lpszShortNameA ? m->dpnName.lpszShortNameA : "?");
        break;
    }
    case DPSYS_SETPLAYERORGROUPDATA: {
        DPMSG_SETPLAYERORGROUPDATA *m = (DPMSG_SETPLAYERORGROUPDATA *)g;
        if (m->dwPlayerType == DPPLAYERTYPE_GROUP) {
            n_grp_data++;
            out("EVENT group data id=%lu size=%lu", (unsigned long)m->dpId, (unsigned long)m->dwDataSize);
            break;
        }
        n_data++;
        out("EVENT data_changed id=%lu size=%lu", (unsigned long)m->dpId, (unsigned long)m->dwDataSize);
        break;
    }
    case DPSYS_CHAT: {
        DPMSG_CHAT *m = (DPMSG_CHAT *)g;
        n_chat++;
        out("EVENT chat from=%lu text=\"%s\"", (unsigned long)m->idFromPlayer,
            m->lpChat && m->lpChat->lpszMessageA ? m->lpChat->lpszMessageA : "?");
        break;
    }
    case DPSYS_HOST:        out("EVENT became_host"); break;
    case DPSYS_SESSIONLOST: out("EVENT session_lost"); break;
    case DPSYS_SENDCOMPLETE: {
        DPMSG_SENDCOMPLETE *m = (DPMSG_SENDCOMPLETE *)g;
        n_complete++;
        if (m->hr == DP_OK) n_complete_ok++;
        if (n_complete <= 3)
            out("EVENT send_complete from=%lu to=%lu flags=0x%lx prio=%lu timeout=%lu ctx=%p msgid=%lu hr=0x%08lx ms=%lu",
                (unsigned long)m->idFrom, (unsigned long)m->idTo, (unsigned long)m->dwFlags,
                (unsigned long)m->dwPriority, (unsigned long)m->dwTimeout, m->lpvContext, (unsigned long)m->dwMsgID,
                (unsigned long)m->hr, (unsigned long)m->dwSendTime);
        break;
    }
    default:                out("EVENT sysmsg 0x%lx", (unsigned long)g->dwType); break;
    }
}

static void pump(void)
{
    static BYTE *buf;
    static DWORD cap = 4096;
    if (!buf) buf = (BYTE *)malloc(cap);
    for (;;) {
        DPID from = 0, to = 0;
        DWORD size = cap;
        HRESULT hr = IDirectPlayX_Receive(dp, &from, &to, DPRECEIVE_ALL, buf, &size);
        if (hr == DPERR_BUFFERTOOSMALL) { cap = size; buf = (BYTE *)realloc(buf, cap); continue; }
        if (hr != DP_OK) break;
        if (from == DPID_SYSMSG) { handle_sys((DPMSG_GENERIC *)buf); continue; }
        if (size < sizeof(tmsg)) continue;
        tmsg *m = (tmsg *)buf;
        remote *r = rem_get(from, NULL);
        if (!r) continue;
        if (m->type == MSG_PING) {
            r->pings_rx++;
            tmsg p = {MSG_PONG, m->seq, m->tick};
            IDirectPlayX_Send(dp, me, from, send_flags, &p, sizeof(p));
        } else if (m->type == MSG_BURST) {
            if (m->seq != r->burst_last + 1) r->burst_ooo++;
            r->burst_last = m->seq;
            r->burst_rx++;
        } else if (m->type == MSG_PONG) {
            DWORD rtt = GetTickCount() - m->tick;
            r->pongs_rx++;
            r->last_pong = GetTickCount();
            r->rtt_sum += rtt;
            if (rtt > r->rtt_max) r->rtt_max = rtt;
        }
    }
}

/* Windows' registered dplayx through COM, or a given dplayx.dll through its own class factory */
static HRESULT create_dp(const char *dll, IDirectPlay4A **pdp, IDirectPlayLobby3A **plob)
{
    HRESULT hr;
    if (!dll || !*dll) {
        hr = CoCreateInstance(&CLSID_DirectPlay, NULL, CLSCTX_INPROC_SERVER, &IID_IDirectPlay4A, (void **)pdp);
        if (FAILED(hr)) return hr;
        return CoCreateInstance(&CLSID_DirectPlayLobby, NULL, CLSCTX_INPROC_SERVER, &IID_IDirectPlayLobby3A, (void **)plob);
    }
    HMODULE m = LoadLibraryA(dll);
    if (!m) return HRESULT_FROM_WIN32(GetLastError());
    typedef HRESULT (WINAPI *gco_t)(REFCLSID, REFIID, void **);
    gco_t gco = (gco_t)GetProcAddress(m, "DllGetClassObject");
    if (!gco) return E_NOINTERFACE;
    IClassFactory *cf;
    if (FAILED(hr = gco(&CLSID_DirectPlay, &IID_IClassFactory, (void **)&cf))) return hr;
    hr = cf->lpVtbl->CreateInstance(cf, NULL, &IID_IDirectPlay4A, (void **)pdp);
    cf->lpVtbl->Release(cf);
    if (FAILED(hr)) return hr;
    if (FAILED(hr = gco(&CLSID_DirectPlayLobby, &IID_IClassFactory, (void **)&cf))) return hr;
    hr = cf->lpVtbl->CreateInstance(cf, NULL, &IID_IDirectPlayLobby3A, (void **)plob);
    cf->lpVtbl->Release(cf);
    return hr;
}

/* which DLL really implements this object (proves --dplayx took effect) */
static const char *impl_of(void *obj)
{
    static char path[MAX_PATH];
    HMODULE m = NULL;
    path[0] = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)*(void **)*(void **)obj, &m))
        GetModuleFileNameA(m, path, sizeof(path));
    return path[0] ? path : "?";
}

/* the lobby-style changes (see --features) */
static void feature_actions(int host, const char *name)
{
    char nm[80], data[80], text[80];
    snprintf(nm, sizeof(nm), "%s2", name);
    snprintf(data, sizeof(data), "pdata-%s", name);
    snprintf(text, sizeof(text), "chat from %s", name);
    DPNAME pn;
    memset(&pn, 0, sizeof(pn));
    pn.dwSize = sizeof(pn);
    pn.lpszShortNameA = nm;
    HRESULT h1 = IDirectPlayX_SetPlayerName(dp, me, &pn, DPSET_REMOTE);
    HRESULT h2 = IDirectPlayX_SetPlayerData(dp, me, data, (DWORD)strlen(data) + 1, DPSET_REMOTE | DPSET_GUARANTEED);
    DPCHAT chat;
    memset(&chat, 0, sizeof(chat));
    chat.dwSize = sizeof(chat);
    chat.lpszMessageA = text;
    HRESULT h3 = IDirectPlayX_SendChatMessage(dp, me, DPID_ALLPLAYERS, 0, &chat);
    HRESULT h4 = DP_OK;
    if (host) {
        DWORD sz = 0;
        IDirectPlayX_GetSessionDesc(dp, NULL, &sz);
        DPSESSIONDESC2 *sd = (DPSESSIONDESC2 *)malloc(sz);
        if (sd && IDirectPlayX_GetSessionDesc(dp, sd, &sz) == DP_OK) {
            sd->dwUser1 = FEATURE_USER1;
            h4 = IDirectPlayX_SetSessionDesc(dp, sd, 0);
        }
        free(sd);
    }
    out("EVENT features_sent name=0x%lx data=0x%lx chat=0x%lx desc=0x%lx", (unsigned long)h1,
        (unsigned long)h2, (unsigned long)h3, (unsigned long)h4);
}

/* --groups: one step per call; returns 0 when this side has nothing more to do */
static BOOL FAR PASCAL count_groups_cb(DPID id, DWORD type, LPCDPNAME name, DWORD flags, LPVOID ctx)
{
    (void)id; (void)type; (void)name; (void)flags;
    (*(int *)ctx)++;
    return TRUE;
}

static int group_step(int host, int step)
{
    static DPID g1, g2, g3, gj;
    DPNAME pn;
    HRESULT hr = DP_OK;
    const char *what = NULL;
    DPID other = nrem > 0 ? rem[0].id : 0;
    memset(&pn, 0, sizeof(pn));
    pn.dwSize = sizeof(pn);
    if (!host) {
        switch (step) {
        case 0: pn.lpszShortNameA = (LPSTR)"joinergroup"; hr = IDirectPlayX_CreateGroup(dp, &gj, &pn, NULL, 0, 0); what = "CreateGroup(joinergroup)"; break;
        case 1: hr = IDirectPlayX_AddPlayerToGroup(dp, gj, me); what = "AddPlayerToGroup(joinergroup, me)"; break;
        case 2: hr = IDirectPlayX_DestroyGroup(dp, gj); what = "DestroyGroup(joinergroup)"; break;
        default: return 0;
        }
    } else {
        switch (step) {
        case 0: pn.lpszShortNameA = (LPSTR)"grpA"; hr = IDirectPlayX_CreateGroup(dp, &g1, &pn, (void *)"gdata", 6, 0); what = "CreateGroup(grpA, 6 bytes)"; break;
        case 1: hr = IDirectPlayX_AddPlayerToGroup(dp, g1, me); what = "AddPlayerToGroup(grpA, me)"; break;
        case 2: hr = IDirectPlayX_AddPlayerToGroup(dp, g1, other); what = "AddPlayerToGroup(grpA, remote)"; break;
        case 3: pn.lpszShortNameA = (LPSTR)"grpA2"; hr = IDirectPlayX_SetGroupName(dp, g1, &pn, DPSET_REMOTE); what = "SetGroupName(grpA2)"; break;
        case 4: hr = IDirectPlayX_SetGroupData(dp, g1, (void *)"gdata22", 8, DPSET_REMOTE | DPSET_GUARANTEED); what = "SetGroupData(8 bytes)"; break;
        case 5: pn.lpszShortNameA = (LPSTR)"sub"; hr = IDirectPlayX_CreateGroupInGroup(dp, g1, &g2, &pn, NULL, 0, 0); what = "CreateGroupInGroup(grpA, sub)"; break;
        case 6: pn.lpszShortNameA = (LPSTR)"grpB"; hr = IDirectPlayX_CreateGroup(dp, &g3, &pn, NULL, 0, 0); what = "CreateGroup(grpB)"; break;
        case 7: hr = IDirectPlayX_AddGroupToGroup(dp, g3, g1); what = "AddGroupToGroup(grpB, grpA)"; break;
        case 8: hr = IDirectPlayX_DeleteGroupFromGroup(dp, g3, g1); what = "DeleteGroupFromGroup(grpB, grpA)"; break;
        case 9: hr = IDirectPlayX_DeletePlayerFromGroup(dp, g1, other); what = "DeletePlayerFromGroup(grpA, remote)"; break;
        case 10: hr = IDirectPlayX_DestroyGroup(dp, g2); what = "DestroyGroup(sub)"; break;
        case 11: hr = IDirectPlayX_DestroyGroup(dp, g1); what = "DestroyGroup(grpA)"; break;
        case 12: hr = IDirectPlayX_DestroyGroup(dp, g3); what = "DestroyGroup(grpB)"; break;
        default: return 0;
        }
    }
    out("EVENT group_call %s -> 0x%08lx", what, (unsigned long)hr);
    return 1;
}

static void group_stat(void)
{
    int n = 0;
    IDirectPlayX_EnumGroups(dp, NULL, count_groups_cb, &n, DPENUMGROUPS_ALL);
    out("STAT groups created=%d destroyed=%d add_player=%d delete_player=%d add_group=%d delete_group=%d name=%d data=%d groups_left=%d",
        n_grp_create, n_grp_destroy, n_grp_add, n_grp_del, n_grp_addgrp, n_grp_delgrp, n_grp_name, n_grp_data, n);
}

/* after everyone's changes: do the Get* calls agree? */
static int feature_check(int host)
{
    int names_ok = 0, data_ok = 0, n = 0;
    for (int i = 0; i < nrem; i++) {
        remote *r = &rem[i];
        char want[80], got[160];
        DWORD sz = sizeof(got);
        n++;
        if (IDirectPlayX_GetPlayerName(dp, r->id, got, &sz) == DP_OK) {
            DPNAME *pn = (DPNAME *)got;
            snprintf(want, sizeof(want), "%s2", r->orig);
            if (pn->lpszShortNameA && !strcmp(pn->lpszShortNameA, want)) names_ok++;
            else out("EVENT feature_name_mismatch id=%lu got=%s want=%s", (unsigned long)r->id,
                     pn->lpszShortNameA ? pn->lpszShortNameA : "(null)", want);
        }
        sz = sizeof(got);
        if (IDirectPlayX_GetPlayerData(dp, r->id, got, &sz, DPGET_REMOTE) == DP_OK) {
            snprintf(want, sizeof(want), "pdata-%s", r->orig);
            if (sz == strlen(want) + 1 && !memcmp(got, want, sz)) data_ok++;
            else out("EVENT feature_data_mismatch id=%lu size=%lu", (unsigned long)r->id, (unsigned long)sz);
        }
    }
    if (!host) {
        DWORD sz = 0;
        IDirectPlayX_GetSessionDesc(dp, NULL, &sz);
        DPSESSIONDESC2 *sd = (DPSESSIONDESC2 *)malloc(sz);
        if (sd && IDirectPlayX_GetSessionDesc(dp, sd, &sz) == DP_OK && sd->dwUser1 == FEATURE_USER1) desc_value_ok |= 2;
        free(sd);
    }
    int ok = n > 0 && names_ok == n && data_ok == n && n_chat >= n && n_name >= n && n_data >= n &&
             (host || (n_desc >= 1 && (desc_value_ok & 2)));
    out("STAT features remotes=%d names_ok=%d data_ok=%d name_msgs=%d data_msgs=%d chat_msgs=%d desc_msgs=%d desc_now=%s -> %s",
        n, names_ok, data_ok, n_name, n_data, n_chat, n_desc, host ? "(host)" : (desc_value_ok & 2) ? "new" : "OLD",
        ok ? "ok" : "FAIL");
    return ok;
}

static int count_players(void)
{
    /* dplayx's own view: every player in the session including us */
    DWORD n = 0;
    DPSESSIONDESC2 *sd = NULL;
    DWORD sz = 0;
    IDirectPlayX_GetSessionDesc(dp, NULL, &sz);
    sd = (DPSESSIONDESC2 *)malloc(sz);
    if (sd && IDirectPlayX_GetSessionDesc(dp, sd, &sz) == DP_OK) n = sd->dwCurrentPlayers;
    free(sd);
    return (int)n;
}

/* a crash prints where it happened (module + offset; addr2line -e <dll> resolves our own DLLs) */
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    HMODULE m = NULL;
    char name[MAX_PATH] = "?";
    void *at = ep->ExceptionRecord->ExceptionAddress;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)at, &m))
        GetModuleFileNameA(m, name, sizeof(name));
    out("CRASH code 0x%08lx at %s+0x%lx", (unsigned long)ep->ExceptionRecord->ExceptionCode, name,
        (unsigned long)((char *)at - (char *)m));
    DWORD *f = (DWORD *)ep->ContextRecord->Ebp;   /* frame-pointer walk: our DLLs keep frame pointers */
    for (int i = 0; i < 12 && f && !IsBadReadPtr(f, 8); i++) {
        void *ret = (void *)f[1];
        HMODULE rm = NULL;
        char rn[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)ret, &rm))
            GetModuleFileNameA(rm, rn, sizeof(rn));
        out("CRASH   called from %s+0x%lx", rn, (unsigned long)((char *)ret - (char *)rm));
        f = (DWORD *)f[0];
    }
    out("RESULT fail");
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char **argv)
{
    const char *sp = "okplay", *ip = "", *name = "tester", *mode = NULL, *app = "test";
    const char *dplayx = getenv("DPTEST_DPLAYX");
    DWORD sflags = getenv("DPTEST_SFLAGS") ? strtoul(getenv("DPTEST_SFLAGS"), NULL, 0)
                                            : DPSESSION_KEEPALIVE | DPSESSION_MIGRATEHOST;
    if (getenv("DPTEST_GUARANTEED") && atoi(getenv("DPTEST_GUARANTEED"))) send_flags = DPSEND_GUARANTEED;
    /* the drop-in asks a host for a game password in a box; tests host open games unless told
       otherwise (OKP_TEST_HOST_PASSWORD=<text>; joiners answer with OKP_TEST_JOIN_PASSWORD) */
    if (!getenv("OKP_TEST_HOST_PASSWORD")) _putenv("OKP_TEST_HOST_PASSWORD=<none>");
    /* nor do tests answer the report boxes (0.8.3): no connection reports, no crash reports sent,
       unless a test asks (OKP_TEST_CONN_REPORTS=yes, OKP_TEST_CRASH_REPORT=send) */
    if (!getenv("OKP_TEST_CONN_REPORTS")) _putenv("OKP_TEST_CONN_REPORTS=no");
    if (!getenv("OKP_TEST_CRASH_REPORT")) _putenv("OKP_TEST_CRASH_REPORT=dont");
    DWORD eflags = DPENUMSESSIONS_AVAILABLE;   /* --eflags: EnumSessions flags (0x2 = DPENUMSESSIONS_ALL) */
    int secs = 10, expect = 0, cycles = 1, nosend = 0, nolongname = 0, retries = 0, extra = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sp") && i + 1 < argc) sp = argv[++i];
        else if (!strcmp(argv[i], "--ip") && i + 1 < argc) ip = argv[++i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--secs") && i + 1 < argc) secs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--expect") && i + 1 < argc) expect = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--app") && i + 1 < argc) app = argv[++i];
        else if (!strcmp(argv[i], "--dplayx") && i + 1 < argc) dplayx = argv[++i];
        else if (!strcmp(argv[i], "--sflags") && i + 1 < argc) sflags = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--eflags") && i + 1 < argc) eflags = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--guaranteed")) send_flags = DPSEND_GUARANTEED;
        else if (!strcmp(argv[i], "--async")) async_sends = 1;
        else if (!strcmp(argv[i], "--burst") && i + 1 < argc) burst = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--busy") && i + 1 < argc) sscanf(argv[++i], "%d:%d", &busy_from, &busy_secs);
        else if (!strcmp(argv[i], "--features")) features = 1;
        else if (!strcmp(argv[i], "--groups")) groups = 1;
        else if (!strcmp(argv[i], "--cycles") && i + 1 < argc) cycles = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--nosend")) nosend = 1;
        else if (!strcmp(argv[i], "--extra")) extra = 1;
        else if (!strcmp(argv[i], "--session") && i + 1 < argc) lstrcpynA(want_name, argv[++i], sizeof(want_name));
        else if (!strcmp(argv[i], "--aoe2names")) aoe2names = 1;
        else if (!strcmp(argv[i], "--nolongname")) nolongname = 1;
        else if (!strcmp(argv[i], "--retry") && i + 1 < argc) retries = atoi(argv[++i]);
        else mode = argv[i];
    }
    if (!mode) {
        fprintf(stderr, "usage: dptest [--sp okplay|tcpip] [--ip HOST] [--name N] [--secs S] [--expect P]\n"
                        "              [--app test|aoe2|any] [--nosend] [--eflags X] host|join|list|conns\n");
        return 2;
    }
    if (!strcmp(app, "aoe2")) g_app = AOE2TC_APP;
    else if (!strcmp(app, "any")) memset(&g_app, 0, sizeof(g_app));
    else g_app = DPTEST_APP;
    /* a fake player inside a real AoE2 game could upset the host: only look, or (--nosend) join
       a lobby silently - DirectPlay's own messages only, never our pings */
    if (strcmp(app, "test") != 0 && strcmp(mode, "list") != 0 &&
        !((!strcmp(app, "aoe2") || !strcmp(app, "any")) && !strcmp(mode, "join") && nosend)) {
        fprintf(stderr, "--app %s is only allowed with 'list' (or 'join --app aoe2|any --nosend')\n", app);
        return 2;
    }
    t0 = GetTickCount();
    SetUnhandledExceptionFilter(crash_filter);
    CoInitialize(NULL);

    IDirectPlayLobby3A *lob = NULL;
    HRESULT hr = create_dp(dplayx, &dp, &lob);
    if (FAILED(hr)) return fail(dplayx && *dplayx ? "creating DirectPlay from --dplayx"
                                                : "CoCreateInstance(DirectPlay) - is the DirectPlay feature enabled?", hr);
    out("EVENT dplayx %s", impl_of(dp));
    if (aoe2names) {
        /* do what AoE2 does: call the WCHAR interface (IDirectPlay4) but hand it ANSI bytes, and
           read every string it returns as bytes. Windows copies WCHAR strings byte for byte, so
           the names arrive intact; anything that converts them shows up here as '?' or garbage.
           The vtables have the same layout, so every IDirectPlayX_ call below goes through it. */
        IDirectPlay4 *w = NULL;
        hr = IDirectPlayX_QueryInterface(dp, &IID_IDirectPlay4, (void **)&w);
        if (FAILED(hr)) return fail("QueryInterface(IDirectPlay4)", hr);
        dp = (IDirectPlay4A *)w;
        out("EVENT aoe2names: WCHAR interface, ANSI byte names");
    }

    if (!strcmp(mode, "conns")) {
        /* the same enumeration AoE2's multiplayer screen uses to fill its connection list */
        hr = IDirectPlayX_EnumConnections(dp, NULL, conn_cb, NULL, DPCONNECTION_DIRECTPLAY);
        if (FAILED(hr)) return fail("EnumConnections", hr);
        out("STAT online_registered=%d lan_registered=%d", found_okplay, found_lan);
        out("RESULT %s", found_okplay ? "ok" : "fail");
        return found_okplay ? 0 : 1;
    }

    int tcpip = !strcmp(sp, "tcpip");
    /* okplay = registered Online Play, lan = registered LAN Play,
     * ipx = the dev-loop slot (bin\dpwsockx.dll + OKP_TEST_AS=online|lan) */
    /* auto = the drop-in's built-in connections, Online or LAN as OKP_TEST_AS says (the same
     * scenario scripts drive both editions) */
    if (!strcmp(sp, "auto")) {
        const char *as = getenv("OKP_TEST_AS");
        sp = as && !strcmp(as, "lan") ? "lan" : "okplay";
    }
    const GUID *spguid = tcpip ? &DPSPGUID_TCPIP : !strcmp(sp, "ipx") ? &DPSPGUID_IPX
                       : !strcmp(sp, "lan") ? &OKP_LANGUID : &OKP_SPGUID;
    DPCOMPOUNDADDRESSELEMENT el[2];
    DWORD nel = 0;
    el[nel].guidDataType = DPAID_ServiceProvider;
    el[nel].dwDataSize = sizeof(GUID);
    el[nel].lpData = (LPVOID)spguid;
    nel++;
    if (tcpip) {
        el[nel].guidDataType = DPAID_INet;
        el[nel].dwDataSize = (DWORD)strlen(ip) + 1;
        el[nel].lpData = (LPVOID)ip;
        nel++;
    }
    DWORD asz = 0;
    IDirectPlayLobby_CreateCompoundAddress(lob, el, nel, NULL, &asz);
    void *addr = malloc(asz);
    hr = IDirectPlayLobby_CreateCompoundAddress(lob, el, nel, addr, &asz);
    if (FAILED(hr)) return fail("CreateCompoundAddress", hr);
    hr = IDirectPlayX_InitializeConnection(dp, addr, 0);
    if (FAILED(hr)) return fail("InitializeConnection - is the provider registered?", hr);
    out("EVENT connection_ready sp=%s", sp);

    int all_ok = 1;
    for (int cyc = 1; cyc <= (cycles < 1 ? 1 : cycles); cyc++) {
    char sessname[96];
    if (cycles > 1) {
        snprintf(sessname, sizeof(sessname), "dptest %s#%d", name, cyc);
        snprintf(want_suffix, sizeof(want_suffix), "#%d", cyc);
        nrem = 0;
        memset(rem, 0, sizeof(rem));
        out("EVENT cycle %d", cyc);
    } else
        snprintf(sessname, sizeof(sessname), "dptest %s", name);

    int names_bad = 0;   /* --aoe2names: a name did not arrive byte for byte */
    if (!strcmp(mode, "host")) {
        DPSESSIONDESC2 sd;
        memset(&sd, 0, sizeof(sd));
        sd.dwSize = sizeof(sd);
        sd.dwFlags = sflags;
        sd.guidApplication = g_app;
        sd.dwMaxPlayers = 8;
        sd.lpszSessionNameA = sessname;
        char aoe2sess[160];
        if (aoe2names) {
            /* AoE2's own layout: "<game>\0[<host>]\0" and a WCHAR terminator; it also opens with
               dwCurrentPlayers = 1 (DirectPlay must keep its own count) */
            memset(aoe2sess, 0, sizeof(aoe2sess));
            int n = snprintf(aoe2sess, sizeof(aoe2sess) - 4, "%s", sessname);
            snprintf(aoe2sess + n + 1, sizeof(aoe2sess) - n - 5, "[%s]", name);
            sd.lpszSessionNameA = aoe2sess;
            sd.dwCurrentPlayers = 1;
        }
        hr = IDirectPlayX_Open(dp, &sd, DPOPEN_CREATE);
        if (FAILED(hr)) return fail("Open(create)", hr);
        out("EVENT hosting \"%s\" flags 0x%lx", sessname, (unsigned long)sflags);
    } else {
        enum_ctx e;
        memset(&e, 0, sizeof(e));
        DPSESSIONDESC2 sd;
        memset(&sd, 0, sizeof(sd));
        sd.dwSize = sizeof(sd);
        sd.guidApplication = g_app;
        DWORD until = GetTickCount() + secs * 1000;
        while (!e.found && GetTickCount() < until) {
            hr = IDirectPlayX_EnumSessions(dp, &sd, 1500, enum_cb, &e, eflags);
            if (FAILED(hr)) return fail("EnumSessions", hr);
        }
        if (!strcmp(mode, "list")) {
            out("RESULT %s sessions=%d", e.count ? "ok" : "fail", e.count);
            return e.count ? 0 : 1;
        }
        if (!e.found) { out("ERROR no session found"); out("RESULT fail"); return 1; }
        DWORD tj = GetTickCount();
        hr = IDirectPlayX_Open(dp, &e.sd, DPOPEN_JOIN);
        /* --retry N: after a failed join, list again and join again with the same DirectPlay object,
           as a player does who clicks Join a second time (a refused attempt once made that hang) */
        for (int attempt = 0; FAILED(hr) && attempt < retries; attempt++) {
            out("EVENT join_failed 0x%08lx - joining again", (unsigned long)hr);
            Sleep(1000);
            memset(&e, 0, sizeof(e));
            DWORD until2 = GetTickCount() + secs * 1000;
            while (!e.found && GetTickCount() < until2) {
                hr = IDirectPlayX_EnumSessions(dp, &sd, 1500, enum_cb, &e, eflags);
                if (FAILED(hr)) return fail("EnumSessions", hr);
            }
            if (!e.found) { out("ERROR no session found"); out("RESULT fail"); return 1; }
            tj = GetTickCount();
            hr = IDirectPlayX_Open(dp, &e.sd, DPOPEN_JOIN);
        }
        if (FAILED(hr)) return fail("Open(join)", hr);
        out("EVENT joined \"%s\" in %lums", e.name, (unsigned long)(GetTickCount() - tj));
        if (aoe2names && (!e.name[0] || strchr(e.name, '?'))) {   /* any host: dptest's or AoE2's */
            out("STAT aoe2names session name garbled: \"%s\"", e.name);
            names_bad = 1;
        }
    }

    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    DPNAME pn;
    memset(&pn, 0, sizeof(pn));
    pn.dwSize = sizeof(pn);
    char pname[80];   /* zero-filled: ends in a WCHAR terminator too (--aoe2names) */
    memset(pname, 0, sizeof(pname));
    lstrcpynA(pname, name, sizeof(pname) - 4);
    pn.lpszShortNameA = pname;
    /* a long name too, as games do: AoE2 copies both without a NULL check (a joiner without
       one crashed a real AoE2 host at age2_x1.5.exe+0x1db37f, 2026-09-28) */
    pn.lpszLongNameA = nolongname ? NULL : pname;   /* --nolongname: what crashed the AoE2 host */
    hr = IDirectPlayX_CreatePlayer(dp, &me, &pn, ev, NULL, 0, 0);
    if (FAILED(hr)) return fail("CreatePlayer", hr);
    out("EVENT my_player id=%lu", (unsigned long)me);
    if (extra) {
        DPID me2;
        hr = IDirectPlayX_CreatePlayer(dp, &me2, &pn, ev, NULL, 0, 0);
        if (FAILED(hr)) return fail("CreatePlayer (--extra)", hr);
        out("EVENT extra_player id=%lu", (unsigned long)me2);
    }

    /* seed the remote list with players already in the session */
    DWORD seq = 0, last_ping = 0, end = GetTickCount() + secs * 1000;
    int reached = expect == 0, max_players = 1;
    int is_host = !strcmp(mode, "host");
    DWORD feature_at = 0, feature_done = 0, burst_at = 0, burst_fail = 0;
    int feature_result = -1;   /* checked while everyone is still here */
    DWORD group_next = 0;
    int group_n = 0, group_done = 0;
    while (GetTickCount() < end) {
        WaitForSingleObject(ev, 20);
        pump();
        int np = count_players();
        if (np > max_players) max_players = np;
        if (!nosend && GetTickCount() - last_ping >= 200) {
            tmsg p = {MSG_PING, ++seq, GetTickCount()};
            if (async_sends) {
                DWORD msgid = 0;
                HRESULT ah = IDirectPlayX_SendEx(dp, me, DPID_ALLPLAYERS, send_flags | DPSEND_ASYNC, &p, sizeof(p),
                                                 0, 0, (LPVOID)(ULONG_PTR)seq, &msgid);
                if (seq == 1) {
                    async_hr_first = (DWORD)ah;
                    out("EVENT async_send hr=0x%08lx msgid=%lu", (unsigned long)ah, (unsigned long)msgid);
                }
                if (ah == DPERR_PENDING) n_async_pending++;
                else if (ah == DP_OK) n_async_ok++;
                else n_async_other++;
            } else
                IDirectPlayX_Send(dp, me, DPID_ALLPLAYERS, send_flags, &p, sizeof(p));
            last_ping = GetTickCount();
        }
        /* --burst: once someone else is here, everything at once (fills okplay's reliable window) */
        if (burst && !burst_at && np >= 2) burst_at = GetTickCount() + 1500;
        if (burst && burst_at && burst_at != 1 && GetTickCount() >= burst_at) {
            bmsg b;
            memset(&b, 0, sizeof(b));
            b.type = MSG_BURST;
            for (int k = 1; k <= burst; k++) {
                b.seq = k;
                b.tick = GetTickCount();
                if (IDirectPlayX_Send(dp, me, DPID_ALLPLAYERS, DPSEND_GUARANTEED, &b, sizeof(b)) != DP_OK) burst_fail++;
            }
            out("EVENT burst_sent n=%d send_failures=%lu", burst, (unsigned long)burst_fail);
            burst_at = 1;
        }
        if (busy_from >= 0 && np >= 2) {   /* --busy: a match's worth of small game messages */
            static DWORD busy_t0, busy_sent;
            DWORD t = GetTickCount();
            if (!busy_t0) busy_t0 = t;
            if (t - busy_t0 >= (DWORD)busy_from * 1000 && t - busy_t0 < (DWORD)(busy_from + busy_secs) * 1000) {
                tmsg m = {MSG_BUSY, ++busy_sent, t};
                for (int k = 0; k < 2; k++) IDirectPlayX_Send(dp, me, DPID_ALLPLAYERS, 0, &m, sizeof(m));
                if (busy_sent == 1) out("EVENT busy_start");
            } else if (busy_sent && t - busy_t0 >= (DWORD)(busy_from + busy_secs) * 1000 && busy_sent != (DWORD)-1) {
                out("EVENT busy_end sent=%lu", (unsigned long)busy_sent * 2);
                busy_sent = (DWORD)-1;
            }
        }
        if (!reached && np >= expect) {
            reached = 1;
            out("EVENT expected_players_reached %d", expect);
        }
        /* host: once everyone is in; joiners: a little after joining */
        if (features && !feature_at && reached && (!is_host || expect))   /* everyone is in */
            feature_at = GetTickCount() + 1500 + (is_host ? 0 : 1000);
        if (features && feature_at && !feature_done && GetTickCount() >= feature_at) {
            feature_actions(is_host, name);
            feature_done = GetTickCount();
        }
        if (features && feature_done && feature_result < 0 && GetTickCount() - feature_done >= 4000)
            feature_result = feature_check(is_host);
        /* --groups: the host's steps start once everyone is in, a joiner's 3 s after it joined */
        if (groups && !group_next && reached && (!is_host || expect))
            group_next = GetTickCount() + (is_host ? 1500 : 3000);
        if (groups && group_next && !group_done && GetTickCount() >= group_next) {
            if (group_step(is_host, group_n++)) group_next = GetTickCount() + 600;
            else group_done = 1;
        }
    }

    if (groups) group_stat();
    int ok = reached, with_pongs = 0;
    for (int i = 0; i < nrem; i++) {
        remote *r = &rem[i];
        out("STAT peer id=%lu name=%s pings_rx=%lu pongs_rx=%lu rtt_avg=%lu rtt_max=%lu last_pong_ago=%ld", (unsigned long)r->id,
            r->name, (unsigned long)r->pings_rx, (unsigned long)r->pongs_rx,
            (unsigned long)(r->pongs_rx ? r->rtt_sum / r->pongs_rx : 0), (unsigned long)r->rtt_max,
            r->last_pong ? (long)(GetTickCount() - r->last_pong) : -1L);
        if (r->pongs_rx > 0) with_pongs++;
        if (burst) {
            out("STAT burst from=%s rx=%lu/%d out_of_order=%lu", r->name, (unsigned long)r->burst_rx, burst,
                (unsigned long)r->burst_ooo);
            if (r->burst_rx != (DWORD)burst || r->burst_ooo) ok = 0;
        }
    }
    if (burst && (burst_at != 1 || burst_fail)) ok = 0;
    int others = max_players - 1;   /* judge on the peak: peers may leave before we finish */
    out("STAT sent_pings=%lu max_players=%d peers_answering=%d", (unsigned long)seq, max_players, with_pongs);
    if (async_sends)
        out("STAT async first_hr=0x%08lx pending=%lu ok=%lu other=%lu send_complete=%d (ok %d)", (unsigned long)async_hr_first,
            (unsigned long)n_async_pending, (unsigned long)n_async_ok, (unsigned long)n_async_other, n_complete, n_complete_ok);
    if (others < 1 || with_pongs < others) ok = 0;
    if (aoe2names) {
        for (int i = 0; i < nrem; i++)
            if (!rem[i].orig[0] || strchr(rem[i].orig, '?')) {
                out("STAT aoe2names player name garbled: id=%lu \"%s\"", (unsigned long)rem[i].id, rem[i].orig);
                names_bad = 1;
            }
        if (expect && max_players > expect) {   /* DirectPlay counted a player that isn't there */
            out("STAT aoe2names player count %d > %d players in the game", max_players, expect);
            ok = 0;
        }
        out("STAT aoe2names names %s", names_bad ? "GARBLED" : "intact");
        if (names_bad) ok = 0;
    }
    if (null_names) {   /* our DirectPlay must never hand a game a NULL player name (it crashed AoE2) */
        if (dplayx && *dplayx) {
            out("STAT null_names=%d", null_names);
            ok = 0;
        } else   /* Windows' dplayx passes the joiner's NULL long name on: the reference, not a failure */
            out("STAT null_names=%d (Windows' dplayx does this - the drop-in must not)", null_names);
    }
    if (features && feature_result != 1) {
        if (feature_result < 0) out("STAT features never checked (ran too short)");
        ok = 0;
    }
    if (cycles > 1) out("STAT cycle %d max_players=%d peers_answering=%d %s", cyc, max_players, with_pongs, ok ? "ok" : "FAIL");
    if (!ok) all_ok = 0;
    if (aoe2names)   /* as AoE2 does when a player resigns or quits: remove our player, then leave */
        out("EVENT destroyed_own_player 0x%08lx", (unsigned long)IDirectPlayX_DestroyPlayer(dp, me));
    hr = IDirectPlayX_Close(dp);
    if (cycles > 1) out("EVENT closed 0x%lx", (unsigned long)hr);
    }
    IDirectPlayX_Release(dp);
    out("RESULT %s", all_ok ? "ok" : "fail");
    return all_ok ? 0 : 1;
}
