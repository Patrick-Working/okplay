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
- **Room name** (shared rule, per-game source; since wsock 0.3.9): "NFS3: <PC> - <player>'s Game" - the computer's
  name, then the player: okplay.ini `name=` when set, else the name the player has in the game itself (per-game hook
  `player_name`; NFS3: the menus' Player 1 Name buffer, the one the lobby shows, read only), else only
  "NFS3: <PC>'s Game".
- **Online / LAN / Original**: on the 1.6.x builds chosen in the game's own Network menu (next section); other
  builds, or okplay.ini `nfs3_menu=0`: the same box as in every okplay game (`wsock_mode=ask|online|lan|original`),
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

## In-game menus (wsock 0.3.5 - 0.3.10, 2026-11-19)

Every okplay question for this game is asked in the game's own menus and popups, drawn by the game in its own
style; okplay's Windows boxes (which fought full-screen DirectDraw and wrappers such as DDrawCompat: see
`docs/OVERLAY.md`, `docs/BOX_METHOD.md`) are only the fallback. Code: `okplay/wsock/nfs3_menu.c` [game: NFS3].
Builds: the 1.6.x exes (Modern Patch 1.6.1, portable 1.6.2 Beta), recognised by the game's own data (the menu
handler table entry for "connect" with its known handlers). Anything else, or okplay.ini `nfs3_menu=0`: the
Windows boxes as before. Nothing in the game folder changes; the game's exe is never changed.

- **Network menu**: the one "Network (TCP)" entry becomes three: "Oldkid.net Online", "Oldkid.net LAN" and the
  game's own "Network (TCP)" (okplay stays out). The game reads its menus (`fedata\menus\*.mnu`) and texts
  (`fedata\text\text.<lang>`) with `CreateFileA`; okplay hooks that import and hands the game edited copies made at
  run time from the player's own files (in okplay's log folder): three tab buttons instead of one, and four new
  texts in entries the game does not use. A menu's behaviour comes from the game's handler table (enter, leave,
  idle per menu); okplay chains the Network menu's enter handler (one pointer, written at load) and gives its
  callback to all three entries; the choice is known before any socket exists (`wsock_mode=game menu` in the log).
- **Popups**: the game has one popup engine (choice, notice, text entry) driven by its menu loop. okplay calls it
  from the menus' idle handlers (the Network menu had none; the lobby's is chained), on the game's own thread,
  with Watcom register-call thunks.
- **Host Game / Join Game box**: with Oldkid.net Online or LAN the game's box says "Oldkid.net Online" (or LAN) and
  has no Port button (okplay carries the connections; the port never matters there).
- **Host password**: asked in the lobby, right after Host Game: "Password for your game:" / "Leave it empty for an
  open game.", Ok / No Password. The room is listed only after the answer (`wsock.c`: `host_deferred`).
- **Join**: the game's address box ("Enter IP address", pre-filled oldkid.net) is replaced by the list of games in
  a choice popup (up to 7 games, Refresh, Cancel; it follows the room list while open; the line under each game:
  players, "full", "password"). Picking a game tells wsock the room (`okw_join_pick`: "the room picked before");
  a locked game first asks "Password for <game>:" in a text-entry popup (Ok / Back). okplay then answers the game's
  own address box itself (as if `oldkid.net` were typed and Ok pressed), so the game's own "Connecting..." and
  connect run unchanged.
- **Notices and questions**: connection reports, crash report, wrong password, game full, no answer from the host,
  room server unreachable, a newer version: game popups (`okw_ui_take` / `okw_ui_answer`, queued kinds as before).
- Screenshots: the game draws everything, so NFS3's own screenshot key (Print Screen) captures the popups too.
- **Universal parts** (`wsock.c`, for any game whose menus can show them): `okw_set_ui_ingame`,
  `okw_host_pw_pending/answer`, `okw_rooms`, `okw_join_pick`, `okw_ui_take/answer`.
- **Other builds** (in work): an address finder that reads any NFS3 exe at launch - starting from the game's own
  data (the menu table) and following its code to each function and value the menus need, every value
  cross-checked, else the Windows boxes. Profiles of the four known exes are its test.
- Tested 2026-11-19 on two PCs over Oldkid.net Online: Modern Patch 1.6.1 hosting, portable 1.6.2 joining from the
  game's list, lobby and a race.

## Leaving a game (wsock 0.3.9)

A joiner that left forgot its room at once and from then on dropped every packet from that host, also the acks
for its own last frame (the FIN) and the host's last frame: both sides resent into nothing, and the joiner's
`WSACleanup` waited its full 3 s. Since 0.3.9 the joiner keeps taking its old host's reliable lane (acks and data
frames only, that host only, that room id only) for 5 s after leaving (`okp_ctx.left_room/left_host/left_until`,
`net.c` handle_packet); acks echo the packet's room id. Unchanged on the wire for released builds.

## Measuring smoothness (wsock 0.3.10, `wsock_nettrace=1`)

NFS3 races in lockstep (every PC simulates every car; inputs to the host about 32 times a second, the host's
bundles back): a late message makes both PCs wait, so transport delay shows directly as a hitch. okplay.ini
`wsock_nettrace=1` (off by default) writes every game `send`/`recv` that moves data, in every mode (okplay or the
game's own TCP/IP), with a microsecond time and its stream offset, plus okplay's "arrived" time, to
`logs\okplay-net-<time>-<pid>.csv` (`okplay/wsock/nettrace.c`; the game's thread only copies into a buffer, a
low-priority thread writes). `python okplay/tools/nettrace_report.py HOST.csv JOINER.csv` lines the two PCs' clocks
up from the traces (fastest message each way taken as equal), then prints one-way times above the fastest
(p50/p90/p99/max, counts over 50 and 100 ms), the gaps between the game's reads, and okplay's split (network +
okplay vs the game picking the data up). Plan: the same race over okplay and over the game's own TCP/IP between
the same PCs, then transport fixes judged by the same numbers (small messages sent again in the next packets,
loss seen from gaps instead of a timer). First numbers (LAN, Wi-Fi laptop, 2026-11-19, from okplay's own stats):
32 messages a second each way, gaps mostly under 62 ms with peaks of 125-172 ms, one burst of 76 resends in 10 s.

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
- Room name (fixed in 0.3.9): the network name list holds its placeholder `gamename` until a race; the name now
  comes from the menus' Player 1 Name buffer.
- The Join box buffer of the 1998 OEM build is not known: no pre-fill there (type `oldkid.net` or `okplay`).
- The 1998 OEM build must be started in its own folder (its data paths are relative): started elsewhere it says
  "Need For Speed 3 files are corrupted; please re-install." - with or without okplay.
- Test note: copying files from a PC or capturing its screen while its race runs stalls that PC for seconds
  (okplay logs "pause" warnings then). Fetch logs after the race.
