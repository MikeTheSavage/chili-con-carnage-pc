#include "file_runtime.hpp"
#include "fat_names.hpp"
#include "font_advances.hpp"
#include "fixed_file_time.hpp"
#include "frame_costs.hpp"
#include <algorithm>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cwchar>
#include <cctype>
#include <ctime>
#include <cstdio>
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <sys/stat.h>
namespace chili {
namespace {
constexpr auto badfd = 0x80010009u, badarg = 0x80010016u, badptr = 0x8001000Eu;
std::uint32_t error(int value = errno) {
    switch (value) {
    case ENOENT: return 0x80010002u;
    case EACCES: return 0x8001000Du;
    case EEXIST: return 0x80010011u;
    case EBADF: return badfd;
    case EINVAL: return badarg;
    case ENOSPC: return 0x8001001Cu;
    default: return 0x80010005u;
    }
}
struct PathError { std::uint32_t code; };
// Path text for a log line; never throws on names outside the ANSI code page.
std::string utf8(const std::filesystem::path &path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}
// A file or folder name as plain ASCII, lower case; empty when it is not.
std::string ascii_lower(const std::filesystem::path &name) {
    std::string out;
    for (const auto c : name.native()) {
        if (c < 32 || c > 126) return {};
        out.push_back(c >= L'A' && c <= L'Z' ? static_cast<char>(c - L'A' + L'a') : static_cast<char>(c));
    }
    return out;
}
std::string file_text(const std::filesystem::path &file) {
    std::ifstream stream(file, std::ios::binary);
    std::ostringstream content;
    content << stream.rdbuf();
    return content.str();
}
}
FileRuntime::FileRuntime(psprecomp::Runtime &rt, std::filesystem::path memory_stick)
    : runtime_(rt), disc_(std::filesystem::weakly_canonical(rt.game_root())),
      memory_stick_(std::filesystem::weakly_canonical(memory_stick)), image_(disc_iso) {
    // The memory stick is the one folder the guest writes to; it must not share a tree with the disc.
    if (vfs::inside(memory_stick_, disc_) || vfs::inside(disc_, memory_stick_))
        throw std::runtime_error("The memory stick folder and the disc folder must be separate: " + utf8(memory_stick_));
    std::filesystem::create_directories(memory_stick_);
    auto bind = [&](std::uint32_t nid, auto function) {
        rt.register_hle("IoFileMgrForUser", nid, [function](auto &runtime, auto &ctx) {
            try { function(runtime, ctx); }
            catch (const PathError &e) { ctx.set_gpr(2u, e.code); }
            catch (const std::filesystem::filesystem_error &) { ctx.set_gpr(2u, 0x80010005u); }
        });
    };
    bind(0x109F50BCu, [this](auto &, auto &ctx) { open(ctx); });
    bind(0x810C4BC3u, [this](auto &, auto &ctx) { close(ctx); });
    bind(0x6A638D83u, [this](auto &, auto &ctx) { transfer(ctx, false); });
    bind(0x42EC03ACu, [this](auto &, auto &ctx) { transfer(ctx, true); });
    bind(0x27EB27B8u, [this](auto &, auto &ctx) { seek(ctx); });
    bind(0xACE946E8u, [this](auto &, auto &ctx) { stat(ctx); });
    bind(0xB29DDF9Cu, [this](auto &, auto &ctx) { directory(ctx, 0u); });
    bind(0xE3EB004Cu, [this](auto &, auto &ctx) { directory(ctx, 1u); });
    bind(0xEB092469u, [this](auto &, auto &ctx) { directory(ctx, 2u); });
    bind(0x06A70004u, [this](auto &, auto &ctx) { mutate(ctx, 0u); });
    bind(0x1117C65Fu, [this](auto &, auto &ctx) { mutate(ctx, 1u); });
    bind(0xF27A9C51u, [this](auto &, auto &ctx) { mutate(ctx, 2u); });
    bind(0x779103A0u, [this](auto &, auto &ctx) { mutate(ctx, 3u); });
    rt.register_hle("StdioForUser", 0x172D316Eu, [](auto &, auto &ctx) { ctx.set_gpr(2u, 0u); });
    rt.register_hle("StdioForUser", 0xA6BAB2E9u, [](auto &, auto &ctx) { ctx.set_gpr(2u, 1u); });
    rt.register_hle("StdioForUser", 0xF78BA90Au, [](auto &, auto &ctx) { ctx.set_gpr(2u, 2u); });
}
FileRuntime::~FileRuntime() { for (const auto &[uid, file] : files_) if (file.host >= 0) _close(file.host); }
void FileRuntime::mount_mods(const ModSetup &setup) {
    overlays_.clear(); virtual_files_.clear(); archive_log_.clear(); menu_entries_ = {}; font_spacing_ = {};
    // Archive path -> replacement file. A later mod overwrites an earlier one's entry.
    std::map<std::string, std::filesystem::path> archive_files, root_files;
    // The lines of every mod's properties.ini; a later line is applied after an earlier one.
    std::vector<props::Override> overrides;
    std::vector<std::string> problems;
    std::error_code status;
    // The game's own settings come first, so a mod's line for the same value is applied after them.
    if (!setup.builtin_properties.empty()) overrides = props::parse(setup.builtin_properties, "built-in settings", problems);
    for (const auto &mod : setup.mods) {
        std::size_t count = 0u, lines = 0u;
        bool disc = false;
        for (const auto &top : std::filesystem::directory_iterator(mod, status)) {
            const auto name = ascii_lower(top.path().filename());
            if (top.is_regular_file(status)) {
                if (name == "properties.ini") {
                    auto parsed = props::parse(file_text(top.path()), utf8(mod.filename()) + "/properties.ini", problems);
                    lines = parsed.size();
                    std::move(parsed.begin(), parsed.end(), std::back_inserter(overrides));
                } else if (!name.empty()) root_files[name] = top.path();
                continue;
            }
            if (!top.is_directory(status)) continue;
            if (name == "disc0") { overlays_.push_back(std::filesystem::weakly_canonical(top.path())); disc = true; }
            if (!name.starts_with("data")) continue;
            for (const auto &entry : std::filesystem::recursive_directory_iterator(top.path(), status)) {
                if (!entry.is_regular_file(status)) continue;
                std::string inside = name;
                for (const auto &part : entry.path().lexically_relative(top.path())) {
                    const auto text = ascii_lower(part);
                    if (text.empty()) { inside.clear(); break; }
                    inside += '/' + text;
                }
                if (inside.empty() || !entry.file_size(status)) {
                    std::cout << "[mods] ignored (" << (inside.empty() ? "name is not plain ASCII" : "empty file") << "): " << utf8(entry.path()) << '\n';
                    continue;
                }
                archive_files[inside] = entry.path();
                ++count;
            }
        }
        std::cout << "[mods] " << utf8(mod.filename()) << ": " << count << " archive file" << (count == 1u ? "" : "s")
                  << (disc ? ", disc0 overlay" : "");
        if (lines) std::cout << ", " << lines << " property line" << (lines == 1u ? "" : "s");
        std::cout << '\n';
    }
    const auto report = [&] { for (const auto &problem : problems) std::cerr << "[mods] " << problem << '\n'; problems.clear(); };
    // The letter spacing a pack carries for the text font it replaces (font_advances.hpp): the file beside the
    // replacement sheet in force, when it reads as one and was made for that very picture. Anything else: one
    // line, and the game's own spacing.
    font_advances::FontAdvances spacing;
    std::filesystem::path spacing_file;
    double spacing_ms = 0.0;   // what the file costs the start: reading it, the sheet's hash, the block's tables
    {
        const auto started = std::chrono::steady_clock::now();
        const auto found = setup.font_advances_file();
        const auto pack = [](const std::filesystem::path &file) { return utf8(file.parent_path().parent_path().filename()); };
        for (const auto &other : found.ignored)
            std::cout << "[mods] font advances: " << pack(other) << "'s file ignored, "
                      << (found.sheet.empty() ? "no mod replaces the fill sheet" : "the fill sheet in force is " + pack(found.sheet) + "'s") << '\n';
        if (!found.file.empty()) {
            const auto refused = [&](const std::string &why) {
                std::cout << "[mods] font advances: " << utf8(found.file.lexically_relative(setup.folder)) << " ignored: " << why << '\n';
            };
            std::vector<std::uint8_t> text;
            std::string problem;
            if (!setup.replace_textures) refused("the software renderer draws the game's own font");
            else if (!font_advances::read_file(found.file, std::size_t{1u} << 20u, text)) refused("it cannot be read");
            else if (!font_advances::parse(std::string_view(reinterpret_cast<const char *>(text.data()), text.size()), spacing, problem) ||
                     !font_advances::matches_sheet(spacing, found.sheet, problem)) refused(problem);
            else spacing_file = found.file;
        }
        spacing_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    }
    // The file a mod-made one is built from: the topmost loose copy, or the disc's own. Null when the
    // disc has no such file.
    const auto loose = [&](const char *inside) -> std::shared_ptr<vfs::Source> {
        for (auto overlay = overlays_.rbegin(); overlay != overlays_.rend(); ++overlay)
            if (std::filesystem::is_regular_file(*overlay / inside, status)) return std::make_shared<vfs::HostFileSource>(*overlay / inside);
        if (image_) {
            const auto *entry = image_->file(inside);
            return entry ? image_->open(*entry) : nullptr;
        }
        if (!std::filesystem::is_regular_file(disc_ / inside, status)) return nullptr;
        return std::make_shared<vfs::HostFileSource>(disc_ / inside);
    };
    if (!setup.game_config.empty()) {
        std::string original;
        if (const auto source = loose(config_disc_path)) {
            original.resize(static_cast<std::size_t>(source->size()));
            source->read(0u, std::span(reinterpret_cast<std::uint8_t *>(original.data()), original.size()));
        }
        const auto text = apply_game_config(original, setup.game_config);
        virtual_files_[config_disc_path] = std::make_shared<vfs::MemorySource>(std::vector<std::uint8_t>(text.begin(), text.end()));
        std::cout << "[mods] configpsp.txt:";
        for (const auto &[key, value] : setup.game_config) std::cout << ' ' << key << " = " << value << ';';
        std::cout << '\n';
    }
    const bool menu_entries = setup.menu_entry_main || setup.menu_entry_pause;
    const bool always = setup.virtual_archive || setup.log_files;
    if (archive_files.empty() && root_files.empty() && overrides.empty() && !always && !menu_entries && spacing_file.empty()) { report(); return; }
    auto base = loose(archive_disc_path);
    if (!base) throw std::runtime_error("Cannot read " + utf8(disc_ / archive_disc_path) + ": the disc has no such file");
    const auto original = naz::read_directory(*base);
    std::map<std::string, const naz::Entry *> known;
    for (const auto &entry : original.entries) known[naz::normal_name(entry.name)] = &entry;
    // A file in a mod's top folder counts only when the archive has one of that name in its root.
    for (const auto &[name, file] : root_files) if (known.contains(name)) archive_files[name] = file;
    // Property lines: each block they name is patched in memory and served like a replacement file.
    std::map<std::string, std::shared_ptr<vfs::Source>> patched;
    // An archive file as the mods leave it.
    const auto bytes_of = [&](const std::string &name) {
        std::vector<std::uint8_t> bytes;
        if (const auto file = archive_files.find(name); file != archive_files.end()) {
            vfs::HostFileSource source(file->second);
            bytes.resize(static_cast<std::size_t>(source.size()));
            source.read(0u, bytes);
        } else if (const auto entry = known.find(name); entry != known.end()) {
            bytes.resize(entry->second->size);
            base->read(entry->second->data_offset(), bytes);
        }
        return bytes;
    };
    if (!overrides.empty()) {
        try {
            const auto database = props::read_database(bytes_of(props::database_path));
            std::vector<std::string> blocks;
            for (const auto &entry : overrides)
                if (std::find(blocks.begin(), blocks.end(), entry.block) == blocks.end()) blocks.push_back(entry.block);
            std::size_t shown = 0u;
            for (const auto &block : blocks) {
                // The archive file of that name; the copy every language shares when there are several.
                std::string file;
                const auto consider = [&](const std::string &name) {
                    if ((name == block || name.ends_with('/' + block)) && (file.empty() || name.find("/shared/") != std::string::npos)) file = name;
                };
                for (const auto &[name, entry] : known) consider(name);
                for (const auto &[name, path] : archive_files) consider(name);
                if (file.empty()) {
                    for (const auto &entry : overrides)
                        if (entry.block == block) problems.push_back(entry.origin + ": the archive has no file named " + block);
                    continue;
                }
                auto bytes = bytes_of(file);
                std::vector<std::string> done;
                const auto written = props::apply(bytes, block, database, overrides, done, problems);
                for (const auto &line : done) if (++shown <= 20u) std::cout << "[mods] set " << line << '\n';
                if (!written) continue;
                std::cout << "[mods] " << file << ": " << written << " value" << (written == 1u ? "" : "s") << " changed\n";
                patched[file] = std::make_shared<vfs::MemorySource>(std::move(bytes));
            }
            if (shown > 20u) std::cout << "[mods] ... and " << shown - 20u << " more property lines\n";
        } catch (const std::runtime_error &error) { problems.push_back(std::string("properties.ini lines ignored: ") + error.what()); }
    }
    report();
    // A block as the mods leave it: with property lines written into it, a mod's own file, or the archive's.
    struct Block { std::shared_ptr<vfs::Source> source; std::uint64_t offset{}, size{}; };
    const auto block_of = [&](const std::string &name) -> Block {
        if (const auto made = patched.find(name); made != patched.end()) return {made->second, 0u, made->second->size()};
        if (const auto file = archive_files.find(name); file != archive_files.end()) {
            auto source = std::make_shared<vfs::HostFileSource>(file->second);
            const auto size = source->size();
            return {std::move(source), 0u, size};
        }
        if (const auto entry = known.find(name); entry != known.end()) return {base, entry->second->data_offset(), entry->second->size};
        return {};
    };
    // The "Settings" entries of the game's own menus (menu_entry_blocks.hpp), made in the blocks as the mods
    // leave them. Only the ranges that change are kept in memory; the rest of a block is still read where it lies.
    if (menu_entries) {
        const auto started = std::chrono::steady_clock::now();
        // Every block of the game but the small per-language ones, which hold no menu.
        std::vector<std::string> names;
        const auto consider = [&](const std::string &name) {
            if (name.ends_with(".map") && !name.ends_with("_uk.map") && !name.ends_with("_fr.map") && !name.ends_with("_es.map") &&
                std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
        };
        for (const auto &[name, entry] : known) consider(name);
        for (const auto &[name, path] : archive_files) consider(name);
        std::size_t read_bytes = 0u, kept_bytes = 0u;
        std::vector<std::string> unreadable;
        try {
            const auto database = props::read_database(bytes_of(props::database_path));
            for (const auto &name : names) {
                const bool main = name == "mainmenuscene.map" || name.ends_with("/mainmenuscene.map");
                if (main ? !setup.menu_entry_main : !setup.menu_entry_pause) continue;
                const auto block = block_of(name);
                if (!block.source) continue;
                auto result = menu_entry::edit_block([&block](std::uint64_t offset, std::span<std::uint8_t> out) -> std::size_t {
                    if (offset >= block.size) return 0u;
                    return block.source->read(block.offset + offset, out.first(static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), block.size - offset))));
                }, block.size, database, main ? menu_entry::What::MainMenu : menu_entry::What::PauseLine);
                read_bytes += result.read_bytes;
                if (main) { menu_entries_.main = result.main_menu; menu_entries_.main_problem = result.problem; }
                else if (!result.problem.empty()) unreadable.push_back(name.substr(name.rfind('/') + 1u) + " (" + result.problem + ")");
                menu_entries_.story += result.story; menu_entries_.hangman += result.hangman; menu_entries_.other += result.other; menu_entries_.already += result.already;
                if (result.edits.empty()) continue;
                if (!main) ++menu_entries_.blocks;
                auto served = std::make_shared<vfs::SegmentedSource>();
                std::uint64_t at = 0u;
                for (auto &edit : result.edits) {
                    served->append(block.source, block.offset + at, edit.offset - at);
                    const auto length = edit.bytes.size();
                    kept_bytes += length;
                    served->append(std::make_shared<vfs::MemorySource>(std::move(edit.bytes)), 0u, length);
                    at = edit.offset + edit.old_size;
                }
                served->append(block.source, block.offset + at, block.size - at);
                patched[name] = std::move(served);
            }
        } catch (const std::runtime_error &error) {
            if (menu_entries_.main_problem.empty()) menu_entries_.main_problem = error.what();
            unreadable.push_back(std::string("database.bin (") + error.what() + ")");
        }
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (setup.menu_entry_main) {
            if (menu_entries_.main) std::cout << "[menu] main menu: the Fiesta line is Settings (mainmenuscene.map as it is served: the text of the line and of its two boxes, its command, its action)\n";
            else std::cout << "[menu] main menu: mainmenuscene.map keeps the game's Fiesta line (" << menu_entries_.main_problem << ")\n";
        }
        if (setup.menu_entry_pause) {
            std::cout << "[menu] pause menu: a Settings line in " << menu_entries_.blocks << " mission block" << (menu_entries_.blocks == 1u ? "" : "s") << " as served ("
                      << menu_entries_.story << " window" << (menu_entries_.story == 1u ? "" : "s") << " with four lines, " << menu_entries_.hangman << " with two); "
                      << menu_entries_.other << " pause window" << (menu_entries_.other == 1u ? "" : "s") << " of another kind keep the game's lines";
            // A mod's own mission block that was written with the line in it is served as it is: not counted above.
            if (menu_entries_.already) std::cout << "; " << menu_entries_.already << " already carrying the line";
            std::cout << '\n';
            for (std::size_t i = 0; i < unreadable.size() && i < 8u; ++i) std::cout << "[menu] left as it is: " << unreadable[i] << '\n';
            if (unreadable.size() > 8u) std::cout << "[menu] ... and " << unreadable.size() - 8u << " more blocks left as they are\n";
        }
        char line[160];
        std::snprintf(line, sizeof(line), "[menu] %.1f MB of the blocks read for it in %.0f ms, %.1f MB kept in memory", static_cast<double>(read_bytes) / 1048576.0, ms, static_cast<double>(kept_bytes) / 1048576.0);
        std::cout << line << '\n';
    }
    // The pack's letter spacing: the two glyph tables of todpsp_game.main (the copy every language shares) are
    // served from memory with the file's advances, the rest of the block from where it lies. The block keeps
    // its size. A block whose tables are not the game's own is served as it is.
    if (!spacing_file.empty()) {
        const auto started = std::chrono::steady_clock::now();
        const auto refused = [&](const std::string &why) {
            std::cout << "[mods] font advances: " << utf8(spacing_file.lexically_relative(setup.folder)) << " ignored: " << why << '\n';
        };
        std::string name;
        const auto consider = [&](const std::string &candidate) {
            if ((candidate == font_advances::block_name || candidate.ends_with(std::string("/") + font_advances::block_name)) &&
                (name.empty() || candidate.find("/shared/") != std::string::npos)) name = candidate;
        };
        for (const auto &[candidate, entry] : known) consider(candidate);
        for (const auto &[candidate, path] : archive_files) consider(candidate);
        try {
            const auto block = name.empty() ? Block{} : block_of(name);
            if (!block.source) refused(std::string("the archive has no ") + font_advances::block_name);
            else {
                auto tables = font_advances::edit_tables([&block](std::uint64_t offset, std::span<std::uint8_t> out) -> std::size_t {
                    if (offset >= block.size) return 0u;
                    return block.source->read(block.offset + offset, out.first(static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), block.size - offset))));
                }, block.size, spacing);
                if (!tables.problem.empty()) refused(std::string(font_advances::block_name) + ": " + tables.problem);
                else {
                    auto served = std::make_shared<vfs::SegmentedSource>();
                    std::uint64_t at = 0u;
                    for (auto &edit : tables.edits) {
                        served->append(block.source, block.offset + at, edit.offset - at);
                        const auto length = edit.bytes.size();
                        served->append(std::make_shared<vfs::MemorySource>(std::move(edit.bytes)), 0u, length);
                        at = edit.offset + length;
                    }
                    served->append(block.source, block.offset + at, block.size - at);
                    patched[name] = std::move(served);
                    font_spacing_ = {true, utf8(spacing_file.parent_path().parent_path().filename()), spacing.face, tables.patched, tables.unknown};
                    spacing_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                    char line[320];
                    std::snprintf(line, sizeof(line), "[mods] font advances: %s%s%s%s: %u glyphs in both font tables of %s, %u unknown; pen steps x%.2f of the game's over those letters (%.1f ms at the start)",
                        font_spacing_.pack.c_str(), spacing.face.empty() ? "" : " (", spacing.face.c_str(), spacing.face.empty() ? "" : ")", tables.patched, font_advances::block_name, tables.unknown,
                        tables.game_steps > 0.0 ? tables.face_steps / tables.game_steps : 1.0, spacing_ms);
                    std::cout << line << '\n';
                }
            }
        } catch (const std::runtime_error &error) { refused(error.what()); }
    }
    if (archive_files.empty() && patched.empty() && !always) return;
    std::vector<naz::Replacement> files;
    std::size_t shown = 0u;
    for (const auto &[name, file] : archive_files) {
        if (++shown <= 20u)
            std::cout << "[mods] " << (known.contains(name) ? "replace " : "add ") << name << " <- " << utf8(file.lexically_relative(setup.folder)) << '\n';
        // A patched block was built from this file and is served from memory instead.
        if (!patched.contains(name)) files.push_back({name, file, {}});
    }
    if (shown > 20u) std::cout << "[mods] ... and " << shown - 20u << " more\n";
    for (const auto &[name, source] : patched) files.push_back({name, {}, source});
    auto view = naz::overlay(std::move(base), files);
    std::cout << "[mods] archive: " << view.replaced << " replaced, " << view.added << " added, " << view.entries.size()
              << " files, " << view.archive->size() << " bytes\n";
    if (setup.log_files) {
        for (const auto &entry : view.entries) archive_log_.push_back({entry.data_offset(), entry.data_offset() + entry.size, entry.name, false});
        std::sort(archive_log_.begin(), archive_log_.end(), [](const auto &a, const auto &b) { return a.begin < b.begin; });
    }
    virtual_files_[archive_disc_path] = std::move(view.archive);
}
void FileRuntime::log_archive_read(std::uint64_t offset) {
    auto span = std::upper_bound(archive_log_.begin(), archive_log_.end(), offset,
        [](std::uint64_t value, const ArchiveSpan &entry) { return value < entry.begin; });
    if (span == archive_log_.begin() || offset >= (--span)->end || span->seen) return;
    span->seen = true;
    std::cout << "[mods] game reads " << span->name << '\n';
}
std::vector<std::string> FileRuntime::short_names(const std::filesystem::path &directory,
    const std::vector<std::filesystem::path> &entries) {
    std::vector<std::string> names;
    for (const auto &entry : entries) names.push_back(entry.filename().string());
    auto &previous = aliases_[directory];
    const auto result = fat_short_names(names, previous);
    previous.clear();
    for (std::size_t i = 0; i < names.size(); ++i) previous.emplace(names[i], result[i]);
    return result;
}
FileRuntime::Path FileRuntime::resolve(const std::string &input) {
    auto text = input;
    std::replace(text.begin(), text.end(), '\\', '/');
    auto colon = text.find(':');
    if (colon == std::string::npos) {
        text = text.starts_with('/') ? "disc0:" + text : "disc0:/PSP_GAME/USRDIR/" + text;
        colon = text.find(':');
    }
    auto device = text.substr(0u, colon);
    std::transform(device.begin(), device.end(), device.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool writable = device == "ms0";
    if (!writable && device != "disc0" && device != "umd0") throw PathError{0x80010013u};
    if (!writable && !disc_mounted_) throw PathError{0x80010013u};
    auto relative = text.substr(colon + 1u);
    while (!relative.empty() && relative.front() == '/') relative.erase(relative.begin());
    std::filesystem::path clean;
    for (const auto &part : std::filesystem::path(relative)) {
        if (part == ".." || part.has_root_path() || part.native().find(L':') != std::wstring::npos)
            throw PathError{0x8001000Du};
        if (part != ".") clean /= part;
    }
    const auto &root = writable ? memory_stick_ : disc_;
    auto expanded = root;
    for (const auto &part : clean) {
        auto component = part;
        if (writable && part.string().find('~') != std::string::npos &&
            std::filesystem::is_directory(expanded)) {
            std::vector<std::filesystem::path> entries;
            for (const auto &entry : std::filesystem::directory_iterator(expanded)) entries.push_back(entry.path());
            std::sort(entries.begin(), entries.end());
            const auto aliases = short_names(expanded, entries);
            for (std::size_t i = 0; i < aliases.size(); ++i)
                if (aliases[i] == fat_upper(part.string())) { component = entries[i].filename(); break; }
        }
        expanded /= component;
    }
    // A path below a disc image names nothing on the host: it is put together, not looked up.
    const bool image = !writable && image_;
    const auto host = image ? expanded.lexically_normal() : std::filesystem::weakly_canonical(expanded);
    const auto within = host.lexically_relative(root);
    if (within.empty() || *within.begin() == "..") throw PathError{0x8001000Du};
    Path result{host, writable, {}, {}, {}};
    if (writable) return result;
    result.key = naz::normal_name(clean.generic_string());
    if (const auto made = virtual_files_.find(result.key); made != virtual_files_.end()) result.source = made->second;
    for (auto overlay = overlays_.rbegin(); overlay != overlays_.rend(); ++overlay) {
        const auto layered = std::filesystem::weakly_canonical(*overlay / clean);
        const auto inside = layered.lexically_relative(*overlay);
        if (inside.empty() || *inside.begin() == "..") throw PathError{0x8001000Du};
        result.layers.push_back(layered);
    }
    if (!image) result.layers.push_back(host);
    bool found = false;
    for (const auto &layer : result.layers) {
        std::error_code status;
        if (std::filesystem::exists(layer, status)) { result.host = layer; found = true; break; }
    }
    // The image is the bottom layer: it serves what no mod folder has.
    if (image && !found) {
        result.image = image_->find(result.key);
        if (result.image && !result.image->directory && !result.source) result.source = image_->open(*result.image);
    }
    return result;
}
void FileRuntime::open(psprecomp::AllegrexContext &ctx) {
    const auto requested = runtime_.memory().read_c_string(ctx.gpr[4], 1024u);
    std::cout << "[io] open request path=" << requested << " flags=" << ctx.gpr[5] << '\n';
    const auto path = resolve(requested);
    const auto flags = ctx.gpr[5], access = flags & 3u;
    if (!access || (flags & ~0x8F0Fu) || (flags & 8u)) { ctx.set_gpr(2u, badarg); return; }
    const bool writing = (access & 2u) != 0u;
    if (!path.writable && (writing || (flags & 0xF00u))) { ctx.set_gpr(2u, 0x8001001Eu); return; }
    if (path.writable && writing) {
        // A save file that shares its bytes with another name (a hard link to the game archive, say) is not written through.
        std::error_code status;
        if (std::filesystem::is_regular_file(path.host, status) && std::filesystem::hard_link_count(path.host, status) > 1u) {
            ctx.set_gpr(2u, 0x8001000Du); return;
        }
    }
    if (path.source) {
        const auto uid = next_uid_++;
        files_.emplace(uid, File{-1, true, false, path.source, 0u, path.key == archive_disc_path});
        std::cout << "[io] open uid=" << uid << " path=" << utf8(path.host) << " ("
                  << (path.image && !virtual_files_.contains(path.key) ? "disc image, " : "mod layer, ") << path.source->size() << " bytes)\n";
        ctx.set_gpr(2u, uid);
        return;
    }
    int host_flags = _O_BINARY | (access == 1u ? _O_RDONLY : access == 2u ? _O_WRONLY : _O_RDWR);
    if (flags & 0x100u) host_flags |= _O_APPEND;
    if (flags & 0x200u) host_flags |= _O_CREAT;
    if (flags & 0x400u) host_flags |= _O_TRUNC;
    if (flags & 0x800u) host_flags |= _O_EXCL;
    const auto host = _wopen(path.host.c_str(), host_flags, _S_IREAD | _S_IWRITE);
    if (host < 0) { ctx.set_gpr(2u, error()); return; }
    const auto uid = next_uid_++;
    files_.emplace(uid, File{host, (access & 1u) != 0u, writing});
    std::cout << "[io] open uid=" << uid << " path=" << utf8(path.host) << '\n';
    ctx.set_gpr(2u, uid);
}
void FileRuntime::close(psprecomp::AllegrexContext &ctx) {
    const auto uid = ctx.gpr[4];
    if (uid < 3u && standard_open_[uid]) { standard_open_[uid] = false; ctx.set_gpr(2u, 0u); return; }
    auto found = files_.find(uid);
    if (found == files_.end()) { ctx.set_gpr(2u, badfd); return; }
    const auto result = found->second.source ? 0 : _close(found->second.host);
    files_.erase(found);
    ctx.set_gpr(2u, result == 0 ? 0u : error());
}
void FileRuntime::transfer(psprecomp::AllegrexContext &ctx, bool write) {
    const auto uid = ctx.gpr[4], address = ctx.gpr[5], count = ctx.gpr[6];
    int host = -1;
    File *made = nullptr;
    if (uid < 3u && standard_open_[uid] && (write ? uid > 0u : uid == 0u)) host = static_cast<int>(uid);
    else {
        auto found = files_.find(uid);
        if (found != files_.end() && (write ? found->second.writable : found->second.readable)) {
            host = found->second.host;
            if (found->second.source) made = &found->second;
        }
    }
    if (host < 0 && !made) { ctx.set_gpr(2u, badfd); return; }
    if (count > 0x7FFFFFFFu) { ctx.set_gpr(2u, badarg); return; }
    if (!count) { ctx.set_gpr(2u, 0u); return; }
    if (!runtime_.memory().contains(address, count)) { ctx.set_gpr(2u, badptr); return; }
    std::vector<std::uint8_t> data(count);
    // The stall log's file reads (frame_costs.hpp): the read itself and the copy into guest memory.
    CostTimer cost(frame_costs.file_ns);
    if (!write) { ++frame_costs.file_reads; frame_costs.file_bytes += count; }
    if (made) {
        if (made->archive && !archive_log_.empty()) log_archive_read(made->position);
        const auto got = made->source->read(made->position, data);
        made->position += got;
        runtime_.memory().copy_in(address, std::span<const std::uint8_t>(data.data(), got));
        ctx.set_gpr(2u, static_cast<std::uint32_t>(got));
        return;
    }
    if (write) {
        runtime_.memory().copy_out(address, data);
        if (uid == 1u) std::cout.flush();
        if (uid == 2u) std::cerr.flush();
    }
    const auto result = write ? _write(host, data.data(), count) : _read(host, data.data(), count);
    if (result < 0) { ctx.set_gpr(2u, error()); return; }
    if (!write) runtime_.memory().copy_in(address, std::span<const std::uint8_t>(data.data(), result));
    ctx.set_gpr(2u, static_cast<std::uint32_t>(result));
}
void FileRuntime::seek(psprecomp::AllegrexContext &ctx) {
    const auto found = files_.find(ctx.gpr[4]);
    std::int64_t result;
    if (found == files_.end()) result = static_cast<std::int32_t>(badfd);
    else {
        const auto offset = std::bit_cast<std::int64_t>(ctx.gpr[6] | (static_cast<std::uint64_t>(ctx.gpr[7]) << 32u));
        if (ctx.gpr[8] > 2u) result = static_cast<std::int32_t>(badarg);
        else if (auto &file = found->second; file.source) {
            const auto base = static_cast<std::int64_t>(ctx.gpr[8] == 0u ? 0u : ctx.gpr[8] == 1u ? file.position : file.source->size());
            if (offset < -base || offset > std::numeric_limits<std::int64_t>::max() - base) result = static_cast<std::int32_t>(badarg);
            else { result = base + offset; file.position = static_cast<std::uint64_t>(result); }
        }
        else { result = _lseeki64(found->second.host, offset, ctx.gpr[8]); if (result < 0) result = static_cast<std::int32_t>(error()); }
    }
    ctx.set_gpr(2u, static_cast<std::uint32_t>(result));
    ctx.set_gpr(3u, static_cast<std::uint32_t>(static_cast<std::uint64_t>(result) >> 32u));
}
std::uint32_t FileRuntime::write_stat(const std::filesystem::path &path, std::uint32_t output, bool writable, const vfs::Source *source,
    const IsoImage::Entry *image) {
    if (!output || !runtime_.memory().contains(output, 88u)) return badptr;
    struct _stat64 info{};
    if (image) {
        // An entry of the disc image: what the file of an unpacked disc would report.
        info.st_mode = static_cast<unsigned short>(image->directory ? _S_IFDIR : _S_IFREG);
        info.st_size = static_cast<__int64>(image->size);
        info.st_ctime = info.st_atime = info.st_mtime = image->time;
    } else if (_wstat64(path.c_str(), &info) != 0) return error();
    auto &memory = runtime_.memory();
    memory.zero(output, 88u);
    const bool dir = (info.st_mode & _S_IFDIR) != 0;
    memory.store32(output, (dir ? 0x1000u : 0x2000u) | (writable ? 0666u : 0444u) | (dir ? 0111u : 0u));
    memory.store32(output + 4u, dir ? 0x10u : 0x20u);
    const auto size = source ? source->size() : static_cast<std::uint64_t>(dir ? 0 : info.st_size);
    memory.store32(output + 8u, static_cast<std::uint32_t>(size));
    memory.store32(output + 12u, static_cast<std::uint32_t>(size >> 32u));
    const auto fixed = fixed_file_time;
    const __time64_t times[] = {fixed ? *fixed : info.st_ctime, fixed ? *fixed : info.st_atime, fixed ? *fixed : info.st_mtime};
    for (unsigned i = 0; i < 3u; ++i) {
        std::tm date{};
        if (_localtime64_s(&date, &times[i]) != 0) continue;
        const unsigned values[] = {static_cast<unsigned>(date.tm_year + 1900), static_cast<unsigned>(date.tm_mon + 1),
            static_cast<unsigned>(date.tm_mday), static_cast<unsigned>(date.tm_hour), static_cast<unsigned>(date.tm_min), static_cast<unsigned>(date.tm_sec)};
        for (unsigned j = 0; j < 6u; ++j) memory.store16(output + 16u + 16u*i + 2u*j, static_cast<std::uint16_t>(values[j]));
    }
    return 0u;
}
void FileRuntime::stat(psprecomp::AllegrexContext &ctx) {
    const auto path = resolve(runtime_.memory().read_c_string(ctx.gpr[4], 1024u));
    // A file made in memory has its own size; a file of the disc image reports the entry's.
    const auto *made = path.image && !virtual_files_.contains(path.key) ? nullptr : path.source.get();
    ctx.set_gpr(2u, write_stat(path.host, ctx.gpr[5], path.writable, made, path.image));
}
void FileRuntime::directory(psprecomp::AllegrexContext &ctx, unsigned operation) {
    if (!operation) {
        const auto path = resolve(runtime_.memory().read_c_string(ctx.gpr[4], 1024u));
        std::error_code ec;
        const bool image_directory = path.image && path.image->directory;
        if (!image_directory && !std::filesystem::is_directory(path.host, ec)) {
            ctx.set_gpr(2u, path.image || std::filesystem::exists(path.host, ec) ? 0x80010014u : 0x80010002u); return;
        }
        Directory directory;
        directory.writable = path.writable;
        if (path.writable) {
            for (const auto &entry : std::filesystem::directory_iterator(path.host)) directory.entries.push_back(entry.path());
            std::sort(directory.entries.begin(), directory.entries.end());
        } else {
            // Every layer's entries; a name in several layers is listed once, from the topmost.
            for (const auto &layer : path.layers) {
                if (!std::filesystem::is_directory(layer, ec)) continue;
                for (const auto &entry : std::filesystem::directory_iterator(layer)) {
                    const auto name = entry.path().filename();
                    if (std::none_of(directory.entries.begin(), directory.entries.end(), [&](const auto &listed) {
                            return _wcsicmp(listed.filename().c_str(), name.c_str()) == 0; }))
                        directory.entries.push_back(entry.path());
                }
            }
            // The disc image's own entries, where no mod folder has the name.
            std::vector<std::filesystem::path> from_image;
            if (const auto *below = image_ ? image_->find(path.key) : nullptr; below && below->directory) {
                for (const auto &child : below->children) {
                    const std::filesystem::path name(image_->find(child)->name);
                    if (std::none_of(directory.entries.begin(), directory.entries.end(), [&](const auto &listed) {
                            return _wcsicmp(listed.filename().c_str(), name.c_str()) == 0; })) {
                        directory.entries.push_back(disc_ / path.key / name);
                        from_image.push_back(directory.entries.back());
                    }
                }
            }
            std::sort(directory.entries.begin(), directory.entries.end(), [](const auto &a, const auto &b) { return a.filename() < b.filename(); });
            for (const auto &entry : directory.entries) {
                const auto key = path.key + (path.key.empty() ? "" : "/") + ascii_lower(entry.filename());
                const auto made = virtual_files_.find(key);
                directory.sources.push_back(made == virtual_files_.end() ? nullptr : made->second);
                directory.images.push_back(std::find(from_image.begin(), from_image.end(), entry) == from_image.end() ? nullptr : image_->find(key));
            }
        }
        directory.short_names = short_names(path.host, directory.entries);
        const auto uid = next_uid_++;
        directories_.emplace(uid, std::move(directory)); ctx.set_gpr(2u, uid); return;
    }
    const auto found = directories_.find(ctx.gpr[4]);
    if (found == directories_.end()) { ctx.set_gpr(2u, badfd); return; }
    if (operation == 2u) { directories_.erase(found); ctx.set_gpr(2u, 0u); return; }
    auto &dir = found->second;
    if (dir.index == dir.entries.size()) { ctx.set_gpr(2u, 0u); return; }
    const auto output = ctx.gpr[5];
    if (!output || !runtime_.memory().contains(output, 352u)) { ctx.set_gpr(2u, badptr); return; }
    const auto &entry = dir.entries[dir.index];
    const auto filename = entry.filename().string();
    if (filename.size() > 255u) { ctx.set_gpr(2u, badarg); return; }
    const auto private_address = runtime_.memory().load32(output + 344u);
    bool write_private = false;
    std::uint32_t short_offset{}, long_offset{};
    if (private_address && dir.writable) {
        const bool modern = compiled_sdk_ > 0x0307FFFFu;
        short_offset = modern ? 4u : 0u;
        long_offset = modern ? 20u : 13u;
        if (modern && !runtime_.memory().contains(private_address, 4u)) { ctx.set_gpr(2u, badptr); return; }
        write_private = !modern || runtime_.memory().load32(private_address) >= 1044u;
        if (write_private && (!runtime_.memory().contains(private_address + short_offset, dir.short_names[dir.index].size() + 1u) ||
            !runtime_.memory().contains(private_address + long_offset, filename.size() + 1u))) {
            ctx.set_gpr(2u, badptr); return;
        }
    }
    const auto result = write_stat(entry, output, dir.writable, dir.index < dir.sources.size() ? dir.sources[dir.index].get() : nullptr,
        dir.index < dir.images.size() ? dir.images[dir.index] : nullptr);
    if (result) { ctx.set_gpr(2u, result); return; }
    runtime_.memory().zero(output + 88u, 256u);
    for (std::size_t i = 0; i < filename.size(); ++i) runtime_.memory().store8(output + 88u + static_cast<std::uint32_t>(i), filename[i]);
    runtime_.memory().store32(output + 348u, 0u);
    if (write_private) {
        auto store_string = [&](std::uint32_t address, const std::string &value) {
            for (std::size_t i = 0; i <= value.size(); ++i)
                runtime_.memory().store8(address + static_cast<std::uint32_t>(i), i == value.size() ? 0u : value[i]);
        };
        store_string(private_address + short_offset, dir.short_names[dir.index]);
        store_string(private_address + long_offset, filename);
    }
    ++dir.index; ctx.set_gpr(2u, 1u);
}
void FileRuntime::mutate(psprecomp::AllegrexContext &ctx, unsigned operation) {
    const auto path = resolve(runtime_.memory().read_c_string(ctx.gpr[4], 1024u));
    if (!path.writable) { ctx.set_gpr(2u, 0x8001001Eu); return; }
    int result = -1;
    if (!operation) result = _wmkdir(path.host.c_str());
    else if (operation == 1u) result = _wrmdir(path.host.c_str());
    else if (operation == 2u) result = _wunlink(path.host.c_str());
    else {
        const auto target = resolve(runtime_.memory().read_c_string(ctx.gpr[5], 1024u));
        if (!target.writable) { ctx.set_gpr(2u, 0x8001001Eu); return; }
        result = _wrename(path.host.c_str(), target.host.c_str());
    }
    ctx.set_gpr(2u, result == 0 ? 0u : error());
}
}
