"""S1 score: Wine's dplayx conformance tests, Windows' dplayx vs ours (docs/DPLAY_CONFORMANCE.md section 4).

Both logs come from the same test binary run with WINETEST_REPORT_SUCCESS=1 (see run_s1.sh). A check is
identified by its source line plus how many times that line has reported before (loops repeat lines).
Only checks that PASS on Windows count: checks failing on Windows depend on this PC or are wrong tests.

  python s1_compare.py results/s1_windows.txt results/s1_plain.txt [--list]
"""
import collections, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
TEST_C = os.path.join(HERE, '..', 'winedplay', 'port', 'dlls', 'dplayx', 'tests', 'dplayx.c')
LINE_RE = re.compile(r'dplayx\.c:(\d+): (Test succeeded|Test failed|Test marked todo|Test succeeded inside todo)')


def functions():
    """source line -> name of the enclosing test function"""
    names, cur = {}, '?'
    with open(TEST_C, encoding='latin-1') as f:
        for n, line in enumerate(f, 1):
            m = re.match(r'(?:static\s+)?void\s+(\w+)\s*\(', line)
            if m:
                cur = m.group(1)
            names[n] = cur
    return names


def parse(path):
    seen, res = collections.Counter(), {}
    with open(path, encoding='latin-1', errors='replace') as f:
        for line in f:
            m = LINE_RE.search(line)
            if not m:
                continue
            ln = int(m.group(1))
            key = (ln, seen[ln])
            seen[ln] += 1
            res[key] = m.group(2) == 'Test succeeded'
    return res


def main():
    win, ours = parse(sys.argv[1]), parse(sys.argv[2])
    fn = functions()
    counted = [k for k, ok in win.items() if ok]
    per = collections.defaultdict(lambda: [0, 0, 0])   # function -> [counted, pass, missing in ours]
    fails = []
    for k in counted:
        f = fn.get(k[0], '?')
        per[f][0] += 1
        if ours.get(k) is True:
            per[f][1] += 1
        else:
            if k not in ours:
                per[f][2] += 1
            fails.append(k)
    total, passed = len(counted), sum(v[1] for v in per.values())
    print('S1: %d of %d Windows-passing checks also pass on ours (%.1f%%); Windows log: %d checks, %d fail on Windows itself'
          % (passed, total, 100.0 * passed / max(total, 1), len(win), len(win) - total))
    print('\n| test function | checks | pass | fail | not reached |')
    print('|---|---|---|---|---|')
    for f, (c, p, m) in sorted(per.items(), key=lambda kv: (kv[1][1] - kv[1][0], kv[0])):
        print('| %s | %d | %d | %d | %d |' % (f, c, p, c - p - m, m))
    if '--list' in sys.argv:
        print('\nfailing checks (line#occurrence):')
        for ln, i in sorted(fails):
            print('  %d#%d %s' % (ln, i, fn.get(ln, '?')))


if __name__ == '__main__':
    main()
