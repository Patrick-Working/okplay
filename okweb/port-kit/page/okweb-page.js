// okweb-page.js - the page side of a game's browser port, in one call: Online Play from the page's
// address, a log a player can save or send, and a ping overlay for testing. Generic: the game is only
// named. Load it next to okweb-sp.js (same ?v= as okweb.js):
//
//   import { connectPage } from './okweb-page.js?v=1.2.0';
//   connectPage(Module, { gameName: 'My Game (browser)', overlayParent: document.getElementById('game-container') });
//
// Address options (all optional):
//   ?netserver=URL   the room server (default: oldkid.net's)     ?netname=Ann     the player's name
//   ?relayonly       always through the room server's relay       ?noping          no ping measuring, no overlay
//   ?nopassword      never ask for a password when hosting        ?netpassword=x   answer password questions with x (tests)
//   ?autoplay=host   (a test run) hosts open games
// Returns a promise of Module.okwebSP (or null when the build has no Online Play).
import { installOkwebProvider } from './okweb-sp.js?v=1.2.0';

const CSS = `
.okweb-bar { display: flex; margin-top: 6px; font-size: 12px; color: #888; align-items: center; gap: 8px;
  flex-wrap: wrap; justify-content: center; }
.okweb-bar button { background: #231c0e; color: #d4af37; border: 1px solid #5a471c; padding: 3px 8px;
  border-radius: 3px; cursor: pointer; font-size: 11px; }
.okweb-ping { display: none; position: absolute; left: 6px; top: 30px; z-index: 50; background: rgba(0,0,0,.68);
  color: #d8d0b8; font: 11px/1.35 Consolas, 'Courier New', monospace; padding: 4px 7px; border: 1px solid #5a471c;
  border-radius: 3px; pointer-events: none; white-space: pre; }
.okweb-ping .good { color: #8fdc8f; } .okweb-ping .fair { color: #e8c45a; } .okweb-ping .poor { color: #f08070; }
.okweb-ping .dim { color: #8a8270; }
`;

/** opts: { gameName, server?, overlayParent? (the ping overlay's container, positioned), barParent? (where
 *  the log buttons go; default: after overlayParent), brand?, ...any installOkwebProvider option } */
export function connectPage(Module, opts = {}) {
  const p = new URLSearchParams(location.search);
  if (!document.getElementById('okweb-page-css')) {
    const st = document.createElement('style');
    st.id = 'okweb-page-css';
    st.textContent = CSS;
    document.head.appendChild(st);
  }
  const ping = !p.has('noping');
  const sp = installOkwebProvider(Module, Object.assign({
    server: p.get('netserver') || 'https://oldkid.net/aoe2net/okplay.php',
    playerName: p.get('netname') || 'Player',
    relayOnly: p.has('relayonly'),
    pingMs: ping ? 1000 : 0,
    usePassword: (p.has('nopassword') || p.get('autoplay') === 'host') ? 'off' : 'ask',
    password: p.has('netpassword') ? p.get('netpassword') : undefined,
    log: (...a) => console.log('[netplay]', ...a),
  }, opts));

  // the log bar: save the connection log, or send it to the room server
  const bar = document.createElement('div');
  bar.className = 'okweb-bar';
  bar.innerHTML = '<span>Online Play problem?</span><button type="button">Save netplay log</button>' +
                  '<button type="button">Send log</button><button type="button">Hide ping</button><span></span>';
  const [save, send, pingBtn] = bar.querySelectorAll('button');
  const status = bar.querySelector('span:last-child');
  save.onclick = () => sp.saveReport();
  send.onclick = () => {
    status.textContent = 'Sending...';
    sp.sendReport().then((ref) => { status.textContent = 'Sent - report id ' + ref; })
      .catch((e) => { status.textContent = 'Not sent (' + e.message + ') - use "Save netplay log" instead'; });
  };
  const anchor = opts.barParent || (opts.overlayParent && opts.overlayParent.parentNode) || document.body;
  if (opts.barParent) anchor.appendChild(bar);
  else if (opts.overlayParent) anchor.insertBefore(bar, opts.overlayParent.nextSibling);
  else anchor.appendChild(bar);

  startPingOverlay(sp, opts.overlayParent || document.body, pingBtn, ping);
  return sp;
}

// One line per other player: direct or relayed (and the route), the last round trip, average and range
// of the last 20, a graph of the last 30, messages per second each way. okweb's own pings, not the game's.
function startPingOverlay(sp, parent, btn, on) {
  const box = document.createElement('div');
  box.className = 'okweb-ping';
  parent.appendChild(box);
  let shown = on;
  const last = {}, BARS = '▁▂▃▄▅▆▇█', STEPS = [50, 100, 150, 250, 400, 600, 900];
  btn.textContent = on ? 'Hide ping' : 'Show ping';
  btn.disabled = !on;
  btn.title = on ? '' : 'Reload without ?noping to measure ping';
  btn.onclick = () => { shown = !shown; btn.textContent = shown ? 'Hide ping' : 'Show ping'; tick(); };
  if (!on) return;
  const esc = (t) => String(t).replace(/[&<>]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]));
  const grade = (ms) => (ms == null ? 'dim' : ms < 150 ? 'good' : ms < 400 ? 'fair' : 'poor');
  const bar = (ms) => { let i = 0; while (i < STEPS.length && ms >= STEPS[i]) i++; return BARS.charAt(i); };
  const pad = (t, n) => { t = String(t); return t.length >= n ? t.slice(0, n) : t + ' '.repeat(n - t.length); };
  function tick() {
    const d = sp.debug ? sp.debug() : {}, rows = [], now = performance.now();
    let role = '';
    for (const k in d) {
      if (d[k].host === null) continue;
      role = d[k].host ? 'hosting' : 'joined';
      for (const id in d[k].peers) {
        const s = d[k].peers[id], o = last[id], secs = o ? (now - o.at) / 1000 : 0;
        const inR = o && secs > 0 ? (s.received - o.received) / secs : 0, outR = o && secs > 0 ? (s.sent - o.sent) / secs : 0;
        const inB = o && secs > 0 ? (s.bytesReceived - o.bytesReceived) / secs / 1024 : 0;
        last[id] = { at: now, received: s.received, sent: s.sent, bytesReceived: s.bytesReceived };
        const route = !s.open ? 'connecting' : s.route === 'relay' ? 'relay' : 'direct' + (s.route ? ' ' + s.route : '');
        const spark = (s.pings || []).slice(-30).map((ms) => '<span class="' + grade(ms) + '">' + bar(ms) + '</span>').join('');
        rows.push(esc(pad(s.name || id.slice(0, 8), 12)) + ' ' + esc(pad(route, 18)) +
          '<span class="' + grade(s.ping) + '">' + pad(s.ping == null ? '  -- ms' : (s.ping < 1000 ? ('   ' + s.ping).slice(-4) : s.ping) + ' ms', 8) + '</span>' +
          '<span class="dim">' + (s.pingAvg == null ? '' : ' avg ' + s.pingAvg + ' (' + s.pingMin + '-' + s.pingMax + ')') + '</span> ' + spark +
          '<span class="dim">  in ' + inR.toFixed(1) + '/s ' + inB.toFixed(1) + ' KB/s  out ' + outR.toFixed(1) + '/s' +
          (s.pingLost ? '  lost ' + s.pingLost : '') + (s.buffered > 65536 ? '  backlog ' + Math.round(s.buffered / 1024) + ' KB' : '') + '</span>');
      }
    }
    box.style.display = shown && role ? 'block' : 'none';
    if (shown && role) box.innerHTML = '<span class="dim">Online Play (' + role + ') - ping test</span>\n' + (rows.join('\n') || '<span class="dim">waiting for players</span>');
  }
  setInterval(tick, 1000);
}
