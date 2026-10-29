#!/usr/bin/env bash
# Show every drop-in dialog for real, one after another, for a visual check (nothing is asserted):
#   1 host password   2 join password   3 wrong password   4 update notice
#   5 TCP/IP not working   6 server unreachable   7 different game version (build_check)
#   8 send a crash report?   9 send connection reports?
# Each waits for you to close it. Uses the fresh drop-in build (winedplay/bin/dropin) and starts a
# test room server on port 8788 that advertises v9.9.9. Usage: tests/preview_boxes.sh [first-box#]
cd "$(dirname "$0")/.."
. ../env.sh
DLL_SRC=../winedplay/bin/dropin/dplayx.dll
W=$(mktemp -d)
trap 'kill $SRV 2>/dev/null; rm -rf "$W"' EXIT
"$PY" server/rdv_standin.py --port 8788 --latest 9.9.9 \
    --latest-note "Example note: what's new in this version." > "$W/server.log" 2>&1 &
SRV=$!
sleep 2
FIRST=${1:-1}

dir() {   # a folder with its own copy of the DLL and an okplay.ini pointing at $1
    local d="$W/$2"
    mkdir -p "$d"; cp "$DLL_SRC" "$d/"
    printf '[okplay]\r\nserver=%s\r\nbrand=Oldkid.net Online Play\r\n' "$1" > "$d/okplay.ini"
    echo "$d"
}
dp() { ./bin/dptest.exe --dplayx "$1/dplayx.dll" --sp okplay "${@:2}"; }
step() { [ "$1" -ge "$FIRST" ] && echo ">> $1: $2"; [ "$1" -ge "$FIRST" ]; }

S=http://127.0.0.1:8788/okplay
if step 1 "host password box (type anything, then Host or No Password)"; then
    D=$(dir $S host1); OKP_PREVIEW_BOXES=1 dp "$D" --name Preview --secs 2 host > /dev/null
fi
if step 2 "join password box - type a WRONG password and press Join"; then
    H=$(dir $S host2); J=$(dir $S join2)
    OKP_TEST_HOST_PASSWORD=right dp "$H" --name PwGame --secs 90 host > /dev/null &
    HP=$!; sleep 4
    echo ">> 3: then the wrong-password box"
    OKP_PREVIEW_BOXES=1 dp "$J" --name Joiner --session "dptest PwGame" --secs 20 join > /dev/null
    kill $HP 2>/dev/null; wait $HP 2>/dev/null
fi
if step 4 "update notice (pretends v9.9.9 is out)"; then
    D=$(dir $S upd); OKP_PREVIEW_BOXES=1 dp "$D" --app any --secs 3 list > /dev/null
fi
if step 5 "TCP/IP not working (simulated)"; then
    D=$(dir $S nonet); OKP_PREVIEW_BOXES=1 OKP_TEST_NONET=1 dp "$D" --name NoNet --secs 2 host > /dev/null
fi
if step 6 "can't reach the server (points at a closed port)"; then
    D=$(dir http://127.0.0.1:9/okplay unreach); OKP_PREVIEW_BOXES=1 dp "$D" --app any --secs 10 list > /dev/null
fi
if step 7 "different game version (the joiner pretends another exe build)"; then
    S7=http://127.0.0.1:8787/okplay   # (the main stand-in: no update notice first; tests/run_local.sh needs it too)
    H=$(dir $S7 host7); J=$(dir $S7 join7)
    dp "$H" --name Build --secs 60 host > /dev/null &
    HP=$!; sleep 4
    OKP_PREVIEW_BOXES=1 OKP_TEST_EXEBUILD=0123456789abcdef dp "$J" --name Joiner --session "dptest Build" --secs 5 join > /dev/null
    kill $HP 2>/dev/null; wait $HP 2>/dev/null
fi
if step 8 "crash report (after a crash on purpose; try View Report)"; then
    D=$(dir $S crash8)
    OKP_CRASHTEST=1 dp "$D" --name C --secs 2 host > /dev/null 2>&1
    OKP_REPORT_SYNC=1 OKP_TEST_CRASH_REPORT=ask dp "$D" --name C --secs 2 host > /dev/null
fi
if step 9 "connection reports, asked once after the first game"; then
    D=$(dir $S conn9)
    OKP_TEST_CONN_REPORTS=ask dp "$D" --name First --secs 2 host > /dev/null
    OKP_REPORT_SYNC=1 OKP_TEST_CONN_REPORTS=ask dp "$D" --name Second --secs 2 host > /dev/null
fi
echo "done"
