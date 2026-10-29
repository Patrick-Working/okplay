/* okplay - opt-in reports to the room server (0.8.3): crash reports and connection reports.
 *
 * Nothing is sent without the player's yes:
 *   crash_reports=ask|always|never        (okplay.ini, default ask). A crash writes a pending report next
 *                                         to the logs (okplay-crash-report.txt); the next time the game
 *                                         opens Multiplayer the drop-in asks whether to send it, with a
 *                                         button to read it first. always = send without asking.
 *   connection_reports=ask|yes|no         (default ask). Each game leaves one summary line (role, how the
 *                                         join went, connection type, ping, resends); with ask they wait
 *                                         in okplay-conn-reports.txt and the player is asked once, the
 *                                         next time Multiplayer opens; the answer is written back here.
 * The okplay.dll provider (Windows' DirectPlay) has no boxes: there ask means no.
 *
 * What a report holds: okplay's version, Windows' version, the game exe's file name, and for a crash
 * where it happened (module+offset), the loaded modules' file names and okplay's last error/warning/
 * info lines with every address and folder taken out (scrub). Never player names, chat or addresses:
 * debug lines (where DirectPlay's own traces and message dumps go) are left out entirely.
 * Tests: OKP_TEST_CRASH_REPORT=send|dont|always|never and OKP_TEST_CONN_REPORTS=yes|no answer the
 * boxes (=ask: show them anyway, for tests/preview_boxes.sh); OKP_REPORT_SYNC=1 asks and sends on the
 * calling thread (so a test sees it done when the call returns). */
#include "okp.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define CRASH_FILE OKP_CRASH_REPORT_FILE
#define CONN_FILE  "okplay-conn-reports.txt"
#define CONN_KEEP  4096        /* bytes of unsent connection lines kept (newest win) */

#ifdef OKP_BUILTIN
int DP_BUILTIN_AskCrashReport(const char *path);   /* builtin.c: 0 don't send, 1 send, 2 always send */
int DP_BUILTIN_AskConnReports(void);               /* builtin.c: 1 yes, 0 no (written to okplay.ini) */
#endif

static int report_path(const char *name, char *out, int n)
{
    const char *d = okp_log_dir();
    if (!d || !d[0]) return 0;   /* logging off (loglevel=0): nowhere to keep anything */
    snprintf(out, n, "%s%s", d, name);
    return 1;
}

static void ini_get(const char *key, const char *def, char *out, int n)
{
    char ini[MAX_PATH];
    snprintf(ini, sizeof(ini), "%sokplay.ini", okp_dll_dir());
    GetPrivateProfileStringA("okplay", key, def, out, n, ini);
}

/* ------------------------------------------------------------------ files */

/* the whole file (malloc'd, NUL-terminated) or NULL; opened without sharing, retried briefly: two
 * games on one PC (or a test's several processes) share the log folder */
static HANDLE open_locked(const char *path, DWORD access, DWORD disp)
{
    for (int i = 0; i < 40; i++) {
        HANDLE f = CreateFileA(path, access, 0, NULL, disp, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) return f;
        if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) return f;
        Sleep(25);
    }
    return INVALID_HANDLE_VALUE;
}

static char *read_all(const char *path, DWORD *len)
{
    HANDLE f = open_locked(path, GENERIC_READ, OPEN_EXISTING);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    DWORD size = GetFileSize(f, NULL), got = 0;
    char *buf = size != INVALID_FILE_SIZE && size < 65536 ? (char *)malloc(size + 1) : NULL;
    if (buf && !ReadFile(f, buf, size, &got, NULL)) got = 0;
    CloseHandle(f);
    if (!buf) return NULL;
    buf[got] = 0;
    *len = got;
    return buf;
}

/* drop the first `sent` bytes (they reached the server; newer lines may have been added since) */
static void drop_front(const char *path, DWORD sent)
{
    HANDLE f = open_locked(path, GENERIC_READ | GENERIC_WRITE, OPEN_EXISTING);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(f, NULL), got = 0, w;
    char *buf = size != INVALID_FILE_SIZE && size >= sent ? (char *)malloc(size - sent + 1) : NULL;
    if (buf) {
        SetFilePointer(f, (LONG)sent, NULL, FILE_BEGIN);
        ReadFile(f, buf, size - sent, &got, NULL);
        SetFilePointer(f, 0, NULL, FILE_BEGIN);
        WriteFile(f, buf, got, &w, NULL);
        SetEndOfFile(f);
        free(buf);
    }
    CloseHandle(f);
    if (got == 0) DeleteFileA(path);
}

static int send_report(const char *kind, const char *body)
{
    size_t n = strlen(body) * 3 + 256;
    char *enc = (char *)malloc(n), *post = (char *)malloc(n + 256);
    int ok = 0;
    if (enc && post) {
        okp_urlenc(body, enc, (int)n);
        snprintf(post, n + 256, "k=%s&a=report&kind=%s&ver=" OKP_VERSION_STR "&body=%s", okp_cfg.key, kind, enc);
        ok = okp_rdv_post_simple(post);
    }
    free(enc);
    free(post);
    okp_log(1, "report: %s report %s", kind, ok ? "sent" : "not sent (the server didn't take it; kept for next time)");
    return ok;
}

/* ------------------------------------------------------------------ connection reports */

static int conn_choice(void)   /* 1 yes, 0 no, -1 ask */
{
    char v[16];
    const char *t = getenv("OKP_TEST_CONN_REPORTS");
    ini_get("connection_reports", "ask", v, sizeof(v));
    if (t && *t && _stricmp(t, "ask")) return !_stricmp(t, "yes");
    if (!_stricmp(v, "yes") || !strcmp(v, "1")) return 1;
    if (!_stricmp(v, "no") || !strcmp(v, "0")) return 0;
#ifdef OKP_BUILTIN
    return -1;
#else
    return 0;   /* no box to ask with */
#endif
}

static void send_conn_pending(void)
{
    char path[MAX_PATH];
    DWORD len = 0;
    if (!report_path(CONN_FILE, path, sizeof(path))) return;
    char *body = read_all(path, &len);
    if (body && len && send_report("conn", body)) drop_front(path, len);
    free(body);
}

static volatile LONG g_conn_sending;

static DWORD WINAPI conn_send_thread(LPVOID unused)
{
    (void)unused;
    send_conn_pending();
    InterlockedExchange(&g_conn_sending, 0);
    return 0;
}

void okp_report_session_begin(okp_ctx *c, int host)
{
    c->sess_t0 = okp_now();
    c->sess_role = host ? 1 : 2;
    c->sess_peers = 0;
    lstrcpynA(c->sess_result, host ? "ok" : "joining", sizeof(c->sess_result));
}

void okp_report_session_result(okp_ctx *c, const char *result)
{
    if (c->sess_role) lstrcpynA(c->sess_result, result, sizeof(c->sess_result));
}

/* CloseEx (or a failed join): one line per game - queued, then sent if the player said yes */
void okp_report_session_end(okp_ctx *c)
{
    char line[1024], path[MAX_PATH], sysname[80];
    if (!c->sess_role) return;
    int role = c->sess_role;
    c->sess_role = 0;
    int choice = conn_choice();
    if (choice == 0 || !report_path(CONN_FILE, path, sizeof(path))) return;

    /* the peers: how they were reached and how the reliable lane did */
    int peers = 0, direct = 0, lan = 0, v6 = 0;
    DWORD rtt_sum = 0, rtt_max = 0;
    uint32_t sent = 0, retrans = 0, giveup = 0, badtok = 0, changes = 0, qdrop = 0;
    EnterCriticalSection(&c->lock);
    for (int i = 0; i < OKP_MAX_PEERS; i++) {
        okp_peer *p = &c->peers[i];
        if (!p->used || !p->rx_pkts) continue;
        peers++;
        if (p->have_path) {
            direct++;
            if (!okp_addr_is_v4(&p->path)) v6++;
            else if (!okp_addr_is_public_v4(&p->path)) lan++;
        }
        rtt_sum += p->rtt;
        if (p->rtt > rtt_max) rtt_max = p->rtt;
        sent += p->r_sent; retrans += p->r_retrans; giveup += p->r_giveup;
        badtok += p->rx_badtoken; changes += p->path_changes; qdrop += p->dropped_q;
    }
    LeaveCriticalSection(&c->lock);
    if (peers > c->sess_peers) c->sess_peers = peers;
    char exe[64];
    okp_exe_name(exe, sizeof(exe));
    if (role == 2 && !strcmp(c->sess_result, "joining")) lstrcpynA(c->sess_result, "ok", sizeof(c->sess_result));
    int n = snprintf(line, sizeof(line),
        "result=%s role=%s mode=%s secs=%lu peers=%d paths=%d lan=%d v6=%d rtt_avg=%lu rtt_max=%lu "
        "transport=%s reliable_sent=%lu resent=%lu gave_up=%lu bad_token=%lu path_changes=%lu queue_drops=%lu "
        "public_addr=%s ver=%s win=%lu.%lu.%lu exe=%s sys=\"%s\"\n",
        c->sess_result, role == 1 ? "host" : "join", c->mode == OKP_MODE_LAN ? "lan" : "online",
        (unsigned long)((okp_now() - c->sess_t0) / 1000), peers, direct, lan, v6,
        (unsigned long)(peers ? rtt_sum / peers : 0), (unsigned long)rtt_max, okp_transport_name(okp_effective_transport(c)),
        (unsigned long)sent, (unsigned long)retrans, (unsigned long)giveup, (unsigned long)badtok, (unsigned long)changes,
        (unsigned long)qdrop, c->have_srflx ? c->srflx_src : c->have_guess ? "server" : "none", OKP_VERSION_STR,
        (unsigned long)okp_sys.major, (unsigned long)okp_sys.minor, (unsigned long)okp_sys.build, exe,
        okp_sys_name(sysname, sizeof(sysname)));
    okp_log(1, "report: connection summary: %.*s", n > 0 ? n - 1 : 0, line);

    HANDLE f = open_locked(path, GENERIC_READ | GENERIC_WRITE, OPEN_ALWAYS);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(f, NULL), w;
    if (size != INVALID_FILE_SIZE && size + n > CONN_KEEP) {   /* keep the newest: drop whole old lines */
        char *buf = (char *)malloc(size + 1);
        DWORD got = 0;
        if (buf) {
            ReadFile(f, buf, size, &got, NULL);
            buf[got] = 0;
            char *keep = buf;
            while (keep && (DWORD)(buf + got - keep) + n > CONN_KEEP) { keep = strchr(keep, '\n'); if (keep) keep++; }
            if (!keep) keep = buf + got;
            SetFilePointer(f, 0, NULL, FILE_BEGIN);
            WriteFile(f, keep, (DWORD)(buf + got - keep), &w, NULL);
            SetEndOfFile(f);
            free(buf);
        }
    }
    SetFilePointer(f, 0, NULL, FILE_END);
    WriteFile(f, line, n, &w, NULL);
    CloseHandle(f);

    if (choice == 1 && !InterlockedExchange(&g_conn_sending, 1)) {
        if (GetEnvironmentVariableA("OKP_REPORT_SYNC", NULL, 0)) conn_send_thread(NULL);
        else {
            HANDLE t = CreateThread(NULL, 0, conn_send_thread, NULL, 0, NULL);
            if (t) CloseHandle(t); else InterlockedExchange(&g_conn_sending, 0);
        }
    }
}

/* ------------------------------------------------------------------ at SPInit: what's pending */

static volatile LONG g_crash_asked, g_conn_asked, g_pending_busy;

/* OKP_REPORTS_NO_ASK=1 (Connection Check sets it): no boxes in this program - what waits is asked
 * about the next time a game opens Multiplayer */
static int no_boxes(void) { return GetEnvironmentVariableA("OKP_REPORTS_NO_ASK", NULL, 0) != 0; }

static int exists(const char *name, char *path, int n)
{
    return report_path(name, path, n) && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static DWORD WINAPI pending_thread(LPVOID unused)
{
    char path[MAX_PATH], ask[16];
    (void)unused;
    /* a crash report from an earlier run (asked once per game run) */
    if (exists(CRASH_FILE, path, sizeof(path)) && !InterlockedExchange(&g_crash_asked, 1)) {
        int send = 0;
        const char *t = getenv("OKP_TEST_CRASH_REPORT");
        ini_get("crash_reports", "ask", ask, sizeof(ask));
        if (t && *t && _stricmp(t, "ask")) {
            send = !_stricmp(t, "send") || !_stricmp(t, "always");
            if (!_stricmp(t, "always") || !_stricmp(t, "never")) okp_report_set_ini("crash_reports", t);
        } else if (!_stricmp(ask, "always")) send = 1;
        else if (!_stricmp(ask, "never")) send = 0;
        else if (no_boxes()) {
            InterlockedExchange(&g_crash_asked, 0);   /* asked later, by the game */
            InterlockedExchange(&g_pending_busy, 0);
            return 0;
        } else {
#ifdef OKP_BUILTIN
            int r = DP_BUILTIN_AskCrashReport(path);
            send = r > 0;
            if (r == 2) okp_report_set_ini("crash_reports", "always");
#endif
        }
        DWORD len = 0;
        char *body = send ? read_all(path, &len) : NULL;
        if (!send) {
            okp_log(1, "report: the player chose not to send the crash report");
            DeleteFileA(path);
        } else if (body && send_report("crash", body)) {
            DeleteFileA(path);
        }
        free(body);
    }
    /* connection reports: the first answer (asked once per game run), or lines an earlier send left */
    if (exists(CONN_FILE, path, sizeof(path))) {
        int choice = conn_choice();
#ifdef OKP_BUILTIN
        if (choice < 0 && !no_boxes() && !InterlockedExchange(&g_conn_asked, 1)) {
            choice = DP_BUILTIN_AskConnReports();
            okp_report_set_ini("connection_reports", choice ? "yes" : "no");
        }
#endif
        if (choice > 0) {
            if (!InterlockedExchange(&g_conn_sending, 1)) conn_send_thread(NULL);
        } else if (choice == 0) {
            DeleteFileA(path);
        }
    }
    InterlockedExchange(&g_pending_busy, 0);
    return 0;
}

/* SPInit (the game opened Multiplayer - never during a match): offer or send what is waiting */
void okp_reports_kick(void)
{
    char path[MAX_PATH];
    int crash = !g_crash_asked && exists(CRASH_FILE, path, sizeof(path)) && !no_boxes();
    int conn = exists(CONN_FILE, path, sizeof(path)) && conn_choice() != 0 &&
               !(conn_choice() < 0 && (g_conn_asked || no_boxes()));
    if ((!crash && !conn) || InterlockedExchange(&g_pending_busy, 1)) return;
    if (GetEnvironmentVariableA("OKP_REPORT_SYNC", NULL, 0)) { pending_thread(NULL); return; }
    HANDLE t = CreateThread(NULL, 0, pending_thread, NULL, 0, NULL);
    if (t) CloseHandle(t); else InterlockedExchange(&g_pending_busy, 0);
}

void okp_report_set_ini(const char *key, const char *value)
{
    char ini[MAX_PATH];
    snprintf(ini, sizeof(ini), "%sokplay.ini", okp_dll_dir());
    WritePrivateProfileStringA("okplay", key, value, ini);
    okp_log(1, "report: okplay.ini %s=%s", key, value);
}
