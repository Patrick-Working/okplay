#!/usr/bin/env bash
# apidiff - does our dplayx answer every IDirectPlay4 call the way Windows' does?
#
# Runs tests/apistate.c twice over the SAME service provider (Windows' own TCP/IP, so only the
# dplayx layer differs): once on Windows' dplayx (CoCreateInstance), once on ours (okplay.ini
# mode=oldkid + show_windows_connections=1, so it accepts the classic provider). Then lists every
# call whose HRESULT differs, per state. Needs Windows' DirectPlay (Legacy Components) on this PC.
#   apidiff.sh [dplayx.dll]        default: ../winedplay/bin/dropin/dplayx.dll
# Output: tests/out/apidiff/{windows,ours}.txt and the difference table; exit 0 either way (the
# table is the hit-list, not a pass/fail), 2 if a run crashed or could not start. Differences we
# keep on purpose (the "deliberate" list below, docs/DPLAY_CONFORMANCE.md) are listed apart as
# expected; diff.txt marks them with a fifth column.
set -e
cd "$(dirname "$0")/.."
. ../env.sh
DLL="${1:-../winedplay/bin/dropin/dplayx.dll}"
T=tests/out/apidiff
mkdir -p "$T/ours"
cp "$DLL" "$T/ours/dplayx.dll"
printf '[okplay]\r\nmode=oldkid\r\nshow_windows_connections=1\r\nloglevel=2\r\n' > "$T/ours/okplay.ini"
export OKP_SERVER=http://127.0.0.1:9/nothing OKP_STUN=
unset DPTEST_DPLAYX OKP_TEST_AS
OURS="$(cygpath -w "$PWD/$T/ours/dplayx.dll")"   # absolute: apistate runs from bin\

# Windows registers TCP/IP and IPX both as a bare "dpwsockx.dll": started from bin\, Windows' dplayx
# loads okplay's dev copy for TCP/IP too, which refuses (DPERR_UNAVAILABLE) - and then every state is
# "uninitialized" on both sides and the table compares nothing (2026-10-16). Run from a clean folder.
mkdir -p "$T/clean" && cp bin/apistate.exe "$T/clean/"
echo "--- Windows' dplayx + Windows' TCP/IP"
( cd "$T/clean" && ./apistate.exe --sp tcpip all ) > "$T/windows.txt" 2>&1 || true
grep -h "RESULT\|CRASH\|HANG\|create failed" "$T/windows.txt" | sed 's/^/    /'
grep -q "setup InitializeConnection 0x00000000" "$T/windows.txt" || echo "    WARNING: the TCP/IP provider did not initialize - the states are not real"
echo "--- our dplayx + Windows' TCP/IP"
( cd "$T/clean" && ./apistate.exe --dplayx "$OURS" --sp tcpip all ) > "$T/ours.txt" 2>&1 || true
grep -h "RESULT\|CRASH\|HANG\|create failed" "$T/ours.txt" | sed 's/^/    /'

"$PY" - "$T/windows.txt" "$T/ours.txt" <<'PYEOF'
import re, sys
def parse(path):
    out, state = {}, None
    for line in open(path, errors="replace"):
        m = re.match(r"state (\w+)", line)
        if m: state = m.group(1); continue
        m = re.match(r"(call|setup) (.+?)\s+(0x[0-9a-f]{8})\s*$", line)
        if m and state: out.setdefault(state, {})[(m.group(1), m.group(2).strip())] = m.group(3); continue
        m = re.match(r"data (.{30}) (.*)$", line.rstrip("\n"))   # what a call returned (apistate_dp1.c)
        if m and state: out.setdefault(state, {})[("data", m.group(1).strip())] = m.group(2).strip()
    return out
w, o = parse(sys.argv[1]), parse(sys.argv[2])
names = {
 0x00000000:"DP_OK", 0x80004001:"E_NOTIMPL", 0x80004005:"E_FAIL", 0x80070057:"E_INVALIDARG", 0x8000000a:"E_PENDING",
 0x80004002:"E_NOINTERFACE", 0x8007000e:"E_OUTOFMEMORY", 0x88770005:"ALREADYINITIALIZED", 0x8877000a:"ACCESSDENIED",
 0x88770014:"ACTIVEPLAYERS", 0x8877001e:"BUFFERTOOSMALL", 0x88770028:"CANTADDPLAYER", 0x88770032:"CANTCREATEGROUP",
 0x8877003c:"CANTCREATEPLAYER", 0x88770046:"CANTCREATESESSION", 0x88770050:"CAPSNOTAVAILABLEYET", 0x8877005a:"EXCEPTION",
 0x88770078:"INVALIDFLAGS", 0x88770082:"INVALIDOBJECT", 0x88770096:"INVALIDPLAYER", 0x8877009b:"INVALIDGROUP",
 0x887700a0:"NOCAPS", 0x887700aa:"NOCONNECTION", 0x887700be:"NOMESSAGES", 0x887700c8:"NONAMESERVERFOUND",
 0x887700d2:"NOPLAYERS", 0x887700dc:"NOSESSIONS", 0x887700e6:"SENDTOOBIG", 0x887700f0:"TIMEOUT", 0x887700fa:"UNAVAILABLE",
 0x8877010e:"BUSY", 0x88770118:"USERCANCEL", 0x88770122:"CANNOTCREATESERVER", 0x8877012c:"PLAYERLOST",
 0x88770136:"SESSIONLOST", 0x88770140:"UNINITIALIZED", 0x8877014a:"NONEWPLAYERS", 0x88770154:"INVALIDPASSWORD",
 0x8877015e:"CONNECTING", 0x88770168:"CONNECTIONLOST", 0x88770172:"UNKNOWNMESSAGE", 0x8877017c:"CANCELFAILED",
 0x88770186:"INVALIDPRIORITY", 0x88770190:"NOTHANDLED", 0x8877019a:"CANCELLED", 0x887701a4:"ABORTED",
 0x887703e8:"BUFFERTOOLARGE", 0x88770410:"NOSERVICEPROVIDER", 0x8877042e:"NOTLOBBIED",
}
def nm(h):
    try: v = int(h, 16)
    except ValueError: return h   # a data line: shown as is
    return names.get(v, h)
same = diff = 0
rows = []
for state in ["fresh", "init", "failedjoin", "closed", "open", "dp1host", "dp1create"]:
    for key in w.get(state, {}):
        a, b = w[state][key], o.get(state, {}).get(key)
        if b is None: rows.append((state, key[1], nm(a), "(missing)")); diff += 1
        elif a != b: rows.append((state, key[1], nm(a), nm(b))); diff += 1
        else: same += 1
    for key in o.get(state, {}):
        if key not in w.get(state, {}): rows.append((state, key[1], "(missing)", nm(o[state][key]))); diff += 1
# the differences we keep on purpose (docs/DPLAY_CONFORMANCE.md): listed apart, marked "expected"
deliberate = [
 # DirectX 3's 28-byte DPCAPS (dplay.c dp1_caps): Windows 98 SE and 10 refuse it, which stops
 # Necrodome at its first GetCaps; ours answers it (so an unknown player gets INVALIDPLAYER, not
 # the size's E_INVALIDARG, and the caps come back as data)
 (re.compile(r"GetCaps\(28\)|GetPlayerCaps\(P1,28\)|GetPlayerCaps\(unknown\)"), "DirectX 3's 28-byte DPCAPS accepted"),
]
def why(label):
    for rx, reason in deliberate:
        if rx.search(label): return reason
    return None
expected = [r + (why(r[1]),) for r in rows if why(r[1])]
rows = [r for r in rows if not why(r[1])]
print()
print(f"apidiff: {same} calls answer the same, {len(rows) + len(expected)} differ "
      f"({len(expected)} deliberate, {len(rows)} unexpected)")
def show(r):
    if len(r[2]) > 22 or len(r[3]) > 22: print(f"  {r[0]:<10} {r[1]}\n      Windows: {r[2]}\n      ours:    {r[3]}")
    else: print(f"  {r[0]:<10} {r[1]:<28} {r[2]:<22} {r[3]}")
if rows:
    print(f"  {'state':<10} {'call':<28} {'Windows':<22} ours")
    for r in rows: show(r)
if expected:
    print("  expected (deliberate differences, docs/DPLAY_CONFORMANCE.md):")
    for reason in sorted(set(r[4] for r in expected)):
        print(f"    {reason}: {sum(1 for r in expected if r[4] == reason)} rows")
        for r in expected:
            if r[4] == reason: show(r)
open(sys.argv[2].replace("ours.txt", "diff.txt"), "w").write(
    "".join("%s\t%s\t%s\t%s\n" % r for r in rows) +
    "".join("%s\t%s\t%s\t%s\texpected: %s\n" % r for r in expected))
PYEOF
grep -q "CRASH\|HANG\|create failed" "$T/windows.txt" "$T/ours.txt" && exit 2 || exit 0
