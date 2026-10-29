#!/bin/bash
# A DirectPlay 1 object behind the drop-in's switcher (tests/dp1switch_test.c): trace=1 with Windows'
# connections listed, both with the drop-in's dplay.dll. Each run in its own folder; no server needed.
#   bash okplay/tests/dp1switch_test.sh      (after winedplay/build.sh; needs mingw32 gcc)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
DROPIN=$HERE/../../winedplay/bin/dropin
T=$HERE/out/dp1switch
rm -rf "$T"; mkdir -p "$T"
gcc -O2 -Wall -o "$T/dp1switch_test.exe" "$HERE/dp1switch_test.c" -luuid || exit 1
fails=0
for trace in 0 1; do
    d=$T/trace$trace; mkdir -p "$d"
    cp "$T/dp1switch_test.exe" "$DROPIN/dplay.dll" "$DROPIN/dplayx.dll" "$d/"
    printf '[okplay]\r\nshow_windows_connections=1\r\ntrace=%s\r\nuse_password=off\r\nloglevel=2\r\n' "$trace" > "$d/okplay.ini"
    out=$(cd "$d" && OKP_SERVER=http://127.0.0.1:1/none OKP_STUN= timeout 60 ./dp1switch_test.exe "$(cygpath -w "$d/dplay.dll")" 2>&1)
    if [[ "$out" == *"RESULT ok"* ]]; then echo "PASS trace=$trace"; else echo "FAIL trace=$trace"; echo "$out" | sed 's/^/    /'; fails=$((fails + 1)); fi
done
echo "dp1switch_test: $fails failed"
exit $fails
