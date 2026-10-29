# Real-game smoke test for the drop-in: install into the game folder, start the game, read its log,
# restore the folder. Logs only - no screenshots.
#
#   powershell -File tests\realgame_smoke.ps1 [-GameDir <game>\age2_x1] [-Release <drop-in folder>]
#
# Defaults are found like ../../env.sh does: GameDir from $env:AOE2_GAME_DIR, then AOE2_GAME_DIR in
# the workspace's local.env, then the game's registry entry, then a <folder>\age2_x1 beside the
# workspace; Release = the newest "* drop-in v*" folder in the deliverables.
#
# PASS needs: the window is never titled "Error"; the intro movies decode (frames > 0, 0 failed);
# both connections initialise when the game probes them; no unfinished Wine code reached. The game
# probes them at the main menu, after the intros: ~45 s on a PC where it cuts the intros short (and
# then exits by itself, also with Windows' DirectPlay), ~95 s where they play in full. The test stops
# the game 5 s after both connections are up, or at -Seconds.
#
# -Mode windows starts the game with OKP_MODE=windows (classic Windows DirectPlay, see shim/mode.c):
# then PASS needs the log to say so, the intro movies to decode as usual, and okplay's connections
# to stay off; the game runs the full -Seconds (default 110 in that mode).
param(
    [string]$GameDir = "",
    [string]$Release = "",
    [int]$Seconds = 180,
    [ValidateSet("", "all", "oldkid", "windows")][string]$Mode = "",
    [switch]$NoIntro   # intro=0 in the copied okplay.ini: the movies must be skipped (NOSTARTUP)
)
if ($Mode -eq "windows" -and -not $PSBoundParameters.ContainsKey("Seconds")) { $Seconds = 110 }
$expectMode = if ($Mode) { $Mode } else { "all" }   # the shipped okplay.ini says mode=all
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
function LocalEnv($name) { # KEY=value from local.env; MSYS paths (/c/x) become C:\x
    $f = Join-Path $root "local.env"
    if (-not (Test-Path $f)) { return $null }
    $line = Get-Content $f | Where-Object { $_ -match "^\s*$name=" } | Select-Object -Last 1
    if (-not $line) { return $null }
    $v = ($line -replace "^\s*$name=", "").Trim().Trim('"', "'")
    if ($v -match '^/([a-zA-Z])/(.*)$') { $v = "$($Matches[1]):\$($Matches[2])" }
    return $v -replace '/', '\'
}
if (-not $GameDir) { $GameDir = $env:AOE2_GAME_DIR }
if (-not $GameDir) { $GameDir = LocalEnv "AOE2_GAME_DIR" }
if (-not $GameDir) {
    $reg = Get-ItemProperty "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Microsoft Games\Age of Empires II: The Conquerors Expansion\1.0" -ErrorAction SilentlyContinue
    if ($reg -and $reg.'EXE Path' -and (Test-Path (Join-Path $reg.'EXE Path' "age2_x1"))) { $GameDir = Join-Path $reg.'EXE Path' "age2_x1" }
}
if (-not $GameDir) {
    $GameDir = Get-ChildItem (Split-Path $root -Parent) -Directory | ForEach-Object { Join-Path $_.FullName "age2_x1" } |
        Where-Object { Get-ChildItem (Join-Path $_ "age2_x1*.exe") -ErrorAction SilentlyContinue } | Select-Object -First 1
}
if (-not $GameDir) { "no game folder found - pass -GameDir or set AOE2_GAME_DIR in $root\local.env"; exit 2 }
if (-not $Release) {
    $del = LocalEnv "DELIVERABLES"; if (-not $del) { $del = Join-Path $root "deliverables" }
    $Release = Get-ChildItem $del -Directory -Filter "* drop-in v*" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime | Select-Object -Last 1 -ExpandProperty FullName
}
if (-not $Release) { "no drop-in release found - run winedplay/package.sh or pass -Release"; exit 2 }
"game:    $GameDir"
"release: $Release"
$files = "dplayx.dll", "okindeo.dll", "okplay.ini"
$keep = Join-Path (Split-Path $PSScriptRoot -Parent) ("test-logs\realgame-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
$exe = @("age2_x1.5.exe", "age2_x1.exe") | ForEach-Object { Join-Path $GameDir $_ } | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $exe) { "no game exe in $GameDir"; exit 2 }
foreach ($f in $files + "netplay-logs") { if (Test-Path (Join-Path $GameDir $f)) { "refusing: $f already in $GameDir"; exit 2 } }
if (Get-Process | Where-Object { $_.ProcessName -like "age2*" }) { "refusing: the game is running"; exit 2 }

$fail = @()
try {
    foreach ($f in $files) { Copy-Item (Join-Path $Release $f) $GameDir }
    if ($NoIntro) {
        $iniPath = Join-Path $GameDir "okplay.ini"
        $ini = (Get-Content $iniPath) -replace '^intro=.*$', 'intro=0'
        if (-not ($ini -match '^intro=0$')) { $ini = $ini + 'intro=0' }
        Set-Content -Path $iniPath -Value $ini -Encoding ASCII
        "intro:   0 (skip the movies)"
    }
    $start = Get-Date
    $oldMode = $env:OKP_MODE
    if ($Mode) { $env:OKP_MODE = $Mode } else { Remove-Item Env:OKP_MODE -ErrorAction SilentlyContinue }
    $p = Start-Process -FilePath $exe -WorkingDirectory $GameDir -PassThru
    $env:OKP_MODE = $oldMode
    "mode:    $expectMode"
    $titles = @{}
    $ready = $null
    while (((Get-Date) - $start).TotalSeconds -lt $Seconds -and -not $p.HasExited) {
        Start-Sleep -Milliseconds 1000
        # both connections up = the main menu probed them: give it 5 s more, then we have what we need
        if (-not $ready) {
            $l = Get-ChildItem (Join-Path $GameDir "netplay-logs\okplay-*.log") -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
            if ($l) {
                $t = Get-Content $l.FullName -ErrorAction SilentlyContinue
                if (($t | Select-String "SPInit: .*Online Play") -and ($t | Select-String "SPInit: .*LAN Play")) {
                    $ready = Get-Date
                    "{0,5:N1}s both connections initialised" -f ($ready - $start).TotalSeconds
                }
            }
        } elseif (((Get-Date) - $ready).TotalSeconds -ge 5) { break }
        $w = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
        if ($w -and $w.MainWindowTitle -and -not $titles[$w.MainWindowTitle]) {
            $titles[$w.MainWindowTitle] = 1
            "{0,5:N1}s window '{1}'" -f ((Get-Date) - $start).TotalSeconds, $w.MainWindowTitle
        }
    }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Start-Sleep 2; "stopped the game at {0:N0} s" -f ((Get-Date) - $start).TotalSeconds }
    else { "{0,5:N1}s the game exited (code {1})" -f ((Get-Date) - $start).TotalSeconds, $p.ExitCode }

    if ($titles["Error"]) { $fail += "the game showed an Error window" }
    $log = Get-ChildItem (Join-Path $GameDir "netplay-logs\okplay-*.log") -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
    if (-not $log) { $fail += "no session log (dplayx.dll not loaded?)" }
    else {
        $text = Get-Content $log.FullName
        $frames = ($text | Select-String "video: (\d+) intro frames decoded this session \((\d+) failed\)" | Select-Object -Last 1)
        $played = ($text | Select-String "video: Indeo 5 .* playing").Count
        "log: $($log.Name)  intro starts: $played  $($frames.Line -replace '^.*video: ', '')"
        if ($NoIntro) {
            if (-not ($text | Select-String "intro: intro movies skipped")) { $fail += "the log does not say the intros were skipped" }
            if ($played -gt 0) { $fail += "intro=0, but $played intro movie(s) still played" }
        } elseif ($played -lt 1) { $fail += "no intro movie reached the decoder" }
        if ($frames -and $frames.Matches[0].Groups[2].Value -ne "0") { $fail += "intro frames failed to decode" }
        if (-not ($text | Select-String "mode: $expectMode ")) { $fail += "the log does not say mode: $expectMode" }
        if ($expectMode -eq "windows") {
            if ($text | Select-String "SPInit: ") { $fail += "okplay's connections started in windows mode" }
        } else {
            if (-not ($text | Select-String "SPInit: .*Online Play")) { $fail += "Online Play never initialised" }
            if (-not ($text | Select-String "SPInit: .*LAN Play")) { $fail += "LAN Play never initialised" }
        }
        $handoff = ($text | Select-String "all: classic connection").Count
        if ($expectMode -eq "all") { "classic connections handed to Windows' DirectPlay: $handoff" }
        $unfinished = $text | Select-String "census: unfinished \(fixme\) reached: (?!none)"
        if ($unfinished) { $fail += "unfinished Wine code reached: $($unfinished[-1].Line -replace '^.*reached: ', '')" }
        if ($text | Select-String -CaseSensitive "=+ CRASH =+|UNHANDLED - process") { $fail += "crash report in the log" }
    }
}
finally {
    Get-Process | Where-Object { $_.ProcessName -like "age2*" } | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep 1
    if (Test-Path (Join-Path $GameDir "netplay-logs")) {
        New-Item -ItemType Directory -Force $keep | Out-Null
        Move-Item -Force (Join-Path $GameDir "netplay-logs\*") $keep
        Remove-Item -Force (Join-Path $GameDir "netplay-logs")
    }
    foreach ($f in $files + "dplayx-windows9x.dll") { Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $GameDir $f) }
    $left = $files + "netplay-logs" | Where-Object { Test-Path (Join-Path $GameDir $_) }
    if ($left) { $fail += "could not restore the game folder: $left" } else { "game folder restored; logs in $keep" }
}
if ($fail.Count) { $fail | ForEach-Object { "  - $_" }; "RESULT FAIL"; exit 1 }
"RESULT PASS"; exit 0
