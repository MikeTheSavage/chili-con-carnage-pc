// The game's own settings page (host/settings_page.hpp) worked without the game: pad samples, the
// pointer and the clock go in, the rows, the settings file and the draw list come out.
//
//   chili_settings_page_test <scratch folder>
//
// - The pages and their rows, in the design's order; Back from a section lands on the line that opened it.
// - The pad: what is held when the page opens does nothing until it is let go; up and down wrap and repeat
//   after 12 samples at every third; a list longer than the frame moves by whole rows; left, right and
//   Cross change a value, a choice wraps, a slider repeats and is written when the key is let go; Circle
//   goes back, Start closes.
// - A row that takes a restart is written at once and shows as pending; in the pause menu it is locked
//   and says why for three seconds when it is tried; the reason goes with the focus.
// - Defaults asks first; Cancel changes nothing.
// - The pointer: the row under it takes the focus, a click chooses, a held slider follows it, the wheel
//   moves the focus, the legends and the list pointers are buttons.
// - The draw list: nothing of a page in the pause menu's box leaves the box, a value too long for its
//   row is cut short, a reason too long for the box takes two lines.
// The font here is made up (every letter 19 wide); the game's own is read from the running game.
#include "settings_page.hpp"
#include <cstdio>
#include <fstream>
#include <array>
#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

namespace {
namespace st = chili::settings;
using chili::PageFrame;
using st::SettingsPage;
unsigned checks, failures;
const bool keep_going = [] { const char *value = std::getenv("CHILI_TEST_KEEP_GOING"); return value && *value == '1'; }();
void require(bool ok, const std::string &what) {
    ++checks;
    if (ok) return;
    if (!keep_going) throw std::runtime_error(what);
    ++failures;
    std::cerr << "FAILED CHECK: " << what << '\n';
}
template <class A, class B> void is(const A &a, const B &b, const std::string &what) {
    require(a == b, what + ": " + std::to_string(a) + " is not " + std::to_string(b));
}

struct Fixture {
    std::set<int> keys;
    std::optional<XINPUT_STATE> pad;
    double now{100.0};
    chili::PcInput input;
    explicit Fixture(chili::PcInputConfig config) {
        input.configure(std::move(config));
        chili::PcInputHost host;
        host.focused = [] { return true; };
        host.key_down = [this](int key) { return keys.count(key) != 0; };
        host.pad = [this](unsigned slot, XINPUT_STATE &state) { if (slot != 1u || !pad) return false; state = *pad; return true; };
        host.seconds = [this] { return now; };
        host.pad_worker = false;
        host.grab = [](bool) {};
        host.vibrate = [](unsigned, std::uint16_t, std::uint16_t) { return true; };
        host.log = [](const std::string &) {};
        input.set_host(std::move(host));
    }
};
std::string file_text(const std::filesystem::path &file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
bool has_line(const std::filesystem::path &file, const std::string &line) {
    const std::string text = file_text(file);
    std::size_t at = 0;
    while (at <= text.size()) {
        auto end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string have = text.substr(at, end - at);
        if (!have.empty() && have.back() == '\r') have.pop_back();
        if (have == line) return true;
        at = end + 1u;
    }
    return false;
}
const char *other_sections =
    "\n[Mods]\n; Folder=mods\n; DumpTextures=false\n"
    "\n[Game]\n; FrameLock=1\n; Camera=tight\n"
    "\n[Rendering]\n; Preset=high\n; InternalScale=auto\n; AntiAliasing=fxaa\n; Anisotropy=16\n; TextureFilter=game\n; SSAO=off\n; Glow=smooth\n; Fullscreen=false\n; EarlyPresent=true\n"
    "\n[Audio]\n; BufferMs=70\n";
std::filesystem::path fresh_folder(const std::filesystem::path &scratch, const char *name, std::initializer_list<const char *> packs = {}) {
    const auto folder = scratch / name;
    std::error_code status;
    std::filesystem::remove_all(folder, status);
    std::filesystem::create_directories(folder / "mods");
    if (!chili::ini_file::write(folder / "ChiliNative.ini", std::string(chili::default_input_ini()) + other_sections)) throw std::runtime_error("the test cannot write its settings file");
    for (const char *pack : packs) { std::filesystem::create_directories(folder / "mods" / pack / "textures"); std::ofstream(folder / "mods" / pack / "textures" / "0000000000000001.png") << "x"; }
    return folder;
}
// `frame_lock`: "2" is the game's 30 frames per second, "1" 60.
st::Host host_for(const std::filesystem::path &folder, chili::PcInput *input, const std::string &mods = "", const char *frame_lock = "2") {
    st::Host host;
    host.ini = folder / "ChiliNative.ini";
    host.input = input;
    host.mods_folder = folder / "mods";
    host.started = {{"Rendering/Fullscreen", "false"}, {"Rendering/InternalScale", "auto"}, {"Rendering/EarlyPresent", "true"}, {"Rendering/Preset", "high"},
        {"Rendering/AntiAliasing", "fxaa"}, {"Rendering/MSAA", "1"}, {"Rendering/TextureFilter", "game"}, {"Rendering/Anisotropy", "16"}, {"Rendering/SSAO", "off"}, {"Rendering/Glow", "smooth"},
        {"Game/FrameLock", frame_lock}, {"Game/Camera", "tight"}, {"Audio/BufferMs", "70"}, {"Mods/Enabled", mods}, {"Mods/DumpTextures", "false"}};
    return host;
}
// A page with its settings, its input layer and its clock.
struct Bench {
    std::filesystem::path folder, ini;
    Fixture f;
    st::Live live;
    SettingsPage page;
    double now{10.0};
    Bench(const std::filesystem::path &scratch, const char *name, std::initializer_list<const char *> packs = {}, const std::string &mods = "", const char *frame_lock = "2")
        : folder(fresh_folder(scratch, name, packs)), ini(folder / "ChiliNative.ini"), f(load(ini)), live(host_for(folder, &f.input, mods, frame_lock)), page(&live) { page.tick(now); }
    static chili::PcInputConfig load(const std::filesystem::path &ini) {
        std::vector<std::string> problems;
        auto config = chili::PcInputConfig::load(ini, problems, false);
        if (!problems.empty()) throw std::runtime_error("the game does not read the test's settings file: " + problems.front());
        return config;
    }
    // One pad sample, a thirtieth of a second on.
    void sample(std::uint32_t buttons, std::uint8_t x = 128u, std::uint8_t y = 128u) { now += 1.0 / 30.0; page.tick(now); page.pad(buttons, x, y); }
    // A button pressed for one sample and let go for one.
    void tap(std::uint32_t button) { sample(button); sample(0u); }
    void wait(double seconds) { now += seconds; page.tick(now); }
    const SettingsPage::RowView &row() const { return page.rows()[page.focus()]; }
    std::string name() const { return row().name; }
    std::string value() const { return row().value; }
    // The focus on the row of that name, by Down.
    void go(std::string_view wanted) {
        for (std::size_t i = 0; i < page.rows().size() && name() != wanted; ++i) page.move(+1);
        if (name() != wanted) throw std::runtime_error("no row '" + std::string(wanted) + "' on " + std::string(st::page_name(page.page())));
    }
    bool event(std::string_view part) {
        const auto lines = page.take_events();
        seen.insert(seen.end(), lines.begin(), lines.end());
        for (const auto &line : lines) if (line.find(part) != std::string::npos) return true;
        return false;
    }
    std::vector<std::string> seen;
};

// Every printable ASCII letter 19 wide, the lock and the chili as pictures, the four buttons a legend may need.
chili::PageFont made_up_font() {
    std::vector<chili::PageGlyph> glyphs;
    const auto add = [&](char32_t code, float width, bool picture) {
        chili::PageGlyph glyph;
        glyph.code = static_cast<std::uint16_t>(code);
        glyph.y_offset = 9.0f; glyph.advance = width;
        glyph.u = static_cast<float>(glyphs.size() % 12u) * 21.0f / 256.0f; glyph.v = static_cast<float>(glyphs.size() / 12u) * 30.0f / 512.0f; glyph.u_width = width / 256.0f;
        glyph.picture = picture;
        glyphs.push_back(glyph);
    };
    for (char32_t code = U'0'; code <= U'9'; ++code) add(code, 19.0f, false);
    for (char32_t code = 32; code < 127; ++code) {
        if (code >= U'0' && code <= U'9') continue;
        const bool picture = code == U'}' || code == U'~' || code == U'{';
        add(code, picture ? 24.0f : code == U' ' ? 14.0f : 19.0f, picture);
    }
    for (char32_t code : {chili::page_glyph::circle, chili::page_glyph::cross, chili::page_glyph::dpad_left, chili::page_glyph::dpad_right}) add(code, 24.0f, true);
    chili::PageFont font;
    font.assign(std::move(glyphs), 30.0f);
    return font;
}
struct Bounds { float x0{1e9f}, y0{1e9f}, x1{-1e9f}, y1{-1e9f}; std::size_t vertices{}, runs{}, textured{}; };
// The page's draw list and what it covers; the plain rectangles of the frame (the box, the dimming) left out
// of the bounds when `text_only`.
Bounds drawn(const SettingsPage &page, const chili::PageFont &font, bool text_only, bool sprites = true) {
    chili::PageDraw draw;
    page.build(draw, font, [sprites](std::uint64_t) { return sprites; });
    const auto list = draw.finish();
    Bounds bounds;
    bounds.vertices = list->vertices.size(); bounds.runs = list->runs.size();
    for (const auto &run : list->runs) {
        if (run.texture) ++bounds.textured;
        if (text_only && run.texture != chili::page_texture::font_fill && run.texture != chili::page_texture::font_outline) continue;
        for (std::uint32_t i = 0; i < run.vertex_count; ++i) {
            const auto &v = list->vertices[run.first_vertex + i];
            bounds.x0 = std::min(bounds.x0, v.x); bounds.y0 = std::min(bounds.y0, v.y); bounds.x1 = std::max(bounds.x1, v.x); bounds.y1 = std::max(bounds.y1, v.y);
        }
    }
    return bounds;
}
using namespace st::pad;

// ---- Pages, rows and the pad ----------------------------------------------------------------------------

void pages_and_pad(const std::filesystem::path &scratch) {
    {
        SettingsPage none;
        require(!none.open_page(PageFrame::MainMenu) && !none.open(), "without settings the page does not open");
        none.pad(cross); none.pointer(100.0f, 100.0f); none.press(100.0f, 100.0f); none.wheel(1); none.close();
        require(!none.open(), "a closed page takes buttons and the pointer without doing anything");
    }
    Bench b(scratch, "pad");
    require(b.page.open_page(PageFrame::MainMenu) && b.page.open() && b.page.page() == st::Page::Top, "the page opens on the top page");
    require(b.event("page open: Settings, over the menu"), "and says so");
    is(b.page.rows().size(), std::size_t{5}, "the top page's rows");
    {
        const char *names[]{"Display", "Graphics", "Controls", "Textures & mods", "Sound"};
        for (std::size_t i = 0; i < 5u; ++i) require(b.page.rows()[i].name == names[i] && b.page.rows()[i].act == SettingsPage::Act::Link && b.page.rows()[i].value.empty(), std::string("top page row ") + names[i]);
        require(b.page.legends().left == "Back" && b.page.legends().right == "Select" && !b.page.legends().right_is_dpad, "a section row's legends: Back and Select");
    }
    // Held at the opening: the Cross that chose the game's line does not choose a row.
    for (int i = 0; i < 20; ++i) b.sample(cross | down);
    require(b.page.page() == st::Page::Top && b.page.focus() == 0u, "what is held when the page opens does nothing, however long");
    b.sample(down);
    require(b.page.focus() == 0u, "nor a direction that was held with it");
    b.sample(0u);
    b.tap(down);
    is(b.page.focus(), std::size_t{1}, "let go and pressed again, Down moves the focus");
    b.tap(up); b.tap(up);
    is(b.page.focus(), std::size_t{4}, "Up from the first row wraps to the last");
    b.tap(down);
    is(b.page.focus(), std::size_t{0}, "Down from the last row wraps to the first");
    // The stick past half is the D-pad.
    b.sample(0u, 128u, 255u); b.sample(0u);
    is(b.page.focus(), std::size_t{1}, "the stick down moves the focus");
    b.sample(0u, 128u, 150u); b.sample(0u);
    is(b.page.focus(), std::size_t{1}, "a stick less than half way does not");
    b.sample(0u, 128u, 0u); b.sample(0u);
    is(b.page.focus(), std::size_t{0}, "the stick up moves it back");
    // Held: again after 12 samples, then at every third.
    {
        std::vector<int> moved;
        std::size_t before = b.page.focus();
        for (int i = 0; i < 22; ++i) { b.sample(down); if (b.page.focus() != before) { moved.push_back(i); before = b.page.focus(); } }
        b.sample(0u);
        require(moved == std::vector<int>({0, 12, 15, 18, 21}), "a held Down moves at once, after 12 samples and then at every third");
    }
    // At 60 game frames per second the pad is read twice as often: the same 0.4 seconds, the same ten a second.
    {
        Bench fast(scratch, "pad60", {}, "", "1");
        require(fast.page.open_page(PageFrame::MainMenu), "the page opens in a game at 60 frames per second");
        fast.sample(0u);
        std::vector<int> moved;
        std::size_t before = fast.page.focus();
        for (int i = 0; i < 44; ++i) { fast.sample(down); if (fast.page.focus() != before) { moved.push_back(i); before = fast.page.focus(); } }
        require(moved == std::vector<int>({0, 24, 30, 36, 42}), "at 60 frames per second a held Down moves at once, after 24 samples and then at every sixth");
    }
    // Into a section and back: the focus is on the line that opened it.
    b.page.close();
    require(!b.page.open() && b.event("page closed"), "close closes");
    require(b.page.open_page(PageFrame::MainMenu), "and it opens again");
    b.sample(0u);
    b.go("Controls");
    b.tap(cross);
    require(b.page.page() == st::Page::Controls, "Cross on a section opens it");
    {
        const char *names[]{"Style", "Camera", "Keyboard & mouse", "Gamepad", "Prompts", "Button pictures", "Defaults"};
        is(b.page.rows().size(), std::size_t{7}, "the rows of Controls");
        for (std::size_t i = 0; i < 7u; ++i) require(b.page.rows()[i].name == names[i], std::string("Controls row ") + names[i]);
        is(st::main_layout.visible, std::size_t{7}, "seven rows are shown in the menu frame");
        is(st::pause_layout.visible, std::size_t{7}, "and seven in the pause menu's box");
        is(b.page.first_visible(), std::size_t{0}, "seven rows, seven shown: the list begins at its first row");
        for (int i = 0; i < 6; ++i) b.tap(down);
        require(b.name() == "Defaults" && b.page.first_visible() == 0u, "the focus on the seventh row: Controls and its Defaults are all on the page, nothing moves");
        b.tap(down);
        require(b.name() == "Style" && b.page.first_visible() == 0u && b.value() == "Total Overdose PC", "and it wraps to the first: the control style, Total Overdose PC");
        // The control style from the main menu: written with its keys, from the next start.
        b.tap(right);
        require(b.value() == "Modern" && b.row().pending && has_line(b.ini, "Layout=modern") && has_line(b.ini, "Circle=F,Backspace") && has_line(b.ini, "Scheme=dual_analog"),
            "Right on Style: Modern, written with its keys, after a restart");
        b.tap(left);
        require(b.value() == "Total Overdose PC" && !b.row().pending && has_line(b.ini, "Layout=tod_pc") && has_line(b.ini, "Circle=E,Backspace"), "Left: Total Overdose PC again, as the game runs");
    }
    b.go("Keyboard & mouse");
    b.tap(cross);
    require(b.page.page() == st::Page::KeyboardMouse && b.page.rows().size() == 13u && b.name() == "Turn speed", "Keyboard & mouse has its twelve rows and Defaults");
    for (int i = 0; i < 7; ++i) b.tap(down);
    require(b.name() == "Grab the mouse" && b.page.first_visible() == 1u, "the focus on the eighth row moves the list by one whole row");
    for (int i = 0; i < 6; ++i) b.tap(down);
    require(b.name() == "Turn speed" && b.page.first_visible() == 0u, "round the list: back at its top");
    b.tap(up);
    require(b.name() == "Defaults" && b.page.first_visible() == 6u, "Up from the first row: the last row, and the list's last seven");
    b.tap(circle);
    require(b.page.page() == st::Page::Controls && b.name() == "Keyboard & mouse", "Circle goes back to Controls, on the line that opened the page");
    b.tap(circle);
    require(b.page.page() == st::Page::Top && b.name() == "Controls", "and to the top page, on Controls");
    b.tap(circle);
    require(!b.page.open(), "Circle on the top page closes the settings page");
    // Start closes from anywhere.
    require(b.page.open_page(PageFrame::MainMenu, st::Page::Gamepad) && b.page.page() == st::Page::Gamepad, "the page can open on a section");
    b.sample(0u);
    b.tap(start);
    require(!b.page.open(), "Start closes the page from a section");
    require(b.page.open_page(PageFrame::MainMenu, st::Page::Gamepad), "open on Gamepad");
    b.page.back();
    require(b.page.open() && b.page.page() == st::Page::Controls && b.name() == "Gamepad", "a page opened on a section still goes back to its parent");
    b.page.close();
}

// ---- Changing values ------------------------------------------------------------------------------------

void values(const std::filesystem::path &scratch) {
    Bench b(scratch, "values");
    require(b.page.open_page(PageFrame::MainMenu, st::Page::KeyboardMouse), "open on Keyboard & mouse");
    b.sample(0u);
    (void)b.event("");
    // A switch: Left, Right and Cross flip it; in force and written at once.
    b.go("Invert up/down");
    require(b.value() == "off" && b.row().act == SettingsPage::Act::Switch && !b.f.input.config().mouse_invert_y, "the switch is off");
    require(b.page.legends().right == "Change" && b.page.legends().right_is_dpad, "a value row's legend: the D-pad and Change");
    b.tap(right);
    require(b.value() == "on" && b.f.input.config().mouse_invert_y && has_line(b.ini, "InvertY=true"), "Right flips it: in force and in the file");
    require(b.event("Input.Mouse/InvertY=true applied live, written"), "with its line for the log");
    b.tap(left);
    require(b.value() == "off" && !b.f.input.config().mouse_invert_y && has_line(b.ini, "InvertY=false"), "Left flips it back");
    b.tap(cross);
    require(b.value() == "on" && b.f.input.config().mouse_invert_y, "Cross flips it too");
    // A held Left or Right does not run a switch round.
    for (int i = 0; i < 30; ++i) b.sample(right);
    b.sample(0u);
    require(b.value() == "off", "a held Right flips a switch once");
    // A slider: steps, repeats while held, and is written when the key is let go.
    b.go("Turn speed");
    require(b.value() == "0.08" && b.row().act == SettingsPage::Act::Slider, "the slider's value");
    b.sample(right);
    require(b.value() == "0.09" && b.f.input.config().mouse_degrees_per_count > 0.089f && !has_line(b.ini, "SensitivityX=0.09"), "Right steps it: in force, not written while the key is down");
    b.sample(0u);
    require(has_line(b.ini, "SensitivityX=0.09") && b.event("written"), "written when the key is let go");
    for (int i = 0; i < 22; ++i) b.sample(right);
    require(b.value() == "0.14" && !has_line(b.ini, "SensitivityX=0.14"), "held: at once, after 12 samples and at every third (five steps in 22 samples)");
    b.sample(0u);
    require(has_line(b.ini, "SensitivityX=0.14"), "and written at the end");
    for (int i = 0; i < 400; ++i) b.sample(left);
    b.sample(0u);
    require(b.value() == "0.01" && has_line(b.ini, "SensitivityX=0.01"), "a slider stops at its lowest value");
    b.tap(left);
    require(b.value() == "0.01", "and stays there");
    // Leaving the row writes a slider that was changed by a step of a test (no key to let go).
    b.page.change(+1);
    require(b.value() == "0.02" && !has_line(b.ini, "SensitivityX=0.02"), "a step without a key");
    b.page.move(+1);
    require(has_line(b.ini, "SensitivityX=0.02"), "is written when the focus leaves the row");
    // A choice wraps.
    b.page.back();
    b.go("Prompts");
    require(b.value() == "Automatic" && b.row().act == SettingsPage::Act::Choice, "Prompts is a choice");
    b.tap(right);
    require(b.value() == "Keyboard" && b.f.input.config().prompts == chili::PromptsSetting::Keyboard && has_line(b.ini, "Prompts=keyboard"), "Right: the next value, in force and written");
    b.tap(right); b.tap(right);
    require(b.value() == "PSP original", "the last value");
    b.tap(right);
    require(b.value() == "Automatic", "Right from the last value wraps to the first");
    b.tap(left);
    require(b.value() == "PSP original", "Left from the first wraps to the last");
    b.tap(cross);
    require(b.value() == "Automatic", "Cross steps to the right");
    // A row that takes a restart: written at once, shown as pending.
    b.page.back();
    b.go("Display"); b.tap(cross);
    b.go("Resolution");
    require(b.value() == "Auto" && !b.row().pending && b.row().lock == st::Lock::None, "Resolution is Auto and free in the main menu");
    b.tap(right);
    require(b.value() == "480 x 272" && b.row().pending && has_line(b.ini, "InternalScale=1"), "a new resolution is written and waits for a restart");
    require(b.event("Rendering/InternalScale=1 waits for a restart, written"), "with its line");
    // What the chili after the value means is said once, for three seconds, and goes with the focus.
    require(b.page.reason() == "After a restart.", "a value that now waits for the next start says so");
    b.wait(2.8);
    require(b.page.reason() == "After a restart.", "still after 2.8 seconds");
    b.wait(0.3);
    require(b.page.reason().empty() && b.row().pending, "gone after three; the chili stays");
    b.tap(right);
    require(b.value() == "960 x 544" && b.page.reason() == "After a restart.", "and again with the next value");
    b.tap(left); b.tap(left);
    require(b.value() == "Auto" && !b.row().pending && b.page.reason().empty(), "back at the value the game runs with: nothing waits, nothing is said");
    b.tap(right);
    require(b.value() == "480 x 272" && b.row().pending, "a new resolution again");
    // An input row is in force at once: nothing is said.
    b.page.back(); b.go("Controls"); b.page.ok(); b.go("Prompts");
    b.tap(right);
    require(b.page.reason().empty(), "a change that is in force at once says nothing");
    b.tap(left);
    b.page.back(); b.go("Display"); b.page.ok(); b.go("Resolution");
    (void)b.event("");
    b.go("Auto limit");
    require(b.row().lock == st::Lock::OnlyWithAuto && b.page.legends().right.empty(), "Auto limit is locked while the resolution is not Auto, and has no legend at the right");
    b.tap(right);
    require(b.page.reason() == "Only used with Auto." && b.value() == "No limit", "tried, it says why and stays");
    b.go("Resolution");
    require(b.page.reason().empty(), "the reason goes with the focus");
    b.tap(left);
    require(b.value() == "Auto" && !b.row().pending, "back to Auto: nothing waits any more");
    b.page.close();
    require(b.page.take_events().back() == "page closed", "closed");
}

// ---- The pause menu: locks, reasons, Defaults -------------------------------------------------------------

void locks_and_defaults(const std::filesystem::path &scratch) {
    Bench b(scratch, "locks");
    require(b.page.open_page(PageFrame::Pause, st::Page::Display) && b.page.frame() == PageFrame::Pause, "the page in the pause menu's box");
    b.sample(0u);
    for (const auto &row : b.page.rows()) require(row.lock == st::Lock::NeedsRestart, "in a mission every row of Display is locked: " + row.name);
    b.go("Frame rate");
    require(b.page.legends().left == "Back" && b.page.legends().right.empty(), "a locked row has only Back");
    const std::string before = file_text(b.ini);
    b.tap(right);
    require(b.page.reason() == "Change it in the main menu." && b.value() == "60 fps" && file_text(b.ini) == before, "tried: the reason, and nothing changed");
    b.wait(2.8);
    require(!b.page.reason().empty(), "the reason is still there after 2.8 seconds");
    b.wait(0.3);
    require(b.page.reason().empty(), "and gone after three");
    b.tap(cross);
    require(!b.page.reason().empty(), "Cross on a locked row says it too");
    b.tap(down);
    require(b.page.reason().empty(), "the reason goes with the focus");
    b.go("Defaults");
    require(b.row().lock == st::Lock::NeedsRestart, "Defaults is locked where every row it would reset is");
    b.tap(cross);
    require(!b.page.asking() && b.page.reason() == "Change it in the main menu.", "and says why instead of asking");
    // Controls in a mission: the input rows are free, the two that take a restart are not.
    b.page.back(); b.go("Controls"); b.page.ok();
    require(b.page.rows()[0].lock == st::Lock::NeedsRestart && b.page.rows()[1].lock == st::Lock::NeedsRestart, "Style and Camera are locked in a mission");
    b.go("Prompts");
    require(b.row().lock == st::Lock::None, "Prompts is not");
    b.tap(right);
    require(b.value() == "Keyboard" && has_line(b.ini, "Prompts=keyboard"), "and changes in a mission");
    b.page.back(); b.go("Controls"); b.page.ok(); b.go("Gamepad"); b.page.ok();
    b.go("Rumble");
    b.tap(left);
    require(b.value() == "95 %" && has_line(b.ini, "Rumble=0.95"), "a gamepad row changes in a mission");
    b.page.back();
    // Defaults: a question first.
    b.go("Defaults");
    require(b.row().lock == st::Lock::None && b.page.legends().right == "Select", "Defaults of Controls is free: Select");
    b.tap(cross);
    require(b.page.asking() && b.page.legends().left == "Cancel" && b.page.legends().right == "Ok", "Cross asks, with Cancel and Ok");
    b.tap(down); b.tap(right);
    require(b.page.asking() && b.name() == "Defaults", "the directions do nothing while it asks");
    b.tap(circle);
    require(!b.page.asking() && b.page.open() && has_line(b.ini, "Prompts=keyboard") && has_line(b.ini, "Rumble=0.95"), "Circle cancels: nothing changed, the page stays");
    b.tap(cross); b.tap(cross);
    require(!b.page.asking() && has_line(b.ini, "Prompts=auto") && has_line(b.ini, "Rumble=1") && b.f.input.config().prompts == chili::PromptsSetting::Auto,
        "Ok resets Controls with its two device pages, in force and written");
    b.go("Prompts");
    require(b.value() == "Automatic", "and the row shows the default");
    b.go("Defaults"); b.tap(cross);
    b.tap(start);
    require(!b.page.open(), "Start closes the page also while it asks");
    // The gamepad cannot be switched off from the gamepad.
    {
        XINPUT_STATE state{};
        state.Gamepad.wButtons = XINPUT_GAMEPAD_A;
        b.f.pad = state;
        b.f.input.scan_pads();
        b.f.now += 1.0 / 30.0;
        (void)b.f.input.sample();
        require(b.f.input.device() == chili::InputDevice::Gamepad, "a pad is the device in use");
        require(b.page.open_page(PageFrame::MainMenu, st::Page::Gamepad), "open on Gamepad");
        b.go("Use the gamepad");
        require(b.row().lock == st::Lock::InUse, "Use the gamepad is locked while the pad is in use");
        b.page.change(-1);
        require(b.page.reason() == "In use." && b.f.input.config().pad_enabled, "and says so");
        b.page.close();
    }
}

// ---- The pointer ------------------------------------------------------------------------------------------

void pointer(const std::filesystem::path &scratch) {
    Bench b(scratch, "pointer", {"alpha_pack", "beta_pack_with_a_very_long_folder_name"}, "alpha_pack,beta_pack_with_a_very_long_folder_name");
    require(b.page.open_page(PageFrame::MainMenu), "open");
    const auto middle = [&](std::size_t visible_row, float &x, float &y) { const auto box = b.page.row_box(visible_row); x = (box.x0 + box.x1) * 0.5f; y = (box.y0 + box.y1) * 0.5f; };
    float x = 0.0f, y = 0.0f;
    middle(2u, x, y);
    b.page.pointer(x, y);
    require(b.name() == "Controls", "the row under the pointer takes the focus");
    b.page.pointer(x, 60.0f);
    require(b.name() == "Controls", "the pointer over nothing leaves it");
    b.page.press(x, y);
    require(b.page.page() == st::Page::Controls, "a click on a section opens it");
    b.page.right_click();
    require(b.page.page() == st::Page::Top && b.name() == "Controls", "a right click goes back");
    b.page.wheel(-1);
    require(b.name() == "Textures & mods", "the wheel towards the player moves the focus down");
    b.page.wheel(2);
    require(b.name() == "Graphics", "and away from the player up, a row per notch");
    // The legends are buttons.
    {
        const auto back_box = b.page.left_legend_box(), act_box = b.page.right_legend_box();
        b.page.press((act_box.x0 + act_box.x1) * 0.5f, (act_box.y0 + act_box.y1) * 0.5f);
        require(b.page.page() == st::Page::Graphics, "a click on Select chooses the focused row");
        b.page.press((back_box.x0 + back_box.x1) * 0.5f, (back_box.y0 + back_box.y1) * 0.5f);
        require(b.page.page() == st::Page::Top, "a click on Back goes back");
    }
    // A switch and a choice: a click steps to the right.
    b.go("Controls"); b.page.ok(); b.go("Keyboard & mouse"); b.page.ok();
    middle(3u, x, y);
    b.page.pointer(x, y);
    require(b.name() == "Invert up/down" && b.value() == "off", "over the fourth row");
    b.page.press(x, y); b.page.release(x, y);
    require(b.value() == "on" && has_line(b.ini, "InvertY=true"), "a click flips a switch, written");
    {
        const auto act_box = b.page.right_legend_box();
        b.page.press((act_box.x0 + act_box.x1) * 0.5f, (act_box.y0 + act_box.y1) * 0.5f);
        require(b.value() == "off", "a click on Change steps the focused row");
    }
    // A slider: a click steps to the right; held and moved, it follows by the distance.
    middle(0u, x, y);
    b.page.pointer(x, y);
    require(b.name() == "Turn speed" && b.value() == "0.08", "over the slider");
    b.page.press(x, y);
    require(b.page.dragging() && b.value() == "0.08", "pressed, the slider is held and has not moved");
    b.page.release(x, y);
    require(!b.page.dragging() && b.value() == "0.09" && has_line(b.ini, "SensitivityX=0.09"), "let go where it was pressed: one step, written");
    b.page.set_pointer_scale(3.0f);   // a window of 1440 x 816: 4 window pixels are 4/3 of a PSP pixel
    b.page.press(x, y);
    b.page.pointer(x + 40.0f, y - 30.0f);
    require(b.value() == "0.39" && b.f.input.config().mouse_degrees_per_count > 0.389f && !has_line(b.ini, "SensitivityX=0.39"),
        "moved 40 PSP pixels to the right while held (120 of the window): 30 steps, in force, not written yet");
    require(b.name() == "Turn speed", "the pointer leaving the row while it holds the slider does not move the focus");
    b.page.pointer(x + 20.0f, y);
    require(b.value() == "0.24", "moved back: it follows");
    b.page.pointer(x - 400.0f, y);
    require(b.value() == "0.01", "far to the left: the lowest value");
    b.page.release(x + 8.0f, y);
    require(b.value() == "0.15" && has_line(b.ini, "SensitivityX=0.15") && !b.page.dragging(), "let go: where the pointer is, and written");
    b.page.set_pointer_scale(1.0f);
    b.page.press(x, y); b.page.pointer(x + 8.0f, y); b.page.release(x + 8.0f, y);
    require(b.value() == "0.17", "in a window of 480 x 272 four pixels are a step");
    // The list pointers: the list a page on.
    is(b.page.first_visible(), std::size_t{0}, "the list of Keyboard & mouse at its top");
    {
        const auto lower = b.page.pointer_box(true), upper = b.page.pointer_box(false);
        // A list pointer is never on a row's own line, where it would read as that value's arrow: the upper one
        // stands above the first row shown, the lower one under the last, both at the right edge of the rows.
        const auto first_row = b.page.row_box(0u), last_row = b.page.row_box(st::main_layout.visible - 1u);
        require(upper.y1 <= first_row.y0 && lower.y0 >= last_row.y1 && upper.x0 >= st::main_layout.value_right - 0.01f && lower.x0 == upper.x0, "the list pointers stand above and under the rows");
        for (const PageFrame frame : {PageFrame::MainMenu, PageFrame::Pause}) {
            const auto &l = st::layout_of(frame);
            const float last_y = l.row_y + l.row_pitch * static_cast<float>(l.visible - 1u);
            require(l.row_y - 9.0f > l.headline_y && last_y + l.row_pitch + 10.0f <= l.area.y1 && l.value_right + 12.0f <= l.area.x1 + 3.01f, "there is room for them in both frames");
        }
        b.page.press((lower.x0 + lower.x1) * 0.5f, (lower.y0 + lower.y1) * 0.5f);
        require(b.page.first_visible() == 6u && b.page.focus() == 6u, "a click on the lower pointer: the last seven rows, the focus among them");
        b.page.press((lower.x0 + lower.x1) * 0.5f, (lower.y0 + lower.y1) * 0.5f);
        require(b.page.first_visible() == 6u && b.page.focus() == 6u, "at the list's end there is no lower pointer: a click there does nothing");
        b.page.press((upper.x0 + upper.x1) * 0.5f, (upper.y0 + upper.y1) * 0.5f);
        require(b.page.first_visible() == 0u, "a click on the upper pointer: seven rows back");
        middle(5u, x, y);
        b.page.pointer(x, y);
        require(b.page.focus() == 5u, "the pointer chooses among the rows shown");
    }
    // Defaults by the pointer: the question's legends answer.
    b.page.back();
    b.go("Defaults");
    b.page.ok();
    require(b.page.asking(), "asks");
    middle(1u, x, y);
    b.page.pointer(x, y); b.page.press(x, y);
    require(b.page.asking() && b.name() == "Defaults", "the rows are not the pointer's while it asks");
    {
        const auto back_box = b.page.left_legend_box();
        b.page.press((back_box.x0 + back_box.x1) * 0.5f, (back_box.y0 + back_box.y1) * 0.5f);
        require(!b.page.asking() && has_line(b.ini, "SensitivityX=0.17"), "a click on Cancel: nothing changed");
        b.page.ok();
        const auto act_box = b.page.right_legend_box();
        b.page.press((act_box.x0 + act_box.x1) * 0.5f, (act_box.y0 + act_box.y1) * 0.5f);
        require(!b.page.asking() && has_line(b.ini, "SensitivityX=0.08"), "a click on Ok resets");
    }
    b.page.right_click(); b.page.right_click();
    require(!b.page.open(), "a right click on the top page closes");
    // Textures & mods with two packs: the HD pack row is there.
    require(b.page.open_page(PageFrame::MainMenu, st::Page::TexturesMods), "open on Textures & mods");
    is(b.page.rows().size(), std::size_t{4}, "Textures, HD pack, Dump textures, Defaults");
    require(b.page.rows()[0].value == "HD pack" && b.page.rows()[1].name == "HD pack", "the pack row with two packs installed");
    b.page.close();
}

// ---- The draw list ----------------------------------------------------------------------------------------

void drawing(const std::filesystem::path &scratch) {
    const chili::PageFont font = made_up_font();
    Bench b(scratch, "drawing", {"alpha_pack", "beta_pack_with_a_very_long_folder_name"}, "alpha_pack,beta_pack_with_a_very_long_folder_name");
    {
        chili::PageDraw draw;
        b.page.build(draw, font, [](std::uint64_t) { return true; });
        require(draw.empty(), "a closed page draws nothing");
    }
    // Every page in both frames, every row in focus in turn.
    for (const PageFrame frame : {PageFrame::MainMenu, PageFrame::Pause}) {
        const auto &l = st::layout_of(frame);
        for (const st::Page page : {st::Page::Top, st::Page::Display, st::Page::Graphics, st::Page::Controls, st::Page::KeyboardMouse, st::Page::Gamepad, st::Page::TexturesMods, st::Page::Sound}) {
            require(b.page.open_page(frame, page), "open");
            const std::string where = std::string(frame == PageFrame::Pause ? "pause" : "menu") + " > " + std::string(st::page_name(page));
            for (std::size_t i = 0; i < b.page.rows().size(); ++i) {
                b.wait(0.5);
                const Bounds all = drawn(b.page, font, false), text = drawn(b.page, font, true), bare = drawn(b.page, font, true, false);
                require(all.vertices > 0u && all.vertices % 3u == 0u && all.runs > 0u, where + ": a list of whole triangles");
                require(text.x0 >= l.area.x0 && text.x1 <= l.area.x1 && text.y0 >= l.area.y0 && text.y1 <= l.area.y1,
                    where + ", row " + b.name() + ": the text stays inside the frame (" + std::to_string(text.x0) + " to " + std::to_string(text.x1) + ")");
                require(bare.x0 >= l.area.x0 && bare.x1 <= l.area.x1 && bare.y0 >= l.area.y0 && bare.y1 <= l.area.y1, where + ", row " + b.name() + ": also with the legends from the font's pictures");
                if (frame == PageFrame::Pause) require(all.x0 >= l.area.x0 && all.x1 <= l.area.x1 && all.y0 >= l.area.y0 && all.y1 <= l.area.y1, where + ": nothing leaves the pause menu's box");
                // A value never reaches into its row's name (the selected row stands 10 pixels to the right).
                const auto &view = b.row();
                if (!view.value.empty()) {
                    std::string name = view.name;
                    if (view.lock != st::Lock::None) { name.push_back(' '); chili::page_append(name, chili::page_glyph::lock_closed); }
                    const float name_end = l.name_x + st::nudge_pixels + chili::page_text_width(font, name, b.page.name_style());
                    const float room = l.value_right - name_end - SettingsPage::value_gap;
                    const std::string shown = b.page.value_text(font, view, room);
                    require(chili::page_text_width(font, shown, b.page.value_style()) <= room + 0.01f || shown.size() <= 3u, where + ", row " + view.name + ": the value '" + shown + "' has room beside the name");
                }
                b.page.move(+1);
            }
            b.page.close();
        }
    }
    // A value too long for its row is cut short with two dots; the mark of a pending value stays.
    require(b.page.open_page(PageFrame::Pause, st::Page::TexturesMods), "open on Textures & mods in the pause box");
    {
        const auto &pack = b.page.rows()[1];
        require(pack.name == "HD pack" && pack.value == "beta_pack_with_a_very_long_folder_name", "the pack's folder name is the value");
        const std::string shown = b.page.value_text(font, pack, 120.0f);
        require(shown.size() < pack.value.size() && shown.substr(shown.size() - 2u) == ".." && chili::page_text_width(font, shown, b.page.value_style()) <= 120.0f, "cut to its room: '" + shown + "'");
        SettingsPage::RowView waiting = pack;
        waiting.pending = true;
        const std::string marked = b.page.value_text(font, waiting, 120.0f);
        require(marked.back() == '~' && marked.find("..") != std::string::npos, "a pending value keeps its chili: '" + marked + "'");
        require(b.page.value_text(font, pack, 2000.0f) == pack.value, "with room it is whole");
    }
    // The reason of a locked row is one line of the pause box, between the last row and the legends' buttons
    // (this test's letters are wider than the game's, so what fits here fits there).
    b.page.change(+1);
    require(b.page.reason() == "Change it in the main menu.", "a locked row tried in the pause box");
    {
        is(b.page.reason_lines(font).size(), std::size_t{1}, "the reason's lines in the pause box");
        for (const st::Lock lock : {st::Lock::NeedsRestart, st::Lock::PspCannotLook, st::Lock::NotWithPsp, st::Lock::InUse, st::Lock::SetOutside})
            require(chili::page_text_width(font, st::lock_text(lock), b.page.reason_style()) <= st::pause_layout.reason_width, "a reason a row can have in a mission fits one line: " + std::string(st::lock_text(lock)));
        require(chili::page_text_width(font, st::page_word::after_restart, b.page.reason_style()) <= st::pause_layout.reason_width, "and so does 'After a restart.'");
        const auto &l = st::pause_layout;
        const float reason_top = l.reason_y + 9.0f * l.reason_scale - l.reason_lift, reason_bottom = reason_top + 29.0f * l.reason_scale;
        const float last_row_bottom = l.row_y + l.row_pitch * static_cast<float>(l.visible - 1u) + 9.0f * l.scale - l.lift + 29.0f * l.scale;
        require(reason_top >= last_row_bottom && reason_bottom <= l.left_button.y0 + 0.5f, "the reason's line stands between the last row and the legends' buttons");
        const auto &m = st::main_layout;
        require(m.reason_y + 9.0f * m.reason_scale - m.reason_lift >= m.row_y + m.row_pitch * static_cast<float>(m.visible - 1u) + 9.0f * m.scale - m.lift + 29.0f * m.scale - 3.0f, "and in the menu frame under the seventh row");
    }
    b.page.close();
    // The top page in the pause box: its five lines stand in the middle of the box, as the pause menu's own do.
    require(b.page.open_page(PageFrame::Pause), "the top page in the pause box");
    {
        chili::PageDraw draw;
        b.wait(0.5);
        b.page.build(draw, font, [](std::uint64_t) { return true; });
        const auto list = draw.finish();
        const auto &l = st::pause_layout;
        const float middle = (l.area.x0 + l.area.x1) * 0.5f;
        // The fill copy of each row's text (not the black shadow copy): from its first glyph's left to its last one's right.
        std::map<long, std::pair<float, float>> spans;
        for (const auto &run : list->runs) {
            if (run.texture != chili::page_texture::font_fill) continue;
            for (std::uint32_t i = 0; i + 5u < run.vertex_count; i += 6u) {
                const auto &a = list->vertices[run.first_vertex + i], &c = list->vertices[run.first_vertex + i + 2u];
                if ((a.rgba & 0x00FFFFFFu) == 0u) continue;
                const long row = std::lround((a.y - l.row_y) / l.row_pitch);
                if (row < 0 || row > 4 || std::fabs(a.y - (l.row_y + l.row_pitch * static_cast<float>(row))) > 2.0f) continue;
                auto &span = spans.try_emplace(row, 1e9f, -1e9f).first->second;
                span.first = std::min(span.first, a.x); span.second = std::max(span.second, c.x);
            }
        }
        require(spans.size() == 5u, "five lines of text on the top page: " + std::to_string(spans.size()));
        for (const auto &[row, span] : spans) {
            const float centre = (span.first + span.second) * 0.5f - (static_cast<std::size_t>(row) == b.page.focus() ? st::nudge_pixels : 0.0f);
            require(std::fabs(centre - middle) < 1.5f, "row " + std::to_string(row) + " of the top page stands in the middle of the pause box (" + std::to_string(centre) + ")");
        }
    }
    b.page.close();
    // The Change legend without the game's left/right sprite in the cache: one D-pad with both arms lit where
    // the font's two D-pad pictures are one pad (the PSP's own, a controller's), two pictures side by side
    // for key caps.
    require(b.page.open_page(PageFrame::MainMenu, st::Page::KeyboardMouse), "open on a page whose first row has a value");
    {
        using Pictures = SettingsPage::DpadPictures;
        const auto glyph_quads = [&](Pictures pictures) {
            b.page.set_dpad_pictures(pictures);
            chili::PageDraw draw;
            b.page.build(draw, font, [](std::uint64_t key) { return key == chili::page_texture::font_fill || key == chili::page_texture::font_outline; });
            const auto list = draw.finish();
            const chili::PageGlyph *left_cell = font.find(chili::page_glyph::dpad_left), *right_cell = font.find(chili::page_glyph::dpad_right);
            // Quads of the fill sheet with texels of the two D-pad cells: where each stands (x0, x1) and which part
            // of its cell it shows (0 to 1 of D-pad left, 2 to 3 of D-pad right).
            std::vector<std::array<float, 4>> found;
            for (const auto &run : list->runs) {
                if (run.texture != chili::page_texture::font_fill) continue;
                for (std::uint32_t i = 0; i + 5u < run.vertex_count; i += 6u) {
                    const auto &a = list->vertices[run.first_vertex + i], &c = list->vertices[run.first_vertex + i + 2u];
                    for (const auto *cell : {left_cell, right_cell}) {
                        if (a.v != cell->v || a.u < cell->u - 1e-6f || c.u > cell->u + cell->u_width + 1e-6f) continue;
                        const float base = cell == left_cell ? 0.0f : 2.0f;
                        found.push_back({a.x, c.x, base + (a.u - cell->u) / cell->u_width, base + (c.u - cell->u) / cell->u_width});
                    }
                }
            }
            return found;
        };
        const auto two = glyph_quads(Pictures::Keys);
        require(two.size() == 2u, "key caps: two pictures: " + std::to_string(two.size()));
        require(std::fabs(two[0][1] - two[0][0] - 24.0f) < 0.01f && two[0][1] <= two[1][0] + 0.01f && std::fabs(two[0][3] - 1.0f) < 1e-4f && std::fabs(two[1][2] - 2.0f) < 1e-4f, "side by side, each whole");
        const auto one = glyph_quads(Pictures::Pad);
        require(one.size() == 2u, "one D-pad: two halves: " + std::to_string(one.size()));
        // The left half of the left-lit picture and the right half of the right-lit one meet in the middle of the sprite's place.
        const float middle = (st::main_layout.right_dpad.x0 + st::main_layout.right_dpad.x1) * 0.5f;
        require(std::fabs(one[0][1] - middle) < 0.01f && std::fabs(one[1][0] - middle) < 0.01f && std::fabs(one[0][2]) < 1e-4f && std::fabs(one[0][3] - 0.5f) < 1e-4f && std::fabs(one[1][2] - 2.5f) < 1e-4f && std::fabs(one[1][3] - 3.0f) < 1e-4f,
            "the left half of D-pad left and the right half of D-pad right, joined in the middle");
        require(std::fabs((one[1][1] - one[0][0]) - 24.0f * (32.0f + 2.0f * SettingsPage::button_glyph_grow) / 29.0f) < 0.01f, "at the size a button's picture has in a legend");
        // The PSP's own D-pad picture fills its cell (27 wide in the game's font): it is drawn as wide as the sprite's box.
        const auto psp = glyph_quads(Pictures::Psp);
        require(psp.size() == 2u && std::fabs(psp[0][1] - middle) < 0.01f && std::fabs(psp[1][0] - middle) < 0.01f, "the PSP's own pictures: two halves joined in the same place");
        require(std::fabs((psp[1][1] - psp[0][0]) - 24.0f * (32.0f + 2.0f * SettingsPage::dpad_glyph_grow_psp) / 29.0f) < 0.01f && 27.0f * (32.0f + 2.0f * SettingsPage::dpad_glyph_grow_psp) / 29.0f <= 32.1f,
            "a cell of 27 comes out no wider than the sprite's 32");
        // With the game's sprite in the cache the sprite is drawn, whatever the pictures are.
        b.page.set_dpad_pictures(Pictures::Pad);
        chili::PageDraw draw;
        b.page.build(draw, font, [](std::uint64_t) { return true; });
        const auto list = draw.finish();
        bool sprite = false;
        for (const auto &run : list->runs) sprite = sprite || run.texture == chili::page_texture::button_dpad_lr;
        require(sprite, "the game's own left/right sprite when it is in the cache");
        b.page.set_dpad_pictures(Pictures::Keys);
    }
    b.page.close();
    require(b.page.open_page(PageFrame::MainMenu, st::Page::Display), "open on Display in the menu");
    b.go("Auto limit");
    (void)b.live.set(st::get("Rendering", "InternalScale"), "4", PageFrame::MainMenu);
    b.wait(0.6);
    b.page.change(+1);
    is(b.page.reason_lines(font).size(), std::size_t{1}, "a short reason in the menu frame is one line");
    // The selected row: on its way to 10 pixels in a tenth of a second, the one before on its way back.
    b.page.move(-1);
    const std::size_t now = b.page.focus(), before = now + 1u;
    require(b.page.nudge(now) == 0.0f && b.page.nudge(before) == st::nudge_pixels, "at the moment the focus moves the old row is still out and the new one in");
    b.wait(0.05);
    require(std::fabs(b.page.nudge(now) - 5.0f) < 0.01f && std::fabs(b.page.nudge(before) - 5.0f) < 0.01f, "half way after a twentieth of a second");
    b.wait(0.06);
    require(b.page.nudge(now) == st::nudge_pixels && b.page.nudge(before) == 0.0f, "there after a tenth");
    // The pulse: dark red to bright red and back in two seconds.
    {
        float low = 2.0f, high = -1.0f;
        for (int i = 0; i < 60; ++i) { b.wait(1.0 / 30.0); const auto colour = b.page.pulse(); low = std::min(low, colour.r); high = std::max(high, colour.r); require(colour.g == 0.0f && colour.b == 0.0f, "the pulse is red"); }
        require(low < 0.51f && low >= 0.5f && high > 0.99f && high <= 1.0f, "between half and full red within two seconds");
    }
    // The logo beats as the game's does: a tenth larger and back, 16 frames of a thirtieth to the round.
    {
        float low = 2.0f, high = 0.0f;
        const double round_start = std::ceil(b.now / st::SettingsPage::logo_round_seconds) * st::SettingsPage::logo_round_seconds;
        b.now = round_start; b.page.tick(b.now);
        require(b.page.logo_scale() == 1.0f, "the round begins at the logo's own size");
        for (int i = 1; i < 16; ++i) {
            b.now = round_start + i / 30.0; b.page.tick(b.now);
            const float scale = b.page.logo_scale();
            low = std::min(low, scale); high = std::max(high, scale);
            if (i == 9) require(std::fabs(scale - 1.1f) < 0.001f, "largest after 0.3 seconds");
            if (i == 15) require(scale == 1.0f, "and its own size again before the round ends");
        }
        require(low >= 1.0f && high <= 1.1001f, "between its own size and a tenth more");
    }
    b.page.close();
}
}

int main(int argc, char **argv) {
    try {
        if (argc != 2) { std::cerr << "usage: chili_settings_page_test <scratch folder>\n"; return 2; }
        const std::filesystem::path scratch = argv[1];
        pages_and_pad(scratch);
        values(scratch);
        locks_and_defaults(scratch);
        pointer(scratch);
        drawing(scratch);
        if (failures) { std::cerr << failures << " of " << checks << " checks FAILED\n"; return 1; }
        std::cout << "settings page: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
