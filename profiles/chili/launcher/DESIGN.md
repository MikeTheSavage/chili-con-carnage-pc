# Chili Con Carnage PC launcher: design specification

Written from the game's own art (menu backgrounds, logo, load screens, HUD, the ODMidText font sheet, as
extracted from the disc) and captured frames (boot screen, main menu, Story 1-1 HUD). The launcher itself
carries none of it: it draws its own look, and shows the game's pictures only from the player's own disc
(`get_art.ps1`). Everything below is normative unless marked "may".

## Design rules: these override everything below

Where a later section says otherwise, this section wins.

1. No explanatory text, and nothing written under a setting's name or under a toggle. So: no one-line summary
   under a page's title (the layout sketch in section 1 still shows one: ignore it; the rows start at y 234);
   no hint line under a setting's name; no note beside a group heading; no description under a mod's name
   (the name alone, level with its switch; an empty folder is dimmed as a whole and locked); no "why it is
   locked" line, no notice line, no green confirmation line; the controller header says nothing while a
   controller is connected and `No controller found`, as a problem, when none is; no note on the Sound page.
   These texts are not in the source: there is no switch that brings them back (the hints of section 2 below
   are history). A setting's name sits level with its control. The single exception under a name: a problem
   the player must act on, red with the warning triangle (an invalid disc, a missing game program).
   A row that cannot be used now is dimmed and carries a padlock after its name, in the place of the gold
   changed-dot. Why it is locked is said in the message line at the bottom only when the player tries to use
   it: a click anywhere on the row (its name included), or A / Enter with the focus on it (the focus may
   land on a locked row; the ring is drawn, the control does nothing). Never for moving the focus. A dimmed
   choice of a row that is not locked says its own reason when it is tried: a click on it, or A / Enter or
   Left / Right that has no other choice to go to. A report never destroys a
   standing message: it lies over an error and the error is there again when the report has gone; it goes
   when it is no longer true, when the focus leaves the row and with the page. A row the game beside the
   launcher has no setting for is not shown at all.
   Not under a setting and kept: the Play page's Ready strip, disc line, three key chips with the names of
   what they do (`Full screen`, `Free the mouse`, `Pause`) and "Last played" (nothing when the game was never
   played); the button prompts along the bottom, each a picture and one or two words; the bottom line;
   questions; error messages, each one short sentence in red with the warning triangle whose button does
   the rest (`The game stopped right away.` with `Open logs`, `Could not write ChiliNative.ini.` with
   `Details`); two confirmations in the message line for a few seconds (`Settings saved.`, `Disc found:
   <path>`). The launcher's and the game's version stand in the window's title bar, not on a page.
   Turning on "Skip this window next time" says nothing: the way back is the package's second shortcut,
   "Chili Con Carnage settings", which starts the launcher with `--settings` (Shift held at the start still
   shows the window; it is not announced). No name, choice or button carries an explanation in brackets.
   Do not add text under settings again, in any state, for any reason short of a red problem.
2. Flames on the selected "HD textures" choice (Textures & mods): animated comic fire behind and above the
   capsule (`Art.Flames`, `Surface.Burn` and `DrawFire`), the capsule itself an ember gradient. This is the one
   exception to "no flames, no loops" in sections 3.6 and 6. While that page is open with HD textures
   chosen only the fire is drawn again (it is a visual of its own), about sixteen times a second;
   everything else is drawn only when something changes. The fire stands still in screenshots, where
   Windows' animations are switched off, and while the launcher is not the window in front.
3. Jargon that used to live only in hints (section 6) now appears nowhere; the labels stay plain words.

## Revisions: where the program now differs from the sections below

- Display: no Window size row until a game build has such a key. Under a closed "Advanced" divider: "Auto
  resolution limit" (No limit / 1920 x 1088 / 960 x 544), which writes `[Rendering] Preset` high / medium / low
  and is locked unless Rendering resolution is Auto.
- Graphics: Quality sets anti-aliasing, texture filtering, sharp textures, ambient shadows and the glow, and
  does not write `[Rendering] Preset` (the game uses that key to limit the automatic resolution, not as a
  look).
- Controls: the device switch is two tabs at the left under the title, without a name. Order: tabs, Control
  style, (gamepad: one status line), "Aiming" (turn speed, look speed, inverts, keep the aim, Camera),
  "Actions", then the rest ("Mouse" and "Keyboard", or "Sticks and triggers" beginning with the live sticks).
- Textures & mods: the choices are "Standard" and "HD pack"; with several packs on, the drop-down names the
  one that wins ("hd_upscaled_4x + 1 more").
- Sound: a second row, Volume, with the button "Open Windows volume mixer".
- A setting the game lacks has no row.
- The rail's focus ring on the selected tab goes round pointer, name and underline.
- The Play picture is enlarged by 1.3 at most, in the card's top right corner, faded at its left and foot.
- Paths are shortened by whole folders (`C:\...\mods\hd_textures`).
- The fire of the HD pack choice moves only while the launcher is the front window.
- A click on the choice that is already selected changes nothing. The open drop-down list is as wide as
  its field.
- A list that scrolls rests on whole rows. Its rest positions are row tops: the wheel (a notch is a row or
  two; small turns of a smooth wheel add up to a notch), Page Up and Page Down, the focus, the scrollbar's
  thumb and the right stick (both free while held, settling when let go) all leave a whole row at the list's
  top edge. A row the bottom edge would cut is not drawn, nor is a group heading whose first row is not;
  the space it leaves is at most one row high. No fade at either edge. A block much taller than a row (the
  Play summary, "What the game said") is the one thing that may fade at the bottom edge.
- More above, more below: a gold arrow with an ink outline (the menu pointer, turned) at each end of the
  4 px scrollbar, inside the card at its right edge, there while the list goes on that way. They are buttons:
  a click moves the list by a screen.
- Graphics has six rows and the Advanced divider and scrolls by one row (the sixth is Glow: Smooth, Original,
  Off, `[Rendering] Glow`, a name and three values with no line under it). Camera (`[Game] Camera`) is the
  last row of "Aiming" on Controls, on both tabs; Defaults on either tab resets it.
- Quality: "Custom" is drawn only while the five rows under it match no quality (Original has the glow at
  Original, the other three at Smooth; a settings file without the key counts as Smooth), and then it is
  the selected choice.
- Rendering resolution: the size of the chosen multiple stands after the choices in gold, the way a slider
  shows its number ("1440 x 816"); with Auto, the limit when one is in force ("≤ 1920 x 1088"), else
  nothing. The choices of this row are 20 px wider than their word instead of 28, to make room.
- Frame rate: the value the row shows is written on the first save even when it is the default (60 fps,
  `FrameLock=1`), because game builds differ in what they do without the line. Not when there is no settings
  file yet and nothing else to write.
- The bottom line holds messages only. The sentence "Changes are saved when you press Play or close the
  launcher." is gone; the gold dot after a changed name stays. A message is one line, cut with an ellipsis.
  Could not write: `Could not write ChiliNative.ini.` with the warning triangle and a `Details` button, which
  opens a box with the file's path, what Windows said and `Close`. A controller presses the message line's
  button (Details, Open logs) with View.
- An action with no key or button: a dim dashed `none` that is not a button, and the `+` chip after it as on
  every other row. Nothing is said about it in the message line.
- A row's report in the message line goes when it is no longer true or the page is left.
- Textures & mods: the folder row is named "Mods folder".
- The fire on the HD pack choice: tongues of uneven width (0.6 to 1.6 of an even step, weighted towards the
  ends, so they never come out level) of which the wide ones are the tall ones, an orange layer in each and a
  gold core only in the larger ones, set a little to one side; a small lick over each shoulder of the capsule
  whose outer side runs into the capsule's round end at half height; the glow above the capsule and no wider
  than it; three small gold diamonds as sparks. The fire may rise 20 px above the list's top edge.
- The Play picture on a large monitor: when one pixel of the picture would cover more than about one and a
  half of the monitor's (the disc's 512 x 272 load screen at 150 % and up), it is blurred slightly, darkened a
  little more and faded sooner. A larger copy of the picture is shown at the same size without that
  (`get_art.ps1 -Mods` takes one from a texture pack).
- Play, after a failed start: the Ready block is as high as its contents, so the page does not scroll with
  a log of up to four lines.
- Prompts along the bottom (y 692): each is a picture and one or two words. They follow the device used
  last. After a controller: its buttons (`A` Change, `B` Back, `Y` Defaults, `X` Open or Remove, `LB` `RB`
  Page, Menu Play, View for the message line's button). After the keyboard: key caps (`Enter`, `Esc`,
  `Del`, `Ctrl` `Tab`). After the mouse: nothing. Before anything was touched: the controller's, when one
  is connected, and the focus ring is drawn then too, so that the prompts point at something. On a slider
  the first prompt is the D-pad with left and right lit (or the two arrow keys) and "Change". A prompt is
  there only for what the button does here: on a button A is named after it (`Open`, `Choose .iso`), and X
  is left out where it would do the same; no A on a locked row, on the open page's own rail entry or on a
  row whose other choices are all dimmed; no "Play page" for B on the Play page. While a controller button
  is waited for, the strip is empty for the player who holds the controller (the row has its own Cancel
  and the count-down) and shows the `Esc` key cap and "Cancel" after the keyboard. While a question is
  open the prompts are the question's (`A` Choose, `B` Keep) and are drawn over its veil. When the
  controller goes, its pictures go from the strip.
- Controller pictures come in two families, drawn alike (an ink disc, a pill for shoulder buttons and
  sticks). Xbox: coloured letters A B X Y, LB RB LT RT, LS RS, the Menu and View signs. PlayStation: cross
  (blue), circle (red), square (pink), triangle (green) as outlines, L1 R1 L2 R2, L3 R3, Options (three
  lines), Share / Create (a small upright button with three rays). The D-pad is the same four-triangle
  cross in both. In a binding chip a picture that is its own name stands alone (the Xbox letters A B X Y);
  a shape or sign is followed by its name (Cross, Triangle, `RT` Right trigger ...); a PlayStation pad's
  L1 to R3 are written as the chip itself, in the chip's own lettering. A keyboard key is a square cap
  wherever it is drawn (binding chips, the Play page, the prompts); a controller's button and the mouse
  stay round. Which family: "Button pictures" (`[Input.Gamepad] Glyphs`), and with Automatic the family of
  the controller in use, Xbox when there is none.
- Controls, for a game that has the keys: after "Sticks and triggers" on the Gamepad tab a group
  "Controller" (Which controller, Use PlayStation controllers, Rumble with a "Test" button after its value,
  Use the gamepad), and on both tabs a last group "Prompts" (On screen: Automatic / Keyboard / Gamepad /
  PSP original; Button pictures: Automatic / Xbox / PlayStation). No heading is repeated by the row under
  it, and a switch's name does not hold its state ("Use the mouse", not "Mouse on"). "Test" is dimmed
  without an Xbox controller and says so when pressed.
- The controller header: `No controller found` in the problems' red with the warning triangle, only while
  there is none. The stick dials have no captions; the left one is the left stick. Each dial holds one
  dot, the stick's position (gold outside the dead zone, cream inside), and the dead zone as a thin ring.
- The padlock: 12 px wide, locked grey lightened a quarter towards cream, 2 px ink outline, 10 px after the
  name; a changed and locked row shows the padlock, then the gold dot.
- Textures & mods: an empty or unreadable mod folder is a locked row (its switch drawn off and dimmed, the
  name, the arrows; the padlock after the name) and is not counted among the mods on the Play page. With no
  mods there is no "Installed mods" heading; "Mods folder" follows the Textures row. The Defaults question
  is the plain one. In the pack list every pack that is on has a tick. While Textures is Standard the
  locked HD pack field names the pack that would be used.
- Motion: the picture is a stack of visuals, so that what moves by itself or with the controller is drawn
  alone: the fire (about sixteen times a second, a drawn fire "on twos"), the breathing focus ring (twenty
  times), the two stick dots of the Gamepad tab (when a dot has moved by a pixel of the monitor, and only
  while the dials are in view), the waiting chip's pulsing outline and count-down (thirty times a second
  for its five seconds), and the message line while it fades. Nothing moves, and nothing is drawn, while
  the launcher is hidden or not the window in front.
- Messages: F5 says nothing (the page shows what was found, and a standing error keeps its place and its
  button); a capture that is offered a key it cannot take says `That key cannot be used` in the waiting
  chip.
- Text. The versions stand in the title bar: `Chili Con Carnage  ·  Launcher 1.0  ·  Game 0.9.0`. An
  error is one short sentence with the triangle; where something is, or what Windows said, is behind its
  button. No brackets in a name or a choice: the control styles are `Modern` and `PSP original`, the action
  is `Walk`. Triangle is `Reload`, Circle is `Weapon wheel / back`. Wrong disc: the path, then `This is not
  Chili Con Carnage (USA).` in red with the triangle. Game program missing: the heading `Game not found`,
  the path, `This program was not found.` in red with the triangle. The sound buffer's marks are numbers
  alone (40, 70, 150). The folder picker's title is `Choose the disc folder`. The binding question is two
  lines (`W is already Move forward.` / `Move it to Shoot?`) with `Move` and `Keep`.
- Controllers. The launcher reads PlayStation pads for its own use whatever "Use PlayStation controllers"
  says; that switch writes the game's key and nothing else. Each PlayStation pad keeps its own state and
  one is listened to, the one touched last; a pad counts as connected once it has sent a report; only the
  known products are taken (DualShock 4: 05C4, 09CC, adaptor 0BA0; DualSense: 0CE6, 0DF2); a Microsoft
  HID controller (vendor 045E) is XInput's. Nothing held is carried across the game: when the launcher
  comes back, the buttons count again only after all were let go.
- Layout. The Controls page's two tabs stay under the title while the list moves under them. One ring
  shape for every rail entry (round the pointer, the name and the place of the underline). The focused
  row's ring is not drawn while its list is open. Cancel and Remove of a waiting chip are buttons; the
  count-down drains round a small dial in Cancel. After a failed start the strip under "What the game
  said" is drawn without the word `Ready`, dimmed.
- Kept as they are: the row pitch (64, whole-row rests: a list may end with an empty band of up to a row);
  the names that are whole clauses ("Start moving at the dead zone's edge", "Keep the aim where you put it",
  "Draw on a second processor core", "Sharp textures at an angle", "Clicks work in menus", "Dump textures
  while playing", "Skip this window next time"), because shortening them would need the jargon section 6
  forbids; the Play page's empty lower third (room for "What the game said").

## 0. What makes it this game

- Heat. Every menu is scorched paper: dark rust at the edges, a hot yellow-orange core, soot blotches,
  grain, ash. Nothing is grey, blue or clean (the one teal background is for a side mode).
- Two letterings. The logo: wide, heavy, all-caps block letters, yellow-gold with a darker gold base, a thick
  black outline and a hard black drop shadow, slightly rough edges. The menu and HUD font (ODMidText): a
  bold, rounded, italic sans; white or yellow-orange fill, 3 px black stroke; headings in hot orange
  (`Singleplayer`), sub-items in chili red or orange-yellow; the combo names (`NOSE CRUSHER`) in pale yellow
  with a black stroke and a soft glow.
- Shapes: capsules and circles (health bar, PSP button glyphs on near-black discs), white-outlined rounded
  rectangles (combo meter, yellow-to-red gradient), chalk underlines, torn dark edges, locked things
  greyscale and blurred. Humour (chicken suits, a chili as a glyph): one wink per page in a hint, never a label.

## 1. Layout (logical 1280x720)

The window is drawn on a 1280x720 logical canvas scaled uniformly by `s = min(width/1280, height/720)`
and centred; the background fills the whole window, never bars. Minimum window 960x540 logical (then
`s = 0.75`). Default size 1280x720 times the DPI scale; resizable; title "Chili Con Carnage"; icon
`app_icon.ico`. All sizes below are logical pixels; the implementer multiplies by `s` and by DPI.

```
x: 0        40            300 330                                   1240 1280
y:40  [TITLE BLOCK 40,28-420,124]                   [status chip, right-aligned, y 40]
  150 [RAIL 40..300]      [CONTENT CARD 330,150 - 1240,600, torn top edge]
  168   Play              [ page title 360,176 | one-line page summary 362,222 ]
  230   Display           [ rows from y 262, pitch 64, label col x 362, control col x 760..1210 ]
  ...   Controls
  600 ----------------------------------------------------------------------
  618 [hairline 40..1240]
  626 [status line 40,640 + message 40,662]     [Defaults 860..1000]  [PLAY 1020,626-1240,692]
  700 [gamepad glyph hints, only when a pad is connected, y 702, x 40]
```

- Title block: two lines of the launcher's own lettering "CHILI CON" / "CARNAGE" at 40 px cap height
  (lines at y 30 and 76), plus a 22 px italic "PC" tag in chili red to the right of the second line
  (not overlapping). When the art folder has the game logo (section 3.6) it replaces this block at the
  same box, left-aligned, 380 px wide.
- Rail: 7 tabs, left edge of text x 56, first baseline area y 168, pitch 62. The selected tab has a gold
  triangle pointer at x 28 and a chalk underline (170x6, cream at 80%) 40 px below its top.
- Card: 910x450, corner radius 8, fill soot `#140804` at 78% alpha; its top edge is torn (section 3.5).
  Inside: 32 px padding left, 30 px right; rows are 56 px tall with 8 px between; a page with more rows
  than fit (Controls) scrolls inside the card with a 4 px cream scrollbar at x 1230, never the window.
- Row: label (18 px) at the row's top-left, hint (13 px) 24 px below it, control right-aligned in a
  450 px column starting at x 760. Controls in a column align their left edges.
- Bottom bar: status line and message left; "Defaults" (secondary) and "PLAY" (primary, 220x66) right.
  PLAY is the only element that uses the logo lettering apart from the title.

## 2. Pages, in rail order (most used first)

Setting names are the labels; the hint is the one-line text under each label; the key is the ini contract
(`ChiliNative.ini` beside the game executable). A value the file lacks is shown as its default. Writing
keeps the player's other lines and comments; a disabled line (`; Key=`) is replaced by a live one.

### Play
The card holds: a "Ready" strip summarising what will run (`Full screen · 1920x1088 · 60 fps · 4x
anti-aliasing · Keyboard & mouse · 2 mods`), the game-data status with its path, three reminders (`F11 full
screen`, `F1 frees the mouse`, `Esc pauses`) and `Last played: <date>` if a log exists. The right third may
show a game picture (3.6). PLAY writes the ini, starts the game executable beside the launcher and closes;
if the game exits within 3 s with an error the launcher reopens on Play with the message of section 5.

### Display
| Label | Hint | Control | Key |
|---|---|---|---|
| Screen | Full screen or a window. F11 switches while playing. | segmented: Window / Full screen | `[Rendering] Fullscreen` false/true |
| Window size | Only used in a window. | drop-down: 960 x 544, 1440 x 816, 1920 x 1088, 2880 x 1632 | window-size key (open question; hide the row if the game has none) |
| Rendering resolution | Sharper picture; the game keeps its 30:17 shape. | segmented: Auto, 1x, 2x, 3x, 4x, 6x, 8x; the hint shows the pixels of the choice (`2x = 960 x 544`) | `[Rendering] InternalScale` auto, 1-8 |
| Frame rate | 60 is smoother; 30 is how the PSP ran it. | segmented: 30 fps / 60 fps | `[Game] FrameLock` 2 / 1 |
| Show frames early | Shows a scene as soon as it is drawn. Leave it on. | toggle | `[Rendering] EarlyPresent` |

### Graphics
| Label | Hint | Control | Key |
|---|---|---|---|
| Quality | Sets everything below. Change one and it says Custom. | segmented: Original, Medium, High, Ultra, Custom | `[Rendering] Preset` |
| Anti-aliasing | Smooths jagged edges. | segmented: Off, 2x, 4x, 8x | `[Rendering] MSAA` (or `AntiAliasing`) 1/2/4/8 |
| Texture filtering | Original is blocky like the PSP; Smooth blends. | segmented: Original, Smooth | `[Rendering] TextureFilter` |
| Sharp textures at an angle | Anisotropic filtering. | segmented: Off, 2x, 4x, 8x, 16x | `[Rendering] Anisotropy` |
| Ambient shadows | Soft shading in corners (ambient occlusion). | toggle | `[Rendering] SSAO` |
| Glow | none | segmented: Smooth, Original, Off | `[Rendering] Glow` smooth / game / off |
| Camera | Responsive follows your aim closely; Original is the PSP camera. | segmented: Responsive / Original | `[Game] Camera` tight / original |
| Renderer (Advanced) | Software is the slow reference renderer, for checking a picture. | segmented: Graphics card / Software | `[Rendering] Backend` d3d12 / software |
"Advanced" rows sit under a collapsed "Advanced" divider at the card's bottom, closed by default.

### Controls
Top of the card: a two-way switch `Keyboard & mouse / Gamepad` (what the rows show, not a setting) and the
scheme: `Control style: Modern (move + aim) / PSP original` with the hint "PSP original turns with
left/right and cannot look up or down." (`[Input] Scheme` dual_analog / psp). Changing the style asks
"Load that style's default bindings too?" Yes / No.

Keyboard & mouse view, rows by action, each with up to three binding chips and a "+" chip:
Move forward, Move back, Move left, Move right, Walk (hold), Shoot, Headshot mode, Jump / dive / use,
Reload / back, Weapon wheel / cancel, Target objects / exit stunt, Previous weapon, Next weapon, Rewind,
Pause, Select, Look left, Look right, Look up, Look down (keys only), Free the mouse, Input test line.
Keys: `[Input.Keyboard] <Target>=key[,key]`. Mouse buttons and wheel appear as chips in the same rows and
are stored the other way round (`[Input.Mouse] Mouse1..5, WheelUp, WheelDown = <Target>`).
Then the mouse group: Turn speed and Look speed (sliders, 0.01-1.00 shown with two decimals, stored
`SensitivityX`/`SensitivityY` 0.001-5, default 0.08, logarithmic track so 0.08 sits a third along),
Invert left/right, Invert up/down (toggles `InvertX`, `InvertY`), Keep the aim where you put it (`HoldPitch`),
Precise aim (`Exact`, hint "Turns by exactly what the mouse moved. Leave it on."), Smoothing (slider
0-0.95), Grab the mouse (segmented: On click / Never = `Capture`), Clicks work in menus (`MenuClicks`).
Keyboard-only extras: Walk speed (`WalkScale` 0.31-1), Look key speed (`LookKeySpeed` 0.05-1).

Gamepad view, the same action rows with one button chip each (stored `[Input.Gamepad] <Button>=<Target>`),
then: Left stick dead zone, Right stick dead zone (sliders 0-0.9, shown as percent), Trigger pull
(`TriggerThreshold`), Turn speed and Look speed (`TurnSensitivity`, `LookSensitivity` 0-4, 1 = PSP),
Invert up/down (`InvertY`), Start moving at the dead zone's edge (`CompensateEngineDeadzone`),
Controller (`Index`: Automatic, 1, 2, 3, 4), Gamepad on (`Enabled`). The header shows "Controller
connected: Xbox pad 1" or "No controller found" and a live left/right stick dot (24 px circles) so the dead
zone slider can be judged by eye. "Defaults" in the bottom bar resets the visible device only, after
"Reset keyboard & mouse bindings to the defaults?" Yes / No.

### Textures & mods
| Label | Hint | Control | Key |
|---|---|---|---|
| Textures | Standard are the disc's. An HD pack is a mod with a `textures` folder. | segmented: Standard / HD pack, plus a drop-down naming the pack when more than one qualifies | pack = a mod folder containing `textures\`; chosen pack is put into `[Mods] Enabled` |
| Installed mods | On, off and order. A later mod wins over an earlier one. | list rows: toggle, name, one-line description from the mod's `README` or `properties.ini` first comment, up/down arrows | `[Mods] Enabled` in list order; "every folder, alphabetical" when all are on and untouched |
| Open mods folder | Drop a mod folder in here, then come back. | button | `[Mods] Folder` (default `mods`) |
| Dump textures while playing | Writes every texture the game draws into `texture_dump`. For modders. | toggle | `[Mods] DumpTextures` |
Empty list text: "No mods yet. Put a mod folder into the mods folder." Texture packs are listed in the
mod list too, marked with a small picture icon.

### Sound
| Sound buffer | Milliseconds of sound kept ready. Raise it if the sound crackles; everything is heard a little later. | slider 20-500, steps of 10, with three marks: Fast 40, Normal 70, Safe 150 | `[Audio] BufferMs` |

### Game data
| Label | Hint | Control | Key |
|---|---|---|---|
| Disc | Your own copy of the game: the .iso file or the unpacked disc folder. | path field (read-only) + "Choose .iso..." + "Choose folder..."; under it the check result | `[Disc] Path` |
| Saves | Where your progress lives. | path + "Open" | `saves\` beside the executable |
| Settings file | Everything on these pages, as text. | path + "Open" | `ChiliNative.ini` |
| Logs | What the game wrote last time. Send this when something goes wrong. | path + "Open" | `logs\` |
| Game thread priority (Advanced) | High keeps the game smooth when other programs are busy. | segmented: Normal / High | `[Game] Priority` |
Bottom of the card: "Launcher <version> · Game <file version of the executable>".

## 3. Visual system

### 3.1 Palette (hex, taken from the art)
| Name | Hex | From | Use |
|---|---|---|---|
| Ink | `#0A0503` | logo outline, HUD strokes | outlines, shadows, text on light fills |
| Soot | `#140804` | load-screen bars | card fill (78% alpha), drop-down panels (96%) |
| Rust dark | `#3C0C05` | background01 corners | background edges, pressed state |
| Rust | `#7A1F08` | background01 | background mid-tone |
| Ember | `#B8440C` | background01 core, PLAY fill | background inner tone, PLAY button face |
| Flame | `#F0A020` | background02, footer text | selected segment, slider fill start, pointer |
| Hot orange | `#F85808` | `Singleplayer` heading | unselected rail tabs, warnings |
| Chili | `#E81010` | logo chilies, `El Gringo Loco` | "PC" tag, destructive confirms, error text |
| Gold | `#F8D820` | logo fill | selected rail tab, page titles, focus ring, PLAY lettering |
| Gold dark | `#D89A10` | logo base shading | gradient bottoms, slider thumb edge |
| Pale yellow | `#F8C868` | combo names | hover text, values next to sliders |
| Cream | `#F6E8C8` | chalk underline, bootscreen sand | body text, hints (at 70%), unselected chip text |
| White | `#FFFFFF` | ODMidText fill | labels |
| Health red | `#F82808` | health bar | toggle ON track |
| Chalk green | `#9BE86A` | font sheet check mark | success text, "found" chip |
| Locked grey | `#8A7A6A` | locked level thumbnails | disabled text (50%), disabled controls desaturated |

### 3.2 Typography (Windows-shipped only)
- Display face: **Arial Black** (`ariblk.ttf`), fallbacks Impact, Segoe UI Black. All caps, tracking +2%.
  Fill: vertical gradient pale `#FFF0A0` (top) to Gold (45%) to Gold dark (bottom); stroke 4 px Ink; hard
  shadow Ink 60% offset (4,5) at 40 px; everything scales with the text size. Used for the title block and
  PLAY only.
- Heading face: **Segoe UI Black Italic** (`seguibli.ttf`), fallbacks Verdana Bold Italic, Arial Bold Italic.
  Stroke 3 px Ink, shadow Ink 50% offset (2,3). Rail tabs 26 px (selected 30 px, Gold; others Hot orange),
  page titles 34 px Gold, dialog titles 24 px Pale yellow. This imitates ODMidText; do not shear or fake it
  further.
- UI face: **Segoe UI** (`segoeui.ttf`, `seguisb.ttf` Semibold), fallback Tahoma. Labels Semibold 18 White,
  hints Regular 13 Cream 70%, chips Semibold 15, status 14, paths Regular 14 (Consolas 13 is allowed
  for paths). No stroke on UI text; the card gives it contrast.
- Numbers next to sliders in Semibold 15 Gold. Keyboard key names Title Case as the ini writes them.

### 3.3 Controls
- Segmented buttons: capsules 32 px tall, radius 16, 14 px side padding, 8 px apart. Off: Ink 45% fill,
  Cream 35% outline 2 px, Cream text. On: Flame fill, Ink outline 2 px, Ink text. More than 7 options or
  long values become a drop-down.
- Drop-down: capsule like a segment, chevron (the HUD triangle, 10 px, Cream) at right; opens a Soot 96%
  panel, radius 8, rows 36 px, hovered row Ember 60%, selected row Flame with Ink text.
- Toggle: 64x28 capsule (the health bar): track Ink 50% with 2 px Ink outline; ON track Health red with a
  3 px Flame inner edge, knob Cream 24 px with 2 px Ink outline on the right; OFF knob on the left, track
  unchanged. Switch travels in 100 ms.
- Slider: track 420x10 radius 5, Ink 55% with 2 px Ink outline; filled part is the combo meter gradient
  Gold `#F8F000` to Flame to Health red along the whole track length (so a low value shows yellow only);
  thumb 22 px Cream disc with 3 px Ink outline; value text at right. Marks (Sound) are 2x8 Cream ticks with
  11 px labels. Arrow keys step, Shift+arrow steps 10x.
- Button (secondary, e.g. Defaults, Open): capsule 46 px tall, Ink 45% fill, Cream 50% outline, Cream text.
- PLAY: 220x66, radius 10, Ink frame 4 px, face Ember, Gold outline 2 px inside, "PLAY" in the display face
  38 px centred. Disabled when game data is missing (face Rust dark, lettering Locked grey, no gradient).
- Binding chip: capsule 30 px, Soot 90% fill, Cream outline, key name in Semibold 15; a gamepad chip shows
  the button glyph (section 3.7) before the name. Capturing: the chip widens to 200 px, Gold outline
  pulsing, text "Press a key..." / "Press a button...", a small "Cancel" text button at its right and a
  5-second arc draining around the Cancel button.
- List rows (mods): 44 px, alternating Ink 0%/15%, toggle at left, name Semibold 16 White, description
  13 Cream 70%, up/down triangle buttons at right (24 px hit area each). Dragging is optional; arrows
  are required.
- Device switch (Controls): two segments of the segmented style, 36 px tall. Path field: read-only capsule,
  Cream text, middle-ellipsised; Enter on the row opens the folder. Scrollbar: 4 px Cream 40% thumb, no arrows.

### 3.4 States
| State | Look |
|---|---|
| Normal | as above |
| Hover (mouse) | fill +12% lighter, text Pale yellow for Cream text; cursor hand on anything clickable |
| Focused (keyboard or gamepad) | 2 px Gold ring, radius +2, 3 px outside the control; on a row, the ring wraps the whole row (348..1222) and the label turns Pale yellow; the focused rail tab shows the pointer triangle even if not selected (selected keeps the underline) |
| Pressed | fill Rust dark, content moves (1,1) |
| Disabled | 50% alpha, desaturated to Locked grey tones, no hover; a hint line says why ("Only used in a window.") |
| Changed, unsaved | a 6 px Gold dot after the label until saved (settings save on PLAY and on closing; the message line says "Settings saved.") |
| Error on the row | hint replaced by the message in Chili with a 14 px warning triangle |

### 3.5 Backgrounds (drawn by the launcher, no game files)
- Radial gradient centred at (62%, 28%) of the window: Flame at the core to Ember at 0.5 radius to Rust at
  0.8 to Rust dark at the corners. Over it: monochrome noise grain 6% amplitude (fixed seed), about sixty
  blurred soot ellipses (radius 20-140, Ink 8-28%, blur 40), a vignette (Ink 0% centre to 55% at the
  corners, blur 120). Static; rendered once per size and cached.
- The card's torn top: a polyline from x 330 to 1240 with steps of 6-18 px and heights within -6..+8 of
  y 150, filled with the card colour; the same seed each time so it does not crawl.
- The only ornaments: the chalk underline (section 1) and two chilies crossed above the title block at
  (44,8), 60 px wide, Chili body, green `#3D8B2F` stem, 3 px Ink outline, drawn as paths. Nothing from the
  game is embedded in the executable or committed.

### 3.6 Game pictures (only when present in `art\` beside the launcher; the folder is git-ignored)
| File in `art\` | Game source (index key) | Where shown |
|---|---|---|
| `logo.png` | GameLogo_FrontScreen `048e4eecf23e28a1` (512x256, alpha) | title block, 380 px wide |
| `background.png` | BackGround01 `e1215277c6aafa16` (256x256) | tiled at 512 px over the gradient, multiply 35%, under the grain |
| `hero.png` | any load screen, e.g. LoadScreenMacho1 (512x272) | Play page, right 300 px of the card, cropped to its left 480 columns, faded to transparent over its left 80 px |
| `pack_<modname>.png` | a 4x texture from that pack | Textures page, 96x96 preview next to the pack name |
A missing or unreadable file means the drawn version; never a broken-image box. A "Fill this from my disc"
button on Game data may run the extractor into `art\` (future; not required for the first build).

### 3.7 Icons (vector, 2 px Ink stroke, Cream fill unless said)
Pointer triangle (the HUD menu arrow); chevron; folder; check mark in Chalk green drawn as one chalk
stroke; warning triangle Flame with Ink "!"; picture (texture pack); gamepad glyphs as 20 px Ink discs
with a Cream letter or symbol (A B X Y, LB RB LT RT, LS RS, ☰ for Start, ⧉ for Back, D-pad as the game's
four-triangle cross), mouse glyph with the pressed button in Flame; keyboard key chip as in 3.3.

### 3.8 Motion (restrained)
Page switch: card content fades in over 120 ms while moving up 12 px; the rail underline slides to the new
tab in 150 ms. Hover and press 80 ms; toggle and segment 100 ms; focus ring breathes 70-100% alpha at 1.2 s
only while a gamepad drove the focus last; PLAY's shadow grows 2 px on hover. No loops, flames or
particles. If Windows reports animations off, every duration is 0.

## 4. Controller and keyboard operation
- Focus order: rail (tabs, up/down) -> card rows top to bottom (within a row, the control; within a
  segmented control, left/right picks) -> Defaults -> PLAY -> back to the rail. Tab/Shift+Tab and the D-pad
  or left stick follow this order; Left/Right move inside a control, otherwise between rail and card.
- Gamepad: A activates or opens; B closes a drop-down or capture, otherwise returns to the rail, and on the
  rail goes to Play; LB/RB previous/next page from anywhere; Start = PLAY (asks "Start the game?" only when
  game data is missing, where it does nothing); Y = Defaults when the page has one; X = "Open" on a path
  row; right stick scrolls the card. Glyph hints at y 702 name what A, B, Y and Start do here.
- Keyboard: Enter activates, Space toggles, Esc closes or returns to the rail (on the rail it does nothing;
  the launcher closes only by its window button or Alt+F4), Ctrl+Tab / Ctrl+Shift+Tab switch pages, Ctrl+S
  saves ("Settings saved."), F5 rescans mods, game data and controllers.
- Mouse: everything works by click; the wheel scrolls the card, never a segmented control.
- Binding capture: starts on activating a chip; takes the first key (keyboard view) or button (gamepad
  view), including Escape and Enter; ends on Cancel (click, or the other device's Escape/B), on a click
  anywhere else, or after 5 s. The launcher ignores its own shortcuts while capturing. A key another action
  holds raises the conflict dialog (section 5). "+" adds a chip, three at most; Delete or Backspace on a
  focused chip removes it (an action may have no key: the row shows a dashed chip "none"). XInput is polled
  every 16 ms; a pad plugged in later is found within 2 s and the header updates.

## 5. Wording
Short, plain, Title Case for page names, Sentence case for everything else. Say "the game", not "the
emulator" or "the port". No trademarks of the publisher in the UI text. Messages (message line unless said):
- Game data found: status chip `Game data: found` in Chalk green; Play page: `Your disc: <path>`.
(The messages as the program says them; the explanatory sentences this list once held are gone.)
- Missing: chip `Game data: not found` in Chili; Play page card: title `Choose your disc` and the two
  Choose buttons, no text; PLAY disabled.
- Wrong disc: the path, and under it in red with the triangle `This is not Chili Con Carnage (USA).`;
  unreadable: `This file cannot be read as an .iso disc image.`; a folder: `This folder is not a PSP game
  disc.`
- Saved: `Settings saved.` (Chalk green, 3 s, then fades). Disc chosen: `Disc found: <path>` (4 s). Could
  not write: `Could not write ChiliNative.ini.` (red with the triangle, stays) with a `Details` button for
  the reason.
- Binding conflict dialog: title `Already used`, text `W is already Move forward.` / `Move it to Shoot?` on
  two lines, buttons `Move` / `Keep`. Gamepad: `Y is already Jump / dive / use.` / `Move it to Reload?`
- Removing the last key: no dialog; the row shows a dim `none` and the `+` chip.
- HD pack not installed: the HD choice is dimmed and says `Textures: No HD pack found.` in the message line
  when it is tried.
- Game failed to start (returned within 3 s): `The game stopped right away.` with an `Open logs` button;
  later: `The game stopped with error <code>.`
- Scheme change: `Load the default bindings for PSP original too?` `Yes` / `No`.
- Defaults: `Reset keyboard & mouse bindings to the defaults?` `Reset` / `Cancel`.
- Mods: nothing is said when there are none; an empty folder is a locked row that says `<name>: This folder
  is empty.` when tried. Controller header: nothing with a controller, `No controller found` without one.

## 6. Avoid
- Grey dialogs, system blue, system check boxes and combo boxes drawn by Windows; mixed native and custom.
- Flames, smoke, particles or music in the launcher; animated backgrounds; sounds on hover. (One exception:
  the flames on the selected "HD textures" choice. See "Design rules" at the top.)
- Game textures as UI parts (health bar, HUD numbers) and anything from the disc inside the executable.
- Orange text on the hot core without a stroke; outlined body text; italic body text; all-caps labels.
- Settings jargon as labels (MSAA, SSAO, anisotropy, dead zone appear only in hints).
- Hidden traps: a capture that cannot be left, a window Alt+F4 cannot close, a Reset without a question,
  saving on every click (save on PLAY, close and Ctrl+S); letterbox bars, horizontal scrolling, tooltips as
  the only explanation, more than one modal at a time.
- Guessing the ini: a key the launcher does not know is kept untouched, never deleted or rewritten.

## Controllers

- The whole launcher is comfortable with a controller alone: start it, move between pages and controls, change
  every setting, rebind, press Play, with no mouse or keyboard. It is a first-class way to use it, and the
  screenshots show it (a focused control on every page, the hint bar).
- The on-screen hints in the launcher (the hint bar and the binding rows) show the glyphs of what the player is using
  right now: Xbox-style buttons (A B X Y, LB RB LT RT, sticks, View/Menu) when a controller was used last, keys and
  mouse buttons when the keyboard or mouse was used last, switching as soon as the other device is touched. The
  glyphs are vector art (coloured letter buttons, keycaps, a mouse with the button marked). A second controller
  style (PlayStation shapes: cross, circle, square, triangle, L1 R1 L2 R2) is chosen by
  `[Input.Gamepad] Glyphs = auto | xbox | playstation`, the same setting the game uses for its own prompts.
- Controller mapping on the Controls page: every gamepad button and stick function can be reassigned by pressing the
  button, per control scheme, with reset to defaults; the page shows a simple controller picture or list that makes
  the current layout readable at a glance.
