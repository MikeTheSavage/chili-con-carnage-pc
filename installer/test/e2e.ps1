# The installer from end to end, without a window: installs into a stand-in home (--home), installs over it,
# uninstalls keeping and removing the player's files, and tries what must be refused. Made-up saves, art and
# packs only; the player's own .iso is read in place. Everything it makes is under -Work, which is deleted
# at the end (it holds the test install, which holds the game's program).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File installer\test\e2e.ps1 -Setup <ChiliConCarnagePC-Setup-x.y.z.exe> `
#       -Iso <your .iso> -Work <empty scratch folder> [-Keep]
#
# The Apps entry it makes is the test one (HKCU\...\Uninstall\ChiliConCarnagePC-Test, shown as
# "Chili Con Carnage PC (test)") and is removed by the test's own uninstall; nothing of a real installation
# of the current user is touched.
param(
    [Parameter(Mandatory = $true)][string]$Setup,
    [Parameter(Mandatory = $true)][string]$Iso,
    [Parameter(Mandatory = $true)][string]$Work,
    [switch]$Keep
)
$ErrorActionPreference = 'Stop'
$Setup = (Resolve-Path -LiteralPath $Setup).Path
$Work = [IO.Path]::GetFullPath($Work)
$home_ = Join-Path $Work 'home'
$dir = Join-Path $home_ 'Local\Programs\Chili Con Carnage PC'
$key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\ChiliConCarnagePC-Test'
$script:failed = 0
$script:passed = 0
function Check([string]$What, [bool]$Ok, [string]$Detail = '') {
    if ($Ok) { $script:passed++; Write-Host "PASS $What" } else { $script:failed++; Write-Host "FAIL $What $(if ($Detail) { "($Detail)" })" }
}
function Run([string]$Exe, [string[]]$Arguments) {
    $p = Start-Process -FilePath $Exe -ArgumentList $Arguments -PassThru -Wait -WindowStyle Hidden
    return $p.ExitCode
}
function Tree([string]$Root) {
    if (-not (Test-Path -LiteralPath $Root)) { return @() }
    return @(Get-ChildItem -LiteralPath $Root -Recurse -Force | ForEach-Object { $_.FullName.Substring($Root.Length + 1) } | Sort-Object)
}
function Hashes([string]$Root, [string[]]$Files) { return @($Files | ForEach-Object { (Get-FileHash -LiteralPath (Join-Path $Root $_) -Algorithm SHA256).Hash }) }
function Manifest { return (Get-Content -LiteralPath (Join-Path $dir 'install.manifest') -Raw | ConvertFrom-Json) }
function WaitGone([string]$Path, [int]$Seconds = 10) {
    for ($i = 0; $i -lt $Seconds * 4 -and (Test-Path -LiteralPath $Path); $i++) { Start-Sleep -Milliseconds 250 }
    return -not (Test-Path -LiteralPath $Path)
}

if (Test-Path -LiteralPath $Work) { throw "-Work must not exist yet: $Work" }
if (Test-Path $key) { throw "A test Apps entry is already there ($key): remove it first." }
New-Item -ItemType Directory -Force $home_ | Out-Null
$isoQ = "`"$Iso`""
$homeQ = "`"$home_`""
$started = Get-Date

# 1. A fresh install, silent.
$code = Run $Setup @('--silent', '--iso', $isoQ, '--home', $homeQ, '--log', "`"$Work\setup1.log`"")
Check 'fresh install: exit 0' ($code -eq 0) "exit $code"
$m = Manifest
$listed = @($m.files | ForEach-Object { $_.path.Replace('/', '\') })
$bad = @($m.files | Where-Object { $_.part -ne 'setup-made' -and (Get-FileHash -LiteralPath (Join-Path $dir $_.path) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $_.sha256 })
Check "fresh install: the $($listed.Count) files of install.manifest are there with their sha256" ($bad.Count -eq 0) (($bad | ForEach-Object { $_.path }) -join ', ')
$extra = @(Get-ChildItem -LiteralPath $dir -Recurse -File -Force | ForEach-Object { $_.FullName.Substring($dir.Length + 1) } |
    Where-Object { $listed -notcontains $_ -and @('install.manifest', 'ChiliNative.ini', 'launcher.ini', 'logs\setup.log') -notcontains $_ })
Check 'fresh install: nothing else in the folder' ($extra.Count -eq 0) ($extra -join ', ')
Check 'fresh install: the 27 loading pictures' (@(Get-ChildItem -LiteralPath (Join-Path $dir 'mods\hd_loading\textures') -Filter *.png).Count -eq 27)
Check 'fresh install: no music tool, no templates, no notes' (-not (Test-Path (Join-Path $dir 'setup')) -and -not (Test-Path (Join-Path $dir 'template')) -and -not (Test-Path (Join-Path $dir 'WHAT_CHANGED.txt')))
$ini = [IO.File]::ReadAllText((Join-Path $dir 'ChiliNative.ini'))
Check 'fresh install: ChiliNative.ini names the disc' ($ini.Contains("`nPath=$Iso")) ''
foreach ($link in "$home_\Roaming\Microsoft\Windows\Start Menu\Programs\Chili Con Carnage PC\Chili Con Carnage.lnk",
    "$home_\Roaming\Microsoft\Windows\Start Menu\Programs\Chili Con Carnage PC\Chili Con Carnage settings.lnk", "$home_\Desktop\Chili Con Carnage.lnk", "$dir\Chili Con Carnage settings.lnk") {
    Check "fresh install: $(Split-Path -Leaf (Split-Path -Parent $link))\$(Split-Path -Leaf $link)" (Test-Path -LiteralPath $link)
}
$entry = Get-ItemProperty -Path $key -ErrorAction SilentlyContinue
Check 'fresh install: the Apps entry (HKCU, test name)' ($entry -and $entry.DisplayName -eq 'Chili Con Carnage PC (test)' -and $entry.UninstallString -eq "`"$dir\Uninstall.exe`"" -and $entry.InstallLocation -eq $dir) ''
if ($entry) { Write-Host ("     DisplayName={0} DisplayVersion={1} Publisher={2} EstimatedSize={3} KB" -f $entry.DisplayName, $entry.DisplayVersion, $entry.Publisher, $entry.EstimatedSize) }
$errors = @(Select-String -LiteralPath "$Work\setup1.log" -Pattern 'error:' -SimpleMatch)
Check 'fresh install: no error in the log' ($errors.Count -eq 0) (($errors | ForEach-Object { $_.Line }) -join ' / ')

# 2. The player's own files (made up), then an update without the pack.
$save = 'saves\PSP\SAVEDATA\ULUS10216DATA00'
New-Item -ItemType Directory -Force (Join-Path $dir $save), (Join-Path $dir 'art'), (Join-Path $dir 'mods\my_pack\textures') | Out-Null
[IO.File]::WriteAllText((Join-Path $dir "$save\made_up.txt"), 'a made-up save')
[IO.File]::WriteAllBytes((Join-Path $dir "$save\PARAM.SFO"), (New-Object byte[] 16))
[IO.File]::WriteAllBytes((Join-Path $dir 'art\logo.png'), [byte[]](0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A))
[IO.File]::WriteAllBytes((Join-Path $dir 'mods\my_pack\textures\0000000000000001.png'), [byte[]](1))
$iniLines = [IO.File]::ReadAllText((Join-Path $dir 'ChiliNative.ini'))
$changed = [regex]::Replace($iniLines, '(?m)^BufferMs=\d+', 'BufferMs=120', 1)
if ($changed -eq $iniLines) { $changed = $iniLines + "`n[Audio]`nBufferMs=120`n" }
[IO.File]::WriteAllText((Join-Path $dir 'ChiliNative.ini'), $changed)
$mine = @("$save\made_up.txt", "$save\PARAM.SFO", 'art\logo.png', 'mods\my_pack\textures\0000000000000001.png', 'ChiliNative.ini', 'launcher.ini')
$before = Hashes $dir $mine
$pictures = Hashes $dir @(Get-ChildItem -LiteralPath (Join-Path $dir 'mods\hd_loading\textures') | ForEach-Object { "mods\hd_loading\textures\$($_.Name)" })
$code = Run $Setup @('--silent', '--iso', $isoQ, '--home', $homeQ, '--no-hd-loading', '--no-desktop', '--log', "`"$Work\setup2.log`"")
Check 'update without the pack: exit 0' ($code -eq 0) "exit $code"
Check 'update: the made-up save, art, pack, the changed ini line and launcher.ini are byte for byte the same' ((Compare-Object $before (Hashes $dir $mine)) -eq $null)
Check 'update: the loading pictures, not chosen, are left as they were' ((Compare-Object $pictures (Hashes $dir @(Get-ChildItem -LiteralPath (Join-Path $dir 'mods\hd_loading\textures') | ForEach-Object { "mods\hd_loading\textures\$($_.Name)" }))) -eq $null)
Check 'update: install.manifest still names the pack (kept), so the uninstaller knows it' (@((Manifest).files | Where-Object { $_.part -eq 'hd_loading' }).Count -eq 28 -and -not (Manifest).hdLoading)
Check 'update: the desktop shortcut not wanted this time is gone' (-not (Test-Path -LiteralPath "$home_\Desktop\Chili Con Carnage.lnk"))
Check 'update: the desktop folder itself stays, even empty' (Test-Path -LiteralPath "$home_\Desktop")
Check 'update: logs\setup.log says update' ((Get-Content -LiteralPath (Join-Path $dir 'logs\setup.log') -Raw) -match 'update')
$code = Run $Setup @('--silent', '--iso', $isoQ, '--home', $homeQ, '--log', "`"$Work\setup3.log`"")
$bad = @((Manifest).files | Where-Object { $_.part -eq 'hd_loading' -and (Get-FileHash -LiteralPath (Join-Path $dir $_.path) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $_.sha256 })
Check 'update with the pack again: exit 0, the pictures equal the manifest' ($code -eq 0 -and $bad.Count -eq 0) "exit $code"
Check 'update: the save is still the same' ((Compare-Object $before (Hashes $dir $mine)) -eq $null)

# 3. Uninstall, keeping the player's files.
$code = Run (Join-Path $dir 'Uninstall.exe') @('--silent', '--log', "`"$Work\uninstall1.log`"")
Check 'uninstall: exit 0' ($code -eq 0) "exit $code"
Check 'uninstall: Uninstall.exe is gone a moment later' (WaitGone (Join-Path $dir 'Uninstall.exe'))
$left = Tree $dir
$want = @('art', 'art\logo.png', 'ChiliNative.ini', 'launcher.ini', 'logs', 'logs\setup.log', 'mods', 'mods\my_pack', 'mods\my_pack\textures', 'mods\my_pack\textures\0000000000000001.png',
    'saves', 'saves\PSP', 'saves\PSP\SAVEDATA', $save, "$save\made_up.txt", "$save\PARAM.SFO") | Sort-Object
Check 'uninstall: only the player''s files are left' ((Compare-Object $left $want) -eq $null) ($left -join ', ')
Check 'uninstall: the save is byte for byte the same' ((Compare-Object $before (Hashes $dir $mine)) -eq $null)
Check 'uninstall: the Start menu folder and the Apps entry are gone' (-not (Test-Path "$home_\Roaming\Microsoft\Windows\Start Menu\Programs\Chili Con Carnage PC") -and -not (Test-Path $key))

# 4. Install where the kept files are (an update), then uninstall with the saves.
$code = Run $Setup @('--silent', '--iso', $isoQ, '--home', $homeQ, '--log', "`"$Work\setup4.log`"")
Check 'install over what an uninstall kept: exit 0, the save kept' ($code -eq 0 -and (Compare-Object $before (Hashes $dir $mine)) -eq $null) "exit $code"
$code = Run (Join-Path $dir 'Uninstall.exe') @('--silent', '--remove-saves', '--log', "`"$Work\uninstall2.log`"")
Check 'uninstall with saves: exit 0, the folder is gone' ($code -eq 0 -and (WaitGone $dir)) "exit $code; left: $((Tree $dir) -join ', ')"
Check 'uninstall with saves: the home holds no file of the game' (@(Get-ChildItem -LiteralPath $home_ -Recurse -File -Force).Count -eq 0) ((Tree $home_) -join ', ')

# 5. What is refused, and how.
$fake = Join-Path $Work 'fake'
New-Item -ItemType Directory -Force $fake | Out-Null
$cso = New-Object byte[] 65536; [Text.Encoding]::ASCII.GetBytes('CISO').CopyTo($cso, 0)
[IO.File]::WriteAllBytes("$fake\game.cso", $cso)
$code = Run $Setup @('--silent', '--iso', "`"$fake\game.cso`"", '--home', $homeQ, '--log', "`"$Work\refuse.log`"")
Check 'a .cso: exit 3' ($code -eq 3) "exit $code"
[IO.File]::WriteAllBytes("$fake\zeros.iso", (New-Object byte[] 81920))
$code = Run $Setup @('--silent', '--iso', "`"$fake\zeros.iso`"", '--home', $homeQ, '--log', "`"$Work\refuse.log`"")
Check 'a file that is no disc image: exit 3' ($code -eq 3) "exit $code"
$code = Run $Setup @('--silent', '--iso', "`"$fake\missing.iso`"", '--home', $homeQ, '--log', "`"$Work\refuse.log`"")
Check 'a missing .iso: exit 3' ($code -eq 3) "exit $code"
$stray = Join-Path $Work 'stray'
New-Item -ItemType Directory -Force $stray | Out-Null
[IO.File]::WriteAllText("$stray\notes.txt", 'x')
$code = Run $Setup @('--silent', '--iso', $isoQ, '--home', $homeQ, '--dir', "`"$stray`"", '--log', "`"$Work\refuse.log`"")
Check 'a folder with a stray file: exit 4' ($code -eq 4) "exit $code"
$code = Run $Setup @('--silent', '--iso', $isoQ, '--home', $homeQ, '--dir', ([IO.Path]::GetPathRoot($Work)), '--log', "`"$Work\refuse.log`"")
Check 'a drive: exit 4' ($code -eq 4) "exit $code"
$busy = Join-Path $Work 'busy'
New-Item -ItemType Directory -Force $busy | Out-Null
[IO.File]::WriteAllText("$busy\ChiliNative.ini", "[Disc]`n")
Copy-Item -LiteralPath (Join-Path $env:WINDIR 'System32\PING.EXE') -Destination "$busy\ping.exe"
$pinger = Start-Process -FilePath "$busy\ping.exe" -ArgumentList '-n 30 127.0.0.1' -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 500
$code = Run $Setup @('--silent', '--iso', $isoQ, '--home', $homeQ, '--dir', "`"$busy`"", '--log', "`"$Work\refuse.log`"")
Stop-Process -Id $pinger.Id -ErrorAction SilentlyContinue
Check 'a program running from the folder: exit 7' ($code -eq 7) "exit $code"
Check 'the refusals installed nothing' (-not (Test-Path $dir) -and @(Get-ChildItem -LiteralPath $stray).Count -eq 1 -and -not (Test-Path $key))

# 6. Setup's temp folders are gone; the registry keys of the test are gone.
$temps = @(Get-ChildItem -LiteralPath ([IO.Path]::GetTempPath()) -Directory -Filter 'ChiliConCarnagePC-Setup-*' -ErrorAction SilentlyContinue | Where-Object { $_.CreationTime -ge $started })
Check 'no temp folder of Setup is left' ($temps.Count -eq 0) (($temps | ForEach-Object { $_.Name }) -join ', ')
Check 'no test registry key is left' (-not (Test-Path $key) -and -not (Test-Path 'HKCU:\Software\ChiliConCarnagePC\Test'))

Write-Host "$script:passed passed, $script:failed failed ($([int]((Get-Date) - $started).TotalSeconds) s)"
if (-not $Keep) {
    Remove-Item -LiteralPath $Work -Recurse -Force
    Write-Host "removed $Work"
}
exit $script:failed
