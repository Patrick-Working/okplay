#!/usr/bin/env bash
# Build Wine's dplayx (port/, see WINE_COMMIT) as 32-bit Windows DLLs:
#   bin/dropin/dplayx.dll   the drop-in: + okplay's connections built in, okplay logging/crash
#                           capture, intro-video hook (okindeo.dll copied alongside)
#   bin/plain/dplayx.dll    Wine + our DirectPlay fixes only; providers from the registry
#                           (for Wine's conformance tests and A/B checks)
set -e
cd "$(dirname "$0")"
. ../env.sh
need_mingw32
OK=../okplay
mkdir -p obj/plain obj/dropin obj/okplay bin/plain bin/dropin
# one DLL for Windows 95/98 .. 11 on any x86 since the Pentium MMX (check9x.sh verifies it)
CPU="-march=pentium-mmx -mtune=generic -D__USE_MINGW_ANSI_STDIO=0"
WFLAGS="$CPU -O2 -g -fno-omit-frame-pointer -Wall -Wno-unused -Wno-discarded-qualifiers -D__WINESRC__ -include shim/wine_compat.h -Iport/include -Iport/dlls/dplayx"
LIBS="-ldxguid -luuid -lwinmm -lole32 -luser32 -ladvapi32"

build_wine() { # variant extra-cflags
    local v=$1 extra=$2 objs=""
    for f in port/dlls/dplayx/*.c shim/winedbg_shim.c shim/com_exports.c shim/mode.c shim/switch.c shim/win9x.c shim/gamefix.c; do
        local o=obj/$v/$(basename "$f" .c).o
        [[ $v == plain && ( $(basename "$f") == builtin.c || $(basename "$f") == mode.c || $(basename "$f") == switch.c || $(basename "$f") == win9x.c || $(basename "$f") == gamefix.c ) ]] && continue
        gcc $WFLAGS $extra -c -o "$o" "$f" || { echo "build: $f failed to compile" >&2; exit 1; }   # never link a stale object
        objs="$objs $o"
    done
    echo "$objs"
}

# plain
P=$(build_wine plain "")
gcc -shared -o bin/plain/dplayx.dll $P shim/dplayx.def -Wl,--enable-stdcall-fixup -static-libgcc $LIBS

# drop-in: Wine + okplay (compiled with okplay's own flags) + version resource
# (the drop-in's printf family: okplay's okp_printf.c, see okp_stdio.h)
D=$(build_wine dropin "-DOKP_BUILTIN -include $OK/src/okp_stdio.h -iquote $OK/src")
OFLAGS="$CPU -O2 -g -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -Wno-format-truncation -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -DOKP_BUILTIN -include $OK/src/okp_stdio.h -I$OK/include -I$OK/src"
O=""
for f in sp net rdv lan util crash compat okp_printf report; do
    gcc $OFLAGS -c -o obj/okplay/$f.o $OK/src/$f.c
    O="$O obj/okplay/$f.o"
done
windres -DOKP_BUILTIN -I$OK/src -O coff -o obj/dropin/dplayx_res.o shim/dplayx.rc
# the public DirectPlay entry points go through the mode switch (shim/mode.c: okplay.ini mode=)
{ sed -E 's/^(DirectPlay[A-Za-z]*|DllGetClassObject)=([A-Za-z]+@)/\1=MODE_\2/' shim/dplayx.def
  echo "SPInit=SPInit@4"; } > obj/dropin/dplayx.def
grep -q "DirectPlayCreate=MODE_DirectPlayCreate@12" obj/dropin/dplayx.def || { echo "mode switch not wired into the exports"; exit 1; }
gcc -shared -o bin/dropin/dplayx.dll $D $O obj/dropin/dplayx_res.o obj/dropin/dplayx.def \
    -Wl,--enable-stdcall-fixup -static-libgcc $LIBS -lwsock32 -lversion -lvfw32 -lgdi32
# (no -lwinhttp -lbcrypt -liphlpapi: okplay's compat.c loads those at run time, so Windows 9x can load us)
cp $OK/bin/okindeo.dll bin/dropin/ 2>/dev/null || echo "note: $OK/bin/okindeo.dll missing (codec/build.sh)"

# negative-control build for tests: the Wine side compiled with DPLAYX_TEST_SKIP_AUTH (DP_SenderOwns
# trusts everyone, as 0.7 did), the same okplay objects. run_local.sh `rogue` runs the attack against
# both and checks the fixed build refuses it while this one does not.
#   VARIANT=noauth VARIANT_CFLAGS=-DDPLAYX_TEST_SKIP_AUTH ./build.sh
if [ -n "${VARIANT:-}" ]; then
    mkdir -p obj/$VARIANT bin/$VARIANT
    V=$(build_wine $VARIANT "-DOKP_BUILTIN -include $OK/src/okp_stdio.h -iquote $OK/src ${VARIANT_CFLAGS:-}")
    gcc -shared -o bin/$VARIANT/dplayx.dll $V $O obj/dropin/dplayx_res.o obj/dropin/dplayx.def \
        -Wl,--enable-stdcall-fixup -static-libgcc $LIBS -lwsock32 -lversion -lvfw32 -lgdi32
    cp $OK/bin/okindeo.dll bin/$VARIANT/ 2>/dev/null || true
    echo "variant $VARIANT: bin/$VARIANT/dplayx.dll (${VARIANT_CFLAGS:-})"
fi

# dplay.dll: DirectX 3 era games import DPLAY.DLL; this hands them to the drop-in's dplayx.dll
gcc $CPU -O2 -Wall -shared -o bin/dropin/dplay.dll shim/dplay_forward.c shim/dplay.def -Wl,--enable-stdcall-fixup -static-libgcc
bash check9x.sh "$(pwd)/bin/dropin/dplay.dll"
for v in plain dropin; do
    printf "%-7s %8s bytes  imports: " $v "$(stat -c %s bin/$v/dplayx.dll)"
    objdump -p bin/$v/dplayx.dll | grep "DLL Name" | sed 's/.*DLL Name: //' | tr '\n' ' '; echo
done
# (matches both older and newer binutils' export-table layout)
EXP=$(objdump -p bin/dropin/dplayx.dll | grep -E "[[:space:]](SPInit|DirectPlayCreate|DirectPlayLobbyCreateA)$")
echo "$EXP" | sed 's/^/  export /'
[ "$(echo "$EXP" | grep -c .)" -eq 3 ] || { echo "dropin dplayx.dll is missing an export"; exit 1; }
# Windows 98 on a Pentium MMX must be able to load it
bash check9x.sh bin/dropin/dplayx.dll
