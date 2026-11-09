#!/usr/bin/env bash
# Builds bin/rrf_sim.dll: rr_fix.c (unchanged code, RRF_SIM mode) for a 64-bit replay simulator (see rrf_sim.c).
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
OK=$(cd "$here/.." && pwd)
GCC=${MINGW64_GCC:-/c/msys64/mingw64/bin/gcc.exe}
mkdir -p "$here/bin"
"$GCC" -O2 -Wall -Wno-unused-function -shared -DRRF_SIM -DOKP_WSOCK -I"$OK/include" -I"$OK/src" -I"$here" \
    -o "$here/bin/rrf_sim.dll" "$here/rr_fix.c" "$here/tests/rrf_sim.c" -Wl,--export-all-symbols
echo "built $here/bin/rrf_sim.dll"
