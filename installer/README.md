# The installer

`ChiliConCarnagePC-Setup-<version>.exe` installs Chili Con Carnage PC for the current Windows user, with no
administrator rights, from the player's own `.iso` of the game. It is one program: the installer when a
payload follows it in its file, the uninstaller (`Uninstall.exe` in the game's folder) when none does, and
the tool that makes the installer (`--pack`).

It is written in C# for .NET Framework 4.8 (part of Windows 10 1903 and later and of Windows 11), drawn with
WPF in the launcher's look, and compiled by the same C# compiler as the launcher. It shares three of the
launcher's source files unchanged (`profiles/chili/launcher/src/Theme.cs`, `Pads.cs`, `Native.cs`);
`src/Shims.cs` stands in for the few launcher types they use.

## What it does

| Page | Shows | Does |
|---|---|---|
| Welcome | the disk space needed, the Windows versions | |
| Your disc | a drop area, the path, Browse…, Browse folder… | checks the `.iso` (or an unpacked disc folder) exactly as the game does: disc id `ULUS-10216`, `PSP_GAME/SYSDIR/BOOT.BIN` of 8,616,992 bytes with FNV-1a 64 `a18a6d5249286c4a`, `assets00.naz`, `configpsp.txt` and `module/psmf.prx` present; a `.cso` is refused. Once the disc is accepted, its own `ICON0.PNG` and `PIC1.PNG` are shown (read into memory only) and the music starts |
| Folder | the path, Browse…, the space needed and free | default `%LOCALAPPDATA%\Programs\Chili Con Carnage PC`; refuses a drive, a folder that is not empty, one that cannot be written, too little space, a folder the game or the launcher runs from. A folder with an earlier installation (or what an uninstall kept) is an update |
| Options | HD loading pictures (on), Desktop shortcut (on), Start menu shortcut (on), Open the launcher when done (on) | |
| Install | the progress, the file being written | everything goes into a hidden `.setup-stage` folder first, then is moved into place; Cancel before the move leaves an earlier installation as it was and removes a new folder |
| Done | the folder, the first keys (F11, F1, Esc) | Open launcher starts the launcher (Finish when that option is off); on a failure, the reason and Show log |

The uninstaller has one page: Remove, with "Delete saves, settings and mods too" off. With it on, a second
question comes; a disc image in the folder is never deleted.

An update replaces the programs, DLLs, documents and tools and keeps `ChiliNative.ini`, `launcher.ini`,
`saves\`, `logs\`, `art\`, `textures\`, `texture_dump\` and every mod folder of the player's. A player's saves
are never read, listed, copied or deleted, except by the uninstaller when the player asked for it twice.

Written outside the game's folder: the shortcuts (Start menu folder `Chili Con Carnage PC`, the desktop), the
entry in Windows' installed apps (`HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\ChiliConCarnagePC`),
and what Setup remembers for its next run (`HKCU\Software\ChiliConCarnagePC\Setup`: volume, mute, the last
folder and disc). The uninstaller removes all of them. The log is `%TEMP%\ChiliConCarnagePC-Setup.log`;
after an install it is copied to `logs\setup.log` in the game's folder.

## The music

When the disc is accepted, Setup runs the music tool of its payload (`setup/ChiliSoundExport.exe`, built
from `profiles/chili/tools/sound_export.cpp`, with three FFmpeg DLLs) on the player's disc. The tool writes
the game's title and main menu theme as a WAV into Setup's temp folder; Setup reads it into memory, deletes
the file at once and plays it through `waveOut`, looped at the tool's loop points (the whole track, end to
start, as the game loops it), fading in over 2.5 s. Volume and mute are remembered. Any failure is silence
and one line in the log. Setup carries no audio of the game.

## Command line

```
Setup.exe --silent --iso <file or folder> [--dir <folder>] [--no-hd-loading] [--no-desktop] [--no-start-menu] [--launch] [--log <file>]
Uninstall.exe [--silent] [--remove-saves]
```

Exit codes: 0 done; 1 cancelled; 2 bad arguments; 3 the disc was refused; 4 the folder is not allowed or
cannot be written; 5 not enough free space; 6 the installer's data is damaged; 7 the game or the launcher
runs from the folder; 8 a file could not be written; 9 another error.

For building and testing: `--test [--iso <file>]`, `--screenshot <folder> [--scale 2] [--iso <file>]`,
`--selftest --iso <file> --home <folder>`, `--list`, `--pack ...`, `--home <folder>` (a stand-in for the
user's folders and registry keys; the Apps entry is then `ChiliConCarnagePC-Test`).

## Building

```
powershell -NoProfile -ExecutionPolicy Bypass -File installer\build.ps1 -Test
powershell -NoProfile -ExecutionPolicy Bypass -File installer\pack.ps1 -Package <folder made by scripts\package.ps1> `
    -HdLoading <the hd_loading pack> -SetupFiles <folder of ChiliSoundExport.exe, avcodec-61.dll, avutil-59.dll, swresample-5.dll>
```

`build.ps1` makes `bin\Setup.stub.exe` (and `bin\tools\MakeIcon.exe`; without
`profiles\chili\resources\app_icon.ico` it draws the icon into `bin\icon`). `pack.ps1` refuses a staging
folder that holds saves, art, textures, logs, another pack, a file of the game, a live `[Disc] Path` or a
`[GameProbe]` section, and leaves out `WHAT_CHANGED.txt` and shortcuts.

## The payload

`Setup.exe` = the program, then one blob per file (compressed whole with LZMS through the Windows
Compression API in `cabinet.dll`, or stored), then a JSON manifest with every file's path, part and sha256
and every blob's sha256, then a 72-byte trailer: `CHILISETUP1\0`, the payload's start, the manifest's offset
and length, the manifest's sha256. Every blob and every file is checked as it is read. Parts: `program`
(installed), `hd_loading` (installed under `mods\hd_loading` when chosen), `template` (the default
`ChiliNative.ini` and `launcher.ini`, written only when the folder has none), `setup` (the music tool, never
installed).

## Tests

- `build.ps1 -Test [-Iso <your .iso>]`: the payload round trip and damage, the disc check on made-up discs
  (and on the player's, read only), the folder rules, the settings file edit, an install, an update and both
  uninstalls into a temp home, the music loop and fade, every text of every page.
- `test\e2e.ps1 -Setup <Setup.exe> -Iso <your .iso> -Work <folder>`: the real installer, silent, end to end.
- `--selftest`: the real window, off screen and silent, driven by made-up keys and a made-up controller.
- `test\check_no_game_data.ps1 -Path <folder or Setup.exe>`: nothing of the game or of a player.
- `--screenshot` and `test\sheet.ps1`: every page as a picture.

## Fonts

`fonts\`: Changa One Italic, Saira Italic (variable) and Tektur (variable), each under the SIL Open Font
License 1.1 (`fonts\OFL.txt`, also installed as `licenses\OFL-fonts.txt`). They are embedded in Setup and
used only by it; Segoe UI stands in when a face cannot be loaded.
