#!/usr/bin/env bash
# Our DirectPlay (winedplay/port/dlls/dplayx, Wine-derived) built for the browser with Emscripten:
#   okweb/dplay/build_wasm.sh          -> okweb/dplay/out/libdplayx_web.a
# The same C files as the Windows build (minus builtin.c, the drop-in's Windows UI), on winweb
# (win32/winweb.h + winweb.c: the 64 Windows functions dplayx uses, for a one-thread page).
# Needs C:\emsdk; emsdk's own Python (the system one may be older than Emscripten accepts).
# -fno-builtin: WCHAR is 16-bit here (-fshort-wchar), but the optimizer turns "count to the zero" loops
# into calls to the C library's wcslen, which counts 32-bit characters (lstrlenW came out halved).
set -e
cd "$(dirname "$0")"
EMSDK=${EMSDK:-/c/emsdk}
export EMSDK_PYTHON=${EMSDK_PYTHON:-$EMSDK/python/3.13.3_64bit/python.exe}
EMCC="$EMSDK/upstream/emscripten/emcc.exe"
EMAR="$EMSDK/upstream/emscripten/emar.exe"
W=../../winedplay
mkdir -p out/obj
CFLAGS="-O2 -g -fshort-wchar -fno-builtin -Wall -Wno-unused -Wno-unused-parameter -Wno-pointer-sign -Wno-incompatible-pointer-types-discards-qualifiers
        -D__WINESRC__ -DWINE_NOWINSOCK -DOKWEB_DPLAY -Iwin32 -Iinclude -I$W/port/include -I$W/port/dlls/dplayx"
objs=""
for f in $W/port/dlls/dplayx/*.c $W/shim/winedbg_shim.c winweb.c okweb_sp.c okweb_password.c guids.c okweb_flat.c ${EXTRA_SRC:-}; do
    case $(basename "$f") in builtin.c) continue ;; esac
    o=out/obj/$(basename "$f" .c).o
    "$EMCC" $CFLAGS -c -o "$o" "$f" || { echo "build_wasm: $f failed" >&2; exit 1; }
    objs="$objs $o"
done
rm -f out/libdplayx_web.a
"$EMAR" rcs out/libdplayx_web.a $objs
echo "built out/libdplayx_web.a ($(echo $objs | wc -w) objects)"
