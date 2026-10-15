<#
.SYNOPSIS
    Assembles the folder a player runs: the launcher, ChiliConCarnage.exe and what they need, and no game data.

.DESCRIPTION
    Takes a finished build (out\chili-rel by default) and makes or updates the package folder:

        ChiliConCarnageLauncher.exe      the program a player starts: the settings, and PLAY. launcher\build.ps1
                                         compiles it here (the C# compiler of Visual Studio or of the .NET SDK)
        launcher.ini                     the launcher's own switches (kept when the folder already has one)
        Chili Con Carnage settings.lnk   starts the launcher with --settings: the way back to the settings
                                         for a player who switched on "Skip this window next time"
                                         (not with -NoSettingsShortcut)
        ChiliConCarnage.exe      the build's chili_recomp.exe, marked as a program without a console
        *.dll                    FFmpeg, and the Visual C++ runtime when Visual Studio has it
        ChiliNative.ini          the default settings (kept when the folder already has one)
        README.txt, CHANGELOG.md, CONTROLS.md, RENDERING.md
        Dump textures.cmd
        licenses\                this project's licence (MIT), the third-party notices, FFmpeg's licence (LGPL 2.1)
        mods\README.md, mods\TEXTURES.md
        tools\                   the Python mod and texture tools

    The folder is portable: the game makes saves\ and logs\ beside the executable on its first start
    and asks for the player's own disc (an .iso or an unpacked disc folder). Nothing of the game is put
    in the package, and the script stops if it finds a game file there.

    Updating an existing package replaces the program files only: ChiliNative.ini, launcher.ini, saves\,
    logs\, textures\, art\ and the mod folders stay as they are. The files of saves\ and art\ are hashed
    before and after, and the script stops if one of them changed or is gone.

    The launcher shows the game's own logo and a load-screen picture only when the player has them in the
    folder (art\, or textures\ after "Dump textures.cmd" on the player's own disc). This script puts no
    picture there.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File package.ps1
.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File package.ps1 -Build ..\..\..\out\chili-rel -Out D:\Games\ChiliConCarnagePC `
        -Disc "D:\dumps\Chili Con Carnage (USA).iso" -DesktopShortcut
#>
[CmdletBinding()]
param(
    # The build directory (the one that holds bin\chili_recomp.exe), or the executable itself.
    [string]$Build = '',
    # The package folder. Default: dist\ChiliConCarnagePC beside the repository folder.
    [string]$Out = '',
    # Writes this disc (.iso file or unpacked disc folder) into [Disc] Path of a newly made ChiliNative.ini,
    # so the first start does not ask. Only the path is written; the game checks the disc when it starts.
    [string]$Disc = '',
    # Uses this ChiliNative.ini instead of the defaults when the package has none yet.
    [string]$ImportSettings = '',
    # Also makes "Chili Con Carnage.lnk" on the desktop of the current user: it starts the launcher.
    [switch]$DesktopShortcut,
    # Leave the launcher out: the package is then the game alone, started by ChiliConCarnage.exe.
    [switch]$NoLauncher,
    # Leave out "Chili Con Carnage settings.lnk" (a folder made for the installer, which makes its own shortcuts:
    # a shortcut file records the folder and the computer it was made on).
    [switch]$NoSettingsShortcut,
    # Leave the Visual C++ runtime DLLs out (the PC then needs the Visual C++ Redistributable).
    [switch]$NoRuntime
)
$ErrorActionPreference = 'Stop'

$profile_ = Split-Path -Parent $PSScriptRoot
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
function Resolve-FullPath([string]$Path) {
    return [IO.Path]::GetFullPath($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path))
}
if (-not $Build) { $Build = Join-Path $repo 'out\chili-rel' }
$Build = Resolve-FullPath $Build
$sourceExe = $Build
if (Test-Path -LiteralPath $Build -PathType Container) { $sourceExe = Join-Path $Build 'bin\chili_recomp.exe' }
if (-not (Test-Path -LiteralPath $sourceExe -PathType Leaf)) { throw "No build there: $sourceExe is missing (scripts\build.ps1 -Config rel makes it)" }
$bin = Split-Path -Parent $sourceExe
if (-not $Out) { $Out = Join-Path (Split-Path -Parent $repo) 'dist\ChiliConCarnagePC' }
$Out = Resolve-FullPath $Out
$exe = Join-Path $Out 'ChiliConCarnage.exe'
$launcher = Join-Path $Out 'ChiliConCarnageLauncher.exe'

# The package is never made inside the disc folder, the build tree's bin folder or the play folder.
foreach ($reserved in @((Join-Path $profile_ 'game'), $bin, (Join-Path (Split-Path -Parent $repo) 'play'))) {
    $full = (Resolve-FullPath $reserved).TrimEnd('\') + '\'
    if (($Out.TrimEnd('\') + '\').StartsWith($full, [StringComparison]::OrdinalIgnoreCase)) { throw "The package folder must not be inside $reserved" }
}
# A running game has its files open: never replace them under it.
$running = @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
        try { $_.Path -and $_.Path.StartsWith($Out.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) } catch { $false } })
if ($running.Count) { throw "The game or its launcher in $Out is running (pid $($running[0].Id)). Close it first." }

# Sets the subsystem field of a PE file to Windows GUI, as "editbin /SUBSYSTEM:WINDOWS" does: Windows then
# opens no console window for the program. The entry point stays main(); the program itself decides
# whether it writes to the caller's output or to its log file (host/launcher.cpp).
function Set-WindowsSubsystem([string]$File) {
    $stream = [IO.File]::Open($File, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite)
    try {
        $reader = New-Object IO.BinaryReader($stream)
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw "$File is not an executable" }
        $stream.Position = 0x3C
        $pe = $reader.ReadInt32()
        $stream.Position = $pe
        if ($reader.ReadUInt32() -ne 0x00004550) { throw "$File has no PE header" }
        $stream.Position = $pe + 24
        $magic = $reader.ReadUInt16()
        if ($magic -ne 0x20B -and $magic -ne 0x10B) { throw "$File has an unknown optional header" }
        # Subsystem is at offset 68 of the optional header in both PE32 and PE32+.
        $stream.Position = $pe + 24 + 68
        $subsystem = $reader.ReadUInt16()
        if ($subsystem -eq 3) {
            $stream.Position = $pe + 24 + 68
            $stream.Write([byte[]](2, 0), 0, 2)
        } elseif ($subsystem -ne 2) { throw "$File has subsystem $subsystem, expected console (3) or Windows (2)" }
    } finally { $stream.Dispose() }
}

function Copy-Text([string]$Source, [string]$Target) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) { throw "Missing in the source tree: $Source" }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Target) | Out-Null
    Copy-Item -LiteralPath $Source -Destination $Target -Force
}

New-Item -ItemType Directory -Force -Path $Out | Out-Null
Write-Host "[package] from $sourceExe"
Write-Host "[package] to   $Out"

# The player's own files in a folder that is updated: saves\ (with the kept earlier saves) and art\ are hashed
# before the first change and compared at the end. This script writes to neither; the comparison is there so that no later change to it can take a save or a
# picture away unnoticed.
function Get-OwnFiles {
    $lines = foreach ($folder in 'saves', 'art') {
        $path = Join-Path $Out $folder
        if (Test-Path -LiteralPath $path -PathType Container) {
            Get-ChildItem -LiteralPath $path -Recurse -File | Sort-Object FullName | ForEach-Object {
                $_.FullName.Substring($Out.TrimEnd('\').Length + 1) + ' ' + (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
        }
    }
    return @($lines)
}
$ownBefore = Get-OwnFiles

# 1. The program.
Copy-Item -LiteralPath $sourceExe -Destination $exe -Force
Set-WindowsSubsystem $exe
$version = (Get-Item -LiteralPath $exe).VersionInfo
if ($version.OriginalFilename -ne 'ChiliConCarnage.exe') {
    throw "This build has no version information: it was made before the packaging work. Rebuild it (scripts\build.ps1 -Config rel) and run this script again."
}

# 2. FFmpeg and its licence.
$ffmpeg = @(Get-ChildItem -LiteralPath $bin -Filter '*.dll' | Where-Object { $_.Name -match '^(avcodec|avformat|avutil|swresample|swscale)-\d+\.dll$' })
if ($ffmpeg.Count -ne 5) { throw "Expected the five FFmpeg DLLs in $bin, found $($ffmpeg.Count)" }
foreach ($dll in $ffmpeg) { Copy-Item -LiteralPath $dll.FullName -Destination (Join-Path $Out $dll.Name) -Force }
Copy-Text (Join-Path $bin 'COPYING.FFmpeg.LGPLv2.1') (Join-Path $Out 'licenses\COPYING.FFmpeg.LGPLv2.1.txt')
# This project's licence (MIT, with PSPRecomp's notice) and the notices of everything else the package holds.
Copy-Text (Join-Path $repo 'LICENSE') (Join-Path $Out 'licenses\LICENSE.txt')
Copy-Text (Join-Path $repo 'THIRD_PARTY_NOTICES.md') (Join-Path $Out 'licenses\THIRD_PARTY_NOTICES.md')

# 3. The Visual C++ runtime beside the program, so the PC needs no redistributable installed.
$runtimeNote = ''
$runtimeNames = @('msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
if (-not $NoRuntime) {
    $crt = $null
    $roots = @('C:\Program Files\Microsoft Visual Studio\18\Community')
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) { $roots += @(& $vswhere -all -products * -property installationPath) }
    foreach ($root in ($roots | Where-Object { $_ } | Select-Object -Unique)) {
        $found = @(Get-ChildItem -Path (Join-Path $root 'VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT') -Directory -ErrorAction SilentlyContinue |
                Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'vcruntime140.dll') } | Sort-Object FullName)
        if ($found.Count) { $crt = $found[-1].FullName; break }
    }
    if ($crt) {
        foreach ($name in $runtimeNames) {
            $file = Join-Path $crt $name
            if (Test-Path -LiteralPath $file) { Copy-Item -LiteralPath $file -Destination (Join-Path $Out $name) -Force }
        }
        $runtimeNote = "`n  msvcp140*.dll and vcruntime140*.dll are the Microsoft Visual C++ runtime, redistributed as its`n  licence allows.`n"
        Write-Host "[package] Visual C++ runtime from $crt"
    } else {
        Write-Host '[package] Visual C++ runtime not found on this PC: the package needs the Visual C++ Redistributable (x64) installed'
    }
}
if (-not $runtimeNote) {
    foreach ($name in $runtimeNames) { $stale = Join-Path $Out $name; if (Test-Path -LiteralPath $stale) { [IO.File]::Delete($stale) } }
    $runtimeNote = "`n  The program needs the Microsoft Visual C++ Redistributable (x64), which most PCs have; it is at`n  https://aka.ms/vs/17/release/vc_redist.x64.exe`n"
}

# 4. Documents, mods folder, tools.
$readme = [IO.File]::ReadAllText((Join-Path $profile_ 'package\README.txt'))
$readme = $readme.Replace('@VERSION@', $version.ProductVersion).Replace('@RUNTIME_NOTE@', $runtimeNote.TrimEnd("`n"))
[IO.File]::WriteAllText((Join-Path $Out 'README.txt'), ($readme -replace "`r?`n", "`r`n"), (New-Object Text.UTF8Encoding($false)))
# What changed, for the player.
Copy-Text (Join-Path $repo 'CHANGELOG.md') (Join-Path $Out 'CHANGELOG.md')
# The documents name the executable as the build tree does; in the package it has its player's name.
foreach ($document in 'CONTROLS.md', 'RENDERING.md', 'mods\README.md', 'mods\TEXTURES.md') {
    $source = Join-Path $profile_ $document
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing in the source tree: $source" }
    $target = Join-Path $Out $document
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    [IO.File]::WriteAllText($target, [IO.File]::ReadAllText($source).Replace('chili_recomp.exe', 'ChiliConCarnage.exe'), (New-Object Text.UTF8Encoding($false)))
}
foreach ($tool in 'naz.py', 'props.py', 'texture_psp.py', 'extract_textures.py', 'make_sample_mod.py', 'dump_textures.py') {
    Copy-Text (Join-Path $profile_ "tools\$tool") (Join-Path $Out "tools\$tool")
}
Copy-Text (Join-Path $profile_ 'package\Dump textures.cmd') (Join-Path $Out 'Dump textures.cmd')

# 5. Settings: the defaults as the program itself writes them, unless the folder already has a file.
$ini = Join-Path $Out 'ChiliNative.ini'
$newIni = -not (Test-Path -LiteralPath $ini)
if ($newIni) {
    if ($ImportSettings) { Copy-Item -LiteralPath (Resolve-FullPath $ImportSettings) -Destination $ini }
    $written = Start-Process -FilePath $exe -ArgumentList @('--write-default-ini', "`"$ini`"") -WorkingDirectory $Out -PassThru -Wait
    if ($written.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $ini)) {
        throw "The program did not write its default settings (exit code $($written.ExitCode)): this build was made before the packaging work. Rebuild it and run this script again."
    }
    if ($Disc) {
        $text = [IO.File]::ReadAllText($ini)
        $line = 'Path=' + (Resolve-FullPath $Disc)
        # The commented example line of [Disc] becomes the setting.
        $example = [regex]::new('(?ms)(^\[Disc\].*?^); Path=[^\r\n]*')
        if (-not $example.IsMatch($text)) { throw 'The default settings have no [Disc] section with a Path example' }
        $text = $example.Replace($text, { param($found) $found.Groups[1].Value + $line }, 1)
        [IO.File]::WriteAllText($ini, $text, (New-Object Text.UTF8Encoding($false)))
    }
} else {
    Write-Host '[package] ChiliNative.ini is already there and was kept'
    if ($Disc -or $ImportSettings) { Write-Host '[package] -Disc and -ImportSettings were not applied to the existing ChiliNative.ini' }
}

# 6. The launcher: the program a player starts. One file, compiled here from launcher\src; it needs nothing
#    else of its own in the package and brings no picture of the game. The folder launcher\bin is not copied:
#    it holds test files and, on a developer's PC, pictures from the disc.
$settingsLink = Join-Path $Out 'Chili Con Carnage settings.lnk'
if ($NoLauncher) {
    foreach ($stale in $launcher, $settingsLink) { if (Test-Path -LiteralPath $stale) { [IO.File]::Delete($stale) } }
    Write-Host '[package] no launcher (-NoLauncher): ChiliConCarnage.exe is the program to start'
} else {
    $icon = Join-Path $profile_ 'resources\app_icon.ico'
    $build = Join-Path $profile_ 'launcher\build.ps1'
    $built = @(& powershell -NoProfile -ExecutionPolicy Bypass -File $build -Out $Out -Icon $icon 2>&1 | ForEach-Object { "$_" })
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $launcher -PathType Leaf)) {
        throw "The launcher was not built: $($built -join ' / '). It needs the C# compiler of Visual Studio or the .NET SDK; -NoLauncher makes a package without it."
    }
    # The launcher starts on this PC, its own tests pass, and it carries the program's icon byte for byte.
    $report = Join-Path ([IO.Path]::GetTempPath()) ("chili_package_launcher_" + [guid]::NewGuid().ToString('N') + '.txt')
    try {
        $tested = Start-Process -FilePath $launcher -ArgumentList '--test-ini' -WorkingDirectory $Out -PassThru -Wait -NoNewWindow -RedirectStandardOutput $report
        $lines = @(Get-Content -LiteralPath $report)
    } finally { if (Test-Path -LiteralPath $report) { [IO.File]::Delete($report) } }
    if ($tested.ExitCode -ne 0) { throw "The packaged launcher failed its own tests: $(($lines | Where-Object { $_ -like 'FAIL*' }) -join ' / ')" }
    $carried = [string]($lines | Where-Object { $_ -match '^icon: .* sha1 [0-9a-f]{40}$' } | Select-Object -First 1)
    $wanted = (Get-FileHash -LiteralPath $icon -Algorithm SHA1).Hash.ToLowerInvariant()
    if (-not $carried.EndsWith($wanted)) { throw "The launcher does not carry $icon (sha1 $wanted): $carried" }
    # The game program shows the same picture: what Windows takes as each program's icon is equal, pixel for pixel.
    Add-Type -AssemblyName System.Drawing
    function Get-ShownIcon([string]$File) {
        $shown = [Drawing.Icon]::ExtractAssociatedIcon($File)
        try {
            $bitmap = $shown.ToBitmap()
            try {
                $text = New-Object Text.StringBuilder
                for ($y = 0; $y -lt $bitmap.Height; $y++) { for ($x = 0; $x -lt $bitmap.Width; $x++) { [void]$text.Append($bitmap.GetPixel($x, $y).ToArgb().ToString('x8')) } }
                return $text.ToString()
            } finally { $bitmap.Dispose() }
        } finally { $shown.Dispose() }
    }
    if ((Get-ShownIcon $exe) -ne (Get-ShownIcon $launcher)) { throw 'ChiliConCarnage.exe and ChiliConCarnageLauncher.exe do not show the same icon. Rebuild the game (scripts\build.ps1 -Config rel).' }
    Write-Host "[package] launcher built ($((Get-Item -LiteralPath $launcher).Length) bytes), its $(@($lines | Where-Object { $_ -like 'PASS*' }).Count) tests pass, both programs carry $icon"

    # launcher.ini is the player's, like ChiliNative.ini: written once. The game program beside the launcher,
    # the disc from [Disc] Path of ChiliNative.ini, saves\ and logs\ beside the game: no line for any of them.
    $launcherIni = Join-Path $Out 'launcher.ini'
    if (Test-Path -LiteralPath $launcherIni) { Write-Host '[package] launcher.ini is already there and was kept' }
    else {
        $text = "; Chili Con Carnage launcher. Delete this file to get the defaults back.`r`n`r`n[Launcher]`r`nGame=ChiliConCarnage.exe`r`nStartGameDirectly=false`r`nAfterGame=return`r`n"
        [IO.File]::WriteAllText($launcherIni, $text, (New-Object Text.UTF8Encoding($false)))
    }
    # The way back to the settings for a player who switched on "Skip this window next time".
    if ($NoSettingsShortcut) {
        if (Test-Path -LiteralPath $settingsLink) { [IO.File]::Delete($settingsLink) }
    } else {
        $shell = New-Object -ComObject WScript.Shell
        $link = $shell.CreateShortcut($settingsLink)
        $link.TargetPath = $launcher
        $link.Arguments = '--settings'
        $link.WorkingDirectory = $Out
        $link.IconLocation = "$launcher,0"
        $link.Description = 'Chili Con Carnage: settings'
        $link.Save()
    }
}

# 7. Nothing of the game may be in the program files of the package. (saves\ is the owner's own data; mods,
#    textures\ and art\ hold what the player put there from the player's own disc.)
$gameFiles = @(Get-ChildItem -LiteralPath $Out -Recurse -File | Where-Object {
        $inside = $_.FullName.Substring($Out.TrimEnd('\').Length + 1)
        $inside -notlike 'saves\*' -and $inside -notlike 'mods\*\*' -and $inside -notlike 'textures\*' -and $inside -notlike 'art\*' -and
        ($_.Name -match '\.(naz|iso|cso|pmf|at3|prx|sfo|texture_psp)$' -or $_.Name -match '^(E?BOOT\.BIN|UMD_DATA\.BIN|ICON0\.PNG|PIC1\.PNG|configpsp\.txt)$') })
if ($gameFiles.Count) { throw "Game files in the package, remove them: $(($gameFiles | ForEach-Object { $_.FullName }) -join ', ')" }

if ($DesktopShortcut) {
    $shell = New-Object -ComObject WScript.Shell
    $link = $shell.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Desktop')) 'Chili Con Carnage.lnk'))
    $link.TargetPath = $(if ($NoLauncher) { $exe } else { $launcher })
    $link.IconLocation = "$($link.TargetPath),0"
    $link.WorkingDirectory = $Out
    $link.Description = 'Chili Con Carnage (PC build)'
    $link.Save()
    Write-Host "[package] desktop shortcut: $($link.FullName)"
}

# The player's saves and pictures are what they were.
$ownAfter = Get-OwnFiles
$lost = @($ownBefore | Where-Object { $ownAfter -notcontains $_ })
if ($lost.Count) { throw "Files of the player's in saves\ or art\ changed or went missing during the update: $($lost -join ', ')" }
if ($ownBefore.Count) { Write-Host "[package] saves\ and art\ are as they were: $($ownBefore.Count) files, equal by sha256" }

# What the folder holds.
$total = 0
Write-Host '[package] contents:'
foreach ($file in (Get-ChildItem -LiteralPath $Out -Recurse -File | Sort-Object FullName)) {
    $total += $file.Length
    Write-Host ('[package]   {0,13:N0}  {1}' -f $file.Length, $file.FullName.Substring($Out.Length + 1))
}
Write-Host ('[package] {0:N1} MB in {1}' -f ($total / 1MB), $Out)
Write-Host "[package] start: $(if ($NoLauncher) { $exe } else { $launcher })"
