# Classic Netplay for the web - what it is and how a game uses it

A plain-language guide for anyone who wants to put an old DirectPlay game (or a new web game) online
in the browser. The technical design is in `docs/WEB_NETPLAY.md`.

## The idea in one paragraph

Old Windows games like Age of Empires II and Midtown Madness 2 did multiplayer through Microsoft's
**DirectPlay**. We rebuilt DirectPlay from Wine's open-source version, tested it against Windows
until every check matched, and gave it a way to find players over the internet (oldkid.net). That's
the Windows drop-in people already play with. The web version is the same thing in a browser: the
game (compiled to WebAssembly) calls DirectPlay exactly as it always did, our DirectPlay answers, and
the players' browsers talk to each other directly.

## The pieces

```
  ┌──────────────────────────── a player's browser ────────────────────────────┐
  │                                                                             │
  │  the game (WebAssembly)        "host a game", "join", "send this to Bob"     │
  │        │  same DirectPlay calls as in 1999                                   │
  │        ▼                                                                    │
  │  our DirectPlay (WebAssembly)  players, game list, messages, host hand-over │
  │        │  "deliver these bytes to that machine"                              │
  │        ▼                                                                    │
  │  okweb.js (the SDK)            finds games, connects browsers, carries bytes │
  │        │                                                                    │
  └────────┼────────────────────────────────────────────────────────────────────┘
           │ 1. "who is hosting?" / "I want to join"          ┌─────────────────┐
           ├────────────────────────────────────────────────► │ oldkid.net      │
           │    (small web requests, only while finding and    │ room server     │
           │     connecting - never game data)                 │ (okplay.php)    │
           │                                                   └─────────────────┘
           │ 2. the game itself: browser to browser (WebRTC)
           └──────────────────────────────────────────────────► the other players' browsers
                 (if two networks can't reach each other: through a relay, optional)
```

| Piece | What it does | Where | State |
|---|---|---|---|
| **Room server** (`okplay.php`) | Keeps the list of open games and passes the first "hello" between a joiner and the host. One PHP file, no database. The same server the Windows drop-in uses; browser games are listed separately. | oldkid.net/aoe2net/okplay.php (anyone can run their own) | live |
| **okweb.js** (the SDK) | One JavaScript file. Lists games, hosts, joins, and sends bytes between browsers: a reliable lane (arrives, in order) and a fast lane (may drop). | oldkid.net/aoe2net/sdk/v1/okweb.js | live, tested |
| **Our DirectPlay for the web** | The tested DirectPlay library compiled to WebAssembly, with okweb.js underneath instead of the Windows network code. Games that already speak DirectPlay need no network code of their own. | being built (milestone M6) | in progress |
| **Relay (TURN server)** | For the few network pairs that can't connect directly (two strict routers, some mobile carriers): it passes their game data along. | not set up yet; any standard TURN server works | optional, a decision |

## How another project uses it

### A) A web game written from scratch: use okweb.js directly

```js
import { OkWeb } from 'https://oldkid.net/aoe2net/sdk/v1/okweb.js';
const net = new OkWeb({ server: 'https://oldkid.net/aoe2net/okplay.php',
                        game: 'a1b2c3d4e5f60718293a4b5c6d7e8f90' });   // your game's own 32-hex id
const s = await net.host({ name: 'My room', maxPlayers: 4, playerName: 'Ann' });   // or net.join(...)
s.on('message', (from, bytes) => { /* ... */ });
s.send(null, bytes);   // to everyone
```

That's all: no server of your own, no network code. Details in `sdk/v1/README.md`, a working chat
example in `demo/index.html`.

### B) An old DirectPlay game compiled to WebAssembly (decompilations, ports)

The game keeps calling DirectPlay. The project links our DirectPlay library instead of a stub and
lists "Oldkid.net Online Play" as a connection type. Nothing in the game's own logic changes. Our
DirectPlay in the browser is working, and rogan2 (the AoE2 decompilation) is the first game using
it: two browser tabs host, join and play a random map game together through the game's own
Multiplayer screens. The steps:

1. **Copy the library in:** `okweb/dplay/export_to_game.sh <game dir>` writes a versioned copy to
   `<game>/third_party/classic-netplay/` (sources, headers, a CMakeLists.txt, the two JS files, LGPL licence).
2. **Build it with the game:** link the `classic_netplay` library, add `-sASYNCIFY` (DirectPlay waits for
   the host's replies while joining) and `-sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=['$UTF8ToString']`.
3. **Connect the game's DirectPlay calls:** if the game calls Windows' DirectPlay directly (C COM), it just
   works - the library *is* DirectPlay. If it has its own stand-in headers (rogan2 does), a small file
   forwards each method to the plain C calls in `include/okweb_dplay.h` (rogan2: `age2_x1/okweb_netplay.cpp`).
4. **The page:** before the game starts multiplayer,
   `import('./okweb-sp.js').then(m => m.installOkwebProvider(Module, { server: 'https://oldkid.net/aoe2net/okplay.php' }))`.
5. The game id is the game's own DirectPlay application GUID, so every copy of that game finds the others
   automatically. Windows players don't see browser games (and vice versa) for now.
6. **Background tabs are handled:** a browser stops drawing a tab that isn't in front and slows its
   timers, which would freeze that player for everyone else. While an online game is open, okweb-sp.js
   keeps the page's main loop going (about 20 frames a second, from a small background worker), and
   our DirectPlay's own timers run from the same worker. Pass `keepAlive: false` to turn that off.

### C) A different server

Copy `okplay.php` to any PHP host and pass its URL as `server`. Games only see games on the same server.

## A relay, simply put

Most players connect directly. Some home routers and mobile networks block that, and then two
particular players can't reach each other. Then their game data goes through a server both can reach:

- **Built in, automatic:** if a direct connection doesn't open within 8 seconds (or fails), those two
  players' messages go through the room server itself (`okplay.php`, actions `rsend`/`rpoll`). No
  account, works wherever the page loads; slower (about 0.1-0.4 s more) and about 6 small requests a
  second per relayed player on the web host. A game page can force it with `relayOnly: true`
  (rogan2 and the demo: `?relayonly`). Tested 2026-10-22: a whole rogan2 game over it.
- **A TURN server** (a relay service) is the faster, heavier-duty option if many players need a relay;
  using one is a single setting in the game:

```js
new OkWeb({ ..., iceServers: [
  { urls: 'stun:stun.cloudflare.com:3478' },
  { urls: 'turn:relay.example.com:3478', username: '...', credential: '...' },   // your relay
] });
```

## What stays the same, what's different from Windows

- **Same:** the room server, the game list, how DirectPlay behaves for the game.
- **Different:** browsers can't use the Windows network path (UDP), so browser players connect with
  WebRTC instead. Windows players and browser players don't see each other's games yet; mixing them
  is possible later because both run the same DirectPlay.

## Versions

`sdk/v1/` never changes in a way that breaks a game. A breaking change becomes `sdk/v2/`, and `v1`
stays online for games built against it.

The reusable parts of a game's browser port (page wiring, Asyncify-safe waits, on-demand game files,
AVI/Indeo movies) are in `port-kit/` (`port-kit/README.md`).
