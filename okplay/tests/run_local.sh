#!/usr/bin/env bash
# Local scenarios for okplay: several dptest processes on this PC.
#
#   tests/run_local.sh [quick|pair|mesh3|loss|v6|lan|lanmesh|lanneg|online|all|full|
#                       burst|mesh8|lanmesh8|loss30|migrate|dropout|hostcrash|features|rejoin|aoe2names|
#                       twohosts|lantwohosts|password|lanpassword|pwretry|lanpwretry|pwnames|classic|mixed98|serverbox|
#                       rogue [deletehost|iamhost|sessiondesc]]
#
# Every dptest loads the FRESH build through the IPX provider slot (bin\dpwsockx.dll,
# OKP_TEST_AS=online|lan), so no admin/registration is needed between builds.
# Online scenarios need the rendezvous stand-in: python server/rdv_standin.py
# Each dptest prints RESULT ok|fail; a scenario passes only if every process does
# (lanneg is a negative control: it passes only if the join FAILS).
cd "$(dirname "$0")"
# Default: the drop-in (winedplay\bin\dropin\dplayx.dll - what ships) with its built-in LAN/Online
# Play (DPTEST_SP=auto). DPTEST_SP=ipx: Windows' own DirectPlay with okplay's dev copy in the IPX slot
# (bin\dpwsockx.dll) - okplay as a provider under the reference DirectPlay; our dplayx is not in it.
DROPIN_DLL=../../winedplay/bin/dropin/dplayx.dll
if [[ -z ${DPTEST_SP-} && -f $DROPIN_DLL ]]; then
    DPTEST_SP=auto
    export DPTEST_DPLAYX=${DPTEST_DPLAYX-$(cygpath -aw "$DROPIN_DLL")}
fi
echo "dptest: --sp ${DPTEST_SP:-ipx}, dplayx: ${DPTEST_DPLAYX:-the system one}"
DPT="../bin/dptest.exe --sp ${DPTEST_SP:-ipx}"
PY="${PY:-python}"   # the rogue scenario's log analyzer (tests/rogue_check.py)
OUT=out
mkdir -p "$OUT"
fails=0
export OKP_SERVER=${OKP_SERVER:-http://127.0.0.1:8787/okplay}

# Which dplayx.dll each process uses: Windows' (default), or a file such as Wine's build.
#   DPTEST_DPLAYX=<dll>                  everyone
#   DPLAYX_HOST=<dll> / DPLAYX_JOIN=<dll>  per role (mixed games); "" = Windows' own
run_bg() { # name args...
    local name=$1; shift
    local dll=${DPLAYX_JOIN-${DPTEST_DPLAYX-}}
    [[ $name == *_host ]] && dll=${DPLAYX_HOST-${DPTEST_DPLAYX-}}
    DPTEST_DPLAYX="$dll" $DPT "$@" > "$OUT/$name.txt" 2>&1 &
}

result_of() { grep -o "RESULT.*" "$OUT/$1.txt" | tail -1; }

check() { # scenario files...
    local sc=$1; shift
    local ok=1
    for f in "$@"; do
        local r peers
        r=$(result_of "$f")
        peers=$(grep -o "max_players=[0-9]* peers_answering=[0-9]*" "$OUT/$f.txt" | tail -1)
        printf "  %-14s %-12s %s\n" "$f" "${r:-RESULT missing}" "$peers"
        [[ "$r" == "RESULT ok" ]] || ok=0
    done
    if [[ $ok == 1 ]]; then echo "PASS $sc"; else echo "FAIL $sc"; fails=$((fails+1)); fi
}

pair() { # label
    local p=$1
    run_bg ${p}_host host --name H --secs 14 --expect 2
    sleep 3
    run_bg ${p}_join join --name J --secs 9 --session "dptest H"   # (not a room left over from the last scenario)
    wait
    check "$p" ${p}_host ${p}_join
}

burstpair() { # label: both sides send 600 guaranteed 1000-byte messages at once (fills the
    # reliable window 4x over); each must receive all 600, in order. Run with a reliable transport.
    local p=$1
    run_bg ${p}_host host --name H --secs 24 --expect 2 --burst 600
    sleep 3
    run_bg ${p}_join join --name J --secs 19 --burst 600
    wait
    check "$p" ${p}_host ${p}_join
    grep -h "STAT burst\|reliable window\|burst_sent" "$OUT/${p}_host.txt" "$OUT/${p}_join.txt" | sed 's/^/    /'
}

buildcheck() { # label: the joiner's game exe differs from the host's (OKP_TEST_EXEBUILD fakes it):
    # warned before joining (DP_BUILTIN_CheckBuild); same build: no warning
    [[ -n ${DPTEST_DPLAYX-} ]] || { echo "SKIP buildcheck (needs DPTEST_DPLAYX=<drop-in dll>)"; return; }
    local p=$1 m="$OUT/buildcheck.marker" ok=1 r case want_res want_log
    for case in same:ok:"host runs the same build" cancel:fail:"player cancelled the join" join:ok:"joining anyway"; do
        IFS=: read -r answer want_res want_log <<< "$case"
        touch "$m"; sleep 1
        run_bg ${p}_host host --name BC --secs 12 --expect 2
        sleep 3
        if [[ $answer == same ]]; then
            run_bg ${p}_join join --name J --secs 6 --session "dptest BC"
        else
            OKP_TEST_EXEBUILD=0123456789abcdef OKP_TEST_BUILD_ANSWER=$answer \
                run_bg ${p}_join join --name J --secs 6 --session "dptest BC"
        fi
        wait
        r=$(result_of ${p}_join)
        local seen
        seen=$(newlogs "$m" | xargs -0 grep -h -E "[IWE] build: " 2>/dev/null | sed "s/^.* build: //" | head -2 | tr '\n' ' ')
        printf "  %-8s join %-12s build log: %s\n" "$answer" "$r" "$seen"
        [[ "$r" == "RESULT $want_res" ]] || ok=0
        echo "$seen" | grep -q "$want_log" || ok=0
    done
    if [[ $ok == 1 ]]; then echo "PASS $p"; else echo "FAIL $p"; fails=$((fails+1)); fi
}

roomstate() { # label: the host's room-list state follows the traffic (rdv.c room_state): the joiner
    # sends match-like traffic for 20 s (dptest --busy), then goes quiet - lobby -> playing -> between
    [[ -n ${DPTEST_DPLAYX-} ]] || { echo "SKIP roomstate (needs DPTEST_DPLAYX=<drop-in dll>)"; return; }
    local p=$1 m="$OUT/roomstate.marker" seen
    touch "$m"; sleep 1
    run_bg ${p}_host host --name RS --secs 58 --expect 2
    sleep 3
    run_bg ${p}_join join --name J --secs 52 --session "dptest RS" --busy 4:20
    wait
    seen=$(newlogs "$m" | xargs -0 grep -h "room state:" 2>/dev/null | sed 's/^.*room state: //')
    echo "$seen" | sed 's/^/    /'
    if echo "$seen" | grep -q "^lobby -> playing" && echo "$seen" | grep -q "^playing -> between"; then
        echo "PASS $p"
    else echo "FAIL $p"; fails=$((fails+1)); fi
}

youaredead() { # label: the host drops a silent joiner; the joiner, which waits longer (drop_after=60),
    # talks again after the blackout and the host answers YOUAREDEAD: the joiner's game gets
    # DPSYS_SESSIONLOST and the host's player leaves it, as Windows' DirectPlay does (lifecycle.c)
    local p=$1 at
    at=$(( $(date +%s) + 12 ))
    OKP_BLACKOUT="$at:22" run_bg ${p}_host host --name YD --secs 60 --expect 2
    sleep 3
    OKP_DROP_AFTER=60 OKP_BLACKOUT="$at:22" run_bg ${p}_join join --name J --secs 54 --session "dptest YD"
    wait
    local hl jl
    hl=$(grep -c "EVENT player_left" "$OUT/${p}_host.txt")
    jl=$(grep -c "EVENT session_lost" "$OUT/${p}_join.txt")
    printf "  %-16s host dropped the joiner: %s, joiner's session lost: %s (expected: 1, 1)\n" "$p" "$hl" "$jl"
    if [[ $hl -ge 1 && $jl == 1 ]]; then echo "PASS $p"; else echo "FAIL $p"; fails=$((fails+1)); fi
}

blackout() { # label seconds expect(stays|drops): the line goes dead both ways for N s mid-game
    # (OKP_BLACKOUT, net.c); "stays" = nobody leaves and pings flow again afterwards
    local p=$1 secs=$2 expect=$3 at
    at=$(( $(date +%s) + 12 ))
    OKP_BLACKOUT="$at:$secs" run_bg ${p}_host host --name H --secs $((secs + 30)) --expect 2
    sleep 3
    OKP_BLACKOUT="$at:$secs" run_bg ${p}_join join --name J --secs $((secs + 24)) --session "dptest H"
    wait
    local left ok=1 f last
    # the joiner's own goodbye when its run ends (~ (secs+24+3) s into the host's) is not a drop
    left=$(grep -h "EVENT player_left" "$OUT/${p}_host.txt" | tr -d '[]' |
           awk -v end=$(( (secs + 24 + 3 - 3) * 1000 )) '$1 + 0 < end {n++} END {print n + 0}')
    for f in ${p}_host ${p}_join; do
        last=$(grep -o "last_pong_ago=-\?[0-9]*" "$OUT/$f.txt" | tail -1 | cut -d= -f2)
        printf "  %-16s %s  last pong %s ms before the end\n" "$f" "$(result_of $f)" "${last:-?}"
        if [[ $expect == stays ]]; then [[ -n $last && $last -ge 0 && $last -lt 3000 ]] || ok=0; fi
    done
    printf "  %-16s %s-s blackout: players who left: %s (expected: %s)\n" "$p" "$secs" "$left" "$expect"
    if [[ $expect == stays ]]; then [[ $left == 0 ]] || ok=0; else [[ $left -gt 0 ]] || ok=0; fi
    if [[ $ok == 1 ]]; then echo "PASS $p"; else echo "FAIL $p"; fails=$((fails+1)); fi
}

mesh3() { # label
    local p=$1
    run_bg ${p}_host host --name H --secs 22 --expect 3
    sleep 3
    run_bg ${p}_a join --name A --secs 15
    sleep 2
    run_bg ${p}_b join --name B --secs 12
    wait
    check "$p" ${p}_host ${p}_a ${p}_b
}

mesh8() { # AoE2's maximum: host + 7 joiners, everyone must hear from all 7 others
    local p=$1
    run_bg ${p}_host host --name H --secs 42 --expect 8
    local files="${p}_host"
    for i in 1 2 3 4 5 6 7; do
        sleep 1.5
        run_bg ${p}_j$i join --name J$i --secs $((34 - i * 2))
        files="$files ${p}_j$i"
    done
    wait
    check "$p" $files
    for f in $files; do
        grep -q "peers_answering=7" "$OUT/$f.txt" || { echo "  $f did not hear from all 7 others"; fails=$((fails+1)); return; }
    done
}

migrate() { # the host quits mid-game: the other two must keep talking and one becomes host
    local p=$1 m="$OUT/migrate.marker"
    touch "$m"; sleep 1
    run_bg ${p}_host host --name H --secs 12 --expect 3
    sleep 3
    run_bg ${p}_a join --name A --secs 30
    sleep 2
    run_bg ${p}_b join --name B --secs 27
    wait
    local ok=1
    grep -q "RESULT ok" "$OUT/${p}_host.txt" || ok=0
    for j in a b; do
        # after the host left, this player must still be hearing from the other one (answer < 3 s old)
        local fresh
        fresh=$(grep -o "last_pong_ago=[0-9]*" "$OUT/${p}_$j.txt" | cut -d= -f2 | awk '$1 < 3000' | wc -l)
        printf "  %-14s answers fresher than 3s after host left: %s\n" "${p}_$j" "$fresh"
        [[ $fresh -ge 1 ]] || ok=0
    done
    if grep -q "EVENT became_host" "$OUT/${p}_a.txt" "$OUT/${p}_b.txt"; then echo "  a new host took over"; else echo "  nobody became host"; ok=0; fi
    # ...and lists the game on the server under itself (MTM2 hands the game on when its host quits;
    # the drop-in's Online Play kept quiet and the game vanished from the list). Drop-in only.
    if [[ -n ${DPTEST_DPLAYX-} ]]; then
        local listed=0 f
        while IFS= read -r -d '' f; do
            awk '/this PC is the new host/ {n=1} n && /rdv: room [0-9a-f]+ is listed/ {print; exit}' "$f" | grep -q . &&
                listed=$((listed+1))
        done < <(newlogs "$m")
        printf "  %-14s new host listed its room on the server: %s (expected 1)
" "$p" "$listed"
        [[ $listed == 1 ]] || ok=0
    fi
    if [[ $ok == 1 ]]; then echo "PASS $p"; else echo "FAIL $p"; fails=$((fails+1)); fi
}

features3() { # lobby changes (names, player data, chat, session description) reach all 3 machines
    local p=$1
    run_bg ${p}_host host --name H --secs 24 --expect 3 --features
    sleep 3
    run_bg ${p}_a join --name A --secs 18 --expect 3 --features
    sleep 2
    run_bg ${p}_b join --name B --secs 15 --expect 3 --features
    wait
    check "$p" ${p}_host ${p}_a ${p}_b
    grep -h "STAT features" "$OUT/${p}_host.txt" "$OUT/${p}_a.txt" "$OUT/${p}_b.txt" | sed 's/^.*STAT /    /'
}

aoe2names() { # AoE2's way of calling: WCHAR interface fed ANSI bytes, opened with 1 player already
    local p=$1   # counted - names must arrive byte for byte and the count must stay right. The
    # joiner has no long name (that crashed a real AoE2 host): the host must never get a NULL name
    run_bg ${p}_host host --name H --secs 14 --expect 2 --aoe2names
    sleep 3
    # --session: with every game now having its own instance GUID, a room left over from the
    # previous scenario is listed too (for up to 15 s) - join ours by name
    run_bg ${p}_join join --name J --secs 9 --expect 2 --aoe2names --nolongname --session "dptest H"
    wait
    check "$p" ${p}_host ${p}_join
    grep -h "STAT aoe2names\|STAT null_names\|EVENT null_name" "$OUT/${p}_host.txt" "$OUT/${p}_join.txt" | sed 's/^.*\(STAT\|EVENT\) /    /'
}

twohosts() { # two games at once: the joiner must list both and reach the one it picked
    local p=$1   # (every game once had the same instance GUID: only one was ever listed)
    run_bg ${p}_h1 host --name H1 --secs 18
    sleep 1
    run_bg ${p}_h2 host --name H2 --secs 16 --expect 2
    sleep 3
    run_bg ${p}_join join --name J --secs 9 --session "dptest H2"
    wait
    local ok=1 listed h1max
    listed=$(grep -o 'EVENT session "dptest H[12]"' "$OUT/${p}_join.txt" | sort -u | wc -l)
    h1max=$(grep -o "max_players=[0-9]*" "$OUT/${p}_h1.txt" | tail -1 | cut -d= -f2)
    printf "  %-14s games listed: %s (want 2)\n" "${p}_join" "$listed"
    [[ $listed -eq 2 ]] || ok=0
    grep -q '\[PW\]' "$OUT/${p}_join.txt" && { echo "  an open game was marked [PW]"; ok=0; }
    for f in ${p}_h2 ${p}_join; do
        printf "  %-14s %s\n" "$f" "$(result_of "$f")"
        [[ "$(result_of "$f")" == "RESULT ok" ]] || ok=0
    done
    printf "  %-14s players seen: %s (want 1: the joiner went to H2)\n" "${p}_h1" "${h1max:-?}"
    [[ "$h1max" == "1" ]] || ok=0
    if [[ $ok == 1 ]]; then echo "PASS $p"; else echo "FAIL $p"; fails=$((fails+1)); fi
}

password() { # a game with a password: the right one gets in, a wrong one and Cancel do not
    local p=$1 ok=1   # (answers come from OKP_TEST_*_PASSWORD instead of the boxes)
    OKP_TEST_HOST_PASSWORD=swordfish run_bg ${p}_host host --name H --secs 32 --expect 2
    sleep 3
    OKP_TEST_JOIN_PASSWORD=swordfish run_bg ${p}_right join --name R --secs 8
    sleep 10
    OKP_TEST_JOIN_PASSWORD=wrongpass run_bg ${p}_wrong join --name W --secs 6
    sleep 8
    OKP_TEST_JOIN_PASSWORD='<cancel>' run_bg ${p}_cancel join --name C --secs 6
    wait
    local r
    r=$(result_of ${p}_right); printf "  %-14s %-12s (right password: must get in)\n" ${p}_right "$r"
    [[ "$r" == "RESULT ok" ]] || ok=0
    grep -q "RESULT ok" "$OUT/${p}_host.txt" || { echo "  ${p}_host never saw the right joiner"; ok=0; }
    grep -qi "ERROR Open(join): 0x88770154" "$OUT/${p}_wrong.txt" && echo "  ${p}_wrong     refused: DPERR_INVALIDPASSWORD" || { echo "  ${p}_wrong     NOT refused with DPERR_INVALIDPASSWORD"; ok=0; }
    grep -qi "ERROR Open(join): 0x88770118" "$OUT/${p}_cancel.txt" && echo "  ${p}_cancel    stopped: DPERR_USERCANCEL" || { echo "  ${p}_cancel    did NOT stop with DPERR_USERCANCEL"; ok=0; }
    # joiners must see the game marked as needing a password (DPSESSION_PASSWORDREQUIRED 0x400)
    grep -qE 'EVENT session .* flags 0x[0-9a-f]*[4-7cdef][0-9a-f]{2}$' "$OUT/${p}_right.txt" && echo "  listed as needing a password" || { echo "  NOT listed as needing a password"; ok=0; }
    grep -q 'EVENT session "dptest H \[PW\]"' "$OUT/${p}_right.txt" && echo "  shown as \"dptest H [PW]\"" || { echo "  name NOT marked [PW]"; ok=0; }
    if [[ $ok == 1 ]]; then echo "PASS $p"; else echo "FAIL $p"; fails=$((fails+1)); fi
}

pwretry() { # wrong password, then the right one - same player, same game session (this hung:
    local p=$1 ok=1   # the retried join request was taken for a duplicate of the refused one)
    OKP_TEST_HOST_PASSWORD=swordfish run_bg ${p}_host host --name H --secs 50 --expect 2
    sleep 3
    OKP_TEST_JOIN_PASSWORD='wrongpass|swordfish' run_bg ${p}_join join --name J --secs 8 --retry 1 --session "dptest H [PW]"
    wait
    grep -q "EVENT join_failed 0x88770154" "$OUT/${p}_join.txt" && echo "  first try refused (wrong password)" || { echo "  first try NOT refused as a wrong password"; ok=0; }
    local r; r=$(result_of ${p}_join)
    printf "  %-14s %-12s (second try, right password: must get in, no hang)\n" ${p}_join "$r"
    [[ "$r" == "RESULT ok" ]] || ok=0
    grep -q "ERROR Open(join): 0x887700aa" "$OUT/${p}_join.txt" && { echo "  second try TIMED OUT (the hang)"; ok=0; }
    grep -q "RESULT ok" "$OUT/${p}_host.txt" || { echo "  ${p}_host never saw the player"; ok=0; }
    if [[ $ok == 1 ]]; then echo "PASS $p"; else echo "FAIL $p"; fails=$((fails+1)); fi
}

pwnames() { # AoE2-style names + a password: the list must show "Name [PW]" (the mark goes inside AoE2's
    local p=$1 ok=1   # own Name/0/[Host]/0 bytes) and the join must still work
    OKP_TEST_HOST_PASSWORD=swordfish run_bg ${p}_host host --name H --secs 16 --expect 2 --aoe2names
    sleep 3
    OKP_TEST_JOIN_PASSWORD=swordfish run_bg ${p}_join join --name J --secs 8 --aoe2names --session "dptest H [PW]"
    wait
    grep -q 'EVENT session "dptest H \[PW\]"' "$OUT/${p}_join.txt" && echo "  shown as \"dptest H [PW]\"" || { echo "  name NOT marked [PW]"; ok=0; }
    for f in ${p}_host ${p}_join; do printf "  %-14s %s
" $f "$(result_of $f)"; [[ "$(result_of $f)" == "RESULT ok" ]] || ok=0; done
    if [[ $ok == 1 ]]; then echo "PASS $p"; else echo "FAIL $p"; fails=$((fails+1)); fi
}

rejoin() { # leave a game and host/join the next with the same DirectPlay object (2 cycles)
    local p=$1
    run_bg ${p}_host host --name H --secs 10 --expect 2 --cycles 2
    sleep 3
    run_bg ${p}_join join --name J --secs 6 --cycles 2
    wait
    check "$p" ${p}_host ${p}_join
}

my_id() { grep -o "my_player id=[0-9]*" "$OUT/$1.txt" | head -1 | cut -d= -f2; }
left_seen() { grep -q "EVENT player_left id=$2\b" "$OUT/$1.txt"; }

dropout() { # a joiner's game dies mid-game (killed: no goodbye) - the others must remove its player
    local p=$1
    run_bg ${p}_host host --name H --secs 50 --expect 3
    sleep 3
    run_bg ${p}_a join --name A --secs 45
    sleep 2
    run_bg ${p}_b join --name B --secs 90
    local bpid=$!
    sleep 8
    kill -9 $bpid 2>/dev/null
    wait
    local bid ok=1
    bid=$(my_id ${p}_b)
    for f in ${p}_host ${p}_a; do
        if left_seen $f "$bid"; then echo "  $f saw B ($bid) leave"; else echo "  $f never saw B ($bid) leave"; ok=0; fi
    done
    grep -q "RESULT ok" "$OUT/${p}_host.txt" "$OUT/${p}_a.txt" || true
    if [[ $ok == 1 ]]; then echo "PASS $p"; else echo "FAIL $p"; fails=$((fails+1)); fi
}

hostcrash() { # the host's game dies mid-game: the others notice, one becomes host, they carry on
    local p=$1
    run_bg ${p}_host host --name H --secs 90 --expect 3
    local hpid=$!
    sleep 3
    run_bg ${p}_a join --name A --secs 55
    sleep 2
    run_bg ${p}_b join --name B --secs 52
    sleep 6
    kill -9 $hpid 2>/dev/null
    wait
    local hid ok=1
    hid=$(my_id ${p}_host)
    for j in a b; do
        if left_seen ${p}_$j "$hid"; then echo "  ${p}_$j saw the host ($hid) leave"; else echo "  ${p}_$j never saw the host leave"; ok=0; fi
        local fresh
        fresh=$(grep -o "last_pong_ago=[0-9]*" "$OUT/${p}_$j.txt" | cut -d= -f2 | awk '$1 < 3000' | wc -l)
        [[ $fresh -ge 1 ]] || { echo "  ${p}_$j stopped hearing the other player"; ok=0; }
    done
    if grep -q "EVENT became_host" "$OUT/${p}_a.txt" "$OUT/${p}_b.txt"; then echo "  a new host took over"; else echo "  nobody became host"; ok=0; fi
    if [[ $ok == 1 ]]; then echo "PASS $p"; else echo "FAIL $p"; fails=$((fails+1)); fi
}

# LAN Play with the internet pieces deliberately broken: dead server, no STUN
lan_env() { OKP_TEST_AS=lan OKP_SERVER=http://127.0.0.1:9/nothing OKP_STUN= "$@"; }

lanneg() {
    # host is Online Play, joiner is LAN Play with no server: the joiner must NOT find it
    OKP_TEST_AS=online run_bg lanneg_host host --name H --secs 12
    sleep 3
    OKP_TEST_AS=lan OKP_SERVER=http://127.0.0.1:9/nothing OKP_STUN= run_bg lanneg_join join --name J --secs 6
    wait
    local r
    r=$(result_of lanneg_join)
    printf "  %-14s %-12s (expected: fail - LAN must not see an Online-only game)\n" lanneg_join "$r"
    if [[ "$r" == "RESULT fail" ]]; then echo "PASS lanneg"; else echo "FAIL lanneg"; fails=$((fails+1)); fi
}

classic() {
    # mode=all's classic half: a classic TCP/IP game between the drop-in and plain Windows
    # DirectPlay, both ways (the drop-in hands it to Windows' own). Control: mode=oldkid has no
    # classic TCP/IP, so that join must fail. Needs DPTEST_DPLAYX=<drop-in dplayx.dll> and Windows'
    # DirectPlay feature.
    [[ -n ${DPTEST_DPLAYX-} ]] || { echo "SKIP classic (needs DPTEST_DPLAYX=<drop-in dll>)"; return; }
    # Windows registers TCP/IP and IPX both as a bare "dpwsockx.dll", so a dptest started from bin\
    # loads okplay's dev copy for TCP/IP too (it refuses: DPERR_UNAVAILABLE). Run from a folder
    # without it, so TCP/IP is Windows' own provider.
    mkdir -p "$OUT/clean" && cp ../bin/dptest.exe "$OUT/clean/"
    local DPT="$OUT/clean/dptest.exe --sp ${DPTEST_SP:-ipx}"
    local dll=$DPTEST_DPLAYX tcp="--sp tcpip --ip 127.0.0.1"
    OKP_MODE=all DPLAYX_HOST=$dll DPLAYX_JOIN= run_bg classic_dropin_host host $tcp --name CA --secs 14 --expect 2
    sleep 3
    OKP_MODE=all DPLAYX_HOST=$dll DPLAYX_JOIN= run_bg classic_win_join join $tcp --name WJ --session "dptest CA" --secs 9
    wait
    check classic_dropin_hosts classic_dropin_host classic_win_join
    OKP_MODE=all DPLAYX_HOST= DPLAYX_JOIN=$dll run_bg classic_win_host host $tcp --name CB --secs 14 --expect 2
    sleep 3
    OKP_MODE=all DPLAYX_HOST= DPLAYX_JOIN=$dll run_bg classic_dropin_join join $tcp --name DJ --session "dptest CB" --secs 9
    wait
    check classic_win_hosts classic_win_host classic_dropin_join
    OKP_MODE=all DPLAYX_HOST= DPLAYX_JOIN=$dll run_bg classicneg_host host $tcp --name CC --secs 10
    sleep 3
    OKP_MODE=oldkid DPLAYX_HOST= DPLAYX_JOIN=$dll run_bg classicneg_join join $tcp --name NJ --session "dptest CC" --secs 5
    wait
    local r
    r=$(result_of classicneg_join)
    printf "  %-14s %-12s (expected: fail - mode=oldkid has no classic TCP/IP)\n" classicneg_join "$r"
    if [[ "$r" == "RESULT fail" ]]; then echo "PASS classicneg"; else echo "FAIL classicneg"; fails=$((fails+1)); fi
}

# the drop-in's session logs (next to DPTEST_DPLAYX) written since a marker file
# (paths may contain spaces: always -print0 / xargs -0)
newlogs() { find "$(dirname "$DPTEST_DPLAYX")/netplay-logs" -name 'okplay-*.log' -newer "$1" -print0 2>/dev/null; }

mixed98() {
    # a Windows 98-style player (IPv4-only socket, plain HTTP...) with a normal dual-stack one, both ways.
    # Besides connecting, the 98 side must not keep sending to the other side's IPv6 addresses: that
    # failed ~5 times a second for a whole game and filled the Windows 98 log (found in the VM).
    [[ -n ${DPTEST_DPLAYX-} ]] || { echo "SKIP mixed98 (needs DPTEST_DPLAYX=<drop-in dll>)"; return; }
    local m="$OUT/mixed98.marker" bad
    touch "$m"; sleep 1
    OKP_TEST_AS=online OKP_EMULATE_9X=1 run_bg mixed98a_host host --name M98 --secs 14 --expect 2
    sleep 3
    OKP_TEST_AS=online run_bg mixed98a_join join --name J --session "dptest M98" --secs 9
    wait
    check mixed98_host98 mixed98a_host mixed98a_join
    OKP_TEST_AS=online run_bg mixed98b_host host --name MNT --secs 14 --expect 2
    sleep 3
    OKP_TEST_AS=online OKP_EMULATE_9X=1 run_bg mixed98b_join join --name J --session "dptest MNT" --secs 9
    wait
    check mixed98_join98 mixed98b_host mixed98b_join
    bad=$(newlogs "$m" | xargs -0 cat 2>/dev/null | grep -c "sendto \[")
    printf "  %-14s %s IPv6 send attempt(s) from the IPv4-only side (expected: 0)\n" mixed98_v6 "$bad"
    if [[ $bad == 0 ]]; then echo "PASS mixed98_v6"; else echo "FAIL mixed98_v6"; fails=$((fails+1)); fi
}

serverbox() {
    # the "can't reach the server" box: shown while hosting in the lobby, held back during a match
    # (a 3-second internet drop must never pop a box over a running game). Tests only log the decision.
    [[ -n ${DPTEST_DPLAYX-} ]] || { echo "SKIP serverbox (needs DPTEST_DPLAYX=<drop-in dll>)"; return; }
    local m="$OUT/serverbox.marker" shown held
    touch "$m"; sleep 1
    OKP_TEST_AS=online OKP_SERVER=http://127.0.0.1:9/okplay run_bg serverbox_lobby host --name SB1 --secs 8
    OKP_TEST_AS=online OKP_SERVER=http://127.0.0.1:9/okplay run_bg serverbox_match host --name SB2 --secs 8 --sflags 0x20
    # MTM2-style: never locks the session, but someone else is in the game -> must hold it too
    OKP_TEST_AS=online OKP_SERVER=http://127.0.0.1:9/okplay run_bg serverbox_company host --name SB3 --secs 8 --extra
    wait
    # each host's FIRST decision counts (a held box is shown once the match is over - by design)
    shown=0; held=0
    while IFS= read -r -d '' f; do
        case "$(grep -m1 -oE "showing the player the can't-reach-the-server box|not showing the server box now" "$f")" in
            "showing the player"*) shown=$((shown+1)) ;;
            "not showing"*) held=$((held+1)) ;;
        esac
    done < <(newlogs "$m")
    printf "  %-14s lobby host showed it: %s, started-game + host-with-company held it: %s (expected 1 and 2)\n" serverbox "$shown" "$held"
    if [[ $shown == 1 && $held == 2 ]]; then echo "PASS serverbox"; else echo "FAIL serverbox"; fails=$((fails+1)); fi
}

rogue() {   # a player already IN the game forges a "the host left" (or "I'm the host") at the others.
    # wire 4's token stops outsiders; this is the insider case - the fix (DP_SenderOwns) must refuse it,
    # the game must go on under the real host, and a build with the check removed (bin/noauth) must let
    # it through (that proves the test actually bites). Drop-in only. Usage: rogue [deletehost|iamhost|sessiondesc]
    [[ -n ${DPTEST_DPLAYX-} ]] || { echo "SKIP rogue (needs DPTEST_DPLAYX=<drop-in dll>)"; return; }
    local attack=${1:-deletehost}
    _rogue_run() {   # dll label  -> "refused=<0|1> took_effect=<0|1>" (tests/rogue_check.py reads the logs)
        local dll=$1 label=$2 m="$OUT/$label.marker"
        touch "$m"; sleep 1
        DPLAYX_HOST=$dll DPLAYX_JOIN=$dll run_bg ${label}_host host --name RH --secs 22 --expect 3
        sleep 3
        DPLAYX_HOST=$dll DPLAYX_JOIN=$dll run_bg ${label}_a join --name RA --secs 18
        sleep 2
        OKP_TEST_ROGUE=$attack DPLAYX_HOST=$dll DPLAYX_JOIN=$dll run_bg ${label}_b join --name RB --secs 16
        wait
        "$PY" rogue_check.py "$(dirname "$dll")/netplay-logs" "$m" "$attack"
    }
    local ok=1
    eval "local $(_rogue_run "$DPTEST_DPLAYX" "rogue_fix_$attack")"   # -> refused=.. took_effect=..
    printf "  %-12s fix build:    forgery refused: %s (want 1), took effect: %s (want 0)\n" "$attack" "$refused" "$took_effect"
    [[ $refused == 1 && $took_effect == 0 ]] || ok=0
    local noauth="$(dirname "$DPTEST_DPLAYX")/../noauth/dplayx.dll"
    if [[ -f $noauth && $noauth -ot $DPTEST_DPLAYX ]]; then
        # a stale control stops biting after dplay.c changes and fails the scenario although the fix is right (2026-10-04)
        echo "               WARNING: bin/noauth is older than the drop-in - rebuild it (VARIANT=noauth VARIANT_CFLAGS=-DDPLAYX_TEST_SKIP_AUTH ./build.sh)"
    fi
    if [[ -f $noauth ]]; then
        local refused=0 took_effect=0
        eval "$(_rogue_run "$noauth" "rogue_noauth_$attack")"
        printf "  %-12s noauth build: forgery refused: %s (want 0), took effect: %s (want 1 - proves the test bites)\n" "$attack" "$refused" "$took_effect"
        [[ $refused == 0 && $took_effect == 1 ]] || ok=0
    else
        echo "               (no bin/noauth build - skipping the negative control;"
        echo "                build it: cd winedplay && VARIANT=noauth VARIANT_CFLAGS=-DDPLAYX_TEST_SKIP_AUTH ./build.sh)"
    fi
    if [[ $ok == 1 ]]; then echo "PASS rogue ($attack)"; else echo "FAIL rogue ($attack)"; fails=$((fails+1)); fi
}

online() {
    OKP_TEST_AS=online pair pair
    OKP_TEST_AS=online mesh3 mesh3
    OKP_TEST_AS=online OKP_DROP_PCT=15 pair loss15
    OKP_TEST_AS=online OKP_FAMILY=6 OKP_STUN= pair v6only
}
lan() {
    export OKP_TEST_AS=lan OKP_SERVER=http://127.0.0.1:9/nothing OKP_STUN=
    pair lan
    mesh3 lanmesh
    unset OKP_TEST_AS OKP_STUN
    export OKP_SERVER=http://127.0.0.1:8787/okplay
}

sc=${1:-all}
case $sc in
    pair)    OKP_TEST_AS=online pair pair ;;
    mesh3)   OKP_TEST_AS=online mesh3 mesh3 ;;
    loss)    OKP_TEST_AS=online OKP_DROP_PCT=15 pair loss15 ;;
    loss30)  OKP_TEST_AS=online OKP_DROP_PCT=30 pair loss30 ;;
    burst)   (export OKP_TEST_AS=lan OKP_SERVER=http://127.0.0.1:9/nothing OKP_STUN= OKP_TRANSPORT=udp+reliable
              burstpair burst; OKP_DROP_PCT=15 burstpair burstloss15; OKP_DROP_PCT=30 burstpair burstloss30
              OKP_TRANSPORT=reliable OKP_DROP_PCT=15 burstpair burstall15; exit $fails) || fails=$? ;;
    roomstate) OKP_TEST_AS=online roomstate roomstate ;;
    buildcheck) OKP_TEST_AS=online buildcheck buildcheck
                (export OKP_TEST_AS=lan OKP_SERVER=http://127.0.0.1:9/nothing OKP_STUN=; buildcheck lanbuildcheck; exit $fails) || fails=$? ;;
    blackout) (export OKP_TEST_AS=lan OKP_SERVER=http://127.0.0.1:9/nothing OKP_STUN=
              blackout blackout5 5 stays; blackout blackout10 10 stays; blackout blackout20 20 drops
              # drop_after= is the drop-in's (lifecycle.c): Windows' DirectPlay - which the ipx dev slot
              # ends up in - always drops at ~15 s, so these two run on the drop-in's own LAN Play
              DPT="../bin/dptest.exe --sp auto"; export DPTEST_DPLAYX=${DPTEST_DPLAYX:-$(cygpath -aw "$DROPIN_DLL")}
              OKP_DROP_AFTER=30 blackout blackout20da30 20 stays; OKP_DROP_AFTER=30 blackout blackout40da30 40 drops
              youaredead youaredead
              exit $fails) || fails=$? ;;
    v6)      OKP_TEST_AS=online OKP_FAMILY=6 OKP_STUN= pair v6only ;;
    online)  online ;;
    lan)     (export OKP_TEST_AS=lan OKP_SERVER=http://127.0.0.1:9/nothing OKP_STUN=; pair lan; exit $fails) || fails=$? ;;
    lanmesh) (export OKP_TEST_AS=lan OKP_SERVER=http://127.0.0.1:9/nothing OKP_STUN=; mesh3 lanmesh; exit $fails) || fails=$? ;;
    lanneg)  lanneg ;;
    mesh8)   OKP_TEST_AS=online mesh8 mesh8 ;;
    lanmesh8) (export OKP_TEST_AS=lan OKP_SERVER=http://127.0.0.1:9/nothing OKP_STUN=; mesh8 lanmesh8; exit $fails) || fails=$? ;;
    migrate) OKP_TEST_AS=online migrate migrate ;;
    dropout) OKP_TEST_AS=online dropout dropout ;;
    features) OKP_TEST_AS=online features3 features3 ;;
    rejoin)  OKP_TEST_AS=online rejoin rejoin ;;
    aoe2names) OKP_TEST_AS=online aoe2names aoe2names ;;
    twohosts) OKP_TEST_AS=online twohosts twohosts ;;
    password) OKP_TEST_AS=online password password ;;
    pwretry) OKP_TEST_AS=online pwretry pwretry ;;
    pwnames) OKP_TEST_AS=online pwnames pwnames ;;
    lanpwretry) (export OKP_TEST_AS=lan OKP_SERVER=http://127.0.0.1:9/nothing OKP_STUN=; pwretry lanpwretry; exit $fails) || fails=$? ;;
    lanpassword) (export OKP_TEST_AS=lan OKP_SERVER=http://127.0.0.1:9/nothing OKP_STUN=; password lanpassword; exit $fails) || fails=$? ;;
    lantwohosts) (export OKP_TEST_AS=lan OKP_SERVER=http://127.0.0.1:9/nothing OKP_STUN=; twohosts lantwohosts; exit $fails) || fails=$? ;;
    hostcrash) OKP_TEST_AS=online hostcrash hostcrash ;;
    classic) classic ;;
    mixed98) mixed98 ;;
    serverbox) serverbox ;;
    rogue)   OKP_TEST_AS=online rogue "${2:-deletehost}" ;;
    # quick (~4 min, every change): one scenario per layer - server, LAN, host hand-over, the 98 paths,
    # the classic pass-through, AoE2's names, the server box. full is for release candidates.
    quick)   OKP_TEST_AS=online pair pair
             (export OKP_TEST_AS=lan OKP_SERVER=http://127.0.0.1:9/nothing OKP_STUN=; pair lan; exit $fails) || fails=$?
             OKP_TEST_AS=online migrate migrate
             OKP_TEST_AS=online aoe2names aoe2names
             classic; mixed98; serverbox ;;
    all)     online; lan; lanneg ;;
    full)    online; lan; lanneg
             OKP_TEST_AS=online mesh8 mesh8
             (export OKP_TEST_AS=lan OKP_SERVER=http://127.0.0.1:9/nothing OKP_STUN=; mesh8 lanmesh8; exit $fails) || fails=$?
             OKP_TEST_AS=online OKP_DROP_PCT=30 pair loss30
             for s in migrate dropout hostcrash features3 rejoin aoe2names twohosts password pwretry pwnames; do OKP_TEST_AS=online $s $s; done
             (export OKP_TEST_AS=lan OKP_SERVER=http://127.0.0.1:9/nothing OKP_STUN=; twohosts lantwohosts; password lanpassword; pwretry lanpwretry; exit $fails) || fails=$?
             classic; mixed98; serverbox
             for a in deletehost iamhost sessiondesc; do OKP_TEST_AS=online rogue $a; done ;;
    *)       echo "unknown scenario $sc"; exit 2 ;;
esac
echo "failures: $fails"
exit $fails
