#!/usr/bin/env bash
# Crash and connection reports (okplay/src/report.c, 0.8.3) against the stand-in room server:
#   1 a game with connection reports on: host and joiner each send one line, result=ok
#   2 a join to a host that is gone: result=no_path
#   3 connection reports off (dptest's default): nothing sent, nothing kept
#   4 a crash (OKP_CRASHTEST) leaves a report; the next start sends it when the player says yes
#   5 ... and deletes it unsent when the player says no
# Every report is checked for addresses and folders. Uses winedplay/bin/dropin/dplayx.dll.
#   report_test.sh [dplayx.dll]
cd "$(dirname "$0")/.."
. ../env.sh
DLL="${1:-../winedplay/bin/dropin/dplayx.dll}"
PORT=8794
T=tests/out/report
rm -rf "$T"; mkdir -p "$T"
"$PY" server/rdv_standin.py --port $PORT > "$T/server.log" 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
sleep 2
export OKP_SERVER=http://127.0.0.1:$PORT/okplay OKP_STUN= OKP_REPORT_SYNC=1
fails=0
ok() { echo "PASS $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

side() {   # a folder with its own drop-in and okplay.ini (its own netplay-logs)
    local d="$T/$1"
    mkdir -p "$d"; cp "$DLL" "$d/"
    printf '[okplay]\r\nloglevel=3\r\n%s' "${2:-}" > "$d/okplay.ini"
    echo "$d"
}
dp() { local d=$1; shift; bin/dptest.exe --dplayx "$(cygpath -w "$PWD/$d/dplayx.dll")" --sp okplay "$@"; }
reports() { curl -s "http://127.0.0.1:$PORT/okplay?k=okp_7Qm2xV9rLs4Hd8Wn&a=reports"; }
count() { reports | "$PY" -c "import json,sys; print(sum(1 for r in json.load(sys.stdin) if r['kind']=='$1'))"; }
clean_check() {   # no IPv4 address (other than x.x.x.x) and no folder in any report body
    reports | "$PY" -c "
import json,re,sys
bad=[]
for r in json.load(sys.stdin):
    b=r['body']
    if re.search(r'(?<![\d.v])(\d{1,3}\.){3}\d{1,3}(?![\d.])', b): bad.append('address in a %s report' % r['kind'])
    if re.search(r'[A-Za-z]:\\\\\\\\', b) or 'Users' in b: bad.append('folder in a %s report' % r['kind'])
print('; '.join(bad))"
}

echo "== 1: connection reports on"
H=$(side host1); J=$(side join1)
OKP_TEST_CONN_REPORTS=yes dp "$H" --name Host1 --secs 12 --expect 2 host > "$T/host1.txt" 2>&1 &
HP=$!; sleep 4
OKP_TEST_CONN_REPORTS=yes dp "$J" --name J1 --session "dptest Host1" --secs 6 join > "$T/join1.txt" 2>&1
wait $HP
n=$(count conn)
[ "$n" = 2 ] && ok "two connection reports (host, joiner)" || bad "connection reports: $n, want 2"
reports | grep -q 'result=ok role=host' && ok "host: result=ok" || bad "host line missing"
reports | grep -q 'result=ok role=join' && ok "joiner: result=ok" || bad "joiner line missing"
[ ! -f "$H/netplay-logs/okplay-conn-reports.txt" ] && ok "nothing left waiting after sending" || bad "sent lines still kept"

echo "== 2: joining a host that is gone"
H=$(side host2); J=$(side join2)
dp "$H" --name Gone --secs 30 host > "$T/host2.txt" 2>&1 &
sleep 4
# gone without a word (as a crash or a pulled cable): its room stays listed until the server's TTL
WP=$(powershell -NoProfile -Command "(Get-CimInstance Win32_Process -Filter \"Name='dptest.exe'\" | Where-Object { \$_.CommandLine -like '*--name Gone*' }).ProcessId")
taskkill //F //PID $WP > /dev/null 2>&1
sleep 1
OKP_TEST_CONN_REPORTS=yes dp "$J" --name J2 --session "dptest Gone" --secs 4 join > "$T/join2.txt" 2>&1
reports | grep -q 'result=no_path role=join' && ok "failed join: result=no_path" || bad "no no_path line"

echo "== 3: connection reports off"
before=$(count conn)
H=$(side host3)
dp "$H" --name Off --secs 3 host > /dev/null 2>&1
[ "$(count conn)" = "$before" ] && ok "nothing sent" || bad "sent although off"
[ ! -f "$H/netplay-logs/okplay-conn-reports.txt" ] && ok "nothing kept" || bad "kept although off"

echo "== 4: a crash, then Send"
C=$(side crash1)
OKP_CRASHTEST=1 dp "$C" --name C --secs 2 host > /dev/null 2>&1
[ -f "$C/netplay-logs/okplay-crash-report.txt" ] && ok "crash report kept for asking" || bad "no crash report file"
grep -q "^sig=ACCESS_VIOLATION at dplayx.dll+0x" "$C/netplay-logs/okplay-crash-report.txt" && ok "says where (dplayx.dll+offset)" || bad "no sig line"
OKP_TEST_CRASH_REPORT=send dp "$C" --name C --secs 2 host > /dev/null 2>&1
[ "$(count crash)" = 1 ] && ok "crash report sent after yes" || bad "crash reports: $(count crash), want 1"
[ ! -f "$C/netplay-logs/okplay-crash-report.txt" ] && ok "crash report deleted after sending" || bad "crash report still kept"

echo "== 5: a crash, then Don't Send"
C=$(side crash2)
OKP_CRASHTEST=1 dp "$C" --name C --secs 2 host > /dev/null 2>&1
dp "$C" --name C --secs 2 host > /dev/null 2>&1   # dptest answers "dont" by default
[ "$(count crash)" = 1 ] && ok "nothing more sent" || bad "sent although declined"
[ ! -f "$C/netplay-logs/okplay-crash-report.txt" ] && ok "declined report deleted" || bad "declined report still kept"

echo "== every report"
c=$(clean_check)
[ -z "$c" ] && ok "no addresses or folders in any report" || bad "$c"
reports > "$T/reports.json"
echo "failures: $fails  (reports: $T/reports.json)"
[ $fails = 0 ]
