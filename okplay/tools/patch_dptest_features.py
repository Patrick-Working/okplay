"""One-off: add --features (lobby-style changes + checks) to tools/dptest.c."""
import os
import sys

p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dptest.c")
s = open(p, encoding="utf-8", newline="").read()


def rep(old, new):
    global s
    if s.count(old) != 1:
        sys.exit("no unique match: " + old[:70])
    s = s.replace(old, new)


rep(" *   --guaranteed    send pings/pongs with DPSEND_GUARANTEED (env DPTEST_GUARANTEED=1)\n",
    " *   --guaranteed    send pings/pongs with DPSEND_GUARANTEED (env DPTEST_GUARANTEED=1)\n"
    " *   --features      also exercise what a lobby does: once everyone is in, rename our player,\n"
    " *                   set its remote data and send a chat line; the host also changes the\n"
    " *                   session description. Checks the notifications arrive and the Get* calls\n"
    " *                   return the new values (STAT features ...).\n")

rep("static int found_okplay, found_lan;\n",
    "static int found_okplay, found_lan;\n"
    "static int features, n_desc, n_name, n_data, n_chat, desc_value_ok;\n"
    "#define FEATURE_USER1 0xC0FFEE42\n")

rep('    case DPSYS_HOST:        out("EVENT became_host"); break;\n',
    '    case DPSYS_SETSESSIONDESC: {\n'
    '        DPMSG_SETSESSIONDESC *m = (DPMSG_SETSESSIONDESC *)g;\n'
    '        n_desc++;\n'
    '        if (m->dpDesc.dwUser1 == FEATURE_USER1) desc_value_ok = 1;\n'
    '        out("EVENT session_desc_changed user1=0x%lx", (unsigned long)m->dpDesc.dwUser1);\n'
    '        break;\n'
    '    }\n'
    '    case DPSYS_SETPLAYERORGROUPNAME: {\n'
    '        DPMSG_SETPLAYERORGROUPNAME *m = (DPMSG_SETPLAYERORGROUPNAME *)g;\n'
    '        n_name++;\n'
    '        out("EVENT name_changed id=%lu name=%s", (unsigned long)m->dpId,\n'
    '            m->dpnName.lpszShortNameA ? m->dpnName.lpszShortNameA : "?");\n'
    '        break;\n'
    '    }\n'
    '    case DPSYS_SETPLAYERORGROUPDATA: {\n'
    '        DPMSG_SETPLAYERORGROUPDATA *m = (DPMSG_SETPLAYERORGROUPDATA *)g;\n'
    '        n_data++;\n'
    '        out("EVENT data_changed id=%lu size=%lu", (unsigned long)m->dpId, (unsigned long)m->dwDataSize);\n'
    '        break;\n'
    '    }\n'
    '    case DPSYS_CHAT: {\n'
    '        DPMSG_CHAT *m = (DPMSG_CHAT *)g;\n'
    '        n_chat++;\n'
    '        out("EVENT chat from=%lu text=\\"%s\\"", (unsigned long)m->idFromPlayer,\n'
    '            m->lpChat && m->lpChat->lpszMessageA ? m->lpChat->lpszMessageA : "?");\n'
    '        break;\n'
    '    }\n'
    '    case DPSYS_HOST:        out("EVENT became_host"); break;\n')

rep("static int count_players(void)\n", r'''/* the lobby-style changes (see --features) */
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
''')

rep("    DWORD last_pong;\n} remote;", "    DWORD last_pong;\n    char orig[64];   /* name when first seen */\n} remote;")
rep('    lstrcpynA(r->name, name ? name : "?", sizeof(r->name));\n',
    '    lstrcpynA(r->name, name ? name : "?", sizeof(r->name));\n'
    '    if (!name && dp) {   /* already in the session when we joined: ask DirectPlay */\n'
    '        char buf[160];\n'
    '        DWORD sz = sizeof(buf);\n'
    '        if (IDirectPlayX_GetPlayerName(dp, id, buf, &sz) == DP_OK && ((DPNAME *)buf)->lpszShortNameA)\n'
    '            lstrcpynA(r->name, ((DPNAME *)buf)->lpszShortNameA, sizeof(r->name));\n'
    '    }\n'
    '    lstrcpynA(r->orig, r->name, sizeof(r->orig));\n')

rep('        else if (!strcmp(argv[i], "--guaranteed")) send_flags = DPSEND_GUARANTEED;\n',
    '        else if (!strcmp(argv[i], "--guaranteed")) send_flags = DPSEND_GUARANTEED;\n'
    '        else if (!strcmp(argv[i], "--features")) features = 1;\n')

rep("    int reached = expect == 0, max_players = 1;\n",
    "    int reached = expect == 0, max_players = 1;\n"
    "    int is_host = !strcmp(mode, \"host\");\n"
    "    DWORD feature_at = 0, feature_done = 0;\n")
rep('        if (!reached && np >= expect) {\n            reached = 1;\n            out("EVENT expected_players_reached %d", expect);\n        }\n',
    '        if (!reached && np >= expect) {\n            reached = 1;\n            out("EVENT expected_players_reached %d", expect);\n        }\n'
    '        /* host: once everyone is in; joiners: a little after joining */\n'
    '        if (features && !feature_at && (is_host ? (reached && expect) : 1))\n'
    '            feature_at = GetTickCount() + 1500 + (is_host ? 0 : 1000);\n'
    '        if (features && feature_at && !feature_done && GetTickCount() >= feature_at) {\n'
    '            feature_actions(is_host, name);\n'
    '            feature_done = 1;\n'
    '        }\n')
rep("    if (others < 1 || with_pongs < others) ok = 0;\n",
    "    if (others < 1 || with_pongs < others) ok = 0;\n"
    "    if (features && !feature_check(is_host)) ok = 0;\n")
open(p, "w", encoding="utf-8", newline="").write(s)
print("ok")
