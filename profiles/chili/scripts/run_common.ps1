# Shared helpers for run_replay.ps1, run_live.ps1 and live_send.ps1.
# Works in Windows PowerShell 5.1 and PowerShell 7. Dot-source it.

$script:ChiliProfile = Split-Path -Parent $PSScriptRoot
$script:ChiliDisc = Join-Path $script:ChiliProfile 'game\disc0'
$script:ChiliBoot = Join-Path $script:ChiliDisc 'PSP_GAME\SYSDIR\BOOT.BIN'
$script:ChiliMs0 = Join-Path $script:ChiliProfile 'game\ms0'

# Every variable the test mode reads. A run sets the ones it wants and clears
# the rest, so nothing leaks in from the calling shell.
$script:ChiliVariables = @(
    'CHILI_DETERMINISTIC', 'CHILI_INPUT_RECORD', 'CHILI_INPUT_REPLAY', 'CHILI_INPUT_TRACE', 'CHILI_FRAME_LOG',
    'CHILI_STATE_HASH_EVERY', 'CHILI_STATE_HASH_REGIONS', 'CHILI_STATE_HASH_EXCLUDE', 'CHILI_STOP_AT_FRAME',
    'CHILI_FRAME_PNG_EVERY', 'CHILI_FRAME_PNG_DIR', 'CHILI_MS0_DIR', 'CHILI_NULL_AUDIO',
    'CHILI_AUTO_DIALOGS', 'CHILI_AUTO_SAVE_SLOT', 'CHILI_AUTO_MESSAGE', 'CHILI_AUTO_DIALOG_DELAY_MS',
    'CHILI_LOG_FLUSH', 'CHILI_UNPACED', 'CHILI_WATCH', 'CHILI_CONFIG', 'CHILI_GE_CAPTURE_DIR', 'CHILI_GE_CAPTURE_LISTS', 'CHILI_GE_CAPTURE_MAX',
    'CHILI_GE_CAPTURE_AFTER', 'CHILI_GE_TRACE', 'CHILI_GE_TRACE_LISTS', 'CHILI_GE_LIST_INDEX', 'PSPRECOMP_PROBE_SECONDS', 'PSPRECOMP_LIVE_CONTROL', 'PSPRECOMP_LIVE_FRAME_PREFIX',
    'PSPRECOMP_LIVE_FRAME_FORMAT', 'PSPRECOMP_FRAME_DUMP', 'PSPRECOMP_LOG', 'PSPRECOMP_CONTROLLER_REPLAY',
    'CHILI_MODS_DIR', 'CHILI_VIRTUAL_NAZ',
    'CHILI_RENDER_BACKEND', 'CHILI_RENDER_SCALE', 'CHILI_RENDER_MSAA', 'CHILI_RENDER_STRICT',
    'CHILI_RENDER_AA', 'CHILI_RENDER_ANISOTROPY', 'CHILI_RENDER_FILTER', 'CHILI_RENDER_SSAO', 'CHILI_RENDER_GLOW', 'CHILI_RENDER_EARLY_PRESENT',
    'CHILI_TEXTURE_DUMP', 'PSPRECOMP_GPU_TIMING_DIAG',
    'CHILI_GE_NULL_DRAW', 'CHILI_TEST_HOOK', 'CHILI_SAMPLER', 'CHILI_SAMPLER_HZ', 'CHILI_SAMPLER_FRAMES',
    'CHILI_AUDIO_WAV', 'CHILI_AUDIO_TRACE', 'CHILI_AUDIO_BUFFER_MS', 'CHILI_AUDIO_BAND_MS', 'CHILI_AUDIO_CATCH_UP',
    'CHILI_GAME_PRIORITY',
    'CHILI_SETTINGS_AT', 'CHILI_PAGE', 'CHILI_PAGE_AT', 'CHILI_PAGE_SHOTS',
    'CHILI_PROMPTS', 'CHILI_PROMPTS_AT', 'CHILI_PROMPT_DEVICE_AT', 'CHILI_PROMPT_KEY_ALL', 'CHILI_PROMPT_PAINTER',
    'CHILI_FAKE_XINPUT', 'CHILI_HID_DUMP', 'CHILI_RUMBLE_TEST', 'CHILI_PAGE_ENTRY',
    'CHILI_OVERLAY_TEST', 'CHILI_OVERLAY_SHOTS', 'CHILI_OVERLAY_FULLSCREEN', 'CHILI_OVERLAY_SIZE')

function Resolve-ChiliFullPath([string]$Path) {
    return [IO.Path]::GetFullPath($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path))
}

# Only one game instance may run at a time. Waits up to $WaitSeconds for another
# one to finish, then fails; it never stops a process it did not start.
# CHILI_ALLOW_CONCURRENT=1 skips the check for functional runs whose timings do
# not matter (each run still needs its own output folder and ms0 copy).
function Assert-ChiliIdle([int]$WaitSeconds = 0) {
    if ($env:CHILI_ALLOW_CONCURRENT -eq '1') { return }
    $deadline = [DateTime]::UtcNow.AddSeconds($WaitSeconds)
    while ($true) {
        # chili_recomp is the build's name of the program, ChiliConCarnage the packaged one's (scripts\package.ps1).
        $running = @(Get-Process | Where-Object { $_.ProcessName -like 'chili_recomp*' -or $_.ProcessName -eq 'ChiliConCarnage' })
        if ($running.Count -eq 0) { return }
        if ([DateTime]::UtcNow -ge $deadline) {
            $list = ($running | ForEach-Object { "$($_.ProcessName) pid $($_.Id)" }) -join ', '
            throw "Another game instance is running ($list). Wait for it, or pass -WaitForOtherSeconds."
        }
        Start-Sleep -Seconds 2
    }
}

# Copies a memory stick folder for one run and gives every entry the same fixed
# timestamps: the save list returns file times to the guest, and a fresh copy
# would otherwise differ from run to run.
function New-ChiliMs0Copy([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source -PathType Container)) { throw "Memory stick source not found: $Source" }
    if (Test-Path -LiteralPath $Destination) { throw "Memory stick copy already exists: $Destination" }
    Copy-Item -LiteralPath $Source -Destination $Destination -Recurse
    $stamp = [DateTime]::new(2026, 1, 1, 0, 0, 0, [DateTimeKind]::Utc)
    $items = @(Get-ChildItem -LiteralPath $Destination -Recurse -Force)
    foreach ($item in ($items | Where-Object { -not $_.PSIsContainer })) {
        if ($item.IsReadOnly) { $item.IsReadOnly = $false }
        $item.CreationTimeUtc = $stamp; $item.LastWriteTimeUtc = $stamp; $item.LastAccessTimeUtc = $stamp
    }
    $folders = @($items | Where-Object { $_.PSIsContainer } | Sort-Object { $_.FullName.Length } -Descending)
    $folders += Get-Item -LiteralPath $Destination
    foreach ($item in $folders) {
        $item.CreationTimeUtc = $stamp; $item.LastWriteTimeUtc = $stamp; $item.LastAccessTimeUtc = $stamp
    }
}

# Some tool runners hand down both "Path" and "PATH"; Windows PowerShell's
# Start-Process refuses to start anything with that pair. Merge them.
function Repair-ChiliPathVariable {
    $all = [Environment]::GetEnvironmentVariables('Process')
    $names = @($all.Keys | Where-Object { $_ -ieq 'Path' })
    if ($names.Count -le 1) { return }
    $entries = @()
    foreach ($name in $names) { $entries += @([string]$all[$name] -split ';' | Where-Object { $_ }) }
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
    [Environment]::SetEnvironmentVariable('Path', (($entries | Select-Object -Unique) -join ';'), 'Process')
}

# Starts the game with exactly the given test variables (all others cleared),
# stdout and stderr redirected to files. Returns the process; the caller's
# environment is restored before returning. $GameRoot is a disc folder to run
# instead of game\disc0 (a copy with an edited configpsp.txt, for example).
function Start-ChiliGame([string]$Exe, [hashtable]$Variables, [string]$StdOut, [string]$StdErr, [string]$Ms0 = '', [string]$DispatchLimit = '', [string]$GameRoot = '') {
    if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) { throw "Executable not found: $Exe" }
    $root = $script:ChiliDisc; $boot = $script:ChiliBoot
    if ($GameRoot) { $root = $GameRoot; $boot = Join-Path $GameRoot 'PSP_GAME\SYSDIR\BOOT.BIN' }
    if (-not (Test-Path -LiteralPath $boot -PathType Leaf)) { throw "BOOT.BIN not found: $boot" }
    Repair-ChiliPathVariable
    # A scripted run does not read the player's ChiliNative.ini or the mods folder beside the exe:
    # without a CHILI_CONFIG of its own it gets scripts\unattended.ini (scheme psp, nothing else set).
    if (-not $Variables.ContainsKey('CHILI_CONFIG') -or "$($Variables['CHILI_CONFIG'])" -eq '') {
        $Variables = $Variables.Clone()
        $Variables['CHILI_CONFIG'] = Join-Path $script:ChiliProfile 'scripts\unattended.ini'
    }
    $names = @($script:ChiliVariables) + @($Variables.Keys) | Select-Object -Unique
    $saved = @{}
    foreach ($name in $names) {
        $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        $value = $null
        if ($Variables.ContainsKey($name) -and $null -ne $Variables[$name] -and "$($Variables[$name])" -ne '') { $value = "$($Variables[$name])" }
        [Environment]::SetEnvironmentVariable($name, $value, 'Process')
    }
    try {
        $arguments = @("`"$boot`"", "`"$root`"")
        if ($DispatchLimit) { $arguments += $DispatchLimit }
        if ($Ms0) { $arguments += @('--ms0', "`"$Ms0`"") }
        $process = Start-Process -FilePath $Exe -ArgumentList $arguments -WorkingDirectory (Split-Path -Parent $Exe) `
            -RedirectStandardOutput $StdOut -RedirectStandardError $StdErr -NoNewWindow -PassThru
        $null = $process.Handle   # keeps ExitCode readable after the process ends
        return $process
    } finally {
        foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
    }
}

# Reads a file another process may be writing or replacing.
function Read-ChiliSharedText([string]$Path) {
    for ($attempt = 0; $attempt -lt 20; $attempt++) {
        try {
            $stream = [IO.FileStream]::new($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
                ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
            try {
                $reader = [IO.StreamReader]::new($stream)
                return $reader.ReadToEnd()
            } finally { $stream.Dispose() }
        } catch [IO.FileNotFoundException] { return $null
        } catch [IO.DirectoryNotFoundException] { return $null
        } catch { Start-Sleep -Milliseconds 10 }
    }
    return $null
}

# Replaces a small text file in one step so a reader never sees half of it.
function Publish-ChiliText([string]$Path, [string]$Text) {
    $stage = "$Path.$([guid]::NewGuid().ToString('N')).tmp"
    [IO.File]::WriteAllText($stage, $Text, [Text.Encoding]::ASCII)
    for ($attempt = 0; $attempt -lt 50; $attempt++) {
        try {
            if ([IO.File]::Exists($Path)) { [IO.File]::Replace($stage, $Path, [NullString]::Value) }
            else { [IO.File]::Move($stage, $Path) }
            return
        } catch {
            if ($attempt -eq 49) { throw }
            Start-Sleep -Milliseconds 10
        }
    }
}

# Nearest-rank percentile of an already sorted array.
function Get-ChiliRank([double[]]$Sorted, [double]$Fraction) {
    if ($Sorted.Count -eq 0) { return 0.0 }
    $index = [Math]::Ceiling($Fraction * $Sorted.Count) - 1
    if ($index -lt 0) { $index = 0 }
    if ($index -ge $Sorted.Count) { $index = $Sorted.Count - 1 }
    return $Sorted[[int]$index]
}

# Pulls the measured results out of a finished (or running) log.
function Get-ChiliRunResult([string]$LogPath, [string]$CsvPath = '', [int]$FromFrame = 0, [int]$ToFrame = 0) {
    $text = Read-ChiliSharedText $LogPath
    if ($null -eq $text) { $text = '' }
    $lines = $text -split "`r?`n"
    $result = [ordered]@{
        hashes      = @($lines | Where-Object { $_ -like '[[]state-hash[]] *' })
        frame_timer = [string]($lines | Where-Object { $_ -like '[[]frame-timer[]] *' } | Select-Object -Last 1)
        ended       = [string]($lines | Where-Object { $_ -like 'Execution ended at *' } | Select-Object -Last 1)
        failure     = [string]($lines | Where-Object { $_ -like 'Chili bootstrap failure*' } | Select-Object -Last 1)
        dialogs     = @($lines | Where-Object { $_ -like '[[]dialog[]] *' })
        window      = $null
    }
    if ($CsvPath -and (Test-Path -LiteralPath $CsvPath) -and ($FromFrame -gt 0 -or $ToFrame -gt 0)) {
        $rows = @(Import-Csv -LiteralPath $CsvPath | Where-Object {
                [int64]$_.frame -ge $FromFrame -and ($ToFrame -le 0 -or [int64]$_.frame -le $ToFrame) })
        if ($rows.Count -gt 0) {
            $wall = [double[]]@($rows | ForEach-Object { [double]$_.wall_ms } | Sort-Object)
            $guest = ($rows | ForEach-Object { [double]$_.guest_ms } | Measure-Object -Sum).Sum
            $mean = ($wall | Measure-Object -Average).Average
            $result.window = ('frames {0}-{1} count={2} wall_ms_mean={3:F3} wall_ms_p50={4:F3} wall_ms_p90={5:F3} wall_ms_p99={6:F3} wall_ms_max={7:F3} guest_fps={8:F3}' -f `
                    $rows[0].frame, $rows[-1].frame, $rows.Count, $mean, (Get-ChiliRank $wall 0.5), (Get-ChiliRank $wall 0.9),
                (Get-ChiliRank $wall 0.99), $wall[-1], $(if ($guest -gt 0) { $rows.Count * 1000.0 / $guest } else { 0 }))
        }
    }
    return [pscustomobject]$result
}
