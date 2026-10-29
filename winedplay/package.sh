#!/usr/bin/env bash
# Assemble the drop-in release, branded by ../okplay/package/okplay.ini:
#   <deliverables>/<brand> drop-in v<ver>/          the three files players copy + README
#   <deliverables>/upload to <host> (drop-in v<ver>)/  okplay.php + okstats.php + index.html + the zip
# The source (LGPL) is this repository at the tag v<ver>: the page and SOURCE.txt link it (OKPLAY_REPO
# overrides the address), and FFmpeg's at ffmpeg.org. Tag the release: git tag v<ver> && git push origin v<ver>.
# (<deliverables> = DELIVERABLES from ../env.sh)
# and keep the exact debug build in builds\<ver>\ (crash offsets -> source lines with addr2line).
set -e
cd "$(dirname "$0")"
. ../env.sh
need_mingw32
need_python
OK=../okplay
( cd $OK && codec/build.sh > /dev/null )
./build.sh > /dev/null
VER=$(grep -A6 "ifdef OKP_BUILTIN" $OK/src/version.h | grep -o 'OKP_VERSION_STR "[^"]*"' | cut -d'"' -f2)

ini() { grep -m1 "^$1=" $OK/package/okplay.ini | cut -d= -f2- | tr -d '\r'; }
BRAND=$(ini brand); ONLINE=$(ini online_name); LAN=$(ini lan_name); SERVER=$(ini server); KEY=$(ini key)
HOST=$(echo "$SERVER" | sed -E 's#^[a-z]+://([^/]+).*#\1#')

DEL=$DELIVERABLES
NAME="$BRAND drop-in v$VER"
REL="$DEL/$NAME"
UP="$DEL/upload to $HOST (drop-in v$VER)"
ZIP="$(echo "$BRAND" | tr ' ' '-')-dropin-v$VER.zip"
REPO="${OKPLAY_REPO:-https://github.com/Patrick-Working/okplay}"
SITE="$(cygpath -w "$AOE2TOOL/site-extras.html" 2>/dev/null || echo "$AOE2TOOL/site-extras.html")"   # the page's site-only parts (not in git)
SRCURL="$REPO/tree/v$VER"
FFSRC="https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz"
git -C .. rev-parse -q --verify "refs/tags/v$VER" > /dev/null 2>&1 ||
    echo "warning: no tag v$VER here - the page and SOURCE.txt link $SRCURL: git tag v$VER && git push origin v$VER"
rm -rf "$REL" "$UP"
mkdir -p "$REL" "$UP"

strip --strip-debug -o "$REL/dplayx.dll" bin/dropin/dplayx.dll
strip --strip-debug -o "$REL/dplay.dll" bin/dropin/dplay.dll   # for DirectX 3 era games (DPLAY.DLL)
# AoE2 refuses to start with a dplayx that reports a pre-DirectX version (it did with 0.3.0.0)
FV=$(powershell -NoProfile -Command "(Get-Item '$(cygpath -w "$REL/dplayx.dll")').VersionInfo.FileMajorPart")
[ "${FV//[^0-9]/}" -ge 5 ] || { echo "dplayx.dll reports file version major $FV - games reject it"; exit 1; }
cp $OK/bin/okindeo.dll "$REL/"
# Connection Check: lets players test hosting/joining, the room server, ping and delivery before a game
( cd $OK && ./build.sh > /dev/null )
strip --strip-debug -o "$REL/netcheck.exe" $OK/bin/netcheck.exe
# optional fourth file, in its own folder: only for games whose intro movies are played by a separate
# program (Steam's Galactic Battlegrounds) - see README "INTRO MOVIES PLAYED BY ANOTHER PROGRAM"
mkdir -p "$REL/movie helper"
cp $OK/bin/moviehelper/version.dll "$REL/movie helper/"

fill() { # src dst [crlf: 1 = Windows line ends (default), 0 = as is]
    "$PY" - "$1" "$2" "${3:-1}" "$BRAND" "$ONLINE" "$LAN" "$SERVER" "$KEY" "$VER" "$ZIP" "${ZIPSIZE:-}" "${DATE:-}" "$SRCURL" "$FFSRC" "$SITE" <<'PYEOF'
import os, sys
src, dst, crlf, brand, online, lan, server, key, ver, zipn, size, date, srcurl, ffsrc, site = sys.argv[1:16]
s = open(src, encoding="utf-8").read()
# the page's site-only parts (site-extras.html in the workspace root, not in git): "<!-- SITE_WHY -->" and
# "<!-- SITE_PATCHES -->" blocks; without the file, a plain paragraph and no Patches section
extra = {"SITE_WHY": '    <p>A while back, I put together a setup for AoE2 with <a href="https://userpatch.aiscripters.net/">UserPatch</a> and '
                     '<a href="https://github.com/FunkyFr3sh/cnc-ddraw/releases">CnC-DDraw</a> that &ldquo;just works&rdquo; on Windows 10/11. '
                     'Network play over the internet still needed a VPN, and LAN games '
                     '<a href="https://github.com/solemnwarning/ipxwrapper">IPXWrapper</a>, because of how DirectPlay works on modern '
                     'Windows. This drop-in completes the mission of simple click-and-play online and LAN gaming.</p>',
         "SITE_PATCHES": ""}
if site and os.path.exists(site):
    t = open(site, encoding="utf-8").read()
    for k in extra:
        mark = "<!-- " + k + " -->"
        if mark in t:
            part = t.split(mark, 1)[1]
            nxt = part.find("<!-- SITE_")
            extra[k] = (part[:nxt] if nxt >= 0 else part).strip()
for k, v in extra.items():
    s = s.replace("{{" + k + "}}", v)
for k, v in {"BRAND": brand, "ONLINE": online, "LAN": lan, "SERVER": server, "KEY": key, "VERSION": ver,
             "ZIP": zipn, "ZIPSIZE": size, "DATE": date, "SRCURL": srcurl, "FFSRC": ffsrc}.items():
    s = s.replace("{{" + k + "}}", v)
if crlf == "1":
    s = s.replace("\r\n", "\n").replace("\n", "\r\n")
open(dst, "w", encoding="utf-8", newline="").write(s)
PYEOF
}
fill package/README.txt "$REL/README.txt"
fill package/okplay.ini "$REL/okplay.ini"
cat > "$REL/SOURCE.txt" <<EOF
Source code for this drop-in (LGPL 2.1+):

  $SRCURL
      Wine's dplayx (commit $(cat WINE_COMMIT)) with our changes, okplay, the okindeo decoder
      and the build scripts - the exact source of this release (tag v$VER)
  $FFSRC
      the FFmpeg release okindeo.dll is built from (its Indeo 5 decoder only)
EOF
sed -i 's/$/\r/' "$REL/SOURCE.txt"
for f in "$REL"/*.txt "$REL"/okplay.ini; do grep -q "{{" "$f" && { echo "unfilled placeholder in $f"; exit 1; }; done

# zip with the folder inside, so "extract here" gives one tidy folder
"$PY" - "$REL" "$UP/$ZIP" "$NAME" <<'PYEOF'
import os, sys, zipfile
src, out, top = sys.argv[1:4]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for dp, dn, fn in os.walk(src):
        for f in sorted(fn):
            full = os.path.join(dp, f)
            z.write(full, os.path.join(top, os.path.relpath(full, src)))
PYEOF

# the Wine diff that ships with the source: regenerated from the two trees every time (the checked-in
# copy once fell behind port/ by two days). diff exits 1 when the trees differ - that's the point.
diff -ruN src port > wine-dplayx-windows.patch || [ $? -eq 1 ]
grep -q "^+++ port/dlls/dplayx/builtin.c" wine-dplayx-windows.patch || { echo "wine-dplayx-windows.patch looks wrong"; exit 1; }


# the server side: rendezvous script, download page (live game list), self-hosting guide
cp $OK/server/okplay.php $OK/server/okstats.php $OK/docs/SELF-HOSTING.md "$UP/"
# the update notice: the server announces THIS release as the newest (the note stays as written)
sed -i "s/^const LATEST_VERSION = '[^']*';/const LATEST_VERSION = '$VER';/" "$UP/okplay.php"
grep -q "^const LATEST_VERSION = '$VER';" "$UP/okplay.php" || { echo "okplay.php: LATEST_VERSION not set to $VER"; exit 1; }
# files the site-only part of the page offers for download (site-extras.html links "patches/<file>"):
# copied from the folder SITE_FILES names (local.env); none of them is in git
if [ -f "$AOE2TOOL/site-extras.html" ]; then
    for f in $(grep -o 'href="patches/[^"]*"' "$AOE2TOOL/site-extras.html" | sed 's#href="patches/##; s#"$##'); do
        mkdir -p "$UP/patches"
        if [ -n "$SITE_FILES" ] && [ -f "$SITE_FILES/$f" ]; then cp "$SITE_FILES/$f" "$UP/patches/"
        else echo "warning: the page links patches/$f - put it in SITE_FILES (local.env)"; fi
    done
fi
ZIPSIZE=$("$PY" -c "import os,sys; b=os.path.getsize(sys.argv[1]); print(f'{b/1048576:.1f} MB' if b >= 1048576 else f'{b//1024} KB')" "$UP/$ZIP")
DATE=$(date +"%B %-d, %Y")
fill package/index.html "$UP/index.html" 0
grep -q "{{" "$UP/index.html" && { echo "unfilled placeholder in $UP/index.html"; exit 1; }

mkdir -p "builds/$VER"
cp bin/dropin/dplayx.dll "builds/$VER/dplayx-debug.dll"

echo "release: $REL"
( cd "$REL" && ls -la && md5sum dplayx.dll okindeo.dll )
echo "upload:  $UP"
( cd "$UP" && ls -la . )
