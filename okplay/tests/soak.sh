#!/usr/bin/env bash
# Soak: 3 players for N minutes with mild loss; samples every dptest's memory once a minute.
#   tests/soak.sh [minutes]      (DROP=3 by default; needs the rendezvous stand-in on :8787)
# Pass = all three RESULT ok, memory flat, no gave-up/window-full/reassembly errors in the logs.
MIN=${1:-60}
cd "$(dirname "$0")"
OUT=out/soak
rm -rf "$OUT"; mkdir -p "$OUT"
export OKP_TEST_AS=online OKP_SERVER=${OKP_SERVER:-http://127.0.0.1:8787/okplay} OKP_DROP_PCT=${DROP:-3}
SECS=$((MIN * 60))
../bin/dptest.exe --sp ipx host --name H --secs $((SECS + 40)) --expect 3 > "$OUT/host.txt" 2>&1 &
sleep 3
../bin/dptest.exe --sp ipx join --name A --secs $((SECS + 20)) > "$OUT/a.txt" 2>&1 &
sleep 2
../bin/dptest.exe --sp ipx join --name B --secs $SECS > "$OUT/b.txt" 2>&1 &
sleep 10
echo "minute,image,pid,session,num,mem" > "$OUT/memory.csv"
for ((m = 0; m <= MIN; m++)); do
    tasklist //FI "IMAGENAME eq dptest.exe" //FO CSV //NH | tr -d '\r' | sed "s/^/$m,/" >> "$OUT/memory.csv"
    sleep 60
done
wait
for f in host a b; do printf "%-5s %s\n" "$f" "$(grep -o 'RESULT.*' $OUT/$f.txt)"; done
