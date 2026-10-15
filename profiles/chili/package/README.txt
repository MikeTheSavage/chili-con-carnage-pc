CHILI CON CARNAGE - PC BUILD @VERSION@
==========================================

WHAT THIS IS

  The PSP game Chili Con Carnage (USA release, ULUS-10216) as a Windows program: the game's own code
  translated for the PC, a Direct3D 12 picture at up to 3840x2176, 60 frames per second, keyboard,
  mouse and gamepad controls, mods, and a launcher with every setting.


YOU NEED YOUR OWN COPY OF THE GAME

  This folder holds no game data. Setup asks for your disc when it installs the game. Without
  Setup, or when the disc has moved, the launcher's first page is "Choose your disc", with two
  buttons:

    - "Choose .iso..." for the .iso file made from your own UMD, or
    - "Choose folder..." for the folder of an unpacked disc (the one that holds PSP_GAME and
      UMD_DATA.BIN).

  The disc is read where it is and nothing is copied. Its place is written into ChiliNative.ini
  ([Disc] Path), so you choose only once; the Game data page shows it and can change it. Only the
  USA release works. An .iso placed in this folder is found without asking. Started without the
  launcher, the game asks the same question in a small window of its own.

  Chili Con Carnage belongs to its rights holders; this build is not made or endorsed by them.
  The program contains the game's code, translated to run on a PC, for people who own the game.
  The folders art\, textures\ and saves\ hold what comes from your own copy: do not share them.


START

  Double-click ChiliConCarnageLauncher.exe. The launcher opens: seven pages with every setting, and
  PLAY at the bottom right. PLAY saves the settings and starts the game. The launcher steps aside
  while you play and comes back when you quit the game (close the game's window, or Alt+F4).

  The launcher works with the mouse, the keyboard and a controller. With a controller: stick or
  D-pad to move, A / Cross to choose, B / Circle to go back, LB and RB (L1, R1) for the next page,
  Start / Options to play. The strip along the bottom shows what each button does.

  "Skip this window next time" (Play page) starts the game at once from then on. To see the launcher
  again, open the shortcut "Chili Con Carnage settings" in this folder, or hold Shift while you
  start the launcher.

  ChiliConCarnage.exe is the game itself. It can be started alone and uses the same settings.

  Windows 10 (version 1903 or later) or 11, 64-bit, and a graphics card with Direct3D 12.
  When the game asks for a profile, a Windows dialog lists the save slots.


SETTINGS INSIDE THE GAME

  The game's own menus have a line "Settings":

    - Main menu: the last line, where the PSP game had "Fiesta". (Fiesta was the PSP's wireless
      mode, which this program does not have.)
    - Pause menu (Escape or Start in a mission): the last line, under "Chicken out".

  It opens a page that looks like the game's menus, with five sections: Display, Graphics,
  Controls (with Keyboard & mouse and Gamepad under it), Textures & mods, Sound.

                              Keys              Controller          Mouse
  Move                        Up, Down          D-pad or stick      point at a row; the wheel
  Open, switch, next value    Space, Enter      A / Cross           left click
  Change a value              Left, Right       D-pad left, right   click; hold and move a number
  Back                        Backspace, F      B / Circle          right click
  Close the page              Escape            Start / Options

  The buttons shown at the bottom are those of whatever you touched last. The last row of each
  section is "Defaults"; it asks before it resets. Every change is saved at once.

  At once, also in a paused mission: "Screen" (window or full screen, as F11) and everything
  under Controls for the mouse, the keys and the controller (turn and look speed, invert, dead
  zones, rumble and the rest), "Prompts" and "Button pictures".

  From the next start of the game: the rest of Display, Graphics, "Control style", "Camera",
  Textures & mods and Sound. The value is saved the moment you change it, turns orange with a
  chili after it until it runs, and the page says "After a restart." for three seconds. In a
  mission these rows are dimmed with a padlock, and say why only when you try one.

  Still only in the launcher: which key or button does what, the list of mods and their order,
  the disc, and the rows under "Advanced".

  "Sync Highscores" in Profile Options is dark. Like Fiesta it started the PSP's wireless play.
  The game has a "Settings" of its own as well: Profile Options > Settings (Music, Invert
  Y-axis, Button Layout, Subtitles). That is the game's own screen and is as it was.

  To switch the lines and the page off, should one of them ever misbehave: in ChiliNative.ini,
  under [Game], write
      SettingsInGame=false
  The menus are then as the PSP game has them ("Fiesta" is back and does nothing), and every
  setting is still in the launcher.

SAVES

  The game saves by itself, after a mission and when you leave Profile Options after changing
  something. It writes over your profile's save and asks nothing, as the PSP game does. "Save
  profile" in Profile Options lists the slots. A slot that already holds a save asks "Overwrite
  this existing save?" first, and Back is the answer it starts on.

  One earlier save is kept. Each time a save is written over, the save that was there is moved to
      saves\PSP\SAVEDATA_PREVIOUS\ULUS10216DATA00
  in place of the one kept there before. DATA00 is the first slot, and each slot has its own
  folder there. To go back one save, quit the game, then copy that folder over
      saves\PSP\SAVEDATA\ULUS10216DATA00
  and let Windows replace the files. The log, logs\ChiliConCarnage.log, names the folder in the
  line of every save. It reads "[savedata] native profile committed slot=DATA00 over the save
  that was there; the save before it is kept in ...".

  "Delete save games" in Profile Options lists the saves. The one you choose and confirm is not
  erased. Its folder is moved to the same SAVEDATA_PREVIOUS place, so it can be copied back the
  same way.

  Keep your own copies of a save outside saves\PSP\SAVEDATA. A slot folder with other files or
  folders in it may refuse to load or to save.


KEYS

  Move and strafe             W A S D   (hold X to walk)
  Turn and look               mouse     (J L I K without a mouse)
  Shoot                       left mouse button, G
  Headshot mode               right mouse button, V
  Jump, dive, use, confirm    Space, E, Enter
  Reload                      R
  Weapon wheel                F, Backspace, middle mouse button
  Target objects, exit stunt  Q
  Previous / next weapon      mouse wheel, 1 / 2, Left / Right
  Rewind                      Z, Down
  Pause                       Escape
  Free or capture the mouse   F1   (Alt+Tab frees it too)
  Full screen and back        F11, Alt+Enter

  The first click in the window captures the mouse for looking. In menus the arrow keys move, Enter
  or a left click confirms, Backspace, F or a right click goes back.


CONTROLLERS

  An Xbox-style controller works without setup. So does a PlayStation controller (DualShock 4 or
  DualSense, by cable or Bluetooth); it needs no helper program such as DS4Windows.

                              Xbox-style         PlayStation
  Move, look                  left, right stick  left, right stick
  Shoot                       RT                 R2
  Headshot mode               LT                 L2
  Jump, dive, use, confirm    A (also Y)         Cross (also Triangle)
  Reload                      X                  Square
  Weapon wheel; back in menus B                  Circle
  Target objects, exit stunt  left stick click   L3
  Previous / next weapon      LB / RB            L1 / R1
  Rewind                      D-pad down         D-pad down
  Pause                       Start              Options

  With two controllers connected, the one you touched last is used. A controller can be plugged in
  or taken out while the game runs.

  The buttons the game shows on screen (in the menus and the tutorial lines) are those of whatever
  you touched last: key caps and a mouse, Xbox buttons, or PlayStation shapes. They change about a
  quarter of a second after you switch.

  No real controller has been tried with this build yet. If yours does not work, CONTROLS.md ("Gamepads") says how to make a log that shows why.
  Vibration: in the first mission the game never asked for any, so nothing vibrates there. If a
  later mission asks, an Xbox-style controller and a PlayStation controller on a cable get it; a
  PlayStation controller over Bluetooth does not.

  Every key, mouse button and controller button can be given another job on the launcher's Controls
  page (two tabs: Keyboard & mouse, Gamepad): click a chip and press what you want there; a right
  click on a chip takes it away. CONTROLS.md lists every binding and setting as ChiliNative.ini
  spells them.


WHERE THINGS ARE (all in this folder)

  ChiliNative.ini   the game's settings: what the launcher's pages and the game's "Settings" show
                    and write. Delete it, with the game and the launcher closed, to get the
                    defaults back (you are then asked for the disc again).
  launcher.ini      the launcher's own switches ("Skip this window next time", the order of mods
                    that are switched off). Delete it to get the defaults back.
  saves\            your saved games (see SAVES above): PSP\SAVEDATA holds the profiles,
                    PSP\SAVEDATA_PREVIOUS the one earlier save of each. They are in this program's
                    own format: a save from a PSP or from an emulator is listed but cannot be
                    loaded.
  art\              pictures from your own copy of the game that the launcher shows (its logo, its
                    background, a load-screen picture). Do not share them.
  mods\             mods; mods\README.md explains how they work.
  logs\             ChiliConCarnage.log is the last run, ChiliConCarnage.previous.log the one before.
  tools\            Python tools for making mods (see mods\README.md and mods\TEXTURES.md).
  textures\         made by "Dump textures.cmd": every texture of the game as a PNG (needs Python 3.9
                    or later; about half a minute, 20 MB). Once it is there the launcher shows the
                    game's own logo and a load-screen picture, taken from your disc, in place of
                    its drawn title.

  Installed with Setup, the game is removed again from Windows' Apps list; your saves stay unless
  you tick the box to delete them. Without Setup the whole folder can be moved or copied to another
  PC; nothing is written outside it. The shortcut "Chili Con Carnage settings" remembers where the folder was: after a
  move or a copy, delete it and hold Shift while starting the launcher instead, or make a new
  shortcut to ChiliConCarnageLauncher.exe with --settings after the program's name.


FRAME RATE

  Missions run at 60 frames per second (the PSP ran them at 30). If the sound crackles or the game
  runs slow on your PC, choose "30 fps" in the launcher (Display, Frame rate) and press PLAY: that
  is 30, as on the PSP. By hand it is the line FrameLock=2 in the section [Game] of ChiliNative.ini.
  Nobody has yet checked by eye that nothing in the game runs at double speed at 60; if something
  does, 30 fps is the way back.


PICTURE SETTINGS (the launcher's Display and Graphics pages; here as ChiliNative.ini spells them,
section [Rendering]; a line starting with ";" is switched off)

  Preset=high            how much the picture may cost: high, medium, low (integrated graphics)
  InternalScale=auto     picture resolution: auto fits your monitor, or 1 to 8 times 480x272
  AntiAliasing=fxaa      edge smoothing: fxaa, off, msaa2, msaa4, msaa8
  Anisotropy=16          sharper ground and walls at a slant: 16, 8, 4, 2, 1 (the game's own)
  TextureFilter=game     game, linear, nearest (blocky)
  SSAO=off               soft shadow in corners and where things meet the ground: off, low, high
  Fullscreen=true        start in full screen (F11 switches while playing)
  GeThread=true          drawing on a second processor core; false if the picture flickers
  EarlyPresent=true      show each picture as soon as it is drawn

  RENDERING.md describes each one and what it costs.


OTHER SETTINGS (the launcher's Controls, Sound and Textures & mods pages)

  [Input.Mouse] SensitivityX / SensitivityY   mouse speed (degrees per mouse count)
  [Input] Prompts=auto              the buttons shown on screen: auto (what you touched last),
                                    keyboard, gamepad, psp (the PSP's own pictures)
  [Input.Gamepad] Glyphs=auto       how a controller's buttons are drawn: auto, xbox, playstation
                                    (playstation if a PlayStation controller shows Xbox buttons)
  [Input.Gamepad] Rumble=1          vibration strength, 0 (off) to 1
  [Input.Gamepad] PlayStationPads=true   false: only Xbox-style controllers are read
  [Input] ExactTurn=false           the game's own turning for the stick and the look keys
  [Game] Camera=original            the game's own, slower camera
  [Audio] BufferMs=120              more sound buffer if the sound crackles (default 70)
  [Mods] DumpTextures=true          writes every texture the game draws to texture_dump\ as a PNG;
                                    a PNG at mods\<name>\textures\<key>.png replaces one
                                    (mods\TEXTURES.md)


IF SOMETHING GOES WRONG

  The launcher says in one red line what is wrong; the button beside it ("Open logs", "Details")
  shows the rest. "Game not found": ChiliConCarnage.exe has to be in the same folder as the
  launcher. The launcher itself needs the .NET Framework 4.8, which is part of Windows 10 (1903 and
  later) and Windows 11.
  Started alone, the game says in a message what stopped it and where the log is
  (logs\ChiliConCarnage.log).
  "is another disc" or "a different game program": the disc is not an unmodified copy of the USA
  release. A compressed image (.cso) has to be converted to a plain .iso first.
  A crawling game whose title bar says "software renderer": Direct3D 12 did not start; update the
  graphics driver.
  "VCRUNTIME140.dll was not found" (only if the runtime files beside the program were removed):
  install the Microsoft Visual C++ Redistributable (x64).


LICENCES

  This program is free software under the MIT licence: licenses\LICENSE.txt. It is built on
  PSPRecomp (MIT). licenses\THIRD_PARTY_NOTICES.md lists every other component.

  The video and sound decoding uses FFmpeg 7.1 (avcodec, avformat, avutil, swresample, swscale)
  under the GNU Lesser General Public License 2.1 or later; its text is in
  licenses\COPYING.FFmpeg.LGPLv2.1.txt. The FFmpeg source code for these DLLs (ffmpeg-7.1.tar.xz)
  is attached to each release of this program, beside the download, and is also at
  https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz. The five DLLs beside the program can be
  replaced by other builds of the same versions (avcodec-61, avformat-61, avutil-59, swresample-5,
  swscale-8).
@RUNTIME_NOTE@
