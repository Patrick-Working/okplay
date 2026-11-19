# okplay's overlay - questions and notices drawn into the game's own frames

Version: plan 1, 2026-11-19 (wsock 0.3.0 = milestone 1). Universal: every game okplay supports; Need for Speed III
(portable 1.6.2 with DDrawCompat, 1.6.1, the 1998 CD) is the first that must work cleanly.

## Why

okplay asked its questions (Online / LAN / original, host password, join password, room list) in Windows dialog
boxes opened *inside* the game's network calls. A full-screen game stops drawing while it waits in such a call, so
whatever shows the game (Windows' full-screen mode, DDrawCompat, other wrappers) keeps a frozen picture, and the
box can only be forced onto the screen by tricks that differ per setup (docs/BOX_METHOD.md sections 10 - 11.2:
twenty test builds, each working on some setups and not others).

Two rules end that:

1. **Never ask inside a network call.** Answers exist before the call (chosen earlier, remembered, or handed over
   by the launcher). A call that cannot go on fails quickly with a reason, shown once the game draws again.
2. **Draw only as part of the game's own frame.** Just before the game shows a frame, okplay draws its overlay onto
   that frame. The game keeps the screen; nothing competes with it or a wrapper.

## How (universal parts)

- **Frame hook** (per graphics API; DirectDraw today in `wsock/ddshow.c`): the game's primary surface is a stand-in
  (made by our DirectDraw stand-in's CreateSurface); at its Flip the back buffer is drawn on through GetDC/ReleaseDC;
  a game that copies its frame onto the primary (Blt) gets the overlay on the copy's source first. Direct3D 8/9 games
  later get the same at Present.
- **Drawing** (`src/ovl.c`): into a device context of the frame, sized from the frame (640x480 and up), any colour
  depth GDI can draw on (8-bit through the surface's palette).
- **Input** (milestone 2): while the panel is open, keys and mouse go to the overlay and are held back from the game:
  the game window's messages (the existing message guard) and DirectInput (a stand-in for the device's
  GetDeviceState/GetDeviceData). A hotkey opens and closes the panel.
- **Flow** (milestones 3 - 4): a banner when the game starts ("Online Play - F11 to change"); the panel sets
  mode, host password, room to join; network calls only use these; refusals ("wrong password", "room full",
  "no answer") make the call fail fast and show as a notice in the next frames.

## Game-specific parts (notated per game, `docs/GAME_*.md`)

- Which renderer module to watch (NFS3: `drivers\dx7\dx7a.dll`), which input path (NFS3: DirectInput keyboard).
- Where a call can fail fast and how the game shows it (NFS3: its own "Failed to connect" box).
- Later, optional: questions in the game's own menus for builds we have studied (NFS3 study: private lab notes).

## Milestones

| # | What | Test |
|---|---|---|
| 1 | banner drawn into the frames for 20 s (wsock 0.3.0) | NFS3 portable with DDrawCompat, and without; stand-in (Blt path) passes |
| 2 | panel + hotkey, input taken from the game while open | NFS3 menus behind it do not move |
| 3 | settings in the panel: mode, host password, room list (from the room server) | host + join between two PCs |
| 4 | no questions inside network calls; fail-fast notices; old boxes only as fallback | full NFS3 online game; Road Rash |

Settings: `okplay.ini` `wsock_overlay=0` (off), `wsock_overlay_banner_s=N`.

## Investigation: hang after Host Game with 0.3.0 / 0.3.1 (Need for Speed III portable, DDrawCompat, 2026-11-19)

Facts: the banner draws fine (milestone 1 works); clicking Host Game then hangs hard (0.3.0: reset needed;
0.3.1: hangs after the first popup appears). 0.2.16 - 0.2.21 (no screen-surface stand-in) never hung like this.

| Suspect | Status |
|---|---|
| S1: the game hands our stand-in for its screen surface to another DirectDraw/Direct3D call | **ruled out for the normal setup** by a static study of the game's renderer (full screen with a back buffer: the screen surface is only ever the object of calls - Flip, GetAttachedSurface, Lock/Unlock, IsLost, GetSurfaceDesc - never an argument; no copy from the screen). Only its single-page mode (no back buffer) passes it to Direct3D CreateDevice / SetRenderTarget: make no stand-in for a screen surface without DDSCAPS_FLIP |
| S2: our drawing into frames | 0.3.1 draws only during the banner's 20 s; the user's run hung after Host Game - whether the banner was still up is not known yet |
| S3: the stopgap's box drawing (0.2.21) on its own | not tested alone on the laptop - test: `wsock_overlay=0` |
| S4: box drawing + stand-in together | open |
| S5 (new, from the study): the renderer can post a message to the game window and wait for it (to set the mode or restore lost surfaces); a game window thread that does not answer then hangs both | open - a level-3 okplay log of a hung run decides |

Next evidence: the okplay logs of the hung runs (last lines), one run with `wsock_overlay=0`, and DDrawCompat's
own log (`LogLevel = debug` in `DDrawCompat-nfs3.ini`). Hardening already known from the study: no stand-in for a
screen surface without a back buffer; the renderer's windowed mode copies each frame with Blt (overlay goes on the
copy's source, done in 0.3.1).

Evidence 2026-11-19, 3:55 PM (laptop logs): the banner was already gone when Host Game was clicked (so S2, our
drawing, is out). Two 0.3.0 runs: the first popup (mode) works; during an open popup, after the game window gets a
position/activation change (the player clicking or pressing Alt+F4 because the popup was not seen), okplay's popup
drawing fails - "the box could not be drawn into the game's screen (no GDI surface / no DC)" - and the game hangs
around the popup's end. Baseline: 0.2.2 (11-11, FlipToGDISurface method) hosted and took a joiner on the same laptop
without a hang. Points at the popup path losing the game's screen (surfaces lost on a focus change) and the
renderer waiting on its window to restore them (S5), with or without our stand-in (S4) - test 1 decides.

Test 1, 2026-11-19 3:54 PM: overlay off (`wsock_overlay=0`) - mode popup, password popup, room hosted and listed,
**no hang** (one blink when a popup first shows). The popup path alone (S3) is fine. The log also shows the game was
run from `Downloads\NFS 3 HP` with **0.3.0**, as were all the hung runs: 0.3.1 (which stops touching frames after the
banner) has not actually been run yet. Next: 0.3.1 in that folder with the overlay on - no hang would put the cause
on 0.3.0's drawing into every frame all game long (S2).

Test 3 (0.3.2 diagnosis build), 2026-11-19 4:04 PM: the first popup froze this time. The watcher's report fired
early (its timer wrapped: "4294967280 ms" - a bug in the diagnosis, fixed next build), but its snapshot of the popup's
thread is real: the thread was inside **DDrawCompat's own window hooks** (USER32 -> DDRAW.dll+0x1e9810/+0x1eb09d ->
USER32 -> win32u), i.e. DDrawCompat handles every message of okplay's popup window (it tries to draw it into the
game, GdiInterops) and was waiting there on another thread. Our Windows dialog and DDrawCompat's window machinery are
tangled: it holds with the overlay off (test 1) and breaks with the overlay's stand-in in the mix.

Decision proposed: stop patching the Windows dialog over DDrawCompat. Interim default: overlay off (test 1's
configuration, which works). Next: milestones 2 - 4, where the overlay replaces the dialog over a wrapper entirely -
okplay draws the panel into the frames and reads input from the game window's messages, so there is no window of
ours for DDrawCompat to hook, and nothing is asked inside a network call.

0.3.3 (overlay off, popups only), 2026-11-19 4:12 PM: hung at the first popup, right after it was drawn and made
see-through - with the overlay off. So the hang is in the DDrawCompat popup method itself (draw into the screen +
see-through layered window, which DDrawCompat handles with its own layered-window machinery), and it is intermittent
(test 1 passed with the same method). Baseline: the 0.8.7 shim (wsock 0.2.2, FlipToGDISurface, popup takes the
front, no wrapper handling) hosted fine on this laptop on 11-11. Test 4: 0.3.3 with `wsock_box_wrapper=off` +
`wsock_box_front=1` (the 0.2.2 way), three launches.

Test 4, 2026-11-19 4:14 PM (0.3.3, `wsock_box_wrapper=off` + `wsock_box_front=1` = the 0.8.7 method): mode popup,
host password, room hosted and listed, back to the menu, mode popup again, connection-reports popup, room list -
**no hang** in the whole run. Only the room list ("Looking for games") misbehaved: two mouse pointers, focus going
back to the game ("box: no longer the active window"), Cancel hard to click; the game window's thread paused
0.75 - 1.5 s at times meanwhile ("stall" lines). Conclusion: the 0.8.7 method is the safe base over DDrawCompat;
everything after it (beta 1's out-of-front box, the wrapper drawing, the see-through window) caused the hangs.
