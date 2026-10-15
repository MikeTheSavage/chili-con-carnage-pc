# The launcher

The program a player starts first: every setting of `ChiliNative.ini` on seven pages, and a PLAY button that
starts the game. One executable for .NET Framework 4.8, which is part of Windows 10 and 11, so nothing has to
be installed. The look and the wording are specified in `DESIGN.md`.

## Build

```
powershell -NoProfile -ExecutionPolicy Bypass -File profiles\chili\launcher\build.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File profiles\chili\launcher\build.ps1 -Out <game folder>
powershell -NoProfile -ExecutionPolicy Bypass -File profiles\chili\launcher\build.ps1 -Test
```

It compiles `src\*.cs` with the C# compiler that is on the machine (Visual Studio's Roslyn `csc.exe`, else the
.NET SDK's) against the .NET Framework 4.8 reference assemblies. No project file, no packages, nothing is
downloaded. The result is `bin\ChiliConCarnageLauncher.exe`, or the same file in `-Out`. `-Test` also builds
the stand-in game and runs the tests that need no window (the settings file, controllers, the icon).

The program's icon is the game program's, `profiles\chili\resources\app_icon.ico` (an original picture, drawn by
code). It is read when the launcher is built: from this checkout, else from the repository's main checkout;
`-Icon <file>` names another. `-Test` checks that the program carries that file byte for byte.

## Where it goes

Beside the game: the launcher reads and writes `ChiliNative.ini` in its own folder and starts
`ChiliConCarnage.exe` there (`build.ps1 -Out ..\dist\ChiliConCarnagePC` puts it into the package folder).
Everything else it needs is in the game's folder already: `mods\`, `saves\`, `logs\`.

`launcher.ini` beside the launcher (written on the first save, with comments) names another game program and
holds the launcher's own switches:

| Key (`[Launcher]`) | Meaning |
|---|---|
| `Game` | The game program, beside the launcher unless a full path is given. Default `ChiliConCarnage.exe`, then `chili_recomp.exe`, `bin\chili_recomp.exe` |
| `StartGameDirectly` | `true`: start the game at once next time, without the window (the switch on the Play page). Holding Shift while starting, or `--settings`, shows the window |
| `AfterGame` | `return` (default: the window comes back when the game ends) or `close` |
| `IniFolder` | The folder of `ChiliNative.ini` when it is not the launcher's. The game is then told with `CHILI_CONFIG` |
| `Arguments` | Arguments for a developer's build, with `{boot}`, `{disc}`, `{saves}`, `{ini}` filled in. With arguments the game's output is kept in `last_run.log` and `last_run.err.log` in the log folder |
| `Disc`, `Saves`, `Logs` | The disc for `{disc}` and `{boot}` when `ChiliNative.ini` names none, the save folder, the log folder |

With a developer's build from the build tree (a build that takes the disc and the save folder as
arguments):

```ini
[Launcher]
Game=<repo>\out\chili-rel\bin\chili_recomp.exe
IniFolder=<repo>\out\chili-rel\bin
Arguments="{boot}" "{disc}" --ms0 "{saves}"
Disc=<repo>\profiles\chili\game\disc0
Saves=<repo>\profiles\chili\game\ms0_play
Logs=<repo>\out\chili-rel
```

## Command line

| | |
|---|---|
| (nothing) | The window; or the game at once when the player asked for that |
| `--settings` | Show the window even then |
| `--ini <folder or file>` | Where `ChiliNative.ini` is |
| `--page <name>` | Open on a page (`Display`, `"Textures & mods"`, ...) |
| `--no-art` | Ignore the art folder |
| `--base <folder>` | Act as if the launcher were in that folder (tests) |
| `--screenshot <folder> [--size 1280x720] [--scale 1] [--only <start of a name>] [--fire-time <seconds>]` | Render every page and state to PNG files, off screen, and exit; `texts.txt` beside them lists every text each picture draws. `--only 21` renders the pictures whose names start with 21; `--fire-time` is the moment of the HD pack choice's fire that the pictures show (it stands still in them). `--size` is the window in units (what the layout sees) and `--scale` the monitor's scale, so the pictures are size times scale pixels: `--size 1280x720 --scale 2` gives 2560x1440 pictures of the 1280x720 layout as a 200 % monitor shows it; `--size 2560x1440 --scale 2` would be a 2560x1440 window on such a monitor, 5120x2880 pixels |
| `--test-ini [file]` | Run the tests that need no window (settings reader and writer, disc check, controller reports, the icon) and exit (exit code 0 when all pass) |
| `--selftest <folder>` | Drive the real window through keys, clicks, made-up controllers and PLAY; writes `selftest.txt` and pictures |
| `--pads` | List the controllers as the launcher sees them (XInput slots, the HID devices of Raw Input and what each is taken for) and exit |
| `--measure <file> [--seconds 10] [--page <name>] [--back] [--stick \| --stick-out-of-view] [--whole]` | The live window, not activated, left alone for some seconds; the processor time it used and how often it drew are appended to the file. `--back`: as when another window is in front. `--stick`: the Gamepad tab with the stick dials in view and a made-up controller whose sticks never rest, as in a hand (`--stick-out-of-view`: the dials scrolled away); `--whole` with it: every sample draws the whole picture, as before the dots had a visual of their own. On the made-up game folder only |
| `--every-value <folder> [--disc <.iso or folder>]...` | Every save of the run is kept too (`transitions.tsv`: the changes in the order made; `transitions\`: the file before and after), for the check that the game's own writer makes the same bytes (`tools\ini_document_test.cpp`). For the check that launcher and game agree (`scripts\check_launcher_settings.ps1`): off screen, every row of every page is put to each of its values through the row's own control, the settings are saved as PLAY saves them, and the file written each time is kept in the folder as `001.ini`, `002.ini`, ... with `list.tsv` (what each one is), `rows.tsv` (the rows there are) and `skipped.txt`. The folder holds the `ChiliNative.ini` to start from and, for the mod rows, a `mods` folder; use with `--base` (`src\EveryValue.cs`) |

One environment variable, for the scripted check of a packaged start (`scripts\check_package_start.ps1`):
`CHILI_LAUNCHER_CHECK=1`. The launcher takes every switch of a scripted test run out of the environment of the
game it starts (a player's start has none of them, and the settings file decides); with this variable it
leaves three in, `CHILI_UNATTENDED`, `CHILI_NULL_AUDIO` and `PSPRECOMP_PROBE_SECONDS`, so that the game opens
behind the other windows, makes no sound and ends by itself, and the launcher's own window, should it have to
appear, does not take the focus.

## What it does with ChiliNative.ini

- Reading tolerates anything. A value the file lacks is shown as the game's default.
- Saving happens on PLAY, on closing the window and on Ctrl+S, never on a click. Only the lines of keys that
  were changed are touched: a live line keeps its spelling, spacing and trailing comment; a line switched off
  with `;` becomes live; a key the file lacks is added at the end of its section. Comments, unknown sections and
  unknown keys stay byte for byte. Saving with nothing changed does not write the file, with one exception:
- `[Game] FrameLock` is written on the first save even when nothing was changed and the row shows the default
  (60 fps, `FrameLock=1`). Game builds differ in what they do without the line (30 or 60), so the launcher
  writes the one it shows; a line the file has is left as it is.
- The file is read again from disk before the changes are written, so a line the game added meanwhile stays.
- A setting the game beside the launcher does not know has no row, and nothing is written for it. The
  launcher finds that out by reading the game program once for the text its settings reader holds for the key
  (`Preset must be`, `AntiAliasing must be`, `[Disc]`, `Prompts must be`, ...; the list is `S` in
  `src\Model.cs`); the result is kept in `launcher.ini` until the program's size or date changes, and is used
  at once at the next start.

## What is written on the pages

No explanatory text: nothing stands under a setting's name, beside a heading or under a mod. The one thing
that may stand under a name is a problem the player has to act on, in red with the warning triangle. A row
that cannot be used now is dimmed and has a padlock after its name; why is said in the message line at the
bottom only when the player tries to use it (a click anywhere on the row, or A / Enter with the focus on it),
never for moving the focus. A dimmed choice of a row that is not locked says its own reason when it is tried
(a click on it, or A / Enter or Left / Right with no other choice to go to). Such a report lies over a
standing message (an error) and does not destroy it. An error is one short sentence in red with the warning
triangle and a button for the rest (`Open logs`, `Details`). Two confirmations are said for a few seconds
(`Settings saved.`, `Disc found: <path>`); switching on "Skip this window next time" says nothing (the
package's second shortcut, "Chili Con Carnage settings", starts the launcher with `--settings`). The versions
of the launcher and the game stand in the window's title bar. `PadTests.cs` holds a list of the sentences
that were taken out (49) and checks that the program has none of them; the self test reads every page from
top to end for a bracket, a sentence or a version line.

## Controllers

- Xbox-style controllers through XInput, PlayStation controllers (DualShock 4, DualSense; vendor 054C) through
  Raw Input: a `WM_INPUT` hook on the window, the report read by the system's HID parser (`hid.dll`), and a
  decoder by byte layout behind it for the long Bluetooth reports the descriptor does not describe
  (`src\Pads.cs`). A PlayStation pad is mapped by position onto the sixteen buttons an Xbox pad has (Cross = A,
  Circle = B, Square = X, Triangle = Y, L1 = LB ... Options = Start, Share / Create = Back), so bindings are
  stored under the same names (`[Input.Gamepad] A=Cross`) whichever pad pressed them. Devices whose path holds
  `IG_` are XInput's and are skipped, and so is a HID controller of Microsoft's (vendor 045E). Only the
  known products are taken (DualShock 4: 05C4, 09CC, the wireless adaptor 0BA0; DualSense: 0CE6, 0DF2), and
  a pad counts as connected once it has sent a report. One controller is listened to at a time, also of two
  PlayStation pads (each keeps its own state): the other takes over when it moves while the first has
  rested for a quarter of a second, so a pad that a helper program also presents as an Xbox pad does not
  press twice. Nothing held is carried across a game session.
- No real controller has been tried yet. The byte layouts are general knowledge of the
  two controllers and are tested only against hand-made reports; the path through `hid.dll` has not met a
  device. `--pads` shows what the launcher makes of the devices Windows reports.
- The pictures of the buttons (binding chips, the prompts along the bottom) are Xbox letters or PlayStation
  shapes: what "Button pictures" (`[Input.Gamepad] Glyphs`) says, and with Automatic the family of the
  controller in use. The prompts follow the device used last: controller pictures after a controller, key
  caps after the keyboard, nothing after the mouse. A prompt is there only for what the button does here.
- Rows for a game that has the keys: "On screen" (`[Input] Prompts` = auto, keyboard, gamepad, psp),
  "Button pictures" (`[Input.Gamepad] Glyphs` = auto, xbox, playstation), "Use PlayStation controllers"
  (`PlayStationPads` = true, false: the game's key; the launcher itself goes on reading PlayStation pads, so
  the row can be switched back on with one), "Rumble" (`Rumble` = 0 to 1, shown in percent) with "Test",
  which shakes a connected Xbox controller for a third of a second.

## Processor time

The picture is a stack of visuals (`Surface.cs`, "layers"), so that what moves by itself or with the
controller is drawn alone: the fire of the HD pack choice about sixteen times a second, the focus ring's
breathing after a controller, the two stick dots of the Gamepad tab, the waiting chip of a binding capture,
the message line while it fades. With the Textures page open, HD chosen and the launcher in front, ten
seconds cost 0.28 to 0.41 s of processor time (3 to 4 % of one core; before the split, when every frame of
the fire drew the whole window, 7.56 s). On the Gamepad tab with a stick that never rests: 0.28 to 0.39 s
with the dials in view (7.14 s when every stick sample drew the whole window, as it did), 0.09 s with the
dials scrolled away. On another page, or with another window in front, 0.05 s or less. `--measure` gives
these numbers.

## Pictures from the game

None is in the executable or in the repository. When `art\logo.png`, `art\background.png` or `art\hero.png` are
beside the launcher they replace the drawn title, tint the backdrop and fill the right of the Play page; so do
the same pictures in `textures\data\textures\menu\` once the player has run the package's `Dump textures.cmd`
on their own disc. `get_art.ps1 -Textures <extracted textures> [-To <launcher folder>] [-Mods <mods folder>]`
copies the three files during development (`bin\art`, git-ignored). `art\pack_<mod>.png` is shown in the mod
list for that pack.

The load screen is 512 x 272. On a monitor that would show it at more than about one and a half times its
size (the launcher at 1280 x 720 units on a 150 % or 200 % screen) it is put slightly out of focus and fades
sooner, so that it reads as a backdrop and not as a failed enlargement. A larger copy of the same picture is
shown at the same size, sharp: with `-Mods`, `get_art.ps1` takes the copy a texture pack holds
(`mods\<pack>\textures\<key>.png`, the key read from the extracted folder's `index.csv`) in place of the disc's.

## Tests and pictures

- `build.ps1 -Test`: 65 checks. The reader and writer on `test\sample_ChiliNative.ini` (the file the game
  wrote), made-up odd files, the disc check and the capability scan (32); PlayStation reports made by hand
  through the decoder and the position map, the rule for two controllers and for two PlayStation pads, the
  buttons' names and pictures, the list of sentences that must not be in the program, the icon (33).
- `shots.ps1 -Out <folder> [-Scale 2] [-Size 1280x720] [-NoArt] [-Game <exe>] [-Only 21] [-FireTime 2.5]`: a
  made-up game folder in `bin\fixture` (settings, five mods, a disc folder with only the id file, a log) and 88
  pictures of it, with `texts.txt` (every text each picture draws). `-Size` is in units and `-Scale` multiplies
  it (see `--screenshot` above): `-Scale 2` alone gives 2560x1440 pictures. With `-Game` the rows follow what
  that game build knows. `-Only` renders the pictures whose names start that way. Pictures 44 to 49 are drawn
  with a made-up PlayStation controller, 50 to 66 show the new rows, the prompts by device and the locked rows.
- `shots.ps1 -Out x -NoShots [-Dev]`, then `bin\ChiliConCarnageLauncher.exe --base bin\fixture --selftest <folder>`:
  the live window, not activated, driven through the same entry points as keyboard, mouse and the controllers
  (made-up XInput states, and hand-made PlayStation reports through the decoder), and PLAY with the stand-in
  game `test\FakeGame.cs` (clean exit, failing exit; `-Dev`: started with arguments). 172 checks in the
  player's layout, 174 in the developer's.
- With the real game, from `profiles\chili\scripts` (both need a game build, the second also your own disc):
  - `check_launcher_settings.ps1 -Work <new folder> [-Disc <iso>, <folder>] [-PlayerExe <packaged game in a
    copy>]`: the launcher writes a settings file for every row at every value (`--every-value`; 373 files
    with version 0.9.0: every choice and switch, seven places of every slider and each of its
    steps, every key the launcher names, every mouse input and controller button on every action, nothing
    bound, each page's Defaults, two disc paths), and the game is started on each for one frame. It fails on
    any line of the game's settings readers on stderr (`unknown ...`, `... must be ...`), on a file that
    lacks the line its row stands for, and where the game's log repeats another value than the launcher
    wrote. Five to ten minutes.
  - `check_package_start.ps1 -Package <a copy of a package>`: the launcher with "Skip this window next time"
    on starts the game for 20 seconds; the settings file it saved is the one the game's log names, the disc
    comes from `[Disc] Path`, the game ends with exit code 0, the launcher ends by itself and no process is
    left. Never on the folder someone plays from.
