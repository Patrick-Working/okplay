#!/usr/bin/env bash
# Builds okplay wsock as dbghelp.dll, for games that import ws2_32.dll (a Windows "known DLL" that a wsock32.dll or
# ws2_32.dll next to the exe can't replace) but also dbghelp.dll: unmodified dethrace (Carmageddon).
# proxy_dbghelp.c passes the debug functions to Windows' dbghelp.dll; wsock.c (OKW_PROXY) points the game's own
# Winsock imports at okplay, in memory. Output: bin/proxy32/dbghelp.dll (32-bit games);
# "build_proxy.sh 64": bin/proxy64/dbghelp.dll (64-bit games, MSYS2's mingw64 compiler).
set -e
ARCH=${1:-32}
here=$(cd "$(dirname "$0")" && pwd)
OK=$(cd "$here/.." && pwd)
source "$OK/../env.sh"
if [ "$ARCH" = 64 ]; then
    for _d in "$MINGW64_BIN" /c/msys64/mingw64/bin /mingw64/bin; do
        [ -n "$_d" ] && [ -x "$_d/gcc.exe" ] && { export PATH="$_d:$PATH"; break; }
    done
    [ "$(gcc -dumpmachine)" = x86_64-w64-mingw32 ] || { echo "no 64-bit MinGW gcc (set MINGW64_BIN)"; exit 1; }
    OUT="$here/bin/proxy64"
    CPU="-D__USE_MINGW_ANSI_STDIO=0"
    GAMES="$here/games/none64.c"   # (the game patches are 32-bit code for 32-bit games)
else
    need_mingw32
    OUT="$here/bin/proxy32"
    CPU="-march=pentium-mmx -mtune=generic -D__USE_MINGW_ANSI_STDIO=0"
    GAMES="$here/games/roadrash/rr_fix.c $here/games/nfs3/nfs3_fix.c $here/games/nfs3/nfs3_menu.c $here/games/nfs3/nfs3_find.c"
fi
mkdir -p "$OUT"
FLAGS="$CPU -DOKP_WSOCK -DOKW_PROXY -O2 -g -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -Wno-format-truncation -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -include $OK/src/okp_stdio.h -I$OK/include -I$OK/src -I$here -I$here/games/nfs3 -I$here/games/roadrash"
windres -DOKP_WSOCK -I"$OK/src" -O coff -o "$OUT/wsock32_res.o" "$here/wsock32.rc"   # the version, as Properties shows it
gcc $FLAGS -shared -o "$OUT/dbghelp.dll" "$OUT/wsock32_res.o" "$here/wsock.c" "$here/proxy_dbghelp.c" $GAMES "$here/ddshow.c" "$here/nettrace.c" "$here/dbghelp.def" \
    $OK/src/net.c $OK/src/testchan.c $OK/src/rdv.c $OK/src/lan.c $OK/src/util.c $OK/src/compat.c $OK/src/crash.c $OK/src/report.c $OK/src/upnp.c $OK/src/okp_printf.c $OK/src/okbox.c $OK/src/ovl.c \
    -Wl,--enable-stdcall-fixup -Wl,--kill-at -static-libgcc -lws2_32 -liphlpapi -lwinhttp -lbcrypt -lversion -lgdi32
echo "built $OUT/dbghelp.dll"
