#!/usr/bin/env bash
# Build okplay.dll (32-bit DirectPlay service provider) and dptest.exe.
set -e
cd "$(dirname "$0")"
. ../env.sh
need_mingw32
mkdir -p bin
# -g + frame pointers: crash reports in the log resolve to okplay.dll+offset -> source line (addr2line)
CFLAGS="-O2 -g -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -Wno-format-truncation -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -Iinclude -Isrc"
windres -Isrc -O coff -o bin/okplay_res.o src/okplay.rc
gcc $CFLAGS -shared -o bin/okplay.dll src/sp.c src/net.c src/rdv.c src/lan.c src/util.c src/crash.c src/compat.c src/report.c bin/okplay_res.o \
    -Wl,--kill-at -static-libgcc -lws2_32 -liphlpapi -lwinhttp -lbcrypt -lversion
gcc $CFLAGS -o bin/dptest.exe tools/dptest.c -static-libgcc -lole32 -luuid
# dplaunch.exe: a minimal DirectPlay lobby program (classic TCP/IP sessions on Microsoft's DirectPlay;
# interop tests on machines without the drop-in). Windows 95/98 too.
gcc -O2 -g -march=pentium-mmx -mtune=generic -D__USE_MINGW_ANSI_STDIO=0 -Wall -o bin/dplaunch.exe tools/dplaunch.c -static-libgcc -lole32
bash ../winedplay/check9x.sh "$(pwd)/bin/dplaunch.exe"
# dev loop without admin: dptest.exe (in bin\) resolves the IPX provider's bare "dpwsockx.dll"
# to this copy; run tests with OKP_TEST_AS=online|lan and dptest --sp ipx
cp bin/okplay.dll bin/dpwsockx.dll
objdump -p bin/okplay.dll | grep -E "[[:space:]]SPInit$" || { echo "okplay.dll does not export SPInit"; exit 1; }
ls -la bin/okplay.dll bin/dptest.exe
# test tools: API calls in odd states, the parser fuzzer, the name tests, and the NAT matrix's own copy
gcc -O2 -Wall -o bin/apistate.exe tests/apistate.c -lole32 -luuid -ldxguid
# a DirectX 3 era game's view of the drop-in's dplay.dll forwarder: bin/dplay1_test.exe <folder>
gcc -O2 -Wall -o bin/dplay1_test.exe tests/dplay1_test.c -luuid -ldxguid
# black-box probes of a DPLAY.DLL / dplayx: the player ids it hands out; vanished players and EnableNewPlayers on the wire
gcc -O2 -Wall -o bin/dp1ids.exe tests/dp1ids.c -luuid
gcc -O2 -Wall -o bin/dp1drop.exe tests/dp1drop.c -lole32 -luuid -ldxguid
# Connection Check (tools/netcheck.c), shipped next to the drop-in; must load on Windows 95/98 on a Pentium MMX
windres -Itools -O coff -o bin/netcheck_res.o tools/netcheck.rc
gcc -O2 -g -march=pentium-mmx -mtune=generic -D__USE_MINGW_ANSI_STDIO=0 -Wall -mwindows -o bin/netcheck.exe tools/netcheck.c bin/netcheck_res.o -static-libgcc -lole32 -luuid -ldxguid -lwsock32 -lwinmm -lversion -lshell32 -luser32 -lgdi32 -ladvapi32
bash ../winedplay/check9x.sh "$(pwd)/bin/netcheck.exe"
gcc -O2 -Wall -Wno-misleading-indentation -o bin/gpa.exe tests/gpa.c -lole32 -luuid -ldxguid -ldplayx
gcc -O2 -Wall -o bin/askagain.exe tests/askagain.c -lole32 -luuid -ldxguid
gcc -O1 -g -fno-omit-frame-pointer -Wall -Wno-unused-function -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-cast-function-type -Wno-format-truncation \
    -D_WIN32_WINNT=0x0601 -Iinclude -Isrc -o bin/fuzz.exe tests/fuzz.c src/util.c src/crash.c src/compat.c -lws2_32 -liphlpapi -lwinhttp -lbcrypt -lversion
gcc -O1 -D_WIN32_WINNT=0x0601 -Iinclude -Isrc -o tests/test_names.exe tests/test_names.c src/util.c src/net.c src/lan.c src/crash.c src/compat.c \
    -lws2_32 -liphlpapi -lwinhttp -lbcrypt -lversion
mkdir -p bin_matrix && cp bin/okplay.dll bin_matrix/dpwsockx.dll && cp bin/dptest.exe bin_matrix/
# compat.c / okp_printf.c (the Windows 9x fallbacks) against Windows' own functions
gcc -O1 -g -Wall -Wno-format -D_WIN32_WINNT=0x0601 -DOKP_BUILTIN -Iinclude -Isrc -o bin/test_compat.exe tests/test_compat.c \
    src/compat.c src/okp_printf.c src/util.c src/crash.c -lws2_32 -lbcrypt -lversion
# the intro-movie decoder (needs the static FFmpeg from codec/build-ffmpeg.sh; skipped without it)
codec/build.sh
