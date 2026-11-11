# Oldkid.net Online Play v0.8.7 - release notes

Released 2026-11-11. DirectPlay drop-in 0.8.7, Winsock edition (`wsock32.dll`) 0.2.2.
A fix release for the boxes okplay shows over a game. Nothing changed on the wire: players on 0.8.3
to 0.8.6 keep playing with 0.8.7 players.

## Fixed

- **No mouse pointer on okplay's boxes (Windows 10/11, Need for Speed III).** The game hides the
  pointer, so the Online Play / LAN Play box, the room list and the password box could only be used
  with the keyboard. The pointer is shown while one of our boxes is open and put back as it was when
  the box closes.
- **Oversized boxes.** With Windows' text size above 100 % and a game screen of 600 lines or fewer,
  the boxes filled most of the screen. The box font now follows the game's screen.

## Back end: what is shared, what is per game

- **Shared (every game, both editions):** `okplay/src/okbox.c` draws the boxes for the DirectPlay
  drop-in and the Winsock edition. Two helpers are new there: `okbox_cursor_begin/end` (show the
  pointer for a box, restore the game's state after it) and `okbox_font_pt` (the font size rule). The
  Winsock edition's room list has its own layout and uses the same two helpers.
- **Not covered:** the Age of Empires II style boxes keep their fixed 9 pt font.
- **Per game, noted:** the Online Play / LAN Play / Original question is asked again when the shared
  "new game" rule is met. Road Rash meets it after Disconnect; Need for Speed III never does, so it is
  asked once per launch (`docs/GAME_NFS3.md`, "When it is asked again").
- **Room server (`okplay.php`):** announces Winsock edition 0.2.2. No other change.

## Tested

- Need for Speed III on Windows 10: pointer, clicks and box size.
- Road Rash and Age of Empires II: The Conquerors on Windows 98 (VM): pointer, clicks and box size.
  Not tested on Windows 10 for these two games.
- Release gate of 2026-11-10 (night) on these binaries: 19 steps passed, 3 failed, none in the
  changed code: `test_names` (the test's expected name did not match its data; fixed here),
  `enumfilter_test` (Windows' own TCP/IP provider would not start on the test PC; the same with every
  released build), `lanfullroom` (once; passed when run again).
