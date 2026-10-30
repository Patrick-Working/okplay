# okplay - Oldkid.net Online Play

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

## Clean room

DirectPlay is reimplemented from Wine's LGPL source, Microsoft's published protocol specification
([MC-DPL4CS]) and black-box tests: our tests call Windows' DirectPlay and ours the same way and compare
what comes back, on the API and on the wire. Microsoft's DirectPlay binaries are never disassembled,
decompiled or debugged, and no Microsoft code or SDK file is in this repository. The rules and the
measurements are in `docs/DPLAY_CONFORMANCE.md`.

## Layout

| folder | what |
|---|---|
| `winedplay/` | the drop-in: Wine's dplayx ported to Windows (`port/`), the pristine Wine sources it came from (`src/`), the Windows shims (`shim/`), packaging, Windows 95/98 checks |
| `okplay/` | the Online/LAN Play transport, the room server (`server/`), Connection Check, tests (`tests/`) |
| `okweb/` | DirectPlay for the browser (`dplay/`), the JavaScript SDK (`sdk/`), demo, how a browser port plugs in (`integrations/`) |
| `oknative/` | okplay for native games on ENet (first used by the UFO 50 Android port): room list, hole punching, UPnP, the relay, staying online in the background on Android |
| `conformance/` | the suites that compare ours with Windows' DirectPlay (S1 Wine's tests, S3 wire tap, S4 two machines) |
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

## Games

Any DirectPlay game should work; tested so far: Age of Empires II, Midtown Madness 2, Necrodome
(`docs/GAME_NECRODOME.md`). Fixes for one game's own bugs are kept apart, in memory only and for an
exact game file (`winedplay/shim/gamefix.c`) - the game's files are never changed.

## Licence

LGPL 2.1 or later (`LICENSE`), as Wine's DirectPlay it is built from. Copyright the Wine authors and
the okplay contributors.
