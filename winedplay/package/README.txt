{{BRAND}} drop-in  v{{VERSION}}
Classic Netplay for AoE2 - no installer
======================================

Online and LAN play for the original Age of Empires II: The Conquerors.
No VPN, no port forwarding, no accounts - and nothing to install:
no administrator rights, no registry changes, no Windows "DirectPlay" feature.

INSTALL
-------
Copy these files into the folder that holds the game's exe:

    dplayx.dll     DirectPlay (Wine's, with our fixes) + the two connections below
    dplay.dll      the same for older games that load DPLAY.DLL (DirectX 3 era; optional)
    okindeo.dll    the intro-movie decoder (optional)
    okplay.ini     settings (optional)
    netcheck.exe   Connection Check: test before you play (optional)

  - UserPatch 1.5: the folder with age2_x1.5.exe, usually ...\age2_x1\
  - otherwise: the folder with age2_x1.exe (or empires2.exe for Age of Kings)

Then start the game as usual -> Multiplayer. The connection list shows only:

    {{ONLINE}}    internet games (listed on the server; players then
                  connect straight to each other - IPv6, or IPv4 "hole punching"
                  through routers and phone hotspots)
    {{LAN}}       same-network games, found by broadcast. No internet needed.

The first time you host, Windows may ask to let the game through the firewall:
click Allow (LAN games need it; online games work either way).

WHAT'S NEW IN 0.8.2
-------------------
- The room list no longer shows a match as "between games" while it is still
  being played on slower PCs, and still shows it when a match is paused
  because a player dropped.
- Fixed: a game that hands its connection settings back to DirectPlay (Age
  of Empires II does) could get its session and player names garbled.
- Tested on Windows 98: 3-player and 4-player matches mixing Windows 98 and
  Windows 10, LAN and online play, a 45-minute match, dropped players, and
  games against PCs running Microsoft's own DirectPlay over classic TCP/IP.

WHAT'S NEW IN 0.8.1
-------------------
- Steadier games: messages the game marks "guaranteed" are now resent until
  they arrive (as Windows' own DirectPlay does), and a burst of them never
  gets dropped. Tested in real AoE2 matches with heavy packet loss.
- Short internet outages: drop_after= in okplay.ini (below) keeps a player in
  the game through a Wi-Fi reconnect instead of dropping them.
- Version warning: joining a host whose game exe is a different version from
  yours (another patch or mod) shows a warning first - such games usually go
  out of sync. You can still join.
- The room list on the website shows each game's name and version, and
  whether it is waiting for players, full, in progress or between games.
- Connection Check (netcheck.exe): test hosting, joining, the server, your
  router and firewall before you play.
- Tested on Windows 98 against Windows 10 in a full AoE2 match.

GAME PASSWORDS
--------------
When you host, a box asks for an optional password (leave it empty for an open
game anyone can join). Joining a game that has one asks you for it. The typed
password never leaves your PC - only a scrambled form tied to that one game.
Don't want the host box? Tick "Disable Hosting Password", or set
use_password=off in okplay.ini (use_password=ask brings it back).

SKIPPING THE INTRO MOVIES
-------------------------
Set intro=0 in okplay.ini and the game goes straight to its menu.

INTRO MOVIES PLAYED BY ANOTHER PROGRAM (STEAM GALACTIC BATTLEGROUNDS)
--------------------------------------------------------------------
Some releases don't let the game play its own intro movies: Steam's "Star Wars
Galactic Battlegrounds Saga" starts player.exe, which plays them and then starts
the game. dplayx.dll can only give the picture to the game itself, so those
movies have sound but no picture.

For such a game, also copy the file in the "movie helper" folder next to the
game's exe (okindeo.dll must be there too):

    version.dll    passes everything on to Windows' own version.dll and gives
                   the movie decoder to whatever program in that folder plays
                   the movies. No registry change; delete it to undo.

Age of Empires II does not need it. Windows XP and later only (Windows 98/95
have the codec themselves). Its log is netplay-logs\moviehelper-*.log.

PLAYING WITH PEOPLE WHO DON'T HAVE THIS (CLASSIC CONNECTIONS)
-------------------------------------------------------------
The game's connection list shows {{ONLINE}} and {{LAN}} first, then the
classic Windows ones (Internet TCP/IP, IPX, Modem, Serial). Pick a classic one
and the game runs on Windows' own DirectPlay, exactly as without this drop-in,
so you can play with anyone using plain Windows DirectPlay.

The classic entries need Windows' own DirectPlay: Control Panel > Programs >
Turn Windows features on or off > Legacy Components > DirectPlay. Without it
only {{BRAND}}'s two entries are listed. A game started by a lobby program
always uses Windows' DirectPlay. Intro movies work either way.

To show only one kind, set mode= in okplay.ini: oldkid ({{BRAND}} only),
windows (classic only) or ask (the game asks at every start).

GAMES WITH A FIXED CONNECTION LIST
---------------------------------
Some games never ask Windows which connections exist: their Multiplayer screen
is a fixed list (MSN Gaming Zone, IPX, TCP/IP, Serial, Modem - Midtown Madness
is one), so {{ONLINE}} and {{LAN}} can't appear. For those, hand them the two
entries nobody uses any more - set in okplay.ini
    classic_modem=online      the game's "Modem" entry becomes {{ONLINE}}
    classic_serial=lan        its "Serial" entry becomes {{LAN}}
TCP/IP and IPX stay Windows' own, for games with plain-Windows players. To
keep them AND reach {{ONLINE}} / {{LAN}} through them, use classic_tcpip=ask
(or classic_ipx=ask): a box then offers {{ONLINE}}, {{LAN}} or Windows' own
the first time the game picks that entry. Age of
Empires II needs none of this.

WINDOWS 98 AND 95
-----------------
The same three files are built for Windows 98 (and should work on Windows 95) up to
Windows 11, on any PC from a Pentium up. On Windows 98 and 95:
- The game talks to the room server over plain http:// (those Windows can't
  do today's HTTPS). Games themselves go straight between the PCs, as always.
- IPv4 only (those Windows have no IPv6).
- The intro movies use Windows' own Indeo codec.
- For the classic connections the game makes a copy of Windows' DirectPlay
  named dplayx-windows9x.dll next to the game (Windows 98 can't load two files
  with the same name). It's refreshed every time the game starts.
Windows 95 needs msvcrt.dll in C:\WINDOWS\SYSTEM (Internet Explorer 4 and
many games install it).

SETTINGS WORTH KNOWING (okplay.ini)
-----------------------------------
    transport=udp+reliable  how game messages travel - the HOST decides for
                            everyone: udp+reliable (default, like Windows'
                            DirectPlay), udp (fastest, nothing resent) or
                            reliable (everything resent: no losses, can stutter)
    drop_after=15           seconds another player may go silent before being
                            dropped (15 = Windows; up to 120 for shaky Wi-Fi)
    build_check=ask         warn before joining a host on another game version
                            (off = never warn)

UNINSTALL
---------
Delete the three files (and version.dll, if you copied the movie helper, and
the netplay-logs folder if you like). On Windows
98/95 also dplayx-windows9x.dll, if it's there.

GOOD TO KNOW
------------
- Everyone in an {{ONLINE}} or {{LAN}} game needs this drop-in. It can't play
  with people using the older installer version (v0.2) or plain Windows
  DirectPlay - you won't even see each other's games, so nothing half-works.
  For those, pick a classic connection (above).
- {{ONLINE}} and {{LAN}} games can't see each other. If anyone in the game
  plays over the internet, everyone - including players on the same home
  network - picks {{ONLINE}}. Players on the same network still connect to
  each other directly over that network.
- If the host quits mid-game, DirectPlay hands the host role to another player,
  as Windows does. A player who goes silent (crash, lost connection) is dropped
  after 15 seconds, like Windows' DirectPlay; drop_after= raises it (below).
- The intro movies play with picture (Windows dropped their Indeo codec). If you
  already have an Indeo 5 codec installed, that one is used instead.
- UserPatch spectating is unchanged: it doesn't use DirectPlay. The host sets
  spectator slots (S0/S1-S4/Sx) in the game setup screen; over the internet the
  host still needs TCP port 53754 open (UserPatch's UPnP option can do it).
- Other classic DirectPlay 4 games may work too, but only AoE2 has been tested.

CONNECTION CHECK
----------------
Start netcheck.exe from the game folder (next to dplayx.dll):
  1. Check my connection   the files, the connection list, the room server,
                           your public address, UDP and how your router
                           handles direct connections.
  2. Host a test game      one player hosts...
  3. Join a test game      ...the other joins: both see the ping (now, average,
                           worst), jitter and loss, with a graph of the last
                           minute; the joiner then runs a transmission test.
Test games are only visible to Connection Check, never in the game.
"Save report" writes everything (plus the end of the log) to a text file you
can send when asking for help. Works on Windows 95/98 up to 11.

IF SOMETHING GOES WRONG
-----------------------
Logs go to netplay-logs\ next to dplayx.dll (one file per game run; a crash also
leaves an okplay-crash-*.dmp there). Zip that folder and send it back. The log
ends with a "census" of the DirectPlay functions the game used - that's what we
need to fix anything unfinished.
For a connection problem in a game other than AoE2, set trace=1 in okplay.ini:
the log then lists every DirectPlay call the game makes and what it got back
(big logs - turn it off again afterwards).

LICENSES
--------
dplayx.dll is built from Wine (LGPL 2.1+) with our changes; okindeo.dll contains
FFmpeg (LGPL 2.1+). The full source for both is published next to this download
(see SOURCE.txt).
