#!/usr/bin/env bash
# classic_*=ask asks again once the game has released the DirectPlay object it answered for
# (tests/askagain.c). Red on a build without DP_BUILTIN_RemapForget (asked once), green with it.
#   askagain_test.sh [dplayx.dll]      default: ../winedplay/bin/dropin/dplayx.dll
set -e
cd "$(dirname "$0")/.."
DLL="${1:-../winedplay/bin/dropin/dplayx.dll}"
T=tests/out/askagain
mkdir -p "$T"
find "$T" -name "*.log" -delete 2>/dev/null || true
cp "$DLL" "$T/dplayx.dll"
printf '[okplay]\r\nmode=all\r\nclassic_tcpip=ask\r\nloglevel=3\r\n' > "$T/okplay.ini"
export OKP_SERVER=http://127.0.0.1:9/nothing OKP_STUN= OKP_TEST_CLASSIC_ASK=lan
bin/askagain.exe "$(cygpath -w "$PWD/$T/dplayx.dll")" | sed 's/^/    /'
N=$(cat "$T"/netplay-logs/*.log 2>/dev/null | grep -c "the player chose" || true)
echo "    the question was put $N time(s) in two rounds"
if [ "$N" = 2 ]; then echo "askagain: PASS"; else echo "askagain: FAIL (want 2)"; exit 1; fi
