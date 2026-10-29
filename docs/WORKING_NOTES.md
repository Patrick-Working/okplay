# Working notes

How to work in this repository: the rules, the recipes, and where things stand. Read this first.

## Rules

- **Clean room.** DirectPlay is rebuilt from Wine's LGPL source, Microsoft's published protocol
  specification ([MC-DPL4CS]) and black-box tests. Microsoft's DirectPlay binaries are never
  disassembled, decompiled or debugged, and no Microsoft code, SDK header or binary goes into this
  repository. Parity is measured, not copied: `docs/DPLAY_CONFORMANCE.md`.
- **Games are never changed on disk.** A game's own bugs are fixed in memory, for one exact game file
  (`winedplay/shim/gamefix.c`, keyed by name, size, SHA-256 and PE timestamp).
- **Nothing from a game goes into this repository**: no game files, movies, music, fonts or
  installers, and no screenshots. Tests take the game's folder from `local.env` / the command line.
  Test with legitimately obtained copies (store releases such as GOG/Steam, or original discs).
- **One library, every target.** The Windows drop-in and the browser build compile the same
  dplayx (`winedplay/port/dlls/dplayx/`); a change for one is built and tested for both
  (`bash winedplay/build.sh` and `bash okweb/dplay/build_wasm.sh`).
- **Version the browser SDK** with every change to `okweb/sdk/v1/*.js`: `VERSION` in `okweb.js` and
  every `?v=` that imports it (`okweb-sp.js`, `okweb/demo/index.html`, each game page) move together -
  web hosts cache scripts for days.
- **Releases are packed from this repository** (`winedplay/package.sh`): the source zip published next
  to each release is made from these folders, so what is here is what is public.

## Layout in one line each

| path | what |
|---|---|
| `winedplay/port/dlls/dplayx/` | our DirectPlay (Wine's, ported, with the conformance fixes) - shared by every target |
| `winedplay/shim/`, `winedplay/port/dlls/dpwsockx/` | the Windows drop-in's own parts (switcher, game fixes, TCP/IP provider) |
| `okplay/src/` | Online/LAN Play for Windows: UDP transport, hole punching, the reliable lane, the relay client |
| `okplay/server/` | the room server (`okplay.php`, `okstats.php`) and a Python stand-in for tests (`rdv_standin.py`) |
| `okweb/dplay/` | the same dplayx for the browser: `winweb.c` (the Windows functions it needs), the okweb service provider, game passwords (`okweb_password.c`), `export_to_game.sh` |
| `okweb/sdk/v1/` | the JavaScript side: `okweb.js` (rooms, WebRTC, relay), `okweb-sp.js` (the DirectPlay provider's page half, the password box) |
| `okweb/integrations/` | how a game's browser build plugs in; `rogan2/` is the worked example (and its two-tab test) |
| `okweb/port-kit/` | the reusable parts of a browser port: page wiring, Asyncify-safe waits and file fetching, AVI/Indeo movies |
| `conformance/` | the suites that compare ours with Windows' DirectPlay |

## Browser work: the recipe

1. Change the library here (`winedplay/port/...`, `okweb/dplay/...`, `okweb/sdk/v1/...`).
2. `bash okweb/dplay/build_wasm.sh` - the library builds for the browser (needs emsdk; `EMSDK`,
   `EMSDK_PYTHON`).
3. `bash okweb/dplay/export_to_game.sh <game checkout>` - copies it into the game as
   `third_party/classic-netplay/` (replaced whole; never edit the game's copy).
4. Build the game's browser build, serve it with its data:
   `python okweb/dplay/test/serve_game.py --build <game>/build_wasm --game <installed game folder> --port 8893`
   and a room server: `python okplay/server/rdv_standin.py --port 8796`.
5. Test hands-free: `python okweb/integrations/rogan2/two_tab_test.py --runs 10` (two headless Chrome
   tabs host, join, ready up and start; see its header). Pages on `127.0.0.1` and `localhost` are two
   different sites, so two tabs are two players.
6. Commit here; the game's repository gets the export as its own commit.

Gotchas learned the hard way:
- Chrome's `/json/new?URL` drops everything after the URL's first `&`: open `about:blank`, then
  `Page.navigate`. (One test once announced itself on the public room list because of it.)
- A background tab's timers are throttled to once a minute after 5 minutes hidden: tests pass
  `--disable-features=IntensiveWakeUpThrottling`, and pages that wait on the browser check
  `document.hidden`.
- An Asyncify build must not call into the game while one of its frames is suspended in a wait
  (`Asyncify.currData`): doing so leaves Asyncify "rewinding" and every later call traps. Test
  consoles queue their actions for the next frame instead.
- SDL's Emscripten port yields in every `SDL_RenderPresent` in an Asyncify build unless
  `SDL_HINT_EMSCRIPTEN_ASYNCIFY` is "0".

## Where things stand (2026-10-28)

- Drop-in: 0.8.2 released; parity with Windows' DirectPlay measured in `docs/DPLAY_CONFORMANCE.md`.
- Browser: okweb 1.2.0 (game passwords as in the drop-in, the lock shown on the room list). A full
  AoE2 browser port (rogan2) hosts, joins, readies and plays over Online Play, directly or through the
  room server's relay.
- Open: host migration in okweb; TURN for players behind strict NATs on both sides; whether browser
  "LAN Play" should mean "rooms from your own network" (browsers can't broadcast) or a room server on
  the LAN; cross-play between the browser and Windows builds of the same game needs both to compute
  floating point identically (see the game's own notes; not a library task).
