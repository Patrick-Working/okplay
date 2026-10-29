# Web netplay: browser-to-browser multiplayer through oldkid.net

Goal: a browser build of a DirectPlay game (a decompiled game's web port such as the AoE2
decompilation `rogan2`, or any web app) can find other players through the same oldkid.net
server as today's drop-in and play peer to peer over WebRTC. The Windows drop-in keeps working
exactly as it does.

This was the punch list (2026-10-21), adapted to what the code already has. Rules that carry
over unchanged: audit before building; the server only introduces players, never carries game
traffic; browser-to-browser first; Windows interop later and only if needed; versioned SDK; no
always-running service unless PHP cannot do the job.

## 1. Audit (M1) - what exists and what the browser can reuse

### Layers of the Windows drop-in

| Layer | Files | What it does | Browser? |
|---|---|---|---|
| DirectPlay API (IDirectPlay4/IDirectPlayLobby3) | winedplay/port/dlls/dplayx (Wine-derived, ~16.9k lines) | sessions, players, groups, system messages, host migration, the DirectPlay wire protocol; S1/S2 100% vs Windows | **yes, compiled to wasm** (section 3.3) - portable C; the Windows parts are a short list (below) |
| Service provider interface (SPInit callbacks) | dplaysp.c / okplay sp.c | EnumSessions, Open, Send, Reply, CreatePlayer, DeletePlayer, CloseEx, ShutdownEx, GetCaps - DirectPlay hands the SP whole messages plus an SP header | **this is the seam**: a JS provider ("okweb") replaces okplay's UDP provider |
| Transport | okplay net.c | one UDP socket, peers by 64-bit id, hole punching with probes, per-peer tokens, STUN, a reliable in-order lane with acks/resends (40 ms, then ~2xRTT growing 1.5x to 1 s), fragmentation at 1200 bytes, 64 KB message cap | **no** - browsers have no UDP; WebRTC data channels give the same two lanes (reliable ordered = SCTP, unreliable = maxRetransmits 0) and do ICE/STUN themselves |
| Room server client | okplay rdv.c | form POSTs to okplay.php: announce ~1/s while hosting, list ~1/s while browsing, knock to ask a host to punch | **same protocol from JS**, plus the signaling actions below |
| Room server | okplay/server/okplay.php (one file, flat store, flock) | rooms with a TTL of 15 s, knocks, per-IP limits, latest-version line, daily stats, reports | **extended, backward compatible** (section 2) |

Windows-only pieces in dplayx (what the wasm build must stub or replace): threads
(`CreateThread` in dplay.c, dplayx_messages.c, lifecycle.c - the message and keepalive threads),
critical sections and events (6 files), the registry (provider and lobby-app lookup: dplay.c,
dplayx_main.c, dplobby.c), shared memory for the lobby (`CreateFileMapping`, dplayx_global.c),
COM class registration and `LoadLibrary` of providers, builtin.c (drop-in UI boxes, video hook).
None of this is the protocol itself. The plan in docs/FORK_DESIGN.md already chose this route
(G8 "DirectPlay API stays the network boundary", row "browser netplay": "the fork's own DirectPlay
with a WebRTC data-channel service provider", D16 "all fork builds interoperate").

### okplay.php today (protocol facts the browser side relies on)

- Form-encoded POST (GET also accepted), plain-text line replies; `k=` is a public app key.
- `announce room peer tok cands enum name players maxp flags ver wire app game ...`: the host's
  random `tok` from its first announce owns the room; rooms vanish 15 s after the last announce.
- `list [app=]` returns `room <id> <peer> <age> <cands> <enum> <name> <wire> <build> <exe>` lines;
  `fmt=json` is the public page view (no addresses, CORS open).
- `knock room peer cands`: queued for the host, delivered in its next announce reply (20 s TTL).
- Authentication: only the room token (host). Joiners are not authenticated - a knock only asks
  the host to send probes, and okplay's per-peer tokens protect the game traffic itself.
- **Windows clients already ignore rooms of another wire version** (rdv.c parse_list skips
  `wire != OKP_WIRE_VER`), and filter by application GUID. Browser rooms announce `wire=100`, so a
  Windows drop-in never lists them and a browser never sees Windows rooms - no format change needed.

## 2. Signaling (M2/M3) - okplay.php actions, protocol "sig1"

PHP has no persistent connections on ordinary hosting, so signaling is a mailbox per room that the
two sides poll - only while a connection is being set up. Once a joiner's data channel to the host
is open, everything else (introductions to the other players, their offers and answers) travels
over the data channels, relayed by the host. The server sees one offer/answer exchange per joiner.

Browser host: the normal `announce` with `wire=100`, `cands=-`, `ver=<sdk version>`,
`app=<32 hex game id>`, `enum=<base64 of the session JSON>` (name, players, max, the game's own
meta; at most 1.5 KB). New actions, all answering JSON with `Access-Control-Allow-Origin: *`:

| action | fields | reply | rules |
|---|---|---|---|
| `sjoin` | room peer | `{"ok":true,"tok":"<16 hex>"}` | registers a joiner in a web room (wire 100); at most 32 waiting; a joiner token dies 60 s after its last poll |
| `signal` | room peer tok to data | `{"ok":true}` | `peer`+`tok` must be the host (room token) or a registered joiner; `to` must be the host or a registered joiner; `data` <= 16 KB (the SDK sends SDP and ICE candidates as JSON); at most 64 messages waiting per recipient |
| `poll` | room peer tok | `{"ok":true,"msgs":[{"from":"<peer>","data":"..."}]}` | returns and removes the caller's messages; a gone room answers `{"ok":false,"error":"no such room"}` |

Errors are `{"ok":false,"error":"..."}` with HTTP 4xx. Rate: the host polls once a second (with its
announce), a joiner every 300 ms for at most 20 s. The mailbox lives in the room record, so it
expires with the room. Impersonation: nobody can post as the host without the room token, or as a
joiner without that joiner's token; a joiner can only write to the host and the host to its joiners.

## 3. SDK (M4/M5)

### 3.1 okweb.js v1 - game-agnostic JS module (no wasm)

`sdk/v1/okweb.js`, an ES module with no dependencies, served from `https://oldkid.net/aoe2net/sdk/v1/`.

```js
import { OkWeb } from 'https://oldkid.net/aoe2net/sdk/v1/okweb.js';
const net = new OkWeb({ server, key, game /* 32 hex */, iceServers /* optional */ });
const rooms = await net.list();                       // [{ room, name, players, maxPlayers, age, meta }]
const s = await net.host({ name, maxPlayers, meta, playerName });
const s = await net.join(room, { playerName });
s.on('peer-joined', p => ...);  s.on('peer-left', p => ...);
s.on('message', (fromPeer, bytes /* Uint8Array */, reliable) => ...);
s.on('closed', reason => ...);
s.send(toPeer /* or null = everyone */, bytes, { reliable: true });
s.peers; s.me; s.isHost; s.stats(); s.close();
```

- Full mesh (DirectPlay's peer-to-peer model): every pair has one RTCPeerConnection with two
  pre-negotiated channels, id 0 `rel` (ordered, reliable) and id 1 `unrel` (unordered,
  maxRetransmits 0) - okplay's two lanes.
- Frames: first byte 0 = application bytes, 1 = SDK control JSON (`hello`, `welcome`, `reject`,
  `peer-joined`, `peer-left`, `relay`, `bye`). Messages up to 64 KB (okplay's cap); bigger is an error.
- Membership: the host is the authority. A joiner's `hello` carries game id, SDK protocol version
  and name; the host answers `welcome` (peer list, session meta) or `reject` (full, wrong game or
  version). The joiner then connects to every other peer through `relay` messages via the host.
- Version 1 has no host migration: when the host leaves, the session ends (`closed: host-left`).
  DirectPlay games get migration from dplayx itself (3.3), which only needs the mesh.
- ICE: STUN only by default (Cloudflare, Google, as okplay); TURN can be passed in `iceServers`.
  A pair that finds no path reports `peer-failed` - no relay through oldkid.net.

### 3.2 Versioning and hosting

`sdk/v1/` never changes incompatibly; fixes go out as `v1` file updates only if the wire and API
stay the same, otherwise `v2/`. `.htaccess` there serves `.js` as `text/javascript` with
`Access-Control-Allow-Origin: *` (cross-origin module imports need it).

### 3.3 DirectPlay in the browser (M6) - the adapter for decompiled games

What rogan2's browser build does today (`C:\rogan2`, branch `feat/browser-port`, a366a48 of
2026-10-22; read only, never built here): Emscripten + SDL2, the game on the browser's main loop,
**no Asyncify and no threads**. DirectPlay is `include/fake/dplay.h`: C++ classes whose methods all
succeed and do nothing, so multiplayer is stubbed out. The game's networking (com_hand.cpp, com_sess.cpp,
com_loby.cpp, com_serv.cpp, ~13k lines) uses about 25 methods: DirectPlayEnumerate, DirectPlayCreate,
InitializeConnection, EnumSessions (DPENUMSESSIONS_AVAILABLE, with or without ASYNC; the join screen
erases its list and enumerates again on every refresh), Open (CREATE / JOIN - DPERR_CONNECTING is
**not** a retry: the join screen shows "already connecting" and gives up, so Open must answer at once), CreatePlayer,
DestroyPlayer, EnumPlayers, GetPlayerName, Send (flags 0 for game traffic: AoE2 resends its own
commands), Receive (PEEK for the size, then ALL), GetMessageCount, CancelMessage, GetSessionDesc,
EnableNewPlayers, GetCaps/GetPlayerCaps, Close, and the lobby's Get/SetConnectionSettings,
WaitForConnectionSettings, Connect, Send/ReceiveLobbyMessage, CreateAddress. It handles the system
messages DPSYS_CREATEPLAYERORGROUP, DESTROYPLAYERORGROUP, HOST, SESSIONLOST and a few more.

**Decision (2026-10-21, after review with the user): reuse our dplayx.** The paragraphs below were the
first reaction to the blocking waits (a new thin layer); it would duplicate a DirectPlay we already
have tested to 100% and other games would need ever more of it. The route is our dplayx compiled with
Emscripten:

- **The waits:** dplayx blocks in one place, `DP_MSG_ExpectReply` (dplayx_messages.c), used three
  times while joining (REQUESTPLAYERID, the ADDFORWARD/SUPERENUMPLAYERS exchange, a 5 s ping), plus
  synchronous EnumSessions. In the browser that wait becomes an Emscripten Asyncify import
  (`-sASYNCIFY` limited with `ASYNCIFY_ONLY` to the functions on the stack during a join; `ASYNCIFY_ADVISE`
  lists them), later JSPI where browsers have it. dplayx's code and wire messages stay unchanged.
- **Mini-Windows:** the plain dplayx imports 61 Windows functions (plus 30 C runtime ones Emscripten
  has): critical sections (no-ops, one thread), events and semaphores (flags the wait checks), threads
  (keepalive, async session search, lobby: timers pumped from the game's calls), registry (a fixed
  table naming the okweb provider), file mappings (lobby shared memory: ordinary memory), LoadLibrary/
  GetProcAddress (a table), string, time and GUID helpers. MinGW's headers do not compile for wasm32
  (x86 intrinsics, a different size_t), so the shim brings its own Windows type header as well.
- **Provider:** a service provider in C + JS (the role of okplay's sp.c) that sends through okweb.js
  and hands incoming messages to `IDirectPlaySP_HandleMessage` from the same pump.
- **rogan2 glue:** its fake IDirectPlay4/Lobby3A C++ classes have a different method order from COM,
  so a small C++ subclass forwards each method to our dplayx object; its factories (DirectPlayEnumerate,
  DirectPlayCreate, DirectPlayLobbyCreate, CoCreateInstance) return ours behind one define.
- **Hidden tabs (2026-10-21):** a tab that isn't in front gets no animation frames (an Emscripten main
  loop stops) and its timers slow to about 1/s, later about 1/min. A hidden host stopped answering
  (pings of 2-3 s, a frozen lobby) and would expire from the room list after 15 s. Fixed in three places, all
  from one clock in okweb.js whose timers run from a dedicated worker while the page is hidden (worker
  timers aren't slowed): okweb.js's announce/poll/connect timers; winweb's 30 ms pump and its waits
  (`Module.okwebClock`, falling back to Emscripten's timers without it); and `keepAlive()`, which
  makes the page's requestAnimationFrame fall back to that clock (20 frames/s) while an online game is
  open, so the game itself keeps running. No game code changes. Checked with the host in a background
  tab, in its own process: joined, pings ~50-70 ms (before: 2-3 s). Seen along the way: Electron's hidden pane
  reports `visible` yet draws nothing; keepAlive doesn't rely on the visibility flag, so it covers that too.

Superseded first reaction, kept for the record - a thin layer instead of dplayx:
dplayx waits for replies (Open, synchronous EnumSessions, player-id requests), which the browser's
main thread cannot do without Asyncify or JSPI, and browser-to-browser needs no DirectPlay wire
compatibility. So M6 is **okweb-dplay**: a non-blocking C++ IDirectPlay4A / IDirectPlayLobby3A with
the same shapes as rogan2's fake header (a drop-in replacement behind one define), on okweb.js
through Emscripten's JS bridge:

```
rogan2 (wasm)  --IDirectPlay4A-->  okweb-dplay (C++)  --EM_JS-->  okweb.js  --WebRTC-->  other browsers
```

- EnumSessions answers at once from the last `list` and starts a new one. Open(JOIN) answers DP_OK at
  once (DPERR_NONEWPLAYERS if the list says the game is full or started) and connects in the
  background: the game's sends wait in a queue until the host's welcome, and a join that fails arrives
  as DPSYS_SESSIONLOST, which the game already handles. Open(CREATE) is `host`, also answered at once.
- Players: the host hands out DPIDs; CreatePlayer/DestroyPlayer are relayed as small control
  messages, and each machine queues DPSYS_CREATEPLAYERORGROUP / DESTROYPLAYERORGROUP / HOST /
  SESSIONLOST for Receive, as DirectPlay does.
- Send maps DPSEND_GUARANTEED to the reliable lane and everything else to the unreliable one
  (okplay's default); Receive and GetMessageCount read the queue the JS callbacks fill.
- The game's simulation and its own command resends stay as they are.

Our full dplayx stays the route for Windows interop (section 4), and for any port that builds with
Asyncify/JSPI: it then needs the JS service provider of the first draft.

### 3.4 Working with a game's repository (rogan2 as the example)

- A game port is someone else's repository: our changes there go on a feature branch, in small commits
  in that project's own style; its owner reviews and merges.
- What lives where: the game gets only the integration (a CMake option, the C++ glue, the factory
  swap; `okweb/integrations/rogan2/README.md`). The DirectPlay library stays here (LGPL, from Wine)
  and goes into the game as a versioned copy under `third_party/classic-netplay/` with its licence
  and a VERSION file, refreshed by `okweb/dplay/export_to_game.sh` from this repository.

## 4. Windows interop (later, only if needed)

A browser cannot open a UDP socket, and okplay's datagrams are not WebRTC. Mixing Windows and
browser players needs a WebRTC endpoint on the Windows side (e.g. libdatachannel in the drop-in -
not possible on Windows 98) or a relay. Decision deferred until browser-to-browser works; the SDK's
frames are already transport-neutral (bytes + reliable flag), so a native okweb peer is possible later.

## 5. Milestones

| | Deliverable | Done when | State |
|---|---|---|---|
| M1 | audit | this document, sections 1-2 | done 2026-10-21 |
| M2 | API + signaling spec | sections 2-3 | done 2026-10-21 |
| M3 | PHP signaling | `sjoin/signal/poll` in okplay.php and rdv_standin.py, PHP tests | done 2026-10-21: okplay/tests/server_php_test.py (PHP 7.4 and 8.4) |
| M4 | WebRTC prototype | two browsers exchange bytes, both lanes | done 2026-10-21 |
| M5 | okweb.js v1 | versioned module, demo page, automated browser test (3-way mesh) | done 2026-10-21: okweb/tests/okweb_test.html 22/22 against the stand-in (cross-origin) and against okplay.php on PHP 8.4; the demo chats between two tabs |
| M6 | dplayx.wasm + okweb provider + rogan2 glue | its multiplayer screens host and join between two tabs | **our DirectPlay plays in the browser** (2026-10-21, 46c4d12): okweb/dplay/test/dpweb_test - DirectPlayCreate on the provider, Open(CREATE), EnumSessions(ASYNC), Open(JOIN) (3 s), CreatePlayer, EnumPlayers, 10/10 guaranteed pings answered, the host sees the joiner arrive and leave. rogan2 (feat/okweb-netplay): its own Multiplayer screens list the connection, host, join; both lobbies show both players with pings (done 2026-10-21) |
| M7 | game validation | a rogan2 match between two browsers, then two internet connections | **first game 2026-10-21**: two rogan2 tabs (separate processes, one in the background) hosted, joined, readied and started a random map game; both reach the game screen and keep exchanging turns (~3 messages/s each way). Needed rogan2 fix 4158247 (VC98 RAND_MAX: every random map hung in land placement in the browser build, with or without netplay). Driven with no mouse: age2_x1/wasm_console.cpp (window.rogan2, ?autoplay=host/join, ?keepalive). **Two homes 2026-10-22**: first attempt "no network path" -> the relay (lobby ping ~850 ms; ~500 after 1.1.1); later games connected **directly** (srflx-srflx, okweb ping ~300 ms to the Asia-Pacific player; the game's Ping column ~500). The ping ticker (1.1.2) shows route and round trip in the game |
| M8 | deployment | okplay.php + sdk/v1 on oldkid.net, integration guide | **live 2026-10-22**: oldkid.net/aoe2net/ (okplay.php with relay, sdk/v1 1.1.2, demo/). Open: the web host's bot protection has to let Windows tools reach okplay.php; a TURN service only if relayed players need lower ping |

**First test between two homes (2026-10-22)** and what came of it:
- The browser join failed with "no network path" (ICE found routes on both sides but no pair opened).
  So okweb.js now falls back to a **relay through the room server**: `okplay.php` `rsend`/`rpoll`, a
  mailbox per player per game in `okplay_relay/<room>.php` (outside the room store; `rpoll` waits up to
  1.5 s), a key per mailbox, size caps, idle files removed after 5 min. A peer whose direct connection
  hasn't opened 8 s after the descriptions are exchanged (or fails) switches; the other side follows on
  its first relayed message; an empty description means relay only (`relayOnly`). Tests: PHP suite
  (7.4/8.4), okweb_test 27/27 (forced no-direct and relay-only cases), and a whole rogan2 game over the
  relay (lobby ping 255 ms, ~2.5 messages/s each way in game, ~5.7 requests/s per player on the server).
- Between the two homes the relay worked; the lobby showed ~851 ms ping (a measured relay round trip from
  here: median 502 ms). **1.1.1 trims the relay:** a message goes out at once (no 120 ms batching; what
  queues while a send is in flight goes with the next one), each player has his own mailbox file
  (`okplay_relay/<room>-<peer>.php`), a waiting `rpoll` checks that file's size every 20 ms (was: lock
  and read the room's file every 100 ms), and the wire is compact (`f=to.data*to.data`, base64url;
  `rpoll c=1` answers the same way; the old JSON `frames` still works). Locally, PHP 8.4 behind a
  6-process pool: relay round trip (two hops, server only) 110 ms -> 33 ms median; live from here
  502 -> 416 ms (okplay/tests/relay_bench.py; php_pool.py runs PHP locally with parallel requests).
- The friend's **Windows** tools couldn't reach the room server at all: the web host's anti-bot layer
  (Imunify360 on LiteSpeed) answered his address with an HTML "One moment, please..." page, which a
  browser passes and a program can't. Not in our PHP. Fix on the host side: whitelist the address and
  exclude `/aoe2net/okplay.php` from bot protection. Connection Check now names such pages (title,
  start, what they mean) - Imunify, Cloudflare, captchas, filters, Wi-Fi sign-in pages.

Tested on one PC (all cases, automated) and between two homes (direct srflx-srflx and the relay, M7).
Field note (2026-10-22): the Asia-Pacific player's own connection gave no direct path (relay only); on a VPN
it connected directly and with a lower ping - the VPN's exit has a friendlier NAT and better routes than
his ISP. Worth suggesting to a player whose games always say "relay". Known limits of v1: no host migration in okweb.js itself, no TURN unless a game passes one; a room
stays listed up to 15 s after its page closes (a reload shows it twice until then). Open: whether TURN is wanted for
players behind strict NATs on both sides (it relays their game traffic, so it costs bandwidth).
