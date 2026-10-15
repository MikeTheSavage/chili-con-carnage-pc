# Builds the launcher: one executable for .NET Framework 4.8 (in the box on Windows 10 and 11), compiled
# with the C# compiler that is already on this machine. Nothing is downloaded.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File profiles\chili\launcher\build.ps1
#   ... -Out <folder>      where the executable goes (default: bin beside this script)
#   ... -Test              also builds the stand-in game (test\FakeGame.cs) and runs the tests that need no window
#   ... -Icon <file>       the program's icon (default: profiles\chili\resources\app_icon.ico, see below)
param(
    [string]$Out = "",
    [switch]$Test,
    [string]$Icon = ""
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Out) { $Out = Join-Path $here 'bin' }
New-Item -ItemType Directory -Force $Out | Out-Null

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

# The .NET Framework 4.8 reference assemblies (installed with Visual Studio's .NET Framework targeting pack);
# without them, the assemblies of the framework itself.
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

# The program's icon (Explorer, the taskbar, the window): the same original picture as the game program's,
# profiles\chili\resources\app_icon.ico, read from there when the launcher is built: in this checkout, else
# in the repository's main checkout (for a worktree of a branch without that file).
if ($Icon) {
    if (-not (Test-Path -LiteralPath $Icon)) { throw "The icon is missing: $Icon" }
    $iconFile = $Icon
} else {
    $candidates = @((Join-Path $here '..\resources\app_icon.ico'))
    try {
        $common = & git -C $here rev-parse --path-format=absolute --git-common-dir 2>$null
        if ($LASTEXITCODE -eq 0 -and $common) { $candidates += (Join-Path (Split-Path -Parent $common) 'profiles\chili\resources\app_icon.ico') }
    } catch { }
    $iconFile = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $iconFile) { throw "The icon is missing: $($candidates -join ', ')" }
}
$icon = (Resolve-Path -LiteralPath $iconFile).Path
Write-Host "icon  $icon"

# The version, the publisher and the notice of the file (Explorer's Details): the game program's own, from
# host\version.hpp and resources\ChiliConCarnage.rc, written into a temporary source file for this build.
$version = '0.0.0'
$hpp = Join-Path $here '..\host\version.hpp'
if ((Test-Path -LiteralPath $hpp) -and [IO.File]::ReadAllText($hpp) -match '#define\s+CHILI_VERSION_TEXT\s+"(\d+\.\d+\.\d+)"') { $version = $Matches[1] }
$company = ''; $copyright = ''
$rc = Join-Path $here '..\resources\ChiliConCarnage.rc'
if (Test-Path -LiteralPath $rc) {
    $rcText = [IO.File]::ReadAllText($rc)
    if ($rcText -match 'VALUE "CompanyName", "([^"\\]*)"') { $company = $Matches[1] }
    if ($rcText -match 'VALUE "LegalCopyright", "([^"\\]*)"') { $copyright = $Matches[1] }
}
$versionFile = Join-Path ([IO.Path]::GetTempPath()) ('chili_launcher_version_' + [guid]::NewGuid().ToString('N') + '.cs')
[IO.File]::WriteAllText($versionFile, ("[assembly: System.Reflection.AssemblyVersion(`"$version.0`")]`r`n" +
    "[assembly: System.Reflection.AssemblyFileVersion(`"$version.0`")]`r`n[assembly: System.Reflection.AssemblyInformationalVersion(`"$version`")]`r`n" +
    "[assembly: System.Reflection.AssemblyCompany(`"$company`")]`r`n[assembly: System.Reflection.AssemblyCopyright(`"$copyright`")]`r`n"))
Write-Host "version $version"

$exe = Join-Path $Out 'ChiliConCarnageLauncher.exe'
$sources = @(Get-ChildItem (Join-Path $here 'src') -Filter *.cs | ForEach-Object { $_.FullName }) + $versionFile
$common = @('/nologo', '/noconfig', '/nostdlib+', '/codepage:65001', '/optimize+', '/debug-', '/deterministic+', '/langversion:7.3', '/warn:4', '/nowarn:1591')
try {
    & $csc @cscArgs @common '/target:winexe' '/platform:anycpu' "/out:$exe" "/win32icon:$icon" "/win32manifest:$(Join-Path $here 'app.manifest')" `
        "/resource:$icon,icon" @references @sources
} finally { [IO.File]::Delete($versionFile) }
if ($LASTEXITCODE -ne 0) { throw "The launcher did not compile (exit code $LASTEXITCODE)." }
Write-Host "built $exe ($((Get-Item $exe).Length) bytes)"

if ($Test) {
    $fake = Join-Path $Out 'test'
    New-Item -ItemType Directory -Force $fake | Out-Null
    $fakeExe = Join-Path $fake 'FakeGame.exe'
    & $csc @cscArgs @common '/target:winexe' "/out:$fakeExe" "/resource:$(Join-Path $here 'test\probes.txt'),probes" `
        "/r:$(Join-Path $ref 'mscorlib.dll')" "/r:$(Join-Path $ref 'System.dll')" "/r:$(Join-Path $ref 'System.Core.dll')" (Join-Path $here 'test\FakeGame.cs')
    if ($LASTEXITCODE -ne 0) { throw "The stand-in game did not compile (exit code $LASTEXITCODE)." }
    Write-Host "built $fakeExe"
    $real = Join-Path $here 'test\sample_ChiliNative.ini'
    $p = Start-Process -FilePath $exe -ArgumentList @('--test-ini', "`"$real`"") -NoNewWindow -Wait -PassThru -RedirectStandardOutput (Join-Path $Out 'test\ini_test.txt')
    Get-Content (Join-Path $Out 'test\ini_test.txt')
    if ($p.ExitCode -ne 0) { throw 'The tests failed.' }
    # The program says which icon it carries; it must be the file named above, byte for byte.
    $carried = (Get-Content (Join-Path $Out 'test\ini_test.txt') | Select-String -Pattern '^icon: .* sha1 ([0-9a-f]{40})$' | Select-Object -First 1)
    $wanted = (Get-FileHash -LiteralPath $icon -Algorithm SHA1).Hash.ToLowerInvariant()
    if (-not $carried -or $carried.Matches[0].Groups[1].Value -ne $wanted) { throw "The program does not carry the icon $icon (sha1 $wanted)." }
    Write-Host "the program carries $icon"
}
