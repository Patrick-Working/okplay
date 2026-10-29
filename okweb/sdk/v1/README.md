# okweb.js v1

Browser-to-browser multiplayer for web games, through an okplay.php room server (oldkid.net's,
or your own). Players find each other through the server; game data goes directly between
browsers over WebRTC. One file, no dependencies, an ES module.

```js
import { OkWeb } from 'https://oldkid.net/aoe2net/sdk/v1/okweb.js';

const net = new OkWeb({
  server: 'https://oldkid.net/aoe2net/okplay.php',
  game: 'a1b2c3d4e5f60718293a4b5c6d7e8f90',   // 32 hex digits, your game's own (players only see games with the same id)
  gameName: 'My Game',                       // shown on the server's web page
  // iceServers: [...]                       // optional; default: public STUN only (no TURN - the room server relays when needed)
});

const rooms = await net.list();   // [{ room, name, players, maxPlayers, hostName, meta, age }]
const s = await net.host({ name: 'My room', maxPlayers: 4, playerName: 'Ann', meta: { map: 'Arabia' } });
// or: const s = await net.join(rooms[0], { playerName: 'Bob' });

s.on('peer-joined', (p) => console.log(p.name, 'joined'));      // p = { id, name }
s.on('peer-left', (p, why) => console.log(p.name, 'left', why));
s.on('peer-failed', (p, err) => console.log('no connection to', p.name));
s.on('message', (from, bytes, reliable) => { /* bytes: Uint8Array */ });
s.on('closed', (why) => { /* 'host-left', 'left', 'rejected', ... */ });

s.send(null, bytes);                          // everyone, reliable and in order
s.send(peerId, bytes, { reliable: false });   // one player, may be lost or reordered (fast)
s.players;  s.me;  s.isHost;  s.stats();
await s.close();
```

- `host({ ..., locked: true })` (or `s.setLocked(true)` later) shows a lock on the room list - for a
  game that checks a password itself (okweb never sees passwords).
- `send` takes a `Uint8Array`, an `ArrayBuffer` or a string, at most 64 KB. It returns `false`
  when that player isn't connected or isn't reading (more than 8 MB waiting).
- Every player connects to every other player (a full mesh). The host decides who gets in
  (game id, SDK protocol version, room full). When the host leaves, the game ends for everyone.
- `join` resolves once you are connected to the host. `s.ready` resolves when your connections to
  the players already in the game have opened or failed.
- Some network pairs can't connect directly (two strict NATs). After `relayAfterMs` (8 s) without a
  direct connection, their messages go through the room server's relay instead (`relay: false` turns
  that off and they get `peer-failed`; `relayOnly: true` never tries directly). A TURN server in
  `iceServers` is the faster alternative for many relayed players.
- Ping: `pingMs: 1000` measures each player's round trip (a tiny message on the reliable lane, that
  often). `s.stats()[id]` then has `ping` (ms, the last), `pingAvg`/`pingMin`/`pingMax` (last 20),
  `pings` (last 60), `pingLost` and `route` (`'relay'`, or the direct route's kinds such as
  `'srflx-srflx'`). Players on okweb before 1.1.2 don't answer pings; their ping stays `null`.
- Background tabs: browsers slow a hidden page's timers (to about once a second, later about once a
  minute) and stop its animation frames. okweb.js's own timers run from a small worker while the
  page is hidden, so a hosting tab keeps announcing and stays on the list. For a game whose main
  loop runs on `requestAnimationFrame` (Emscripten), call `keepAlive()` once before it starts:
  while hidden, its frames come from the same worker clock (20 a second; `keepAlive({ fps, when })`).
  `clock.setTimeout / setInterval / clear / sleep` are exported for a game's own network timers.

## okweb-sp.js: DirectPlay games (classic-netplay)

A game built with our DirectPlay for the browser (classic-netplay, `okweb/dplay/export_to_game.sh`)
gets "Oldkid.net Online Play" as its connection by installing the provider before its multiplayer starts:

```js
import { installOkwebProvider } from 'https://oldkid.net/aoe2net/sdk/v1/okweb-sp.js?v=1.2.0';
installOkwebProvider(Module, { server: 'https://oldkid.net/aoe2net/okplay.php', playerName: 'Ann', gameName: 'My Game' });
```

- Game passwords (1.2.0), as in the Windows drop-in: hosting asks for an optional password, joining
  a game that has one asks for it (a box in the drop-in's style; `brand` sets its title, default
  "Classic Netplay"; a wrong password says so). The typed text never leaves the page: DirectPlay gets
  `okp1-` + a salted SHA-256 of it (the drop-in's value), and the host checks it on every join.
  `usePassword: 'off'` never asks when hosting (joining a password game still asks); a player's
  "Disable Hosting Password" tick is remembered in the browser (`Module.okwebSP.resetPasswordSetting()`).
  `password: 'text'` answers every question without a box (tests; `''` = open games).
  `askPassword(kind)` replaces the box with the game's own: `'host'` resolves to the password (`''` =
  none), `'join'` to the password or `null` (Cancel).
- `Module.okwebSP.saveReport()` / `sendReport()`: the connection log a player can save or send.

Protocol and design: `docs/WEB_NETPLAY.md`. Demo: `../../demo/index.html`. Test: `../../tests/okweb_test.html`.

Versions: `v1/` stays compatible. An incompatible change becomes `v2/`, and `v1/` stays where it is.
