// host/settings_table.hpp, the settings as the game's own settings page knows them, against the launcher:
// the two are read from the same file by different programs and must not drift apart.
//
//   chili_settings_table_test <launcher/src folder>
//
// The launcher's side is read from its source text: Model.cs (every key with its default, the actions, the
// mouse's inputs and a pad's buttons with the defaults of both control styles) and Pages.cs (every row: the
// values of a choice and the word of each, a slider's range and step, the switches, the Quality bundles).
// The table must have the same keys in the same order with the same defaults, the same values in the same
// order, the same words (the rendering resolution is shown as its size in the game, as a multiple in the
// launcher), and the same ranges. Where the game's page names a row otherwise than the launcher (its font
// has room for 18 characters), the pair is listed below, so a rename on either side is noticed.
//
// And against the game: every value the table offers for a key, every step of every slider and every
// default is put through the game's own reader of that section, which must take it without a complaint.
#include "audio_settings.hpp"
#include "pc_input.hpp"
#include "render_settings.hpp"
#include "settings_table.hpp"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {
namespace st = chili::settings;
unsigned checks, failures;
const bool keep_going = [] { const char *value = std::getenv("CHILI_TEST_KEEP_GOING"); return value && *value == '1'; }();
void require(bool ok, const std::string &what) {
    ++checks;
    if (ok) return;
    if (!keep_going) throw std::runtime_error(what);
    ++failures;
    std::cerr << "FAILED CHECK: " << what << '\n';
}
std::string file_text(const std::filesystem::path &file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read " + file.string());
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}
// C# source without its // comments (a commented-out row is not a row), strings kept as they are.
std::string without_comments(const std::string &text) {
    std::string out;
    bool in_string = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) { out.push_back(c); if (c == '\\' && i + 1u < text.size()) out.push_back(text[++i]); else if (c == '"') in_string = false; continue; }
        if (c == '"') { in_string = true; out.push_back(c); continue; }
        if (c == '/' && i + 1u < text.size() && text[i + 1u] == '/') { while (i < text.size() && text[i] != '\n') ++i; out.push_back('\n'); continue; }
        out.push_back(c);
    }
    return out;
}
// The text of a call from `open` (the place of its opening parenthesis) to the matching one.
std::string call_text(const std::string &text, std::size_t open) {
    int depth = 0;
    bool in_string = false;
    for (std::size_t i = open; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) { if (c == '\\') ++i; else if (c == '"') in_string = false; continue; }
        if (c == '"') in_string = true;
        else if (c == '(') ++depth;
        else if (c == ')' && --depth == 0) return text.substr(open, i - open + 1u);
    }
    throw std::runtime_error("a call in the launcher's source is not closed");
}
using Pairs = std::vector<std::pair<std::string, std::string>>;
Pairs all_pairs(const std::string &text, const std::regex &pattern) {
    Pairs out;
    for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) out.emplace_back((*it)[1].str(), (*it)[2].str());
    return out;
}
std::string block_after(const std::string &text, const std::string &marker, const std::string &until) {
    const auto from = text.find(marker);
    if (from == std::string::npos) throw std::runtime_error("the launcher's source has no '" + marker + "'");
    const auto to = text.find(until, from);
    return text.substr(from, to == std::string::npos ? std::string::npos : to - from);
}
std::string lookup(const Pairs &pairs, const std::string &name, const std::string &otherwise) {
    for (const auto &[key, value] : pairs) if (st::same_name(key, name)) return value;
    return otherwise;
}

// ---- Model.cs: the keys ----------------------------------------------------------------------------

struct LauncherKey { std::string ident, section, key, fallback; };
std::map<std::string, const st::Setting *> by_ident;

void keys(const std::string &model) {
    // public static readonly Setting Fullscreen = Add("Rendering", "Fullscreen", "false", "Fullscreen must be");
    std::vector<LauncherKey> named;
    const std::regex add(R"re(Setting\s+(\w+)\s*=\s*Add\("([^"]*)",\s*"([^"]*)",\s*(null|"([^"]*)"))re");
    for (std::sregex_iterator it(model.begin(), model.end(), add), end; it != end; ++it)
        named.push_back({(*it)[1].str(), (*it)[2].str(), (*it)[3].str(), (*it)[4].str() == "null" ? std::string() : (*it)[5].str()});
    require(named.size() == 52u, "Model.cs names 52 settings with Add (found " + std::to_string(named.size()) + ")");
    for (std::size_t i = 0; i < named.size(); ++i) {
        const auto &theirs = named[i];
        const auto &ours = st::table[i];
        const std::string where = "[" + theirs.section + "] " + theirs.key;
        require(ours.section == theirs.section && ours.key == theirs.key, "key " + std::to_string(i + 1u) + " of the launcher is " + where + ", of the table " + ours.id());
        require(ours.fallback == theirs.fallback, where + ": default '" + theirs.fallback + "' in the launcher, '" + std::string(ours.fallback) + "' in the table");
        require(ours.kind != st::Kind::Binding, where + " is a setting, not a binding");
        by_ident[theirs.ident] = &ours;
    }
    // The defaults that follow something: the camera goes with the control style; the two that follow the
    // game build are the newer build's here.
    require(model.find(R"(Camera.DefaultFn = m => m.Scheme == "psp" ? "original" : "tight";)") != std::string::npos, "the launcher's camera default follows the control style");
    const auto &camera = st::get("Game", "Camera");
    require(camera.default_for(false) == "tight" && camera.default_for(true) == "original", "the table's camera default follows the control style");
    require(model.find(R"(AntiAliasing.DefaultFn = m => m.Game.Knows(AntiAliasing.Probe) ? "fxaa" : "off";)") != std::string::npos &&
            model.find(R"(Anisotropy.DefaultFn = m => m.Game.Knows(Anisotropy.Probe) ? "16" : "1";)") != std::string::npos, "the launcher's defaults for a game that knows the newer keys");
    for (const auto &setting : st::table)
        require(!setting.fallback_psp || setting.kind == st::Kind::Binding || &setting == &camera, setting.id() + ": only the camera and bindings have a default of their own with Scheme=psp");
    require(model.find("DiscPath.Raw = true;") != std::string::npos, "the disc path is the launcher's one raw value");

    // The bindings: the actions' keys, the mouse's inputs, a pad's buttons, in the launcher's order.
    Pairs actions;
    {
        const std::regex target(R"re(new ActionDef \{ Target = "(\w+)", Label = "([^"]*)")re");
        actions = all_pairs(block_after(model, "public static readonly ActionDef[] All", "};"), target);
    }
    require(actions.size() == 23u, "the launcher has 23 actions");
    const std::regex pair(R"re(\{\s*"([^"]*)",\s*"([^"]*)"\s*\})re"), word(R"re("()([^"]*)")re");
    const Pairs keyboard = all_pairs(block_after(model, "> keyboard = new", "};"), pair), mouse = all_pairs(block_after(model, "> mouse = new", "};"), pair),
        pad_dual = all_pairs(block_after(model, "> padDual = new", "};"), pair), pad_psp = all_pairs(block_after(model, "> padPsp = new", "};"), pair);
    const Pairs mouse_inputs = all_pairs(block_after(model, "string[] MouseInputs", "};"), word), pad_buttons = all_pairs(block_after(model, "string[] PadButtons", "};"), word);
    require(keyboard.size() == 23u && mouse.size() == 7u && pad_dual.size() == 16u && pad_psp.size() == 16u && mouse_inputs.size() == 7u && pad_buttons.size() == 16u,
        "the launcher's default tables: 23 keys, 7 mouse inputs, 16 pad buttons for each control style");
    require(model.find(R"(if (scheme == "psp" && target.StartsWith("Look", StringComparison.OrdinalIgnoreCase)) return "";)") != std::string::npos, "the launcher gives the look keys no key with Scheme=psp");
    std::size_t at = named.size();
    const auto binding = [&](const std::string &section, const std::string &key, const std::string &fallback, const std::string &fallback_psp) {
        require(at < st::table.size(), "the table has an entry for the launcher's [" + section + "] " + key);
        const auto &ours = st::table[at++];
        const std::string where = "[" + section + "] " + key;
        require(ours.section == section && ours.key == key && ours.kind == st::Kind::Binding, "entry " + std::to_string(at) + " of the table is the binding " + where + " (it is " + ours.id() + ")");
        require(ours.default_for(false) == fallback, where + ": default '" + fallback + "' in the launcher, '" + std::string(ours.default_for(false)) + "' in the table");
        require(ours.default_for(true) == fallback_psp, where + " with Scheme=psp: '" + fallback_psp + "' in the launcher, '" + std::string(ours.default_for(true)) + "' in the table");
        require(!ours.shown() && ours.cls == st::Class::Live, where + ": a binding is live and not a row of the game's page");
    };
    for (const auto &action : actions) {
        const auto &target = action.first;
        const auto fallback = lookup(keyboard, target, "");
        binding("Input.Keyboard", target, fallback, target.rfind("Look", 0u) == 0u ? std::string() : fallback);
    }
    for (const auto &input : mouse_inputs) { const auto target = lookup(mouse, input.second, "None"); binding("Input.Mouse", input.second, target, target); }
    for (const auto &button : pad_buttons) binding("Input.Gamepad", button.second, lookup(pad_dual, button.second, "None"), lookup(pad_psp, button.second, "None"));
    require(at == st::table.size(), "the table has no entry the launcher lacks (" + std::to_string(st::table.size()) + " entries, " + std::to_string(at) + " in the launcher)");
    // No key twice, none empty.
    std::set<std::string> seen;
    for (const auto &setting : st::table) {
        require(!setting.section.empty() && !setting.key.empty(), "no empty entry in the table");
        require(seen.insert(st::lowered(setting.id())).second, setting.id() + " is in the table once");
        require(st::find(setting.section, setting.key) == &setting && st::find(setting.id()) == &setting, setting.id() + " is found by its name");
    }
    require(!st::find("Rendering", "Nonsense") && !st::find("Rendering") && st::find("input.mouse/invertx") == &st::get("Input.Mouse", "InvertX"), "a wrong name finds nothing; names are without case");
}

// ---- Pages.cs: the rows ----------------------------------------------------------------------------

// The game's short names where they differ from the launcher's labels (design 2.3).
const std::map<std::string, std::string> renamed{
    {"Rendering resolution", "Resolution"}, {"Auto resolution limit", "Auto limit"}, {"Texture filtering", "Texture filter"},
    {"Sharp textures at an angle", "Sharp textures"}, {"Keep the aim where you put it", "Hold the aim"}, {"Clicks work in menus", "Clicks in menus"},
    {"Precise turning for stick and keys", "Precise turning"}, {"Left stick dead zone", "Left dead zone"}, {"Right stick dead zone", "Right dead zone"},
    {"Start moving at the dead zone's edge", "Move at the edge"}, {"Use PlayStation controllers", "PlayStation pads"}, {"On screen", "Prompts"},
    {"Dump textures while playing", "Dump textures"}};

std::set<const st::Setting *> launcher_rows;
const st::Setting &of_ident(const std::string &ident) {
    const auto found = by_ident.find(ident);
    require(found != by_ident.end(), "Pages.cs names S." + ident + ", which Model.cs makes with Add");
    return *found->second;
}
void row_name(const st::Setting &setting, const std::string &label) {
    launcher_rows.insert(&setting);
    if (!setting.shown()) return;
    const auto other = renamed.find(label);
    const std::string expected = other == renamed.end() ? label : other->second;
    require(setting.name == expected, setting.id() + ": the launcher's row is '" + label + "', the game's '" + std::string(setting.name) + "' (expected '" + expected + "')");
}
void same_values(const st::Setting &setting, const Pairs &options, bool words) {
    const std::string where = setting.id();
    require(setting.kind == st::Kind::Choice, where + " is a choice in the launcher and in the table");
    require(setting.values.size() == options.size(), where + ": " + std::to_string(options.size()) + " values in the launcher, " + std::to_string(setting.values.size()) + " in the table");
    for (std::size_t i = 0; i < options.size() && i < setting.values.size(); ++i) {
        require(setting.values[i].ini == options[i].second, where + ": value " + std::to_string(i + 1u) + " is '" + options[i].second + "' in the launcher, '" + std::string(setting.values[i].ini) + "' in the table");
        if (words) require(setting.values[i].word == options[i].first, where + "=" + options[i].second + ": '" + options[i].first + "' in the launcher, '" + std::string(setting.values[i].word) + "' in the table");
    }
}

void rows(const std::string &pages) {
    const std::regex option(R"re((?:\bO|new Option)\("([^"]*)",\s*"([^"]*)")re");
    // Seg("Label", S.Key, O("word", "value"), ...)
    unsigned choices = 0u, switches = 0u, sliders = 0u;
    for (std::size_t at = 0; (at = pages.find("Seg(\"", at)) != std::string::npos; ++at) {
        const auto call = call_text(pages, at + 3u);
        std::smatch head;
        require(std::regex_search(call, head, std::regex(R"re(^\("([^"]*)",\s*S\.(\w+),)re")), "a Seg row names its label and its setting: " + call.substr(0u, 60u));
        const auto &setting = of_ident(head[2].str());
        row_name(setting, head[1].str());
        const bool resolution = &setting == &st::get("Rendering", "InternalScale");
        same_values(setting, all_pairs(call, option), !resolution);
        if (resolution) {
            // The launcher shows the multiple and reads the size out beside it; the game's row shows the size.
            for (const auto &value : setting.values)
                require(value.ini == "auto" ? value.word == "Auto" : value.word == std::to_string(480 * std::stoi(std::string(value.ini))) + " x " + std::to_string(272 * std::stoi(std::string(value.ini))),
                    "Rendering/InternalScale=" + std::string(value.ini) + " is shown as its size");
        }
        ++choices;
    }
    // new Segmented(get, set, O(...), ...) inside a row that is made by hand: its setting stands in the row.
    for (std::size_t at = 0; (at = pages.find("new Segmented(", at)) != std::string::npos; ++at) {
        const auto call = call_text(pages, at + 13u);
        const auto options = all_pairs(call, option);
        const auto plain = pages.rfind("new Row", at), tabs = pages.rfind("new TabsRow", at);
        const auto row = plain == std::string::npos ? tabs : tabs == std::string::npos ? plain : std::max(plain, tabs);
        if (row == std::string::npos || options.empty()) continue;
        const std::string before = pages.substr(row, at - row);
        std::smatch found;
        if (std::regex_search(before, found, std::regex(R"re(Settings = new\[\] \{ S\.(\w+) \})re"))) {
            const auto &setting = of_ident(found[1].str());
            std::smatch label;
            if (std::regex_search(before, label, std::regex(R"re(Label = "([^"]*)")re"))) row_name(setting, label[1].str());
            same_values(setting, options, true);
            ++choices;
        } else if (before.find("Id = \"aa\"") != std::string::npos) {
            const auto &setting = st::get("Rendering", "AntiAliasing");
            row_name(setting, "Anti-aliasing");
            same_values(setting, options, true);
            ++choices;
        } else if (before.find("Id = \"quality\"") != std::string::npos) {
            require(options.size() == std::size(st::qualities) + 1u, "Quality has four bundles and Custom");
            for (std::size_t i = 0; i < std::size(st::qualities); ++i)
                require(options[i].second == st::qualities[i].name && options[i].first == st::qualities[i].word, "Quality's choice " + std::to_string(i + 1u) + " is " + options[i].first + " (" + options[i].second + ")");
            require(options.back().second == st::quality_custom && options.back().first == st::quality_custom_word, "Quality's last choice is Custom");
        } else if (before.find("Id = \"textures\"") != std::string::npos) {
            require(options.size() == 2u && options[0].second == st::textures_values[0].ini && options[0].first == st::textures_values[0].word &&
                    options[1].second == st::textures_values[1].ini && options[1].first == st::textures_values[1].word, "Textures is Standard or HD pack");
        }
    }
    // Tog("Label", S.Key)
    const std::regex toggle(R"re(Tog\("([^"]*)",\s*S\.(\w+)\))re");
    for (std::sregex_iterator it(pages.begin(), pages.end(), toggle), end; it != end; ++it) {
        const auto &setting = of_ident((*it)[2].str());
        row_name(setting, (*it)[1].str());
        require(setting.kind == st::Kind::Switch, setting.id() + " is a switch in the launcher and in the table");
        require(setting.values.size() == 2u && setting.values[0].ini == "false" && setting.values[1].ini == "true" && setting.values[0].word == "off" && setting.values[1].word == "on", setting.id() + ": a switch is off or on");
        ++switches;
    }
    // Sld("Label", S.Key, min, max, step, show[, log[, format]])
    const std::regex slide(R"re(Sld\("([^"]*)",\s*S\.(\w+),\s*([0-9.]+),\s*([0-9.]+),\s*([0-9.]+),\s*(\w+))re");
    for (std::sregex_iterator it(pages.begin(), pages.end(), slide), end; it != end; ++it) {
        const auto &setting = of_ident((*it)[2].str());
        row_name(setting, (*it)[1].str());
        const double min = std::stod((*it)[3].str()), max = std::stod((*it)[4].str()), step = std::stod((*it)[5].str());
        require(setting.kind == st::Kind::Slider, setting.id() + " is a slider in the launcher and in the table");
        require(setting.min == min && setting.max == max && setting.step == step, setting.id() + ": from " + (*it)[3].str() + " to " + (*it)[4].str() + " in steps of " + (*it)[5].str() + " in the launcher");
        const std::string show = (*it)[6].str();
        require(setting.show == (show == "Percent" ? st::Show::Percent : show == "null" ? st::Show::Number : st::Show::Milliseconds), setting.id() + ": shown as the launcher shows it (" + show + ")");
        ++sliders;
    }
    require(choices == 17u && switches == 17u && sliders == 12u, "the launcher's rows: 17 choices, 17 switches, 12 sliders (found " + std::to_string(choices) + ", " + std::to_string(switches) + ", " + std::to_string(sliders) + ")");
    // Every switch, choice and slider of the table is a row of the launcher; the older MSAA key is none (the
    // launcher keeps it in step with AntiAliasing), and neither is any binding or text.
    for (const auto &setting : st::table) {
        const bool control = setting.kind == st::Kind::Switch || setting.kind == st::Kind::Choice || setting.kind == st::Kind::Slider;
        const bool msaa = &setting == &st::get("Rendering", "MSAA");
        require(launcher_rows.count(&setting) == (control && !msaa ? 1u : 0u), setting.id() + (control ? " is a row of the launcher" : " is no switch, choice or slider of the launcher"));
    }
    // The Quality bundles.
    const std::regex bundle(R"re(new Quality \{ Name = "(\w+)", Aa = "(\w+)", AaOld = "\w+", Aniso = "(\w+)", Filter = "(\w+)", Ssao = "(\w+)", Glow = "(\w+)" \})re");
    std::size_t index = 0;
    for (std::sregex_iterator it(pages.begin(), pages.end(), bundle), end; it != end; ++it, ++index) {
        require(index < std::size(st::qualities), "the launcher has four Quality bundles");
        const auto &q = st::qualities[index];
        require(q.name == (*it)[1].str() && q.anti_aliasing == (*it)[2].str() && q.anisotropy == (*it)[3].str() && q.texture_filter == (*it)[4].str() && q.ambient == (*it)[5].str() && q.glow == (*it)[6].str(),
            "Quality " + (*it)[1].str() + " is the same bundle in the launcher and in the table");
    }
    require(index == std::size(st::qualities), "the table has the launcher's four Quality bundles");
    require(pages.find("ApplyQuality(\"high\")") != std::string::npos && st::quality_default == "high", "Defaults puts Quality to High");
}

// ---- the table by itself ---------------------------------------------------------------------------

// The characters the game's text font has as letters (design 2.1): ASCII without < > \ ^ and the backtick;
// { } [ ] @ ~ | are pictures in it.
bool writable(std::string_view text) {
    for (const char c : text) if (c < 0x20 || c > 0x7E || std::string_view("<>\\^`{}[]@~|").find(c) != std::string_view::npos) return false;
    return true;
}
void table() {
    std::map<st::Class, unsigned> classes;
    std::set<std::string> hidden;
    for (const auto &setting : st::table) {
        ++classes[setting.cls];
        if (!setting.shown() && setting.kind != st::Kind::Binding) hidden.insert(setting.id());
        if (setting.shown()) {
            require(!setting.name.empty() && setting.name.size() <= 18u && writable(setting.name), setting.id() + ": a name of at most 18 characters the game's font can write ('" + std::string(setting.name) + "')");
            require(setting.cls != st::Class::Off, setting.id() + ": a row of the page is not class Off");
            for (const auto &value : setting.values) require(!value.word.empty() && value.word.size() <= 12u && writable(value.word), setting.id() + "=" + std::string(value.ini) + ": a word of at most 12 characters ('" + std::string(value.word) + "')");
        } else require(setting.name.empty(), setting.id() + " is not shown and has no name");
        if (setting.kind == st::Kind::Slider) {
            require(setting.max > setting.min && setting.step > 0.0 && std::fabs(st::slider_steps(setting) * setting.step - (setting.max - setting.min)) < 1e-9, setting.id() + ": the steps fill the range");
            require(st::allowed(setting, setting.fallback) && st::number(setting.fallback), setting.id() + ": the default is in the range");
        }
        if (setting.kind == st::Kind::Choice || setting.kind == st::Kind::Switch) require(st::value_index(setting, setting.fallback).has_value(), setting.id() + ": the default is one of the values");
    }
    // 97 keys, 79 live, 3 moderate, 2 split by value, 7 restart, 6 not for a page;
    // and [Rendering] Glow, a live one.
    require(classes[st::Class::Live] == 80u && classes[st::Class::Moderate] == 3u && classes[st::Class::Split] == 2u && classes[st::Class::Restart] == 7u && classes[st::Class::Off] == 6u,
        "the classes: 80 live, 3 moderate, 2 split, 7 restart, 6 off (" + std::to_string(classes[st::Class::Live]) + ", " + std::to_string(classes[st::Class::Moderate]) + ", " +
        std::to_string(classes[st::Class::Split]) + ", " + std::to_string(classes[st::Class::Restart]) + ", " + std::to_string(classes[st::Class::Off]) + ")");
    // What stays in the launcher (design 2.9), beside the bindings; [Mods] Enabled is shown through Textures and HD pack.
    const std::set<std::string> expected{"Rendering/WindowSize", "Rendering/MSAA", "Rendering/Backend", "Rendering/GeThread", "Game/Priority", "Mods/Folder", "Mods/Enabled",
        "Mods/LogFiles", "Disc/Path", "Input/BackgroundInput", "Input/ShowInput", "Input/Version"};
    require(hidden == expected, "the keys the game's page does not show are the twelve of the design");

    // The rows: every shown key once, on its page; a section's page ends with Defaults; the counts of the design.
    std::map<const st::Setting *, unsigned> placed;
    std::map<st::Page, unsigned> counts;
    for (const auto &row : st::rows) {
        ++counts[row.page];
        require(!row.name.empty() && row.name.size() <= 18u && writable(row.name), "a row's name fits and can be written: '" + std::string(row.name) + "'");
        if (row.kind == st::RowKind::Setting) {
            require(row.setting && row.setting->page == row.page && row.name == row.setting->name, std::string(row.name) + " is a row of its setting's page");
            ++placed[row.setting];
        } else require(!row.setting, std::string(row.name) + " has no setting");
        if (row.kind == st::RowKind::Link) require(row.target != st::Page::None && st::page_parent(row.target) == row.page && st::page_name(row.target) == row.name, std::string(row.name) + " opens the page of its name, which goes back to this one");
    }
    for (const auto &setting : st::table) require(placed[&setting] == (setting.shown() ? 1u : 0u), setting.id() + (setting.shown() ? " is a row once" : " is no row"));
    require(counts[st::Page::Top] == 5u && counts[st::Page::Display] == 6u && counts[st::Page::Graphics] == 7u && counts[st::Page::Controls] == 7u &&
            counts[st::Page::KeyboardMouse] == 13u && counts[st::Page::Gamepad] == 13u && counts[st::Page::TexturesMods] == 4u && counts[st::Page::Sound] == 2u, "the pages have the design's rows");
    for (const st::Page page : {st::Page::Display, st::Page::Graphics, st::Page::Controls, st::Page::KeyboardMouse, st::Page::Gamepad, st::Page::TexturesMods, st::Page::Sound}) {
        const auto list = st::rows_of(page);
        require(list.size() == counts[page] && list.back().kind == st::RowKind::Defaults, std::string(st::page_name(page)) + " ends with Defaults");
        for (const auto &row : list) require(row.page == page, "rows_of gives a page's own rows");
    }
    require(st::rows_of(st::Page::Top).front().name == "Display" && st::rows_of(st::Page::Top).back().name == "Sound" && st::rows_of(st::Page::None).empty(), "the top page lists the five sections");
    require(st::rows_of(st::Page::Display)[1].name == "Resolution" && st::rows_of(st::Page::Gamepad)[3].name == "Precise turning" && st::rows_of(st::Page::Controls)[2].target == st::Page::KeyboardMouse, "rows in the design's order");
    require(st::rows_of(st::Page::Graphics)[4].name == "Ambient shadows" && st::rows_of(st::Page::Graphics)[5].name == "Glow" && st::rows_of(st::Page::Graphics)[5].setting == &st::get("Rendering", "Glow"), "Glow is the Graphics page's sixth row, before Defaults");
    for (const st::Lock lock : {st::Lock::NeedsRestart, st::Lock::OnlyWithAuto, st::Lock::PspCannotLook, st::Lock::NotWithPsp, st::Lock::NoHdPack, st::Lock::TexturesStandard, st::Lock::InUse, st::Lock::SetOutside})
        require(!st::lock_text(lock).empty() && st::lock_text(lock).back() == '.' && writable(st::lock_text(lock)) && st::lock_text(lock).size() <= 30u, "a lock's reason is one short clause that fits a line of the pause box: " + std::string(st::lock_text(lock)));
    require(st::lock_text(st::Lock::None).empty(), "no reason without a lock");

    // Values.
    const auto &scale = st::get("Rendering", "InternalScale"), &aa = st::get("Rendering", "AntiAliasing"), &turn = st::get("Input.Mouse", "SensitivityX"),
               &walk = st::get("Input.Keyboard", "WalkScale"), &buffer = st::get("Audio", "BufferMs"), &hold = st::get("Input.Mouse", "HoldPitch"), &rumble = st::get("Input.Gamepad", "Rumble");
    require(st::equivalent("TRUE", "1") && st::equivalent(" false ", "0") && st::equivalent("0.60", "0.6") && st::equivalent("Auto", "auto") && !st::equivalent("1", "2") && !st::equivalent("true", "false") && !st::equivalent("fxaa", "off"), "values that say the same");
    require(st::number_text(0.08) == "0.08" && st::number_text(1.0) == "1" && st::number_text(0.6000000001) == "0.6" && st::number_text(70.0) == "70" && st::number_text(0.0) == "0" && st::number_text(0.125) == "0.125" && st::number_text(3.95) == "3.95", "a number as the launcher writes it");
    require(st::shown(scale, "auto") == "Auto" && st::shown(scale, "4") == "1920 x 1088" && st::shown(scale, "5") == "2400 x 1360" && st::shown(scale, "nonsense") == "nonsense", "a resolution is shown as its size");
    require(st::shown(aa, "FXAA") == "Light" && st::shown(aa, "none") == "Off" && st::shown(st::get("Rendering", "Preset"), "custom") == "No limit" && st::shown(st::get("Rendering", "SSAO"), "0") == "Off", "the spellings the game's reader takes are shown as their value");
    const auto &glow = st::get("Rendering", "Glow");
    require(st::shown(glow, "smooth") == "Smooth" && st::shown(glow, "GAME") == "Original" && st::shown(glow, "off") == "Off" && glow.fallback == "smooth" && glow.environment == "CHILI_RENDER_GLOW" &&
            st::stepped(glow, "smooth", 1) == "game" && st::stepped(glow, "off", 1) == "smooth" && st::stepped(glow, "smooth", -1) == "off" && !st::allowed(glow, "bright"), "the glow is Smooth, Original or Off");
    for (const auto &q : st::qualities) require(q.glow == (q.name == "original" ? "game" : "smooth"), "Quality " + std::string(q.name) + ": the game's own glow with Original, the smooth one otherwise");
    require(st::shown(turn, "0.08") == "0.08" && st::shown(turn, "1") == "1.00" && st::shown(walk, "0.6") == "60 %" && st::shown(rumble, "1") == "100 %" && st::shown(buffer, "70") == "70 ms" && st::shown(hold, "1") == "on" && st::shown(hold, "FALSE") == "off", "a slider's number in its manner, a switch as on or off");
    require(st::stepped(scale, "auto", 1) == "1" && st::stepped(scale, "auto", -1) == "8" && st::stepped(scale, "8", 1) == "auto" && st::stepped(scale, "4", 1) == "6" && st::stepped(scale, "5", 1) == "auto" && st::stepped(scale, "5", -1) == "8", "a choice steps through its values and wraps");
    require(st::stepped(hold, "true", 1) == "false" && st::stepped(hold, "false", -1) == "true" && st::stepped(hold, "1", 1) == "false", "a switch flips either way");
    require(st::stepped(turn, "0.08", 1) == "0.09" && st::stepped(turn, "0.08", -1) == "0.07" && st::stepped(turn, "0.01", -1) == "0.01" && st::stepped(turn, "1", 1) == "1" && st::stepped(turn, "0.085", 1) == "0.09" && st::stepped(turn, "0.085", -1) == "0.08", "a slider steps and stops at its ends; from between two steps to the one beside it");
    // The file may hold more than a row offers (the reader takes a mouse speed up to 5, the row ends at 1).
    require(st::stepped(turn, "1.5", 1) == "1.5" && st::stepped(turn, "1.5", -1) == "1" && st::stepped(turn, "0.005", -1) == "0.005" && st::stepped(turn, "0.005", 1) == "0.01",
        "a slider's value outside the row's range: a step away from the range leaves it, a step towards it goes to the range's end");
    require(st::stepped(buffer, "70", 1) == "80" && st::stepped(buffer, "500", 1) == "500" && st::stepped(buffer, "25", -1) == "20" && st::stepped(walk, "0.31", 1) == "0.32" && st::stepped(rumble, "0.95", 1) == "1", "sliders of other ranges");
    require(st::allowed(scale, "AUTO") && st::allowed(scale, "8") && !st::allowed(scale, "5") && !st::allowed(scale, "") && st::allowed(turn, "0.5") && !st::allowed(turn, "0") && !st::allowed(turn, "1.01") && !st::allowed(turn, "fast") &&
            st::allowed(hold, "0") && !st::allowed(hold, "maybe") && st::allowed(st::get("Disc", "Path"), "anything; at all"), "what a key may hold");
    // A slider that is stepped from one end to the other meets every step once and each is its own text.
    for (const auto &setting : st::table) {
        if (setting.kind != st::Kind::Slider) continue;
        std::string value = st::slider_value(setting, 0);
        std::set<std::string> met{value};
        for (long i = 0; i < st::slider_steps(setting); ++i) { value = st::stepped(setting, value, 1); require(met.insert(value).second, setting.id() + ": every step up is a new value (" + value + ")"); }
        require(st::equivalent(value, st::number_text(setting.max)) && st::stepped(setting, value, 1) == value, setting.id() + ": the last step is the highest value");
        for (long i = 0; i < st::slider_steps(setting); ++i) { const auto down = st::stepped(setting, value, -1); require(met.count(down) == 1u && down != value, setting.id() + ": every step down is a step that was met going up"); value = down; }
        require(st::equivalent(value, st::number_text(setting.min)), setting.id() + ": and back at the lowest");
    }
}

// ---- the game's own readers ------------------------------------------------------------------------

// The complaints of the game's reader of the key's section about one line.
std::vector<std::string> read_by_game(const st::Setting &setting, std::string_view value) {
    const std::string text = "[" + std::string(setting.section) + "]\n" + std::string(setting.key) + "=" + std::string(value) + "\n";
    if (setting.section.substr(0u, 5u) == "Input") { auto config = chili::PcInputConfig::defaults(); return config.apply(text); }
    if (setting.section == "Rendering") { chili::RenderSettings render; render.apply(text); return render.problems; }
    if (setting.section == "Audio") { chili::AudioSettings audio; audio.apply(text); return audio.problems; }
    if (setting.section == "Game" || setting.section == "Mods") {
        chili::ModSetup mods; std::string folder; std::vector<std::string> enabled; bool listed = false;
        mods.apply(text, folder, enabled, listed);
        return mods.problems;
    }
    return {};
}
void readers() {
    unsigned lines = 0u;
    const auto taken = [&](const st::Setting &setting, std::string_view value) {
        const auto problems = read_by_game(setting, value);
        ++lines;
        require(problems.empty(), setting.id() + "=" + std::string(value) + " is taken by the game's reader" + (problems.empty() ? "" : ": " + problems.front()));
    };
    for (const auto &setting : st::table) {
        // The game has no reader for a window size yet; the launcher shows that row only for a build that has.
        if (&setting == &st::get("Rendering", "WindowSize") || &setting == &st::get("Disc", "Path")) continue;
        taken(setting, setting.default_for(false));
        taken(setting, setting.default_for(true));
        for (const auto &value : setting.values) taken(setting, value.ini);
        if (setting.kind == st::Kind::Slider) for (long step = 0; step <= st::slider_steps(setting); ++step) taken(setting, st::slider_value(setting, step));
    }
    // And the reader does complain when it should, so the check above means something.
    require(!read_by_game(st::get("Rendering", "Anisotropy"), "3").empty() && !read_by_game(st::get("Rendering", "Glow"), "bright").empty() && !read_by_game(st::get("Input.Mouse", "SensitivityX"), "9").empty() &&
            !read_by_game(st::get("Game", "FrameLock"), "3").empty() && !read_by_game(st::get("Audio", "BufferMs"), "5").empty() && !read_by_game(st::get("Input.Gamepad", "A"), "Banana").empty(), "the game's readers refuse a wrong value");
    std::cout << "values put through the game's own readers: " << lines << '\n';
}
}

int main(int argc, char **argv) {
    try {
        if (argc != 2) { std::cerr << "usage: chili_settings_table_test <launcher/src folder>\n"; return 2; }
        const std::filesystem::path source = argv[1];
        const std::string model = without_comments(file_text(source / "Model.cs")), pages = without_comments(file_text(source / "Pages.cs"));
        keys(model);
        rows(pages);
        table();
        readers();
        if (failures) { std::cerr << failures << " of " << checks << " checks FAILED\n"; return 1; }
        std::cout << "settings table: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
