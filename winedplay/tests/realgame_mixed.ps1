# A mixed AoE2 game: this PC hosts, Windows 98 machines running okagent join (86Box VMs, or real old
# PCs; -Agent A,B,C for several - up to 4 players with the PC), over Online Play through the local room-server stand-in (okplay/server/rdv_standin.py,
# port 8787 - the VM reaches it as 10.0.2.2). Both sides use the drop-in's test lobby (OKP_TEST_LOBBY),
# so no menus; Ready/Start are clicked (the VM through okagent, this PC with posted clicks).
#
#   powershell -File tests\realgame_mixed.ps1 -Release <folder with dplayx.dll + okplay.ini>
#              [-Play 120] [-PcExe _age2_x1.exe] [-VmGame "C:\Program Files\...\age2_x1"] [-Agent NAME[,NAME...]]
#              [-VmHost]   (the first agent hosts; this PC joins, then the other agents)
#
# The VM needs the drop-in (with server=http://10.0.2.2:8787/okplay) already in its game folder.
# Both sides must run the same AoE2 build (the PC's -PcExe is copied to age2_x1.exe for the run).
param(
    [string]$Release = "",
    [string]$GameDir = "C:\games\AOE2-CON\age2_x1",
    [string]$PcExe = "_age2_x1.exe",
    [string]$VmGame = "C:\Program Files\Microsoft Games\Age of Empires II\age2_x1",
    [string]$Agent = "",
    [int]$Play = 120,
    [switch]$VmHost
)
. $PSScriptRoot\realgame_lib.ps1
if (-not $Release) { "pass -Release <folder with dplayx.dll and okplay.ini>"; exit 2 }
$vmctl = Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) "okagent\vmctl.py"
$agents = @($Agent -split "," | Where-Object { $_ })
if (-not $agents.Count) { $agents = @("") }    # (the only agent)
function VmOn([string]$ag, [string[]]$a) {
    $args2 = @($vmctl) + $(if ($ag) { @("--agent", $ag) } else { @() }) + $a
    $out = & python @args2 2>&1
    return ($out | Out-String).Trim()
}
function Tag([int]$i) { if ($agents.Count -gt 1) { "vm$($i + 1)" } else { "vm" } }
$keep = Join-Path (Split-Path $PSScriptRoot -Parent) ("test-logs\realgame-mixed-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
New-Item -ItemType Directory -Force $keep | Out-Null
$session = "mixed " + (Get-Random -Maximum 9999)
$logs = Join-Path $GameDir "netplay-logs"
$t0 = Get-Date
function Say($text) { "{0,6:N1}s {1}" -f ((Get-Date) - $t0).TotalSeconds, $text }
function PcLog { Get-ChildItem (Join-Path $logs "okplay-*-$($pc.Id).log") -ErrorAction SilentlyContinue | Select-Object -First 1 }
function PcWait($pattern, $secs) {
    $t = Get-Date
    while (((Get-Date) - $t).TotalSeconds -lt $secs) {
        $l = PcLog
        if ($l -and (Select-String -Path $l.FullName -Pattern $pattern -Quiet)) { return $true }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

function RoomPlayers {
    try {
        $j = Invoke-RestMethod -TimeoutSec 5 "http://127.0.0.1:8787/okplay?a=list&fmt=json"
        $r = $j.rooms | Where-Object { $_.name -eq $session } | Select-Object -First 1
        if ($r) { return [int]$r.players }
    } catch {}
    return 0
}
function VmStart([int]$i, [string]$role) {
    $name = if ($role -eq "host") { "Host98" } elseif ($agents.Count -gt 1) { "Win98-$($i + 1)" } else { "Win98" }
    foreach ($e in "OKP_TEST_LOBBY=${role}:$session", "OKP_TEST_LOBBY_SP=online", "OKP_TEST_LOBBY_PLAYER=$name",
                   "OKP_TEST_LOBBY_MAX=$maxp", "OKP_TEST_HOST_PASSWORD=<none>", "OKP_TEST_BUILD_ANSWER=join",
                   "OKP_TEST_EXEBUILD=", "OKP_BLACKOUT_FILE=") {
        [void](VmOn $agents[$i] "setenv", $e)
    }
    $r = VmOn $agents[$i] "run", "`"$VmGame\age2_x1.exe`" Lobby"
    if ($r -notmatch "^ok pid (\d+)") { throw "could not start AoE2 in $(Tag $i): $r" }
    $vmPids[$i] = $Matches[1]
    Say "$(Tag $i): AoE2 started as $role (pid $($vmPids[$i]))"
}
function WaitRoom([int]$n, [string]$what) {
    $t = Get-Date
    while (((Get-Date) - $t).TotalSeconds -lt 180) {
        if ((RoomPlayers) -ge $n) { Say "$what ($n players)"; return }
        Start-Sleep 2
    }
    throw "timed out: $what"
}

$fail = @()
$st = $null; $pc = $null; $vmPids = @{}; $tempExe = $null
$maxp = 1 + $agents.Count
"session: $session"
for ($i = 0; $i -lt $agents.Count; $i++) { "agent:   $(VmOn $agents[$i] 'info')" }
try {
    $st = Install-DropIn -GameDir $GameDir -Release $Release -Ini @{ intro = "0"; trace = "1" } -Windowed
    $tempExe = Join-Path $GameDir "age2_x1.exe"
    if (Test-Path $tempExe) { throw "refusing: $tempExe exists already" }
    Copy-Item (Join-Path $GameDir $PcExe) $tempExe

    $first = 0
    if ($VmHost) {
        # the first machine hosts, this PC joins
        VmStart 0 "host"
        WaitRoom 1 "$(Tag 0) hosts (room listed)"
        $env:OKP_TEST_LOBBY = "join:$session"; $env:OKP_TEST_LOBBY_SP = "online"; $env:OKP_TEST_LOBBY_PLAYER = "Win10"
        $env:OKP_TEST_LOBBY_MAX = "$maxp"; $env:OKP_TEST_BUILD_ANSWER = "join"; $env:OKP_SERVER = "http://127.0.0.1:8787/okplay"
        $pc = Start-Process -FilePath $tempExe -ArgumentList "Lobby" -WorkingDirectory $GameDir -PassThru
        foreach ($v in "OKP_TEST_LOBBY", "OKP_TEST_LOBBY_SP", "OKP_TEST_LOBBY_PLAYER", "OKP_TEST_LOBBY_MAX", "OKP_TEST_BUILD_ANSWER", "OKP_SERVER") {
            Remove-Item "Env:$v" -ErrorAction SilentlyContinue
        }
        if (-not (PcWait "lobby Connect: join .* -> 0x0+$" 180)) { throw "the PC could not join the VM's session" }
        WaitRoom 2 "PC joined"
        Move-GameWindow $pc 0 380
        for ($i = 1; $i -lt $agents.Count; $i++) {
            VmStart $i "join"
            WaitRoom ($i + 2) "$(Tag $i) joined"
        }
    } else {
        # this PC hosts
        $env:OKP_TEST_LOBBY = "host:$session"; $env:OKP_TEST_LOBBY_SP = "online"; $env:OKP_TEST_LOBBY_PLAYER = "Win10"
        $env:OKP_TEST_LOBBY_MAX = "$maxp"; $env:OKP_TEST_HOST_PASSWORD = "<none>"; $env:OKP_SERVER = "http://127.0.0.1:8787/okplay"
        $pc = Start-Process -FilePath $tempExe -ArgumentList "Lobby" -WorkingDirectory $GameDir -PassThru
        foreach ($v in "OKP_TEST_LOBBY", "OKP_TEST_LOBBY_SP", "OKP_TEST_LOBBY_PLAYER", "OKP_TEST_LOBBY_MAX", "OKP_TEST_HOST_PASSWORD", "OKP_SERVER") {
            Remove-Item "Env:$v" -ErrorAction SilentlyContinue
        }
        if (-not (PcWait "lobby Connect: create .* -> 0x0+$" 60)) { throw "the PC host did not create the session" }
        Say "PC host created the session"
        Move-GameWindow $pc 0 380

        # the Windows 98 machines join, one at a time
        for ($i = 0; $i -lt $agents.Count; $i++) {
            VmStart $i "join"
            $t = Get-Date; $n = 0
            while (((Get-Date) - $t).TotalSeconds -lt 180) {
                $l = PcLog
                $n = if ($l) { @(Select-String -Path $l.FullName -Pattern "CreatePlayer remote .* flags 0x0 ").Count } else { 0 }
                if ($n -ge $i + 1) { break }
                Start-Sleep -Milliseconds 500
            }
            if ($n -lt $i + 1) { throw "$(Tag $i)'s player never arrived on the PC host" }
            Say "$(Tag $i) player is in the PC's room ($($i + 2) players)"
        }
    }
    Start-Sleep 15
    for ($i = 0; $i -lt $agents.Count; $i++) { [void](VmOn $agents[$i] "shot", "$keep\$(Tag $i)-room.png") }
    [void](Save-GameWindow $pc "$keep\pc-room.png")

    if ($Play -gt 0) {
        # Ready on both sides (VM: a real click inside the VM; the game runs at 800x600 there too)
        for ($i = $(if ($VmHost) { 1 } else { 0 }); $i -lt $agents.Count; $i++) {
            $r = VmOn $agents[$i] "click", "51", "489", "150"; Say "$(Tag $i): I'm Ready! ($r)"
            Start-Sleep 2
        }
        Start-Sleep 1
        if ($VmHost) {
            [void](Post-Click $pc 51 489); Start-Sleep 3; Say "PC: I'm Ready!"
            [void](VmOn $agents[0] "click", "751", "471", "150"); Start-Sleep 1; Say "$(Tag 0): Record Game off"
            $r = VmOn $agents[0] "click", "51", "489", "150"; Start-Sleep 3; Say "$(Tag 0): I'm Ready! ($r)"
            $r = VmOn $agents[0] "click", "118", "560", "400"; Say "$(Tag 0): Start Game ($r)"
            Start-Sleep 6
            [void](VmOn $agents[0] "click", "118", "560", "400")
        } else {
            [void](Post-Click $pc 751 471); Start-Sleep 1; Say "PC: Record Game off"
            [void](Post-Click $pc 51 489); Start-Sleep 3; Say "PC: I'm Ready!"
            [void](Post-Click $pc 118 560 -HoldMs 400); Say "PC: Start Game"
            Start-Sleep 6
            [void](Post-Click $pc 118 560 -HoldMs 400)
        }
        $started = Get-Date
        foreach ($at in 30, $Play) {
            $w = $at - ((Get-Date) - $started).TotalSeconds
            if ($w -gt 0) { Start-Sleep -Milliseconds ([int]($w * 1000)) }
            if ($pc.HasExited) { throw "the PC game exited during the match (code $($pc.ExitCode))" }
            for ($i = 0; $i -lt $agents.Count; $i++) {
                if ((VmOn $agents[$i] "ps") -notmatch "(?i)age2_x1\.exe") { throw "the $(Tag $i) game is gone" }
                [void](VmOn $agents[$i] "shot", "$keep\$(Tag $i)-play-${at}s.png")
            }
            [void](Save-GameWindow $pc "$keep\pc-play-${at}s.png")
            Say "captured at $at s"
        }
    }
}
catch { $fail += "$_" }
finally {
    if ($pc) {
        $l = PcLog
        if ($l) {
            $s = Select-String -Path $l.FullName -Pattern "stats peer .* path " | Select-Object -Last $agents.Count
            $s | ForEach-Object { "  PC: " + ($_.Line -replace '^.*stats peer ', 'peer ' -replace ' \| bad token.*$', '') }
            $rs = Select-String -Path $l.FullName -Pattern "room state:|build: " | ForEach-Object { "  PC: " + ($_.Line -replace '^.*\] [A-Z] ', '') }
            $rs
        }
    }
    foreach ($i in @($vmPids.Keys)) { [void](VmOn $agents[$i] "kill", $vmPids[$i]) }
    Start-Sleep 2
    # each machine's newest session log comes back too
    for ($i = 0; $i -lt $agents.Count; $i++) {
        if (-not $vmPids.ContainsKey($i)) { continue }
        $list = VmOn $agents[$i] "ls", "$VmGame\netplay-logs\okplay-*.log"
        $newest = ($list -split "`n" | Where-Object { $_ -match "^file\t" } | ForEach-Object { ($_ -split "`t")[2] } | Sort-Object | Select-Object -Last 1)
        if ($newest) { [void](VmOn $agents[$i] "get", "$VmGame\netplay-logs\$newest", "$keep\$(Tag $i)-$newest"); "  $(Tag $i) log: $(Tag $i)-$newest" }
        foreach ($e in "OKP_TEST_LOBBY=", "OKP_TEST_LOBBY_SP=", "OKP_TEST_LOBBY_PLAYER=", "OKP_TEST_LOBBY_MAX=", "OKP_TEST_BUILD_ANSWER=", "OKP_TEST_HOST_PASSWORD=") { [void](VmOn $agents[$i] "setenv", $e) }
    }
    if ($st) { $r = Restore-DropIn $st $keep; if ($r) { $fail += $r } }
    if ($tempExe -and (Test-Path $tempExe)) { Remove-Item -Force $tempExe }
    "logs and captures in $keep"
}
if ($fail.Count) { $fail | ForEach-Object { "  - $_" }; "RESULT FAIL"; exit 1 }
"RESULT PASS"; exit 0
