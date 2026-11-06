# Road Rash (Steam "30th Anniversary Edition", app 2448570)

A modern rebuild of Road Rash 95 (Media Foundation video, XInput, MSVC). No DirectPlay. Multiplayer
("Mano-à-Mano", up to 8 players) goes through Winsock 1.1 (`WSOCK32.dll`), which Windows loads from
the game folder first (it is not a KnownDLL), so a `wsock32.dll` beside the exe is the hook point.
The exe must be started through Steam (`steam://rungameid/2448570`); started directly it shows a bare
"Warning" box and exits.

## What it does on the network (wsocklog, 2026-11-05, black box)

Captured with `okplay/tools/wsocklog` on Windows 10, one PC, menus only (no second player yet).

- **TCP/IP, host**: `gethostname` + `gethostbyname` for its own LAN address; a throwaway UDP socket
  bound to that address on port 0 (its port is written into the advert); then a UDP socket on
  **0.0.0.0:6995** with `SO_BROADCAST`, sending a **57-byte advert to 255.255.255.255:6995 every
  500 ms**, and a UDP socket on **0.0.0.0:6996** (race traffic, not yet seen). The advert starts with a
  `sockaddr_in` holding the host's **LAN address and port** (`02 00 <port> <ip>`), then game settings
  and the host's computer name.
- **TCP/IP, join**: binds UDP 0.0.0.0:6995 and lists every advert it hears ("Pick from among the
  available TCP/IP games": host, level, slots, AI bikers, cars, status). **There is no address box**:
  TCP/IP play is LAN-only by design.
- **IP/X**: `socket(AF_IPX, SOCK_DGRAM, NSPROTO_IPX)` three times every 500 ms; Windows 10 refuses
  (10047, address family not supported). The game shows "Your network address is
  00000000-000000000000". The text resources also have an "IPX Hosts" list (net-node addresses typed by
  hand, "Unlisted games") for hosts on other subnets.
- **Modem** (TAPI) exists in the code but is not offered in this edition's menu.

## Plan for okplay

A `wsock32.dll` shim on okplay's transport (rooms, hole punching, relay, LAN Play), passing every other
call to Windows' own Winsock:

1. **TCP/IP mode over okplay** (first): the shim owns ports 6995/6996 for the game. Adverts to
   255.255.255.255:6995 go to everyone in the okplay room; adverts received are rewritten so the
   embedded `sockaddr_in` points at a stand-in address for that peer; datagrams to a stand-in address go
   to that peer through okplay. The game's own "Pick from among the available TCP/IP games" list is
   the lobby: okplay feeds the room's games into it.
2. **One okplay window when the game picks TCP/IP** (user, 2026-11-05): Online or LAN only, plus an
   optional password when hosting (the drop-in's game passwords: only a scrambled form leaves the PC).
   Not shown where the game lists okplay's connections itself (DirectPlay games): this is for games with
   a fixed connection list - the same need as Midtown Madness.
3. **IPX over okplay** (then, and generic for other IPX games): emulate `AF_IPX` datagram sockets with
   okplay peers as IPX nodes; broadcasts go to the room.
4. Goal: feature parity with the DirectPlay drop-in (passwords, room list, LAN Play, update notices, the
   ini), not needed for the first version.

## Cross-play works (2026-11-05): Steam and the 1996 CD version race over okplay

`okplay/wsock` (0.1.0) beside each game: Steam Road Rash on Windows 10 hosted (TCP/IP), the CD version
in a Windows 98 VM listed the room in its own "Pick from among the available TCP/IP games" box (via
Online Play and the room-server stand-in), joined, and both started a race on The City ("Let's welcome
win98c" on the host, "Let's welcome <host PC>" on the joiner). The two builds speak the same protocol. Path: the VM behind 86Box's
NAT, punched directly; the game's packets went as okplay datagrams. Next: a full race (lag, results),
LAN Play between real PCs, the Online/LAN + password window, 3+ players, IPX mode.

## The 1996 CD version (2026-11-05, Windows 98 VMs)

An EA reissue CD (`ROADRASH\ROADRASH.EXE` of 1996-08-23; the installer
offers DirectX 2 - say No on Windows 98, it overwrites DirectPlay files that Windows then restores).
Menus: Modem, TCP/IP, IP/X (Steam dropped Modem). Compared with the Steam rebuild:

- **Same Winsock imports** (the same 19 functions), same TAPI imports, same network menu texts, and both
  exes contain the network library's banner "Copyright (c) 1996 by Papyrus D.G, Inc".
- **Same host sequence and the same 57-byte advert**: own address via gethostname/gethostbyname, a
  throwaway UDP socket bound to it to learn a port, UDP 6995 (SO_BROADCAST) and 6996, the advert to
  255.255.255.255:6995 every ~500 ms: `02 00 <port> <ip>` + 8 zero bytes + `00 <free slots>` + ... + the
  host's computer name. The CD joiner lists the host as "win98b, Level 1, 7, None, No, Available".
- **Join** (CD to CD): the joiner opens a UDP socket on its own new port and `connect`s it to the host's
  advertised port, sends a 19-byte request to host:6996 (`02 00 <its port> <its ip>` + zeros + 2 bytes),
  then repeats a 76-byte hello on the connected socket (`01 00 00 "RIDLE Copyright (c) 1996 by Papyrus
  D.G, Inc"`..., every ~33 ms for 5 s). The host answers the request by opening a socket on the same port
  number, `connect`ed back to the joiner. The host then showed "2 players connected"; the joiner stayed on
  "Connecting to win98b..." - the hellos did not arrive at the host's socket (not finished: see below).
- So far everything matches byte for byte in layout: cross-play between the CD and Steam versions looks
  likely; unconfirmed until a CD-to-Steam join.

**Control test (2026-11-05, later): CD to CD works** when each VM has a single address (the NAT card's
DHCP address released with `winipcfg /release_all`) and no shim: the joiner reached character select.
So the stalls below came from the two-card rig and/or the in-process steering, not from the game. In the
stalled runs each side's game made one helper thread that only `connect`s (2 s wait, 3 tries) and the
hello-sending worker started on at most one side. For the okplay shim this means: give the game ONE
consistent address (a stand-in per peer), rather than steering a multi-homed machine.

Test-rig notes (not game bugs): the VMs have two network cards (86Box NAT for control, a switch between
VMs); Windows 98 answers gethostbyname(own name) with the NAT address first and sends 255.255.255.255
out the NAT card, so the game advertised an unusable address. `wsocklog.ini` `prefer=192.168.77.` and
`broadcast=192.168.77.255` steer that (and bind "any" to the preferred address). On Windows 9x one
loaded `wsock32.dll` serves every program: okagent now has a Winsock 2 build (`okagent-ws2.exe`) so the
game loads the shim, and the shim forwards to `ws2_32.dll` (forwarding to the system wsock32 beside it
failed every socket() with WSAEAFNOSUPPORT). A connected-UDP test outside the game (one thread) passed both
ways (`tools/wsocklog/udpconn.c`), so why the in-game hellos don't arrive is still open.

## Windows 98 to Windows 98 over okplay, with passwords (2026-11-05, later)

Two Windows 98 VMs, each with `okplay/wsock` and `wsock_mode=online`: the host's password window came
up over the game when it picked TCP/IP; the joiner's list showed the room as "win98b (pw)". Picking it
opens the join password window over the list. The shim then waits, still in the list, for the host's
answer (about 100 ms here): a wrong password says so and leaves the player in the list (the room keeps
its "(pw)"), the right one lets the game's own join go straight in. Then lobby ("2 players connected"),
a race on The City (both screens agree on the gap), results back in the lobby. Packet trace
(`wsock_trace=1`): no packets lost apart from the first hellos that reach the host before the game opens
its socket for that joiner (the game repeats them, as on a real LAN).

Why the answer is awaited in the list: a "no" that arrived after the game had left its list put a window
over the full-screen game, and Windows 98 sent the game to the desktop; Road Rash shows only black when
it comes back (any focus change does that, e.g. Windows' own network logon window - close that before
starting the game).

Earlier "joiner stalls" in the VMs were the game's own 33 ms multimedia timer no longer firing in the
game process after many forced kills and hard resets of the VM; a clean restart fixed it. Exit the game
normally between runs.

Clean room: Winsock is a public standard; Microsoft's wsock32/ws2_32 are never disassembled. The game
is observed only through its socket calls.
