#!/usr/bin/env bash
# Builds okplay wsock (okplay for games that use Winsock directly): bin/wsock32.dll, Windows 95/98..11.
set -e
here=$(cd "$(dirname "$0")" && pwd)
OK=$(cd "$here/.." && pwd)
source "$OK/../env.sh"
need_mingw32
mkdir -p "$here/bin"
CPU="-march=pentium-mmx -mtune=generic -D__USE_MINGW_ANSI_STDIO=0"
FLAGS="$CPU -DOKP_WSOCK -O2 -g -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -Wno-format-truncation -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -include $OK/src/okp_stdio.h -I$OK/include -I$OK/src"
gcc $FLAGS -shared -o "$here/bin/wsock32.dll" "$here/wsock.c" "$here/wsock32.def" \
    $OK/src/net.c $OK/src/rdv.c $OK/src/lan.c $OK/src/util.c $OK/src/compat.c $OK/src/crash.c $OK/src/report.c $OK/src/upnp.c $OK/src/okp_printf.c \
    -Wl,--enable-stdcall-fixup -Wl,--kill-at -static-libgcc -lws2_32 -liphlpapi -lwinhttp -lbcrypt -lversion
bash "$OK/../winedplay/check9x.sh" "$(cygpath -aw "$here/bin/wsock32.dll")"
echo "built $here/bin/wsock32.dll"
