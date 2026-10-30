# oknative - okplay for native games (ENet)

Online play for games that aren't DirectPlay games: a native (C/C++) game, or a port of one, that
moves its packets with [ENet](https://github.com/lsalzman/enet). oknative gives it the same things
the DirectPlay drop-in gets from okplay:

- **a room list** on the okplay room server (`okplay/server/okplay.php`, live at
  <https://oldkid.net/aoe2net/okplay.php>), kept separate from every other game by an app id and
  a wire number;
- **a direct connection whenever one is possible**: LAN, then UDP hole punching on the game's
  own ENet socket, then a UPnP port forward the host asks its router for;
- **the okplay relay** (the server's `rsend`/`rpoll` mailboxes) when nothing direct gets through;
- on Android, **staying connected while the app is in the background**.

The first game to use it is the UFO 50 Android port (UFO 50 with
[PFO 50](https://github.com/Blaizer/PFO-50) and MultiMadnessMod). The code here is lifted from that
port, so it still uses that port's names (`fakesteam`, `PFO LAN:` log lines); `example-pfo50/` is
how it is wired in there.

## How a connection is made

1. **LAN.** A UDP broadcast finds hosts on the same network (no server needed).
2. **Direct.** The host announces its room about once a second with its candidate addresses: its
   interface addresses, the public address a STUN server reports for the ENet socket, and the UPnP
   address if it has one. A joiner knocks (every 3 s). The server passes knocks to the host, and
   both sides send to each other's candidates at once. That opens the path through most home
   routers and phone carriers. The first candidate to answer wins.
3. **UPnP (host only).** When a lobby opens, the host asks its router (if UPnP is enabled there) to
   forward the game's UDP port, and advertises that address first. This helps joiners behind strict
   NATs reach the host. It does nothing for a joiner's own router, since joiners connect outward.
4. **Relay.** If nothing direct answers within 2.5 s, the joiner also tries the relay. Whichever
   path connects first is used.

While a lobby is open, a STUN request every 20 s keeps the host's router mapping alive. Without it,
a host who sits waiting can end up advertising a public address its router has already forgotten.

### The relay is slower; you will usually be direct

Each relayed packet makes an HTTPS round trip through the room server: the sender posts it with
`rsend`, and the receiver long-polls `rpoll` for it. That adds roughly 100-200 ms each way, which is
fine for a lobby or a turn-based game but noticeable in an action game. UFO 50's lockstep input
delay went from 6 frames direct to 12 frames over the relay. It is a fallback. Most connections
(home Wi-Fi, phone carriers, the mobile-data-to-home case we tested) connect **directly**, and the
relay is there for the rest: double NAT, symmetric NAT, emulators, strict firewalls.

UFO 50 shows which path you got in a small tag: **LAN** (green), **DIRECT** (blue), **RELAY**
(orange). A host shows the slowest link among its players. `g_ConnKind` in `src/netplus.inc` is
where that comes from.

## Files

| file | what |
|---|---|
| `src/okplay_client.inc` | the room server client: announce / list / knock / close, STUN on the ENet socket, candidate lists, keep-alive |
| `src/netplus.inc` | the relay (rsend/rpoll threads + ENet socket hooks), UPnP (a POSIX port of `okplay/src/upnp.c`), the background pump, connection status |
| `enet/enet-1.3.18-relay-hooks.patch` | 17 lines added to ENet 1.3.18's `unix.c`: two optional hooks so relayed peers can be ordinary ENet peers |
| `android/OkplayHttp.java` | the HTTP POST the native code calls through JNI (with a retry for stale kept-alive connections) |
| `android/PFOOnlineService.java` | a foreground service that keeps the app running while online and in the background (a GameMaker template; swap `${YYAndroidPackageName}` for your package) |
| `example-pfo50/fakesteam_impl.inc` | the worked example: PFO 50's Steam lobbies and Steam networking calls, implemented over ENet plus the files above |
| `example-pfo50/steam/steam_api.h` | the stand-in Steam header that example compiles against |

### How the relay fits under ENet

Relayed peers get stand-in addresses in `0.0.0.0/8` (`0.0.0.1:1`, `0.0.0.2:1`, ...), which are
never valid on the wire. The ENet patch sends datagrams for those addresses to
`enet_virtual_send_hook`, and `enet_socket_receive` asks `enet_virtual_receive_hook` first. oknative
sets both hooks. ENet's reliability, ordering, pings and timeouts work unchanged on top of the
relay, and the game code doesn't know which path a peer is on.

The mailbox API (on the room server, keyed by room, peer id and a per-peer token `ptok`):

- `a=rsend&room=<hex8>&peer=<hex16>&ptok=<tok>&f=<to>.<base64url>*<to>.<base64url>...`: up to 64
  datagrams per request.
- `a=rpoll&room=...&peer=...&ptok=...&wait=1500&c=1`: waits up to `wait` ms and returns
  `{"ok":true,"f":"<from>.<base64url>*..."}`.

The host only polls while a relayed peer is connected, or for 60 s after a knock. The joiner polls
for 20 s after it starts trying the relay. When nobody is relayed, nothing is polled.

## Wiring it into a game

oknative is a set of `#include` files meant to sit inside your networking namespace. They are not
a separate library. They expect these from the code around them (as `example-pfo50` provides):

- `ENetHost* g_Host` (one host, one UDP port, `g_Port`), `uint64 g_MyId`, `int64 NowMs()`;
- lobby state: `g_InLobby`, `g_IsOwner`, `g_Lobby` (id, name, data), `g_Conns` (open game
  connections), `PumpRendezvous()`;
- `g_HttpPost`, a function that does one form POST and returns `"<status>\n<body>"`. On Android
  that is `OkplayHttp.okpPost` through JNI. On a desktop, use any HTTP client.

Then:

1. Build ENet 1.3.18 with the patch applied.
2. Pick your own app id (32 hex digits) and wire number in `okplay_client.inc` (`c_OkpApp`,
   `c_OkpWire`) so your rooms never show up in another game's list.
3. Call `PumpOkplay()` and your ENet service loop from the game's frame. Call `StartBackgroundPump()`
   once after creating the ENet host. Take `g_NetMutex` around every call from the game into the
   network code, so the background thread never runs at the same time as the game.
4. Hosting: `UpnpMapAsync()` when a lobby opens and `UpnpUnmapAsync()` when the app quits.
   Keep the relay and the mapping up when a lobby turns into a match: many games leave the lobby
   when the match starts, and the match runs over the same paths.
5. Joining: knock, connect to every candidate, and after 2.5 s with no answer call
   `RelayStart(room, 20000)` and connect to `RelayAddressFor(hostPeer)` as one more candidate.
   `example-pfo50/fakesteam_impl.inc` (`JoinLobby`, `PumpNetwork`) does exactly this.

### Android: staying online in the background

Android stops a GameMaker game's loop when the app leaves the screen (`onPause`), and newer
versions freeze a backgrounded app within seconds. Two things keep a session alive:

- **The background pump** (`netplus.inc`): every 50 ms, if the game hasn't called in for 300 ms,
  a thread services ENet (acks and pings keep flowing; received events are saved for the game
  thread) and keeps the room announced.
- **A foreground service** (`PFOOnlineService.java`), started while online, with a quiet
  notification ("Keeping your online game connected"). It needs the `FOREGROUND_SERVICE`,
  `FOREGROUND_SERVICE_SPECIAL_USE` and `POST_NOTIFICATIONS` permissions, and the service declared
  in the manifest.

GameMaker gotcha: an extension's `androidinject` field is not applied by Igor (2024.14). Put the
`<service>` element in the extension's `androidcodeinjection` field, wrapped in
`<YYAndroidManifestApplicationInject>...</YYAndroidManifestApplicationInject>`, and it lands inside
`<application>`.

UFO 50 test: a joiner and a relayed host each sent to the home screen for 40-45 s stayed in the
lobby.

## Testing

- Force the relay on a joiner: create an empty file `forcerelay` in the app's files folder
  (Android: `/sdcard/Android/data/<package>/files/forcerelay`). The joiner skips every direct
  address. The log should show `joined via 0.0.0.1:1`, and the tag should read RELAY on both sides.
- Point a build at a local room server with a `okplay.txt` file holding the URL, and run
  `python okplay/server/rdv_standin.py --port 8787`.
- Two emulators on one PC can reach each other through the host PC's address (`10.0.2.2:<port>`),
  which the client adds as an extra candidate.

## Credits and licences

- [ENet](https://github.com/lsalzman/enet) by Lee Salzman, MIT licence. The patch here is to its
  `unix.c`.
- [PFO 50](https://github.com/Blaizer/PFO-50) by Blaizer, MIT licence: the online multiplayer mod
  for UFO 50 this was written for. `example-pfo50/` is part of the Android port of it.
- UPnP: ported from okplay's own `okplay/src/upnp.c`.
- The room server, relay and STUN use are okplay's (this repository). STUN servers: Cloudflare and
  Google public STUN.

The files in `oknative/` are under the MIT licence, the same as ENet and PFO 50, so they can go
into either. No Valve, Steamworks or game code is in this folder: `example-pfo50/steam/steam_api.h`
is a small stand-in written for the port that declares only the Steam types and calls PFO 50 uses.

The full UFO 50 Android build (overlay, game patches, build script) is in
[UFO 50 Android Online](https://github.com/Patrick-Working/UFO50AndroidOnline). Its build fetches
`src/` and the ENet patch from this folder.
