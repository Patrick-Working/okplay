"""okplay.php + okstats.php end to end, on a local PHP (php -S): rooms, stats, reports, the stats page.

  python okplay/tests/server_php_test.py <path to php.exe> [port]

Copies the two PHP files into a fresh temporary web folder, so nothing in server/ is touched.
"""
import http.cookiejar, os, re, shutil, subprocess, sys, tempfile, time, urllib.error, urllib.parse, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(HERE, '..', 'server')
KEY = 'okp_7Qm2xV9rLs4Hd8Wn'
PASSWORD = 'test-password-123'
APP = '5de93f3ffc904ee1ae5a63dafa055950'
TEST_APP = '0e7c4b6193a24f0b8d356a1f2c9b7e58'

fails = 0


def check(cond, what):
    global fails
    print(('PASS ' if cond else 'FAIL ') + what)
    if not cond:
        fails += 1


def post(base, fields, opener=None, path='okplay.php'):
    data = urllib.parse.urlencode(fields).encode()
    try:
        r = (opener or urllib.request.build_opener()).open(base + path, data, timeout=10)
        return r.status, r.read().decode('utf-8', 'replace')
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode('utf-8', 'replace')


def get(base, path, opener=None):
    try:
        r = (opener or urllib.request.build_opener()).open(base + path, timeout=10)
        return r.status, r.read().decode('utf-8', 'replace')
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode('utf-8', 'replace')


def main():
    php = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8791
    web = tempfile.mkdtemp(prefix='okp_php_')
    for f in ('okplay.php', 'okstats.php'):
        shutil.copy(os.path.join(SERVER, f), web)
    proc = subprocess.Popen([php, '-S', '127.0.0.1:%d' % port, '-t', web], stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    base = 'http://127.0.0.1:%d/' % port
    try:
        time.sleep(1.0)
        ann = dict(k=KEY, a='announce', room='0000abcd', peer='1' * 16, tok='a' * 16, cands='1.2.3.4:5000',
                   enum='', name='Test room', players=1, maxp=8, flags=0, ver='0.8.3', wire=4, app=APP,
                   game='Age of Empires II: The Conquerors', state='lobby')
        s, b = post(base, ann)
        check(s == 200 and b.startswith('ok '), 'announce: ok')
        post(base, ann)   # the same room again: not a second game
        post(base, dict(ann, room='0000abce', tok='b' * 16, app=TEST_APP, game='Connection Check'))
        s, b = post(base, dict(k=KEY, a='list', app=APP))
        check(s == 200 and 'room 0000abcd' in b, 'list: shows the room')
        s, b = post(base, dict(k=KEY, a='knock', room='0000abcd', peer='2' * 16, cands='5.6.7.8:6000'))
        check(s == 200, 'knock: ok')
        post(base, dict(k=KEY, a='knock', room='0000abcd', peer='2' * 16, cands='5.6.7.8:6000'))   # repeat knock
        s, b = get(base, 'okplay_stats.php')
        check(s == 404 and KEY not in b and 'salt' not in b, 'stats store: 404 over the web')

        # reports
        s, b = post(base, dict(k=KEY, a='report', kind='crash', ver='0.8.3',
                               body='okplay-crash 1\nsig=ACCESS_VIOLATION at empires2.exe+0x1234\nexe=empires2.exe\nwin=10.0.19045\n'
                                    'log=<script>alert(1)</script>'))
        check(s == 200 and re.match(r'ok \d{4}-[0-9a-f]{6}\n$', b), 'report crash: ok with a reference')
        crash_ref = b.split()[1] if s == 200 else ''
        s, b = post(base, dict(k=KEY, a='report', kind='conn', ver='0.8.3',
                               body='role=join mode=online result=ok peers=1\nrole=join mode=online result=no_path peers=0'))
        check(s == 200, 'report conn: ok')
        s, b = post(base, dict(k=KEY, a='report', kind='check', ver='0.8.3', body='Connection Check report\nall good'))
        check(s == 200, 'report check: ok')
        s, b = post(base, dict(k=KEY, a='report', kind='other', body='x'))
        check(s == 400, 'report: unknown kind refused')
        s, b = post(base, dict(k='wrong', a='report', kind='crash', body='x'))
        check(s == 403, 'report: bad key refused')
        s, b = post(base, dict(k=KEY, a='report', kind='conn', body='   '))
        check(s == 400, 'report: empty refused')
        files = os.listdir(os.path.join(web, 'okplay_reports'))
        check(any(f.endswith('.php') for f in files) and 'index.html' in files, 'reports: dated PHP file + index.html')
        day = [f for f in files if f.endswith('.php')][0]
        s, b = get(base, 'okplay_reports/' + day)
        check(s == 404 and 'ACCESS_VIOLATION' not in b, 'reports file: 404 over the web')
        raw = open(os.path.join(web, 'okplay_reports', day), encoding='utf-8').read()
        check('127.0.0.1' not in raw, 'reports file: no address stored')
        raw = open(os.path.join(web, 'okplay_stats.php'), encoding='utf-8').read()
        check('127.0.0.1' not in raw, 'stats file: no address stored')
        for i in range(45):
            s, b = post(base, dict(k=KEY, a='report', kind='conn', body='result=ok'))
        check(s == 429, 'report: per-address daily cap')

        # browser signaling (docs/WEB_NETPLAY.md section 2)
        import json
        webann = dict(ann, room='0000ef01', peer='a' * 16, tok='c' * 16, cands='-', wire=100, ver='1.0.0',
                   app='11112222333344445555666677778888', game='Web test')
        s, b = post(base, webann)
        check(s == 200 and b.startswith('ok '), 'web host: announce wire=100')
        s, b = post(base, dict(k=KEY, a='list', app=webann['app']))
        check(' 100 ' in b and 'room 0000ef01' in b, 'web room listed with wire 100 (Windows clients skip it)')
        s, b = post(base, dict(k=KEY, a='sjoin', room='0000abcd', peer='b' * 16))
        check(s == 404, 'sjoin: refused for a Windows room')
        s, b = post(base, dict(k=KEY, a='sjoin', room='0000ef01', peer='b' * 16))
        jt = json.loads(b).get('tok', '') if s == 200 else ''
        check(s == 200 and len(jt) == 16, 'sjoin: joiner token')
        s, b = post(base, dict(k=KEY, a='signal', room='0000ef01', peer='b' * 16, tok=jt, to='a' * 16, data='{"sdp":"offer"}'))
        check(s == 200 and json.loads(b)['ok'], 'signal: joiner -> host')
        s, b = post(base, dict(k=KEY, a='signal', room='0000ef01', peer='b' * 16, tok='0' * 16, to='a' * 16, data='x'))
        check(s == 403, 'signal: wrong joiner token refused')
        s, b = post(base, dict(k=KEY, a='signal', room='0000ef01', peer='a' * 16, tok='0' * 16, to='b' * 16, data='x'))
        check(s == 403, 'signal: posing as the host refused')
        s, b = post(base, dict(k=KEY, a='signal', room='0000ef01', peer='b' * 16, tok=jt, to='d' * 16, data='x'))
        check(s == 403, 'signal: joiner -> someone not the host refused')
        s, b = post(base, dict(k=KEY, a='signal', room='0000ef01', peer='b' * 16, tok=jt, to='a' * 16, data='x' * 17000))
        check(s == 413, 'signal: oversized refused')
        s, b = post(base, dict(k=KEY, a='poll', room='0000ef01', peer='a' * 16, tok='c' * 16))
        msgs = json.loads(b).get('msgs', []) if s == 200 else []
        check(len(msgs) == 1 and msgs[0]['from'] == 'b' * 16 and msgs[0]['data'] == '{"sdp":"offer"}', 'poll: host gets the offer')
        s, b = post(base, dict(k=KEY, a='poll', room='0000ef01', peer='a' * 16, tok='c' * 16))
        check(s == 200 and json.loads(b)['msgs'] == [], 'poll: delivered once')
        post(base, dict(k=KEY, a='signal', room='0000ef01', peer='a' * 16, tok='c' * 16, to='b' * 16, data='{"sdp":"answer"}'))
        s, b = post(base, dict(k=KEY, a='poll', room='0000ef01', peer='b' * 16, tok=jt))
        check(s == 200 and json.loads(b)['msgs'][0]['data'] == '{"sdp":"answer"}', 'poll: joiner gets the answer')
        r = urllib.request.urlopen(urllib.request.Request(base + 'okplay.php', urllib.parse.urlencode(
            dict(k=KEY, a='poll', room='0000ef01', peer='b' * 16, tok=jt)).encode()), timeout=10)
        check(r.headers.get('Access-Control-Allow-Origin') == '*' and 'json' in r.headers.get('Content-Type', ''),
              'signaling replies: JSON with CORS')

        # the relay (browser players who can't connect directly; docs/WEB_NETPLAY.md)
        ra, rb = 'a1' * 8, 'b2' * 8
        ta, tb = '1' * 16, '2' * 16
        s, b = post(base, dict(k=KEY, a='rsend', room='0000ef01', peer=ra, ptok=ta,
                               frames=json.dumps([[rb, 'AQBoZWxsbw=='], [rb, 'AQB3b3JsZA=='], [ra, 'c2VsZg=='], [rb, 'not base64!']])))
        check(s == 200 and json.loads(b)['ok'], 'rsend: accepted')
        s, b = post(base, dict(k=KEY, a='rpoll', room='0000ef01', peer=rb, ptok=tb, wait='0'))
        frames = json.loads(b).get('frames', []) if s == 200 else []
        check(frames == [[ra, 'AQBoZWxsbw=='], [ra, 'AQB3b3JsZA==']], 'rpoll: both messages, in order (to self and bad base64 dropped)')
        s, b = post(base, dict(k=KEY, a='rpoll', room='0000ef01', peer=rb, ptok=tb, wait='0'))
        check(s == 200 and json.loads(b)['frames'] == [], 'rpoll: delivered once')
        s, b = post(base, dict(k=KEY, a='rpoll', room='0000ef01', peer=rb, ptok='9' * 16, wait='0'))
        check(s == 403, 'rpoll: someone else\'s mailbox refused')
        s, b = post(base, dict(k=KEY, a='rsend', room='0000ef01', peer=rb, ptok='9' * 16, frames='[]'))
        check(s == 403, 'rsend: posing as another player refused')
        s, b = post(base, dict(k=KEY, a='rpoll', room='0000ef01', peer=rb))
        check(s == 400, 'rpoll: no mailbox key refused')
        t0 = time.time()
        s, b = post(base, dict(k=KEY, a='rpoll', room='0000ef01', peer=rb, ptok=tb, wait='700'))
        check(s == 200 and json.loads(b)['frames'] == [] and time.time() - t0 >= 0.6, 'rpoll: waits for messages, then answers empty')
        # the compact format (okweb 1.1.1+): f = "to.data*..." in base64url, c=1 answers the same way
        s, b = post(base, dict(k=KEY, a='rsend', room='0000ef01', peer=ra, ptok=ta, f=rb + '.AQBoZWxsbw*' + rb + '.AQB3b3JsZA'))
        check(s == 200 and json.loads(b)['ok'], 'rsend (compact): accepted')
        s, b = post(base, dict(k=KEY, a='rpoll', room='0000ef01', peer=rb, ptok=tb, wait='0', c='1'))
        check(s == 200 and json.loads(b).get('f') == ra + '.AQBoZWxsbw*' + ra + '.AQB3b3JsZA', 'rpoll (compact): both, in order')
        post(base, dict(k=KEY, a='rsend', room='0000ef01', peer=ra, ptok=ta, f=rb + '.-_8'))
        s, b = post(base, dict(k=KEY, a='rpoll', room='0000ef01', peer=rb, ptok=tb, wait='0'))
        check(s == 200 and json.loads(b)['frames'] == [[ra, '+/8=']], 'compact in, old format out: converted')
        # a waiting rpoll answers soon after a message arrives (php -S serves one request at a time, so
        # this only checks that a message already waiting comes back at once, not the 20 ms wake-up)
        post(base, dict(k=KEY, a='rsend', room='0000ef01', peer=ra, ptok=ta, f=rb + '.AQ'))
        t0 = time.time()
        s, b = post(base, dict(k=KEY, a='rpoll', room='0000ef01', peer=rb, ptok=tb, wait='1500', c='1'))
        check(s == 200 and json.loads(b)['f'] == ra + '.AQ' and time.time() - t0 < 0.5, 'rpoll: a waiting message comes back at once')
        s, b = get(base, 'okplay_relay/0000ef01-' + rb + '.php')
        check(s == 404 and 'AQ' not in b and os.path.isfile(os.path.join(web, 'okplay_relay', '0000ef01-' + rb + '.php')),
              'mailbox file: one per player, not readable over the web')

        # stats page
        s, b = get(base, 'okstats.php')
        check('Not set up' in b, 'okstats: setup message without okplay_admin.php')
        open(os.path.join(web, 'okplay_admin.php'), 'w').write("<?php define('OKSTATS_PASSWORD', '%s');\n" % PASSWORD)
        s, b = get(base, 'okplay_admin.php')
        check(PASSWORD not in b, 'okplay_admin.php: password not shown over the web')
        jar = http.cookiejar.CookieJar()
        op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))
        s, b = get(base, 'okstats.php', op)
        check('Sign in' in b and 'games hosted' not in b, 'okstats: sign-in form, no data before sign-in')
        s, b = get(base, 'okstats.php?v=crash', op)
        check('ACCESS_VIOLATION' not in b, 'okstats: crash view needs sign-in')
        s, b = post(base, dict(password='nope'), op, 'okstats.php')
        check('Wrong password' in b, 'okstats: wrong password refused')
        s, b = post(base, dict(password=PASSWORD), op, 'okstats.php')
        check('games hosted' in b, 'okstats: signed in, overview shown')
        hosted = re.search(r'<b>(\d+)</b><span>games hosted</span>', b)
        check(hosted and hosted.group(1) == '2', 'okstats: 2 games hosted (Windows + browser; repeat announce and test game not counted)')
        joins = re.search(r'<b>(\d+)</b><span>join attempts</span>', b)
        viewers = re.search(r'<b>(\d+)</b><span>opened the game list</span>', b)
        check(viewers and viewers.group(1) == '1', 'okstats: 1 list viewer')
        check(joins and joins.group(1) == '2', 'okstats: 2 join attempts (a knock, a browser sjoin; the repeat knock not counted)')
        check('Age of Empires II: The Conquerors' in b and '0.8.3' in b, 'okstats: games and versions breakdown')
        s, b = get(base, 'okstats.php?v=crash', op)
        check('ACCESS_VIOLATION at empires2.exe+0x1234' in b, 'okstats: crash grouped by signature')
        m = re.search(r'id=([0-9-]+:\d+)">latest', b)
        s, b = get(base, 'okstats.php?v=report&id=' + (m.group(1) if m else ''), op)
        check('&lt;script&gt;alert(1)&lt;/script&gt;' in b and '<script>alert' not in b, 'okstats: report body escaped')
        s, b = get(base, 'okstats.php?v=conn', op)
        check('no_path' in b and 'result=ok' in b, 'okstats: connection reports and results')
        s, b = get(base, 'okstats.php?v=check', op)
        check('all good' in b, 'okstats: Connection Check reports')
        s, b = get(base, 'okstats.php?v=find&ref=' + crash_ref, op)
        check('ACCESS_VIOLATION' in b and crash_ref in b, 'okstats: find a report by its reference')
        s, b = get(base, 'okstats.php?v=find&ref=0000-000000', op)
        check('No report with the id' in b, 'okstats: unknown reference')
        s, b = get(base, 'okstats.php?v=report&id=../../x:1', op)
        check('No such report' in b, 'okstats: bad report id refused')
    finally:
        proc.terminate()
        proc.wait()
        shutil.rmtree(web, ignore_errors=True)
    print('failures: %d' % fails)
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
