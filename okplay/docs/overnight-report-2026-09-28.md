# Overnight report — Oldkid.net Play v0.2.0

## Your to-do list (about 15 minutes)
1. **Upload** everything in `D:\aoe2tool\deliverables\upload to oldkid.net (v0.2.0)\` to
   oldkid.net's `/aoe2net/` folder: `okplay.php` (replaces the old one), `index.html` (new
   page: live games + download), `Oldkid.net-Play-v0.2.0.zip`. Then tell me — I'll run the
   17-check conformance test against the live site.
2. **Install on both PCs**: run `Oldkid.net Play v0.2.0\Oldkid.net Play.exe` → **Update** →
   Yes on the Windows prompt → all green → **Launch Game**. v0.1 and v0.2 can't play together.
3. **Try LAN Play** in game (Multiplayer → Oldkid.net LAN Play), then Online Play again.
4. **Intro videos** (optional): `AoE2 intro video fix (your PCs only)\Apply video fix.cmd`,
   start the game, watch the intros. `Restore original videos.cmd` undoes it.
5. If anything misbehaves: launcher → **Collect logs for support** → bring me the zip.

## What changed
- **Your logs looked healthy**: hotspot games punched straight through the carrier NAT (IPv4)
  and also connected over IPv6; LAN game fine; no crashes or errors. One fix: players who left
  were probed forever — now forgotten when they leave.
- **Found and fixed a real join bug**: Windows' DirectPlay never re-sends its join message, so
  one lost packet = a 30-second failed join. okplay now delivers DirectPlay's own messages
  reliably. Joins now succeed at 15% loss (8/8) and 30% loss (6/6) in tests.
- **Oldkid.net LAN Play**: second connection type, finds games by broadcast, zero internet.
  Tested with the internet parts deliberately broken.
- **Launcher** (`Oldkid.net Play.exe`, 58 KB): green checks for DirectPlay / Online Play / LAN
  Play / oldkid.net server, Install/Update/Repair (one prompt), Launch Game, logs, uninstall.
  Replaces all the .cmd/.ps1 scripts.
- **oldkid.net page**: live game list (name, players, waiting/in progress), download button,
  3-step instructions. Public list never shows IP addresses.
- **WAN-IP lookup** now has fallbacks: 4 STUN servers (3 operators, one on port 443) →
  oldkid.net's view of your IP → addresses other players report.
- **Intro videos**: cause = Indeo 5 codec removed from Windows; re-encoded to Cinepak (built into
  Windows), verified decodable, quality 35.7–41.6 dB vs originals.

## Not verified yet (needs you / real hardware)
- The launcher's first real admin install (dry-run covered files, registry, permissions,
  shortcuts; the DirectPlay-feature and firewall commands only run for real).
- LAN Play and the new reliable lane inside the actual game.
- The intro-video picture inside the game window (cnc-ddraw).
- The "players report your address" fallback (needs real public addresses).

## Explorations
- `D:\aoe2tool\okplay\docs\wine-dplayx-exploration.md` — Wine's DirectPlay could replace
  Windows' (loads from the game folder, no Windows feature); main gaps and a 5-step path.
- Video fix for the public pack: recommend a small Indeo decoder DLL (~1–2 MB, touches no game
  files) instead of shipping 71 MB of Microsoft's videos.
