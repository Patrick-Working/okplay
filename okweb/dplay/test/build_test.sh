#!/usr/bin/env bash
# The two-tab test of our DirectPlay in the browser (dpweb_test.c):
#   okweb/dplay/test/build_test.sh   -> okweb/dplay/out/test/ (dpweb_test.html/.js/.wasm)
# Serve okweb/ and a room server (okweb/tests/run_okweb_test.sh does both), then open
#   http://127.0.0.1:8798/dplay/out/test/dpweb_test.html?role=host&server=<room server>
# and the same with role=join in a second tab.
set -e
cd "$(dirname "$0")/.."
bash build_wasm.sh
EMSDK=${EMSDK:-/c/emsdk}
export EMSDK_PYTHON=${EMSDK_PYTHON:-$EMSDK/python/3.13.3_64bit/python.exe}
W=../../winedplay
mkdir -p out/test
"$EMSDK/upstream/emscripten/emcc.exe" -O2 -g -fshort-wchar -Wno-incompatible-pointer-types-discards-qualifiers \
    -D__WINESRC__ -DOKWEB_DPLAY -Iwin32 -I$W/port/include -I$W/port/dlls/dplayx \
    test/dpweb_test.c out/libdplayx_web.a -o out/test/dpweb_test.js \
    -sASYNCIFY -sASYNCIFY_STACK_SIZE=262144 -sALLOW_MEMORY_GROWTH=1 -sEXIT_RUNTIME=0 \
    -sEXPORTED_RUNTIME_METHODS=UTF8ToString,ENV
cp test/dpweb_test.html out/test/
echo "built out/test/dpweb_test.{html,js,wasm}"
