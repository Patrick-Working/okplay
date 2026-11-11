# Oldkid.net Online Play v0.8.6 - release notes

Released 2026-11-10. DirectPlay drop-in 0.8.6, Winsock edition (`wsock32.dll`) 0.2.1.
Players on 0.8.3 to 0.8.5 keep playing with 0.8.6 players (Road Rash 0.2.0 with 0.2.1 checked on LAN
Play and Online Play, either side hosting; the drop-in checked against Windows' own DirectPlay).

## New game: Need for Speed III: Hot Pursuit

Copy `wsock32.dll` and `okplay.ini` into the game folder; nothing in the game is changed.

- **A room list instead of typed addresses.** Host Game puts the game in the list; Join shows the list
  over the full-screen game. No port forwarding. Online Play, LAN Play or the game's original TCP/IP,
  chosen at the first network screen.
- **Both releases, in any mix.** The Modern Patch and the original 1998 release race each other.
  A different game build is named before the join, not after a failed race.
- **Both game types.** Client-Server and Peer-to-Peer; the player who pressed Host Game hosts in both.
- **A password per hosting**, asked each time a game is hosted; locked games show "(pw)".
- **Up to 8 players.** A full game says "full" in the list and turns more players away
  ("That game is full."); the count follows players who leave.
- **Rooms named after the host**, from the moment the game is hosted.
- **Clear endings.** Joining a race that has already started says "No answer from the host. The game
  may have started or closed." A host who leaves the lobby takes the room off the list.
- **Races hold on slow PCs** (Windows 98 class): the network thread keeps its turn, resends are capped.

Details and test results: `docs/GAME_NFS3.md`.

## Changes for every game

- **DirectPlay games: a full session refuses late joiners and extra players**, as Windows' DirectPlay does.
- **Connection Check works in every folder.** In a Winsock game's folder (Road Rash, Need for Speed III)
  its test game runs through `wsock32.dll`, in a DirectPlay game's folder through `dplayx.dll`, and
  players in both kinds of folders test with each other. It says how this PC reaches the other players,
  warns about two public addresses, reads all log folders and shows the version.
- **Road Rash Online:** unchanged netcode (0.2.0's fixes); `wsock32.dll` now carries its version.

## Back end

What changed underneath, and what is shared between games:

- **TCP games over okplay (shared).** The Winsock edition used to carry UDP games only. It now gives a
  game virtual stream sockets: listen, connect, accept, send and receive run over okplay's reliable lane
  (`OKP_SEND_RELIABLE`), with the room list, hole punching, relay and passwords the UDP games already had.
  Any Winsock TCP game can use this part.
- **Game-specific code in its own file (per game).** `okplay/wsock/rr_fix.c` (Road Rash) and
  `okplay/wsock/nfs3_fix.c` (Need for Speed III) hold what only that game needs: which exe builds are
  known, the host rule, the player limit, the room name. A fix applies only to an exe it recognises;
  any other build plays with the shared part alone. The shared code asks the game table (for example for
  the player limit, or `wsock_max_players` in `okplay.ini`) instead of knowing a game by name.
- **Core (shared):** stricter checks on everything received from the network, in the core, the drop-in
  and the room server; peers can be held while a box is open; large work buffers moved off the stack and
  okplay's threads get their own stack size on Windows 9x; `okp_peer_info` for the shim and Connection Check.
- **Room server (`okplay.php`):** stricter input checks, safer store writes, limits per network; tells
  each edition its own newest version. Older builds keep working with it. `latest.php` always links the
  newest zip.
- **Releases are rebuildable.** One script (`winedplay/package.sh`) builds every file, the pack and the
  site folder; every shipped binary carries its version; `okplay/server/site_upload.py` uploads a release
  folder (dry run by default).
- **One test command.** `okplay/tests/release_gate.sh` runs every suite: unit and server tests, local
  games through each edition, 60 TCP scenarios with a stand-in for the game, 25 router-type pairs, and
  the comparisons with Windows' DirectPlay (wire traffic, mixed host/joiner games, API tests).

## Not in this release

- Need for Speed III is tested on Windows 98 PCs on one network; reports from internet games and from
  Windows 10/11 are welcome.
- Falling back to `http://` when HTTPS to the room server fails, and crisp boxes on high-DPI screens,
  are planned for the next release.
