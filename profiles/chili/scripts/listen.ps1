<#
.SYNOPSIS
Plays a recorded run in real time with sound, for listening. Nobody has to hold the controls.

.DESCRIPTION
The game runs in an ordinary window in front (it is meant to be watched and heard, so it takes the
keyboard focus like any program you start) from a copy of the save (the real save is never touched),
answers its own dialogs and takes its input from a replay file:
  mission   boot, menus, then 67 seconds of Story 1-1 (replays\mission1_baseline.replay)
  menu      boot, then the main menu with its music (replays\menu.replay)
  movies    the two intro movies, nothing skipped (replays\idle.replay)
  speech    boot, menus, then 22 seconds of Story 1-1 with voice lines (replays\ram_taunt.replay): the enemies
            shout from the first seconds of the mission, the player (Ram) taunts after some of his kills.
            The voice lines that were played are listed at the end, by name. No two real-time runs go the
            same way, so Ram's taunt (one kill in ten) is not in every one. -SoundOnly computes the scene on
            the game clock instead, where it always goes the same way, and then plays the sound it produced:
            an enemy's taunt after 0.7 s, a dying enemy at 2.4 s, Ram's taunt at 4.7 s. No picture.
The run's log, its sound statistics and a capture of what the audio device was given (audio.wav) end up in
the output folder, which is printed at the end.

.EXAMPLE
.\listen.ps1 mission
.EXAMPLE
.\listen.ps1 speech
.EXAMPLE
.\listen.ps1 speech -SoundOnly
.EXAMPLE
.\listen.ps1 menu -Seconds 40 -FrameLock 1 -BufferMs 100
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)][ValidateSet('mission', 'menu', 'movies', 'speech')][string]$What = 'mission',
    [ValidateRange(10, 600)][int]$Seconds = 0,
    # 1 runs the game at 60 frames per second, 2 at 30. 0 leaves the game's own setting.
    [ValidateSet(0, 1, 2)][int]$FrameLock = 0,
    # Milliseconds of sound kept ahead of the device (0 = the default, 70).
    [ValidateRange(0, 500)][int]$BufferMs = 0,
    [string]$Exe = '',
    [string]$OutDir = '',
    # Keep the window behind the others and never activated, as other scripted runs do.
    [switch]$Background,
    # With speech: the scene computed on the game clock (always the same), then its sound played. No picture.
    [switch]$SoundOnly
)
$ErrorActionPreference = 'Stop'
$chili = Split-Path -Parent $PSScriptRoot
$repo = (Resolve-Path (Join-Path $chili '..\..')).Path
if (-not $Exe) { $Exe = Join-Path $repo 'out\chili-rel\bin\chili_recomp.exe' }
if (-not $OutDir) { $OutDir = Join-Path ([IO.Path]::GetTempPath()) ("chili_listen_{0}_{1}" -f $What, (Get-Date -Format 'yyyyMMdd_HHmmss')) }
$replay = @{ mission = 'mission1_baseline.replay'; menu = 'menu.replay'; movies = 'idle.replay'; speech = 'ram_taunt.replay' }[$What]
if ($What -eq 'mission' -and $FrameLock -eq 1) { $replay = 'mission1_baseline_fl1.replay' }
if ($Seconds -eq 0) { $Seconds = @{ mission = 100; menu = 60; movies = 45; speech = 40 }[$What] }
# The game data lives in one tree only; a second worktree borrows it.
$game = Join-Path $chili 'game'
if (-not (Test-Path -LiteralPath (Join-Path $game 'disc0'))) {
    $game = Join-Path (Split-Path -Parent $repo) 'PSPRecomp\profiles\chili\game'
    if (-not (Test-Path -LiteralPath (Join-Path $game 'disc0'))) { throw "Game data not found beside this script or in $game" }
}
# The game thread at the priority a player's run has: this run is for hearing what a player hears.
$environment = @{ CHILI_AUTO_DIALOGS = '1'; CHILI_AUDIO_WAV = (Join-Path $OutDir 'audio.wav'); CHILI_GAME_PRIORITY = 'high' }
if ($BufferMs -gt 0) { $environment.CHILI_AUDIO_BUFFER_MS = $BufferMs }
if ($FrameLock -gt 0) {
    # The scripted runs' settings with the frame rate added.
    $ini = Join-Path ([IO.Path]::GetTempPath()) "chili_listen_framelock$FrameLock.ini"
    $text = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'unattended.ini')) -replace '(?m)^FrameLock\s*=.*\r?\n', ''
    $text = $text -replace '(?m)^\[Game\]\s*$', "[Game]`r`nFrameLock=$FrameLock"
    [IO.File]::WriteAllText($ini, $text, [Text.Encoding]::ASCII)
    $environment.CHILI_CONFIG = $ini
}
$env:CHILI_ALLOW_CONCURRENT = '1'
$log = Join-Path $OutDir 'run.log'
# Every one-channel stream is a voice line. The sound trace has its key; the archive gives the name.
function Write-VoiceLines([double]$From) {
    if (-not (Test-Path -LiteralPath $log)) { return }
    $python = Get-Command python -ErrorAction SilentlyContinue
    $named = @()
    if ($python) { $named = @(& $python.Source (Join-Path $chili 'tools\sound_assets.py') trace (Join-Path $game 'disc0\PSP_GAME\USRDIR\assets00.naz') $log 2>$null) }
    if ($named.Count -eq 0) { $named = @(Select-String -LiteralPath $log -Pattern '^\[sound\] stream \d+ start' | ForEach-Object { $_.Line }) }
    $lines = @($named | Where-Object { $_ -match ' start .* out=([\d.]+) .* channels=1 .* samples=(\d+) .* key=(\w+)\s*(\S*)' -and [double]$Matches[1] -ge $From } | ForEach-Object {
        $name = if ($Matches[4]) { [IO.Path]::GetFileNameWithoutExtension($Matches[4]) } else { "key $($Matches[3])" }
        '  {0,6:N1} s  {1,4:N1} s long  {2}{3}' -f ([double]$Matches[1] - $From), ([double]$Matches[2] / 44100), $name, $(if ($name -match '_ram_') { '   <- the player' } else { '' })
    })
    Write-Output "voice lines (seconds of sound since the start): $($lines.Count)"
    $lines | ForEach-Object { Write-Output $_ }
}
if ($SoundOnly) {
    if ($What -ne 'speech') { throw '-SoundOnly goes with speech' }
    # The game opens no device in a deterministic run; CHILI_AUDIO_WAV gets its blocks back to back.
    $capture = Join-Path $OutDir 'audio.wav'
    & (Join-Path $PSScriptRoot 'run_replay.ps1') -Exe $Exe -Replay (Join-Path $chili "replays\$replay") -OutDir $OutDir -Seconds 300 -Frames 900 `
        -HashEvery 0 -Log 'sound' -Ms0Source (Join-Path $game 'ms0') -Disc (Join-Path $game 'disc0') -Environment @{ CHILI_AUDIO_WAV = $capture; CHILI_GE_NULL_DRAW = '1' }
    if (-not (Test-Path -LiteralPath $capture)) { throw "The run left no capture: $capture" }
    # The mission starts 7.9 s into the capture; what is before is the menus.
    $from = 7.9
    Write-VoiceLines $from
    $bytes = [IO.File]::ReadAllBytes($capture)
    $skip = 44 + [int]($from * 44100) * 4
    if ($bytes.Length -le $skip) { throw 'The capture is shorter than the menus' }
    $scene = New-Object byte[] (44 + $bytes.Length - $skip)
    [Array]::Copy($bytes, 0, $scene, 0, 44)
    [Array]::Copy($bytes, $skip, $scene, 44, $bytes.Length - $skip)
    [BitConverter]::GetBytes([uint32]($scene.Length - 8)).CopyTo($scene, 4)
    [BitConverter]::GetBytes([uint32]($scene.Length - 44)).CopyTo($scene, 40)
    $sceneFile = Join-Path $OutDir 'scene.wav'
    [IO.File]::WriteAllBytes($sceneFile, $scene)
    Write-Output ("playing {0} ({1:N1} s)" -f $sceneFile, (($scene.Length - 44) / 176400))
    $player = New-Object System.Media.SoundPlayer $sceneFile
    $player.PlaySync()
    Write-Output "output folder: $OutDir"
    return
}
& (Join-Path $PSScriptRoot 'run_replay.ps1') -Exe $Exe -Replay (Join-Path $chili "replays\$replay") -OutDir $OutDir -Seconds $Seconds `
    -HashEvery 0 -RealTime -Watch:(-not $Background) -Backend d3d12 -Log 'audio,sound' -Ms0Source (Join-Path $game 'ms0') -Disc (Join-Path $game 'disc0') -Environment $environment
if (Test-Path -LiteralPath $log) { Select-String -LiteralPath $log -Pattern '^\[audio\] (output:|total|device took)' | ForEach-Object { $_.Line } }
if ($What -eq 'speech') { Write-VoiceLines 0 }
Write-Output "output folder: $OutDir"
