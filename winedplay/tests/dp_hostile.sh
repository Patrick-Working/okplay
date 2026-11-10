#!/usr/bin/env bash
# Hostile input to the drop-in's DirectPlay (tests/dp_hostile.c): builds the dplayx sources twice into
# test exes - plain, and with OKP_BUILTIN (the drop-in's sender checks; okplay itself is faked) - and
# runs every case in its own process. Exit 1 if any case fails or crashes.
#
#   tests/dp_hostile.sh [fuzz-iterations] [seed]
#   DP_SRC=<folder with the dplayx sources>   test other sources (default: port/dlls/dplayx), e.g. an
#                                             older tree, to see the cases fail there
# Nothing here touches the network.

cd "$(dirname "$0")/.."
. ../env.sh
need_mingw32
ITER=${1:-3000}
SEED=${2:-1}
SRC=${DP_SRC:-port/dlls/dplayx}
OUT=obj/dp_hostile
mkdir -p "$OUT" bin/tests
CPU="-march=pentium-mmx -mtune=generic -D__USE_MINGW_ANSI_STDIO=0"
WFLAGS="$CPU -O1 -g -fno-omit-frame-pointer -w -D__WINESRC__ -include shim/wine_compat.h -Iport/include -I$SRC"
LIBS="-ldxguid -luuid -lwinmm -lole32 -luser32 -ladvapi32"
fails=0

build() { # variant extra-cflags -> bin/tests/dp_hostile_<variant>.exe
    local v=$1 extra=$2 objs="" f o
    mkdir -p "$OUT/$v"
    for f in "$SRC"/*.c; do
        case $(basename "$f") in builtin.c) continue ;; esac
        o=$OUT/$v/$(basename "$f" .c).o
        gcc $WFLAGS $extra -c -o "$o" "$f" || return 1
        objs="$objs $o"
    done
    gcc $WFLAGS -c -o "$OUT/$v/winedbg_shim.o" shim/winedbg_shim.c || return 1
    gcc $WFLAGS $extra -c -o "$OUT/$v/dp_hostile.o" tests/dp_hostile.c || return 1
    gcc -o bin/tests/dp_hostile_$v.exe $objs "$OUT/$v/winedbg_shim.o" "$OUT/$v/dp_hostile.o" \
        -Wl,--enable-stdcall-fixup -static-libgcc $LIBS || return 1
}

for v in plain okp; do
    extra=""; [[ $v == okp ]] && extra="-DOKP_BUILTIN -DDP_HOSTILE_STUBS"
    if ! build $v "$extra"; then echo "FAIL build ($v)"; fails=$((fails+1)); continue; fi
    echo "== $v ($SRC)"
    for c in joinoffset sizedstring unsolicited shortreq dp1sysmsg idwrap dupcreate gameowner replysource; do
        r=$(DPLAYX_DEBUG=-all DPLAYX_LOG=nul ./bin/tests/dp_hostile_$v.exe $c 2>&1)
        echo "$r" | sed 's/^/  /'
        echo "$r" | grep -q "^PASS $c\|^SKIP $c" || fails=$((fails+1))
    done
    for c in fuzz fuzzjoin; do
        n=$ITER; [[ $c == fuzzjoin ]] && n=$((ITER / 10 + 1))
        r=$(DPLAYX_DEBUG=-all DPLAYX_LOG=nul ./bin/tests/dp_hostile_$v.exe $c $n $SEED 2>&1)
        echo "$r" | grep -v "^dplay:" | sed 's/^/  /'
        echo "$r" | grep -q "^PASS $c" || fails=$((fails+1))
    done
done
if [[ $fails == 0 ]]; then echo "dp_hostile: PASS"; else echo "dp_hostile: FAIL ($fails)"; exit 1; fi
