#!/usr/bin/env bash
# classic_*=ask answered "Windows' DirectPlay" through an IDirectPlay2A interface (tests/dp_askold.c):
# no crash, and the game is handed to Windows' object (shim/switch.c sw_resolve_pending).
# Needs Windows' DirectPlay feature. Calls Windows' TCP/IP provider (a join with no session).
#   dp_askold.sh [dplayx.dll]      default: ../winedplay/bin/dropin/dplayx.dll
set -e
cd "$(dirname "$0")/.."
. ../env.sh
need_mingw32
DLL="${1:-../winedplay/bin/dropin/dplayx.dll}"
T=tests/out/dp_askold
mkdir -p "$T"
find "$T" -name "*.log" -delete 2>/dev/null || true
gcc -O2 -Wall -o "$T/dp_askold.exe" tests/dp_askold.c -lole32 -luuid
cp "$DLL" "$T/dplayx.dll"
printf '[okplay]\r\nmode=all\r\nclassic_tcpip=ask\r\nloglevel=3\r\n' > "$T/okplay.ini"
export OKP_SERVER=http://127.0.0.1:9/nothing OKP_STUN= OKP_TEST_CLASSIC_ASK=windows
out=$("$T/dp_askold.exe" "$(cygpath -w "$PWD/$T/dplayx.dll")" 2>&1) || true
echo "$out" | sed 's/^/    /'
moved=$(cat "$T"/netplay-logs/*.log 2>/dev/null | grep -c "this game uses Windows' DirectPlay" || true)
echo "    handed to Windows' DirectPlay: $moved time(s)"
if [[ "$out" == *"RESULT ok"* && $moved -ge 1 ]]; then echo "dp_askold: PASS"; else echo "dp_askold: FAIL"; exit 1; fi
