# How okplay shows its boxes over a game

Document version 3 (2026-11-16; section 11 added). Study of the code as it stands for the box fix that follows 0.8.7 (not yet released).
Covers the Winsock shim games: Road Rash (UDP) and Need for Speed III (TCP).

This is the method every box must follow, for every game we add. It has four parts:
what we do today, the rules, what is universal and what belongs to one game, and how a
box is tested before a player sees it.

---

## 1. Why boxes over games are hard

A full-screen game from the 1990s assumes it is the only thing on screen. Our box breaks
that assumption in five ways, and every box problem we have had is one of these:

| # | What goes wrong | Seen as |
|---|---|---|
| A | **The game loses the front.** It minimises, goes black, or cannot come back. | Road Rash black screen on Windows 98; NFS3 minimised on Windows 11 |
| B | **The box is not visible.** The game owns the screen page and draws over us. | NFS3 "Connecting..." invisible; game redrew over the box |
| C | **Two threads wait on each other.** The box thread and the thread of the game window. | Windows 98 hang (Win16 lock); NFS3 room list frozen, a 0.3 s connection taking 16 s |
| D | **The game keeps running under the box.** It re-enters our code, opens more boxes, or its clock runs out. | Five boxes open at once; LAN picked but Online ran; Road Rash join timed out |
| E | **Input goes to the wrong place.** No pointer, pointer trapped, Enter pressed for the game answers our box. | Hidden pointer on Windows 10/11; Enter opened the browser; pointer jumped through the room list |

A box is correct only when it is safe against all five, on every Windows we support.

---

## 2. What we do today

All boxes are built by one shared builder (`src/okbox.c`), used by both the DirectPlay
drop-in and the Winsock shim. The Winsock shim adds a runner around it (`run_box` in
`wsock/wsock.c`) and, for DirectDraw games, the screen code (`wsock/ddshow.c`).

### 2.1 The boxes

| Box | Road Rash | NFS3 | When |
|---|---|---|---|
| Mode (Online / LAN / original) | yes | yes | first network use |
| Host password | yes | yes | when the game starts hosting |
| Join password | over the game's own room list | yes | joining a locked room |
| Wrong password | yes | yes | after a refused password |
| Build mismatch | yes | yes | joining a room of another build |
| Room list / "Connecting..." | (the game has its own) | yes | Join without an address |
| No answer, Room full | no | yes | after a failed join |
| Update, server unreachable, network unavailable, crash report, connection reports | yes | yes | queued, shown at a safe moment |

### 2.2 Road Rash (UDP)

1. The box opens on a thread that owns game windows. (An own thread deadlocked on Windows 98.)
2. A message filter on that thread drops the game's timer, key and mouse messages while the box is open, so the game cannot re-enter.
3. The pointer is shown and freed (`okbox_cursor_begin`).
4. The box is topmost, **takes the front**, and gets the keyboard the normal way.
5. On Windows NT the game's other threads wait at their next network call until the box is answered.
6. The join password is asked over the game's own room list, before the join starts, with a wait cursor for up to 4 s.
7. On close: filter removed, pointer put back.

Road Rash has **none** of the full-screen protection NFS3 has: no screen-page switch, no guard on the game window, no restore of the front afterwards.

### 2.3 Need for Speed III (TCP)

1. The box opens on a network thread of the game, **not** the thread of the game window.
2. The screen is switched to the page Windows can draw on (`FlipToGDISurface`).
3. A guard is put on the game window: it hides "you lost the front" messages from the game, keeps the pointer an arrow, and stops the game moving or trapping the pointer.
4. Windows NT (new in the 0.8.7 fix): the box **never becomes the active window**. The game keeps the front. The mouse works as it is; the keyboard is read by a hook and handed to the box. `wsock_box_front=1` in okplay.ini brings the old way back.
5. Windows 9x: the box takes the front the old way; the guard keeps the game from hearing about it.
6. On close: hooks removed, pointer put back, the game is given the front again.

### 2.4 Settings that change box behaviour

`wsock_mode`, `use_password`, `build_check`, `wsock_join`, `update_check`, `crash_reports`,
`connection_reports` (each can skip its box); `wsock_box_focus` (auto / release);
`wsock_box_front` (0 / 1); `simple_textboxes`.

---

## 3. The rules

These are the rules a box must obey. Each is marked **[U]** universal or **[G]** game-specific.
"Today" says whether the code already does it.

### Rule 1 - The game never loses the front. [U]
Over a full-screen game the box is a topmost window that is never activated; the game stays
the active window from start to end. Where that is not possible (Windows 9x), the game must
not hear that it lost the front, and it gets the front back when the box closes.
*Today: NFS3 on NT yes. NFS3 on 9x by hiding the messages. Road Rash no - the box takes the front.*

### Rule 2 - The box thread never waits for the thread of the game window. [U]
No call from the box that Windows answers through the game window (activating, focus,
owner windows, `SendMessage` without a time limit). Anything we must tell the game window
is posted or sent with a time limit.
*Today: NFS3 on NT yes, with one known hole (pointer message, see 6.7). Road Rash: the box is on a game thread, so the rule is met by being the same thread.*

### Rule 3 - The game stands still, on purpose, while a box is open. [U]
Exactly one box at a time. Other game threads that reach our network code wait. The game's
own messages on the box thread are held back. If a box is asked for while one is open, the
second waits or is refused - it never opens over the first.
*Today: partly. The runner does not check for a box already open (6.2), and two Road Rash boxes do not use the runner at all (6.3).*

### Rule 4 - A box is asked before the game's clock starts, not during. [U] with [G] timing
A question that must be answered goes before the game begins the step it belongs to
(password before the join, mode before the first connection). Each game profile states its
safe moments and the game's time limits.
*Today: yes for Road Rash (password over the room list) and NFS3. The moments are in code, not written down per game.*

### Rule 5 - Notices wait for a safe moment. [U]
Anything that is not a question for this step (update, reports, server unreachable, full,
no answer) is queued and shown only when no box is open and no race is running. Never on
the thread of the game window from a network call the game did not expect to be slow.
*Today: yes, with one open point - a notice may open from a socket call we do not handle (6.10).*

### Rule 6 - The player can always see it and always answer it. [U]
Visible on top of the game; pointer visible and free; keyboard works (Enter, Esc, Tab,
typing); if any of the three cannot be guaranteed, fall back to the plain front-taking box
rather than show a box that cannot be answered.
*Today: no fallback. If the keyboard hook fails the box stays up without keys (6.6). If the screen switch finds nothing, the box silently takes the old path (6.5).*

### Rule 7 - A key meant for the game never answers a box. [U]
The default button is always the harmless one. Enter never downloads, never sends a report,
never opens a browser. Nothing opens a browser over a full-screen game on Windows 9x.
*Today: yes.*

### Rule 8 - Everything is put back. [U]
Pointer count, pointer trap, hooks, window styles, the screen page, the front. Put back on
the same thread that changed it. Checked by the log: every "begin" line has its "end" line.
*Today: mostly. Pointer count across threads and the saved pointer trap have holes (6.8, 6.15).*

### Rule 9 - Windows 9x is its own path, tested on its own. [U]
No hook-based keyboard, no waiting threads, no Unicode calls, no browser. Any new box code
states what it does on 9x, and the 9x path is chosen by one switch that the test knob
(`OKP_EMULATE_9X`) also reaches.
*Today: the paths exist; the switch is split in two (6.11), so the PC test of the 9x path covers only half.*

### Rule 10 - Every box says in the log what it did. [U]
One line when a box opens (which box, which thread, which path: no-front / front / 9x),
one when it closes (answer, how long), and one for each thing put back. A stall of the game
window over 0.7 s is logged with where it stood. A player's log must be enough to judge a box.
*Today: yes for NFS3 since the test builds; Road Rash boxes log less.*

### Rule 11 - Game-specific code lives in the game's file and says so. [G]
Universal code goes in `okbox.c`, `ddshow.c` and the runner. Anything true for one game only
goes in that game's file (`rr_fix.c`, `nfs3_fix.c`) or its profile entry, with a comment line
starting `[game: name]`. Universal code learned from one game says `[universal; found with: name]`.
*Today: the comments exist in free form; the screen code calls itself universal but only the TCP kind switches it on.*

---

## 4. Universal and game-specific, side by side

| Part | Universal | Game-specific |
|---|---|---|
| Look, texts, sizes, fonts, safe default buttons | all | title (`brand`) |
| The runner: one box at a time, other threads wait, messages held | all | - |
| Screen page switch + guard on the game window | any DirectDraw full-screen game | which library creates DirectDraw (NFS3 Modern Patch: `dx7a.dll`) |
| No-front box with hooked keyboard | any full-screen game on NT | - |
| Pointer shown / freed / put back | all | - |
| **Which thread** asks | - | Road Rash: a game-window thread. NFS3: a network thread |
| **When** each box is asked, and the game's time limits | the rule (before the clock) | Road Rash: over its room list, about 4 s. NFS3: at bind / listen / connect; the game gives up after about 2 s of failed sends |
| What the game cannot survive | - | Road Rash on Windows 98: any loss of the front = black screen. NFS3: activation changes stall its window thread |
| Room list | ours where the game has none | Road Rash uses its own list; NFS3 uses ours |

**Per-game sheet.** Every game gets these lines in its `GAME_*.md`, filled in before its
first box is shown to a player:

1. Screen: full-screen DirectDraw / windowed / other; which library creates it.
2. Threads: which thread makes the first network call, which owns the window.
3. Clocks: how long the game waits at join, at host, in a race.
4. Safe moments: where each box may be asked.
5. Cannot survive: what kills it (loss of front, browser, minimise).
6. Windows 9x differences.
7. Which universal parts are on, and any that are off and why.

---

## 5. How a box is tested

Today no automated test shows a real box: every test answers through environment variables
or runs with boxes off. So the five problems of section 1 are only ever seen by a player.
That is the gap this method closes.

### 5.1 A stand-in game (new test program)

A small full-screen DirectDraw program of our own, `tests/boxgame`, that behaves like the
hard cases and loads the shim like a game does:

- exclusive full-screen DirectDraw, flipping every frame, hidden and trapped pointer;
- **thread model switch**: network calls from the window thread (Road Rash shape), from a
  second thread (NFS3 shape), from a timer thread;
- **slow window thread switch**: the window thread answers messages only every 2 s
  (the NFS3 Windows 11 case);
- a join clock: fails the "join" if the network call takes longer than N seconds;
- writes its own log: every activation, focus, size and minimise message it receives,
  its frame count, and whether it was ever minimised.

### 5.2 What is checked, from the two logs

| Check | Pass when |
|---|---|
| Front (rule 1) | the stand-in saw no deactivate and no minimise; foreground window is the game after the box |
| Visible (rule 6) | a screen capture taken while the box is open contains the box |
| No waits (rule 2) | box open-to-usable under 0.5 s even with the slow window thread; no "stall" line |
| One box (rule 3) | never two "box opens" without a "box closes" between |
| Clock (rule 4) | the stand-in's join clock did not run out when the box was answered at once |
| Input (rules 6, 7) | synthetic keys: typing lands in the field, Tab moves, Esc cancels, Enter gives the safe answer; the stand-in received none of them; pointer visible and free |
| Put back (rule 8) | pointer count, trap and foreground equal before and after; all hooks removed |

Runs from a fixed folder, makes no sound, and takes the screen for a few seconds only -
so it is run on the VMs by default, and on this PC only when asked.

### 5.3 The matrix

Each box kind x thread model x {Windows 98 VM, Windows XP VM, Windows 10/11} x
{`wsock_box_front` 0, 1}. The full matrix is for a release; a change to box code runs the
row for the box it touched on all three Windows.

### 5.4 Then the real games

Only after the stand-in passes: one pass in Road Rash and NFS3 on the VMs (host, join with
password, wrong password, room list), then the test build goes to the player with a short
list of what to try. The player's test confirms; it should no longer be where we find out.

---

## 6. Open points found by this study

Ordered by how likely they are to hurt a player. None is changed yet.

1. **Road Rash boxes still take the front** (rule 1). On Windows 10/11 this is where the
   remaining Road Rash box trouble would come from. Decide: switch Road Rash to the no-front
   box on NT, after the NFS3 test confirms it.
2. **A box opened while another is open damages the first** - the runner has no check. Fix: one gate in the runner.
3. **Two Road Rash boxes bypass the runner** (join password on its own thread, late wrong password). Bring both under the runner.
4. **Message hold-back is not what the comment says**: timer and input messages are dropped, not kept for later. Decide which is wanted and make code and comment agree.
5. **Silent fallback** when the screen switch finds no DirectDraw object or no window. Log it loudly; choose the front-taking path on purpose.
6. **Keyboard hook failure leaves a box without keys.** Fall back to the front-taking box.
7. **Pointer message order**: a late "show" can arrive after the "hide" and leave the pointer on in the game.
8. **Pointer count across threads** can end one off.
9. **The screen is switched once.** If a game keeps flipping, the box can be covered again. The stand-in will show whether this happens.
10. **A notice can open from a socket call we do not handle**, possibly on the window thread.
11. **The 9x test knob reaches only half the code.** One switch for both.
12. Comment in `ddshow.h` says "keep" is for 9x; it is on for every Windows.
13. Small races on the shared "box open" flags.
14. **The keyboard hook is system-wide.** If the box thread stalls, typing on the whole PC lags. Keep the hook's lifetime short and the box thread free of waits (rule 2).
15. The pointer trap saved at open is restored even if the game changed it meanwhile.

Also still open from earlier work: Windows 98 join box costing the first join attempt in
Road Rash; blurred boxes at high DPI; NFS3 asking the mode once per launch; dragging a box
over a full-screen game (off by design for now).

---

## 7. Order of work proposed

1. Finish the NFS3 no-front box with the player's test (in progress).
2. Build the stand-in game and the checks of section 5.
3. Close points 2, 3, 5, 6 (the runner gate, the two stray boxes, the two fallbacks) and prove them on the stand-in.
4. Move Road Rash on NT to the no-front box; prove on the stand-in, then on the VMs.
5. Write the per-game sheet into GAME_ROADRASH.md and GAME_NFS3.md; mark the comments `[universal]` / `[game: ...]`.
6. Then the remaining points, and the release.

## 8. Status after the first round of work (2026-11-11, evening; nothing committed)

Tested with the stand-in game (boxgame) full screen on a Windows 98 VM. Not yet tested: the Windows NT way
(the box out of the front) over a real full-screen surface.

- 6.9 (box covered by a game that keeps flipping): fixed as an option. `okplay.ini wsock_box_flip=hold`
  [G: per game, default off] puts a stand-in in front of the game's primary surface and holds its Flip while a
  box is open. Flips stop before the switch to Windows' page (a flip that slipped in after it gave a black
  screen in the first try). Off by default: the stand-in surface is not safe when a game hands its primary
  surface to other DirectDraw / Direct3D calls (list in ddshow.c). No shipped game is known to need it.
- 6.2 (no re-entry gate): `run_box` now keeps one box at a time [U]. A box asked for inside an open one on the
  same thread is shown inside it and leaves the outer one's hooks and guard alone; one asked for on another
  thread waits (Windows NT, not the game window's thread).
- 6.6 (no keys if the keyboard hook fails): the box then takes the front the old way [U].
- 6.5, logs: a line when no DirectDraw object / game window is found; "box: opens (...)" and
  "box: closed (..., hooks removed)"; "ddshow: box over - guard off ..."; "ddshow: held N page flips".
- 6.7 (pointer show/hide order): the game thread applies the state wanted last, whatever order the messages
  arrive in [U].

Found by the stand-in, still open:
- When Esc closes a box that runs on the game window's thread, the key-up of that Esc reaches the game.
- UDP profile (Road Rash): the box is visible, but the game is told it lost the front (6.1, as known).
- boxgame `--net timer` on Windows 98: the stand-in itself starves its window thread after the box
  (its timer thread loops); a fault of the test tool, not of the box.
- boxgame_check.py: only messages after the "game: window" line count for the "front" rule.

## 9. First full-screen runs on Windows 10 (2026-11-11, night; nothing committed)

Run with the stand-in (boxgame) on a Windows 10 PC, logs and captures in `okplay/tests/out/boxgame/pc/` (h1-h9).

**Found: the no-front box did not keep the front with the game.** Windows moved the front off the game the moment
the box *asked* for the activation - before the WH_CBT refusal counted. The front then belonged to nobody: the key
hook passed every key on (its "another program is in front" test), so the box could only be answered by mouse.
The asker was the resize of the box in `okbox.c` (`SetWindowPos` without `SWP_NOACTIVATE`); `DS_SETFOREGROUND`,
`SetFocus` and DialogBox's own show would have asked next.

**[universal] Rule 12: a box that leaves the front to the game never asks for the activation.**
- every `SetWindowPos` on it carries `SWP_NOACTIVATE`; no `DS_SETFOREGROUND`; no `SetFocus`;
- it sets `WS_EX_NOACTIVATE` and shows itself with `SW_SHOWNOACTIVATE` in WM_INITDIALOG, and returns FALSE there;
- it tells the key hook its window and first control (`okbox_no_front_wnd`, `okbox_no_front_focus`);
- the WH_CBT refusal stays as the net (log line "box: asked for the front and was refused" = a new asker to find).
- the key hook also takes the keys when nothing at all is in front.

**[universal] Keys that can close the box (Esc, Enter, Space on a button) act when let go**, not when pressed:
before, the box and its hook were gone before the key came up and the game got that key-up. (No-front path only;
the Windows 9x path and the take-the-front path still have this - section 8.)

Results after the fix (Windows 10): host box and room list come up with the game still in front (no
deactivation message at all), usable at once even with the game window's thread answering only every 2 s,
Esc/Enter answer them, no key reaches the game, front and pointer as before afterwards. The box is visible
without the flip hold (capture h5.png: the game keeps drawing behind it). `wsock_box_front=1` also passes.
Windows 98 re-check with this build and `wsock_box_flip=hold`: all checks pass, 5 of 6 captures show the box,
one (the first run after the files were replaced) was black - not explained.

Open:
- join, slow game thread: the pointer count is back one late (asked of a thread that answers every 2 s) - check
  whether it settles; the room list came 2.4 s after the game's connect (two room-server fetches first, nothing on
  screen meanwhile) - show the list at once and fill it in.
- the one black capture on Windows 98 with the hold on.

## 10. Round of 2026-11-12 (after the 0.8.7 release; working version 0.8.8 / wsock 0.2.3)

All [universal], Winsock shim (`wsock.c`, `ddshow.c`):

- **One gate, in one step** (`box_claim` / `box_release`): "is a box open" and "then it is mine" happen under one
  lock, and the gate is given back last, after hooks and guard are put back. The join-password box on its own
  thread (Road Rash) goes through the same gate. A box that cannot wait (Windows 9x, the game window's thread)
  is shown beside the open one and touches nothing the open one set up.
- **A box inside a box gives the keys back** to the one under it when it closes (`na_take` / `na_gone`).
- **AltGr characters are typed into the box** (@ \ { } | and the euro sign on many keyboards), and **Alt+F4
  cancels the box** rather than reaching the game.
- **The stall sampler is off unless asked for** (`okplay.ini wsock_box_stall_log=1`): without it, a game window
  that stops reading its messages gets one log line and no thread is held.
- The players' `okplay.ini` now lists `wsock_box_front`, `wsock_box_flip` and `wsock_box_stall_log`.

Tested: 15 headless TCP scenarios that pass through the box runner (test answers, no window). **Not yet tested:**
the three key changes over a real full-screen surface (stand-in, then Need for Speed III), and Windows 98.

Still open from section 2.2 of the review: Shift/Ctrl selection and dead keys in a no-front box; a keyboard
hook Windows drops silently; Alt+Tab while a box is open; a key held when the box opens; mouse click does not
move the keyboard to a field; Road Rash on NT still takes the front (section 6.1).

### 10.1 Stand-in runs over a real full-screen surface (2026-11-12 evening, Windows 10, US keyboard)

`boxgame` full screen (640x480 exclusive), no-front box, NFS3 shape (TCP, network calls on a second thread):

| Case | Result |
|---|---|
| Alt+F4 on the mode box | the box is cancelled, the game keeps running and keeps the front; F4 never reaches the game |
| Alt+F4 on the room list of a joiner whose window reads its messages every 300 ms | the same |
| Host box: typed `Ab1x` + Backspace (Shift, digit, Backspace) | the field holds `Ab1`; no key reaches the game |
| Road Rash shape (UDP, calls on the window's thread; this box still takes the front) | Alt+F4 closes the box as Windows does; the game is not closed |

Found and changed by these runs: Alt+F4 now cancels on the key going **up** (cancelling on the way down let the
key-up reach the game after the box and its hook were gone). Shift, Ctrl and Alt alone still go on to the game on
purpose; `boxgame_check.py` (version 2) notes them and no longer fails on them, and it understands `{bs}`.
`boxgame --keys` knows `{altf4}` and types AltGr characters as Ctrl+Alt+key.

Still not tested: AltGr characters (needs a keyboard layout that has them: a VM with a German or French layout),
a box inside a box and two threads at the gate (no stand-in case stages them yet), Windows 98, and the real game.

## 11. DirectDraw wrappers (2026-11-16; wsock 0.2.4 - 0.2.9, tested on a laptop with the real game)

What we had missed: players' game packs often bring their own `ddraw.dll`. The Need for Speed III portable 1.6.2
ships **DDrawCompat 0.7.1** with `FullscreenMode = borderless`, `GdiInterops = all`, `AltTabFix = noactivateapp(1)`
and `CpuAffinity = 1` (every thread on one core; SilentPatch pins new threads too). Every stand-in run so far used
Windows' own DirectDraw, so none of them saw this. With the wrapper, beta 1's out-of-the-front box (section 10)
could not be seen or used, the system-wide keyboard hook stopped answering (Alt+F4 reached the game), and the
laptop had to be reset.

**Rule 9 (universal): find out what shows the game before choosing a method.** `ddshow.c dds_wrapper` looks at the
module the game's `DirectDrawCreate` is in (not "ddraw.dll" by name: the wrapper loads Windows' own one too) and
reads its version text (RT_VERSION, read in place). For DDrawCompat it reads `FullscreenMode` and
`EnableDDrawCompat` from `DDrawCompat-<exe name>.ini` beside the exe, then `DDrawCompat.ini` beside the wrapper.

| What shows the game | Method |
|---|---|
| DDrawCompat, enabled, borderless (its default) | **wrapper method** (below) |
| DDrawCompat with `FullscreenMode = exclusive`, or `EnableDDrawCompat = off` | the usual measures (sections 2, 10) |
| dgVoodoo, cnc-ddraw | the usual measures, and a log line "not tested with okplay's boxes yet" |
| Windows' own DirectDraw | the usual measures (unchanged) |

**The wrapper method** (one round of laptop tests per step, 2026-11-16):

| Step | Result |
|---|---|
| 0.2.4: plain box, takes the front, no guard | box usable, game went down (minimised) under it |
| 0.2.5: + guard (game not told it lost the front) | game stayed up, but its window covered the box |
| 0.2.6: + the box **owned by the game window** (`okbox_set_owner`) | works: box on top, keys, Enter, buttons; dragging left a "hall of mirrors" |
| 0.2.8: + the game's picture kept when the box opens, painted back in the game window's WM_PAINT | works, dragging clean |

So over a borderless wrapper: no screen-page switch, no out-of-the-front box, no system-wide keyboard hook; the
guard stays (activation messages kept from the game, a minimise asked by message refused, the game restored if it
went down anyway, the pointer looked after); the box is owned by the game window (an owned window always stays
above its owner; the game window takes no clicks meanwhile); a picture of the screen is kept for under the box
(`okplay.ini wsock_box_drag=0`: no picture, no dragging). `okplay.ini wsock_box_wrapper=off` turns the whole method off.

Not known yet (each needs a real run): the original 1998 exe and 1.6.1 without a wrapper on Windows 10/11 (they get
the usual measures, i.e. beta 1's method, which has only been run against the stand-in); dgVoodoo and cnc-ddraw;
DDrawCompat on other games; Windows 98 is not affected (no wrapper there, the 9x path is unchanged). The stand-in
cannot test the wrapper method: DDrawCompat 0.7.1 crashes boxgame.exe even without okplay.

### 11.1 Screenshot key (2026-11-16, wsock 0.2.10 - 0.2.11)

Windows' screenshot keys get nothing of a game DDrawCompat shows, and never the game and a box together. The shim
registers a key (default Ctrl+F12, `okplay.ini wsock_screenshot_key`, `off` to turn it off) on a thread of its own and
saves a BMP in `okplay-screenshots` beside the game: the game's picture with okplay's open boxes drawn in (PrintWindow).
0.2.10 read the screen from the key's own thread while a box was open over DDrawCompat: the game froze (laptop,
DDrawCompat AVs at ddraw.dll+0x2c7ca, no "screenshot:" line). 0.2.11: with a box open the key's thread posts the
work to the box's thread (taken in the box's message filter, `dds_shot_here`), and over a wrapper the picture kept
when the box opened is used (no new screen read); with a wrapper and no box open nothing is taken (logged); every step
is logged. Stand-in run here (no wrapper): taken on the box's thread, box drawn in, saved. Over DDrawCompat: not run
yet (DDrawCompat 0.7.1 crashes any program on this PC at "Installing DirectDraw hooks", so it cannot be tried here).
Test knob: `OKP_TEST_SHOT_MS=N` "presses" the key N ms after start.
