# okplay - Oldkid.net Online Play

**Latest release: v0.8.7** (2026-11-11) - one download for every game: [oldkid.net](https://oldkid.net/aoe2net/)

| inside | version | for |
|---|---|---|
| **DirectPlay drop-in** (`dplayx.dll`, `dplay.dll`) | 0.8.7 | DirectPlay games (AoE2, Midtown Madness 2, Necrodome, ...) |
| **Winsock shim** (`wsock32.dll`) | 0.2.2 | games that use Winsock directly: Road Rash, Need for Speed III. Inert in every other game |

**What's new in v0.8.7:** [release notes](docs/RELEASE_0.8.7.md) - the mouse pointer shows on okplay's boxes again (Windows 10/11, Need for Speed III),
boxes keep a sensible size on low-resolution game screens.

**In v0.8.6:** [release notes](docs/RELEASE_0.8.6.md) - Need for Speed III: Hot Pursuit, TCP games over okplay,
full DirectPlay sessions refuse late joiners, Connection Check in every game folder.

**Latest supported games:** Need for Speed III: Hot Pursuit (new in v0.8.6: Modern Patch and the 1998 release, room list, passwords, up to 8 players) · Road Rash Online (Steam/GoG/CD-Rom; new in v0.8.5: [netcode fixes](#road-rash-online-steamgogcd-rom-what-v085-improves)) · Age of Empires II: The
Conquerors (original and UserPatch 1.5) · Midtown Madness 2 · Necrodome. Online Play with game passwords, LAN Play, Windows 98 to
11. Details under [Games and projects](#games-and-projects).

A clean-room replacement for Microsoft DirectPlay, so DirectPlay games from the late 1990s can be
played online again - on Windows 95 through Windows 11, and in the browser.

- **The Windows drop-in** (`winedplay/`): `dplayx.dll` and `dplay.dll` built from Wine's open-source
  DirectPlay, brought to Windows and tested call by call against Microsoft's own until they answer
  the same. A game uses it by having the two DLLs in its folder; nothing in the game is changed.
- **Online Play and LAN Play** (`okplay/`): the transport underneath - finds players through the
  oldkid.net room server, connects them directly (with a relay when it must), and carries DirectPlay's
  messages over UDP with an optional reliable lane. Also the room server itself and Connection Check.
- **The browser layer** (`okweb/`): the same DirectPlay compiled to WebAssembly with a browser
  transport, for games ported to the web. A game's browser build calls DirectPlay exactly as it
  always did; `okweb/README.md` is the guide for wiring a port to it.

Released builds: <https://oldkid.net>.

Working here: start with `docs/WORKING_NOTES.md` (rules, recipes, where things stand).

## Road Rash Online (Steam/GoG/CD-Rom): what v0.8.5 improves

`wsock32.dll` 0.2.0 fixes Road Rash's own netcode. It works for the Steam and GOG editions (the same exe,
byte for byte) and the 1996 CD release, in any mix; any other build still plays online with the original
netcode. It also works alongside [RoadRash30-MultiFix](https://github.com/magoowa/RoadRash30-MultiFix)
(a `ddraw.dll` mod with a Direct3D 11 renderer and widescreen): tested together, Steam with MultiFix 2.15.0
hosting a CD player.

- **Matched race clocks.** Each PC counts race time from its own start, a few steps apart, so the game
  guessed where the other bikes are for the wrong moment. The two shims now compare clocks once a second
  and take the difference out, all race long (the difference drifts during a race).
- **Glide instead of snap.** When a new position report shows the game's guess was wrong, the bike glides
  to the right place instead of jumping there.
- **No more flying bikes.** Gravity in the height guess (the game kept a bike rising past the top of a
  jump until the next report), and AI bikers, or players during a dropout, stop rising when no news comes.
- **AI bikers in sync.** Both PCs now agree on who runs each AI biker (the game's own resend never
  happened), and a PC ignores its own AI bikers sent back to it by the host.
- **Clean exits.** A game closed in the middle of a race now ends instead of staying in the background.
- **The same boxes as DirectPlay games:** update notices, crash and connection reports, "server
  unreachable" and "no network", and a warning when the host's game is a build that doesn't race with yours.

Measured against the original netcode, with a replay simulator over recorded two-player races and a
two-PC Windows 98 test bench with simulated lag and dropouts:

| connection (one way) | sudden jumps of the other bikes |
|---|---|
| LAN | -41 % |
| 100 ms | -32 % |
| 150 ms | -26 % |
| 250 ms | -20 % |
| 1 s dropouts every 6 s | -37 % |

Players drawn in the air at 150 ms: a half to a seventh as often on the test bench. At high ping a bike sits
slightly further from where it really is - the price of gliding instead of snapping. Every fix can be
turned off in `okplay.ini` (the `rr_` settings); details in `docs/GAME_ROADRASH.md`. Not changed yet: hits
are decided on the victim's machine, so two screens can still disagree about a hit.

## Clean room

DirectPlay is reimplemented from Wine's LGPL source, Microsoft's published protocol specification
([MC-DPL4CS]) and black-box tests: our tests call Windows' DirectPlay and ours the same way and compare
what comes back, on the API and on the wire. Microsoft's DirectPlay binaries are never disassembled,
decompiled or debugged, and no Microsoft code or SDK file is in this repository. The rules and the
measurements are in `docs/DPLAY_CONFORMANCE.md`.

Game-specific fixes (Road Rash's netcode, Necrodome's network list) use what studying the game taught us -
addresses and what the values there mean - and are our own code: no game code or bytes are copied, the
game's files are never changed, and they switch on only for the exact exes they were made for.

## Layout

| folder | what |
|---|---|
| `winedplay/` | the drop-in: Wine's dplayx ported to Windows (`port/`), the pristine Wine sources it came from (`src/`), the Windows shims (`shim/`), packaging, Windows 95/98 checks |
| `okplay/` | the Online/LAN Play transport, the room server (`server/`), Connection Check, tests (`tests/`) |
| `okweb/` | DirectPlay for the browser (`dplay/`), the JavaScript SDK (`sdk/`), demo, how a browser port plugs in (`integrations/`) |
| `oknative/` | okplay for native games on ENet (first used by the UFO 50 Android port): room list, hole punching, UPnP, the relay, staying online in the background on Android |
| `conformance/` | the suites that compare ours with Windows' DirectPlay (S1 Wine's tests, S3 wire tap, S4 two machines) |
| `okplay/wsock/` | okplay for games that use Winsock directly: a `wsock32.dll` beside the game (first: Road Rash) |
| `okplay/tools/wsocklog/` | a pass-through `wsock32.dll` that logs a game's Winsock calls (for non-DirectPlay games) |
| `okagent/` | a small remote-control agent for the Windows 98 test VMs |
| `docs/` | design, conformance results, per-game notes (`GAME_*.md`) |

## Building

On Windows with [MSYS2](https://www.msys2.org/) and its 32-bit compiler
(`pacman -S mingw-w64-i686-gcc`):

```
bash env.sh                 # shows what it found (compiler, Python, ...)
bash okplay/build.sh        # the transport, Connection Check, test tools
bash winedplay/build.sh     # the drop-in: winedplay/bin/dropin/dplayx.dll and dplay.dll
bash winedplay/package.sh   # a release folder (needs the builds above)
```

Optional: the Indeo video decoder some games need for their intro movies links FFmpeg (LGPL, the
Indeo 5 decoder only); `okplay/codec/build-ffmpeg.sh` builds it, and the drop-in build picks it up.
Without it the drop-in builds and plays; only that decoder is left out. Machine-specific paths go in
`local.env` (see `env.sh`).

The browser build (`okweb/dplay/build_wasm.sh`) needs Emscripten.

## Testing

- `okplay/tests/run_local.sh quick` - two-player sessions on one PC through the drop-in (needs the
  room-server stand-in, `python okplay/server/rdv_standin.py`).
- `okplay/tests/apidiff.sh` - every API call in many states, ours against Windows' DirectPlay
  (needs Windows' DirectPlay feature).
- `conformance/run_s3.sh` - what each DirectPlay puts on the wire, message by message.

## Games and projects

**DirectPlay games (the drop-in)** - any DirectPlay game should work, from DirectX 3 (`DPLAY.DLL`) to
DirectX 7. Tested:

- **Age of Empires II: The Conquerors**, original and UserPatch 1.5 - Online Play, LAN Play, passwords,
  Windows 98 to 11.
- **Midtown Madness 2**.
- **Necrodome** (DirectX 3 era): the GOG edition and the original CD release, Windows 10 against
  Windows 98 (`docs/GAME_NECRODOME.md`).

Fixes for one game's own bugs are kept apart, in memory only and for an exact game file
(`winedplay/shim/gamefix.c`) - the game's files are never changed.

**Other projects built on okplay**

- [UFO 50 Android Online](https://github.com/Patrick-Working/UFO50AndroidOnline) - build your own
  Android version of UFO 50 with online play; its room list, hole punching and relay are okplay's
  (`oknative/`), on top of [PFO 50](https://github.com/Blaizer/PFO-50)'s multiplayer.
- **Browser ports**: `okweb/` gives a game compiled to WebAssembly the same DirectPlay; first used by a
  browser port of Age of Empires II (in development).

**Winsock games (`okplay/wsock`)** - for games that talk UDP or TCP themselves instead of through DirectPlay:
a `wsock32.dll` beside the game carries their traffic over okplay (room list, hole punching, the same
passwords and boxes as the drop-in), and the game's own LAN game list becomes the room list. Tested:

- **Road Rash Online (Steam/GoG/CD-Rom)**: the Steam edition and the original CD release play each other,
  Windows 98 to 11, Online Play and LAN Play with game passwords, lobby and races, and since v0.8.5 smoother
  racing ([netcode fixes](#road-rash-online-steamgogcd-rom-what-v085-improves), `docs/GAME_ROADRASH.md`).
- **Need for Speed III: Hot Pursuit** (new in v0.8.6): the Modern Patch and the original 1998 release race
  each other, Client-Server and Peer-to-Peer games, a room list instead of typed addresses, a password per
  hosting, up to 8 players, full games say so (`docs/GAME_NFS3.md`).

Next: races with 3+ players, IPX games (Road Rash's IPX mode and others) over okplay.

## Licence

LGPL 2.1 or later (`LICENSE`), as Wine's DirectPlay it is built from. Copyright the Wine authors and
the okplay contributors.
