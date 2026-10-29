#!/usr/bin/env bash
# The FFmpeg that okindeo.dll links: 7.1, LGPL 2.1+, indeo5 decoder only, static, 32-bit.
#   codec/build-ffmpeg.sh [prefix]      default prefix FFMPEG_PREFIX from ../../env.sh
# Needs MSYS2 (make + the mingw32 gcc); run from any bash, it re-runs itself in MSYS2's MINGW32 shell.
# The source tarball is kept next to the prefix (package.sh ships it as the LGPL source).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../../env.sh"
need_mingw32
PREFIX="$(mkdir -p "${1:-$FFMPEG_PREFIX}" && cd "${1:-$FFMPEG_PREFIX}" && pwd)"

if [ "$MSYSTEM" != MINGW32 ] || ! command -v make >/dev/null; then
    MSYS_BASH="$(dirname "$(dirname "$MINGW32_BIN")")/usr/bin/bash.exe"
    [ -x "$MSYS_BASH" ] || { echo "no MSYS2 bash at $MSYS_BASH"; exit 1; }
    exec env MSYSTEM=MINGW32 CHERE_INVOKING=1 "$MSYS_BASH" -lc '"$0" "$1"' "$(cygpath -m "$HERE/build-ffmpeg.sh")" "$(cygpath -m "$PREFIX")"
fi

WORK="$(dirname "$PREFIX")"
cd "$WORK"
[ -f ffmpeg-7.1.tar.xz ] || curl -LO https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz
# FFmpeg's configure/make break on paths with spaces: build and install in a temp folder, then copy
BUILD="$(mktemp -d /tmp/ffbuild.XXXXXX)"
trap 'rm -rf "$BUILD"' EXIT
tar xf ffmpeg-7.1.tar.xz -C "$BUILD"
cd "$BUILD/ffmpeg-7.1"
# w32threads: Windows' video player calls the codec from its own thread, and an FFmpeg built with
# no threading at all is not safe for that. okindeo still decodes single-threaded (thread_count=1).
./configure --arch=x86 --target-os=mingw32 --disable-everything --enable-decoder=indeo5 \
    --disable-programs --disable-doc --disable-avdevice --disable-avformat --disable-swscale \
    --disable-swresample --disable-avfilter --disable-network --disable-autodetect --disable-x86asm \
    --disable-pthreads --enable-w32threads --enable-static --disable-shared --enable-small \
    --prefix="$BUILD/ff"
make -j"$(nproc)"
make install
rm -rf "$PREFIX"/{include,lib}
cp -r "$BUILD/ff/include" "$BUILD/ff/lib" "$PREFIX/"
echo "FFmpeg installed to $PREFIX"
