#!/usr/bin/env bash
# Assemble the release - one download for every game - branded by ../okplay/package/okplay.ini:
#   <deliverables>/<brand> v<ver>/                  what players copy (DirectPlay drop-in, wsock32.dll,
#                                                   Connection Check, movie helper) + README, okplay.ini, SOURCE.txt
#   <deliverables>/upload to <host> (v<ver>)/       okplay.php + okstats.php + index.html + the zip
# <ver> is the drop-in's version (OKP_BUILTIN in ../okplay/src/version.h); wsock32.dll carries its own
# (OKP_WSOCK, the Winsock edition). Everything is rebuilt from this tree.
# The source (LGPL) is this repository at the tag v<ver>: the page and SOURCE.txt link it (OKPLAY_REPO
# overrides the address), and FFmpeg's at ffmpeg.org. Tag the release: git tag v<ver> && git push origin v<ver>.
# (<deliverables> = DELIVERABLES from ../env.sh)
# The exact debug builds stay in builds\<ver>\ and builds\wsock-<wsock ver>\ (crash offsets -> source lines
# with addr2line).
set -e
cd "$(dirname "$0")"
. ../env.sh
need_mingw32
need_python
OK=../okplay
# one build at a time: the movie decoder, the drop-in, Connection Check (with okplay), the Winsock edition
( cd $OK && codec/build.sh > /dev/null )
./build.sh > /dev/null
( cd $OK && ./build.sh > /dev/null )
$OK/wsock/build.sh > /dev/null
ver_of() { grep -A6 "$1" $OK/src/version.h | grep -m1 -o 'OKP_VERSION_STR "[^"]*"' | cut -d'"' -f2; }
VER=$(ver_of "ifdef OKP_BUILTIN")
WVER=$(ver_of "defined(OKP_WSOCK)")
[ -n "$VER" ] && [ -n "$WVER" ] || { echo "version.h: no drop-in or wsock version found"; exit 1; }

ini() { grep -m1 "^$1=" $OK/package/okplay.ini | cut -d= -f2- | tr -d '\r'; }
BRAND=$(ini brand); ONLINE=$(ini online_name); LAN=$(ini lan_name); SERVER=$(ini server); KEY=$(ini key)
HOST=$(echo "$SERVER" | sed -E 's#^[a-z]+://([^/]+).*#\1#')

DEL=$DELIVERABLES
NAME="$BRAND v$VER"
REL="$DEL/$NAME"
UP="$DEL/upload to $HOST (v$VER)"
ZIP="$(echo "$BRAND" | tr ' ' '-')-v$VER.zip"
REPO="${OKPLAY_REPO:-https://github.com/Patrick-Working/okplay}"
SITE="$(cygpath -w "$AOE2TOOL/site-extras.html" 2>/dev/null || echo "$AOE2TOOL/site-extras.html")"   # the page's site-only parts (not in git)
SRCURL="$REPO/tree/v$VER"
FFSRC="https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz"
git -C .. rev-parse -q --verify "refs/tags/v$VER" > /dev/null 2>&1 ||
    echo "warning: no tag v$VER here - the page and SOURCE.txt link $SRCURL: git tag v$VER && git push origin v$VER"
# the update notices: the room server must already announce this Winsock edition (okplay.php is uploaded with
# the release, and wsock32.dll compares itself with LATEST_WSOCK_VERSION)
grep -q "^const LATEST_WSOCK_VERSION = '$WVER';" $OK/server/okplay.php ||
    { echo "okplay.php: LATEST_WSOCK_VERSION is not $WVER (version.h) - set it and its note first"; exit 1; }
rm -rf "$REL" "$UP"
mkdir -p "$REL" "$UP"

# a file's version resource, as Windows' Properties shows it
verinfo() { powershell -NoProfile -Command "(Get-Item '$(cygpath -w "$1")').VersionInfo.$2" | tr -d '\r'; }

strip --strip-debug -o "$REL/dplayx.dll" bin/dropin/dplayx.dll
strip --strip-debug -o "$REL/dplay.dll" bin/dropin/dplay.dll   # for DirectX 3 era games (DPLAY.DLL)
# AoE2 refuses to start with a dplayx that reports a pre-DirectX version (it did with 0.3.0.0)
FV=$(verinfo "$REL/dplayx.dll" FileMajorPart)
[ "${FV//[^0-9]/}" -ge 5 ] || { echo "dplayx.dll reports file version major $FV - games reject it"; exit 1; }
# our own version is in its Comments (Connection Check reads it from there)
verinfo "$REL/dplayx.dll" Comments | grep -q "drop-in v$VER\b" || { echo "dplayx.dll: Comments don't say drop-in v$VER"; exit 1; }
cp $OK/bin/okindeo.dll "$REL/"
# Connection Check: lets players test hosting/joining, the room server, ping and delivery before a game
strip --strip-debug -o "$REL/netcheck.exe" $OK/bin/netcheck.exe
[ "$(verinfo "$REL/netcheck.exe" ProductVersion)" = "$VER" ] || { echo "netcheck.exe doesn't report $VER"; exit 1; }
# okplay wsock: online play for games that use Winsock directly (Road Rash, Need for Speed III); in any
# other game it does nothing
strip --strip-debug -o "$REL/wsock32.dll" $OK/wsock/bin/wsock32.dll
[ "$(verinfo "$REL/wsock32.dll" ProductVersion)" = "$WVER" ] || { echo "wsock32.dll doesn't report $WVER"; exit 1; }
# optional, in its own folder: only for games whose intro movies are played by a separate
# program (Steam's Galactic Battlegrounds) - see README "INTRO MOVIES PLAYED BY ANOTHER PROGRAM"
mkdir -p "$REL/movie helper"
cp $OK/bin/moviehelper/version.dll "$REL/movie helper/"

fill() { # src dst [crlf: 1 = Windows line ends (default), 0 = as is]
    "$PY" - "$1" "$2" "${3:-1}" "$BRAND" "$ONLINE" "$LAN" "$SERVER" "$KEY" "$VER" "$ZIP" "${ZIPSIZE:-}" "${DATE:-}" "$SRCURL" "$FFSRC" "$SITE" "$HOST" "$WVER" <<'PYEOF'
import os, sys
src, dst, crlf, brand, online, lan, server, key, ver, zipn, size, date, srcurl, ffsrc, site, host, wver = sys.argv[1:18]
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
             "ZIP": zipn, "ZIPSIZE": size, "DATE": date, "SRCURL": srcurl, "FFSRC": ffsrc, "HOST": host,
             "WSOCKVER": wver}.items():
    s = s.replace("{{" + k + "}}", v)
if crlf == "1":
    s = s.replace("\r\n", "\n").replace("\n", "\r\n")
open(dst, "w", encoding="utf-8", newline="").write(s)
PYEOF
}
fill package/README.txt "$REL/README.txt"
fill package/okplay.ini "$REL/okplay.ini"
cat > "$REL/SOURCE.txt" <<EOF
Source code for $BRAND v$VER (LGPL 2.1+):

  $SRCURL
      the exact source of this release (tag v$VER), with the build scripts:
      dplayx.dll, dplay.dll - Wine's dplayx (commit $(cat WINE_COMMIT)) with our changes and okplay
      wsock32.dll           - okplay wsock $WVER (okplay/wsock), for games that use Winsock directly
      netcheck.exe          - Connection Check (okplay/tools)
      okindeo.dll, movie helper\\version.dll - the intro-movie decoder (okplay/codec)
  $FFSRC
      the FFmpeg release okindeo.dll is built from (its Indeo 5 decoder only)
EOF
sed -i 's/$/\r/' "$REL/SOURCE.txt"
# every text file names its version, and none has a placeholder left
for f in "$REL"/*.txt "$REL"/okplay.ini; do
    grep -q "{{" "$f" && { echo "unfilled placeholder in $f"; exit 1; }
    grep -q "v$VER\b" "$f" || { echo "$f doesn't say v$VER"; exit 1; }
done

# zip with the folder inside, so "extract here" gives one tidy folder
"$PY" - "$REL" "$UP/$ZIP" "$NAME" <<'PYEOF'
import os, sys, zipfile
src, out, top = sys.argv[1:4]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for dp, dn, fn in os.walk(src):
        dn.sort()
        for f in sorted(fn):
            full = os.path.join(dp, f)
            z.write(full, os.path.join(top, os.path.relpath(full, src)))
PYEOF

# the Wine diff that ships with the source: regenerated from the two trees every time (the checked-in
# copy once fell behind port/ by two days). diff exits 1 when the trees differ - that's the point.
diff -ruN src port > wine-dplayx-windows.patch || [ $? -eq 1 ]
grep -q "^+++ port/dlls/dplayx/builtin.c" wine-dplayx-windows.patch || { echo "wine-dplayx-windows.patch looks wrong"; exit 1; }


# the server side: rendezvous script, download page (live game list), private stats page
# (SELF-HOSTING.md is in the source, where the page points; it isn't uploaded)
cp $OK/server/okplay.php $OK/server/okstats.php "$UP/"
# the update notice: the server announces THIS release as the newest (the note stays as written)
sed -i "s/^const LATEST_VERSION = '[^']*';/const LATEST_VERSION = '$VER';/" "$UP/okplay.php"
grep -q "^const LATEST_VERSION = '$VER';" "$UP/okplay.php" || { echo "okplay.php: LATEST_VERSION not set to $VER"; exit 1; }
# (the scripts carry their own version line, changed with every upload)
for f in okplay.php okstats.php; do
    grep -q "Script version: " "$UP/$f" || { echo "$f has no 'Script version:' line"; exit 1; }
done
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
grep -q "v$VER\b" "$UP/index.html" || { echo "index.html doesn't say v$VER"; exit 1; }

mkdir -p "builds/$VER" "builds/wsock-$WVER"
cp bin/dropin/dplayx.dll "builds/$VER/dplayx-debug.dll"
cp $OK/wsock/bin/wsock32.dll "builds/wsock-$WVER/wsock32.dll"

echo "release: $REL"
( cd "$REL" && ls -la && md5sum dplayx.dll wsock32.dll okindeo.dll )
echo "upload:  $UP"
( cd "$UP" && ls -la . )
