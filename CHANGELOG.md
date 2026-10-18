# Changelog

## 0.9.2

- **HUD layout.** Classic (the game's own) or Modern, under Settings > Graphics, right after HUD size.
  Modern puts health at the lower left, the weapon and the combo at the lower right and the objective small
  at the upper left, so the middle of the screen stays clear. It follows HUD size.
- **HUD size fixes.** A long health bar stays in one piece, the sun's lens flare keeps its size, and the lines
  of one subtitle stay together.

Known limits of this version:

- Some HUD pieces have not been seen in the Modern layout yet: the boss bar, the overheat bar, the countdown,
  and the driving and helicopter HUD.

## 0.9.1

- **Sound.** If the music, effects and voices all go silent in a mission, the sound comes back by itself after
  two seconds. A sound device that stops taking sound is opened again after one second.
- **Running.** W with A or D runs diagonally, as a stick does. W alone still sprints.
- **Mouse aim.** The mouse aims mounted guns (turrets) up, down, left and right, as on foot, and aims while
  hanging from Marco's helicopter chain.
- **HUD size.** 100, 90, 80 or 70 % (Settings > Graphics).
- **Camera distance.** Original, Far or Farther (Settings > Controls), from the next start.
- **Field of view.** Original, Total Overdose or Wide (Settings > Controls; "View angle" inside the game).

All new settings start at the game's own values.

Known limits of this version:

- The mouse on a mounted gun was tried on the truck gun of Story 3-3; the meat hooks of Story 3-2 were not
  reached in a test.
- Whether shots land exactly on the aim dot with a wider field of view has not been checked.

## 0.9.0

The first public version.

- **Installer.** One Setup program for Windows 10 and 11, no administrator rights. It checks your `.iso`,
  installs per user, offers the HD loading pictures (the 27 mission and El Macho loading screens at
  2048x1088), makes the shortcuts and an entry in Windows' Apps list. An install over an earlier one keeps
  saves, settings and mods.
- **Picture.** Direct3D 12 at up to 3840x2176 inside, chosen by the monitor; FXAA or MSAA, anisotropic
  filtering, optional ambient occlusion; borderless full screen with F11.
- **Glow.** The game's full-screen glow is drawn at the PSP's own size and enlarged smoothly, so it no
  longer turns into squares and ghost edges at high resolutions (`[Rendering] Glow`).
- **Frame rate.** Missions run at 60 frames per second; 30, as on the PSP, is one setting away.
- **Settings inside the game.** A `Settings` line in the main menu and the pause menu opens a page in the
  game's own look, worked with keys, mouse or controller.
- **Launcher.** Every setting on seven pages, usable with a controller alone; rebinding of every key, mouse
  button and controller button; mods on and off and in order.
- **Controls.** The default keys and mouse are those Total Overdose (2005, the same engine) had on PC: W A S
  D, mouse look and aim, left button shoot, right button headshot, Space shoot-dodge, Left Shift jump, F use,
  R reload, Left Ctrl the loco move, Tab rewind, the wheel for weapons, Escape and P pause. "Modern" and
  "PSP original" are one choice away in the launcher and on the settings page inside the game.
- **Controllers.** Xbox-style controllers and PlayStation controllers (DualShock 4, DualSense) without a
  helper program; the one touched last is used. The buttons the game shows on screen follow the device you
  used last.
- **Mouse and aim.** The mouse turns the player by exactly the angle moved, in the frame it is read, also in
  dives; a tighter camera; each picture shown as soon as it is drawn.
- **Sound.** Steady sound without gaps while the game is busy; the voice lines play correctly.
- **Saves.** A save is written as a whole or not at all. The automatic save works over an existing save, and
  the save it replaces is kept in `SAVEDATA_PREVIOUS`. "Delete save games" moves the save there instead of
  erasing it.
- **Texture packs.** Every picture of a pack is decoded when the game starts, the pictures of the first
  screens first, so replaced textures show from their first frame.

Known limits of this version:

- No real controller has been tried yet; controllers were tested with made-up input only.
- Nobody has yet checked by eye that nothing in the game runs at double speed at 60 frames per second; if
  something does, choose 30 fps.
- Only the first story mission and the menus have been tested in depth.
- The sound of the movies comes about a tenth of a second after the picture.
