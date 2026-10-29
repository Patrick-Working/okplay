# Two copies of AoE2 on this PC, one hosting and one joining over LAN Play, with no menus clicked:
# each is started with "Lobby" on its command line (as the MSN Gaming Zone started it) and the
# drop-in's test lobby (OKP_TEST_LOBBY, builtin.c) as the lobby's settings. OKP_TEST_MULTI lets the
# second copy run beside the first. The test lobby answers the game's one lobby question
# (GETPROPERTY {b3f2e132-...}, AoE2's own), and both copies reach the Multiplayer Game room by
# themselves, with game traffic flowing (pings, chat channel, settings).
# -Play N unticks "Record Game" (both copies would record into one SaveGame folder: the joiner crashed
# at the match start), ticks both copies' "I'm Ready!", presses the host's "Start Game" and lets the
# match run N seconds (captures at 15 s and the end). Clicks are posted window messages - no real
# mouse, no foreground needed - which works because the run's ddraw.ini sets cnc-ddraw's devmode
# (no cursor lock: its first click would only lock the cursor) and noactivateapp (the game never
# hears it is inactive); cnc-ddraw, not the game, sits on the input functions (the input census in
# the session log shows the game's own imports unused). "Start Game" acts on release: held 400 ms,
# pressed twice. -Shots "6,20" saves both windows at those seconds after the join (log folder).
#
#   powershell -File tests\realgame_pair.ps1 [-GameDir <game>\age2_x1] -Release <drop-in folder>
#                 [-Seconds 60] [-Play 120] [-Transport udp+reliable] [-DropPct 0] [-Shots "6,20"]
#
# PASS needs: both copies stay up; the host's lobby Connect creates the session; the joiner's finds
# and joins it; each side's log sees the other as a peer with a path; no crash, no unfinished Wine
# code. The game folder is put back afterwards (ddraw.ini included); logs are kept in test-logs\.
param(
    [string]$GameDir = "C:\games\AOE2-CON\age2_x1",
    [string]$Release = "",
    [int]$Seconds = 60,
    [int]$Play = 0,        # > 0: start the match from the room and let it run this many seconds
    [switch]$Record,
    [int]$Blackout = 0,    # > 0: the line goes dead both ways for this many seconds, ~30 s into the match
    [int]$DropAfter = 0,
    [switch]$Online,       # Online Play through the local room-server stand-in (okplay/server/rdv_standin.py)   # okplay drop_after= (seconds) for both copies; 0 = the default (15)       # keep "Record Game" on (off by default: both copies would record into one folder)
    [string]$Transport = "udp+reliable",
    [int]$DropPct = 0,
    [string]$Shots = "",   # "4,20": seconds after the join to save both windows (host-<s>s.png, join-<s>s.png)
                           # (a string: through powershell -File an int[] "4,20" arrives as the number 420)
    [string]$Debug = ""    # DPLAYX_DEBUG for both copies, e.g. +dplay (Wine's trace into the session log)
)
. $PSScriptRoot\realgame_lib.ps1
if (-not $Release) { "pass -Release <folder with dplayx.dll and okplay.ini>"; exit 2 }
$exe = @("age2_x1.5.exe", "age2_x1.exe") | ForEach-Object { Join-Path $GameDir $_ } | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $exe) { "no game exe in $GameDir"; exit 2 }
$keep = Join-Path (Split-Path $PSScriptRoot -Parent) ("test-logs\realgame-pair-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
$session = "okptest " + (Get-Random -Maximum 9999)
$logs = Join-Path $GameDir "netplay-logs"

function LogOf($p) { Get-ChildItem (Join-Path $logs "okplay-*-$($p.Id).log") -ErrorAction SilentlyContinue | Select-Object -First 1 }
function Grep($p, $pattern) { $l = LogOf $p; if ($l) { Select-String -Path $l.FullName -Pattern $pattern -ErrorAction SilentlyContinue } }
function WaitFor($p, $pattern, $secs) {
    $t = Get-Date
    while (((Get-Date) - $t).TotalSeconds -lt $secs) {
        if ($p.HasExited) { return $null }
        $m = Grep $p $pattern
        if ($m) { return $m | Select-Object -Last 1 }
        Start-Sleep -Milliseconds 500
    }
    return $null
}
function Start-Copy($role, $name) {
    $env:OKP_TEST_MULTI = "1"
    $env:OKP_TEST_LOBBY = "${role}:$session"
    $env:OKP_TEST_LOBBY_PLAYER = $name
    $env:OKP_TEST_HOST_PASSWORD = "<none>"
    $env:OKP_TRANSPORT = $Transport
    if ($DropAfter) { $env:OKP_DROP_AFTER = "$DropAfter" }
    if ($Online) { $env:OKP_TEST_LOBBY_SP = "online"; $env:OKP_SERVER = "http://127.0.0.1:8787/okplay" }
    if ($Blackout) { $env:OKP_BLACKOUT = "${script:blackoutAt}:$Blackout" }
    if ($Debug) { $env:DPLAYX_DEBUG = $Debug }
    if ($DropPct) { $env:OKP_DROP_PCT = "$DropPct" } else { Remove-Item Env:OKP_DROP_PCT -ErrorAction SilentlyContinue }
    $p = Start-Process -FilePath $exe -ArgumentList "Lobby" -WorkingDirectory $GameDir -PassThru
    foreach ($v in "OKP_TEST_MULTI", "OKP_TEST_LOBBY", "OKP_TEST_LOBBY_PLAYER", "OKP_TEST_HOST_PASSWORD", "OKP_TRANSPORT", "OKP_DROP_PCT", "DPLAYX_DEBUG", "OKP_DROP_AFTER", "OKP_BLACKOUT", "OKP_TEST_LOBBY_SP", "OKP_SERVER") {
        Remove-Item "Env:$v" -ErrorAction SilentlyContinue
    }
    return $p
}

"game:      $GameDir"
"release:   $Release"
"session:   $session  transport=$Transport drop=$DropPct%"
$fail = @()
$st = $null
$ddHash = (Get-FileHash (Join-Path $GameDir "ddraw.ini")).Hash
$t0 = Get-Date
# the match starts ~40 s after launch: the blackout comes ~30 s into it (unix time, both copies)
$script:blackoutAt = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds() + 70   # (UTC: -UFormat %s is local time in PowerShell 5.1)
function Say($text) { "{0,6:N1}s {1}" -f ((Get-Date) - $t0).TotalSeconds, $text }
try {
    $st = Install-DropIn -GameDir $GameDir -Release $Release -Ini @{ intro = "0"; trace = "1" } -Windowed
    $host1 = Start-Copy "host" "Host"
    $m = WaitFor $host1 "lobby Connect: create .* -> 0x0+$" 40
    if (-not $m) { throw "the host's lobby Connect did not create the session" }
    Say "host created the session"
    $join1 = Start-Copy "join" "Joiner"
    $m = WaitFor $join1 "lobby Connect: join .* -> 0x" 60
    if (-not $m) { throw "the joiner's lobby Connect never returned" }
    if ($m.Line -notmatch "-> 0x0+$") { throw "the joiner's lobby Connect failed: $($m.Line -replace '^.*-> ', '')" }
    Say "joiner joined the session"
    Move-GameWindow $host1 0 380
    Move-GameWindow $join1 1110 380
    $joined = Get-Date
    foreach ($at in ($Shots -split '[,\s]+' | Where-Object { $_ } | ForEach-Object { [int]$_ })) {
        New-Item -ItemType Directory -Force $keep | Out-Null
        $wait = $at - ((Get-Date) - $joined).TotalSeconds
        if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]($wait * 1000)) }
        if (Save-GameWindow $host1 (Join-Path $keep "host-${at}s.png")) { Say "saved host-${at}s.png" }
        if (Save-GameWindow $join1 (Join-Path $keep "join-${at}s.png")) { Say "saved join-${at}s.png" }
    }
    if ($Play -gt 0) {
        # the room comes up once each copy has the lobby's answer (a few seconds after the join)
        foreach ($p in $host1, $join1) {
            if (-not (WaitFor $p "test_lobby: the game received the lobby's answer" 30)) { throw "pid $($p.Id) never got to the room" }
        }
        New-Item -ItemType Directory -Force $keep | Out-Null
        $wait = 8 - ((Get-Date) - $joined).TotalSeconds
        if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]($wait * 1000)) }
        [void](Post-Text $host1 "okp test" -Enter); Start-Sleep 2; Say "host: chat 'okp test'"
        if (Save-GameWindow $join1 (Join-Path $keep "join-chat.png")) { Say "saved join-chat.png" }
        # the room's spots at 800x600: "I'm Ready!" box (51,489), "Start Game" (118,560), "Record Game" (751,471)
        if (-not $Record) {
            # two copies in one game folder would both write the recorded game into SaveGame\ at once
            $f = Post-Click $host1 751 471; Start-Sleep 1; Say "host: Record Game off"
        }
        $f = Post-Click $join1 51 489; Start-Sleep 2; Say "joiner: I'm Ready! (window in front: $f)"
        $f = Post-Click $host1 51 489; Start-Sleep 2; Say "host: I'm Ready! (window in front: $f)"
        Start-Sleep 3   # the room enables Start Game once every player's Ready has gone round
        $f = Post-Click $host1 118 560 -HoldMs 400; Say "host: Start Game"
        Start-Sleep 5
        New-Item -ItemType Directory -Force $keep | Out-Null
        if (Save-GameWindow $host1 (Join-Path $keep "host-after-start1.png")) { Say "saved host-after-start1.png" }
        $f = Post-Click $host1 118 560 -HoldMs 400; Say "host: Start Game (second try)"
        Start-Sleep 5
        $c = Grep $host1 "input: the game took msg 0x0201" | Measure-Object
        New-Item -ItemType Directory -Force $keep | Out-Null
        if (Save-GameWindow $host1 (Join-Path $keep "host-after-start.png")) { Say "saved host-after-start.png" }
        New-Item -ItemType Directory -Force $keep | Out-Null
        $started = Get-Date
        foreach ($at in 15, [Math]::Max(16, $Play - 2)) {
            $w = $at - ((Get-Date) - $started).TotalSeconds
            if ($w -gt 0) { Start-Sleep -Milliseconds ([int]($w * 1000)) }
            foreach ($p in $host1, $join1) { if ($p.HasExited) { throw "a copy of the game exited during the match (pid $($p.Id), code $($p.ExitCode))" } }
            if (Save-GameWindow $host1 (Join-Path $keep "host-play-${at}s.png")) { Say "saved host-play-${at}s.png" }
            if (Save-GameWindow $join1 (Join-Path $keep "join-play-${at}s.png")) { Say "saved join-play-${at}s.png" }
        }
        Say "match ran $Play s"
    }
    $until = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $until) {
        foreach ($p in $host1, $join1) { if ($p.HasExited) { throw "a copy of the game exited (pid $($p.Id), code $($p.ExitCode))" } }
        Start-Sleep 2
    }
    Say "both copies still up after $Seconds s"
}
catch { $fail += "$_" }
finally {
    # the evidence, before the logs move: stats lines say what the reliable lane did
    foreach ($p in @($host1, $join1) | Where-Object { $_ }) {
        $s = Grep $p "stats peer .* path " | Select-Object -Last 1
        if ($s) { "  pid $($p.Id): " + ($s.Line -replace '^.*stats peer ', 'peer ') }
        elseif (LogOf $p) { $fail += "pid $($p.Id): no peer with a path in its log" }
        if (Grep $p "census: unfinished \(fixme\) reached: (?!none)") { $fail += "pid $($p.Id): unfinished Wine code reached" }
        if (Grep $p "=+ CRASH =+|UNHANDLED - process") { $fail += "pid $($p.Id): crash report in the log" }
    }
    if ($st) { $r = Restore-DropIn $st $keep; if ($r) { $fail += $r } else { "game folder restored; logs in $keep" } }
    if ((Get-FileHash (Join-Path $GameDir "ddraw.ini")).Hash -ne $ddHash) { $fail += "ddraw.ini differs from before the run" }
}
if ($fail.Count) { $fail | ForEach-Object { "  - $_" }; "RESULT FAIL"; exit 1 }
"RESULT PASS"; exit 0
