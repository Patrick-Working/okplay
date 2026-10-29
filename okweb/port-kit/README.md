# port-kit: the reusable parts of a game's browser port

What we built for the AoE2 browser port (rogan2), taken out of it and made game-neutral, so the next
DirectPlay game's web port starts with them. Each piece stands alone.

| piece | what it does |
|---|---|
| `page/okweb-page.js` | the page side in one call: Online Play from the page's address (`?netserver`, `?netname`, `?relayonly`, `?noping`, `?nopassword`, `?netpassword`), the "Save netplay log" / "Send log" buttons, a ping overlay for testing |
| `emscripten/okport.{h,c}` | waits that are safe in an Asyncify build (`okport_can_wait`, `okport_show_frame`) and fetching one of the game's files when it is first needed (`okport_fetch_file`) |
| `video/okavi.{h,cpp}` | a game's AVI movies: Indeo Video 5 through FFmpeg's decoder, PCM sound through WebAudio, drawn on a canvas over the game |
| `video/build_ffmpeg_indeo5_wasm.sh` | builds that decoder (FFmpeg 7.1, LGPL, indeo5 only) for the browser |
| `../integrations/rogan2/two_tab_test.py` | two players in two headless tabs: host, join, ready, start, both in the game |

## DirectPlay: does a port need an adapter?

A port that compiles against the standard DirectPlay headers (the ones in `winedplay/port/include/`,
`-fshort-wchar` so WCHAR is 16-bit as on Windows) calls the library's `DirectPlayCreate` /
`DirectPlayLobbyCreateA` directly: nothing to adapt. A port with its own, different `dplay.h` (rogan2's
has shortened system-message structures) needs a thin adapter from its interfaces to the plain C API in
`okweb/dplay/include/okweb_dplay.h` - that adapter is the port's code, as rogan2's
`age2_x1/okweb_netplay.cpp` is. Either way the library goes in with `okweb/dplay/export_to_game.sh`
and links with `-sASYNCIFY` (DirectPlay's calls wait for the network, as they block on Windows).

## The page

```js
import { connectPage } from './okweb-page.js?v=1.2.0';   // next to okweb-sp.js and okweb.js
connectPage(Module, { gameName: 'My Game (browser)', overlayParent: document.getElementById('game-container') });
```

Install it before the game starts its multiplayer (at page load is fine). Any `installOkwebProvider`
option can be passed through (`brand`, `askPassword`, `iceServers`, ...).

## Waits and files (okport)

- Wrap the game's main-loop callback in `okport_frame_enter()` / `okport_frame_leave()` (RAII in C++).
- A status screen drawn before long work ("Loading...") only shows if the browser gets a turn:
  `okport_show_frame()` right after drawing it - and only then: a game that calls its "paint now"
  function every frame would halve its frame rate (rogan2 did, until it checked for the status screen).
- Files the page doesn't load at start (music, movies, voice): where the game opens one and it is
  missing, `okport_fetch_file(path)` and open it again. Files a game finds by listing a folder
  (`taunt\*.mp3`) can't be fetched on demand: load those at start.
- `SDL_SetHint(SDL_HINT_EMSCRIPTEN_ASYNCIFY, "0")` before `SDL_Init` when the game runs on
  `emscripten_set_main_loop`: otherwise SDL yields in every `SDL_RenderPresent`.

## Movies (okavi) behind MCIWnd

Games of that era play movies through Windows' MCIWnd: `MCIWndCreate(parent, ..., file)`, then MCI
messages to the window. rogan2's stand-in maps them onto okavi:

| the game sends | okavi |
|---|---|
| `MCIWndCreate(file)` | `okavi_open(file)` (fetch it first if the page didn't load it), return a window handle |
| `MoveWindow(movie, x, y, w, h)` | `okavi_place(x, y, w, h, screen_w, screen_h)` |
| `MCI_PLAY` 0x0806 / `MCI_STOP` 0x0808 / `MCI_PAUSE` 0x0809 / `MCI_RESUME` 0x0855 | `okavi_play` / `okavi_stop` / `okavi_pause` / `okavi_resume` |
| `MCIWNDM_GETMODEA` (WM_USER+106): "playing", "paused", "stopped" | `okavi_mode()` |
| `WM_CLOSE` | `okavi_close()` |
| `GetClientRect(movie)` | `okavi_size()` |
| `ICInfo(ICTYPE_VIDEO, 'iv50')` (is the codec there?) | `okavi_available()` |

The window keeps a window procedure the game can subclass (`Get/SetWindowLongPtr(GWLP_WNDPROC)`,
`SendMessage` goes through it), as MCIWnd's does. Call `okavi_tick()` once per frame. A movie that the
game plays during its setup (before the main loop runs) has to start at the first frame instead.

## A test console (the pattern)

Hands-free tests need the page to read the game's screens and press its buttons. That code reads the
game's own UI, so it is the port's, but two rules from rogan2's apply to any port:
- Never call into the game while one of its frames is suspended in a wait (`Asyncify.currData`): read
  a cached copy of the screen then, and refuse actions. Calling in corrupts the waiting frame.
- Run actions (press, type, pick) at the start of the game's next frame, as a click is, not from the
  page's call: an action that waits (joining a game) then waits inside the frame like any other.
