# Need for Speed III: Hot Pursuit (1998)

Online play through the game's own **Connect Players > Network (TCP)** menu, with okplay's `wsock32.dll`
beside `nfs3.exe` (`okplay/wsock/`, the same shim as Road Rash; this game uses its TCP half). No DirectPlay.
The game's exe is never changed.

Builds known to the shim (by exe fingerprint, `okplay/wsock/nfs3_fix.c`): Modern Patch 1.6.1, the portable
1.6.2 Beta, the 1998 OEM release and the 1998 CD release. Tested on Windows 98 (86Box, software renderer),
two PCs: Modern Patch against Modern Patch and 1998 against 1998, host and join, races to the end, clean exit.

## What the game does on the network (black box, `okplay/tools/wsocklog`)

- **Network (TCP) menu**: starts its TCP service at once - a probe socket, then a listener on port 9803
  (1998 builds: 1030), `listen(5)`, `getsockname`, and a comms thread that sits in `select`.
- **Host Game** (Peer-to-Peer for 2 players, Client-Server for 2-8): closes the listener, restarts Winsock and
  listens on the same port again. Entering the menu a second time makes exactly the same calls.
- **Join Game**: a box "Enter domain name or IP address", then `gethostbyname` and a `connect` loop to the host's
  port.
- **Lobby > Disconnect**: closes the connections; the listener stays open until the next Host/Join.
- Frames are read with `recv(MSG_PEEK)` first; the game expects stream semantics exactly as Winsock 1.1 gives them.

## How okplay fits in

- **TCP over okplay**: every game connection becomes a reliable ordered stream on okplay's transport (rooms,
  hole punching, relay, LAN Play). The game still calls `listen`/`connect`/`select`/`recv`; the shim answers.
- **Which listen is Host Game**: the socket calls can't tell, the game's own state can. Two values of the game
  are read (never written): a "network game state" and a "this PC is the server" flag. Host Game sets both before
  it listens; a menu entry sets neither; a joiner clears the server flag. Unknown builds fall back to the
  listen-again rule. Peer-to-Peer and Client-Server hosting set the same two values; a third value tells them
  apart (below).
- **Rooms**: a room is listed when Host Game is seen, and comes off the list when the game closes its listener,
  when the host leaves its lobby (the state goes back while the listener stays open - taken off after 1.5 s with
  nobody connected), or when the game exits. Hosting again lists a new room.
- **Password**: asked at every Host Game (okplay.ini `use_password=ask`); joiners of a locked room are asked.
  Only a scrambled form leaves the PC.
- **Join box**: with okplay on, the box is pre-filled with `oldkid.net` (`nfs3_join_prefill=0` turns that off).
  Ok opens okplay's room list instead of looking the name up; the list refreshes by itself (every 1.5 s) and
  shows players and a password marker. A typed address of a LAN host that okplay knows still connects directly.
  With the game's original TCP/IP the text is taken out again, so the game never connects to the real site.
- **Player limit**: shared by every TCP game of the shim - a room takes a set number of players, host included;
  okplay counts players, not connections (a joiner that reconnects counts once); when the room is full a new
  player is refused by okplay before the game sees the connection (reason 4, "the game is full": the joiner's game
  gets "connection refused", as with every refusal; a newer joiner also logs it and shows "That game is full."
  once); the room list shows "(2/2 players) - full"; the count in the list goes down again when a player leaves.
  okplay.ini `wsock_max_players=N` (2-64) sets the limit for any game; otherwise the game's own word, else 8.
  NFS3 only: the game's own word is the game type the host picked, read (never written) next to the network game
  state - Peer-to-Peer = 2 players, Client-Server = 8 (0 = Peer-to-Peer, 1 = Client-Server; seen on the real game,
  Modern Patch 1.6.1, 2026-11-10; the log shows the raw value as "game type N" at every listen and every incoming
  connection). Unknown builds, or another value: 8.
- **Full room in the list** (shared): Join on a line marked "full" stays in the list and says "That game is full.
  Pick another, or wait for a place."; the list refreshes itself, and Join works as soon as a place is free. The
  host's refusal stays as the backstop (two players pressing Join at once, an older build, a list a second old);
  the notice "That game is full." is then shown as the game closes the refused connection.
- **Room name** (shared rule, per-game source): "<game> - <player>'s game". The player is okplay.ini `name=` when
  set, else the name the player has in the game itself (per-game hook `player_name`; NFS3: Player 1 Name, read
  only), else the computer's name.
- **Online / LAN / Original**: the same box as in every okplay game (`wsock_mode=ask|online|lan|original`),
  shown when Host Game or Join Game is chosen.
  **When it is asked again** (noted 2026-11-10, wsock 0.2.2 test): the shared rule is "a new game asks again" -
  `check_new_game()` in wsock.c clears the choice once the game has had no UDP socket open for 2 s (Road Rash:
  Disconnect) or a host closed its room with no player left. NFS3 never meets that rule from its menus (seen in
  the live test: Join Game, Cancel, Join Game again shows no box), so here the choice holds for the whole game
  run: **once per launch**. To change it the player restarts the game; `wsock_mode=online|lan|original` fixes it
  with no box. Host or Join is the game's own menu and comes every time; so do the room list and the password
  boxes. Possible later hook (game-only, not built): ask again when the game leaves its network screen (the log
  line "game state now: net 0").
- **Windows 9x**: the 1998 exe gives its threads a 32,000-byte stack; okplay's own threads ask for their own
  stack size and keep large buffers on the heap.

## Tests

`python okplay/wsock/tests/tcp_run.py --mode shim` runs `tcpsim.exe` (a stand-in for the game's socket
patterns, including the game's state values at its addresses) against the shim: 59 scenarios - hosting,
joining, menu re-entry, lobby leave, role swap, passwords, the Join box, Original mode, stream timing, the player
limit.
`TCP_RUN_PHP_SERVER=<released okplay.php>` runs the online scenarios against a released room server instead
of the stand-in.

## Open

- Play over the internet is not tested yet. On three Windows 98 PCs (2026-11-10) these were tested through the
  room server: three players Client-Server (Modern Patch); a Peer-to-Peer game hosted by the 1998 OEM build
  (listed "1/2", a third player is told "That game is full."); a race between two 1998 OEM builds; a Modern Patch
  player joining a 1998 OEM host (the different-version box, then "Join anyway": lobby and race worked).
- A join to a game whose race is running: the host's game accepts the connection and sends nothing; the joining
  game gives up after about 4 s. okplay then says "No answer from the host. The game may have started or closed."
  (shared TCP rule in `wsock.c`: a join that came up, got no byte and was closed by the game). Tested on the
  PCs above; no `tcp_run.py` scenario yet.
- Room name: the game's name place holds its placeholder `gamename` until the first player joins; okplay then
  uses the computer name (or `name=` in okplay.ini) and renames the room once the player name is there.
- The Join box buffer of the 1998 OEM build is not known: no pre-fill there (type `oldkid.net` or `okplay`).
- The 1998 OEM build must be started in its own folder (its data paths are relative): started elsewhere it says
  "Need For Speed 3 files are corrupted; please re-install." - with or without okplay.
- Test note: copying files from a PC or capturing its screen while its race runs stalls that PC for seconds
  (okplay logs "pause" warnings then). Fetch logs after the race.
