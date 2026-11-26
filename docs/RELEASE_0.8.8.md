# Oldkid.net Online Play v0.8.8 - release notes

Released 2026-11-25. DirectPlay drop-in 0.8.8, Winsock edition (`wsock32.dll`) 0.3.15.
Nothing changed on the wire: players on 0.8.3 to 0.8.7 keep playing with 0.8.8 players.

## New

- **Need for Speed III in the game's own menus.** Connect Players lists Oldkid.net Online, Oldkid.net LAN and
  the game's own Network (TCP). Join shows the open games in the game's own popup (no address to type); the
  host password, the join password, "a different version of the game", "That game is full." and okplay's other
  notices are game popups too. Nothing pops out of the full-screen game any more, so it also works over
  DirectDraw wrappers such as DDrawCompat. Builds: the Modern Patch 1.6.1 and the portable 1.6.2 by okplay's
  table; the original 1998 releases (OEM and CD) through an address finder that reads the game at start
  (`okplay/wsock/games/nfs3/`). A build okplay can't confirm gets the small boxes as before; okplay.ini
  `nfs3_menu=0` too. Details: `docs/GAME_NFS3.md`.
- **Room names** "NFS3: <PC> - <player>'s Game" (the player name the lobby shows).
- **Public usage statistics** on the room server: `stats.php` (games hosted per day, time played, players per
  game, the latest games). Every game that ends is recorded without names, addresses or ids; daily totals
  and these records are kept.
- **Network timing trace** (okplay.ini `wsock_nettrace=1`, off by default) and `okplay/tools/nettrace_report.py`
  for measuring how smooth a connection is.

## Fixed

- Leaving a race no longer stalls the game's exit for 3 seconds (the last messages to the old host are
  acknowledged for 5 s after leaving).
- Need for Speed III: picking the game's own Network (TCP) and then Oldkid.net Online in the same run left the
  game list empty.
- The drop-in's version resource read 0.8.15 in test builds; it is 0.8.8.

## Tested

- Need for Speed III over Oldkid.net Online on two PCs (Modern Patch host, portable joiner) and on Windows 98
  VMs with the 1998 OEM release: Online and LAN, host and join passwords, wrong password, Peer-to-Peer with a
  third player told the game is full, a Modern Patch player joining a 1998 host, a race, leaving mid-race.
- Not tested: the 1998 CD release in a running game (its copy protection hangs in the VMs), an internet race
  between two homes.
- Release gate: see the end of this file.

## Release gate (2026-11-25)

28 steps passed, 2 failed, none in changed code:
- `enumfilter_test`: Windows' own DirectPlay TCP/IP provider would not start on the test PC (the same as at 0.8.7;
  it fails before any of our code is reached).
- `run_local_full`, scenario `rogue (sessiondesc)`: our build refused the forged message as it should; the test's
  control build (made without the fix, to prove the test can fail) did not take the forgery this time either, so
  the scenario is marked failed. Not a change in behaviour.
Conformance in the same run: S1 5388 of 5388 (100%), S2 875 the same (29 deliberate), S3 lowest 94.6%, S4 full 8 of 8.
