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
| Headshot mode | Right mouse button, V | LT | L2 |
| Jump, dive, use, confirm | Space, E, Enter | A, Y | Cross, Triangle |
| Reload | R | X | Square |
| Weapon wheel; back in menus | F, Backspace, middle mouse button | B | Circle |
| Target objects, exit stunt | Q | Left stick click | L3 |
| Previous / next weapon | Mouse wheel, 1 / 2, Left / Right | LB / RB | L1 / R1 |
| Rewind | Z, Down | D-pad down | D-pad down |
| Pause | Escape | Start | Options |
| Select | Tab | Back | Share or Create |
| Free or capture the mouse | F1 | | |
| Full screen and back | F11, Alt+Enter | | |

- The first click in the window captures the mouse for looking. F1, Alt+Tab, a menu, a cutscene or another
  window frees it.
- In menus the arrow keys or the D-pad move; Enter, a left click or A (Cross) confirms; Backspace, F, a right
  click or B (Circle) goes back.
- `[Input] Scheme=psp` in `ChiliNative.ini` (the launcher's "Control style: PSP original") gives the PSP's
  own layout: left and right turn, hold L to strafe, no looking up or down.

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
| Back | Backspace, F | B / Circle | right click |
| Close the page | Escape | Start / Options | |

Changes are saved at once. Controls, prompts and the window mode apply at once; the rest from the next start
of the game. `[Game] SettingsInGame=false` in `ChiliNative.ini` takes the line and the page away again.
