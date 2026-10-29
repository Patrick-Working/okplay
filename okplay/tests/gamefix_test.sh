#!/bin/bash
# The drop-in's game fixes (winedplay/shim/gamefix.c) touch only the exact exe they were made for.
# Runs a tiny exe of ours that loads the drop-in, under names and sizes that are close to a fixed
# game but not it, and checks each one's log: another name - no mention; the game's name but not
# its size or bytes - "left as it is". Never uses or changes a real game exe.
#   bash okplay/tests/gamefix_test.sh        (after winedplay/build.sh; needs mingw32 gcc)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
DROPIN=$HERE/../../winedplay/bin/dropin/dplayx.dll
T=$HERE/out/gamefix
rm -rf "$T"; mkdir -p "$T"
gcc -O1 -o "$T/gamefix_test.exe" "$HERE/gamefix_test.c" || exit 1
fails=0
check() { if eval "$1"; then echo "PASS $2"; else echo "FAIL $2"; fails=$((fails + 1)); fi; }

run() { # folder exe-name [pad-to-size]
    local d=$T/$1; mkdir -p "$d"
    cp "$T/gamefix_test.exe" "$d/$2"
    [ -n "${3:-}" ] && python -c "import sys; p=sys.argv[1]; n=int(sys.argv[2]); b=open(p,'rb').read(); open(p,'ab').write(b'\0'*(n-len(b)))" "$d/$2" "$3"
    cp "$DROPIN" "$d/"
    printf '[okplay]\r\nloglevel=3\r\n' > "$d/okplay.ini"
    (cd "$d" && OKP_SERVER=http://127.0.0.1:1/none ./"$2" >/dev/null)
    cat "$d"/netplay-logs/*.log 2>/dev/null
}

L=$(run other other_game.exe)
check '[[ -n "$L" && "$L" != *"game fix"* ]]' "another exe: no game fix, nothing said"
L=$(run size NECRO95.EXE)
check '[[ "$L" == *"game fix:"*"a different exe (size) - left as it is"* ]]' "NECRO95.EXE of another size: left as it is"
L=$(run bytes NECRO95.EXE 795136)
check '[[ "$L" == *"game fix:"*"a different exe (SHA-256 "*") - left as it is"* && "$L" != *"changes in memory"* ]]' \
      "NECRO95.EXE of the same size, other bytes: left as it is (its SHA-256 logged)"
echo "gamefix_test: $fails failed"
exit $fails
