# Necrodome and Online Play

Necrodome (Raven Software / SSI, 1996, Windows 95): car combat with a driver and a gunner; network play
through DirectPlay (DirectX 3: it imports DPLAY.DLL, `DirectPlayCreate` and `DirectPlayEnumerate` by
ordinal) and TEN. First DPLAY.DLL game we test for real (2026-10-22). Status: **works over Online Play:
a Windows 98 VM hosted, this Windows 10 PC joined, lobby chat both ways, and both played the same match
(2026-10-22).** The GOG exe needs two game fixes, in memory only (below).

## Versions

| version | exe | multiplayer |
|---|---|---|
| GOG 2023 (SNEG), installer `setup_necrodome_1.2_(65863).exe` | `NECRO95.EXE` 795136 bytes, SHA-256 `0300090e6d6a8852e1a3927b9aa1e84459cf62ff9027ed222204e5bc00b3da7e`, PE timestamp 0x325acc81 (1996) | store page: single-player only. The exe's provider list callback has its two calls replaced by NOPs (below) |
| Steam 2023 (SNEG, app 2464680) | not seen yet | same symptom; players restored it with a patched exe, Radmin VPN, `DPWSOCK.REG` and DirectPlay DLLs copied to system32 ([forum, Oct 2023](https://steamcommunity.com/app/2464680/discussions/0/3875968426430661935/)) |
| community patch from old-games.ru (repackaged on the [GOG forum](https://www.gog.com/forum/necrodome/having_mouse_problems_click_here_necrodome_unofficial_patch)) | patched exe | "Fixed mouse. Fixed multiplayer. AudioTracks ... in ogg", crash messages removed |
| original CD (1996) | - | not available to us |

**Why vanilla GOG has no multiplayer on Windows 10 (measured 2026-10-22):** two things. The exe's provider list
is NOPed (below), and the `dplay.dll` GOG ships next to it is Windows' standalone DirectPlay 1.1 (5.00.2134.1,
a Polish build) that needs DirectX 2's `system32\dpwsock.dll` for TCP/IP - Windows 10 has none; DirectPlayCreate
then shows a hidden "cannot load the DirectPlay service" box. Not documented anywhere we found; The re-release plays its CD
music from ogg files through `_inmm.dll` like the community patch, so it may be built on such an exe; the
NOPs fit "crash messages removed" taken too far. An inference, not a fact.

## What the GOG exe does (the game's own code)

- Network Game -> name box -> `DirectPlayEnumerate(callback 0x44c16c)`. The callback should
  `sprintf(buf, "%s %d.%d", name, major, minor)` (its sprintf, 0x495ccd) and add the provider to its list
  (0x44c8f4: max 20 entries of name[60] + GUID, skips "XBAND"). Both calls are 5 NOP bytes, so the list
  stays empty and the game returns to the menu - with Microsoft's DirectPlay too.
- Picking a provider: `DirectPlayCreate(guid)`, then `IDirectPlay::GetCaps` with a 28-byte (DX3) DPCAPS;
  failing that it shows "No capabilities available for service (result=...)". A provider named MODEM sets
  a modem flag.
- Main thread stack: 64 KB reserved in the header (Windows 98 gave 128 KB; measured 1.4 KB in use at the
  first DirectPlay call - not a problem, checked).
- Imports `_INMM.dll` (GOG's renamed WINMM: music from ogg, needs SSE and libvorbis) and `DDRAW.dll` (GOG
  ships a DirectX 1-7 wrapper `ddraw.dll` + `dxcfg.ini`).

### Its network code (found while fixing "CALCULATING NETWORK LATENCY")

- Every packet goes through `Send(from = my id, to, DPSEND_TRYONCE (4), data + a sum-of-bytes checksum
  byte)` and `Receive(DPRECEIVE_TOPLAYER)`; a bad checksum prints "bad checksum" and drops the packet;
  system messages (from 0) are ignored.
- Its own reliable layer on top: `08 <seq dword> <len word> <payload>` is acked with `18 <seq>`; control
  messages are `00 <type> ...` (jump table at 0x44e0bc). The sequence numbers live at
  **`0x5a0a20 + player id * 8`** (send, receive) - a table of 200 entries, so it needs player ids below 200.
  An old sequence number is acked and dropped, which is how the stall showed up.
- Receiving: `while (WaitForSingleObject(player event, 0) == 0) { Receive one; if nothing usable,
  ResetEvent }` - it reads only while the event from `CreatePlayer` is set.
- Start of a match ("CALCULATING NETWORK LATENCY", net state at 0x5a130c): state 3 - everyone broadcasts
  `00 05` + five dwords every 5 s and answers each with `00 0a`; 20 timed pings (`00 07 <time>` / `00 08
  <time>`, reliable); the host sends `00 09 <latency>`; state 3 again; then the host sends `00 06` every
  5 s until every player has answered `00 0a` (state 4 -> 0). A joiner's first `00 05` of the second
  round can arrive before the host is back in state 3 and is ignored - the 5 s resend covers it (the game's
  own race, also on Windows).
- **Why it never ended with our ids:** we numbered players from 1000 (Wine's scheme: 0x3ea, 0x3ec), so
  the counters landed on other game data (0x5a2970 and up). When that data changed, the host's receive
  counter for the joiner ran ahead, the joiner's last `00 0a` was acked as a duplicate and dropped, and
  the host sent `00 06` forever. Windows' own ids are larger still: DirectPlay 1 on Windows 10 hands out
  0x12cd979f, 0x12d9554d... (measured, `apistate dp1create`, "raw ids" line), so `id * 8` points far outside
  the exe - very likely why multiplayer did not work on modern Windows and the re-release hid the
  connection list. An inference from the code, not tested on Microsoft's DirectPlay.

### Player ids, measured (black box: `okplay/tests/dp1ids.c`, our harness calling the DLLs)

| DirectPlay | ids for two players |
|---|---|
| DirectPlay 1.1 `dplay.dll` (GOG's, 5.00.2134.1, IPX provider) | 1, 2 |
| Windows 10 dplayx, IDirectPlay | 0x12cd979f, 0x12cd979c (keyed, different each run) |
| ours before 2026-10-22 | 1002, 1003 |
| ours through our dplay.dll now | 2, 3 (our system player takes 1) |

The game was written for DirectPlay 1.1's small ids.

### Two more DirectPlay 1 questions, answered on Windows (`okplay/tests/dp1drop.c`, Windows' TCP/IP provider on one PC)

- **A joiner that vanishes (process killed):** Windows' dplayx host reports nothing in 90 s - a DirectPlay 1
  session has no DPSESSION_KEEPALIVE (Open sets flags 0x2), so the player stays. Ours: the same (our
  keepalive runs only with that flag). In Necrodome's lobby the dead player then stays listed and a match
  would wait for it; the host has to make a new session. DirectPlay 1.1's behaviour is not measured: two
  ipxwrapper instances on one PC share one IPX node address, so it needs two Windows 10 machines.
- **EnableNewPlayers:** Windows sends the other machines nothing - not for four calls that change nothing,
  not for FALSE then TRUE (the joiner read through IDirectPlay and IDirectPlay4). Ours sent a
  SESSIONDESCCHANGED every time (Necrodome's host calls it 4 times a second); fixed, now the same as Windows.

## What we changed in the drop-in

- **Game fix** (`winedplay/shim/gamefix.c`), two parts, both for the exact GOG exe only:
  1. **player ids 1-199** (`max_id = 199` -> `DP_LimitObjectIds` in dplay.c): our id counter starts at 1
     and stays below the game's table (wraps to 1 with an ERR in the log after 199 ids in one run; a lobby
     uses two per player).
  2. the provider list: restores the two calls in memory only, only for the exact
  GOG exe above (name, size, SHA-256, PE timestamp, expected bytes at and around both places, the target
  functions' first bytes). Anything else is left alone and logged ("found, but a different exe (SHA-256 ...)
  - left as it is"); `okplay.ini game_fixes=0` turns it off. Test: `okplay/tests/gamefix_test.sh`.
- **Windows 9x/XP crash in every DPLAY.DLL game** (`winedplay/shim/winedbg_shim.c`): `__thread` (a .tls
  section) gets no slot in a DLL loaded with LoadLibrary after start-up on 95/98/ME/XP - and DPLAY.DLL games
  load dplayx.dll exactly so. Now TlsAlloc; `check9x.sh` rule 4 refuses .tls sections. Found with an SEH
  frame in a probe build of dplay.dll (the game's runtime catches faults before our unhandled-exception
  filter): c0000005 reading ffffffff in `__wine_dbg_strdup`.
- **DirectPlay 1** (`IDirectPlay`, all 25 methods) in `dplay.c` on the DirectPlay 2 code: v1 session
  description (sessions named by a DWORD), v1 system messages (ADDPLAYER 88 bytes, DELETEPLAYER,
  DELETEGROUP), DPCAPS 24/40 as Windows plus DirectX 3's 28 (Windows refuses it; Necrodome needs it - our
  one deliberate difference here; the flags of a 28-byte DPCAPS are ignored because the game reuses the
  struct), CreatePlayer making the event. Conformance: `apistate` states dp1host and dp1create.
- **Our dplay.dll numbers players 1, 2, 3...** like the DirectPlay 1.1 dplay.dll it stands in for
  (`OkpDplay11Numbering`, called by `shim/dplay_forward.c` before its first DirectPlayCreate). Every
  DPLAY.DLL game gets it, so other Necrodome exes play without a fingerprint;
  dplayx's own DirectPlayCreate keeps the dplayx numbering.
- **EnableNewPlayers sends nothing** to the other machines (as Windows, above).
- **The switcher's DirectPlay 1 Open** had DirectPlay 2's two arguments: each Open behind the switcher
  (trace=1, or a game creating its object with GUID_NULL while Windows' connections are listed) popped
  4 bytes too many and the game's later calls trampled its own locals (a CreatePlayer result read as
  0x006da073, a Receive crash). Now its own one-argument Open; `okplay/tests/dp1switch_test.sh`.
- **trace=1 sees a DirectPlay 1 game:** a connection of ours asked for by GUID is made as before and
  wrapped in the switcher (`SW_Wrap`), so its calls are logged.
- **Receive copies the caller's from/to in** (DPRECEIVE_TOPLAYER / FROMPLAYER filter on them): lobby chat
  and Ready were never delivered before.
- **EnumSessions holds a reference** while it runs: the game enumerates on its own thread and Cancel
  released the object underneath it (crash on Windows 98).
- Windows' own `DirectPlayEnumerate` reports 6.0 for every provider, as ours does ("... 6.0" in the list).

## Testing

- **This PC (Windows 10):** GOG install in `C:\games\NECRODOME` with the drop-in's `dplay.dll`, `dplayx.dll`,
  `okplay.ini` (GOG's Microsoft `dplay.dll` kept as `dplay.dll.gog-original`). It runs fullscreen at
  desktop size; okagent on the host can click it (`vmctl.py --agent <PC> click`): title -> click, Network
  Game (960,752), name Accept (960,742) at 1920x1080.
- **Windows 98 VM** (WIN98A, `C:\NECRO`): the GOG files without `ddraw.dll` (system DirectDraw), without
  the music folder, GOG's exe unchanged, `winedplay/tests/necrodome/inmm_win98.c` built as `_inmm.dll`
  (hands winmm calls to Windows; music off). Copy in with `okagent/putdir.py AGENT DIR C:\NECRO` (big files
  in 2 MB pieces). Clicks at 640x480: Network Game (320,336), Accept (320,330). The game uses 8-bit
  DirectDraw: okagent's in-VM screenshot is garbled then - capture the 86Box window from the host instead
  (`vmctl.py --agent <PC> shot out.png win98-agent`), and its error boxes hide behind the game.
- **A match** (2026-10-22): VM hosts - title clicks (320,240) x4, Network Game (320,336), Accept (320,330),
  Online Play (130,95), OK (466,357), session name box (222,336), create (445,336); PC joins - title
  (960,540), Network Game (960,752), Accept (960,762), Online Play (390,226), OK (1320,796), the session
  (640,214), join (1240,266); PC Ready (1466,840), VM start (545,376). Tracing: `DPLAYX_DEBUG=trace+dplay`
  in the PC's environment, `vmctl.py setenv DPLAYX_DEBUG=trace+dplay` before `run` on the VM; every DP1
  Send/Receive is then logged with its bytes. Keep 86Box minimised while clicking the fullscreen PC game
  (it grabs the mouse otherwise; Ctrl+End releases it).
- LAN Play between this PC and the VM is not possible as the VM is set up: 86Box's NAT (10.0.2.15) keeps
  broadcasts out; it needs bridged networking.

## Next

1. The Steam exe: likely the GOG build; with the new numbering only its provider list needs a fingerprint.
2. LAN Play PC vs VM once 86Box has bridged networking; a 3-4 player match.
3. Whether DirectPlay 1.1 dropped a vanished player (two Windows 10 machines with DirectPlay 1.1's own stack).
4. A GOG match with `trace=1` on the PC and the final DLLs (36c533d, 4b8e412). Not run on 2026-10-22: the
   PC's monitor was not detected (Windows at 1024x768 on "Default Monitor"; DirectDraw could not start
   once, and AnyDesk's Desktop Duplication capture stopped - Win+Ctrl+Shift+B made AnyDesk fall back to GDI).
   What the run would check is covered: dp1switch_test (the switcher with trace=1), dp1ids, the second exe's match
   on the numbering build, the quick suite. The Windows 98 VM can go into standby when idle and stop
   answering okagent; a hard reset from 86Box brings it back (it stops at a network logon box - Escape).
