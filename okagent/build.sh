#!/usr/bin/env bash
# Build okagent.exe (the in-machine remote control for test automation) and prove it loads on
# Windows 95/98 on a Pentium MMX (../winedplay/check9x.sh).
set -e
cd "$(dirname "$0")"
. ../env.sh
need_mingw32
mkdir -p bin
gcc -O2 -g -march=pentium-mmx -mtune=generic -D__USE_MINGW_ANSI_STDIO=0 -Wall -mwindows -o bin/okagent.exe okagent.c \
    -static-libgcc -lwsock32 -luser32 -lgdi32 -ladvapi32
bash ../winedplay/check9x.sh "$(pwd)/bin/okagent.exe"
ls -la bin/okagent.exe
