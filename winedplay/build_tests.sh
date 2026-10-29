#!/usr/bin/env bash
# Build Wine's dplayx conformance test (port/dlls/dplayx/tests/dplayx.c) as a Windows exe:
#   bin/tests/dplayx_test.exe
# shim/test_compat.h routes COM creation to whichever dplayx.dll the process loaded, so the same exe
# measures Windows' dplayx (nothing next to it) or ours (a copy next to it). Runner: ../conformance/run_s1.sh
set -e
cd "$(dirname "$0")"
. ../env.sh
need_mingw32
mkdir -p bin/tests
gcc -O1 -g -DSTANDALONE -D__WINESRC__ -w -include shim/test_compat.h -Iport/include \
    -o bin/tests/dplayx_test.exe port/dlls/dplayx/tests/dplayx.c shim/winedbg_shim.c \
    -lole32 -loleaut32 -luuid -ldxguid -lws2_32 -ladvapi32 -luser32 -lversion
ls -la bin/tests/dplayx_test.exe
