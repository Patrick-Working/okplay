"""Two players in two headless Chrome tabs: a game's browser build hosts, joins, readies up and starts
an Online Play game, with the game's test console polled every --interval ms on the joining tab
(that once crashed a joining page: the console called into a frame suspended in a wait).

For a game page that has rogan2's test console (window.rogan2: text/screen/press/type, ?autoplay=host|join,
?keepalive). Serve the build with its data, then:

    python okweb/dplay/test/serve_game.py --build <game>/build_wasm --game <installed game> --port 8893
    python okweb/integrations/rogan2/two_tab_test.py --runs 10 --game 8893 --rdv 8796

Each run starts its own room-server stand-in (okplay/server/rdv_standin.py) on --rdv. The host's page is
http://127.0.0.1:<game>/, the joiner's http://localhost:<game>/ - two sites, so two players. A fresh
browser profile each time (a temp folder): the New Player box is answered with the player's name.
Needs: Python 3 with websocket-client (pip install websocket-client), Google Chrome or Microsoft Edge.
"""
import argparse, json, os, shutil, subprocess, sys, tempfile, time, urllib.request, urllib.parse
import websocket

HERE = os.path.dirname(os.path.abspath(__file__))
RDV = os.path.normpath(os.path.join(HERE, '..', '..', '..', 'okplay', 'server', 'rdv_standin.py'))


def find_browser():
    for p in (os.environ.get('CHROME', ''),
              r"C:\Program Files\Google\Chrome\Application\chrome.exe",
              r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
              r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
              shutil.which('google-chrome') or '', shutil.which('chromium') or ''):
        if p and os.path.exists(p):
            return p
    sys.exit('no Chrome or Edge found: set CHROME to its path')


NEWPLAYER_JS = r"""
(() => { const t = setInterval(() => { try { if (window.rogan2 && rogan2.type && rogan2.has('New Player')) { rogan2.type(new URLSearchParams(location.search).get('netname') || 'Tester'); rogan2.press('OK'); } } catch (e) {} }, 400); })();
"""

LOOP_JS = NEWPLAYER_JS + r"""
(() => {
  window.__st = { calls: 0, traps: 0, first: null, errors: [] };
  window.addEventListener('error', (e) => { window.__st.errors.push(String(e.message)); });
  window.addEventListener('unhandledrejection', (e) => { window.__st.errors.push('rej: ' + String(e.reason && e.reason.stack || e.reason)); });
  const tick = () => {
    if (window.rogan2) {
      try { window.rogan2.screen(); window.__st.calls++; }
      catch (e) { window.__st.traps++; if (!window.__st.first) window.__st.first = String(e.stack || e); }
    }
    setTimeout(tick, %d);
  };
  setTimeout(tick, 50);
})();
"""


class Page:
    def __init__(self, port, url, script=None):
        q = urllib.request.Request('http://127.0.0.1:%d/json/new?about:blank' % port, method='PUT')
        info = json.loads(urllib.request.urlopen(q).read())
        self.tid = info['id']
        self.port = port
        self.ws = websocket.create_connection(info['webSocketDebuggerUrl'], timeout=30, suppress_origin=True)
        self.n = 0
        self.console = []
        self.cmd('Runtime.enable')
        self.cmd('Page.enable')
        if script:
            self.cmd('Page.addScriptToEvaluateOnNewDocument', source=script)
        self.cmd('Page.navigate', url=url)

    def _event(self, m):
        if m.get('method') == 'Runtime.consoleAPICalled':
            a = m['params']['args']
            self.console.append(' '.join(str(x.get('value', x.get('description', ''))) for x in a))
        elif m.get('method') == 'Runtime.exceptionThrown':
            d = m['params']['exceptionDetails']
            self.console.append('EXC ' + str(d.get('exception', {}).get('description', d.get('text'))))

    def cmd(self, method, **params):
        self.n += 1
        my = self.n
        self.ws.send(json.dumps({'id': my, 'method': method, 'params': params}))
        while True:
            m = json.loads(self.ws.recv())
            if m.get('id') == my:
                return m.get('result', m)
            self._event(m)

    def js(self, expr):
        r = self.cmd('Runtime.evaluate', expression=expr, returnByValue=True, awaitPromise=True)
        return r.get('result', {}).get('value')

    def close(self):
        try:
            self.ws.close()
        except Exception:
            pass
        try:
            urllib.request.urlopen('http://127.0.0.1:%d/json/close/%s' % (self.port, self.tid)).read()
        except Exception:
            pass


def text(p):
    return p.js("(() => { try { return rogan2.text(); } catch (e) { return 'ERR ' + e; } })()") or ''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--runs', type=int, default=10)
    ap.add_argument('--game', type=int, default=8893)
    ap.add_argument('--rdv', type=int, default=8796)
    ap.add_argument('--cdp', type=int, default=9341)
    ap.add_argument('--interval', type=int, default=50)
    ap.add_argument('--timeout', type=int, default=150)
    a = ap.parse_args()
    prof = tempfile.mkdtemp(prefix='okweb-two-tab-')
    chrome = subprocess.Popen([find_browser(), '--headless=new', '--remote-debugging-port=%d' % a.cdp, '--user-data-dir=' + prof,
                               '--autoplay-policy=no-user-gesture-required', '--mute-audio',   # never a sound on the PC
                               '--remote-allow-origins=*', '--window-size=1280,960',
                               '--disable-background-timer-throttling', '--disable-renderer-backgrounding',
                               '--disable-backgrounding-occluded-windows', '--disable-features=IntensiveWakeUpThrottling', 'about:blank'],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(3)
    ns = urllib.parse.quote('http://127.0.0.1:%d/okplay' % a.rdv, safe='')
    ok = 0
    results = []
    try:
        for run in range(a.runs):
            rdv = subprocess.Popen([sys.executable, RDV, '--port', str(a.rdv)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(1.5)
            host = Page(a.cdp, 'http://127.0.0.1:%d/?netserver=%s&netname=One&keepalive&autoplay=host' % (a.game, ns), NEWPLAYER_JS)
            t0 = time.time()
            while time.time() - t0 < 90 and 'Start Game' not in text(host) and "Ready" not in text(host):
                time.sleep(1)
            join = Page(a.cdp, 'http://localhost:%d/?netserver=%s&netname=Two&keepalive&autoplay=join' % (a.game, ns),
                        LOOP_JS % a.interval)
            t0 = time.time()
            res = 'timeout'
            while time.time() - t0 < a.timeout:
                time.sleep(1)
                st = join.js('JSON.stringify(window.__st)')
                st = json.loads(st) if st else {}
                if st.get('traps'):
                    res = 'TRAP'
                    break
                if "I'm Ready" in text(join):
                    res = 'joined'
                    # ready up and start: the multiplayer start (status screens, waits) with screen() still hammered
                    def retry(p, expr, secs=20):
                        t1 = time.time()
                        while time.time() - t1 < secs:
                            if p.js(expr): return True
                            time.sleep(0.5)
                        return False
                    time.sleep(2)
                    retry(join, "rogan2.press(\"I'm Ready!\")")
                    time.sleep(2)
                    retry(host, "rogan2.press(\"I'm Ready!\")")
                    time.sleep(2)
                    started = retry(host, "rogan2.press('Start Game')", 30)
                    t1 = time.time()
                    while time.time() - t1 < 120:
                        if 'Game Screen' in text(host).split(chr(10))[0] and 'Game Screen' in text(join).split(chr(10))[0]: break
                        time.sleep(2)
                    ingame = 'Game Screen' in text(host).split(chr(10))[0] and 'Game Screen' in text(join).split(chr(10))[0]
                    res = 'joined, started=%s, both in game=%s' % (started, ingame)
                    time.sleep(3)
                    st = json.loads(join.js('JSON.stringify(window.__st)') or '{}')
                    if st.get('traps'):
                        res = 'TRAP (after join) ' + res
                    break
            hl = host.js("JSON.stringify((Module.okwebSP && Module.okwebSP.report) ? 'has report' : '')")
            line = 'run %d: %s calls=%s traps=%s %.0fs' % (run, res, st.get('calls'), st.get('traps'), time.time() - t0)
            print(line, flush=True)
            asy = join.js("typeof Asyncify !== 'undefined' ? Asyncify.state + '/' + !!Asyncify.currData : '?'")
            print('  joiner Asyncify state/sleeping:', asy, flush=True)
            if not res.startswith('joined') or 'False' in res:
                print('  first:', (st.get('first') or '')[:3000])
                print('  errors:', st.get('errors', [])[:5])
                print('  joiner screen:', text(join)[:800])
                print('  joiner console tail:', '\n    '.join(join.console[-25:]))
                print('  host screen:', text(host)[:500])
            else:
                ok += 1
            results.append(res)
            host.close()
            join.close()
            rdv.terminate()
            rdv.wait()
            time.sleep(1)
    finally:
        chrome.terminate()
        time.sleep(1)
        shutil.rmtree(prof, ignore_errors=True)
    print('joined %d/%d' % (ok, a.runs))


if __name__ == '__main__':
    main()
