#!/usr/bin/env bash
# Build okindeo.dll: the Indeo Video 5 decoder for Video for Windows (fixes AoE2's intro movies,
# which play sound but no picture on Windows 7+). 32-bit, fully static: imports only Windows DLLs.
#
# Needs a minimal static FFmpeg (indeo5 decoder only). Build it once with build-ffmpeg.sh, or
# point FFMPEG_PREFIX at an existing one (default <workspace>/okindeo/ff, see env.sh).
set -e
cd "$(dirname "$0")/.."
. ../env.sh
need_mingw32
FF="$FFMPEG_PREFIX"
if [ ! -f "$FF/lib/libavcodec.a" ]; then
    echo "codec: no FFmpeg at $FF - run codec/build-ffmpeg.sh (skipping okindeo.dll)"; exit 0
fi
mkdir -p bin
gcc -O2 -Wall -shared -static -s -I"$FF/include" codec/okindeo.c -o bin/okindeo.dll \
    -Wl,--kill-at -L"$FF/lib" -lavcodec -lavutil -lvfw32 -lwinmm -lbcrypt -lm
gcc -O2 -Wall -o bin/vfwprobe.exe tools/vfwprobe.c -lvfw32 -lwinmm
# the movie helper: a version.dll for games whose intros are played by a separate program (see
# moviehelper.c). No C runtime, Windows 95-level imports only; check9x verifies it.
mkdir -p bin/moviehelper obj
windres -DOKP_BUILTIN -O coff -o obj/moviehelper_res.o codec/moviehelper.rc
gcc -march=pentium-mmx -mtune=generic -O2 -Wall -ffreestanding -fno-stack-protector -shared -s -nostdlib \
    -o bin/moviehelper/version.dll codec/moviehelper.c obj/moviehelper_res.o codec/moviehelper.def \
    -Wl,-e,_DllMain@12 -Wl,--enable-stdcall-fixup -lkernel32 -luser32 -ladvapi32
[ "$(objdump -p bin/moviehelper/version.dll | grep -cE "[[:space:]](GetFileVersionInfo|Ver)[A-Za-z]+$")" -eq 17 ] ||
    { echo "moviehelper: version.dll does not export all 17 version.dll functions"; exit 1; }
bash ../winedplay/check9x.sh "$PWD/bin/moviehelper/version.dll"
gcc -O2 -Wall -o bin/moviehost.exe tests/moviehost.c -lvfw32 -lwinmm -lversion
gcc -O2 -Wall -o bin/fuzz_indeo.exe tests/fuzz_indeo.c -lvfw32
objdump -p bin/okindeo.dll | grep "DLL Name" | tr -d '\t' | tr '\n' ' '; echo
ls -la bin/okindeo.dll bin/vfwprobe.exe
