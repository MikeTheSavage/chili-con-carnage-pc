<#
.SYNOPSIS
    Checks that the launcher and the game agree about ChiliNative.ini: every settings file the launcher can
    write is read by the game without a complaint.

.DESCRIPTION
    1. The game writes its own default ChiliNative.ini into a work folder (--write-default-ini), and four
       made-up mod folders are put beside it (two texture packs with one tiny picture each, a mod with one
       value, an empty folder). No game data is used for them.
    2. The launcher (launcher\bin\ChiliConCarnageLauncher.exe --every-value) puts every row of every page to
       each of its values through the row's own control, off screen, saves as PLAY saves, and keeps the file
       it wrote each time: 001.ini, 002.ini, ... with list.tsv saying what each one is.
    3. Each file must hold the live line its row stands for (read back here, independently of the launcher).
    4. The game is started on each file for one frame, unattended (deterministic mode, its window behind the
       others, no sound, a copy of the save folder). Its settings readers print what they do not like to
       stderr as "[input] ...", "[mods] ...", "[render] ..." or "[audio] ...": none of those lines may
       appear ("unknown ...", "... must be ..." are the usual ones), the game must say it read that file and
       must end with exit code 0. Where the game's log says the value it uses (rendering resolution,
       anti-aliasing, anisotropy, frame rate, camera, control style), that is compared too.
    5. With -PlayerExe (a packaged ChiliConCarnage.exe in a folder of its own, never the player's), the files
       of the Disc row are also given to a player's start for a few seconds: the game must find the disc
       through the [Disc] Path the launcher wrote.

    Nothing is written outside -Work. The run takes five to ten minutes.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File check_launcher_settings.ps1 -Work C:\Temp\chili\agree `
        -Disc "D:\dumps\Chili Con Carnage (USA).iso"
#>
[CmdletBinding()]
param(
    # An empty or new folder for everything this check writes.
    [Parameter(Mandatory = $true)][string]$Work,
    # The game program (default: the rel build of this checkout).
    [string]$Exe = '',
    # The launcher (default: launcher\bin of this checkout; launcher\build.ps1 makes it).
    [string]$Launcher = '',
    # Discs for the Disc row: an .iso file, an unpacked disc folder, or both.
    [string[]]$Disc = @(),
    # A packaged game program for the player's start of step 5 (in a scratch copy of a package).
    [string]$PlayerExe = '',
    # How many games run at the same time.
    [ValidateRange(1, 8)][int]$Parallel = 3
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'run_common.ps1')
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if (-not $Exe) { $Exe = Join-Path $repo 'out\chili-rel\bin\chili_recomp.exe' }
if (-not $Launcher) { $Launcher = Join-Path $script:ChiliProfile 'launcher\bin\ChiliConCarnageLauncher.exe' }
$Exe = Resolve-ChiliFullPath $Exe
$Launcher = Resolve-ChiliFullPath $Launcher
$Work = Resolve-ChiliFullPath $Work
foreach ($needed in $Exe, $Launcher) { if (-not (Test-Path -LiteralPath $needed -PathType Leaf)) { throw "Not found: $needed" } }
if ((Test-Path -LiteralPath $Work) -and @(Get-ChildItem -LiteralPath $Work -Force).Count -gt 0) { throw "The work folder is not empty, choose a new one: $Work" }

# Never beside a game that someone is playing.
$playing = @(Get-Process | Where-Object { $_.ProcessName -like 'chili_recomp*' -or $_.ProcessName -eq 'ChiliConCarnage' })
if ($playing.Count) { throw "A game is running (pid $($playing[0].Id)). Close it first." }

$base = Join-Path $Work 'base'
$files = Join-Path $Work 'files'
$runs = Join-Path $Work 'runs'
New-Item -ItemType Directory -Force -Path $base, $files, $runs | Out-Null

# 1. The game's own settings file, and made-up mods.
$ini = Join-Path $files 'ChiliNative.ini'
$written = Start-Process -FilePath $Exe -ArgumentList @('--write-default-ini', "`"$ini`"") -WorkingDirectory (Split-Path -Parent $Exe) -PassThru -Wait -NoNewWindow `
    -RedirectStandardOutput (Join-Path $Work 'write_ini.out.txt') -RedirectStandardError (Join-Path $Work 'write_ini.err.txt')
if ($written.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $ini)) { throw "The game did not write its default settings (exit code $($written.ExitCode))." }
Add-Type -AssemblyName System.Drawing
function New-TinyPicture([string]$Path, [Drawing.Color]$Color) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
    $bitmap = New-Object Drawing.Bitmap 4, 4
    try {
        for ($x = 0; $x -lt 4; $x++) { for ($y = 0; $y -lt 4; $y++) { $bitmap.SetPixel($x, $y, $Color) } }
        $bitmap.Save($Path, [Drawing.Imaging.ImageFormat]::Png)
    } finally { $bitmap.Dispose() }
}
New-TinyPicture (Join-Path $files 'mods\alpha_pack\textures\0000000000000001.png') ([Drawing.Color]::FromArgb(255, 200, 60, 40))
New-TinyPicture (Join-Path $files 'mods\beta_pack\textures\0000000000000002.png') ([Drawing.Color]::FromArgb(255, 40, 120, 200))
New-Item -ItemType Directory -Force -Path (Join-Path $files 'mods\camera_mod'), (Join-Path $files 'mods\empty_mod') | Out-Null
[IO.File]::WriteAllText((Join-Path $files 'mods\camera_mod\properties.ini'), "; A made-up mod for the check: one value of the block every mission loads.`r`n[ComboSystem]`r`nCombo_Ctrl.ComboTimeLimit = 20`r`n", [Text.Encoding]::ASCII)
[IO.File]::WriteAllText((Join-Path $base 'launcher.ini'), "[Launcher]`r`nGame=$Exe`r`n", [Text.Encoding]::ASCII)

# 2. The launcher writes one settings file per row and value.
$arguments = @('--base', "`"$base`"", '--every-value', "`"$files`"")
foreach ($path in $Disc) { $arguments += @('--disc', "`"$(Resolve-ChiliFullPath $path)`"") }
$made = Start-Process -FilePath $Launcher -ArgumentList $arguments -PassThru -Wait -NoNewWindow `
    -RedirectStandardOutput (Join-Path $Work 'launcher.out.txt') -RedirectStandardError (Join-Path $Work 'launcher.err.txt')
Get-Content -LiteralPath (Join-Path $Work 'launcher.out.txt') | ForEach-Object { "[launcher] $_" }
Get-Content -LiteralPath (Join-Path $Work 'launcher.err.txt') -ErrorAction SilentlyContinue | ForEach-Object { "[launcher] $_" }
$faults = @(Get-Content -LiteralPath (Join-Path $files 'faults.txt') -ErrorAction SilentlyContinue | Where-Object { $_ })
$faults | ForEach-Object { "[launcher] FAULT $_" }
if ($made.ExitCode -ne 0) { throw "The launcher's run failed (exit code $($made.ExitCode))." }
$cases = @(Import-Csv -LiteralPath (Join-Path $files 'list.tsv') -Delimiter "`t" -Header 'file', 'page', 'row', 'value', 'how', 'holds', 'lines' | Select-Object -Skip 1)
$skipped = @(Get-Content -LiteralPath (Join-Path $files 'skipped.txt') -ErrorAction SilentlyContinue | Where-Object { $_ })
"[check] $($cases.Count) settings files; $($skipped.Count) values the launcher does not offer here (skipped.txt)"

# 3. The live value of a key in a settings file, as an ini reader sees it: the last live line of the section.
function Get-IniValue([string]$Path, [string]$Section, [string]$Key) {
    $in = ''; $found = $null
    foreach ($raw in [IO.File]::ReadAllLines($Path)) {
        $line = $raw.Trim()
        if ($line.Length -eq 0 -or $line[0] -eq ';' -or $line[0] -eq '#') { continue }
        if ($line[0] -eq '[') { $in = $line.Trim('[', ']').Trim(); continue }
        if ($in -ne $Section) { continue }
        $at = $line.IndexOf('=')
        if ($at -lt 1) { continue }
        if ($line.Substring(0, $at).Trim() -ne $Key) { continue }
        $found = $line.Substring($at + 1)
        # A comment after the value is not the value (a path may hold these characters, so not for [Disc]).
        if ($Section -ne 'Disc') { $cut = $found.IndexOfAny([char[]]';#'); if ($cut -ge 0) { $found = $found.Substring(0, $cut) } }
        $found = $found.Trim()
    }
    return $found
}

# 4. The game on each file, one frame, $Parallel at a time.
$ms0 = @()
for ($slot = 0; $slot -lt $Parallel; $slot++) {
    $copy = Join-Path $Work "ms0_$slot"
    New-ChiliMs0Copy $script:ChiliMs0 $copy
    $ms0 += $copy
}
$results = New-Object Collections.ArrayList
$watch = [Diagnostics.Stopwatch]::StartNew()
for ($from = 0; $from -lt $cases.Count; $from += $Parallel) {
    $batch = @()
    for ($slot = 0; $slot -lt $Parallel -and $from + $slot -lt $cases.Count; $slot++) {
        $case = $cases[$from + $slot]
        $name = [IO.Path]::GetFileNameWithoutExtension($case.file)
        $out = Join-Path $runs "$name.out.log"; $err = Join-Path $runs "$name.err.log"
        $variables = @{
            CHILI_DETERMINISTIC = 1; CHILI_STOP_AT_FRAME = 1; PSPRECOMP_PROBE_SECONDS = 90
            CHILI_MS0_DIR = $ms0[$slot]; CHILI_CONFIG = (Join-Path $files $case.file)
        }
        $process = Start-ChiliGame $Exe $variables $out $err
        $batch += [pscustomobject]@{ case = $case; process = $process; out = $out; err = $err }
    }
    foreach ($entry in $batch) {
        $code = 'killed'
        if ($entry.process.WaitForExit(150000)) { $code = $entry.process.ExitCode } else { try { $entry.process.Kill() } catch { } }
        $case = $entry.case
        $file = Join-Path $files $case.file
        $log = @(Get-Content -LiteralPath $entry.out -ErrorAction SilentlyContinue)
        $said = @(Get-Content -LiteralPath $entry.err -ErrorAction SilentlyContinue | Where-Object { $_ -and $_ -notmatch 'swscaler' })
        $problems = @()
        # 3. the line the row stands for
        if ($case.holds) {
            $key, $value = $case.holds -split '=', 2
            $section, $name = $key -split '/', 2
            $live = Get-IniValue $file $section $name
            if ($null -eq $live -or ($live -replace '\s', '') -ne ($value -replace '\s', '')) { $problems += "the file holds [$section] $name=$live, the launcher shows $value" }
        }
        # 4. the game
        if ($code -ne 0) { $problems += "exit code $code" }
        if (-not ($log | Where-Object { $_ -eq "[input] bindings: $file" })) { $problems += 'the game did not say it read this file' }
        $complaints = @($said | Where-Object { $_ -match '^\[(input|mods|render|audio)\] ' -or $_ -match 'unknown|must be' })
        foreach ($line in $complaints) { $problems += "the game says: $line" }
        $other = @($said | Where-Object { $complaints -notcontains $_ })
        # What the game's log says about the value it uses.
        $compared = 0
        if ($case.holds) {
            $key, $value = $case.holds -split '=', 2
            $render = [string]($log | Where-Object { $_ -like '[[]render[]] Direct3D 12 GE backend active*' } | Select-Object -First 1)
            $expect = $null; $have = $null
            switch ($key) {
                'Rendering/InternalScale' { if ($value -match '^\d+$' -and $render) { $expect = "scale=$value"; $have = [regex]::Match($render, 'scale=\d+').Value } }
                'Rendering/Anisotropy' { if ($render) { $expect = "anisotropy=$value"; $have = [regex]::Match($render, 'anisotropy=\d+').Value } }
                'Rendering/AntiAliasing' {
                    if ($render) {
                        $expect = $(if ($value -match '^msaa(\d)$') { "msaa=$($Matches[1])" } elseif ($value -eq 'fxaa') { 'msaa=1 fxaa' } else { 'msaa=1' })
                        $have = [regex]::Match($render, 'msaa=\d+( fxaa)?').Value
                    }
                }
                'Game/FrameLock' { $expect = $(if ($value -eq '2') { '30' } else { '60' }); $have = [regex]::Match([string]($log | Where-Object { $_ -like '[[]game[]] frame rate:*' } | Select-Object -First 1), 'frame rate: (\d+)').Groups[1].Value }
                'Game/Camera' { $expect = $value; $have = $(if ($log | Where-Object { $_ -like '[[]game[]] camera: tight*' }) { 'tight' } else { 'original' }) }
                'Input/Scheme' { $expect = $value; $have = [regex]::Match([string]($log | Where-Object { $_ -like '[[]input[]] scheme *' } | Select-Object -First 1), 'scheme (\w+)').Groups[1].Value }
                'Input/Layout' { $expect = $value; $have = [regex]::Match([string]($log | Where-Object { $_ -like '[[]input[]] scheme *' } | Select-Object -First 1), 'control style (\w+)').Groups[1].Value }
            }
            if ($null -ne $expect) {
                $compared = 1
                if ($expect -ne $have) { $problems += "the game's log says '$have' where the launcher wrote $($case.holds) ('$expect' expected)" }
            }
        }
        [void]$results.Add([pscustomobject]@{
                file = $case.file; page = $case.page; row = $case.row; value = $case.value; exit = $code; compared = $compared
                problems = ($problems -join ' ; '); other_stderr = ($other -join ' ; ')
            })
        if ($problems.Count) { "[check] FAIL $($case.file)  $($case.page) / $($case.row) / $($case.value): $($problems -join ' ; ')" }
    }
}
# Textures the game wrote for the "Dump textures while playing" files are the game's pictures: not kept.
Remove-Item -LiteralPath (Join-Path $files 'texture_dump') -Recurse -Force -ErrorAction SilentlyContinue
$results | Export-Csv -LiteralPath (Join-Path $Work 'result.csv') -NoTypeInformation -Encoding UTF8
$failed = @($results | Where-Object { $_.problems })
$noise = @($results | Where-Object { $_.other_stderr } | ForEach-Object { $_.other_stderr } | Sort-Object -Unique)
"[check] the game read $($results.Count) launcher-written files in $([int]$watch.Elapsed.TotalSeconds) s: $($results.Count - $failed.Count) without a complaint, $($failed.Count) with one"
"[check] values the game's log repeats, compared with what the launcher wrote: $(@($results | Where-Object { $_.compared }).Count)"
if ($noise.Count) { '[check] other lines on stderr (no settings complaint):'; $noise | Select-Object -First 10 | ForEach-Object { "[check]   $_" } }

# 5. The Disc row's files in a player's start.
$status = 0
if ($PlayerExe) {
    $PlayerExe = Resolve-ChiliFullPath $PlayerExe
    $discCases = @($cases | Where-Object { $_.row -eq 'Disc' })
    if (-not $discCases.Count) { '[check] no file of the Disc row to start (no -Disc given, or none was taken)' }
    foreach ($case in $discCases) {
        $name = [IO.Path]::GetFileNameWithoutExtension($case.file)
        $out = Join-Path $runs "$name.player.out.log"; $err = Join-Path $runs "$name.player.err.log"
        $variables = @{ CHILI_UNATTENDED = 1; CHILI_NULL_AUDIO = 1; PSPRECOMP_PROBE_SECONDS = 4; CHILI_CONFIG = (Join-Path $files $case.file); CHILI_MS0_DIR = $ms0[0] }
        Repair-ChiliPathVariable
        $names = @($script:ChiliVariables) + @('CHILI_UNATTENDED', 'CHILI_DISC_ANSWER') + @($variables.Keys) | Select-Object -Unique
        $saved = @{}
        foreach ($variable in $names) {
            $saved[$variable] = [Environment]::GetEnvironmentVariable($variable, 'Process')
            $value = $null
            if ($variables.ContainsKey($variable)) { $value = "$($variables[$variable])" }
            [Environment]::SetEnvironmentVariable($variable, $value, 'Process')
        }
        try {
            $process = Start-Process -FilePath $PlayerExe -WorkingDirectory (Split-Path -Parent $PlayerExe) -PassThru -NoNewWindow -RedirectStandardOutput $out -RedirectStandardError $err
            $null = $process.Handle
            $code = 'killed'
            if ($process.WaitForExit(90000)) { $code = $process.ExitCode } else { try { $process.Kill() } catch { } }
        } finally { foreach ($variable in $names) { [Environment]::SetEnvironmentVariable($variable, $saved[$variable], 'Process') } }
        $disc = [string](Get-Content -LiteralPath $out | Where-Object { $_ -like '[[]disc[]] *' } | Select-Object -First 1)
        $good = $code -eq 0 -and $disc -match 'named by \[Disc\] Path'
        "[check] player's start on $($case.file) ($($case.value)): exit code $code; $disc"
        if (-not $good) { $status = 1; "[check] FAIL the game did not take the disc from the [Disc] Path the launcher wrote" }
    }
}
if ($failed.Count -or $faults.Count) { $status = 1 }
"[check] list: $(Join-Path $files 'list.tsv'); results: $(Join-Path $Work 'result.csv'); logs: $runs"
exit $status
