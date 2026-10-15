# Makes the installer: Setup's program (bin\Setup.stub.exe, from build.ps1) with the payload after it.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File installer\pack.ps1 -Package <staging folder> `
#       [-HdLoading <mods\hd_loading folder>] [-SetupFiles <folder of ChiliSoundExport.exe and its DLLs>] `
#       [-Version 0.9.0] [-Publisher <name>] [-Out <file>]
#
# -Package      the folder scripts\package.ps1 made (the programs, DLLs, documents, licences, tools and the
#               default ChiliNative.ini and launcher.ini). Refused when it holds anything that must not be in a
#               release: saves, art, textures, logs, another pack, a file of the game, a live [Disc] Path, a
#               [GameProbe] section. WHAT_CHANGED.txt and shortcuts are left out.
# -HdLoading    the hd_loading pack (README.md and textures\<key>.png): an option of Setup, on by default.
# -SetupFiles   the music tool and its DLLs (profiles\chili\tools: ChiliSoundExport.exe, avcodec-61.dll,
#               avutil-59.dll, swresample-5.dll). Never installed: Setup runs it on the player's disc to play
#               the game's own title music while it installs. Without it Setup is silent.
# -Out          default: ..\release\dist\ChiliConCarnagePC-Setup-<version>.exe beside the repository.
param(
    [Parameter(Mandatory = $true)][string]$Package,
    [string]$HdLoading = '',
    [string]$SetupFiles = '',
    [string]$Version = '',
    [string]$Publisher = 'Chili Con Carnage PC project',
    [string]$MusicArgs = '"{disc}" "{wav}"',
    [string]$Stub = '',
    [string]$Out = ''
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Stub) { $Stub = Join-Path $here 'bin\Setup.stub.exe' }
if (-not (Test-Path -LiteralPath $Stub)) { throw "No ${Stub}: run installer\build.ps1 first." }
if (-not $Version) { $Version = (Get-Item -LiteralPath $Stub).VersionInfo.ProductVersion }
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "-Version must be like 0.9.0, not $Version" }
if (-not $Out) { $Out = Join-Path (Split-Path -Parent (Split-Path -Parent $here)) "dist\ChiliConCarnagePC-Setup-$Version.exe" }
$Out = [IO.Path]::GetFullPath($Out)

$arguments = @('--pack', '--package', "`"$([IO.Path]::GetFullPath($Package))`"", '--version', $Version, '--publisher', "`"$Publisher`"", '--out', "`"$Out`"")
if ($HdLoading) { $arguments += @('--hd-loading', "`"$([IO.Path]::GetFullPath($HdLoading))`"") }
if ($SetupFiles) { $arguments += @('--setup-files', "`"$([IO.Path]::GetFullPath($SetupFiles))`"", '--music-args', "`"$($MusicArgs.Replace('"', '\"'))`"") }
$report = Join-Path ([IO.Path]::GetTempPath()) ("chili_pack_" + [guid]::NewGuid().ToString('N') + '.txt')
try {
    $p = Start-Process -FilePath $Stub -ArgumentList $arguments -NoNewWindow -Wait -PassThru -RedirectStandardOutput $report -RedirectStandardError "$report.err"
    Get-Content -LiteralPath $report
    if (Test-Path -LiteralPath "$report.err") { Get-Content -LiteralPath "$report.err" }
} finally { Remove-Item -LiteralPath $report, "$report.err" -ErrorAction SilentlyContinue }
if ($p.ExitCode -ne 0) { throw "The installer was not made (exit code $($p.ExitCode))." }
$size = (Get-Item -LiteralPath $Out).Length
Write-Host ('{0} ({1:N1} MB)' -f $Out, ($size / 1MB))
if ($size -gt 250MB) { Write-Host 'Setup is over 250 MB: consider leaving -HdLoading out and offering the pack as a separate archive.' }
