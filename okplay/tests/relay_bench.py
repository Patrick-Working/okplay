"""The okplay.php relay's round trip: A sends to B, B's waiting rpoll returns, B sends back, A's returns.

  python okplay/tests/relay_bench.py <okplay.php URL> [count] [old]

Uses a room of its own (0000beNN), so it can run against the live server while people play.
`old`: the JSON frames format (okplay.php before okweb 1.1.1) instead of the compact one.
Each request opens a new connection (browsers keep theirs open, so they see a little less).
For a local PHP with requests in parallel (as on real hosting) see php_pool.py.
Measured 2026-10-22: oldkid.net from here 502 ms (1.1.0) -> 416 ms (1.1.1); local PHP 8.4 pool 110 -> 33 ms.
"""
import json, statistics, sys, threading, time, urllib.parse, urllib.request

base = sys.argv[1]
n = int(sys.argv[2]) if len(sys.argv) > 2 else 20
old = len(sys.argv) > 3 and sys.argv[3] == 'old'
KEY = 'okp_7Qm2xV9rLs4Hd8Wn'
room = '0000be%02d' % (int(time.time()) % 100)
A, B = ('a1' * 8, '1' * 16), ('b2' * 8, '2' * 16)
op = urllib.request.build_opener()


def post(f):
    return json.loads(op.open(base, urllib.parse.urlencode(dict(f, k=KEY, room=room)).encode(), timeout=10).read())


def send(me, to):
    if old:
        post(dict(a='rsend', peer=me[0], ptok=me[1], frames=json.dumps([[to, 'AQBwaW5n']])))
    else:
        post(dict(a='rsend', peer=me[0], ptok=me[1], f=to + '.AQBwaW5n'))


def poll(me):
    while True:
        r = post(dict(a='rpoll', peer=me[0], ptok=me[1], wait='2000', **({} if old else {'c': '1'})))
        if r.get('frames') or r.get('f'):
            return


for me in (A, B):
    post(dict(a='rpoll', peer=me[0], ptok=me[1], wait='0'))


def echo():
    for _ in range(n):
        poll(B)
        send(B, A[0])


threading.Thread(target=echo, daemon=True).start()
time.sleep(0.3)
rt = []
for _ in range(n):
    t0 = time.perf_counter()
    send(A, B[0])
    poll(A)
    rt.append((time.perf_counter() - t0) * 1000)
    time.sleep(0.05)
print('relay round trip ms (%s): median %.0f, min %.0f, max %.0f' % ('old' if old else 'compact', statistics.median(rt), min(rt), max(rt)))
