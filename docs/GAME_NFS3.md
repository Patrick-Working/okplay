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
  listen-again rule. Peer-to-Peer and Client-Server hosting look the same here.
- **Rooms**: a room is listed when Host Game is seen, and comes off the list when the game closes its listener,
  when the host leaves its lobby (the state goes back while the listener stays open - taken off after 1.5 s with
  nobody connected), or when the game exits. Hosting again lists a new room.
- **Password**: asked at every Host Game (okplay.ini `use_password=ask`); joiners of a locked room are asked.
  Only a scrambled form leaves the PC.
- **Join box**: with okplay on, the box is pre-filled with `oldkid.net` (`nfs3_join_prefill=0` turns that off).
  Ok opens okplay's room list instead of looking the name up; the list refreshes by itself (every 1.5 s) and
  shows players and a password marker. A typed address of a LAN host that okplay knows still connects directly.
  With the game's original TCP/IP the text is taken out again, so the game never connects to the real site.
- **Online / LAN / Original**: the same box as in every okplay game (`wsock_mode=ask|online|lan|original`),
  shown when Host Game or Join Game is chosen.
- **Windows 9x**: the 1998 exe gives its threads a 32,000-byte stack; okplay's own threads ask for their own
  stack size and keep large buffers on the heap.

## Tests

`python okplay/wsock/tests/tcp_run.py --mode shim` runs `tcpsim.exe` (a stand-in for the game's socket
patterns, including the two state values at the game's addresses) against the shim: 55 scenarios - hosting,
joining, menu re-entry, lobby leave, role swap, passwords, the Join box, Original mode, stream timing.
`TCP_RUN_PHP_SERVER=<released okplay.php>` runs the online scenarios against a released room server instead
of the stand-in.

## Open

- Three and more players, and play over the internet, are not tested yet.
- A Peer-to-Peer room shows "1/8 players" (the two host modes can't be told apart yet).
- The Join box buffer of the 1998 OEM build is not known: no pre-fill there (type `oldkid.net` or `okplay`).
- Mixed 1998 / Modern Patch games: untested (the game itself decides whether they are compatible).
