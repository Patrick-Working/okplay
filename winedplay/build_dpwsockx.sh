#!/usr/bin/env bash
# Build Wine's TCP/IP service provider (port/dlls/dpwsockx) as bin/dpwsockx/dpwsockx.dll: the provider
# half of a fully-ours DirectPlay stack (outside Windows there is no Microsoft dpwsockx). Used by
# ../conformance/run_s4.sh full (our dplayx + this provider against Windows' dplayx + Windows' provider).
# Like the drop-in it must load on Windows 95/98 on a Pentium MMX: okplay's printf family, the
# shim/win9x.c replacements, and check9x.sh at the end.
set -e
cd "$(dirname "$0")"
. ../env.sh
need_mingw32
OK=../okplay
mkdir -p obj/dpwsockx bin/dpwsockx
CPU="-march=pentium-mmx -mtune=generic -D__USE_MINGW_ANSI_STDIO=0"
FLAGS="$CPU -O2 -g -fno-omit-frame-pointer -Wall -Wno-unused -D__WINESRC__ -include shim/wine_compat.h -include $OK/src/okp_stdio.h -iquote $OK/src -Iport/include"
gcc $FLAGS -Iport/dlls/dpwsockx -c -o obj/dpwsockx/dpwsockx_main.o port/dlls/dpwsockx/dpwsockx_main.c
gcc $FLAGS -c -o obj/dpwsockx/winedbg_shim.o shim/winedbg_shim.c
gcc $FLAGS -c -o obj/dpwsockx/win9x.o shim/win9x.c
gcc $CPU -O2 -g -DOKP_BUILTIN -I$OK/src -c -o obj/dpwsockx/okp_printf.o $OK/src/okp_printf.c
# the "same router" lookup: okplay's UPnP client
gcc $CPU -O2 -g -Wall -include $OK/src/okp_stdio.h -I$OK/src -c -o obj/dpwsockx/upnp.o $OK/src/upnp.c
# okp_win9x_paths (shim/win9x.c asks it whether this is Windows 9x)
gcc $CPU -O2 -g -c -o obj/dpwsockx/win9x_detect.o shim/win9x_detect.c
gcc -shared -o bin/dpwsockx/dpwsockx.dll obj/dpwsockx/dpwsockx_main.o obj/dpwsockx/winedbg_shim.o obj/dpwsockx/win9x.o \
    obj/dpwsockx/okp_printf.o obj/dpwsockx/win9x_detect.o obj/dpwsockx/upnp.o -Wl,--kill-at -static-libgcc -lws2_32 -ldxguid -luuid -ladvapi32
objdump -p bin/dpwsockx/dpwsockx.dll | grep -E "[[:space:]]SPInit$" > /dev/null || { echo "no SPInit export"; exit 1; }
ls -la bin/dpwsockx/dpwsockx.dll
bash check9x.sh "$(pwd)/bin/dpwsockx/dpwsockx.dll"
