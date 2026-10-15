<#
.SYNOPSIS
    Checks the start a player gets from a package: the launcher, with "Skip this window next time" on,
    saves the settings and starts the game, and both are gone when the game ends.

.DESCRIPTION
    Give it a COPY of a package folder (scripts\package.ps1 -Out <folder>), never the folder someone plays
    from: the game is really started in it, for some seconds, and may write its settings, logs and saves.

    The launcher is started as a double-click starts it, with launcher.ini saying StartGameDirectly=true.
    The game it starts is a run nobody watches: its window opens behind the others and never takes the
    keyboard or the mouse, it makes no sound, and it ends by itself after -Seconds. The launcher hands
    those three switches on only because CHILI_LAUNCHER_CHECK=1 is set here; a player's start has none
    of them. Should the launcher have to show its window (the game failed), the window does not take the
    focus either, and this script closes it.

    Looked at afterwards:
      - the launcher wrote the settings file before the game started (the frame rate line it pins);
      - the game's own log names that file as the one it read, and found the disc through [Disc] Path;
      - the game ended with exit code 0, the launcher ended by itself, and no process of the folder is left.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File check_package_start.ps1 -Package C:\Temp\chili\ChiliConCarnagePC
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Package,
    [ValidateRange(5, 600)][int]$Seconds = 20
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'run_common.ps1')
$Package = (Resolve-ChiliFullPath $Package).TrimEnd('\')
$launcher = Join-Path $Package 'ChiliConCarnageLauncher.exe'
$game = Join-Path $Package 'ChiliConCarnage.exe'
$ini = Join-Path $Package 'ChiliNative.ini'
$launcherIni = Join-Path $Package 'launcher.ini'
$log = Join-Path $Package 'logs\ChiliConCarnage.log'
foreach ($needed in $launcher, $game, $ini) { if (-not (Test-Path -LiteralPath $needed -PathType Leaf)) { throw "Not a package with a launcher and settings: $needed is missing" } }
# The folder beside this checkout that a player starts from is not a place for this check.
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
foreach ($theirs in (Join-Path (Split-Path -Parent $repo) 'dist'), (Join-Path (Split-Path -Parent $repo) 'play')) {
    if (($Package + '\').StartsWith((Resolve-ChiliFullPath $theirs).TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "This check starts the game. Give it a copy of the package, not $Package"
    }
}
# One launcher at a time (a second start only brings the first to the front), and no game beside a game.
$busy = @(Get-Process | Where-Object { $_.ProcessName -eq 'ChiliConCarnageLauncher' -or $_.ProcessName -eq 'ChiliConCarnage' -or $_.ProcessName -like 'chili_recomp*' })
if ($busy.Count) { throw "A game or a launcher is running ($($busy[0].ProcessName), pid $($busy[0].Id)). Close it first." }

function Get-LiveLine([string]$Path, [string]$Section, [string]$Key) {
    $in = ''; $found = $null
    foreach ($raw in [IO.File]::ReadAllLines($Path)) {
        $line = $raw.Trim()
        if ($line.Length -eq 0 -or $line[0] -eq ';' -or $line[0] -eq '#') { continue }
        if ($line[0] -eq '[') { $in = $line.Trim('[', ']').Trim(); continue }
        $at = $line.IndexOf('=')
        if ($in -eq $Section -and $at -ge 1 -and $line.Substring(0, $at).Trim() -eq $Key) { $found = $line.Substring($at + 1).Trim() }
    }
    return $found
}

# "Skip this window next time": the key the switch of the Play page writes.
$before = $null
if (Test-Path -LiteralPath $launcherIni) { $before = [IO.File]::ReadAllText($launcherIni) }
$text = $(if ($null -ne $before) { $before } else { "[Launcher]`r`nGame=ChiliConCarnage.exe`r`nStartGameDirectly=false`r`nAfterGame=return`r`n" })
if ($text -match '(?m)^StartGameDirectly\s*=.*$') { $text = $text -replace '(?m)^StartGameDirectly\s*=.*$', 'StartGameDirectly=true' }
else { $text = $text -replace '(?m)^\[Launcher\]\s*$', "[Launcher]`r`nStartGameDirectly=true" }
[IO.File]::WriteAllText($launcherIni, $text, (New-Object Text.UTF8Encoding($false)))

$frameLockBefore = Get-LiveLine $ini 'Game' 'FrameLock'
$discBefore = Get-LiveLine $ini 'Disc' 'Path'
$iniBefore = [IO.File]::ReadAllBytes($ini)
$logBefore = $(if (Test-Path -LiteralPath $log) { (Get-Item -LiteralPath $log).LastWriteTimeUtc } else { [DateTime]::MinValue })
"[start-check] package $Package"
"[start-check] before: [Game] FrameLock is $(if ($null -eq $frameLockBefore) { 'not set' } else { $frameLockBefore }); [Disc] Path is $(if ($discBefore) { $discBefore } else { 'not set' })"

$variables = @{ CHILI_LAUNCHER_CHECK = 1; CHILI_UNATTENDED = 1; CHILI_NULL_AUDIO = 1; PSPRECOMP_PROBE_SECONDS = $Seconds }
Repair-ChiliPathVariable
$names = @($script:ChiliVariables) + @('CHILI_UNATTENDED', 'CHILI_DISC_ANSWER', 'CHILI_LAUNCHER_CHECK') | Select-Object -Unique
$saved = @{}
foreach ($name in $names) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    $value = $null
    if ($variables.ContainsKey($name)) { $value = "$($variables[$name])" }
    [Environment]::SetEnvironmentVariable($name, $value, 'Process')
}
$problems = @()
$gameSeen = $null; $gameCode = $null; $parent = $null
$watch = [Diagnostics.Stopwatch]::StartNew()
try {
    $started = Get-Date
    $process = Start-Process -FilePath $launcher -WorkingDirectory $Package -PassThru
    $null = $process.Handle
} finally { foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') } }
# The game the launcher starts: found by its path, its handle kept so that its exit code can be read.
$deadline = $Seconds + 90
while (-not $process.HasExited -and $watch.Elapsed.TotalSeconds -lt $deadline) {
    if (-not $gameSeen) {
        $found = @(Get-CimInstance Win32_Process -Filter "Name='ChiliConCarnage.exe'" -ErrorAction SilentlyContinue | Where-Object { $_.ExecutablePath -eq $game })
        if ($found.Count) {
            try { $gameSeen = Get-Process -Id $found[0].ProcessId -ErrorAction Stop; $null = $gameSeen.Handle; $parent = $found[0].ParentProcessId } catch { $gameSeen = $null }
        }
    }
    Start-Sleep -Milliseconds 200
}
$launcherShown = $false
if (-not $process.HasExited) {
    # The launcher stayed: it shows its window because the game did not start or failed. Close it.
    $launcherShown = $true
    $problems += "the launcher was still there after $deadline s (it shows its window when the game cannot be started or fails)"
    try { [void]$process.CloseMainWindow() } catch { }
    if (-not $process.WaitForExit(5000)) { try { $process.Kill() } catch { } }
}
if ($gameSeen) { if ($gameSeen.WaitForExit(15000)) { $gameCode = $gameSeen.ExitCode } }
$wall = $watch.Elapsed.TotalSeconds

# Nothing of the folder may be left running.
Start-Sleep -Milliseconds 500
$left = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($Package + '\', [StringComparison]::OrdinalIgnoreCase) })
foreach ($one in $left) {
    $problems += "left running: $($one.Name) pid $($one.ProcessId)"
    try { Stop-Process -Id $one.ProcessId -Force } catch { }
}

# launcher.ini as it was, but for what the launcher itself added (what the game program knows).
if ($null -ne $before) {
    $now = [IO.File]::ReadAllText($launcherIni)
    $was = [regex]::Match($before, '(?m)^StartGameDirectly\s*=.*$').Value
    if ($was) { $now = $now -replace '(?m)^StartGameDirectly\s*=.*$', $was.TrimEnd("`r") } else { $now = $now -replace '(?m)^StartGameDirectly=true\r?\n', '' }
    [IO.File]::WriteAllText($launcherIni, $now, (New-Object Text.UTF8Encoding($false)))
}

# What happened.
"[start-check] the launcher ran for $([Math]::Round($wall, 1)) s and ended with exit code $(if ($process.HasExited) { $process.ExitCode } else { 'none' })"
if (-not $gameSeen) { $problems += 'no ChiliConCarnage.exe of the package was seen running' }
else {
    "[start-check] the game ran as pid $($gameSeen.Id), started by pid $parent (the launcher was pid $($process.Id)); exit code $gameCode"
    if ($parent -ne $process.Id) { $problems += "the game's parent was pid $parent, not the launcher" }
    if ($gameCode -ne 0) { $problems += "the game ended with exit code $gameCode" }
}
$frameLockAfter = Get-LiveLine $ini 'Game' 'FrameLock'
$iniAfter = [IO.File]::ReadAllBytes($ini)
$changed = -not [Linq.Enumerable]::SequenceEqual($iniBefore, $iniAfter)
"[start-check] ChiliNative.ini: $(if ($changed) { 'written' } else { 'not changed' }); [Game] FrameLock is now $(if ($null -eq $frameLockAfter) { 'not set' } else { $frameLockAfter })"
if ($null -eq $frameLockBefore -and $null -eq $frameLockAfter) { $problems += 'the launcher did not write the frame rate line it shows ([Game] FrameLock)' }
$lines = @()
if ((Test-Path -LiteralPath $log) -and (Get-Item -LiteralPath $log).LastWriteTimeUtc -gt $logBefore) { $lines = @(Get-Content -LiteralPath $log) }
else { $problems += "the game wrote no log ($log)" }
$wanted = '^\[start\]', '^\[disc\]', '^\[input\] bindings:', '^\[game\] frame rate', '^\[options\] scripted run', '^\[render\] Direct3D', '^\[frame-timer\]', '^Execution ended', '^\[message\]', '^\[crash\]'
foreach ($line in $lines) { foreach ($pattern in $wanted) { if ($line -match $pattern) { '[start-check]   log: ' + $line.Substring(0, [Math]::Min(230, $line.Length)); break } } }
if ($lines.Count) {
    if (-not ($lines | Where-Object { $_ -eq "[input] bindings: $ini" })) { $problems += "the game's log does not name $ini as the settings file it read" }
    if (-not ($lines | Where-Object { $_ -match '^\[disc\] .*named by \[Disc\] Path' })) { $problems += 'the game did not take the disc from [Disc] Path' }
    $rate = [string]($lines | Where-Object { $_ -like '[[]game[]] frame rate:*' } | Select-Object -First 1)
    if ($rate -match 'default for') { $problems += "the game did not find the frame rate line in the settings file: $rate" }
    if (-not ($lines | Where-Object { $_ -like 'Execution ended*' })) { $problems += 'the log has no "Execution ended" line' }
    $complaints = @($lines | Where-Object { $_ -match '^\[(input|mods|render|audio)\] ChiliNative\.ini .*(unknown|must be)' })
    foreach ($line in $complaints) { $problems += "the game says: $line" }
}
if ($problems.Count) { $problems | ForEach-Object { "[start-check] FAIL $_" }; exit 1 }
'[start-check] PASS: the launcher saved the settings and started the game, the game read that file and the disc it names, ended with exit code 0, and nothing is left running'
exit 0
