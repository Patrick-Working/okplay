"""S3 score: what Windows' dplayx and ours hand the TCP/IP provider (and get back) in the same dptest
scenario, message by message (docs/DPLAY_CONFORMANCE.md section 4). Input: two sptap logs of the same
role in the same scenario (conformance/run_s3.sh).

Each log becomes a list of events: messages dplayx sent (Send/SendEx/Reply/EnumSessions, the
provider's header stripped), messages it was handed (HandleMessage), and the provider calls that
shape the wire (Open, CreatePlayer, DeletePlayer, with their flags). In the bytes, player ids
(known from the calls) become tokens P0, P1... in order of first appearance, and socket addresses
(AF_INET, port, IP) are masked - they differ between any two runs.

Messages are paired per kind ("out cmd 0x13 v14", "in game"...) in order of appearance: the k-th
of a kind on Windows with the k-th on ours. A pair matches when the masked bytes are identical.
Timing-driven counts (pings per second) differ between runs, so the score counts pairs, and lists
count differences apart.

  python s3_compare.py win.log ours.log [--label NAME] [--show 6]
"""
import collections, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
try:
    from spec_coverage import SPEC
except Exception:   # (the names are only labels)
    SPEC = {}

LINE = re.compile(r'^\[\s*\d+\] \[[0-9a-f]+\] (.*)$')
HEX = re.compile(r'^  ([a-z0-9-]+) ([0-9a-f]{4}): ((?:[0-9a-f]{2} )*)$')
MORE = re.compile(r'^  ([a-z0-9-]+) \.\.\. (\d+) more bytes$')


def parse(path):
    """-> (events, header size, ids in order of first appearance)"""
    lines = []
    for raw in open(path, encoding='latin-1', errors='replace'):
        m = LINE.match(raw.rstrip('\n'))
        if m:
            lines.append(m.group(1))
    hdr, ids, events = 0, [], []

    def note_id(v):
        v = int(v, 16)
        if v and v not in ids:
            ids.append(v)

    i = 0
    while i < len(lines):
        s = lines[i]
        i += 1
        m = re.match(r'SPInit -> 0x[0-9a-f]+, header (\d+) bytes', s)
        if m:
            hdr = int(m.group(1))
            continue
        # payload lines that follow a call, by tag
        def payload():
            nonlocal i
            bufs, sizes = collections.OrderedDict(), {}
            while i < len(lines) and lines[i].startswith('  '):
                h = HEX.match(lines[i])
                if h:
                    bufs.setdefault(h.group(1), bytearray()).extend(bytes.fromhex(h.group(3).replace(' ', '')))
                mm = MORE.match(lines[i])
                if mm:
                    sizes[mm.group(1)] = int(mm.group(2))
                mb = re.match(r'^  (buf\d+): (\d+) bytes at', lines[i])
                if mb:
                    bufs.setdefault(mb.group(1), bytearray())
                i += 1
            return bufs, sizes

        m = re.match(r'Send\(flags (0x[0-9a-f]+), to (0x[0-9a-f]+), from (0x[0-9a-f]+), (\d+) bytes, system (\d)\)', s)
        if m:
            note_id(m.group(3)); note_id(m.group(2))
            bufs, _ = payload()
            body = bytes(bufs.get('out', b''))[hdr:]
            events.append(('out', 'Send flags %s system %s' % (m.group(1), m.group(5)), body, int(m.group(4)) - hdr))
            continue
        m = re.match(r'SendEx\(flags (0x[0-9a-f]+), to (0x[0-9a-f]+), from (0x[0-9a-f]+), (\d+) bytes in (\d+) buffers, system (\d)', s)
        if m:
            note_id(m.group(3)); note_id(m.group(2))
            bufs, _ = payload()
            parts = [bytes(b) for k, b in bufs.items() if k.startswith('buf')]
            if parts and len(parts[0]) in (0, hdr) and len(parts) > 1:   # the header space first
                parts = parts[1:]
            events.append(('out', 'SendEx flags %s system %s' % (m.group(1), m.group(6)), b''.join(parts),
                           sum(len(p) for p in parts)))
            continue
        m = re.match(r'Reply\((\d+) bytes, name server (0x[0-9a-f]+)', s)
        if m:
            note_id(m.group(2))
            bufs, _ = payload()
            events.append(('out', 'Reply', bytes(bufs.get('out', b''))[hdr:], int(m.group(1)) - hdr))
            continue
        m = re.match(r'EnumSessions\((\d+) bytes', s)
        if m:
            bufs, _ = payload()
            events.append(('out', 'EnumSessions', bytes(bufs.get('out', b''))[hdr:], int(m.group(1)) - hdr))
            continue
        m = re.match(r'HandleMessage <- (\d+) bytes', s)
        if m:
            bufs, _ = payload()
            events.append(('in', 'HandleMessage', bytes(bufs.get('in', b'')), int(m.group(1))))
            continue
        m = re.match(r'(Open)\(create (\d), header \S+, return status (\d), open flags (0x[0-9a-f]+), session flags (0x[0-9a-f]+)\)', s)
        if m:
            events.append(('call', 'Open create %s status %s open %s session %s' % m.groups()[1:], b'', 0))
            continue
        m = re.match(r'(CreatePlayer|DeletePlayer)\((0x[0-9a-f]+), flags (0x[0-9a-f]+)', s)
        if m:
            note_id(m.group(2))
            events.append(('call', '%s %%P%s flags %s' % (m.group(1), m.group(2), m.group(3)), b'', 0))
            continue
    return events, hdr, ids


def kind(ev):
    d, how, body, size = ev
    if d == 'call':
        return 'call ' + re.sub(r' %P0x[0-9a-f]+', '', how)
    if len(body) >= 8 and body[:4] == b'play':
        cmd = body[4] | body[5] << 8
        ver = body[6] | body[7] << 8
        name = SPEC.get(cmd, ('?',))[0]
        return '%s cmd 0x%02x %s v%d' % (d, cmd, name, ver)
    return '%s game' % d


def session_guid(events):
    """the session's instance GUID and its dwReserved1 (both random per session) from the first
    session-list reply: the description starts at byte 8, the GUID at 16, dwReserved1 at 64"""
    for d, how, body, size in events:
        if len(body) >= 68 and body[:4] == b'play' and (body[4] | body[5] << 8) == 0x01:
            return bytes(body[16:32]), bytes(body[64:68])
    return None, None


def masked(ev, ids, sess=(None, None)):
    """bytes -> tokens: a known player id -> 'Pn', the session's instance GUID -> 'GUID' and its key
    (dwReserved1) -> 'SKEY', an AF_INET sockaddr (port and address set) -> 'ADDR' with its 8 padding
    bytes (Windows leaves junk there)"""
    guid, skey = sess
    d, how, body, size = ev
    if d == 'call':
        return [re.sub(r'%P(0x[0-9a-f]+)', lambda m: 'P%d' % ids.index(int(m.group(1), 16))
                       if int(m.group(1), 16) in ids else 'P?', how)]
    idpos = {}
    for n, v in enumerate(ids):
        idpos[v.to_bytes(4, 'little')] = 'P%d' % n
    out, k = [], 0
    while k < len(body):
        if guid and body[k:k + 16] == guid:
            out.append('GUID')
            k += 16
            continue
        if k + 16 <= len(body) and body[k:k + 2] == b'\x02\x00' and body[k + 2:k + 4] != b'\0\0':
            # (the address may be 0.0.0.0: our TCP/IP provider writes that in its own data)
            out += ['af2', 'ADDR']
            k += 16
            continue
        w = body[k:k + 4]
        if skey and skey != b'\0' * 4 and w == skey:
            out += ['SKEY'] * 4   # (four tokens: a log without a key keeps its 4 bytes in line)
            k += 4
            continue
        if len(w) == 4 and w in idpos:
            out.append(idpos[w])
            k += 4
            continue
        out.append('%02x' % body[k])
        k += 1
    out.append('size=%d' % size)
    return out


def diff_spans(a, b):
    """where two token lists differ: [(token index, a token, b token)], first few"""
    res = []
    for n in range(max(len(a), len(b))):
        x = a[n] if n < len(a) else '-'
        y = b[n] if n < len(b) else '-'
        if x != y:
            res.append((n, x, y))
    return res


def main():
    args = sys.argv[1:]
    label = 'S3'
    show = 6
    if '--label' in args:
        label = args[args.index('--label') + 1]
    if '--show' in args:
        show = int(args[args.index('--show') + 1])
    winp, ourp = [a for a in args if not a.startswith('--') and os.path.exists(a)][:2]
    wev, whdr, wids = parse(winp)
    oev, ohdr, oids = parse(ourp)
    wguid, oguid = session_guid(wev), session_guid(oev)

    wk, ok_ = collections.OrderedDict(), collections.OrderedDict()
    for e in wev:
        wk.setdefault(kind(e), []).append(e)
    for e in oev:
        ok_.setdefault(kind(e), []).append(e)

    paired = matched = 0
    missing, extra, counts, diffs = [], [], [], []
    for k, wl in list(wk.items()):
        if k.endswith(' game'):
            # the application's own data (dptest's sequence numbers and clock) differs in any two
            # runs, and its order follows timing: only the envelope counts - from, to and size,
            # compared as a multiset
            ol = ok_.get(k, [])

            def env(e, ids):
                t = masked((e[0], e[1], e[2][:8], e[3]), ids)
                return ' '.join(t[:-1] + [t[-1]])
            wc = collections.Counter(env(e, wids) for e in wl)
            oc = collections.Counter(env(e, oids) for e in ol)
            for key in set(wc) | set(oc):
                a, b = wc.get(key, 0), oc.get(key, 0)
                paired += min(a, b)
                matched += min(a, b)
                if a != b:
                    counts.append(('%s [%s]' % (k, key), a, b))
            continue
        ol = ok_.get(k, [])
        if not ol:
            missing.append((k, len(wl)))
            continue
        if len(wl) != len(ol):
            counts.append((k, len(wl), len(ol)))
        for n in range(min(len(wl), len(ol))):
            paired += 1
            a, b = masked(wl[n], wids, wguid), masked(ol[n], oids, oguid)
            if a == b:
                matched += 1
            else:
                diffs.append((k, n, diff_spans(a, b), a, b))
    for k, ol in ok_.items():
        if k not in wk:
            extra.append((k, len(ol)))

    denom = paired + sum(c for _, c in missing)
    pct = 100.0 * matched / denom if denom else 100.0
    print('S3 %s: %d of %d messages/calls match Windows (%.1f%%); kinds missing on ours: %d, extra on ours: %d, '
          'count differences: %d' % (label, matched, denom, pct, len(missing), len(extra), len(counts)))
    print('  provider header: Windows %d bytes, ours %d; ids seen: Windows %d, ours %d' % (whdr, ohdr, len(wids), len(oids)))
    if missing:
        print('\nOnly on Windows (ours never does this):')
        for k, c in missing:
            print('  %-48s x%d' % (k, c))
    if extra:
        print('\nOnly on ours (Windows never does this):')
        for k, c in extra:
            print('  %-48s x%d' % (k, c))
    if counts:
        print('\nSame kind, different count (often timing):')
        for k, a, b in counts:
            print('  %-48s Windows %d, ours %d' % (k, a, b))
    if diffs:
        print('\nDiffering pairs (token index: Windows -> ours; the tokens are bytes after the masking):')
        bykind = collections.OrderedDict()
        for k, n, sp, a, b in diffs:
            bykind.setdefault(k, []).append((n, sp, a, b))
        for k, lst in bykind.items():
            print('  %s: %d of its pairs differ' % (k, len(lst)))
            for n, sp, a, b in lst[:show]:
                txt = ', '.join('%d: %s->%s' % s for s in sp[:12])
                print('    #%d: %s%s' % (n, txt, ' ...' if len(sp) > 12 else ''))
            n, sp, a, b = lst[0]
            print('    Windows #%d: %s' % (n, ' '.join(a[:80])))
            print('    ours    #%d: %s' % (n, ' '.join(b[:80])))


if __name__ == '__main__':
    main()
