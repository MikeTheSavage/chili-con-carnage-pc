// Keyboard, mouse and gamepad bindings to the PSP pad: defaults, ChiliNative.ini
// overrides, tap latching, mouse turn, wheel pulses and pad dead zones, in the
// psp scheme and in the dual_analog scheme with its second stick; the controls
// the dual-analog layer defines for the game and the rate each second-stick
// byte stands for; pointers that report positions or only move the cursor;
// the capture rules with a replay, a script session and menus; the title bar's
// input line; an older build's ini file; the gamepad search; the pad-read-to-present timer.
//
// Gamepads (wave 6): the pad sample every kind of pad is read into and that it gives what the XInput
// state gave before; several pads at once (the one touched last is the pad in use, one pad that shows
// up twice presses once); PlayStation pads: the HID position map and the report layouts of the DualShock 4
// and the DualSense on hand-made reports; the device used last (keyboard and mouse or pad) as the prompt
// pictures are told it, and that a replay's, a script's and a deterministic run's samples leave it alone;
// the settings Prompts, Glyphs, PlayStationPads, Rumble and the section [Input.Gamepad.PlayStation];
// vibration.
// NO REAL PAD WAS EVER CONNECTED TO THE PC THESE TESTS WERE WRITTEN ON. The pads are XINPUT_STATE values
// and made-up reports; the byte layouts of the PlayStation pads' reports are from general knowledge of
// the devices, not from a device.
#include "pc_input.hpp"
#include "input_title.hpp"
#include "dual_analog.hpp"
#include "realtime_pacer.hpp"
#include "present_latency.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <set>

namespace {
unsigned checks, failures;
// CHILI_TEST_KEEP_GOING=1: a check that fails is printed and the run goes on, so one run shows every check
// that fails (the way to see what a change of the input code breaks, or what a new check finds in old code).
const bool keep_going = [] { const char *value = std::getenv("CHILI_TEST_KEEP_GOING"); return value && *value == '1'; }();
void require(bool ok, const char *what) {
    ++checks;
    if (ok) return;
    if (!keep_going) throw std::runtime_error(what);
    ++failures;
    std::cerr << "FAILED CHECK: " << what << '\n';
}
constexpr std::uint32_t select_button = 1u, start_button = 8u, up = 0x10u, right = 0x20u, down = 0x40u, left = 0x80u, l_button = 0x100u,
    r_button = 0x200u, triangle = 0x1000u, circle = 0x2000u, cross = 0x4000u, square = 0x8000u;
using Controls = chili::DualAnalogControls;
// Degrees per second the player turns at full pressure on the PSP stick.
constexpr float turn_rate = chili::engine_turn_degrees_per_second;
struct Fixture {
    std::set<int> keys;
    // The pad in XInput slot 1, and pads in the other slots (slots[1] is used while `pad` is empty).
    std::optional<XINPUT_STATE> pad;
    std::array<std::optional<XINPUT_STATE>, 4> slots;
    unsigned pad_queries{};
    bool focused{true};
    double now{100.0};
    // The lines the input layer logged, and what it sent to the motors: the pad's index, the large slow motor, the small fast one.
    std::vector<std::string> log;
    struct Motors { unsigned pad; std::uint16_t slow, fast; };
    std::vector<Motors> motors;
    chili::PcInput input;
    explicit Fixture(chili::PcInputConfig config = chili::PcInputConfig::defaults(chili::InputScheme::Psp)) {
        input.configure(std::move(config));
        chili::PcInputHost host;
        host.focused = [this] { return focused; };
        host.key_down = [this](int key) { return keys.count(key) != 0; };
        host.pad = [this](unsigned slot, XINPUT_STATE &state) {
            ++pad_queries;
            const auto &source = slot == 1u && pad ? pad : slots[slot];
            if (!source) return false;
            state = *source;
            return true;
        };
        host.seconds = [this] { return now; };
        host.pad_worker = false;
        host.vibrate = [this](unsigned index, std::uint16_t slow, std::uint16_t fast) { motors.push_back({index, slow, fast}); return true; };
        host.log = [this](const std::string &line) { log.push_back(line); };
        input.set_host(std::move(host));
    }
    bool logged(std::string_view line) const { return std::find(log.begin(), log.end(), line) != log.end(); }
    // One guest sample one 30 Hz frame after the previous one.
    chili::ControllerInput sample() { now += 1.0 / 30.0; return input.sample(); }
    void key_message(UINT message, int key) { LRESULT result{}; (void)input.window_message(nullptr, message, static_cast<WPARAM>(key), 0, result); }
};
int key(const char *name) { return chili::PcInputConfig::key_from_name(name); }
// The pressure the engine derives from a stick byte on one axis.
float pressure(std::uint8_t byte) { return std::max(0.0f, (std::fabs(byte / 128.0f - 1.0f) - 0.3f) / 0.7f); }
bool close_to(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }
bool binds(const chili::PcInputConfig &config, const char *name, chili::PadTarget target) {
    for (const auto &binding : config.keys) if (binding.vk == key(name) && binding.target == target) return true;
    return false;
}
// The PSP button a target presses; 0 for the stick and look targets and for None.
std::uint32_t psp_mask(chili::PadTarget target) {
    using chili::PadTarget;
    switch (target) {
    case PadTarget::Select: return select_button; case PadTarget::Start: return start_button; case PadTarget::DpadUp: return up;
    case PadTarget::DpadRight: return right; case PadTarget::DpadDown: return down; case PadTarget::DpadLeft: return left;
    case PadTarget::L: return l_button; case PadTarget::R: return r_button; case PadTarget::Triangle: return triangle;
    case PadTarget::Circle: return circle; case PadTarget::Cross: return cross; case PadTarget::Square: return square;
    default: return 0u;
    }
}
// What a pad's buttons gave before the pad sample existed (PcInput::press_pad over the XInput state, as it
// was on main before wave 6): each button bit and each trigger past the threshold through [Input.Gamepad].
std::uint32_t buttons_before_refit(const chili::PcInputConfig &config, const XINPUT_STATE &pad) {
    std::uint32_t out = 0u;
    for (std::size_t i = 0; i < chili::PcInputConfig::pad_button_names.size(); ++i)
        if (pad.Gamepad.wButtons & chili::PcInputConfig::pad_button_names[i].second) out |= psp_mask(config.pad_buttons[i]);
    if (pad.Gamepad.bLeftTrigger / 255.0f > config.trigger_threshold) out |= psp_mask(config.pad_buttons[chili::PcInputConfig::pad_left_trigger]);
    if (pad.Gamepad.bRightTrigger / 255.0f > config.trigger_threshold) out |= psp_mask(config.pad_buttons[chili::PcInputConfig::pad_right_trigger]);
    return out;
}
void reset_shared_state() {
    auto &shared = chili::input_device_state();
    shared.device = chili::InputDevice::KeyboardMouse; shared.pad_kind = chili::PadKind::None;
}
// A made-up PlayStation pad for PcInput::hid_arrived.
chili::HidPadInfo playstation_pad(std::uint16_t pid = 0x0CE6u) {
    chili::HidPadInfo info;
    info.vid = chili::hid_sony_vendor; info.pid = pid; info.model = chili::hid_pad_model(info.vid, pid);
    info.name = "Wireless Controller"; info.buttons = info.model == chili::HidPadModel::DualSense ? 15u : 14u; info.axes = 6u; info.hat = true;
    return info;
}
// A report of `size` zero bytes with the given bytes at its start.
std::vector<std::uint8_t> report_bytes(std::size_t size, std::initializer_list<int> head) {
    std::vector<std::uint8_t> bytes(size, std::uint8_t{0u});
    std::size_t i = 0;
    for (const int value : head) bytes[i++] = static_cast<std::uint8_t>(value);
    return bytes;
}
// Puts the Bluetooth check sum into the last four of the report's first 78 bytes.
void seal_bluetooth(std::vector<std::uint8_t> &report) {
    const std::uint8_t header = 0xA1u;
    const auto sum = chili::hid_crc32(report.data(), chili::hid_bluetooth_report_bytes - 4u, chili::hid_crc32(&header, 1u));
    for (unsigned i = 0; i < 4u; ++i) report[chili::hid_bluetooth_report_bytes - 4u + i] = static_cast<std::uint8_t>(sum >> (8u * i));
}
// The host's devices as the input pipeline reaches them; the game's own is in controller_runtime.cpp.
Fixture *pipeline_host{};
}
chili::ControllerInput chili::poll_host_controller() { return pipeline_host ? pipeline_host->sample() : chili::ControllerInput{}; }

int main() {
    try {
        using chili::PadTarget;
        // The built-in defaults of both schemes parse cleanly.
        {
            chili::PcInputConfig config;
            require(config.apply(chili::default_input_ini(chili::InputScheme::Psp)).empty(), "default ini has problems");
            require(config.scheme == chili::InputScheme::Psp && config.keys.size() == 23u && config.toggle_capture_vk == VK_F1, "default key count");
            require(config.mouse_buttons[0] == PadTarget::Square && config.mouse_buttons[1] == PadTarget::R, "default mouse buttons");
            require(config.pad_buttons[chili::PcInputConfig::pad_right_trigger] == PadTarget::Square, "default trigger");
            require(key("w") == key("W") && key("Esc") == VK_ESCAPE && key("F12") == VK_F12 && key("Numpad7") == VK_NUMPAD7 && key("Nope") == 0 && key("F13") == 0, "key names");

            // The default: Total Overdose PC (input_layout.hpp), on the dual_analog scheme.
            chili::PcInputConfig dual;
            require(dual.apply(chili::default_input_ini()).empty(), "dual_analog default ini has problems");
            require(dual.scheme == chili::InputScheme::DualAnalog && dual.layout == chili::InputLayout::TodPc && dual.layout_named && dual.keys.size() == 31u &&
                    dual.toggle_capture_vk == VK_F1, "the default is Total Overdose PC: 31 keys");
            require(std::string_view(chili::default_input_ini()) == chili::default_input_ini(chili::InputLayout::TodPc) &&
                    std::string_view(chili::default_input_ini(chili::InputScheme::Psp)) == chili::default_input_ini(chili::InputLayout::Psp), "a scheme's defaults are its style's");
            // Total Overdose on PC: W A S D move and strafe, Space shoot-dodges, Left Shift jumps, F uses and enters a vehicle, R
            // reloads, Left Ctrl the loco move, Tab rewinds, Escape the menu, P pauses, Page Up and Down the forks; the
            // chili's own on the free keys beside: E the quick-select and back, Q and C target objects and lean out of a
            // vehicle, V headshot without a mouse, Grave Select. 1 and 2 (weapon classes in Total Overdose) are free.
            require(binds(dual, "W", PadTarget::MoveForward) && binds(dual, "S", PadTarget::MoveBack) && binds(dual, "A", PadTarget::MoveLeft) &&
                    binds(dual, "D", PadTarget::MoveRight) && binds(dual, "Space", PadTarget::Cross) && binds(dual, "LShift", PadTarget::Cross) &&
                    binds(dual, "F", PadTarget::Cross) && binds(dual, "Enter", PadTarget::Cross) && binds(dual, "E", PadTarget::Circle) &&
                    binds(dual, "Backspace", PadTarget::Circle) && binds(dual, "R", PadTarget::Triangle) && binds(dual, "LCtrl", PadTarget::Triangle) &&
                    binds(dual, "Q", PadTarget::L) && binds(dual, "C", PadTarget::L) && binds(dual, "V", PadTarget::R) && binds(dual, "G", PadTarget::Square) &&
                    binds(dual, "Tab", PadTarget::DpadDown) && binds(dual, "Down", PadTarget::DpadDown) && binds(dual, "PageDown", PadTarget::DpadDown) &&
                    binds(dual, "Up", PadTarget::DpadUp) && binds(dual, "PageUp", PadTarget::DpadUp) && binds(dual, "Left", PadTarget::DpadLeft) &&
                    binds(dual, "Right", PadTarget::DpadRight) && binds(dual, "Escape", PadTarget::Start) && binds(dual, "P", PadTarget::Start) &&
                    binds(dual, "Grave", PadTarget::Select) && binds(dual, "X", PadTarget::Walk) && binds(dual, "J", PadTarget::LookLeft) &&
                    binds(dual, "K", PadTarget::LookDown) && dual.toggle_display_vk == VK_F3, "Total Overdose PC's keys");
            for (const char *free : {"1", "2", "3", "4", "5", "6", "7", "8", "Z", "H"})
                require(std::none_of(dual.keys.begin(), dual.keys.end(), [&](const auto &binding) { return binding.vk == key(free); }), "1 to 8, Z and H are free in Total Overdose PC");
            // The order of a line is the order the prompts picture: Space for Cross, E for Circle, R for Triangle, Q for L, Escape for Start.
            const auto first = [&](PadTarget target) { for (const auto &binding : dual.keys) if (binding.target == target) return binding.vk; return 0; };
            require(first(PadTarget::Cross) == VK_SPACE && first(PadTarget::Circle) == key("E") && first(PadTarget::Triangle) == key("R") &&
                    first(PadTarget::L) == key("Q") && first(PadTarget::Start) == VK_ESCAPE && first(PadTarget::DpadDown) == VK_TAB, "the first key of each line");
            // Modern: the port's own layout before tod_pc, as it was.
            const auto modern = chili::PcInputConfig::defaults(chili::InputLayout::Modern);
            require(modern.scheme == chili::InputScheme::DualAnalog && modern.layout == chili::InputLayout::Modern && modern.keys.size() == 27u, "Modern: 27 keys");
            require(binds(modern, "W", PadTarget::MoveForward) && binds(modern, "S", PadTarget::MoveBack) && binds(modern, "A", PadTarget::MoveLeft) &&
                    binds(modern, "D", PadTarget::MoveRight) && binds(modern, "Space", PadTarget::Cross) && binds(modern, "E", PadTarget::Cross) &&
                    binds(modern, "F", PadTarget::Circle) && binds(modern, "R", PadTarget::Triangle) && binds(modern, "Q", PadTarget::L) && binds(modern, "1", PadTarget::DpadLeft) &&
                    binds(modern, "2", PadTarget::DpadRight) && binds(modern, "Z", PadTarget::DpadDown) && binds(modern, "Tab", PadTarget::Select) &&
                    binds(modern, "Escape", PadTarget::Start), "Modern's keys");
            require(modern.mouse_buttons == dual.mouse_buttons && modern.wheel_up == dual.wheel_up && modern.wheel_down == dual.wheel_down && modern.pad_buttons == dual.pad_buttons &&
                    modern.mouse_degrees_per_count == dual.mouse_degrees_per_count && modern.mouse_exact == dual.mouse_exact && modern.exact_turn == dual.exact_turn,
                "Total Overdose PC has Modern's mouse, pad and settings: only keys differ");
            require(config.layout == chili::InputLayout::Psp && chili::PcInputConfig::defaults(chili::InputLayout::Psp).keys.size() == 23u, "PSP original as it was: 23 keys");
            const auto pad_button = [&](const char *name) {
                for (std::size_t i = 0; i < chili::PcInputConfig::pad_button_names.size(); ++i)
                    if (std::string_view(chili::PcInputConfig::pad_button_names[i].first) == name) return dual.pad_buttons[i];
                return PadTarget::None;
            };
            require(pad_button("A") == PadTarget::Cross && pad_button("B") == PadTarget::Circle && pad_button("X") == PadTarget::Triangle &&
                    pad_button("Y") == PadTarget::Cross && pad_button("LB") == PadTarget::DpadLeft && pad_button("RB") == PadTarget::DpadRight &&
                    pad_button("Start") == PadTarget::Start && dual.pad_buttons[chili::PcInputConfig::pad_right_trigger] == PadTarget::Square &&
                    dual.pad_buttons[chili::PcInputConfig::pad_left_trigger] == PadTarget::R, "dual_analog pad buttons");
            require(dual.mouse_buttons[0] == PadTarget::Square && dual.mouse_buttons[1] == PadTarget::R && dual.wheel_up == PadTarget::DpadRight &&
                    dual.wheel_down == PadTarget::DpadLeft && dual.mouse_smoothing == 0.0f && !dual.mouse_invert_y, "dual_analog mouse");
            require(chili::scheme_from_name("psp") == chili::InputScheme::Psp && chili::scheme_from_name("dual_analog") == chili::InputScheme::DualAnalog &&
                    !chili::scheme_from_name("dual") && std::string_view(chili::scheme_name(chili::InputScheme::DualAnalog)) == "dual_analog", "scheme names");
            require(chili::PcInputConfig::defaults().scheme == chili::InputScheme::DualAnalog, "dual_analog is the default scheme");
            require(dual.version == chili::input_ini_version && config.version == chili::input_ini_version && !dual.show_input && !config.show_input &&
                    dual.toggle_display_vk == VK_F3 && config.toggle_display_vk == VK_F3 && dual.mouse_menu_clicks && dual.mouse_capture_on_click &&
                    dual.mouse_hold_pitch,
                "the defaults carry the file version, the input test off, its key, menu clicks and the held pitch");
            // Modern and PSP original have no key Windows has a shortcut on: Shift (Sticky Keys), Ctrl and Alt (language
            // switch, Ctrl+Escape, Alt+Tab, Alt+Space), the Windows keys, Caps Lock, Num Lock.
            static constexpr int modifiers[]{VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_MENU, VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN, VK_CAPITAL, VK_NUMLOCK};
            const auto is_modifier = [](int vk) { return std::find(std::begin(modifiers), std::end(modifiers), vk) != std::end(modifiers); };
            for (const auto *defaults : std::initializer_list<const chili::PcInputConfig *>{&config, &modern}) {
                bool clean = defaults->toggle_capture_vk == VK_F1;
                for (const auto &binding : defaults->keys) clean = clean && !is_modifier(binding.vk);
                require(clean, "a default key collides with a Windows shortcut");
            }
            // Total Overdose PC keeps Left Shift (jump) and Left Ctrl (the loco move) as Total Overdose had them, but only
            // beside a plain key of the same target: nothing needs a modifier, and no Alt, Windows key, Caps or Num Lock.
            {
                bool only_beside = true, others = true;
                for (const auto &binding : dual.keys) {
                    if (!is_modifier(binding.vk)) continue;
                    others = others && (binding.vk == VK_LSHIFT || binding.vk == VK_LCONTROL);
                    only_beside = only_beside && std::any_of(dual.keys.begin(), dual.keys.end(), [&](const auto &plain) { return plain.target == binding.target && !is_modifier(plain.vk); });
                }
                require(others && only_beside && binds(dual, "LShift", PadTarget::Cross) && binds(dual, "LCtrl", PadTarget::Triangle),
                    "Total Overdose PC: Left Shift and Left Ctrl only as further keys of a target that has a plain key");
            }
        }
        // Keyboard: buttons, stick directions, opposite keys cancel, walk modifier.
        {
            Fixture f;
            auto s = f.sample();
            require(s.buttons == 0u && s.x == 128u && s.y == 128u, "idle is neutral");
            f.keys = {key("W"), VK_SPACE, key("G")};
            s = f.sample();
            require(s.buttons == (cross | square) && s.x == 128u && s.y == 0u, "forward, cross, square");
            f.keys = {key("S"), key("D"), VK_ESCAPE, VK_TAB, key("V"), key("Q"), key("R"), key("F")};
            s = f.sample();
            require(s.buttons == (start_button | select_button | r_button | l_button | triangle | circle) && s.x == 255u && s.y == 255u, "back, right and face buttons");
            f.keys = {key("A"), key("D"), key("W"), key("S"), VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT};
            s = f.sample();
            require(s.buttons == (up | down | left | right) && s.x == 128u && s.y == 128u, "opposite keys cancel, d-pad");
            f.keys = {key("W"), key("X")};
            s = f.sample();
            require(s.y == 51u && std::fabs(pressure(s.y) - 0.43f) < 0.02f, "walk modifier scales the stick");
            f.keys = {VK_RETURN, key("Z"), VK_BACK, key("1"), key("2")};
            s = f.sample();
            require(s.buttons == (cross | down | circle | left | right), "second keys of a target");
            f.keys = {key("E")};
            require(f.sample().buttons == cross, "E is Cross");
            f.keys = {VK_LSHIFT, VK_LCONTROL, VK_LMENU};
            s = f.sample();
            require(s.buttons == 0u && s.x == 128u && s.y == 128u, "Shift, Ctrl and Alt do nothing by default");
        }
        // A press shorter than a frame is latched for exactly one sample; peek does not consume it.
        {
            auto config = chili::PcInputConfig::defaults(chili::InputScheme::Psp);
            require(config.apply("[Input.Keyboard]\nSquare=LCtrl\nWalk=LAlt\n").empty(), "modifier keys can still be bound");
            Fixture f(config);
            f.key_message(WM_KEYDOWN, VK_SPACE);
            f.key_message(WM_KEYUP, VK_SPACE);
            require(f.input.peek().buttons == cross && f.input.peek().buttons == cross, "peek keeps the latch");
            require(f.sample().buttons == cross && f.sample().buttons == 0u, "latched tap lasts one sample");
            LRESULT result{};
            (void)f.input.window_message(nullptr, WM_KEYDOWN, VK_SPACE, 1ll << 30, result);
            require(f.sample().buttons == 0u, "auto-repeat is not a new press");
            // Generic modifier messages latch the left/right key they came from.
            (void)f.input.window_message(nullptr, WM_KEYDOWN, VK_CONTROL, 0, result);
            require(f.sample().buttons == square, "left control from a generic message");
            (void)f.input.window_message(nullptr, WM_KEYDOWN, VK_CONTROL, 1ll << 24, result);
            require(f.sample().buttons == 0u, "right control is not bound");
            require(f.input.window_message(nullptr, WM_SYSCOMMAND, SC_KEYMENU, 0, result) && result == 0, "Alt does not open the window menu");
            require(!f.input.window_message(nullptr, WM_SYSCOMMAND, SC_CLOSE, 0, result), "other system commands pass through");
        }
        // No input while another application has the focus, and latches are dropped.
        {
            Fixture f;
            f.keys = {VK_SPACE, key("W")};
            f.key_message(WM_KEYDOWN, VK_RETURN);
            f.focused = false;
            auto s = f.sample();
            require(s.buttons == 0u && s.x == 128u && s.y == 128u, "unfocused is neutral");
            f.focused = true; f.keys.clear();
            require(f.sample().buttons == 0u, "latch cleared while unfocused");
            auto config = chili::PcInputConfig::defaults(chili::InputScheme::Psp);
            require(config.apply("[Input]\nBackgroundInput=true\n").empty(), "background option");
            Fixture background(config);
            background.focused = false; background.keys = {VK_SPACE};
            require(background.sample().buttons == cross, "background input when asked for");
        }
        // ini overrides: a target that appears replaces its keys; mistakes are reported, the rest applies.
        {
            auto config = chili::PcInputConfig::defaults(chili::InputScheme::Psp);
            const auto problems = config.apply(
                "[Input.Keyboard]\nCross = K , Numpad0 ; comment\nSquare=\nTriangle=Nope\nBogus=W\nWalkScale=2\n"
                "[Input.Mouse]\nMouse1=Cross\nSensitivityX=0.5\nCapture=never\nMouse9=Cross\n"
                "[Input.Gamepad]\nA=Square\nLT=Nope\nIndex=1\nDeadzoneLeft=0.5\n[Input]\nScheme=dual\nnonsense\n");
            require(problems.size() == 7u, "seven problems reported");
            Fixture f(config);
            f.keys = {VK_SPACE, VK_RETURN, key("E"), key("G")};
            require(f.sample().buttons == 0u, "replaced and cleared bindings are gone");
            f.keys = {key("K")};
            require(f.sample().buttons == cross, "new key bound");
            f.keys = {VK_NUMPAD0, key("R")};
            require(f.sample().buttons == cross, "second new key; target with a bad key list is unbound");
            require(config.mouse_buttons[0] == PadTarget::Cross && !config.mouse_capture_on_click && config.mouse_degrees_per_count == 0.5f &&
                    config.pad_index == 1 && config.deadzone_left == 0.5f && config.walk_scale == 0.6f, "values applied, bad ones kept at defaults");
            require(config.scheme == chili::InputScheme::Psp, "an unknown scheme name leaves the scheme alone");
            require(config.apply("[Input]\nScheme=Dual_Analog\n[Input.Mouse]\nSensitivityY=0.2\nInvertY=true\nSmoothing=0.5\n"
                                 "[Input.Gamepad]\nLookSensitivity=0.5\nTurnSensitivity=2.5\nInvertY=true\nDeadzoneRight=0.1\nTriggerThreshold=0.5\n"
                                 "[Input.Keyboard]\nLookUp=PageUp\nLookKeySpeed=0.4\n").empty(), "dual_analog settings");
            require(config.scheme == chili::InputScheme::DualAnalog && config.mouse_degrees_per_count_y == 0.2f && config.mouse_invert_y &&
                    config.mouse_smoothing == 0.5f && config.pad_look_sensitivity == 0.5f && config.pad_turn_sensitivity == 2.5f && config.pad_invert_y &&
                    config.deadzone_right == 0.1f && config.trigger_threshold == 0.5f && config.look_key_speed == 0.4f, "dual_analog values applied");
            require(config.apply("[Input.Mouse]\nSmoothing=1\nSensitivityY=0\n[Input.Keyboard]\nLookKeySpeed=0\n"
                                 "[Input.Gamepad]\nTurnSensitivity=4.5\nDeadzoneRight=0.95\n").size() == 5u, "out-of-range values reported");
        }
        // Mouse: buttons need the grab; movement becomes a turn rate; the engine's cap carries over.
        {
            Fixture f;
            LRESULT result{};
            require(!f.input.window_message(nullptr, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0, result), "wheel ignored before the grab");
            f.input.set_captured_for_test(true);
            require(f.input.window_message(nullptr, WM_LBUTTONDOWN, 0, 0, result), "button handled");
            require(f.sample().buttons == square, "mouse 1 shoots");
            require(f.input.window_message(nullptr, WM_LBUTTONUP, 0, 0, result) && f.sample().buttons == 0u, "release");
            (void)f.input.window_message(nullptr, WM_RBUTTONDOWN, 0, 0, result);
            (void)f.input.window_message(nullptr, WM_RBUTTONUP, 0, 0, result);
            require(f.sample().buttons == r_button && f.sample().buttons == 0u, "a click shorter than a frame still registers once");
            (void)f.input.window_message(nullptr, WM_XBUTTONDOWN, MAKEWPARAM(0, XBUTTON1), 0, result);
            require(f.sample().buttons == 0u, "unbound mouse button");
            (void)f.input.window_message(nullptr, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1), 0, result);

            // 25 counts at 0.08 degrees per count in one 30 Hz frame is 60 degrees per second.
            f.input.add_mouse_counts(25.0);
            auto s = f.sample();
            const float rate = turn_rate * pressure(s.x) * pressure(s.x);
            require(s.x > 128u && std::fabs(rate - 60.0f) < 4.0f && s.y == 128u, "mouse right turns right at the asked rate");
            require(f.sample().x == 128u, "no movement, no turn");
            f.input.add_mouse_counts(-25.0);
            s = f.sample();
            require(s.x < 128u && std::fabs(turn_rate * pressure(s.x) * pressure(s.x) - 60.0f) < 4.0f, "mouse left turns left");
            // 150 counts = 12 degrees; a frame at the cap of 171 degrees per second gives 5.7, the rest follows.
            f.input.add_mouse_counts(150.0);
            require(f.sample().x == 255u && f.sample().x == 255u, "fast movement saturates and carries over");
            s = f.sample();
            require(s.x > 128u && s.x < 255u && std::fabs(turn_rate * pressure(s.x) * pressure(s.x) - 18.0f) < 3.0f, "the last 0.6 degrees");
            require(f.sample().x == 128u, "carry is used up");
            // Turning and moving combine on the stick's X axis and clamp.
            f.keys = {key("D")}; f.input.add_mouse_counts(-10.0);
            s = f.sample();
            require(s.x > 128u && s.x < 255u, "keyboard and mouse sum");
            f.keys.clear();
            // The game's own frame time, when the host is told it, is what turns the angle into a rate:
            // the same 2 degrees in a 60 Hz frame is 120 degrees per second.
            f.input.set_frame_seconds(1.0 / 60.0);
            f.input.add_mouse_counts(25.0);
            s = f.sample();
            require(std::fabs(turn_rate * pressure(s.x) * pressure(s.x) - 120.0f) < 6.0f, "frame time sets the rate");
            // A slow game: the engine's frame time stops at a tenth of a second (0.1f is a little more than 0.1).
            f.input.set_frame_seconds(0.1f);
            f.input.add_mouse_counts(25.0);
            s = f.sample();
            require(std::fabs(turn_rate * pressure(s.x) * pressure(s.x) - 20.0f) < 3.0f, "the engine's longest frame time is accepted");
            f.input.set_frame_seconds(5.0);
            f.input.add_mouse_counts(25.0);
            s = f.sample();
            require(std::fabs(turn_rate * pressure(s.x) * pressure(s.x) - 60.0f) < 4.0f, "a frame time that makes no sense falls back to the sample interval");

            // Two wheel notches are two one-sample presses with a gap between them.
            (void)f.input.window_message(nullptr, WM_MOUSEWHEEL, MAKEWPARAM(0, 2 * WHEEL_DELTA), 0, result);
            require(f.sample().buttons == right && f.sample().buttons == 0u && f.sample().buttons == right && f.sample().buttons == 0u && f.sample().buttons == 0u, "wheel up pulses");
            (void)f.input.window_message(nullptr, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0, result);
            require(f.sample().buttons == left && f.sample().buttons == 0u, "wheel down pulse");
            // Losing the focus drops the grab and everything held.
            (void)f.input.window_message(nullptr, WM_LBUTTONDOWN, 0, 0, result);
            (void)f.input.window_message(nullptr, WM_KILLFOCUS, 0, 0, result);
            require(!f.input.mouse_captured() && f.sample().buttons == 0u, "focus loss releases the mouse");
        }
        // Gamepad: found by the search, buttons, triggers, left stick past both dead zones, right stick turns.
        {
            Fixture f;
            XINPUT_STATE pad{};
            pad.Gamepad.wButtons = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_DPAD_LEFT;
            pad.Gamepad.bRightTrigger = 200; pad.Gamepad.bLeftTrigger = 20;
            f.pad = pad;
            require(f.sample().buttons == 0u && f.pad_queries == 0u, "the sampling thread does not look for a pad");
            f.input.scan_pads();
            require(f.pad_queries == 4u, "a search asks every slot, so a pad beside another is found");
            auto s = f.sample();
            require(s.buttons == (cross | triangle | l_button | start_button | left | square), "pad buttons and right trigger");
            require(s.x == 128u && s.y == 128u, "centred sticks");
            const auto queries = f.pad_queries;
            (void)f.sample();
            require(f.pad_queries == queries + 1u, "a found pad costs one query per sample");
            pad = {}; pad.Gamepad.sThumbLY = 32767;
            f.pad = pad; s = f.sample();
            require(s.y == 0u && s.x == 128u, "full forward");
            pad.Gamepad.sThumbLY = 6000;
            f.pad = pad; s = f.sample();
            require(s.y == 128u, "inside the pad dead zone");
            // Half of the travel left after the pad dead zone is half pressure in the engine.
            pad.Gamepad.sThumbLY = 0; pad.Gamepad.sThumbLX = static_cast<SHORT>(32767 * (0.24f + 0.76f * 0.5f));
            f.pad = pad; s = f.sample();
            require(s.x > 128u && std::fabs(pressure(s.x) - 0.5f) < 0.03f, "left stick compensates the engine dead zone");
            pad = {}; pad.Gamepad.sThumbRX = -32767;
            f.pad = pad; s = f.sample();
            require(s.x == 0u && s.y == 128u, "right stick turns");
            // Half of the right stick's travel is a quarter of the engine's turn rate, as on its own stick.
            pad.Gamepad.sThumbRX = static_cast<SHORT>(32767 * (0.24f + 0.76f * 0.5f)); pad.Gamepad.sThumbRY = 32767;
            f.pad = pad; s = f.sample();
            require(s.x > 128u && std::fabs(pressure(s.x) - 0.5f) < 0.03f && s.y == 128u, "psp: right stick X only, squared response");
            // A pad that stops answering is given up after one failed read; the sampling thread never scans.
            f.pad.reset();
            (void)f.sample();
            const auto before = f.pad_queries;
            f.now += 30.0;
            for (int i = 0; i < 10; ++i) (void)f.sample();
            require(f.pad_queries == before, "no query on the sampling thread while there is no pad");
            f.input.scan_pads();
            require(f.pad_queries == before + 4u, "a search with no pad asks every slot once");
            pad = {}; pad.Gamepad.wButtons = XINPUT_GAMEPAD_B; f.pad = pad;
            f.input.scan_pads();
            require(f.sample().buttons == circle, "a pad plugged in later is found by the next search");

            auto config = chili::PcInputConfig::defaults(chili::InputScheme::Psp);
            require(config.apply("[Input.Gamepad]\nEnabled=false\n").empty(), "pad off");
            Fixture off(config);
            pad = {}; pad.Gamepad.wButtons = XINPUT_GAMEPAD_A; off.pad = pad;
            off.input.scan_pads();
            require(off.sample().buttons == 0u && off.pad_queries == 0u, "disabled pad is never queried");
            require(config.apply("[Input.Gamepad]\nEnabled=true\nIndex=3\n").empty(), "fixed slot");
            Fixture fixed(config);
            fixed.pad = pad;
            fixed.input.scan_pads();
            require(fixed.pad_queries == 1u && fixed.sample().buttons == 0u, "a fixed slot is the only one asked");
        }
        // The native dialogs get the gamepad alone: a key or mouse button bound to Cross is theirs to handle,
        // so one Enter press is one action.
        {
            Fixture f(chili::PcInputConfig::defaults());
            f.keys = {VK_RETURN, VK_SPACE, VK_UP};
            f.input.set_captured_for_test(true);
            LRESULT result{};
            (void)f.input.window_message(nullptr, WM_LBUTTONDOWN, 0, 0, result);
            require(f.input.peek().buttons == (cross | up | square), "the game sees the keys and the mouse");
            require(f.input.peek_pad().buttons == 0u, "the dialogs do not");
            XINPUT_STATE pad{};
            pad.Gamepad.wButtons = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_DPAD_DOWN; pad.Gamepad.sThumbLX = 32767; pad.Gamepad.sThumbRX = 32767;
            f.pad = pad;
            f.input.scan_pads();
            const auto dialog = f.input.peek_pad();
            require(dialog.buttons == (cross | down) && dialog.x == 128u && dialog.rx == 128u, "the dialogs get the pad's buttons");
            f.focused = false;
            require(f.input.peek_pad().buttons == 0u, "and nothing while another application has the focus");
        }
        // The gamepad search on its worker thread: at start, again when a pad is lost or a device changes, never on
        // the caller's thread; every slot is asked, so a second pad is found beside the first.
        {
            std::atomic<unsigned> asked{0u};
            // One bit per slot with a pad.
            std::atomic<unsigned> present{1u << 2};
            const auto caller = GetCurrentThreadId();
            std::atomic<bool> on_caller{false};
            const auto wait_for = [](const auto &done) { for (int i = 0; i < 3000 && !done(); ++i) Sleep(1); return done(); };
            {
                chili::PadFinder finder([&](unsigned slot, XINPUT_STATE &) {
                    ++asked; if (GetCurrentThreadId() == caller) on_caller = true; return (present.load() >> slot & 1u) != 0u; }, -1, true);
                require(wait_for([&] { return finder.searches() == 1u; }) && finder.slot() == 2 && finder.present() == 4u, "the worker finds the pad at start");
                require(asked.load() == 4u, "every slot is asked once");
                Sleep(30);
                require(asked.load() == 4u, "the next search of its own is two seconds away");
                present = 1u | (1u << 2);
                finder.look();
                require(wait_for([&] { return finder.present() == 5u; }) && finder.slot() == 0, "a device change finds a second pad, in a lower slot too");
                present = 1u;
                auto searched = finder.searches();
                finder.lost(2u);
                require(finder.present() == 1u && wait_for([&] { return finder.searches() > searched; }) && finder.present() == 1u, "a lost pad is forgotten at once and looked for");
                present = 0u;
                searched = finder.searches();
                finder.lost(0u);
                require(wait_for([&] { return finder.searches() > searched; }) && finder.slot() == -1, "no pad: the search comes up empty");
                const auto idle = asked.load();
                Sleep(50);
                require(asked.load() == idle, "and waits before the next search");
                present = 1u << 3;
                finder.look();
                require(wait_for([&] { return finder.slot() == 3; }), "a device change starts a search");
            }
            require(!on_caller.load(), "every query ran on the worker");
            chili::PadFinder fixed([&](unsigned slot, XINPUT_STATE &) { return slot == 1u; }, 0, false);
            fixed.scan();
            require(fixed.slot() == -1 && fixed.searches() == 1u, "without a worker nothing is found until asked; a fixed slot ignores the others");
        }
        // A second pad that arrives without any device-change message while the first one is there: the worker asks
        // all four slots again every two seconds, pad or no pad, so the pad is found and the game reads it. (Real
        // time: this block waits for one such search.)
        {
            std::atomic<unsigned> present{1u << 2};
            std::vector<std::string> lines;
            chili::PcInput input;
            input.configure(chili::PcInputConfig::defaults());
            chili::PcInputHost host;
            host.focused = [] { return true; };
            host.key_down = [](int) { return false; };
            host.pad = [&](unsigned slot, XINPUT_STATE &state) { state = {}; return (present.load() >> slot & 1u) != 0u; };
            host.vibrate = [](unsigned, std::uint16_t, std::uint16_t) { return true; };
            host.log = [&](const std::string &line) { lines.push_back(line); };
            host.pad_worker = true;
            input.set_host(std::move(host));
            const auto logged = [&](std::string_view line) { return std::find(lines.begin(), lines.end(), line) != lines.end(); };
            // The game's thread goes on taking samples; the looking is the worker's.
            const auto sample_until = [&](const auto &done, unsigned milliseconds) {
                for (const auto end = GetTickCount64() + milliseconds; GetTickCount64() < end && !done();) { (void)input.sample(); Sleep(5); }
                return done();
            };
            require(sample_until([&] { return logged("gamepad: Xbox-style pad, XInput slot 2"); }, 3000), "the worker finds the pad that is there at the start");
            present = (1u << 2) | 1u;
            const auto before = GetTickCount64();
            require(sample_until([&] { return logged("gamepad: Xbox-style pad, XInput slot 0"); }, 5500),
                "a second pad that arrives without a device-change message is found by the next search, with a pad present");
            std::cout << "[pads] a second pad was found " << (GetTickCount64() - before) << " ms after it arrived, without a device-change message\n";
        }
        // First run writes the defaults; a later run reads the edited file.
        {
            wchar_t temp[MAX_PATH]{};
            GetTempPathW(MAX_PATH, temp);
            const auto file = std::filesystem::path(temp) / ("chili-input-test-" + std::to_string(GetCurrentProcessId()) + ".ini");
            std::filesystem::remove(file);
            std::vector<std::string> problems;
            auto config = chili::PcInputConfig::load(file, problems);
            require(problems.empty() && std::filesystem::exists(file) && config.keys.size() == 31u && config.layout == chili::InputLayout::TodPc &&
                    config.scheme == chili::InputScheme::DualAnalog, "first run creates the file with the defaults of Total Overdose PC");
            { std::ofstream edit(file, std::ios::binary | std::ios::trunc); edit << "[Input.Keyboard]\r\nCross=J\r\nwhat\r\n"; }
            config = chili::PcInputConfig::load(file, problems);
            require(problems.size() == 1u && config.keys.size() == 28u, "edited file is read, CRLF accepted");
            // A file that names psp and nothing else gets the psp bindings, not the dual_analog ones.
            { std::ofstream edit(file, std::ios::binary | std::ios::trunc); edit << "[Input]\nScheme=psp\n"; }
            config = chili::PcInputConfig::load(file, problems);
            std::filesystem::remove(file);
            const auto psp = chili::PcInputConfig::defaults(chili::InputScheme::Psp);
            require(problems.empty() && config.scheme == chili::InputScheme::Psp && config.layout == chili::InputLayout::Psp && config.keys.size() == 23u &&
                    config.pad_buttons == psp.pad_buttons, "unlisted settings follow the file's scheme");
            // A Layout line names the style whose defaults the lines a file leaves out take.
            { std::ofstream edit(file, std::ios::binary | std::ios::trunc); edit << "[Input]\nLayout=modern\n[Input.Keyboard]\nWalk=H\n"; }
            config = chili::PcInputConfig::load(file, problems);
            require(problems.empty() && config.layout == chili::InputLayout::Modern && config.scheme == chili::InputScheme::DualAnalog && config.keys.size() == 27u &&
                    binds(config, "F", PadTarget::Circle) && binds(config, "H", PadTarget::Walk) && !binds(config, "X", PadTarget::Walk), "Layout=modern: Modern's keys where the file has none");
            { std::ofstream edit(file, std::ios::binary | std::ios::trunc); edit << "[Input]\nScheme=dual_analog\n"; }
            config = chili::PcInputConfig::load(file, problems);
            require(problems.empty() && config.layout == chili::InputLayout::TodPc && config.keys.size() == 31u, "no Layout line with dual_analog: Total Overdose PC");
            { std::ofstream edit(file, std::ios::binary | std::ios::trunc); edit << "[Input]\nLayout=tod_pc\nScheme=psp\n"; }
            config = chili::PcInputConfig::load(file, problems);
            require(problems.empty() && config.layout == chili::InputLayout::TodPc && config.scheme == chili::InputScheme::Psp && binds(config, "LShift", PadTarget::Cross),
                "the Layout line and the scheme are two lines: a file may pair them otherwise");
            require(chili::PcInputConfig().apply("[Input]\nLayout=dual_analog\n").size() == 1u && chili::PcInputConfig().apply("[Input]\nLayout=TOD_PC\n").empty(),
                "an unknown style is reported; names without case");
            std::filesystem::remove(file);
            require(!std::filesystem::exists(file) && chili::PcInputConfig::load(file, problems, false).scheme == chili::InputScheme::DualAnalog &&
                    !std::filesystem::exists(file), "a missing file is not created when asked not to");
        }
        // dual_analog: the move keys and the left stick strafe; the mouse, the look keys and the right stick
        // go out on the second stick as a turn and a pitch rate and never touch the first.
        {
            Fixture f(chili::PcInputConfig::defaults());
            auto s = f.sample();
            require(s.x == 128u && s.y == 128u && s.rx == 128u && s.ry == 128u, "dual idle is neutral");
            f.keys = {key("A"), key("W")};
            s = f.sample();
            require(s.x == 0u && s.y == 0u && s.rx == 128u && s.ry == 128u, "A and W push the left stick only");
            f.keys = {key("F"), key("Q"), key("G"), key("R"), key("E")};
            require(f.sample().buttons == (cross | l_button | square | triangle | circle), "F uses (Cross), Q is L, G shoots, R reloads, E is Circle");
            f.keys = {key("V"), key("C"), key("P")};
            require(f.sample().buttons == (r_button | l_button | start_button), "V is R, C is L, P pauses");
            f.keys = {VK_LSHIFT};
            require(f.sample().buttons == cross, "Left Shift jumps (Cross), as in Total Overdose");
            f.keys = {VK_LCONTROL};
            require(f.sample().buttons == triangle, "Left Ctrl is the loco move (Triangle), as in Total Overdose");
            f.keys = {VK_TAB, VK_PRIOR, VK_OEM_3};
            require(f.sample().buttons == (down | up | select_button), "Tab rewinds, Page Up raises the forks, Grave is Select");
            f.keys = {key("1"), key("2"), key("Z"), key("H"), VK_RSHIFT, VK_RCONTROL, VK_LMENU};
            s = f.sample();
            require(s.buttons == 0u && s.x == 128u && s.y == 128u, "1, 2, Z, H, the right Shift and Ctrl and Alt do nothing in Total Overdose PC");
            Fixture modern(chili::PcInputConfig::defaults(chili::InputLayout::Modern));
            modern.keys = {key("V"), key("1"), key("2")};
            require(modern.sample().buttons == (r_button | left | right), "Modern: V is R, 1 and 2 change weapon");
            modern.keys = {key("E"), key("F"), VK_LSHIFT, VK_LCONTROL};
            require(modern.sample().buttons == (cross | circle), "Modern: E uses, F is Circle, Shift and Ctrl do nothing");
            // LookKeySpeed 0.7: the engine's own response to 70% pressure, 0.49 of its turn rate and 0.343 of its pitch rate.
            f.keys = {key("L")};
            s = f.sample();
            require(s.x == 128u && s.rx > 128u && close_to(Controls::turn_of(s.rx), turn_rate * 0.49, 3.0) && s.ry == 128u, "look key turns on the second stick");
            f.keys = {key("I")};
            s = f.sample();
            require(s.ry < 128u && close_to(Controls::pitch_of(s.ry), -19.65, 1.5) && s.rx == 128u, "look-up key");
            f.keys = {key("J"), key("L"), key("I"), key("K")};
            s = f.sample();
            require(s.rx == 128u && s.ry == 128u, "opposite look keys cancel");
            f.keys.clear();

            // The mouse is an angle of its own, beside the sticks: exactly the movement, in the sample it arrived in.
            require(f.input.config().mouse_exact, "exact mouse look is the default");
            f.input.set_captured_for_test(true);
            f.input.add_mouse_counts(25.0, 10.0);
            s = f.sample();
            require(s.x == 128u && s.y == 128u && s.rx == 128u && s.ry == 128u, "the mouse moves neither stick");
            require(close_to(s.turn, 25 * 0.08, 1e-5) && close_to(s.pitch, 10 * 0.08, 1e-5), "the mouse's angle goes out as it is");
            s = f.sample();
            require(s.turn == 0.0f && s.pitch == 0.0f, "nothing is carried into the next sample");
            f.input.add_mouse_counts(-1000.0, -3.0);
            s = f.sample();
            require(close_to(s.turn, -80.0, 1e-4) && close_to(s.pitch, -0.24, 1e-5) && f.sample().turn == 0.0f, "a flick is one angle, however large");
            // The look keys stay on the second stick and add to the mouse.
            f.keys = {key("L")}; f.input.add_mouse_counts(5.0);
            s = f.sample();
            require(s.rx > 128u && close_to(s.turn, 0.4, 1e-5), "look key on the stick, mouse beside it");
            f.keys.clear();
            // The game's frame time does not enter it.
            f.input.set_frame_seconds(1.0 / 60.0);
            f.input.add_mouse_counts(25.0);
            require(close_to(f.sample().turn, 2.0, 1e-5), "the angle does not depend on the frame time");
            f.input.set_frame_seconds(0.0);
            f.input.set_captured_for_test(false);
            f.input.add_mouse_counts(50.0, 50.0);
            s = f.sample();
            require(s.turn == 0.0f && s.pitch == 0.0f, "a free mouse does not look");
            {
                auto inverted = chili::PcInputConfig::defaults();
                require(inverted.apply("[Input.Mouse]\nInvertY=true\nInvertX=true\nSensitivityY=0.16\n").empty(), "invert options");
                Fixture g(inverted);
                g.input.set_captured_for_test(true);
                g.input.add_mouse_counts(10.0, 5.0);
                s = g.sample();
                require(close_to(s.turn, -0.8, 1e-5) && close_to(s.pitch, -0.8, 1e-5), "inverted axes at their own sensitivity");
                auto smooth = chili::PcInputConfig::defaults();
                require(smooth.apply("[Input.Mouse]\nSmoothing=0.5\n").empty(), "smoothing option");
                Fixture h(smooth);
                h.input.set_captured_for_test(true);
                h.input.add_mouse_counts(40.0);
                const auto one = h.sample(), two = h.sample(), three = h.sample();
                require(close_to(one.turn, 1.6, 1e-5) && close_to(two.turn, 0.8, 1e-5) && close_to(three.turn, 0.4, 1e-5), "smoothed movement tails off");
            }
        }
        // [Input] ExactTurn: the second stick's turn as each frame's angle, on unless the ini says otherwise.
        {
            auto config = chili::PcInputConfig::defaults();
            require(config.exact_turn && chili::PcInputConfig::defaults(chili::InputScheme::Psp).exact_turn, "ExactTurn is on by default");
            require(config.apply("[Input]\nExactTurn=false\n").empty() && !config.exact_turn, "ExactTurn option");
            require(config.apply("[Input]\nExactTurn=maybe\n").size() == 1u && !config.exact_turn, "ExactTurn takes true or false");
            require(std::string_view(chili::default_input_ini()).find("\nExactTurn=true\n") != std::string_view::npos, "the first-run file names ExactTurn");
        }
        // dual_analog with [Input.Mouse] Exact=false: the mouse pushes the second stick, as a turn and a pitch rate.
        {
            auto stick = chili::PcInputConfig::defaults();
            require(stick.apply("[Input.Mouse]\nExact=false\n").empty() && !stick.mouse_exact, "Exact option");
            Fixture f(stick);
            auto s = f.sample();
            f.input.set_captured_for_test(true);
            // Samples until the second stick is back at rest, and the degrees turned and pitched on the way.
            double turned{}, pitched{};
            const auto settle = [&] {
                for (int i = 0; i < 8; ++i) {
                    const auto rest = f.sample();
                    if (rest.rx == 128u && rest.ry == 128u) return true;
                    turned += Controls::turn_of(rest.rx) / 30.0; pitched += Controls::pitch_of(rest.ry) / 30.0;
                }
                return false;
            };
            // 25 counts right and 10 down at 0.08 degrees per count in a 30 Hz frame: 60 and 24 degrees per second.
            f.input.add_mouse_counts(25.0, 10.0);
            s = f.sample();
            require(s.x == 128u && s.y == 128u && s.rx > 128u && s.ry > 128u && s.turn == 0.0f && s.pitch == 0.0f, "mouse moves the second stick only");
            require(close_to(Controls::turn_of(s.rx), 60.0, 4.0), "mouse turn rate");
            require(close_to(Controls::pitch_of(s.ry), 24.0, 2.0), "mouse pitch rate");
            // What the byte's rounding left over follows in the next frames, then the view rests.
            turned = Controls::turn_of(s.rx) / 30.0; pitched = Controls::pitch_of(s.ry) / 30.0;
            require(settle(), "no movement, no look");
            require(close_to(turned, 25 * 0.08, 0.02) && close_to(pitched, 10 * 0.08, 0.02), "the angle asked for is the angle turned");
            f.input.add_mouse_counts(0.0, -10.0);
            s = f.sample();
            require(s.ry < 128u && s.rx == 128u, "mouse up looks up");
            require(settle(), "rest after looking up");
            // Moving and looking at once stay on their own sticks.
            f.keys = {key("D")}; f.input.add_mouse_counts(-25.0);
            s = f.sample();
            require(s.x == 255u && s.rx < 128u, "strafe right while turning left");
            f.keys.clear();
            require(settle(), "rest after turning left");
            // A mouse turns faster than the PSP stick could: 300 counts in a frame is 24 degrees, 720 degrees per second.
            f.input.add_mouse_counts(300.0);
            s = f.sample();
            require(s.rx > 128u && s.rx < 255u && close_to(Controls::turn_of(s.rx), 720.0, 25.0), "a fast mouse turn is not capped at the PSP stick's rate");
            require(settle(), "rest after a fast turn");
            // 20 frames of 7 counts turn 11.2 degrees.
            turned = 0.0;
            for (int i = 0; i < 20; ++i) { f.input.add_mouse_counts(7.0); turned += Controls::turn_of(f.sample().rx) / 30.0; }
            require(settle() && close_to(turned, 20 * 7 * 0.08, 0.02), "steady movement adds up to its angle");
            // Faster than the second stick can say: saturate, carry the rest over.
            f.input.add_mouse_counts(1000.0);
            const auto first = f.sample(), second = f.sample();
            require(first.rx == 255u && second.rx > 128u && second.rx < 255u, "a flick beyond the range saturates and carries over");
            turned = (Controls::turn_of(first.rx) + Controls::turn_of(second.rx)) / 30.0;
            require(settle() && close_to(turned, 1000 * 0.08, 0.02), "and loses nothing below the carry limit");
            f.input.add_mouse_counts(0.0, 1000.0);
            require(f.sample().ry == 255u && settle(), "pitch saturates the same way");

            auto inverted = stick;
            require(inverted.apply("[Input.Mouse]\nInvertY=true\nSensitivityY=0.16\n").empty(), "invert option");
            Fixture g(inverted);
            g.input.set_captured_for_test(true);
            g.input.add_mouse_counts(0.0, 5.0);
            s = g.sample();
            require(s.ry < 128u && close_to(Controls::pitch_of(s.ry), -24.0, 2.0), "inverted Y at its own sensitivity");

            // Smoothing spreads a movement over the following samples.
            auto smooth = stick;
            require(smooth.apply("[Input.Mouse]\nSmoothing=0.5\n").empty(), "smoothing option");
            Fixture h(smooth);
            h.input.set_captured_for_test(true);
            h.input.add_mouse_counts(40.0);
            const auto one = h.sample(), two = h.sample(), three = h.sample();
            require(one.rx > two.rx && two.rx > three.rx && three.rx > 128u, "smoothed movement tails off");
            Fixture plain(stick);
            plain.input.set_captured_for_test(true);
            plain.input.add_mouse_counts(40.0);
            const auto whole = plain.sample();
            require(whole.rx > one.rx && close_to(Controls::turn_of(whole.rx) / 30.0, 40 * 0.08, 0.2), "smoothing is off by default: the whole movement in one frame");

            // The psp scheme has no second stick: mouse Y and the look-up keys do nothing, look-left keys turn.
            auto psp = chili::PcInputConfig::defaults(chili::InputScheme::Psp);
            require(psp.apply("[Input.Keyboard]\nLookLeft=J\nLookUp=I\n").empty(), "look keys in psp");
            Fixture old(psp);
            old.input.set_captured_for_test(true);
            old.input.add_mouse_counts(25.0, 40.0);
            s = old.sample();
            require(s.x > 128u && s.y == 128u && s.rx == 128u && s.ry == 128u, "psp: mouse X turns on the stick, mouse Y is ignored");
            old.keys = {key("J"), key("I")};
            s = old.sample();
            require(s.x < 128u && s.y == 128u && s.rx == 128u && s.ry == 128u, "psp: look-left key turns, look-up key does nothing");
        }
        // dual_analog gamepad: left stick moves, right stick is the second stick with a radial dead zone.
        {
            Fixture f(chili::PcInputConfig::defaults());
            XINPUT_STATE pad{};
            pad.Gamepad.wButtons = XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_LEFT_THUMB;
            pad.Gamepad.bLeftTrigger = 200;
            f.pad = pad;
            f.input.scan_pads();
            auto s = f.sample();
            require(s.buttons == (triangle | cross | left | l_button | r_button), "dual pad buttons");
            pad = {}; pad.Gamepad.wButtons = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_START; pad.Gamepad.bRightTrigger = 255;
            f.pad = pad; s = f.sample();
            require(s.buttons == (cross | circle | right | start_button | square), "A jumps, B is Circle, RB next weapon, Start pauses, RT shoots");
            // Full deflection at sensitivity 1 is the engine's own full rate.
            pad = {}; pad.Gamepad.sThumbLX = -32767; pad.Gamepad.sThumbRX = 32767;
            f.pad = pad; s = f.sample();
            require(s.x == 0u && s.y == 128u && s.ry == 128u && close_to(Controls::turn_of(s.rx), turn_rate, 6.0), "left stick strafes, right stick turns");
            pad = {}; pad.Gamepad.sThumbRY = 32767;
            f.pad = pad; s = f.sample();
            require(s.rx == 128u && s.x == 128u && close_to(Controls::pitch_of(s.ry), -57.3, 3.0), "right stick up looks up");
            pad.Gamepad.sThumbRY = 6000; pad.Gamepad.sThumbRX = -3000;
            f.pad = pad; s = f.sample();
            require(s.rx == 128u && s.ry == 128u, "inside the right dead zone");
            // Half of the travel left after the dead zone: a quarter of the turn rate, an eighth of the pitch rate.
            pad.Gamepad.sThumbRX = 0; pad.Gamepad.sThumbRY = static_cast<SHORT>(-32767 * (0.24f + 0.76f * 0.5f));
            f.pad = pad; s = f.sample();
            require(s.ry > 128u && close_to(Controls::pitch_of(s.ry), 57.3 / 8.0, 0.8), "half of the right stick's travel pitches at an eighth");
            pad.Gamepad.sThumbRY = 0; pad.Gamepad.sThumbRX = static_cast<SHORT>(-32767 * (0.24f + 0.76f * 0.5f));
            f.pad = pad; s = f.sample();
            require(s.rx < 128u && close_to(Controls::turn_of(s.rx), -turn_rate / 4.0, 3.0), "and turns at a quarter");

            auto config = chili::PcInputConfig::defaults();
            require(config.apply("[Input.Gamepad]\nInvertY=true\nLookSensitivity=0.5\nTurnSensitivity=2\nDeadzoneRight=0.5\n").empty(), "pad look options");
            Fixture g(config);
            pad = {}; pad.Gamepad.sThumbRY = 32767; g.pad = pad;
            g.input.scan_pads();
            s = g.sample();
            require(s.ry > 128u && close_to(Controls::pitch_of(s.ry), 57.3 * 0.5, 2.0), "inverted pad Y at half sensitivity");
            pad = {}; pad.Gamepad.sThumbRX = 32767; g.pad = pad;
            s = g.sample();
            require(s.rx > 128u && close_to(Controls::turn_of(s.rx), turn_rate * 2.0, 12.0), "turn sensitivity 2 doubles the engine's rate");
            pad.Gamepad.sThumbRX = 16000; g.pad = pad;
            require(g.sample().rx == 128u, "a wider dead zone from the ini");
        }
        // The grab follows play: freed while the game is not using the look controls, taken back when it is,
        // and never taken at all in a scripted run.
        {
            Fixture f(chili::PcInputConfig::defaults());
            LRESULT result{};
            require(f.input.window_message(nullptr, WM_LBUTTONDOWN, 0, 0, result) && f.input.mouse_captured(), "first click grabs");
            (void)f.input.window_message(nullptr, WM_LBUTTONUP, 0, 0, result);
            require(f.sample().buttons == square && f.sample().buttons == 0u, "in play the click that grabs also shoots, once");
            // Held: the shot lasts as long as the button.
            (void)f.input.window_message(nullptr, WM_KILLFOCUS, 0, 0, result);
            require(f.input.window_message(nullptr, WM_LBUTTONDOWN, 0, 0, result) && f.input.mouse_captured(), "grabbed again");
            require(f.sample().buttons == square && f.sample().buttons == square, "the grabbing click is held like any other");
            (void)f.input.window_message(nullptr, WM_LBUTTONUP, 0, 0, result);
            require(f.sample().buttons == 0u, "and released");
            // The click that brings the window to the front grabs and does nothing else.
            (void)f.input.window_message(nullptr, WM_KILLFOCUS, 0, 0, result);
            (void)f.input.window_message(nullptr, WM_MOUSEACTIVATE, 0, MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN), result);
            require(f.input.window_message(nullptr, WM_LBUTTONDOWN, 0, 0, result) && f.input.mouse_captured(), "the activating click grabs");
            (void)f.input.window_message(nullptr, WM_LBUTTONUP, 0, 0, result);
            require(f.sample().buttons == 0u, "the activating click does not shoot");
            f.input.set_look_active(false);
            require(!f.input.mouse_captured(), "menus free the mouse");
            f.input.add_mouse_counts(50.0, 50.0);
            auto s = f.sample();
            require(s.rx == 128u && s.ry == 128u && s.turn == 0.0f && s.pitch == 0.0f, "a free mouse does not look");
            f.input.set_look_active(true);
            require(f.input.mouse_captured(), "play takes the mouse back");
            f.input.set_look_active(false);
            (void)f.input.window_message(nullptr, WM_KILLFOCUS, 0, 0, result);
            f.input.set_look_active(true);
            require(!f.input.mouse_captured(), "no grab after the window lost the focus");
            // A click in a menu is remembered instead of grabbing there.
            f.input.set_look_active(false);
            require(f.input.window_message(nullptr, WM_LBUTTONDOWN, 0, 0, result) && !f.input.mouse_captured(), "no grab in a menu");
            f.input.set_look_active(true);
            require(f.input.mouse_captured(), "grab once play starts");
            // The toggle key frees it for good.
            f.key_message(WM_KEYDOWN, VK_F1);
            require(!f.input.mouse_captured(), "F1 frees the mouse");
            f.input.set_look_active(false); f.input.set_look_active(true);
            require(!f.input.mouse_captured(), "and it stays free");
            f.key_message(WM_KEYDOWN, VK_F1);
            require(f.input.mouse_captured(), "F1 grabs it again");
            (void)f.input.window_message(nullptr, WM_ACTIVATE, WA_INACTIVE, 0, result);
            require(!f.input.mouse_captured(), "deactivating the window frees it");

            Fixture scripted(chili::PcInputConfig::defaults());
            scripted.input.forbid_capture();
            require(!scripted.input.window_message(nullptr, WM_LBUTTONDOWN, 0, 0, result) && !scripted.input.mouse_captured(), "scripted run: a click does not grab");
            scripted.key_message(WM_KEYDOWN, VK_F1);
            scripted.input.set_look_active(false); scripted.input.set_look_active(true);
            require(!scripted.input.mouse_captured(), "scripted run: nothing grabs");
        }
        // Pointers. A mouse reports movement; a streamed or remote pointer reports positions.
        {
            chili::MouseMotion mouse;
            mouse.relative(3.0, -2.0); mouse.relative(4.0, 1.0);
            require(mouse.x == 7.0 && mouse.y == -1.0 && !mouse.strayed(0.0, 0.0, 1.0), "a mouse adds up its movement and has no position");
            mouse.take();
            require(mouse.x == 0.0 && mouse.y == 0.0, "taken");

            chili::MouseMotion pointer;
            pointer.absolute(500.0, 300.0);
            require(pointer.x == 0.0 && pointer.y == 0.0, "the first position is no movement");
            pointer.absolute(510.0, 296.0); pointer.absolute(530.0, 300.0);
            require(pointer.x == 30.0 && pointer.y == 0.0, "movement is the change of position");
            require(!pointer.strayed(480.0, 272.0, 136.0), "near the middle");
            pointer.take();
            pointer.absolute(900.0, 300.0);
            require(pointer.x == 370.0 && pointer.strayed(480.0, 272.0, 136.0), "far from the middle");
            pointer.take();
            // Put back in the middle, and the device follows (remote desktop): its next position counts from there.
            pointer.warped(480.0, 272.0);
            pointer.absolute(486.0, 270.0);
            require(pointer.x == 6.0 && pointer.y == -2.0, "a pointer that followed the move is measured from the middle");
            pointer.absolute(496.0, 270.0);
            require(pointer.x == 16.0, "and carries on from there");
            pointer.take();
            // Put back, and the device ignores it (it has its own idea of where it is): nothing is lost or invented.
            pointer.absolute(905.0, 300.0);
            pointer.take();
            pointer.warped(480.0, 272.0);
            pointer.absolute(912.0, 301.0); pointer.absolute(915.0, 303.0);
            require(pointer.x == 10.0 && pointer.y == 3.0, "a pointer that did not follow keeps counting from where it was");
            pointer.take();
            pointer.absolute(916.0, 303.0); pointer.absolute(917.0, 303.0); pointer.absolute(918.0, 303.0);
            pointer.take();
            pointer.absolute(500.0, 280.0);
            require(pointer.x == -418.0, "long after the move a jump is movement again");
            pointer.reset();
            pointer.absolute(10.0, 10.0);
            require(pointer.x == 0.0 && !pointer.strayed(10.0, 10.0, 1.0), "reset forgets the position");
        }
        // One sample's movement from whichever measurement is alive.
        {
            using Tracker = chili::PointerTracker;
            using chili::MouseSource;
            Tracker mouse;
            mouse.raw_relative(3.0, -1.0); mouse.raw_relative(2.0, 0.0);
            require(mouse.pending().x == 5.0 && mouse.pending().source == MouseSource::RawRelative, "raw movement is pending until taken");
            auto moved = mouse.take(true, 400.0, 300.0);
            require(moved.x == 5.0 && moved.y == -1.0 && moved.source == MouseSource::RawRelative, "a mouse: raw relative reports");
            moved = mouse.take(true, 405.0, 299.0);
            require(moved.source == MouseSource::None && moved.x == 0.0, "the cursor trailing a raw report is not movement");
            mouse.raw_relative(4.0, 0.0);
            moved = mouse.take(true, 409.0, 299.0);
            require(moved.x == 4.0 && moved.source == MouseSource::RawRelative, "and the next raw report counts once");

            // A streamed pointer that reports positions through raw input.
            Tracker tablet;
            tablet.raw_absolute(1000.0, 500.0);
            moved = tablet.take(true, 1000.0, 500.0);
            require(moved.source == MouseSource::RawAbsolute && moved.x == 0.0, "the first position is no movement");
            tablet.raw_absolute(1012.0, 497.0);
            moved = tablet.take(true, 1012.0, 497.0);
            require(moved.source == MouseSource::RawAbsolute && moved.x == 12.0 && moved.y == -3.0, "raw positions: the change of position");

            // No raw input at all: the cursor is the device after three samples, and their movement is not lost.
            Tracker remote;
            require(remote.take(true, 500.0, 300.0).source == MouseSource::None, "first cursor position");
            require(remote.take(true, 504.0, 300.0).source == MouseSource::None && remote.take(true, 509.0, 301.0).source == MouseSource::None,
                "two samples of cursor movement are held back");
            moved = remote.take(true, 515.0, 301.0);
            require(moved.source == MouseSource::Cursor && moved.x == 15.0 && moved.y == 1.0, "the third delivers all of it");
            moved = remote.take(true, 518.0, 300.0);
            require(moved.source == MouseSource::Cursor && moved.x == 3.0 && moved.y == -1.0, "then every sample");
            require(remote.take(true, 518.0, 300.0).source == MouseSource::None, "a resting cursor is no movement");
            // Put back in the middle by the game: counted from there.
            remote.warped(480.0, 272.0);
            moved = remote.take(true, 486.0, 272.0);
            require(moved.source == MouseSource::Cursor && moved.x == 6.0 && moved.y == 0.0, "after the game moved the cursor the count starts there");
            // Raw input coming to life takes over at once.
            remote.raw_relative(-9.0, 0.0);
            moved = remote.take(true, 477.0, 272.0);
            require(moved.source == MouseSource::RawRelative && moved.x == -9.0, "raw input wins over the cursor");
            require(remote.take(true, 470.0, 272.0).source == MouseSource::None, "and the cursor has to prove itself again");
            require(remote.take(false, 0.0, 0.0).source == MouseSource::None, "an unknown cursor position is nothing");
        }
        // The input layer with a pointer that only moves the cursor: movement is measured, the cursor is hidden
        // and confined while captured, and put back in the middle when it strays.
        {
            struct Screen { double x{480.0}, y{272.0}; int warps{}, grabs{}, frees{}; } screen;
            Fixture f(chili::PcInputConfig::defaults());
            f.input.set_host({[&f] { return f.focused; }, [&f](int k) { return f.keys.count(k) != 0; }, [](unsigned, XINPUT_STATE &) { return false; },
                [&f] { return f.now; }, false,
                [&screen](double &x, double &y) { x = screen.x; y = screen.y; return true; },
                [&screen](double x, double y) { screen.x = x; screen.y = y; ++screen.warps; return true; },
                [](RECT &area) { area = RECT{0, 0, 960, 544}; return true; },
                [&screen](bool on) { ++(on ? screen.grabs : screen.frees); }});
            LRESULT result{};
            require(f.input.window_message(nullptr, WM_LBUTTONDOWN, 0, 0, result) && f.input.mouse_captured() && screen.grabs == 1 && screen.frees == 0,
                "a click captures: the cursor is hidden and confined");
            (void)f.input.window_message(nullptr, WM_LBUTTONUP, 0, 0, result);
            auto s = f.sample();
            double turned = 0.0;
            for (int i = 0; i < 6; ++i) { screen.x += 10.0; s = f.sample(); turned += s.turn; }
            require(close_to(turned, 60 * 0.08, 1e-4) && f.sample().turn == 0.0f, "60 pixels of cursor movement turn 4.8 degrees");
            require(f.input.report().source == chili::MouseSource::Cursor && f.input.report().cursor_samples >= 4u && f.input.report().travel_x == 60.0,
                "the report names the cursor as the measurement");
            require(screen.warps == 0, "near the middle the cursor is left alone");
            // 136 pixels is a quarter of the window's height: beyond it the cursor goes back to the middle.
            screen.x = 480.0 + 150.0;
            (void)f.sample();
            require(screen.warps == 1 && std::fabs(screen.x - 480.0) <= 1.0 && screen.y == 272.0, "a strayed cursor is put back in the middle");
            s = f.sample();
            require(s.turn == 0.0f && f.input.report().travel_x == 150.0, "putting it back is not movement");
            screen.y -= 20.0;
            s = f.sample();
            require(close_to(s.pitch, -20 * 0.08, 1e-4), "cursor up looks up");
            // Raw reports from here on: the cursor is no longer the measurement.
            f.input.add_mouse_counts(25.0);
            screen.x += 25.0;
            (void)f.sample();
            require(f.input.report().source == chili::MouseSource::RawRelative && f.input.report().moved_x == 25.0, "raw reports take over");
            f.key_message(WM_KEYDOWN, VK_F1);
            require(!f.input.mouse_captured() && screen.frees == 1, "F1 gives the cursor back");
            f.key_message(WM_KEYDOWN, VK_F1);
            (void)f.input.window_message(nullptr, WM_ACTIVATEAPP, FALSE, 0, result);
            require(!f.input.mouse_captured() && screen.grabs == 2 && screen.frees == 2, "Alt+Tab to another application gives it back");
            f.key_message(WM_KEYDOWN, VK_F1);
            f.input.release();
            require(!f.input.mouse_captured() && screen.frees == 3, "and so does the end of the game");
            // A device that reports positions through raw input is re-centred the same way.
            f.key_message(WM_KEYDOWN, VK_F1);
            screen.x = 480.0; screen.y = 272.0; screen.warps = 0;
            f.input.add_mouse_position(480.0, 272.0); (void)f.sample();
            f.input.add_mouse_position(700.0, 272.0); screen.x = 700.0;
            s = f.sample();
            require(close_to(s.turn, 220 * 0.08, 1e-4) && f.input.report().source == chili::MouseSource::RawAbsolute && f.input.report().moved_x == 220.0 && screen.warps == 1,
                "raw positions turn and the strayed cursor is put back");
            // The device did not follow the cursor: its next position continues from where it was.
            f.input.add_mouse_position(710.0, 272.0);
            (void)f.sample();
            require(f.input.report().moved_x == 10.0, "a device that keeps its own position loses nothing");
            f.input.release();
        }
        // A replay owns the input: nothing the player does is kept, the mouse stays free, and the title says why.
        // When it ends a click captures. A session with the script channel never captures.
        {
            Fixture f(chili::PcInputConfig::defaults());
            LRESULT result{};
            using chili::InputSource;
            const chili::ControllerInput idle{};
            require(chili::input_title_text(f.input, InputSource::Host, idle) == "click to capture the mouse", "title: free mouse");
            f.input.set_scripted(true);
            require(chili::input_title_text(f.input, InputSource::Replay, idle).empty(), "title: a replay nobody disturbs keeps the plain title");
            require(!f.input.window_message(nullptr, WM_LBUTTONDOWN, 0, 0, result) && !f.input.mouse_captured() && !f.input.capture_pending(),
                "replay: a click does not capture");
            require(f.input.seconds_since_ignored() < 1.0 && f.input.report().ignored == 1u, "the click is counted as ignored");
            require(chili::input_title_text(f.input, InputSource::Replay, idle) ==
                    "a replay is driving the input: keyboard and mouse are ignored until it ends", "title: the replay notice");
            f.key_message(WM_KEYDOWN, VK_SPACE);
            f.key_message(WM_KEYDOWN, VK_F1);
            (void)f.input.window_message(nullptr, WM_MOUSEMOVE, 0, MAKELPARAM(10, 10), result);
            (void)f.input.window_message(nullptr, WM_MOUSEMOVE, 0, MAKELPARAM(10, 10), result);
            (void)f.input.window_message(nullptr, WM_MOUSEMOVE, 0, MAKELPARAM(30, 10), result);
            require(f.input.report().ignored == 4u && !f.input.mouse_captured(),
                "a key, the capture key and a mouse movement are ignored; the first position is not a movement");
            f.now += 6.0;
            require(chili::input_title_text(f.input, InputSource::Replay, idle).empty(), "the notice goes away");
            f.input.set_scripted(true);
            f.input.set_scripted(false);
            require(f.sample().buttons == 0u, "nothing pressed during the replay arrives after it");
            require(f.input.window_message(nullptr, WM_LBUTTONDOWN, 0, 0, result) && f.input.mouse_captured(), "after the replay a click captures");
            require(chili::input_title_text(f.input, InputSource::Host, idle) == "mouse captured, F1 frees it", "title: captured");
            // A script command takes the samples again: the mouse is let go.
            f.input.set_scripted(true);
            require(!f.input.mouse_captured() && !f.input.capture_pending(), "a script command frees the mouse");
            f.input.set_scripted(false);
            f.input.set_look_active(false);
            (void)f.input.window_message(nullptr, WM_LBUTTONUP, 0, 0, result);
            (void)f.input.window_message(nullptr, WM_LBUTTONDOWN, 0, 0, result);
            require(chili::input_title_text(f.input, InputSource::Host, idle) == "the mouse is captured when play starts, F1 cancels", "title: waiting for play");

            Fixture scripted(chili::PcInputConfig::defaults());
            scripted.input.forbid_capture();
            require(chili::input_title_text(scripted.input, InputSource::Host, idle).empty(), "script session: plain title");
            (void)scripted.input.window_message(nullptr, WM_LBUTTONDOWN, 0, 0, result);
            require(chili::input_title_text(scripted.input, InputSource::Host, idle) == "scripted session: the mouse is not captured", "script session: a click is answered");

            // The input test line.
            auto shown = chili::PcInputConfig::defaults();
            require(shown.apply("[Input]\nShowInput=true\n").empty(), "ShowInput option");
            Fixture test(shown);
            require(test.input.show_input(), "the input test starts on when the ini says so");
            test.input.set_captured_for_test(true);
            test.input.add_mouse_counts(12.0, -3.0);
            test.keys = {key("W")};
            (void)test.input.window_message(nullptr, WM_LBUTTONDOWN, 0, 0, result);
            const auto sent = test.sample();
            const auto line = chili::input_title_text(test.input, InputSource::Host, sent);
            require(line.find("mouse captured, F1 frees it | dual_analog | input from player | mouse captured") == 0u &&
                    line.find("moved +12 -3 by raw relative, applied as that angle |") != std::string::npos && line.find("sent buttons 8000 move 128,0 look") != std::string::npos,
                "the input test line shows the capture, the movement with its source and what was sent");
            require(line.ends_with(" | pad: none | last device: keyboard"), "and the pad in use and the device used last");
            test.key_message(WM_KEYDOWN, VK_F3);
            require(!test.input.show_input() && test.sample().buttons == square, "F3 switches the line off and is not a game key");
            const auto summary = chili::input_summary(test.input);
            require(summary.find("travel=12,3 counts") != std::string::npos && summary.find("last movement by raw relative") != std::string::npos &&
                    summary.find("button presses=1") != std::string::npos, "the exit summary");
            require(summary.find("[input] gamepad: pad in use none; pads that arrived: xbox-style=0, playstation=0") != std::string::npos &&
                    summary.find("[input] device used last: keyboard; host samples with the pad=0, with keyboard and mouse=2, changes=0") != std::string::npos &&
                    summary.find("[input] vibration: calls by the game=0, settings sent to a pad=0") != std::string::npos, "the exit summary's gamepad lines");
        }
        // Menus (the game is not using the look controls): a left click confirms and a right click goes back,
        // except the click that brings the window to the front.
        {
            Fixture f(chili::PcInputConfig::defaults());
            LRESULT result{};
            f.input.set_look_active(false);
            require(f.input.window_message(nullptr, WM_LBUTTONDOWN, 0, 0, result) && !f.input.mouse_captured() && f.input.capture_pending(), "a menu click does not capture");
            (void)f.input.window_message(nullptr, WM_LBUTTONUP, 0, 0, result);
            require(f.sample().buttons == cross && f.sample().buttons == cross && f.sample().buttons == cross && f.sample().buttons == 0u,
                "a left click in a menu is Cross for three samples");
            // The menus go back with Circle (Triangle does nothing there), so that is what the right click presses.
            (void)f.input.window_message(nullptr, WM_RBUTTONDOWN, 0, 0, result);
            (void)f.input.window_message(nullptr, WM_RBUTTONUP, 0, 0, result);
            const auto back = f.sample().buttons;
            require(back == circle, "a right click in a menu is Circle: back");
            require(f.sample().buttons == circle && f.sample().buttons == circle && f.sample().buttons == 0u, "for three samples, like the left click");
            (void)f.input.window_message(nullptr, WM_MBUTTONDOWN, 0, 0, result);
            require(f.sample().buttons == 0u, "the other buttons do nothing in a menu");
            (void)f.input.window_message(nullptr, WM_MOUSEACTIVATE, 0, MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN), result);
            (void)f.input.window_message(nullptr, WM_LBUTTONDOWN, 0, 0, result);
            (void)f.input.window_message(nullptr, WM_LBUTTONUP, 0, 0, result);
            require(f.sample().buttons == 0u, "the click that activates the window confirms nothing");
            (void)f.input.window_message(nullptr, WM_LBUTTONDOWN, 0, 0, result);
            require(f.sample().buttons == cross, "the next one does");
            // Play starts: the mouse is captured and a click shoots.
            f.input.set_look_active(true);
            require(f.input.mouse_captured(), "captured when play starts");
            for (int i = 0; i < 3; ++i) (void)f.sample();
            (void)f.input.window_message(nullptr, WM_LBUTTONDOWN, 0, 0, result);
            require(f.sample().buttons == square, "in play mouse 1 shoots");
            (void)f.input.window_message(nullptr, WM_LBUTTONUP, 0, 0, result);
            (void)f.sample();
            (void)f.input.window_message(nullptr, WM_RBUTTONDOWN, 0, 0, result);
            require(f.sample().buttons == r_button, "and mouse 2 is R, as it is bound: only the menus' right click is Circle");
            (void)f.input.window_message(nullptr, WM_RBUTTONUP, 0, 0, result);
            require(std::string_view(chili::default_input_ini()).find("a left click is Cross (confirm, skip) and a right click Circle (back)") != std::string_view::npos,
                "the first-run file says what the right click does in a menu");

            auto off = chili::PcInputConfig::defaults();
            require(off.apply("[Input.Mouse]\nMenuClicks=false\nHoldPitch=false\n").empty() && !off.mouse_hold_pitch, "MenuClicks and HoldPitch options");
            Fixture g(off);
            g.input.set_look_active(false);
            (void)g.input.window_message(nullptr, WM_LBUTTONDOWN, 0, 0, result);
            require(g.sample().buttons == 0u && g.input.capture_pending(), "menu clicks can be switched off");
        }
        // An ini the game wrote before the file had a version: its input sections get the current defaults once,
        // the other sections and a backup of the old file are kept. A hand-written file is left alone.
        {
            wchar_t temp[MAX_PATH]{};
            GetTempPathW(MAX_PATH, temp);
            const auto file = std::filesystem::path(temp) / ("chili-input-old-" + std::to_string(GetCurrentProcessId()) + ".ini");
            auto backup = file; backup += ".bak";
            std::filesystem::remove(file); std::filesystem::remove(backup);
            const std::string old_text =
                "; Chili Con Carnage PC input. Delete this file to get the defaults back.\n; Key names: A-Z\n\n[Input]\nScheme=psp\nBackgroundInput=false\n\n"
                "[Input.Keyboard]\nSquare=LCtrl\nL=LShift,Q\n\n[Input.Mouse]\nCapture=click\nSensitivityX=0.08\n\n[Input.Gamepad]\nA=Cross\n\n"
                "[Mods]\n; Mods live in the \"mods\" folder beside this file.\nEnabled=first_mod\n\n[Rendering]\nMSAA=4\n";
            const auto read_text = [](const std::filesystem::path &path) { std::ifstream in(path, std::ios::binary); std::ostringstream text; text << in.rdbuf(); return text.str(); };
            { std::ofstream out(file, std::ios::binary | std::ios::trunc); out << old_text; }
            std::vector<std::string> problems, notes;
            // A deterministic run reads the file as it is.
            auto config = chili::PcInputConfig::load(file, problems, false, &notes);
            require(config.scheme == chili::InputScheme::Psp && notes.empty() && read_text(file) == old_text && !std::filesystem::exists(backup),
                "a run that must not write leaves an old file alone");
            config = chili::PcInputConfig::load(file, problems, true, &notes);
            const auto fresh = chili::PcInputConfig::defaults();
            require(problems.empty() && config.scheme == chili::InputScheme::DualAnalog && config.version == chili::input_ini_version &&
                    config.keys.size() == fresh.keys.size() && !binds(config, "LCtrl", PadTarget::Square), "an older build's file gets the current input defaults");
            require(notes.size() == 1u && notes[0].find("older build (scheme psp)") != std::string::npos && notes[0].find(".bak") != std::string::npos, "and says so");
            require(read_text(backup) == old_text, "the old file is kept as .bak");
            const auto rewritten = read_text(file);
            require(rewritten.find(chili::default_input_ini()) == 0u &&
                    rewritten.find("\n\n[Mods]\n; Mods live in the \"mods\" folder beside this file.\nEnabled=first_mod\n\n[Rendering]\nMSAA=4\n") != std::string::npos &&
                    rewritten.find("Square=LCtrl") == std::string::npos && rewritten.find("Scheme=psp") == std::string::npos, "the other sections stay, the old input sections go");
            notes.clear();
            config = chili::PcInputConfig::load(file, problems, true, &notes);
            require(notes.empty() && read_text(file) == rewritten && config.scheme == chili::InputScheme::DualAnalog, "the next start changes nothing");
            // The player picks psp in the new file: it stays.
            auto chosen = rewritten;
            chosen.replace(chosen.find("Scheme=dual_analog"), 18u, "Scheme=psp");
            { std::ofstream out(file, std::ios::binary | std::ios::trunc); out << chosen; }
            config = chili::PcInputConfig::load(file, problems, true, &notes);
            require(notes.empty() && config.scheme == chili::InputScheme::Psp && read_text(file) == chosen, "a scheme chosen in a current file is kept");
            // A hand-written file without the game's first line is nobody's old default.
            { std::ofstream out(file, std::ios::binary | std::ios::trunc); out << "[Input]\nScheme=psp\n\n[Mods]\n"; }
            config = chili::PcInputConfig::load(file, problems, true, &notes);
            require(notes.empty() && config.scheme == chili::InputScheme::Psp && read_text(file) == "[Input]\nScheme=psp\n\n[Mods]\n", "a hand-written file is not touched");
            std::filesystem::remove(file); std::filesystem::remove(backup);
        }
        // A file the game wrote with [Input] Version 2 (the builds before Total Overdose PC): every binding still the
        // old default with dual_analog -> Total Overdose PC's; psp untouched -> PSP original; any binding of the
        // player's own -> every binding kept, the style it was made with. Everything else stays.
        {
            wchar_t temp[MAX_PATH]{};
            GetTempPathW(MAX_PATH, temp);
            const auto file = std::filesystem::path(temp) / ("chili-input-v2-" + std::to_string(GetCurrentProcessId()) + ".ini");
            auto backup = file; backup += ".bak";
            const auto read_text = [](const std::filesystem::path &path) { std::ifstream in(path, std::ios::binary); std::ostringstream text; text << in.rdbuf(); return text.str(); };
            const auto write_text = [](const std::filesystem::path &path, const std::string &text) { std::ofstream out(path, std::ios::binary | std::ios::trunc); out << text; };
            // The launcher's sample: a file as the builds before the control styles wrote it (Version 2, Modern's bindings).
            const auto profile = std::filesystem::path(__FILE__).parent_path().parent_path();
            const std::string v2 = read_text(profile / "launcher" / "test" / "sample_ChiliNative.ini");
            require(v2.find("\nVersion=2\r\n") != std::string::npos || v2.find("\nVersion=2\n") != std::string::npos, "the launcher's sample is a Version 2 file");
            const std::string nl = v2.find("\r\n") != std::string::npos ? "\r\n" : "\n";
            // The sample as the game brings it up to date: the bytes of sample_ChiliNative.migrated.ini beside it, which
            // the launcher's tests hold its own update to as well (launcher/src/Program.cs, IniTests).
            {
                const auto expected_file = profile / "launcher" / "test" / "sample_ChiliNative.migrated.ini";
                const std::string made = chili::PcInputConfig::brought_up_to_date(v2, 2).text;
                if (!std::filesystem::exists(expected_file)) {
                    const auto written = std::filesystem::path(temp) / "sample_ChiliNative.migrated.ini";
                    std::ofstream(written, std::ios::binary | std::ios::trunc) << made;
                    std::cerr << "no " << expected_file.string() << ": the game's update of the sample is in " << written.string() << '\n';
                }
                require(read_text(expected_file) == made, "the launcher's sample brought up to date: the bytes of sample_ChiliNative.migrated.ini");
            }
            const auto replaced = [&](std::string text, const std::string &from, const std::string &to) {
                const auto at = text.find(from);
                require(at != std::string::npos, "the sample has the line to change");
                if (at != std::string::npos) text.replace(at, from.size(), to);
                return text;
            };
            // A player who changed one setting that is no binding: the file goes over to Total Overdose PC.
            const std::string untouched = replaced(v2, "SensitivityX=0.08", "SensitivityX=0.13");
            write_text(file, untouched);
            write_text(backup, "an older backup\n");
            std::vector<std::string> problems, notes;
            auto config = chili::PcInputConfig::load(file, problems, false, &notes);
            require(notes.empty() && read_text(file) == untouched && config.layout == chili::InputLayout::TodPc, "a run that must not write leaves a Version 2 file alone");
            config = chili::PcInputConfig::load(file, problems, true, &notes);
            const auto tod = chili::PcInputConfig::defaults(chili::InputLayout::TodPc);
            const std::string after = read_text(file);
            require(problems.empty() && config.layout == chili::InputLayout::TodPc && config.version == chili::input_ini_version && config.keys.size() == 31u &&
                    binds(config, "LShift", PadTarget::Cross) && binds(config, "E", PadTarget::Circle) && binds(config, "Tab", PadTarget::DpadDown) &&
                    config.pad_buttons == tod.pad_buttons && config.mouse_buttons == tod.mouse_buttons && config.mouse_degrees_per_count == 0.13f,
                "every binding an older build's default: Total Overdose PC's now, the player's mouse speed kept");
            require(notes.size() == 1u && notes[0].find("Total Overdose PC") != std::string::npos && notes[0].find("Layout=tod_pc") != std::string::npos && notes[0].find(".bak") != std::string::npos,
                "and the log line says so");
            require(read_text(backup) == untouched, "the file as it was is the .bak, over the older backup");
            require(after.find("Cross=Space,F,Enter,LShift" + nl) != std::string::npos && after.find("Circle=E,Backspace" + nl) != std::string::npos &&
                    after.find("DpadDown=Tab,Down,PageDown" + nl) != std::string::npos && after.find("Select=Grave" + nl) != std::string::npos &&
                    after.find("Version=3" + nl) != std::string::npos && after.find("Layout=tod_pc" + nl) != std::string::npos &&
                    after.find("SensitivityX=0.13" + nl) != std::string::npos && after.find("Space jumps and dives, E uses") == std::string::npos &&
                    after.find("; Jump, dive, use, enter a vehicle, confirm: the game has them all on Cross." + nl) != std::string::npos &&
                    after.find("Delete this file after changing the scheme") == std::string::npos && after.find("Version=2") == std::string::npos,
                "the binding lines, Layout, Version and three comment lines changed in place");
            // Line by line: only the lines the update names are other ones, and one line more (Layout).
            {
                const auto lines = [](const std::string &text) { std::vector<std::string> out; std::istringstream in(text); for (std::string line; std::getline(in, line);) out.push_back(line); return out; };
                const auto a = lines(untouched), b = lines(after);
                std::size_t differ = 0u;
                for (std::size_t i = 0, j = 0; i < a.size() && j < b.size(); ++i, ++j) {
                    if (b[j].rfind("Layout=", 0u) == 0u && a[i].rfind("Layout=", 0u) != 0u) { --i; continue; }
                    if (a[i] != b[j]) ++differ;
                }
                require(b.size() == a.size() + 1u && differ == 10u + 5u + 1u, "ten binding lines, five comment lines and Version differ; nothing else");
            }
            notes.clear();
            config = chili::PcInputConfig::load(file, problems, true, &notes);
            require(notes.empty() && read_text(file) == after && config.layout == chili::InputLayout::TodPc, "the next start changes nothing");
            // The same text as the launcher makes: the launcher's sample brought up to date by the game, line for line what
            // launcher/src/Model.cs InputMigration does (the C# side is checked by the launcher's own tests).
            const auto by_text = chili::PcInputConfig::brought_up_to_date(untouched, 2);
            require(by_text.text == after, "the update is a function of the text alone");

            // One binding of the player's own: every binding kept, the style it was made with.
            const std::string changed = replaced(v2, "Square=G", "Square=G,H");
            write_text(file, changed);
            notes.clear();
            config = chili::PcInputConfig::load(file, problems, true, &notes);
            const auto modern = chili::PcInputConfig::defaults(chili::InputLayout::Modern);
            require(problems.empty() && config.layout == chili::InputLayout::Modern && config.keys.size() == modern.keys.size() + 1u && binds(config, "H", PadTarget::Square) &&
                    binds(config, "E", PadTarget::Cross) && binds(config, "F", PadTarget::Circle) && !binds(config, "LShift", PadTarget::Cross), "a binding of the player's own: every binding kept, Modern");
            require(notes.size() == 1u && notes[0].find("1 binding of the player's own") != std::string::npos && notes[0].find("Layout=modern") != std::string::npos, "and the log line says so");
            {
                const std::string text = read_text(file);
                require(text.find("Layout=modern") != std::string::npos && text.find("Version=3") != std::string::npos && text.find("Cross=Space,E,Enter") != std::string::npos &&
                        text.find("; Space jumps and dives, E uses: the game has both on Cross.") != std::string::npos && text.find("Delete this file after changing the scheme") == std::string::npos,
                    "its key lines and their comments stay; the three lines about the older build change");
            }
            // A table of a PlayStation pad's own counts as the player's.
            write_text(file, v2 + "[Input.Gamepad.PlayStation]" + nl + "X=Circle" + nl);
            config = chili::PcInputConfig::load(file, problems, true, &notes);
            require(config.layout == chili::InputLayout::Modern && binds(config, "E", PadTarget::Cross), "a PlayStation pad's own table: every binding kept");
            // The PSP layout as that build had it: it stays, and the file names it.
            std::string psp_file = replaced(v2, "Scheme=dual_analog", "Scheme=psp");
            for (const char *look : {"LookLeft=J", "LookRight=L", "LookUp=I", "LookDown=K"}) psp_file = replaced(psp_file, look, std::string(look).substr(0u, std::string(look).find('=') + 1u));
            psp_file = replaced(replaced(replaced(replaced(psp_file, "X=Triangle", "X=Square"), "Y=Cross", "Y=Triangle"), "LB=DpadLeft", "LB=L"), "RB=DpadRight", "RB=R");
            psp_file = replaced(psp_file, "LS=L", "LS=None");
            write_text(file, psp_file);
            notes.clear();
            config = chili::PcInputConfig::load(file, problems, true, &notes);
            require(config.layout == chili::InputLayout::Psp && config.scheme == chili::InputScheme::Psp && config.keys.size() == 23u && notes.size() == 1u &&
                    notes[0].find("stays") != std::string::npos && read_text(file).find("Layout=psp") != std::string::npos && read_text(file).find("Cross=Space,E,Enter") != std::string::npos,
                "PSP original as an older build wrote it: kept, and named");
            // A hand-written Version 2 file (no first line of the game's) is nobody's old default.
            write_text(file, "[Input]\nVersion=2\nScheme=dual_analog\n[Input.Keyboard]\nCross=Space,E,Enter\n");
            notes.clear();
            config = chili::PcInputConfig::load(file, problems, true, &notes);
            require(notes.empty() && read_text(file) == "[Input]\nVersion=2\nScheme=dual_analog\n[Input.Keyboard]\nCross=Space,E,Enter\n" && config.layout == chili::InputLayout::TodPc &&
                    binds(config, "E", PadTarget::Cross) && binds(config, "E", PadTarget::Circle), "a hand-written file is not touched");
            std::filesystem::remove(file); std::filesystem::remove(backup);
        }
        // A switch of control style moves the bindings that still hold the old style's default, and only those.
        {
            auto doc = chili::IniDocument::parse(chili::default_input_ini(chili::InputLayout::TodPc));
            doc.set("Input.Keyboard", "Square", "G,H");             // the player's own
            doc.set("Input.Keyboard", "Cross", "f, space, LSHIFT, Return");   // Total Overdose PC's in another spelling
            const auto to_modern = chili::PcInputConfig::layout_binding_changes(doc, chili::InputLayout::TodPc, chili::InputLayout::Modern);
            const auto names = [](const std::vector<chili::IniChange> &changes) { std::string out; for (const auto &c : changes) out += c.key + "=" + c.value + ";"; return out; };
            require(names(to_modern) == "Cross=Space,E,Enter;Triangle=R;Circle=F,Backspace;L=Q;DpadLeft=Left,1;DpadRight=Right,2;DpadDown=Down,Z;DpadUp=Up;Start=Escape;Select=Tab;",
                "Total Overdose PC to Modern: the ten keys that differ, not the player's Square, not the mouse or the pad");
            const auto to_psp = chili::PcInputConfig::layout_binding_changes(doc, chili::InputLayout::TodPc, chili::InputLayout::Psp);
            require(to_psp.size() == 10u + 4u + 5u && names(to_psp).find("LookLeft=;") != std::string::npos && names(to_psp).find("X=Square;") != std::string::npos &&
                    names(to_psp).find("LS=None;") != std::string::npos, "to PSP original: the look keys go, the PSP's pad table");
            require(chili::PcInputConfig::layout_binding_changes(doc, chili::InputLayout::TodPc, chili::InputLayout::TodPc).empty(), "the same style: nothing");
            for (const auto &change : to_modern) doc.set(change.section, change.key, change.value);
            doc.set("Input", "Layout", "modern");
            chili::PcInputConfig switched;
            require(switched.apply(doc.bytes()).empty() && switched.layout == chili::InputLayout::Modern && switched.keys.size() == 28u && binds(switched, "H", PadTarget::Square),
                "the file then holds Modern's keys and the player's own");
            const auto back = chili::PcInputConfig::layout_binding_changes(doc, chili::InputLayout::Modern, chili::InputLayout::TodPc);
            require(back.size() == 10u, "and back again the same ten");
        }
        // The controls the dual-analog layer answers for the game.
        {
            require(Controls::from_name("run_left") == Controls::RunLeft && Controls::from_name("run_right") == Controls::RunRight &&
                    Controls::from_name("look_up") == Controls::LookUp && Controls::from_name("look_down") == Controls::LookDown &&
                    Controls::from_name("look_left") == Controls::LookLeft && Controls::from_name("Look_Right") == Controls::LookRight, "host control names");
            require(Controls::from_name("run") == Controls::Count && Controls::from_name("look_trigger") == Controls::Count &&
                    Controls::from_name("") == Controls::Count && Controls::from_name("run_backwards") == Controls::Count, "the game's own controls are not taken");
            require(Controls::is_id(Controls::first_id) && Controls::is_id(Controls::first_id + Controls::LookRight) &&
                    !Controls::is_id(Controls::first_id + Controls::Count) && !Controls::is_id(13u) && !Controls::is_id(0xFFFFFFFFu), "host control ids");

            Controls controls;
            require(!controls.look_active(), "no look before the game asks");
            controls.sampled({0u, 0u, 128u, 255u, 64u});   // left stick full left; right stick full right, half up
            auto a = controls.query(Controls::RunLeft);
            require(a.pressure == 1.0f && a.real_pressure == 1.0f && a.pressed && !a.released, "full left strafes at full pressure, pressed edge");
            a = controls.query(Controls::RunLeft);
            require(a.pressure == 1.0f && !a.pressed, "pressed is reported once");
            require(controls.query(Controls::RunRight).pressure == 0.0f, "the opposite direction is up");
            // The look controls are a rate: the second stick's full travel is look_gain times the PSP stick's.
            a = controls.query(Controls::LookRight);
            require(a.pressure > 0.98f * Controls::look_gain && a.pressure < Controls::look_gain && a.real_pressure > 0.99f * Controls::look_gain, "right stick right turns right");
            require(controls.query(Controls::LookLeft).pressure == 0.0f, "not left");
            a = controls.query(Controls::LookUp);
            require(close_to(a.real_pressure, 0.5 * Controls::look_gain, 0.001) && close_to(a.pressure, 0.2 / 0.7 * Controls::look_gain, 0.001), "half up: the engine's 30% dead zone");
            require(controls.query(Controls::LookDown).pressure == 0.0f, "not down");
            require(controls.axis_x() > 0.99f && std::fabs(controls.axis_y() + 0.5f) < 0.001f, "second axis for the pad object");
            controls.sampled({0u, 128u, 128u, 128u, 160u});   // right stick a quarter down: inside the dead zone
            require(!controls.look_active(), "one sample of look queries is not play: the game asks once or twice while it starts");
            a = controls.query(Controls::LookDown);
            require(a.pressure == 0.0f && a.real_pressure > 0.0f && !a.pressed, "inside the dead zone: no pressure, real pressure kept");
            a = controls.query(Controls::RunLeft);
            require(a.pressure == 0.0f && a.released, "released edge");
            require(!controls.query(Controls::RunLeft).released, "released is reported once");
            // Input switched off by the game: everything reads up.
            controls.sampled({0u, 0u, 128u, 255u, 128u});
            a = controls.query(Controls::RunLeft, false);
            require(a.pressure == 0.0f && a.real_pressure == 0.0f && !a.pressed, "disabled input reads nothing");
            // Asked in four samples: play.
            (void)controls.query(Controls::LookLeft); controls.sampled({});
            require(!controls.look_active(), "three samples are not enough");
            (void)controls.query(Controls::LookLeft); controls.sampled({});
            require(controls.look_active(), "look is active once the game has asked in four samples");
            // Eight samples without a look query: menus.
            for (int i = 0; i < 7; ++i) controls.sampled({});
            require(controls.look_active(), "seven idle samples are still play");
            controls.sampled({});
            require(!controls.look_active(), "eight idle samples are not");
            (void)controls.query(Controls::RunLeft); controls.sampled({});
            require(!controls.look_active(), "a move query is not a look query");
            for (int i = 0; i < 3; ++i) { (void)controls.query(Controls::LookLeft); controls.sampled({}); }
            require(!controls.look_active(), "after a pause the count starts again");
            (void)controls.query(Controls::LookLeft); controls.sampled({});
            require(controls.look_active(), "and four samples of look queries are play again");
            // One sample without a query in the middle of play changes nothing.
            controls.sampled({});
            (void)controls.query(Controls::LookUp); controls.sampled({});
            require(controls.look_active(), "a gap of one sample is still play");
        }
        // The rate a second-stick byte stands for is what the player script makes of the pressure the game is told.
        {
            require(Controls::byte_for_turn(0.0f) == 128u && Controls::byte_for_pitch(0.0f) == 128u && Controls::turn_of(128u) == 0.0f &&
                    Controls::pitch_of(128u) == 0.0f, "centre is no rate");
            require(Controls::turn_of(166u) == 0.0f && Controls::turn_of(167u) > 0.0f && Controls::turn_of(90u) == 0.0f && Controls::turn_of(89u) < 0.0f,
                "the first 30% of travel is the engine's dead zone");
            bool consistent = true, rising = true;
            for (unsigned byte = 1u; byte <= 255u; ++byte) {
                Controls controls;
                controls.sampled({0u, 128u, 128u, static_cast<std::uint8_t>(byte), static_cast<std::uint8_t>(byte)});
                const float turn = controls.query(Controls::LookRight).pressure - controls.query(Controls::LookLeft).pressure;
                const float pitch = controls.query(Controls::LookDown).pressure - controls.query(Controls::LookUp).pressure;
                consistent = consistent && close_to(std::copysign(turn_rate * turn * turn, turn), Controls::turn_of(static_cast<std::uint8_t>(byte)), 0.01) &&
                             close_to(57.29578f * pitch * pitch * pitch, Controls::pitch_of(static_cast<std::uint8_t>(byte)), 0.01);
                if (byte > 1u) rising = rising && Controls::turn_of(static_cast<std::uint8_t>(byte)) >= Controls::turn_of(static_cast<std::uint8_t>(byte - 1u)) &&
                                        Controls::pitch_of(static_cast<std::uint8_t>(byte)) >= Controls::pitch_of(static_cast<std::uint8_t>(byte - 1u));
            }
            require(consistent, "byte rates match the script's response to the answered pressure");
            require(rising, "rates rise with the byte");
            // Every rate in range comes back as the nearest byte: within half a step, both signs alike.
            bool close = true, mirrored = true;
            for (float rate = 0.5f; rate < 1400.0f; rate *= 1.07f) {
                const auto byte = Controls::byte_for_turn(rate);
                const float step = byte < 255u ? Controls::turn_of(static_cast<std::uint8_t>(byte + 1u)) - Controls::turn_of(byte) : 40.0f;
                close = close && std::fabs(Controls::turn_of(byte) - rate) <= std::max(step, 0.2f);
                mirrored = mirrored && Controls::byte_for_turn(-rate) == static_cast<std::uint8_t>(256u - byte) &&
                           Controls::byte_for_pitch(-rate) == static_cast<std::uint8_t>(256u - Controls::byte_for_pitch(rate));
                const auto pitch_byte = Controls::byte_for_pitch(rate);
                const float pitch_step = pitch_byte < 255u ? Controls::pitch_of(static_cast<std::uint8_t>(pitch_byte + 1u)) - Controls::pitch_of(pitch_byte) : 60.0f;
                close = close && std::fabs(Controls::pitch_of(pitch_byte) - rate) <= std::max(pitch_step, 0.2f);
            }
            require(close, "a rate and its byte agree to within a step");
            require(mirrored, "left and right, up and down are mirror images");
            require(Controls::byte_for_turn(1.0e6f) == 255u && Controls::byte_for_turn(-1.0e6f) == 1u && Controls::turn_of(255u) > 1400.0f &&
                    Controls::pitch_of(255u) > 1400.0f, "beyond the range saturates");
            require(close_to(Controls::turn_of(Controls::byte_for_turn(turn_rate)), turn_rate, 5.0) && close_to(Controls::pitch_of(Controls::byte_for_pitch(57.3f)), 57.3, 2.5),
                "the PSP stick's own full rates sit a third of the way along");
        }
        // Pad read to present: a picture is timed from the pad read of its own frame, whichever pad read came
        // last when it is presented; one picture per pad read; nothing before the first pad read.
        {
            const auto worst = [](const chili::PresentLatency &timer) {
                const auto line = timer.report("test");
                return std::atof(line.c_str() + line.find("max=") + 4u);
            };
            chili::PresentLatency timer;
            timer.presented(true, timer.stamp());
            require(timer.count() == 0u, "no pad read, no time");
            timer.sampled();
            const auto first = timer.stamp();
            Sleep(30);
            timer.sampled();
            const auto second = timer.stamp();
            require(first == 1u && second == 2u, "pad reads are numbered");
            timer.presented(true, first);
            require(timer.count() == 1u && worst(timer) >= 29.0, "a late picture is timed from its own pad read, not the last one");
            timer.presented(true, first);
            require(timer.count() == 1u, "one picture per pad read");
            timer.presented(true, second);
            require(timer.count() == 2u && worst(timer) < 200.0, "the next picture from the next pad read");
            // Without the early present the game's copy shows the scene of the frame before the copy's.
            timer.sampled();
            timer.presented(false, timer.stamp());
            require(timer.count() == 2u, "the frame before was already answered");
            timer.sampled();
            timer.presented(false, timer.stamp());
            require(timer.count() == 3u, "the copy shows the frame before its own");
            // A pad read too long ago to be remembered is not timed.
            for (unsigned i = 0; i < 80u; ++i) timer.sampled();
            timer.presented(true, second);
            require(timer.count() == 3u, "a forgotten pad read");
        }
        // Pacer: guest time ahead of the host waits; a slow host is not made to catch up.
        {
            chili::RealTimePacer pacer;
            const auto begin = std::chrono::steady_clock::now();
            pacer.idle_until(1000000u);
            pacer.idle_until(1030000u);
            const auto first = std::chrono::steady_clock::now() - begin;
            require(first >= std::chrono::milliseconds(29) && first < std::chrono::milliseconds(200), "30 ms of guest time takes 30 ms");
            Sleep(120);
            auto late = std::chrono::steady_clock::now();
            pacer.idle_until(1040000u);
            pacer.idle_until(1050000u);
            require(std::chrono::steady_clock::now() - late < std::chrono::milliseconds(8), "a host that is behind does not wait");
            late = std::chrono::steady_clock::now();
            pacer.idle_until(1100000u);
            const auto second = std::chrono::steady_clock::now() - late;
            require(second >= std::chrono::milliseconds(8) && second < std::chrono::milliseconds(60), "catching up is capped at the slack");
            require(pacer.slept() >= std::chrono::milliseconds(38), "slept time is accounted");
        }
        // ---- Gamepads (wave 6) ----
        // The pad sample: an XInput state keeps its buttons, triggers and sticks, and the bindings give exactly
        // what they gave when they read the XInput state themselves, for the default tables of both schemes.
        {
            using chili::PadSample;
            XINPUT_STATE state{};
            state.dwPacketNumber = 77u;
            state.Gamepad.wButtons = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_RIGHT_THUMB;
            state.Gamepad.bLeftTrigger = 255; state.Gamepad.bRightTrigger = 30;
            state.Gamepad.sThumbLX = 32767; state.Gamepad.sThumbLY = 32767; state.Gamepad.sThumbRX = -32768; state.Gamepad.sThumbRY = -16384;
            const auto sample = chili::pad_sample_from_xinput(state);
            require(sample.kind == chili::PadKind::Xbox && sample.packet == 77u && sample.vid == 0u && sample.pid == 0u, "an XInput pad is an Xbox-style pad");
            require(sample.buttons == (PadSample::bit(PadSample::A) | PadSample::bit(PadSample::DpadLeft) | PadSample::bit(PadSample::RS)), "buttons by position");
            require(sample.lt == 1.0f && close_to(sample.rt, 30 / 255.0, 1e-6) && sample.lx == 1.0f && sample.ly == -1.0f && sample.rx == -1.0f && close_to(sample.ry, 0.5, 1e-4),
                "triggers 0 to 1, sticks -1 to 1 with down positive");
            require(sample.with_triggers(0.12f) == (sample.buttons | PadSample::bit(PadSample::LT)) && sample.with_triggers(0.1f) == (sample.buttons | PadSample::trigger_bits),
                "a trigger is a button beyond the threshold");
            require(chili::PcInputConfig::pad_button_from_name("lt") == std::size_t{PadSample::LT} && chili::PcInputConfig::pad_button_from_name("Back") == std::size_t{PadSample::Back} &&
                    !chili::PcInputConfig::pad_button_from_name("Square") && std::string_view(chili::PcInputConfig::pad_button_name(PadSample::RT)) == "RT" &&
                    std::string_view(chili::PcInputConfig::pad_button_name(PadSample::DpadUp)) == "DpadUp", "the sixteen button names");
            require(std::string_view(chili::pad_kind_name(chili::PadKind::PlayStation)) == "playstation" && std::string_view(chili::pad_kind_name(chili::PadKind::None)) == "none", "kind names");

            for (const auto scheme : {chili::InputScheme::Psp, chili::InputScheme::DualAnalog}) {
                Fixture f(chili::PcInputConfig::defaults(scheme));
                XINPUT_STATE pad{};
                f.pad = pad;
                f.input.scan_pads();
                bool same = true, dialogs = true;
                std::uint32_t seed = 20261004u;
                const auto next = [&] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
                static constexpr BYTE pulls[]{0, 29, 30, 31, 32, 128, 255};
                for (unsigned round = 0; round < 3000u; ++round) {
                    pad = {};
                    if (round < 14u) pad.Gamepad.wButtons = static_cast<WORD>(chili::PcInputConfig::pad_button_names[round].second);
                    else if (round < 21u) pad.Gamepad.bLeftTrigger = pulls[round - 14u];
                    else if (round < 28u) pad.Gamepad.bRightTrigger = pulls[round - 21u];
                    else {
                        pad.Gamepad.wButtons = static_cast<WORD>(next() & 0xF3FFu);
                        pad.Gamepad.bLeftTrigger = (round & 1u) ? pulls[next() % 7u] : static_cast<BYTE>(next());
                        pad.Gamepad.bRightTrigger = (round & 2u) ? pulls[next() % 7u] : static_cast<BYTE>(next());
                    }
                    f.pad = pad;
                    const auto before = buttons_before_refit(f.input.config(), pad);
                    same = same && f.sample().buttons == before;
                    dialogs = dialogs && f.input.peek_pad().buttons == before;
                }
                require(same, "the pad's buttons and triggers give what they gave before the refit");
                require(dialogs, "and so does the dialogs' read of the pad");
            }
        }
        // Two pads: every slot that has one is read, one of them is the pad in use and never both at once; a pad
        // takes over when it is touched while the pad in use has been at rest for 250 ms.
        {
            Fixture f(chili::PcInputConfig::defaults());
            XINPUT_STATE first{}, second{};
            f.slots[0] = first; f.slots[2] = second;
            f.input.scan_pads();
            require(f.pad_queries == 4u, "the search asks all four slots");
            auto s = f.sample();
            require(f.pad_queries == 6u && s.buttons == 0u, "both pads are read at a sample");
            require(f.input.pad_text() == "xbox slot 0" && f.logged("gamepad: Xbox-style pad, XInput slot 0") && f.logged("gamepad: Xbox-style pad, XInput slot 2"),
                "untouched, the first pad there is is the pad in use; both are logged as they arrive");
            second.Gamepad.wButtons = XINPUT_GAMEPAD_B; f.slots[2] = second;
            require(f.sample().buttons == circle && f.input.pad_text() == "xbox slot 2", "the later slot wins when it is touched");
            first.Gamepad.wButtons = XINPUT_GAMEPAD_A; f.slots[0] = first;
            require(f.sample().buttons == circle && f.sample().buttons == circle, "a press on the other pad is not added while this one is held");
            second.Gamepad.wButtons = 0; f.slots[2] = second;
            // Seven samples at 30 Hz are 233 ms, the eighth comes at 267.
            bool waited = true;
            for (int i = 0; i < 7; ++i) waited = waited && f.sample().buttons == 0u && f.input.pad_text() == "xbox slot 2";
            require(waited, "the pad in use keeps the input for 250 ms after it was let go");
            require(f.sample().buttons == cross && f.input.pad_text() == "xbox slot 0", "then the pad that is held takes over");
            // A stick is a touch once it is pushed well out (past the dead zone and 0.45), a trigger past its threshold.
            first.Gamepad.wButtons = 0; f.slots[0] = first;
            f.now += 1.0; (void)f.sample();
            second.Gamepad.sThumbLX = 6000; f.slots[2] = second;
            (void)f.sample();
            require(f.input.pad_text() == "xbox slot 0", "a stick inside its dead zone is not a touch");
            second.Gamepad.sThumbLX = 20000; f.slots[2] = second;
            s = f.sample();
            require(f.input.pad_text() == "xbox slot 2" && s.x > 128u, "a stick pushed well out is");
            second.Gamepad.sThumbLX = 0; f.slots[2] = second;
            f.now += 1.0; (void)f.sample();
            first.Gamepad.bRightTrigger = 25; f.slots[0] = first;
            (void)f.sample();
            require(f.input.pad_text() == "xbox slot 2", "a trigger short of its threshold is not a touch");
            first.Gamepad.bRightTrigger = 60; f.slots[0] = first;
            require(f.sample().buttons == square && f.input.pad_text() == "xbox slot 0", "a trigger past it is");
            require(f.input.report().pad_changes == 5u && f.input.report().pads_xbox == 2u, "the changes of the pad in use and the pads that arrived are counted");
            // The pad in use is pulled out: the other one is the pad in use; the last one out leaves none.
            f.slots[0].reset();
            s = f.sample();
            require(f.logged("gamepad: XInput slot 0 removed") && !f.logged("gamepad: none") && f.input.pad_text() == "xbox slot 2" && s.buttons == 0u,
                "a pad that is pulled out is given up and the other is the pad in use");
            f.slots[2].reset();
            (void)f.sample();
            require(f.log.back() == "gamepad: none" && f.input.pad_text() == "none" && f.input.active_pad() == nullptr, "no pad left");

            // Index names a slot: only that pad is read.
            auto pinned = chili::PcInputConfig::defaults();
            require(pinned.apply("[Input.Gamepad]\nIndex=2\n").empty(), "a pinned slot");
            Fixture g(pinned);
            first = {}; first.Gamepad.wButtons = XINPUT_GAMEPAD_A; second = {}; second.Gamepad.wButtons = XINPUT_GAMEPAD_B;
            g.slots[0] = first; g.slots[2] = second;
            g.input.scan_pads();
            require(g.pad_queries == 1u && g.sample().buttons == circle && g.input.pad_text() == "xbox slot 2", "Index pins the slot: another pad, though held, is never read");
            require(!g.input.hid_arrived(5u, playstation_pad()), "and no PlayStation pad is taken either");

            // No pad at all: the log says so once, and says when one arrives.
            Fixture quiet(chili::PcInputConfig::defaults());
            (void)quiet.sample();
            require(quiet.log.empty(), "nothing is said before the slots were looked at");
            quiet.input.scan_pads();
            (void)quiet.sample(); (void)quiet.sample();
            require(quiet.log.size() == 1u && quiet.log[0] == "gamepad: none", "the log says once that no pad was found");
            quiet.pad = XINPUT_STATE{};
            quiet.input.scan_pads();
            (void)quiet.sample();
            require(quiet.log.size() == 2u && quiet.log[1] == "gamepad: Xbox-style pad, XInput slot 1", "and when one arrives");
        }
        // "The pad is being used" is not the game's dead zone alone. A stick that rests a little off its centre (a
        // worn pad lying on the desk) is past a dead zone of 0.24 for good: it must not keep that pad the pad in use
        // when another one is picked up. A stick counts when it is pushed well out (past the dead zone and past 0.45)
        // or when it is past the dead zone and has moved by more than a tenth. The game's own reading of the stick
        // keeps the configured dead zone.
        {
            Fixture f(chili::PcInputConfig::defaults());
            XINPUT_STATE worn{}, other{};
            worn.Gamepad.sThumbLX = 9830;   // 0.30
            f.slots[0] = worn; f.slots[2] = other;
            f.input.scan_pads();
            auto s = f.sample();
            require(f.input.pad_text() == "xbox slot 0" && s.x > 128u, "a stick at 0.30 still moves the player: the game's dead zone is the configured one");
            for (int i = 0; i < 12; ++i) (void)f.sample();
            other.Gamepad.wButtons = XINPUT_GAMEPAD_A; f.slots[2] = other;
            s = f.sample();
            require(s.buttons == cross && f.input.pad_text() == "xbox slot 2", "a stick that rests off centre does not block the pad that is picked up");
            other.Gamepad.wButtons = 0; f.slots[2] = other;
            for (int i = 0; i < 12; ++i) (void)f.sample();
            require(f.input.pad_text() == "xbox slot 2", "nor does it take the input back while it rests");
            worn.Gamepad.sThumbLX = 14000;   // 0.43: past the dead zone, short of 0.45, moved by 0.13
            f.slots[0] = worn;
            (void)f.sample();
            require(f.input.pad_text() == "xbox slot 0", "a stick past the dead zone that moved by more than a tenth is a touch");
            other.Gamepad.wButtons = XINPUT_GAMEPAD_B; f.slots[2] = other;
            bool kept = true;
            for (int i = 0; i < 7; ++i) kept = kept && f.sample().buttons == 0u && f.input.pad_text() == "xbox slot 0";
            require(kept, "that touch keeps the pad for 250 ms");
            require(f.sample().buttons == circle && f.input.pad_text() == "xbox slot 2", "held where it is, short of 0.45, it is not a touch any more: the other pad takes over");
            other.Gamepad.wButtons = 0; f.slots[2] = other;
            worn.Gamepad.sThumbLX = 16400;   // 0.50: moved by 0.07 only, but past 0.45
            f.slots[0] = worn;
            for (int i = 0; i < 9; ++i) (void)f.sample();
            require(f.input.pad_text() == "xbox slot 0", "a stick held past 0.45 is a touch without moving");
            for (int i = 0; i < 30; ++i) (void)f.sample();
            other.Gamepad.wButtons = XINPUT_GAMEPAD_B; f.slots[2] = other;
            for (int i = 0; i < 12; ++i) (void)f.sample();
            require(f.input.pad_text() == "xbox slot 0", "and stays one as long as it is held there");

            // A larger dead zone is the threshold when it is the larger of the two.
            auto wide = chili::PcInputConfig::defaults();
            require(wide.apply("[Input.Gamepad]\nDeadzoneLeft=0.6\n").empty(), "a dead zone of 0.6");
            Fixture g(wide);
            XINPUT_STATE held{}, idle{};
            g.slots[0] = idle; g.slots[1] = held;
            g.input.scan_pads();
            (void)g.sample();
            held.Gamepad.sThumbLX = 18000;   // 0.55: past 0.45, inside this dead zone
            g.slots[1] = held;
            for (int i = 0; i < 12; ++i) (void)g.sample();
            require(g.input.pad_text() == "xbox slot 0", "with a dead zone of 0.6 a stick at 0.55 is not a touch");
            held.Gamepad.sThumbLX = 22000;   // 0.67
            g.slots[1] = held;
            (void)g.sample();
            require(g.input.pad_text() == "xbox slot 1", "and one at 0.67 is");
        }
        // One pad that shows up twice (a helper program presents a PlayStation pad as an Xbox-style pad too), the
        // second appearance in step or one sample behind: each press arrives once and the pad in use settles.
        {
            for (const std::size_t lag : {std::size_t{0u}, std::size_t{1u}}) for (const bool later_slot_leads : {false, true}) {
                Fixture f(chili::PcInputConfig::defaults());
                XINPUT_STATE rest{}, held{};
                held.Gamepad.wButtons = XINPUT_GAMEPAD_A;
                f.slots[0] = rest; f.slots[3] = rest;
                f.input.scan_pads();
                (void)f.sample();
                // Presses of four samples, apart by two samples and by twelve (400 ms, longer than the idle time) in turn.
                std::vector<bool> timeline;
                unsigned presses = 0u;
                for (unsigned press = 0; press < 24u; ++press) {
                    timeline.insert(timeline.end(), (press & 1u) ? 2u : 12u, false);
                    timeline.insert(timeline.end(), 4u, true);
                    ++presses;
                }
                timeline.insert(timeline.end(), 4u, false);
                unsigned edges = 0u;
                bool was = false, doubled = false;
                for (std::size_t i = 0; i < timeline.size(); ++i) {
                    const bool lead = timeline[i], late = i >= lag && timeline[i - lag];
                    f.slots[later_slot_leads ? 3u : 0u] = lead ? held : rest;
                    f.slots[later_slot_leads ? 0u : 3u] = late ? held : rest;
                    const auto buttons = f.sample().buttons;
                    doubled = doubled || (buttons != 0u && buttons != cross);
                    const bool pressed = buttons == cross;
                    if (pressed && !was) ++edges;
                    was = pressed;
                }
                require(edges == presses && !doubled, "a pad that shows up twice presses once");
                require(f.input.report().pad_changes <= 2u, "and the pad in use does not go back and forth between its two appearances");
            }
        }
        // PlayStation pads: the position map from HID usages onto the sixteen logical buttons, sticks and triggers.
        {
            using chili::PadSample;
            using chili::HidPadReading;
            HidPadReading reading;
            auto sample = chili::hid_pad_sample(reading, 0x054Cu, 0x0CE6u, 9u);
            require(sample.kind == chili::PadKind::PlayStation && sample.vid == 0x054Cu && sample.pid == 0x0CE6u && sample.packet == 9u && sample.buttons == 0u &&
                    sample.lt == 0.0f && sample.lx == 0.0f, "a PlayStation pad at rest");
            // Button usage 1 Square, 2 Cross, 3 Circle, 4 Triangle, 5 L1, 6 R1, 9 Share or Create, 10 Options, 11 L3, 12 R3.
            static constexpr std::pair<unsigned, unsigned> map[]{{1u, PadSample::X}, {2u, PadSample::A}, {3u, PadSample::B}, {4u, PadSample::Y}, {5u, PadSample::LB},
                {6u, PadSample::RB}, {9u, PadSample::Back}, {10u, PadSample::Start}, {11u, PadSample::LS}, {12u, PadSample::RS}};
            bool mapped = true;
            for (const auto &[usage, button] : map) {
                reading = {}; reading.triggers = true; reading.buttons = 1u << (usage - 1u);
                mapped = mapped && chili::hid_pad_sample(reading, 0u, 0u).buttons == PadSample::bit(button);
            }
            require(mapped, "Cross is A, Circle B, Square X, Triangle Y, L1 LB, R1 RB, Share or Create Back, Options Start, L3 LS, R3 RS");
            reading = {}; reading.triggers = true; reading.buttons = (1u << 6) | (1u << 7) | (1u << 12) | (1u << 13) | (1u << 14);
            sample = chili::hid_pad_sample(reading, 0u, 0u);
            require(sample.buttons == 0u && sample.lt == 0.0f && sample.rt == 0.0f, "with trigger axes the two trigger buttons are not used; PS, the touch pad and mute are not mapped");
            reading.triggers = false;
            sample = chili::hid_pad_sample(reading, 0u, 0u);
            require(sample.buttons == 0u && sample.lt == 1.0f && sample.rt == 1.0f && sample.with_triggers(0.12f) == PadSample::trigger_bits, "without them the buttons are the triggers, fully pulled");
            reading = {}; reading.triggers = true; reading.rx = 0.25f; reading.ry = 0.75f; reading.x = -1.0f; reading.y = 0.5f; reading.z = 0.25f; reading.rz = -0.5f;
            sample = chili::hid_pad_sample(reading, 0u, 0u);
            require(sample.lt == 0.25f && sample.rt == 0.75f && sample.lx == -1.0f && sample.ly == 0.5f && sample.rx == 0.25f && sample.ry == -0.5f,
                "Rx and Ry are L2 and R2, X and Y the left stick, Z and Rz the right one");
            static constexpr unsigned hats[9]{PadSample::bit(PadSample::DpadUp), PadSample::bit(PadSample::DpadUp) | PadSample::bit(PadSample::DpadRight),
                PadSample::bit(PadSample::DpadRight), PadSample::bit(PadSample::DpadRight) | PadSample::bit(PadSample::DpadDown), PadSample::bit(PadSample::DpadDown),
                PadSample::bit(PadSample::DpadDown) | PadSample::bit(PadSample::DpadLeft), PadSample::bit(PadSample::DpadLeft),
                PadSample::bit(PadSample::DpadLeft) | PadSample::bit(PadSample::DpadUp), 0u};
            bool hat = true;
            for (unsigned value = 0; value < 9u; ++value) {
                reading = {}; reading.hat = static_cast<std::uint8_t>(value);
                hat = hat && chili::hid_pad_sample(reading, 0u, 0u).buttons == hats[value];
            }
            reading.hat = 15u;
            require(hat && chili::hid_pad_sample(reading, 0u, 0u).buttons == 0u, "the hat is the D-pad: up, clockwise through the diagonals, 8 and beyond released");

            require(chili::hid_stick_axis(0, 0, 255) == -1.0f && chili::hid_stick_axis(128, 0, 255) == 0.0f && chili::hid_stick_axis(255, 0, 255) == 1.0f &&
                    close_to(chili::hid_stick_axis(64, 0, 255), -64.0 / 127.0, 1e-6), "an 8-bit stick axis: 128 is the middle");
            require(chili::hid_stick_axis(-32768, -32768, 32767) == -1.0f && chili::hid_stick_axis(0, -32768, 32767) == 0.0f && chili::hid_stick_axis(32767, -32768, 32767) == 1.0f &&
                    chili::hid_stick_axis(5, 3, 3) == 0.0f, "a signed axis; a range that makes no sense");
            require(chili::hid_trigger_axis(0, 0, 255) == 0.0f && chili::hid_trigger_axis(255, 0, 255) == 1.0f && close_to(chili::hid_trigger_axis(51, 0, 255), 0.2, 1e-6) &&
                    chili::hid_trigger_axis(1023, 0, 1023) == 1.0f && chili::hid_trigger_axis(300, 0, 255) == 1.0f, "a trigger axis");
            using chili::HidPadModel;
            require(chili::hid_pad_model(0x054Cu, 0x05C4u) == HidPadModel::DualShock4 && chili::hid_pad_model(0x054Cu, 0x09CCu) == HidPadModel::DualShock4 &&
                    chili::hid_pad_model(0x054Cu, 0x0BA0u) == HidPadModel::DualShock4 && chili::hid_pad_model(0x054Cu, 0x0CE6u) == HidPadModel::DualSense &&
                    chili::hid_pad_model(0x054Cu, 0x0DF2u) == HidPadModel::DualSense && chili::hid_pad_model(0x054Cu, 0x0268u) == HidPadModel::Unknown &&
                    chili::hid_pad_model(0x045Eu, 0x0CE6u) == HidPadModel::Unknown, "the product ids of the DualShock 4 and the DualSense");
            HidPadReading a, b;
            a.buttons = b.buttons = 5u; a.x = 0.5f; b.x = 0.51f;
            require(chili::hid_pad_readings_agree(a, b), "two readings of one report may differ by rounding");
            b.x = 0.56f;
            require(!chili::hid_pad_readings_agree(a, b), "not by more");
            b.x = 0.5f; b.buttons = 4u;
            require(!chili::hid_pad_readings_agree(a, b), "and not in a button");
            b.buttons = 5u; b.hat = 2u;
            require(!chili::hid_pad_readings_agree(a, b), "or the hat");
        }
        // The reports of the PlayStation pads, read by their byte layouts. THE BYTES BELOW WERE MADE UP BY HAND FROM
        // GENERAL KNOWLEDGE OF THE LAYOUTS (hid_pad.hpp); none of them comes from a pad. A real pad's reports for
        // more of these come out of a run with CHILI_HID_DUMP=1 (the hex after "report n (.. bytes):").
        {
            using chili::HidPadLayout;
            using chili::HidPadModel;
            using chili::HidPadReading;
            using chili::PadSample;
            const auto bit = [](unsigned button) { return PadSample::bit(button); };
            HidPadReading reading;
            // The check sum is the zip CRC-32.
            const std::uint8_t digits[]{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
            require(chili::hid_crc32(digits, 9u) == 0xCBF43926u && chili::hid_crc32(digits + 4, 5u, chili::hid_crc32(digits, 4u)) == 0xCBF43926u && chili::hid_crc32(digits, 0u) == 0u,
                "CRC-32, in one go and in two");

            // DualShock 4, USB report 01, 64 bytes, at rest: sticks in the middle, hat 8, nothing down.
            auto report = report_bytes(64u, {0x01, 0x80, 0x80, 0x80, 0x80, 0x08, 0x00, 0x00, 0x00, 0x00});
            require(chili::hid_pad_decode(HidPadModel::DualShock4, false, report.data(), report.size(), reading) == HidPadLayout::DualShock4 && reading.buttons == 0u &&
                    reading.hat == 8u && reading.x == 0.0f && reading.y == 0.0f && reading.z == 0.0f && reading.rz == 0.0f && reading.rx == 0.0f && reading.ry == 0.0f && reading.triggers,
                "DualShock 4 by USB, at rest");
            // Left stick fully left; Cross (byte 5 bit 5); R1 (byte 6 bit 1); the counter in byte 7's top six bits; R2 half pulled.
            report = report_bytes(64u, {0x01, 0x00, 0x80, 0x80, 0x80, 0x28, 0x02, 0xFC, 0x00, 0x80});
            require(chili::hid_pad_decode(HidPadModel::DualShock4, false, report.data(), report.size(), reading) == HidPadLayout::DualShock4 && reading.buttons == 0x22u &&
                    reading.hat == 8u && reading.x == -1.0f && reading.rx == 0.0f && close_to(reading.ry, 128.0 / 255.0, 1e-6), "DualShock 4 by USB: Cross, R1, left stick left, R2 half");
            auto sample = chili::hid_pad_sample(reading, 0x054Cu, 0x09CCu);
            require(sample.buttons == (bit(PadSample::A) | bit(PadSample::RB)) && sample.lx == -1.0f && sample.lt == 0.0f && sample.with_triggers(0.12f) == (sample.buttons | bit(PadSample::RT)),
                "as logical buttons: A, RB and the right trigger");
            // Everything down: the four face buttons with the hat down, byte 6 full, PS and the touch pad, both triggers.
            report = report_bytes(64u, {0x01, 0xFF, 0xFF, 0x00, 0x00, 0xF4, 0xFF, 0x03, 0xFF, 0xFF});
            require(chili::hid_pad_decode(HidPadModel::DualShock4, false, report.data(), report.size(), reading) == HidPadLayout::DualShock4 && reading.buttons == 0x3FFFu &&
                    reading.hat == 4u && reading.x == 1.0f && reading.y == 1.0f && reading.z == -1.0f && reading.rz == -1.0f && reading.rx == 1.0f && reading.ry == 1.0f,
                "DualShock 4 by USB, everything down");
            sample = chili::hid_pad_sample(reading, 0x054Cu, 0x05C4u);
            require(sample.with_triggers(0.12f) == (0xFFFFu & ~(bit(PadSample::DpadUp) | bit(PadSample::DpadLeft) | bit(PadSample::DpadRight))), "fourteen logical buttons: all but three D-pad directions");
            // The same report over plain Bluetooth is ten bytes long.
            report = report_bytes(10u, {0x01, 0x80, 0x80, 0x80, 0x80, 0x12, 0x00, 0x01, 0x40, 0x00});
            require(chili::hid_pad_decode(HidPadModel::DualShock4, true, report.data(), report.size(), reading) == HidPadLayout::DualShock4 && reading.buttons == 0x1001u &&
                    reading.hat == 2u && close_to(reading.rx, 64.0 / 255.0, 1e-6), "DualShock 4, plain Bluetooth report: Square, PS, hat right, L2 a quarter");
            report.resize(9u);
            require(chili::hid_pad_decode(HidPadModel::DualShock4, true, report.data(), report.size(), reading) == HidPadLayout::None, "a report that is cut short is not read");

            // DualShock 4, Bluetooth report 11, 78 bytes: two more bytes in front of the fields, CRC-32 at the end.
            // Left stick up, Circle (byte 7 bit 6), PS (byte 9 bit 0).
            report = report_bytes(78u, {0x11, 0xC0, 0x00, 0x80, 0x00, 0x80, 0x80, 0x48, 0x00, 0x01, 0x00, 0x00});
            reading = {};
            require(chili::hid_pad_decode(HidPadModel::DualShock4, true, report.data(), report.size(), reading) == HidPadLayout::None && reading.buttons == 0u,
                "a long Bluetooth report without its check sum is not read");
            seal_bluetooth(report);
            require(chili::hid_bluetooth_sum_ok(report.data(), 78u) && chili::hid_pad_decode(HidPadModel::DualShock4, true, report.data(), report.size(), reading) == HidPadLayout::DualShock4Bluetooth &&
                    reading.buttons == 0x1004u && reading.hat == 8u && reading.y == -1.0f && reading.x == 0.0f, "DualShock 4, Bluetooth report 11: Circle, PS, left stick up");
            // Raw Input hands every report over at the length of the device's longest one, zeros after the report.
            auto padded = report;
            padded.resize(547u, std::uint8_t{0u});
            require(chili::hid_pad_decode(HidPadModel::DualShock4, true, padded.data(), padded.size(), reading) == HidPadLayout::DualShock4Bluetooth, "the same report with zeros after it");
            report[20] ^= 0x01u;
            require(chili::hid_pad_decode(HidPadModel::DualShock4, true, report.data(), report.size(), reading) == HidPadLayout::None, "one wrong bit in a long Bluetooth report: not read");

            // DualSense, USB report 01, 64 bytes: the triggers come before the buttons, a counter in byte 7.
            // Right stick right, L2 a quarter, hat down with Square (byte 8), Create (byte 9 bit 4), PS and mute (byte 10).
            report = report_bytes(64u, {0x01, 0x80, 0x80, 0xFF, 0x80, 0x40, 0x00, 0x2A, 0x14, 0x10, 0x05});
            require(chili::hid_pad_decode(HidPadModel::DualSense, false, report.data(), report.size(), reading) == HidPadLayout::DualSenseUsb && reading.buttons == 0x5101u &&
                    reading.hat == 4u && reading.z == 1.0f && reading.rz == 0.0f && close_to(reading.rx, 64.0 / 255.0, 1e-6) && reading.ry == 0.0f && reading.x == 0.0f,
                "DualSense by USB: Square, Create, PS, mute, hat down, right stick right, L2 a quarter");
            sample = chili::hid_pad_sample(reading, 0x054Cu, 0x0CE6u);
            require(sample.with_triggers(0.12f) == (bit(PadSample::X) | bit(PadSample::Back) | bit(PadSample::DpadDown) | bit(PadSample::LT)) && sample.rx == 1.0f,
                "as logical buttons: X, Back, D-pad down and the left trigger");
            report = report_bytes(64u, {0x01, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00, 0xFF, 0x08, 0x00, 0x00});
            require(chili::hid_pad_decode(HidPadModel::DualSense, false, report.data(), report.size(), reading) == HidPadLayout::DualSenseUsb && reading.buttons == 0u && reading.hat == 8u &&
                    reading.rx == 0.0f && reading.ry == 0.0f, "DualSense by USB, at rest: the counter is not a trigger");
            // The plain Bluetooth report of a DualSense is laid out like the DualShock 4's: Triangle, Options.
            report = report_bytes(78u, {0x01, 0x80, 0x80, 0x80, 0x80, 0x88, 0x20, 0x00, 0x00, 0x00});
            require(chili::hid_pad_decode(HidPadModel::DualSense, true, report.data(), report.size(), reading) == HidPadLayout::DualSenseBluetoothPlain && reading.buttons == 0x208u &&
                    reading.hat == 8u, "DualSense, plain Bluetooth report: Triangle, Options");
            // DualSense, Bluetooth report 31, 78 bytes: a tag byte, then the USB fields. R2 full, Cross, L3.
            report = report_bytes(78u, {0x31, 0x01, 0x80, 0x80, 0x80, 0x80, 0x00, 0xFF, 0x07, 0x28, 0x40, 0x00});
            require(chili::hid_pad_decode(HidPadModel::DualSense, true, report.data(), report.size(), reading) == HidPadLayout::None, "without its check sum: not read");
            seal_bluetooth(report);
            require(chili::hid_pad_decode(HidPadModel::DualSense, true, report.data(), report.size(), reading) == HidPadLayout::DualSenseBluetooth && reading.buttons == 0x402u &&
                    reading.ry == 1.0f && reading.rx == 0.0f && reading.hat == 8u, "DualSense, Bluetooth report 31: Cross, L3, R2 full");
            sample = chili::hid_pad_sample(reading, 0x054Cu, 0x0DF2u);
            require(sample.with_triggers(0.12f) == (bit(PadSample::A) | bit(PadSample::LS) | bit(PadSample::RT)), "as logical buttons: A, LS and the right trigger");
            // What is not known is not read.
            report = report_bytes(64u, {0x01, 0x80, 0x80, 0x80, 0x80, 0x28});
            require(chili::hid_pad_decode(HidPadModel::Unknown, false, report.data(), report.size(), reading) == HidPadLayout::None, "a pad of an unknown model has no layout");
            report[0] = 0x02u;
            require(chili::hid_pad_decode(HidPadModel::DualShock4, false, report.data(), report.size(), reading) == HidPadLayout::None &&
                    chili::hid_pad_decode(HidPadModel::DualSense, false, report.data(), report.size(), reading) == HidPadLayout::None &&
                    chili::hid_pad_decode(HidPadModel::DualSense, false, report.data(), 0u, reading) == HidPadLayout::None &&
                    chili::hid_pad_decode(HidPadModel::DualSense, false, nullptr, 64u, reading) == HidPadLayout::None, "another report id, no bytes");
            require(std::string_view(chili::hid_pad_layout_name(HidPadLayout::DualSenseUsb)) == "dualsense usb report 01" && chili::hid_hex(digits, 3u) == "31 32 33", "names and hex for the dump");

            // The vibration reports for a pad on USB (never sent to a pad from here): only the motors are marked valid.
            std::array<std::uint8_t, 64> out{};
            require(chili::hid_pad_rumble_report(HidPadModel::DualShock4, false, 0x40u, 0x80u, out) == 32u && out[0] == 0x05u && out[1] == 0x01u && out[4] == 0x80u && out[5] == 0x40u &&
                    out[2] == 0u && out[3] == 0u && out[6] == 0u && out[31] == 0u, "DualShock 4: report 05, 32 bytes, the small motor then the large one");
            require(chili::hid_pad_rumble_report(HidPadModel::DualSense, false, 0x40u, 0x80u, out) == 48u && out[0] == 0x02u && out[1] == 0x03u && out[2] == 0u && out[3] == 0x80u &&
                    out[4] == 0x40u && out[5] == 0u && out[47] == 0u, "DualSense: report 02, 48 bytes, the small motor then the large one");
            require(chili::hid_pad_rumble_report(HidPadModel::DualSense, true, 1u, 1u, out) == 0u && chili::hid_pad_rumble_report(HidPadModel::DualShock4, true, 1u, 1u, out) == 0u &&
                    chili::hid_pad_rumble_report(HidPadModel::Unknown, false, 1u, 1u, out) == 0u, "no report for Bluetooth or for an unknown model");
            chili::HidPadOutput writer(L"\\\\?\\not-a-device", HidPadModel::DualSense, false, 48u), wrong_length(L"x", HidPadModel::DualSense, false, 547u),
                bluetooth(L"x", HidPadModel::DualSense, true, 78u);
            require(writer.supported() && !wrong_length.supported() && !bluetooth.supported(), "a report is written only to a device that takes one of exactly that length, by USB");
            wrong_length.set(255u, 255u); bluetooth.set(255u, 255u);
            require(wrong_length.written() == 0u && wrong_length.failed() == 0u && bluetooth.failed() == 0u, "otherwise nothing is even tried");
            // A device that cannot be opened is a counted failure on the writer's own thread, nothing more.
            writer.set(10u, 20u);
            for (int i = 0; i < 3000 && !writer.failed(); ++i) Sleep(1);
            require(writer.failed() >= 1u && writer.error() != 0u && writer.written() == 0u, "a pad that cannot be opened for writing: counted, with the system's error");
        }
        // A PlayStation pad in the input layer (made up here; in the game its Raw Input reports end up in the same
        // three calls): by position through [Input.Gamepad], the log lines, the silence rule, removal.
        {
            reset_shared_state();
            const auto &shared = chili::input_device_state();
            using chili::HidPadReading;
            Fixture f(chili::PcInputConfig::defaults());
            const auto info = playstation_pad();
            require(info.describe() == "PlayStation pad 054C:0CE6 'Wireless Controller' usb, 15 buttons 6 axes hat", "what the log says of a PlayStation pad");
            require(f.input.hid_arrived(0x77u, info) && f.log.back() == "gamepad: " + info.describe(), "the pad arrives and is logged");
            HidPadReading reading;
            reading.triggers = true;
            const auto send = [&](const HidPadReading &now) { f.input.hid_report(0x77u, chili::hid_pad_sample(now, 0u, 0u)); return f.sample(); };
            auto s = send(reading);
            require(s.buttons == 0u && f.input.pad_text() == "playstation 054C:0CE6" && f.input.active_pad() && f.input.active_pad()->kind == chili::PadKind::PlayStation &&
                    f.input.active_pad()->vid == 0x054Cu && f.input.active_pad()->pid == 0x0CE6u, "at rest it is the pad in use, and a PlayStation pad");
            reading.buttons = 1u << 1;
            require(send(reading).buttons == cross, "Cross is A: Cross");
            reading.buttons = 1u << 0;
            require(send(reading).buttons == triangle, "Square is X: Triangle, reload, in the dual_analog defaults");
            reading.buttons = (1u << 2) | (1u << 3);
            require(send(reading).buttons == (circle | cross), "Circle is B, Triangle is Y");
            reading.buttons = (1u << 4) | (1u << 5) | (1u << 10);
            require(send(reading).buttons == (left | right | l_button), "L1 and R1 are LB and RB, L3 is LS");
            reading.buttons = (1u << 8) | (1u << 9);
            require(send(reading).buttons == (select_button | start_button), "Share or Create is Back, Options is Start");
            reading.buttons = 0u; reading.rx = 0.1f; reading.ry = 0.5f;
            require(send(reading).buttons == square, "R2 past the threshold shoots, L2 short of it does nothing");
            reading.rx = 0.13f; reading.ry = 0.0f;
            require(send(reading).buttons == r_button, "L2 past the threshold");
            reading.rx = 0.0f; reading.hat = 6u;
            require(send(reading).buttons == left, "the hat is the D-pad");
            reading.hat = 8u; reading.x = 1.0f; reading.rz = -1.0f;
            s = send(reading);
            require(s.x == 255u && s.y == 128u && s.ry < 128u && s.rx == 128u, "left stick right, right stick up");
            require(shared.device.load() == chili::InputDevice::Gamepad && shared.pad_kind.load() == chili::PadKind::PlayStation, "the prompt pictures are told: a pad, a PlayStation pad");
            // A decoded report all the way through: the hand-made DualShock 4 report with Cross, R1 and R2 half pulled.
            const auto report = report_bytes(64u, {0x01, 0x80, 0x80, 0x80, 0x80, 0x28, 0x02, 0xFC, 0x00, 0x80});
            HidPadReading decoded;
            require(chili::hid_pad_decode(chili::HidPadModel::DualShock4, false, report.data(), report.size(), decoded) != chili::HidPadLayout::None &&
                    send(decoded).buttons == (cross | right | square), "a report's Cross, R1 and R2 reach the game as Cross, D-pad right and Square");
            // This pad has only ever sent a report when something on it changed. Such a pad says nothing while a
            // button is held, so its silence means nothing (the next block has the rule and its timings).
            reading = {}; reading.triggers = true; reading.buttons = 1u << 1;
            require(send(reading).buttons == cross, "held");
            bool still_held = true;
            for (int i = 0; i < 30; ++i) still_held = still_held && f.sample().buttons == cross;
            require(still_held, "a pad that reports only changes keeps what is held while it says nothing, for a second and longer");
            f.input.hid_removed(0x77u);
            require(f.logged("gamepad: PlayStation pad 054C:0CE6 removed") && f.log.back() == "gamepad: none" && f.input.pad_text() == "none" && f.sample().buttons == 0u,
                "the pad is removed: logged, and no pad is left");
            require(shared.pad_kind.load() == chili::PadKind::PlayStation, "the kind of the pad used last is kept after it is gone");
            f.input.hid_report(0x77u, chili::hid_pad_sample(reading, 0u, 0u));
            require(f.sample().buttons == 0u, "a report of a pad that is gone is nothing");

            // Settings that keep PlayStation pads out, and the limit of four.
            auto off = chili::PcInputConfig::defaults();
            require(off.apply("[Input.Gamepad]\nPlayStationPads=false\n").empty(), "PlayStationPads option");
            Fixture g(off);
            require(!g.input.hid_arrived(1u, info) && g.log.empty(), "PlayStationPads=false: the pad is not taken");
            auto none = chili::PcInputConfig::defaults();
            require(none.apply("[Input.Gamepad]\nEnabled=false\n").empty(), "pads off");
            Fixture h(none);
            require(!h.input.hid_arrived(1u, info), "pads switched off: not taken");
            Fixture many(chili::PcInputConfig::defaults());
            require(many.input.hid_arrived(1u, info) && many.input.hid_arrived(2u, info) && many.input.hid_arrived(3u, info) && many.input.hid_arrived(4u, info) &&
                    many.input.hid_arrived(2u, info) && !many.input.hid_arrived(5u, info) && many.input.report().pads_playstation == 4u, "four PlayStation pads are read, a fifth is not");

            // The same pad as a PlayStation pad and, through a helper program, as an Xbox-style pad: one press.
            Fixture twin(chili::PcInputConfig::defaults());
            (void)twin.input.hid_arrived(9u, info);
            XINPUT_STATE xbox{};
            twin.slots[0] = xbox;
            twin.input.scan_pads();
            (void)twin.sample();
            reading = {}; reading.triggers = true; reading.buttons = 1u << 1;
            xbox.Gamepad.wButtons = XINPUT_GAMEPAD_A; twin.slots[0] = xbox;
            twin.input.hid_report(9u, chili::hid_pad_sample(reading, 0u, 0u));
            require(twin.sample().buttons == cross && twin.sample().buttons == cross && twin.input.report().pad_changes == 1u, "both appearances pressed: one press, one pad in use");
        }
        // A PlayStation pad that says nothing, with made-up report timings. There are two kinds of pad:
        // - one that streams: it sends its state all the time, a few milliseconds apart, also when nothing changed.
        //   When such a pad falls silent it is out of range or switched off, and Windows may take seconds to say so:
        //   it is taken to be at rest after half a second, or after twenty of its own report intervals if that is
        //   longer, and the log says so once;
        // - one that sends a report only when something changed (a Bluetooth pad in its plain report mode may). It
        //   says nothing while a stick is held, so silence must never put it to rest: only its removal does.
        // Which kind a pad is, is measured: reports that repeat the state before them, at short intervals.
        {
            using chili::PadSample;
            const auto info = playstation_pad(0x09CCu);
            // The pad reports `state` every `every` seconds (0: not at all) and the game takes a sample every 60th
            // of a second, for `seconds`. True when every sample had `buttons` and the left stick's byte `x`.
            const auto run = [](Fixture &f, std::uintptr_t id, double seconds, double every, const PadSample &state, std::uint32_t buttons, std::uint8_t x) {
                bool all = true;
                const double end = f.now + seconds;
                double report_at = f.now + every, sample_at = f.now + 1.0 / 60.0;
                while (true) {
                    const bool report = every > 0.0 && report_at <= sample_at;
                    const double at = report ? report_at : sample_at;
                    if (at > end + 1.0e-9) break;
                    f.now = at;
                    if (report) { f.input.hid_report(id, state); report_at += every; }
                    else { const auto s = f.input.sample(); all = all && s.buttons == buttons && s.x == x; sample_at += 1.0 / 60.0; }
                }
                f.now = end;
                return all;
            };
            const auto silence_lines = [](const Fixture &f) {
                return std::count_if(f.log.begin(), f.log.end(), [](const std::string &line) { return line.find("has sent nothing for") != std::string::npos; });
            };
            PadSample held;   // Cross, and the left stick all the way to the right
            held.buttons = PadSample::bit(PadSample::A); held.lx = 1.0f;
            const PadSample rest{};

            // Reports only on change: one report when the stick was pushed and the button pressed, then nothing.
            Fixture change(chili::PcInputConfig::defaults());
            (void)change.input.hid_arrived(1u, info);
            change.input.hid_report(1u, rest);
            (void)change.sample();
            change.input.hid_report(1u, held);
            require(run(change, 1u, 5.0, 0.0, held, cross, 255u), "a pad that reports only on change: a stick and a button held for five seconds stay held");
            // The same pad while its stick is moved: a report every 8 ms, no two alike, then held again.
            for (int i = 0; i < 125; ++i) {
                change.now += 0.008;
                PadSample moving = held;
                moving.ly = -0.5f + 0.004f * static_cast<float>(i);
                change.input.hid_report(1u, moving);
                if (i & 1) (void)change.input.sample();
            }
            change.now += 0.008;
            change.input.hid_report(1u, held);
            require(run(change, 1u, 5.0, 0.0, held, cross, 255u), "nor does a second of reports 8 ms apart, each a change, make its silence mean anything: held for five seconds more");
            // A few reports that repeat the state before them (a button this code does not map, pressed and let go)
            // are not a stream.
            for (int i = 0; i < 6; ++i) { change.now += 0.03; change.input.hid_report(1u, held); (void)change.input.sample(); }
            require(run(change, 1u, 5.0, 0.0, held, cross, 255u) && silence_lines(change) == 0, "a handful of repeated reports is not a stream; nothing was logged about silence");
            change.input.hid_removed(1u);
            change.now += 1.0 / 60.0;
            require(change.input.sample().buttons == 0u && change.input.pad_text() == "none", "its removal lets go of what was held");

            // A pad that streams, 250 reports a second, with the stick held: held as long as it reports.
            Fixture stream(chili::PcInputConfig::defaults());
            (void)stream.input.hid_arrived(2u, info);
            require(run(stream, 2u, 5.0, 0.004, held, cross, 255u), "a pad that streams: a stick held for five seconds stays held");
            // Out of range: nothing more comes.
            require(run(stream, 2u, 0.45, 0.0, held, cross, 255u), "silent for less than half a second: its state is kept");
            (void)run(stream, 2u, 0.15, 0.0, held, cross, 255u);
            stream.now += 1.0 / 60.0;
            auto s = stream.input.sample();
            require(s.buttons == 0u && s.x == 128u, "silent for more than half a second after streaming: taken to be at rest");
            require(silence_lines(stream) == 1 && stream.logged("gamepad: PlayStation pad 054C:09CC has sent nothing for 0.50 s after a report every 4 ms: taken to be at rest until it reports again"),
                "and the log says so");
            require(stream.input.pad_text() == "playstation 054C:09CC", "it is still there: only its removal takes it away");
            // In range again.
            require(run(stream, 2u, 1.0, 0.004, held, cross, 255u), "when it reports again its state is back at once");
            (void)run(stream, 2u, 0.7, 0.0, held, cross, 255u);
            stream.now += 1.0 / 60.0;
            require(stream.input.sample().buttons == 0u && silence_lines(stream) == 1, "silent again: at rest again, and the log line came once");
            require(stream.input.report().pad_silences == 2u &&
                    chili::input_summary(stream.input).find("; a PlayStation pad that fell silent was put to rest 2 times\n") != std::string::npos, "the exit summary counts both");
            // After that the pad has to stream again before its silence means something: one report, then nothing.
            stream.now += 0.5;
            stream.input.hid_report(2u, held);
            require(run(stream, 2u, 5.0, 0.0, held, cross, 255u), "one report after the rule fired, then silence: held, the pad has not shown again that it streams");

            // A pad that streams slowly, 25 reports a second: twenty of its intervals are 0.8 s.
            Fixture slow(chili::PcInputConfig::defaults());
            (void)slow.input.hid_arrived(3u, info);
            slow.input.hid_report(3u, held);
            require(run(slow, 3u, 3.0, 0.04, held, cross, 255u), "a pad that reports every 40 ms: held while it reports");
            require(run(slow, 3u, 0.7, 0.0, held, cross, 255u), "it is given twenty of its intervals: still held after 0.7 s of silence");
            (void)run(slow, 3u, 0.2, 0.0, held, cross, 255u);
            slow.now += 1.0 / 60.0;
            require(slow.input.sample().buttons == 0u && slow.logged("gamepad: PlayStation pad 054C:09CC has sent nothing for 0.80 s after a report every 40 ms: taken to be at rest until it reports again"),
                "at rest after 0.8 s");

            // Reports as the game gets them: the window's queue is emptied right before each sample, so four reports
            // arrive at the same instant. That is a stream too.
            Fixture burst(chili::PcInputConfig::defaults());
            (void)burst.input.hid_arrived(4u, info);
            bool burst_held = true;
            for (int i = 0; i < 120; ++i) {
                burst.now += 1.0 / 60.0;
                for (int report = 0; report < 4; ++report) burst.input.hid_report(4u, held);
                burst_held = burst_held && burst.input.sample().buttons == cross;
            }
            require(burst_held && run(burst, 4u, 0.45, 0.0, held, cross, 255u), "reports that arrive four at a time: held, and for 0.45 s of silence");
            (void)run(burst, 4u, 0.15, 0.0, held, cross, 255u);
            burst.now += 1.0 / 60.0;
            require(burst.input.sample().buttons == 0u && silence_lines(burst) == 1, "then at rest");
            // The game's thread stood still for two seconds (a mission loading) and takes a sample before the
            // waiting reports are read: that is not the pad's silence.
            Fixture stall(chili::PcInputConfig::defaults());
            (void)stall.input.hid_arrived(5u, info);
            require(run(stall, 5u, 1.0, 0.004, held, cross, 255u), "streaming");
            stall.now += 2.0;
            require(stall.input.sample().buttons == cross && silence_lines(stall) == 0, "a pause of the game itself is not counted as the pad's silence");
            // Nor do the reports that waited through that pause, read all at once afterwards, look like reports
            // after a pause of the pad's: it is still known to stream, and half a second of real silence puts it
            // to rest.
            for (int report = 0; report < 8; ++report) stall.input.hid_report(5u, held);
            require(run(stall, 5u, 0.45, 0.0, held, cross, 255u), "after the pause: held");
            (void)run(stall, 5u, 0.15, 0.0, held, cross, 255u);
            stall.now += 1.0 / 60.0;
            require(stall.input.sample().buttons == 0u && silence_lines(stall) == 1, "and the pad that then falls silent is put to rest: the pause did not undo what was measured");
        }
        // Which HID game controllers are read here: PlayStation pads. An Xbox pad is XInput's: Windows marks its device
        // path with IG_, and a controller of Microsoft's own (vendor 045E) is one whatever its path says. Telling its
        // owner that it is "not a PlayStation pad" and to "use an XInput-compatible mode" would be wrong twice.
        {
            using Refused = chili::HidPadDevice::Refused;
            std::string note = "x";
            require(chili::HidPadDevice::refusal(0x054Cu, 0x0CE6u, false, "Wireless Controller", note) == Refused::No && note.empty(), "a PlayStation pad is read here");
            require(chili::HidPadDevice::refusal(0x045Eu, 0x028Eu, true, "Controller (XBOX 360 For Windows)", note) == Refused::XInput && note.empty(),
                "a device with an XInput path is left to XInput and nothing is said");
            require(chili::HidPadDevice::refusal(0x054Cu, 0x05C4u, true, "Wireless Controller", note) == Refused::XInput && note.empty(),
                "so is a PlayStation pad that a helper program presents as an XInput pad");
            const auto microsoft = chili::HidPadDevice::refusal(0x045Eu, 0x0B13u, false, "Xbox Wireless Controller", note);
            require(microsoft == Refused::XInput, "a Microsoft controller without that path is an Xbox pad too: left to XInput");
            require(note == "gamepad: HID 045E:0B13 'Xbox Wireless Controller' is a Microsoft pad: it is read through XInput", "with one line that says so");
            require(note.find("ignored") == std::string::npos && note.find("not a PlayStation pad") == std::string::npos, "and not the line of a pad of another make");
            require(chili::HidPadDevice::refusal(0x046Du, 0xC216u, false, "Logitech Dual Action", note) == Refused::NotSony &&
                    note == "gamepad: HID 046D:C216 'Logitech Dual Action' ignored (not a PlayStation pad; use an XInput-compatible mode)", "a pad of another make is not read, and the log says why");
        }
        // [Input.Gamepad.PlayStation]: a PlayStation pad's own targets for the buttons the section names; Glyphs names
        // the kind a pad is taken for.
        {
            using chili::PadKind;
            using chili::PadSample;
            auto config = chili::PcInputConfig::defaults();
            require(config.playstation_named == 0u && config.pad_target(PadSample::X, PadKind::PlayStation) == PadTarget::Triangle, "without the section a PlayStation pad uses [Input.Gamepad]");
            require(config.apply("[Input.Gamepad.PlayStation]\nX=Square\nY = Triangle\nlt=None\n").empty(), "the section is read");
            require(config.playstation_named == (PadSample::bit(PadSample::X) | PadSample::bit(PadSample::Y) | PadSample::bit(PadSample::LT)) &&
                    config.pad_target(PadSample::X, PadKind::PlayStation) == PadTarget::Square && config.pad_target(PadSample::Y, PadKind::PlayStation) == PadTarget::Triangle &&
                    config.pad_target(PadSample::LT, PadKind::PlayStation) == PadTarget::None && config.pad_target(PadSample::A, PadKind::PlayStation) == PadTarget::Cross &&
                    config.pad_target(PadSample::X, PadKind::Xbox) == PadTarget::Triangle && config.pad_target(PadSample::LT, PadKind::Xbox) == PadTarget::R &&
                    config.pad_target(PadSample::X, PadKind::None) == PadTarget::Triangle && config.pad_target(99u, PadKind::PlayStation) == PadTarget::None,
                "a named button has the section's target; the others, and every button of an Xbox-style pad, [Input.Gamepad]'s");
            const auto problems = config.apply("[Input.Gamepad.PlayStation]\nSquare=Cross\nA=Nope\nDeadzoneLeft=0.3\n");
            require(problems.size() == 3u && !(config.playstation_named & PadSample::bit(PadSample::A)), "the section takes the sixteen button names only, and a bad target names nothing");
            Fixture f(config);
            (void)f.input.hid_arrived(1u, playstation_pad(0x09CCu));
            chili::HidPadReading reading;
            reading.triggers = true; reading.buttons = 1u;
            f.input.hid_report(1u, chili::hid_pad_sample(reading, 0u, 0u));
            require(f.sample().buttons == square, "Square on the PlayStation pad is the section's Square");
            reading.buttons = 0u; reading.rx = 1.0f;
            f.input.hid_report(1u, chili::hid_pad_sample(reading, 0u, 0u));
            require(f.sample().buttons == 0u, "its L2 is unbound by the section");
            f.input.hid_removed(1u);
            XINPUT_STATE xbox{};
            xbox.Gamepad.wButtons = XINPUT_GAMEPAD_X; xbox.Gamepad.bLeftTrigger = 255;
            f.pad = xbox; f.input.scan_pads();
            require(f.sample().buttons == (triangle | r_button), "an Xbox-style pad keeps [Input.Gamepad]: X reloads, LT is R");
            require(config.kind_of(PadKind::Xbox) == PadKind::Xbox && config.kind_of(PadKind::PlayStation) == PadKind::PlayStation, "Glyphs=auto: a pad is what it is");
            auto forced = config;
            require(forced.apply("[Input.Gamepad]\nGlyphs=playstation\n").empty() && forced.kind_of(PadKind::Xbox) == PadKind::PlayStation, "Glyphs=playstation");
            Fixture g(forced);
            g.pad = xbox; g.input.scan_pads();
            require(g.sample().buttons == square, "Glyphs=playstation: a pad that shows up as an Xbox-style pad uses the PlayStation table");
            auto plain = config;
            require(plain.apply("[Input.Gamepad]\nGlyphs=xbox\n").empty() && plain.kind_of(PadKind::PlayStation) == PadKind::Xbox, "Glyphs=xbox");
            Fixture h(plain);
            (void)h.input.hid_arrived(1u, playstation_pad());
            reading = {}; reading.triggers = true; reading.buttons = 1u;
            h.input.hid_report(1u, chili::hid_pad_sample(reading, 0u, 0u));
            require(h.sample().buttons == triangle, "Glyphs=xbox: a PlayStation pad uses [Input.Gamepad]");
        }
        // The device used last, as the prompt pictures are told it (input_device.hpp): keyboard and mouse or the pad,
        // whichever was used in a sample; both or neither change nothing.
        {
            using chili::InputDevice;
            using chili::PadKind;
            reset_shared_state();
            const auto &shared = chili::input_device_state();
            const auto is = [&](InputDevice device, PadKind kind) { return shared.device.load() == device && shared.pad_kind.load() == kind; };
            LRESULT result{};
            Fixture f(chili::PcInputConfig::defaults());
            (void)f.sample();
            require(is(InputDevice::KeyboardMouse, PadKind::None) && f.input.device() == InputDevice::KeyboardMouse, "nothing touched and no pad: keyboard and mouse");
            f.keys = {key("W")};
            (void)f.sample();
            f.keys.clear();
            XINPUT_STATE pad{};
            f.pad = pad; f.input.scan_pads();
            (void)f.sample();
            require(is(InputDevice::KeyboardMouse, PadKind::Xbox), "a pad that arrives after a key was used: its kind is known, the keyboard stays the device");
            pad.Gamepad.wButtons = XINPUT_GAMEPAD_A; f.pad = pad;
            (void)f.sample();
            require(is(InputDevice::Gamepad, PadKind::Xbox) && f.input.device() == InputDevice::Gamepad && f.input.report().device_changes == 1u, "a pad button: the pad");
            f.keys = {key("W")};
            (void)f.sample();
            require(is(InputDevice::Gamepad, PadKind::Xbox), "a key and a pad button in one sample change nothing");
            pad = {}; f.pad = pad; f.keys.clear();
            (void)f.sample();
            require(is(InputDevice::Gamepad, PadKind::Xbox), "nor does a sample with neither");
            f.keys = {key("W")};
            (void)f.sample();
            require(is(InputDevice::KeyboardMouse, PadKind::Xbox), "a bound key: the keyboard");
            f.keys.clear();
            pad.Gamepad.sThumbRX = 7000; f.pad = pad;
            (void)f.sample();
            require(is(InputDevice::KeyboardMouse, PadKind::Xbox), "a stick inside its dead zone is not use of the pad");
            pad.Gamepad.sThumbRX = 12000; f.pad = pad;
            (void)f.sample();
            require(is(InputDevice::Gamepad, PadKind::Xbox), "a stick past its dead zone that was moved there is");
            pad = {}; f.pad = pad;
            f.keys = {VK_F9};
            (void)f.sample();
            require(is(InputDevice::Gamepad, PadKind::Xbox), "a key that is bound to nothing changes nothing");
            f.keys.clear();
            f.key_message(WM_KEYDOWN, VK_SPACE);
            f.key_message(WM_KEYUP, VK_SPACE);
            (void)f.sample();
            require(is(InputDevice::KeyboardMouse, PadKind::Xbox), "a tap shorter than a frame counts");
            pad.Gamepad.bLeftTrigger = 25; f.pad = pad;
            (void)f.sample();
            require(is(InputDevice::KeyboardMouse, PadKind::Xbox), "a trigger short of its threshold is not use");
            pad.Gamepad.bLeftTrigger = 40; f.pad = pad;
            (void)f.sample();
            require(is(InputDevice::Gamepad, PadKind::Xbox), "a trigger past it is");
            pad = {}; f.pad = pad;
            (void)f.sample();

            // The free mouse over the window: more than 40 pixels within a tenth of a second.
            (void)f.input.window_message(nullptr, WM_MOUSEMOVE, 0, MAKELPARAM(100, 100), result);
            (void)f.sample();
            (void)f.input.window_message(nullptr, WM_MOUSEMOVE, 0, MAKELPARAM(120, 110), result);
            (void)f.sample();
            require(is(InputDevice::Gamepad, PadKind::Xbox), "the free mouse moved 30 pixels: not enough to be the device");
            f.now += 0.3;
            (void)f.input.window_message(nullptr, WM_MOUSEMOVE, 0, MAKELPARAM(150, 130), result);
            (void)f.sample();
            require(is(InputDevice::KeyboardMouse, PadKind::Xbox), "50 pixels: the mouse");
            const auto pad_again = [&] {
                pad = {}; pad.Gamepad.wButtons = XINPUT_GAMEPAD_B; f.pad = pad;
                (void)f.sample();
                pad = {}; f.pad = pad;
                (void)f.sample();
                f.now += 0.3;
                return is(InputDevice::Gamepad, PadKind::Xbox);
            };
            require(pad_again(), "back to the pad");
            (void)f.input.window_message(nullptr, WM_MOUSEMOVE, 0, MAKELPARAM(180, 130), result);
            (void)f.sample();
            require(is(InputDevice::Gamepad, PadKind::Xbox), "30 pixels in one sample");
            (void)f.input.window_message(nullptr, WM_MOUSEMOVE, 0, MAKELPARAM(180, 160), result);
            (void)f.sample();
            require(is(InputDevice::KeyboardMouse, PadKind::Xbox), "and 30 more in the next: movement adds up within a tenth of a second");
            // The captured mouse: more than 4 counts.
            require(pad_again(), "back to the pad again");
            f.input.set_captured_for_test(true);
            f.input.add_mouse_counts(3.0);
            (void)f.sample();
            require(is(InputDevice::Gamepad, PadKind::Xbox), "the captured mouse moved 3 counts: not enough");
            f.now += 0.3;
            f.input.add_mouse_counts(4.0, -1.0);
            (void)f.sample();
            require(is(InputDevice::KeyboardMouse, PadKind::Xbox), "5 counts: the mouse");
            // A mouse button, a wheel notch.
            require(pad_again(), "back to the pad a third time");
            (void)f.input.window_message(nullptr, WM_LBUTTONDOWN, 0, 0, result);
            (void)f.sample();
            require(is(InputDevice::KeyboardMouse, PadKind::Xbox), "a mouse button: the mouse");
            (void)f.input.window_message(nullptr, WM_LBUTTONUP, 0, 0, result);
            require(pad_again(), "back to the pad a fourth time");
            (void)f.input.window_message(nullptr, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0, result);
            (void)f.sample();
            require(is(InputDevice::KeyboardMouse, PadKind::Xbox), "a wheel notch: the mouse");
            for (int i = 0; i < 3; ++i) (void)f.sample();
            // A click in a menu (the mouse is free there).
            f.input.set_captured_for_test(false);
            require(pad_again(), "back to the pad a fifth time");
            f.input.set_look_active(false);
            (void)f.input.window_message(nullptr, WM_RBUTTONDOWN, 0, 0, result);
            (void)f.input.window_message(nullptr, WM_RBUTTONUP, 0, 0, result);
            (void)f.sample();
            require(is(InputDevice::KeyboardMouse, PadKind::Xbox), "a click in a menu: the mouse");
            require(f.input.report().samples_gamepad + f.input.report().samples_keyboard_mouse == f.input.report().samples && f.input.report().samples_gamepad > 10u &&
                    f.input.report().device_changes == 16u, "every host sample is counted under the device used last, and the changes are counted");

            // A pad that is there before anything was touched is the device from the first sample on.
            reset_shared_state();
            Fixture g(chili::PcInputConfig::defaults());
            g.pad = XINPUT_STATE{};
            g.input.scan_pads();
            (void)g.sample();
            require(is(InputDevice::Gamepad, PadKind::Xbox) && g.input.report().device_changes == 1u, "a game started with a pad connected: the pad is the device before anything is touched");
            (void)g.sample();
            g.keys = {key("W")};
            (void)g.sample();
            g.keys.clear();
            (void)g.sample();
            require(is(InputDevice::KeyboardMouse, PadKind::Xbox) && g.input.report().samples_gamepad == 2u && g.input.report().samples_keyboard_mouse == 2u,
                "then a key makes it the keyboard, and the pad at rest does not take it back");
            g.pad.reset();
            (void)g.sample();
            require(is(InputDevice::KeyboardMouse, PadKind::Xbox) && g.logged("gamepad: XInput slot 1 removed"), "the pad's kind is kept when it is unplugged");

            // A pad on the desk whose stick rests off centre (0.30, past the dead zone of 0.24) while the player uses
            // the keyboard and the mouse: the prompts are not pinned to the pad.
            reset_shared_state();
            Fixture worn(chili::PcInputConfig::defaults());
            pad = {}; pad.Gamepad.sThumbRX = 9830; worn.pad = pad;
            worn.input.scan_pads();
            (void)worn.sample();
            require(is(InputDevice::Gamepad, PadKind::Xbox), "a pad that is there at the start is the device, as before");
            worn.keys = {key("W")};
            (void)worn.sample();
            require(is(InputDevice::KeyboardMouse, PadKind::Xbox), "a key, with a pad whose stick rests off centre: the keyboard is the device");
            worn.keys.clear();
            bool stays = true;
            for (int i = 0; i < 60; ++i) { (void)worn.sample(); stays = stays && is(InputDevice::KeyboardMouse, PadKind::Xbox); }
            require(stays && worn.input.report().device_changes == 2u, "and the resting stick does not take the prompts back");
            (void)worn.input.window_message(nullptr, WM_LBUTTONDOWN, 0, 0, result);
            (void)worn.sample();
            (void)worn.input.window_message(nullptr, WM_LBUTTONUP, 0, 0, result);
            require(is(InputDevice::KeyboardMouse, PadKind::Xbox), "a mouse button beside it: still keyboard and mouse");
            pad.Gamepad.sThumbRX = 20000; worn.pad = pad;
            (void)worn.sample();
            require(is(InputDevice::Gamepad, PadKind::Xbox), "the stick pushed out: the pad");
            pad.Gamepad.sThumbRX = 9830; worn.pad = pad;
            worn.keys = {key("W")};
            (void)worn.sample(); (void)worn.sample();
            require(is(InputDevice::KeyboardMouse, PadKind::Xbox), "let go, back at its resting place, and a key: the keyboard again");

            // Another window has the focus: the sample says nothing.
            reset_shared_state();
            Fixture h(chili::PcInputConfig::defaults());
            pad = {}; pad.Gamepad.wButtons = XINPUT_GAMEPAD_A; h.pad = pad;
            h.input.scan_pads();
            h.focused = false;
            (void)h.sample();
            require(is(InputDevice::KeyboardMouse, PadKind::None) && h.input.report().samples == 1u && h.input.report().samples_gamepad + h.input.report().samples_keyboard_mouse == 0u,
                "a sample taken while another window has the focus says nothing about the device");

            // The title bar's input line and the exit summary name the pad and the device.
            auto shown = chili::PcInputConfig::defaults();
            require(shown.apply("[Input]\nShowInput=true\n").empty(), "ShowInput");
            Fixture t(shown);
            pad = {}; pad.Gamepad.wButtons = XINPUT_GAMEPAD_A; t.pad = pad;
            t.input.scan_pads();
            const auto sent = t.sample();
            require(chili::input_title_text(t.input, chili::InputSource::Host, sent).ends_with(" | sent buttons 4000 move 128,128 look 128,128 | pad: xbox slot 1 | last device: pad"),
                "the input test line names the pad in use and the device used last");
            const auto summary = chili::input_summary(t.input);
            require(summary.find("[input] gamepad: pad in use xbox slot 1; pads that arrived: xbox-style=1, playstation=0 (PlayStation pads are not read); the pad in use changed 1 times") != std::string::npos &&
                    summary.find("[input] device used last: pad; host samples with the pad=1, with keyboard and mouse=0, changes=1") != std::string::npos, "and so does the exit summary");
        }
        // A replay's samples, a script's and a deterministic run's never reach the host's devices, so the device used
        // last and the pad's kind stay as they were, whatever is held meanwhile.
        {
            using chili::InputDevice;
            using chili::InputSource;
            using chili::PadKind;
            namespace lc = chili::live_control;
            wchar_t temp[MAX_PATH]{};
            GetTempPathW(MAX_PATH, temp);
            const auto root = std::filesystem::path(temp) / ("chili-input-pipeline-" + std::to_string(GetCurrentProcessId()));
            std::filesystem::remove_all(root);
            std::filesystem::create_directories(root);
            const auto replay_file = root / "five.replay";
            { std::ofstream out(replay_file); out << "0 0 128 128\n2 0x4000 128 128\n3 0 128 128\nend 5\n"; }
            reset_shared_state();
            const auto &shared = chili::input_device_state();
            const auto untouched = [&] { return shared.device.load() == InputDevice::KeyboardMouse && shared.pad_kind.load() == PadKind::None; };
            Fixture f(chili::PcInputConfig::defaults());
            XINPUT_STATE pad{};
            pad.Gamepad.wButtons = XINPUT_GAMEPAD_B;
            f.pad = pad; f.input.scan_pads();
            pipeline_host = &f;
            {
                std::uint64_t poll_ms = 0, pulse_ms = 0;
                lc::Config live_config{root / "control.txt", root / "frame"};
                live_config.ack_file = root / "control.txt.ack";
                lc::DiagnosticLiveChannel live(live_config, [&] { return poll_ms; }, [](const std::string &) {}, [&] { return pulse_ms; });
                chili::InputReplay replay(replay_file);
                chili::InputRecorder recorder{std::filesystem::path{}};
                chili::InputPipeline pipeline(live, replay, recorder, true);
                const auto queries = f.pad_queries;
                // As the game does before each sample (main.cpp): the input layer is told who owns it.
                const auto take = [&] { f.input.set_scripted(pipeline.next_source() != InputSource::Host); return pipeline.sample(); };
                bool replayed = true;
                for (int i = 0; i < 5; ++i) {
                    const auto s = take();
                    replayed = replayed && pipeline.source() == InputSource::Replay && s.buttons == (i == 2 ? cross : 0u);
                }
                require(replayed && f.pad_queries == queries && f.input.report().samples == 0u, "the replay's five samples are the replay's: the held pad is not even read");
                require(untouched() && f.input.device() == InputDevice::KeyboardMouse, "a replay's samples leave the device state alone");
                { std::ofstream out(live_config.control_file); out << "1 tap 0x8000 3 128 128\n"; }
                poll_ms += 50;
                live.host_pump([](const std::filesystem::path &) { return true; });
                bool scripted = true;
                for (int i = 0; i < 3; ++i) {
                    const auto s = take();
                    scripted = scripted && pipeline.source() == InputSource::Live && s.buttons == square;
                }
                require(scripted && f.pad_queries == queries && untouched(), "and so do the samples a script injects");
                const auto mine = take();
                require(pipeline.source() == InputSource::Host && mine.buttons == circle && f.pad_queries == queries + 1u, "the next sample is the player's: the pad is read");
                require(shared.device.load() == InputDevice::Gamepad && shared.pad_kind.load() == PadKind::Xbox, "and it is the first to say which device is in use");
            }
            {
                // A deterministic run has no host input at all.
                reset_shared_state();
                lc::DiagnosticLiveChannel off({{}, {}});
                chili::InputReplay none{std::filesystem::path{}};
                chili::InputRecorder recorder{std::filesystem::path{}};
                chili::InputPipeline pipeline(off, none, recorder, false);
                const auto queries = f.pad_queries;
                const auto s = pipeline.sample();
                require(pipeline.source() == InputSource::Host && s.buttons == 0u && f.pad_queries == queries && untouched(),
                    "a deterministic run reads no host device and leaves the device state alone");
            }
            pipeline_host = nullptr;
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
        }
        // The settings of wave 6: [Input] Prompts, [Input.Gamepad] Glyphs, PlayStationPads and Rumble; what
        // configuring the input layer hands to the prompt pictures; a file as the launcher writes it; the ini of
        // scripted runs.
        {
            using chili::GlyphsSetting;
            using chili::PromptsSetting;
            const auto npos = std::string_view::npos;
            for (const auto scheme : {chili::InputScheme::Psp, chili::InputScheme::DualAnalog}) {
                const std::string_view text = chili::default_input_ini(scheme);
                require(text.find("\nPrompts=auto\n") != npos && text.find("\nGlyphs=auto\n") != npos && text.find("\nPlayStationPads=true\n") != npos &&
                        text.find("\nRumble=1\n") != npos && text.find("\nIndex=auto\n") != npos && text.find("; A section [Input.Gamepad.PlayStation]") != npos,
                    "the first-run file names the new settings");
                require(text.find("Triangle reload, Circle weapon wheel and back in menus") != npos && text.find("reload/back") == npos, "and says that Circle, not Triangle, goes back in menus");
                const auto config = chili::PcInputConfig::defaults(scheme);
                require(config.prompts == PromptsSetting::Auto && config.glyphs == GlyphsSetting::Auto && config.playstation_pads && config.rumble == 1.0f &&
                        config.playstation_named == 0u && config.pad_index == -1, "their defaults");
            }
            static constexpr std::pair<const char *, PromptsSetting> prompts[]{{"auto", PromptsSetting::Auto}, {"keyboard", PromptsSetting::Keyboard},
                {"gamepad", PromptsSetting::Gamepad}, {"psp", PromptsSetting::Psp}};
            static constexpr std::pair<const char *, GlyphsSetting> glyphs[]{{"auto", GlyphsSetting::Auto}, {"xbox", GlyphsSetting::Xbox}, {"playstation", GlyphsSetting::PlayStation}};
            auto &shared = chili::input_device_state();
            bool round_trip = true, published = true;
            for (const auto &[prompt_name, prompt] : prompts) for (const auto &[glyph_name, glyph] : glyphs) for (const bool pads : {true, false}) for (const float strength : {0.0f, 0.35f, 1.0f}) {
                std::ostringstream text;
                text << "[Input]\nPrompts=" << prompt_name << "\n[Input.Gamepad]\nGlyphs=" << glyph_name << "\nPlayStationPads=" << (pads ? "true" : "false") << "\nRumble=" << strength << "\n";
                auto config = chili::PcInputConfig::defaults();
                round_trip = round_trip && config.apply(text.str()).empty() && config.prompts == prompt && config.glyphs == glyph && config.playstation_pads == pads && config.rumble == strength;
                const auto revision = shared.bindings_revision.load();
                Fixture f(config);
                published = published && shared.prompts.load() == prompt && shared.glyphs.load() == glyph && shared.bindings_revision.load() == revision + 1u;
            }
            require(round_trip, "every value of the four settings is read back as it was written");
            require(published, "configuring the input layer hands Prompts and Glyphs to the prompt pictures and counts a load of the tables");
            auto config = chili::PcInputConfig::defaults();
            require(config.apply("[Input]\nprompts = PSP\n[Input.Gamepad]\nGLYPHS=PlayStation\nplaystationpads=0\nRumble = 0.25 ; a comment\n").empty() && config.prompts == PromptsSetting::Psp &&
                    config.glyphs == GlyphsSetting::PlayStation && !config.playstation_pads && config.rumble == 0.25f, "names and values in any case, 0 and 1 for a switch");
            const auto problems = config.apply("[Input]\nPrompts=sometimes\n[Input.Gamepad]\nGlyphs=nintendo\nPlayStationPads=maybe\nRumble=1.5\nRumble=-0.1\nRumble=loud\n");
            require(problems.size() == 6u && config.prompts == PromptsSetting::Psp && config.glyphs == GlyphsSetting::PlayStation && !config.playstation_pads && config.rumble == 0.25f,
                "bad values are reported and change nothing");
            require(problems[0].find("Prompts must be auto, keyboard, gamepad or psp") != std::string::npos && problems[1].find("Glyphs must be auto, xbox or playstation") != std::string::npos &&
                    problems[2].find("PlayStationPads must be true or false") != std::string::npos && problems[3].find("Rumble must be a number from 0 to 1") != std::string::npos,
                "and the message says what is allowed");
            require(config.apply("[Input.Gamepad.Nintendo]\nA=Cross\n").size() == 1u, "another section under [Input.Gamepad] is not known");

            // Through a file, as the game loads it.
            wchar_t temp[MAX_PATH]{};
            GetTempPathW(MAX_PATH, temp);
            const auto file = std::filesystem::path(temp) / ("chili-input-pads-" + std::to_string(GetCurrentProcessId()) + ".ini");
            { std::ofstream out(file, std::ios::binary | std::ios::trunc);
              out << "[Input]\nScheme=psp\nPrompts=gamepad\n\n[Input.Gamepad]\nGlyphs=xbox\nPlayStationPads=false\nRumble=0.5\nIndex=auto\n\n[Input.Gamepad.PlayStation]\nX=Circle\n"; }
            std::vector<std::string> file_problems;
            const auto loaded = chili::PcInputConfig::load(file, file_problems, false);
            std::filesystem::remove(file);
            require(file_problems.empty() && loaded.scheme == chili::InputScheme::Psp && loaded.prompts == PromptsSetting::Gamepad && loaded.glyphs == GlyphsSetting::Xbox &&
                    !loaded.playstation_pads && loaded.rumble == 0.5f && loaded.playstation_named == chili::PadSample::bit(chili::PadSample::X) &&
                    loaded.pad_target(chili::PadSample::X, chili::PadKind::PlayStation) == PadTarget::Circle && loaded.pad_buttons[chili::PadSample::X] == PadTarget::Square,
                "a file with the new settings, the rest by the file's scheme");

            // A file as the launcher writes it: every key of its lists (launcher/src/Model.cs: S, Actions.All, MouseInputs,
            // PadButtons), with the new ones of wave 6 and the PlayStation section, in both schemes.
            static constexpr const char *launcher_targets[]{"MoveForward", "MoveBack", "MoveLeft", "MoveRight", "Walk", "Square", "R", "Cross", "Triangle", "Circle", "L",
                "DpadLeft", "DpadRight", "DpadDown", "DpadUp", "Start", "Select", "LookLeft", "LookRight", "LookUp", "LookDown"};
            static constexpr const char *launcher_buttons[]{"A", "B", "X", "Y", "LB", "RB", "LT", "RT", "LS", "RS", "Back", "Start", "DpadUp", "DpadDown", "DpadLeft", "DpadRight"};
            for (const char *scheme : {"dual_analog", "psp"}) {
                std::ostringstream text;
                text << "[Input]\nScheme=" << scheme << "\nExactTurn=true\nBackgroundInput=false\nShowInput=false\nVersion=2\nPrompts=auto\n\n[Input.Keyboard]\n";
                for (const char *target : launcher_targets) text << target << "=F5,Numpad1\n";
                text << "ToggleMouseCapture=F1\nToggleInputDisplay=\nWalkScale=0.6\nLookKeySpeed=0.7\n\n"
                        "[Input.Mouse]\nEnabled=true\nCapture=click\nMenuClicks=true\nMouse1=Square\nMouse2=R\nMouse3=Circle\nMouse4=None\nMouse5=None\nWheelUp=DpadRight\nWheelDown=DpadLeft\n"
                        "SensitivityX=0.08\nSensitivityY=0.08\nInvertX=false\nInvertY=false\nHoldPitch=true\nExact=true\nSmoothing=0\n\n[Input.Gamepad]\nEnabled=true\nIndex=auto\n";
                for (const char *button : launcher_buttons) text << button << "=Cross\n";
                text << "DeadzoneLeft=0.24\nDeadzoneRight=0.24\nTriggerThreshold=0.12\nCompensateEngineDeadzone=true\nTurnSensitivity=1.0\nLookSensitivity=1.0\nInvertY=false\n"
                        "Glyphs=auto\nPlayStationPads=true\nRumble=1\n\n[Input.Gamepad.PlayStation]\n";
                for (const char *button : launcher_buttons) text << button << "=Circle\n";
                text << "\n[Rendering]\nWindowSize=1280x720\n\n[Launcher]\nAnything=1\n";
                chili::PcInputConfig written;
                const auto unknown = written.apply(text.str());
                require(unknown.empty(), "a file the launcher writes has no line the game does not know");
                require(written.playstation_named == 0xFFFFu && written.pad_target(chili::PadSample::RT, chili::PadKind::PlayStation) == PadTarget::Circle &&
                        written.pad_target(chili::PadSample::RT, chili::PadKind::Xbox) == PadTarget::Cross && written.toggle_display_vk == 0, "and all of it is taken");
            }
            // What a build that knows the new settings holds as text: the launcher looks for these words in the executable.
            const std::string_view first_run = chili::default_input_ini();
            for (const char *word : {"Prompts", "Glyphs", "PlayStationPads", "Rumble"}) require(first_run.find(std::string("\n") + word + "=") != npos, "the setting's name is in the program");

            // scripts\unattended.ini and replays\mouse.ini: the game's own PSP pictures, and no line the game does not know.
            const auto profile = std::filesystem::path(__FILE__).parent_path().parent_path();
            for (const auto &name : {profile / "scripts" / "unattended.ini", profile / "replays" / "mouse.ini"}) {
                if (!std::filesystem::exists(name)) continue;
                std::vector<std::string> ini_problems;
                const auto scripted = chili::PcInputConfig::load(name, ini_problems, false);
                require(ini_problems.empty() && scripted.prompts == PromptsSetting::Psp, "a scripted run's ini pins Prompts=psp and has no line the game does not know");
            }
        }
        // Vibration: the motors of the pad in use, scaled by Rumble, sent only when they change, stopped when the pad
        // is not the player's; the game's own call by motor.
        {
            const auto motor = [](float value) { return static_cast<std::uint16_t>(std::lround(value * 65535.0f)); };
            Fixture f(chili::PcInputConfig::defaults());
            f.input.rumble(0.5f, 1.0f);
            require(f.motors.empty(), "no pad, nothing to send");
            XINPUT_STATE pad{};
            f.pad = pad; f.input.scan_pads();
            (void)f.sample();
            require(f.motors.size() == 1u && f.motors[0].pad == 1u && f.motors[0].slow == 32768u && f.motors[0].fast == 65535u, "the pad in use gets the motors' values at the next sample");
            (void)f.sample();
            f.input.rumble(0.5f, 1.0f);
            require(f.motors.size() == 1u, "the same values are not sent again");
            f.input.rumble(2.0f, -1.0f);
            require(f.motors.size() == 2u && f.motors[1].slow == 65535u && f.motors[1].fast == 0u, "values are held to 0 to 1");
            f.focused = false;
            (void)f.sample();
            require(f.motors.size() == 3u && f.motors[2].slow == 0u && f.motors[2].fast == 0u, "another window has the focus: the motors stop");
            f.focused = true;
            (void)f.sample();
            require(f.motors.size() == 4u && f.motors[3].slow == 65535u, "and run again when the focus is back");
            f.input.set_scripted(true);
            require(f.motors.size() == 5u && f.motors[4].slow == 0u, "a replay or a script takes the input: the motors stop");
            f.input.rumble(1.0f, 0.0f);
            require(f.motors.size() == 5u, "and stay stopped while it has it");
            f.input.set_scripted(false);
            (void)f.sample();
            require(f.motors.size() == 6u && f.motors[5].slow == 65535u, "they run again when the player has the input");
            XINPUT_STATE other{};
            other.Gamepad.wButtons = XINPUT_GAMEPAD_A;
            f.slots[3] = other;
            f.input.scan_pads();
            (void)f.sample();
            require(f.motors.size() == 8u && f.motors[6].pad == 1u && f.motors[6].slow == 0u && f.motors[7].pad == 3u && f.motors[7].slow == 65535u,
                "another pad becomes the pad in use: the old one stops, the new one takes the values");
            f.input.release();
            require(f.motors.size() == 9u && f.motors.back().pad == 3u && f.motors.back().slow == 0u && f.motors.back().fast == 0u, "the game ends: the motors stop");
            require(f.input.report().vibration_sent == f.motors.size(), "every setting sent to a pad is counted");

            auto half = chili::PcInputConfig::defaults();
            require(half.apply("[Input.Gamepad]\nRumble=0.5\n").empty(), "Rumble option");
            Fixture g(half);
            g.pad = pad; g.input.scan_pads();
            (void)g.sample();
            g.input.rumble(1.0f, 0.5f);
            require(g.motors.size() == 1u && g.motors[0].slow == 32768u && g.motors[0].fast == 16384u, "Rumble scales both motors");
            auto off = chili::PcInputConfig::defaults();
            require(off.apply("[Input.Gamepad]\nRumble=0\n").empty(), "Rumble off");
            Fixture h(off);
            h.pad = pad; h.input.scan_pads();
            (void)h.sample();
            h.input.rumble(1.0f, 1.0f);
            h.input.game_vibration(1u, 1.0f);
            require(h.motors.empty() && h.input.report().game_vibration_calls == 1u, "Rumble=0: nothing is sent, and the game's call is still counted");

            // The game's own call (the hook over its empty pad function): motor 1 is taken as the large motor, 0 as the small one.
            Fixture game(chili::PcInputConfig::defaults());
            game.pad = pad; game.input.scan_pads();
            (void)game.sample();
            game.input.game_vibration(1u, 0.8f);
            require(game.motors.size() == 1u && game.motors[0].slow == motor(0.8f) && game.motors[0].fast == 0u, "the game's motor 1 runs the large motor");
            game.input.game_vibration(0u, 1.0f);
            require(game.motors.size() == 2u && game.motors[1].slow == motor(0.8f) && game.motors[1].fast == 65535u, "its motor 0 the small one, the other keeps its value");
            game.input.game_vibration(7u, 1.0f);
            require(game.motors.size() == 2u, "a motor the pad has not got changes nothing");
            game.input.game_vibration(1u, 0.0f);
            game.input.game_vibration(0u, 0.0f);
            require(game.motors.back().slow == 0u && game.motors.back().fast == 0u && game.input.report().game_vibration_calls == 5u, "both off; five calls counted");
            require(game.logged("vibration: the game set motor 1 to 0.8") && game.logged("vibration: the game set motor 7 to 1"), "the first calls are logged as they come");
            require(chili::input_summary(game.input).find("[input] vibration: calls by the game=5, settings sent to a pad=4") != std::string::npos, "and the exit summary has the counts");

            // A PlayStation pad (made up: no device behind it, so its values come out of the host's function as well).
            Fixture sony(chili::PcInputConfig::defaults());
            (void)sony.input.hid_arrived(3u, playstation_pad());
            (void)sony.sample();
            sony.input.rumble(1.0f, 0.25f);
            require(sony.motors.size() == 1u && sony.motors[0].pad == 4u && sony.motors[0].slow == 65535u && sony.motors[0].fast == motor(0.25f), "a PlayStation pad in use gets the values");
            sony.input.hid_removed(3u);
            sony.input.rumble(0.0f, 0.0f);
            require(sony.motors.size() == 1u, "a pad that is gone gets nothing");
        }
        // The Windows side of the HID reader, on whatever HID devices this PC lists (mice, keyboards, pads of any
        // make, taken through the diagnostic switch of HidPadDevice::open): each device's description is read with
        // the system's parser, and a report of zeros goes through it without harm. No PlayStation pad is needed
        // for this and none was ever there; on a PC without HID devices the block does nothing.
        {
            UINT count = 0u;
            (void)GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST));
            std::vector<RAWINPUTDEVICELIST> list(count);
            const UINT listed = count ? GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST)) : 0u;
            unsigned devices = 0u, described = 0u, parsed = 0u, pads = 0u;
            for (UINT i = 0; listed != static_cast<UINT>(-1) && i < listed; ++i) {
                if (list[i].dwType != RIM_TYPEHID) continue;
                ++devices;
                chili::HidPadDevice::Refused refused{};
                std::string note;
                auto device = chili::HidPadDevice::open(list[i].hDevice, refused, note, true);
                if (device) {
                    ++described;
                    require(refused == chili::HidPadDevice::Refused::No && !device->caps_text().empty() && device->ids().size() == 9u, "a HID device is described");
                    std::vector<std::uint8_t> zeros(std::max<std::size_t>(device->input_report_bytes(), 1u), std::uint8_t{0u});
                    zeros[0] = device->first_report_id();
                    chili::HidPadReading reading;
                    if (device->read(zeros.data(), zeros.size(), reading)) {
                        ++parsed;
                        require(std::fabs(reading.x) <= 1.0f && std::fabs(reading.y) <= 1.0f && std::fabs(reading.z) <= 1.0f && std::fabs(reading.rz) <= 1.0f &&
                                reading.rx >= 0.0f && reading.rx <= 1.0f && reading.ry >= 0.0f && reading.ry <= 1.0f && reading.hat <= 8u, "a report of zeros reads as values in range");
                    }
                }
                // As the game looks at it: only a PlayStation pad is taken, and a refusal has its reason.
                chili::HidPadDevice::Refused strict{};
                std::string strict_note;
                const auto pad = chili::HidPadDevice::open(list[i].hDevice, strict, strict_note);
                if (pad) ++pads;
                require(pad ? strict == chili::HidPadDevice::Refused::No && pad->info().vid == chili::hid_sony_vendor : strict != chili::HidPadDevice::Refused::No,
                    "a device is taken as a PlayStation pad or refused with a reason");
                require(strict != chili::HidPadDevice::Refused::NotSony || strict_note.find("ignored (not a PlayStation pad") != std::string::npos, "a pad of another make gets its log line");
            }
            std::cout << "[hid] this PC lists " << devices << " HID devices: " << described << " described by the system's parser, " << parsed
                      << " whose report of zeros it read as a stick's, " << pads << " PlayStation pads\n";
        }
        // For the record, not a check: what the pad finder's look costs on this PC (the real XInput, all four
        // slots, as its worker asks them every two seconds), and how many slots answer.
        {
            LARGE_INTEGER frequency{}, before{}, after{};
            QueryPerformanceFrequency(&frequency);
            unsigned answering = 0u;
            double longest = 0.0, total = 0.0;
            for (int round = 0; round < 10; ++round) {
                QueryPerformanceCounter(&before);
                for (DWORD slot = 0; slot < 4u; ++slot) { XINPUT_STATE state{}; if (XInputGetState(slot, &state) == ERROR_SUCCESS && round == 0) ++answering; }
                QueryPerformanceCounter(&after);
                const double ms = static_cast<double>(after.QuadPart - before.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
                longest = std::max(longest, ms); total += ms;
            }
            char line[160];
            std::snprintf(line, sizeof(line), "[xinput] this PC: %u of 4 slots answer; one look at all four takes %.3f ms on average, %.3f ms at most (10 looks)", answering, total / 10.0, longest);
            std::cout << line << '\n';
        }
        if (failures) { std::cerr << "FAIL: " << failures << " of " << checks << " checks\n"; return 1; }
        std::cout << "PASS " << checks << " checks\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
