# Classic Netplay for AoE2 (okplay) — "Online Play" + "LAN Play" DirectPlay providers (v0.2)

> **The release is the drop-in edition** (`../winedplay`, v0.3.0: okplay built into Wine's
> dplayx.dll, released with `winedplay/package.sh`). The installer edition described here (launcher
> + okplay.dll on Windows' DirectPlay, v0.2.0) was retired on 2026-09-28; its last release is in
> `deliverables/archive/`. This folder stays the home of the okplay source, its tests and the server.

A DirectPlay 4 service provider for AoE2 (it should also help other DirectPlay 4 games; only AoE2
is tested), registered twice. Display names come from `okplay.ini` (per server); GUIDs are fixed:

| Provider | GUID | How games are found |
|---|---|---|
| Online Play | `{66D8DC59-28E8-4D3E-BA93-2E98D251C0F3}` | `server/okplay.php` + STUN, then direct UDP (IPv6 or IPv4 hole punching) |
| LAN Play | `{6BCFF704-CE20-4150-98B4-5A7198175D7D}` | UDP broadcast to every subnet on port 47632 — no server, no internet |

One UDP socket per game; peers are random 64-bit ids, never IPs. DirectPlay's own messages
("play" envelope) and anything over 1200 bytes ride a reliable, in-order lane (seq/ack/retransmit,
early duplicate, pieces reassembled in order). Wire format v2 — 0.1 peers can't talk to 0.2, and the
server's list carries each host's protocol version so incompatible rooms aren't shown.

**Two Windows dplayx quirks this works around (both found by tests here):**
1. dplayx does not resend its join handshake over an unreliable SP → reliable lane.
2. dplayx's own packetizer (used when a message exceeds the SP's `dwMaxBufferSize`) builds a
   corrupt second packet for the ~1.1 KB player list of a 6-player game; the joiner never acks it
   and the join times out. We report `dwMaxBufferSize = 16384` and split big messages ourselves.

## Layout

    src/            okplay.dll: sp.c (DirectPlay glue), net.c (UDP, punching, STUN, reliable lane,
                    router-sim hook), rdv.c (server client), lan.c (LAN discovery), crash.c, util.c
    launcher/       ClassicNetplay.exe (C# 5, Windows' own csc): checks, install, hide other
                    connection types, launch, logs, uninstall. --selftest, --dryrun-install
    codec/          okindeo.dll: Indeo 5 decoder for Video for Windows (FFmpeg, LGPL) - the intro movies
    tools/          dptest.c (AoE2 stand-in), vfwprobe.c (can VfW decode this AVI?)
    server/         okplay.php + index.html (templated), rdv_standin.py (same API, local)
    tests/          run_local.sh, soak.sh, fuzz.c, natsim.py + nat_matrix.py, rdv_conformance.py
    docs/           SELF-HOSTING.md, wine-dplayx-exploration.md, overnight report
    package/        README.txt + okplay.ini templates for a release (package.sh fills them)

## Build / test (MSYS2 mingw32, 32-bit)

Setup on a new PC: install MSYS2, then in its shell `pacman -S mingw-w64-i686-gcc
mingw-w64-i686-binutils make`; any Python 3.8+. Nothing machine-specific is written in the scripts:
they all source `../env.sh`, which finds the 32-bit gcc, Python, the game folder, the deliverables
folder and the FFmpeg prefix at run time. `bash ../env.sh` shows what it found; put corrections in
`../local.env` (per machine, KEY=value). The scripts run from Git Bash or the MSYS2 shell, and the
workspace path may contain spaces.

    ./build.sh                     # bin/okplay.dll, bin/dptest.exe (+ bin/dpwsockx.dll copy, see below)
    launcher/build.sh              # bin/ClassicNetplay.exe
    python server/rdv_standin.py [--port 8787] [--www server] [--fake-client-ip 203.0.113.7]
    tests/run_local.sh all         # pair mesh3 loss15 v6only lan lanmesh lanneg
    tests/run_local.sh mesh8|lanmesh8|migrate|loss30   # 8 players, host quits mid-game, 30% loss
    tests/soak.sh 60               # 3 players, 3% loss, memory sampled each minute
    bin/fuzz.exe 2000000 [seed]    # all network/server parsers, inputs end at a guard page
    bin/fuzz.exe 1000 --control    # the old player-data parser - must be CAUGHT (proves the trap)
    python tests/nat_matrix.py     # 25 router-type pairs through natsim (uses bin_matrix\)
    python tests/rdv_conformance.py [URL]      # 19 checks; run against the live server after uploading
    bin/ClassicNetplay.exe --selftest out.txt   # layout + garbled-text audit
    bin/ClassicNetplay.exe --dryrun-install <dir> <game.exe>   # install/hide/rename/uninstall, no admin
    ./package.sh                   # -> deliverables\<brand> v<ver>\ + "upload to <host> (v<ver>)"

## Intro movies (codec/)
AoE2's AVIs are Indeo 5 ("IV50"), which Windows dropped, so MCIWnd plays sound with no picture.
`okindeo.dll` is a VfW decoder (DriverProc) around FFmpeg's indeo5 decoder, static, importing only
Windows DLLs. Install registers it as `vidc.IV50` under HKLM (32-bit view) `...\Windows NT\
CurrentVersion\Drivers32` unless a working IV50 decoder is already there; Uninstall removes only ours.
No game file changes. The install folder is locked to read-only for Users (logs\ stays writable),
because any program that plays Indeo loads this DLL.

    codec/build-ffmpeg.sh          # once: FFmpeg 7.1, indeo5 only, w32threads, static (re-runs itself in MSYS2 MINGW32)
    codec/build.sh                 # bin/okindeo.dll, vfwprobe.exe, fuzz_indeo.exe (build.sh runs it)
    bin/vfwprobe.exe --codec bin/okindeo.dll --all --mci 5 <game>/Avi/AGE2.AVI
        # loads the codec in-process (no registry/admin), decodes every frame, then PLAYS 5 s through
        # MCIAVI and counts the frames it handed the codec. --dump N x.bmp for PSNR vs ffmpeg.
    bin/fuzz_indeo.exe bin/okindeo.dll <avi> 200000     # corrupted frames at a guard page
    bin/fuzz_indeo.exe bin/okindeo.dll <avi> 100 --control   # lies about frame size: must be CAUGHT

Verified: all 4 intros decode every frame (~1000 fps; need 15), PSNR 38-48 dB vs ffmpeg's decode
(the frame+12 control scores 11-26 dB), MCIAVI playback 15 fps with 0 skipped, 300k fuzzed
frames clean. Output is 24/32-bit only; MCIAVI falls back to 24-bit when the codec turns down
its first choice (`vfwprobe --refuse 32 --mci 3`), so an 8-bit display gets 24-bit + DrawDib dithering.
Still to confirm in the real game.

**Dev loop without admin:** the IPX provider's registry Path is the bare name `dpwsockx.dll`, so
`dptest.exe` in `bin\` loads `bin\dpwsockx.dll` (= this build). With `OKP_TEST_AS=online|lan`,
SPInit accepts the IPX GUID and runs as that mode. Tests use `dptest --sp ipx`. (Keep the
launcher's "only show our connections" box unticked on the dev PC or the IPX entry is hidden.)
If some other tool rewrote that Path to a full path, set it back (admin, 32-bit view):
`reg add "HKLM\SOFTWARE\WOW6432Node\Microsoft\DirectPlay\Service Providers\IPX Connection For DirectPlay" /v Path /t REG_SZ /d dpwsockx.dll /f`

Test knobs (env overrides okplay.ini): `OKP_SERVER`, `OKP_STUN` (empty = off), `OKP_DROP_PCT`
(drops every outgoing datagram), `OKP_FAMILY` (4/6), `OKP_LOGLEVEL`, `OKP_STATS_MS`, `OKP_TRACE=1`
(hex-trace every dplayx message), `OKP_CRASHTEST=1`, `OKP_TEST_NAT` + `OKP_TEST_PRIVATE` (natsim).

## Security hardening (0.2)
Parsers of network/server data are bounds-checked and fuzzed with a guard-page harness:
player data (size checked per address), session records (validated before dplayx sees them),
LAN discovery (answered only for LAN sources), probe targets (no broadcast/multicast/port 0,
loopback only in LAN mode), never-joined peers (15 s probe window), peer table (idle peers evicted
when full). Known limitation: if a reliable message is abandoned after 30 s of unreachability,
that peer's in-order stream stays blocked (the peer is normally gone by then).

## Public address (WAN IP) discovery, in order
1. STUN, asked in parallel: Cloudflare, Google (x2), Nextcloud on 443.
2. The server's view of our IP + our local port (port-preserving routers).
3. Peer-reflexive: the public address a peer reports in its probe ack.

## Diagnostics
One log per game run: `C:\ProgramData\Classic Netplay for AoE2\logs\okplay-<date>-<time>-<pid>.log`
(appends when AoE2 reloads the provider). Header, callback failures/slow calls, path changes,
10 s per-peer stats incl. reliable-lane counters, crash report + minidump. Map a crash offset with
`addr2line -f -e builds/<ver>/okplay.dll 0x<ImageBase+offset>`.
