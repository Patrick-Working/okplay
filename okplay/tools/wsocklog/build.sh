#!/usr/bin/env bash
# Builds wsocklog's wsock32.dll (32-bit) into okplay/tools/wsocklog/bin/.
set -e
here=$(cd "$(dirname "$0")" && pwd)
source "$here/../../../env.sh"
need_mingw32
mkdir -p "$here/bin"
gcc -O2 -march=i586 -Wall -shared -static-libgcc -o "$here/bin/wsock32.dll" "$here/wsocklog.c" "$here/wsock32.def" \
    -Wl,--enable-stdcall-fixup -Wl,--kill-at
echo "built $here/bin/wsock32.dll"
