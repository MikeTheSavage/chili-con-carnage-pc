<#
.SYNOPSIS
Starts a live session: the game runs detached, takes input from live_send.ps1
and records every controller sample it was given.

.DESCRIPTION
This is how a replay gets authored. The session runs in deterministic mode by
default, so the recording (<OutDir>\input.rec) played back with run_replay.ps1
reproduces the session exactly. Pass -Replay to start from an existing
recording and continue it by hand: the new recording contains both.

The game uses a copy of game\ms0 inside the session folder; the real save is
never touched. The script returns as soon as the game has started. Session
files:
  session.json   paths and the process id (read by live_send.ps1)
  control.txt    the command file, control.txt.ack the acknowledgement
  input.rec      the recording
  run.log        game output (flushed line by line), frames.csv per-frame times
  frame_<seq>_<label>.png   captures

Stop it with:  live_send.ps1 -Session <OutDir> -Action quit

.EXAMPLE
.\run_live.ps1 -Exe $exe -OutDir C:\Temp\chili\live1
.\live_send.ps1 -Session C:\Temp\chili\live1 -Action tap -Buttons cross -Samples 4 -ThenCapture after_cross
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [ValidateRange(1, 7200)][int]$Seconds = 3600,
    [string]$Replay = '',
    [string]$Ms0Source = '',
    # Disc folder to run instead of game\disc0.
    [string]$Disc = '',
    [int]$HashEvery = 0,
    # Same meaning as in run_replay.ps1.
    [string]$HashExclude = '',
    [string]$Log = '',
    # Host clock, audio device and native dialogs instead of deterministic mode.
    # The recording then replays the input but not the timing.
    [switch]$RealTime,
    # An ordinary window, activated and in front. Without it the window opens behind the others and never
    # takes the keyboard or the mouse from the window in use.
    [switch]$Watch,
    # Renderer: d3d12 or software. Empty leaves the choice to ChiliNative.ini (default d3d12).
    [ValidateSet('', 'd3d12', 'software')][string]$Backend = '',
    [ValidateRange(0, 8)][int]$Scale = 0,
    [ValidateSet(0, 1, 2, 4, 8)][int]$Msaa = 0,
    [hashtable]$Environment = @{},
    [int]$WaitForOtherSeconds = 0
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
    throw "Session folder is not empty, choose a new one: $OutDir"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$ms0 = Join-Path $OutDir 'ms0'
New-ChiliMs0Copy $Ms0Source $ms0
$session = [ordered]@{
    exe      = $Exe
    folder   = $OutDir
    control  = (Join-Path $OutDir 'control.txt')
    ack      = (Join-Path $OutDir 'control.txt.ack')
    sequence = (Join-Path $OutDir 'control.seq')
    frames   = (Join-Path $OutDir 'frame')
    record   = (Join-Path $OutDir 'input.rec')
    log      = (Join-Path $OutDir 'run.log')
    errors   = (Join-Path $OutDir 'run.err.log')
    csv      = (Join-Path $OutDir 'frames.csv')
    ms0      = $ms0
    deterministic = (-not $RealTime)
    pid      = 0
    started  = [DateTime]::UtcNow.ToString('o')
}
$variables = @{
    PSPRECOMP_LIVE_CONTROL      = $session.control
    PSPRECOMP_LIVE_FRAME_PREFIX = $session.frames
    CHILI_INPUT_RECORD          = $session.record
    CHILI_INPUT_REPLAY          = $Replay
    CHILI_FRAME_LOG             = $session.csv
    CHILI_MS0_DIR               = $ms0
    CHILI_LOG_FLUSH             = 1
    PSPRECOMP_PROBE_SECONDS     = $Seconds
    PSPRECOMP_FRAME_DUMP        = (Join-Path $OutDir 'final.png')
    PSPRECOMP_LOG               = $Log
}
if (-not $RealTime) { $variables.CHILI_DETERMINISTIC = 1 }
if ($Watch) { $variables.CHILI_WATCH = 1 }
if ($Backend) { $variables.CHILI_RENDER_BACKEND = $Backend }
if ($Backend -eq 'd3d12') { $variables.CHILI_RENDER_STRICT = 1 }
if ($Scale -gt 0) { $variables.CHILI_RENDER_SCALE = $Scale }
if ($Msaa -gt 0) { $variables.CHILI_RENDER_MSAA = $Msaa }
if ($HashEvery -gt 0) { $variables.CHILI_STATE_HASH_EVERY = $HashEvery; $variables.CHILI_STATE_HASH_EXCLUDE = $HashExclude }
foreach ($key in $Environment.Keys) { $variables[$key] = $Environment[$key] }

Assert-ChiliIdle $WaitForOtherSeconds
$process = Start-ChiliGame $Exe $variables $session.log $session.errors -GameRoot $Disc
$session.pid = $process.Id
$session | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutDir 'session.json') -Encoding ASCII

# Give it a moment to fail fast (bad paths, missing DLLs) before reporting success.
Start-Sleep -Milliseconds 1500
if ($process.HasExited) {
    Write-Output "Game exited immediately with code $($process.ExitCode)."
    foreach ($file in @($session.log, $session.errors)) {
        if (Test-Path -LiteralPath $file) { Get-Content -LiteralPath $file -Tail 15 | ForEach-Object { Write-Output "  $_" } }
    }
    exit 1
}
Write-Output "Live session started: pid $($process.Id), deadline ${Seconds}s"
Write-Output "  session  $OutDir"
Write-Output "  send     $(Join-Path $PSScriptRoot 'live_send.ps1') -Session `"$OutDir`" -Action tap -Buttons cross"
Write-Output "  record   $($session.record)"
exit 0
