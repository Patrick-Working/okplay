#!/usr/bin/env bash
# S3 wire (docs/DPLAY_CONFORMANCE.md section 4): the same dptest scenario on Windows' dplayx and on
# ours, both over Windows' own TCP/IP provider with the sptap logging provider underneath, then
# s3_compare.py diffs what each dplayx handed the provider and got back, message by message.
#   conformance/run_s3.sh [pair|migrate|groups|dead|all|mixjoin]       (default all)
#     pair     host + joiner: session search, join, create player, plain sends (pings), leave
#     migrate  host + two joiners, the host quits: host migration
#     dead     pair, the joiner frozen 25 s then let go on (suspend.py): a dropped machine that talks again
#     groups   pair, plus every group call (dptest --groups): what each dplayx puts on the wire for groups
#     mixjoin  Windows hosts, our dplayx joins (on Windows' provider): our joiner vs a known-good host
#   S3_OURS_SP=ours: our side runs on our own TCP/IP provider (winedplay/bin/dpwsockx, the tap wraps
#   it; the full stack a port uses). Default: Windows' provider under both. On Windows 10 Windows'
#   provider refuses our joiner's CreatePlayer for the host (DPLAY_CONFORMANCE.md S4), so a join
#   on it never gets past that point.
# Each process runs from a folder of its own holding dptest.exe and the tap as dpwsockx.dll (dplayx
# loads the TCP/IP provider by that file name, exe folder first); ours adds winedplay/bin/plain's
# dplayx.dll. Build the tap first: conformance/sptap/build.sh
# Output: results/s3run/<impl>/<scenario>_<role>.{log,txt}, then the comparison per role.
cd "$(dirname "$0")"
want=${1:-all}
OUT=results/s3run
TAP=sptap/bin/dpwsockx.dll
[ -f "$TAP" ] || { echo "build the tap first: conformance/sptap/build.sh"; exit 2; }
PLAIN=../winedplay/bin/plain/dplayx.dll

prep() { # impl scenario role...   set up every folder before anything starts: Defender scans each
    # freshly copied unsigned DLL as it is written (seconds for a new build), which used to eat into
    # the fixed delays between host and joiners - a joiner arriving after the host had left
    local impl=$1 sc=$2 role; shift 2
    for role in "$@"; do
        local d=$OUT/$impl/${sc}_$role
        rm -rf "$d"; mkdir -p "$d"
        cp ../okplay/bin/dptest.exe "$TAP" "$d/"
        [ "$impl" = ours ] && cp "$PLAIN" "$d/"
        [ "$impl" = ours ] && [ "${S3_OURS_SP:-}" = ours ] && cp ../winedplay/bin/dpwsockx/dpwsockx.dll "$d/dpwsockx_ours.dll"
    done
    return 0
}

run() { # impl scenario role args...   (in the background; the log goes next to the folder; prep first)
    local impl=$1 sc=$2 role=$3; shift 3
    local d=$OUT/$impl/${sc}_$role
    local log; log=$(cygpath -w "$(pwd)/$OUT/$impl/${sc}_$role.log")
    local dll=; [ "$impl" = ours ] && dll=$(cygpath -w "$(pwd)/$d/dplayx.dll")
    local real=
    [ -f "$d/dpwsockx_ours.dll" ] && real=$(cygpath -w "$(pwd)/$d/dpwsockx_ours.dll")
    rm -f "$OUT/$impl/${sc}_$role.log"
    (cd "$d" && SPTAP_LOG="$log" SPTAP_REAL="$real" DPTEST_DPLAYX="$dll" timeout ${S3_TIMEOUT:-60} ./dptest.exe --sp tcpip --ip 127.0.0.1 "$@") \
        > "$OUT/$impl/${sc}_$role.txt" 2>&1 &
}

result() { grep -o "RESULT.*" "$OUT/$1/$2.txt" 2>/dev/null | tail -1; }

pair() { # impl
    local n=S3P
    prep "$1" pair host join
    run "$1" pair host --name "$n" --secs 14 --expect 2 host
    sleep 3
    run "$1" pair join --name J --session "dptest $n" --secs 9 join
    wait
    printf "  %-5s pair     host: %-10s join: %s\n" "$1" "$(result "$1" pair_host)" "$(result "$1" pair_join)"
}

groups() { # impl: pair, and the host walks through every group call; the joiner makes one group
    local n=S3G
    prep "$1" groups host join
    run "$1" groups host --name "$n" --secs 22 --expect 2 --groups host
    sleep 3
    run "$1" groups join --name J --session "dptest $n" --secs 17 --groups join
    wait
    printf "  %-5s groups   host: %-10s join: %s\n" "$1" "$(result "$1" groups_host)" "$(result "$1" groups_join)"
}

dead() { # impl: pair, the joiner frozen for 25 s (past the 15 s drop) and then let go on: what the host
    # says to a machine it dropped that talks again (YOUAREDEAD 0x18?), and what the joiner's game hears
    local n=S3D
    prep "$1" dead host join
    run "$1" dead host --name "$n" --secs $(( ${DEAD_SECS:-25} + 25 )) --expect 2 host
    sleep 3
    run "$1" dead join --name J --session "dptest $n" --secs $(( ${DEAD_SECS:-25} + 20 )) join
    sleep 8
    "${PY:-python}" suspend.py "dead_join" "${DEAD_SECS:-25}"
    wait
    printf "  %-5s dead     host: %-10s join: %s\n" "$1" "$(result "$1" dead_host)" "$(result "$1" dead_join)"
}

mixjoin() { # (impl unused) Windows hosts, ours joins: our joiner against a known-good host
    local n=S3X
    prep win mixjoin host; prep ours mixjoin join
    run win mixjoin host --name "$n" --secs 14 --expect 2 host
    sleep 3
    run ours mixjoin join --name J --session "dptest $n" --secs 9 join
    wait
    printf "  mixed    mixjoin  host (win): %-10s join (ours): %s\n" "$(result win mixjoin_host)" "$(result ours mixjoin_join)"
}

migrate() { # impl: the host leaves after 12 s; A and B must keep going and one of them hosts
    local n=S3M
    prep "$1" migrate host a b
    run "$1" migrate host --name "$n" --secs 12 --expect 3 host
    sleep 3
    run "$1" migrate a --name A --session "dptest $n" --secs 30 join
    sleep 2
    run "$1" migrate b --name B --session "dptest $n" --secs 27 join
    wait
    printf "  %-5s migrate  host: %-10s a: %-10s b: %s\n" "$1" "$(result "$1" migrate_host)" \
        "$(result "$1" migrate_a)" "$(result "$1" migrate_b)"
}

mkdir -p "$OUT/win" "$OUT/ours"
if [ "$want" = mixjoin ]; then   # our joiner's log against the all-Windows pair's joiner
    mixjoin
    "${PY:-python}" s3_compare.py "$OUT/win/pair_join.log" "$OUT/ours/mixjoin_join.log" --label mixjoin_join \
        > "$OUT/mixjoin_join.diff.txt"
    grep -m1 "^S3 " "$OUT/mixjoin_join.diff.txt"
    exit 0
fi
for sc in pair migrate groups dead; do
    [ "$want" = all ] || [ "$want" = "$sc" ] || continue
    for impl in win ours; do "$sc" "$impl"; sleep 2; done
    case $sc in pair|groups|dead) roles="host join" ;; migrate) roles="host a b" ;; esac
    for r in $roles; do
        "${PY:-python}" s3_compare.py "$OUT/win/${sc}_$r.log" "$OUT/ours/${sc}_$r.log" --label "${sc}_$r" \
            > "$OUT/${sc}_$r.diff.txt"
        grep -m1 "^S3 " "$OUT/${sc}_$r.diff.txt"
    done
done
