#!/usr/bin/env bash
# The movie helper (codec/moviehelper.c -> bin/moviehelper/version.dll), red then green:
#   1. a player program with okindeo.dll beside it but no helper: no decoder (the fault being fixed)
#   2. the same program with the helper's version.dll beside it: decoder opens, MCI plays through it
#      and the helper's log counts the frames; version.dll calls still answer as Windows' own do.
#   moviehelper_test.sh [movie.avi]     (default: AoE2's intro, found by env.sh)
set -e
cd "$(dirname "$0")/.."
. ../env.sh
AVI="${1:-$AOE2_GAME_DIR/../Avi/Age2_X1.avi}"
[ -f "$AVI" ] || { echo "no test movie: $AVI"; exit 2; }
T=tests/out/moviehelper
rm -rf "$T"; mkdir -p "$T"
cp bin/moviehost.exe bin/okindeo.dll "$T/"
W=$(cygpath -w "$AVI")
fail=0

echo "--- 1. without the helper (must have NO picture: proves the test can fail)"
( cd "$T" && ./moviehost.exe "$W" 1 ) | tee "$T/red.txt" || true
grep -q "RESULT NO_PICTURE" "$T/red.txt" || { echo "FAIL: expected NO_PICTURE without the helper (is an Indeo codec installed on this PC?)"; fail=1; }

echo "--- 2. with the helper"
cp bin/moviehelper/version.dll "$T/"
( cd "$T" && ./moviehost.exe "$W" 2 ) | tee "$T/green.txt" || true
grep -q "RESULT PICTURE" "$T/green.txt" || { echo "FAIL: no decoder with the helper"; fail=1; }
LOG=$(ls "$T"/netplay-logs/moviehelper-*.log 2>/dev/null | head -1)
[ -n "$LOG" ] && sed 's/^/    /' "$LOG"
N=$(grep -o "[0-9]* frames decoded in this program" "$LOG" 2>/dev/null | grep -o "^[0-9]*" || echo 0)
[ "${N:-0}" -ge 20 ] || { echo "FAIL: the helper's log counts ${N:-0} frames (want 20+)"; fail=1; }
grep -q "(0 failed)" "$LOG" || { echo "FAIL: frames failed to decode"; fail=1; }
A=$(grep "^version.dll:" "$T/red.txt"); B=$(grep "^version.dll:" "$T/green.txt")
[ -n "$A" ] && [ "$A" = "$B" ] || { echo "FAIL: version.dll answers differ: '$A' vs '$B'"; fail=1; }

[ $fail = 0 ] && echo "moviehelper: PASS ($N frames decoded through the helper)" || echo "moviehelper: FAIL"
exit $fail
