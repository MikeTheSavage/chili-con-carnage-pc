#pragma once
#include "controller_runtime.hpp"
#include "dual_analog_controls.hpp"
#include "ini_document.hpp"
#include "input_device.hpp"
#include "input_layout.hpp"
#include "settings_table.hpp"
#include "pad_sample.hpp"
#include "hid_pad.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <Xinput.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
namespace chili {
// PC input: keyboard, mouse and gamepads mapped onto the PSP pad the
// game reads (16 buttons and one analog stick) plus a second stick for the
// dual-analog scheme. Bindings come from ChiliNative.ini; default_input_ini()
// is both the file written on first run and the built-in defaults.
//
// Gamepads: Xbox-style pads through XInput (all four slots), PlayStation pads
// through Raw Input and the HID parser (hid_pad.hpp). Every pad is read into
// a PadSample (pad_sample.hpp) and one pad at a time is the pad in use: the one
// touched last. What the player used last, keyboard and mouse or a pad, and
// the kind of that pad go to input_device_state() for the on-screen prompts.
//
// Scheme psp is the game's own PSP layout: the stick's Y axis runs, its X axis
// turns the player (the camera follows), holding L turns X into strafing. Mouse
// X and the gamepad's right stick X are fed in as turn rate on the stick's X
// axis, where the player turns at no more than about 170 degrees per second.
// Scheme dual_analog uses the engine's two-stick path (dual_analog.hpp): the
// left stick and the move keys run and strafe, and the right stick, the mouse
// and the look keys go out as ControllerInput::rx/ry, which turn the player and
// pitch the view at the rate dual_analog_controls.hpp gives each byte.
// Either way the engine's response curve and its 30% stick dead zone are undone
// here, so a mouse count is a fixed angle.
enum class InputScheme : std::uint8_t { Psp, DualAnalog };
// Where a mouse movement was measured: raw input from a device that reports
// movement, raw input from one that reports positions, or, when no raw report
// arrives at all, the change of the cursor's own position.
enum class MouseSource : std::uint8_t { None, RawRelative, RawAbsolute, Cursor };
inline const char *mouse_source_name(MouseSource source) noexcept {
    switch (source) {
    case MouseSource::RawRelative: return "raw relative";
    case MouseSource::RawAbsolute: return "raw absolute";
    case MouseSource::Cursor: return "cursor";
    default: return "none";
    }
}
// Written into ChiliNative.ini as [Input] Version. A file the game wrote
// without it, or with a lower number, comes from an older build. 3: the
// [Input] Layout line and the control style Total Overdose PC.
inline constexpr int input_ini_version = 3;
inline const char *scheme_name(InputScheme scheme) noexcept { return scheme == InputScheme::DualAnalog ? "dual_analog" : "psp"; }
inline constexpr InputScheme layout_scheme(InputLayout layout) noexcept { return layout_is_psp(layout) ? InputScheme::Psp : InputScheme::DualAnalog; }
// The control style a file means when it has no [Input] Layout line: the PSP's with Scheme=psp, else Total Overdose PC.
inline constexpr InputLayout default_layout(InputScheme scheme) noexcept { return scheme == InputScheme::Psp ? InputLayout::Psp : InputLayout::TodPc; }
inline std::optional<InputScheme> scheme_from_name(std::string_view name) noexcept {
    if (name == "psp") return InputScheme::Psp;
    if (name == "dual_analog") return InputScheme::DualAnalog;
    return {};
}
enum class PadTarget : std::uint8_t {
    None, Select, Start, DpadUp, DpadRight, DpadDown, DpadLeft, L, R, Triangle, Circle, Cross, Square,
    MoveForward, MoveBack, MoveLeft, MoveRight, Walk, LookLeft, LookRight, LookUp, LookDown
};
inline constexpr float engine_stick_deadzone = DualAnalogControls::dead_zone;
inline constexpr float engine_turn_degrees_per_second = DualAnalogControls::turn_degrees_per_second;
inline constexpr float engine_pitch_degrees_per_second = DualAnalogControls::pitch_degrees_per_second;

// The first-run file and built-in defaults of each control style (input_layout.hpp). Total Overdose PC
// has Left Shift on Cross (jump) and Left Ctrl on Triangle (the loco move) as Total Overdose had them, each
// beside a plain key of the same button (Space, F, Enter; R), so nothing needs a modifier key; no default
// is Alt, a Windows key, Caps Lock or Num Lock. Five taps of Shift raise Sticky Keys, Ctrl+Escape opens the
// Start menu (CONTROLS.md). Modern and PSP original have no modifier key at all.
#define CHILI_INPUT_INI_HEAD \
    "; Chili Con Carnage PC input. Delete this file to get the defaults back.\n" \
    "; Key names: A-Z 0-9 F1-F12 Space Enter Escape Tab Backspace Up Down Left Right\n" \
    ";   LShift RShift LCtrl RCtrl LAlt RAlt Insert Delete Home End PageUp PageDown CapsLock\n" \
    ";   Comma Period Slash Semicolon Quote LBracket RBracket Backslash Minus Equals Grave Numpad0-Numpad9\n" \
    "; Letters mean the key at that position on a US keyboard, so WASD stays put on other layouts.\n" \
    "; Shift, Ctrl and Alt can be bound, but Windows has shortcuts on them (Sticky Keys, language switch, Alt+Tab).\n" \
    "; Targets: Cross Circle Square Triangle L R Start Select DpadUp DpadDown DpadLeft DpadRight\n" \
    ";   MoveForward MoveBack MoveLeft MoveRight Walk LookLeft LookRight LookUp LookDown None\n"
// The two comment lines above Version=; input_migration_comments() replaces an older build's two with them.
#define CHILI_INPUT_INI_VERSION_COMMENT_1 "; Written by the game. A file of the game's with a lower number or none comes from an older build: once,"
#define CHILI_INPUT_INI_VERSION_COMMENT_2 ";   its input settings are brought up to date, and the old file is kept as ChiliNative.ini.bak."
#define CHILI_INPUT_INI_VERSION \
    CHILI_INPUT_INI_VERSION_COMMENT_1 "\n" \
    CHILI_INPUT_INI_VERSION_COMMENT_2 "\n" \
    "Version=3\n"
#define CHILI_INPUT_INI_LAYOUT_COMMENT \
    "; Control style: tod_pc (Total Overdose PC: its keys and mouse), modern (the port's own keys) or psp (the\n" \
    ";   PSP's layout). A binding this file leaves out takes the style's default. The launcher's Controls page and\n" \
    ";   the game's Settings page change it and give every binding that still has the old style's key the new one.\n"
#define CHILI_INPUT_INI_SCHEME_FOLLOWS ";   The control style sets it: dual_analog with tod_pc and modern, psp with psp."
#define CHILI_INPUT_INI_COMMON \
    "; Read input while another window has the focus.\n" \
    "BackgroundInput=false\n" \
    "; Input test: the title bar shows whether the mouse is captured, where its movement is measured (raw\n" \
    ";   relative, raw absolute or cursor) with the last counts, and the buttons and sticks sent to the game.\n" \
    ";   F3 switches it while playing; the same summary is printed when the game exits.\n" \
    "ShowInput=false\n" \
    "; The buttons the game shows on screen (its hints and the legends of its menus). auto: keys and mouse or\n" \
    ";   the pad's buttons, whichever was used last. keyboard, gamepad: always those. psp: the game's own PSP pictures.\n" \
    "Prompts=auto\n"
#define CHILI_INPUT_INI_EXACT_TURN \
    "; true: the right stick and the look keys turn the player by each frame's angle, like the mouse, and\n" \
    ";   mouse and stick go on turning him on the ground after a dive.\n" \
    "; false: the stick goes through the game's own turn rate, which it takes about every 70 ms and holds,\n" \
    ";   and on the ground after a dive nothing turns the player, as on the PSP.\n" \
    "ExactTurn=true\n"
#define CHILI_INPUT_INI_TOGGLES \
    "; Frees or captures the mouse.\n" \
    "ToggleMouseCapture=F1\n" \
    "; Switches the input test line in the title bar.\n" \
    "ToggleInputDisplay=F3\n"
#define CHILI_INPUT_INI_KEYBOARD_TAIL \
    "; Stick deflection while Walk is held (0.31 to 1).\n" \
    "WalkScale=0.6\n" \
    "; Look keys: 1 = the engine's own full rate, about 170 degrees per second of turn and 57 of pitch (0.05 to 1).\n" \
    "LookKeySpeed=0.7\n" \
    CHILI_INPUT_INI_TOGGLES
#define CHILI_INPUT_INI_MOUSE_BUTTONS \
    "Mouse1=Square\n" \
    "Mouse2=R\n" \
    "Mouse3=Circle\n" \
    "Mouse4=None\n" \
    "Mouse5=None\n" \
    "WheelUp=DpadRight\n" \
    "WheelDown=DpadLeft\n"
#define CHILI_INPUT_INI_DUAL_MOUSE \
    "[Input.Mouse]\n" \
    "Enabled=true\n" \
    "; click: a click in the window captures the mouse (hidden and kept inside the window); it is freed in\n" \
    ";   menus and cutscenes, with F1, with Alt+Tab and when another window takes the focus.\n" \
    "; never: buttons work without capturing, no mouse look.\n" \
    "Capture=click\n" \
    "; In menus, movies and cutscenes a left click is Cross (confirm, skip) and a right click Circle (back),\n" \
    ";   whatever Mouse1 and Mouse2 are bound to below; those bindings are for play.\n" \
    "MenuClicks=true\n" \
    CHILI_INPUT_INI_MOUSE_BUTTONS \
    "; Degrees the player turns (X) and the view pitches (Y) per mouse count (0.001 to 5).\n" \
    "SensitivityX=0.08\n" \
    "SensitivityY=0.08\n" \
    "InvertX=false\n" \
    "InvertY=false\n" \
    "; While the mouse is captured the view keeps the pitch it was given. false: the game levels the view\n" \
    ";   again whenever the player moves, as it does for a thumb stick.\n" \
    "HoldPitch=true\n" \
    "; true: the player turns and the view pitches by exactly the angle the mouse moved, in the frame it moved.\n" \
    ";   false: the mouse pushes the second stick, which the game reads about every 70 ms and holds.\n" \
    "Exact=true\n" \
    "; 0 = none. Up to 0.95 averages the movement over more frames.\n" \
    "Smoothing=0\n"
#define CHILI_INPUT_INI_PAD_TAIL \
    "Back=Select\n" \
    "Start=Start\n" \
    "DpadUp=DpadUp\n" \
    "DpadDown=DpadDown\n" \
    "DpadLeft=DpadLeft\n" \
    "DpadRight=DpadRight\n" \
    "; Stick travel that counts as centred (0 to 0.9) and how far a trigger must be pulled (0.01 to 1).\n" \
    "DeadzoneLeft=0.24\n" \
    "DeadzoneRight=0.24\n" \
    "TriggerThreshold=0.12\n" \
    "; Start moving as soon as the stick leaves its dead zone instead of after the engine's extra 30%.\n" \
    "CompensateEngineDeadzone=true\n"
#define CHILI_INPUT_INI_PAD_HEAD \
    "Enabled=true\n" \
    "; auto: every pad is read and the one touched last is used. 0-3: only the pad in that XInput slot.\n" \
    "Index=auto\n"
#define CHILI_INPUT_INI_PAD_END \
    "; How the pad's buttons are drawn on screen: auto (by the pad in use), xbox or playstation.\n" \
    "Glyphs=auto\n" \
    "; Read PlayStation pads (DualShock 4, DualSense) directly, by USB or Bluetooth. false: XInput pads only.\n" \
    "PlayStationPads=true\n" \
    "; Vibration strength, 0 (off) to 1.\n" \
    "Rumble=1\n" \
    "; A section [Input.Gamepad.PlayStation] with any of the sixteen button lines above gives a PlayStation pad\n" \
    ";   targets of its own. Its buttons go by position: Cross is A, Circle B, Square X, Triangle Y, L1 LB, R1 RB,\n" \
    ";   L2 LT, R2 RT, L3 LS, R3 RS, Share or Create is Back, Options is Start.\n"
#define CHILI_INPUT_INI_DUAL_PAD \
    "[Input.Gamepad]\n" \
    CHILI_INPUT_INI_PAD_HEAD \
    "A=Cross\n" \
    "B=Circle\n" \
    "X=Triangle\n" \
    "Y=Cross\n" \
    "LB=DpadLeft\n" \
    "RB=DpadRight\n" \
    "LT=R\n" \
    "RT=Square\n" \
    "LS=L\n" \
    "RS=None\n" \
    CHILI_INPUT_INI_PAD_TAIL \
    "; Right stick at full deflection: 1 = the engine's own rate, about 170 degrees per second of turn (X) and\n" \
    ";   57 of pitch (Y). 0 to 4.\n" \
    "TurnSensitivity=1.0\n" \
    "LookSensitivity=1.0\n" \
    "InvertY=false\n" \
    CHILI_INPUT_INI_PAD_END
#define CHILI_INPUT_INI_GAME_LINES \
    "; In the game: Cross jump/dive/use/confirm, Square shoot, Triangle reload, Circle weapon wheel and back in menus,\n" \
    ";   L target objects/exit stunt, R headshot mode, DpadLeft/DpadRight change weapon, DpadDown rewind, Start pause.\n"
// The comments above Cross and Triangle in Total Overdose PC's text (they name what the game does, so they
// hold for every style); input_migration_comments() puts them where an older build's file has its own.
#define CHILI_INPUT_INI_CROSS_COMMENT "; Jump, dive, use, enter a vehicle, confirm: the game has them all on Cross."
#define CHILI_INPUT_INI_TRIANGLE_COMMENT "; Reload and the loco move: the game has both on Triangle."
inline const char *default_input_ini(InputLayout layout) {
    if (layout == InputLayout::Psp) return
        CHILI_INPUT_INI_HEAD
        "; In the game: Cross jump/dive/use/confirm, Square shoot, Triangle reload, Circle weapon wheel and back in menus,\n"
        ";   L strafe/target, R headshot mode, DpadLeft/DpadRight change weapon, DpadDown rewind, Start pause.\n"
        "\n"
        "[Input]\n"
        CHILI_INPUT_INI_VERSION
        CHILI_INPUT_INI_LAYOUT_COMMENT
        "Layout=psp\n"
        "; psp: left/right turns the player and holding L strafes, as on the PSP.\n"
        "; dual_analog: WASD or the left stick moves and strafes, the mouse or the right stick turns and looks.\n"
        CHILI_INPUT_INI_SCHEME_FOLLOWS "\n"
        "Scheme=psp\n"
        CHILI_INPUT_INI_COMMON
        "\n"
        "[Input.Keyboard]\n"
        "; target=key[,key...]\n"
        "MoveForward=W\n"
        "MoveBack=S\n"
        "; A and D turn; hold L to strafe.\n"
        "MoveLeft=A\n"
        "MoveRight=D\n"
        "Walk=X\n"
        "Cross=Space,E,Enter\n"
        "Circle=F,Backspace\n"
        "Square=G\n"
        "Triangle=R\n"
        "L=Q\n"
        "R=V\n"
        "DpadUp=Up\n"
        "DpadDown=Down,Z\n"
        "; Previous and next weapon.\n"
        "DpadLeft=Left,1\n"
        "DpadRight=Right,2\n"
        "Start=Escape\n"
        "Select=Tab\n"
        "; Stick deflection while Walk is held (0.31 to 1).\n"
        "WalkScale=0.6\n"
        CHILI_INPUT_INI_TOGGLES
        "\n"
        "[Input.Mouse]\n"
        "Enabled=true\n"
        "; click: a click in the window captures the mouse (hidden and kept inside the window); F1, Alt+Tab or\n"
        ";   another window taking the focus frees it. never: buttons work without capturing, no mouse turn.\n"
        "Capture=click\n"
        CHILI_INPUT_INI_MOUSE_BUTTONS
        "; Degrees the player turns per mouse count (0.001 to 5); this layout turns at about 170 degrees per second at most.\n"
        "SensitivityX=0.08\n"
        "InvertX=false\n"
        "\n"
        "[Input.Gamepad]\n"
        CHILI_INPUT_INI_PAD_HEAD
        "A=Cross\n"
        "B=Circle\n"
        "X=Square\n"
        "Y=Triangle\n"
        "LB=L\n"
        "RB=R\n"
        "LT=R\n"
        "RT=Square\n"
        "LS=None\n"
        "RS=None\n"
        CHILI_INPUT_INI_PAD_TAIL
        "; Right stick X turns the player; 1 = the engine's full turn rate at full deflection (0 to 1).\n"
        "TurnSensitivity=1.0\n"
        CHILI_INPUT_INI_PAD_END;
    if (layout == InputLayout::Modern) return
        CHILI_INPUT_INI_HEAD
        CHILI_INPUT_INI_GAME_LINES
        "\n"
        "[Input]\n"
        CHILI_INPUT_INI_VERSION
        CHILI_INPUT_INI_LAYOUT_COMMENT
        "Layout=modern\n"
        "; dual_analog: WASD or the left stick moves and strafes, the mouse or the right stick turns and looks.\n"
        "; psp: left/right turns the player and holding L strafes, as on the PSP; no looking up or down.\n"
        CHILI_INPUT_INI_SCHEME_FOLLOWS "\n"
        "Scheme=dual_analog\n"
        CHILI_INPUT_INI_COMMON
        CHILI_INPUT_INI_EXACT_TURN
        "\n"
        "[Input.Keyboard]\n"
        "; target=key[,key...]\n"
        "MoveForward=W\n"
        "MoveBack=S\n"
        "MoveLeft=A\n"
        "MoveRight=D\n"
        "; Turning and looking without a mouse.\n"
        "LookLeft=J\n"
        "LookRight=L\n"
        "LookUp=I\n"
        "LookDown=K\n"
        "Walk=X\n"
        "; Space jumps and dives, E uses: the game has both on Cross.\n"
        "Cross=Space,E,Enter\n"
        "Circle=F,Backspace\n"
        "; Shoot without a mouse.\n"
        "Square=G\n"
        "; Reload.\n"
        "Triangle=R\n"
        "L=Q\n"
        "R=V\n"
        "DpadUp=Up\n"
        "DpadDown=Down,Z\n"
        "; Previous and next weapon.\n"
        "DpadLeft=Left,1\n"
        "DpadRight=Right,2\n"
        "; Pause.\n"
        "Start=Escape\n"
        "Select=Tab\n"
        CHILI_INPUT_INI_KEYBOARD_TAIL
        "\n"
        CHILI_INPUT_INI_DUAL_MOUSE
        "\n"
        CHILI_INPUT_INI_DUAL_PAD;
    return
        CHILI_INPUT_INI_HEAD
        CHILI_INPUT_INI_GAME_LINES
        "\n"
        "[Input]\n"
        CHILI_INPUT_INI_VERSION
        CHILI_INPUT_INI_LAYOUT_COMMENT
        "Layout=tod_pc\n"
        "; dual_analog: WASD or the left stick moves and strafes, the mouse or the right stick turns and looks.\n"
        "; psp: left/right turns the player and holding L strafes, as on the PSP; no looking up or down.\n"
        CHILI_INPUT_INI_SCHEME_FOLLOWS "\n"
        "Scheme=dual_analog\n"
        CHILI_INPUT_INI_COMMON
        CHILI_INPUT_INI_EXACT_TURN
        "\n"
        "[Input.Keyboard]\n"
        "; target=key[,key...]\n"
        "MoveForward=W\n"
        "MoveBack=S\n"
        "MoveLeft=A\n"
        "MoveRight=D\n"
        "; Turning and looking without a mouse.\n"
        "LookLeft=J\n"
        "LookRight=L\n"
        "LookUp=I\n"
        "LookDown=K\n"
        "Walk=X\n"
        CHILI_INPUT_INI_CROSS_COMMENT "\n"
        "Cross=Space,F,Enter,LShift\n"
        "; The weapon quick-select (hold, then a face button), leave a vehicle, back in menus, skip a cutscene.\n"
        "Circle=E,Backspace\n"
        "; Shoot without a mouse.\n"
        "Square=G\n"
        CHILI_INPUT_INI_TRIANGLE_COMMENT "\n"
        "Triangle=R,LCtrl\n"
        "; Target objects; held in a vehicle, lean out (then Cross dives out).\n"
        "L=Q,C\n"
        "; Headshot mode without a mouse; the handbrake in a vehicle.\n"
        "R=V\n"
        "; In a vehicle also the forks: up, and down with rewind.\n"
        "DpadUp=Up,PageUp\n"
        "DpadDown=Tab,Down,PageDown\n"
        "; Previous and next weapon.\n"
        "DpadLeft=Left\n"
        "DpadRight=Right\n"
        "; Pause.\n"
        "Start=Escape,P\n"
        "Select=Grave\n"
        CHILI_INPUT_INI_KEYBOARD_TAIL
        "\n"
        CHILI_INPUT_INI_DUAL_MOUSE
        "\n"
        CHILI_INPUT_INI_DUAL_PAD;
}
// The defaults of a scheme: Total Overdose PC with dual_analog, the PSP's own layout with psp.
inline const char *default_input_ini(InputScheme scheme = InputScheme::DualAnalog) { return default_input_ini(default_layout(scheme)); }

// Comment lines of an older build's file that its migration replaces (section, the old line, the new one):
// `always` ones in every file brought up to Version 3, the others only where the bindings take Total
// Overdose PC's. A line is replaced only when it is exactly the old text; the launcher has the same list
// (launcher/src/Model.cs, InputMigration; tools/settings_table_test.cpp holds the two alike).
struct InputCommentChange { const char *section, *before, *after; bool always; };
inline constexpr InputCommentChange input_migration_comments[]{
    {"Input", "; Written by the game. A file of the game's without it comes from an older build: its input sections are", CHILI_INPUT_INI_VERSION_COMMENT_1, true},
    {"Input", ";   replaced by the current defaults once and the old file is kept as ChiliNative.ini.bak.", CHILI_INPUT_INI_VERSION_COMMENT_2, true},
    {"Input", ";   Delete this file after changing the scheme to get that scheme's default bindings.", CHILI_INPUT_INI_SCHEME_FOLLOWS, true},
    {"Input.Keyboard", "; Space jumps and dives, E uses: the game has both on Cross.", CHILI_INPUT_INI_CROSS_COMMENT, false},
    {"Input.Keyboard", "; Reload.", CHILI_INPUT_INI_TRIANGLE_COMMENT, false},
};
#undef CHILI_INPUT_INI_HEAD
#undef CHILI_INPUT_INI_VERSION_COMMENT_1
#undef CHILI_INPUT_INI_VERSION_COMMENT_2
#undef CHILI_INPUT_INI_VERSION
#undef CHILI_INPUT_INI_LAYOUT_COMMENT
#undef CHILI_INPUT_INI_SCHEME_FOLLOWS
#undef CHILI_INPUT_INI_COMMON
#undef CHILI_INPUT_INI_EXACT_TURN
#undef CHILI_INPUT_INI_TOGGLES
#undef CHILI_INPUT_INI_KEYBOARD_TAIL
#undef CHILI_INPUT_INI_MOUSE_BUTTONS
#undef CHILI_INPUT_INI_DUAL_MOUSE
#undef CHILI_INPUT_INI_PAD_TAIL
#undef CHILI_INPUT_INI_PAD_HEAD
#undef CHILI_INPUT_INI_PAD_END
#undef CHILI_INPUT_INI_DUAL_PAD
#undef CHILI_INPUT_INI_GAME_LINES
#undef CHILI_INPUT_INI_CROSS_COMMENT
#undef CHILI_INPUT_INI_TRIANGLE_COMMENT

struct PcInputConfig {
    struct KeyBinding { int vk; PadTarget target; };
    InputScheme scheme{InputScheme::DualAnalog};
    // [Input] Layout: the control style whose defaults a binding the file leaves out takes. `layout_named`:
    // the text had the line (without it a file means default_layout(scheme)).
    InputLayout layout{InputLayout::TodPc};
    bool layout_named{};
    std::vector<KeyBinding> keys;
    int toggle_capture_vk{}, toggle_display_vk{};
    // [Input] Version of the file the settings came from; 0 when it has none.
    int version{};
    float walk_scale{0.6f}, look_key_speed{0.7f};
    bool background_input{}, show_input{};
    bool mouse_enabled{true}, mouse_capture_on_click{true}, mouse_invert_x{}, mouse_invert_y{}, mouse_menu_clicks{true}, mouse_hold_pitch{true};
    // dual_analog: the mouse goes out as ControllerInput::turn/pitch, an angle, instead of as second-stick travel.
    bool mouse_exact{true};
    // dual_analog: the second stick's turn (and the look keys') is applied as each frame's angle, and the
    // player turns on the ground after a dive ([Input] ExactTurn).
    bool exact_turn{true};
    std::array<PadTarget, 5> mouse_buttons{};
    PadTarget wheel_up{}, wheel_down{};
    float mouse_degrees_per_count{0.08f}, mouse_degrees_per_count_y{0.08f}, mouse_smoothing{};
    bool pad_enabled{true}, compensate_engine_deadzone{true}, pad_invert_y{};
    // -1: every pad is read and the one touched last is used. 0-3: only that XInput slot.
    int pad_index{-1};
    // In XInput button-bit order for the first 14 named buttons, then LT, RT (PadSample::Button).
    std::array<PadTarget, 16> pad_buttons{};
    float deadzone_left{0.24f}, deadzone_right{0.24f}, trigger_threshold{0.12f}, pad_turn_sensitivity{1.0f}, pad_look_sensitivity{1.0f};
    // [Input] Prompts and [Input.Gamepad] Glyphs: how the game's on-screen buttons are drawn (input_device.hpp).
    PromptsSetting prompts{PromptsSetting::Auto};
    GlyphsSetting glyphs{GlyphsSetting::Auto};
    // [Input.Gamepad] PlayStationPads: PlayStation pads are read through Raw Input (hid_pad.hpp).
    bool playstation_pads{true};
    // [Input.Gamepad] Rumble: vibration strength, 0 = off.
    float rumble{1.0f};
    // [Input.Gamepad.PlayStation]: the targets a PlayStation pad has instead of pad_buttons, for the buttons
    // that section names (bit i of playstation_named). Read them through pad_target().
    std::array<PadTarget, 16> playstation_buttons{};
    std::uint16_t playstation_named{};

    static constexpr std::array<std::pair<const char *, int>, 14> pad_button_names{{
        {"DpadUp", XINPUT_GAMEPAD_DPAD_UP}, {"DpadDown", XINPUT_GAMEPAD_DPAD_DOWN}, {"DpadLeft", XINPUT_GAMEPAD_DPAD_LEFT},
        {"DpadRight", XINPUT_GAMEPAD_DPAD_RIGHT}, {"Start", XINPUT_GAMEPAD_START}, {"Back", XINPUT_GAMEPAD_BACK},
        {"LS", XINPUT_GAMEPAD_LEFT_THUMB}, {"RS", XINPUT_GAMEPAD_RIGHT_THUMB}, {"LB", XINPUT_GAMEPAD_LEFT_SHOULDER},
        {"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER}, {"A", XINPUT_GAMEPAD_A}, {"B", XINPUT_GAMEPAD_B},
        {"X", XINPUT_GAMEPAD_X}, {"Y", XINPUT_GAMEPAD_Y}}};
    static constexpr std::size_t pad_left_trigger = 14u, pad_right_trigger = 15u;
    // The index (0-15) of a pad button's ini name: the fourteen above, LT and RT.
    static std::optional<std::size_t> pad_button_from_name(std::string_view name) {
        for (std::size_t i = 0; i < pad_button_names.size(); ++i) if (same_text(name, pad_button_names[i].first)) return i;
        if (same_text(name, "LT")) return pad_left_trigger;
        if (same_text(name, "RT")) return pad_right_trigger;
        return {};
    }
    static const char *pad_button_name(std::size_t button) noexcept {
        return button < pad_button_names.size() ? pad_button_names[button].first : button == pad_left_trigger ? "LT" : button == pad_right_trigger ? "RT" : "";
    }
    // The kind a pad is taken for, by its buttons' pictures and by its table: Glyphs when that names a kind,
    // otherwise what the pad is.
    PadKind kind_of(PadKind pad) const noexcept {
        return glyphs == GlyphsSetting::Xbox ? PadKind::Xbox : glyphs == GlyphsSetting::PlayStation ? PadKind::PlayStation : pad;
    }
    // The target of logical pad button `button` (0-15) on a pad taken for `kind`: [Input.Gamepad], or for a
    // PlayStation pad the line of [Input.Gamepad.PlayStation] when that section has one for the button.
    PadTarget pad_target(std::size_t button, PadKind kind) const noexcept {
        if (button >= pad_buttons.size()) return PadTarget::None;
        return kind == PadKind::PlayStation && (playstation_named >> button & 1u) ? playstation_buttons[button] : pad_buttons[button];
    }
    // Whether two configurations show the same prompt pictures: Prompts, Glyphs and every table a picture is
    // chosen from (prompt_style.hpp: the keys and mouse inputs of a target, a pad's two tables).
    bool same_prompts(const PcInputConfig &other) const noexcept {
        return prompts == other.prompts && glyphs == other.glyphs && scheme == other.scheme && mouse_enabled == other.mouse_enabled &&
            mouse_buttons == other.mouse_buttons && wheel_up == other.wheel_up && wheel_down == other.wheel_down && pad_buttons == other.pad_buttons &&
            playstation_buttons == other.playstation_buttons && playstation_named == other.playstation_named &&
            std::equal(keys.begin(), keys.end(), other.keys.begin(), other.keys.end(),
                [](const KeyBinding &a, const KeyBinding &b) { return a.vk == b.vk && a.target == b.target; });
    }
    // Whether two configurations are the same in everything.
    bool same_as(const PcInputConfig &other) const noexcept {
        return same_prompts(other) && layout == other.layout && toggle_capture_vk == other.toggle_capture_vk && toggle_display_vk == other.toggle_display_vk && version == other.version &&
            walk_scale == other.walk_scale && look_key_speed == other.look_key_speed && background_input == other.background_input && show_input == other.show_input &&
            mouse_capture_on_click == other.mouse_capture_on_click && mouse_invert_x == other.mouse_invert_x && mouse_invert_y == other.mouse_invert_y &&
            mouse_menu_clicks == other.mouse_menu_clicks && mouse_hold_pitch == other.mouse_hold_pitch && mouse_exact == other.mouse_exact && exact_turn == other.exact_turn &&
            mouse_degrees_per_count == other.mouse_degrees_per_count && mouse_degrees_per_count_y == other.mouse_degrees_per_count_y && mouse_smoothing == other.mouse_smoothing &&
            pad_enabled == other.pad_enabled && compensate_engine_deadzone == other.compensate_engine_deadzone && pad_invert_y == other.pad_invert_y && pad_index == other.pad_index &&
            deadzone_left == other.deadzone_left && deadzone_right == other.deadzone_right && trigger_threshold == other.trigger_threshold &&
            pad_turn_sensitivity == other.pad_turn_sensitivity && pad_look_sensitivity == other.pad_look_sensitivity && playstation_pads == other.playstation_pads && rumble == other.rumble;
    }
    // Hands the settings the prompt pictures follow to the shared state and says that the tables were (re)loaded.
    void publish() const noexcept {
        auto &shared = input_device_state();
        shared.prompts.store(prompts, std::memory_order_relaxed);
        shared.glyphs.store(glyphs, std::memory_order_relaxed);
        shared.bindings_revision.fetch_add(1u, std::memory_order_relaxed);
    }

    static bool same_text(std::string_view a, std::string_view b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
            const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
            return lower(x) == lower(y); });
    }
    static std::string_view trim(std::string_view text) {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) text.remove_prefix(1u);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1u);
        return text;
    }
    static std::optional<PadTarget> target_from_name(std::string_view name) {
        static constexpr std::pair<const char *, PadTarget> names[]{
            {"None", PadTarget::None}, {"Select", PadTarget::Select}, {"Start", PadTarget::Start}, {"DpadUp", PadTarget::DpadUp},
            {"DpadRight", PadTarget::DpadRight}, {"DpadDown", PadTarget::DpadDown}, {"DpadLeft", PadTarget::DpadLeft},
            {"L", PadTarget::L}, {"R", PadTarget::R}, {"Triangle", PadTarget::Triangle}, {"Circle", PadTarget::Circle},
            {"Cross", PadTarget::Cross}, {"Square", PadTarget::Square}, {"MoveForward", PadTarget::MoveForward},
            {"MoveBack", PadTarget::MoveBack}, {"MoveLeft", PadTarget::MoveLeft}, {"MoveRight", PadTarget::MoveRight},
            {"Walk", PadTarget::Walk}, {"LookLeft", PadTarget::LookLeft}, {"LookRight", PadTarget::LookRight},
            {"LookUp", PadTarget::LookUp}, {"LookDown", PadTarget::LookDown}};
        if (name.empty()) return PadTarget::None;
        for (const auto &[text, target] : names) if (same_text(name, text)) return target;
        return {};
    }
    // Virtual-key code for a key name; 0 when the name is unknown.
    static int key_from_name(std::string_view name) {
        if (name.size() == 1u) {
            const char c = name[0] >= 'a' && name[0] <= 'z' ? static_cast<char>(name[0] - 'a' + 'A') : name[0];
            if (c >= '0' && c <= '9') return c;
            if (c >= 'A' && c <= 'Z') {
                // US scan code for the letter, so the binding follows the key's position.
                static constexpr unsigned char scan[26]{0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                    0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
                const auto mapped = MapVirtualKeyW(scan[c - 'A'], MAPVK_VSC_TO_VK);
                return mapped ? static_cast<int>(mapped) : c;
            }
        }
        if (name.size() >= 2u && name.size() <= 3u && (name[0] == 'F' || name[0] == 'f')) {
            int number = 0;
            for (const char c : name.substr(1u)) { if (c < '0' || c > '9') { number = 0; break; } number = number * 10 + (c - '0'); }
            if (number >= 1 && number <= 12) return VK_F1 + number - 1;
        }
        if (name.size() == 7u && same_text(name.substr(0u, 6u), "Numpad") && name[6] >= '0' && name[6] <= '9') return VK_NUMPAD0 + (name[6] - '0');
        static constexpr std::pair<const char *, int> names[]{
            {"Space", VK_SPACE}, {"Enter", VK_RETURN}, {"Return", VK_RETURN}, {"Escape", VK_ESCAPE}, {"Esc", VK_ESCAPE}, {"Tab", VK_TAB},
            {"Backspace", VK_BACK}, {"Up", VK_UP}, {"Down", VK_DOWN}, {"Left", VK_LEFT}, {"Right", VK_RIGHT},
            {"LShift", VK_LSHIFT}, {"RShift", VK_RSHIFT}, {"Shift", VK_SHIFT}, {"LCtrl", VK_LCONTROL}, {"RCtrl", VK_RCONTROL},
            {"Ctrl", VK_CONTROL}, {"LAlt", VK_LMENU}, {"RAlt", VK_RMENU}, {"Alt", VK_MENU}, {"Insert", VK_INSERT}, {"Delete", VK_DELETE},
            {"Home", VK_HOME}, {"End", VK_END}, {"PageUp", VK_PRIOR}, {"PageDown", VK_NEXT}, {"CapsLock", VK_CAPITAL},
            {"Comma", VK_OEM_COMMA}, {"Period", VK_OEM_PERIOD}, {"Slash", VK_OEM_2}, {"Semicolon", VK_OEM_1}, {"Quote", VK_OEM_7},
            {"LBracket", VK_OEM_4}, {"RBracket", VK_OEM_6}, {"Backslash", VK_OEM_5}, {"Minus", VK_OEM_MINUS}, {"Equals", VK_OEM_PLUS},
            {"Grave", VK_OEM_3}};
        for (const auto &[text, key] : names) if (same_text(name, text)) return key;
        return 0;
    }
    // Applies ini text on top of the current values. A keyboard target that
    // appears replaces every key bound to that target. Returns one message per
    // line that was not understood.
    std::vector<std::string> apply(std::string_view text) {
        std::vector<std::string> problems;
        std::string section;
        unsigned line_number = 0u;
        while (!text.empty()) {
            const auto end = std::min(text.find('\n'), text.size());
            auto line = text.substr(0u, end);
            text.remove_prefix(std::min(end + 1u, text.size()));
            ++line_number;
            if (const auto comment = line.find_first_of(";#"); comment != std::string_view::npos) line = line.substr(0u, comment);
            line = trim(line);
            if (line.empty()) continue;
            const auto problem = [&](const std::string &what) { problems.push_back("line " + std::to_string(line_number) + ": " + what); };
            if (line.front() == '[') {
                if (line.back() != ']') { problem("section header is not closed"); section.clear(); continue; }
                section = std::string(trim(line.substr(1u, line.size() - 2u)));
                continue;
            }
            const auto equals = line.find('=');
            if (equals == std::string_view::npos) { problem("expected name=value"); continue; }
            const auto name = trim(line.substr(0u, equals)), value = trim(line.substr(equals + 1u));
            const auto flag = [&](bool &out) {
                if (same_text(value, "true") || value == "1") out = true;
                else if (same_text(value, "false") || value == "0") out = false;
                else problem(std::string(name) + " must be true or false");
            };
            const auto number = [&](float &out, float low, float high) {
                try {
                    std::size_t used{};
                    const auto parsed = std::stof(std::string(value), &used);
                    if (used != value.size() || !(parsed >= low && parsed <= high)) throw std::out_of_range("range");
                    out = parsed;
                } catch (const std::exception &) {
                    std::ostringstream message;
                    message << name << " must be a number from " << low << " to " << high;
                    problem(message.str());
                }
            };
            const auto target = [&](PadTarget &out) {
                if (const auto found = target_from_name(value)) out = *found;
                else problem("unknown target '" + std::string(value) + "'");
            };
            if (same_text(section, "Input")) {
                if (same_text(name, "Scheme")) {
                    if (same_text(value, "psp")) scheme = InputScheme::Psp;
                    else if (same_text(value, "dual_analog")) scheme = InputScheme::DualAnalog;
                    else problem("Scheme must be dual_analog or psp");
                }
                else if (same_text(name, "Layout")) {
                    if (const auto named = layout_from_name(value)) { layout = *named; layout_named = true; }
                    else problem("Layout must be tod_pc, modern or psp");
                }
                else if (same_text(name, "BackgroundInput")) flag(background_input);
                else if (same_text(name, "ShowInput")) flag(show_input);
                else if (same_text(name, "ExactTurn")) flag(exact_turn);
                else if (same_text(name, "Prompts")) {
                    if (same_text(value, "auto")) prompts = PromptsSetting::Auto;
                    else if (same_text(value, "keyboard")) prompts = PromptsSetting::Keyboard;
                    else if (same_text(value, "gamepad")) prompts = PromptsSetting::Gamepad;
                    else if (same_text(value, "psp")) prompts = PromptsSetting::Psp;
                    else problem("Prompts must be auto, keyboard, gamepad or psp");
                }
                else if (same_text(name, "Version")) {
                    int parsed = 0;
                    bool digits = !value.empty() && value.size() <= 4u;
                    for (const char c : value) { digits = digits && c >= '0' && c <= '9'; parsed = parsed * 10 + (c - '0'); }
                    if (digits) version = parsed; else problem("Version must be a number");
                }
                else problem("unknown setting '" + std::string(name) + "'");
            } else if (same_text(section, "Input.Keyboard")) {
                if (same_text(name, "WalkScale")) number(walk_scale, 0.31f, 1.0f);
                else if (same_text(name, "LookKeySpeed")) number(look_key_speed, 0.05f, 1.0f);
                else if (same_text(name, "ToggleMouseCapture") || same_text(name, "ToggleInputDisplay")) {
                    const auto key = value.empty() ? 0 : key_from_name(value);
                    if (!value.empty() && !key) problem("unknown key '" + std::string(value) + "'");
                    else (same_text(name, "ToggleMouseCapture") ? toggle_capture_vk : toggle_display_vk) = key;
                } else if (const auto bound = target_from_name(name); bound && *bound != PadTarget::None && !name.empty()) {
                    std::erase_if(keys, [&](const KeyBinding &binding) { return binding.target == *bound; });
                    for (auto rest = value; !rest.empty();) {
                        const auto comma = std::min(rest.find(','), rest.size());
                        const auto key_name = trim(rest.substr(0u, comma));
                        rest.remove_prefix(std::min(comma + 1u, rest.size()));
                        if (key_name.empty()) continue;
                        if (const auto key = key_from_name(key_name)) keys.push_back({key, *bound});
                        else problem("unknown key '" + std::string(key_name) + "'");
                    }
                } else problem("unknown keyboard target '" + std::string(name) + "'");
            } else if (same_text(section, "Input.Mouse")) {
                if (same_text(name, "Enabled")) flag(mouse_enabled);
                else if (same_text(name, "Capture")) {
                    if (same_text(value, "click")) mouse_capture_on_click = true;
                    else if (same_text(value, "never")) mouse_capture_on_click = false;
                    else problem("Capture must be click or never");
                } else if (name.size() == 6u && same_text(name.substr(0u, 5u), "Mouse") && name[5] >= '1' && name[5] <= '5') target(mouse_buttons[name[5] - '1']);
                else if (same_text(name, "WheelUp")) target(wheel_up);
                else if (same_text(name, "WheelDown")) target(wheel_down);
                else if (same_text(name, "SensitivityX")) number(mouse_degrees_per_count, 0.001f, 5.0f);
                else if (same_text(name, "SensitivityY")) number(mouse_degrees_per_count_y, 0.001f, 5.0f);
                else if (same_text(name, "InvertX")) flag(mouse_invert_x);
                else if (same_text(name, "InvertY")) flag(mouse_invert_y);
                else if (same_text(name, "MenuClicks")) flag(mouse_menu_clicks);
                else if (same_text(name, "HoldPitch")) flag(mouse_hold_pitch);
                else if (same_text(name, "Exact")) flag(mouse_exact);
                else if (same_text(name, "Smoothing")) number(mouse_smoothing, 0.0f, 0.95f);
                else problem("unknown mouse setting '" + std::string(name) + "'");
            } else if (same_text(section, "Input.Gamepad")) {
                bool handled = true;
                if (same_text(name, "Enabled")) flag(pad_enabled);
                else if (same_text(name, "Index")) {
                    if (same_text(value, "auto")) pad_index = -1;
                    else if (value.size() == 1u && value[0] >= '0' && value[0] <= '3') pad_index = value[0] - '0';
                    else problem("Index must be auto or 0-3");
                } else if (same_text(name, "DeadzoneLeft")) number(deadzone_left, 0.0f, 0.9f);
                else if (same_text(name, "DeadzoneRight")) number(deadzone_right, 0.0f, 0.9f);
                else if (same_text(name, "TriggerThreshold")) number(trigger_threshold, 0.01f, 1.0f);
                else if (same_text(name, "CompensateEngineDeadzone")) flag(compensate_engine_deadzone);
                else if (same_text(name, "TurnSensitivity")) number(pad_turn_sensitivity, 0.0f, 4.0f);
                else if (same_text(name, "LookSensitivity")) number(pad_look_sensitivity, 0.0f, 4.0f);
                else if (same_text(name, "InvertY")) flag(pad_invert_y);
                else if (same_text(name, "Glyphs")) {
                    if (same_text(value, "auto")) glyphs = GlyphsSetting::Auto;
                    else if (same_text(value, "xbox")) glyphs = GlyphsSetting::Xbox;
                    else if (same_text(value, "playstation")) glyphs = GlyphsSetting::PlayStation;
                    else problem("Glyphs must be auto, xbox or playstation");
                }
                else if (same_text(name, "PlayStationPads")) flag(playstation_pads);
                else if (same_text(name, "Rumble")) number(rumble, 0.0f, 1.0f);
                else handled = false;
                if (!handled) {
                    if (const auto button = pad_button_from_name(name)) target(pad_buttons[*button]);
                    else problem("unknown gamepad setting '" + std::string(name) + "'");
                }
            } else if (same_text(section, "Input.Gamepad.PlayStation")) {
                // Any of the sixteen buttons; a button the section leaves out keeps its [Input.Gamepad] target.
                if (const auto button = pad_button_from_name(name)) {
                    const auto before = problems.size();
                    target(playstation_buttons[*button]);
                    if (problems.size() == before) playstation_named = static_cast<std::uint16_t>(playstation_named | (1u << *button));
                } else problem("unknown gamepad button '" + std::string(name) + "'");
            } else if (section.empty() || same_text(std::string_view(section).substr(0u, 5u), "Input")) problem("setting outside a known section");
            // Other sections ([Mods], [Game], ...) belong to other readers of this file.
        }
        return problems;
    }
    static PcInputConfig defaults(InputLayout layout) {
        PcInputConfig config; (void)config.apply(default_input_ini(layout)); return config;
    }
    // The defaults of a scheme: Total Overdose PC with dual_analog, PSP original with psp.
    static PcInputConfig defaults(InputScheme scheme = InputScheme::DualAnalog) { return defaults(default_layout(scheme)); }
    // The control style this text means: its Layout line, else the one its scheme goes with.
    InputLayout layout_meant() const noexcept { return layout_named ? layout : default_layout(scheme); }
    // Whether the game wrote this file: its first-run text starts with this line.
    static bool written_by_game(std::string_view text) {
        constexpr std::string_view marker = "; Chili Con Carnage PC input.";
        if (text.substr(0u, 3u) == "\xEF\xBB\xBF") text.remove_prefix(3u);
        return text.substr(0u, marker.size()) == marker;
    }
    // An older build's file with the current input defaults: everything before
    // the first section and every [Input...] section goes, the other sections
    // ([Mods], [Game], [Rendering], ...) stay as they are.
    static std::string with_current_input_defaults(std::string_view text) {
        std::string out = default_input_ini();
        bool keep = false;
        while (!text.empty()) {
            const auto end = std::min(text.find('\n'), text.size());
            const auto line = text.substr(0u, std::min(end + 1u, text.size()));
            text.remove_prefix(line.size());
            const auto bare = trim(line.substr(0u, std::min(line.find('\n'), line.size())));
            if (!bare.empty() && bare.front() == '[') {
                keep = !same_text(bare.substr(1u, 5u), "Input");
                if (keep && !out.ends_with("\n\n")) out += '\n';
            }
            if (keep) out += line;
        }
        return out;
    }

    // ---- control styles in the file -------------------------------------------------------------------

    // The changes that move a file's bindings from control style `from` to `to`: every binding line that
    // holds `from`'s default (the same keys or target in any order, case and spelling) takes `to`'s; a line
    // the player changed stays, and a binding the file leaves out follows the style by itself. [Input] Layout
    // and Scheme are not among them. The bindings are the settings table's (settings_table.hpp), in its order,
    // which is the launcher's (launcher/src/Model.cs, Model.SwitchLayout does the same).
    static std::vector<IniChange> layout_binding_changes(const IniDocument &doc, InputLayout from, InputLayout to) {
        std::vector<IniChange> out;
        if (from == to) return out;
        for (const auto &setting : settings::table) {
            if (setting.kind != settings::Kind::Binding) continue;
            const auto held = doc.get(setting.section, setting.key);
            if (!held || !settings::binding_equivalent(setting, *held, setting.default_for(from))) continue;
            const auto wanted = setting.default_for(to);
            if (settings::binding_equivalent(setting, *held, wanted)) continue;
            IniChange change;
            change.section = setting.section; change.key = setting.key; change.value = wanted;
            out.push_back(std::move(change));
        }
        return out;
    }
    // How many binding lines of the file differ from the defaults of `layout`, counting every line of an
    // [Input.Gamepad.PlayStation] section (a table of the player's own).
    static std::size_t bindings_changed(const IniDocument &doc, InputLayout layout) {
        std::size_t changed = 0u;
        for (const auto &setting : settings::table)
            if (setting.kind == settings::Kind::Binding)
                if (const auto held = doc.get(setting.section, setting.key); held && !settings::binding_equivalent(setting, *held, setting.default_for(layout))) ++changed;
        for (const auto &line : doc.lines)
            if (line.kind == IniDocument::Kind::KeyValue && IniDocument::same(line.section, "Input.Gamepad.PlayStation")) ++changed;
        return changed;
    }
    // input_migration_comments: an older build's comment lines that are exactly the old text get the new one.
    static void replace_migration_comments(IniDocument &doc, bool bindings_too) {
        for (auto &line : doc.lines) {
            if (line.kind != IniDocument::Kind::Comment || line.has_key) continue;
            for (const auto &change : input_migration_comments)
                if ((change.always || bindings_too) && IniDocument::same(line.section, change.section) && line.text == change.before) { line.text = change.after; break; }
        }
    }
    struct Migration { std::string text, what; };
    // A file the game wrote with an [Input] Version below input_ini_version, brought up to date; `what` says
    // how, for the log. Below 2 (an early build wrote Scheme=psp, which would otherwise stay for good): every
    // [Input...] section is replaced by the current defaults. 2 (the builds before the control styles, whose
    // defaults were Modern with dual_analog and PSP original with psp): when every binding line holds that
    // default and the scheme is dual_analog, the bindings take Total Overdose PC's; with psp they stay; a file
    // with any binding of the player's own keeps every binding. Then [Input] Layout names the style and
    // Version is 3; every other line stays as it is, but for three comment lines that described the older
    // build and the two above Cross and Triangle when those take Total Overdose PC's keys
    // (input_migration_comments). The launcher does the same to the same bytes (launcher/src/Model.cs,
    // InputMigration), whichever of the two reads the file first.
    static Migration brought_up_to_date(std::string_view text, int version) {
        Migration out;
        if (version < 2) {
            PcInputConfig named;
            (void)named.apply(text);
            out.text = with_current_input_defaults(text);
            out.what = std::string("was written by an older build (scheme ") + scheme_name(named.scheme) +
                "): its input sections now hold the current defaults (control style Total Overdose PC)";
            return out;
        }
        auto doc = IniDocument::parse(text);
        const bool psp = settings::same_name(settings::trimmed(doc.get("Input", "Scheme").value_or(std::string())), "psp");
        const auto was = psp ? InputLayout::Psp : InputLayout::Modern;
        const auto changed = bindings_changed(doc, was);
        const bool to_tod = changed == 0u && !psp;
        replace_migration_comments(doc, to_tod);
        if (to_tod)
            for (const auto &change : layout_binding_changes(doc, was, InputLayout::TodPc)) doc.set(change.section, change.key, change.value);
        const auto now = to_tod ? InputLayout::TodPc : was;
        doc.set("Input", "Layout", layout_name(now));
        doc.set("Input", "Version", std::to_string(input_ini_version));
        out.text = doc.bytes();
        out.what = std::string("was written by an older build (Version ") + std::to_string(version) + ", scheme " + (psp ? "psp" : "dual_analog") + "): " +
            (to_tod ? std::string("every binding was that build's default, the bindings are now those of Total Overdose PC")
                    : changed ? std::to_string(changed) + " binding" + (changed == 1u ? "" : "s") + " of the player's own, every binding kept"
                              : std::string("every binding was that build's default and stays")) +
            " (Layout=" + layout_name(now) + ", Version=" + std::to_string(input_ini_version) + ")";
        return out;
    }
    // The defaults of the control style the file means, then `file` on top when
    // it exists. A missing file is created with the defaults so there is
    // something to edit; failing to write it is not an error. With `create`, a
    // file the game itself wrote before input_ini_version is brought up to date
    // (brought_up_to_date) under the gate the launcher shares; the file as it was
    // is kept beside it as <name>.bak (over an older one) and `notes` says so. A
    // file somebody wrote by hand is never touched.
    static PcInputConfig load(const std::filesystem::path &file, std::vector<std::string> &problems, bool create = true,
                              std::vector<std::string> *notes = nullptr) {
        auto config = defaults();
        std::error_code status;
        if (std::filesystem::exists(file, status)) {
            std::ifstream stream(file, std::ios::binary);
            std::ostringstream buffer;
            buffer << stream.rdbuf();
            if (stream.bad()) problems.push_back("cannot read " + file.string());
            else {
                stream.close();
                auto text = buffer.str();
                // A setting the file leaves out takes the default of the file's own control style.
                PcInputConfig named;
                (void)named.apply(text);
                if (create && named.version < input_ini_version && written_by_game(text)) {
                    ini_file::Gate gate;
                    // Under the gate, the file as it is now (the launcher may have brought it up to date).
                    if (auto now = ini_file::read(file)) { text = std::move(*now); named = {}; (void)named.apply(text); }
                    if (named.version < input_ini_version && written_by_game(text)) {
                        const auto updated = brought_up_to_date(text, named.version);
                        auto backup = file;
                        backup += ".bak";
                        std::error_code copied;
                        std::filesystem::copy_file(file, backup, std::filesystem::copy_options::overwrite_existing, copied);
                        const bool written = ini_file::write_patiently(file, updated.text);
                        if (notes) notes->push_back(file.filename().string() + " " + updated.what +
                            (!written ? ", for this run only: the file could not be rewritten" : copied ? "; no copy of the old file could be made" : "; the old file is " + backup.filename().string()));
                        text = updated.text;
                        named = {};
                        (void)named.apply(text);
                    }
                }
                config = defaults(named.layout_meant());
                problems = config.apply(text);
            }
        } else if (create) {
            std::ofstream stream(file, std::ios::binary | std::ios::trunc);
            stream << default_input_ini();
        }
        return config;
    }
};

// Raw mouse reports as movement in counts. A mouse reports movement. A pen, a
// touch screen and most remote-desktop and streaming pointers report positions
// (screen pixels here), and their movement is the change of position. While
// the game holds the pointer it puts a strayed pointer back in the middle of
// its window; some of those devices follow that and some carry on from where
// they were, and the next reports tell which.
struct MouseMotion {
    double x{}, y{};
    void relative(double dx, double dy) noexcept { x += dx; y += dy; }
    void absolute(double px, double py) noexcept {
        if (have_) {
            double dx = px - last_x_, dy = py - last_y_;
            if (warp_reports_) {
                // Nearer to where the pointer was put than to where it was: it followed.
                const double wx = px - warp_x_, wy = py - warp_y_;
                if (wx * wx + wy * wy < dx * dx + dy * dy) { dx = wx; dy = wy; warp_reports_ = 0u; }
                else --warp_reports_;
            }
            x += dx; y += dy;
        }
        last_x_ = px; last_y_ = py; have_ = true;
    }
    // Whether the last reported position is more than `reach` pixels from (cx, cy) on either axis.
    bool strayed(double cx, double cy, double reach) const noexcept {
        return have_ && (std::fabs(last_x_ - cx) > reach || std::fabs(last_y_ - cy) > reach);
    }
    void warped(double cx, double cy) noexcept { warp_x_ = cx; warp_y_ = cy; warp_reports_ = 4u; }
    void take() noexcept { x = y = 0.0; }
    void reset() noexcept { *this = {}; }

private:
    double last_x_{}, last_y_{}, warp_x_{}, warp_y_{};
    unsigned warp_reports_{};
    bool have_{};
};

// The mouse movement of one sample from whichever measurement is alive. Raw
// input is taken when it arrives: reports of movement as they are, reports of
// positions as the change of position. A pointer that sends no raw input at
// all (some remote desktops) still moves the cursor; after a few samples of
// that the cursor's own change of position is the movement, counted from where
// the game last put it, and nothing of those first samples is lost.
class PointerTracker {
public:
    struct Movement { double x{}, y{}; MouseSource source{MouseSource::None}; };
    // Samples in which the cursor moved and no raw report came before the cursor is believed.
    static constexpr unsigned cursor_samples = 3u;
    void raw_relative(double dx, double dy) noexcept { relative_x_ += dx; relative_y_ += dy; ++relative_reports_; }
    void raw_absolute(double px, double py) noexcept { absolute_.absolute(px, py); ++absolute_reports_; }
    // The game put the cursor at (cx, cy).
    void warped(double cx, double cy) noexcept { absolute_.warped(cx, cy); cursor_.warped(cx, cy); }
    // Raw movement that has arrived since the last take().
    Movement pending() const noexcept {
        if (!relative_reports_ && !absolute_reports_) return {};
        return {relative_x_ + absolute_.x, relative_y_ + absolute_.y, relative_reports_ ? MouseSource::RawRelative : MouseSource::RawAbsolute};
    }
    // Once per sample. (cursor_x, cursor_y) is the cursor's place on the screen when `cursor_known`.
    Movement take(bool cursor_known, double cursor_x, double cursor_y) noexcept {
        if (cursor_known) cursor_.absolute(cursor_x, cursor_y);
        auto out = pending();
        if (out.source != MouseSource::None) {
            // Raw input is alive and the cursor only echoes it.
            cursor_only_ = 0u; held_x_ = held_y_ = 0.0;
        } else if (cursor_.x != 0.0 || cursor_.y != 0.0) {
            held_x_ += cursor_.x; held_y_ += cursor_.y;
            if (cursor_only_ < cursor_samples) ++cursor_only_;
            if (cursor_only_ >= cursor_samples) { out = {held_x_, held_y_, MouseSource::Cursor}; held_x_ = held_y_ = 0.0; }
        }
        relative_x_ = relative_y_ = 0.0; relative_reports_ = absolute_reports_ = 0u;
        absolute_.take(); cursor_.take();
        return out;
    }
    void reset() noexcept { *this = {}; }

private:
    MouseMotion absolute_, cursor_;
    double relative_x_{}, relative_y_{}, held_x_{}, held_y_{};
    unsigned relative_reports_{}, absolute_reports_{}, cursor_only_{};
};

// An XInput state as a pad sample: the same buttons in the same order, the sticks with down positive.
inline PadSample pad_sample_from_xinput(const XINPUT_STATE &state) noexcept {
    static_assert(PcInputConfig::pad_button_names[PadSample::DpadUp].second == XINPUT_GAMEPAD_DPAD_UP &&
                  PcInputConfig::pad_button_names[PadSample::DpadDown].second == XINPUT_GAMEPAD_DPAD_DOWN &&
                  PcInputConfig::pad_button_names[PadSample::DpadLeft].second == XINPUT_GAMEPAD_DPAD_LEFT &&
                  PcInputConfig::pad_button_names[PadSample::DpadRight].second == XINPUT_GAMEPAD_DPAD_RIGHT &&
                  PcInputConfig::pad_button_names[PadSample::Start].second == XINPUT_GAMEPAD_START &&
                  PcInputConfig::pad_button_names[PadSample::Back].second == XINPUT_GAMEPAD_BACK &&
                  PcInputConfig::pad_button_names[PadSample::LS].second == XINPUT_GAMEPAD_LEFT_THUMB &&
                  PcInputConfig::pad_button_names[PadSample::RS].second == XINPUT_GAMEPAD_RIGHT_THUMB &&
                  PcInputConfig::pad_button_names[PadSample::LB].second == XINPUT_GAMEPAD_LEFT_SHOULDER &&
                  PcInputConfig::pad_button_names[PadSample::RB].second == XINPUT_GAMEPAD_RIGHT_SHOULDER &&
                  PcInputConfig::pad_button_names[PadSample::A].second == XINPUT_GAMEPAD_A &&
                  PcInputConfig::pad_button_names[PadSample::B].second == XINPUT_GAMEPAD_B &&
                  PcInputConfig::pad_button_names[PadSample::X].second == XINPUT_GAMEPAD_X &&
                  PcInputConfig::pad_button_names[PadSample::Y].second == XINPUT_GAMEPAD_Y &&
                  PcInputConfig::pad_left_trigger == PadSample::LT && PcInputConfig::pad_right_trigger == PadSample::RT,
        "PadSample's buttons are the ini's pad buttons in the same order");
    PadSample sample;
    sample.kind = PadKind::Xbox;
    sample.packet = state.dwPacketNumber;
    for (std::size_t i = 0; i < PcInputConfig::pad_button_names.size(); ++i)
        if (state.Gamepad.wButtons & PcInputConfig::pad_button_names[i].second) sample.buttons = static_cast<std::uint16_t>(sample.buttons | PadSample::bit(static_cast<unsigned>(i)));
    sample.lt = state.Gamepad.bLeftTrigger / 255.0f; sample.rt = state.Gamepad.bRightTrigger / 255.0f;
    sample.lx = std::clamp(state.Gamepad.sThumbLX / 32767.0f, -1.0f, 1.0f); sample.ly = std::clamp(-state.Gamepad.sThumbLY / 32767.0f, -1.0f, 1.0f);
    sample.rx = std::clamp(state.Gamepad.sThumbRX / 32767.0f, -1.0f, 1.0f); sample.ry = std::clamp(-state.Gamepad.sThumbRY / 32767.0f, -1.0f, 1.0f);
    return sample;
}

// Finds the XInput slots that have a pad. An empty slot is slow to query, so the
// looking is done on a worker thread: at start, every two seconds after that,
// pad or no pad, and at once when Windows reports a device change or a pad stops
// answering. A second pad does not always come with a device-change message (a
// wireless pad that is switched on beside one that is in use, a helper program
// that adds a pad), so the looking goes on while a pad is present. The game's
// thread only ever reads the slots that answered.
class PadFinder {
public:
    using Query = std::function<bool(unsigned, XINPUT_STATE &)>;
    static constexpr unsigned slots = 4u;
    // The time between two looks of the worker's own.
    static constexpr std::chrono::milliseconds period{2000};
    // `fixed`: the one slot (0-3) to use, or -1 for all four.
    // Without a worker nothing is looked for until scan() is called.
    PadFinder(Query query, int fixed, bool worker) : query_(std::move(query)), fixed_(fixed) {
        if (worker) worker_ = std::thread([this] { run(); });
    }
    ~PadFinder() {
        if (!worker_.joinable()) return;
        { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; }
        wake_.notify_all();
        worker_.join();
    }
    PadFinder(const PadFinder &) = delete;
    PadFinder &operator=(const PadFinder &) = delete;
    // One bit per slot that answered when it was last asked.
    unsigned present() const noexcept { return present_.load(std::memory_order_relaxed); }
    // The lowest slot with a pad, or -1 while no pad is known.
    int slot() const noexcept {
        const auto mask = present();
        for (unsigned slot = 0; slot < slots; ++slot) if (mask >> slot & 1u) return static_cast<int>(slot);
        return -1;
    }
    // Passes over the slots that have been finished.
    unsigned searches() const noexcept { return searches_.load(std::memory_order_relaxed); }
    // A slot stopped answering.
    void lost(unsigned slot) { present_.fetch_and(~(1u << slot), std::memory_order_relaxed); look(); }
    // Look now instead of when the period is over.
    void look() {
        { std::lock_guard<std::mutex> lock(mutex_); asked_ = true; }
        wake_.notify_all();
    }
    // One pass over the slots: every one of them is asked, so a pad in any slot is found, also beside another.
    void scan() {
        const unsigned first = fixed_ >= 0 ? static_cast<unsigned>(fixed_) : 0u, last = fixed_ >= 0 ? first : slots - 1u;
        for (unsigned slot = first; slot <= last; ++slot) {
            XINPUT_STATE state{};
            if (query_(slot, state)) present_.fetch_or(1u << slot, std::memory_order_relaxed);
            else present_.fetch_and(~(1u << slot), std::memory_order_relaxed);
        }
        searches_.fetch_add(1u, std::memory_order_relaxed);
    }

private:
    void run() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stop_) {
            // Every pass asks all the slots, also while a pad is present: that is how a pad that arrived
            // without a device-change message is found.
            asked_ = false; lock.unlock(); scan(); lock.lock();
            wake_.wait_for(lock, period, [this] { return stop_ || asked_; });
        }
    }
    Query query_;
    int fixed_;
    std::atomic<unsigned> present_{0u}, searches_{0u};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    bool stop_{}, asked_{};
};

// Where input comes from. Tests replace these; the game uses the Win32 defaults.
struct PcInputHost {
    std::function<bool()> focused;
    std::function<bool(int)> key_down;
    std::function<bool(unsigned, XINPUT_STATE &)> pad;
    std::function<double()> seconds;
    // Look for the pad on a worker thread; without it scan_pads() does the looking.
    bool pad_worker{true};
    // The cursor: where it is on the screen, putting it somewhere, the window's
    // client area on the screen, and hiding and confining it (true) or giving
    // it back (false). Left empty they are the Win32 calls on the attached
    // window, and nothing at all while no window is attached.
    std::function<bool(double &, double &)> cursor;
    std::function<bool(double, double)> warp;
    std::function<bool(RECT &)> area;
    std::function<void(bool)> grab;
    // Sets the two motors of the pad in an XInput slot: the large, slow one and the small, fast one, 0 to
    // 65535. Left empty it is XInputSetState.
    std::function<bool(unsigned, std::uint16_t, std::uint16_t)> vibrate;
    // One line for the log, without the "[input] " in front. Left empty it goes to standard output.
    std::function<void(const std::string &)> log;
};

// What the input layer has seen since the start, for the exit summary and the
// title bar's input test line.
struct PcInputReport {
    std::uint64_t raw_relative_reports{}, raw_absolute_reports{}, cursor_samples{}, captures{}, mouse_presses{}, key_presses{},
        wheel_notches{}, ignored{}, samples{};
    // Counts moved while captured, both directions added up.
    double travel_x{}, travel_y{};
    // The kind of the last raw report (None before the first), captured or not.
    MouseSource last_report{MouseSource::None};
    // The last movement that reached a sample, where it was measured, and when.
    MouseSource source{MouseSource::None};
    double moved_x{}, moved_y{}, moved_at{};
    // The pad state of the last sample.
    ControllerInput sent{};
    // Gamepads: pads that arrived, by kind; how often the pad in use changed; host samples by the device used
    // last and how often that changed; the game's own vibration calls and the settings sent on to a pad; how
    // often a PlayStation pad that had been streaming fell silent and was put to rest.
    std::uint64_t pads_xbox{}, pads_playstation{}, pad_changes{}, samples_keyboard_mouse{}, samples_gamepad{}, device_changes{},
        game_vibration_calls{}, vibration_sent{}, pad_silences{};
};

class PcInput {
public:
    static PcInput &instance() { static PcInput input; return input; }
    PcInput() : config_(PcInputConfig::defaults()) {
        host_.focused = [] {
            DWORD owner{};
            const auto window = GetForegroundWindow();
            return window && GetWindowThreadProcessId(window, &owner) && owner == GetCurrentProcessId();
        };
        host_.key_down = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
        host_.pad = [](unsigned slot, XINPUT_STATE &state) { return XInputGetState(slot, &state) == ERROR_SUCCESS; };
        host_.seconds = [] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
        host_.vibrate = [](unsigned slot, std::uint16_t left, std::uint16_t right) {
            XINPUT_VIBRATION motors{left, right};
            return XInputSetState(slot, &motors) == ERROR_SUCCESS;
        };
        host_.log = [](const std::string &line) { std::cout << "[input] " << line << '\n'; };
        // CHILI_RUMBLE_TEST=1: both motors of a pad run for half a second when it becomes the pad in use,
        // the way to see that vibration reaches a pad (the game itself was never seen to ask for any).
        const char *test = std::getenv("CHILI_RUMBLE_TEST");
        rumble_test_ = test && *test && *test != '0';
    }
    ~PcInput() {
        // The cursor goes back to Windows whatever state the game ends in.
        if (captured_ && !host_.grab && window_) { ClipCursor(nullptr); if (cursor_hidden_) ShowCursor(TRUE); }
    }
    // The bindings and settings of this run. The prompt pictures are told the Prompts and Glyphs settings and
    // that the tables were loaded (input_device.hpp).
    void configure(PcInputConfig config) { finder_.reset(); config_ = std::move(config); show_input_ = config_.show_input; config_.publish(); }
    const PcInputConfig &config() const noexcept { return config_; }
    // What reconfigure() had to do beyond taking the values.
    struct Reconfigured {
        bool finder_rebuilt{};      // [Input.Gamepad] Index or Enabled changed: the pads are looked for again
        bool prompts_published{};   // something the prompt pictures show changed: they are painted again
        bool mouse_released{};      // the held mouse was let go (the mouse switched off, or never captured)
        bool hid_started{}, hid_stopped{};   // PlayStation pads are read from now on, or not any more
    };
    // New settings while the game runs (the game's own settings page): every value is in force from the
    // next sample or message on, as after configure(), but what the player has in hand stays. The pad finder
    // and the pad in use are kept unless [Input.Gamepad] Index or Enabled changed; the title bar's input line
    // keeps what its key set unless ShowInput changed; a held mouse is let go when the mouse is switched off
    // or is no longer captured on a click; PlayStation pads are registered for or dropped as PlayStationPads
    // (with Index and Enabled) says; and the prompt pictures are told only when something they show changed,
    // so a slider that is dragged does not have them painted at every step. On the game's thread, like
    // everything else here.
    Reconfigured reconfigure(PcInputConfig config) {
        Reconfigured done;
        const bool read_playstation = reads_playstation_pads();
        const PcInputConfig old = std::exchange(config_, std::move(config));
        if (old.pad_index != config_.pad_index || old.pad_enabled != config_.pad_enabled) {
            finder_.reset();
            done.finder_rebuilt = true;
            if (!config_.pad_enabled) {
                // The motors stop (apply_rumble sends nothing to a switched-off pad later) and no pad is in use.
                apply_rumble(true);
                for (std::size_t slot = 0; slot < hid_first; ++slot) if (pads_[slot].present) { forget_pad(static_cast<int>(slot)); pads_[slot] = {}; }
                log("gamepad: switched off ([Input.Gamepad] Enabled=false)");
            } else if (window_) (void)finder();   // looked for at once, as attach() does
        }
        if (read_playstation != reads_playstation_pads()) {
            if (read_playstation) {
                for (std::size_t i = hid_first; i < pads_.size(); ++i)
                    if (pads_[i].present) {
                        char text[72];
                        std::snprintf(text, sizeof(text), "gamepad: PlayStation pad %04X:%04X is not read any more", unsigned(pads_[i].info.vid), unsigned(pads_[i].info.pid));
                        log(text);
                        forget_pad(static_cast<int>(i));
                        pads_[i] = {};
                    }
                register_hid_pads(false);
                done.hid_stopped = true;
            } else {
                hid_refused_.clear();
                register_hid_pads(true);
                done.hid_started = true;
            }
        }
        if (old.show_input != config_.show_input) show_input_ = config_.show_input;
        if ((captured_ || grab_pending_) && (!config_.mouse_enabled || !config_.mouse_capture_on_click)) {
            capture(false); grab_pending_ = false;
            done.mouse_released = true;
        }
        if (!config_.mouse_enabled) { mouse_down_.fill(false); mouse_latch_.fill(false); menu_click_.fill(0u); wheel_delta_ = 0; wheel_up_pending_ = wheel_down_pending_ = 0u; }
        if (!old.same_prompts(config_)) { config_.publish(); done.prompts_published = true; }
        return done;
    }
    void set_host(PcInputHost host) {
        finder_.reset();
        // A host that leaves the first four out keeps the Win32 ones.
        if (!host.focused) host.focused = host_.focused;
        if (!host.key_down) host.key_down = host_.key_down;
        if (!host.pad) host.pad = host_.pad;
        if (!host.seconds) host.seconds = host_.seconds;
        // So do the motors and the log.
        if (!host.vibrate) host.vibrate = host_.vibrate;
        if (!host.log) host.log = host_.log;
        host_ = std::move(host);
    }
    bool mouse_captured() const noexcept { return captured_; }
    // The player asked for the mouse while the game was not using it; it is taken when play resumes.
    bool capture_pending() const noexcept { return grab_pending_; }
    // A run with a script channel attached never captures the mouse.
    void forbid_capture() { capture_allowed_ = false; capture(false); grab_pending_ = false; }
    bool capture_forbidden() const noexcept { return !capture_allowed_; }
    // Whether something other than the player's devices (a replay, a script
    // command) owns the coming samples. While it does the mouse is not
    // captured, and what the player presses or moves is counted as ignored
    // instead of being kept for later.
    void set_scripted(bool scripted) {
        if (scripted) { if (captured_ || grab_pending_) { capture(false); grab_pending_ = false; } clear_transient(); if (!scripted_) apply_rumble(true); }
        scripted_ = scripted;
    }
    bool scripted() const noexcept { return scripted_; }
    // The settings page took a click or a wheel notch for itself (settings_page.cpp): the mouse was used all
    // the same, which is one of the signs of the device in use.
    void note_mouse_used() noexcept { if (!scripted_ && config_.mouse_enabled) mouse_event_ = true; }
    // Seconds since the player last pressed or moved something that was ignored; huge when never.
    double seconds_since_ignored() const { return ignored_at_ > 0.0 ? host_.seconds() - ignored_at_ : 1.0e9; }
    // The title bar's input test line: the ini's ShowInput, switched by its toggle key.
    bool show_input() const noexcept { return show_input_; }
    const PcInputReport &report() const noexcept { return report_; }
    // Whether the game is using the look controls (dual_analog.hpp). While it
    // is not (menus, cutscenes, the pause screen) a captured mouse is let go,
    // and captured again when play resumes.
    void set_look_active(bool active) {
        if (active == look_active_) return;
        look_active_ = active;
        if (!active && captured_) { capture(false); grab_pending_ = true; }
        else if (active && grab_pending_) { grab_pending_ = false; if (host_.focused()) capture(true); }
    }
    bool look_active() const noexcept { return look_active_; }
    // The game's frame time: the engine turns by rate times this, so mouse
    // movement divided by it is the rate that turns exactly that far. The
    // engine itself never uses more than a tenth of a second. Outside 1/240 to
    // 1/10 of a second the time between samples is used instead.
    void set_frame_seconds(double seconds) noexcept { frame_seconds_ = seconds >= 1.0 / 240.0 && seconds <= 0.1001 ? seconds : 0.0; }

    // Registers the game window for raw mouse input and for the reports of HID game pads, and starts
    // looking for XInput pads.
    void attach(HWND window) {
        window_ = window;
        RAWINPUTDEVICE device{};
        device.usUsagePage = 1u; device.usUsage = 2u; device.hwndTarget = window;
        raw_registered_ = RegisterRawInputDevices(&device, 1u, sizeof(device)) != FALSE;
        if (reads_playstation_pads()) register_hid_pads(true);
        if (config_.pad_enabled) (void)finder();
    }
    bool raw_registered() const noexcept { return raw_registered_; }
    // Whether the window was registered for the reports of HID game pads (PlayStation pads).
    bool hid_registered() const noexcept { return hid_registered_; }
    // The attached window; none before attach() and after the window is destroyed.
    HWND window() const noexcept { return window_; }
    double now() const { return host_.seconds(); }
    // Gives the cursor back; the window is going away or the game is ending.
    void release() { capture(false); grab_pending_ = false; apply_rumble(true); }
    // Feed for the game window's procedure. Returns true when the message is
    // fully handled and `result` holds the value to return.
    bool window_message(HWND, UINT message, WPARAM w, LPARAM l, LRESULT &result) {
        switch (message) {
        case WM_KEYDOWN: case WM_SYSKEYDOWN: {
            if (l & (1ll << 30)) return false;   // auto-repeat
            const auto key = specific_key(w, l);
            if (config_.toggle_display_vk && key == config_.toggle_display_vk) { show_input_ = !show_input_; return false; }
            if (config_.toggle_capture_vk && key == config_.toggle_capture_vk && config_.mouse_enabled && config_.mouse_capture_on_click) {
                if (captured_ || grab_pending_) { capture(false); grab_pending_ = false; }
                else if (!request_capture()) ignored();
                return false;
            }
            ++report_.key_presses;
            if (scripted_) { ignored(); return false; }
            latch_key(static_cast<int>(w)); latch_key(key);
            return false;
        }
        case WM_SYSCOMMAND:
            // A bare Alt bound to something must not open the window menu.
            if ((w & 0xFFF0u) == SC_KEYMENU) { result = 0; return true; }
            return false;
        case WM_INPUT: raw_input(reinterpret_cast<HRAWINPUT>(l)); return false;
        case WM_MOUSEACTIVATE:
            // The click that brings the window to the front captures the mouse but confirms nothing in a menu.
            if (LOWORD(l) == HTCLIENT) activating_click_ = true;
            return false;
        case WM_MOUSEMOVE:
            // A replay is playing and the player moves the mouse over the window.
            if (scripted_ && last_move_ != -1 && l != last_move_) ignored();
            // A free mouse moving over the window, in pixels: one of the signs of which device is in use.
            else if (!scripted_ && !captured_ && last_move_ != -1 && config_.mouse_enabled)
                free_motion_ += std::abs(static_cast<short>(LOWORD(l)) - static_cast<short>(LOWORD(last_move_))) +
                                std::abs(static_cast<short>(HIWORD(l)) - static_cast<short>(HIWORD(last_move_)));
            last_move_ = l;
            return false;
        case WM_LBUTTONDOWN: return mouse_button(0u, true, result);
        case WM_LBUTTONUP: return mouse_button(0u, false, result);
        case WM_RBUTTONDOWN: return mouse_button(1u, true, result);
        case WM_RBUTTONUP: return mouse_button(1u, false, result);
        case WM_MBUTTONDOWN: return mouse_button(2u, true, result);
        case WM_MBUTTONUP: return mouse_button(2u, false, result);
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: {
            const bool handled = mouse_button(HIWORD(w) == XBUTTON2 ? 4u : 3u, message == WM_XBUTTONDOWN, result);
            if (handled) result = TRUE;
            return handled;
        }
        case WM_MOUSEWHEEL:
            if (scripted_) { ignored(); return false; }
            if (!mouse_live()) return false;
            wheel_delta_ += GET_WHEEL_DELTA_WPARAM(w);
            for (; wheel_delta_ >= WHEEL_DELTA; wheel_delta_ -= WHEEL_DELTA) { ++report_.wheel_notches; mouse_event_ = true; if (wheel_up_pending_ < 8u) ++wheel_up_pending_; }
            for (; wheel_delta_ <= -WHEEL_DELTA; wheel_delta_ += WHEEL_DELTA) { ++report_.wheel_notches; mouse_event_ = true; if (wheel_down_pending_ < 8u) ++wheel_down_pending_; }
            result = 0; return true;
        case WM_SETCURSOR:
            if (captured_ && LOWORD(l) == HTCLIENT) { SetCursor(nullptr); result = TRUE; return true; }
            return false;
        case WM_SIZE: case WM_MOVE:
            if (captured_ && !host_.grab) clip(window_);
            return false;
        case WM_DEVICECHANGE:
            // Something was plugged in or pulled out: every XInput slot is asked again, so a second pad is found.
            if (finder_) finder_->look();
            return false;
        case WM_INPUT_DEVICE_CHANGE:
            // A HID game pad arrived (also sent for the ones that are there when the window registers) or left.
            if (w == GIDC_ARRIVAL) (void)hid_device(reinterpret_cast<HANDLE>(l));
            else if (w == GIDC_REMOVAL) hid_removed(static_cast<std::uintptr_t>(l));
            return false;
        case WM_KILLFOCUS: case WM_ENTERSIZEMOVE: case WM_DESTROY:
            capture(false); grab_pending_ = false; activating_click_ = false; clear_transient();
            if (message == WM_DESTROY) window_ = nullptr;
            return false;
        case WM_ACTIVATE:
            if (LOWORD(w) == WA_INACTIVE) { capture(false); grab_pending_ = false; activating_click_ = false; clear_transient(); }
            return false;
        case WM_ACTIVATEAPP:
            if (!w) { capture(false); grab_pending_ = false; activating_click_ = false; clear_transient(); }
            return false;
        default: return false;
        }
    }
    // Takes the mouse messages that wait in the window's queue: raw movement reports and button changes.
    // The window is otherwise pumped on a timer (every 8 ms), so without this a sample misses whatever
    // the mouse sent since the last pump. Called right before a sample is taken; nothing else is dispatched.
    // A PlayStation pad's reports come the same way and are taken with them.
    void pump_mouse() {
        if (!window_ || (!config_.mouse_enabled && !hid_registered_)) return;
        MSG message;
        for (unsigned count = 0u; count < 512u && PeekMessageW(&message, window_, WM_INPUT, WM_INPUT, PM_REMOVE); ++count) DispatchMessageW(&message);
        if (!config_.mouse_enabled) return;
        for (unsigned count = 0u; count < 64u && PeekMessageW(&message, window_, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE); ++count) DispatchMessageW(&message);
    }
    // Mouse movement in device counts (positive = right, down), as raw input
    // from a device that reports movement delivers it.
    void add_mouse_counts(double x, double y = 0.0) noexcept { pointer_.raw_relative(x, y); }
    // A raw report from a device that reports positions, in screen pixels.
    void add_mouse_position(double x, double y) noexcept { pointer_.raw_absolute(x, y); }
    void set_captured_for_test(bool captured) noexcept { captured_ = captured; }
    // One pass of the gamepad search on the calling thread (hosts without the worker).
    void scan_pads() { if (config_.pad_enabled) finder().scan(); }

    // One guest controller sample: consumes key presses latched since the last
    // sample, the accumulated mouse movement and one step of a wheel pulse.
    ControllerInput sample() { return read(true); }
    // The current pad state without consuming anything.
    ControllerInput peek() { return read(false); }
    // The gamepad's buttons alone. The native save and message dialogs take
    // these: they handle the keyboard and the mouse themselves, and a key bound
    // to a pad button would otherwise act twice.
    ControllerInput peek_pad() {
        ControllerInput out;
        if (!config_.background_input && !host_.focused()) return out;
        Accumulated state;
        if (const auto *pad = poll_pads(host_.seconds())) press_pad(state, *pad);
        out.buttons = state.buttons;
        return out;
    }

    // The pad in use, as it was at the last sample: none before a pad was found and after the last one left.
    const PadSample *active_pad() const noexcept { return active_pad_ >= 0 ? &pads_[static_cast<std::size_t>(active_pad_)].sample : nullptr; }
    // "xbox slot 1", "playstation 054C:0CE6" or "none": the pad in use, for the title bar and the log.
    std::string pad_text() const {
        if (active_pad_ < 0) return "none";
        char text[48];
        const auto &pad = pads_[static_cast<std::size_t>(active_pad_)];
        if (static_cast<std::size_t>(active_pad_) < hid_first) std::snprintf(text, sizeof(text), "xbox slot %d", active_pad_);
        else std::snprintf(text, sizeof(text), "playstation %04X:%04X", unsigned(pad.info.vid), unsigned(pad.info.pid));
        return text;
    }
    // What the player used last, as this layer told the prompt pictures.
    InputDevice device() const noexcept { return device_; }
    // One line per PlayStation pad that is connected: its report counts and what became of its vibration.
    std::vector<std::string> hid_pad_lines() const {
        std::vector<std::string> lines;
        for (std::size_t i = hid_first; i < pads_.size(); ++i)
            if (pads_[i].present && pads_[i].device)
                lines.push_back("gamepad " + pads_[i].device->ids() + ": " + pads_[i].device->counts_text() + "; " + pads_[i].device->rumble_text());
        return lines;
    }

    // The motors of the pad in use, each 0 to 1: `left` is the large, slow one, `right` the small, fast one.
    // [Input.Gamepad] Rumble scales both and 0 switches them off. Nothing is sent while the values stay as
    // they are; a pad that stops being the pad in use is stopped, and so is the pad while the game has not
    // got the focus, while a replay or a script has the input, and when the game ends.
    void rumble(float left, float right) {
        rumble_left_ = unit(left); rumble_right_ = unit(right);
        apply_rumble(scripted_);
    }
    // The game's own call: its pad's SetControllerVibration(motor, strength), an empty function on the PSP
    // (pad_rumble_hook.hpp). Which motor is which is not known, the game was never seen to call it: taken as
    // on a PlayStation 2 pad, 0 the small motor and 1 the large one. The first calls are logged as they come.
    void game_vibration(std::uint32_t motor, float strength) {
        if (report_.game_vibration_calls++ < 16u) {
            char text[96];
            std::snprintf(text, sizeof(text), "vibration: the game set motor %u to %g", unsigned(motor), static_cast<double>(strength));
            log(text);
        }
        if (motor < game_motor_.size()) game_motor_[motor] = unit(strength);
        rumble(game_motor_[1], game_motor_[0]);
    }

    // A PlayStation pad arrived, sent a report, left. The window's messages end up here (hid_device,
    // hid_input); a test calls them with made-up pads. hid_arrived is false when the pad is not taken:
    // pads are off, PlayStationPads=false, Index names an XInput slot, or four are there already.
    bool hid_arrived(std::uintptr_t id, const HidPadInfo &info) { return hid_add(id, info, nullptr) != nullptr; }
    void hid_report(std::uintptr_t id, PadSample sample) {
        auto *pad = hid_find(id);
        if (!pad) return;
        sample.kind = PadKind::PlayStation; sample.vid = pad->info.vid; sample.pid = pad->info.pid; sample.packet = pad->reported.packet + 1u;
        const double now = host_.seconds();
        // How the pad reports: a report that says again what the one before it said, shortly after it, is
        // what a pad that streams sends and a pad that reports only changes does not. "Shortly" is in the
        // game's own samples (pad->silence), so the reports that waited in the queue through a pause of the
        // game's thread are not taken for reports after a pause of the pad's.
        if (pad->has_report && same_pad_state(pad->reported, sample)) {
            if (pad->silence <= hid_stream_gap_seconds) {
                if (const double gap = now - pad->report_at; gap >= 0.0 && gap <= hid_stream_gap_seconds) pad->interval = std::max(gap, pad->interval * 0.9);
                if (pad->repeats < 1000000u) ++pad->repeats;
            } else { pad->repeats = 0u; pad->interval = 0.0; }
        }
        pad->reported = sample; pad->has_report = true; pad->fresh_report = true; pad->silence = 0.0;
        pad->sample = sample; pad->report_at = now;
    }
    void hid_removed(std::uintptr_t id) {
        std::erase(hid_refused_, id);
        auto *pad = hid_find(id);
        if (!pad) return;
        const auto index = static_cast<int>(pad - pads_.data());
        char text[64];
        std::snprintf(text, sizeof(text), "gamepad: PlayStation pad %04X:%04X removed", unsigned(pad->info.vid), unsigned(pad->info.pid));
        std::string line = text;
        if (pad->device) line += " (" + pad->device->counts_text() + "; " + pad->device->rumble_text() + ")";
        log(line);
        forget_pad(index);
        *pad = {};
        if (!any_pad()) log("gamepad: none");
    }

private:
    struct Accumulated { std::uint32_t buttons{}; bool forward{}, back{}, left{}, right{}, walk{}, look_left{}, look_right{}, look_up{}, look_down{}; };
    static void press(Accumulated &state, PadTarget target) {
        switch (target) {
        case PadTarget::Select: state.buttons |= 0x0001u; break;
        case PadTarget::Start: state.buttons |= 0x0008u; break;
        case PadTarget::DpadUp: state.buttons |= 0x0010u; break;
        case PadTarget::DpadRight: state.buttons |= 0x0020u; break;
        case PadTarget::DpadDown: state.buttons |= 0x0040u; break;
        case PadTarget::DpadLeft: state.buttons |= 0x0080u; break;
        case PadTarget::L: state.buttons |= 0x0100u; break;
        case PadTarget::R: state.buttons |= 0x0200u; break;
        case PadTarget::Triangle: state.buttons |= 0x1000u; break;
        case PadTarget::Circle: state.buttons |= 0x2000u; break;
        case PadTarget::Cross: state.buttons |= 0x4000u; break;
        case PadTarget::Square: state.buttons |= 0x8000u; break;
        case PadTarget::MoveForward: state.forward = true; break;
        case PadTarget::MoveBack: state.back = true; break;
        case PadTarget::MoveLeft: state.left = true; break;
        case PadTarget::MoveRight: state.right = true; break;
        case PadTarget::Walk: state.walk = true; break;
        case PadTarget::LookLeft: state.look_left = true; break;
        case PadTarget::LookRight: state.look_right = true; break;
        case PadTarget::LookUp: state.look_up = true; break;
        case PadTarget::LookDown: state.look_down = true; break;
        case PadTarget::None: break;
        }
    }
    // The pad's sixteen logical buttons (a trigger past TriggerThreshold is one) through the table of its kind.
    void press_pad(Accumulated &state, const PadSample &pad) const {
        const auto kind = config_.kind_of(pad.kind);
        const auto down = pad.with_triggers(config_.trigger_threshold);
        for (std::size_t i = 0; i < config_.pad_buttons.size(); ++i)
            if (down >> i & 1u) press(state, config_.pad_target(i, kind));
    }
    void log(const std::string &line) const { if (host_.log) host_.log(line); }
    static float unit(float value) noexcept { return value > 0.0f ? std::min(value, 1.0f) : 0.0f; }
    static int specific_key(WPARAM key, LPARAM l) {
        const auto scan = static_cast<UINT>((l >> 16) & 0xFF);
        const bool extended = ((l >> 24) & 1) != 0;
        if (key == VK_SHIFT) { const auto mapped = MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX); return mapped ? static_cast<int>(mapped) : VK_LSHIFT; }
        if (key == VK_CONTROL) return extended ? VK_RCONTROL : VK_LCONTROL;
        if (key == VK_MENU) return extended ? VK_RMENU : VK_LMENU;
        return static_cast<int>(key);
    }
    void latch_key(int key) noexcept { if (key > 0 && key < 256) key_latch_[static_cast<std::size_t>(key)] = true; }
    bool mouse_live() const noexcept { return config_.mouse_enabled && (captured_ || !config_.mouse_capture_on_click); }
    // Something the player did went nowhere: a replay or a script owns the input.
    void ignored() { ++report_.ignored; ignored_at_ = host_.seconds(); }
    void clear_transient() noexcept {
        key_latch_.fill(false); mouse_down_.fill(false); mouse_latch_.fill(false); menu_click_.fill(0u);
        wheel_delta_ = 0; wheel_up_pending_ = wheel_down_pending_ = 0u; wheel_gap_ = false;
        clear_motion();
    }
    void clear_motion() noexcept { pointer_.reset(); turn_carry_ = pitch_carry_ = 0.0; smooth_x_ = smooth_y_ = 0.0; }
    // The window's client area in screen coordinates.
    static bool client_area(HWND window, RECT &screen) {
        RECT area{};
        if (!window || !GetClientRect(window, &area)) return false;
        POINT corners[2]{{area.left, area.top}, {area.right, area.bottom}};
        MapWindowPoints(window, nullptr, corners, 2u);
        screen = RECT{corners[0].x, corners[0].y, corners[1].x, corners[1].y};
        return screen.right > screen.left && screen.bottom > screen.top;
    }
    static void clip(HWND window) {
        RECT screen{};
        if (client_area(window, screen)) ClipCursor(&screen);
    }
    bool cursor_area(RECT &screen) const { return host_.area ? host_.area(screen) : client_area(window_, screen); }
    bool cursor_position(double &x, double &y) const {
        if (host_.cursor) return host_.cursor(x, y);
        POINT at{};
        if (!window_ || !GetCursorPos(&at)) return false;
        x = at.x; y = at.y;
        return true;
    }
    bool warp_cursor(double x, double y) const {
        if (host_.warp) return host_.warp(x, y);
        return window_ && SetCursorPos(static_cast<int>(x), static_cast<int>(y)) != FALSE;
    }
    // Hidden and confined to the window: that is also what makes a streaming
    // client send movement instead of positions. The show count is the
    // thread's own, so every hide is paired with one show.
    void grab_pointer(bool on) {
        if (host_.grab) { host_.grab(on); return; }
        if (!window_) return;
        if (on) { clip(window_); if (!cursor_hidden_) { ShowCursor(FALSE); cursor_hidden_ = true; } SetCursor(nullptr); }
        else {
            ClipCursor(nullptr);
            if (cursor_hidden_) { ShowCursor(TRUE); cursor_hidden_ = false; }
            SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
        }
    }
    void capture(bool on) {
        if (on && (!capture_allowed_ || scripted_)) return;
        if (on == captured_) return;
        captured_ = on;
        clear_motion();
        if (on) ++report_.captures; else mouse_down_.fill(false);
        grab_pointer(on);
    }
    // The player asked for the mouse: taken now, or when play resumes. False
    // when a replay or a script session rules it out.
    bool request_capture() {
        if (!capture_allowed_ || scripted_) return false;
        if (look_active_) capture(true); else grab_pending_ = true;
        return true;
    }
    bool mouse_button(unsigned index, bool down, LRESULT &result) {
        if (!config_.mouse_enabled) return false;
        if (down) ++report_.mouse_presses;
        if (scripted_) { if (down) ignored(); return false; }
        if (down) mouse_event_ = true;
        if (down && !captured_ && config_.mouse_capture_on_click) {
            const bool activating = std::exchange(activating_click_, false);
            if (!request_capture()) { ignored(); return false; }
            // In play the click that captures also acts (the first shot counts), unless it is the click that
            // brought the window to the front. Where the game has no use for the look controls the mouse is
            // only captured when play resumes, and the click works the menu instead.
            if (captured_ && !activating) { mouse_down_[index] = true; mouse_latch_[index] = true; }
            else if (!captured_ && !activating && config_.mouse_menu_clicks && index < menu_click_.size()) menu_click_[index] = menu_click_samples;
            result = 0; return true;
        }
        if (!down) activating_click_ = false;
        if (!mouse_live()) return false;
        if (down) { mouse_down_[index] = true; mouse_latch_[index] = true; }
        else mouse_down_[index] = false;
        result = 0; return true;
    }
    void raw_input(HRAWINPUT handle) {
        // A mouse report and a pad's USB report fit here; a longer one (a pad on Bluetooth) goes through raw_buffer_.
        union { RAWINPUT input; std::uint64_t room[64]; } local{};
        UINT size = sizeof(local);
        const RAWINPUT *read = &local.input;
        if (GetRawInputData(handle, RID_INPUT, &local, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) {
            size = 0u;
            if (GetRawInputData(handle, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0u || size <= sizeof(local) || size > (1u << 20)) return;
            raw_buffer_.resize((size + 7u) / 8u);
            if (GetRawInputData(handle, RID_INPUT, raw_buffer_.data(), &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) return;
            read = reinterpret_cast<const RAWINPUT *>(raw_buffer_.data());
        }
        const RAWINPUT &input = *read;
        if (input.header.dwType == RIM_TYPEHID) { hid_input(input); return; }
        if (input.header.dwType != RIM_TYPEMOUSE) return;
        const auto &mouse = input.data.mouse;
        const bool absolute = (mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0;
        // A report with buttons only says nothing about how the device reports movement.
        if (absolute || mouse.lLastX || mouse.lLastY) {
            ++(absolute ? report_.raw_absolute_reports : report_.raw_relative_reports);
            report_.last_report = absolute ? MouseSource::RawAbsolute : MouseSource::RawRelative;
        }
        if (!captured_) return;
        if (absolute) {
            // 0-65535 across the primary screen, or across the whole desktop.
            const bool desktop = (mouse.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
            pointer_.raw_absolute(
                (desktop ? GetSystemMetrics(SM_XVIRTUALSCREEN) : 0) + mouse.lLastX / 65535.0 * GetSystemMetrics(desktop ? SM_CXVIRTUALSCREEN : SM_CXSCREEN),
                (desktop ? GetSystemMetrics(SM_YVIRTUALSCREEN) : 0) + mouse.lLastY / 65535.0 * GetSystemMetrics(desktop ? SM_CYVIRTUALSCREEN : SM_CYSCREEN));
        } else pointer_.raw_relative(mouse.lLastX, mouse.lLastY);
    }
    // Once per sample while the mouse is held: the movement since the last
    // sample from whichever measurement is alive. A cursor that has wandered
    // more than a quarter of the window from its middle is put back there, so
    // a pointer that reports positions, or one that is only seen through the
    // cursor, never runs out of screen.
    PointerTracker::Movement take_pointer() {
        double x{}, y{};
        const bool known = cursor_position(x, y);
        const auto moved = pointer_.take(known, x, y);
        RECT screen{};
        if (known && cursor_area(screen)) {
            const double cx = (screen.left + screen.right) / 2, cy = (screen.top + screen.bottom) / 2;
            const double reach = std::min(screen.right - screen.left, screen.bottom - screen.top) / 4.0;
            if (std::fabs(x - cx) > reach || std::fabs(y - cy) > reach) {
                // Remote desktops drop a move to where they believe the pointer already is.
                wobble_ = wobble_ >= 1 ? -1 : wobble_ + 1;
                if (warp_cursor(cx + wobble_, cy)) pointer_.warped(cx + wobble_, cy);
            }
        }
        return moved;
    }
    PadFinder &finder() {
        if (!finder_) finder_ = std::make_unique<PadFinder>(host_.pad, config_.pad_index, host_.pad_worker);
        return *finder_;
    }

    // --- Gamepads ---------------------------------------------------------------------------------
    // The first four sources are the XInput slots, the rest PlayStation pads read through Raw Input.
    static constexpr std::size_t hid_first = PadFinder::slots, hid_pads = 4u;
    // The pad in use changes only to a pad that is touched while the pad in use has been at rest this long,
    // so one pad that shows up twice (a PlayStation pad that a helper program also presents as an Xbox-style
    // pad) neither presses twice nor goes back and forth between its two appearances.
    static constexpr double pad_idle_seconds = 0.25;
    // "The player is using this pad" (which pad is the pad in use, which device was used last) is a button, a
    // trigger past its threshold, or a stick: one that is pushed further out than its dead zone and than
    // pad_use_stick, or one that is past its dead zone and has moved by more than pad_use_travel from where
    // it last stood. The dead zone alone would not do: a worn stick rests a little past it for good, and
    // would keep its pad the pad in use and the prompts on the pad while the player uses another pad or the
    // keyboard. What the game gets from the stick keeps the configured dead zone.
    static constexpr float pad_use_stick = 0.45f, pad_use_travel = 0.1f;
    // A PlayStation pad that says nothing. There are two kinds of pad. One streams: it sends its state every
    // few milliseconds, also when nothing changed. When it falls silent it is out of range or switched off
    // (Windows can take seconds to say so), and after hid_silence_seconds, or hid_silence_intervals of its
    // own report intervals if that is longer, it is taken to be at rest. The other kind sends a report only
    // when something changed (a Bluetooth pad in its plain report mode may): it says nothing while a stick is
    // held, so silence must never put it to rest; only its removal does. Which kind a pad is, is measured:
    // a pad streams once hid_stream_repeats of its reports repeated the state of the report before them, each
    // not later than hid_stream_gap_seconds after it. A report that changes something says nothing either
    // way, a repeat after a longer pause starts the count again, and so does the rule itself when it fires.
    // The silence, and that pause, are counted in the game's own samples (at most hid_poll_gap_seconds
    // each), so a pause of the game's thread, during which reports wait in the window's queue, is not taken
    // for the pad's.
    static constexpr double hid_silence_seconds = 0.5, hid_silence_intervals = 20.0, hid_stream_gap_seconds = 0.1, hid_poll_gap_seconds = 0.1;
    static constexpr unsigned hid_stream_repeats = 24u;
    struct PadSource {
        bool present{};
        PadSample sample{};
        // When something on it was last held; when its last report came (PlayStation pads).
        double touched_at{-1.0e9}, report_at{};
        // Where each stick stood when it last moved by more than pad_use_travel (left x, y, right x, y), and
        // whether the pad was in use at a poll since the last sample.
        std::array<float, 4> stood{};
        bool stood_known{}, used{};
        // PlayStation pads, how the pad reports: the state of its last report, how many reports repeated the
        // one before them, the time between such reports (the largest of the last few), the silence counted
        // since its last report, and whether that silence has been logged.
        PadSample reported{};
        unsigned repeats{};
        double interval{}, silence{};
        bool has_report{}, fresh_report{}, silence_logged{};
        // PlayStation pads: the Raw Input device handle, what the device is, and the device itself (none
        // for a test's made-up pad).
        std::uintptr_t id{};
        HidPadInfo info{};
        std::unique_ptr<HidPadDevice> device;
    };
    bool reads_playstation_pads() const noexcept { return config_.pad_enabled && config_.playstation_pads && config_.pad_index < 0; }
    // Registers the attached window for the reports of HID game pads, or takes the registration back (when
    // PlayStationPads is switched off while the game runs). Game pads (usage 5) and joysticks (usage 4) of the
    // Generic Desktop page: a message when one arrives or leaves (also for the ones that are there when the
    // window registers, which is how a registration made later finds the pads), and their reports also while
    // another window has the focus, as XInput gives them (read() still drops them then unless BackgroundInput
    // is set). Nothing without a window (a test's made-up pads arrive through hid_arrived).
    void register_hid_pads(bool on) {
        if (!window_ || on == hid_registered_) return;
        RAWINPUTDEVICE pads[2]{};
        for (unsigned i = 0; i < 2u; ++i) {
            pads[i].usUsagePage = 1u; pads[i].usUsage = static_cast<USHORT>(5u - i);
            pads[i].dwFlags = on ? RIDEV_DEVNOTIFY | RIDEV_INPUTSINK : RIDEV_REMOVE; pads[i].hwndTarget = on ? window_ : nullptr;
        }
        const bool done = RegisterRawInputDevices(pads, 2u, sizeof(pads[0])) != FALSE;
        if (on) {
            hid_registered_ = done;
            if (!done) log("gamepad: PlayStation pads cannot be read (Raw Input registration failed, error " + std::to_string(GetLastError()) + ")");
        } else hid_registered_ = false;
    }
    bool any_pad() const noexcept { return std::any_of(pads_.begin(), pads_.end(), [](const PadSource &pad) { return pad.present; }); }
    // Whether the player is using this pad right now: a button, a trigger past its threshold, a stick pushed
    // well out or a stick past its dead zone that has moved (pad_use_stick, pad_use_travel). Remembers where
    // the sticks stood, so it is asked once per poll.
    bool pad_touched(PadSource &source) const {
        const PadSample &pad = source.sample;
        if (!source.stood_known) { source.stood = {pad.lx, pad.ly, pad.rx, pad.ry}; source.stood_known = true; }
        const auto stick = [](float x, float y, float deadzone, float &stood_x, float &stood_y) {
            const float reach = std::hypot(x, y);
            const bool moved = std::hypot(x - stood_x, y - stood_y) > pad_use_travel;
            if (moved) { stood_x = x; stood_y = y; }
            return reach > std::max(deadzone, pad_use_stick) || (moved && reach > deadzone);
        };
        const bool left = stick(pad.lx, pad.ly, config_.deadzone_left, source.stood[0], source.stood[1]);
        const bool right = stick(pad.rx, pad.ry, config_.deadzone_right, source.stood[2], source.stood[3]);
        return pad.with_triggers(config_.trigger_threshold) != 0u || left || right;
    }
    // Whether two reports of a pad say the same: its buttons, triggers and sticks, to the last bit.
    static bool same_pad_state(const PadSample &a, const PadSample &b) noexcept {
        return a.buttons == b.buttons && a.lt == b.lt && a.rt == b.rt && a.lx == b.lx && a.ly == b.ly && a.rx == b.rx && a.ry == b.ry;
    }
    PadSource *hid_find(std::uintptr_t id) noexcept {
        for (std::size_t i = hid_first; i < pads_.size(); ++i) if (pads_[i].present && pads_[i].id == id) return &pads_[i];
        return nullptr;
    }
    PadSource *hid_add(std::uintptr_t id, const HidPadInfo &info, std::unique_ptr<HidPadDevice> device) {
        if (!reads_playstation_pads()) return nullptr;
        if (auto *known = hid_find(id)) return known;
        for (std::size_t i = hid_first; i < pads_.size(); ++i) {
            auto &pad = pads_[i];
            if (pad.present) continue;
            pad = {};
            pad.present = true; pad.id = id; pad.info = info; pad.device = std::move(device); pad.report_at = host_.seconds();
            pad.sample.kind = PadKind::PlayStation; pad.sample.vid = info.vid; pad.sample.pid = info.pid;
            ++report_.pads_playstation;
            log("gamepad: " + info.describe());
            if (pad.device && hid_dump_enabled()) log("hid " + pad.device->ids() + " describes itself as: " + pad.device->caps_text());
            return &pad;
        }
        log("gamepad: " + info.describe() + " ignored (four PlayStation pads are read already)");
        return nullptr;
    }
    // The device behind a Raw Input handle: known already, or looked at now. Null when it is not a pad
    // that is read here (an XInput pad, another make, a device that cannot be read); that is remembered, so
    // its reports cost nothing more.
    PadSource *hid_device(HANDLE raw) {
        const auto id = reinterpret_cast<std::uintptr_t>(raw);
        if (auto *known = hid_find(id)) return known;
        if (!reads_playstation_pads() || std::find(hid_refused_.begin(), hid_refused_.end(), id) != hid_refused_.end()) return nullptr;
        HidPadDevice::Refused refused{};
        std::string note;
        auto device = HidPadDevice::open(raw, refused, note);
        PadSource *pad = nullptr;
        if (device) {
            device->set_log([this](const std::string &line) { log(line); });
            const auto info = device->info();
            pad = hid_add(id, info, std::move(device));
        } else if (!note.empty()) log(note);
        else if (refused == HidPadDevice::Refused::Unreadable) log("gamepad: a HID game controller could not be read and is ignored");
        if (!pad && hid_refused_.size() < 64u) hid_refused_.push_back(id);
        return pad;
    }
    // The reports of one WM_INPUT message from a HID device: the last one that could be read is the pad's state.
    void hid_input(const RAWINPUT &input) {
        auto *pad = hid_device(input.header.hDevice);
        if (!pad || !pad->device) return;
        const auto &hid = input.data.hid;
        HidPadReading reading;
        bool read = false;
        for (DWORD i = 0; i < hid.dwCount; ++i) {
            HidPadReading one;
            if (pad->device->read(hid.bRawData + static_cast<std::size_t>(i) * hid.dwSizeHid, hid.dwSizeHid, one)) { reading = one; read = true; }
        }
        if (read) hid_report(pad->id, hid_pad_sample(reading, pad->info.vid, pad->info.pid));
    }
    // A pad is gone: it is not the pad in use any more and nothing is owed to its motors.
    void forget_pad(int index) noexcept {
        if (active_pad_ == index) active_pad_ = -1;
        if (rumble_pad_ == index) { rumble_pad_ = -1; sent_left_ = sent_right_ = 0u; }
    }
    void xinput_present(std::size_t slot, bool here) {
        auto &pad = pads_[slot];
        if (pad.present == here) return;
        if (here) {
            pad.present = true; pad.touched_at = -1.0e9;
            ++report_.pads_xbox;
            log("gamepad: Xbox-style pad, XInput slot " + std::to_string(slot));
        } else {
            forget_pad(static_cast<int>(slot));
            pad = {};
            log("gamepad: XInput slot " + std::to_string(slot) + " removed");
            if (!any_pad()) log("gamepad: none");
        }
    }
    // The silence rule for a PlayStation pad (see hid_silence_seconds): `polled` seconds of the game's samples
    // have passed since the last poll. A pad that has been streaming and has now said nothing for long enough
    // is put to rest until its next report, which the log says once per pad; a pad that never streamed is left
    // as its last report said.
    void hid_silence(PadSource &pad, double polled) {
        if (pad.fresh_report) { pad.fresh_report = false; return; }
        pad.silence += polled;
        if (pad.repeats < hid_stream_repeats) return;
        const double limit = std::max(hid_silence_seconds, hid_silence_intervals * pad.interval);
        if (pad.silence <= limit) return;
        PadSample rest;
        rest.kind = pad.sample.kind; rest.vid = pad.sample.vid; rest.pid = pad.sample.pid; rest.packet = pad.sample.packet;
        pad.sample = rest;
        if (!pad.silence_logged) {
            pad.silence_logged = true;
            char text[200];
            std::snprintf(text, sizeof(text), "gamepad: PlayStation pad %04X:%04X has sent nothing for %.2f s after a report every %.0f ms: taken to be at rest until it reports again",
                unsigned(pad.info.vid), unsigned(pad.info.pid), limit, pad.interval * 1000.0);
            log(text);
        }
        // It has to show again that it streams before its silence means anything.
        pad.repeats = 0u; pad.interval = 0.0;
        ++report_.pad_silences;
    }
    // Reads every pad there is and says which one the player is using; null when there is none. The XInput
    // slots that have a pad are asked here; the PlayStation pads were kept up to date by their reports.
    const PadSample *poll_pads(double now) {
        if (!config_.pad_enabled) return nullptr;
        auto &found = finder();
        const unsigned mask = found.present();
        for (unsigned slot = 0; slot < PadFinder::slots; ++slot) {
            bool here = false;
            if (mask >> slot & 1u) {
                XINPUT_STATE state{};
                if (host_.pad(slot, state)) { here = true; xinput_present(slot, true); pads_[slot].sample = pad_sample_from_xinput(state); }
                else found.lost(slot);
            }
            if (!here) xinput_present(slot, false);
        }
        std::array<bool, PadFinder::slots + hid_pads> touched{};
        // What of the time since the last poll counts as a pad's silence: not a pause of the game's own.
        const double polled = std::clamp(now - polled_at_, 0.0, hid_poll_gap_seconds);
        for (std::size_t i = 0; i < pads_.size(); ++i) {
            auto &pad = pads_[i];
            if (!pad.present) continue;
            if (i >= hid_first) hid_silence(pad, polled);
            touched[i] = pad_touched(pad);
            if (touched[i]) { pad.touched_at = now; pad.used = true; }
        }
        const bool have = active_pad_ >= 0 && pads_[static_cast<std::size_t>(active_pad_)].present;
        if (!have) active_pad_ = -1;
        if (!have || (!touched[static_cast<std::size_t>(active_pad_)] && now - pads_[static_cast<std::size_t>(active_pad_)].touched_at >= pad_idle_seconds)) {
            int next = -1;
            for (std::size_t i = 0; i < pads_.size() && next < 0; ++i) if (pads_[i].present && touched[i]) next = static_cast<int>(i);
            // No pad in use yet: the first one there is, so its kind is known before it is touched.
            for (std::size_t i = 0; i < pads_.size() && next < 0 && !have; ++i) if (pads_[i].present) next = static_cast<int>(i);
            if (next >= 0 && next != active_pad_) {
                active_pad_ = next;
                ++report_.pad_changes;
                if (rumble_test_) rumble_test_until_ = now + 0.5;
            }
        }
        if (!pads_announced_ && found.searches()) {
            // The first look at the XInput slots is done: say so when it found nothing.
            pads_announced_ = true;
            if (!any_pad()) log("gamepad: none");
        }
        polled_at_ = now;
        return active_pad();
    }
    static std::uint16_t motor16(float value) noexcept { return static_cast<std::uint16_t>(std::lround(unit(value) * 65535.0f)); }
    // Sends the motors' values to the pad in use when they differ from what it was sent last; `silent`
    // stops them whatever the values are.
    void apply_rumble(bool silent = false) {
        const float scale = silent || !config_.pad_enabled ? 0.0f : config_.rumble;
        const bool test = !silent && rumble_test_ && polled_at_ < rumble_test_until_;
        const auto left = motor16((test ? 1.0f : rumble_left_) * scale), right = motor16((test ? 1.0f : rumble_right_) * scale);
        if (rumble_pad_ != active_pad_) {
            // The pad that was in use goes quiet.
            if (rumble_pad_ >= 0 && (sent_left_ || sent_right_)) send_rumble(rumble_pad_, 0u, 0u);
            rumble_pad_ = active_pad_; sent_left_ = sent_right_ = 0u;
        }
        if (rumble_pad_ < 0 || (left == sent_left_ && right == sent_right_)) return;
        send_rumble(rumble_pad_, left, right);
        sent_left_ = left; sent_right_ = right;
    }
    void send_rumble(int index, std::uint16_t left, std::uint16_t right) {
        auto &pad = pads_[static_cast<std::size_t>(index)];
        if (!pad.present) return;
        ++report_.vibration_sent;
        // An XInput slot, or a test's made-up PlayStation pad, through the host's function; a real
        // PlayStation pad through its own report (hid_pad.hpp: USB only, never tried on a pad).
        if (pad.device) pad.device->rumble(static_cast<std::uint8_t>(left >> 8), static_cast<std::uint8_t>(right >> 8));
        else if (host_.vibrate) (void)host_.vibrate(static_cast<unsigned>(index), left, right);
    }

    // --- Which device the player used last ------------------------------------------------------------
    // Mouse movement counts as use beyond this much within device_motion_window seconds: counts while the
    // mouse is captured, pixels over the window while it is free (ten times as much, so a mouse that is
    // nudged while a pad is played does not change the prompts).
    static constexpr double device_counts_captured = 4.0, device_counts_free = 40.0, device_motion_window = 0.1;
    bool mouse_moved(double now, double counts) {
        if (counts <= 0.0) return false;
        if (now - motion_since_ > device_motion_window || now < motion_since_) { motion_ = 0.0; motion_since_ = now; }
        motion_ += counts;
        if (motion_ <= (captured_ ? device_counts_captured : device_counts_free)) return false;
        motion_ = 0.0;
        return true;
    }
    // One host sample: keyboard and mouse, or the pad, whichever was used in it; a sample with both or
    // with neither leaves it as it was. A pad that is there before anything at all was touched counts as
    // the device, so a game started with a pad connected shows the pad's buttons from the first menu on.
    // The pad's kind is that of the pad in use, kept after it is unplugged. This is what the prompt pictures
    // follow (input_device.hpp); samples of a replay, of the script channel and of a deterministic run never
    // come here.
    void note_device(bool keyboard_mouse, bool pad_used, const PadSample *pad) {
        auto &shared = input_device_state();
        if (pad) pad_kind_ = pad->kind;
        auto device = device_;
        if (pad_used && !keyboard_mouse) device = InputDevice::Gamepad;
        else if (keyboard_mouse && !pad_used) device = InputDevice::KeyboardMouse;
        else if (pad && !device_touched_) device = InputDevice::Gamepad;
        if (keyboard_mouse || pad_used) device_touched_ = true;
        if (device != device_) { device_ = device; ++report_.device_changes; }
        ++(device_ == InputDevice::Gamepad ? report_.samples_gamepad : report_.samples_keyboard_mouse);
        shared.pad_kind.store(pad_kind_, std::memory_order_relaxed);
        shared.device.store(device_, std::memory_order_relaxed);
    }
    static std::uint8_t stick_byte(float value) {
        return static_cast<std::uint8_t>(std::clamp(std::lround(128.0f + value * 128.0f), 0l, 255l));
    }
    // Stick deflection that makes the engine see `pressure` (0-1) on one axis.
    static float deflection(float pressure) {
        if (pressure <= 0.0f) return 0.0f;
        return engine_stick_deadzone + (1.0f - engine_stick_deadzone) * std::min(pressure, 1.0f);
    }
    // Degrees the mouse asks for on one axis in the coming frame: the counts
    // since the last sample, smoothed, plus what earlier frames did not
    // deliver. Once the mouse rests, a remainder too small to see is dropped
    // so the view rests too.
    double mouse_degrees(double counts, double &smoothed, double carry, float degrees_per_count, bool invert, bool consume) const {
        const double filtered = smoothed * config_.mouse_smoothing + counts * (1.0 - config_.mouse_smoothing);
        if (consume) smoothed = filtered;
        if (filtered == 0.0 && std::fabs(carry) < std::min(0.01, 0.5 * degrees_per_count)) carry = 0.0;
        return filtered * degrees_per_count * (invert ? -1.0 : 1.0) + carry;
    }
    // What is left for later frames of a mouse movement after the game was
    // asked for `delivered` of the `wanted` degrees per second: the rounding
    // of the stick byte, and whatever was beyond the stick's range.
    static double carry(double mouse, double wanted, double delivered, double frame) {
        return mouse != 0.0 ? std::clamp((wanted - delivered) * frame, -60.0, 60.0) : 0.0;
    }
    ControllerInput read(bool consume) {
        ControllerInput out;
        if (!config_.background_input && !host_.focused()) {
            if (consume) { clear_transient(); report_.sent = out; ++report_.samples; apply_rumble(true); }
            return out;
        }
        const auto now = host_.seconds();
        const bool dual = config_.scheme == InputScheme::DualAnalog;
        Accumulated state;
        // For "which device was used last": a bound key down or latched; a mouse button, a wheel notch or
        // enough movement; a button, a trigger or a stick of the pad in use.
        bool keyboard_used = false, mouse_used = mouse_event_ || menu_click_[0] || menu_click_[1], pad_used = false;
        for (const auto &binding : config_.keys)
            if (host_.key_down(binding.vk) || (binding.vk > 0 && binding.vk < 256 && key_latch_[static_cast<std::size_t>(binding.vk)])) {
                press(state, binding.target);
                keyboard_used = true;
            }
        if (mouse_live())
            for (std::size_t i = 0; i < mouse_down_.size(); ++i)
                if (mouse_down_[i] || mouse_latch_[i]) { press(state, config_.mouse_buttons[i]); mouse_used = true; }
        // A click in a menu: left confirms, right goes back. The game's menus go back with Circle (Triangle
        // does nothing in them), so that is what the right click presses there, whatever Mouse2 is bound to.
        if (menu_click_[0]) press(state, PadTarget::Cross);
        if (menu_click_[1]) press(state, PadTarget::Circle);
        // One wheel notch is a one-sample press followed by a one-sample gap.
        if (wheel_up_pending_ && !wheel_gap_) press(state, config_.wheel_up);
        else if (wheel_down_pending_ && !wheel_gap_) press(state, config_.wheel_down);

        // The left stick, -1 to 1, and the turn and pitch asked for in degrees per second.
        float stick_x{}, stick_y{};
        double turn{}, pitch{};
        const PadSample *pad = poll_pads(now);
        if (pad) {
            // In use at this poll or at one since the last sample (the dialogs poll in between).
            pad_used = pads_[static_cast<std::size_t>(active_pad_)].used;
            press_pad(state, *pad);
            // Left stick: radial dead zone, then each axis lifted past the
            // engine's own 30% so the engine sees the stick's pressure directly.
            const float lx = pad->lx, ly = pad->ly;
            const float length = std::hypot(lx, ly);
            if (length > config_.deadzone_left) {
                const float scale = std::min((length - config_.deadzone_left) / (1.0f - config_.deadzone_left), 1.0f) / length;
                const auto axis = [&](float value) {
                    const float pressure = std::min(std::fabs(value * scale), 1.0f);
                    if (pressure < 0.01f) return 0.0f;
                    return std::copysign(config_.compensate_engine_deadzone ? deflection(pressure) : pressure, value);
                };
                stick_x = axis(lx); stick_y = axis(ly);
            }
            // Right stick: radial dead zone, then the engine's own response,
            // the square of the travel for the turn and its cube for the pitch.
            const float rx = pad->rx, ry = pad->ry;
            if (const float reach = dual ? std::hypot(rx, ry) : std::fabs(rx); reach > config_.deadzone_right) {
                const float scale = std::min((reach - config_.deadzone_right) / (1.0f - config_.deadzone_right), 1.0f) / reach;
                const float along = std::min(std::fabs(rx * scale), 1.0f), across = std::min(std::fabs(ry * scale), 1.0f);
                turn += std::copysign(engine_turn_degrees_per_second * config_.pad_turn_sensitivity * along * along, rx);
                if (dual) pitch += std::copysign(engine_pitch_degrees_per_second * config_.pad_look_sensitivity * across * across * across,
                    config_.pad_invert_y ? -ry : ry);
            }
        }
        const float key = config_.look_key_speed;
        turn += ((state.look_right ? 1.0 : 0.0) - (state.look_left ? 1.0 : 0.0)) * engine_turn_degrees_per_second * key * key;
        pitch += ((state.look_down ? 1.0 : 0.0) - (state.look_up ? 1.0 : 0.0)) * engine_pitch_degrees_per_second * key * key * key;
        // Mouse: the movement since the last sample is the angle to cover in the coming frame.
        const double frame = frame_seconds_ > 0.0 ? frame_seconds_ : std::clamp(last_sample_ > 0.0 ? now - last_sample_ : 1.0 / 30.0, 1.0 / 240.0, 0.1);
        double mouse_turn{}, mouse_pitch{};
        // What the mouse moved since the last sample: its counts while captured, pixels over the window while free.
        double mouse_travel = free_motion_;
        if (captured_ && config_.mouse_enabled) {
            const auto moved = consume ? take_pointer() : pointer_.pending();
            mouse_travel = std::fabs(moved.x) + std::fabs(moved.y);
            if (consume && moved.source != MouseSource::None) {
                if (moved.source == MouseSource::Cursor) ++report_.cursor_samples;
                if (moved.x != 0.0 || moved.y != 0.0) {
                    report_.source = moved.source; report_.moved_x = moved.x; report_.moved_y = moved.y; report_.moved_at = now;
                    report_.travel_x += std::fabs(moved.x); report_.travel_y += std::fabs(moved.y);
                }
            }
            if (dual && config_.mouse_exact) {
                // The angle itself: the dual-analog layer turns the player and pitches the view by it in this frame.
                // Nothing is carried over and nothing is capped by the stick's range.
                out.turn = static_cast<float>(std::clamp(mouse_degrees(moved.x, smooth_x_, 0.0, config_.mouse_degrees_per_count, config_.mouse_invert_x, consume), -720.0, 720.0));
                out.pitch = static_cast<float>(std::clamp(mouse_degrees(moved.y, smooth_y_, 0.0, config_.mouse_degrees_per_count_y, config_.mouse_invert_y, consume), -720.0, 720.0));
            } else {
                mouse_turn = mouse_degrees(moved.x, smooth_x_, turn_carry_, config_.mouse_degrees_per_count, config_.mouse_invert_x, consume);
                if (dual) mouse_pitch = mouse_degrees(moved.y, smooth_y_, pitch_carry_, config_.mouse_degrees_per_count_y, config_.mouse_invert_y, consume);
                turn += mouse_turn / frame; pitch += mouse_pitch / frame;
            }
        }
        double turned{}, pitched{};
        if (dual) {
            out.rx = DualAnalogControls::byte_for_turn(static_cast<float>(turn));
            out.ry = DualAnalogControls::byte_for_pitch(static_cast<float>(pitch));
            turned = DualAnalogControls::turn_of(out.rx); pitched = DualAnalogControls::pitch_of(out.ry);
        } else if (std::fabs(turn) > 0.5) {
            // The PSP stick's X axis: the square root undoes the engine's squared response.
            turned = std::clamp(turn, -static_cast<double>(engine_turn_degrees_per_second), static_cast<double>(engine_turn_degrees_per_second));
            stick_x += static_cast<float>(std::copysign(deflection(static_cast<float>(std::sqrt(std::fabs(turned) / engine_turn_degrees_per_second))), turned));
        }
        const float digital = state.walk ? config_.walk_scale : 1.0f;
        stick_x += (state.right ? digital : 0.0f) - (state.left ? digital : 0.0f);
        stick_y += (state.back ? digital : 0.0f) - (state.forward ? digital : 0.0f);
        out.buttons = state.buttons;
        out.x = stick_byte(std::clamp(stick_x, -1.0f, 1.0f));
        out.y = stick_byte(std::clamp(stick_y, -1.0f, 1.0f));
        if (consume) {
            turn_carry_ = carry(mouse_turn, turn, turned, frame);
            pitch_carry_ = dual ? carry(mouse_pitch, pitch, pitched, frame) : 0.0;
            key_latch_.fill(false); mouse_latch_.fill(false);
            for (auto &left : menu_click_) if (left) --left;
            if (wheel_gap_) wheel_gap_ = false;
            else if (wheel_up_pending_) { --wheel_up_pending_; wheel_gap_ = true; }
            else if (wheel_down_pending_) { --wheel_down_pending_; wheel_gap_ = true; }
            last_sample_ = now;
            report_.sent = out; ++report_.samples;
            if (!scripted_) note_device(keyboard_used || mouse_used || mouse_moved(now, mouse_travel), pad_used, pad);
            mouse_event_ = false; free_motion_ = 0.0;
            for (auto &source : pads_) source.used = false;
            // A pad that became the pad in use, or the focus coming back, gets the motors' values.
            apply_rumble(scripted_);
        }
        return out;
    }
    // Samples a menu click is held: the game reads the pad once per frame and wants to see the press.
    static constexpr std::uint8_t menu_click_samples = 3u;
    PcInputConfig config_;
    PcInputHost host_;
    PcInputReport report_;
    std::unique_ptr<PadFinder> finder_;
    std::array<PadSource, PadFinder::slots + hid_pads> pads_{};
    // Raw Input devices that were looked at and are not pads read here.
    std::vector<std::uintptr_t> hid_refused_;
    std::vector<std::uint64_t> raw_buffer_;
    // The pad in use and the pad the motors' values were last sent to (indices into pads_, -1 for none).
    int active_pad_{-1}, rumble_pad_{-1};
    std::uint16_t sent_left_{}, sent_right_{};
    float rumble_left_{}, rumble_right_{};
    std::array<float, 2> game_motor_{};
    double rumble_test_until_{}, polled_at_{};
    // The device used last and the kind of the pad used last, as told to the prompt pictures.
    InputDevice device_{InputDevice::KeyboardMouse};
    PadKind pad_kind_{PadKind::None};
    double motion_{}, motion_since_{}, free_motion_{};
    bool device_touched_{}, mouse_event_{}, pads_announced_{}, hid_registered_{}, rumble_test_{};
    HWND window_{};
    std::array<bool, 256> key_latch_{};
    std::array<bool, 5> mouse_down_{}, mouse_latch_{};
    std::array<std::uint8_t, 2> menu_click_{};
    PointerTracker pointer_;
    double turn_carry_{}, pitch_carry_{}, smooth_x_{}, smooth_y_{};
    double last_sample_{}, frame_seconds_{}, ignored_at_{};
    LPARAM last_move_{-1};
    int wheel_delta_{}, wobble_{};
    unsigned wheel_up_pending_{}, wheel_down_pending_{};
    bool captured_{}, raw_registered_{}, wheel_gap_{}, cursor_hidden_{}, activating_click_{}, show_input_{}, scripted_{};
    bool capture_allowed_{true}, look_active_{true}, grab_pending_{};
};
}
