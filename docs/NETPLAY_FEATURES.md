# Classic Netplay drop-in (Oldkid.net Online Play): feature list

State as of 2026-10-16. **Live** = in v0.8.0 at oldkid.net. **0.8.1** = released at oldkid.net on 2026-10-16 (okplay.php announces it, so 0.8.0 players are offered the update once).

## What it is
A replacement `dplayx.dll` (Wine's DirectPlay, ported to Windows and extended) that a player copies next to the game's exe. Classic DirectPlay games (DirectPlay 4 and older, DirectX 7 era and before) get internet and LAN play without port forwarding, VPN, admin rights or Windows' optional DirectPlay feature.

## Playing
- **Online Play** (live): rooms on the oldkid.net room server; players connect straight to each other (UDP hole punching, STUN, IPv6, LAN addresses tried first); the server only introduces them, game traffic never passes through it.
- **LAN Play** (live): broadcast discovery, no internet needed.
- **Classic connections kept** (live): `mode=all` lists ours first, then Windows' own TCP/IP, IPX, modem, serial; picking a classic one hands the game to Windows' DirectPlay. Also `oldkid`, `windows`, `ask` modes.
- **Game passwords** (live): host is asked for an optional password; joiners are asked when a game has one; the list shows a `[PW]` mark; only a scrambled hash leaves the PC.
- **Host migration** (live): if the host leaves, another player takes over (DirectPlay's own rule).
- **Transmit mode** (0.8.1): Fast / DirectPlay-style (the default) / All reliable, **chosen by the host** for the whole game (`transport=`), personal override `transport_force=`. All reliable delivers everything in order even at 30% loss, at the cost of stutter. A guaranteed message is never dropped for arriving in a burst: past the 128-message window it waits its turn (600 at once arrive complete and in order at 30% loss).
- **Game names on the room list** (0.8.1, needs the new okplay.php): the host sends its game's name (its DirectPlay registry entry, else the exe's product name) and version; the web page shows e.g. "Age of Empires II Expansion · 0.7.26.809" under each room. Rooms from older hosts get a name from their game id where it is known (AoK, AoC).
- **Version warning** (0.8.1, needs the new okplay.php): every drop-in fingerprints its game exe (SHA-256); hosts send theirs with the room, online and on LAN. Joining a host on a different exe shows "The host is running a different version of the game - Theirs / Yours - Join anyway / Cancel" (`build_check=off` turns it off). No list of versions needed: patched and modded exes are covered too. The web page shows the host's exe name next to the game.
- **Room status that knows** (0.8.1, needs the new okplay.php): the host judges what its room is doing - in the lobby, in a game, or between games (back in a lobby with the same players) - from its traffic as well as the session flags, because AoE2 never sets the flags when a match starts (the page used to show every AoE2 match as "Waiting for players"). The page shows Waiting for players / Full / In progress / Between games.
- **Riding out outages** (0.8.1): `drop_after=` in okplay.ini - how long another player may go silent before being dropped (15 s like Windows' DirectPlay by default, up to 120 s). Tested in a real AoE2 match: a 20 s outage with drop_after=30 and both players carried on.
- **Live version check** (live): the room server names the newest drop-in; older ones offer the download once, never blocking (`update_check=`).

## Game compatibility (the shim)
- **Mode switch and connection merging** (live): one connection list from our providers and Windows' (`shim/switch.c`, `mode.c`).
- **COM redirect** (0.8.1): games that create DirectPlay through `CoCreateInstance` (Midtown Madness) reach the drop-in, not Windows' copy (`com_redirect=1`).
- **Fixed-list games** (0.8.1): `classic_tcpip/ipx/serial/modem = online | lan | ask`; `ask` shows a 3-button box (Online / LAN / Original) at the first session search, after the game's own probing.
- **dplay.dll forwarder** (0.8.1): DirectX 3 era games that load `DPLAY.DLL` get the drop-in too.
- **Async sends** (0.8.1): `DPSEND_ASYNC` + `DPSYS_SENDCOMPLETE` exactly as Windows answers them.
- **Windows-parity fixes** (0.8.1): ~25 behaviours measured against Windows and matched (return codes, caps, address layout, enumeration filters, message version, provider calls).
- **AoE2 specifics** (live): ANSI-in-Unicode names handled byte for byte; AoE2-styled dialog boxes (plain Windows boxes for other games, `simple_textboxes=`).
- **Intro movies** (live): `okindeo.dll` decodes AoE2's Indeo intros that modern Windows can't play (`intro=`, `videos=`); a `version.dll` movie helper does the same for Steam Galactic Battlegrounds' player (0.8.1).
- **keep_desktop_mode** (0.8.1): answers a game's display-mode change without changing the desktop (with dgVoodoo for 16-bit games).
- **Windows 95/98 through 11 from one DLL** (live): everything newer than Windows 98 is looked up at run time; `check9x.sh` proves every build loads on Windows 98 on a Pentium MMX.

## Tools for players
- **Connection Check** (`netcheck.exe`, 0.8.1): files and connection list, room server (time, public address, open games, newest version), UDP and router behaviour (STUN), Windows Firewall, then host/join test games from a clickable list (kept off the game lists and the website), ping/jitter/loss with a one-minute graph, a transmission test (unguaranteed, guaranteed, large), a firewall hint when traffic only flows one way, Save/Copy report with the end of the log.
- **Logs** (live): `netplay-logs\` next to the game, per-peer stats every 10 s, crash dumps (`crashdump=`), `trace=1` logs every DirectPlay call (0.8.1).

## Security
- **Per-peer packet tokens** (live, wire 4): every packet must carry the token the receiver gave the sender; strangers who read the room list can't speak as a player.
- **Sender authorization** (live): a message that acts on a player is honoured only from that player's machine; host-only messages only from the host (kick, host takeover, session rewrite, player data, chat, ping...).
- **Room server hardening** (live): the room store can't be read over the web (404), per-address room and knock limits, strict field validation, a capped reply size in the client.
- **Fuzzed parsers** (live): guard-page fuzzing of the packet parsers; a rogue-peer test proves the authorization checks go red without them.
- **Known and documented**: the list key is in every DLL (so key holders see host addresses); passwords use a single SHA-256 (fine for a lobby lock); Windows 9x talks plain HTTP; crash dumps can contain addresses.

## Compared with Windows' original DirectPlay
| | Windows DirectPlay | Classic Netplay drop-in |
|---|---|---|
| Availability | optional "Legacy Components" feature since Windows 8, deprecated since 2004 | a file next to the game; no install, no admin; Windows 95-11 |
| Internet behind routers | TCP/IP needs open ports (port forwarding, or UPnP via its NAT helper) | hole punching, no forwarding; IPv6; LAN addresses first |
| Finding games | LAN broadcast or type the host's IP | online room list (+ web page) and LAN broadcast |
| Guaranteed messages | TCP for guaranteed, UDP for the rest | host-chosen: Fast / DirectPlay-style / All reliable, over reliable UDP |
| Security | none: any packet from anyone is believed | per-peer tokens + sender authorization |
| Passwords | sessions hidden unless the right password is sent | prompt, `[PW]` mark, hashed |
| API behaviour | the reference | 99.9% of Wine's test checks give Windows' result (5,385/5,388); API-state diff 388/390 |
| Plays with Windows machines (classic TCP/IP) | - | our host + Windows joiner: works; our full stack joining a Windows host: works (same-router UPnP lookup included); our joiner on Windows' provider and our provider hosting: not yet |
| DirectPlay reliable protocol (messages 48/49) | yes, when the game asks | **not yet** (matters for classic-TCP/IP games with Windows players) |
| Groups, secure sessions, voice | full | groups partial; secure sessions and voice no |
| Lobby launching | full | lobby-launched games are handed to Windows' DirectPlay; our own lobby launching planned |
| Diagnostics | none for players | logs, trace, crash dumps, Connection Check |
| Outside Windows | no | Wine's code: runs under Wine; a full own stack (our dplayx + our TCP/IP provider) works on Windows, Linux run not tried yet |

## Not done yet (main items)
Reliable protocol layer (48/49); an AoE2 game with DirectPlay-style as the default; silent UPnP port mapping as a hosting last resort; lobby launching through the room server; per-game profiles; LAN discovery on VPN adapters; a run under Wine on Linux. Plan and test results: `docs/DPLAY_CONFORMANCE.md`.
