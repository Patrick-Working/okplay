#!/usr/bin/env bash
# Assemble a release of Classic Netplay for AoE2 from a fresh build, branded by package/okplay.ini:
#   <deliverables>/<brand> v<ver>/                 the folder players get (zipped)
#   <deliverables>/upload to <server host> (v<ver>)/   okplay.php + index.html + the zip
# (<deliverables> = DELIVERABLES from ../env.sh)
# and keep the exact debug build in builds\<ver>\ so crash offsets map back to source lines.
set -e
cd "$(dirname "$0")"
. ../env.sh
need_python
./build.sh > /dev/null
launcher/build.sh > /dev/null
# the installer edition's version: the #else branch (the #ifdef OKP_BUILTIN one is the drop-in's)
VER=$(sed -n '/^#else/,/^#endif/p' src/version.h | grep -o 'OKP_VERSION_STR "[^"]*"' | cut -d'"' -f2)
[[ $VER =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "could not read the installer version from src/version.h: '$VER'"; exit 1; }
grep -q "Version = \"$VER\"" launcher/OldkidPlay.cs || { echo "launcher version != $VER"; exit 1; }

ini() { grep -m1 "^$1=" package/okplay.ini | cut -d= -f2- | tr -d '\r'; }
BRAND=$(ini brand); ONLINE=$(ini online_name); LAN=$(ini lan_name); SERVER=$(ini server)
HOST=$(echo "$SERVER" | sed -E 's#^[a-z]+://([^/]+).*#\1#')
EXE="$BRAND.exe"

DEL=$DELIVERABLES
NAME="$BRAND v$VER"
REL="$DEL/$NAME"
UP="$DEL/upload to $HOST (v$VER)"
ZIP="$(echo "$BRAND" | tr ' ' '-')-v$VER.zip"
rm -rf "$REL" "$UP"
mkdir -p "$REL/support" "$UP"

cp bin/ClassicNetplay.exe "$REL/$EXE"
cp bin/okplay.dll "$REL/"
cp bin/dptest.exe "$REL/support/"
# intro-movie fix: Indeo 5 decoder (FFmpeg, LGPL) + its source/notice + the probe tool
[ -f bin/okindeo.dll ] || { echo "bin/okindeo.dll missing - build FFmpeg with codec/build-ffmpeg.sh"; exit 1; }
cp bin/okindeo.dll "$REL/"
cp bin/vfwprobe.exe "$REL/support/"
mkdir -p "$REL/support/okindeo-source"
cp codec/okindeo.c codec/build.sh codec/build-ffmpeg.sh codec/LICENSE-okindeo.txt "$REL/support/okindeo-source/"
cp server/okplay.php "$UP/"
cp docs/SELF-HOSTING.md "$UP/"
# LGPL: offer the exact FFmpeg source from the same place as the zip
FFSRC="$(dirname "$FFMPEG_PREFIX")/ffmpeg-7.1.tar.xz"
mkdir -p "$UP/source" && cp "$FFSRC" "$UP/source/"

# templates: {{BRAND}} {{ONLINE}} {{LAN}} {{EXE}} {{VERSION}} (+ zip facts for the web page)
fill() { # src dst crlf
    "$PY" - "$1" "$2" "$3" "$BRAND" "$ONLINE" "$LAN" "$EXE" "$VER" "$ZIP" "${ZIPSIZE:-}" "${DATE:-}" <<'PYEOF'
import sys
src, dst, crlf, brand, online, lan, exe, ver, zipn, size, date = sys.argv[1:12]
s = open(src, encoding="utf-8").read()
for k, v in {"BRAND": brand, "ONLINE": online, "LAN": lan, "EXE": exe, "VERSION": ver,
             "ZIP": zipn, "ZIPSIZE": size, "DATE": date}.items():
    if v:
        s = s.replace("{{" + k + "}}", v)
if crlf == "1":
    s = s.replace("\r\n", "\n").replace("\n", "\r\n")
open(dst, "w", encoding="utf-8", newline="").write(s)
PYEOF
}
fill package/README.txt "$REL/README.txt" 1
fill package/okplay.ini "$REL/okplay.ini" 1

# zip with the folder inside, so "extract here" gives one tidy folder
"$PY" - "$REL" "$UP/$ZIP" "$NAME" <<'PYEOF'
import os, sys, zipfile
src, out, top = sys.argv[1:4]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for root, _, files in os.walk(src):
        for f in files:
            full = os.path.join(root, f)
            z.write(full, os.path.join(top, os.path.relpath(full, src)))
PYEOF

ZIPSIZE=$("$PY" -c "import os,sys; b=os.path.getsize(sys.argv[1]); print(f'{b/1048576:.1f} MB' if b >= 1048576 else f'{b//1024} KB')" "$UP/$ZIP")
DATE=$(date +"%B %-d, %Y")
fill server/index.html "$UP/index.html" 0
for f in "$REL/README.txt" "$UP/index.html"; do
    grep -q "{{" "$f" && { echo "unfilled placeholder in $f"; exit 1; }
done

mkdir -p "builds/$VER"
cp bin/okplay.dll bin/ClassicNetplay.exe bin/okindeo.dll "builds/$VER/"

echo "release: $REL"
( cd "$REL" && find . -type f | sort && md5sum okplay.dll "$EXE" )
echo "upload:  $UP"
( cd "$UP" && ls -la )
