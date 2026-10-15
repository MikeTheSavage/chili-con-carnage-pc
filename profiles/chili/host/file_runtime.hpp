#pragma once
#include "psprecomp/runtime.hpp"
#include "mod_files.hpp"
#include "block_props.hpp"
#include "menu_entry_blocks.hpp"
#include "disc_image.hpp"
#include <map>
#include <vector>
namespace chili {
// Guest file IO. ms0 is a writable host folder, kept apart from the disc folder
// (a file there with a second hard link is not opened for writing either).
// disc0 and umd0 are read-only: every host file below them is opened read-only,
// and no guest call can create, change, rename or remove one. They are
// layered: the disc folder at the bottom, then the disc0 folder of each
// mod, then the files made in memory (the archive with mod files spliced in
// and property lines written into its blocks, configpsp.txt with settings
// written over it). The topmost layer that has a file serves it. The bottom
// layer is a host folder, or the .iso itself when disc_iso is set (disc_image.hpp).
class FileRuntime {
public:
    FileRuntime(psprecomp::Runtime &, std::filesystem::path memory_stick);
    ~FileRuntime();
    void set_compiled_sdk(std::uint32_t version) { compiled_sdk_ = version; }
    void set_disc_mounted(bool mounted) { disc_mounted_ = mounted; }
    // Mounts the enabled mods over the disc and prints what was mounted.
    void mount_mods(const ModSetup &);
    // What the mount made of the "Settings" entries the setup asked for (menu_entry_blocks.hpp).
    struct MenuEntries {
        bool main{};                          // the main menu's Fiesta line is the entry
        unsigned blocks{};                    // mission blocks served with a pause line
        unsigned story{}, hangman{}, other{}; // pause windows: given the line (four lines, two lines), left as they are
        unsigned already{};                   // pause windows that carry the line as they come (a mod's own block)
        std::string main_problem;             // why the main menu's block was left alone
    };
    const MenuEntries &menu_entries() const noexcept { return menu_entries_; }
    // What the mount made of a pack's letter spacing file (font_advances.hpp).
    struct FontSpacing {
        bool applied{};              // todpsp_game.main is served with the file's advances
        std::string pack, face;      // the mod folder it came from and the typeface it names
        unsigned glyphs{}, unknown{};
    };
    const FontSpacing &font_spacing() const noexcept { return font_spacing_; }
private:
    struct Path {
        std::filesystem::path host; bool writable;
        std::shared_ptr<vfs::Source> source;        // set for a file made in memory
        std::vector<std::filesystem::path> layers;  // read-only: the same path in every layer, topmost first
        std::string key;                            // read-only: lower-case path inside the disc
        const IsoImage::Entry *image{};             // disc image: its entry at this path, when no mod folder has the path
    };
    struct File { int host; bool readable, writable; std::shared_ptr<vfs::Source> source; std::uint64_t position{}; bool archive{}; };
    struct Directory {
        std::vector<std::filesystem::path> entries; std::vector<std::string> short_names; std::size_t index{}; bool writable{};
        std::vector<std::shared_ptr<vfs::Source>> sources;  // per entry, set where the guest sees a file made in memory
        std::vector<const IsoImage::Entry *> images;        // per entry, set where the entry is in the disc image
    };
    Path resolve(const std::string &);
    void log_archive_read(std::uint64_t offset);
    std::vector<std::string> short_names(const std::filesystem::path &, const std::vector<std::filesystem::path> &);
    void open(psprecomp::AllegrexContext &);
    void close(psprecomp::AllegrexContext &);
    void transfer(psprecomp::AllegrexContext &, bool write);
    void seek(psprecomp::AllegrexContext &);
    void stat(psprecomp::AllegrexContext &);
    void directory(psprecomp::AllegrexContext &, unsigned operation);
    void mutate(psprecomp::AllegrexContext &, unsigned operation);
    std::uint32_t write_stat(const std::filesystem::path &, std::uint32_t output, bool writable, const vfs::Source *source = nullptr,
        const IsoImage::Entry *image = nullptr);
    psprecomp::Runtime &runtime_;
    std::filesystem::path disc_, memory_stick_;
    std::shared_ptr<IsoImage> image_;               // the disc when it is an .iso
    std::vector<std::filesystem::path> overlays_;   // mod disc0 folders, lowest priority first
    std::map<std::string, std::shared_ptr<vfs::Source>> virtual_files_;
    MenuEntries menu_entries_;
    FontSpacing font_spacing_;
    struct ArchiveSpan { std::uint64_t begin, end; std::string name; bool seen; };
    std::vector<ArchiveSpan> archive_log_;          // sorted by begin; empty unless [Mods] LogFiles is on
    std::map<std::uint32_t, File> files_;
    std::map<std::uint32_t, Directory> directories_;
    std::map<std::filesystem::path, std::map<std::string, std::string>> aliases_;
    std::uint32_t next_uid_{0x30000u};
    std::uint32_t compiled_sdk_{};
    bool standard_open_[3]{true, true, true};
    bool disc_mounted_{true};
};
}
