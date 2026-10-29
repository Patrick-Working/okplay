# Machine-specific settings for the build, test and package scripts, found at run time so the
# workspace can live anywhere (paths with spaces are fine). Source it, don't run it:
#   . "$(dirname "$0")/../env.sh"
# Run it directly to see what it found:  bash env.sh
#
# Anything it finds wrong can be set in local.env next to this file (KEY=value lines, bash syntax;
# not shipped, per machine) or in the environment:
#   MINGW32_BIN     folder with the 32-bit (i686) gcc, e.g. /c/msys64/mingw32/bin
#   PY              a Python 3 interpreter (package scripts, rendezvous stand-in, test scripts)
#   AOE2_GAME_DIR   the game's age2_x1 folder (real-game smoke test, intro-video checks)
#   DELIVERABLES    where package.sh writes releases        (default: <workspace>/deliverables)
#   FFMPEG_PREFIX   the static FFmpeg okindeo.dll links     (default: <workspace>/okindeo/ff)
#   CSC             .NET Framework 4 C# compiler for the launcher
#   SITE_FILES      folder of the files the release page's site-only part offers (site-extras.html
#                   links "patches/<file>"; package.sh copies them; neither is in git)

AOE2TOOL="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -f "$AOE2TOOL/local.env" ] && . "$AOE2TOOL/local.env"

_is_i686_gcc() { [ -x "$1/gcc.exe" ] && [ "$("$1/gcc.exe" -dumpmachine 2>/dev/null)" = i686-w64-mingw32 ]; }

if [ -z "$MINGW32_BIN" ]; then
    for _d in /c/msys64/mingw32/bin /mingw32/bin /c/msys32/mingw32/bin; do
        _is_i686_gcc "$_d" && { MINGW32_BIN=$_d; break; }
    done
fi
if [ -z "$MINGW32_BIN" ]; then # MSYS2 installed somewhere else: its uninstall entry knows where
    _loc=$(reg query 'HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall' //s //f InstallLocation 2>/dev/null |
        tr -d '\r' | grep -i 'msys' | sed -E 's/.*REG_SZ +//' | head -1)
    [ -n "$_loc" ] && _is_i686_gcc "$(cygpath -u "$_loc")/mingw32/bin" && MINGW32_BIN="$(cygpath -u "$_loc")/mingw32/bin"
fi
if [ -z "$MINGW32_BIN" ] && command -v i686-w64-mingw32-gcc >/dev/null 2>&1; then
    MINGW32_BIN=$(dirname "$(command -v i686-w64-mingw32-gcc)")
fi

if [ -z "$PY" ]; then # skip the Microsoft Store alias, which only opens the Store
    for _c in python3 python py; do
        _p=$(command -v $_c 2>/dev/null) || continue
        case "$_p" in *WindowsApps*) continue ;; esac
        "$_p" -c 'import sys; sys.exit(sys.version_info < (3, 8))' 2>/dev/null && { PY=$_p; break; }
    done
fi
if [ -z "$PY" ]; then
    for _p in "$(cygpath -u "${LOCALAPPDATA:-/nonexistent}")"/Programs/Python/Python3*/python.exe /c/Python3*/python.exe; do
        [ -x "$_p" ] && PY=$_p
    done
fi

if [ -z "$AOE2_GAME_DIR" ]; then # the registry (a real install), then a game folder beside the workspace
    _exe=$(reg query 'HKLM\SOFTWARE\WOW6432Node\Microsoft\Microsoft Games\Age of Empires II: The Conquerors Expansion\1.0' //v 'EXE Path' 2>/dev/null |
        tr -d '\r' | grep 'EXE Path' | sed -E 's/.*REG_SZ +//')
    [ -n "$_exe" ] && [ -d "$_exe/age2_x1" ] && AOE2_GAME_DIR="$(cygpath -u "$_exe")/age2_x1"
fi
if [ -z "$AOE2_GAME_DIR" ]; then
    for _d in "$AOE2TOOL"/../*/age2_x1; do
        ls "$_d"/age2_x1*.exe >/dev/null 2>&1 && { AOE2_GAME_DIR="$(cd "$_d" && pwd)"; break; }
    done
fi

DELIVERABLES="${DELIVERABLES:-$AOE2TOOL/deliverables}"
FFMPEG_PREFIX="${FFMPEG_PREFIX:-$AOE2TOOL/okindeo/ff}"
CSC="${CSC:-$(cygpath -u "${WINDIR:-C:\\Windows}")/Microsoft.NET/Framework/v4.0.30319/csc.exe}"
export AOE2TOOL MINGW32_BIN PY AOE2_GAME_DIR DELIVERABLES FFMPEG_PREFIX CSC SITE_FILES
unset _d _c _p _loc _exe

# call before compiling: puts the 32-bit gcc first on PATH, or stops with what to do
need_mingw32() {
    if [ -z "$MINGW32_BIN" ]; then
        echo "no 32-bit gcc found. Install MSYS2, then in its shell: pacman -S mingw-w64-i686-gcc mingw-w64-i686-binutils make" >&2
        echo "(or set MINGW32_BIN in $AOE2TOOL/local.env)" >&2
        exit 1
    fi
    export PATH="$MINGW32_BIN:$PATH"
}
need_python() {
    [ -n "$PY" ] || { echo "no Python 3 found - install one or set PY in $AOE2TOOL/local.env" >&2; exit 1; }
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    for _v in AOE2TOOL MINGW32_BIN PY AOE2_GAME_DIR DELIVERABLES FFMPEG_PREFIX CSC; do
        printf '%-14s %s\n' "$_v" "${!_v:-(not found)}"
    done
fi
