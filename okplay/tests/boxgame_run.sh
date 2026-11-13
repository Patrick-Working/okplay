#!/usr/bin/env bash
# boxgame_run.sh - one box test case on the stand-in game (docs/BOX_METHOD.md 5.1/5.2), version 1.
#
#   okplay/tests/boxgame_run.sh [run options] [--] [boxgame switches]
#
# Run options (before the boxgame switches):
#   --mode ask|lan|online|original  okplay.ini wsock_mode (default ask: the mode box is shown for real)
#   --close-ms MS                    OKP_TEST_BOX_CLOSE_MS: each of okplay's own boxes closes by itself after MS, as Cancel
#                                    (the room list / "Connecting..." box does not take it: answer it with --keys {esc})
#   --front 0|1                      okplay.ini wsock_box_front (default 0: the no-front box on Windows NT)
#   --password ask|off               okplay.ini use_password (default ask: the host password box is shown)
#   --env NAME=VALUE                 another variable for the run (e.g. OKP_EMULATE_9X=1); may repeat
# boxgame switches: see okplay/wsock/tests/boxgame.c (--net, --kind, --role, --slow, --clock, --seconds, --windowed, ...).
#
# Runs from the FIXED folder okplay/tests/out/boxgame (boxgame.exe + wsock32.dll + okplay.ini copied there; never a
# fresh temporary folder: Windows Firewall remembers programs by path). Online Play uses the room-server stand-in
# already running on 127.0.0.1:8787 (this script does not start one). Then runs boxgame_check.py on the two logs.
#
# Without --windowed the stand-in takes the whole screen for --seconds: that is for the VMs. On a PC in use it refuses
# unless BOXGAME_FULLSCREEN_OK=1 is set.
set -e
here=$(cd "$(dirname "$0")" && pwd)
OK=$(cd "$here/.." && pwd)
. "$OK/../env.sh"
BIN="$OK/wsock/bin"
D="$here/out/boxgame"
mode=ask; close_ms=""; front=0; password=ask; extra_env=()
while [ $# -gt 0 ]; do
    case "$1" in
        --mode) mode=$2; shift 2 ;;
        --close-ms) close_ms=$2; shift 2 ;;
        --front) front=$2; shift 2 ;;
        --password) password=$2; shift 2 ;;
        --env) extra_env+=("$2"); shift 2 ;;
        --) shift; break ;;
        *) break ;;
    esac
done
args=("$@")
kind=tcp; windowed=0
for ((i = 0; i < ${#args[@]}; i++)); do
    [ "${args[$i]}" = "--kind" ] && kind=${args[$((i + 1))]}
    [ "${args[$i]}" = "--windowed" ] && windowed=1
done
if [ $windowed = 0 ] && [ "$BOXGAME_FULLSCREEN_OK" != 1 ]; then
    echo "boxgame_run: full screen takes over this PC's screen - add --windowed, or set BOXGAME_FULLSCREEN_OK=1 (VMs)"
    exit 2
fi
game=nfs3; [ "$kind" = udp ] && game=roadrash

mkdir -p "$D/logs"
cp "$BIN/boxgame.exe" "$BIN/wsock32.dll" "$D/"
rm -f "$D"/logs/okplay-*.log "$D/boxgame.log"
printf '[okplay]\r\nloglevel=3\r\nserver=http://127.0.0.1:8787/okplay\r\nwsock_games=boxgame.exe\r\nwsock_game=%s\r\nwsock_mode=%s\r\nwsock_box_front=%s\r\nuse_password=%s\r\nwsock_join=\r\nexit_watch=0\r\nupdate_check=0\r\ncrash_reports=never\r\nconnection_reports=no\r\n' \
    "$game" "$mode" "$front" "$password" > "$D/okplay.ini"

# a clean environment for okplay: no OKP_* from the caller (a test answer would keep the box from showing)
for v in $(env | sed -n 's/^\(OKP_[A-Za-z0-9_]*\)=.*/\1/p'); do unset "$v"; done
export OKP_STUN=""
[ -n "$close_ms" ] && export OKP_TEST_BOX_CLOSE_MS=$close_ms
for e in "${extra_env[@]}"; do export "$e"; done

echo "boxgame_run: $D/boxgame.exe ${args[*]}  (profile $game, wsock_mode=$mode, wsock_box_front=$front${close_ms:+, boxes close after $close_ms ms})"
set +e
(cd "$D" && ./boxgame.exe --log "$(cygpath -aw "$D/boxgame.log")" "${args[@]}")
echo "boxgame_run: boxgame exit code $?"
oklog=$(ls -t "$D"/logs/okplay-*.log 2>/dev/null | head -1)
[ -z "$oklog" ] && echo "boxgame_run: no okplay log (did the shim load? wsock_games=boxgame.exe in okplay.ini)"
"$PY" "$here/boxgame_check.py" "$D/boxgame.log" "$oklog"
