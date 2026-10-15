<#
.SYNOPSIS
The short measuring run: boot, load profile DATA00, start Story 1-1 and run the
first ten seconds of its in-engine intro from a built-in input replay, in
deterministic mode.

.DESCRIPTION
Prints the frame-time summary for the whole run, the frame-time percentiles of
the mission-renderer window (game frames 5780-6076) and the state hash at every
100th frame. With -Repeat 2 it also checks that both runs produce the same hashes.

The window is the mission's in-engine intro, 36 frames of it under the pause
menu. The intro lasts about 500 frames, so this replay ends before the player
gets control and its mission input does nothing. For gameplay (67 seconds of
it, with the intros skipped) use run_replay.ps1 with
replays\mission1_baseline.replay; see replays\README.md.

The replay was recorded with run_live.ps1 at the game's default 30 fps
(frame_lock 2). It needs a memory stick with an existing profile in slot DATA00
(game\ms0 has one; each run uses a copy). Input is keyed on the controller
sample index, and the game samples once per frame, so a build that changes the
frame rate or the guest clock needs a new recording.

What the input does, by controller sample:
  1806 Start (intro; the profile menu follows)  1903 Cross: Load profile (the scripted dialog picks DATA00)
  1992 Cross: autosave notice  2064, 2069 Cross: title and main menu  2123 Cross: Continue
  then the intro movie plays out
  5832 Start (pause)  5868 Cross (resume)  5907-5931 stick forward  5951 Square
  6029-6058 stick forward

.EXAMPLE
.\run_baseline.ps1 -Exe <chili_recomp.exe> -OutDir C:\Temp\chili\baseline -Repeat 2
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [ValidateRange(1, 10)][int]$Repeat = 1,
    [ValidateRange(1, 7200)][int]$Seconds = 600,
    [int]$PngEvery = 0,
    [string]$Log = '',
    [hashtable]$Environment = @{},
    [int]$WaitForOtherSeconds = 0
)
$ErrorActionPreference = 'Stop'
$replayText = @'
# chili input v1: sample buttons x y
# Boot to Story 1-1 gameplay, recorded at frame_lock 2.
1806 0x8 128 128
1809 0x0 128 128
1903 0x4000 128 128
1906 0x0 128 128
1992 0x4000 128 128
1995 0x0 128 128
2064 0x4000 128 128
2067 0x0 128 128
2069 0x4000 128 128
2072 0x0 128 128
2123 0x4000 128 128
2126 0x0 128 128
5832 0x8 128 128
5835 0x0 128 128
5868 0x4000 128 128
5871 0x0 128 128
5907 0x0 128 0
5932 0x0 128 128
5950 0x0 128 0
5951 0x8000 128 128
5955 0x0 128 128
6029 0x0 128 0
6059 0x0 128 128
end 6064
'@
$OutDir = [IO.Path]::GetFullPath($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutDir))
if ((Test-Path -LiteralPath $OutDir) -and @(Get-ChildItem -LiteralPath $OutDir -Force).Count -gt 0) {
    throw "Output folder is not empty, choose a new one: $OutDir"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$replay = Join-Path $OutDir 'boot_to_mission1.rec'
[IO.File]::WriteAllText($replay, ($replayText -replace "`r`n", "`n") + "`n", [Text.Encoding]::ASCII)
& (Join-Path $PSScriptRoot 'run_replay.ps1') -Exe $Exe -Replay $replay -OutDir (Join-Path $OutDir 'replay') `
    -Frames 6076 -Seconds $Seconds -HashEvery 100 -PngEvery $PngEvery -Repeat $Repeat -Log $Log `
    -Environment $Environment -WaitForOtherSeconds $WaitForOtherSeconds -StatsFromFrame 5780 -StatsToFrame 6076
exit $LASTEXITCODE
