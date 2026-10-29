#!/usr/bin/env bash
# The name server's EnumSessions filter (winedplay NS_ReplyToEnumSessionsRequest), as Windows does it:
# a started game (DPSESSION_JOINDISABLED) is NOT listed by a default enumeration and IS listed by one
# that says DPENUMSESSIONS_ALL; an open game is listed either way.
#
# Runs over Windows' own TCP/IP provider under our dplayx (okplay.ini mode=oldkid +
# show_windows_connections=1), because on Online/LAN Play okplay's service provider applies the same
# rule itself (sp.c room_matches) and would hide the Wine-side check. Needs Windows' DirectPlay
# (Legacy Components) on this PC. The default-vs-ALL contrast on one and the same session is what
# proves the filter is live (2026-10-04: the build before it listed the started game both ways).
#   enumfilter_test.sh [dplayx.dll]      default: ../winedplay/bin/dropin/dplayx.dll
set -e
cd "$(dirname "$0")/.."
DLL="${1:-../winedplay/bin/dropin/dplayx.dll}"
T=tests/out/enumfilter
mkdir -p "$T"
cp "$DLL" "$T/dplayx.dll"
printf '[okplay]\r\nmode=oldkid\r\nshow_windows_connections=1\r\nloglevel=3\r\n' > "$T/okplay.ini"
D="$(cygpath -w "$PWD/$T/dplayx.dll")"
export OKP_SERVER=http://127.0.0.1:9/nothing OKP_STUN=
unset OKP_TEST_AS
fail=0

listing() { # eflags -> "ok sessions=N" / "fail sessions=0"
    DPTEST_DPLAYX="$D" bin/dptest.exe --sp tcpip --ip 127.0.0.1 list --name L --secs 4 ${1:+--eflags $1} 2>&1 |
        grep -o "RESULT.*" | sed 's/RESULT //'
}
expect() { # label got want
    if [[ "$2" == "$3"* ]]; then echo "  ok    $1: $2"; else echo "  FAIL  $1: got '$2', want '$3'"; fail=1; fi
}

echo "--- started game (session flags 0x64 = KEEPALIVE|JOINDISABLED|MIGRATEHOST)"
DPTEST_DPLAYX="$D" bin/dptest.exe --sp tcpip host --name Started --secs 14 --sflags 0x64 > "$T/host1.txt" 2>&1 &
sleep 4
expect "default flags hide it"      "$(listing)"     "fail sessions=0"
expect "DPENUMSESSIONS_ALL lists it" "$(listing 0x2)" "ok sessions=1"
wait

echo "--- open game (session flags 0x44)"
DPTEST_DPLAYX="$D" bin/dptest.exe --sp tcpip host --name Open --secs 9 --sflags 0x44 > "$T/host2.txt" 2>&1 &
sleep 4
expect "default flags list it" "$(listing)" "ok sessions=1"
wait

[ $fail = 0 ] && echo "enumfilter: PASS" || echo "enumfilter: FAIL"
exit $fail
