# rogan2 integration (a worked example of a browser port)

rogan2 is a decompilation of Age of Empires II: The Conquerors with a browser build (a private
repository). Its browser build plays Online Play through okweb. The game-side changes live in rogan2
itself - its code is not ours to publish, so no patches of it are kept here - and they are the model
for wiring any other decompiled DirectPlay game:

| what the port adds | why |
|---|---|
| an adapter (`age2_x1/okweb_netplay.cpp` there) and DirectPlay factories in its own `dplay.h` | the game's DirectPlay calls reach okweb's dplayx |
| its build option: Asyncify, links okweb's `classic_netplay` library | DirectPlay calls that wait (EnumSessions, Open) block the game as on Windows |
| `web/shell.html` loads `okweb-sp.js?v=VERSION` | the browser transport; versioned, the host caches scripts for days |
| `dplay.h` constants exactly as in the DirectX SDK | 34 were off in the port's own header |
| `RAND_MAX` = 0x7FFF (VC98's) | the game's random maps assume it |
| "Save netplay log" / "Send log to oldkid.net" buttons, a ping ticker | testing and support |
| URL switches: `?nettrace`, `?relayonly`, `?noping` | the game's DirectPlay traffic in the console, relay only, no ticker |

The library goes into a game with `okweb/dplay/export_to_game.sh <game checkout>` (copies the built
`classic_netplay` and its headers into the game's `third_party/classic-netplay/`). The wire-up is
described in `docs/WEB_NETPLAY.md`; okweb itself knows nothing about any one game.

## Testing a port hands-free

`two_tab_test.py` (here) plays two-player Online Play games in two headless Chrome tabs against a
local room-server stand-in: the host's tab creates a game, the joiner's finds and joins it, both
ready up, the host starts, and both must reach the game screen - while the joiner's test console is
polled every 50 ms. It needs the port's test console (rogan2's `window.rogan2`: screens as text,
buttons by name, `?autoplay=host|join`, `?keepalive`); `docs/WORKING_NOTES.md` has the whole recipe.
