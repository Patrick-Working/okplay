// okweb.js v1 - browser-to-browser netplay through an okplay.php room server (oldkid.net).
// Peer to peer over WebRTC data channels; the server only lists rooms and passes each joiner's
// first offer/answer to the host. Protocol and design: docs/WEB_NETPLAY.md (sections 2 and 3).
// No dependencies; an ES module. License: LGPL 2.1+ like the rest of Classic Netplay.

export const VERSION = '1.2.0';   // (pages load the files as okweb.js?v=VERSION: the web host caches .js for days)
export const PROTOCOL = 1;          // the SDK's own peer protocol (hello/welcome/relay); bump = incompatible
const WEB_WIRE = 100;               // okplay.php rooms of browser games (Windows drop-ins skip other wires)
const MAX_MESSAGE = 65536;          // bytes per application message (okplay's cap)
const MAX_BUFFERED = 8 * 1024 * 1024;   // a peer that stops reading: send() refuses past this
const DEFAULT_KEY = 'okp_7Qm2xV9rLs4Hd8Wn';
const DEFAULT_ICE = [{ urls: 'stun:stun.cloudflare.com:3478' }, { urls: 'stun:stun.l.google.com:19302' }];
const T_APP = 0, T_CTL = 1;         // first byte of every data-channel frame

/* Timers that keep going in a background tab. Browsers slow a hidden page's setTimeout and
 * setInterval to about once a second, and after a few minutes to about once a minute: a hosted
 * room would expire on the server and the game's networking would stall. A dedicated worker's
 * timers are not slowed, so while the page is hidden these timers run from a small worker's ticks
 * (while it is visible, from ordinary timers). Used by everything here, by the DirectPlay provider
 * (okweb-sp.js gives it to the C side as Module.okwebClock) and by keepAlive() below. */
export const clock = (() => {
  const TICK_MS = 15;
  const timers = new Map();   // id -> { due, fn, every }
  let nextId = 1, worker = null, ticking = false, native = 0, nativeDue = Infinity;
  const hidden = () => typeof document !== 'undefined' && document.visibilityState === 'hidden';
  const haveWorker = () => {
    if (worker === null) {
      try {
        const src = 'let t = 0; onmessage = (e) => { clearInterval(t); t = e.data > 0 ? setInterval(() => postMessage(0), e.data) : 0; };';
        worker = new Worker(URL.createObjectURL(new Blob([src], { type: 'text/javascript' })));
        worker.onmessage = run;
      } catch { worker = false; }   // (a page whose security policy forbids such workers: ordinary timers)
    }
    return !!worker;
  };
  const schedule = () => {
    clearTimeout(native);
    native = 0;
    nativeDue = Infinity;
    let due = Infinity;
    for (const t of timers.values()) if (t.due < due) due = t.due;
    const ticks = due !== Infinity && hidden() && haveWorker();
    if (ticks !== ticking) { worker.postMessage(ticks ? TICK_MS : 0); ticking = ticks; }
    if (!ticks && due !== Infinity) { nativeDue = due; native = setTimeout(run, Math.max(0, due - performance.now())); }
  };
  function run() {
    const now = performance.now();
    for (const [id, t] of [...timers]) {
      if (t.due > now || !timers.has(id)) continue;
      if (t.every) t.due = now + t.every; else timers.delete(id);
      try { t.fn(); } catch (e) { console.error('okweb: timer', e); }
    }
    schedule();
  }
  const add = (fn, ms, every) => {
    const id = nextId++, due = performance.now() + Math.max(0, ms || 0);
    timers.set(id, { due, fn, every });
    if (!ticking && (hidden() || due < nativeDue)) schedule();
    return id;
  };
  if (typeof document !== 'undefined') document.addEventListener('visibilitychange', schedule);
  return {
    setTimeout: (fn, ms) => add(fn, ms, 0),
    setInterval: (fn, ms) => add(fn, ms, Math.max(1, ms)),
    clear: (id) => { timers.delete(id); },
    sleep: (ms) => new Promise((resolve) => add(resolve, ms, 0)),
  };
})();

/** Keep a game page running while it is hidden (another tab in front, or the window minimized).
 *  Browsers stop requestAnimationFrame for hidden pages, which stops an Emscripten main loop and
 *  with it the game's side of the network: the other players see it freeze, then drop. This makes
 *  the page's requestAnimationFrame fall back to the clock above (opts.fps frames a second while
 *  hidden, default 20). opts.when: a function, only while it returns true (default: always).
 *  Call once, before the game starts its main loop; returns an undo function. */
export function keepAlive(opts = {}) {
  if (typeof window === 'undefined' || window.okwebKeepAlive) return () => {};
  const wait = 1000 / (opts.fps || 20), when = opts.when || (() => true);
  const nativeRAF = window.requestAnimationFrame.bind(window), nativeCancel = window.cancelAnimationFrame.bind(window);
  const pending = new Map();
  let next = 1;
  const drop = (id) => {
    const p = pending.get(id);
    if (p) { pending.delete(id); nativeCancel(p.raf); clock.clear(p.timer); }
    return p;
  };
  window.requestAnimationFrame = (fn) => {
    const id = next++;
    const fire = (ts) => { if (drop(id)) fn(ts); };   // whichever comes first: the frame, or the clock
    pending.set(id, { raf: nativeRAF(fire), timer: when() ? clock.setTimeout(() => fire(performance.now()), wait) : 0 });
    return id;
  };
  window.cancelAnimationFrame = (id) => { drop(id); };
  window.okwebKeepAlive = true;
  return () => {
    window.requestAnimationFrame = nativeRAF;
    window.cancelAnimationFrame = nativeCancel;
    window.okwebKeepAlive = false;
  };
}

/* the kinds of ICE candidate in a description - "host 2, srflx 1" - and no addresses: local network
 * only (host), through the router as seen from outside (srflx/prflx), through a relay (relay) */
const routes = (sdp) => {
  const n = {};
  for (const m of String(sdp || '').matchAll(/ typ (host|srflx|prflx|relay)/g)) n[m[1]] = (n[m[1]] || 0) + 1;
  return Object.entries(n).map(([k, v]) => k + ' ' + v).join(', ') || 'none';
};

const hex = (bytes) => Array.from(crypto.getRandomValues(new Uint8Array(bytes)), (b) => b.toString(16).padStart(2, '0')).join('');
const sleep = (ms) => clock.sleep(ms);
const utf8 = new TextEncoder(), unutf8 = new TextDecoder();
const b64json = (o) => btoa(String.fromCharCode(...utf8.encode(JSON.stringify(o))));
const unb64json = (s) => JSON.parse(unutf8.decode(Uint8Array.from(atob(s), (c) => c.charCodeAt(0))));

class Emitter {
  constructor() { this._h = {}; }
  on(ev, fn) { (this._h[ev] ||= []).push(fn); return this; }
  off(ev, fn) { this._h[ev] = (this._h[ev] || []).filter((f) => f !== fn); return this; }
  emit(ev, ...args) {
    for (const fn of this._h[ev] || []) { try { fn(...args); } catch (e) { console.error('okweb: handler for', ev, e); } }
  }
}

/* The room server: form POSTs (simple requests: no CORS preflight). */
class Server {
  constructor(url, key, log) { this.url = url; this.key = key; this.log = log; }
  async post(fields) {
    const body = new URLSearchParams({ k: this.key, ...fields });
    const r = await fetch(this.url, { method: 'POST', body, cache: 'no-store' });
    const text = await r.text();
    return { status: r.status, text };
  }
  /** a POST that goes out even while the page unloads (no reply) */
  beacon(fields) {
    const body = new URLSearchParams({ k: this.key, ...fields });
    try {
      if (typeof navigator !== 'undefined' && navigator.sendBeacon && navigator.sendBeacon(this.url, body)) return;
    } catch { /* fall through */ }
    try { fetch(this.url, { method: 'POST', body, keepalive: true, cache: 'no-store' }).catch(() => {}); } catch { /* gone anyway in 15 s */ }
  }
  async json(fields) {
    const { status, text } = await this.post(fields);
    let v;
    try { v = JSON.parse(text); } catch { throw new Error(`room server: HTTP ${status}: ${text.slice(0, 120)}`); }
    if (!v.ok) { const e = new Error(`room server: ${v.error || 'error'}`); e.status = status; throw e; }
    return v;
  }
}

// base64url without padding: travels in a form field unescaped (standard base64's + / = would triple)
const b64u8 = (u8) => {
  let s = '';
  for (let i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode(...u8.subarray(i, i + 0x8000));
  return btoa(s).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
};
const unb64u8 = (s) => Uint8Array.from(atob(s.replace(/-/g, '+').replace(/_/g, '/')), (c) => c.charCodeAt(0));
const RELAY_WAIT_MS = 1500;            // a pickup waits this long on the server for something to arrive
const RELAY_MAX_OUT = 1024 * 1024;     // bytes waiting to go out before send() says no

/* The fallback when two players' routers won't let them connect directly: their messages go
 * through the room server (okplay.php rsend/rpoll), one mailbox per player. Slower than a direct
 * connection, but it works wherever the page itself loads. A message goes out at once when nothing
 * else is on its way (messages queued meanwhile go together next); a pickup waits on the server,
 * which answers within ~20 ms of something arriving. Each frame is [lane (1 reliable / 0 not), type,
 * payload]; everything arrives in order. On the wire: f = "to.data*to.data" (base64url). */
class Relay {
  constructor(session) {
    this.s = session;
    this.tok = hex(8);       // our mailbox's key (the server keeps it from our first request)
    this.out = [];           // [to, base64url]
    this.outBytes = 0;
    this.soon = false;       // a send is about to start (messages queued in the same turn go with it)
    this.sending = false;
    this.active = false;
  }
  start() {
    if (this.active) return;
    this.active = true;
    this.s._log('relay: on - messages to players this browser cannot reach directly go through', this.s.net.server.url);
    this._pickup();
  }
  stop() { this.active = false; }
  queue(to, frame) {
    if (this.outBytes > RELAY_MAX_OUT) return false;
    const d = b64u8(frame);
    this.out.push([to, d]);
    this.outBytes += d.length;
    if (!this.sending && !this.soon) { this.soon = true; Promise.resolve().then(() => { this.soon = false; this._send(); }); }
    return true;
  }
  _wire(batch) { return batch.map(([to, d]) => to + '.' + d).join('*'); }
  async _send() {
    if (this.sending || !this.out.length || !this.active) return;
    this.sending = true;
    const batch = this.out.splice(0, 64);
    try {
      await this.s.net.server.json({ a: 'rsend', room: this.s.room, peer: this.s.me, ptok: this.tok, f: this._wire(batch) });
      for (const [, d] of batch) this.outBytes -= d.length;
    } catch (e) {
      this.s._log('relay: send failed:', e.message);
      this.out.unshift(...batch);   // in order, again shortly
      await sleep(1000);
    } finally {
      this.sending = false;
      if (this.out.length) this._send();   // what was queued meanwhile, at once
    }
  }
  /** what's waiting goes out now (closing: the byes) */
  async flush() {
    for (let i = 0; i < 50 && (this.out.length || this.sending); i++) {
      if (!this.sending) await this._send(); else await sleep(20);
    }
  }
  /** and from a page that is unloading (no reply) */
  beacon() {
    if (!this.out.length) return;
    this.s.net.server.beacon({ a: 'rsend', room: this.s.room, peer: this.s.me, ptok: this.tok, f: this._wire(this.out.splice(0, 64)) });
  }
  async _pickup() {
    while (this.active && !this.s.closed) {
      try {
        const v = await this.s.net.server.json({ a: 'rpoll', room: this.s.room, peer: this.s.me, ptok: this.tok, wait: RELAY_WAIT_MS, c: 1 });
        for (const one of v.f ? v.f.split('*') : []) {
          const dot = one.indexOf('.');
          const from = one.slice(0, dot);
          try { this.s._relayFrame(from, unb64u8(one.slice(dot + 1))); } catch (e) { this.s._log('relay: bad message from', from, e.message); }
        }
      } catch (e) {
        this.s._log('relay: pickup failed:', e.message);
        await sleep(1000);
      }
    }
  }
}

/* One remote machine: a peer connection with okplay's two lanes as pre-negotiated channels - or,
 * when that can't connect, the relay above. */
class Peer {
  constructor(session, id, name) {
    this.session = session;
    this.id = id;
    this.name = name || '';
    this.relay = false;    // messages go through the room server's relay
    this.pc = new RTCPeerConnection({ iceServers: session.net.iceServers });
    this.rel = this.pc.createDataChannel('rel', { negotiated: true, id: 0 });
    this.unrel = this.pc.createDataChannel('unrel', { negotiated: true, id: 1, ordered: false, maxRetransmits: 0 });
    this.open = false;     // both channels open
    this.joined = false;   // in the game (host: said hello and was welcomed; joiner: connected)
    this.gone = false;
    this.stats = { sent: 0, received: 0, bytesSent: 0, bytesReceived: 0 };
    this.pings = { next: 1, waiting: new Map(), times: [], lost: 0 };   // pingMs: round trips (ms), newest last
    this.route = '';       // the direct connection's route kinds, e.g. 'srflx-srflx' (from getStats)
    this.opened = new Promise((resolve, reject) => { this._resolveOpen = resolve; this._rejectOpen = reject; });
    let n = 0;
    for (const ch of [this.rel, this.unrel]) {
      ch.binaryType = 'arraybuffer';
      ch.onopen = () => { if (++n === 2) { this.open = true; this._resolveOpen(this); } };
      ch.onclose = () => this.session._peerClosed(this, 'closed');
      ch.onmessage = (e) => this.session._frame(this, e.data, ch === this.rel);
    }
    this.pc.onconnectionstatechange = () => {
      const s = this.pc.connectionState;
      session._log('peer', id, 'connection:', s);
      if (s === 'failed' && !this.useRelay('failed')) {
        this._rejectOpen(new Error('no network path to ' + id));
        this.session._peerClosed(this, 'failed');
      }
    };
    this.timer = clock.setTimeout(() => {
      if (!this.open && !this.useRelay('did not open in time')) {
        this._rejectOpen(new Error('timed out connecting to ' + id));
        this.session._peerClosed(this, 'timeout');
      }
    }, session.net.connectTimeoutMs);
  }

  // once both sides have each other's description, a direct connection gets relayAfterMs to open
  _armRelay() {
    if (!this.session.net.relay || this.open) return;
    clock.clear(this.timer);
    this.timer = clock.setTimeout(() => { if (!this.open) this.useRelay('did not open in ' + this.session.net.relayAfterMs / 1000 + ' s'); },
                                  this.session.net.relayAfterMs);
  }

  /** go through the room server's relay from now on (false: the relay is turned off) */
  useRelay(why) {
    if (this.relay || this.gone || !this.session.net.relay) return false;
    this.relay = true;
    this.session._log('peer', this.id, 'direct connection', why, '- using the relay');
    clock.clear(this.timer);
    for (const ch of [this.rel, this.unrel]) { ch.onopen = ch.onclose = ch.onmessage = null; }
    this.pc.onconnectionstatechange = null;
    try { this.rel.close(); this.unrel.close(); this.pc.close(); } catch { /* already closed */ }
    this.open = true;
    this.session._relayOn();
    // a player who never answers over the relay either is dropped as before
    this.timer = clock.setTimeout(() => { if (!this.joined) this.session._peerClosed(this, 'timeout'); }, this.session.net.connectTimeoutMs);
    this._resolveOpen(this);
    return true;
  }

  // non-trickle ICE: the whole description once gathering is done (or after gatherMs) - one message each way
  async _gathered() {
    if (this.pc.iceGatheringState !== 'complete') {
      await new Promise((resolve) => {
        const t = clock.setTimeout(resolve, this.session.net.gatherMs);
        this.pc.addEventListener('icegatheringstatechange', () => {
          if (this.pc.iceGatheringState === 'complete') { clock.clear(t); resolve(); }
        });
      });
    }
    // which kinds of network route this browser found (for a log: why two players can't connect)
    this.session._log('peer', this.id, 'my routes:', routes(this.pc.localDescription.sdp));
    let sdp = this.pc.localDescription.sdp;
    if (this.session.net._testNoDirect) sdp = sdp.split('\r\n').filter((l) => !l.startsWith('a=candidate:')).join('\r\n');   // (tests: no direct path)
    return { type: this.pc.localDescription.type, sdp };
  }
  // (an empty description: "relay only" - relayOnly here, or the other side's choice)
  async offer() {
    if (this.session.net.relayOnly) return { type: 'offer', sdp: '' };
    await this.pc.setLocalDescription(await this.pc.createOffer());
    return this._gathered();
  }
  async answer(desc) {
    this.session._log('peer', this.id, 'their routes:', routes(desc && desc.sdp));
    if ((!desc || !desc.sdp || this.session.net.relayOnly) && this.useRelay(desc && desc.sdp ? 'not tried (relay only)' : 'not offered by the other side')) {
      return { type: 'answer', sdp: '' };
    }
    await this.pc.setRemoteDescription(desc);
    await this.pc.setLocalDescription(await this.pc.createAnswer());
    const mine = await this._gathered();
    this._armRelay();
    return mine;
  }
  async accept(desc) {
    this.session._log('peer', this.id, 'their routes:', routes(desc && desc.sdp));
    if (this.relay) return;
    if ((!desc || !desc.sdp) && this.useRelay('not offered by the other side')) return;
    await this.pc.setRemoteDescription(desc);
    this._armRelay();
  }

  sendFrame(type, payload, reliable) {
    if (this.relay) {
      if (this.gone) return false;
      const f = new Uint8Array(payload.length + 2);
      f[0] = reliable ? 1 : 0;
      f[1] = type;
      f.set(payload, 2);
      if (!this.session._relay.queue(this.id, f)) return false;
      if (type === T_APP) { this.stats.sent++; this.stats.bytesSent += payload.length; }
      return true;
    }
    const ch = reliable ? this.rel : this.unrel;
    if (!this.open || ch.readyState !== 'open') return false;
    if (ch.bufferedAmount > MAX_BUFFERED) return false;
    const f = new Uint8Array(payload.length + 1);
    f[0] = type;
    f.set(payload, 1);
    ch.send(f);
    if (type === T_APP) { this.stats.sent++; this.stats.bytesSent += payload.length; }
    return true;
  }
  control(obj) { return this.sendFrame(T_CTL, utf8.encode(JSON.stringify(obj)), true); }

  // pingMs: a ping on the reliable lane; the answer gives the round trip. Peers before 1.1.2 don't
  // answer (they ignore control messages they don't know) - their ping stays unknown
  _ping() {
    const now = performance.now(), w = this.pings.waiting;
    for (const [n, t] of w) if (now - t > 10000) { w.delete(n); this.pings.lost++; }
    if (w.size >= 8) return;
    const n = this.pings.next++;
    if (this.control({ t: 'ping', n })) w.set(n, now);
  }
  _pong(n) {
    const t = this.pings.waiting.get(n);
    if (t === undefined) return;
    this.pings.waiting.delete(n);
    const times = this.pings.times;
    times.push(Math.round(performance.now() - t));
    if (times.length > 60) times.shift();
  }
  async _findRoute() {
    if (this.relay || !this.open) return;
    try {
      const all = new Map();
      (await this.pc.getStats()).forEach((r) => all.set(r.id, r));
      let pair = null;
      for (const r of all.values()) {
        if (r.type === 'transport' && r.selectedCandidatePairId) pair = all.get(r.selectedCandidatePairId);
      }
      if (!pair) for (const r of all.values()) if (r.type === 'candidate-pair' && r.state === 'succeeded' && r.nominated) pair = r;
      if (!pair) return;
      const kind = (id) => (all.get(id) || {}).candidateType || '?';
      this.route = kind(pair.localCandidateId) + '-' + kind(pair.remoteCandidateId);
    } catch { /* no stats in this browser */ }
  }
  close() {
    this.gone = true;
    clock.clear(this.timer);
    try { this.rel.close(); this.unrel.close(); this.pc.close(); } catch { /* already closed */ }
  }
}

/* A game in progress: the host's view or a joiner's. Events:
 *   peer-joined (peer), peer-left (peer, reason), peer-failed (peer, error),
 *   message (fromId, Uint8Array, reliable), closed (reason) */
export class Session extends Emitter {
  constructor(net, isHost, room, me, name) {
    super();
    this.net = net;
    this.isHost = isHost;
    this.room = room;
    this.me = me;
    this.myName = name;
    this.peers = new Map();       // id -> Peer (connected or connecting)
    this.names = new Map();       // id -> name, of everyone in the game
    this.closed = false;
    this.meta = null;
    this.hostId = isHost ? me : null;
    this._relay = null;           // the room server relay, once a peer needs it
    this._pingTicks = 0;
    if (net.pingMs > 0) this._pingTimer = clock.setInterval(() => this._pingAll(), net.pingMs);
    // the page closing or reloading: tell the others and take the room off the list now (a room
    // otherwise stays listed for 15 s, and a player who picks it waits for a host that's gone)
    this._onPageHide = (e) => { if (!e.persisted) this._pageGone(); };
    if (typeof addEventListener === 'function') addEventListener('pagehide', this._onPageHide);
  }

  /** the other players in the game (connected and past the host's welcome) */
  get players() { return [...this.peers.values()].filter((p) => p.joined && p.open).map((p) => ({ id: p.id, name: p.name })); }

  /** bytes to one peer (id) or to everyone (null). reliable: ordered and resent; else may be lost or reordered.
   *  Returns false when the peer isn't connected or isn't reading (send buffer over 8 MB). */
  send(to, data, { reliable = true } = {}) {
    const bytes = data instanceof Uint8Array ? data : typeof data === 'string' ? utf8.encode(data) : new Uint8Array(data);
    if (bytes.length > MAX_MESSAGE) throw new RangeError(`okweb: message of ${bytes.length} bytes (at most ${MAX_MESSAGE})`);
    if (to !== null && to !== undefined) {
      const p = this.peers.get(to);
      return !!p && p.sendFrame(T_APP, bytes, reliable);
    }
    let all = true;
    for (const p of this.peers.values()) if (p.open && !p.sendFrame(T_APP, bytes, reliable)) all = false;
    return all;
  }

  stats() {
    const out = {};
    for (const p of this.peers.values()) {
      const t = p.pings.times, recent = t.slice(-20);
      out[p.id] = { name: p.name, open: p.open, ...p.stats,
                    buffered: p.relay ? (this._relay ? this._relay.outBytes : 0) : p.rel.bufferedAmount + p.unrel.bufferedAmount,
                    state: p.relay ? 'relay' : p.pc.connectionState, route: p.relay ? 'relay' : p.route };
      if (this.net.pingMs) {
        Object.assign(out[p.id], { ping: t.length ? t[t.length - 1] : null, pingLost: p.pings.lost, pings: t.slice(),
                                   pingAvg: recent.length ? Math.round(recent.reduce((a, b) => a + b, 0) / recent.length) : null,
                                   pingMin: recent.length ? Math.min(...recent) : null, pingMax: recent.length ? Math.max(...recent) : null });
      }
    }
    return out;
  }

  /** leave the game (a host closes it for everyone) */
  async close(reason = 'closed') {
    if (this.closed) return;
    this.closed = true;
    if (typeof removeEventListener === 'function') removeEventListener('pagehide', this._onPageHide);
    for (const p of this.peers.values()) { p.control({ t: 'bye' }); }
    if (this._relay) { await this._relay.flush().catch(() => {}); this._relay.stop(); }
    await sleep(50);   // let the byes go out
    for (const p of this.peers.values()) p.close();
    this.peers.clear();
    clock.clear(this._announceTimer);
    clock.clear(this._pollTimer);
    clock.clear(this._pingTimer);
    if (this.isHost) {
      try { await this.net.server.post({ a: 'close', room: this.room, tok: this.tok }); } catch { /* gone anyway in 15 s */ }
    }
    this.emit('closed', reason);
  }

  // ---- internals

  _log(...a) { this.net._log(...a); }

  // what close() does, but synchronously: an unloading page gets no more turns
  _pageGone() {
    if (this.closed) return;
    this.closed = true;
    for (const p of this.peers.values()) { p.control({ t: 'bye' }); p.close(); }
    if (this._relay) { this._relay.beacon(); this._relay.stop(); }
    this.peers.clear();
    clock.clear(this._announceTimer);
    clock.clear(this._pollTimer);
    clock.clear(this._pingTimer);
    if (this.isHost && this.tok) this.net.server.beacon({ a: 'close', room: this.room, tok: this.tok });
  }

  _pingAll() {
    const route = this._pingTicks++ % 5 === 0;
    for (const p of this.peers.values()) {
      if (!p.open || !p.joined || p.gone) continue;
      p._ping();
      if (route) p._findRoute();
    }
  }

  _relayOn() {
    if (!this._relay) this._relay = new Relay(this);
    this._relay.start();
  }

  // a frame from the relay: [lane, type, payload]. A peer still trying to connect directly switches -
  // the other side gave up on that first
  _relayFrame(from, bytes) {
    const p = this.peers.get(from);
    if (!p || p.gone || bytes.length < 2) return;
    if (!p.relay) p.useRelay('given up by the other side');
    if (!p.relay) return;
    this._frame(p, bytes.subarray(1), bytes[0] === 1);
  }

  _frame(peer, data, reliable) {
    const f = new Uint8Array(data);
    if (!f.length) return;
    if (f[0] === T_APP) {
      peer.stats.received++;
      peer.stats.bytesReceived += f.length - 1;
      this.emit('message', peer.id, f.subarray(1), reliable);
      return;
    }
    if (f[0] !== T_CTL || !reliable) return;
    let m;
    try { m = JSON.parse(unutf8.decode(f.subarray(1))); } catch { return; }
    this._control(peer, m);
  }

  _peerClosed(peer, reason) {
    if (peer.gone) return;
    peer.close();
    if (this.peers.get(peer.id) === peer) this.peers.delete(peer.id);
    if (this.closed) return;
    if (!this.isHost && peer.id === this.hostId) {
      // keepMesh (join option): the host left but the others stay connected - for a protocol on top
      // that hands the game to another player itself (DirectPlay's host migration)
      if (!this.keepMesh) { this.close('host-left'); return; }
      this.emit('host-left', { id: peer.id, name: peer.name });
    }
    // events only for players who were in the game; a joiner the host turned away just goes
    if (peer.joined) this.emit('peer-left', { id: peer.id, name: peer.name }, reason);
    else if (reason !== 'rejected' && reason !== 'left') this.emit('peer-failed', { id: peer.id, name: peer.name }, new Error(reason));
    if (this.isHost && peer.joined) {
      this.names.delete(peer.id);
      for (const p of this.peers.values()) p.control({ t: 'peer-left', id: peer.id });
    }
  }

  _peerOpened(peer) {
    clock.clear(peer.timer);
    if (peer.joined || this.closed) return;
    peer.joined = true;
    this.emit('peer-joined', { id: peer.id, name: peer.name });
  }

  _control(peer, m) {
    switch (m.t) {
    case 'bye': this._peerClosed(peer, 'left'); break;
    case 'ping': peer.control({ t: 'pong', n: m.n }); break;   // (answered whether or not we ping ourselves)
    case 'pong': peer._pong(m.n); break;
    case 'relay':   // signaling between two joiners, through the host
      if (this.isHost) {
        const to = this.peers.get(m.to);
        if (to && to.open) to.control({ t: 'relay', from: peer.id, sig: m.sig });
      } else if (peer.id === this.hostId) {
        this._relayed(m.from, m.sig);
      }
      break;
    default:
      if (this.isHost) this._hostControl(peer, m); else this._joinerControl(peer, m);
    }
  }
}

class HostSession extends Session {
  /** false: the game has started - listed as not joinable, newcomers are refused */
  setJoinable(on) { this.joinable = !!on; }
  /** the game's own data shown in list() (sent with the next announce, about once a second) */
  setMeta(meta) { this.meta = meta; }
  /** the room list shows a lock (the game checks the password itself; okweb never sees it) */
  setLocked(locked) { this.locked = !!locked; }

  async start(opts) {
    this.tok = hex(8);
    this.maxPlayers = opts.maxPlayers || 8;
    this.name = (opts.name || 'Game').slice(0, 64);
    this.meta = opts.meta ?? null;
    this.locked = !!opts.locked;
    this.joinable = true;
    this.names.set(this.me, this.myName);
    await this._announce();   // throws if the server refuses (full, bad key)
    this._announceTimer = clock.setInterval(() => this._announce().catch((e) => this._log('announce failed:', e.message)), this.net.announceMs);
    this._pollTimer = clock.setInterval(() => this._poll().catch((e) => this._log('poll failed:', e.message)), this.net.announceMs);
    return this;
  }

  async _announce() {
    const players = 1 + this.players.length;
    const open = this.joinable && players < this.maxPlayers;
    const enumBlob = b64json({ v: PROTOCOL, name: this.name, players, max: this.maxPlayers, meta: this.meta, host: this.myName, open });
    const { status, text } = await this.net.server.post({
      a: 'announce', room: this.room, peer: this.me, tok: this.tok, cands: '-', enum: enumBlob, name: this.name,
      players, maxp: this.maxPlayers, flags: (open ? 0 : 0x20) | (this.locked ? 0x400 : 0), ver: VERSION, wire: WEB_WIRE,
      app: this.net.game, game: this.net.gameName || '', state: this.joinable ? 'lobby' : 'playing',
    });
    if (!text.startsWith('ok')) throw new Error(`room server: HTTP ${status}: ${text.trim().slice(0, 120)}`);
  }

  async _poll() {
    if (this.closed) return;
    const v = await this.net.server.json({ a: 'poll', room: this.room, peer: this.me, tok: this.tok });
    for (const m of v.msgs) {
      let sig;
      try { sig = JSON.parse(m.data); } catch { continue; }
      if (sig.t !== 'offer' || this.peers.has(m.from)) continue;
      if (this.peers.size >= 2 * this.maxPlayers) continue;   // a flood: the joiner times out
      // (a full game still answers: the joiner's hello is refused with "the game is full" at once)
      const p = new Peer(this, m.from, '');
      this.peers.set(m.from, p);
      p.opened.catch(() => {});
      try {
        const answer = await p.answer(sig.desc);
        await this.net.server.json({ a: 'signal', room: this.room, peer: this.me, tok: this.tok, to: m.from,
                                     data: JSON.stringify({ t: 'answer', desc: answer }) });
      } catch (e) { this._log('answering', m.from, 'failed:', e.message); this._peerClosed(p, 'failed'); }
    }
  }

  _hostControl(peer, m) {
    if (m.t !== 'hello' || peer.name) return;
    const reject = (reason) => { peer.control({ t: 'reject', reason }); clock.setTimeout(() => this._peerClosed(peer, 'rejected'), 200); };
    if (m.game !== this.net.game) return reject('a different game');
    if (!this.joinable) return reject('the game has started');
    if (m.proto !== PROTOCOL) return reject(`okweb protocol ${m.proto}, the host has ${PROTOCOL}`);
    if (1 + this.players.length + 1 > this.maxPlayers) return reject('the game is full');   // host + players + newcomer
    peer.name = String(m.name || '').slice(0, 64) || 'Player';
    // only players who finished joining (said hello) are introduced: the newcomer calls each of them,
    // and one still joining calls the newcomer itself once its own welcome lists it - never both ways
    const joined = [...this.peers.values()].filter((p) => p !== peer && p.open && p.joined);
    peer.control({ t: 'welcome', host: this.me, hostName: this.myName, meta: this.meta, name: this.name,
                   peers: joined.map((p) => ({ id: p.id, name: p.name })) });
    for (const p of joined) p.control({ t: 'peer-joined', id: peer.id, name: peer.name });
    this.names.set(peer.id, peer.name);
    this._peerOpened(peer);
  }
}

class JoinSession extends Session {
  async start(room, hostId) {
    this.hostId = hostId;
    const { tok } = await this.net.server.json({ a: 'sjoin', room, peer: this.me });
    this.jtok = tok;
    const host = new Peer(this, hostId, '');
    this.peers.set(hostId, host);
    host.opened.catch(() => {});
    const offer = await host.offer();
    await this.net.server.json({ a: 'signal', room, peer: this.me, tok, to: hostId, data: JSON.stringify({ t: 'offer', desc: offer }) });
    // poll for the answer (only now: once connected, the server is not used again)
    const until = performance.now() + this.net.connectTimeoutMs;
    let answered = false;
    while (!answered && performance.now() < until && !this.closed) {
      await sleep(this.net.joinPollMs);
      const v = await this.net.server.json({ a: 'poll', room, peer: this.me, tok });
      for (const m of v.msgs) {
        const sig = JSON.parse(m.data);
        if (m.from === hostId && sig.t === 'answer') { await host.accept(sig.desc); answered = true; }
      }
    }
    if (!answered) { host.close(); throw new Error('the host did not answer'); }
    await host.opened;
    const welcome = new Promise((resolve, reject) => { this._welcome = { resolve, reject }; });
    host.control({ t: 'hello', game: this.net.game, proto: PROTOCOL, name: this.myName, sdk: VERSION });
    const w = await Promise.race([welcome, sleep(this.net.connectTimeoutMs).then(() => { throw new Error('no welcome from the host'); })]);
    host.name = w.hostName || 'Host';
    this.meta = w.meta;
    this.gameName = w.name;
    this.names.set(hostId, host.name);
    this._peerOpened(host);
    // the rest of the mesh: we call everyone already in the game, through the host
    this.ready = Promise.allSettled(w.peers.map((p) => this._call(p.id, p.name)));
    return this;
  }

  async _call(id, name) {
    this.names.set(id, name);
    if (this.peers.has(id)) return this.peers.get(id);
    const p = new Peer(this, id, name);
    this.peers.set(id, p);
    const offer = await p.offer();
    this.peers.get(this.hostId)?.control({ t: 'relay', to: id, sig: { t: 'offer', desc: offer } });
    await p.opened;
    this._peerOpened(p);
    return p;
  }

  async _relayed(from, sig) {
    if (sig.t === 'offer' && !this.peers.has(from)) {   // a newer joiner calls us
      const p = new Peer(this, from, this.names.get(from) || '');
      this.peers.set(from, p);
      p.opened.then(() => this._peerOpened(p), () => {});
      try {
        const answer = await p.answer(sig.desc);
        this.peers.get(this.hostId)?.control({ t: 'relay', to: from, sig: { t: 'answer', desc: answer } });
      } catch (e) { this._peerClosed(p, 'failed'); }
    } else if (sig.t === 'answer') {
      const p = this.peers.get(from);
      if (p && !p.open) await p.accept(sig.desc).catch(() => this._peerClosed(p, 'failed'));
    }
  }

  _joinerControl(peer, m) {
    if (peer.id !== this.hostId) return;
    if (m.t === 'welcome') this._welcome?.resolve(m);
    else if (m.t === 'reject') { this._welcome?.reject(new Error('refused by the host: ' + m.reason)); this.close('rejected'); }
    else if (m.t === 'peer-joined') { this.names.set(m.id, m.name); const p = this.peers.get(m.id); if (p) p.name = m.name; }
    else if (m.t === 'peer-left') { const p = this.peers.get(m.id); if (p) this._peerClosed(p, 'left'); }
  }
}

/** The entry point: one per game.
 *  new OkWeb({ server, game, key?, gameName?, iceServers?, log?, relay?, relayOnly?, relayAfterMs?, pingMs? })
 *  relay (default on): players who cannot connect directly talk through the room server instead
 *  pingMs (default off): ping each player this often; session.stats() then has ping (ms), pingAvg/Min/Max
 *  (last 20), pings (last 60) and pingLost */
export class OkWeb {
  constructor(opts) {
    if (!opts || !opts.server) throw new Error('okweb: server (the okplay.php URL) is required');
    if (!/^[0-9a-f]{32}$/.test(opts.game || '')) throw new Error('okweb: game must be 32 lowercase hex digits');
    this.game = opts.game;
    this.gameName = opts.gameName || '';
    this.iceServers = opts.iceServers || DEFAULT_ICE;
    this.announceMs = opts.announceMs || 1000;
    this.joinPollMs = opts.joinPollMs || 300;
    this.gatherMs = opts.gatherMs || 2500;
    this.connectTimeoutMs = opts.connectTimeoutMs || 20000;
    // when a direct connection can't be made: the room server's relay (rsend/rpoll), after relayAfterMs.
    // relayOnly: never try directly (networks where WebRTC never gets through)
    this.relay = opts.relay !== false;
    this.relayOnly = !!opts.relayOnly && this.relay;
    this.relayAfterMs = opts.relayAfterMs || 8000;
    this.pingMs = opts.pingMs || 0;   // > 0: measure each player's round trip this often (stats(): ping, pingAvg...)
    this._testNoDirect = !!opts._testNoDirect;   // tests: descriptions without routes, so no direct path opens
    this.logger = opts.log || null;
    this.server = new Server(opts.server, opts.key || DEFAULT_KEY);
  }

  _log(...a) { if (this.logger) this.logger(...a); }

  /** the games of this game id that are open now */
  async list() {
    const { status, text } = await this.server.post({ a: 'list', app: this.game });
    if (!text.startsWith('ok')) throw new Error(`room server: HTTP ${status}: ${text.trim().slice(0, 120)}`);
    const out = [];
    for (const line of text.split('\n')) {
      const f = line.trim().split(' ');
      if (f[0] !== 'room' || f.length < 8 || Number(f[7]) !== WEB_WIRE) continue;   // room id peer age cands enum name wire
      let info = {};
      try { info = unb64json(f[5]); } catch { continue; }
      if (info.v !== PROTOCOL) continue;
      out.push({ room: f[1], host: f[2], age: Number(f[3]), name: info.name ?? decodeURIComponent(f[6]),
                 players: info.players, maxPlayers: info.max, hostName: info.host, meta: info.meta,
                 joinable: info.open !== false });
    }
    return out;
  }

  /** open a game: { name, maxPlayers, meta, playerName, locked? } (locked: the room list shows a lock) */
  async host(opts = {}) {
    // opts.room (8 hex) / opts.peer (16 hex): ids chosen by the caller, e.g. a DirectPlay provider
    // that must name the session before the server has answered
    const room = /^[0-9a-f]{8}$/.test(opts.room || '') ? opts.room : hex(4);
    const peer = /^[0-9a-f]{16}$/.test(opts.peer || '') ? opts.peer : hex(8);
    const s = new HostSession(this, true, room, peer, opts.playerName || 'Host');
    return s.start(opts);
  }

  /** join a game from list(): join(room, { playerName }) or join(roomObject, ...) */
  async join(room, opts = {}) {
    let r = typeof room === 'object' ? room : null;
    if (!r) r = (await this.list()).find((x) => x.room === room);
    if (!r) throw new Error('okweb: no such game');
    const s = new JoinSession(this, false, r.room, hex(8), opts.playerName || 'Player');
    s.keepMesh = !!opts.keepMesh;
    try { return await s.start(r.room, r.host); } catch (e) { await s.close('failed'); throw e; }
  }
}
