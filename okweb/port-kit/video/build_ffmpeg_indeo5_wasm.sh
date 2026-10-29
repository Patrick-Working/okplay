#!/usr/bin/env bash
# FFmpeg's Indeo Video 5 decoder for a game's browser build (okavi.cpp: many late-1990s games' AVI movies
# are Indeo 5, "IV50", which no browser decodes): FFmpeg 7.1, LGPL 2.1+, indeo5 only, static, built with
# Emscripten - the same configuration as the Windows drop-in's okindeo.dll (okplay/codec/build-ffmpeg.sh).
#   okweb/port-kit/video/build_ffmpeg_indeo5_wasm.sh [<dest>]    default <dest>: ./ffmpeg-indeo5
#   -> <dest>/{include,lib}  (and the source tarball, downloaded once, next to them)
# Needs C:\emsdk and MSYS2 (make, and the mingw32 gcc for configure's host tools): run from any
# bash, it re-runs itself in MSYS2's MINGW32 shell.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
DST="$(mkdir -p "${1:-$PWD/ffmpeg-indeo5}" && cd "${1:-$PWD/ffmpeg-indeo5}" && pwd)"
EMSDK=${EMSDK:-/c/emsdk}

if [ "$MSYSTEM" != MINGW32 ] || ! command -v make >/dev/null; then
    MSYS_BASH=/c/msys64/usr/bin/bash.exe
    [ -x "$MSYS_BASH" ] || { echo "needs MSYS2 (make): no $MSYS_BASH"; exit 1; }
    exec env MSYSTEM=MINGW32 CHERE_INVOKING=1 "$MSYS_BASH" -lc '"$0" "$1"' "$(cygpath -m "$HERE/build_ffmpeg_indeo5_wasm.sh")" "$(cygpath -m "$DST")"
fi

export EMSDK_PYTHON=${EMSDK_PYTHON:-$EMSDK/python/3.13.3_64bit/python.exe}
EM="$EMSDK/upstream/emscripten"
BUILD="$(mktemp -d /tmp/ffwasm.XXXXXX)"   # (FFmpeg's configure/make break on paths with spaces)
trap 'rm -rf "$BUILD"' EXIT
TAR="$DST/ffmpeg-7.1.tar.xz"
[ -f "$TAR" ] || { mkdir -p "$(dirname "$TAR")"; curl -L -o "$TAR" https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz; }
tar xf "$TAR" -C "$BUILD"
cd "$BUILD/ffmpeg-7.1"
./configure --cc="$EM/emcc.exe" --cxx="$EM/em++.exe" --ar="$EM/emar.exe" --ranlib="$EM/emranlib.exe" \
    --nm="$EMSDK/upstream/bin/llvm-nm.exe" --host-cc=gcc --target-os=none --arch=x86_32 --enable-cross-compile \
    --disable-everything --enable-decoder=indeo5 \
    --disable-programs --disable-doc --disable-avdevice --disable-avformat --disable-swscale \
    --disable-swresample --disable-avfilter --disable-network --disable-autodetect --disable-asm \
    --disable-pthreads --disable-w32threads --disable-os2threads --disable-runtime-cpudetect \
    --enable-static --disable-shared --enable-small --extra-cflags="-O2" \
    --prefix="$BUILD/ff"
make -j"$(nproc)"
make install
rm -rf "$DST/include" "$DST/lib"
mkdir -p "$DST"
cp -r "$BUILD/ff/include" "$BUILD/ff/lib" "$DST/"
rm -rf "$DST/lib/pkgconfig"
echo "FFmpeg (indeo5, wasm) installed to $DST"
