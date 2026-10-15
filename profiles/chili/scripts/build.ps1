<#
.SYNOPSIS
    Configure and build one named Chili configuration into its own directory.

.DESCRIPTION
    rel  out/chili-rel  Release, generated units /O2 /Ob2, both AOT production defines ON.
    od   out/chili-od   Release host, generated units /Od /Ob0, both AOT production defines OFF.

    The executable is always <build dir>/bin/chili_recomp.exe, with bin/chili_recomp.map beside it.
    Builds are incremental: after a host edit only the touched host sources recompile and the
    executable relinks. The generated corpus (target chili_generated) rebuilds only when a
    generated source, a framework header under include/psprecomp, or a generated-unit flag changes.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Config rel
.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Config od -Target chili_generated -Jobs 6 -KeepGoing
.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Config rel -HotUnits "0001;0242" -Jobs 5
#>
[CmdletBinding()]
param(
    [ValidateSet('rel', 'od')][string]$Config = 'rel',
    # chili_recomp (the game), chili_ge_replay (offline GE list replay tool), chili_generated (corpus only),
    # psprecomp_core, chili_media_backend.
    [string[]]$Target = @('chili_recomp', 'chili_ge_replay'),
    [ValidateRange(1, 64)][int]$Jobs = 6,
    # Keep compiling after a failure so one broken source does not hide the others.
    [switch]$KeepGoing,
    [switch]$Reconfigure,
    [switch]$ConfigureOnly,
    # Sets CHILI_HOT_UNITS (units that also get /d2OptimizeHugeFunctions) and keeps it until changed.
    # "none" clears the list. Only the units whose flags change recompile.
    [string]$HotUnits,
    # A hot unit still compiling after this many minutes is stopped, dropped from CHILI_HOT_UNITS
    # and rebuilt with the ordinary flags. 0 disables the limit.
    [double]$HotTimeLimitMin = 20,
    # auto keeps an existing build directory's generator and otherwise uses Ninja.
    [ValidateSet('auto', 'ninja', 'vs')][string]$Generator = 'auto',
    # The build is stopped when the build drive has less free space than this.
    [double]$MinFreeGB = 4
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$settings = @{
    rel = @{ Dir = 'out\chili-rel'; Level = '2'; Production = 'ON' }
    od  = @{ Dir = 'out\chili-od';  Level = '0'; Production = 'OFF' }
}[$Config]
$buildDir = Join-Path $repo $settings.Dir
$targets = @($Target | ForEach-Object { $_ -split '[,; ]+' } | Where-Object { $_ })

# A tool runner can hand down both "Path" and "PATH". Windows PowerShell's Start-Process and
# MSBuild both fail on that pair, so collapse them into one variable before starting anything.
function Repair-PathVariable {
    $all = [Environment]::GetEnvironmentVariables('Process')
    $names = @($all.Keys | Where-Object { $_ -ieq 'Path' })
    if ($names.Count -le 1) { return }
    $seen = @{}
    $merged = New-Object System.Collections.Generic.List[string]
    foreach ($name in $names) {
        foreach ($entry in ([string]$all[$name] -split ';')) {
            if ($entry -and -not $seen.ContainsKey($entry.ToLowerInvariant())) {
                $seen[$entry.ToLowerInvariant()] = $true
                $merged.Add($entry)
            }
        }
    }
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
    [Environment]::SetEnvironmentVariable('Path', ($merged -join ';'), 'Process')
}

function Find-VisualStudio {
    $known = 'C:\Program Files\Microsoft Visual Studio\18\Community'
    if (Test-Path (Join-Path $known 'VC\Auxiliary\Build\vcvars64.bat')) { return $known }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $found = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($found) { return @($found)[0] }
    }
    throw 'Visual Studio with the x64 C++ tools was not found'
}

function Import-VcVars([string]$vsRoot) {
    if ($env:VSCMD_ARG_TGT_ARCH -eq 'x64' -and (Get-Command cl.exe -ErrorAction SilentlyContinue)) { return }
    $bat = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
    $lines = & cmd.exe /d /s /c "`"$bat`" >nul 2>&1 && set"
    if ($LASTEXITCODE -ne 0) { throw "vcvars64.bat failed with exit code $LASTEXITCODE" }
    foreach ($line in $lines) {
        $split = $line.IndexOf('=')
        if ($split -gt 0) {
            [Environment]::SetEnvironmentVariable($line.Substring(0, $split), $line.Substring($split + 1), 'Process')
        }
    }
    Repair-PathVariable
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw 'cl.exe is not on PATH after vcvars64.bat' }
}

function Join-Arguments([string[]]$items) {
    ($items | ForEach-Object { if ($_ -match '[\s()&^;]' -or $_ -eq '') { '"' + $_ + '"' } else { $_ } }) -join ' '
}

# Last "[n/total]" step in a log the child still has open for writing.
function Get-LastProgress([string]$log) {
    try {
        $stream = New-Object System.IO.FileStream($log, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        try {
            $null = $stream.Seek(-[Math]::Min($stream.Length, 16384), [IO.SeekOrigin]::End)
            $tail = (New-Object System.IO.StreamReader($stream)).ReadToEnd()
        } finally { $stream.Dispose() }
        $found = [regex]::Matches($tail, '(?m)^\[\d+/\d+\]')
        if ($found.Count -gt 0) { return $found[$found.Count - 1].Value }
    } catch { }
    return ''
}

# Runs one child with its output in $log. While it runs: records compiler memory, stops the
# build when the drive runs low, and stops hot-unit compiles that exceed the time limit.
function Invoke-Logged([string]$exe, [string[]]$arguments, [string]$log) {
    $errLog = "$log.stderr"
    $process = Start-Process -FilePath $exe -ArgumentList (Join-Arguments $arguments) -NoNewWindow -PassThru `
        -RedirectStandardOutput $log -RedirectStandardError $errLog
    $null = $process.Handle
    $drive = New-Object System.IO.DriveInfo ([System.IO.Path]::GetPathRoot($buildDir))
    if ($drive.AvailableFreeSpace -lt $stats.MinFreeBytes) { $stats.MinFreeBytes = $drive.AvailableFreeSpace }
    $lastReport = [Diagnostics.Stopwatch]::StartNew()
    $stopped = $false
    while (-not $process.WaitForExit(3000)) {
        $free = $drive.AvailableFreeSpace
        if ($free -lt $stats.MinFreeBytes) { $stats.MinFreeBytes = $free }
        $sum = [uint64]0
        try {
            $all = @(Get-CimInstance Win32_Process -Property ProcessId, ParentProcessId, Name, WorkingSetSize, CommandLine, CreationDate -ErrorAction Stop)
            # A parent id alone is not proof of descent: Windows reuses process ids, and a
            # reused id once made this monitor stop another build's compilers. A real child
            # is never older than its parent.
            $mine = @{}
            $root = $all | Where-Object { $_.ProcessId -eq [uint32]$process.Id } | Select-Object -First 1
            if ($root) { $mine[[uint32]$process.Id] = $root.CreationDate }
            do {
                $grew = $false
                foreach ($item in $all) {
                    if (-not $mine.ContainsKey($item.ProcessId) -and $mine.ContainsKey($item.ParentProcessId) -and
                        $item.CreationDate -ge $mine[$item.ParentProcessId]) {
                        $mine[$item.ProcessId] = $item.CreationDate; $grew = $true
                    }
                }
            } while ($grew)
            $tools = @($all | Where-Object { $mine.ContainsKey($_.ProcessId) -and ($_.Name -eq 'cl.exe' -or $_.Name -eq 'link.exe') })
            foreach ($tool in $tools) {
                $sum += $tool.WorkingSetSize
                if ($tool.WorkingSetSize -gt $stats.PeakSingleBytes) { $stats.PeakSingleBytes = $tool.WorkingSetSize }
                if ($HotTimeLimitMin -gt 0 -and $tool.CommandLine -match 'd2OptimizeHugeFunctions' -and
                    $tool.CommandLine -match 'generated_unit_(\d+)\.cpp' -and
                    ((Get-Date) - $tool.CreationDate).TotalMinutes -gt $HotTimeLimitMin) {
                    $unit = $Matches[1]
                    Stop-Process -Id $tool.ProcessId -Force -ErrorAction SilentlyContinue
                    $stats.HotTimedOut[$unit] = $true
                    Write-Host "[build] unit $unit passed the $HotTimeLimitMin minute hot-unit limit at $([int]($tool.WorkingSetSize / 1MB)) MB; stopped"
                }
            }
            if ($sum -gt $stats.PeakSumBytes) { $stats.PeakSumBytes = $sum }
        } catch { }
        if ($free -lt $MinFreeGB * 1GB) {
            Write-Host ("[build] free space on {0} is {1:N1} GB, below {2} GB: stopping the build" -f $drive.Name, ($free / 1GB), $MinFreeGB)
            & taskkill.exe /PID $process.Id /T /F | Out-Null
            $stopped = $true
            break
        }
        if ($lastReport.Elapsed.TotalSeconds -ge 60) {
            $lastReport.Restart()
            Write-Host ("[build] {0} compilers {1:N2} GB, free {2:N1} GB" -f (Get-LastProgress $log), ($sum / 1GB), ($free / 1GB))
        }
    }
    $process.WaitForExit()
    if (Test-Path $errLog) {
        if ((Get-Item $errLog).Length -gt 0) { Get-Content -LiteralPath $errLog | Add-Content -LiteralPath $log }
        Remove-Item -LiteralPath $errLog
    }
    if ($stopped) { return 99 }
    return $process.ExitCode
}

function Get-CacheValue([string]$name) {
    if (-not (Test-Path $cache)) { return $null }
    $match = Select-String -LiteralPath $cache -Pattern "^${name}:[A-Z]+=(.*)$" | Select-Object -First 1
    if ($match) { return $match.Matches[0].Groups[1].Value }
    return $null
}

# $hot is $null to leave CHILI_HOT_UNITS as cached, otherwise the new list.
function Invoke-Configure($hot) {
    $configure = @('-S', $repo, '-B', $buildDir)
    if ($useNinja) {
        $configure += @('-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$ninja", '-DCMAKE_BUILD_TYPE=Release',
            '-DCMAKE_CXX_COMPILER=cl', '-DPSPRECOMP_MSVC_MP_JOBS=1')
    } else {
        # One MSBuild node, so /MP<Jobs> inside the project being built is the only parallelism.
        $configure += @('-G', 'Visual Studio 18 2026', '-A', 'x64', "-DPSPRECOMP_MSVC_MP_JOBS=$Jobs")
    }
    $configure += @(
        '-DPSPRECOMP_PROFILE=chili',
        "-DPSPRECOMP_GENERATED_OPT_LEVEL=$($settings.Level)",
        "-DPSPRECOMP_AOT_PRODUCTION_FASTPATHS=$($settings.Production)",
        "-DPSPRECOMP_AOT_ASSUME_NO_WRITE_WATCH=$($settings.Production)",
        '-DPSPRECOMP_LTO=OFF', '-DPSPRECOMP_NATIVE_AVX2=OFF',
        '-DCHILI_PARALLEL_SHADE_OBSERVATION=OFF',
        '-DPSPRECOMP_BUILD_TESTS=OFF', '-DPSPRECOMP_BUILD_PROFILE_TESTS=OFF')
    if ($null -ne $hot) { $configure += "-DCHILI_HOT_UNITS=$hot" }
    $script:logIndex++
    $log = Join-Path $logDir "configure_${stamp}_$($script:logIndex).log"
    Write-Host "[build] configuring $Config in $buildDir (log: $log)"
    $code = Invoke-Logged $cmake $configure $log
    if ($code -ne 0) {
        Get-Content -LiteralPath $log -Tail 40 | Write-Host
        Write-Host "[build] configure failed with exit code $code"
        exit $code
    }
}

function Invoke-Build {
    $build = @('--build', $buildDir, '--target') + $targets
    if ($useNinja) {
        $build += @('--parallel', "$Jobs")
        if ($KeepGoing) { $build += @('--', '-k', '0') }
    } else {
        $build += @('--config', 'Release', '--', '/m:1', '/nologo', '/v:minimal')
    }
    $script:logIndex++
    $log = Join-Path $logDir "build_${stamp}_$($script:logIndex).log"
    $script:buildLogs += $log
    Write-Host "[build] building $($targets -join ', ') in $buildDir with $Jobs jobs (log: $log)"
    return (Invoke-Logged $cmake $build $log)
}

Repair-PathVariable
$vsRoot = Find-VisualStudio
Import-VcVars $vsRoot
$cmakeRoot = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake'
$cmake = Join-Path $cmakeRoot 'CMake\bin\cmake.exe'
$ninja = Join-Path $cmakeRoot 'Ninja\ninja.exe'
if (-not (Test-Path $cmake)) { $cmake = (Get-Command cmake.exe).Source }

$cache = Join-Path $buildDir 'CMakeCache.txt'
$cachedGenerator = Get-CacheValue 'CMAKE_GENERATOR'
$useNinja = switch ($Generator) {
    'ninja' { $true }
    'vs'    { $false }
    default { if ($cachedGenerator) { $cachedGenerator -eq 'Ninja' } else { Test-Path $ninja } }
}
if ($cachedGenerator -and (($cachedGenerator -eq 'Ninja') -ne $useNinja)) {
    throw "$buildDir was configured with '$cachedGenerator'. Delete it to switch generators."
}

$logDir = Join-Path $buildDir 'logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$logIndex = 0
$buildLogs = @()
$stats = @{ PeakSumBytes = [uint64]0; PeakSingleBytes = [uint64]0; MinFreeBytes = [long]::MaxValue; HotTimedOut = @{} }

$hotRequested = $null
if ($PSBoundParameters.ContainsKey('HotUnits')) {
    $hotRequested = if ($HotUnits -eq 'none') { '' } else { ($HotUnits -split '[,; ]+' | Where-Object { $_ }) -join ';' }
}
if ($Reconfigure -or $ConfigureOnly -or $null -ne $hotRequested -or -not (Test-Path $cache)) { Invoke-Configure $hotRequested }
if ($ConfigureOnly) { Write-Host '[build] configured'; exit 0 }

$watch = [Diagnostics.Stopwatch]::StartNew()
$code = Invoke-Build
$dropped = @($stats.HotTimedOut.Keys | Sort-Object)
if ($code -ne 99 -and $dropped.Count -gt 0) {
    $kept = @((([string](Get-CacheValue 'CHILI_HOT_UNITS')) -split ';') | Where-Object {
        $_ -match '(\d+)' -and -not $stats.HotTimedOut.ContainsKey($Matches[1].PadLeft(4, '0')) })
    Write-Host "[build] rebuilding without hot flags for: $($dropped -join ', ')"
    Invoke-Configure ($kept -join ';')
    $code = Invoke-Build
}
$watch.Stop()

$suppressed = @($buildLogs | ForEach-Object { Select-String -LiteralPath $_ -Pattern 'generated_unit_(\d+)\.cpp.*warning C4883' } |
    ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object -Unique)
$lastLog = $buildLogs[-1]
$failed = @(Select-String -LiteralPath $lastLog -Pattern '^FAILED: (?:\[[^\]]*\] )?(.*)$' |
    ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() } | Sort-Object -Unique)
$errors = @(Select-String -LiteralPath $lastLog -Pattern '(?:error|fatal error) (?:C|LNK)\d+' | ForEach-Object { $_.Line.Trim() })
$objects = @(Get-ChildItem -LiteralPath $buildDir -Recurse -File -Filter '*.obj' -ErrorAction SilentlyContinue)
$generatedObjects = @($objects | Where-Object { $_.Name -like 'generated_*' })
$objectBytes = ($objects | Measure-Object Length -Sum).Sum
$exe = Join-Path $buildDir 'bin\chili_recomp.exe'

$summary = @(
    "[build] config=$Config targets=$($targets -join ',') jobs=$Jobs exit=$code wall=$([int]$watch.Elapsed.TotalSeconds)s",
    ("[build] peak compiler memory: {0:N2} GB summed, {1:N2} GB largest process; lowest free disk {2:N1} GB" -f `
        ($stats.PeakSumBytes / 1GB), ($stats.PeakSingleBytes / 1GB), ($stats.MinFreeBytes / 1GB)),
    "[build] hot units (CHILI_HOT_UNITS): $(Get-CacheValue 'CHILI_HOT_UNITS'); dropped at the time limit: $($dropped -join ' ')",
    "[build] generated units compiled in this run with C4883 (optimization suppressed): $($suppressed.Count)",
    ("[build] objects: {0} files, {1:N0} MB ({2} generated)" -f $objects.Count, ($objectBytes / 1MB), $generatedObjects.Count),
    "[build] failed steps: $($failed.Count)")
$summary += $failed | ForEach-Object { "[build]   FAILED $_" }
$summary += $errors | Select-Object -First 40 | ForEach-Object { "[build]   $_" }
if (Test-Path $exe) {
    $item = Get-Item $exe
    $summary += ("[build] {0} {1:N0} MB, written {2:yyyy-MM-dd HH:mm:ss}" -f $exe, ($item.Length / 1MB), $item.LastWriteTime)
} else {
    $summary += "[build] $exe does not exist"
}
$summary | ForEach-Object { Write-Host $_ }
$summary | Add-Content -LiteralPath $lastLog
exit $code
