#!/usr/bin/env bash
# Build the S3 wire tap (sptap.c) as bin/dpwsockx.dll. See sptap.c for how to use it.
set -e
cd "$(dirname "$0")"
. ../../env.sh
need_mingw32
mkdir -p bin
gcc -O1 -g -Wall -Wno-unused-parameter -shared -I../../winedplay/port/include -o bin/dpwsockx.dll sptap.c \
    -Wl,--kill-at -static-libgcc -ldxguid -luuid
objdump -p bin/dpwsockx.dll | grep -E "[[:space:]]SPInit$" > /dev/null || { echo "no SPInit export"; exit 1; }
ls -la bin/dpwsockx.dll
