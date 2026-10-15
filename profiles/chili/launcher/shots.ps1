# Renders every page of the launcher to PNG files, off screen, on a made-up game folder (settings file,
# mods, a disc folder, a log), so the pictures do not depend on what is installed.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File shots.ps1 -Out <folder> [-Scale 2] [-Size 1280x720] [-NoArt] [-Game <game exe>] [-Only 21] [-FireTime 2.5]
#
# Beside the pictures it writes texts.txt: every text each picture draws, for reading through.
# -FireTime is the moment of the HD pack choice's fire that the pictures show (it stands still in them).
#
# -Size is the window in units (what the layout sees), -Scale the monitor's scale: the pictures are size times
# scale pixels. "-Scale 2" gives 2560x1440 pictures of the 1280x720 layout as a 200 % monitor shows it.
#
# Without -Game the stand-in game (test\FakeGame.cs) is used, which the launcher takes for the newest build
# (every setting known). With -Game <chili_recomp.exe> the rows that build has no setting for are left out.
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Size = '1280x720',
    [double]$Scale = 1,
    [switch]$NoArt,
    [string]$Game = '',
    [string]$Only = '',  # only the pictures whose name starts like this
    [double]$FireTime = -1,
    [switch]$Dev,        # a developer's layout: the game in its own folder, started with arguments, the settings file elsewhere
    [switch]$NoShots     # only make the folder (for --selftest)
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$bin = Join-Path $here 'bin'
$exe = Join-Path $bin 'ChiliConCarnageLauncher.exe'
if (-not (Test-Path $exe) -or -not (Test-Path (Join-Path $bin 'test\FakeGame.exe'))) { & (Join-Path $here 'build.ps1') -Test }

$fixture = Join-Path $bin 'fixture'
if (Test-Path $fixture) { Remove-Item -Recurse -Force $fixture }
New-Item -ItemType Directory -Force $fixture | Out-Null
'A made-up game folder for the launcher''s screenshots and self test. Safe to delete.' | Set-Content (Join-Path $fixture 'made_up_game_folder.txt')
Copy-Item (Join-Path $here 'test\sample_ChiliNative.ini') (Join-Path $fixture 'ChiliNative.ini')
$extra = ''
if ($Dev) {
    New-Item -ItemType Directory -Force (Join-Path $fixture 'game') | Out-Null
    Copy-Item (Join-Path $bin 'test\FakeGame.exe') (Join-Path $fixture 'game\ChiliConCarnage.exe')
    $Game = 'game\ChiliConCarnage.exe'
    $extra = "Arguments=`"{boot}`" `"{disc}`" --ms0 `"{saves}`"`r`nSaves=ms0`r`nLogs=devlogs"
}
elseif (-not $Game) { Copy-Item (Join-Path $bin 'test\FakeGame.exe') (Join-Path $fixture 'ChiliConCarnage.exe'); $Game = 'ChiliConCarnage.exe' }
@"
[Launcher]
Game=$Game
Disc=disc0
$extra
"@ | Set-Content -Encoding ASCII (Join-Path $fixture 'launcher.ini')

# A disc folder that passes the launcher's check (the id file and an empty BOOT.BIN; no game data).
New-Item -ItemType Directory -Force (Join-Path $fixture 'disc0\PSP_GAME\SYSDIR') | Out-Null
[IO.File]::WriteAllText((Join-Path $fixture 'disc0\UMD_DATA.BIN'), 'ULUS-10216|0000000000000000|0001|G|')
[IO.File]::WriteAllText((Join-Path $fixture 'disc0\PSP_GAME\SYSDIR\BOOT.BIN'), 'x')

# Mods: two texture packs, a gameplay mod, a file replacement and an empty folder.
$mods = Join-Path $fixture 'mods'
New-Item -ItemType Directory -Force (Join-Path $mods 'hd_textures\textures'), (Join-Path $mods 'hd_upscaled_4x\textures'),
    (Join-Path $mods 'pc_camera'), (Join-Path $mods 'sample_bootscreen\data_psp\textures\menu'), (Join-Path $mods 'work_in_progress') | Out-Null
1..214 | ForEach-Object { [IO.File]::WriteAllBytes((Join-Path $mods ("hd_textures\textures\{0:x16}.png" -f $_)), [byte[]]@(0)) }
1..37 | ForEach-Object { [IO.File]::WriteAllBytes((Join-Path $mods ("hd_upscaled_4x\textures\{0:x16}.png" -f $_)), [byte[]]@(0)) }
"# Hand-painted HUD, weapons and characters at four times the size" | Set-Content (Join-Path $mods 'hd_textures\README.md')
"; The camera stays closer behind the player`r`n[CameraDict]`r`nCharacterNormalCam.MaxAngle = 0.02" | Set-Content (Join-Path $mods 'pc_camera\properties.ini')
[IO.File]::WriteAllBytes((Join-Path $mods 'sample_bootscreen\data_psp\textures\menu\bootscreen.texture_psp'), [byte[]]@(0))

New-Item -ItemType Directory -Force (Join-Path $fixture 'logs') | Out-Null
"[start] made-up log" | Set-Content (Join-Path $fixture 'logs\ChiliConCarnage.log')
(Get-Item (Join-Path $fixture 'logs\ChiliConCarnage.log')).LastWriteTime = [datetime]'2026-10-03 21:40:00'

if (-not $NoArt -and (Test-Path (Join-Path $bin 'art'))) { Copy-Item -Recurse (Join-Path $bin 'art') (Join-Path $fixture 'art') }

if ($NoShots) { Write-Host "made $fixture"; return }
New-Item -ItemType Directory -Force $Out | Out-Null
$arguments = @('--base', "`"$fixture`"", '--screenshot', "`"$Out`"", '--size', $Size, '--scale', "$Scale")
if ($NoArt) { $arguments += '--no-art' }
if ($Only) { $arguments += @('--only', $Only) }
if ($FireTime -ge 0) { $arguments += @('--fire-time', $FireTime.ToString([Globalization.CultureInfo]::InvariantCulture)) }
$log = Join-Path $bin 'shots.txt'
$p = Start-Process -FilePath $exe -ArgumentList $arguments -NoNewWindow -Wait -PassThru -RedirectStandardOutput $log -RedirectStandardError (Join-Path $bin 'shots.err.txt')
Get-Content $log | Select-Object -Last 3
Get-Content (Join-Path $bin 'shots.err.txt') -ErrorAction SilentlyContinue
if ($p.ExitCode -ne 0) { throw "The screenshot run failed (exit code $($p.ExitCode))." }
