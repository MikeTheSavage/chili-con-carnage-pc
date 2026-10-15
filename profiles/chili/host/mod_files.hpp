#pragma once
#include "naz_archive.hpp"
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <sstream>
namespace chili {
// Mods: folders that replace game files without touching the disc folder.
//
//   <mods>/<name>/disc0/...        shadows files of the disc, e.g. disc0/PSP_GAME/USRDIR/configpsp.txt
//   <mods>/<name>/data_psp/...     replaces or adds files inside assets00.naz; so does every other
//   <mods>/<name>/data/...         top-level folder whose name starts with "data" (data_stream_psp,
//                                  data_baked), and a top-level file named like one in the archive root
//   <mods>/<name>/properties.ini   changes gameplay values inside the archive's blocks by name (block_props.hpp)
//
// <mods> is the "mods" folder beside ChiliNative.ini, [Mods] Folder in that
// file, or CHILI_MODS_DIR. [Mods] Enabled lists the mod folders to load; a
// later name wins over an earlier one and all of them win over the disc.
// Without the key every folder is loaded in alphabetical order, except those
// whose name starts with a dot.
//
// [Game] FrameLock and the key = value lines of [Game.Config] are written over
// the engine's configpsp.txt in memory.
//
//   CHILI_MODS_DIR=<dir>   the mods folder
//   CHILI_TEXTURE_DUMP=<dir>   write every texture the game draws with into <dir> as <key>.png ([Mods] DumpTextures)
//   CHILI_GAME_PRIORITY=high|normal   [Game] Priority for this run (wins over the file)
//   CHILI_VIRTUAL_NAZ=1    serve the archive through the mod layer even when no mod changes it (test switch)
inline constexpr char archive_disc_path[] = "psp_game/usrdir/assets00.naz";
inline constexpr char config_disc_path[] = "psp_game/usrdir/configpsp.txt";

inline const char *default_mods_ini() {
    return
        "\n"
        "[Mods]\n"
        "; Mods live in the \"mods\" folder beside this file; see README.md there.\n"
        "; Folder=mods\n"
        "; Mod folders to load. A later name wins over an earlier one. Without this line every folder is\n"
        "; loaded in alphabetical order; an empty list loads none.\n"
        "; Enabled=first_mod,second_mod\n"
        "; Print each archive file the first time the game reads it.\n"
        "; LogFiles=false\n"
        "; Write every texture the game draws with, once, as texture_dump\\<key>.png beside this file.\n"
        "; A picture at mods\\<mod>\\textures\\<key>.png replaces that texture (mods\\TEXTURES.md).\n"
        "; DumpTextures=false\n"
        "; Memory for the pictures of texture packs, in MB: auto = a sixteenth of the PC's memory (256 to 2048), or a number.\n"
        "; PictureStoreMB=auto\n"
        "\n"
        "[Game]\n"
        "; Game frames per second: 1 = 60 (the default when you play), 2 = 30 as on the PSP. Use 2 when the\n"
        "; game cannot hold 60: the sound then crackles and the game runs slow.\n"
        "; FrameLock=1\n"
        "; The camera behind the player on foot. tight: it stays within about 4 degrees of where the player\n"
        "; faces, so the picture turns with the mouse (the default with Scheme=dual_analog). original: the\n"
        "; game's own, which may trail by a half turn and catches up over a third of a second (the default\n"
        "; with Scheme=psp).\n"
        "; Camera=tight\n"
        "; high: the game runs one step above the other programs' priority, so a busy PC cannot hold it back\n"
        "; for a tenth of a second at a time (heard as a hole in the sound). normal: no such step.\n"
        "; Priority=high\n"
        "; The Settings line in the game's own menus and the page it opens. false: the menus as the PSP game\n"
        "; has them (Fiesta and Sync Highscores do nothing); every setting is still in this file.\n"
        "; SettingsInGame=true\n"
        "\n"
        "[Game.Config]\n"
        "; key = value lines written over the engine's configpsp.txt, for example an archive of your own that\n"
        "; is searched before the game's:\n"
        "; filesystem = \"mod.naz, assets00.naz\"\n";
}

// [Game] Camera=tight as property lines (block_props.hpp), applied before every mod's own so a mod
// still has the last word. MaxAngle is the largest angle the camera may trail the player by, in half
// turns (the game's 2 and 0.12 never limit anything); AngleEaseInSpeed is the share of the remaining
// angle it closes per second (the game's 7.5 and 5). The ease is not clamped: above about 25 it
// overshoots at 30 frames per second, and MaxAngle bounds whatever a long frame does to it.
inline constexpr char tight_camera_properties[] =
    "[CameraDict]\n"
    "CharacterCombatCam.MaxAngle = 0.02\n"
    "CharacterCombatCam.AngleEaseInSpeed = 20\n"
    "CharacterNormalCam.MaxAngle = 0.02\n"
    "CharacterNormalCam.AngleEaseInSpeed = 20\n";

// The sheet of the game's text font (/data/Fonts/ODMidText_24.font, mods/TEXTURES.md) and what the file is
// called that a pack puts beside its replacement to give the typeface's own letter spacing (font_advances.hpp):
// textures\5d0c6d1a663a8515.advances.txt.
inline constexpr std::uint64_t text_font_sheet_key = 0x5d0c6d1a663a8515ull;
inline constexpr char font_advances_file_name[] = "5d0c6d1a663a8515.advances.txt";

// [Mods] PictureStoreMB in bytes: how much memory the decoded pictures of texture packs may take
// (host/gpu/ge_texture_mods.hpp). `setting_mb` 0 is auto: a sixteenth of the PC's memory, not under 256 MiB
// (the store's size before the setting existed) and not over 2048. It is a limit, not a reservation.
inline std::uint64_t picture_store_bytes(std::uint32_t setting_mb, std::uint64_t physical_memory_bytes) noexcept {
    constexpr std::uint64_t mib = 1024ull * 1024ull;
    if (setting_mb != 0u) return setting_mb * mib;
    return std::clamp(physical_memory_bytes / 16u / mib, std::uint64_t{256}, std::uint64_t{2048}) * mib;
}

enum class CameraChoice : std::uint8_t { Auto, Tight, Original };
enum class PriorityChoice : std::uint8_t { Auto, High, Normal };

struct ModSetup {
    std::filesystem::path folder;
    // Enabled mod folders, lowest priority first.
    std::vector<std::filesystem::path> mods;
    // configpsp.txt keys from the ini, in the order they are applied.
    std::vector<std::pair<std::string, std::string>> game_config;
    bool virtual_archive{}, log_files{};
    // [Mods] DumpTextures or CHILI_TEXTURE_DUMP=<folder>: where each texture is written once as <key>.png.
    // Empty: no dump.
    bool dump_textures{};
    std::filesystem::path texture_dump;
    // [Mods] PictureStoreMB: 0 is auto (picture_store_bytes above).
    std::uint32_t picture_store_mb{};
    // mods/<name>/textures of every enabled mod that has one, lowest priority first (host/gpu/ge_texture_mods.hpp).
    std::vector<std::filesystem::path> texture_folders() const {
        std::vector<std::filesystem::path> out;
        std::error_code status;
        for (const auto &mod : mods) if (std::filesystem::is_directory(mod / "textures", status)) out.push_back(mod / "textures");
        return out;
    }
    // False when the software renderer draws: it never shows a replacement picture, so what goes with one (the
    // letter spacing of a replaced font sheet) stays off.
    bool replace_textures{true};
    // Whether `file` is named as a replacement picture is: 16 hex digits and .png, either case (the rule of
    // host/gpu/ge_texture_mods.cpp).
    static bool texture_key_of(const std::filesystem::path &file, std::uint64_t &key) {
        if (!same_text(file.extension().string(), ".png")) return false;
        const auto stem = file.stem().string();
        if (stem.size() != 16u) return false;
        key = 0u;
        for (const char c : lower(stem)) {
            if (c >= '0' && c <= '9') key = (key << 4u) | static_cast<std::uint64_t>(c - '0');
            else if (c >= 'a' && c <= 'f') key = (key << 4u) | static_cast<std::uint64_t>(c - 'a' + 10);
            else return false;
        }
        return true;
    }
    // The replacement picture in force for the texture `key`: textures\<key>.png of the last enabled mod that
    // has one (a later mod wins, as the renderer takes them). Empty when no mod replaces the texture.
    std::filesystem::path replacement_in_force(std::uint64_t key) const {
        std::error_code status;
        for (auto mod = mods.rbegin(); mod != mods.rend(); ++mod)
            for (const auto &entry : std::filesystem::directory_iterator(*mod / "textures", status))
                if (std::uint64_t named = 0u; entry.is_regular_file(status) && texture_key_of(entry.path(), named) && named == key) return entry.path();
        return {};
    }
    // The letter spacing file that goes with the text font's replacement sheet: the one beside the sheet in
    // force and no other. A pack whose sheet a later pack replaces keeps its file for nothing.
    struct FontAdvancesFile {
        std::filesystem::path sheet;                  // the replacement of the font's sheet in force; empty: none
        std::filesystem::path file;                   // the file beside it; empty: that folder has none
        std::vector<std::filesystem::path> ignored;   // such files in the other mod folders
    };
    FontAdvancesFile font_advances_file() const {
        FontAdvancesFile out;
        out.sheet = replacement_in_force(text_font_sheet_key);
        std::error_code status;
        for (const auto &mod : mods) {
            const auto file = mod / "textures" / font_advances_file_name;
            if (!std::filesystem::is_regular_file(file, status)) continue;
            if (!out.sheet.empty() && std::filesystem::equivalent(file.parent_path(), out.sheet.parent_path(), status)) out.file = file;
            else out.ignored.push_back(file);
        }
        return out;
    }
    // [Game] Camera; Auto when the file does not say.
    CameraChoice camera{CameraChoice::Auto};
    // [Game] Priority or CHILI_GAME_PRIORITY; Auto when neither says.
    PriorityChoice priority{PriorityChoice::Auto};
    // Whether the game thread runs above normal priority while it is paced on the real clock: a player's
    // run does, a scripted run does not unless told to, so its frame times compare with earlier builds'.
    bool raised_priority(bool player) const noexcept { return priority == PriorityChoice::High || (priority == PriorityChoice::Auto && player); }
    // properties.ini text of the game's own settings, applied before the mods' files.
    std::string builtin_properties;
    // The "Settings" entries of the game's own menus, made in the blocks as they are served
    // (menu_entry_blocks.hpp): the main menu's Fiesta line turned into the entry, and a line added to the
    // pause menu of every mission block. Only in a run that has the settings page.
    bool menu_entry_main{}, menu_entry_pause{};
    // [Game] SettingsInGame: false takes the Settings lines and the settings page out of a player's run (the
    // way back when one of them misbehaves; the game's network start is still answered).
    bool settings_in_game{true};
    std::vector<std::string> problems;
    // The camera setting in force: without a Camera line the tight one goes with the two-stick scheme.
    bool tight_camera(bool dual_analog) const noexcept { return camera == CameraChoice::Tight || (camera == CameraChoice::Auto && dual_analog); }

    static std::string lower(std::string_view text) {
        std::string out(text);
        for (auto &c : out) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return out;
    }
    static bool same_text(std::string_view a, std::string_view b) { return a.size() == b.size() && lower(a) == lower(b); }
    static std::string_view trim(std::string_view text) {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) text.remove_prefix(1u);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1u);
        return text;
    }
    static std::wstring environment(const wchar_t *name) {
        wchar_t *value = nullptr;
        std::size_t length = 0u;
        if (_wdupenv_s(&value, &length, name) != 0 || !value) return {};
        std::wstring text(value);
        std::free(value);
        return text;
    }

    // Reads the [Mods], [Game] and [Game.Config] sections of ini text. `listed`
    // is set when an Enabled line was present.
    void apply(std::string_view text, std::string &folder_setting, std::vector<std::string> &enabled, bool &listed) {
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
            if (line.front() == '[') {
                section = line.back() == ']' ? std::string(trim(line.substr(1u, line.size() - 2u))) : std::string();
                continue;
            }
            const bool in_mods = same_text(section, "Mods"), game = same_text(section, "Game"), pass = same_text(section, "Game.Config");
            if (!in_mods && !game && !pass) continue;
            const auto problem = [&](const std::string &what) { problems.push_back("line " + std::to_string(line_number) + ": " + what); };
            const auto equals = line.find('=');
            if (equals == std::string_view::npos) { problem("expected name=value"); continue; }
            const auto name = trim(line.substr(0u, equals)), value = trim(line.substr(equals + 1u));
            if (name.empty()) { problem("expected name=value"); continue; }
            const auto set_config = [&](std::string_view key, std::string_view setting) {
                std::erase_if(game_config, [&](const auto &entry) { return same_text(entry.first, key); });
                game_config.emplace_back(key, setting);
            };
            if (pass) set_config(name, value);
            else if (game) {
                if (same_text(name, "FrameLock")) {
                    if (value == "1" || value == "2") set_config("frame_lock", value);
                    else problem("FrameLock must be 1 (60 fps) or 2 (30 fps)");
                } else if (same_text(name, "Camera")) {
                    if (same_text(value, "tight")) camera = CameraChoice::Tight;
                    else if (same_text(value, "original")) camera = CameraChoice::Original;
                    else problem("Camera must be tight or original");
                } else if (same_text(name, "Priority")) {
                    if (same_text(value, "high")) priority = PriorityChoice::High;
                    else if (same_text(value, "normal")) priority = PriorityChoice::Normal;
                    else problem("Priority must be high or normal");
                } else if (same_text(name, "SettingsInGame")) {
                    if (same_text(value, "true") || value == "1") settings_in_game = true;
                    else if (same_text(value, "false") || value == "0") settings_in_game = false;
                    else problem("SettingsInGame must be true or false");
                } else problem("unknown setting '" + std::string(name) + "'");
            } else if (same_text(name, "Folder")) folder_setting = std::string(value);
            else if (same_text(name, "Enabled")) {
                listed = true;
                enabled.clear();
                for (auto rest = value; !rest.empty();) {
                    const auto comma = std::min(rest.find(','), rest.size());
                    const auto entry = trim(rest.substr(0u, comma));
                    rest.remove_prefix(std::min(comma + 1u, rest.size()));
                    if (!entry.empty()) enabled.emplace_back(entry);
                }
            } else if (same_text(name, "LogFiles")) {
                if (same_text(value, "true") || value == "1") log_files = true;
                else if (same_text(value, "false") || value == "0") log_files = false;
                else problem("LogFiles must be true or false");
            } else if (same_text(name, "DumpTextures")) {
                if (same_text(value, "true") || value == "1") dump_textures = true;
                else if (same_text(value, "false") || value == "0") dump_textures = false;
                else problem("DumpTextures must be true or false");
            } else if (same_text(name, "PictureStoreMB")) {
                std::uint32_t megabytes = 0u;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), megabytes);
                if (same_text(value, "auto")) picture_store_mb = 0u;
                else if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && megabytes >= 64u && megabytes <= 8192u) picture_store_mb = megabytes;
                else problem("PictureStoreMB must be auto or a number of MB from 64 to 8192");
            } else problem("unknown setting '" + std::string(name) + "'");
        }
    }

    // Settings from `ini` (it may be missing) and the environment, and the mod
    // folders they select.
    static ModSetup load(const std::filesystem::path &ini) {
        ModSetup setup;
        std::string folder_setting;
        std::vector<std::string> enabled;
        bool listed = false;
        std::error_code status;
        if (std::filesystem::is_regular_file(ini, status)) {
            std::ifstream stream(ini, std::ios::binary);
            std::ostringstream text;
            text << stream.rdbuf();
            setup.apply(text.str(), folder_setting, enabled, listed);
        }
        setup.virtual_archive = environment(L"CHILI_VIRTUAL_NAZ") == L"1";
        if (const auto priority = environment(L"CHILI_GAME_PRIORITY"); !priority.empty()) {
            if (priority == L"high") setup.priority = PriorityChoice::High;
            else if (priority == L"normal") setup.priority = PriorityChoice::Normal;
            else setup.problems.push_back("CHILI_GAME_PRIORITY must be high or normal");
        }
        if (const auto dump = environment(L"CHILI_TEXTURE_DUMP"); !dump.empty()) setup.texture_dump = std::filesystem::absolute(dump, status);
        else if (setup.dump_textures) setup.texture_dump = std::filesystem::absolute(ini, status).parent_path() / "texture_dump";
        const auto from_environment = environment(L"CHILI_MODS_DIR");
        const bool chosen = !from_environment.empty() || !folder_setting.empty();
        if (!from_environment.empty()) setup.folder = from_environment;
        else if (!folder_setting.empty()) setup.folder = std::filesystem::path(folder_setting);
        else setup.folder = "mods";
        if (setup.folder.is_relative()) setup.folder = std::filesystem::absolute(ini, status).parent_path() / setup.folder;
        setup.folder = std::filesystem::weakly_canonical(setup.folder, status);
        if (!std::filesystem::is_directory(setup.folder, status)) {
            if (chosen || !enabled.empty()) setup.problems.push_back("mods folder not found: " + setup.folder.string());
            return setup;
        }
        if (listed) {
            for (const auto &name : enabled) {
                const bool plain = name.find_first_of("/\\:") == std::string::npos && name != "." && name != "..";
                if (plain && std::filesystem::is_directory(setup.folder / name, status)) setup.mods.push_back(setup.folder / name);
                else setup.problems.push_back("mod '" + name + "' is not a folder in " + setup.folder.string());
            }
        } else {
            for (const auto &entry : std::filesystem::directory_iterator(setup.folder, status))
                if (entry.is_directory(status) && entry.path().filename().native().front() != L'.') setup.mods.push_back(entry.path());
            std::sort(setup.mods.begin(), setup.mods.end(), [](const auto &a, const auto &b) {
                return _wcsicmp(a.filename().c_str(), b.filename().c_str()) < 0; });
        }
        return setup;
    }
};

// configpsp.txt with `settings` written over it: a key the text already has
// gets the new value on its line (the key keeps the file's spelling), a new
// key is added as a line at the end.
inline std::string apply_game_config(std::string text, const std::vector<std::pair<std::string, std::string>> &settings) {
    const std::string newline = text.find("\r\n") != std::string::npos || text.find('\n') == std::string::npos ? "\r\n" : "\n";
    for (const auto &[key, value] : settings) {
        bool found = false;
        for (std::size_t start = 0u; start < text.size();) {
            auto end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            auto stop = end;
            if (stop > start && text[stop - 1u] == '\r') --stop;
            const auto line = std::string_view(text).substr(start, stop - start);
            const auto equals = line.find('=');
            if (equals != std::string_view::npos && ModSetup::same_text(ModSetup::trim(line.substr(0u, equals)), key)) {
                const auto replacement = std::string(ModSetup::trim(line.substr(0u, equals))) + " = " + value;
                text.replace(start, stop - start, replacement);
                end = start + replacement.size() + (end - stop);
                found = true;
            }
            start = end + 1u;
        }
        if (!found) {
            if (!text.empty() && text.back() != '\n') text += newline;
            text += key + " = " + value + newline;
        }
    }
    return text;
}

// Adds the commented [Mods] and [Game] template to an ini that has neither.
inline void add_default_mod_settings(const std::filesystem::path &ini) {
    std::error_code status;
    if (!std::filesystem::is_regular_file(ini, status)) return;
    std::string text;
    {
        std::ifstream stream(ini, std::ios::binary);
        std::ostringstream content;
        content << stream.rdbuf();
        text = ModSetup::lower(content.str());
    }
    if (text.find("[mods]") != std::string::npos || text.find("[game") != std::string::npos) return;
    std::ofstream stream(ini, std::ios::binary | std::ios::app);
    stream << default_mods_ini();
}
}
