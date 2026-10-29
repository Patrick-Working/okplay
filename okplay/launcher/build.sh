#!/usr/bin/env bash
# Build the Classic Netplay for AoE2 launcher with the C# compiler that ships in Windows
# (.NET Framework 4.x, C# 5). package.sh renames it to "<brand>.exe" for a release.
set -e
cd "$(dirname "$0")/.."
. ../env.sh
[ -x "$CSC" ] || { echo "no C# compiler at $CSC (.NET Framework 4) - set CSC in local.env"; exit 1; }
# csc wants Windows paths; stop MSYS from rewriting the -switches
MSYS_NO_PATHCONV=1 "$CSC" -nologo -codepage:65001 -target:winexe -platform:x86 -optimize+ \
    '-out:bin\ClassicNetplay.exe' \
    '-win32icon:launcher\aoe2.ico' \
    -r:System.Management.dll -r:System.IO.Compression.dll -r:System.IO.Compression.FileSystem.dll \
    -r:System.Windows.Forms.dll -r:System.Drawing.dll -r:System.Core.dll \
    'launcher\OldkidPlay.cs'
ls -la bin/ClassicNetplay.exe
