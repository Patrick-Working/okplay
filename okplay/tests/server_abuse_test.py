"""Abuse tests for the room server (okplay.php): bad bytes, oversize fields, knock floods, store size - and
that honest flows still pass. Runs okplay.php on a local PHP (php -S) in a fresh temporary web folder.

  python okplay/tests/server_abuse_test.py <path to php.exe> [port] [okplay.php to test]

(default port 8796; default file: server/okplay.php - pass another copy, e.g. an older one from git, to
see which checks it fails). Light: a few hundred small requests, one PHP process. The knock-rate check
runs last, because it blocks knocks from 127.0.0.1 for a minute on that (temporary) server.
"""
import json, os, re, shutil, subprocess, sys, tempfile, time, urllib.error, urllib.parse, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(HERE, '..', 'server')
KEY = 'okp_7Qm2xV9rLs4Hd8Wn'
APP = '5de93f3ffc904ee1ae5a63dafa055950'
fails = 0


def check(cond, what, detail=''):
    global fails
    print(('PASS ' if cond else 'FAIL ') + what + ('' if cond else '   -> %r' % (detail,)[:300]))
    if not cond:
        fails += 1


def post(base, fields):
    # fields may hold bytes (bad UTF-8 on purpose): urlencode percent-encodes them as they are
    data = urllib.parse.urlencode(dict(k=KEY, **fields)).encode()
    try:
        r = urllib.request.urlopen(base + 'okplay.php', data, timeout=20)
        return r.status, r.read().decode('utf-8', 'replace')
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode('utf-8', 'replace')


def store(web):
    """the room store as Python data (None if it isn't valid: the guard line, then JSON)"""
    p = os.path.join(web, 'okplay_rooms.php')
    raw = open(p, 'rb').read() if os.path.isfile(p) else b''
    nl = raw.find(b'\n')
    if not raw.startswith(b'<?php') or nl < 0:
        return None
    try:
        v = json.loads(raw[nl + 1:].decode('utf-8') or '[]')
    except ValueError:
        return None
    return v


def size(web):
    return os.path.getsize(os.path.join(web, 'okplay_rooms.php'))


def main():
    php = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8796
    src = sys.argv[3] if len(sys.argv) > 3 else os.path.join(SERVER, 'okplay.php')
    web = tempfile.mkdtemp(prefix='okp_abuse_')
    shutil.copy(src, os.path.join(web, 'okplay.php'))
    proc = subprocess.Popen([php, '-S', '127.0.0.1:%d' % port, '-t', web], stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    base = 'http://127.0.0.1:%d/' % port
    try:
        time.sleep(1.0)
        ann = dict(a='announce', room='0000abcd', peer='1' * 16, tok='a' * 16, cands='1.2.3.4:5000', enum='',
                   name='Honest room', players=1, maxp=8, flags=0, ver='0.8.5', wire=4, app=APP, game='AoE2',
                   state='lobby')

        # ---- honest flow first
        s, b = post(base, ann)
        check(s == 200 and b.startswith('ok '), 'honest: announce', (s, b))
        cands8 = ','.join('10.0.0.%d:%d' % (i, 2300 + i) for i in range(7)) + ',[2001:db8::1]:2300'
        s, b = post(base, dict(a='knock', room='0000abcd', peer='2' * 16, cands=cands8))
        check(s == 200, 'honest: knock with 8 addresses', (s, b))
        s, b = post(base, ann)
        check(('knock %s %s' % ('2' * 16, cands8)) in b, 'honest: host gets the knock, all 8 addresses', b)
        uname = 'Pätrick’s 日本語 game'
        s, b = post(base, dict(ann, name=uname))
        s, b = post(base, dict(a='list', app=APP))
        line = next((l for l in b.splitlines() if l.startswith('room 0000abcd ')), '')
        check(line and urllib.parse.unquote(line.split(' ')[6]) == uname, 'honest: UTF-8 name round-trips', line)

        # ---- 2.3 a: one bad byte must not empty the store
        s, b = post(base, dict(ann, room='0000bad1', tok='b' * 16, name=b'bad \xff\xfe name'))
        check(s == 200, 'bad bytes: announce with name=%ff answered', (s, b))
        st = store(web)
        check(isinstance(st, dict) and '0000abcd' in st, 'bad bytes: the store still holds the other room', st)
        s, b = post(base, dict(a='list', app=APP))
        check('room 0000abcd ' in b, 'bad bytes: list still shows the other room', b)
        line = next((l for l in b.splitlines() if l.startswith('room 0000bad1 ')), '')
        nm = urllib.parse.unquote(line.split(' ')[6]) if line else ''
        check(nm.startswith('bad ') and nm.endswith(' name') and '�' in nm, 'bad bytes: name kept, bad bytes as U+FFFD', line)
        for fld in ('game', 'exe'):
            s, b = post(base, dict(ann, room='0000bad1', tok='b' * 16, **{fld: b'x\xc3'}))
            check(s == 200 and isinstance(store(web), dict) and '0000abcd' in store(web), 'bad bytes in %s: store intact' % fld)
        long_name = 'é' * 100   # 200 bytes: cut on a character boundary (64 characters, at most 127 bytes)
        post(base, dict(ann, room='0000bad1', tok='b' * 16, name=long_name))
        s, b = post(base, dict(a='list', app=APP))
        line = next((l for l in b.splitlines() if l.startswith('room 0000bad1 ')), '')
        raw = urllib.parse.unquote_to_bytes(line.split(' ')[6]) if line else b''
        try:
            ok = raw.decode('utf-8') == 'é' * 63
        except UnicodeDecodeError:
            ok = False
        check(ok, 'long non-ASCII name: cut between characters, fits the client (63 x e-acute)', raw)
        # reports with bad bytes: stored as the body (U+FFFD), never a blank line answered "ok"
        s, b = post(base, dict(a='report', kind='check', ver='0.8.5', body=b'report with \xff byte'))
        check(s == 200 and b.startswith('ok '), 'bad bytes: report accepted', (s, b))
        rd = os.path.join(web, 'okplay_reports')
        lines = []
        for f in os.listdir(rd) if os.path.isdir(rd) else []:
            if f.endswith('.php'):
                lines += open(os.path.join(rd, f), 'rb').read().split(b'\n')[1:]
        bodies = []
        for l in lines:
            try:
                bodies.append(json.loads(l.decode('utf-8'))['body'])
            except (ValueError, KeyError):
                if l.strip() == b'':
                    continue
                bodies.append(None)
        check(any(x and x.startswith('report with ') for x in bodies), 'bad bytes: report body stored', lines)
        st = os.path.join(web, 'okplay_stats.php')
        check(os.path.isfile(st) and open(st, 'rb').read().count(b'\n') >= 1 and len(open(st, 'rb').read()) > 60,
              'stats store not emptied')

        # ---- oversize fields: cut, store stays small
        before = size(web)
        s, b = post(base, dict(ann, name='N' * 10000, game='G' * 10000, exe='E' * 10000, cands='1.2.3.4:5,' * 600,
                               enum='A' * 9000))
        check(s == 200, 'oversize: announce answered', (s, b))
        check(size(web) < before + 4096, 'oversize: store grew by under 4 KB', (before, size(web)))

        # ---- knocks: never more than 8 addresses; one network can't fill a room's waiting list
        many = ','.join('10.1.%d.%d:2300' % (i // 200, i % 200) for i in range(40))
        post(base, dict(a='knock', room='0000abcd', peer='3' * 16, cands=many))
        s, b = post(base, ann)
        kl = next((l for l in b.splitlines() if l.startswith('knock ' + '3' * 16)), '')
        check(kl and len(kl.split(' ')[2].split(',')) == 8, 'knock: at most 8 addresses passed on', kl)
        codes = [post(base, dict(a='knock', room='0000abcd', peer='%016x' % (0x100 + i), cands='-'))[0] for i in range(9)]
        check(codes == [200] * 8 + [429], 'knock flood: 8 joiners from one address wait, the 9th gets 429', codes)
        s, b = post(base, ann)
        check(sum(1 for l in b.splitlines() if l.startswith('knock ')) == 8, 'knock flood: the host still gets the 8', b)
        check(post(base, dict(a='knock', room='0000abcd', peer='%016x' % 0x200, cands='-'))[0] == 200,
              'knock: room for more once the host collected them')
        post(base, ann)

        # ---- browser signalling: per-room mailbox cap, bad bytes, token per sjoin
        web_ann = dict(ann, room='0000ef01', peer='a' * 16, tok='c' * 16, cands='-', wire=100, ver='1.1.1',
                       app='11112222333344445555666677778888', game='Web')
        s, b = post(base, web_ann)
        check(s == 200, 'web host: announce', (s, b))
        s, b = post(base, dict(a='sjoin', room='0000ef01', peer='b' * 16))
        jt = json.loads(b).get('tok', '') if s == 200 else ''
        check(len(jt) == 16, 'honest: sjoin', (s, b))
        s, b = post(base, dict(a='signal', room='0000ef01', peer='b' * 16, tok=jt, to='a' * 16, data='{"t":"offer"}'))
        check(s == 200, 'honest: signal', (s, b))
        s, b = post(base, dict(a='poll', room='0000ef01', peer='a' * 16, tok='c' * 16))
        check(s == 200 and len(json.loads(b)['msgs']) == 1, 'honest: host polls the offer', (s, b))
        s, b = post(base, dict(a='signal', room='0000ef01', peer='b' * 16, tok=jt, to='a' * 16, data=b'{"x":"\xff"}'))
        check(s == 400 and isinstance(store(web), dict) and '0000abcd' in store(web),
              'bad bytes: signal data refused, store intact', (s, b))
        s, b = post(base, dict(a='sjoin', room='0000ef01', peer='b' * 16))
        jt2 = json.loads(b).get('tok', '') if s == 200 else ''
        check(len(jt2) == 16 and jt2 != jt, 'sjoin again under a known id: a new token, not the old one', (jt, jt2))
        s, b = post(base, dict(a='poll', room='0000ef01', peer='b' * 16, tok=jt))
        check(s == 403, 'sjoin again: the old token is no longer good', (s, b))
        big = '{"d":"' + 'x' * 15990 + '"}'   # 16000 bytes, under the 16 KB message limit
        codes = []
        for i in range(20):
            codes.append(post(base, dict(a='signal', room='0000ef01', peer='b' * 16, tok=jt2, to='a' * 16, data=big))[0])
        check(codes[:16] == [200] * 16 and codes[16:] == [429] * 4,
              'signal flood: one room holds at most 256 KB (16 x 16 KB, then 429)', codes)
        check(size(web) < 300000, 'signal flood: store under 300 KB', size(web))
        s, b = post(base, dict(a='poll', room='0000ef01', peer='a' * 16, tok='c' * 16))
        check(s == 200 and len(json.loads(b)['msgs']) == 16, 'signal flood: the host gets the 16', s)
        s, b = post(base, dict(a='signal', room='0000ef01', peer='b' * 16, tok=jt2, to='a' * 16, data='{"t":"offer"}'))
        check(s == 200, 'signal: room again once the host polled', (s, b))

        # ---- store size after N mixed requests
        for i in range(60):
            post(base, ann)
            post(base, dict(a='knock', room='0000abcd', peer='%016x' % (0x300 + i % 8), cands=many))
            post(base, dict(a='list', app=APP))
        check(size(web) < 20000, 'store after 180 more requests: under 20 KB', size(web))
        st = store(web)
        check(isinstance(st, dict) and '0000abcd' in st and '0000ef01' in st, 'store after the run: valid, rooms kept')

        # ---- knock rate per address (last: 127.0.0.1 can't knock for a minute after this)
        codes = []
        for i in range(260):
            codes.append(post(base, dict(a='knock', room='0000abcd', peer='%016x' % (0x400 + i % 4), cands='-'))[0])
            if i % 10 == 9:
                post(base, ann)   # the host collects, as it does every second
        first429 = codes.index(429) if 429 in codes else -1
        check(first429 >= 150 and all(c == 429 for c in codes[first429:]),
              'knock rate: one address is stopped after ~240 knocks a minute (4 joiners retrying pass)',
              (first429, codes[-5:]))
        s, b = post(base, ann)
        check(s == 200, 'knock rate: the host still announces', (s, b))
    finally:
        proc.terminate()
        proc.wait()
        shutil.rmtree(web, ignore_errors=True)

    # ---- the address rules (IPv6 counted per /64), straight from the PHP functions
    code = (r'$src = file_get_contents($argv[1]);'
            r'if (!preg_match("/function net_key.*?\n}\n/s", $src, $m)) { echo "none"; exit; }'
            r'eval($m[0]);'
            r'echo json_encode([net_key("1.2.3.4"), net_key("2001:db8:1:2:aaaa::1"), net_key("2001:db8:1:2:bbbb::9"),'
            r'net_key("2001:db8:1:3::1"), net_key("::ffff:1.2.3.4")]);')
    out = subprocess.run([php, '-r', code, src], capture_output=True, text=True).stdout
    try:
        k = json.loads(out)
    except ValueError:
        k = None
    check(k is not None and k[0] == '1.2.3.4' and k[1] == k[2] and k[1] != k[3] and k[4] == '1.2.3.4',
          'net_key: IPv4 as is, IPv6 per /64, IPv4-mapped as IPv4', out)

    print('failures: %d' % fails)
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
