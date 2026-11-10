#!/usr/bin/env bash
# Builds okplay wsock (okplay for games that use Winsock directly): bin/wsock32.dll, Windows 95/98..11.
set -e
here=$(cd "$(dirname "$0")" && pwd)
OK=$(cd "$here/.." && pwd)
source "$OK/../env.sh"
need_mingw32
mkdir -p "$here/bin"
CPU="-march=pentium-mmx -mtune=generic -D__USE_MINGW_ANSI_STDIO=0"
FLAGS="$CPU -DOKP_WSOCK -O2 -g -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -Wno-format-truncation -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -include $OK/src/okp_stdio.h -I$OK/include -I$OK/src -I$here"
# Lab builds: OKW_LAB_SRC="file.c ..." adds sources kept outside this repo (game-specific measurement)
# and builds bin/lab/wsock32.dll with -DOKW_LAB instead; release builds are unchanged.
OUT="$here/bin"
EXTRA=""
if [ -n "$OKW_LAB_SRC" ]; then OUT="$here/bin/lab"; mkdir -p "$OUT"; FLAGS="$FLAGS -DOKW_LAB"; EXTRA="$OKW_LAB_SRC"; fi
gcc $FLAGS -shared -o "$OUT/wsock32.dll" "$here/wsock.c" "$here/rr_fix.c" "$here/ddshow.c" "$here/wsock32.def" $EXTRA \
    $OK/src/net.c $OK/src/rdv.c $OK/src/lan.c $OK/src/util.c $OK/src/compat.c $OK/src/crash.c $OK/src/report.c $OK/src/upnp.c $OK/src/okp_printf.c $OK/src/okbox.c \
    -Wl,--enable-stdcall-fixup -Wl,--kill-at -static-libgcc -lws2_32 -liphlpapi -lwinhttp -lbcrypt -lversion -lgdi32
bash "$OK/../winedplay/check9x.sh" "$(cygpath -aw "$OUT/wsock32.dll")"
echo "built $OUT/wsock32.dll"
# the test game (a Road Rash-like peer: adverts, join request, hellos), for tests on this PC
gcc -O2 -Wall -o "$here/bin/rrsim.exe" "$here/tests/rrsim.c" -lwsock32
echo "built $here/bin/rrsim.exe"
# the TCP test game (Need for Speed III's socket patterns: comms thread, select, peek framing, connect loop)
gcc -O2 -Wall -o "$here/bin/tcpsim.exe" "$here/tests/tcpsim.c" -lwsock32 -lwinmm
echo "built $here/bin/tcpsim.exe"
