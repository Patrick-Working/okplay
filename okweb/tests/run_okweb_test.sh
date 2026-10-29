#!/usr/bin/env bash
# Serves okweb/ and a room server on localhost for the browser test and the demo:
#   okweb/tests/run_okweb_test.sh            room server = okplay/server/rdv_standin.py (port 8799)
#   PHP=/path/to/php.exe okweb/tests/run_okweb_test.sh   room server = okplay.php on PHP (port 8800)
# Then open the printed test URL (any WebRTC browser) and wait for "done: N passed, 0 failed".
# The page is served from a different port than the room server, so CORS is exercised too.
# Ctrl+C stops both servers.
cd "$(dirname "$0")/.."
PY=${PY:-python}
"$PY" -m http.server 8798 --bind 127.0.0.1 > /dev/null 2>&1 &
PIDS=$!
if [ -n "$PHP" ]; then
    W=$(mktemp -d)
    cp ../okplay/server/okplay.php "$W/"
    "$PHP" -S 127.0.0.1:8800 -t "$W" > /dev/null 2>&1 &
    PIDS="$PIDS $!"
    SRV=http://127.0.0.1:8800/okplay.php
else
    "$PY" ../okplay/server/rdv_standin.py --port 8799 > /dev/null 2>&1 &
    PIDS="$PIDS $!"
    SRV=http://127.0.0.1:8799/okplay
fi
trap 'kill $PIDS 2>/dev/null' EXIT
sleep 1
echo "test: http://127.0.0.1:8798/tests/okweb_test.html?server=$SRV"
echo "demo: http://127.0.0.1:8798/demo/index.html?server=$SRV   (open it twice: host in one, join in the other)"
wait
