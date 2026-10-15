<#
.SYNOPSIS
    Regenerate the AOT corpus (profiles/chili/analysis/generated) from BOOT.BIN.

.DESCRIPTION
    Builds the generator (psp_recomp, in out/framework) and runs it with the options the tracked
    corpus is made with: one C++ function per guest function (--outline=all), cut at 0x1800 bytes
    of guest code, so that MSVC optimizes every function at the stock /O2 (no C4883).

    A unit whose text does not change is not rewritten, so the next build.ps1 recompiles only the
    units that changed. A change to a lowering changes every unit: that is a full corpus build
    (about an hour at 4 jobs).

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File regen.ps1
.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File regen.ps1 -Output C:\temp\gen -Options '--outline=all','--outline-max=0x1800','--without=call_overrides'
#>
[CmdletBinding()]
param(
    # Where the units are written. Default: the tracked corpus.
    [string]$Output,
    # Generator options after the unit span. --without=<name,...> has to come last.
    [string[]]$Options = @('--outline=all', '--outline-max=0x1800'),
    [string]$BootBin,
    [ValidateRange(1, 64)][int]$Jobs = 4
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if (-not $Output) { $Output = Join-Path $repo 'profiles\chili\analysis\generated' }
if (-not $BootBin) { $BootBin = Join-Path $repo 'profiles\chili\game\disc0\PSP_GAME\SYSDIR\BOOT.BIN' }
if (-not (Test-Path -LiteralPath $BootBin)) { throw "BOOT.BIN not found: $BootBin" }

$vsRoot = 'C:\Program Files\Microsoft Visual Studio\18\Community'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path (Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat')) -and (Test-Path $vswhere)) {
    $vsRoot = @(& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)[0]
}
$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw 'Visual Studio with the x64 C++ tools was not found' }
foreach ($line in (& cmd.exe /d /s /c "`"$vcvars`" >nul 2>&1 && set")) {
    $split = $line.IndexOf('=')
    if ($split -gt 0 -and $line.Substring(0, $split) -ine 'Path') {
        [Environment]::SetEnvironmentVariable($line.Substring(0, $split), $line.Substring($split + 1), 'Process')
    } elseif ($split -gt 0) {
        $env:Path = $line.Substring($split + 1)
    }
}
$cmakeRoot = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake'
$cmake = Join-Path $cmakeRoot 'CMake\bin\cmake.exe'
$ninja = Join-Path $cmakeRoot 'Ninja\ninja.exe'

$framework = Join-Path $repo 'out\framework'
if (-not (Test-Path (Join-Path $framework 'CMakeCache.txt'))) {
    & $cmake -S $repo -B $framework -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl
    if ($LASTEXITCODE) { throw 'Configuring out\framework failed' }
}
& $cmake --build $framework --target psp_recomp --parallel $Jobs
if ($LASTEXITCODE) { throw 'Building psp_recomp failed' }
$generator = @((Join-Path $framework 'psp_recomp.exe'), (Join-Path $framework 'Release\psp_recomp.exe')) |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $generator) { throw "psp_recomp.exe not found under $framework" }

$watch = [Diagnostics.Stopwatch]::StartNew()
Write-Host "[regen] $generator $BootBin --auto $Output 0x08804000 0x4000 $($Options -join ' ')"
# The generator reports its passes on stderr; that is not a failure.
$ErrorActionPreference = 'Continue'
& $generator $BootBin --auto $Output 0x08804000 0x4000 @Options 2>&1 | ForEach-Object { "$_" } |
    Where-Object { $_ -notmatch '^\[codegen\] unit \d+ ' } | Write-Host
$code = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($code) { throw "AOT generation failed with exit code $code" }
Write-Host ("[regen] done in {0:N0} s" -f $watch.Elapsed.TotalSeconds)
