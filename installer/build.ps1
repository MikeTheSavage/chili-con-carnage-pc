# Builds Setup's program without a payload (bin\Setup.stub.exe) with the C# compiler that is already on this
# machine, as the launcher is built (profiles\chili\launcher\build.ps1): .NET Framework 4.8, WPF, nothing
# downloaded. pack.ps1 then puts the payload after it.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File installer\build.ps1 [-Out <folder>] [-Test] [-Iso <your .iso>]
#   ... -Version 0.9.0      the version Setup reports (default: host\version.hpp's, else 0.9.0)
#   ... -Icon <file.ico>    the icon (default: profiles\chili\resources\app_icon.ico, else drawn by tools\MakeIcon.cs)
#   ... -LauncherSrc <dir>  the launcher's sources Setup shares (default: profiles\chili\launcher\src)
#   ... -Test               also runs Setup.stub.exe --test (with -Iso: the player's disc is checked too, read only)
param(
    [string]$Out = "",
    [switch]$Test,
    [string]$Iso = "",
    [string]$Version = "",
    [string]$Icon = "",
    [string]$LauncherSrc = ""
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent $here
if (-not $Out) { $Out = Join-Path $here 'bin' }
New-Item -ItemType Directory -Force $Out | Out-Null
$obj = Join-Path $here 'obj'
New-Item -ItemType Directory -Force $obj | Out-Null

# The C# compiler: Visual Studio's Roslyn, else the .NET SDK's.
$csc = $null
$cscArgs = @()
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path $vswhere) {
    $vs = & $vswhere -latest -prerelease -products * -property installationPath
    if ($vs) {
        $candidate = Join-Path $vs 'MSBuild\Current\Bin\Roslyn\csc.exe'
        if (Test-Path $candidate) { $csc = $candidate }
    }
}
if (-not $csc) {
    $sdk = Get-ChildItem (Join-Path $env:ProgramFiles 'dotnet\sdk') -Directory -ErrorAction SilentlyContinue |
        Sort-Object { [version]($_.Name -replace '-.*', '') } -Descending |
        Where-Object { Test-Path (Join-Path $_.FullName 'Roslyn\bincore\csc.dll') } | Select-Object -First 1
    if ($sdk) { $csc = 'dotnet'; $cscArgs = @('exec', (Join-Path $sdk.FullName 'Roslyn\bincore\csc.dll')) }
}
if (-not $csc) { throw 'No C# compiler found: install Visual Studio (any edition with MSBuild) or the .NET SDK.' }

$ref = Join-Path ${env:ProgramFiles(x86)} 'Reference Assemblies\Microsoft\Framework\.NETFramework\v4.8'
$wpf = $ref
if (-not (Test-Path (Join-Path $ref 'PresentationCore.dll'))) {
    $ref = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319'
    $wpf = Join-Path $ref 'WPF'
    if (-not (Test-Path (Join-Path $wpf 'PresentationCore.dll'))) { throw '.NET Framework 4.8 was not found.' }
}
$references = @(
    (Join-Path $ref 'mscorlib.dll'), (Join-Path $ref 'System.dll'), (Join-Path $ref 'System.Core.dll'), (Join-Path $ref 'System.Xaml.dll'),
    (Join-Path $wpf 'WindowsBase.dll'), (Join-Path $wpf 'PresentationCore.dll'), (Join-Path $wpf 'PresentationFramework.dll')
) | ForEach-Object { "/r:$_" }
$common = @('/nologo', '/noconfig', '/nostdlib+', '/codepage:65001', '/optimize+', '/debug-', '/deterministic+', '/langversion:7.3', '/warn:4', '/nowarn:1591')

# The launcher's files Setup compiles in unchanged (Shims.cs stands in for the rest of the launcher).
if (-not $LauncherSrc) { $LauncherSrc = Join-Path $repo 'profiles\chili\launcher\src' }
$shared = @('Theme.cs', 'Pads.cs', 'Native.cs') | ForEach-Object { Join-Path $LauncherSrc $_ }
foreach ($file in $shared) { if (-not (Test-Path -LiteralPath $file)) { throw "The launcher's $file is missing (-LauncherSrc names the launcher's src folder)." } }

# The version: the game's (host\version.hpp), unless given.
if (-not $Version) {
    $hpp = Join-Path $repo 'profiles\chili\host\version.hpp'
    if (Test-Path -LiteralPath $hpp) {
        $text = [IO.File]::ReadAllText($hpp)
        if ($text -match '#define\s+CHILI_VERSION_TEXT\s+"(\d+\.\d+\.\d+)"') { $Version = $Matches[1] }
    }
    if (-not $Version) { $Version = '0.9.0' }
}
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "-Version must be like 0.9.0, not $Version" }
$versionFile = Join-Path $obj 'Version.cs'
[IO.File]::WriteAllText($versionFile, "[assembly: System.Reflection.AssemblyVersion(`"$Version.0`")]`r`n[assembly: System.Reflection.AssemblyFileVersion(`"$Version.0`")]`r`n[assembly: System.Reflection.AssemblyInformationalVersion(`"$Version`")]`r`n")
Write-Host "version $Version"

# The icon: the program's own (original, drawn by code); made here when the repository does not have it yet.
if (-not $Icon) {
    $candidate = Join-Path $repo 'profiles\chili\resources\app_icon.ico'
    if (Test-Path -LiteralPath $candidate) { $Icon = $candidate }
}
$makeIcon = Join-Path $Out 'tools\MakeIcon.exe'
New-Item -ItemType Directory -Force (Split-Path -Parent $makeIcon) | Out-Null
& $csc @cscArgs @common '/target:exe' '/platform:anycpu' "/out:$makeIcon" @references (Join-Path $here 'tools\MakeIcon.cs') (Join-Path $here 'src\Shims.cs') (Join-Path $LauncherSrc 'Theme.cs')
if ($LASTEXITCODE -ne 0) { throw "MakeIcon did not compile (exit code $LASTEXITCODE)." }
if (-not $Icon) {
    $iconDir = Join-Path $Out 'icon'
    & $makeIcon $iconDir --sheet (Join-Path $iconDir 'icon_sheet.png')
    if ($LASTEXITCODE -ne 0) { throw 'MakeIcon failed.' }
    $Icon = Join-Path $iconDir 'app_icon.ico'
}
$Icon = (Resolve-Path -LiteralPath $Icon).Path
Write-Host "icon  $Icon"

# The fonts and their licence go in as resources: font:<file>, OFL.txt.
$fonts = Join-Path $here 'fonts'
$resources = @()
foreach ($font in 'ChangaOne-Italic.ttf', 'Tektur-wdth-wght.ttf', 'Saira-Italic-wdth-wght.ttf') {
    $file = Join-Path $fonts $font
    if (-not (Test-Path -LiteralPath $file)) { throw "The font $file is missing." }
    $resources += "/resource:$file,font:$font"
}
$resources += "/resource:$(Join-Path $fonts 'OFL.txt'),OFL.txt"
$resources += "/resource:$Icon,icon"

$exe = Join-Path $Out 'Setup.stub.exe'
$sources = @(Get-ChildItem (Join-Path $here 'src') -Filter *.cs | ForEach-Object { $_.FullName }) + $shared + $versionFile
& $csc @cscArgs @common '/target:winexe' '/platform:anycpu' "/out:$exe" "/win32icon:$Icon" "/win32manifest:$(Join-Path $here 'app.manifest')" @resources @references @sources
if ($LASTEXITCODE -ne 0) { throw "Setup did not compile (exit code $LASTEXITCODE)." }
Write-Host "built $exe ($((Get-Item $exe).Length) bytes)"

if ($Test) {
    $report = Join-Path $Out 'test_report.txt'
    $testArgs = @('--test')
    if ($Iso) { $testArgs += @('--iso', "`"$Iso`"") }
    $p = Start-Process -FilePath $exe -ArgumentList $testArgs -NoNewWindow -Wait -PassThru -RedirectStandardOutput $report
    Get-Content -LiteralPath $report
    if ($p.ExitCode -ne 0) { throw "The tests failed (exit code $($p.ExitCode))." }
}
