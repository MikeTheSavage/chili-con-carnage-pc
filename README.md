# Chili Con Carnage PC

The PSP game Chili Con Carnage (USA release, ULUS-10216) as a native Windows program, made by static
recompilation with [PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp).

You need your own copy of the game as an `.iso` file. This repository and the installer contain no game data.

## What it is

- The game's own executable, translated to C++ ahead of time and compiled for 64-bit Windows.
- A host that stands in for the PSP's firmware: files, sound, movies, controls, saves.
- A Direct3D 12 renderer: internal resolution up to 3840x2176, 60 frames per second in missions.
- Keyboard and mouse, Xbox-style controllers and PlayStation controllers (DualShock 4, DualSense), all
  remappable.
- Mods and texture packs.
- A launcher with every setting, usable with a mouse, a keyboard or a controller.
- Saves in the program's own folder, with the previous save kept beside each one.

## What it is not

- Not an emulator: no PSP code is interpreted while you play.
- Not a decompilation or a modified copy of the PSP game.
- Not the game: you need your own copy of the USA release.
- Not made, endorsed or supported by the game's rights holders.

## Features

- Picture: internal resolution up to 8x the PSP's (automatic by monitor), FXAA or MSAA, anisotropic
  filtering, optional ambient occlusion, a smooth version of the game's full-screen glow, borderless full
  screen (F11).
- Controls: two-stick controls with mouse look and exact mouse aim, or the PSP's own layout; on-screen button
  prompts that follow the device you used last (keys, Xbox buttons or PlayStation shapes).
- Settings inside the game's own menus (main menu and pause menu), and in the launcher.
- Texture packs: a folder of PNG files replaces the game's textures; the pictures the game shows first are
  loaded first.
- Saves: written as a whole or not at all; the save each new one replaces is kept.

## Requirements

- Windows 10 (version 1903 or later) or Windows 11, 64-bit.
- A graphics card with Direct3D 12.
- .NET Framework 4.8 (part of those Windows versions) for the launcher and the installer.
- About 300 MB of disk space (about 120 MB without the HD loading pictures).
- Your own copy of Chili Con Carnage (USA, ULUS-10216) as a plain `.iso`, unmodified. A compressed `.cso`
  must be converted to `.iso` first.

## Installing with Setup

1. Download `ChiliConCarnagePC-Setup-<version>.exe` from the Releases page.
2. Run it. It needs no administrator rights.
3. Choose your `.iso`. Setup checks it and refuses anything but the USA release.
4. Choose the folder (default `%LOCALAPPDATA%\Programs\Chili Con Carnage PC`) and the options: HD loading
   pictures, desktop shortcut, Start menu shortcut.
5. Play.

Once your `.iso` is accepted, Setup plays a track of the game's own music, read from the disc; Setup itself
contains no audio of the game. An install over an earlier
one keeps your saves, settings and mods. Uninstall from Windows' Apps list; your saves stay unless you tick
the box to delete them.

Unattended install:

```
ChiliConCarnagePC-Setup-<version>.exe --silent --iso "D:\Games\Chili Con Carnage (USA).iso"
```

## Your own copy of the game

- Only the USA release works (disc id ULUS-10216). Setup, the launcher and the game check the disc id and
  the game executable's size and hash.
- The `.iso` is read where it is; nothing of it is copied. Its path is written to `ChiliNative.ini`
  (`[Disc] Path`).
- If you move the `.iso` later, the launcher asks for it again ("Choose your disc").
- The folder of an unpacked disc (the one that holds `PSP_GAME` and `UMD_DATA.BIN`) works too.

## Troubleshooting

- **"Windows protected your PC"**: the programs are not signed. Compare the file's SHA-256 with
  `SHA256SUMS.txt` on the Releases page, then choose More info and Run anyway.
- **"This is not Chili Con Carnage (USA)"** or **"a different game program"**: the disc is another release or
  a modified copy.
- **The game stops at once**: the launcher's Open logs button, or `logs\ChiliConCarnage.log` in the game
  folder.
- **Crackling sound**: raise `[Audio] BufferMs` in `ChiliNative.ini` (default 70, try 120), or choose 30 fps
  (launcher, Display, Frame rate).
- **"software renderer" in the title bar**: Direct3D 12 did not start; update the graphics driver.
- **"VCRUNTIME140.dll was not found"**: reinstall, or install the Microsoft Visual C++ Redistributable (x64).
- **Where things are**: everything is in the game folder: `ChiliNative.ini` (settings), `saves\`,
  `mods\`, `logs\`.

## Building from source

Short form; [BUILDING.md](BUILDING.md) has the details.

1. Visual Studio 2026 with the C++ x64 tools (CMake and Ninja come with it), Python 3.9 or later.
2. `profiles\chili\scripts\analyze.ps1 -Iso <your .iso>` extracts your disc into `profiles\chili\game\disc0`
   and generates the C++ of the game's code into `profiles\chili\analysis\generated`. Both folders are
   git-ignored and must never be published.
3. `profiles\chili\scripts\build.ps1 -Config rel` builds the program (about an hour the first time).
4. `profiles\chili\scripts\package.ps1` assembles a game folder with the launcher.

## Modding and texture packs

A mod is a folder in `mods\`. A texture pack is a mod with a `textures\` folder of `<key>.png` files.
"Dump textures.cmd" in the game folder writes every texture of your own disc as a PNG, named by its key.
See [MODDING.md](MODDING.md).

## Controls

| Action | Keyboard and mouse | Xbox-style | PlayStation |
|---|---|---|---|
| Move | W A S D | Left stick | Left stick |
| Look | Mouse | Right stick | Right stick |
| Shoot | Left mouse button | RT | R2 |
| Headshot mode | Right mouse button | LT | L2 |
| Jump, dive, use | Space | A | Cross |
| Reload | R | X | Square |
| Weapon wheel, back | F | B | Circle |
| Pause | Escape | Start | Options |
| Free the mouse | F1 | | |
| Full screen | F11 | | |

All of them in [CONTROLS.md](CONTROLS.md).

## Credits

- [PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp), the static recompiler this is built on (MIT).
- [FFmpeg](https://ffmpeg.org) for ATRAC3plus sound and the movies (LGPL 2.1 or later).
- The Microsoft Visual C++ runtime.
- The typefaces used by the installer, under the SIL Open Font License 1.1.
- Chili Con Carnage was made by Deadline Games and published by Eidos Interactive.

## Licence

MIT: see [LICENSE](LICENSE). Third-party components: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Legal note

This repository contains no game data: no code generated from the game's executable, no art, no sound, no
text of the game. The prebuilt program on the Releases page contains the game's code translated to run on a
PC; it is offered to people who own the game and does nothing without their own copy. Chili Con Carnage, its
names, art and music belong to their rights holders. This project is not affiliated with them.
