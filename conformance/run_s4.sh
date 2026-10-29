#!/usr/bin/env bash
# S4 interop (docs/DPLAY_CONFORMANCE.md section 4): a host and a joiner over TCP/IP, each on Windows'
# DirectPlay or ours, plain sends and DPSEND_GUARANTEED.
#   conformance/run_s4.sh [plain|full]
#     plain  ours = our dplayx (winedplay/bin/plain) on Windows' own TCP/IP provider (dpwsockx,
#            used through its public SP interface only)
#     full   ours = our dplayx + our TCP/IP provider (Wine's, winedplay/build_dpwsockx.sh): the whole
#            stack is ours, as it has to be outside Windows; Windows = Windows' dplayx + provider
# win/win is the control: if it fails, the PC (firewall, network) is the problem, not our DLLs.
# Each side runs dptest from a folder of its own: okplay/bin holds a dpwsockx.dll that is okplay (the
# dev "IPX slot"), and dplayx loads the TCP/IP provider by that file name, exe folder first.
# Output: results/s4/<mode>_<scenario>_{host,join}.txt and a summary line per scenario.
cd "$(dirname "$0")"
mode=${1:-plain}
OUT=results/s4; mkdir -p "$OUT/win" "$OUT/ours_$mode"
cp ../okplay/bin/dptest.exe "$OUT/win/"
cp ../okplay/bin/dptest.exe ../winedplay/bin/plain/dplayx.dll "$OUT/ours_$mode/"
rm -f "$OUT/ours_$mode/dpwsockx.dll"
[ "$mode" = full ] && cp ../winedplay/bin/dpwsockx/dpwsockx.dll "$OUT/ours_$mode/"
ours=$(cygpath -w "$(pwd)/$OUT/ours_$mode/dplayx.dll")
pass=0; total=0
for g in 0 1; do
  for pair in win:win win:ours ours:win ours:ours; do
    h=${pair%:*} j=${pair#*:}
    hdir=$OUT/win; hd=; [ $h = ours ] && { hdir=$OUT/ours_$mode; hd=$ours; }
    jdir=$OUT/win; jd=; [ $j = ours ] && { jdir=$OUT/ours_$mode; jd=$ours; }
    name=${mode}_${h}_host_${j}_join_g$g
    n=H$RANDOM
    (cd "$hdir" && DPTEST_GUARANTEED=$g DPTEST_DPLAYX="$hd" ./dptest.exe --sp tcpip --ip 127.0.0.1 --name $n --secs 16 --expect 2 host) > "$OUT/${name}_host.txt" 2>&1 &
    hp=$!
    sleep 3
    (cd "$jdir" && DPTEST_GUARANTEED=$g DPTEST_DPLAYX="$jd" timeout 30 ./dptest.exe --sp tcpip --ip 127.0.0.1 --name J --session "dptest $n" --secs 9 join) > "$OUT/${name}_join.txt" 2>&1
    wait $hp
    rh=$(grep -o "RESULT.*" "$OUT/${name}_host.txt" | tail -1); rj=$(grep -o "RESULT.*" "$OUT/${name}_join.txt" | tail -1)
    total=$((total+1))
    if [ "$rh" = "RESULT ok" ] && [ "$rj" = "RESULT ok" ]; then pass=$((pass+1)); s=PASS; else s=FAIL; fi
    printf "%-4s %-32s host: %-12s join: %s\n" $s $name "${rh:-none}" "${rj:-none}"
    sleep 2
  done
done
echo "S4 ($mode): $pass of $total scenarios pass"
