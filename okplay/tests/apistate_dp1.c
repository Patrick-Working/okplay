/* apistate: IDirectPlay (DirectPlay 1, the DirectX 1-3 interface), included by apistate.c.
 *
 * What DirectX 3-era games (DPLAY.DLL: Necrodome) do with it, called on the same object through
 * QueryInterface(IID_IDirectPlay). Besides each call's HRESULT ("call" lines) it prints what came back
 * ("data" lines): caps, names, enumerated players/groups/sessions and every received message byte for
 * byte - player and group ids as names (P1, G1, ...) and pointers into the message as offsets, since
 * those legitimately differ between two DirectPlay implementations. apidiff.sh compares both kinds.
 */

static IDirectPlay *dp1;
static const GUID dp1_guid_null;   /* GUID_NULL: "no provider yet" */
static struct { DWORD id; char name[8]; } dp1_ids[16];
static int dp1_nids;

static void dp1_name_id(DWORD id, const char *name)
{
    if (!id || dp1_nids >= 16) return;
    dp1_ids[dp1_nids].id = id;
    lstrcpynA(dp1_ids[dp1_nids].name, name, sizeof(dp1_ids[0].name));
    dp1_nids++;
}

/* an id as a name if it is one we know, else "id?" (never the raw value: they differ by design) */
static const char *dp1_idstr(DWORD id, char *buf)
{
    if (id == 0) return "0";
    if (id == DPID_SERVERPLAYER) return "SERVER";
    if (id == DPID_UNKNOWN) return "UNKNOWN";
    for (int i = 0; i < dp1_nids; i++) if (dp1_ids[i].id == id) return dp1_ids[i].name;
    strcpy(buf, "id?");
    return buf;
}

static void data(const char *label, const char *fmt, ...)
{
    char text[1200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    printf("data %-30s %s\n", label, text);
    fflush(stdout);
}

/* a message as DWORDs: known ids by name, pointers into the message as +offset, and the fields
 * that differ between two implementations by nature shown by what they are, not their value:
 *   DPSYS_SETPLAYERORGROUPNAME (0x103): the DPNAME's dwFlags ("not used", the SDK says) - Windows
 *     leaves whatever was in memory there (a heap pointer, a return address), so "nf"
 *   DPSYS_SETSESSIONDESC (0x104): the session's instance GUID (random per session) as "inst", and
 *     its dwReserved1 (the session key, random per session) as "key" when set */
static void dp1_dump(const char *label, const BYTE *msg, DWORD size)
{
    char out[1100], tmp[24], idb[8];
    int o = 0;
    DWORD type = size >= 4 ? *(const DWORD *)msg : 0;
    o += snprintf(out + o, sizeof(out) - o, "size=%lu", (unsigned long)size);
    for (DWORD i = 0; i + 4 <= size && i < 256 && o < (int)sizeof(out) - 24; i += 4) {
        DWORD v = *(const DWORD *)(msg + i);
        const char *s = tmp;
        if (type == DPSYS_SETPLAYERORGROUPNAME && i == 16) s = "nf";
        else if (type == DPSYS_SETSESSIONDESC && i >= 12 && i < 28) s = "inst";
        else if (type == DPSYS_SETSESSIONDESC && i == 60 && v) s = "key";
        else if (v >= (DWORD)(uintptr_t)msg && v < (DWORD)(uintptr_t)msg + size) snprintf(tmp, sizeof(tmp), "+%lu", (unsigned long)(v - (DWORD)(uintptr_t)msg));
        else {
            const char *n = dp1_idstr(v, idb);
            if (i > 0 && strcmp(n, "id?") && v > 1 && v != DPID_UNKNOWN) s = n;
            else snprintf(tmp, sizeof(tmp), "%08lx", (unsigned long)v);
        }
        o += snprintf(out + o, sizeof(out) - o, " %s", s);
    }
    for (DWORD i = size & ~3u; i < size && i < 256; i++) o += snprintf(out + o, sizeof(out) - o, " b%02x", msg[i]);
    data(label, "%s", out);
}

/* everything waiting, in order */
static void dp1_drain(const char *tag)
{
    char label[64], idb1[8], idb2[8];
    BYTE buf[1024];
    for (int n = 0; n < 48; n++) {   /* the open state's queue holds ~30 by the first drain */
        DPID from = 0, to = 0;
        DWORD sz = sizeof(buf);
        HRESULT hr = IDirectPlay_Receive(dp1, &from, &to, DPRECEIVE_ALL, buf, &sz);
        snprintf(label, sizeof(label), "%sReceive#%d", tag, n);
        CALL(label, hr);
        if (FAILED(hr)) break;
        data(label, "from=%s to=%s", dp1_idstr(from, idb1), dp1_idstr(to, idb2));
        dp1_dump(label, buf, sz);
    }
}

struct dp1_enum_ctx { char tag[48]; int n; };

static BOOL FAR PASCAL dp1_players_cb(DPID id, LPSTR friendly, LPSTR formal, DWORD flags, LPVOID ctx)
{
    struct dp1_enum_ctx *c = ctx;
    char label[64], idb[8];
    snprintf(label, sizeof(label), "%s#%d", c->tag, c->n++);
    data(label, "id=%s friendly=%s formal=%s flags=%08lx", dp1_idstr(id, idb), friendly ? friendly : "(null)",
         formal ? formal : "(null)", (unsigned long)flags);
    return TRUE;
}

static BOOL FAR PASCAL dp1_sessions_cb(LPDPSESSIONDESC sd, LPVOID ctx, LPDWORD timeout, DWORD flags)
{
    struct dp1_enum_ctx *c = ctx;
    char label[64];
    snprintf(label, sizeof(label), "%s#%d", c->tag, c->n++);
    if (flags & DPESC_TIMEDOUT) { data(label, "timed out (flags %08lx)", (unsigned long)flags); return FALSE; }
    if (!sd) { data(label, "NULL desc flags=%08lx", (unsigned long)flags); return TRUE; }
    data(label, "size=%lu app=%s session=%s max=%lu cur=%lu flags=%08lx name=%.32s pw=%.16s user=%lu,%lu,%lu,%lu r=%lu,%lu cbflags=%08lx",
         (unsigned long)sd->dwSize, IsEqualGUID(&sd->guidSession, &APISTATE_APP) ? "ours" : "other",
         sd->dwSession ? "nonzero" : "0", (unsigned long)sd->dwMaxPlayers, (unsigned long)sd->dwCurrentPlayers,
         (unsigned long)sd->dwFlags, sd->szSessionName, sd->szPassword, (unsigned long)sd->dwUser1,
         (unsigned long)sd->dwUser2, (unsigned long)sd->dwUser3, (unsigned long)sd->dwUser4,
         (unsigned long)sd->dwReserved1, (unsigned long)sd->dwReserved2, (unsigned long)flags);
    (void)timeout;
    return TRUE;
}

static void dp1_caps(const char *label, DWORD size, BOOL player, DPID id)
{
    DWORD raw[16];
    char l2[64];
    memset(raw, 0, sizeof(raw));   /* Windows refuses any dwFlags but DPGETCAPS_GUARANTEED */
    raw[0] = size;
    HRESULT hr = player ? IDirectPlay_GetPlayerCaps(dp1, id, (DPCAPS *)raw) : IDirectPlay_GetCaps(dp1, (DPCAPS *)raw);
    CALL(label, hr);
    if (SUCCEEDED(hr)) {
        snprintf(l2, sizeof(l2), "%s", label);
        data(l2, "dwSize=%lu flags=%08lx maxbuf=%lu queue=%lu maxplayers=%lu baud=%lu latency=%lu [7..10]=%08lx %08lx %08lx %08lx",
             (unsigned long)raw[0], (unsigned long)raw[1], (unsigned long)raw[2], (unsigned long)raw[3],
             (unsigned long)raw[4], (unsigned long)raw[5], (unsigned long)raw[6], (unsigned long)raw[7],
             (unsigned long)raw[8], (unsigned long)raw[9], (unsigned long)raw[10]);
    }
}

static void dp1_names(const char *tag, DPID id)
{
    char label[64], f[64], l[64];
    DWORD fs, ls;
    HRESULT hr;
    /* sizes only */
    fs = ls = 0;
    snprintf(label, sizeof(label), "%sGetPlayerName(sizes)", tag);
    hr = IDirectPlay_GetPlayerName(dp1, id, NULL, &fs, NULL, &ls);
    CALL(label, hr);
    data(label, "fs=%lu ls=%lu", (unsigned long)fs, (unsigned long)ls);
    /* big enough */
    fs = ls = sizeof(f);
    memset(f, '#', sizeof(f)); memset(l, '#', sizeof(l));
    snprintf(label, sizeof(label), "%sGetPlayerName", tag);
    hr = IDirectPlay_GetPlayerName(dp1, id, f, &fs, l, &ls);
    CALL(label, hr);
    if (SUCCEEDED(hr)) data(label, "friendly=%.60s fs=%lu formal=%.60s ls=%lu", f, (unsigned long)fs, l, (unsigned long)ls);
    else data(label, "fs=%lu ls=%lu", (unsigned long)fs, (unsigned long)ls);
    /* too small */
    fs = ls = 2;
    memset(f, '#', sizeof(f)); memset(l, '#', sizeof(l));
    snprintf(label, sizeof(label), "%sGetPlayerName(small)", tag);
    hr = IDirectPlay_GetPlayerName(dp1, id, f, &fs, l, &ls);
    CALL(label, hr);
    data(label, "fs=%lu ls=%lu f0=%02x f1=%02x f2=%02x", (unsigned long)fs, (unsigned long)ls, f[0] & 255, f[1] & 255, f[2] & 255);
}

/* every IDirectPlay method, with unknown ids first, then on a player and group of its own */
static void dp1_calls(const char *state)
{
    char tag[32], label[64];
    DPID p1 = 0, p2 = 0, g1 = 0;
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL), evh = ev, nullh = NULL;
    struct dp1_enum_ctx ec;
    BYTE buf[64];
    DWORD n = 0;

    if (FAILED(IDirectPlayX_QueryInterface(dp, &IID_IDirectPlay, (void **)&dp1)) || !dp1) { printf("info no IDirectPlay\n"); return; }
    /* (the ids the IDirectPlay4 calls created keep their names: Q1, H1, H2 - apistate.c) */
#define D(name, expr) do { snprintf(label, sizeof(label), "%s%s", tag, name); CALL(label, expr); } while (0)
    strcpy(tag, "dp1:");
    dp1_caps("dp1:GetCaps(28)", 28, FALSE, 0);
    dp1_caps("dp1:GetCaps(40)", 40, FALSE, 0);
    dp1_caps("dp1:GetCaps(0)", 0, FALSE, 0);
    dp1_caps("dp1:GetCaps(44)", 44, FALSE, 0);
    dp1_caps("dp1:GetCaps(24)", 24, FALSE, 0);
    dp1_caps("dp1:GetPlayerCaps(unknown)", 28, TRUE, 0x1234);
    D("GetMessageCount(unknown)", (n = 0, IDirectPlay_GetMessageCount(dp1, 0x1234, &n)));
    D("SetPlayerName(unknown)", IDirectPlay_SetPlayerName(dp1, 0x1234, (char *)"x", (char *)"y"));
    D("AddPlayerToGroup(unknown)", IDirectPlay_AddPlayerToGroup(dp1, 0x5e7, 0x1234));
    D("DeletePlayerFromGroup(unknown)", IDirectPlay_DeletePlayerFromGroup(dp1, 0x5e7, 0x1234));
    D("DestroyPlayer(unknown)", IDirectPlay_DestroyPlayer(dp1, 0x1234));
    D("DestroyGroup(unknown)", IDirectPlay_DestroyGroup(dp1, 0x5e7));
    D("Send(unknown)", IDirectPlay_Send(dp1, 0x1234, 0, 0, buf, 4));
    D("SaveSession", IDirectPlay_SaveSession(dp1, NULL));
    dp1_names("dp1:unknown:", 0x1234);

    /* players and a group of its own (the friendly name is what games show) */
    D("CreatePlayer", IDirectPlay_CreatePlayer(dp1, &p1, (char *)"dp1", (char *)"DP One", NULL));
    dp1_name_id(p1, "P1");
    D("CreatePlayer(event)", IDirectPlay_CreatePlayer(dp1, &p2, (char *)"two", NULL, &evh));
    dp1_name_id(p2, "P2");
    data("dp1:CreatePlayer(event)", "event handle %s", evh == ev ? "unchanged" : evh ? "changed" : "NULL");
    D("CreatePlayer(null event)", IDirectPlay_CreatePlayer(dp1, NULL, (char *)"three", NULL, &nullh));
    D("CreateGroup", IDirectPlay_CreateGroup(dp1, &g1, (char *)"grp", (char *)"Group One"));
    dp1_name_id(g1, "G1");
    data("dp1:ids", "P1=%s P2=%s G1=%s", p1 ? "set" : "0", p2 ? "set" : "0", g1 ? "set" : "0");
    if (p1) {
        dp1_names("dp1:P1:", p1);
        dp1_caps("dp1:GetPlayerCaps(P1,28)", 28, TRUE, p1);
        dp1_caps("dp1:GetPlayerCaps(P1,40)", 40, TRUE, p1);
        dp1_caps("dp1:GetPlayerCaps(P1,24)", 24, TRUE, p1);
        D("SetPlayerName", IDirectPlay_SetPlayerName(dp1, p1, (char *)"dp1b", (char *)"DP One B"));
        dp1_names("dp1:P1 renamed:", p1);
        D("SetPlayerName(NULLs)", IDirectPlay_SetPlayerName(dp1, p1, NULL, NULL));
        dp1_names("dp1:P1 unnamed:", p1);
        D("SetPlayerName(back)", IDirectPlay_SetPlayerName(dp1, p1, (char *)"dp1", (char *)"DP One"));
    }
    if (g1 && p1) D("AddPlayerToGroup", IDirectPlay_AddPlayerToGroup(dp1, g1, p1));
    ec.n = 0; strcpy(ec.tag, "dp1:EnumPlayers");
    D("EnumPlayers", IDirectPlay_EnumPlayers(dp1, 0, dp1_players_cb, &ec, 0));
    ec.n = 0; strcpy(ec.tag, "dp1:EnumGroups");
    D("EnumGroups", IDirectPlay_EnumGroups(dp1, 0, dp1_players_cb, &ec, 0));
    ec.n = 0; strcpy(ec.tag, "dp1:EnumGroupPlayers");
    D("EnumGroupPlayers", IDirectPlay_EnumGroupPlayers(dp1, g1, dp1_players_cb, &ec, 0));
    dp1_drain("dp1:after create:");
    if (p1) {
        memcpy(buf, "hello dp1", 10);
        D("Send(P1->P2)", IDirectPlay_Send(dp1, p1, p2, 0, buf, 10));
        D("Send(P1->all)", IDirectPlay_Send(dp1, p1, 0, 0, buf, 10));
        D("Send(P1->G1)", IDirectPlay_Send(dp1, p1, g1, 0, buf, 10));
        D("Send(P1->P2 guaranteed)", IDirectPlay_Send(dp1, p1, p2, DPSEND_GUARANTEED, buf, 10));
        Sleep(100);
        D("GetMessageCount(P2)", (n = 0, IDirectPlay_GetMessageCount(dp1, p2, &n)));
        data("dp1:GetMessageCount(P2)", "n=%lu", (unsigned long)n);
        { int a = WaitForSingleObject(evh, 0) == WAIT_OBJECT_0, b = WaitForSingleObject(evh, 0) == WAIT_OBJECT_0; data("dp1:event (returned)", "%s, then %s", a ? "set" : "not set", b ? "set" : "not set"); }
        dp1_drain("dp1:after send:");
    }
    if (g1 && p1) D("DeletePlayerFromGroup", IDirectPlay_DeletePlayerFromGroup(dp1, g1, p1));
    if (g1) D("DestroyGroup", IDirectPlay_DestroyGroup(dp1, g1));
    if (p2) D("DestroyPlayer(P2)", IDirectPlay_DestroyPlayer(dp1, p2));
    dp1_drain("dp1:after destroy:");
    if (p1) D("DestroyPlayer(P1)", IDirectPlay_DestroyPlayer(dp1, p1));
    D("Initialize", IDirectPlay_Initialize(dp1, (GUID *)&DPSPGUID_TCPIP));
    if (!strcmp(state, "open") || !strcmp(state, "dp1host")) {
        DPSESSIONDESC sd;
        memset(&sd, 0, sizeof(sd));
        sd.dwSize = sizeof(sd);
        sd.guidSession = APISTATE_APP;
        ec.n = 0; strcpy(ec.tag, "dp1:EnumSessions");
        D("EnumSessions(available)", IDirectPlay_EnumSessions(dp1, &sd, 300, dp1_sessions_cb, &ec, DPENUMSESSIONS_AVAILABLE));
    }
#undef D
    CloseHandle(ev);
    IDirectPlay_Release(dp1);
    dp1 = NULL;
}

/* state dp1create: exactly a DirectX 3 game's start - DirectPlayCreate(provider) from the DLL
 * (Windows' when none is given), GetCaps, then hosting through IDirectPlay::Open */
static int dp1_create_direct(const char *dll)
{
    typedef HRESULT (WINAPI *create_t)(LPGUID, LPDIRECTPLAY *, IUnknown *);
    HMODULE m = LoadLibraryA(dll && *dll ? dll : "dplayx.dll");
    /* APISTATE_DP1_ORDINAL=1: by ordinal 1, as DirectX 3 games import it (Windows' dplay.dll has no name) */
    create_t create = !m ? NULL : getenv("APISTATE_DP1_ORDINAL") ? (create_t)GetProcAddress(m, MAKEINTRESOURCEA(1))
                                                                 : (create_t)GetProcAddress(m, "DirectPlayCreate");
    DPSESSIONDESC sd;
    HRESULT hr;
    if (!create) { printf("create failed: no DirectPlayCreate\n"); return 2; }
    dp1_nids = 0;
    CALL("dp1c:DirectPlayCreate(NULL guid)", create(NULL, &dp1, NULL));
    if (dp1) { IDirectPlay_Release(dp1); dp1 = NULL; }
    CALL("dp1c:DirectPlayCreate(GUID_NULL)", create((GUID *)&dp1_guid_null, &dp1, NULL));
    if (dp1) {
        dp1_caps("dp1c:GetCaps(28) unbound", 28, FALSE, 0);
        IDirectPlay_Release(dp1); dp1 = NULL;
    }
    hr = create((GUID *)&DPSPGUID_TCPIP, &dp1, NULL);
    CALL("dp1c:DirectPlayCreate(TCP/IP)", hr);
    if (FAILED(hr) || !dp1) return 0;
    dp1_caps("dp1c:GetCaps(28)", 28, FALSE, 0);
    dp1_caps("dp1c:GetCaps(40)", 40, FALSE, 0);
    dp1_caps("dp1c:GetCaps(0)", 0, FALSE, 0);
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.guidSession = APISTATE_APP;
    sd.dwMaxPlayers = 4;
    sd.dwFlags = DPOPEN_CREATE;
    strcpy(sd.szSessionName, "apistate dp1c");
    CALL("dp1c:Open(create)", IDirectPlay_Open(dp1, &sd));
    dp1_caps("dp1c:GetCaps(28) hosting", 28, FALSE, 0);
    dp1_caps("dp1c:GetCaps(40) hosting", 40, FALSE, 0);
    {
        DPID p = 0;
        HANDLE ev = NULL;
        CALL("dp1c:CreatePlayer", IDirectPlay_CreatePlayer(dp1, &p, (char *)"host", (char *)"Host Player", &ev));
        dp1_name_id(p, "P1");
        data("dp1c:CreatePlayer", "event %s", ev ? "returned" : "NULL");
        dp1_caps("dp1c:GetPlayerCaps(P1,28)", 28, TRUE, p);
        dp1_caps("dp1c:GetPlayerCaps(P1,40)", 40, TRUE, p);
        dp1_drain("dp1c:");
        {   /* the raw ids (info, not compared; after the drain, so the messages about P2 stay out
             * of the table): Necrodome indexes a 200-entry table by them */
            DPID q = 0;
            HANDLE ev2 = NULL;
            HRESULT h2 = IDirectPlay_CreatePlayer(dp1, &q, (char *)"two", (char *)"Second Player", &ev2);
            printf("info dp1c:raw ids P1=0x%08lx P2=0x%08lx (0x%08lx)\n", (unsigned long)p, (unsigned long)q,
                   (unsigned long)h2);
            if (SUCCEEDED(h2)) IDirectPlay_DestroyPlayer(dp1, q);
            if (ev2) CloseHandle(ev2);
        }
        CALL("dp1c:DestroyPlayer", IDirectPlay_DestroyPlayer(dp1, p));
        if (ev) CloseHandle(ev);
    }
    CALL("dp1c:Close", IDirectPlay_Close(dp1));
    CALL("dp1c:Release", (HRESULT)IDirectPlay_Release(dp1));
    dp1 = NULL;
    return 0;
}

/* state dp1host: a session opened through IDirectPlay::Open (what the DX3 games do) */
static HRESULT dp1_open_create(void)
{
    DPSESSIONDESC sd;
    HRESULT hr;
    IDirectPlay *d;
    if (FAILED(hr = IDirectPlayX_QueryInterface(dp, &IID_IDirectPlay, (void **)&d))) return hr;
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.guidSession = APISTATE_APP;
    sd.dwMaxPlayers = 8;
    sd.dwFlags = DPOPEN_CREATE;   /* DirectX 3: DPOPEN_CREATESESSION, the same value */
    strcpy(sd.szSessionName, "apistate dp1 host");
    hr = IDirectPlay_Open(d, &sd);
    printf("setup dp1 Open(create) 0x%08lx\n", (unsigned long)hr);
    if (SUCCEEDED(hr)) {
        char buf[1024];
        DWORD sz = sizeof(buf);
        DPSESSIONDESC2 *s2 = (DPSESSIONDESC2 *)buf;
        if (SUCCEEDED(IDirectPlayX_GetSessionDesc(dp, buf, &sz)))
            data("setup dp1 session (DP2 view)", "flags=%08lx max=%lu cur=%lu app=%s name=%s", (unsigned long)s2->dwFlags,
                 (unsigned long)s2->dwMaxPlayers, (unsigned long)s2->dwCurrentPlayers,
                 IsEqualGUID(&s2->guidApplication, &APISTATE_APP) ? "ours" : "other",
                 s2->lpszSessionNameA ? s2->lpszSessionNameA : "(null)");
    }
    IDirectPlay_Release(d);
    return hr;
}
