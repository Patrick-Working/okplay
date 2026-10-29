"""A tiny PHP 'pool' for local tests: N `php -S` servers on one web folder, behind a threaded proxy
that gives each request to a free server - so long-polling requests (the relay's rpoll) run alongside
others, as on real hosting (php -S alone answers one request at a time).

  python okplay/tests/php_pool.py <php.exe> <web folder with okplay.php> <port> [n]

then e.g. python okplay/tests/relay_bench.py http://127.0.0.1:<port>/okplay.php"""
import http.client
import http.server
import queue
import socketserver
import subprocess
import sys
import time

php, web, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
n = int(sys.argv[4]) if len(sys.argv) > 4 else 6
free = queue.Queue()
procs = []
for i in range(n):
    p = port + 1 + i
    procs.append(subprocess.Popen([php, '-S', '127.0.0.1:%d' % p, '-t', web],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
    free.put(p)
time.sleep(1)


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def _go(self, method):
        body = self.rfile.read(int(self.headers.get('Content-Length', 0) or 0)) if method == 'POST' else None
        p = free.get()
        try:
            c = http.client.HTTPConnection('127.0.0.1', p, timeout=30)
            hdr = {'Content-Type': self.headers.get('Content-Type', 'application/x-www-form-urlencoded')}
            c.request(method, self.path, body=body, headers=hdr)
            r = c.getresponse()
            data = r.read()
            self.send_response(r.status)
            for k in ('Content-Type', 'Access-Control-Allow-Origin', 'Cache-Control'):
                if r.getheader(k):
                    self.send_header(k, r.getheader(k))
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            c.close()
        finally:
            free.put(p)

    def do_POST(self):
        self._go('POST')

    def do_GET(self):
        self._go('GET')

    def log_message(self, *a):
        pass


class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


print('pool on', port, 'backends', [port + 1 + i for i in range(n)], flush=True)
try:
    S(('127.0.0.1', port), H).serve_forever()
finally:
    for pr in procs:
        pr.kill()
