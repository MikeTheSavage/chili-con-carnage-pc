<#
.SYNOPSIS
Sends one command to a live session started by run_live.ps1 and waits for the
game to acknowledge it.

.DESCRIPTION
Actions:
  tap      hold the pad state for -Samples controller samples (the game reads
           one sample per game frame). Exact at any speed; prefer it.
  press    hold the pad state for -DurationMs (guest milliseconds in a
           deterministic session, host milliseconds otherwise; at most 5000)
  hold     hold the pad state until the next tap/press/hold/release
  release  let go
  analog   sticks only: like hold, or like tap/press when -Samples/-DurationMs is given
  capture  save the displayed frame as <session>\frame_<seq>_<label>.png
  quit     stop the game cleanly (writes the recording's end marker)
  status   print session progress, send nothing

-Buttons takes names (cross circle square triangle up down left right start
select l r) or a number. -X/-Y are the analog stick, 0..255, 128 centred.
-RX/-RY are the second stick of the dual_analog scheme (turn and pitch); the
psp scheme ignores them.
-ThenWaitMs / -ThenWaitFrames / -ThenCapture chain a wait and a capture after
the command, which is the usual "press, then look" step.

Sequence numbers are kept in the session folder; commands must be sent one at
a time.

.EXAMPLE
.\live_send.ps1 -Session C:\Temp\chili\live1 -Action tap -Buttons cross -Samples 4 -ThenWaitFrames 30 -ThenCapture menu
.\live_send.ps1 -Session C:\Temp\chili\live1 -Action analog -X 128 -Y 0 -Samples 60
.\live_send.ps1 -Session C:\Temp\chili\live1 -Action analog -RX 255 -Samples 30
.\live_send.ps1 -Session C:\Temp\chili\live1 -Action quit
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Session,
    [Parameter(Mandatory = $true)][ValidateSet('tap', 'press', 'hold', 'release', 'analog', 'capture', 'quit', 'status')][string]$Action,
    [string[]]$Buttons = @(),
    [ValidateRange(1, 3600)][int]$Samples = 4,
    [ValidateRange(1, 5000)][int]$DurationMs = 200,
    [ValidateRange(0, 255)][int]$X = 128,
    [ValidateRange(0, 255)][int]$Y = 128,
    [ValidateRange(0, 255)][int]$RX = 128,
    [ValidateRange(0, 255)][int]$RY = 128,
    [string]$Label = 'frame',
    [int]$TimeoutSeconds = 120,
    [int]$ThenWaitMs = 0,
    [int]$ThenWaitFrames = 0,
    [string]$ThenCapture = ''
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'run_common.ps1')

$Session = Resolve-ChiliFullPath $Session
$sessionFile = Join-Path $Session 'session.json'
if (-not (Test-Path -LiteralPath $sessionFile)) { throw "Not a live session folder (no session.json): $Session" }
$info = Get-Content -LiteralPath $sessionFile -Raw | ConvertFrom-Json

$buttonBits = @{
    select = 0x0001; start = 0x0008; up = 0x0010; right = 0x0020; down = 0x0040; left = 0x0080
    l = 0x0100; r = 0x0200; triangle = 0x1000; circle = 0x2000; cross = 0x4000; square = 0x8000
}
function Get-ButtonMask([string[]]$Names) {
    $mask = 0
    foreach ($name in $Names) {
        foreach ($part in ($name -split '[,+ ]+' | Where-Object { $_ })) {
            $key = $part.ToLowerInvariant()
            if ($buttonBits.ContainsKey($key)) { $mask = $mask -bor $buttonBits[$key] }
            elseif ($part -match '^(0x[0-9A-Fa-f]+|\d+)$') { $mask = $mask -bor [int]$part }
            else { throw "Unknown button '$part'. Use: $(($buttonBits.Keys | Sort-Object) -join ' ') or a number." }
        }
    }
    if ($mask -band (-bnot 0xF3F9)) { throw ('Button mask 0x{0:X} has bits outside 0xF3F9' -f $mask) }
    return $mask
}
function Test-GameRunning {
    $process = Get-Process -Id $info.pid -ErrorAction SilentlyContinue
    return ($null -ne $process -and -not $process.HasExited -and $process.ProcessName -like 'chili_recomp*')
}
# Last row of the per-frame CSV: game frame number and controller samples so far.
function Get-Progress {
    $text = Read-ChiliSharedText $info.csv
    if (-not $text) { return $null }
    $lines = @($text -split "`r?`n" | Where-Object { $_ -match '^\d' })
    if ($lines.Count -eq 0) { return $null }
    $fields = $lines[-1] -split ','
    return [pscustomobject]@{ frame = [int64]$fields[0]; guest_ms = [double]$fields[5]; samples = [int64]$fields[6]; wall_ms = [double]$fields[1] }
}
function Get-AckLine {
    $text = Read-ChiliSharedText $info.ack
    if ($null -eq $text) { return '' }
    return $text.Trim()
}
function Send-Command([string]$Body) {
    if (-not (Test-GameRunning)) { throw "The game (pid $($info.pid)) is not running; see $($info.log)" }
    $last = 0
    if (Test-Path -LiteralPath $info.sequence) { $last = [int64](Get-Content -LiteralPath $info.sequence -Raw).Trim() }
    $before = Get-AckLine
    if ($before -match '^(\d+) ') { $last = [Math]::Max($last, [int64]$Matches[1]) }
    $sequence = $last + 1
    [IO.File]::WriteAllText($info.sequence, "$sequence`n", [Text.Encoding]::ASCII)
    $stamp = [DateTime]::MinValue
    if (Test-Path -LiteralPath $info.ack) { $stamp = (Get-Item -LiteralPath $info.ack).LastWriteTimeUtc }
    Publish-ChiliText $info.control "$sequence $Body`n"
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ($true) {
        $line = Get-AckLine
        if ($line -match "^$sequence ok ") { return $line }
        if ($line -match "^$sequence failed ") { throw "Command $sequence failed: $line" }
        $changed = $line -ne $before -or ((Test-Path -LiteralPath $info.ack) -and (Get-Item -LiteralPath $info.ack).LastWriteTimeUtc -ne $stamp)
        if ($changed -and $line -match '^\d+ (rejected|stale) ') { throw "Command '$sequence $Body' was not accepted: $line" }
        if (-not (Test-GameRunning)) { throw "The game exited before acknowledging command $sequence; see $($info.log)" }
        if ([DateTime]::UtcNow -ge $deadline) {
            throw "No acknowledgement for command $sequence within $TimeoutSeconds s. The game only polls between guest dispatches; do not send another command blindly."
        }
        Start-Sleep -Milliseconds 50
    }
}

if ($Action -eq 'status') {
    $progress = Get-Progress
    Write-Output ("running={0} pid={1} deterministic={2}" -f (Test-GameRunning), $info.pid, $info.deterministic)
    if ($progress) { Write-Output ("frame={0} samples={1} guest_s={2:F1} last_frame_wall_ms={3:F1}" -f $progress.frame, $progress.samples, ($progress.guest_ms / 1000.0), $progress.wall_ms) }
    $ack = Get-AckLine
    if ($ack) { Write-Output "last_ack: $ack" }
    $result = Get-ChiliRunResult $info.log
    foreach ($line in @($result.dialogs | Select-Object -Last 3)) { Write-Output $line }
    if ($result.ended) { Write-Output $result.ended }
    if ($result.failure) { Write-Output $result.failure }
    exit 0
}

$mask = Get-ButtonMask $Buttons
$sticks = "$X $Y"
if ($RX -ne 128 -or $RY -ne 128) { $sticks = "$X $Y $RX $RY" }
$body = switch ($Action) {
    'tap' { "tap $mask $Samples $sticks" }
    'press' { "press $mask $DurationMs $sticks" }
    'hold' { "hold $mask $sticks" }
    'release' { 'release' }
    'analog' {
        if ($PSBoundParameters.ContainsKey('Samples')) { "tap $mask $Samples $sticks" }
        elseif ($PSBoundParameters.ContainsKey('DurationMs')) { "press $mask $DurationMs $sticks" }
        else { "hold $mask $sticks" }
    }
    'capture' {
        if ($Label -notmatch '^[A-Za-z0-9_-]{1,48}$') { throw 'Capture label: 1-48 characters of A-Z a-z 0-9 _ -' }
        "capture $Label"
    }
    'quit' { 'quit' }
}
$start = Get-Progress
$ack = Send-Command $body
Write-Output $ack
if ($Action -eq 'capture') { Write-Output (Join-Path $Session ($ack -split ' ')[3]) }
if ($Action -eq 'quit') {
    $process = Get-Process -Id $info.pid -ErrorAction SilentlyContinue
    if ($process) { $process.WaitForExit(60000) | Out-Null }
    $result = Get-ChiliRunResult $info.log
    if ($result.frame_timer) { Write-Output $result.frame_timer }
    if ($result.ended) { Write-Output $result.ended }
    exit 0
}
if ($ThenWaitMs -gt 0) { Start-Sleep -Milliseconds $ThenWaitMs }
if ($ThenWaitFrames -gt 0) {
    # The CSV is flushed about once a second, so this is accurate to roughly a second of frames.
    $from = 0
    if ($start) { $from = $start.frame }
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ($true) {
        $now = Get-Progress
        if ($now -and $now.frame -ge $from + $ThenWaitFrames) { break }
        if (-not (Test-GameRunning)) { throw 'The game exited while waiting for frames' }
        if ([DateTime]::UtcNow -ge $deadline) { throw "Fewer than $ThenWaitFrames frames passed within $TimeoutSeconds s" }
        Start-Sleep -Milliseconds 200
    }
}
if ($ThenCapture) {
    if ($ThenCapture -notmatch '^[A-Za-z0-9_-]{1,48}$') { throw 'Capture label: 1-48 characters of A-Z a-z 0-9 _ -' }
    $ack = Send-Command "capture $ThenCapture"
    Write-Output $ack
    Write-Output (Join-Path $Session ($ack -split ' ')[3])
}
exit 0
