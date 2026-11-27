# Carmageddon (dethrace) over okplay

Version: notes 1, 2026-11-26. Status: **working in the lab, not in a download yet** (okplay wsock 0.3.16 on main).

[dethrace](https://github.com/dethrace-labs/dethrace) is the open-source Carmageddon engine (it needs the game's own
data). okplay carries its network games over Oldkid.net Online Play and LAN Play, with **no change to dethrace**:
the official releases work as they are.

The 1997 `CARM95.EXE` is not covered: it plays over IPX, and its network messages are not compatible with
dethrace's UDP ones.

## How okplay gets in

dethrace imports `ws2_32.dll`, a Windows "known DLL" that is always loaded from System32, so okplay's usual
`wsock32.dll` beside the game is never loaded. dethrace also imports `dbghelp.dll` (for its crash report), which is
not a known DLL: a `dbghelp.dll` next to `dethrace.exe` is loaded instead of Windows' own.

- `okplay/wsock/build_proxy.sh` builds okplay wsock as `bin/proxy32/dbghelp.dll` (32-bit dethrace);
  `build_proxy.sh 64` builds `bin/proxy64/dbghelp.dll` (64-bit dethrace, MSYS2's mingw64 compiler).
- `proxy_dbghelp.c` passes every debug function dethrace's builds use to Windows' `dbghelp.dll` (from System32,
  loaded the first time one is called - only when the game crashes).
- `wsock.c` (`OKW_PROXY`) points `dethrace.exe`'s own Winsock imports at okplay, in memory (the exe file is never
  touched); Windows and okplay itself keep Windows' Winsock. A game that isn't dethrace is left alone.
- The 64-bit build leaves out the 32-bit game patches of Road Rash and Need for Speed III (`games/none64.c`).

## What okplay does for dethrace (profile `carma`, `okplay/wsock/wsock.c`)

- **The game's own address.** dethrace tells the other players the first IPv4 address of its network adapters.
  In Online/LAN Play okplay answers that lookup (`GetAdaptersAddresses`) with this PC's okplay address, so the
  other players' messages reach it.
- **Who hosts.** dethrace never announces a game; it only answers a joiner's search ("XXXXCW95MSG1" -> "...MSG2",
  UDP port 12286). Every 3 s okplay hands the game's socket such a search itself; an answer means this PC hosts,
  and the room is listed (before anyone searches, which Online Play needs). [universal: the profile's `probe`]
- **The game list.** A joiner's game sees every open room as a host that answered, and asks each for its details
  as usual: the list shows host, game type, players and status.
- **Questions** are okplay's own boxes, as for Road Rash: Online Play / LAN Play / Original when the network menu
  opens (okplay.ini `wsock_mode=ask`; asked every time - owner, 2026-11-26), the host's optional password when it
  hosts (`use_password=ask`).
- **Passwords** (profile `pre_auth`, `join_refusal`): in a password room the list's own messages (the search,
  "send me your details", the details, acknowledgements) pass before the password; the password is asked when
  the player presses Join. A wrong one asks again in the same box while the game keeps waiting; Cancel gives the
  game the host's own "you can't join", so it goes back at once. dethrace's list can't show a lock: a password
  game reads "READY, OPEN".

## Tested (one PC, two copies, Oldkid.net LAN Play)

- dethrace built unmodified from source (32-bit, MinGW) and the official v0.10.1 64-bit release: host, list,
  join, lobby, race; no packets lost, none dropped.
- Windowed: the mode question, the host password, join with a wrong then the right password, Cancel.
- Full screen (dethrace's borderless full screen): the boxes show on top and work; the Windows taskbar shows while
  a box is open.
- Network timing measured with scripted driving (a lab build of dethrace with test hooks): through okplay the same
  as without it. dethrace's own netcode moves the other cars in steps (about 11 updates a second, no smoothing).

## Not yet

- Online Play, two PCs, the internet; the official 32-bit release; Windows 7/8.
- A player who takes over 30 s in the join box (dethrace gives up its join; joining again goes straight in).
- Whether dethrace with okplay still meets a plain dethrace on a LAN in Original mode.
- A download: the `dbghelp.dll` and an okplay.ini for dethrace's folder, and the page's instructions.
