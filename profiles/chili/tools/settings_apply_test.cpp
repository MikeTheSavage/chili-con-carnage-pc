// The settings while the game runs: PcInput::reconfigure (pc_input.hpp) and settings::Live
// (settings_apply.hpp), which the game's own settings page works through.
//
//   chili_settings_apply_test <scratch folder>
//
// - New input settings while the game runs keep what the player has in hand: the pad finder and the pad in
//   use (unless Index or Enabled changed), the input line's switch, and the prompt pictures are told only
//   when something they show changed. A held mouse is let go when the mouse is switched off. PlayStation pads
//   are dropped and taken again as the setting says.
// - Every row of the page that is an input key is in force at once, through the game's own reader of the
//   file, and the file holds the value afterwards with every other line as it was.
// - What takes a restart is written and shows as pending; in a mission such a row is locked. The other
//   locks. The rows over more than one key: Quality, Textures and HD pack (the launcher's own lines for the
//   same folders are the expectation), Defaults.
// - The file is read again when the page opens and what another writer put there is in force afterwards.
// NO REAL PAD WAS EVER CONNECTED TO THE PC THIS WAS WRITTEN ON: the pads are XINPUT_STATE values and
// made-up PlayStation pads, as in pc_input_test.cpp.
#include "settings_apply.hpp"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {
namespace st = chili::settings;
using chili::PageFrame;
unsigned checks, failures;
const bool keep_going = [] { const char *value = std::getenv("CHILI_TEST_KEEP_GOING"); return value && *value == '1'; }();
void require(bool ok, const std::string &what) {
    ++checks;
    if (ok) return;
    if (!keep_going) throw std::runtime_error(what);
    ++failures;
    std::cerr << "FAILED CHECK: " << what << '\n';
}
constexpr std::uint32_t cross = 0x4000u;
struct Fixture {
    std::set<int> keys;
    std::optional<XINPUT_STATE> pad;      // the pad in XInput slot 1
    bool focused{true}, grabbed{};
    double now{100.0};
    std::vector<std::string> log;
    struct Motors { unsigned pad; std::uint16_t slow, fast; };
    std::vector<Motors> motors;
    chili::PcInput input;
    explicit Fixture(chili::PcInputConfig config = chili::PcInputConfig::defaults()) {
        input.configure(std::move(config));
        chili::PcInputHost host;
        host.focused = [this] { return focused; };
        host.key_down = [this](int key) { return keys.count(key) != 0; };
        host.pad = [this](unsigned slot, XINPUT_STATE &state) { if (slot != 1u || !pad) return false; state = *pad; return true; };
        host.seconds = [this] { return now; };
        host.pad_worker = false;
        host.grab = [this](bool on) { grabbed = on; };
        host.vibrate = [this](unsigned index, std::uint16_t slow, std::uint16_t fast) { motors.push_back({index, slow, fast}); return true; };
        host.log = [this](const std::string &line) { log.push_back(line); };
        input.set_host(std::move(host));
    }
    bool logged(std::string_view part) const { for (const auto &line : log) if (line.find(part) != std::string::npos) return true; return false; }
    chili::ControllerInput sample() { now += 1.0 / 30.0; return input.sample(); }
    // The pad of slot 1 with A held, found and in use.
    void use_pad() {
        XINPUT_STATE state{};
        state.Gamepad.wButtons = XINPUT_GAMEPAD_A;
        pad = state;
        input.scan_pads();
        (void)sample();
    }
    // A bound key held for one sample: the keyboard is the device used last.
    void use_keyboard() {
        if (pad) pad->Gamepad.wButtons = 0u;
        now += 1.0;
        keys.insert(VK_SPACE); (void)sample(); keys.erase(VK_SPACE); (void)sample();
    }
};
chili::HidPadInfo playstation_pad() {
    chili::HidPadInfo info;
    info.vid = chili::hid_sony_vendor; info.pid = 0x0CE6u; info.model = chili::hid_pad_model(info.vid, info.pid);
    info.name = "Wireless Controller"; info.buttons = 15u; info.axes = 6u; info.hat = true;
    return info;
}
std::uint32_t revision() { return chili::input_device_state().bindings_revision.load(); }

// ---- PcInput::reconfigure --------------------------------------------------------------------------

void reconfigure() {
    {
        Fixture f;
        f.use_pad();
        require(f.input.active_pad() != nullptr && f.input.device() == chili::InputDevice::Gamepad && (f.sample().buttons & cross), "a pad in slot 1 is found, in use and presses Cross");
        // A value that is no binding: nothing is lost, nothing is painted.
        const auto before = revision();
        const auto lines = f.log.size();
        auto config = f.input.config();
        config.deadzone_left = 0.5f; config.rumble = 0.25f; config.mouse_degrees_per_count = 0.3f;
        const auto done = f.input.reconfigure(config);
        require(!done.finder_rebuilt && !done.prompts_published && !done.mouse_released && !done.hid_started && !done.hid_stopped, "a dead zone, the rumble and a sensitivity: nothing else is touched");
        require(f.input.active_pad() != nullptr && (f.sample().buttons & cross), "the pad in use stays in use, without a new search, and still presses");
        require(f.input.config().deadzone_left == 0.5f && f.input.config().mouse_degrees_per_count == 0.3f, "the values are in force");
        require(revision() == before && f.log.size() == lines, "the prompt pictures are not told and nothing is logged");
        // configure(), the start's way, drops the finder: the pad is gone until it is looked for again.
        {
            Fixture g;
            g.use_pad();
            g.input.configure(g.input.config());
            (void)g.sample();
            require(g.input.active_pad() == nullptr && g.logged("XInput slot 1 removed"), "configure() loses the pad until the next search (what reconfigure avoids)");
        }
        // Index: the pads are looked for again, in that slot only.
        config.pad_index = 1;
        require(f.input.reconfigure(config).finder_rebuilt, "Index changed: the finder is made again");
        f.input.scan_pads();
        require((f.sample().buttons & cross) && f.input.active_pad() != nullptr, "and the pad of that slot is in use after the search");
        config.pad_index = 0;
        (void)f.input.reconfigure(config);
        f.input.scan_pads();
        (void)f.sample();
        require(f.input.active_pad() == nullptr && !(f.sample().buttons & cross), "Index names another slot: the pad of slot 1 is not read");
        config.pad_index = -1;
        (void)f.input.reconfigure(config);
        f.input.scan_pads();
        require((f.sample().buttons & cross), "Index=auto again: the pad is back");
        // Enabled=false: the motors stop, no pad is in use; true: found again.
        f.input.rumble(1.0f, 1.0f);
        require(!f.motors.empty() && f.motors.back().slow != 0u, "the pad's motors run");
        config.pad_enabled = false;
        const auto off = f.input.reconfigure(config);
        require(off.finder_rebuilt && f.input.active_pad() == nullptr && f.logged("gamepad: switched off"), "the gamepad switched off: no pad is in use");
        require(f.motors.back().slow == 0u && f.motors.back().fast == 0u, "and its motors were stopped");
        require(!(f.sample().buttons & cross), "a switched-off pad presses nothing");
        config.pad_enabled = true;
        (void)f.input.reconfigure(config);
        f.input.scan_pads();
        require((f.sample().buttons & cross) && f.input.active_pad() != nullptr, "switched on again: found and in use");
    }
    {
        // What the prompt pictures show: told when it changes, and only then.
        Fixture f;
        auto config = f.input.config();
        auto before = revision();
        config.prompts = chili::PromptsSetting::Keyboard;
        require(f.input.reconfigure(config).prompts_published && revision() == before + 1u && chili::input_device_state().prompts.load() == chili::PromptsSetting::Keyboard, "Prompts changed: published");
        config.glyphs = chili::GlyphsSetting::PlayStation;
        require(f.input.reconfigure(config).prompts_published && chili::input_device_state().glyphs.load() == chili::GlyphsSetting::PlayStation, "Glyphs changed: published");
        (void)config.apply("[Input.Keyboard]\nCross=P\n[Input.Gamepad]\nY=Triangle\n[Input.Mouse]\nMouse3=Triangle\n");
        require(f.input.reconfigure(config).prompts_published, "a binding changed: published");
        before = revision();
        require(!f.input.reconfigure(config).prompts_published && revision() == before, "the same again: not published");
        config.walk_scale = 0.4f; config.mouse_smoothing = 0.5f; config.pad_invert_y = true; config.trigger_threshold = 0.5f;
        require(!f.input.reconfigure(config).prompts_published && revision() == before, "values the pictures do not show: not published");
        config.mouse_enabled = false;
        require(f.input.reconfigure(config).prompts_published, "the mouse switched off changes the pictures (no mouse button is shown)");
        // The input line: its key's choice stays through a change of something else.
        LRESULT result{};
        (void)f.input.window_message(nullptr, WM_KEYDOWN, VK_F3, 0, result);
        require(f.input.show_input(), "F3 shows the input line");
        config.deadzone_right = 0.3f;
        (void)f.input.reconfigure(config);
        require(f.input.show_input(), "and it stays through a change of another setting");
        config.show_input = true;
        (void)f.input.reconfigure(config);
        (void)f.input.window_message(nullptr, WM_KEYDOWN, VK_F3, 0, result);
        require(!f.input.show_input(), "F3 hides it again");
        config.show_input = false;
        (void)f.input.reconfigure(config);
        require(!f.input.show_input(), "ShowInput=false leaves it hidden");
        config.show_input = true;
        (void)f.input.reconfigure(config);
        require(f.input.show_input(), "ShowInput=true shows it");
    }
    {
        // A held mouse is let go when the mouse is switched off, or is not to be captured any more.
        Fixture f;
        f.input.set_captured_for_test(true);
        auto config = f.input.config();
        config.mouse_degrees_per_count_y = 0.5f;
        require(!f.input.reconfigure(config).mouse_released && f.input.mouse_captured(), "a sensitivity: the mouse stays held");
        config.mouse_capture_on_click = false;
        require(f.input.reconfigure(config).mouse_released && !f.input.mouse_captured(), "Capture=never: the mouse is let go");
        config.mouse_capture_on_click = true;
        (void)f.input.reconfigure(config);
        f.input.set_captured_for_test(true);
        config.mouse_enabled = false;
        require(f.input.reconfigure(config).mouse_released && !f.input.mouse_captured(), "the mouse switched off: let go");
    }
    {
        // PlayStation pads: dropped when they are not to be read, taken again when they are.
        Fixture f;
        require(f.input.hid_arrived(0x77u, playstation_pad()), "a PlayStation pad arrives");
        chili::PadSample held;
        held.buttons = chili::PadSample::bit(chili::PadSample::A);
        f.input.hid_report(0x77u, held);
        require((f.sample().buttons & cross) && f.input.active_pad() && f.input.active_pad()->kind == chili::PadKind::PlayStation, "and is the pad in use");
        auto config = f.input.config();
        config.playstation_pads = false;
        const auto done = f.input.reconfigure(config);
        require(done.hid_stopped && !done.finder_rebuilt && f.input.active_pad() == nullptr && f.logged("is not read any more"), "PlayStationPads=false: the pad is dropped");
        require(!f.input.hid_arrived(0x77u, playstation_pad()) && !(f.sample().buttons & cross), "and not taken while the setting is off");
        config.playstation_pads = true;
        require(f.input.reconfigure(config).hid_started && f.input.hid_arrived(0x77u, playstation_pad()), "PlayStationPads=true: taken again");
        f.input.hid_report(0x77u, held);
        require((f.sample().buttons & cross), "and it presses again");
        config.pad_index = 2;
        require(f.input.reconfigure(config).hid_stopped && f.input.active_pad() == nullptr, "Index names an XInput slot: PlayStation pads are not read");
    }
}

// ---- settings::Live --------------------------------------------------------------------------------

std::string file_text(const std::filesystem::path &file) { return chili::ini_file::read(file).value_or("<cannot be read>"); }
std::vector<std::string> lines_of(const std::string &text) {
    std::vector<std::string> out;
    std::istringstream stream(text);
    for (std::string line; std::getline(stream, line);) out.push_back(line);
    return out;
}
bool has_line(const std::filesystem::path &file, const std::string &line) {
    for (const auto &have : lines_of(file_text(file))) if (have == line) return true;
    return false;
}
// The lines of `after` that are not lines of `before`, and how many lines of `before` are gone.
std::pair<std::vector<std::string>, std::size_t> changed_lines(const std::string &before, const std::string &after) {
    std::multiset<std::string> old;
    for (const auto &line : lines_of(before)) old.insert(line);
    std::vector<std::string> fresh;
    for (const auto &line : lines_of(after)) { const auto found = old.find(line); if (found == old.end()) fresh.push_back(line); else old.erase(found); }
    return {fresh, old.size()};
}
// The game's own first settings file, as far as this test needs its other sections.
const char *other_sections =
    "\n[Mods]\n; Folder=mods\n; Enabled=first_mod,second_mod\n; DumpTextures=false\n"
    "\n[Game]\n; FrameLock=1\n; Camera=tight\n"
    "\n[Rendering]\n; Preset=high\n; InternalScale=auto\n; AntiAliasing=fxaa\n; Anisotropy=16\n; TextureFilter=game\n; SSAO=off\n; Glow=smooth\n; Fullscreen=false\n; EarlyPresent=true\n"
    "\n[Audio]\n; BufferMs=70\n";
std::filesystem::path fresh_folder(const std::filesystem::path &scratch, const char *name, bool mods = true) {
    const auto folder = scratch / name;
    std::error_code status;
    std::filesystem::remove_all(folder, status);
    std::filesystem::create_directories(folder);
    if (!chili::ini_file::write(folder / "ChiliNative.ini", std::string(chili::default_input_ini()) + other_sections)) throw std::runtime_error("the test cannot write its settings file");
    if (mods) {
        // The folders of scripts/check_launcher_settings.ps1: two texture packs, a mod with a value, an empty folder.
        for (const char *pack : {"alpha_pack", "beta_pack"}) { std::filesystem::create_directories(folder / "mods" / pack / "textures"); std::ofstream(folder / "mods" / pack / "textures" / "0000000000000001.png") << "x"; }
        std::filesystem::create_directories(folder / "mods" / "camera_mod");
        std::ofstream(folder / "mods" / "camera_mod" / "properties.ini") << "[ComboSystem]\n";
        std::filesystem::create_directories(folder / "mods" / "empty_mod");
        std::filesystem::create_directories(folder / "mods" / ".hidden");
    }
    return folder;
}
st::Host host_for(const std::filesystem::path &folder, chili::PcInput *input) {
    st::Host host;
    host.ini = folder / "ChiliNative.ini";
    host.input = input;
    host.mods_folder = folder / "mods";
    host.started = {{"Rendering/Fullscreen", "false"}, {"Rendering/InternalScale", "auto"}, {"Rendering/EarlyPresent", "true"}, {"Rendering/Preset", "high"},
        {"Rendering/AntiAliasing", "fxaa"}, {"Rendering/MSAA", "1"}, {"Rendering/TextureFilter", "game"}, {"Rendering/Anisotropy", "16"}, {"Rendering/SSAO", "off"}, {"Rendering/Glow", "smooth"},
        {"Game/FrameLock", "1"}, {"Game/Camera", "tight"}, {"Audio/BufferMs", "70"}, {"Mods/Enabled", "alpha_pack,beta_pack,camera_mod,empty_mod"}, {"Mods/DumpTextures", "false"}};
    return host;
}

void input_rows(const std::filesystem::path &scratch) {
    const auto folder = fresh_folder(scratch, "input");
    const auto ini = folder / "ChiliNative.ini";
    std::vector<std::string> problems;
    Fixture f(chili::PcInputConfig::load(ini, problems, false));
    require(problems.empty(), "the game reads the test's settings file without a complaint");
    bool allow_exact = false, exact = true;
    auto host = host_for(folder, &f.input);
    host.exact_turn = [&](bool on) { if (!allow_exact) return false; exact = on; return true; };
    st::Live live(host);
    const auto &turn = st::get("Input.Mouse", "SensitivityX");

    // One value: in force at once, written, and nothing else of the file touched.
    require(live.value(turn) == "0.08" && live.running(turn) == "0.08" && !live.pending(turn) && live.shown(turn) == "0.08", "the row shows the file's value, which is the running one");
    const std::string before = file_text(ini);
    auto out = live.set(turn, "0.2", PageFrame::MainMenu);
    require(out.ok && out.applied && !out.pending && out.written && out.lock == st::Lock::None, "a value of a live key is taken, in force and written");
    require(out.line == "Input.Mouse/SensitivityX=0.2 applied live, written", "and says so: " + out.line);
    require(f.input.config().mouse_degrees_per_count == 0.2f, "the game's bindings hold the value");
    const auto diff = changed_lines(before, file_text(ini));
    require(diff.first == std::vector<std::string>{"SensitivityX=0.2"} && diff.second == 1u, "the file differs in that one line");
    require(live.value(turn) == "0.2" && live.shown(turn) == "0.20" && live.running(turn) == "0.2", "the row shows the new value");
    require(live.step(turn, 1, PageFrame::MainMenu).ok && live.value(turn) == "0.21" && live.step(turn, -1, PageFrame::MainMenu).ok && live.step(turn, -1, PageFrame::MainMenu).ok && live.value(turn) == "0.19", "steps to the right and to the left");
    // A slider that is dragged: in force at every step, written when it is let go.
    out = live.set(turn, "0.5", PageFrame::MainMenu, false);
    require(out.ok && out.applied && !out.written && live.unsaved() && f.input.config().mouse_degrees_per_count == 0.5f && has_line(ini, "SensitivityX=0.19"), "not written while dragged: in force, the file as it was");
    require(out.line == "Input.Mouse/SensitivityX=0.5 applied live, not written yet", out.line);
    require(live.value(turn) == "0.5", "the row shows the dragged value");
    require(live.flush().written && !live.unsaved() && has_line(ini, "SensitivityX=0.5"), "let go: written");
    // What is not a value of the key is refused, and nothing changes.
    out = live.set(turn, "7", PageFrame::MainMenu);
    require(!out.ok && !out.error.empty() && f.input.config().mouse_degrees_per_count == 0.5f && has_line(ini, "SensitivityX=0.5"), "a number outside the slider is refused: " + out.line);
    out = live.set(st::get("Input", "Prompts"), "banana", PageFrame::MainMenu);
    require(!out.ok && out.line == "Input/Prompts=banana refused: Prompts cannot be banana", out.line);

    // Every row of the page that is an input key: a step to the right is in force and written, the running
    // value is the row's value, and a step back gives the value it had.
    unsigned rows = 0u;
    allow_exact = true;
    for (const st::Page page : {st::Page::Controls, st::Page::KeyboardMouse, st::Page::Gamepad}) {
        for (const auto &row : st::rows_of(page)) {
            if (row.kind != st::RowKind::Setting || !live.applies_live(*row.setting)) continue;
            const auto &setting = *row.setting;
            // The pad must not be the device in use for "Use the gamepad"; no pad is connected here.
            require(live.locked(setting, PageFrame::MainMenu) == st::Lock::None && live.locked(setting, PageFrame::Pause) == st::Lock::None, setting.id() + " is not locked, in the main menu or in a mission");
            const std::string was = live.value(setting);
            // To the right, or to the left for a slider that stands at its highest value (Rumble).
            const int direction = st::equivalent(st::stepped(setting, was, 1), was) ? -1 : 1;
            const auto step = live.step(setting, direction, PageFrame::Pause);
            const std::string now = live.value(setting);
            require(step.ok && step.applied && step.written && !step.pending && now != was, setting.id() + ": a step is taken, in force and written (" + step.line + ")");
            require(st::equivalent(live.running(setting), now) && chili::IniDocument::parse(file_text(ini)).get(setting.section, setting.key) == now, setting.id() + ": the game runs " + now + " and the file holds it");
            const auto back = live.set(setting, was, PageFrame::Pause);
            require(back.ok && back.applied && st::equivalent(live.value(setting), was) && st::equivalent(live.running(setting), was), setting.id() + ": and back to " + was);
            ++rows;
        }
    }
    require(rows == 26u, "the 26 input rows of the page (12 keyboard and mouse, 12 gamepad, Prompts, Button pictures): " + std::to_string(rows));
    require(exact, "the two-stick layer's switch ended where it began");
    // The values the game's bindings hold are the ones the rows were set to, to the last one.
    {
        std::vector<std::string> again;
        const auto read = chili::PcInputConfig::load(ini, again, false);
        require(again.empty() && read.same_as(f.input.config()), "a game started on the file now would run with exactly the bindings in force");
    }

    // Prompts: the prompt pictures are told.
    auto before_revision = revision();
    out = live.set(st::get("Input", "Prompts"), "psp", PageFrame::MainMenu);
    require(out.applied && chili::input_device_state().prompts.load() == chili::PromptsSetting::Psp && revision() == before_revision + 1u, "Prompts=psp: published to the prompt pictures");
    out = live.set(st::get("Input.Gamepad", "Glyphs"), "playstation", PageFrame::MainMenu);
    require(out.applied && chili::input_device_state().glyphs.load() == chili::GlyphsSetting::PlayStation && revision() == before_revision + 2u, "Button pictures: published");
    before_revision = revision();
    (void)live.set(st::get("Input.Gamepad", "Rumble"), "0.5", PageFrame::MainMenu);
    require(revision() == before_revision, "Rumble: the pictures are not painted again");
    (void)live.set(st::get("Input", "Prompts"), "auto", PageFrame::MainMenu);
    (void)live.set(st::get("Input.Gamepad", "Glyphs"), "auto", PageFrame::MainMenu);

    // ExactTurn where the two-stick layer cannot switch while the game runs: written, waits for a restart.
    const auto &exact_turn = st::get("Input", "ExactTurn");
    allow_exact = false;
    out = live.set(exact_turn, "false", PageFrame::MainMenu);
    require(out.ok && !out.applied && out.pending && out.written && f.input.config().exact_turn && exact && live.pending(exact_turn), "ExactTurn that cannot switch: pending (" + out.line + ")");
    require(out.line == "Input/ExactTurn=false waits for a restart, written", out.line);
    allow_exact = true;
    out = live.set(exact_turn, "false", PageFrame::MainMenu);
    require(out.applied && !out.pending && !f.input.config().exact_turn && !exact, "ExactTurn that can: switched");
    (void)live.set(exact_turn, "true", PageFrame::MainMenu);
    require(exact && f.input.config().exact_turn, "and back");

    // The control style takes a restart: the bindings in force keep theirs, the rows that go with the other
    // style are locked at once (the launcher's rule), and Camera's default follows the chosen style.
    const auto &scheme = st::get("Input", "Scheme"), &camera = st::get("Game", "Camera");
    require(!live.applies_live(scheme) && live.value(camera) == "tight" && !live.pending(camera), "Camera: tight with the modern style");
    out = live.set(scheme, "psp", PageFrame::MainMenu);
    require(out.ok && !out.applied && out.pending && out.written && f.input.config().scheme == chili::InputScheme::DualAnalog && has_line(ini, "Scheme=psp"), "Scheme=psp: written, pending, the game's style as it was");
    require(live.locked(st::get("Input.Mouse", "SensitivityY"), PageFrame::MainMenu) == st::Lock::PspCannotLook && live.locked(st::get("Input.Gamepad", "LookSensitivity"), PageFrame::MainMenu) == st::Lock::PspCannotLook &&
            live.locked(st::get("Input.Keyboard", "LookKeySpeed"), PageFrame::MainMenu) == st::Lock::NotWithPsp, "the look rows are locked with PSP original");
    out = live.step(st::get("Input.Mouse", "SensitivityY"), 1, PageFrame::MainMenu);
    require(!out.ok && out.lock == st::Lock::PspCannotLook && out.error == "Not used with PSP original." && out.line == "Input.Mouse/SensitivityY=0.09 refused: Not used with PSP original.", out.line);
    require(live.value(camera) == "original" && live.pending(camera), "Camera's default is original with PSP original: it waits for the restart too");
    (void)live.set(scheme, "dual_analog", PageFrame::MainMenu);
    require(!live.pending(scheme) && !live.pending(camera) && live.locked(st::get("Input.Mouse", "SensitivityY"), PageFrame::MainMenu) == st::Lock::None, "the modern style again: nothing pending, nothing locked");

    // "Use the gamepad" is locked while the pad is what the player has in hand.
    const auto &pad_on = st::get("Input.Gamepad", "Enabled");
    f.use_pad();
    require(f.input.device() == chili::InputDevice::Gamepad && live.locked(pad_on, PageFrame::MainMenu) == st::Lock::InUse, "the pad was used last: Use the gamepad is locked");
    out = live.set(pad_on, "false", PageFrame::MainMenu);
    require(!out.ok && out.lock == st::Lock::InUse && f.input.config().pad_enabled && out.line == "Input.Gamepad/Enabled=false refused: In use.", out.line);
    f.use_keyboard();
    require(f.input.device() == chili::InputDevice::KeyboardMouse && live.locked(pad_on, PageFrame::MainMenu) == st::Lock::None, "the keyboard was used last: not locked");
    out = live.set(pad_on, "false", PageFrame::MainMenu);
    require(out.applied && !f.input.config().pad_enabled && f.input.active_pad() == nullptr, "and the gamepad can be switched off");
    require(live.locked(pad_on, PageFrame::MainMenu) == st::Lock::None && live.set(pad_on, "true", PageFrame::MainMenu).applied, "and on again (a switched-off pad is never in use)");

    // Another writer (the launcher beside the game) changed the file: read again when the page opens.
    const std::vector<chili::IniChange> theirs{{"Input.Mouse", "InvertY", "true"}, {"Input.Gamepad", "DeadzoneLeft", "0.4"}, {"Rendering", "InternalScale", "2"}};
    require(chili::ini_change_file(ini, theirs).ok, "the launcher writes three keys");
    require(!f.input.config().mouse_invert_y && live.value(st::get("Input.Mouse", "InvertY")) == "false", "before the page opens the game knows nothing of it");
    const auto told = live.reload();
    require(told.size() == 1u && f.input.config().mouse_invert_y && f.input.config().deadzone_left == 0.4f, "after it the input keys are in force: " + (told.empty() ? std::string("no line") : told.front()));
    require(live.value(st::get("Input.Mouse", "InvertY")) == "true" && live.value(st::get("Rendering", "InternalScale")) == "2" && live.pending(st::get("Rendering", "InternalScale")), "the rows show the file, the resolution as pending");
    require(live.reload().empty(), "a file that has not changed puts nothing in force");
    // The file gone while the game runs (another writer's swap failed half way, a scanner took it): the
    // page's next look at it puts no default in force, and its next change writes the whole file again,
    // not the changed line alone.
    {
        const std::string whole = file_text(ini);
        const auto in_force = f.input.config();
        require(DeleteFileW(ini.c_str()) != FALSE, "the test removed the settings file");
        require(live.reload().empty() && f.input.config().same_as(in_force) && live.value(st::get("Input.Mouse", "InvertY")) == "true" && live.value(st::get("Rendering", "InternalScale")) == "2",
            "the file gone when the page opens: nothing put in force, the rows show what was read last");
        out = live.set(turn, "0.44", PageFrame::MainMenu);
        require(out.ok && out.applied && out.written && out.remade, "a change with the file gone: written (" + out.line + ")");
        require(out.line == "Input.Mouse/SensitivityX=0.44 applied live, written (the settings file was gone: written again whole)", out.line);
        const auto again = changed_lines(whole, file_text(ini));
        require(again.first == std::vector<std::string>{"SensitivityX=0.44"} && again.second == 1u, "the whole file is back, different in the one changed line");
        out = live.set(turn, "0.45", PageFrame::MainMenu);
        require(out.ok && out.written && !out.remade && has_line(ini, "SensitivityX=0.45"), "and the next change is an ordinary one");
    }
    // A mouse speed above the row's range, written by hand (the game's reader takes up to 5, the row ends at
    // 1): a step to the right leaves it, with no complaint; a step to the left goes to the row's highest value.
    {
        require(chili::ini_change_file(ini, chili::IniChange{"Input.Mouse", "SensitivityX", "1.5"}).ok && live.reload().size() == 1u && f.input.config().mouse_degrees_per_count == 1.5f, "a mouse speed of 1.5 from the file is in force");
        require(live.value(turn) == "1.5" && live.shown(turn) == "1.50", "and the row shows it");
        out = live.step(turn, 1, PageFrame::MainMenu);
        require(out.ok && out.error.empty() && out.line.empty() && live.value(turn) == "1.5" && f.input.config().mouse_degrees_per_count == 1.5f && has_line(ini, "SensitivityX=1.5"), "a step to the right leaves 1.5 alone");
        out = live.step(turn, -1, PageFrame::MainMenu);
        require(out.ok && out.applied && out.written && live.value(turn) == "1" && has_line(ini, "SensitivityX=1"), "a step to the left goes to the row's end: " + out.line);
        (void)live.set(turn, "0.45", PageFrame::MainMenu);
    }
    // A run that does not write: the change is in force and the file is not touched.
    {
        auto silent = host_for(folder, &f.input);
        silent.writable = false;
        st::Live quiet(silent);
        const std::string as_it_was = file_text(ini);
        out = quiet.set(turn, "0.33", PageFrame::MainMenu);
        require(out.ok && out.applied && !out.written && f.input.config().mouse_degrees_per_count == 0.33f && file_text(ini) == as_it_was, "a run that does not write its file: in force, not written (" + out.line + ")");
    }
}

void other_rows(const std::filesystem::path &scratch) {
    const auto folder = fresh_folder(scratch, "other");
    const auto ini = folder / "ChiliNative.ini";
    st::Live live(host_for(folder, nullptr));
    const auto &scale = st::get("Rendering", "InternalScale"), &preset = st::get("Rendering", "Preset"), &aa = st::get("Rendering", "AntiAliasing"),
               &aniso = st::get("Rendering", "Anisotropy"), &ssao = st::get("Rendering", "SSAO"), &glow = st::get("Rendering", "Glow"), &buffer = st::get("Audio", "BufferMs"), &frames = st::get("Game", "FrameLock");

    // A key that takes a restart: written at once, pending until it runs, locked in a mission.
    require(live.value(scale) == "auto" && live.shown(scale) == "Auto" && !live.pending(scale), "the resolution is Auto");
    auto out = live.set(scale, "4", PageFrame::MainMenu);
    require(out.ok && !out.applied && out.pending && out.written && has_line(ini, "InternalScale=4") && !has_line(ini, "; InternalScale=auto"), "Resolution: written into its line, pending");
    require(out.line == "Rendering/InternalScale=4 waits for a restart, written" && live.shown(scale) == "1920 x 1088" && live.running(scale) == "auto", out.line);
    require(live.locked(preset, PageFrame::MainMenu) == st::Lock::OnlyWithAuto && !live.set(preset, "low", PageFrame::MainMenu).ok, "Auto limit is locked while the resolution is not Auto");
    out = live.set(scale, "auto", PageFrame::MainMenu);
    require(out.ok && !out.pending && !out.applied && out.line == "Rendering/InternalScale=auto as the game runs it, written" && has_line(ini, "InternalScale=auto"), "back to the running value: nothing pending (" + out.line + ")");
    require(live.locked(preset, PageFrame::MainMenu) == st::Lock::None && live.set(preset, "medium", PageFrame::MainMenu).pending && live.shown(preset) == "1920 x 1088", "Auto limit with Auto");
    for (const auto *setting : {&scale, &preset, &aa, &aniso, &ssao, &buffer, &frames, &st::get("Rendering", "Fullscreen"), &st::get("Game", "Camera"), &st::get("Input", "Scheme"), &st::get("Mods", "DumpTextures")})
        require(live.locked(*setting, PageFrame::Pause) == st::Lock::NeedsRestart, setting->id() + " is locked in a mission");
    out = live.step(frames, 1, PageFrame::Pause);
    require(!out.ok && out.lock == st::Lock::NeedsRestart && out.line == "Game/FrameLock=2 refused: Change it in the main menu." && !has_line(ini, "FrameLock=2"), out.line);
    // Screen with a window that can be switched: the row is the window's own state (F11 changes it too), a
    // change switches the window at once, in a mission as well, and the file gets how the next start begins.
    {
        const auto screen_folder = fresh_folder(scratch, "screen");
        const auto screen_ini = screen_folder / "ChiliNative.ini";
        bool window_full = false;
        unsigned switched = 0u;
        auto with_window = host_for(screen_folder, nullptr);
        with_window.fullscreen = [&] { return window_full; };
        with_window.set_fullscreen = [&](bool on) { window_full = on; ++switched; };
        st::Live windowed(with_window);
        const auto &screen = st::get("Rendering", "Fullscreen");
        require(windowed.screen_live() && windowed.applies_live(screen) && windowed.locked(screen, PageFrame::Pause) == st::Lock::None && windowed.locked(screen, PageFrame::MainMenu) == st::Lock::None,
            "Screen is live and not locked, in the main menu or in a mission");
        require(windowed.value(screen) == "false" && windowed.shown(screen) == "Window" && !windowed.pending(screen), "the row shows the window");
        auto screen_out = windowed.step(screen, 1, PageFrame::Pause);
        require(screen_out.ok && screen_out.applied && !screen_out.pending && screen_out.written && window_full && switched == 1u, "a step switches the window at once: " + screen_out.line);
        require(screen_out.line == "Rendering/Fullscreen=true applied live, written" && has_line(screen_ini, "Fullscreen=true") && windowed.shown(screen) == "Full screen" && !windowed.pending(screen), screen_out.line);
        // F11 outside the page: the row follows the window, and nothing is pending although the file says otherwise.
        window_full = false;
        require(windowed.value(screen) == "false" && windowed.shown(screen) == "Window" && !windowed.pending(screen) && has_line(screen_ini, "Fullscreen=true"), "after F11 the row shows the window as it is");
        // The row's own step is from the window's state: to full screen again; the file says so already.
        screen_out = windowed.step(screen, 1, PageFrame::MainMenu);
        require(screen_out.ok && screen_out.applied && window_full && switched == 2u && has_line(screen_ini, "Fullscreen=true"), "a step from the window's own state: " + screen_out.line);
        screen_out = windowed.set(screen, "false", PageFrame::MainMenu);
        require(screen_out.applied && !window_full && switched == 3u && has_line(screen_ini, "Fullscreen=false"), "and back to the window, written");
        screen_out = windowed.set(screen, "false", PageFrame::MainMenu);
        require(screen_out.ok && switched == 3u, "the state the window has already switches nothing");
        // Defaults of Display: back to a window when it is in full screen.
        (void)windowed.set(screen, "true", PageFrame::MainMenu);
        require(window_full && windowed.reset(st::Page::Display, PageFrame::MainMenu).ok && !window_full, "Defaults puts the window back");
    }
    // A value that is the default of a key the file leaves out is nothing to write (the launcher's rule).
    const std::string before = file_text(ini);
    out = live.set(buffer, "70", PageFrame::MainMenu);
    require(out.ok && out.written && file_text(ini) == before, "the default of a key the file leaves out: the file is not written");
    out = live.step(buffer, 1, PageFrame::MainMenu);
    require(out.pending && has_line(ini, "BufferMs=80") && live.shown(buffer) == "80 ms", "Sound buffer: one step is 10 ms");

    // A key the file leaves out whose default this run does not run (a scripted run's 30 frames per second):
    // the default is written out when it is chosen, so the next start takes what the row shows.
    {
        const auto scripted = fresh_folder(scratch, "scripted", false);
        auto host = host_for(scripted, nullptr);
        for (auto &entry : host.started) if (entry.first == "Game/FrameLock") entry.second = "2";
        st::Live run(host);
        require(run.value(frames) == "1" && run.running(frames) == "2" && run.pending(frames), "no line: the row shows the default, this run has the other value");
        const auto pinned = run.set(frames, "1", PageFrame::MainMenu);
        require(pinned.ok && pinned.pending && pinned.written && has_line(scripted / "ChiliNative.ini", "FrameLock=1"), "the default is written out: " + pinned.line);
    }
    // Anti-aliasing with an older MSAA line in the file: read as the game reads the two, kept in step.
    require(live.value(aa) == "fxaa", "no line: Light");
    require(chili::ini_change_file(ini, chili::IniChange{"Rendering", "MSAA", "4"}).ok && live.reload().empty() && live.value(aa) == "msaa4" && live.pending(aa), "MSAA=4 alone is 4x");
    out = live.set(aa, "fxaa", PageFrame::MainMenu);
    require(out.ok && has_line(ini, "MSAA=1") && live.value(aa) == "fxaa" && !live.pending(aa), "Light: the MSAA line follows (MSAA=1), and the game will read Light");
    out = live.set(aa, "msaa8", PageFrame::MainMenu);
    require(has_line(ini, "AntiAliasing=msaa8") && has_line(ini, "MSAA=8") && live.value(aa) == "msaa8" && live.shown(aa) == "8x" && out.pending, "8x: both lines say so");
    require(live.step(aa, 1, PageFrame::MainMenu).ok && live.value(aa) == "off" && has_line(ini, "MSAA=1") && has_line(ini, "AntiAliasing=off"), "a step from 8x wraps to Off");
    (void)live.set(aa, "fxaa", PageFrame::MainMenu);

    // The glow: written at once, in force at the next start, locked in a mission like the other looks.
    require(live.value(glow) == "smooth" && live.shown(glow) == "Smooth" && live.running(glow) == "smooth" && !live.pending(glow) && !live.applies_live(glow), "the glow is Smooth");
    out = live.set(glow, "game", PageFrame::MainMenu);
    require(out.ok && out.pending && !out.applied && out.written && out.line == "Rendering/Glow=game waits for a restart, written" && has_line(ini, "Glow=game") && live.shown(glow) == "Original" && live.pending(glow), out.line);
    require(live.quality() == "custom", "the glow changed by itself: Quality says Custom");
    out = live.step(glow, 1, PageFrame::MainMenu);
    require(out.ok && live.value(glow) == "off" && has_line(ini, "Glow=off") && live.shown(glow) == "Off", "a step from Original: Off");
    require(live.locked(glow, PageFrame::Pause) == st::Lock::NeedsRestart && !live.set(glow, "smooth", PageFrame::Pause).ok && live.value(glow) == "off", "in a mission the glow's row is locked");
    require(!live.set(glow, "bright", PageFrame::MainMenu).ok && live.value(glow) == "off", "a glow that does not exist is refused");
    out = live.step(glow, 1, PageFrame::MainMenu);
    require(out.ok && !out.pending && live.value(glow) == "smooth" && !live.pending(glow) && live.quality() == "high", "and round to Smooth, which is what runs: Quality High again (" + out.line + ")");

    // Quality: the five keys as one row.
    require(live.quality() == "high" && live.quality_shown() == "High" && !live.quality_pending() && live.quality_lock(PageFrame::MainMenu) == st::Lock::None, "the defaults are Quality High");
    out = live.set_quality("ultra", PageFrame::MainMenu);
    require(out.ok && out.pending && !out.applied && out.written && live.quality() == "ultra" && live.value(aa) == "msaa4" && live.value(ssao) == "high" && live.value(aniso) == "16" && live.value(glow) == "smooth", "Ultra: 4x, sharp textures 16x, ambient shadows High, the smooth glow (" + out.line + ")");
    require(has_line(ini, "AntiAliasing=msaa4") && has_line(ini, "SSAO=high") && has_line(ini, "MSAA=4") && live.quality_pending(), "written, pending");
    require(live.step_quality(1, PageFrame::MainMenu).ok && live.quality() == "original" && live.value(aa) == "off" && live.value(aniso) == "1" && live.value(glow) == "game" && has_line(ini, "Glow=game") &&
            live.step_quality(-1, PageFrame::MainMenu).ok && live.quality() == "ultra" && live.value(glow) == "smooth", "Quality steps and wraps; Original is the game's own glow");
    (void)live.set(aniso, "8", PageFrame::MainMenu);
    require(live.quality() == "custom" && live.quality_shown() == "Custom", "a key changed by itself: Custom");
    require(live.step_quality(1, PageFrame::MainMenu).ok && live.quality() == "original" && live.set_quality("high", PageFrame::MainMenu).ok && live.quality() == "high" && !live.quality_pending(), "from Custom to the first bundle; High is what runs");
    require(live.quality_lock(PageFrame::Pause) == st::Lock::NeedsRestart && !live.set_quality("ultra", PageFrame::Pause).ok && !live.set_quality("extreme", PageFrame::MainMenu).ok, "Quality is locked in a mission; a bundle that does not exist is refused");

    // Textures and HD pack over [Mods] Enabled; the launcher's lines for the same folders are the expectation
    // (scripts/check_launcher_settings.ps1, files 183 to 186 of its run).
    const auto &enabled = st::get("Mods", "Enabled");
    const auto names = [&] { std::string all; for (const auto &mod : live.mods()) all += (mod.enabled ? "+" : "-") + mod.name + (mod.textures ? "(t)" : "") + (mod.empty ? "(e)" : "") + " "; return all; };
    require(names() == "+alpha_pack(t) +beta_pack(t) +camera_mod +empty_mod(e) ", "without an Enabled line every mod is on, in order of name: " + names());
    require(live.packs() == std::vector<std::string>{"alpha_pack", "beta_pack"} && live.textures() == "hd" && live.textures_shown() == "HD pack" && live.pack() == "beta_pack" && live.pack_row() && !live.textures_pending(), "two packs, both on: the later one is shown");
    out = live.set_textures("standard", PageFrame::MainMenu);
    require(out.ok && out.pending && out.written && has_line(ini, "Enabled=camera_mod,empty_mod") && live.textures() == "standard" && live.textures_pending(), "Standard: the launcher's line (" + out.line + ")");
    require(live.pack_lock(PageFrame::MainMenu) == st::Lock::TexturesStandard && !live.set_pack("alpha_pack", PageFrame::MainMenu).ok && live.pack() == "beta_pack", "HD pack is locked with Standard and names the pack HD would switch on");
    out = live.set_textures("hd", PageFrame::MainMenu);
    require(out.ok && has_line(ini, "Enabled=beta_pack,camera_mod,empty_mod") && live.textures() == "hd" && live.pack() == "beta_pack", "HD pack again: the launcher's line, the pack that was on before");
    out = live.set_pack("alpha_pack", PageFrame::MainMenu);
    require(out.ok && has_line(ini, "Enabled=alpha_pack,camera_mod,empty_mod") && live.pack() == "alpha_pack", "the other pack: the launcher's line (" + out.line + ")");
    require(live.step_pack(1, PageFrame::MainMenu).ok && live.pack() == "beta_pack" && has_line(ini, "Enabled=beta_pack,camera_mod,empty_mod") && live.step_pack(1, PageFrame::MainMenu).ok && live.pack() == "alpha_pack", "HD pack steps through the packs");
    require(live.step_textures(1, PageFrame::MainMenu).ok && live.textures() == "standard" && live.step_textures(-1, PageFrame::MainMenu).ok && live.textures() == "hd" && live.pack() == "alpha_pack", "Textures flips, and comes back to the pack it had");
    require(live.textures_lock(PageFrame::Pause) == st::Lock::NeedsRestart && live.pack_lock(PageFrame::Pause) == st::Lock::NeedsRestart && live.locked(enabled, PageFrame::MainMenu) == st::Lock::None, "both rows are locked in a mission");
    require(!live.set_pack("camera_mod", PageFrame::MainMenu).ok && !live.set_textures("blurry", PageFrame::MainMenu).ok, "a folder that is no texture pack, a value that is none");

    // Defaults.
    (void)live.set(st::get("Mods", "DumpTextures"), "true", PageFrame::MainMenu);
    out = live.reset(st::Page::TexturesMods, PageFrame::MainMenu);
    require(out.ok && out.written && has_line(ini, "; Enabled=alpha_pack,camera_mod,empty_mod") && has_line(ini, "DumpTextures=false") && live.textures() == "hd" && !live.textures_pending() && !out.pending,
        "Textures & mods: every mod on again (the line switched off, as the launcher does) and no dump: " + out.line);
    (void)live.set(scale, "2", PageFrame::MainMenu);
    (void)live.set(st::get("Rendering", "Fullscreen"), "true", PageFrame::MainMenu);
    (void)live.set(frames, "2", PageFrame::MainMenu);
    out = live.reset(st::Page::Display, PageFrame::MainMenu);
    require(out.ok && !out.pending && live.value(scale) == "auto" && live.value(preset) == "high" && live.value(frames) == "1" && live.value(st::get("Rendering", "Fullscreen")) == "false" && out.line == "Defaults=Display as the game runs it, written", out.line);
    (void)live.set_quality("ultra", PageFrame::MainMenu);
    require(live.reset(st::Page::Graphics, PageFrame::MainMenu).ok && live.quality() == "high" && !live.quality_pending(), "Graphics: Quality High");
    out = live.reset(st::Page::Sound, PageFrame::Pause);
    require(out.ok && live.value(buffer) == "80" && out.line == "Defaults=Sound (1 locked rows kept) waits for a restart, written", "in a mission a locked row keeps its value: " + out.line);
    require(live.reset(st::Page::Sound, PageFrame::MainMenu).ok && live.value(buffer) == "70" && !live.reset(st::Page::Top, PageFrame::MainMenu).ok, "Sound: 70 ms; the top page has no defaults");

    // No texture pack at all: nothing to choose.
    {
        const auto bare = fresh_folder(scratch, "no_packs", false);
        std::filesystem::create_directories(bare / "mods" / "camera_mod");
        st::Live none(host_for(bare, nullptr));
        require(none.packs().empty() && none.textures() == "standard" && none.textures_lock(PageFrame::MainMenu) == st::Lock::NoHdPack && !none.pack_row() && none.pack().empty(), "no pack installed: Textures is Standard and locked with its reason");
        const auto refused = none.set_textures("hd", PageFrame::MainMenu);
        require(!refused.ok && refused.error == "No HD pack found.", refused.line);
    }
    // The mods' order the launcher remembers (launcher.ini beside the file) places a mod that is switched on.
    {
        const auto kept = fresh_folder(scratch, "order");
        require(chili::ini_file::write(kept / "launcher.ini", "[Launcher]\r\nModOrder=camera_mod,beta_pack,empty_mod,alpha_pack\r\n") &&
                chili::ini_change_file(kept / "ChiliNative.ini", chili::IniChange{"Mods", "Enabled", "empty_mod, camera_mod"}).ok, "the launcher's order and a list");
        st::Live ordered(host_for(kept, nullptr));
        std::string all;
        for (const auto &mod : ordered.mods()) all += (mod.enabled ? "+" : "-") + mod.name + " ";
        require(all == "+empty_mod -beta_pack +camera_mod -alpha_pack ", "the launcher's order, the enabled ones in the file's order among themselves: " + all);
        require(ordered.textures() == "standard" && ordered.set_textures("hd", PageFrame::MainMenu).ok && has_line(kept / "ChiliNative.ini", "Enabled=empty_mod,beta_pack,camera_mod"), "the pack is switched on in its remembered place");
    }
    // A key an environment variable overrides in this run is locked: the file's value would not be the running one.
    {
        SetEnvironmentVariableW(L"CHILI_RENDER_SCALE", L"2");
        SetEnvironmentVariableW(L"CHILI_RENDER_MSAA", L"4");
        st::Live outside(host_for(folder, nullptr));
        SetEnvironmentVariableW(L"CHILI_RENDER_SCALE", nullptr);
        SetEnvironmentVariableW(L"CHILI_RENDER_MSAA", nullptr);
        require(outside.locked(scale, PageFrame::MainMenu) == st::Lock::SetOutside && outside.locked(aa, PageFrame::MainMenu) == st::Lock::SetOutside && outside.locked(aniso, PageFrame::MainMenu) == st::Lock::None, "a key set by CHILI_RENDER_* is locked, the others are not");
        const auto refused = outside.set(scale, "8", PageFrame::MainMenu);
        require(!refused.ok && refused.error == "Set outside the game.", refused.line);
    }
}

void steps_and_names(const std::filesystem::path &scratch) {
    const auto folder = fresh_folder(scratch, "steps");
    const auto ini = folder / "ChiliNative.ini";
    Fixture f;
    st::Live live(host_for(folder, &f.input));
    // Assignments as text, the way a test step gives them.
    auto out = live.set_text("Input.Mouse/InvertX = true", PageFrame::MainMenu);
    require(out.ok && out.applied && f.input.config().mouse_invert_x && has_line(ini, "InvertX=true"), out.line);
    require(live.set_text("Quality=medium", PageFrame::MainMenu).ok && live.quality() == "medium" && live.set_text("Textures=standard", PageFrame::MainMenu).ok && live.textures() == "standard" &&
            live.set_text("Textures=hd", PageFrame::MainMenu).ok && live.set_text("HdPack=alpha_pack", PageFrame::MainMenu).ok && live.pack() == "alpha_pack", "the rows over more than one key by name");
    require(live.set_text("Defaults=Graphics", PageFrame::MainMenu).ok && live.quality() == "high" && live.set_text("defaults=keyboard & mouse", PageFrame::MainMenu).ok && !f.input.config().mouse_invert_x, "Defaults by page name");
    for (const char *wrong : {"Nope/Key=1", "Rendering/Anisotropy", "Defaults=Nowhere", "Rendering/Anisotropy=3", "Quality=best"})
        require(!live.set_text(wrong, PageFrame::MainMenu).ok, std::string("refused: ") + wrong);
    require(live.set_text("Rendering/Anisotropy=3", PageFrame::MainMenu).line == "Rendering/Anisotropy=3 refused: Anisotropy cannot be 3", "a refusal names the value");
    // CHILI_SETTINGS_AT.
    std::vector<std::string> problems;
    st::Schedule schedule("300:Input/Prompts=keyboard; 120:Input.Gamepad/Rumble=0.5 ;x:Input/Prompts=psp;400:nothing;;350:Rendering/SSAO=low", problems);
    require(problems.size() == 2u && !schedule.empty(), "two steps that are not <frame>:<name>=<value> are named and left out");
    require(schedule.frame(119u, live).empty(), "nothing is due before its frame");
    auto lines = schedule.frame(120u, live);
    require(lines == std::vector<std::string>{"frame 120: Input.Gamepad/Rumble=0.5 applied live, written"} && f.input.config().rumble == 0.5f, "the step of frame 120");
    lines = schedule.frame(360u, live);
    require(lines.size() == 2u && lines[0] == "frame 360: Input/Prompts=keyboard applied live, written" && lines[1] == "frame 360: Rendering/SSAO=low waits for a restart, written" && schedule.empty(), "steps that are overdue are made in their order");
    require(f.input.config().prompts == chili::PromptsSetting::Keyboard && has_line(ini, "SSAO=low"), "and took effect");
    // The switch of the feature.
    for (const wchar_t *name : {L"CHILI_PAGE", L"CHILI_SETTINGS_AT"}) SetEnvironmentVariableW(name, nullptr);
    require(st::feature_enabled(false) && !st::feature_enabled(true), "a player's run has the page, a scripted run has not");
    SetEnvironmentVariableW(L"CHILI_PAGE", L"1");
    require(st::feature_enabled(true), "CHILI_PAGE=1 asks for it in a scripted run");
    SetEnvironmentVariableW(L"CHILI_PAGE", L"0");
    require(!st::feature_enabled(true), "CHILI_PAGE=0 does not");
    SetEnvironmentVariableW(L"CHILI_SETTINGS_AT", L"10:Input/Prompts=psp");
    require(st::feature_enabled(true), "and so do steps");
    for (const wchar_t *name : {L"CHILI_PAGE", L"CHILI_SETTINGS_AT"}) SetEnvironmentVariableW(name, nullptr);

    // The table's defaults are the game's own: what the bindings hold after the game's first-run text is
    // what the table says, for both control styles, for every input key the game can name.
    unsigned compared = 0u;
    for (const bool psp : {false, true}) {
        const auto defaults = chili::PcInputConfig::defaults(psp ? chili::InputScheme::Psp : chili::InputScheme::DualAnalog);
        for (const auto &setting : st::table) {
            if (setting.section.substr(0u, 5u) != "Input" || &setting == &st::get("Input", "Scheme")) continue;
            const auto held = st::input_value(defaults, setting);
            if (setting.kind == st::Kind::Binding && setting.section == "Input.Keyboard") { require(!held, setting.id() + ": the keys of an action have no running value"); continue; }
            require(held && st::equivalent(*held, setting.default_for(psp)), setting.id() + (psp ? " with Scheme=psp" : "") + ": the game's default is " + (held ? *held : std::string("unknown")) + ", the table's " + std::string(setting.default_for(psp)));
            ++compared;
        }
    }
    require(compared == 2u * (5u + 2u + 17u + 28u), "every input key but the keys of an action was compared with the game's defaults (" + std::to_string(compared) + ")");
}
}

int main(int argc, char **argv) {
    try {
        if (argc != 2) { std::cerr << "usage: chili_settings_apply_test <scratch folder>\n"; return 2; }
        const std::filesystem::path scratch = argv[1];
        reconfigure();
        input_rows(scratch);
        other_rows(scratch);
        steps_and_names(scratch);
        if (failures) { std::cerr << failures << " of " << checks << " checks FAILED\n"; return 1; }
        std::cout << "settings apply: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
