# Changelog

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
