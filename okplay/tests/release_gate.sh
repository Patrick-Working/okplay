#!/usr/bin/env bash
# Version: 2026-11-10
# Release gate for okplay: "does everything still work?" in one command, one table.
#
#   okplay/tests/release_gate.sh [--list] [--only a,b] [--skip a,b] [--no-build] [--quick] [-h]
#
# Builds (unless --no-build), then runs every local suite one after the other (never in
# parallel: the suites share ports and are timing-sensitive), each with a time limit and its own
# log in okplay/tests/out/gate/<step>.log. At the end it prints one table and exits 0 only if no
# step FAILED. SKIP (a missing optional tool, a long step left out by --quick) does not fail the gate.
#
#   --list       print every step with its command, prerequisites and how its result is judged; run nothing
#   --only a,b   run just these steps (also a long or quick-only step, or a manual one: that one only says why it is manual)
#   --skip a,b   leave these steps out
#   --no-build   do not run the build steps; test the binaries that are there
#   --quick      leave out the long steps (they show as SKIP); use the short run_local.sh "quick" instead of "full"
#
# Environment:  PHP=<php exe>  (else "php" on PATH; without PHP the PHP steps are SKIP)
#               S1_MIN_PCT (99.9) and S3_MIN_PCT (90) - the conformance scores that still pass
# The room-server stand-in (server/rdv_standin.py) is started on port 8787 if nothing listens there,
# and stopped at the end. If something already listens, it is used and left running.
# A step is judged by its text as well as its exit code ("failures: N", "N/M ok", "CONFORMANT"...).
# The gate starts with a clean environment: OKP_*, DPTEST_*, DPLAYX_* and TCP_RUN_* are unset.
# Nothing here makes sound or opens a window; steps that would (moviehelper_test, preview_boxes) are manual.
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
cd "$ROOT" || exit 2

usage() {
    echo "usage: okplay/tests/release_gate.sh [--list] [--only a,b] [--skip a,b] [--no-build] [--quick] [-h]"
    echo "  --list       print every step (command, prerequisites, how it is judged); run nothing"
    echo "  --only a,b   run just these steps"
    echo "  --skip a,b   leave these steps out"
    echo "  --no-build   do not run the build steps"
    echo "  --quick      leave out the long steps (shown as SKIP)"
    echo "  -h           this text"
}

LIST=0; ONLY=; SKIP=; NO_BUILD=0; QUICK=0
while [ $# -gt 0 ]; do
    case $1 in
        --list) LIST=1 ;;
        --only) ONLY=${2-}; [ $# -ge 2 ] || { usage; exit 2; }; shift ;;
        --skip) SKIP=${2-}; [ $# -ge 2 ] || { usage; exit 2; }; shift ;;
        --only=*) ONLY=${1#*=} ;;
        --skip=*) SKIP=${1#*=} ;;
        --no-build) NO_BUILD=1 ;;
        --quick) QUICK=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

. "$ROOT/env.sh"
[ -n "$PY" ] || { echo "no Python 3 found - install one or set PY in local.env" >&2; exit 2; }
unset $(env | sed -n 's/^\(\(OKP\|DPTEST\|DPLAYX\|TCP_RUN\)_[A-Za-z0-9_]*\)=.*/\1/p')
[ -n "$MINGW32_BIN" ] && export PATH="$MINGW32_BIN:$PATH"   # the 32-bit gcc first (gamefix_test.sh and dp1switch_test.sh call plain gcc)
export PYTHONDONTWRITEBYTECODE=1

# PHP: $PHP, then php on PATH
PHP_EXE=
if [ -n "${PHP-}" ] && [ -f "$PHP" ]; then PHP_EXE=$PHP
elif [ -n "${PHP-}" ] && command -v "$PHP" >/dev/null 2>&1; then PHP_EXE=$(command -v "$PHP")
elif command -v php >/dev/null 2>&1; then PHP_EXE=$(command -v php); fi
PHP_W=; [ -n "$PHP_EXE" ] && PHP_W=$(cygpath -m "$PHP_EXE")
PHP_SRV=$(cygpath -m "$ROOT/okplay/server/okplay.php")
SYSROOT_U=$(cygpath -u "${SYSTEMROOT:-${WINDIR:-C:\\Windows}}")
EMSDK_U=$(cygpath -u "${EMSDK:-C:\\emsdk}")
export PY PHP_W PHP_SRV ROOT
export S1_MIN_PCT=${S1_MIN_PCT:-99.9} S3_MIN_PCT=${S3_MIN_PCT:-90}

OUT=$HERE/out/gate
STANDIN_PORT=8787

# ---- the steps -----------------------------------------------------------------------------
# def NAME KIND SECONDS "NEEDS" "JUDGE" "COMMAND" "NOTE"
#   KIND   build | test | long (left out by --quick) | quick (only with --quick) | manual (never run)
#   NEEDS  space-separated: mingw32 php dplay emsdk standin file:<path> cmd:<name> free:<port>
#   JUDGE  how the log is read: exit | grep:RE | grepn:N:RE | failures0[:N] | tcp | unittest | apidiff | nat | s1 | s3 | s4
N=0
def() { S_NAME[N]=$1; S_KIND[N]=$2; S_TO[N]=$3; S_NEEDS[N]=$4; S_JUDGE[N]=$5; S_CMD[N]=$6; S_NOTE[N]=${7-}; N=$((N + 1)); }

DROPIN="file:winedplay/bin/dropin/dplayx.dll"
DPTEST="file:okplay/bin/dptest.exe"

# builds
def build_okplay    build 900  "mingw32" exit 'bash okplay/build.sh'
def build_winedplay build 1800 "mingw32" exit 'bash winedplay/build.sh'
def build_wsock     build 600  "mingw32" exit 'bash okplay/wsock/build.sh'
def build_dpwsockx  build 900  "mingw32" exit 'bash winedplay/build_dpwsockx.sh' "Wine's TCP/IP provider, for s4_full"
def build_s1test    build 600  "mingw32" exit 'bash winedplay/build_tests.sh'    "Wine's dplayx test exe, for s1"
def build_sptap     build 300  "mingw32" exit 'bash conformance/sptap/build.sh'  "the wire tap, for s3"
def build_okweb     build 900  "emsdk"   exit 'bash okweb/dplay/test/build_test.sh' "browser (WebAssembly) build; SKIP without Emscripten"

# short checks, no network
def site_upload_test test 120 "" unittest '"$PY" okplay/tests/site_upload_test.py' "fake FTP, no network"
def test_names       test 60  "file:okplay/tests/test_names.exe" grep:'^PASS' 'okplay/tests/test_names.exe'
def test_compat      test 120 "file:okplay/bin/test_compat.exe" grepn:2:'^RESULT ok' 'okplay/bin/test_compat.exe && OKP_EMULATE_9X=1 okplay/bin/test_compat.exe' "normal and Windows-9x code paths"
def fuzz             test 600 "file:okplay/bin/fuzz.exe" exit '(cd okplay/tests && ../bin/fuzz.exe 20000 1 && { ../bin/fuzz.exe --control; [ $? = 1 ]; })' "parser fuzzer, 20000 inputs per target, fixed seed; the control run must be caught (exit 1)"

# room server: stand-in on 8787 (started by the gate), then okplay.php on PHP
def rdv_conformance  test 120 "standin" grep:'^CONFORMANT' '"$PY" okplay/tests/rdv_conformance.py http://127.0.0.1:8787/okplay'
def server_php_test  test 300 "php free:8791" failures0 '"$PY" okplay/tests/server_php_test.py "$PHP_W" 8791'
def server_abuse_test test 600 "php free:8796" failures0 '"$PY" okplay/tests/server_abuse_test.py "$PHP_W" 8796'

# DirectPlay drop-in
def dp_hostile       test 900 "mingw32" grep:'^dp_hostile: PASS' 'bash winedplay/tests/dp_hostile.sh' "builds its own test exes into winedplay/bin/tests"
def gamefix_test     test 240 "mingw32 cmd:python $DROPIN" grep:'^gamefix_test: 0 failed' 'bash okplay/tests/gamefix_test.sh'
def dp1switch_test   test 240 "mingw32 file:winedplay/bin/dropin/dplay.dll $DROPIN" grep:'^dp1switch_test: 0 failed' 'bash okplay/tests/dp1switch_test.sh'
def askagain_test    test 120 "file:okplay/bin/askagain.exe $DROPIN" grep:'^askagain: PASS' 'bash okplay/tests/askagain_test.sh'
def report_test      test 600 "free:8794 $DROPIN $DPTEST" failures0 'bash okplay/tests/report_test.sh' "starts its own stand-in on 8794"
def enumfilter_test  test 180 "dplay $DROPIN $DPTEST" grep:'^enumfilter: PASS' 'bash okplay/tests/enumfilter_test.sh'
def dp_askold        test 180 "dplay mingw32 $DROPIN" grep:'^dp_askold: PASS' 'bash okplay/tests/dp_askold.sh'
def apidiff          test 600 "dplay file:okplay/bin/apistate.exe $DROPIN" apidiff 'bash okplay/tests/apidiff.sh' "exit 0 is not a pass: the gate reads the 'N unexpected' count"

# DirectPlay drop-in, scenarios on this PC (several dptest processes)
def run_local_quick  quick 1500 "standin $DPTEST $DROPIN file:okplay/bin/dpwsockx.dll mingw32" failures0 'bash okplay/tests/run_local.sh quick' "~4 min; only with --quick (the full run covers it)"
def run_local_full   long 5400 "standin $DPTEST $DROPIN file:okplay/bin/dpwsockx.dll mingw32" failures0 'bash okplay/tests/run_local.sh full' "includes the core scenario (core_test.c)"
def run_local_extra  long 3000 "standin $DPTEST $DROPIN file:okplay/bin/dpwsockx.dll mingw32" failures0:6 'for s in burst roomstate buildcheck blackout v6 loss; do bash okplay/tests/run_local.sh $s; done' "scenarios that run_local.sh full does not include; v6 needs IPv6 on this PC"

# Winsock shim
def tcp_php          test 1200 "php free:8790 file:okplay/wsock/bin/tcpsim.exe file:okplay/wsock/bin/wsock32.dll" tcp 'ONLINE=$("$PY" -B -c "import sys; sys.path.insert(0, \"okplay/wsock/tests\"); import tcp_run; print(\",\".join(n for n, s in tcp_run.SCENARIOS.items() if s.get(\"online\")))") && [ -n "$ONLINE" ] && TCP_RUN_PHP_SERVER="$PHP_SRV" TCP_RUN_PHP="$PHP_W" "$PY" okplay/wsock/tests/tcp_run.py --mode shim --only "$ONLINE"' "the *_online scenarios against okplay.php on php -S instead of the stand-in"
def tcp_shim         long 2400 "free:8790 file:okplay/wsock/bin/tcpsim.exe file:okplay/wsock/bin/wsock32.dll" tcp '"$PY" okplay/wsock/tests/tcp_run.py --mode shim' "55 scenarios, ~11 min; the *_online ones use tcp_run's own stand-in on 8790"

# NAT traversal
def nat_matrix       long 1500 "file:okplay/bin_matrix/dptest.exe file:okplay/bin_matrix/dpwsockx.dll free:8800" nat '"$PY" okplay/tests/nat_matrix.py' "25 router-type pairs, ~7 min; each pair has its own stand-in (ports 8800-8849)"

# conformance against Windows' DirectPlay
def s3               test 1500 "dplay $DPTEST file:winedplay/bin/plain/dplayx.dll file:conformance/sptap/bin/dpwsockx.dll" s3 'bash conformance/run_s3.sh all' "wire comparison; passes at S3_MIN_PCT per comparison, 9 comparisons expected"
def s4_full          test 900 "dplay $DPTEST file:winedplay/bin/plain/dplayx.dll file:winedplay/bin/dpwsockx/dpwsockx.dll" s4 'bash conformance/run_s4.sh full' "8 mixed host/joiner runs; win/win is the control"
def s1               long 4500 "dplay file:winedplay/bin/tests/dplayx_test.exe file:winedplay/bin/plain/dplayx.dll" s1 'bash conformance/run_s1.sh windows && bash conformance/run_s1.sh plain && "$PY" conformance/s1_compare.py conformance/results/s1_windows.txt conformance/results/s1_plain.txt' "Wine's API tests on Windows' dplayx and on ours, 10-20 min each; must run alone"

# manual: not run by the gate (shown by --list; --only says why)
def okweb_test       manual 0 "" exit 'bash okweb/tests/run_okweb_test.sh' "serves the page and then waits forever (wait); the result appears in a browser page (\"done: N passed, 0 failed\"). A headless-browser driver (like okweb/integrations/rogan2/two_tab_test.py's Chrome control) would make it a gate step."
def okweb_two_tab    manual 0 "" exit 'python okweb/integrations/rogan2/two_tab_test.py --runs 10 --game <port> --rdv <port>' "needs a game's WebAssembly build served by serve_game.py and the websocket-client module; it does use headless Chrome with --mute-audio"
def dpweb_test       manual 0 "" exit 'bash okweb/dplay/test/build_test.sh' "two-tab browser test (dpweb_test.html): opened by hand in two tabs"
def moviehelper_test manual 0 "" exit 'bash okplay/tests/moviehelper_test.sh' "plays AoE2's intro movie through MCI (moviehost.c:41-47 hides the window but does not turn the audio off) - it would make sound; needs the game's AVI"
def soak             manual 0 "" exit 'bash okplay/tests/soak.sh 60' "an hour of three players; memory flatness is judged by reading memory.csv, not by the script"
def preview_boxes    manual 0 "" exit 'bash okplay/tests/preview_boxes.sh' "shows every dialog for a visual check; asserts nothing"
def rr_bench         manual 0 "" exit 'python okplay/wsock/tests/rr_bench.py --host <vm> --join <vm>' "benchmark on two Windows 98 VMs (okagent); prints a table, no pass/fail"
def rrsim            manual 0 "" exit 'okplay/wsock/bin/rrsim.exe host NAME 40 / rrsim.exe join 40' "built by wsock/build.sh, prints RESULT ok, but needs the shim in its folder, an okplay.ini and OKP_WSOCK_MODE/OKP_TEST_* answers (a harness like tcp_run.py's folder()); none exists yet"
def s4_plain         manual 0 "" exit 'bash conformance/run_s4.sh plain' "known 4 of 8 on Windows 10 (Windows' own provider refuses our joiner; parked in docs/DPLAY_CONFORMANCE.md)"
def relay_bench      manual 0 "" exit 'python okplay/tests/relay_bench.py <okplay.php URL>' "a benchmark (round trips); php_pool.py is its helper"
def realgame_tests   manual 0 "" exit 'winedplay/tests/realgame_*.py, necrodome/' "need VMs or installed games"

# ---- helpers ---------------------------------------------------------------------------------
in_list() { case ",$2," in *",$1,"*) return 0 ;; esac; return 1; }
port_open() { (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }
standin_answers() { "$PY" -B -c "
import sys, urllib.request, urllib.error
try: urllib.request.urlopen('http://127.0.0.1:$STANDIN_PORT/okplay', b'a=list', timeout=3)
except urllib.error.HTTPError: pass
except Exception: sys.exit(1)" 2>/dev/null; }

unmet() { # needs -> prints the first unmet prerequisite
    local t
    for t in $1; do
        case $t in
            mingw32) [ -n "$MINGW32_BIN" ] || { echo "no 32-bit gcc (MINGW32_BIN)"; return; } ;;
            php) [ -n "$PHP_EXE" ] || { echo "no PHP (set PHP= or put php on PATH)"; return; } ;;
            dplay) [ -f "$SYSROOT_U/SysWOW64/dpwsockx.dll" ] || { echo "Windows' DirectPlay feature is not installed"; return; } ;;
            emsdk) [ -f "$EMSDK_U/upstream/emscripten/emcc.exe" ] || { echo "no Emscripten (EMSDK)"; return; } ;;
            file:*) [ -f "${t#file:}" ] || { echo "missing ${t#file:} (not built?)"; return; } ;;
            cmd:*) command -v "${t#cmd:}" >/dev/null 2>&1 || { echo "no ${t#cmd:} on PATH"; return; } ;;
            free:*) if port_open "${t#free:}"; then echo "port ${t#free:} is in use"; return; fi ;;
        esac
    done
}

# which steps run: sets SEL[i] = run | off:<why> | manual
select_steps() {
    local i n k
    for ((i = 0; i < N; i++)); do
        n=${S_NAME[i]}; k=${S_KIND[i]}; SEL[i]=run
        if [ -n "$ONLY" ] && ! in_list "$n" "$ONLY"; then SEL[i]="off:not in --only"; continue; fi
        if [ -n "$SKIP" ] && in_list "$n" "$SKIP"; then SEL[i]="off:--skip"; continue; fi
        if [ "$k" = build ] && [ $NO_BUILD = 1 ]; then SEL[i]="off:--no-build"; continue; fi
        if [ "$k" = manual ]; then SEL[i]=manual; [ -z "$ONLY" ] && SEL[i]="off:manual"; continue; fi
        [ -n "$ONLY" ] && continue   # named by hand: runs even if long or quick-only
        if [ "$k" = long ] && [ $QUICK = 1 ]; then SEL[i]="off:long (--quick)"; continue; fi
        if [ "$k" = quick ] && [ $QUICK = 0 ]; then SEL[i]="off:quick-only"; continue; fi
    done
}

# check --only / --skip names
for opt in "$ONLY" "$SKIP"; do
    for n in ${opt//,/ }; do
        ok=0; for ((i = 0; i < N; i++)); do [ "${S_NAME[i]}" = "$n" ] && ok=1; done
        [ $ok = 1 ] || { echo "unknown step: $n (see --list)" >&2; exit 2; }
    done
done
select_steps

# ---- --list ------------------------------------------------------------------------------------
if [ $LIST = 1 ]; then
    if port_open $STANDIN_PORT; then echo "room-server stand-in: something already listens on $STANDIN_PORT - it would be used and left running"
    else echo "room-server stand-in: port $STANDIN_PORT is free - the gate would start rdv_standin.py there when a step needs it, and stop it at the end"; fi
    echo "PHP: ${PHP_EXE:-(not found: the PHP steps would be SKIP)}"
    echo "32-bit gcc: ${MINGW32_BIN:-(not found)}"
    echo "logs: okplay/tests/out/gate/<step>.log"
    echo
    for ((i = 0; i < N; i++)); do
        s=${SEL[i]}; case $s in run) tag="runs" ;; manual) tag="manual (named in --only)" ;; off:manual) tag="manual - never run" ;; off:*) tag="not run: ${s#off:}" ;; esac
        printf '%s  [%s%s]  %s\n' "${S_NAME[i]}" "${S_KIND[i]}" "$([ "${S_TO[i]}" != 0 ] && echo ", limit ${S_TO[i]}s")" "$tag"
        printf '    command: %s\n' "${S_CMD[i]}"
        if [ "${S_KIND[i]}" != manual ]; then
            printf '    needs:   %s\n' "${S_NEEDS[i]:-nothing}"
            w=$(unmet "$(echo "${S_NEEDS[i]}" | sed 's/standin//')"); [ -n "$w" ] && printf '    now:     would be SKIP - %s\n' "$w"
            printf '    judged:  %s\n' "${S_JUDGE[i]}"
        fi
        [ -n "${S_NOTE[i]}" ] && printf '    note:    %s\n' "${S_NOTE[i]}"
    done
    exit 0
fi

# ---- running -----------------------------------------------------------------------------------
mkdir -p "$OUT"
STANDIN_PID=
STANDIN_MINE=0
stop_pid() { # a background process of ours, and (Windows Python) its Windows process
    local wp; wp=$(cat "/proc/$1/winpid" 2>/dev/null)
    kill "$1" 2>/dev/null
    [ -n "$wp" ] && taskkill //F //T //PID "$wp" >/dev/null 2>&1
    return 0
}
cleanup() { [ -n "$STANDIN_PID" ] && stop_pid "$STANDIN_PID"; STANDIN_PID=; }
trap cleanup EXIT
trap 'echo; echo "interrupted"; exit 130' INT TERM

STANDIN_NOTE=
ensure_standin() { # -> 0 when a stand-in answers on 8787
    if [ -n "$STANDIN_NOTE" ]; then standin_answers; return; fi
    if port_open $STANDIN_PORT; then
        STANDIN_NOTE="room-server stand-in: already listening on $STANDIN_PORT - used, left running"
    else
        "$PY" -B "$ROOT/okplay/server/rdv_standin.py" --port $STANDIN_PORT > "$OUT/standin.log" 2>&1 &
        STANDIN_PID=$!
        STANDIN_NOTE="room-server stand-in: started by the gate on $STANDIN_PORT (stopped at the end)"
    fi
    echo "$STANDIN_NOTE"
    local t; for t in $(seq 1 30); do standin_answers && return 0; sleep 0.5; done
    return 1
}

trim() { cut -c1-120; }
last_line() { grep -v '^[[:space:]]*$' "$1" | tail -1 | sed 's/^[[:space:]]*//' | trim; }
fail_line() { # log rc
    local l; l=$(grep -m1 -E 'FAIL|NOT CONFORMANT|Traceback|rror:|DIFFERS|UNEXPECTED|timed out' "$1" | sed 's/^[[:space:]]*//' | trim)
    [ -n "$l" ] && echo "$l" || echo "exit $2: $(last_line "$1")"
}
num_ge() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a + 0 >= b + 0) }'; }

R=; D=
judge() { # spec rc log limit -> R, D
    local spec=$1 rc=$2 log=$3 kind=${1%%:*} arg= line n m c bad
    [[ $spec == *:* ]] && arg=${spec#*:}
    R=FAIL; D=
    if [ "$rc" = 124 ] || [ "$rc" = 137 ]; then D="timed out after ${4}s"; return; fi
    case $kind in
    exit)
        if [ "$rc" = 0 ]; then R=PASS; D=$(last_line "$log"); else D=$(fail_line "$log" "$rc"); fi ;;
    grep)
        if [ "$rc" = 0 ] && grep -Eq -- "$arg" "$log"; then R=PASS; D=$(grep -E -- "$arg" "$log" | tail -1 | trim)
        else D=$(fail_line "$log" "$rc"); fi ;;
    grepn)
        n=${arg%%:*}; arg=${arg#*:}
        c=$(grep -Ec -- "$arg" "$log")
        if [ "$rc" = 0 ] && [ "$c" = "$n" ]; then R=PASS; D="$c of $n runs: $(grep -E -- "$arg" "$log" | tail -1 | trim)"
        else D="$(fail_line "$log" "$rc") ($c of $n)"; fi ;;
    failures0)
        n=${arg:-1}
        c=$(grep -Ec '^failures: [0-9]+' "$log")
        bad=$(grep -E '^failures: [0-9]+' "$log" | grep -Ecv '^failures: 0([^0-9]|$)')
        if [ "$rc" = 0 ] && [ "$c" = "$n" ] && [ "$bad" = 0 ]; then
            R=PASS; D="failures: 0 ($(grep -c '^PASS' "$log") PASS, $(grep -c '^SKIP' "$log") SKIP lines)"
        elif [ "$c" != "$n" ] && [ "$bad" = 0 ]; then D="$c of $n 'failures:' lines (exit $rc): $(fail_line "$log" "$rc")"
        else D=$(fail_line "$log" "$rc"); fi ;;
    tcp)
        line=$(grep -E '^[0-9]+/[0-9]+ ok' "$log" | tail -1)
        if [[ $line =~ ^([0-9]+)/([0-9]+)\ ok ]]; then n=${BASH_REMATCH[1]}; m=${BASH_REMATCH[2]}; else n=0; m=0; fi
        if [ "$rc" = 0 ] && [ "$m" -gt 0 ] && [ "$n" = "$m" ]; then R=PASS; D=$(echo "$line" | trim)
        else D=$(grep -m1 -E '^[a-z0-9_]+ +(real|shim) +FAIL' "$log" | trim); [ -n "$D" ] || D="${line:-no 'N/M ok' line (exit $rc)}"; fi ;;
    unittest)
        if [ "$rc" = 0 ] && grep -q '^OK' "$log"; then R=PASS; D="$(grep -E '^Ran [0-9]+ tests' "$log" | tail -1), OK"
        else D=$(fail_line "$log" "$rc"); fi ;;
    apidiff)
        line=$(grep -E '^apidiff: .*unexpected\)' "$log" | tail -1)
        n=$(echo "$line" | sed -n 's/.*, \([0-9][0-9]*\) unexpected).*/\1/p')
        if [ "$rc" = 0 ] && [ "$n" = 0 ]; then R=PASS; D=$(echo "${line#apidiff: }" | trim)
        else D="${line:-no summary line} (exit $rc)"; fi ;;
    nat)
        if [ "$rc" = 0 ] && grep -q 'matches theory' "$log"; then R=PASS; D=$(grep 'pairs connected' "$log" | tail -1 | trim)
        else D=$(grep -m1 -E 'UNEXPECTED|DIFFERS' "$log" | trim); [ -n "$D" ] || D=$(fail_line "$log" "$rc"); fi ;;
    s4)
        line=$(grep -E '^S4 \([a-z]+\): [0-9]+ of [0-9]+ scenarios pass' "$log" | tail -1)
        if [[ $line =~ :\ ([0-9]+)\ of\ ([0-9]+)\ scen ]]; then n=${BASH_REMATCH[1]}; m=${BASH_REMATCH[2]}; else n=0; m=0; fi
        if [ "$m" -gt 0 ] && [ "$n" = "$m" ]; then R=PASS; D=$line
        else D="${line:-no summary line}$(grep -m1 '^FAIL' "$log" | sed 's/^/; first /' | trim)"; fi ;;
    s1)
        line=$(grep -E '^S1: ' "$log" | tail -1)
        n=$(echo "$line" | sed -n 's/.*(\([0-9.]*\)%).*/\1/p')
        if [ "$rc" = 0 ] && [ -n "$n" ] && num_ge "$n" "$S1_MIN_PCT"; then R=PASS; D="${n}% (need $S1_MIN_PCT): ${line#S1: }"
        else D="${line:-no S1 line (exit $rc)} (need $S1_MIN_PCT%)"; fi ;;
    s3)
        c=$(grep -c '^S3 ' "$log")
        n=$(grep '^S3 ' "$log" | sed -n 's/.*(\([0-9.]*\)%).*/\1/p' | sort -n | head -1)
        if [ "$c" -ge 9 ] && [ -n "$n" ] && num_ge "$n" "$S3_MIN_PCT"; then R=PASS; D="$c comparisons, lowest ${n}% (need $S3_MIN_PCT)"
        else D="$c of 9 comparisons, lowest ${n:-none}% (need $S3_MIN_PCT)"; fi ;;
    *) D="unknown judge $spec" ;;
    esac
}

declare -a T_NAME T_RES T_SEC T_DET
add_row() { T_NAME+=("$1"); T_RES+=("$2"); T_SEC+=("$3"); T_DET+=("$4"); }

run_step() { # index
    local i=$1 name=${S_NAME[$1]} k=${S_KIND[$1]} why log rc t0 needs
    log="$OUT/$name.log"
    if [ "${SEL[i]}" = manual ]; then add_row "$name" SKIP 0 "manual: ${S_NOTE[i]}"; echo "SKIP  $name (manual)"; return; fi
    needs=${S_NEEDS[i]//standin/}
    why=$(unmet "$needs")
    if [ -n "$why" ]; then
        if [ "$k" = build ] && [[ ${S_NEEDS[i]} == *mingw32* ]] && [[ $why == "no 32-bit gcc"* ]]; then
            add_row "$name" FAIL 0 "$why"; echo "FAIL  $name - $why"
        else add_row "$name" SKIP 0 "$why"; echo "SKIP  $name - $why"; fi
        return
    fi
    if [[ ${S_NEEDS[i]} == *standin* ]] && ! ensure_standin; then
        add_row "$name" FAIL 0 "the room-server stand-in does not answer on $STANDIN_PORT (see out/gate/standin.log)"
        echo "FAIL  $name - stand-in does not answer"; return
    fi
    echo "run   $name (limit ${S_TO[i]}s) ..."
    t0=$SECONDS
    ( cd "$ROOT" && timeout -k 15 "${S_TO[i]}" bash -c "${S_CMD[i]}" ) < /dev/null > "$log" 2>&1
    rc=$?
    judge "${S_JUDGE[i]}" "$rc" "$log" "${S_TO[i]}"
    add_row "$name" "$R" $((SECONDS - t0)) "$D"
    echo "$R  $name  ($((SECONDS - t0)) s)  $D"
}

echo "okplay release gate, $(date '+%Y-%m-%d %H:%M')  (logs: okplay/tests/out/gate/)"
[ $NO_BUILD = 1 ] && echo "--no-build: the binaries that are there are tested"
if [ $QUICK = 1 ]; then
    q=; for ((i = 0; i < N; i++)); do [ "${S_KIND[i]}" = long ] && [ -z "$ONLY" ] && q="$q ${S_NAME[i]}"; done
    echo "--quick leaves out these long steps:${q:- (none)}"
fi
echo "PHP: ${PHP_EXE:-not found (PHP steps are SKIP)}"
echo
for ((i = 0; i < N; i++)); do
    case ${SEL[i]} in
        run|manual) run_step "$i" ;;
        off:long*) add_row "${S_NAME[i]}" SKIP 0 "long step, left out by --quick"; echo "SKIP  ${S_NAME[i]} - long, left out by --quick" ;;
    esac
done

# ---- the table ---------------------------------------------------------------------------------
np=0; nf=0; ns=0
for r in "${T_RES[@]}"; do case $r in PASS) np=$((np + 1)) ;; FAIL) nf=$((nf + 1)) ;; SKIP) ns=$((ns + 1)) ;; esac; done
{
    echo
    echo "okplay release gate - results ($(date '+%Y-%m-%d %H:%M'))"
    [ -n "$STANDIN_NOTE" ] && echo "$STANDIN_NOTE"
    printf '%-20s %-5s %6s  %s
' STEP RESULT SEC DETAIL
    printf '%-20s %-5s %6s  %s
' -------------------- ----- ------ ------
    for ((i = 0; i < ${#T_NAME[@]}; i++)); do
        printf '%-20s %-5s %6s  %s
' "${T_NAME[i]}" "${T_RES[i]}" "${T_SEC[i]}" "${T_DET[i]}"
    done
    echo
    echo "$np PASS, $nf FAIL, $ns SKIP - $([ $nf = 0 ] && echo "gate OK" || echo "gate FAILED")"
} | tee "$OUT/summary.txt"
[ $nf = 0 ]
