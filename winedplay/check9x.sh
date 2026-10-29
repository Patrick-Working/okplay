#!/usr/bin/env bash
# Would this DLL load and run on Windows 98 on a Pentium MMX?  check9x.sh <dll>...
#
#  1. every import must be exported by Windows 98 SE's own system DLLs (win98-exports/*.txt, the
#     export tables of a real install - the 86Box VM's C:\WINDOWS\SYSTEM). A missing one = Windows 98
#     refuses to start the game ("A required .DLL file ... was not found" / "linked to missing export").
#  2. system "W" functions: Windows 9x exports most of them only as stubs that fail. Allowed only if
#     listed in win98-exports/ALLOWED_W.txt (really implemented on 9x, or only called on NT).
#  3. no instruction newer than the Pentium MMX (i586): no cmov/fcmov/fcomi (P6), no SSE.
#     MinGW's prebuilt pseudo-relocator has cmov in the code that patches each pseudo relocation; with
#     none in the DLL (list start == end) that code never runs, so only then is it let through.
#  4. a DLL has no .tls section (the compiler's __thread / thread_local): Windows 95/98/ME and XP
#     give it no TLS slot when it is loaded with LoadLibrary after the program started - which is
#     how DirectX 3-era games load dplayx.dll (DPLAY.DLL at the first call). The first access then
#     crashes (Necrodome on Windows 98, 2026-10-22). Use TlsAlloc.
# Exit 1 on any finding.
cd "$(dirname "$0")"
. ../env.sh
need_mingw32
EXP=win98-exports
fail=0
for dll in "$@"; do
    [ -f "$dll" ] || { echo "check9x: no $dll"; fail=1; continue; }
    name=$(basename "$dll")
    bad_here=0
    n=0
    while read -r lib fn; do
        n=$((n + 1))
        up=$(echo "${lib%.*}" | tr a-z A-Z)
        if [ ! -f "$EXP/$up.txt" ]; then
            echo "check9x: $name imports $lib!$fn - Windows 98 has no $lib"; bad_here=1
        elif ! grep -qx "$fn" "$EXP/$up.txt"; then
            echo "check9x: $name imports $lib!$fn - Windows 98's $lib doesn't export it"; bad_here=1
        elif [[ $up =~ ^(KERNEL32|USER32|GDI32|ADVAPI32|SHELL32|COMDLG32|COMCTL32)$ && $fn =~ W$ ]] &&
             ! grep -qx "$fn" $EXP/ALLOWED_W.txt; then
            echo "check9x: $name imports $lib!$fn - a Unicode function Windows 9x only stubs (see ALLOWED_W.txt)"; bad_here=1
        fi
    done < <(objdump -p "$dll" | awk '/DLL Name:/{lib=$3; next} lib && /^\t[0-9a-f]+ +<none> +[0-9a-f]+ +[^ ]+$/{print lib, $4}')

    skip='^$'
    rl=$(nm "$dll" 2>/dev/null | awk '$3=="___RUNTIME_PSEUDO_RELOC_LIST__"{a=$1} $3=="___RUNTIME_PSEUDO_RELOC_LIST_END__"{b=$1} END{print (a!="" && a==b) ? 1 : 0}')
    [ "$rl" = 1 ] && skip='^<(__GetPEImageBase|_mark_section_writable)>:$'
    ops='(cmov[a-z]+|fcmov[a-z]+|fu?comip?|sfence|lfence|mfence|prefetch[a-z0-9]*|movnt[a-z]*|clflush)'
    bad=$(objdump -d --no-show-raw-insn "$dll" |
          awk -v skip="$skip" -v ops="$ops" '
              /^[0-9a-f]+ <.*>:$/ { fn = $2; next }
              fn !~ skip && ($0 ~ ("\t" ops "[ \t]") || /%xmm|%ymm/) { print fn "  " $0 }' | head -5)
    if [ -n "$bad" ]; then
        echo "check9x: $name has instructions a Pentium MMX lacks, e.g.:"; echo "$bad" | sed 's/^/    /'; bad_here=1
    fi
    if [[ ${name,,} == *.dll ]] && objdump -h "$dll" | grep -q " \.tls "; then
        echo "check9x: $name has a .tls section (__thread) - breaks when loaded with LoadLibrary on 9x/XP; use TlsAlloc"; bad_here=1
    fi
    [ $bad_here = 0 ] && echo "check9x: $name OK - $n imports, all exported by Windows 98; i586 instructions only; no .tls"
    [ $bad_here = 1 ] && fail=1
done
exit $fail
