# Could Wine's DirectPlay remove our dependency on Windows' DirectPlay?

*Exploration only (2026-09-28). Nothing here is built yet. Sources: Wine master (`dlls/dplayx`,
~11,900 lines of C), our own logs from real AoE2 games, and checks on the dev PC.*

## Why this matters

Everything in Oldkid.net Play sits on top of Windows' `dplayx.dll`, which only exists when the
**DirectPlay** optional feature ("Legacy Components") is turned on. It has been deprecated
since DirectX 9 (2004) and has been an optional feature since Windows 8. Microsoft hasn't announced
removing it, but users already report installs where "Legacy Components" is missing, and Microsoft
has removed other old components (the Indeo codecs our intro videos need were one of them).
If it goes, AoE2 multiplayer goes with it — for everyone, not just us.

## What Wine has

Wine ships its own open-source (LGPL 2.1) reimplementations of the whole DirectPlay family:
`dplayx` (DirectPlay 1-4 + Lobby — the one AoE2 uses), `dpwsockx` (the TCP/IP provider),
`dpnet`/`dpnaddr`/`dpnlobby` (DirectPlay 8), `dpnhpast`/`dpnhupnp` (NAT helpers), `dpvoice`.

`dplayx` is actively maintained: a large 2023-2024 series by Anton Baskanov implemented joining,
the player/group name table (`SUPERENUMPLAYERSREPLY`), groups, pings and UDP delivery, each with
tests checked against real Windows behaviour; there were fixes as recently as July 2026.

## Can AoE2 load it instead of Windows' copy? Yes, by the look of it

| Question | Finding |
|---|---|
| Does AoE2 find `dplayx` by name so a game-folder copy wins? | Yes: it imports `DPLAYX.dll` by ordinal 1, 2, 4. |
| Do Wine's ordinals match? | Yes: 1 `DirectPlayCreate`, 2 `DirectPlayEnumerateA`, 4 `DirectPlayLobbyCreateA` (same as Windows). |
| Is `dplayx.dll` a protected "KnownDLL" (can't be overridden)? | No (checked this PC: 37 KnownDLLs, no dplayx). |
| What does it depend on? | Only standard Windows DLLs: ole32, user32, advapi32, winmm (+ static dxguid/uuid). |
| Wine-only code to replace? | Just the logging macros (`wine/debug.h`: TRACE/FIXME, `debugstr_guid`) — a small shim. |
| Does it call our provider the same way? | Yes — same SP interface (we already use Wine's `dplaysp.h`), and it answers session searches through the SP's `Reply` callback, so our "capture the host's enum reply" trick carries over. |

So the mechanics look straightforward: build Wine's `dplayx` as a normal 32-bit Windows DLL, drop
it next to `age2_x1.exe`, and the game uses it — the Windows feature is never touched.

## The gaps (what would break or need work)

1. **Windows' reliable "DirectPlay protocol" is missing.** AoE2 creates its sessions with
   `DPSESSION_DIRECTPLAYPROTOCOL` (flags `0xC044` in our logs); Windows then wraps DirectPlay's own
   messages in its packet/ack protocol (commands 48/49 in our logs). Wine has no such layer — it
   expects the service provider to deliver "guaranteed" messages reliably.
   *Our position is good here:* okplay 0.2 already has its own reliable lane for DirectPlay's
   messages. We'd extend it to honour `DPSEND_GUARANTEED` on any message.
2. **No mixing.** Because of (1), a player on Windows' dplayx and a player on Wine's dplayx can't
   share a game (Wine ignores commands 48/49). The fix is simple but absolute: the pack always
   ships Wine's dplayx, so every player uses the same one.
3. **Known unfinished corners** (from Wine's own FIXMEs): host migration ("need to migrate all
   remotely visible players"), `SetPlayerData`/`SetPlayerName` changes not sent to other players,
   `GetPlayerAddress` stubbed, parts of the Lobby (`EnumLocalApplications`, message-thread
   shutdown) marked partial. Whether any of these matter depends on what AoE2 actually calls.
4. **Admin rights don't fully go away.** Wine's dplayx still reads the provider list from the
   registry (HKLM) — but since we'd ship our own copy, it could simply know about okplay built in.
   The remaining admin need is the firewall rule for LAN hosting (unsolicited LAN discovery packets);
   Online Play's hole punching works without one.

## Bonus: Linux and Steam Deck

Under Wine/Proton, AoE2 already uses Wine's dplayx. okplay.dll is a normal DirectPlay provider, so it
should load there too once registered in the Wine prefix. With everyone on Wine's dplayx (point 2),
Windows and Linux players could share one game.

## Suggested path (when you want to build it)

1. **Call census (small):** a logging stand-in `dplayx.dll` in the game folder that forwards to
   Windows' real one and records every DirectPlay call AoE2 makes. Turns gap (3) from guesses
   into a checklist.
2. **Build Wine's dplayx for Windows** with MinGW plus a logging shim; load it from the test folder.
3. **Run our existing test suite against it** (pair, 3-player mesh, 15%/30% loss, LAN, IPv6) —
   the harness can already point at any dplayx.
4. **okplay changes:** reliable delivery for `DPSEND_GUARANTEED`, built-in provider registration.
5. **Real AoE2 games**, then ship it as the default ("no Windows feature needed"), keeping Windows'
   dplayx as a fallback switch. Fixes worth upstreaming (e.g. host migration) can go back to Wine.

**Risk summary:** the loading mechanics look low-risk; the real unknown is how much of AoE2's
DirectPlay usage hits Wine's unfinished corners. Step 1 answers that cheaply before committing.
