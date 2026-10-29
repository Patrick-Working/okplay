#!/usr/bin/env bash
# S1: run Wine's dplayx conformance tests against one dplayx (docs/DPLAY_CONFORMANCE.md section 4).
#   conformance/run_s1.sh windows        Windows' own (SysWOW64) dplayx
#   conformance/run_s1.sh plain|dropin   winedplay/bin/<variant>/dplayx.dll (copied next to the test exe)
# Output: conformance/results/s1_<variant>.txt; score with s1_compare.py. Takes 10-20 minutes per run.
# Build the test first:  winedplay/build_tests.sh
set -e
cd "$(dirname "$0")"
v=${1:?windows, plain or dropin}
run=results/run_$v
rm -rf "$run"; mkdir -p "$run"
cp ../winedplay/bin/tests/dplayx_test.exe "$run/"
[ "$v" = windows ] || cp "../winedplay/bin/$v/dplayx.dll" "$run/"
cd "$run"
WT_IGNORE_FIREWALL=1 WINETEST_REPORT_SUCCESS=1 timeout ${S1_TIMEOUT:-1800} ./dplayx_test.exe dplayx > ../s1_$v.txt 2>&1 || echo "exit $? (see results/s1_$v.txt)"
tail -1 ../s1_$v.txt
