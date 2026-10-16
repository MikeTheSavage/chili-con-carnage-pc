# Controls

Every binding can be changed on the launcher's Controls page (two tabs: Keyboard & mouse, Gamepad): click a
chip and press what you want there; a right click on a chip takes it away. The bindings are kept in
`ChiliNative.ini`; [profiles/chili/CONTROLS.md](profiles/chili/CONTROLS.md) lists every line of it and how the
input works inside.

## Playing

| Action | Keyboard and mouse | Xbox-style controller | PlayStation controller |
|---|---|---|---|
| Move and strafe | W A S D (hold X to walk) | Left stick | Left stick |
| Turn and look | Mouse (J L I K without one) | Right stick | Right stick |
| Shoot | Left mouse button, G | RT | R2 |
| Headshot mode (hold) | Right mouse button, V | LT | L2 |
| Shoot-dodge, jump, use, confirm | Space, Left Shift, F, Enter | A, Y | Cross, Triangle |
| Reload, loco move | R, Left Ctrl | X | Square |
| Weapon quick-select; back in menus | E, Backspace, middle mouse button | B | Circle |
| Target objects, exit stunt | Q, C | Left stick click | L3 |
| Previous / next weapon | Mouse wheel, Left / Right | LB / RB | L1 / R1 |
| Rewind | Tab, Down | D-pad down | D-pad down |
| Forks up / down (in a vehicle) | Page Up / Page Down | D-pad up / down | D-pad up / down |
| Pause | Escape, P | Start | Options |
| Select | ` (the key left of 1 on a US keyboard) | Back | Share or Create |
| Free or capture the mouse | F1 | | |
| Full screen and back | F11, Alt+Enter | | |

- The first click in the window captures the mouse for looking. F1, Alt+Tab, a menu, a cutscene or another
  window frees it.
- In menus the arrow keys or the D-pad move; Enter, a left click or A (Cross) confirms; Backspace, E, a right
  click or B (Circle) goes back.
- These are the keys Total Overdose (2005, made on the same engine) had on PC, for every action the two games
  share; this game's own actions sit on keys beside them. Total Overdose's 1 to 8 (weapon classes) have no
  match: this game picks a weapon class with its quick-select (E held, then a face button's key).
- Five taps of Shift raise Windows' Sticky Keys question; it can be switched off in Settings >
  Accessibility > Keyboard.
- The launcher's "Control style" (and "Style" on the settings page inside the game) also offers "Modern"
  (Space, E, Enter jump and use; F weapon wheel; 1 / 2 weapons; Z rewind) and "PSP original", the PSP's own
  layout: left and right turn, hold L to strafe, no looking up or down (`[Input] Layout` in
  `ChiliNative.ini`). Keys you changed yourself stay when you switch.

## Controllers

- An Xbox-style controller works without setup, in any of the four XInput slots.
- A PlayStation controller (DualShock 4 or DualSense, by cable or Bluetooth) works without a helper program.
- With two controllers connected, the one you touched last is used. Controllers can be plugged in and taken
  out while the game runs.
- The buttons the game shows on screen (menus, tutorial lines) follow what you touched last: key caps and a
  mouse, Xbox buttons, or PlayStation shapes.
- No real controller has been tried with this version yet. If yours does not work, the "Gamepads" part of
  [profiles/chili/CONTROLS.md](profiles/chili/CONTROLS.md) says how to make a log that shows why.

## The settings page inside the game

The game's own menus have a line `Settings`: the last line of the main menu (where the PSP game had
`Fiesta`) and of the pause menu (under `Chicken out`).

| | Keys | Controller | Mouse |
|---|---|---|---|
| Move | Up, Down | D-pad or stick | point at a row; the wheel |
| Open, switch, next value | Space, Enter | A / Cross | left click |
| Change a value | Left, Right | D-pad left, right | click; hold and move a slider |
| Back | Backspace, E | B / Circle | right click |
| Close the page | Escape | Start / Options | |

Changes are saved at once. Controls, prompts and the window mode apply at once; the rest from the next start
of the game. `[Game] SettingsInGame=false` in `ChiliNative.ini` takes the line and the page away again.
