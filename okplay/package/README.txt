{{BRAND}}  v{{VERSION}}
Classic Netplay for AoE2
==========================

Play the original Age of Empires II (The Conquerors) online and on LAN.
No VPN, no port forwarding, no accounts.

Adds two connection types to the game's Multiplayer screen:

  {{ONLINE}}   internet games. Games are listed on the server; players
          then connect straight to each other (IPv6 when both sides have it,
          otherwise IPv4 "hole punching" through routers, including phone
          hotspots).
  {{LAN}}      same-network games. Found by broadcast on your local
          network. Needs no internet and no server at all.

Other games: it is a standard DirectPlay 4 connection type, so it should also
help other classic DirectPlay 4 games - but only AoE2 has been tested.


GET STARTED
-----------
1. Unzip this whole folder (don't run it from inside the zip).
2. Run "{{EXE}}".
3. Click Install and click Yes on the Windows prompt. It:
     - turns on the Windows "DirectPlay" feature if it is off
     - installs to C:\ProgramData\Classic Netplay for AoE2\
     - registers {{ONLINE}} + {{LAN}} with DirectPlay
     - allows the game through the Windows firewall (UDP)
     - adds an Indeo 5 video decoder, so the intro movies play with picture
       again (Windows dropped that codec; no game files are changed)
     - adds a "{{BRAND}}" shortcut to the Desktop and Start menu
     - if "Only show {{BRAND}} connections in the game" is ticked (default),
       hides the game's other connection types (TCP/IP, IPX, Modem, Serial).
       They are kept, not deleted: untick the box or Uninstall to get them back.
   All the checks should turn green.
4. Click Launch Game -> Multiplayer -> pick {{ONLINE}} or
   {{LAN}} -> Create or Join.

The launcher finds the game through DirectPlay's registry entry and prefers
UserPatch's age2_x1.5.exe. Use "Change..." if it picked the wrong one.

Everyone in a game needs the same version (v{{VERSION}} can't join v0.1 games).


IF SOMETHING GOES WRONG
-----------------------
In the launcher click "Collect logs for support". It puts a zip on your Desktop
with the session logs, any crash dumps, and a system summary - send that back.

Logs live in C:\ProgramData\Classic Netplay for AoE2\logs\ (one file per game
run; crashes also leave okplay-crash-*.dmp there). okplay.ini -> loglevel=3 is
the verbose default for this test build.


UNINSTALL
---------
Launcher -> Uninstall. Removes both connection types, puts back any connection
types it hid, and removes the video decoder, the firewall rule, the shortcuts
and the program files (logs are kept). DirectPlay is left on.


FILES
-----
{{EXE}}   launcher: checks, install/repair, launch, logs
okplay.dll            the DirectPlay service provider (Online Play + LAN Play)
okplay.ini            settings: names, server address, log level
okindeo.dll           Indeo 5 decoder for the intro movies (contains FFmpeg, LGPL -
                      see support\okindeo-source\LICENSE-okindeo.txt)
support\dptest.exe    diagnostics tool ("dptest conns" lists DirectPlay connections)
support\vfwprobe.exe  diagnostics tool ("vfwprobe AGE2.AVI" - can Windows decode it?)
