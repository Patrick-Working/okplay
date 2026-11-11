{{BRAND}}  v{{VERSION}}
Classic Netplay for AoE2, Road Rash, Need for Speed III and more - no installer
===============================================================================

Online and LAN play for the original Age of Empires II: The Conquerors, Road Rash
(1996), Need for Speed III: Hot Pursuit (1998) and other old games (any DirectPlay
game should work).
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
    wsock32.dll    online play for Road Rash and Need for Speed III (see below). In
                   any other game it does nothing and Windows handles the network
                   as usual

  - UserPatch 1.5: the folder with age2_x1.5.exe, usually ...\age2_x1\
  - otherwise: the folder with age2_x1.exe (or empires2.exe for Age of Kings)

Then start the game as usual -> Multiplayer. The connection list shows only:

    {{ONLINE}}    internet games (listed on the server; players then
                  connect straight to each other - IPv6, or IPv4 "hole punching"
                  through routers and phone hotspots)
    {{LAN}}       same-network games, found by broadcast. No internet needed.

The first time you host, Windows may ask to let the game through the firewall:
click Allow (LAN games need it; online games work either way).

WHAT'S NEW IN 0.8.6
-------------------
- Need for Speed III: Hot Pursuit online (wsock32.dll {{WSOCKVER}}): a list of
  games instead of typing addresses, optional passwords, no port forwarding,
  up to 8 players. The Modern Patch and the original 1998 release both work
  and race each other. See NEED FOR SPEED III.
- Full games: a Need for Speed III game that is full says "full" in the list
  and turns away more players ("That game is full."). DirectPlay games: a host
  whose game is full now refuses a late joiner, as Windows' own DirectPlay does.
- Need for Speed III: joining a race that has already started now says "No
  answer from the host. The game may have started or closed." instead of
  dropping back to the menu without a word.
- Connection Check works in a Winsock game's folder too (Road Rash, Need for
  Speed III): its test game runs through wsock32.dll or dplayx.dll, whichever
  is there, and players in both kinds of folders test with each other. The
  report says how this PC reaches the other players (the connection method),
  warns when this PC shows two different public addresses (a second network
  or a VPN), and compares your version with the newest.
- Notices over a full-screen game: a stray Enter no longer says yes. The
  update notice defaults to Not Now, the report questions to Don't Send. On
  Windows 98 and 95 the update notice shows the download address instead of
  opening a browser.
- Steadier on slow PCs: okplay's network thread runs at a higher priority,
  reads in batches and resends at most the 8 oldest messages at once. The
  game file's version is checked when the game starts, not when a player
  joins (that took up to 13 s on Windows 98). After a short outage, messages
  waiting to be resent go again at once.
- Stricter checks on everything received from the network. Windows 7 and 8.0
  reach the room server with newer HTTPS (TLS 1.1 and 1.2).
- The log notes every long pause in a game and its cause (for bug reports).
- The DirectPlay files are rebuilt and report 0.8.6. Nothing changed on the
  wire: players with 0.8.3 to 0.8.5 still play with 0.8.6.

WHAT'S NEW IN 0.8.5
-------------------
- Road Rash online is much smoother (wsock32.dll 0.2.0). Measured against the
  game's own netcode: 20-40 % fewer sudden jumps of the other bikes (most on a
  LAN, fewer the higher the ping, about a third fewer through dropouts).
  - The two PCs' race clocks start a few steps apart; they are now compared
    and the difference taken out, so the other bikes are guessed for the
    right moment.
  - When a guess turns out wrong, the bike glides to the right place instead
    of jumping there.
  - No more "flying" bikes: gravity in the height guess, and AI bikers (or
    players in a dropout) stop rising when no news comes for a while.
  - AI bikers: both PCs now agree on who runs each one, and a PC ignores its
    own AI bikers sent back to it.
  - A game closed in the middle of a race now exits instead of staying in the
    background.
  These work for the Steam edition and the 1996 CD release, in any mix. Each
  can be turned off in okplay.ini (the rr_ settings at the end).
- Road Rash gets the same boxes as DirectPlay games: update notices, crash
  and connection reports, "server unreachable" and "no network", and a
  warning when the host's game is a build that doesn't race with yours.
- The DirectPlay files are the same as in 0.8.3 (they still report 0.8.3).

WHAT'S NEW IN 0.8.4
-------------------
- Road Rash online: wsock32.dll, for games that use Winsock directly instead of
  DirectPlay (see ROAD RASH). The DirectPlay files are the same as in 0.8.3
  (they still report version 0.8.3).
- Road Rash over a poor line: each game packet also carries a copy of the one
  before (Online Play), so single lost packets are filled in, and the log shows
  lost packets per player every 10 s. Players with older files still connect.

WHAT'S NEW IN 0.8.3
-------------------
- DirectX 3 era games (the ones with DPLAY.DLL) now work: the old DirectPlay 1
  interface is in, and players are numbered as those games expect. Tested
  with Necrodome between Windows 10 and Windows 98, online.
- Necrodome (GOG edition): its Network Game list works again. The fix is made
  in memory while the game runs, only for that exact game file; the game's
  files are never changed (okplay.ini game_fixes=0 turns it off).
- Fixed: Windows 95, 98 and XP crashed in DPLAY.DLL games.
- Games that use DirectPlay groups (teams, chat channels) now see them on
  every PC, not just the one that made them.
- A player whose connection dropped long enough to be removed is now told
  the game is over when the connection comes back, as Microsoft's DirectPlay
  does, instead of waiting in a game that has gone on without them.
- Fixed: with trace=1, DirectX 3 era games could crash.

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

ROAD RASH
---------
Road Rash (the Steam edition and the original 1996 CD release - they play each
other) doesn't use DirectPlay; wsock32.dll gives it online play. Copy the files
next to RoadRash95.exe (Steam: ...\steamapps\common\RoadRash) or ROADRASH.EXE (CD).
In the game: Mano-a-Mano -> Host or Join -> TCP/IP. A box then asks:

    Online Play    internet games, listed on {{HOST}}
    LAN Play       same-network games, no internet needed
    Original       the game's own TCP/IP, as if this weren't here

Hosting asks for an optional password. In the game's "Pick from among the
available TCP/IP games" list a locked game says "(pw)" after the host's name
and asks for the password when you pick it; a wrong one keeps you in the list.
Not carried yet: the game's IPX mode. If the game turns black after you switch
away from it, that's the game itself: restart it. Logs: logs\ next to the game.

NEED FOR SPEED III
------------------
Need for Speed III: Hot Pursuit (1998) doesn't use DirectPlay either; wsock32.dll
gives its "Network (TCP)" mode online play. Copy the files next to nfs3.exe.
Known: the Modern Patch (1.6.1, and the portable 1.6.2 Beta) and the original
1998 release (CD and OEM). They race each other: joining a host with the other
one asks first, as the game is a different version - "Join anyway" works.

Host: Game Setup > Connect Players > Network (TCP) > Host Game. A box asks:

    Online Play    internet games, listed on {{HOST}}
    LAN Play       same-network games, no internet needed
    Original       the game's own TCP/IP, as if this weren't here

Then a box offers an optional password (asked at every hosting; empty = anyone
can join). The game is listed as "Need for Speed III - <your name>'s game".
Peer-to-Peer takes 2 players, Client-Server up to 8.

Join: Game Setup > Connect Players > Network (TCP) > Join Game, and the same
box. The Join box shows "oldkid.net": just press Ok (if it's empty - the 1998
OEM release - type oldkid.net). A list of the open games shows the players,
for example "(1/2 players)", and refreshes by itself; pick one and Join. A
locked game asks for its password. A full game says "full" and can be joined
once a place is free. A race that has already started can't be joined.

With Original the game uses its own TCP/IP exactly as in 1998 (type the host's
address; no list, no passwords). Logs: logs\ next to nfs3.exe.

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
Delete the files you copied (and version.dll, if you copied the movie helper, and
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
Start netcheck.exe from the game folder (next to dplayx.dll or wsock32.dll):
  1. Check my connection   the files, the connection list, the room server,
                           your public address, UDP and how your router
                           handles direct connections.
  2. Host a test game      one player hosts...
  3. Find test games       ...the others join it (up to 8 players in all): all
                           see the ping (now, average, worst), jitter and loss,
                           with a graph of the last minute; a joiner then runs
                           a transmission test.
Test games are only visible to Connection Check, never in the game. Connection
Check 0.8.5 and older can't see this version's test games: everyone should use
this one.
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
Road Rash: its logs go to logs\ next to RoadRash95.exe / ROADRASH.EXE (one file
per game run; a crash leaves an okplay-crash-*.dmp there too). Every 10 s the log
notes each player's lost and late packets. Zip the logs folder from every PC in
the game and send it back. wsock_trace=1 in okplay.ini logs every game packet
(big logs - turn it off again afterwards).
Need for Speed III: the same, in logs\ next to nfs3.exe.

LICENSES
--------
dplayx.dll is built from Wine (LGPL 2.1+) with our changes; okindeo.dll contains
FFmpeg (LGPL 2.1+). The full source for both is published next to this download
(see SOURCE.txt).
