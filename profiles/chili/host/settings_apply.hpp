#pragma once
// The settings while the game runs: what the game's own settings page reads, changes and writes.
//
//   settings_table.hpp  the keys, their values and words, the page's rows (no game needed)
//   this file           the value a key has in the file, the value the game runs with, what happens when a
//                       value changes (the appliers), the locks, and the write (ini_document.hpp)
//
// One object, settings::Live, made by main.cpp once the pieces it works on exist and only in a run that
// has the page (feature_enabled: a player's run, or a scripted one that asks with a test variable). The
// page asks it for a row's value and state and hands it the player's change:
//
//   live.value(setting)            what the file says, else the default: what the row shows
//   live.running(setting)          what the game runs with now
//   live.pending(setting)          the two differ: the value waits for a restart
//   live.locked(setting, frame)    why the row cannot be changed here, or Lock::None
//   live.set(setting, value, frame)    takes the value: in force at once where the key is live, written
//   live.step(setting, +1 | -1, frame) the same with the value beside the row's own (a choice wraps)
//
// Live at once (through PcInput::reconfigure, which keeps the pad finder and the pad in use): every key of
// [Input.Keyboard], [Input.Mouse] and [Input.Gamepad], and of [Input] Prompts, ExactTurn, BackgroundInput and
// ShowInput. ExactTurn switches the two-stick layer through Host::exact_turn. Everything else is written and
// waits for the next start (its appliers are work item 8 and later); such a row is locked in a mission.
// [Input] Scheme never changes while the game runs: its host functions are put in place at the start. The
// control style ([Input] Layout, the row Style) is written with the scheme that goes with it and with every
// binding that still holds the old style's default, and waits for the next start like the scheme.
//
// Test variable, for a scripted run (it also switches the feature on, like CHILI_PAGE=1):
//   CHILI_SETTINGS_AT=<frame>:<Section>/<Key>=<value>[;...]
//       at these game frames the value is set as the page would set it; one "[settings] frame N: ..." line each.
//       Quality=<name>, Textures=standard|hd, HdPack=<folder> and Defaults=<page> work the rows over more
//       than one key. A scripted run writes only to a file CHILI_CONFIG names, and never to
//       scripts\unattended.ini (every scripted run's settings): there the change is made and not written.
#include "ini_document.hpp"
#include "pc_input.hpp"
#include "settings_page_api.hpp"
#include "settings_table.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace chili::settings {
// What the appliers act on and what the running values are read from.
struct Host {
    // ChiliNative.ini, or the file CHILI_CONFIG names.
    std::filesystem::path ini;
    // False: the file is read and never written.
    bool writable{true};
    // The bindings and the device settings in force; null in a test of the file side alone.
    PcInput *input{};
    // [Input] ExactTurn: switches the two-stick layer. False when it cannot change while the game runs
    // (DualAnalog::switch_exact_turn). Empty with Scheme=psp, which has no such layer.
    std::function<bool(bool)> exact_turn;
    // The window's state now (F11 changes it too). Empty: the state the game started in.
    std::function<bool()> fullscreen;
    // Puts the window into borderless full screen or back, as F11 does. With it and `fullscreen` the Screen
    // row is the window's own state and changes it at once (and writes how the next start begins). Empty:
    // Screen is written and waits for the next start.
    std::function<void(bool)> set_fullscreen;
    // What the game started with, by key ("Rendering/InternalScale" -> "auto"), for the keys PcInput does
    // not hold: started_values() below makes it from the game's own settings objects.
    std::vector<std::pair<std::string, std::string>> started;
    // The folder the game takes its mods from. Empty: [Mods] Folder beside the file.
    std::filesystem::path mods_folder;
};

inline std::string environment_value(std::string_view name) {
    std::wstring wide(name.begin(), name.end());
    std::wstring value(256u, L'\0');
    for (;;) {
        const auto count = GetEnvironmentVariableW(wide.c_str(), value.data(), static_cast<DWORD>(value.size()));
        if (!count) return {};
        if (count < value.size()) { value.resize(count); break; }
        value.resize(count + 1u);
    }
    std::string out;
    for (const auto c : value) out.push_back(c > 126 || c < 32 ? '?' : static_cast<char>(c));
    return out;
}
// Whether this run has the settings page and everything put in place for it. A player's run has; a
// deterministic, replay or script run only when a test variable asks, so such a run is otherwise exactly
// what it was before the page existed.
inline bool feature_enabled(bool scripted_run) {
    return !scripted_run || environment_value("CHILI_PAGE") == "1" || !environment_value("CHILI_SETTINGS_AT").empty();
}

inline const char *target_name(PadTarget target) noexcept {
    switch (target) {
    case PadTarget::Select: return "Select"; case PadTarget::Start: return "Start"; case PadTarget::DpadUp: return "DpadUp";
    case PadTarget::DpadRight: return "DpadRight"; case PadTarget::DpadDown: return "DpadDown"; case PadTarget::DpadLeft: return "DpadLeft";
    case PadTarget::L: return "L"; case PadTarget::R: return "R"; case PadTarget::Triangle: return "Triangle"; case PadTarget::Circle: return "Circle";
    case PadTarget::Cross: return "Cross"; case PadTarget::Square: return "Square"; case PadTarget::MoveForward: return "MoveForward";
    case PadTarget::MoveBack: return "MoveBack"; case PadTarget::MoveLeft: return "MoveLeft"; case PadTarget::MoveRight: return "MoveRight";
    case PadTarget::Walk: return "Walk"; case PadTarget::LookLeft: return "LookLeft"; case PadTarget::LookRight: return "LookRight";
    case PadTarget::LookUp: return "LookUp"; case PadTarget::LookDown: return "LookDown";
    default: return "None";
    }
}
// The value of an input key as the bindings in force hold it, spelled as the file spells it. Nothing for
// the keys of an action: the game has no name for a key code.
inline std::optional<std::string> input_value(const PcInputConfig &c, const Setting &setting) {
    const auto flag = [](bool on) { return std::optional<std::string>(on ? "true" : "false"); };
    const auto num = [](float value) { return std::optional<std::string>(number_text(static_cast<double>(value))); };
    const std::string_view key = setting.key;
    const auto is = [&](std::string_view name) { return same_name(key, name); };
    if (same_name(setting.section, "Input")) {
        if (is("Scheme")) return std::string(scheme_name(c.scheme));
        if (is("Layout")) return std::string(layout_name(c.layout));
        if (is("ExactTurn")) return flag(c.exact_turn);
        if (is("BackgroundInput")) return flag(c.background_input);
        if (is("ShowInput")) return flag(c.show_input);
        if (is("Version")) return std::to_string(c.version);
        if (is("Prompts")) return std::string(c.prompts == PromptsSetting::Keyboard ? "keyboard" : c.prompts == PromptsSetting::Gamepad ? "gamepad" : c.prompts == PromptsSetting::Psp ? "psp" : "auto");
    } else if (same_name(setting.section, "Input.Keyboard")) {
        if (is("WalkScale")) return num(c.walk_scale);
        if (is("LookKeySpeed")) return num(c.look_key_speed);
    } else if (same_name(setting.section, "Input.Mouse")) {
        if (is("Enabled")) return flag(c.mouse_enabled);
        if (is("Capture")) return std::string(c.mouse_capture_on_click ? "click" : "never");
        if (is("MenuClicks")) return flag(c.mouse_menu_clicks);
        if (is("SensitivityX")) return num(c.mouse_degrees_per_count);
        if (is("SensitivityY")) return num(c.mouse_degrees_per_count_y);
        if (is("InvertX")) return flag(c.mouse_invert_x);
        if (is("InvertY")) return flag(c.mouse_invert_y);
        if (is("HoldPitch")) return flag(c.mouse_hold_pitch);
        if (is("Exact")) return flag(c.mouse_exact);
        if (is("Smoothing")) return num(c.mouse_smoothing);
        if (key.size() == 6u && same_name(key.substr(0u, 5u), "Mouse") && key[5] >= '1' && key[5] <= '5') return std::string(target_name(c.mouse_buttons[static_cast<std::size_t>(key[5] - '1')]));
        if (is("WheelUp")) return std::string(target_name(c.wheel_up));
        if (is("WheelDown")) return std::string(target_name(c.wheel_down));
    } else if (same_name(setting.section, "Input.Gamepad")) {
        if (is("Enabled")) return flag(c.pad_enabled);
        if (is("Index")) return c.pad_index < 0 ? std::string("auto") : std::to_string(c.pad_index);
        if (is("DeadzoneLeft")) return num(c.deadzone_left);
        if (is("DeadzoneRight")) return num(c.deadzone_right);
        if (is("TriggerThreshold")) return num(c.trigger_threshold);
        if (is("CompensateEngineDeadzone")) return flag(c.compensate_engine_deadzone);
        if (is("TurnSensitivity")) return num(c.pad_turn_sensitivity);
        if (is("LookSensitivity")) return num(c.pad_look_sensitivity);
        if (is("InvertY")) return flag(c.pad_invert_y);
        if (is("Glyphs")) return std::string(c.glyphs == GlyphsSetting::Xbox ? "xbox" : c.glyphs == GlyphsSetting::PlayStation ? "playstation" : "auto");
        if (is("PlayStationPads")) return flag(c.playstation_pads);
        if (is("Rumble")) return num(c.rumble);
        if (const auto button = PcInputConfig::pad_button_from_name(key)) return std::string(target_name(c.pad_buttons[*button]));
    }
    return std::nullopt;
}

// Host::started from the settings objects the game's own readers filled at the start (render_settings.hpp,
// mod_files.hpp, audio_settings.hpp; a template so that this header needs none of them). `frame_lock` is "1"
// or "2" as the game's configuration got it; `fullscreen` whether the window started in full screen.
template <typename Render, typename Mods, typename Audio>
std::vector<std::pair<std::string, std::string>> started_values(const Render &render, const Mods &mods, const Audio &audio, bool tight_camera,
                                                                std::string_view frame_lock, bool raised_priority, bool fullscreen) {
    std::vector<std::pair<std::string, std::string>> out;
    const auto flag = [](bool on) { return std::string(on ? "true" : "false"); };
    out.emplace_back("Rendering/Fullscreen", flag(fullscreen));
    out.emplace_back("Rendering/InternalScale", render.internal_scale ? std::to_string(render.internal_scale) : std::string("auto"));
    out.emplace_back("Rendering/EarlyPresent", flag(render.early_present));
    out.emplace_back("Rendering/Preset", render.preset_name());
    out.emplace_back("Rendering/AntiAliasing", render.msaa > 1u ? "msaa" + std::to_string(render.msaa) : render.fxaa ? std::string("fxaa") : std::string("off"));
    out.emplace_back("Rendering/MSAA", std::to_string(render.msaa));
    out.emplace_back("Rendering/TextureFilter", render.texture_filter == Render::TextureFilter::Linear ? "linear" : render.texture_filter == Render::TextureFilter::Nearest ? "nearest" : "game");
    out.emplace_back("Rendering/Anisotropy", std::to_string(render.anisotropy));
    out.emplace_back("Rendering/SSAO", render.ssao == Render::AmbientOcclusion::Low ? "low" : render.ssao == Render::AmbientOcclusion::High ? "high" : "off");
    out.emplace_back("Rendering/Glow", render.glow == Render::Glow::Game ? "game" : render.glow == Render::Glow::Off ? "off" : "smooth");
    out.emplace_back("Rendering/Backend", render.backend == Render::Backend::Software ? "software" : "d3d12");
    out.emplace_back("Rendering/GeThread", flag(render.ge_thread));
    out.emplace_back("Game/FrameLock", std::string(frame_lock));
    out.emplace_back("Game/Camera", tight_camera ? "tight" : "original");
    out.emplace_back("Game/Priority", raised_priority ? "high" : "normal");
    out.emplace_back("Audio/BufferMs", std::to_string(audio.buffer_ms));
    std::string enabled;
    for (const auto &mod : mods.mods) {
        const auto name = mod.filename().u8string();
        if (!enabled.empty()) enabled += ',';
        enabled.append(name.begin(), name.end());
    }
    out.emplace_back("Mods/Enabled", enabled);
    out.emplace_back("Mods/DumpTextures", flag(!mods.texture_dump.empty()));
    out.emplace_back("Mods/LogFiles", flag(mods.log_files));
    return out;
}

class Live {
public:
    // What became of a change.
    struct Outcome {
        bool ok{};          // the value was taken (also when it was the row's value already)
        bool applied{};     // it is in force now
        bool pending{};     // it waits for a restart
        bool written{};     // the file holds it
        bool remade{};      // the file was gone and was written again whole from what this run had read
        Lock lock{Lock::None};   // not taken: the row is locked
        std::string error;  // not taken or not written: why
        std::string line;   // for the log, without "[settings] " in front: "Rendering/Anisotropy=4 waits for a restart, written"
    };
    // A folder of the mods folder, as the launcher lists it.
    struct Mod { std::string name; bool textures{}, empty{}, enabled{}; };

    explicit Live(Host host) : host_(std::move(host)) {
        for (const auto &setting : table) if (!setting.environment.empty() && !environment_value(setting.environment).empty()) outside_.push_back(&setting);
        if (!environment_value("CHILI_RENDER_MSAA").empty()) outside_.push_back(&get("Rendering", "AntiAliasing"));
        // The file is read; nothing is put in force (the game has just read the same file).
        (void)read_file();
    }
    Live(const Live &) = delete;
    Live &operator=(const Live &) = delete;
    const Host &host() const noexcept { return host_; }

    // Reads the file again (the page does when it opens: the launcher may have written meanwhile). The input
    // settings the file now holds are put in force when they differ from the ones in force; a key that takes
    // a restart simply shows as pending. Returns lines for the log.
    std::vector<std::string> reload() {
        std::vector<std::string> lines;
        const std::string text = read_file();
        if (!host_.input) return lines;
        // As the game reads the file at its start (PcInputConfig::load): the defaults, then the file. The
        // control style in force stays, and with it the defaults a key the file leaves out falls back to.
        const auto &now = host_.input->config();
        auto fresh = PcInputConfig::defaults(now.layout);
        (void)fresh.apply(text);
        for (const auto &change : unsaved_) if (!change.unset) (void)fresh.apply("[" + change.section + "]\n" + change.key + "=" + change.value + "\n");
        fresh.scheme = now.scheme; fresh.layout = now.layout; fresh.layout_named = now.layout_named; fresh.version = now.version;
        if (fresh.exact_turn != now.exact_turn && host_.exact_turn && !host_.exact_turn(fresh.exact_turn)) fresh.exact_turn = now.exact_turn;
        if (!fresh.same_as(now)) {
            const auto done = host_.input->reconfigure(std::move(fresh));
            lines.push_back(std::string("the settings file holds other input settings than the game ran with: put in force") +
                (done.prompts_published ? ", the button pictures painted again" : "") + (done.finder_rebuilt ? ", the gamepads looked for again" : ""));
        }
        return lines;
    }

    // What the row shows: the value the file holds (with the changes not yet written), else the default.
    std::string value(const Setting &setting) const {
        if (&setting == &anti_aliasing()) return anti_aliasing_chosen();
        // Screen is what the window is now: F11 changes it as well as the row.
        if (&setting == &screen() && screen_live()) return host_.fullscreen() ? "true" : "false";
        const auto held = setting.kind == Kind::Text && same_name(setting.section, "Disc") ? work_.get_raw(setting.section, setting.key) : work_.get(setting.section, setting.key);
        // A file without a Layout line means the style its scheme goes with.
        if (!held && &setting == &layout_setting()) return layout_name(layout_chosen());
        return held ? *held : std::string(setting.default_for(layout_chosen()));
    }
    std::string shown(const Setting &setting) const { return settings::shown(setting, value(setting)); }
    // What the game runs with now, spelled as the file spells it. For a key whose running value is not known
    // (the keys of an action) the row's own value.
    std::string running(const Setting &setting) const {
        const auto known = running_known(setting);
        return known ? *known : value(setting);
    }
    // The row's value is not the one the game runs with: it waits for a restart.
    bool pending(const Setting &setting) const {
        if (&setting == &mods_enabled()) {
            const auto ran = started("Mods/Enabled");
            return ran && !same_mods(enabled_names(mods()), split_list(*ran));
        }
        const auto known = running_known(setting);
        return known && !equivalent(canonical(setting, value(setting)), canonical(setting, *known));
    }
    // Whether a change of the key is put in force while the game runs.
    bool applies_live(const Setting &setting) const noexcept {
        if (&setting == &screen()) return screen_live();
        return host_.input && same_name(setting.section.substr(0u, 5u), "Input") && !same_name(setting.key, "Scheme") && !same_name(setting.key, "Version") &&
               setting.cls != Class::Off && setting.cls != Class::Restart;
    }
    // Whether the Screen row switches the window itself (the host gave both the window's state and its switch).
    bool screen_live() const noexcept { return static_cast<bool>(host_.fullscreen) && static_cast<bool>(host_.set_fullscreen); }
    // Why the row cannot be changed in this frame; Lock::None when it can.
    Lock locked(const Setting &setting, PageFrame frame) const {
        if (std::find(outside_.begin(), outside_.end(), &setting) != outside_.end()) return Lock::SetOutside;
        // In a mission only what is in force at once: a restart would lose the mission.
        if (frame == PageFrame::Pause && !applies_live(setting)) return Lock::NeedsRestart;
        const auto is = [&](std::string_view section, std::string_view key) { return same_name(setting.section, section) && same_name(setting.key, key); };
        if (is("Rendering", "Preset")) {
            // The automatic resolution is whatever is not a multiple of 1 to 8 (the launcher's IsAutoScale).
            const auto scale = number(value(get("Rendering", "InternalScale")));
            if (scale && *scale >= 1.0 && *scale <= 8.0 && *scale == std::floor(*scale)) return Lock::OnlyWithAuto;
        }
        if ((is("Input.Mouse", "SensitivityY") || is("Input.Gamepad", "LookSensitivity")) && psp_chosen()) return Lock::PspCannotLook;
        if (is("Input.Keyboard", "LookKeySpeed") && psp_chosen()) return Lock::NotWithPsp;
        // Switched off from the pad there would be no way back without a keyboard.
        if (is("Input.Gamepad", "Enabled") && host_.input && host_.input->device() == InputDevice::Gamepad && is_true(value(setting))) return Lock::InUse;
        return Lock::None;
    }

    // Takes a value for the key, as the file spells it. In force at once where the key is live; written to
    // the file at once unless `write` is false (a slider while it is dragged: flush() when it is let go).
    Outcome set(const Setting &setting, std::string_view wanted, PageFrame frame, bool write = true) {
        Outcome out;
        const std::string id = setting.id();
        if (const Lock lock = locked(setting, frame); lock != Lock::None) return refused(out, id, wanted, lock);
        if (!allowed(setting, wanted)) return refused(out, id, wanted, std::string(setting.key) + " cannot be " + std::string(trimmed(wanted)));
        const std::string text = spelled(setting, wanted);
        if (&setting == &screen()) {
            // The window itself, as F11 switches it; the file gets how the next start begins.
            if (screen_live() && host_.fullscreen() != is_true(text)) host_.set_fullscreen(is_true(text));
        } else if (applies_live(setting)) {
            if (!apply_input(setting, text, out.error) && !out.error.empty()) return refused(out, id, wanted, out.error);
        }
        // The control style: every binding that still holds the old style's default takes the new one's, and
        // the scheme goes with the style (PcInputConfig::layout_binding_changes; the launcher does the same).
        std::size_t bindings = 0u;
        if (&setting == &layout_setting()) {
            const auto from = layout_chosen();
            const auto to = layout_from_name(text).value_or(from);
            if (to != from) {
                for (const auto &change : PcInputConfig::layout_binding_changes(work_, from, to)) {
                    remember(get(change.section, change.key), change.value, false, true);
                    ++bindings;
                }
                remember(get("Input", "Scheme"), std::string(layout_is_psp(to) ? "psp" : "dual_analog"));
            }
        }
        remember(setting, text);
        // An older MSAA line in the file must not contradict the choice (the launcher's SetAa).
        if (&setting == &anti_aliasing() && doc_.has("Rendering", "MSAA")) remember(get("Rendering", "MSAA"), text.rfind("msaa", 0u) == 0u ? text.substr(4u) : std::string("1"));
        out.ok = true;
        const bool waits = pending(setting);
        finish(out, id + "=" + text + (bindings ? " with " + std::to_string(bindings) + " bindings" : std::string()), waits, !waits && applies_live(setting), write);
        return out;
    }
    Outcome step(const Setting &setting, int direction, PageFrame frame, bool write = true) {
        const std::string now = value(setting), next = stepped(setting, now, direction);
        // A slider whose value lies outside the row's range (written by hand) and is stepped further out of
        // it: the value stays, and that is no fault of the player's.
        if (setting.kind == Kind::Slider && locked(setting, frame) == Lock::None && equivalent(next, now) && !allowed(setting, next)) {
            Outcome out;
            out.ok = out.written = true;
            return out;
        }
        return set(setting, next, frame, write);
    }
    bool unsaved() const noexcept { return !unsaved_.empty(); }
    // Writes what is not written yet: the file is read fresh, only the changed keys change, the result is
    // swapped in under the gate shared with the launcher.
    Outcome flush() {
        Outcome out;
        out.ok = true;
        if (unsaved_.empty()) { out.written = true; return out; }
        if (!host_.writable || host_.ini.empty()) { out.ok = false; out.error = "this run does not write its settings file"; return out; }
        // `known_` is what this run last saw of the file: a file that is gone for a moment (the launcher's swap
        // under way, a scanner holding it) is waited for and never written again from nothing.
        const auto result = ini_change_file(host_.ini, unsaved_, &known_);
        if (!result.ok) { out.ok = false; out.error = result.error; return out; }
        unsaved_.clear();
        // The file as the writer left it (it read the file fresh, so another writer's lines are in it).
        doc_ = IniDocument::parse(known_.text);
        rework();
        out.written = true;
        out.remade = result.remade;
        return out;
    }
    // What this run last saw of the settings file.
    const IniKnown &known() const noexcept { return known_; }

    // ---- Quality: anti-aliasing, sharp textures, texture filter, ambient shadows and glow in one row -----

    // "original", "medium", "high", "ultra", or "custom" while no bundle matches.
    std::string quality() const {
        const std::string aa = canonical(anti_aliasing(), value(anti_aliasing()));
        for (const auto &q : qualities)
            if (aa == q.anti_aliasing && equivalent(value(get("Rendering", "Anisotropy")), q.anisotropy) &&
                equivalent(value(get("Rendering", "TextureFilter")), q.texture_filter) && equivalent(canonical(get("Rendering", "SSAO"), value(get("Rendering", "SSAO"))), q.ambient) &&
                equivalent(value(get("Rendering", "Glow")), q.glow)) return std::string(q.name);
        return std::string(quality_custom);
    }
    std::string quality_shown() const {
        const auto name = quality();
        for (const auto &q : qualities) if (name == q.name) return std::string(q.word);
        return std::string(quality_custom_word);
    }
    bool quality_pending() const { for (const auto *part : quality_parts()) if (pending(*part)) return true; return false; }
    // Locked when none of its five keys can be changed here.
    Lock quality_lock(PageFrame frame) const {
        Lock first = Lock::None;
        for (const auto *part : quality_parts()) { const Lock lock = locked(*part, frame); if (lock == Lock::None) return Lock::None; if (first == Lock::None) first = lock; }
        return first;
    }
    Outcome set_quality(std::string_view name, PageFrame frame, bool write = true) {
        Outcome out;
        const std::string id = "Quality";
        const QualityBundle *bundle = nullptr;
        for (const auto &q : qualities) if (same_name(q.name, trimmed(name))) bundle = &q;
        if (!bundle) return refused(out, id, name, "Quality cannot be " + std::string(trimmed(name)));
        if (const Lock lock = quality_lock(frame); lock != Lock::None) return refused(out, id, name, lock);
        const std::pair<const Setting *, std::string_view> parts[]{{&anti_aliasing(), bundle->anti_aliasing}, {&get("Rendering", "Anisotropy"), bundle->anisotropy},
            {&get("Rendering", "TextureFilter"), bundle->texture_filter}, {&get("Rendering", "SSAO"), bundle->ambient}, {&get("Rendering", "Glow"), bundle->glow}};
        // A key that is locked here keeps its value; the others take the bundle's.
        for (const auto &part : parts) if (locked(*part.first, frame) == Lock::None) (void)set(*part.first, part.second, frame, false);
        out.ok = true;
        const bool waits = quality_pending();
        bool live = !waits;
        for (const auto *part : quality_parts()) live = live && applies_live(*part);
        finish(out, id + "=" + std::string(bundle->name), waits, live, write);
        return out;
    }
    // The bundle beside the row's own; from "custom" the first (right) or the last (left).
    Outcome step_quality(int direction, PageFrame frame, bool write = true) {
        const auto now = quality();
        const std::size_t count = std::size(qualities);
        std::size_t index = count;
        for (std::size_t i = 0; i < count; ++i) if (now == qualities[i].name) index = i;
        const std::size_t next = index == count ? (direction > 0 ? 0u : count - 1u) : (index + (direction > 0 ? 1u : count - 1u)) % count;
        return set_quality(qualities[next].name, frame, write);
    }

    // ---- Textures and HD pack: [Mods] Enabled -----------------------------------------------------------

    // The mods folder's folders as the launcher lists them: the ones [Mods] Enabled names first and in its
    // order, the others after them (in the order the launcher remembers, else by name). Without the line
    // every mod is on.
    std::vector<Mod> mods() const {
        std::vector<Mod> found;
        std::error_code status;
        const auto folder = mods_folder();
        for (const auto &entry : std::filesystem::directory_iterator(folder, status)) {
            if (!entry.is_directory(status)) continue;
            const auto wide = entry.path().filename().u8string();
            Mod mod;
            mod.name.assign(wide.begin(), wide.end());
            if (mod.name.empty() || mod.name.front() == '.') continue;
            mod.textures = std::filesystem::is_directory(entry.path() / "textures", status);
            mod.empty = std::filesystem::is_empty(entry.path(), status);
            found.push_back(std::move(mod));
        }
        std::sort(found.begin(), found.end(), [](const Mod &a, const Mod &b) { return before(a.name, b.name); });
        const auto line = work_.get("Mods", "Enabled");
        if (!line) { for (auto &mod : found) mod.enabled = true; return found; }
        std::vector<Mod> result;
        const auto in = [](const std::vector<Mod> &list, std::string_view name) { return std::any_of(list.begin(), list.end(), [&](const Mod &m) { return same_name(m.name, name); }); };
        for (const auto &name : split_list(*line))
            for (auto &mod : found) if (same_name(mod.name, name) && !in(result, name)) { mod.enabled = true; result.push_back(mod); break; }
        // Mods that are off keep their place: the one they had when this page last changed the list, else the
        // one the player gave them in the launcher (its launcher.ini beside the file), else they follow by name.
        std::vector<std::string> remembered = order_;
        if (remembered.empty())
            if (const auto launcher = ini_file::read(host_.ini.parent_path() / "launcher.ini"))
                remembered = split_list(IniDocument::parse(*launcher).get_raw("Launcher", "ModOrder").value_or(std::string()));
        if (!remembered.empty()) {
            std::vector<Mod> ordered;
            for (const auto &name : remembered) for (const auto &mod : found) if (same_name(mod.name, name) && !in(ordered, name)) { ordered.push_back(mod); break; }
            for (const auto &mod : found) if (!in(ordered, mod.name)) ordered.push_back(mod);
            // The enabled ones keep the file's order among themselves.
            std::size_t next = 0;
            for (auto &mod : ordered) if (mod.enabled) mod = result[next++];
            return ordered;
        }
        for (const auto &mod : found) if (!mod.enabled) result.push_back(mod);
        return result;
    }
    // The texture packs: the mods with a `textures` folder, in the order of mods().
    std::vector<std::string> packs() const {
        std::vector<std::string> names;
        for (const auto &mod : mods()) if (mod.textures) names.push_back(mod.name);
        return names;
    }
    // "hd" while a texture pack is switched on, else "standard".
    std::string textures() const { return active_pack(mods()).empty() ? "standard" : "hd"; }
    std::string textures_shown() const { return std::string(textures_values[textures() == "hd" ? 1 : 0].word); }
    bool textures_pending() const { return pending(mods_enabled()); }
    // The Textures row: locked in a mission; with no pack installed there is nothing to choose.
    Lock textures_lock(PageFrame frame) const {
        if (const Lock lock = locked(mods_enabled(), frame); lock != Lock::None) return lock;
        return packs().empty() ? Lock::NoHdPack : Lock::None;
    }
    Outcome set_textures(std::string_view which, PageFrame frame, bool write = true) {
        Outcome out;
        const bool hd = same_name(trimmed(which), "hd");
        if (!hd && !same_name(trimmed(which), "standard")) return refused(out, "Textures", which, "Textures cannot be " + std::string(trimmed(which)));
        if (const Lock lock = textures_lock(frame); lock != Lock::None) return refused(out, "Textures", which, lock);
        auto list = mods();
        const auto active = active_pack(list);
        if (!active.empty()) chosen_pack_ = active;
        if (hd == !active.empty()) {
            // The row says so already: the packs stay as they are.
            out.ok = true;
            finish(out, std::string("Textures=") + (hd ? "hd" : "standard"), pending(mods_enabled()), false, write);
            return out;
        }
        std::string target;
        if (hd) {
            for (const auto &mod : list) if (mod.textures && (target.empty() || same_name(mod.name, chosen_pack_))) target = mod.name;
            chosen_pack_ = target;
        }
        for (auto &mod : list) if (mod.textures) mod.enabled = hd && mod.name == target;
        commit_mods(list);
        out.ok = true;
        finish(out, std::string("Textures=") + (hd ? "hd" : "standard"), pending(mods_enabled()), false, write);
        return out;
    }
    Outcome step_textures(int, PageFrame frame, bool write = true) { return set_textures(textures() == "hd" ? "standard" : "hd", frame, write); }
    // The HD pack row is there when more than one pack is installed.
    bool pack_row() const { return packs().size() > 1u; }
    // The pack whose pictures the game shows (with several switched on, the last one), or while Textures is
    // Standard the pack the HD choice would switch on.
    std::string pack() const {
        const auto list = mods();
        const auto active = active_pack(list);
        if (!active.empty()) return active;
        std::string first;
        for (const auto &mod : list) if (mod.textures) { if (first.empty()) first = mod.name; if (same_name(mod.name, chosen_pack_)) return mod.name; }
        return first;
    }
    Lock pack_lock(PageFrame frame) const {
        if (const Lock lock = locked(mods_enabled(), frame); lock != Lock::None) return lock;
        return textures() == "hd" ? Lock::None : Lock::TexturesStandard;
    }
    // Switches this pack on and the other packs off; the mods that are not texture packs stay as they are.
    Outcome set_pack(std::string_view name, PageFrame frame, bool write = true) {
        Outcome out;
        if (const Lock lock = pack_lock(frame); lock != Lock::None) return refused(out, "HdPack", name, lock);
        auto list = mods();
        const auto found = std::find_if(list.begin(), list.end(), [&](const Mod &mod) { return mod.textures && same_name(mod.name, trimmed(name)); });
        if (found == list.end()) return refused(out, "HdPack", name, std::string(pack_not_readable));
        chosen_pack_ = found->name;
        for (auto &mod : list) if (mod.textures) mod.enabled = mod.name == chosen_pack_;
        commit_mods(list);
        out.ok = true;
        finish(out, "HdPack=" + chosen_pack_, pending(mods_enabled()), false, write);
        return out;
    }
    Outcome step_pack(int direction, PageFrame frame, bool write = true) {
        const auto names = packs();
        const auto now = pack();
        if (names.empty()) { Outcome out; return refused(out, "HdPack", "", Lock::NoHdPack); }
        std::size_t index = 0;
        for (std::size_t i = 0; i < names.size(); ++i) if (names[i] == now) index = i;
        return set_pack(names[(index + (direction > 0 ? 1u : names.size() - 1u)) % names.size()], frame, write);
    }

    // ---- Defaults: the last row of a section's page -----------------------------------------------------

    // Every row of the page that can be changed here goes back to its default (the launcher's Defaults
    // button; the bindings stay, the game's page does not change them). Controls: Camera, Prompts and Button
    // pictures, and the rows of Keyboard & mouse and Gamepad; the control style stays as chosen. Graphics:
    // Quality High. Textures & mods: every mod on, in order of name, and no texture dump.
    Outcome reset(Page page, PageFrame frame, bool write = true) {
        Outcome out;
        unsigned locked_rows = 0u;
        const auto one = [&](const Setting &setting) {
            if (locked(setting, frame) != Lock::None) { ++locked_rows; return; }
            (void)set(setting, setting.default_for(layout_chosen()), frame, false);
        };
        const auto page_rows = [&](Page which) {
            for (const auto &row : rows_of(which)) {
                if (row.kind == RowKind::Setting && !same_name(row.setting->key, "Layout")) one(*row.setting);
                else if (row.kind == RowKind::Quality) { if (quality_lock(frame) == Lock::None) (void)set_quality(quality_default, frame, false); else ++locked_rows; }
                else if (row.kind == RowKind::Textures) {
                    if (locked(mods_enabled(), frame) != Lock::None) { ++locked_rows; continue; }
                    auto list = mods();
                    for (auto &mod : list) mod.enabled = true;
                    std::sort(list.begin(), list.end(), [](const Mod &a, const Mod &b) { return before(a.name, b.name); });
                    commit_mods(list);
                }
            }
        };
        if (page == Page::Top || page == Page::None) return refused(out, "Defaults", page_name(page), "this page has no defaults");
        page_rows(page);
        if (page == Page::Controls) { page_rows(Page::KeyboardMouse); page_rows(Page::Gamepad); }
        out.ok = true;
        bool waits = false;
        const auto any_pending = [&](Page which) { for (const auto &row : rows_of(which)) if (row.kind == RowKind::Setting && pending(*row.setting)) waits = true; };
        any_pending(page);
        if (page == Page::Controls) { any_pending(Page::KeyboardMouse); any_pending(Page::Gamepad); }
        if (page == Page::TexturesMods && pending(mods_enabled())) waits = true;
        // In force at once: the pages whose rows are input keys.
        const bool input_page = host_.input && (page == Page::Controls || page == Page::KeyboardMouse || page == Page::Gamepad);
        finish(out, "Defaults=" + std::string(page_name(page)) + (locked_rows ? " (" + std::to_string(locked_rows) + " locked rows kept)" : ""), waits, !waits && input_page, write);
        return out;
    }

    // "Section/Key=value", or one of the rows over more than one key: "Quality=high", "Textures=hd",
    // "HdPack=<folder>", "Defaults=<page name>". For test steps (CHILI_SETTINGS_AT, the page's own).
    Outcome set_text(std::string_view assignment, PageFrame frame) {
        Outcome out;
        const auto equals = assignment.find('=');
        if (equals == std::string_view::npos) return refused(out, std::string(trimmed(assignment)), "", "expected <Section>/<Key>=<value>");
        const auto name = trimmed(assignment.substr(0u, equals)), value = trimmed(assignment.substr(equals + 1u));
        if (same_name(name, "Quality")) return set_quality(value, frame);
        if (same_name(name, "Textures")) return set_textures(value, frame);
        if (same_name(name, "HdPack")) return set_pack(value, frame);
        if (same_name(name, "Defaults")) {
            for (const Page page : {Page::Display, Page::Graphics, Page::Controls, Page::KeyboardMouse, Page::Gamepad, Page::TexturesMods, Page::Sound})
                if (same_name(page_name(page), value)) return reset(page, frame);
            return refused(out, "Defaults", value, "no page of that name");
        }
        const Setting *setting = find(name);
        if (!setting) return refused(out, std::string(name), value, "no such setting");
        return set(*setting, value, frame);
    }

private:
    static const Setting &anti_aliasing() { return get("Rendering", "AntiAliasing"); }
    static const Setting &screen() { return get("Rendering", "Fullscreen"); }
    static const Setting &layout_setting() { return get("Input", "Layout"); }
    static const Setting &mods_enabled() { return get("Mods", "Enabled"); }
    static std::array<const Setting *, 5> quality_parts() { return {&anti_aliasing(), &get("Rendering", "Anisotropy"), &get("Rendering", "TextureFilter"), &get("Rendering", "SSAO"), &get("Rendering", "Glow")}; }
    // By name without case, as the launcher sorts its list.
    static bool before(std::string_view a, std::string_view b) {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
            const auto upper = [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; };
            return static_cast<unsigned char>(upper(x)) < static_cast<unsigned char>(upper(y)); });
    }
    static std::vector<std::string> split_list(std::string_view text) {
        std::vector<std::string> out;
        while (!text.empty()) {
            const auto comma = std::min(text.find(','), text.size());
            const auto entry = trimmed(text.substr(0u, comma));
            text.remove_prefix(std::min(comma + 1u, text.size()));
            if (!entry.empty()) out.emplace_back(entry);
        }
        return out;
    }
    static std::vector<std::string> enabled_names(const std::vector<Mod> &list) {
        std::vector<std::string> names;
        for (const auto &mod : list) if (mod.enabled) names.push_back(mod.name);
        return names;
    }
    static bool same_mods(const std::vector<std::string> &a, const std::vector<std::string> &b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const std::string &x, const std::string &y) { return same_name(x, y); });
    }
    // The pack whose pictures the game shows: with several switched on, the last one.
    static std::string active_pack(const std::vector<Mod> &list) {
        std::string active;
        for (const auto &mod : list) if (mod.textures && mod.enabled) active = mod.name;
        return active;
    }
    std::filesystem::path mods_folder() const {
        if (!host_.mods_folder.empty()) return host_.mods_folder;
        const auto named = trimmed(value(get("Mods", "Folder")));
        const std::string name(named.empty() ? std::string_view("mods") : named);
        std::filesystem::path folder(std::u8string(name.begin(), name.end()));
        return folder.is_relative() ? host_.ini.parent_path() / folder : folder;
    }
    // The launcher's CommitMods: the list's order and switches into [Mods] Enabled. Every mod on and in order
    // of name is the game's own default: the line is switched off.
    void commit_mods(const std::vector<Mod> &list) {
        order_.clear();
        for (const auto &mod : list) order_.push_back(mod.name);
        bool all = true, alphabetical = true;
        for (std::size_t i = 0; i < list.size(); ++i) { all = all && list[i].enabled; if (i && before(list[i].name, list[i - 1u].name)) alphabetical = false; }
        if (list.empty() || (all && alphabetical)) { remember(mods_enabled(), std::nullopt); return; }
        std::string names;
        for (const auto &mod : list) if (mod.enabled) { if (!names.empty()) names += ','; names += mod.name; }
        std::string held = doc_.get("Mods", "Enabled").value_or(std::string("\n"));
        held.erase(std::remove(held.begin(), held.end(), ' '), held.end());
        if (held == names) remember(mods_enabled(), std::nullopt, true);
        else remember(mods_enabled(), names, false, true);
    }
    bool psp_chosen() const { return same_name(trimmed(work_.get("Input", "Scheme").value_or(std::string())), "psp"); }
    // The control style the file names (with the changes not yet written), else the one its scheme goes with.
    InputLayout layout_chosen() const {
        if (const auto named = layout_from_name(trimmed(work_.get("Input", "Layout").value_or(std::string())))) return *named;
        return psp_chosen() ? InputLayout::Psp : InputLayout::TodPc;
    }
    std::optional<std::string> started(std::string_view id) const {
        for (const auto &[key, value] : host_.started) if (same_name(key, id)) return value;
        return std::nullopt;
    }
    std::optional<std::string> running_known(const Setting &setting) const {
        if (same_name(setting.section, "Rendering") && same_name(setting.key, "Fullscreen") && host_.fullscreen) return std::string(host_.fullscreen() ? "true" : "false");
        if (host_.input && same_name(setting.section.substr(0u, 5u), "Input")) return input_value(host_.input->config(), setting);
        return started(setting.id());
    }
    // AntiAliasing as the game's reader will take the file: the AntiAliasing and the older MSAA lines of
    // [Rendering] in the order they stand (render_settings.hpp).
    std::string anti_aliasing_chosen() const {
        unsigned msaa = 1u;
        bool fxaa = true;
        for (const auto &line : work_.lines) {
            if (line.kind != IniDocument::Kind::KeyValue || !same_name(line.section, "Rendering")) continue;
            const std::string value = lowered(line.value);
            if (same_name(line.key, "MSAA")) {
                if (value == "1" || value == "2" || value == "4" || value == "8") { msaa = static_cast<unsigned>(value[0] - '0'); if (msaa > 1u) fxaa = false; }
            } else if (same_name(line.key, "AntiAliasing")) {
                if (value == "off" || value == "none") { msaa = 1u; fxaa = false; }
                else if (value == "fxaa") { msaa = 1u; fxaa = true; }
                else if (value == "msaa2" || value == "msaa4" || value == "msaa8") { msaa = static_cast<unsigned>(value[4] - '0'); fxaa = false; }
            }
        }
        return msaa > 1u ? "msaa" + std::to_string(msaa) : fxaa ? "fxaa" : "off";
    }
    // The value as the file is to hold it: a choice in the table's spelling, a switch as true or false, a
    // slider's number as the launcher writes one.
    static std::string spelled(const Setting &setting, std::string_view wanted) {
        switch (setting.kind) {
        case Kind::Switch: return is_true(wanted) ? "true" : "false";
        case Kind::Choice: return std::string(setting.values[*value_index(setting, wanted)].ini);
        case Kind::Slider: return number_text(*number(wanted));
        default: return std::string(trimmed(wanted));
        }
    }
    // The file as it is now. A file this run has read before and that is not there, or cannot be opened, at
    // this moment (another writer's swap, a scanner) is waited for; if it stays away the text read last
    // stands, so that nothing is taken for the defaults because of a file that was gone for a moment.
    std::string read_file() {
        std::string text;
        if (!host_.ini.empty()) {
            bool exists = false;
            if (auto read = ini_file::read_patiently(host_.ini, &exists, known_.existed)) {
                text = std::move(*read);
                known_.existed = true;
                known_.text = text;
            } else if (known_.existed) text = known_.text;
        }
        doc_ = IniDocument::parse(text);
        rework();
        return text;
    }
    // The file with the changes that are not written yet.
    void rework() {
        work_ = doc_;
        for (const auto &change : unsaved_) {
            if (change.unset) work_.unset(change.section, change.key);
            else work_.set(change.section, change.key, change.value, change.raw);
        }
    }
    // Notes a change for the file, as the launcher's Model.Set and Model.Unset do: a value the file says
    // already (or, for a key it leaves out, the default) is nothing to write, and neither is switching off
    // a key the file does not have. One case more than the launcher's: a key the file leaves out is written
    // with its default when this run does not run that default (a scripted run's frame rate is 30 without
    // the line, a player's 60), so that the value the row shows is the one the next start takes.
    // `value` nothing: the key is switched off. `always`: no such comparison.
    void remember(const Setting &setting, std::optional<std::string> value, bool forget = false, bool always = false) {
        unsaved_.erase(std::remove_if(unsaved_.begin(), unsaved_.end(), [&](const IniChange &change) {
            return same_name(change.section, setting.section) && same_name(change.key, setting.key); }), unsaved_.end());
        const auto held = doc_.get(setting.section, setting.key);
        bool write = !forget;
        if (write && value && !always) {
            const auto ran = held ? std::nullopt : running_known(setting);
            write = !equivalent(held ? *held : std::string(setting.default_for(layout_chosen())), *value) ||
                    (ran && !equivalent(canonical(setting, *ran), canonical(setting, *value)));
        }
        if (write && !value) write = held.has_value();
        if (write) unsaved_.push_back(IniChange{std::string(setting.section), std::string(setting.key), value.value_or(std::string()), false, !value});
        rework();
    }
    // One line of ini text through the game's own reader, on the bindings in force.
    bool apply_input(const Setting &setting, const std::string &text, std::string &error) {
        const auto &now = host_.input->config();
        auto config = now;
        const auto problems = config.apply("[" + std::string(setting.section) + "]\n" + std::string(setting.key) + "=" + text + "\n");
        if (!problems.empty()) {
            // "line 2: SensitivityX must be a number from 0.001 to 5"
            const auto colon = problems.front().find(": ");
            error = colon == std::string::npos ? problems.front() : problems.front().substr(colon + 2u);
            return false;
        }
        config.scheme = now.scheme; config.layout = now.layout; config.layout_named = now.layout_named; config.version = now.version;
        // The two-stick layer's switch; where it cannot move while the game runs the value waits for a restart.
        if (config.exact_turn != now.exact_turn && host_.exact_turn && !host_.exact_turn(config.exact_turn)) return false;
        if (!config.same_as(now)) (void)host_.input->reconfigure(std::move(config));
        return true;
    }
    Outcome &refused(Outcome &out, const std::string &id, std::string_view wanted, Lock lock) {
        out.lock = lock;
        return refused(out, id, wanted, std::string(lock_text(lock)));
    }
    Outcome &refused(Outcome &out, const std::string &id, std::string_view wanted, const std::string &why) {
        out.ok = false;
        out.error = why;
        out.line = id + "=" + std::string(trimmed(wanted)) + " refused: " + why;
        return out;
    }
    // Writes (when asked) and says what became of the change: in force now, waiting for a restart, or
    // neither (the value is the one the game runs with, or its running value is not known).
    void finish(Outcome &out, const std::string &what, bool waits, bool live, bool write) {
        out.pending = waits;
        out.applied = live;
        std::string wrote = "not written yet";
        if (write) {
            const auto flushed = flush();
            out.written = flushed.written;
            if (!flushed.written) out.error = flushed.error;
            out.remade = flushed.remade;
            wrote = flushed.written ? (flushed.remade ? "written (the settings file was gone: written again whole)" : "written") : "NOT written (" + flushed.error + ")";
        }
        out.line = what + (live ? " applied live" : waits ? " waits for a restart" : " as the game runs it") + ", " + wrote;
    }

    Host host_;
    IniDocument doc_, work_;            // the file as last read; the same with the unwritten changes
    IniKnown known_;                    // the file's bytes as last read or written, and whether it was there
    std::vector<IniChange> unsaved_;
    std::vector<const Setting *> outside_;   // keys an environment variable overrides in this run
    std::string chosen_pack_;
    std::vector<std::string> order_;         // the mod list's order when this page last changed it
};

// The run's settings; null in a run without the page and before main.cpp has made them.
inline Live *&live_slot() noexcept { static Live *slot = nullptr; return slot; }
inline Live *live() noexcept { return live_slot(); }

// CHILI_SETTINGS_AT: values set at game frames, as the page would set them.
class Schedule {
public:
    Schedule() = default;
    // `problems`: one line per step that is not <frame>:<name>=<value>.
    Schedule(std::string_view text, std::vector<std::string> &problems) {
        while (!text.empty()) {
            const auto end = std::min(text.find(';'), text.size());
            const auto item = trimmed(text.substr(0u, end));
            text.remove_prefix(std::min(end + 1u, text.size()));
            if (item.empty()) continue;
            const auto colon = item.find(':');
            std::uint64_t frame = 0u;
            bool digits = colon != std::string_view::npos && colon != 0u && colon <= 9u;
            for (std::size_t i = 0; digits && i < colon; ++i) { digits = item[i] >= '0' && item[i] <= '9'; frame = frame * 10u + static_cast<std::uint64_t>(item[i] - '0'); }
            if (!digits || item.find('=', colon) == std::string_view::npos) { problems.push_back("CHILI_SETTINGS_AT: '" + std::string(item) + "' is not <frame>:<Section>/<Key>=<value>, ignored"); continue; }
            steps_.emplace_back(frame, std::string(trimmed(item.substr(colon + 1u))));
        }
        std::stable_sort(steps_.begin(), steps_.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    }
    bool empty() const noexcept { return steps_.empty(); }
    // The steps that are due at this frame, made; one line each for the log.
    std::vector<std::string> frame(std::uint64_t number, Live &settings, PageFrame where = PageFrame::MainMenu) {
        std::vector<std::string> lines;
        while (!steps_.empty() && number >= steps_.front().first) {
            const auto outcome = settings.set_text(steps_.front().second, where);
            lines.push_back("frame " + std::to_string(number) + ": " + outcome.line);
            steps_.erase(steps_.begin());
        }
        return lines;
    }
private:
    std::vector<std::pair<std::uint64_t, std::string>> steps_;
};
}
