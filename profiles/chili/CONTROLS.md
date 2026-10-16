# PC controls

Bindings live in `ChiliNative.ini` beside `chili_recomp.exe`. The file is written with the defaults on the
first run; delete it to get them back. Code: `host/pc_input.hpp` (devices and bindings, the first-run text of
each control style, an older file brought up to date), `host/input_layout.hpp` (the control styles),
`host/pad_sample.hpp` and `host/hid_pad.hpp` (gamepads: the state every pad is read into, PlayStation pads),
`host/pad_rumble_hook.hpp` (the game's vibration call), `host/input_device.hpp` (what the on-screen prompts
are told), `host/prompt_style.hpp` and `host/prompt_glyphs.*` (the on-screen prompts), `host/input_title.hpp`
(what the title bar says), `host/dual_analog_controls.hpp` and `host/dual_analog.cpp` (the two-stick scheme
inside the game). Tests: `tools/pc_input_test.cpp` (bindings, mouse, gamepads, the ini file, the control
styles and the update of an older file), `tools/pad_rumble_hook_test.cpp` (the game's vibration call to the
pad in use), `tools/prompt_glyphs_test.cpp` (the prompt pictures, and the input code and the prompts together)
and `tools/dual_analog_test.cpp` (where a frame's turn goes, over a made-up player in guest memory).

## Control styles

`[Input] Layout` picks the keys, mouse buttons and pad buttons a player starts with. The launcher's Controls
page ("Control style") and the game's own Settings page (Controls, "Style") show the three by name:

| Name | `Layout` | `Scheme` | What it is |
|---|---|---|---|
| Total Overdose PC (the default) | `tod_pc` | `dual_analog` | The keys and the mouse Total Overdose had on PC, for every action the two games share; the chili's own actions on keys in the same spirit (below) |
| Modern | `modern` | `dual_analog` | The port's own layout before Total Overdose PC, unchanged |
| PSP original | `psp` | `psp` | The PSP's layout: left/right turns, L strafes (Modern's keys without the look keys, the PSP's pad table) |

Total Overdose on PC had no controller layout (its PC setup is keyboard and mouse only), so the pad table
of Total Overdose PC is Modern's, and so are the mouse and every setting that is no binding.

A binding the file leaves out takes the default of the file's `Layout`; a file without a `Layout` line means
PSP original with `Scheme=psp` and Total Overdose PC otherwise. Choosing another style, in the launcher or on
the game's page, writes `Layout`, the `Scheme` that goes with it, and the new style's keys into every binding
line that still holds the old style's (the same keys in any order, case or spelling); a binding the player
changed stays as it is (`PcInputConfig::layout_binding_changes`, the launcher's `Model.SwitchLayout`). On the
game's page the style is written at once and runs from the next start, like the scheme. The launcher no
longer asks "Load the default bindings too?".

## Schemes

`[Input] Scheme=dual_analog` (with Total Overdose PC and Modern): keyboard and mouse, or a two-stick gamepad.
The move keys or the left stick run and strafe; the mouse or the right stick turns the player and pitches the
view. This is the engine's own two-stick path: the player script asks for `run_left`, `run_right`, `look_up`
and `look_down`, which the PSP data never defined, and the host answers them (the same
`Control`/`ControlSetup` layer as Total Overdose). The second stick carries a rate: one byte per axis, where
the PSP stick's full pressure sits a third of the way along and full travel is about 1,500 degrees per second
of turn and of pitch. The mouse does not go through that stick: it is an angle, applied as it is, and the
stick's turn is applied as each frame's angle too (see "Mouse latency and aim"). The engine limits the pitch
to 45 degrees up and down, and auto-aim still picks the target.

`Scheme=psp` (with PSP original). The PSP layout untouched: left/right turns, holding L strafes, no pitch,
turning capped at about 170 degrees per second; the mouse turns left and right only. Deterministic runs use
it with PSP original's bindings unless `CHILI_CONFIG` or the replay file says otherwise, so replays recorded
before the second stick existed give the same state hashes. Bindings go to PSP buttons and no host input is
read in those runs, so the control style moves no hash.

## Settings files of older builds

`[Input] Version` is 3 since the control styles. When the game starts on a file it wrote itself (the file
begins with the game's own first line) with a lower number, it brings the file up to date once, keeps the
file as it was as `ChiliNative.ini.bak` (over an older backup), and says so in one `[input]` line of the log:

- No `Version`, or 1 (an early build wrote `Scheme=psp`): every `[Input...]` section is replaced by the current
  defaults (Total Overdose PC); the other sections stay.
- 2 (earlier builds, whose defaults were Modern with `dual_analog` and PSP original
  with `psp`): every binding line is compared with that default. All of them equal and `Scheme=dual_analog`:
  the binding lines take Total Overdose PC's keys (ten keyboard lines change; the mouse and pad lines are the
  same in both) and `Layout=tod_pc` is added. All equal and `Scheme=psp`: nothing is rewritten,
  `Layout=psp`. Any line of the player's own (or an `[Input.Gamepad.PlayStation]` section): every binding is
  kept and the file gets the style it was made with, `Layout=modern` or `psp`. In every case `Version=3`;
  every other line stays as it is (mouse and stick speeds, inversion, `ExactTurn`, `Prompts`, `[Rendering]`,
  `[Game]`, `[Audio]`, `[Mods]`, `[Disc]`), except three comment lines that described the older build and,
  where the keys change, the comments above Cross and Triangle.

The launcher does the same when it opens such a file (`launcher/src/Model.cs`, `InputMigration`, under the
mutex the two share), to the same bytes: `launcher/test/sample_ChiliNative.migrated.ini` is the update of
the launcher's sample (a file of an earlier build), and the game's test and the launcher's test both have to make
exactly it. A hand-written ini (one that does not start with the game's own first line) and any ini read by a
deterministic run are never changed. An older build reads a Version 3 file without changing it and says
`Layout` is a setting it does not know.

## Defaults (Total Overdose PC)

| Action | Keyboard and mouse | Xbox-style pad | PlayStation pad (same buttons by position) |
|---|---|---|---|
| Move and strafe | W A S D (X held: walk) | Left stick | Left stick |
| Turn and look | Mouse (J L I K without one) | Right stick | Right stick |
| Shoot (Square) | Mouse 1, G | RT | R2 |
| Headshot mode, held; the handbrake in a vehicle (R) | Mouse 2, V | LT | L2 |
| Shoot-dodge, jump, use, enter a vehicle, dive out of one; confirm (Cross) | Space, F, Enter, Left Shift; in menus also a left click | A, Y | Cross, Triangle |
| Reload, the loco move (Triangle) | R, Left Ctrl | X | Square |
| Weapon quick-select (held, then a face button), leave a vehicle; back and cancel in menus, skip a cutscene (Circle) | E, Backspace, Mouse 3; in menus also a right click | B | Circle |
| Target objects; held in a vehicle, lean out (L) | Q, C | Left stick click | L3 |
| Previous / next weapon | Wheel down / up, Left / Right | LB / RB | L1 / R1 |
| Rewind; in a vehicle the forks down (D-pad down) | Tab, Down, Page Down | D-pad down | D-pad down |
| Menu up; in a vehicle the forks up (D-pad up) | Up, Page Up | D-pad up | D-pad up |
| Pause (Start) | Escape, P | Start | Options |
| Select | ` (the key left of 1 on a US keyboard) | Back | Share or Create |
| Free or capture the mouse | F1 | | |
| Input test line in the title bar | F3 | | |
| Full screen and back | F11, Alt+Enter | | |

The game has several actions on one PSP button, so a key does everything its button does: Space, F, Enter
and Left Shift each jump, dive and use (the game decides which); R and Left Ctrl each reload and do the loco
move. The order of a line is the order the prompts picture: Space for Cross, E for Circle, R for Triangle,
Q for L, Escape for Start (a D-pad target pictures its arrow key, so rewind shows the Down arrow). 1 to 8
and H are free.

Modern differs in ten lines: Cross `Space,E,Enter`, Circle `F,Backspace`, Triangle `R`, L `Q`, D-pad up
`Up`, down `Down,Z`, left `Left,1`, right `Right,2`, Start `Escape`, Select `Tab`.

**Total Overdose on PC and this game.** From Total Overdose's own control data (the `PC` > `Keyboard` and
`Mouse` setups of `overdose.main`), its manual and an options file the game wrote:

| Total Overdose PC | Here |
|---|---|
| W A S D move and strafe, mouse look and aim | the same |
| Left button fire, right button headshot (held) | the same (Square, R) |
| Space shoot-dodge, Left Shift jump, F use and enter a vehicle | Cross: Space, F, Left Shift (and Enter) |
| R reload, Left Ctrl loco move | Triangle: R, Left Ctrl |
| Wheel up / down cycle weapons | the same (D-pad right / left) |
| Tab rewind | D-pad down: Tab |
| Page Up / Page Down raise and lower the forks | D-pad up / down: Page Up, Page Down |
| Escape the in-game menu, P pause | Start: Escape, P |
| Enter and left click select in menus, arrow keys move | the same |
| C prepare to exit a vehicle (keyboard data), V brake | L: C; R: V (the handbrake) |
| 1 to 8 weapon classes | not matched: the game selects a class by holding Circle and pressing a face button, and a key presses one button; 1 to 8 are left free |
| Escape back in menus | not matched: Escape on Start and Circle would pause and leave a vehicle at once; back is E, Backspace, a right click |
| Space handbrake; F leave a vehicle; left button exit stunt | not matched: Space and F are Cross, leaving is Circle (E), the stunt is L held then Cross (Q or C, then Space) |
| H horn; crouch; loco-move cycling (Q, E); freeze time | not in this game (the horn is on R3, which the PSP has not) |
| The numpad and the arrows steer a vehicle | not bound: the arrows are the D-pad, the numpad would move the player on foot |
| Mouse sensitivity 0 to 10 | not matched: the port's 0.08 degrees per count stays; no inversion, as Total Overdose's NORMAL |

**Shift and Ctrl.** Modern and PSP original have no Shift, Ctrl or Alt. Total Overdose PC keeps Left Shift
and Left Ctrl as Total Overdose had them, each beside a plain key of the same button (Space, F, Enter; R),
so nothing needs them; no default is Alt, a Windows key, Caps Lock or Num Lock (`tools/pc_input_test.cpp`
holds both rules). Windows has shortcuts on them: five taps of Shift with no other key between raise the
Sticky Keys question, which takes the focus and frees the mouse (Total Overdose had the same; switch the
shortcut off under Settings > Accessibility > Keyboard > Sticky keys, "Keyboard shortcut for Sticky keys"),
Ctrl+Escape (the loco move held while pausing) opens the Start menu, and Ctrl+Shift switches the input
language where Windows has two. F11 and Alt+Enter belong to the window (`RENDERING.md`) and cannot be bound.

The game's Button Layout B (Profile Options) swaps shoot and headshot on the PSP buttons: with it the left
button gives headshots and the right one shoots. Total Overdose PC, like the launcher's labels, is laid out
for Layout A.

**In the menus Circle is Back and Triangle is not.** Checked in Profile Options: Triangle did nothing for
two seconds, Circle then left the screen. So what goes back in a menu is E, Backspace and the right mouse
button (F with Modern), on a pad B (Circle on a PlayStation pad); R and the pad's X (Triangle) reload in play and do nothing
in a menu.

## The mouse

- A click in the window captures the mouse: the cursor is hidden and kept inside the window, and the title
  bar says `mouse captured, F1 frees it`. No key has to be pressed first. In play that click also does what
  the button does, so the first shot after a pause or Alt+Tab counts; only the click that brings the window
  to the front captures and does nothing else.
- It is given back with F1, with Alt+Tab, when another window takes the focus, when the window is moved or
  resized, and when the game ends. After that the title says `click to capture the mouse`.
- In the main menu, in movies, cutscenes and the pause screen the mouse is free. There a left click is Cross
  (confirm, skip) and a right click Circle (back), so the game can be started and its menus walked with the
  mouse alone; a click made there is remembered (`the mouse is captured when play starts, F1 cancels`) and
  the mouse is captured when the mission begins. The click that brings the window to the front does
  nothing. These two are fixed (`[Input.Mouse] MenuClicks=false` switches them off): `Mouse1` and `Mouse2`
  of the ini are the bindings for play, where the right button stays headshot mode. Checked in a real-time
  run: a right click posted to the window in Profile Options left it for the main menu.
- All of that about menus is the dual_analog scheme, which knows when the player is being steered. The psp
  scheme knows less: it leaves the mouse free, with the same confirm and back clicks, while the main menu's
  scene is loaded (from the start of the game until a mission begins), and cannot tell a cutscene or the pause
  screen from play, so the mouse stays captured there.
- While the mouse is captured the view keeps the pitch it was given (`HoldPitch`). Without it the engine
  levels the view again whenever the player moves, as it does for a thumb stick.
- Movement is measured three ways and the one that is alive is used: raw input from a device that reports
  movement, raw input from a device that reports positions (the change of position), and, when no raw input
  arrives at all, the change of the cursor's own position. A cursor that has wandered more than a quarter of
  the window from its middle is put back there, so none of them runs out of screen.
- A replay never captures the mouse and takes no keyboard or mouse input while it plays. Press or move
  something then and the title says `a replay is driving the input: keyboard and mouse are ignored until it
  ends`. When the replay has ended the player has the input and a click captures. A session with the script
  channel (`run_live.ps1`) never captures; a click there is answered with `scripted session: the mouse is not
  captured`. Deterministic runs read no host input at all.

### Input test (what does the game receive?)

`[Input] ShowInput=true`, or F3 while playing, adds a line to the title bar, for example

```
mouse captured, F1 frees it | dual_analog | input from player | mouse captured, last report raw relative,
moved +12 -3 by raw relative, applied as that angle | sent buttons 8000 move 128,0 look 128,128 |
pad: xbox slot 0 | last device: keyboard
```

`last report` is the kind of the last raw mouse report that carried movement, captured or not. `moved ... by`
is the last movement that reached the game, in counts, and where it was measured (`raw relative`, `raw
absolute`, `cursor`), then how it is applied: `as that angle` (`Exact=true`) or `as stick travel`. `sent` is
the pad of the last sample: the PSP button mask (8000 Square, 4000 Cross,
2000 Circle, 1000 Triangle, 0200 R, 0100 L, 0008 Start) and the two sticks, 128 at rest. `input from` names
who owned that sample: the player, a replay or a script. `pad` is the pad in use (`xbox slot 0` to `3`,
`playstation 054C:0CE6`, `none`) and `last device` what the on-screen prompts follow (`pad` or `keyboard`,
which stands for keyboard and mouse). When the game exits the log (`last_run.log` of `play.ps1`, `run.log`
of the run scripts) has the totals:

```
[input] mouse: raw input registered, captures=2, raw relative reports=1031, raw absolute reports=149,
        samples measured by the cursor=0, travel=8633,1536 counts, last movement by raw relative, ...
[input] keyboard: key presses=13; presses and movements ignored while a replay or script had the input=0; ...
[input] gamepad: pad in use xbox slot 0; pads that arrived: xbox-style=1, playstation=0; the pad in use changed 1 times;
        a PlayStation pad that fell silent was put to rest 0 times
[input] device used last: pad; host samples with the pad=5210, with keyboard and mouse=388, changes=3
[input] vibration: calls by the game=0, settings sent to a pad=0
```

### Mouse on a streamed or remote PC

The pointer of a streaming client is not a mouse. A cloud PC streamed with Shadow, for example, behaves like
this: while the cursor is visible the client sends positions (raw absolute reports from `BladeSysVirtHID`
collection 2); once the game hides and confines the cursor it switches by itself to movement (raw relative
reports from collection 3, about 110 per second). So on such a PC the title line should read
`last report raw absolute` before the click and `moved ... by raw relative` after it. If it stays
`raw absolute` while captured, turning still works but stops when the pointer reaches the edge of the client's
own screen: switch the client to its relative or "locked" mouse mode. If no raw report arrives at all the
line reads `by cursor`; that works too, a little coarser. If `moved` stays at `+0 +0` while you move the mouse,
nothing reaches the game: check that the title says `mouse captured`, and that no replay is driving the input.

## Gamepads

**Nothing in this section has been tried with a real controller yet.** Everything below is unit-tested with made-up pad states and hand-made reports
(`tools/pc_input_test.cpp`); the last list of the section says what only a real pad can confirm.

- **Which pads.** Xbox-style pads through XInput, all four slots (`xinput9_1_0`). PlayStation pads
  (DualShock 4: product ids `05C4`, `09CC`, wireless adapter `0BA0`; DualSense `0CE6`, DualSense Edge
  `0DF2`; any other device of vendor `054C` that is a game pad) directly, with no helper program: the game
  window registers for the Raw Input reports of HID game pads and joysticks (usage page 1, usages 5 and 4,
  also while another window has the focus, as XInput does), and each report is read with the system's HID
  parser (`hid.dll`) from the device's own description. An Xbox-style pad also shows up there (its device
  path has `IG_`) and is skipped, it is read through XInput. So is a controller of Microsoft's own (vendor
  `045E`) whose path lacks that mark: it is an Xbox pad all the same, and the log says `[input] gamepad:
  HID 045E:pppp 'name' is a Microsoft pad: it is read through XInput` (if XInput does not list it, it is
  not read at all). A HID pad of another make is not read; the log says `[input] gamepad: HID vvvv:pppp
  'name' ignored (not a PlayStation pad; use an XInput-compatible mode)`. `[Input.Gamepad]
  PlayStationPads=false` switches the Raw Input path off.
- **A PlayStation pad's buttons go by position** through the same `[Input.Gamepad]` table: Cross is `A`,
  Circle `B`, Square `X`, Triangle `Y`, L1 `LB`, R1 `RB`, L2 `LT`, R2 `RT` (by their axes, so
  `TriggerThreshold` applies), L3 `LS`, R3 `RS`, Share or Create `Back`, Options `Start`, the D-pad the
  D-pad. PS, the touch pad's click and mute are not mapped. An optional section `[Input.Gamepad.PlayStation]`
  with any of the sixteen button lines gives a PlayStation pad targets of its own for those buttons.
- **One pad at a time: the pad in use is the one touched last.** With `Index=auto` every pad is read, and
  a sample is taken from one of them, never added up across pads. Another pad becomes the pad in use when
  it is touched while the pad in use has been untouched for 250 ms. So a second pad can be picked up at any
  time, and one pad that shows up twice (a PlayStation pad that Steam Input, DS4Windows or a streaming
  client also presents as an Xbox-style pad) presses once and does not flip between its two appearances.
  `Index=0` to `3` reads only the pad in that XInput slot, and then no PlayStation pad. Before this, the
  first slot that answered was kept for good and a second pad was never read.
- **What "touched" means.** A button held, a trigger past `TriggerThreshold`, or a stick: one that is
  pushed further out than its dead zone and than 0.45 of its travel, or one that is past its dead zone and
  has moved by more than 0.1 from where it last stood. The dead zone alone is not the measure: a worn stick rests a little past it for good, and such a pad lying on the desk would keep
  itself the pad in use, block a second pad, and hold the on-screen prompts on the pad while the player
  uses keyboard and mouse. **What the game gets from a stick is unchanged**: `DeadzoneLeft` and
  `DeadzoneRight` as configured, so a stick that rests past its dead zone still moves or turns the player
  until the dead zone is raised. The price of the rule: a stick held still between the dead zone and 0.45
  (a slow walk) counts as touched only in the sample it moved in. While it is held like that, a button on a
  second pad takes the input after the 250 ms, and a mouse that moves enough to count (see "The device
  used last") brings the keyboard's pictures; the next button, or the stick moving by 0.1, brings both
  back. If real pads show this in play, the place a stick "last stood" should be taken only after it has
  been still for a while.
- **Hot-plug.** The XInput slots are asked on a worker thread at start, every two seconds after that
  whether a pad is there or not, and at once when Windows reports a device change or a pad stops answering;
  a slot that stops answering is given up after one failed read. The look every two seconds is what finds
  a second pad that arrives without a device-change message (measured with made-up slots: found 2.0 s after
  it arrived). A PlayStation pad arrives and leaves with a Raw Input notice. The log says
  `[input] gamepad: Xbox-style pad, XInput slot 0`, `[input] gamepad: PlayStation pad 054C:0CE6 'Wireless
  Controller' usb, 15 buttons 6 axes hat`, `... removed` and `[input] gamepad: none`. These lines come with
  the first sample the player owns, so a run that is all replay prints none.
- **The device used last** is what the on-screen prompts follow with `[Input] Prompts=auto`
  (`host/input_device.hpp`, read by the prompt pictures). Per sample the player owns: the keyboard and
  mouse count as used when a bound key is down or was tapped, a mouse button or the wheel was used, or the
  mouse moved more than 4 counts within a tenth of a second while captured (more than 40 pixels over the
  window while free, so a nudged mouse does not change the prompts); the pad counts when the pad in use is
  touched (above: a button, a trigger, a stick pushed well out or moved; not a stick that merely rests past
  its dead zone). A sample with both, or with neither, changes nothing.
  A pad that is connected before anything at all was touched counts as the device, so a game started with a
  pad shows the pad's buttons from the first menu on. The pad's kind (Xbox-style or PlayStation) is that of
  the pad in use and is kept after it is unplugged. Samples of a replay, of the script channel and of a
  deterministic run say nothing about the device: they never reach this code.
- **A PlayStation pad that says nothing.** Pads report in one of two ways. A pad that streams sends its
  state every few milliseconds, also when nothing changed (both pads by USB, and over Bluetooth once they
  send their long reports). A pad that reports only changes sends a report when something on it changed
  and nothing while a stick is held still (a Bluetooth pad in its plain report mode may). Which kind a pad
  is, is measured from its reports, not assumed: it streams once 24 of its reports repeated the state of
  the report before them, each at most 0.1 s after it. A report that changes something says nothing either
  way; a repeat after a longer pause starts the count again.
  - A pad that streams and then says nothing is out of range or switched off, and Windows may take seconds
    to remove it. After 0.5 s of silence, or after 20 of its own report intervals if that is longer (0.8 s
    for a pad that reports every 40 ms), it is taken to be at rest, so the player does not keep running.
    The log says so once per pad: `[input] gamepad: PlayStation pad 054C:09CC has sent nothing for 0.50 s
    after a report every 4 ms: taken to be at rest until it reports again`, and the exit summary counts
    it. Its next report brings its state back, and it has to show again that it streams before its silence
    means anything.
  - A pad that reports only changes is never put to rest by silence: a stick held for five seconds stays
    held. Only its removal lets go (the Raw Input notice; an Xbox-style pad: one failed read).
  - The silence is counted in the game's own samples (at most 0.1 s each), so a pause of the game's thread,
    during which the pad's reports wait in the window's queue, is not taken for the pad's.
  - The report interval is timed when the game takes the reports out of the window's queue (no more often
    than every 8 ms, and right before a sample), not when the pad sent them. A pad that sends every 4 ms
    arrives several reports at a time, and the interval comes out as the time between two looks at the
    queue. So `a report every N ms` in the log line is not the pad's own rate, and the limit can come out
    longer than half a second (20 x 33 ms = 0.67 s if the reports were taken only 30 times a second),
    never shorter. Timing a report by its message time would fix the figure; not done.
- **Prompts and glyphs.** `[Input] Prompts` = `auto` (the device used last), `keyboard`, `gamepad`, or `psp`
  (the game's own PSP pictures; `scripts/unattended.ini` and `replays/mouse.ini` say `psp`).
  `[Input.Gamepad] Glyphs` = `auto` (by the pad in use), `xbox` or `playstation`: the kind the pad is taken
  for, both for its pictures and for which table it uses. A PlayStation pad that arrives as an Xbox-style
  pad through a helper program cannot be told from one; `Glyphs=playstation` is the answer. The pictures
  themselves are described under "On-screen prompts" below.
- **Vibration.** The engine has the call all the way down to its pad's `SetControllerVibration`
  (guest `0x088248A8`), which is empty on the PSP; the host puts a function over it
  (`host/pad_rumble_hook.hpp`) that hands the motor and the strength to the pad in use: `XInputSetState`
  for an Xbox-style pad, an output report for a PlayStation pad on USB (DualShock 4 report `05`, DualSense
  report `02`; **not written for Bluetooth**, which needs another report). `[Input.Gamepad] Rumble` (0 to
  1) scales it, 0 is off. The motors stop when the pad stops being the pad in use, when another window has
  the focus, while a replay or a script has the input, and when the game ends. **The game was measured
  never to call it** in 105 seconds of Story 1-1 (shooting, kills, dives, the player knocked down), so
  today nothing vibrates; the exit log counts the calls (`[input] vibration: calls by the game=..`) and
  the first sixteen are logged with their values, which is how a mission that does call it will show.
  Which of the call's two motors is the large one is a guess (as on a PlayStation 2 pad: 0 small, 1
  large). `CHILI_RUMBLE_TEST=1` in the environment runs both motors for half a second when a pad becomes
  the pad in use: the way to see that vibration reaches a pad at all. In a deterministic run the function
  is not installed (a host function over guest code moves that run's guest clock).
- **`CHILI_HID_DUMP=1`** logs, for each HID game pad, what the device says about itself and then its
  reports: the first four, and after them every report whose buttons, hat or axes changed, up to 200 per
  pad. A line has the bytes in hex, what the system's parser read, what the known byte layout read, and
  the logical buttons:
  `[input] hid 054C:0CE6 report 812 (64 bytes): 01 80 7f ... | system: buttons 0002 hat 8 x +0.00 ... |
  dualsense usb report 01: buttons 0002 ... | pad buttons 0400`. This is the way to check the reader
  against a real pad and to make test fixtures of its reports. With the packaged game: open a command
  prompt in its folder, `set CHILI_HID_DUMP=1`, start `ChiliConCarnage.exe` from that prompt, press every
  button once and move both sticks and triggers, quit; the lines are in `logs\ChiliConCarnage.log`.
  (`CHILI_RUMBLE_TEST=1` is set the same way.)
- **Two readers of a PlayStation pad's report.** The system's parser reads the USB report and the plain
  Bluetooth report (report `01`). The long Bluetooth reports (`11` of the DualShock 4, `31` of the
  DualSense, sent once another program has asked the pad for its calibration) are vendor data to the
  system; they are read by their known byte layout, and only when their CRC-32 is right. Where both readers
  apply the game uses the system's and counts the reports on which the two differ (one log line the first
  time, the count when the pad leaves or the game ends: `readings that differed=..`).
- **`CHILI_FAKE_XINPUT=<file>`** makes up Xbox-style pads for a check of the real program on a PC without a
  controller (`host/fake_xinput.hpp`): the four XInput slots answer from a text file, one line per pad,
  `<slot 0-3> <buttons> [<left trigger> <right trigger> [<lx> <ly> <rx> <ry>]]` with the buttons as XInput's
  bits in hexadecimal (`1000` A, `2000` B, `4000` X, `8000` Y, `0100` LB, `0200` RB, `0040` LS, `0080` RS,
  `0010` Start, `0020` Back, `0001` to `0008` the D-pad), triggers 0 to 255, sticks -32768 to 32767. A slot
  without a line has no pad; no file, no pad. A script changes the pad by replacing the file in one step.
  Everything behind the slot's answer is the program's own code. The motor values sent to such a pad are
  logged (`[input] made-up pad, XInput slot 0: motors 65535 65535 of 65535`). A PlayStation pad cannot be
  made up this way; its path is tested in `tools/pc_input_test.cpp` with hand-made reports.

What only a real controller can confirm:

1. An Xbox-style pad: that it is found at start and when plugged in later, in which slot, and that
   unplugging it is noticed; the feel of the dead zones (0.24) and of the right stick's response.
2. Two pads at once: that the one picked up takes over after a quarter of a second, and that nothing is
   pressed twice.
3. A DualShock 4 and a DualSense by USB: that Windows lists them as HID game pads, that the arrival line
   names them, that every button, the D-pad, both sticks and both triggers arrive as the table above says
   (`CHILI_HID_DUMP=1`), and that `readings that differed` stays 0.
4. The same two over Bluetooth, before and after another program (Steam, DS4Windows) has switched them to
   their long reports; that the CRC-32 check accepts those reports.
5. A PlayStation pad behind Steam Input, DS4Windows or a streaming client: that it is not doubled and what
   kind it arrives as.
6. Vibration: `CHILI_RUMBLE_TEST=1` on an Xbox-style pad and on each PlayStation pad by USB (the report
   bytes were never sent to a pad), and whether any mission makes the game call it.
7. That the prompts do not flicker with a pad and a mouse both at rest on the desk.
8. That touching the pad changes the on-screen buttons to the pad's about a quarter of a second later and a
   key or the mouse changes them back, that an Xbox-style pad gives Xbox pictures and a DualShock 4 or
   DualSense PlayStation pictures, and that the button pictured is the one that does the action.
9. How a real PlayStation pad reports in each mode (USB, plain Bluetooth, long Bluetooth reports): whether
   it streams or reports only changes. The pad's line at removal and at exit has `reports=..`: hundreds
   for every second it was connected when it streams, a few per touch when it reports only changes; the
   silence line above, if it ever appears, says the same. The rule was tested with made-up report timings
   only. That a held stick stays held on each of them, and that a pad carried out of range lets go.
10. A stick that rests off its centre: 0.45 and 0.1 (above) were chosen, not measured on a worn pad.
11. A Microsoft controller that Windows lists as a HID game pad without the XInput mark (if there is such a
    case): that XInput has it.
12. That the look at all four XInput slots every two seconds, on its own thread, does not hold up the
    game's read of the pad in use (if XInput lets only one call in at a time, the game's read waits for the
    look at an empty slot). With no pad, a look at the four slots took 0.4 ms on average and 2.8 ms at most
    (`chili_pc_input_test` prints it); with a pad connected it has not been measured.

## On-screen prompts

The game shows a PSP button in three ways, all of them data: pictures in its text font (the tutorial lines),
five small sprites beside the menu's legend words (Select, Back, Continue, Start, Ok, Change), and the Cross
sprite again as the "use" marker. Seven textures carry all of it: the two font sheets and the five sprites.
While the game runs, each of those pictures is replaced by the picture of **what is bound to that PSP button
on the device used last** (`host/prompt_style.hpp` picks the style and the binding, `host/prompt_glyphs.*`
paints, `host/gpu/ge_texture_mods.*` puts the pictures into the textures when they are uploaded). Nothing in
the game's memory changes, and the game's own Button Layout A/B still decides which PSP button a line names.

- **`[Input] Prompts`**: `auto` (default) follows the device used last (see "Gamepads"): the pictures change
  a quarter of a second after a key, a mouse button or a pad button was touched, and at most twice a second,
  so a nudged mouse does not make them flicker. `keyboard` and `gamepad` fix the style. `psp` keeps the game's
  own pictures.
- **When the style is decided.** At the start of every game frame, and with every pad sample the player
  owns when no frame has started for 50 ms (some screens read the pad without ending a frame). A new style
  shows in the next picture the game draws. **The briefing of a mission is a still picture**: the game
  draws it once and then only reads the pad until Continue is pressed (measured on Story 1-1: no display
  list in 12 seconds, 60 pad samples a second). A device change there switches the style, as the log says
  (`[prompts] frame 585: Xbox pad -> keyboard and mouse; 0 textures left the cache, 0 drawn again ...`),
  but the Continue picture stays that of the device in use when the briefing appeared, and the screen
  after it has the new one. Drawing that still picture again would mean running the game's last display
  lists a second time; not done.
- **`[Input.Gamepad] Glyphs`**: `auto` draws a pad's buttons as the pad in use has them: Xbox letters, or
  PlayStation shapes with L1, R1, L2, R2, L3, R3, Options and Share. `xbox` or `playstation` fixes it. With
  `Prompts=gamepad` and no pad seen yet the pictures are the Xbox ones.
- **Which binding is pictured** when several inputs press the same PSP button. Keyboard and mouse: the left
  or right mouse button before a key, a key before the other mouse buttons and the wheel, and for the D-pad
  an arrow key before other keys; otherwise the first key in the ini's line. A pad: the control of the same
  name when it is bound to it (the D-pad to the D-pad, Start to Start, Back to Select), otherwise the first
  button in the order of the ini. Nothing bound: a grey cap with a dash. A PlayStation pad with a section
  `[Input.Gamepad.PlayStation]` is pictured by that section.
- With the default bindings (Total Overdose PC):

  | PSP button (what it does) | Keyboard and mouse | Xbox-style pad | PlayStation pad |
  |---|---|---|---|
  | Cross (jump, use, confirm) | Space, as a sign | A | Cross |
  | Circle (weapon wheel; back in menus) | E (F with Modern) | B | Circle |
  | Square (shoot) | a mouse with the left button red | RT | R2 |
  | Triangle (reload) | R | X | Square |
  | L (target objects) | Q | LS | L3 |
  | R (headshot mode) | a mouse with the right button red | LT | L2 |
  | D-pad left, right, down | arrow key caps (Tab rewinds too, but the arrow is pictured) | the D-pad with that arm red | the same |
  | Analog stick | W A S D as a cluster, or the one key | the left stick with an arrow | the same |
  | Start, Select | Esc; ` as the layout in use prints that key (the key left of 1 on a US keyboard; Tab with Modern) | Menu, View | Options, Share |

  Space, Enter, Shift, Backspace and the arrow keys are drawn as signs; other long names are shortened
  (Esc, Tab, Ctrl, Ins, Del, PgUp, PgDn, Caps, N7). A letter key shows the letter the keyboard layout in use
  prints on it. The PSP's own pictures of HOME, the volume, display and sound buttons stay as they are.
- **Size.** The font sheets are painted at 1, 2 or 4 times their size (internal scale 1, 2 to 3, 4 to 8), the
  sprites at the internal scale. At internal scale 1 a picture in a help line is about 10 pixels wide: single
  letters, discs, the mouse and arrows can be read there, three and four letter key names and LS / RS cannot.
  They read from scale 2 (LS, RS) and scale 4 (four-letter names) up.
- **Where it does not apply.** The software renderer (`[Rendering] Backend=software`) draws from the game's
  memory and shows the PSP pictures. Scripted runs (a replay, the script channel, deterministic mode) show
  the PSP pictures whatever the ini says, because their frame images are compared; `scripts/unattended.ini`
  and `replays/mouse.ini` also say `Prompts=psp`. Without Direct2D or DirectWrite the PSP pictures stay and
  the log says why. A texture mod's own pictures of the seven textures are painted over unless `Prompts=psp`
  (`mods/TEXTURES.md`).
- **Text is not changed.** No string of the game names a button. Three help lines say "analog stick"
  (shoot-dodge, climbing and reverse with Button Layout B), which is wrong only for keyboard and mouse.
- **A binding change needs a restart**: the game reads `ChiliNative.ini` once.
- The log says at start `[prompts] pictures of keyboard and mouse, following the input in use ...`, at every
  change `[prompts] frame 350: Xbox pad -> keyboard and mouse; 4 textures left the cache, 4 drawn again with
  new pictures in 0.83 ms` (three frames after the change, or a quarter of a second after it where no
  frame ends), and at exit the count of changes, what they cost, and how often the style was looked at
  from the pad-sample path.

For scripted checks (`host/prompt_style.hpp`): `CHILI_PROMPTS=keyboard|xbox|playstation|psp` forces a style
in any run; `CHILI_PROMPTS=auto` makes a scripted run follow the settings and the device like a player's run
(with an ini that says `Prompts=auto`): a deterministic one by its frame count, 30 frames to the second, a
scripted run in real time by the real clock, and there the samples the player's devices own are looked at
between frames too; `CHILI_PROMPTS_AT=<frame>:<style>,...` changes the forced style at
those frames; `CHILI_PROMPT_DEVICE_AT=<frame>:<keyboard|xbox|playstation>,...` sets the device used last at
those frames; `CHILI_PROMPT_KEY_ALL=1` and `CHILI_PROMPT_PAINTER=off` measure the texture keying and show
the fallback.

Not seen in any run: the "use" marker, the Triangle sprite, the death and mission-complete screens, the
trophy room, El Macho, Hangman, Fiesta, the lobby, vehicles; help lines other than "shoot" and "weapons"; a
PlayStation pad's pictures chosen by a real pad (they were forced or set by `Glyphs`). Only Story 1-1 was
run: a later mission that draws the font with another palette would show the PSP pictures there.

## The settings page inside the game

The game's own menus have a line `Settings` that opens a page of the port's settings (`host\settings_page.hpp`
the page, `host\settings_entry.hpp` and `host\menu_entry_blocks.hpp` the lines): a top page with `Display`, `Graphics`, `Controls`, `Textures & mods`
and `Sound`, and a page for each; `Controls` has `Keyboard & mouse` and `Gamepad` below it. It is worked with the
PSP's buttons through the bindings, like the game's own menus, so its legends are the pictures of whatever is
bound, for the device in use.

Where the line is:

| Where | What opens the page |
|---|---|
| Main menu | The last line, `Settings`, where the PSP game had `Fiesta` (its wireless mode, which this port does not have): Down three times, then Cross. The word is the game's own, so it is `Paramètres` and `Ajustes` in the game's other two languages |
| Pause menu of a story mission and of Hangman | The last line, `Settings` (under `Chicken out` in a story mission, where the four lines above it stand 12 pixels higher than in the PSP game; Hangman's pause menu has two lines above it and was not run) |
| Pause menu of another kind (the Macho and style challenges, Fiesta) | `<button> Settings` under the pause box. The button is whatever is bound to Triangle: R, the pad's X, Square on a PlayStation pad. A click on those words does the same |
| For tests, in no legend | F2 in the main menu, Profile Options, the game's own Settings screen and the pause menu |

Circle (or a right click) on the page's top page closes the page, and so does Start on any of its pages; the
game's menu is then where it was, on the same line. In the pause menu Start closes the page only; the next
Start or Circle resumes the game.
`Profile Options > Settings` is still the game's own screen (`Music`, `Invert Y-axis`, `Button Layout`,
`Subtitles`) and saves into the profile; the page saves into `ChiliNative.ini` and touches no profile and no
save. So the game has two things called `Settings`: its own under Profile Options, the port's in the main menu
and the pause menu. In Profile Options
`Sync Highscores` is dark and the cursor skips it: like `Fiesta` it started the PSP's wireless play, and either
line ended the program before.

| | Keys (default) | Xbox-style pad | PlayStation pad | Mouse |
|---|---|---|---|---|
| Move the focus | Up, Down (W, S) | D-pad, left stick | D-pad, left stick | the row under the pointer; the wheel, a row per notch |
| Open a section, flip a switch, the next value | Space, F, Enter (Modern: Space, E, Enter) | A | Cross | left click |
| Change a value | Left, Right (A, D) | D-pad left, right; LB, RB | D-pad; L1, R1 | left click (the next value); a slider held and moved |
| Back a page; Cancel | E, Backspace (Modern: F, Backspace) | B | Circle | right click, or a click on the legend |
| Close the page | Escape | Start | Options | |

- A held direction acts again after 0.4 seconds and then ten times a second (counted in pad samples: 12 and 3
  at 30 game frames per second, 24 and 6 at 60). A held Left or Right goes on with a slider only.
- A switch, a choice and the row over several keys (`Quality`, `Textures`, `HD pack`) are written to
  `ChiliNative.ini` when they change, a slider when its key or the mouse button is let go, and everything
  before the page closes. Every row of `Keyboard & mouse` and `Gamepad`, `Prompts` and `Button pictures` are in
  force at once, and so is `Screen`: it switches the window to borderless full screen and back as F11 does,
  shows the window's own state (so it follows F11), and writes how the next start begins. The other rows are
  written and wait for the next start: such a value is drawn in the headline's orange with the chili after
  it, and the reason line says `After a restart.` for three seconds when the change is made.
- A row that cannot be changed where the page is open is at half brightness with the closed lock after its
  name (in focus it pulses between the halves of the two reds). Tried, it says why for three seconds: in a
  mission every row that takes a restart (`Change it in the main menu.`), `Auto limit` while the resolution
  is not `Auto` (`Only used with Auto.`), `Look speed` and `Look key speed` with `PSP original` (`Not used
  with PSP original.`), `Textures` without a pack, `HD pack` while `Textures` is `Standard`, `Use the gamepad`
  while the pad is the device in use (`In use.`), and a key a `CHILI_*` variable overrides. Every reason a
  row can have in a mission fits one line of the pause box.
- Seven rows are shown, in the menu and in the pause box; only `Keyboard & mouse` and `Gamepad` (13 rows
  each) scroll, by whole rows. A small gold pointer above the first row or under the last says the list goes
  on; a click on it moves the list by a page.
- A slider whose value in the file lies outside the row's range (a mouse speed of 1.5 written by hand; the
  row ends at 1) shows that value; a step away from the range leaves it, a step towards it goes to the
  range's end.
- `Defaults` is the last row of a section and asks first (`Reset to the defaults?`, Ok or Cancel). On
  `Controls` it resets the two device pages too; the control style and the bindings stay.
- While the page is open the game gets a neutral pad (no button, both sticks in the middle), so its own menu
  stays on its line; the button that closed the page is kept from the game until it is let go.
- The mouse: the pointer is mapped through the picture's rectangle in the window, at any window size and in
  full screen. With `Use the mouse` off the page takes no mouse at all; with `Clicks in menus` off it takes
  the pointer and the wheel and no click.
- **What stays in the launcher**: the keys, mouse buttons and pad buttons of each action (rebinding), the
  list of mods and their order, the disc, the renderer, `Draw on a second processor core`, the game thread's
  priority, `Listen in the background`, the input test line, file logging, the folders and the Windows volume
  mixer. A row that waits for a restart (resolution, frame rate, anti-aliasing and the like) can be set on
  the page from the main menu; the game has no "restart now" yet, so it runs from the next start.
- The page's words are English in every language of the game; only the line `Settings` in the two menus
  follows the game's language.
- Direct3D 12 only: the software renderer draws no page, and the page does not open there.
- The look: in the pause menu the page fills the game's box in the tone that box has over a mission's
  picture (85, 80, 64 of 255; the game's own box is translucent, the page's has to hide the game's lines
  under it), and the top page's five lines stand in the middle as the pause menu's own do. The `Change`
  legend is the game's own left/right sprite when that is in the renderer's cache (after the game's own
  Settings screen was shown); otherwise one D-pad with both arms lit, put together from the left half of the
  font's D-pad-left picture and the right half of its D-pad-right picture, where those two are one pad (the
  PSP's own, an Xbox or PlayStation pad's), and two key caps side by side with keys.
- With a mod's own lettering (an HD pack that replaces the font sheet `5d0c6d1a663a8515`) the font's two
  padlocks and its chili are drawn new by the port, at eight times the game's size, into the same cells
  (`host\prompt_glyphs.cpp`, `GlyphKind::Lock` and `Chili`; `host\prompt_style.hpp`, `PromptMarks`): a pack
  can only enlarge the game's 26 x 30 pictures, and beside sharp letters they were the one soft thing. They
  are the game's marks everywhere it draws them (its mission lists too), in every prompt style. Without such
  a mod the game's own pictures stay.
- **The switch.** `[Game] SettingsInGame=false` in `ChiliNative.ini` (or `CHILI_PAGE=0` for one run) takes
  the two `Settings` lines, the legend and the page out of a player's run: the menus are the PSP game's,
  `Fiesta` and `Sync Highscores` are back and do nothing (the game's network start is still answered
  "false"). The log says `[menu] no Settings line in the game's menus and no settings page in this run`.
- Not live yet, although the game could: `Show frames early`, `Texture filter`, `Sharp textures`, light
  anti-aliasing, `Textures`, `Dump textures`, `Sound buffer`; and with a rebuilt renderer `Resolution`,
  multisampling and `Ambient shadows`. They are the first thing for the next version.

## Mouse latency and aim

What happens between the mouse and the picture, and what the port does about each step. All numbers are
frame-exact from deterministic runs (30 game frames per second unless said; `[Game] FrameLock=1` gives 60,
the default for a player).

| Step | Before | Now | Setting |
|---|---|---|---|
| The mouse report waits for the window's message pump | up to 8 ms | taken right before the pad is read | none |
| The turn waits for the game's physics step | 0-70 ms, and the angle is quantised: a 90 degree flick turned 63 or 127, a short tap 63 or nothing, 63% as far while running | none: the player turns by exactly the angle moved, in the frame that reads it, standing or running, and stops with the mouse | `[Input.Mouse] Exact=true` |
| The turn in a dive | lost in the air of a dive forward, left or right (60-110 frames in slow motion) and on the ground after it; a dive backwards turned at 79% | exactly the angle in every direction, in the air and on the ground after it | the same; on the ground `[Input] ExactTurn=true` |
| The right stick and the look keys | the same 0-70 ms and quanta: a flick of 90 turned 63 or 127 and coasted up to 61 after release, a one-frame tap 63 or nothing | the rate times the frame, as an angle: 90.2 in the three frames, coast 0, a tap 30 | `[Input] ExactTurn=true` |
| The pitch | exact to the second stick's byte, the rounding carried into later frames, capped at 1,500 degrees per second | exactly the angle, in the frame that reads it, no cap | `[Input.Mouse] Exact=true` |
| The camera eases behind the player | half of a turn after 100-167 ms, 90% after 300-333 ms | within 3.6 degrees of the player at all times: 90% of a flick in the frame of its last movement | `[Game] Camera=tight` |
| The camera's tilt eases behind the pitch | half of a pitch after two frames, 90% after four, then part of it given back over the next second or two where a level tilts the view | 79% in the frame of the movement, 90% in the frame of the last movement of a flick, all of it one or two frames later, and it stays | `[Game] Camera=tight` |
| A drawn scene waits for the game's copy to the screen | shown at the second frame end after the pad read | shown at the first one, as soon as it is drawn | `[Rendering] EarlyPresent=true` |
| The frame's wait sits between the pad read and the picture | the scene was drawn after the frame-end waits: a pad read reached the window 33 ms later at 30 frames per second whatever the work took | the scene is drawn at the frame boundary, before the waits (`RENDERING.md`, early draw): the pad is read right after the waits, then update, draw, present | `[Rendering] EarlyPresent=true` |

- `Exact`: the player's character is moved by a physics controller that steps 15 times a second
  (`character_physic_ctrl`, step 0.0667 s) and takes the turn rate the look controls ask for once per step;
  the frames in between only move the player from one end of the step to the other (`character_root`). That
  is why a turn asked through the stick is held for 70 ms and comes in quanta of rate times 1/15 s. The mouse
  now bypasses the rate: when the player script reads `look_left` on foot, the host turns both ends of the
  step in progress by the mouse's angle about the vertical (`DualAnalog::turn_player`), so the frame's
  position between them, the next step and the node all carry it, through the game's own path. The pitch is
  answered as the pressure whose cube times the frame time is the angle. With the mouse the script's turn
  rate stays zero, so what the game does with that rate (its `Look.y`, turn animations) does not happen. A
  read of the turn controls from anywhere else in the game gets the mouse as a rate, as before.
  `Exact=false` brings the stick path back for the mouse.
- `ExactTurn` (`[Input]`, default `true`): the right stick and the look keys take the same path. Their rate
  (171 degrees per second times the pressure squared, the rate at which the player really turned) times the
  frame time is an angle, added to the mouse's, and both turn controls are then answered as zero. Measured
  with stick byte 235 for three frames, six starting frames each: 90.2 degrees (91.1 in one, the frame times
  of a deterministic run alternate between 33 and 34 ms), first movement in the frame of the input, nothing
  after release, the same while running; before 63.3 (four starts) or 126.6 (two), 50.6 or 101.3 running, up
  to 61 after release. A one-frame tap: 29.8 in that frame; before 63.3 or nothing. With `ExactTurn=false`
  the stick goes through the game's own turn rate again and nothing turns the player on the ground after a
  dive (below). `scripts/unattended.ini` says `false`: the reference state hashes of the dual_analog replays
  were made with the game's own turning, and turning on the ground after a dive needs one more host function
  over a guest address, which moves the guest clock of a deterministic run.
- A replay line carries the angle as two more fields (`replays/README.md`), so a recording of mouse play
  turns by the same angles when replayed.
- `Camera=tight` is two property lines the mod layer applies before the mods' own (`mods/README.md`):
  `CharacterCombatCam` and `CharacterNormalCam` get `MaxAngle = 0.02` (half turns: 3.6 degrees) and
  `AngleEaseInSpeed = 20`. `Camera=original` gives the game's own camera. The default follows the scheme of
  the ini file: tight with `dual_analog`, original with `psp`. Scripted runs (`scripts/unattended.ini`) name
  `original`, so reference state hashes are those of the game's camera.
- The view's pitch, also part of `Camera=tight`. The player script's look angles go to the player's camera
  controller (`camera_system`) as an orientation, and the controller's own current orientation went 10 a
  second times the frame time of the way there every frame (`rotate_speed`; a third at 30 frames per second):
  half of a pitch after two frames, 90% after four. Now the current orientation is pitched by the mouse's
  angle in the frame that reads it (`DualAnalog::pitch_view`), so the camera does not fall behind by it; what
  else changes the look (the stick's pitch, the game levelling the view) is eased as before. Measured with
  29.49 degrees in three frames: the camera's elevation has 79% of a frame's angle in that frame, 93% of the
  flick in its last frame, 98% one frame later and then all 29.50, which stays; before it peaked at 22.9
  after six frames. The camera rig's own ease (`AngleEaseInSpeed`) is the part that is left, as for the turn.
- The part of a pitch the view gave back: in places a level asks for a tilted view while the player looks up
  or down (`camera_force_tilt_Ctrl`, only while the look is more than 0.1 rad off level; the start of
  Story 1-1 is such a place). The controller eases its tilt there at 1 a second and adds it to the view, so
  after a pitch the view rose by itself for a second or two (29.5 degrees asked: 22.9 after six frames, 14.3
  after seventeen and still falling). With the mouse the
  player looks up or down all the time; `Camera=tight` takes that tilt out as the controller starts its
  update. `Camera=original` keeps it. Vehicles and mounted guns have cameras of their own, not changed and
  not measured.
- `EarlyPresent` (Direct3D 12 only): in a mission the game draws each scene into a surface of its own and
  copies it to the screen at the next frame end. The host shows that surface as soon as its list has been
  drawn, which is one game frame earlier; the copy still happens and shows the same picture again. Menus,
  movies and loading screens draw straight to the screen and are not affected. Screenshots and frame images
  show the surface the window shows.
- One engine frame, in order: the pad is read, the scripts move the player and then the camera, the scene is
  built with that camera, and the frame end waits for the vertical blank (twice at 30 frames per second) and
  hands the scene to the renderer. The game drew the scene after those waits, so they sat between the pad
  read and the picture. With the early draw (`RENDERING.md`) the scene is drawn at the frame boundary,
  before the waits, and shown at once: wait, read the pad (the mouse is taken right before it), update, draw,
  present. What is left between the pad read and the present call is the frame's own work, at 30 and at 60.
  Add up to one frame of waiting for the next pad read (the mouse is read once per game frame): 33 ms at 30
  frames per second, 17 at 60.
- Measured in real time (the debug-speed build, nothing else running, with sound),
  pad read to present call in a mission at 30 game frames per second: median 33 ms before the early draw
  (90th percentile 34; the frame's work took 22.8 ms); with it 19 ms (90th percentile 29) where the game
  thread also draws (`GeThread=false`, work 19.8 ms), and 22 ms (29) with the list thread, where the game
  thread's own work is 17.0 ms and the picture follows about 5 ms after it. Sound: 84.9 blocks per second
  and the one underrun at the mission load in every one of those runs. That build cannot hold 60 (50 frames
  per second, 22 ms). On the optimized build (under the timing lock, with sound, internal scale 8): median 8 ms at 60 game frames per second (90th percentile 12, 99th 22) and 9 ms at 30
  (12, 22), with the game thread working 4.1 and 5.3 ms per frame; `mouse_play.replay` with the player's
  input settings at 30: 11 ms (15, 22). Every interactive run ends its log with that figure:
  `[render] pad read to present, mission pictures: count=.. mean=.. ms p50=.. p90=.. p99=.. max=..`.
  A picture is timed from the pad read of its own frame: a list carries the number of its pad read to its
  present, so a picture the list thread shows after the next pad read is not mistaken for a fast one.
- What a video stream adds on top, and the port cannot see: the desktop compositor (a windowed picture waits
  up to one refresh, 17 ms at 59 Hz; F11 full screen can skip it); the stream's capture of the composed
  desktop (up to another refresh), its encode, the network and the decode and display on the other side;
  and, in the other direction, the client's capture of the mouse and the network again. On a streamed PC that
  round trip is the larger part of what is felt and was not measured here: the port's own share is now the 0-17 ms
  wait for the next pad read plus the frame's work.

### Where the turn is applied

The player's move controller (`character_animation_ctrl`) has a state, and the physics controller a model
(0 on the ground, 1 in the air, 3 held to a wall by a stunt). Found with the input trace
(`CHILI_INPUT_TRACE`, columns `state`, `physics_model`, `physics_turns`) and replays that turn the mouse
through every move:

| Move | State, model | What the game does with a turn | What the port does with the angle |
|---|---|---|---|
| On foot: standing, running, strafing, shooting | 0, 3-7, 21; model 0 | its physics steps take the rate about every 70 ms | turns both ends of the step: exact, same frame |
| Jump | 8, 9, 11; model 1 in the air | does not turn in the air | the same: exact (the game's own stick did nothing there) |
| Dive, taking off | 27 left, 28 right, 29 back, 30 forward; model 0 | as on foot | the same: exact. In the dive's first frame the game sets the player to the camera's heading, and that frame's angle is the camera's small lag instead |
| Dive, in the air (60-110 frames, slow motion) | 27-30; model 1 | the move controller's own state (`character_Combat_Move_ctrl` state_11) turns the node every frame by the turn rate times 0.75 times the frame time and overwrites the physics step | answers the turn controls with the rate that makes the angle (180 degrees per second times the pressure squared, times 0.75): exact, same frame. The game counts the spin: `180� + SHOOT DODGE` |
| On the ground after a dive of his own, getting up (11-19 frames) | 52; model 0 | nothing turns him; his script (`character_player_ai` state_12) eases him to the camera's heading and holds the camera | turns the two orientations the script eases between and the camera's: player and view turn by the angle, plus the game's ease (`ExactTurn`) |
| Thrown by a blow (the men with shovels), 6-10 frames | 27-30; model 1 from the first frame | as in a dive | as in a dive |
| On the ground after a blow, getting up | 52; model 0 | nothing | nothing: the angle is dropped until he is up |
| Running up a wall and the flip off it; other moves with the player held in place | 57, 36; 56, 51 (not identified); model 3 | the game places the player | nothing: the angle is dropped. The dive off the wall is a dive |
| 34, then 19 for good (taken to be the player's death; not looked at) | 34, 19 | nothing | nothing |

- The script adds the turn pressures to a look angle of its own as well (`Look.y`: 5 radians a second times
  the difference of their squares, held to 108 degrees) and eases it back only while the player moves and
  the pitch is not held. On foot the port answers the turn pressures as zero and that angle stays zero. The
  rate a dive is given would pile it up to 108 degrees and, with `HoldPitch`, leave it there, so with
  `ExactTurn` it starts every frame from zero: in a dive it holds one frame's share (0.11 rad at 3 degrees
  a frame) and nothing afterwards. What the game does with that angle was not found; with it at 108 degrees
  on foot the camera and the player's stance looked the same.
- States 29 and 52 used to be taken for "knocked down": 29 is the dive backwards, so that dive got the mouse
  as a rate (79% of the angle) while the other three directions lost it from the moment the player left the
  ground, which is what "the mouse does nothing in a forward dive" was.
- `replays/mouse_dive_forward.replay`, `_back`, `_left`, `_right` (3 degrees per sample through a dive) and
  `mouse_play.replay` (38 seconds of varied play, the mouse always turning): the share of mouse samples in
  which the player's heading changed by exactly the angle, before and after, is in `replays/README.md`.
- The log of every run with the two-stick scheme ends with the count, for example
  `[input] frames with a turn to make=1147: player turned by exactly that angle=1034, turned on the ground
  after a dive (where the game also eases him to the view)=44; left to the game (stunt on a wall, knocked
  down), by move state: 51 x28, 52 x30; not exactly, by move state: 4 x2, 27 x1`. A move state under
  "not exactly" with a large count is a move the table above does not know yet.

## Keys in ChiliNative.ini

- `[Input]` `Version` (written by the game), `Scheme` (`dual_analog`, `psp`), `BackgroundInput`, `ShowInput`,
  `ExactTurn` (the right stick and the look keys as each frame's angle, and turning on the ground after a
  dive; default `true`), `Prompts` (`auto`, `keyboard`, `gamepad`, `psp`; default `auto`: the buttons the
  game shows on screen follow the device used last).
- `[Input.Keyboard]` one line per target, `target=key[,key...]`: `MoveForward MoveBack MoveLeft MoveRight Walk
  LookLeft LookRight LookUp LookDown Cross Circle Square Triangle L R DpadUp DpadDown DpadLeft DpadRight Start
  Select`; plus `WalkScale` (0.31-1), `LookKeySpeed` (0.05-1), `ToggleMouseCapture`, `ToggleInputDisplay`.
- `[Input.Mouse]` `Enabled`, `Capture` (`click`, `never`), `MenuClicks` (in menus, movies and cutscenes a
  left click is Cross and a right click Circle, whatever `Mouse1` and `Mouse2` say; default `true`),
  `Mouse1`-`Mouse5` (the buttons in play), `WheelUp`, `WheelDown`, `SensitivityX`, `SensitivityY` (degrees per count, 0.001-5), `InvertX`, `InvertY`, `HoldPitch`,
  `Exact` (the mouse as an angle, default `true`), `Smoothing` (0-0.95).
- `[Input.Gamepad]` `Enabled`, `Index` (`auto`: every pad is read and the one touched last is used; 0-3:
  only the pad in that XInput slot), `A B X Y LB RB LT RT LS RS Back Start DpadUp DpadDown DpadLeft
  DpadRight`, `DeadzoneLeft`, `DeadzoneRight` (0-0.9), `TriggerThreshold` (0.01-1),
  `CompensateEngineDeadzone`, `TurnSensitivity`, `LookSensitivity` (0-4; 1 is the engine's own 171 and 57
  degrees per second at full deflection), `InvertY`, `Glyphs` (`auto`, `xbox`, `playstation`; default
  `auto`), `PlayStationPads` (`true`, `false`; default `true`), `Rumble` (0-1; default 1).
- `[Input.Gamepad.PlayStation]` (optional; absent by default) any of the sixteen button lines `A B X Y LB RB
  LT RT LS RS Back Start DpadUp DpadDown DpadLeft DpadRight`: the target a PlayStation pad has for that
  button instead of `[Input.Gamepad]`'s. A button the section leaves out keeps the `[Input.Gamepad]` target.

- Elsewhere in the same file: `[Game] Camera` (`tight`, `original`: the camera within 3.6 degrees of the
  player, the view pitching with the mouse, no level tilt), `[Game] FrameLock` and `[Rendering] EarlyPresent`
  (`RENDERING.md`).

A line the game does not understand is reported on stderr at start and the rest of the file still applies.

## What the engine does with it

Measured with `replays/dual_analog_move.replay` and `dual_analog_pitch.replay` (see `replays/README.md`) and
with mouse input injected through Windows (`SendInput`) into a real-time run:

- The left stick's X axis strafes without turning; the right stick's X axis turns without moving.
- The player turns at 0.95 of the rate the script asks for, so the PSP stick's full pressure is 171 degrees
  per second. The pressure is not clamped: three times the pressure is nine times the rate.
- A mouse count is a fixed angle. With `Exact=false`, 400 counts to the right at 0.08 degrees per count
  turned the player 32.97 degrees, 200 counts down pitched the view 0.280 rad (16.0 degrees), and the same
  200 back up returned it to level. With `Exact=true` (the default) the angle is exact: 90 degrees asked in
  three frames turned the player 90.000 from every one of six starting frames, standing and running, a
  30 degree movement in one frame turned 30.000 in that frame, and 29.49 degrees of pitch gave 29.490.
- The game's own camera (`Camera=original`) eases in behind the player: each frame it closes 7.5 times the
  frame time of the gap, a quarter at 30 frames per second, which is the camera script's `AngleEaseInSpeed`
  of 7.5. At 170 degrees per second it trails by about 16 degrees, and after a 200 degree flick it needs
  about a third of a second to catch up. `Camera=tight` bounds the gap at 3.6 degrees.
- The pitch is the script's `Look.x`, limited to 45 degrees up and down; the camera tilts by all of it
  (`Camera=tight`; with the game's own camera, where a level tilts the view, by less). While the player
  moves and both look controls read exactly zero, the script eases
  the pitch back to level (4 per second, the compare at guest `0x08B7641C`). `HoldPitch` answers a look-down
  at rest to that one check as a pressure of 0.0001, whose cube, the pitch rate, is nothing.
- The player script also runs behind the main menu, where its character is part of the backdrop, and asks
  for the look controls there; for a sample or two it also asks while the game starts. Neither is play:
  the host reads the scene tree (`Missions/MainMenuScene` has children only while the menu is up) and wants
  four samples of look queries before the mouse is captured.
- A player on the ground after a dive or a blow (move state 52) does not turn in the game; the script still
  reads the controls. See "Where the turn is applied".
- Jumping while strafing dives sideways: the dive takes its direction from the left stick.

Two places in the player script needed help, both in `host/dual_analog.cpp`: it asked for `look_down` and
dropped the answer, and it built the left-stick vector from the look controls.

## Not there yet

- Number keys pick the previous and next weapon; choosing a weapon class directly needs the quick-select
  sequence (Circle, then a face button) as a macro.
- Feel: the exact turn (mouse, stick, dives), the view's pitch and the tight camera were measured through
  replays; the sensitivity defaults (0.08 degrees per count) have had little real use.
- With the exact turn the game's own turn rate stays zero: if the game plays a turn-on-the-spot animation
  from that rate, it no longer does. Not looked at.
- The dive's first frame (the game sets the player to the camera's heading) and the last frames of a dive
  off a wall, where the game has stopped turning the player before he lands, do not take the angle.
- Only Story 1-1 has been played. Ladders, water, moves of later missions: the log's count by move state
  will show them. In a vehicle or at a mounted gun the game reads its controls elsewhere; if it reads the
  look controls there the mouse arrives as a rate, otherwise not at all. Not tried.
- No physical gamepad has been used yet, of either kind: see the list at the end of "Gamepads".
- A PlayStation pad does not vibrate over Bluetooth (no report is written), and the game itself asks for
  no vibration in what was run. Vibration made by the host from game events (a hit, a shot) is not there.
- The pad's sticks cannot be rebound: left moves, right turns and looks.
- Menus take clicks as Cross (left) and Circle (right) only; there is no pointer in them: a click confirms
  or goes back wherever the mouse is, and the entries are chosen with the arrow keys.
- The briefing of a mission keeps the button picture of the device that was in use when it appeared (it is
  a still picture, see "On-screen prompts").
- A pad whose stick rests past its dead zone still feeds that stick to the game (raise `DeadzoneLeft` or
  `DeadzoneRight`); it only no longer counts as "in use".
