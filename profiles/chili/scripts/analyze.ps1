<#
.SYNOPSIS
    Prepares a checkout from your own copy of the game: extracts the disc and generates the AOT corpus.

.DESCRIPTION
    1. Extracts the .iso into profiles\chili\game\disc0 with tools\extract_iso.py (about 800 MB; git-ignored).
    2. Checks PSP_GAME\SYSDIR\BOOT.BIN: it must be the game's executable of the USA release (ULUS-10216),
       8,616,992 bytes with the FNV-1a 64 hash 0xA18A6D5249286C4A, the same check the game makes when it starts.
    3. Builds the generator and writes the corpus into profiles\chili\analysis\generated (git-ignored): regen.ps1.

    -Report also writes profiles\chili\analysis\boot_report.json with psp_analyze (not needed for the build).
    Nothing this script writes may be committed or published: it is all made from the game.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File profiles\chili\scripts\analyze.ps1 -Iso "D:\Games\Chili Con Carnage (USA).iso"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Iso,
    [switch]$Report,
    [ValidateRange(1, 64)][int]$Jobs = 4
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..\..\..").Path
if (-not (Test-Path -LiteralPath $Iso -PathType Leaf)) { throw "The .iso was not found: $Iso" }
Push-Location $repo
try {
    & py -3 profiles/chili/tools/extract_iso.py $Iso profiles/chili/game/disc0
    if ($LASTEXITCODE) { throw 'ISO extraction failed' }

    $boot = Join-Path $repo 'profiles\chili\game\disc0\PSP_GAME\SYSDIR\BOOT.BIN'
    if (-not (Test-Path -LiteralPath $boot)) { throw "This .iso has no PSP_GAME\SYSDIR\BOOT.BIN: it is not a PSP game disc." }
    & py -3 profiles/chili/tools/check_boot.py $boot
    if ($LASTEXITCODE) { throw 'This disc is not Chili Con Carnage (USA, ULUS-10216); the corpus is made only from that executable.' }

    if ($Report) {
        $analyze = @('out\framework\psp_analyze.exe', 'out\framework\Release\psp_analyze.exe') | Where-Object { Test-Path $_ } | Select-Object -First 1
        if (-not $analyze) { throw 'psp_analyze.exe was not found under out\framework: build the target psp_analyze first.' }
        & $analyze $boot profiles/chili/analysis/boot_report.json
        if ($LASTEXITCODE) { throw 'Executable analysis failed' }
    }
    # The corpus and its generator options: see regen.ps1.
    & "$PSScriptRoot\regen.ps1" -Jobs $Jobs
} finally { Pop-Location }
