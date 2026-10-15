<#
.SYNOPSIS
Runs the game unattended in deterministic mode and leaves the log, the per-frame
CSV, the state hashes and optional frame images in an output folder.

.DESCRIPTION
Each run gets its own copy of game\ms0 (the real save is never touched), a
guest-clocked RTC and audio sink, scripted dialogs and input from the replay
file only. Two runs of the same executable with the same replay must print the
same [state-hash] lines; -Repeat 2 checks exactly that.

The run ends at -Frames game frames when given, otherwise at the -Seconds
wall-clock deadline (the game stops itself; it is only killed if it then fails
to exit). Runs longer than about nine minutes should be started in the
background by the caller.

.EXAMPLE
.\run_replay.ps1 -Exe ..\..\..\out\chili\profiles\chili\Release\chili_recomp.exe -Replay boot.rec -Seconds 120 -OutDir C:\Temp\chili\run1

.EXAMPLE
.\run_replay.ps1 -Exe $exe -Replay boot.rec -Frames 1800 -Seconds 600 -HashEvery 300 -Repeat 2 -OutDir C:\Temp\chili\det
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$Replay = '',
    [ValidateRange(1, 7200)][int]$Seconds = 300,
    [Parameter(Mandatory = $true)][string]$OutDir,
    # Stop at this game frame (0 = run until the deadline).
    [int]$Frames = 0,
    [int]$HashEvery = 300,
    # Main-RAM ranges left out of the state hash (<address>:<bytes>, comma separated),
    # for bisecting a host value that leaks into guest memory.
    [string]$HashExclude = '',
    # Write frame_<n>.png every n game frames into <OutDir>\frames (0 = none).
    [int]$PngEvery = 0,
    # Run this many times into <OutDir>\run1.. and compare the state hashes.
    [ValidateRange(1, 10)][int]$Repeat = 1,
    [string]$Ms0Source = '',
    # Disc folder to run instead of game\disc0, e.g. a copy whose configpsp.txt has frame_lock = 1.
    [string]$Disc = '',
    # PSPRECOMP_LOG channels, e.g. "ge,display,audio".
    [string]$Log = '',
    # Leave the host clock, audio device and dialogs in place (not repeatable).
    [switch]$RealTime,
    # An ordinary window, activated and in front, for a run that is meant to be watched. Without it the
    # window opens behind the others and never takes the keyboard or the mouse from the window in use.
    [switch]$Watch,
    # Renderer: d3d12 or software. Empty leaves the choice to ChiliNative.ini (default d3d12).
    [ValidateSet('', 'd3d12', 'software')][string]$Backend = '',
    # d3d12 rendering resolution in multiples of 480x272, and multisampling (0 = ini or default).
    [ValidateRange(0, 8)][int]$Scale = 0,
    [ValidateSet(0, 1, 2, 4, 8)][int]$Msaa = 0,
    # Extra environment for the game, e.g. @{ CHILI_AUTO_MESSAGE = 'accept,decline' }.
    [hashtable]$Environment = @{},
    [int]$WaitForOtherSeconds = 0,
    # Also report frame-time percentiles for this frame range of the CSV.
    [int]$StatsFromFrame = 0,
    [int]$StatsToFrame = 0
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'run_common.ps1')

$Exe = Resolve-ChiliFullPath $Exe
$OutDir = Resolve-ChiliFullPath $OutDir
if ($Replay) {
    $Replay = Resolve-ChiliFullPath $Replay
    if (-not (Test-Path -LiteralPath $Replay -PathType Leaf)) { throw "Replay file not found: $Replay" }
}
if (-not $Ms0Source) { $Ms0Source = $script:ChiliMs0 }
$Ms0Source = Resolve-ChiliFullPath $Ms0Source
if ($Disc) { $Disc = Resolve-ChiliFullPath $Disc }
if ((Test-Path -LiteralPath $OutDir) -and @(Get-ChildItem -LiteralPath $OutDir -Force).Count -gt 0) {
    throw "Output folder is not empty, choose a new one: $OutDir"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$results = @()
for ($run = 1; $run -le $Repeat; $run++) {
    $dir = $OutDir
    if ($Repeat -gt 1) { $dir = Join-Path $OutDir "run$run"; New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    $ms0 = Join-Path $dir 'ms0'
    New-ChiliMs0Copy $Ms0Source $ms0
    $logPath = Join-Path $dir 'run.log'
    $csvPath = Join-Path $dir 'frames.csv'
    $variables = @{
        CHILI_INPUT_REPLAY      = $Replay
        CHILI_FRAME_LOG         = $csvPath
        CHILI_MS0_DIR           = $ms0
        PSPRECOMP_PROBE_SECONDS = $Seconds
        PSPRECOMP_FRAME_DUMP    = (Join-Path $dir 'final.png')
        PSPRECOMP_LOG           = $Log
    }
    if (-not $RealTime) { $variables.CHILI_DETERMINISTIC = 1 }
    if ($Watch) { $variables.CHILI_WATCH = 1 }
    if ($Backend) { $variables.CHILI_RENDER_BACKEND = $Backend }
    # A scripted run that asks for d3d12 must not quietly fall back to software.
    if ($Backend -eq 'd3d12') { $variables.CHILI_RENDER_STRICT = 1 }
    if ($Scale -gt 0) { $variables.CHILI_RENDER_SCALE = $Scale }
    if ($Msaa -gt 0) { $variables.CHILI_RENDER_MSAA = $Msaa }
    if ($HashEvery -gt 0) { $variables.CHILI_STATE_HASH_EVERY = $HashEvery; $variables.CHILI_STATE_HASH_EXCLUDE = $HashExclude }
    if ($Frames -gt 0) { $variables.CHILI_STOP_AT_FRAME = $Frames }
    if ($PngEvery -gt 0) {
        $pngDir = Join-Path $dir 'frames'
        New-Item -ItemType Directory -Force -Path $pngDir | Out-Null
        $variables.CHILI_FRAME_PNG_EVERY = $PngEvery; $variables.CHILI_FRAME_PNG_DIR = $pngDir
    }
    foreach ($key in $Environment.Keys) { $variables[$key] = $Environment[$key] }

    Assert-ChiliIdle $WaitForOtherSeconds
    $started = [DateTime]::UtcNow
    $process = Start-ChiliGame $Exe $variables $logPath (Join-Path $dir 'run.err.log') -GameRoot $Disc
    # The game enforces the deadline itself; the extra time covers a long frame
    # in flight and the exit path.
    $killed = $false
    if (-not $process.WaitForExit(($Seconds + 120) * 1000)) {
        $killed = $true
        try { $process.Kill() } catch { }
        $process.WaitForExit(10000) | Out-Null
    }
    $wall = ([DateTime]::UtcNow - $started).TotalSeconds
    $result = Get-ChiliRunResult $logPath $csvPath $StatsFromFrame $StatsToFrame
    $errorText = Read-ChiliSharedText (Join-Path $dir 'run.err.log')
    if ($errorText -and $errorText -match '(?m)^Chili bootstrap failure.*$') { $result.failure = $Matches[0].Trim() }
    [IO.File]::WriteAllLines((Join-Path $dir 'hashes.txt'), [string[]]@($result.hashes))
    $summary = [ordered]@{
        run         = $run
        folder      = $dir
        exit_code   = $(if ($killed) { 'killed' } else { $process.ExitCode })
        wall_seconds = [Math]::Round($wall, 1)
        ended       = $result.ended
        failure     = $result.failure
        frame_timer = $result.frame_timer
        window      = $result.window
        checkpoints = $result.hashes.Count
        last_hash   = ($result.hashes | Select-Object -Last 1)
        dialogs     = $result.dialogs
    }
    $summary | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $dir 'summary.json') -Encoding ASCII
    $results += [pscustomobject]$summary
    Write-Output ("run {0}: exit={1} wall={2}s checkpoints={3}" -f $run, $summary.exit_code, $summary.wall_seconds, $summary.checkpoints)
    if ($summary.failure) { Write-Output "  $($summary.failure)" }
    if ($summary.ended) { Write-Output "  $($summary.ended)" }
    if ($summary.frame_timer) { Write-Output "  $($summary.frame_timer)" }
    if ($summary.window) { Write-Output "  $($summary.window)" }
    if ($summary.last_hash) { Write-Output "  $($summary.last_hash)" }
    $errors = Join-Path $dir 'run.err.log'
    if ((Test-Path -LiteralPath $errors) -and (Get-Item -LiteralPath $errors).Length -gt 0) {
        Write-Output '  stderr (last lines):'
        Get-Content -LiteralPath $errors -Tail 5 | ForEach-Object { Write-Output "    $_" }
    }
}

$status = 0
if ($Repeat -gt 1) {
    # Compare the hash value at each frame. The line also carries ram/cpu parts
    # so a mismatch says which half diverged.
    $reference = @(Get-Content -LiteralPath (Join-Path $OutDir 'run1\hashes.txt'))
    $same = $reference.Count -gt 0
    for ($run = 2; $run -le $Repeat; $run++) {
        $other = @(Get-Content -LiteralPath (Join-Path $OutDir "run$run\hashes.txt"))
        $count = [Math]::Min($reference.Count, $other.Count)
        if ($count -eq 0) { $same = $false; Write-Output "run $run printed no state hashes" }
        for ($i = 0; $i -lt $count; $i++) {
            if ($reference[$i] -ne $other[$i]) {
                $same = $false
                Write-Output "DIVERGED at checkpoint $($i + 1):"
                Write-Output "  run1: $($reference[$i])"
                Write-Output "  run${run}: $($other[$i])"
                break
            }
        }
        if ($reference.Count -ne $other.Count) {
            Write-Output "run1 has $($reference.Count) checkpoints, run$run has $($other.Count) (only the common ones were compared)"
        }
    }
    if ($same) { Write-Output "DETERMINISTIC: $Repeat runs agree on all compared state hashes ($($reference.Count) in run1)" }
    else { $status = 3 }
}
foreach ($entry in $results) { if ($entry.failure -or $entry.exit_code -eq 'killed' -or $entry.exit_code -ne 0) { if ($status -eq 0) { $status = 1 } } }
exit $status
