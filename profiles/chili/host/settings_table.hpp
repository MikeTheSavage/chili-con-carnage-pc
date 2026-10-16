#pragma once
// The settings of ChiliNative.ini as the game's own settings page knows them: one entry per key the
// launcher knows (launcher/src/Model.cs), in the launcher's order, each with its kind, the values it may
// take and the word the page shows for each, its short name on the page, its default, and its class.
// tools/settings_table_test.cpp reads the launcher's Model.cs and Pages.cs and fails when the two differ
// (a key, a value, a default, a slider's range), so the game's page and the launcher cannot drift apart.
//
// The page's rows, in the order they are shown, are `rows` below. What a key does when it changes, and its value while the game runs, is settings_apply.hpp;
// nothing here needs the game.
//
// Class:
//   Live      changes at the moment the value changes
//   Moderate  can change while the game runs with more work (sound buffer, texture dump, PlayStation pads)
//   Split     by value: [Rendering] AntiAliasing (off and fxaa while the game runs, multisampling with a
//             restart), [Mods] Enabled (a pack that is only textures while it runs, file mods with a restart)
//   Restart   takes a restart of the game
//   Off       not a key for a page in the game (a path, a spelling of another key, the file's version)
// A key whose page is None is not shown by the game's page; the launcher has it.
#include "input_layout.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace chili::settings {
enum class Kind : std::uint8_t { Switch, Choice, Slider, Binding, Text };
enum class Class : std::uint8_t { Live, Moderate, Split, Restart, Off };
enum class Page : std::uint8_t { None, Top, Display, Graphics, Controls, KeyboardMouse, Gamepad, TexturesMods, Sound };
// How a slider's number is shown: 0.08, 60 %, 70 ms.
enum class Show : std::uint8_t { Number, Percent, Milliseconds };

struct Value { std::string_view ini, word; };
struct Setting {
    std::string_view section, key;
    Kind kind{};
    Class cls{};
    Page page{};                  // None: not on the game's page
    std::string_view name;        // the row's name on the page; empty when not shown
    std::string_view fallback;    // the game's default (with the control style Total Overdose PC)
    const char *fallback_modern{};   // the default with Layout=modern where it differs, else null
    const char *fallback_psp{};   // the default with Layout=psp where it differs from Modern's, else null
    std::span<const Value> values;     // Choice and Switch: what the file holds and the word shown for it
    double min{}, max{}, step{};       // Slider
    Show show{};
    std::string_view environment;      // the variable that overrides the key for scripts; empty when none does

    std::string id() const { return std::string(section) + "/" + std::string(key); }
    constexpr bool shown() const noexcept { return page != Page::None; }
    // The default with a control style: PSP original takes Modern's where it has none of its own.
    constexpr std::string_view default_for(InputLayout layout) const noexcept {
        if (layout == InputLayout::Psp && fallback_psp) return fallback_psp;
        if (layout != InputLayout::TodPc && fallback_modern) return fallback_modern;
        return fallback;
    }
};

namespace words {
inline constexpr Value on_off[]{{"false", "off"}, {"true", "on"}};
inline constexpr Value screen[]{{"false", "Window"}, {"true", "Full screen"}};
inline constexpr Value resolution[]{{"auto", "Auto"}, {"1", "480 x 272"}, {"2", "960 x 544"}, {"3", "1440 x 816"}, {"4", "1920 x 1088"}, {"6", "2880 x 1632"}, {"8", "3840 x 2176"}};
inline constexpr Value auto_limit[]{{"high", "No limit"}, {"medium", "1920 x 1088"}, {"low", "960 x 544"}};
inline constexpr Value anti_aliasing[]{{"off", "Off"}, {"fxaa", "Light"}, {"msaa2", "2x"}, {"msaa4", "4x"}, {"msaa8", "8x"}};
inline constexpr Value msaa[]{{"1", "Off"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}};
inline constexpr Value texture_filter[]{{"game", "Original"}, {"linear", "Smooth"}, {"nearest", "Blocky"}};
inline constexpr Value anisotropy[]{{"1", "Off"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}, {"16", "16x"}};
inline constexpr Value ambient[]{{"off", "Off"}, {"low", "Low"}, {"high", "High"}};
inline constexpr Value glow[]{{"smooth", "Smooth"}, {"game", "Original"}, {"off", "Off"}};
inline constexpr Value renderer[]{{"d3d12", "Graphics card"}, {"software", "Software"}};
inline constexpr Value frame_rate[]{{"2", "30 fps"}, {"1", "60 fps"}};
inline constexpr Value camera[]{{"tight", "Responsive"}, {"original", "Original"}};
inline constexpr Value priority[]{{"normal", "Normal"}, {"high", "High"}};
inline constexpr Value control_style[]{{"tod_pc", "Total Overdose PC"}, {"modern", "Modern"}, {"psp", "PSP original"}};
inline constexpr Value prompts[]{{"auto", "Automatic"}, {"keyboard", "Keyboard"}, {"gamepad", "Gamepad"}, {"psp", "PSP original"}};
inline constexpr Value grab[]{{"click", "On click"}, {"never", "Never"}};
inline constexpr Value which_pad[]{{"auto", "Automatic"}, {"0", "1"}, {"1", "2"}, {"2", "3"}, {"3", "4"}};
inline constexpr Value button_pictures[]{{"auto", "Automatic"}, {"xbox", "Xbox"}, {"playstation", "PlayStation"}};
}

namespace detail {
constexpr Setting choice(std::string_view section, std::string_view key, Class cls, Page page, std::string_view name, std::string_view fallback,
                         std::span<const Value> values, std::string_view environment = {}, const char *fallback_psp = nullptr) {
    return Setting{section, key, Kind::Choice, cls, page, name, fallback, nullptr, fallback_psp, values, 0.0, 0.0, 0.0, Show::Number, environment};
}
constexpr Setting toggle(std::string_view section, std::string_view key, Class cls, Page page, std::string_view name, std::string_view fallback, std::string_view environment = {}) {
    return Setting{section, key, Kind::Switch, cls, page, name, fallback, nullptr, nullptr, words::on_off, 0.0, 0.0, 0.0, Show::Number, environment};
}
constexpr Setting slider(std::string_view section, std::string_view key, Class cls, Page page, std::string_view name, std::string_view fallback,
                         double min, double max, double step, Show show, std::string_view environment = {}) {
    return Setting{section, key, Kind::Slider, cls, page, name, fallback, nullptr, nullptr, {}, min, max, step, show, environment};
}
// `fallback`: Total Overdose PC's; `fallback_modern`: Modern's where it differs; `fallback_psp`: PSP original's where it differs from Modern's.
constexpr Setting binding(std::string_view section, std::string_view key, std::string_view fallback, const char *fallback_modern = nullptr, const char *fallback_psp = nullptr) {
    return Setting{section, key, Kind::Binding, Class::Live, Page::None, {}, fallback, fallback_modern, fallback_psp, {}, 0.0, 0.0, 0.0, Show::Number, {}};
}
constexpr Setting text(std::string_view section, std::string_view key, Class cls, std::string_view fallback, std::string_view environment = {}) {
    return Setting{section, key, Kind::Text, cls, Page::None, {}, fallback, nullptr, nullptr, {}, 0.0, 0.0, 0.0, Show::Number, environment};
}
}

// Every key of launcher/src/Model.cs, in its order: the settings first, then the bindings (the keys of an
// action, the mouse's inputs, a pad's buttons).
inline constexpr std::array<Setting, 99> table{{
    // [Rendering]
    detail::choice("Rendering", "Fullscreen", Class::Live, Page::Display, "Screen", "false", words::screen),
    detail::text("Rendering", "WindowSize", Class::Off, ""),
    detail::choice("Rendering", "InternalScale", Class::Restart, Page::Display, "Resolution", "auto", words::resolution, "CHILI_RENDER_SCALE"),
    detail::toggle("Rendering", "EarlyPresent", Class::Live, Page::Display, "Show frames early", "true", "CHILI_RENDER_EARLY_PRESENT"),
    detail::choice("Rendering", "Preset", Class::Restart, Page::Display, "Auto limit", "high", words::auto_limit, "CHILI_RENDER_PRESET"),
    detail::choice("Rendering", "AntiAliasing", Class::Split, Page::Graphics, "Anti-aliasing", "fxaa", words::anti_aliasing, "CHILI_RENDER_AA"),
    detail::choice("Rendering", "MSAA", Class::Off, Page::None, "", "1", words::msaa, "CHILI_RENDER_MSAA"),
    detail::choice("Rendering", "TextureFilter", Class::Live, Page::Graphics, "Texture filter", "game", words::texture_filter, "CHILI_RENDER_FILTER"),
    detail::choice("Rendering", "Anisotropy", Class::Live, Page::Graphics, "Sharp textures", "16", words::anisotropy, "CHILI_RENDER_ANISOTROPY"),
    detail::choice("Rendering", "SSAO", Class::Restart, Page::Graphics, "Ambient shadows", "off", words::ambient, "CHILI_RENDER_SSAO"),
    detail::choice("Rendering", "Glow", Class::Live, Page::Graphics, "Glow", "smooth", words::glow, "CHILI_RENDER_GLOW"),
    detail::choice("Rendering", "Backend", Class::Restart, Page::None, "", "d3d12", words::renderer, "CHILI_RENDER_BACKEND"),
    detail::toggle("Rendering", "GeThread", Class::Live, Page::None, "", "true", "CHILI_RENDER_GE_THREAD"),
    // [Game]
    detail::choice("Game", "FrameLock", Class::Restart, Page::Display, "Frame rate", "1", words::frame_rate),
    detail::choice("Game", "Camera", Class::Restart, Page::Controls, "Camera", "tight", words::camera, {}, "original"),
    detail::choice("Game", "Priority", Class::Live, Page::None, "", "high", words::priority, "CHILI_GAME_PRIORITY"),
    // [Audio]
    detail::slider("Audio", "BufferMs", Class::Moderate, Page::Sound, "Sound buffer", "70", 20.0, 500.0, 10.0, Show::Milliseconds, "CHILI_AUDIO_BUFFER_MS"),
    // [Mods]
    detail::text("Mods", "Folder", Class::Off, "mods", "CHILI_MODS_DIR"),
    detail::text("Mods", "Enabled", Class::Split, ""),
    detail::toggle("Mods", "DumpTextures", Class::Moderate, Page::TexturesMods, "Dump textures", "false", "CHILI_TEXTURE_DUMP"),
    detail::toggle("Mods", "LogFiles", Class::Off, Page::None, "", "false"),
    // [Disc]
    detail::text("Disc", "Path", Class::Off, ""),
    // [Input]
    // The control style (input_layout.hpp); the scheme goes with it and is not a row of its own.
    detail::choice("Input", "Layout", Class::Restart, Page::Controls, "Style", "tod_pc", words::control_style),
    detail::text("Input", "Scheme", Class::Off, "dual_analog"),
    detail::toggle("Input", "ExactTurn", Class::Live, Page::Gamepad, "Precise turning", "true"),
    detail::toggle("Input", "BackgroundInput", Class::Live, Page::None, "", "false"),
    detail::toggle("Input", "ShowInput", Class::Live, Page::None, "", "false"),
    detail::text("Input", "Version", Class::Off, "3"),
    detail::choice("Input", "Prompts", Class::Live, Page::Controls, "Prompts", "auto", words::prompts),
    // [Input.Keyboard]
    detail::slider("Input.Keyboard", "WalkScale", Class::Live, Page::KeyboardMouse, "Walk speed", "0.6", 0.31, 1.0, 0.01, Show::Percent),
    detail::slider("Input.Keyboard", "LookKeySpeed", Class::Live, Page::KeyboardMouse, "Look key speed", "0.7", 0.05, 1.0, 0.05, Show::Percent),
    // [Input.Mouse]
    detail::toggle("Input.Mouse", "Enabled", Class::Live, Page::KeyboardMouse, "Use the mouse", "true"),
    detail::choice("Input.Mouse", "Capture", Class::Live, Page::KeyboardMouse, "Grab the mouse", "click", words::grab),
    detail::toggle("Input.Mouse", "MenuClicks", Class::Live, Page::KeyboardMouse, "Clicks in menus", "true"),
    detail::slider("Input.Mouse", "SensitivityX", Class::Live, Page::KeyboardMouse, "Turn speed", "0.08", 0.01, 1.0, 0.01, Show::Number),
    detail::slider("Input.Mouse", "SensitivityY", Class::Live, Page::KeyboardMouse, "Look speed", "0.08", 0.01, 1.0, 0.01, Show::Number),
    detail::toggle("Input.Mouse", "InvertX", Class::Live, Page::KeyboardMouse, "Invert left/right", "false"),
    detail::toggle("Input.Mouse", "InvertY", Class::Live, Page::KeyboardMouse, "Invert up/down", "false"),
    detail::toggle("Input.Mouse", "HoldPitch", Class::Live, Page::KeyboardMouse, "Hold the aim", "true"),
    detail::toggle("Input.Mouse", "Exact", Class::Live, Page::KeyboardMouse, "Precise aim", "true"),
    detail::slider("Input.Mouse", "Smoothing", Class::Live, Page::KeyboardMouse, "Smoothing", "0", 0.0, 0.95, 0.05, Show::Number),
    // [Input.Gamepad]
    detail::toggle("Input.Gamepad", "Enabled", Class::Live, Page::Gamepad, "Use the gamepad", "true"),
    detail::choice("Input.Gamepad", "Index", Class::Live, Page::Gamepad, "Which controller", "auto", words::which_pad),
    detail::slider("Input.Gamepad", "DeadzoneLeft", Class::Live, Page::Gamepad, "Left dead zone", "0.24", 0.0, 0.9, 0.01, Show::Percent),
    detail::slider("Input.Gamepad", "DeadzoneRight", Class::Live, Page::Gamepad, "Right dead zone", "0.24", 0.0, 0.9, 0.01, Show::Percent),
    detail::slider("Input.Gamepad", "TriggerThreshold", Class::Live, Page::Gamepad, "Trigger pull", "0.12", 0.01, 1.0, 0.01, Show::Percent),
    detail::toggle("Input.Gamepad", "CompensateEngineDeadzone", Class::Live, Page::Gamepad, "Move at the edge", "true"),
    detail::slider("Input.Gamepad", "TurnSensitivity", Class::Live, Page::Gamepad, "Turn speed", "1.0", 0.0, 4.0, 0.05, Show::Number),
    detail::slider("Input.Gamepad", "LookSensitivity", Class::Live, Page::Gamepad, "Look speed", "1.0", 0.0, 4.0, 0.05, Show::Number),
    detail::toggle("Input.Gamepad", "InvertY", Class::Live, Page::Gamepad, "Invert up/down", "false"),
    detail::choice("Input.Gamepad", "Glyphs", Class::Live, Page::Controls, "Button pictures", "auto", words::button_pictures),
    detail::toggle("Input.Gamepad", "PlayStationPads", Class::Moderate, Page::Gamepad, "PlayStation pads", "true"),
    detail::slider("Input.Gamepad", "Rumble", Class::Live, Page::Gamepad, "Rumble", "1", 0.0, 1.0, 0.05, Show::Percent),
    // The keys of an action ([Input.Keyboard] Target=key[,key]), in the launcher's order of actions: Total
    // Overdose PC's, then Modern's where they differ (PSP original has Modern's, without the look keys).
    detail::binding("Input.Keyboard", "MoveForward", "W"), detail::binding("Input.Keyboard", "MoveBack", "S"),
    detail::binding("Input.Keyboard", "MoveLeft", "A"), detail::binding("Input.Keyboard", "MoveRight", "D"),
    detail::binding("Input.Keyboard", "Walk", "X"), detail::binding("Input.Keyboard", "Square", "G"),
    detail::binding("Input.Keyboard", "R", "V"), detail::binding("Input.Keyboard", "Cross", "Space,F,Enter,LShift", "Space,E,Enter"),
    detail::binding("Input.Keyboard", "Triangle", "R,LCtrl", "R"), detail::binding("Input.Keyboard", "Circle", "E,Backspace", "F,Backspace"),
    detail::binding("Input.Keyboard", "L", "Q,C", "Q"), detail::binding("Input.Keyboard", "DpadLeft", "Left", "Left,1"),
    detail::binding("Input.Keyboard", "DpadRight", "Right", "Right,2"), detail::binding("Input.Keyboard", "DpadDown", "Tab,Down,PageDown", "Down,Z"),
    detail::binding("Input.Keyboard", "DpadUp", "Up,PageUp", "Up"), detail::binding("Input.Keyboard", "Start", "Escape,P", "Escape"),
    detail::binding("Input.Keyboard", "Select", "Grave", "Tab"),
    detail::binding("Input.Keyboard", "LookLeft", "J", nullptr, ""), detail::binding("Input.Keyboard", "LookRight", "L", nullptr, ""),
    detail::binding("Input.Keyboard", "LookUp", "I", nullptr, ""), detail::binding("Input.Keyboard", "LookDown", "K", nullptr, ""),
    detail::binding("Input.Keyboard", "ToggleMouseCapture", "F1"), detail::binding("Input.Keyboard", "ToggleInputDisplay", "F3"),
    // The mouse's inputs ([Input.Mouse] Mouse1..5, WheelUp, WheelDown = Target).
    detail::binding("Input.Mouse", "Mouse1", "Square"), detail::binding("Input.Mouse", "Mouse2", "R"),
    detail::binding("Input.Mouse", "Mouse3", "Circle"), detail::binding("Input.Mouse", "Mouse4", "None"),
    detail::binding("Input.Mouse", "Mouse5", "None"), detail::binding("Input.Mouse", "WheelUp", "DpadRight"),
    detail::binding("Input.Mouse", "WheelDown", "DpadLeft"),
    // A pad's buttons ([Input.Gamepad] Button=Target).
    detail::binding("Input.Gamepad", "A", "Cross"), detail::binding("Input.Gamepad", "B", "Circle"),
    detail::binding("Input.Gamepad", "X", "Triangle", nullptr, "Square"), detail::binding("Input.Gamepad", "Y", "Cross", nullptr, "Triangle"),
    detail::binding("Input.Gamepad", "LB", "DpadLeft", nullptr, "L"), detail::binding("Input.Gamepad", "RB", "DpadRight", nullptr, "R"),
    detail::binding("Input.Gamepad", "LT", "R"), detail::binding("Input.Gamepad", "RT", "Square"),
    detail::binding("Input.Gamepad", "LS", "L", nullptr, "None"), detail::binding("Input.Gamepad", "RS", "None"),
    detail::binding("Input.Gamepad", "Back", "Select"), detail::binding("Input.Gamepad", "Start", "Start"),
    detail::binding("Input.Gamepad", "DpadUp", "DpadUp"), detail::binding("Input.Gamepad", "DpadDown", "DpadDown"),
    detail::binding("Input.Gamepad", "DpadLeft", "DpadLeft"), detail::binding("Input.Gamepad", "DpadRight", "DpadRight"),
}};

// A to Z without case: how every reader of the file compares names.
constexpr bool same_name(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char x = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] - 'A' + 'a') : a[i], y = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
        if (x != y) return false;
    }
    return true;
}
constexpr const Setting *find(std::string_view section, std::string_view key) noexcept {
    for (const auto &setting : table) if (same_name(setting.section, section) && same_name(setting.key, key)) return &setting;
    return nullptr;
}
// "Section/Key".
constexpr const Setting *find(std::string_view id) noexcept {
    const auto slash = id.find('/');
    return slash == std::string_view::npos ? nullptr : find(id.substr(0u, slash), id.substr(slash + 1u));
}
// The key, which must be in the table (a wrong name does not compile where this is a constant).
constexpr const Setting &get(std::string_view section, std::string_view key) {
    const auto *found = find(section, key);
    if (!found) std::abort();
    return *found;
}

// ---- the page's rows -------------------------------------------------------------------------------

// A row of a page. Setting: a key of the table. Link: opens another page. Quality, Textures and HdPack are
// rows over more than one key (settings_apply.hpp works them). Defaults: the last row of a section's page.
enum class RowKind : std::uint8_t { Setting, Link, Quality, Textures, HdPack, Defaults };
struct Row {
    Page page{};
    RowKind kind{};
    std::string_view name;         // what the row is called (a Setting row: the setting's name)
    const Setting *setting{};      // Setting rows
    Page target{};                 // Link rows
};
namespace detail {
constexpr Row key(Page page, std::string_view section, std::string_view name) { const Setting &s = get(section, name); return Row{page, RowKind::Setting, s.name, &s, Page::None}; }
constexpr Row link(Page page, std::string_view name, Page target) { return Row{page, RowKind::Link, name, nullptr, target}; }
constexpr Row other(Page page, RowKind kind, std::string_view name) { return Row{page, kind, name, nullptr, Page::None}; }
}
inline constexpr Row rows[]{
    detail::link(Page::Top, "Display", Page::Display),
    detail::link(Page::Top, "Graphics", Page::Graphics),
    detail::link(Page::Top, "Controls", Page::Controls),
    detail::link(Page::Top, "Textures & mods", Page::TexturesMods),
    detail::link(Page::Top, "Sound", Page::Sound),

    detail::key(Page::Display, "Rendering", "Fullscreen"),
    detail::key(Page::Display, "Rendering", "InternalScale"),
    detail::key(Page::Display, "Game", "FrameLock"),
    detail::key(Page::Display, "Rendering", "EarlyPresent"),
    detail::key(Page::Display, "Rendering", "Preset"),
    detail::other(Page::Display, RowKind::Defaults, "Defaults"),

    detail::other(Page::Graphics, RowKind::Quality, "Quality"),
    detail::key(Page::Graphics, "Rendering", "AntiAliasing"),
    detail::key(Page::Graphics, "Rendering", "TextureFilter"),
    detail::key(Page::Graphics, "Rendering", "Anisotropy"),
    detail::key(Page::Graphics, "Rendering", "SSAO"),
    detail::key(Page::Graphics, "Rendering", "Glow"),
    detail::other(Page::Graphics, RowKind::Defaults, "Defaults"),

    detail::key(Page::Controls, "Input", "Layout"),
    detail::key(Page::Controls, "Game", "Camera"),
    detail::link(Page::Controls, "Keyboard & mouse", Page::KeyboardMouse),
    detail::link(Page::Controls, "Gamepad", Page::Gamepad),
    detail::key(Page::Controls, "Input", "Prompts"),
    detail::key(Page::Controls, "Input.Gamepad", "Glyphs"),
    detail::other(Page::Controls, RowKind::Defaults, "Defaults"),

    detail::key(Page::KeyboardMouse, "Input.Mouse", "SensitivityX"),
    detail::key(Page::KeyboardMouse, "Input.Mouse", "SensitivityY"),
    detail::key(Page::KeyboardMouse, "Input.Mouse", "InvertX"),
    detail::key(Page::KeyboardMouse, "Input.Mouse", "InvertY"),
    detail::key(Page::KeyboardMouse, "Input.Mouse", "HoldPitch"),
    detail::key(Page::KeyboardMouse, "Input.Mouse", "Exact"),
    detail::key(Page::KeyboardMouse, "Input.Mouse", "Smoothing"),
    detail::key(Page::KeyboardMouse, "Input.Mouse", "Capture"),
    detail::key(Page::KeyboardMouse, "Input.Mouse", "MenuClicks"),
    detail::key(Page::KeyboardMouse, "Input.Mouse", "Enabled"),
    detail::key(Page::KeyboardMouse, "Input.Keyboard", "WalkScale"),
    detail::key(Page::KeyboardMouse, "Input.Keyboard", "LookKeySpeed"),
    detail::other(Page::KeyboardMouse, RowKind::Defaults, "Defaults"),

    detail::key(Page::Gamepad, "Input.Gamepad", "TurnSensitivity"),
    detail::key(Page::Gamepad, "Input.Gamepad", "LookSensitivity"),
    detail::key(Page::Gamepad, "Input.Gamepad", "InvertY"),
    detail::key(Page::Gamepad, "Input", "ExactTurn"),
    detail::key(Page::Gamepad, "Input.Gamepad", "DeadzoneLeft"),
    detail::key(Page::Gamepad, "Input.Gamepad", "DeadzoneRight"),
    detail::key(Page::Gamepad, "Input.Gamepad", "TriggerThreshold"),
    detail::key(Page::Gamepad, "Input.Gamepad", "CompensateEngineDeadzone"),
    detail::key(Page::Gamepad, "Input.Gamepad", "Index"),
    detail::key(Page::Gamepad, "Input.Gamepad", "PlayStationPads"),
    detail::key(Page::Gamepad, "Input.Gamepad", "Rumble"),
    detail::key(Page::Gamepad, "Input.Gamepad", "Enabled"),
    detail::other(Page::Gamepad, RowKind::Defaults, "Defaults"),

    detail::other(Page::TexturesMods, RowKind::Textures, "Textures"),
    detail::other(Page::TexturesMods, RowKind::HdPack, "HD pack"),
    detail::key(Page::TexturesMods, "Mods", "DumpTextures"),
    detail::other(Page::TexturesMods, RowKind::Defaults, "Defaults"),

    detail::key(Page::Sound, "Audio", "BufferMs"),
    detail::other(Page::Sound, RowKind::Defaults, "Defaults"),
};
constexpr std::string_view page_name(Page page) noexcept {
    switch (page) {
    case Page::Top: return "Settings";
    case Page::Display: return "Display";
    case Page::Graphics: return "Graphics";
    case Page::Controls: return "Controls";
    case Page::KeyboardMouse: return "Keyboard & mouse";
    case Page::Gamepad: return "Gamepad";
    case Page::TexturesMods: return "Textures & mods";
    case Page::Sound: return "Sound";
    default: return "";
    }
}
// The page a page goes back to; None from the top page (the settings page closes).
constexpr Page page_parent(Page page) noexcept {
    return page == Page::Top || page == Page::None ? Page::None : page == Page::KeyboardMouse || page == Page::Gamepad ? Page::Controls : Page::Top;
}
// The rows of a page, in the order they are shown.
inline std::span<const Row> rows_of(Page page) noexcept {
    std::size_t first = std::size(rows), count = 0u;
    for (std::size_t i = 0; i < std::size(rows); ++i) if (rows[i].page == page) { if (!count) first = i; ++count; }
    return first == std::size(rows) ? std::span<const Row>() : std::span<const Row>(rows + first, count);
}

// Quality: the launcher's four bundles of anti-aliasing, sharp textures, texture filter, ambient shadows and
// glow (launcher/src/Pages.cs, `qualities`). "custom" is what the row says while no bundle matches.
struct QualityBundle { std::string_view name, word, anti_aliasing, anisotropy, texture_filter, ambient, glow; };
inline constexpr QualityBundle qualities[]{
    {"original", "Original", "off", "1", "game", "off", "game"},
    {"medium", "Medium", "fxaa", "4", "game", "off", "smooth"},
    {"high", "High", "fxaa", "16", "game", "off", "smooth"},
    {"ultra", "Ultra", "msaa4", "16", "game", "high", "smooth"},
};
inline constexpr std::string_view quality_custom = "custom", quality_custom_word = "Custom", quality_default = "high";
inline constexpr Value textures_values[]{{"standard", "Standard"}, {"hd", "HD pack"}};

// Why a row cannot be changed; the page shows the text for a few seconds when the player tries the row.
// Each is one short clause that fits one line of the pause menu's box at the size its rows have (254 pixels
// at scale 0.65: about 33 letters; tools/settings_table_test.cpp holds them to 30). NeedsRestart is the reason
// of every row that takes a restart, in a mission: there the row can only be changed from the main menu.
enum class Lock : std::uint8_t { None, NeedsRestart, OnlyWithAuto, PspCannotLook, NotWithPsp, NoHdPack, TexturesStandard, InUse, SetOutside };
constexpr std::string_view lock_text(Lock lock) noexcept {
    switch (lock) {
    case Lock::NeedsRestart: return "Change it in the main menu.";
    case Lock::OnlyWithAuto: return "Only used with Auto.";
    case Lock::PspCannotLook: return "Not used with PSP original.";
    case Lock::NotWithPsp: return "Not used with PSP original.";
    case Lock::NoHdPack: return "No HD pack found.";
    case Lock::TexturesStandard: return "Textures are set to Standard.";
    case Lock::InUse: return "In use.";
    case Lock::SetOutside: return "Set outside the game.";
    default: return "";
    }
}
// A texture pack that could not be switched to (design 3.3).
inline constexpr std::string_view pack_not_readable = "Pack not readable.";

// ---- values ----------------------------------------------------------------------------------------

inline std::string_view trimmed(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) text.remove_prefix(1u);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1u);
    return text;
}
inline std::string lowered(std::string_view text) {
    std::string out(text);
    for (auto &c : out) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}
// A whole number or a decimal one, as the launcher's TryNum reads it (no words, nothing after the number).
inline std::optional<double> number(std::string_view text) {
    const std::string copy(trimmed(text));
    if (copy.empty() || !((copy[0] >= '0' && copy[0] <= '9') || copy[0] == '-' || copy[0] == '+' || copy[0] == '.')) return std::nullopt;
    char *end = nullptr;
    const double value = std::strtod(copy.c_str(), &end);
    if (end != copy.c_str() + copy.size() || !std::isfinite(value)) return std::nullopt;
    return value;
}
inline bool is_true(std::string_view value) noexcept { value = trimmed(value); return value == "1" || same_name(value, "true"); }
inline bool is_false(std::string_view value) noexcept { value = trimmed(value); return value == "0" || same_name(value, "false"); }
// Whether two values of a key say the same: the same text without case, the same number, or the same
// switch (true and 1, false and 0). The launcher's Model.Equivalent.
inline bool equivalent(std::string_view a, std::string_view b) {
    a = trimmed(a); b = trimmed(b);
    if (same_name(a, b)) return true;
    const auto x = number(a), y = number(b);
    if (x && y) return std::fabs(*x - *y) < 1e-9;
    return (is_true(a) && is_true(b)) || (is_false(a) && is_false(b));
}
// Whether two values of a binding key name the same inputs: for the keys of an action the same key names
// (Return is Enter, Esc is Escape; without case, in any order, a name twice counts once), for a mouse input
// or a pad button the same target (nothing is None). The launcher's Model.SameBinding.
inline bool binding_equivalent(const Setting &setting, std::string_view a, std::string_view b) {
    if (same_name(setting.section, "Input.Keyboard")) {
        const auto names = [](std::string_view text) {
            std::vector<std::string> out;
            while (!text.empty()) {
                const auto comma = std::min(text.find(','), text.size());
                auto name = lowered(trimmed(text.substr(0u, comma)));
                text.remove_prefix(std::min(comma + 1u, text.size()));
                if (name.empty()) continue;
                if (name == "return") name = "enter";
                else if (name == "esc") name = "escape";
                if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(std::move(name));
            }
            std::sort(out.begin(), out.end());
            return out;
        };
        return names(a) == names(b);
    }
    const auto target = [](std::string_view text) { auto t = lowered(trimmed(text)); return t.empty() ? std::string("none") : t; };
    return target(a) == target(b);
}
// A number as the launcher writes it into the file: at most three decimals, no zeros at the end ("0.###").
inline std::string number_text(double value) {
    char text[40];
    std::snprintf(text, sizeof(text), "%.3f", std::round(value * 1000.0) / 1000.0);
    std::string out = text;
    if (out.find('.') != std::string::npos) { while (out.back() == '0') out.pop_back(); if (out.back() == '.') out.pop_back(); }
    return out == "-0" ? "0" : out;
}
// The spellings a reader of the game takes for a value the table lists under another one.
inline std::string canonical(const Setting &setting, std::string_view value) {
    std::string text = lowered(trimmed(value));
    if (setting.kind == Kind::Switch) return is_true(text) ? "true" : is_false(text) ? "false" : text;
    if (same_name(setting.section, "Rendering")) {
        if (same_name(setting.key, "Fullscreen")) return is_true(text) ? "true" : is_false(text) ? "false" : text;
        if (same_name(setting.key, "AntiAliasing") && text == "none") return "off";
        if (same_name(setting.key, "SSAO") && (text == "false" || text == "0")) return "off";
        if (same_name(setting.key, "Preset") && text == "custom") return "high";   // no limit either
        if (same_name(setting.key, "Backend")) return text == "dx12" || text == "directx12" ? "d3d12" : text == "cpu" ? "software" : text;
    }
    return text;
}
// The place of a value among the setting's values; nothing for a value the table does not list.
inline std::optional<std::size_t> value_index(const Setting &setting, std::string_view value) {
    const auto wanted = canonical(setting, value);
    for (std::size_t i = 0; i < setting.values.size(); ++i) if (equivalent(setting.values[i].ini, wanted)) return i;
    return std::nullopt;
}
// How many steps a slider has, counted from 0 at its lowest value.
inline long slider_steps(const Setting &setting) noexcept { return std::lround((setting.max - setting.min) / setting.step); }
inline std::string slider_value(const Setting &setting, long step) {
    const long last = slider_steps(setting);
    const double value = setting.min + static_cast<double>(step < 0 ? 0 : step > last ? last : step) * setting.step;
    return number_text(value > setting.max ? setting.max : value);
}
// Whether the key may hold the value: one of a choice's values (in any spelling its reader takes), a
// slider's number within its range, true or false for a switch. A binding or a text takes anything here.
inline bool allowed(const Setting &setting, std::string_view value) {
    switch (setting.kind) {
    case Kind::Switch: return is_true(value) || is_false(value);
    case Kind::Choice: return value_index(setting, value).has_value();
    case Kind::Slider: { const auto n = number(value); return n && *n >= setting.min - 1e-9 && *n <= setting.max + 1e-9; }
    default: return true;
    }
}
// What the page shows for a value: the word of a choice, on or off, a slider's number in its own manner.
// A value the table does not list is shown as it stands (a resolution multiple as its size).
inline std::string shown(const Setting &setting, std::string_view value) {
    if (setting.kind == Kind::Slider) {
        const auto n = number(value);
        if (!n) return std::string(trimmed(value));
        char text[40];
        if (setting.show == Show::Percent) std::snprintf(text, sizeof(text), "%ld %%", std::lround(*n * 100.0));
        else if (setting.show == Show::Milliseconds) std::snprintf(text, sizeof(text), "%ld ms", std::lround(*n));
        else std::snprintf(text, sizeof(text), "%.2f", *n);
        return text;
    }
    if (const auto index = value_index(setting, value)) return std::string(setting.values[*index].word);
    if (same_name(setting.section, "Rendering") && same_name(setting.key, "InternalScale"))
        if (const auto n = number(value); n && *n >= 1.0 && *n <= 8.0 && *n == std::floor(*n)) {
            char text[40];
            std::snprintf(text, sizeof(text), "%d x %d", 480 * static_cast<int>(*n), 272 * static_cast<int>(*n));
            return text;
        }
    return std::string(trimmed(value));
}
// The value one step to the right (direction > 0) or to the left of `value`. A choice and a switch wrap,
// as the game's own value lines do; a slider stops at its ends. From a value the table does not list a
// choice goes to its first value (right) or its last (left), a slider to the step beside it.
inline std::string stepped(const Setting &setting, std::string_view value, int direction) {
    if (setting.kind == Kind::Slider) {
        const auto n = number(value);
        // The file may hold a number outside the row's range (the game's reader takes more than the row
        // offers: a mouse speed of 1.5 written by hand). A step that leads further away leaves it alone; a
        // step towards the range goes to the range's end.
        if (n && ((*n > setting.max + 1e-9 && direction > 0) || (*n < setting.min - 1e-9 && direction < 0))) return std::string(trimmed(value));
        const double at = ((n ? *n : setting.min) - setting.min) / setting.step;
        const long next = direction > 0 ? static_cast<long>(std::floor(at + 1e-6)) + 1 : static_cast<long>(std::ceil(at - 1e-6)) - 1;
        return slider_value(setting, next);
    }
    if (setting.values.empty()) return std::string(trimmed(value));
    const auto count = setting.values.size();
    const auto index = value_index(setting, value);
    if (!index) return std::string(setting.values[direction > 0 ? 0u : count - 1u].ini);
    return std::string(setting.values[(*index + (direction > 0 ? 1u : count - 1u)) % count].ini);
}
}
