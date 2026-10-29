// okweb-sp.js v1 - the JavaScript half of "Oldkid.net Online Play" for our DirectPlay in the browser.
// The C half (okweb/dplay/okweb_sp.c, linked into the game's WebAssembly) calls Module.okwebSP.*
// synchronously; this file does the asynchronous parts with okweb.js and keeps queues the C side
// drains from DirectPlay's pump. A page installs it before the game starts its multiplayer:
//
//   import { installOkwebProvider } from 'https://oldkid.net/aoe2net/sdk/v1/okweb-sp.js';
//   installOkwebProvider(Module, { server: 'https://oldkid.net/aoe2net/okplay.php', playerName: 'Ann' });
//
// Design: docs/WEB_NETPLAY.md section 3.3.

// ?v= must match okweb.js's VERSION: a new version is a new address, so browsers can't keep an old copy
import { OkWeb, clock, keepAlive, VERSION } from './okweb.js?v=1.2.0';

const hex = (n, w) => (n >>> 0).toString(16).padStart(w, '0');
const randHex = (bytes) => Array.from(crypto.getRandomValues(new Uint8Array(bytes)), (b) => b.toString(16).padStart(2, '0')).join('');
const b64 = (u8) => { let s = ''; for (let i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode(...u8.subarray(i, i + 0x8000)); return btoa(s); };
const unb64 = (s) => Uint8Array.from(atob(s), (c) => c.charCodeAt(0));
const peerHex = (lo, hi) => hex(hi, 8) + hex(lo, 8);
const MAX_PENDING = 512;   // messages waiting per peer for its connection to open

// ---- game passwords: the box (okweb/dplay/okweb_password.c asks through Module.okwebSP.askPassword)
// Styled like the Windows drop-in's (builtin.c, "Midnight Gold"): a black panel with navy specks, an
// olive frame, a yellow gothic title, silver text, a navy field and navy buttons with yellow labels.
const PW_CSS = `
.okweb-pw-back { position: fixed; inset: 0; background: rgba(0,0,0,.45); display: flex; align-items: center;
  justify-content: center; z-index: 2147483000; }
.okweb-pw { background-color: #000; background-image: radial-gradient(#000080 1px, transparent 1px);
  background-size: 4px 4px; border: 2px solid #808000; outline: 1px solid #000; box-shadow: 0 0 0 3px #000, 0 8px 30px #000;
  padding: 16px 20px 14px; min-width: 300px; max-width: 420px; color: #c0c0c0; font: 14px Georgia, 'Times New Roman', serif; }
.okweb-pw h2 { margin: 0 0 10px; color: #ff0; font: 22px 'Lucida Blackletter', 'Old English Text MT', Georgia, serif; text-align: center; }
.okweb-pw p { margin: 0 0 12px; white-space: pre-line; text-align: center; }
.okweb-pw input[type=password] { display: block; width: 100%; box-sizing: border-box; margin: 0 0 12px; padding: 5px 7px;
  background: #000080; color: #fff; border: 2px solid #808000; outline: 1px solid #000; font: 15px Georgia, serif; }
.okweb-pw label { display: flex; gap: 8px; align-items: center; margin: 0 0 12px; font-size: 12px; cursor: pointer; }
.okweb-pw input[type=checkbox] { accent-color: #ff0; width: 16px; height: 16px; margin: 0; }
.okweb-pw .row { display: flex; gap: 10px; justify-content: center; }
.okweb-pw button { min-width: 104px; padding: 6px 12px; background: #000080; color: #ff0; border: 2px solid #808000;
  outline: 1px solid #000; font: 14px Georgia, serif; cursor: pointer; }
.okweb-pw button:active { background: #00f; }
.okweb-pw button:focus-visible, .okweb-pw input:focus-visible { outline: 2px solid #ff0; }
`;

/** A box: resolves to { button: 'ok' | 'cancel', text, checked }. Enter = ok, Escape = cancel. */
function passwordBox({ title, text, edit, check, ok, cancel }) {
  return new Promise((resolve) => {
    if (typeof document === 'undefined') { resolve({ button: 'cancel', text: '', checked: false }); return; }
    if (!document.getElementById('okweb-pw-css')) {
      const st = document.createElement('style');
      st.id = 'okweb-pw-css';
      st.textContent = PW_CSS;
      document.head.appendChild(st);
    }
    const back = document.createElement('div');
    back.className = 'okweb-pw-back';
    const box = document.createElement('div');
    box.className = 'okweb-pw';
    box.setAttribute('role', 'dialog');
    box.setAttribute('aria-modal', 'true');
    const h = document.createElement('h2');
    h.textContent = title;
    box.appendChild(h);
    box.setAttribute('aria-label', title);
    const p = document.createElement('p');
    p.textContent = text;
    box.appendChild(p);
    let field = null, tick = null;
    if (edit) {
      field = document.createElement('input');
      field.type = 'password';
      field.autocomplete = 'off';
      field.maxLength = 120;
      field.setAttribute('aria-label', 'Password');
      box.appendChild(field);
    }
    if (check) {
      const label = document.createElement('label');
      tick = document.createElement('input');
      tick.type = 'checkbox';
      label.appendChild(tick);
      label.appendChild(document.createTextNode(check));
      box.appendChild(label);
    }
    const row = document.createElement('div');
    row.className = 'row';
    const done = (button) => {
      back.remove();
      document.removeEventListener('keydown', keys, true);
      resolve({ button, text: field ? field.value : '', checked: !!(tick && tick.checked) });
    };
    const add = (label, button) => {
      const b = document.createElement('button');
      b.type = 'button';
      b.textContent = label;
      b.onclick = () => done(button);
      row.appendChild(b);
      return b;
    };
    const okButton = add(ok, 'ok');
    if (cancel) add(cancel, 'cancel');
    box.appendChild(row);
    back.appendChild(box);
    // (the game's own keyboard handling stays out of it while the box is up)
    const keys = (e) => {
      if (e.key === 'Enter') { e.preventDefault(); done('ok'); }
      else if (e.key === 'Escape') { e.preventDefault(); done(cancel ? 'cancel' : 'ok'); }
      e.stopPropagation();
    };
    document.addEventListener('keydown', keys, true);
    back.addEventListener('keyup', (e) => e.stopPropagation());
    back.addEventListener('keypress', (e) => e.stopPropagation());
    document.body.appendChild(back);
    (field || okButton).focus();
  });
}

const PW_SETTING = 'okweb.usePassword';   // localStorage: 'off' after "Disable Hosting Password"

/** Install the provider on an Emscripten Module (before the game creates its DirectPlay object).
 *  opts: { server, key?, playerName?, gameName?, iceServers?, log?, keepAlive?, relay?, relayOnly?, pingMs?,
 *          brand?, usePassword?, password?, askPassword? }
 *  Game passwords (as the Windows drop-in): hosting asks for an optional password, joining a game
 *  that has one asks for it (a box styled like the drop-in's; brand = its title, default
 *  "Classic Netplay"). usePassword: 'ask' (default) | 'off' (never ask when hosting - joining a
 *  password game still asks); a player's "Disable Hosting Password" tick is remembered in
 *  localStorage. password: answer every question with this text, no box (tests; '' = open games).
 *  askPassword(kind): the game's own box instead - kind 'host' resolves to the password ('' = none),
 *  'join' to the password or null (Cancel). The typed text never leaves the page: DirectPlay gets a
 *  salted SHA-256 of it (okweb_password.c), the same value the drop-in uses.
 *  keepAlive (default true): while an online game is open, the page keeps running when hidden
 *  (okweb.js keepAlive()), so the other players don't see this one freeze or drop.
 *  When something goes wrong a player can save or send a log: Module.okwebSP.saveReport() /
 *  sendReport() (the connection steps, the kinds of network route found - no addresses - and recent
 *  errors; captureErrors: false leaves console.error alone). */
export function installOkwebProvider(Module, opts) {
  const states = new Map();   // provider id (one per DirectPlay object) -> state
  const events = [], errors = [];
  let listed = -1;   // (the game makes a DirectPlay object per list refresh: one log line per change)
  const keep = (list, max, a) => {
    const text = a.map((x) => (typeof x === 'string' ? x : (() => { try { return JSON.stringify(x); } catch { return String(x); } })())).join(' ');
    list.push(new Date().toISOString().slice(11, 23) + ' ' + text.slice(0, 400));
    if (list.length > max) list.splice(0, list.length - max);
  };
  const userLog = opts.log || (() => {});
  const log = (...a) => { keep(events, 300, a); userLog(...a); };
  if (opts.captureErrors !== false && typeof console !== 'undefined') {
    const ce = console.error.bind(console);
    console.error = (...a) => {
      const text = a.map((x) => (x && x.stack) || String(x));
      if (!/:fixme:/.test(text.join(' '))) keep(errors, 120, text);   // (Wine's harmless "not implemented" notes)
      ce(...a);
    };
    if (typeof addEventListener === 'function')
      addEventListener('error', (e) => keep(errors, 120, ['uncaught:', e.message, (e.error && e.error.stack) || '']));
  }
  Module.okwebClock = clock;   // DirectPlay's pump and waits (winweb.c) run from it: they go on in a hidden tab
  if (opts.keepAlive !== false) keepAlive({ when: () => [...states.values()].some((s) => s.session) });
  const st = (id) => {
    if (!states.has(id)) states.set(id, { net: null, app: '', session: null, rooms: [], listing: false, lastList: 0,
                                         inbox: [], pending: new Map(), joinState: 0, room: 0, me: '', roomsVersion: 0 });
    return states.get(id);
  };
  const netFor = (s, app) => {
    if (!s.net || s.app !== app) {
      s.net = new OkWeb({ server: opts.server, key: opts.key, game: app, gameName: opts.gameName || '',
                          iceServers: opts.iceServers, relay: opts.relay, relayOnly: opts.relayOnly, pingMs: opts.pingMs, log });
      s.app = app;
    }
    return s.net;
  };
  const attach = (s, session) => {
    s.session = session;
    session.on('message', (from, bytes) => s.inbox.push({ peer: from, bytes: new Uint8Array(bytes) }));
    session.on('peer-joined', (p) => { log('player connected:', p.id, JSON.stringify(p.name || '')); flush(s, p.id); });
    session.on('peer-left', (p, why) => log('player left:', p.id, why));
    session.on('peer-failed', (p, e) => log('player could not connect:', p.id, (e && e.message) || e));
    session.on('closed', (why) => { log('session closed:', why); });
  };
  const flush = (s, peer) => {
    const q = s.pending.get(peer);
    if (!q || !s.session) return;
    while (q.length && s.session.send(peer, q[0].bytes, { reliable: q[0].reliable })) q.shift();
    if (!q.length) s.pending.delete(peer);
  };

  const brand = opts.brand || 'Classic Netplay';
  const setting = () => { try { return localStorage.getItem(PW_SETTING); } catch { return null; } };

  Module.okwebSP = {
    init(id) { st(id); },

    // ---- game passwords (okweb_password.c): host -> the password ('' = an open game), join -> it or null (Cancel)
    async askPassword(kind) {
      if (typeof opts.password === 'string') return opts.password;
      if (opts.askPassword) return opts.askPassword(kind);
      if (kind === 'host') {
        if (opts.usePassword === 'off' || setting() === 'off') { log('password: not asking (off) - an open game'); return ''; }
        const r = await passwordBox({ title: brand, edit: true, ok: 'Host', cancel: 'No Password',
          text: 'Enter a password for this game (optional).\nAn empty entry will allow anyone to join.',
          check: 'Disable Hosting Password (this browser)' });
        if (r.checked) { try { localStorage.setItem(PW_SETTING, 'off'); } catch { /* private window */ } }
        const typed = r.button === 'ok' ? r.text : '';
        log(typed ? 'password: this game needs a password (only its salted hash is used)' : 'password: none - an open game');
        return typed;
      }
      const r = await passwordBox({ title: brand, edit: true, ok: 'Join', cancel: 'Cancel',
                                    text: 'This game has a password.\nType it to join:' });
      if (r.button !== 'ok') { log('password: the player cancelled the join'); return null; }
      log('password: joining a password game (sending only the salted hash)');
      return r.text;
    },
    wrongPassword() {
      log('password: wrong password - the host did not let us in');
      if (typeof opts.password === 'string' || opts.askPassword) return;
      passwordBox({ title: brand, text: 'Wrong password - the host did not let you in.', ok: 'OK' });
    },
    /** hosting asks for a password again (after "Disable Hosting Password") */
    resetPasswordSetting() { try { localStorage.removeItem(PW_SETTING); } catch { /* private window */ } },

    // ---- finding games
    list(id, app) {
      const s = st(id);
      const net = netFor(s, app);
      if (s.listing || performance.now() - s.lastList < 800) return;
      s.listing = true;
      net.list().then((rooms) => {
        s.rooms = rooms.filter((r) => r.meta && typeof r.meta.dp === 'string').map((r) => {
          let blob = null;
          try { blob = unb64(r.meta.dp); } catch { /* skip */ }
          return { room: parseInt(r.room, 16) >>> 0, host: r.host, blob };
        }).filter((r) => r.blob);
        if (s.rooms.length !== listed) { log('games listed:', s.rooms.length); listed = s.rooms.length; }
        s.roomsVersion++;
      }).catch((e) => log('list failed:', e.message)).finally(() => { s.listing = false; s.lastList = performance.now(); });
    },
    rooms(id) { return st(id).rooms.length; },
    roomsVersion(id) { return st(id).roomsVersion; },
    room(id, i, heap, out, cap, infoIdx, heap32) {
      const r = st(id).rooms[i];
      if (!r || r.blob.length > cap) return 0;
      heap.set(r.blob, out);
      heap32[infoIdx] = r.room;
      heap32[infoIdx + 1] = parseInt(r.host.slice(8), 16) >>> 0;
      heap32[infoIdx + 2] = parseInt(r.host.slice(0, 8), 16) >>> 0;
      heap32[infoIdx + 3] = r.blob.length;
      return r.blob.length;
    },

    // ---- hosting
    newIds(id, heap32, idx) {
      const s = st(id);
      s.room = (crypto.getRandomValues(new Uint32Array(1))[0] | 1) >>> 0;
      s.me = randHex(8);
      heap32[idx] = s.room;
      heap32[idx + 1] = parseInt(s.me.slice(8), 16) >>> 0;
      heap32[idx + 2] = parseInt(s.me.slice(0, 8), 16) >>> 0;
    },
    host(id, app, blob, name, maxp, locked) {
      const s = st(id);
      const net = netFor(s, app);
      s.joinState = 1;
      log('hosting', JSON.stringify(name || 'Game'), 'room', hex(s.room, 8), 'up to', maxp, 'players');
      net.host({ name: name || 'Game', maxPlayers: Math.max(2, maxp), playerName: opts.playerName || 'Host',
                 meta: { dp: b64(blob) }, room: hex(s.room, 8), peer: s.me, locked: !!locked })
        .then((session) => { log('hosting: listed'); attach(s, session); })
        .catch((e) => { log('host failed:', e.message); s.joinState = -1; });
      return 1;
    },
    setEnum(id, blob, players, maxp, open, locked) {
      const s = st(id);
      if (!s.session || !s.session.isHost) return;
      s.session.setMeta({ dp: b64(blob) });
      s.session.setJoinable(!!open);
      s.session.setLocked(!!locked);
    },

    // ---- joining
    join(id, room, lo, hi, app) {
      const s = st(id);
      const net = netFor(s, app);
      s.joinState = 0;
      log('joining room', hex(room, 8), 'host', peerHex(lo, hi));
      net.join({ room: hex(room, 8), host: peerHex(lo, hi) }, { playerName: opts.playerName || 'Player', keepMesh: true })
        .then((session) => { log('joined as', session.me); attach(s, session); s.me = session.me; s.joinState = 1; })
        .catch((e) => { log('join failed:', e.message); s.joinState = -1; });
    },
    joinState(id) { return st(id).joinState; },
    self(id, heap32, idx) {
      const me = st(id).me || '0'.repeat(16);
      heap32[idx] = parseInt(me.slice(8), 16) >>> 0;
      heap32[idx + 1] = parseInt(me.slice(0, 8), 16) >>> 0;
    },

    // ---- messages
    send(id, lo, hi, bytes, reliable) {
      const s = st(id);
      const peer = peerHex(lo, hi);
      if (s.session && s.session.send(peer, bytes, { reliable: !!reliable })) return 1;
      // not connected to that machine yet (the mesh is still forming): keep it for when it opens
      let q = s.pending.get(peer);
      if (!q) s.pending.set(peer, q = []);
      if (q.length >= MAX_PENDING) return 0;
      q.push({ bytes, reliable: !!reliable });
      return 1;
    },
    next(id, heap, out, cap, heap32, idx) {
      const s = st(id);
      const m = s.inbox.shift();
      if (!m) return 0;
      if (m.bytes.length > cap) return 0;
      heap.set(m.bytes, out);
      heap32[idx] = parseInt(m.peer.slice(8), 16) >>> 0;
      heap32[idx + 1] = parseInt(m.peer.slice(0, 8), 16) >>> 0;
      heap32[idx + 2] = m.bytes.length;
      return 1;
    },
    /** diagnostics: per DirectPlay object - role, peers, message counts, what waits */
    debug() {
      const out = {};
      for (const [id, s] of states) {
        out[id] = { app: s.app, joinState: s.joinState, me: s.me, room: s.room ? hex(s.room, 8) : '',
                    host: s.session ? s.session.isHost : null, inbox: s.inbox.length,
                    pending: Object.fromEntries([...s.pending].map(([p, q]) => [p, q.length])),
                    peers: s.session ? s.session.stats() : {}, rooms: s.rooms.length };
      }
      return out;
    },
    close(id) {
      const s = states.get(id);
      if (!s) return;
      if (s.session) s.session.close('closed').catch(() => {});
      s.session = null; s.inbox = []; s.pending.clear(); s.joinState = 0;
    },

    // ---- a log a player can save or send (connection steps, kinds of route found, recent errors)
    report() {
      const head = ['Oldkid.net Online Play log', 'time: ' + new Date().toISOString(),
        'page: ' + (typeof location !== 'undefined' ? location.origin + location.pathname : '?'),
        'browser: ' + (typeof navigator !== 'undefined' ? navigator.userAgent : '?'),
        'okweb ' + VERSION + ', server ' + opts.server + ', game ' + (opts.gameName || '?') + ', player ' + (opts.playerName || '?'),
        'state: ' + JSON.stringify(Object.fromEntries(Object.entries(Module.okwebSP.debug())
          .filter(([, d], i, all) => d.joinState !== 0 || i === all.length - 1)))];   // (the live ones and the newest)
      return head.concat(['--- netplay', ...events, '--- recent errors', ...errors]).join('\n') + '\n';
    },
    /** download the log as a text file */
    saveReport() {
      const blob = new Blob([Module.okwebSP.report()], { type: 'text/plain' });
      const a = document.createElement('a');
      a.href = URL.createObjectURL(blob);
      a.download = 'oldkid-netplay-log-' + new Date().toISOString().slice(0, 19).replace(/[:T]/g, '-') + '.txt';
      document.body.appendChild(a);
      a.click();
      setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
    },
    /** send the log to the room server's reports (okstats.php); resolves to its id, e.g. "1021-a3f09c" */
    async sendReport() {
      let body = Module.okwebSP.report();
      if (body.length > 16000) body = body.slice(0, 1500) + '\n[...]\n' + body.slice(body.length - 14000);
      const net = new OkWeb({ server: opts.server, key: opts.key, game: '0'.repeat(32) });
      const { status, text } = await net.server.post({ a: 'report', kind: 'check', ver: VERSION, body });
      if (!text.startsWith('ok ')) throw new Error('report not accepted: HTTP ' + status + ' ' + text.trim());
      return text.slice(3).trim();
    },
  };
  return Module.okwebSP;
}
