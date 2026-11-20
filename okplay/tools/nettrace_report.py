#!/usr/bin/env python3
"""okplay nettrace report (version 1): one-way message times between two PCs from their wsock nettrace files.

    python nettrace_report.py HOST.csv JOINER.csv

Each file is okplay-net-*.csv from okplay.ini wsock_nettrace=1 (wsock/nettrace.c). The game connection on each side
is the socket that sent the most bytes. Each byte the host's game sent is matched to the joiner's game recv() that
delivered it (stream offsets), and the other way round. The two PCs' clocks are lined up from the traces themselves
(the fastest message each way is taken as equally fast: offset = (min a->b - min b->a) / 2), so the times are one-way
times relative to that; "+0 ms" means as fast as the fastest message. With okplay's "arrived" events (A) the time is
split into: the network with both okplays (S->A) and the receiving game picking it up (A->R)."""
import sys, bisect

def load(path):
    anchor, mode, ev = 0, "?", []
    with open(path, newline="") as f:
        for line in f:
            line = line.strip()
            if not line: continue
            if line.startswith("#"):
                if "anchor_utc100ns=" in line: anchor = int(line.split("anchor_utc100ns=")[1].split()[0])
                if line.startswith("# mode "): mode = line[7:].rsplit(" at ", 1)[0]
                continue
            t, k, s, b, o = line.split(",")
            ev.append((anchor // 10 + int(t), k, int(s), int(b), int(o)))   # absolute us
    return mode, ev

def stream(ev, kind):
    by = {}
    for e in ev:
        if e[1] == kind: by.setdefault(e[2], []).append(e)
    return by

def main_sock(ev):
    tot = {}
    for e in ev:
        if e[1] in "SR": tot[e[2]] = tot.get(e[2], 0) + e[3]
    return max(tot, key=tot.get) if tot else None

def match(sends, recvs):
    """for each send: (send time, time of the recv that delivered its last byte)"""
    ends = [r[4] + r[3] for r in recvs]
    out = []
    for s in sends:
        last = s[4] + s[3]
        i = bisect.bisect_left(ends, last)
        if i < len(recvs): out.append((s[0], recvs[i][0]))
    return out

def pct(v, p):
    if not v: return float("nan")
    v = sorted(v)
    return v[min(len(v) - 1, int(p / 100.0 * len(v)))]

def summary(name, d):
    if not d: print("  %-28s no messages matched" % name); return
    ms = [x / 1000.0 for x in d]
    print("  %-28s n=%5d  p50 %6.1f  p90 %6.1f  p99 %6.1f  max %7.1f ms   >50ms %4d  >100ms %4d" %
          (name, len(ms), pct(ms, 50), pct(ms, 90), pct(ms, 99), max(ms), sum(x > 50 for x in ms), sum(x > 100 for x in ms)))

def gaps(name, recvs):
    t = [r[0] for r in recvs]
    g = [(b - a) / 1000.0 for a, b in zip(t, t[1:])]
    if g: print("  %-28s p50 %6.1f  p99 %6.1f  max %7.1f ms   >100ms %4d" % (name, pct(g, 50), pct(g, 99), max(g), sum(x > 100 for x in g)))

def main():
    if len(sys.argv) != 3: print(__doc__); return 2
    ma, A = load(sys.argv[1]); mb, B = load(sys.argv[2])
    sa, sb = main_sock(A), main_sock(B)
    a_s = [e for e in A if e[2] == sa and e[1] == "S"]; a_r = [e for e in A if e[2] == sa and e[1] == "R"]
    b_s = [e for e in B if e[2] == sb and e[1] == "S"]; b_r = [e for e in B if e[2] == sb and e[1] == "R"]
    ab = match(a_s, b_r); ba = match(b_s, a_r)
    if not ab or not ba: print("not enough matched messages both ways"); return 1
    raw_ab = [r - s for s, r in ab]; raw_ba = [r - s for s, r in ba]
    theta = (min(raw_ab) - min(raw_ba)) / 2.0
    base = (min(raw_ab) + min(raw_ba)) / 2.0
    print("modes: %s / %s   clock offset %.1f ms   fastest one-way %.1f ms" % (ma, mb, theta / 1000, base / 1000))
    print("one-way time above the fastest (send() -> the other game's recv()):")
    summary("host -> joiner", [x - theta - base for x in raw_ab])
    summary("joiner -> host", [x + theta - base for x in raw_ba])
    print("gaps between the game's reads (data):")
    gaps("joiner reads", b_r); gaps("host reads", a_r)
    # okplay's own split, when the traces have okplay's "arrived" events (okplay modes only)
    for name, snd, rcv_a, rcv_r, sign in (("host -> joiner", a_s, [e for e in B if e[1] == "A"], b_r, -1),
                                          ("joiner -> host", b_s, [e for e in A if e[1] == "A"], a_r, 1)):
        if not rcv_a: continue
        print(name + ", split by okplay's arrival time (absolute, clocks lined up):")
        summary("  network + okplay (S->A)", [a + sign * theta - s for s, a in match(snd, rcv_a)])
        summary("  game picks it up (A->R)", [r - a for a, r in match(rcv_a, rcv_r)])
    return 0

if __name__ == "__main__":
    sys.exit(main())
