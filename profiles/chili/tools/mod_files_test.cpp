// Mod file layer: NAZ directory reading and regeneration, the spliced archive
// view, the configpsp.txt overlay, mod selection from ChiliNative.ini,
// properties.ini lines written into blocks, the disc folder staying read-only,
// and all of it seen through the guest's own sceIo calls. With the game's disc folder
// as second argument it also lists the real archive through the mod layer and
// compares files against direct reads.
#include "../host/file_runtime.hpp"
#include "../host/font_advances.hpp"
#include "../host/render_settings.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
namespace {
using chili::naz::Entry;
constexpr auto text = 0x09000000u, status = text + 2048u, buffer = 0x09100000u;
constexpr std::uint32_t buffer_size = 1u << 20u;
unsigned checks = 0u;
void require(bool value, const std::string &message) { ++checks; if (!value) throw std::runtime_error(message); }

void string(psprecomp::Runtime &rt, const std::string &value, std::uint32_t address = text) {
    for (std::size_t i = 0; i <= value.size(); ++i)
        rt.memory().store8(address + static_cast<std::uint32_t>(i), i == value.size() ? 0u : value[i]);
}
std::uint32_t call(psprecomp::Runtime &rt, std::uint32_t nid, std::uint32_t a0, std::uint32_t a1 = 0u, std::uint32_t a2 = 0u) {
    auto &ctx = rt.cpu(); ctx.gpr[4] = a0; ctx.gpr[5] = a1; ctx.gpr[6] = a2;
    rt.invoke_import("IoFileMgrForUser", nid, ctx); return ctx.gpr[2];
}
std::uint32_t open(psprecomp::Runtime &rt, const std::string &path, std::uint32_t flags = 1u) {
    string(rt, path); return call(rt, 0x109F50BCu, text, flags, 0777u);
}
void close(psprecomp::Runtime &rt, std::uint32_t uid) { require(call(rt, 0x810C4BC3u, uid) == 0u, "close failed"); }
std::uint64_t seek(psprecomp::Runtime &rt, std::uint32_t uid, std::int64_t offset, unsigned whence) {
    auto &ctx = rt.cpu(); ctx.gpr[4] = uid; ctx.gpr[5] = 0u;
    ctx.gpr[6] = static_cast<std::uint32_t>(offset); ctx.gpr[7] = static_cast<std::uint32_t>(static_cast<std::uint64_t>(offset) >> 32u);
    ctx.gpr[8] = whence; rt.invoke_import("IoFileMgrForUser", 0x27EB27B8u, ctx);
    return ctx.gpr[2] | (static_cast<std::uint64_t>(ctx.gpr[3]) << 32u);
}
std::uint64_t stat_size(psprecomp::Runtime &rt, const std::string &path) {
    string(rt, path);
    require(call(rt, 0xACE946E8u, text, status) == 0u, "stat failed: " + path);
    return rt.memory().load32(status + 8u) | (static_cast<std::uint64_t>(rt.memory().load32(status + 12u)) << 32u);
}
// A guest file handle as a byte source: seek and read exactly as the game does.
class GuestFile final : public chili::vfs::Source {
public:
    GuestFile(psprecomp::Runtime &rt, const std::string &path) : rt_(rt), uid_(open(rt, path)) {
        require(static_cast<std::int32_t>(uid_) > 2, "guest open failed: " + path);
        size_ = seek(rt_, uid_, 0, 2u);
    }
    ~GuestFile() override { call(rt_, 0x810C4BC3u, uid_); }
    std::uint64_t size() const override { return size_; }
    std::size_t read(std::uint64_t offset, std::span<std::uint8_t> out) override {
        if (seek(rt_, uid_, static_cast<std::int64_t>(offset), 0u) != offset) throw std::runtime_error("guest seek failed");
        std::size_t done = 0u;
        while (done < out.size()) {
            const auto step = static_cast<std::uint32_t>(std::min<std::size_t>(out.size() - done, buffer_size));
            const auto got = call(rt_, 0x6A638D83u, uid_, buffer, step);
            if (got > step) throw std::runtime_error("guest read failed");
            rt_.memory().copy_out(buffer, out.subspan(done, got));
            done += got;
            if (got < step) break;
        }
        return done;
    }
private:
    psprecomp::Runtime &rt_;
    std::uint32_t uid_;
    std::uint64_t size_{};
};
std::vector<std::uint8_t> read_all(chili::vfs::Source &source, std::uint64_t offset, std::size_t count) {
    std::vector<std::uint8_t> out(count);
    out.resize(source.read(offset, out));
    return out;
}
std::vector<std::uint8_t> pattern(std::size_t size, unsigned seed) {
    std::vector<std::uint8_t> out(size);
    std::mt19937 random(seed);
    for (auto &byte : out) byte = static_cast<std::uint8_t>(random());
    return out;
}
void write_file(const std::filesystem::path &file, std::span<const std::uint8_t> bytes) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream) throw std::runtime_error("cannot write " + file.string());
}
void write_text(const std::filesystem::path &file, const std::string &content) {
    write_file(file, std::span(reinterpret_cast<const std::uint8_t *>(content.data()), content.size()));
}
std::vector<std::uint8_t> file_bytes(const std::filesystem::path &file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
// Builds a small archive in the game's layout and returns it with its entries.
std::vector<std::uint8_t> make_archive(const std::vector<std::pair<std::string, std::vector<std::uint8_t>>> &files, std::vector<Entry> &entries) {
    std::vector<std::uint8_t> out;
    for (const auto &[name, data] : files) {
        Entry entry;
        entry.name = name; entry.header_offset = static_cast<std::uint32_t>(out.size()); entry.size = static_cast<std::uint32_t>(data.size());
        entry.time = static_cast<std::uint16_t>(0x6000u + entries.size()); entry.date = 0x362Cu;
        out.insert(out.end(), chili::naz::local_header_size + name.size() + 1u, std::uint8_t{0xEEu});
        out.insert(out.end(), data.begin(), data.end());
        out.insert(out.end(), chili::naz::trailer_size, std::uint8_t{0xDDu});
        entries.push_back(entry);
    }
    const auto records = chili::naz::directory_bytes(entries, out.size());
    out.insert(out.end(), records.begin(), records.end());
    return out;
}

void archive_format(const std::filesystem::path &root) {
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> files{
        {"facecoll.mat", pattern(224u, 1u)}, {"data_psp/textures/a.texture_psp", pattern(5000u, 2u)},
        {"data_psp/maps/big.map", pattern(300000u, 3u)}, {"data/engine/icon0.png", pattern(777u, 4u)}};
    std::vector<Entry> entries;
    const auto bytes = make_archive(files, entries);
    write_file(root / "plain.naz", bytes);
    auto base = std::make_shared<chili::vfs::HostFileSource>(root / "plain.naz");
    const auto directory = chili::naz::read_directory(*base);
    require(directory.entries.size() == 4u && directory.offset + directory.size + chili::naz::end_size == bytes.size(), "directory extent");
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto &entry = directory.entries[i];
        require(entry.name == files[i].first && entry.size == files[i].second.size() && entry.time == 0x6000u + i && entry.date == 0x362Cu, "directory entry fields");
        require(read_all(*base, entry.data_offset(), entry.size) == files[i].second, "entry data offset");
    }
    // The stored name is the plain name with every byte rotated right by two bits.
    require(bytes[static_cast<std::size_t>(directory.offset) + chili::naz::record_size] == static_cast<std::uint8_t>(('f' >> 2) | ('f' << 6)), "name rotation");
    require(chili::naz::directory_bytes(directory.entries, directory.offset) ==
        std::vector<std::uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(directory.offset), bytes.end()), "regenerated directory differs");
    require(chili::naz::normal_name("\\Data_PSP//Maps\\Big.MAP/") == "data_psp/maps/big.map", "name normalisation");

    // No replacement: the view is the file, byte for byte.
    const auto same = chili::naz::overlay(base, {});
    require(same.archive->size() == bytes.size() && read_all(*same.archive, 0u, bytes.size()) == bytes && !same.replaced && !same.added, "empty overlay changed the archive");

    // One file grows, one shrinks, one is new; the rest keep their offsets.
    const auto bigger = pattern(9000u, 10u), smaller = pattern(100u, 11u), fresh = pattern(4321u, 12u);
    write_file(root / "mod" / "a.bin", bigger); write_file(root / "mod" / "b.bin", smaller); write_file(root / "mod" / "c.bin", fresh);
    const auto view = chili::naz::overlay(base, {{"DATA_PSP\\Textures\\A.texture_psp", root / "mod" / "a.bin"},
        {"data_psp/maps/big.map", root / "mod" / "b.bin"}, {"data_psp/new/thing.bin", root / "mod" / "c.bin"}});
    require(view.replaced == 2u && view.added == 1u && view.entries.size() == 5u, "overlay counts");
    const auto listed = chili::naz::read_directory(*view.archive);
    require(listed.entries.size() == 5u, "overlay directory size");
    const std::vector<std::pair<std::string, const std::vector<std::uint8_t> *>> expected{
        {"facecoll.mat", &files[0].second}, {"data_psp/textures/a.texture_psp", &bigger}, {"data_psp/maps/big.map", &smaller},
        {"data/engine/icon0.png", &files[3].second}, {"data_psp/new/thing.bin", &fresh}};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto &entry = listed.entries[i];
        require(entry.name == expected[i].first && entry.size == expected[i].second->size(), "overlay entry " + expected[i].first);
        require(read_all(*view.archive, entry.data_offset(), entry.size) == *expected[i].second, "overlay data " + expected[i].first);
        require(entry.data_offset() + entry.size + chili::naz::trailer_size <= listed.offset, "overlay entry runs into the directory");
    }
    require(listed.entries[0].header_offset == entries[0].header_offset && listed.entries[3].header_offset == entries[3].header_offset,
        "an untouched entry moved");
    require(listed.entries[1].time == entries[1].time && listed.entries[1].date == entries[1].date && listed.entries[4].date == 0x21u, "overlay time stamps");
    require(read_all(*view.archive, 0u, static_cast<std::size_t>(directory.offset)) ==
        std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(directory.offset)), "original data region changed");
    // Reads of every size across every seam match one flat copy of the view.
    const auto flat = read_all(*view.archive, 0u, static_cast<std::size_t>(view.archive->size()));
    require(flat.size() == view.archive->size(), "flat read size");
    std::mt19937 random(99u);
    for (unsigned i = 0; i < 4000u; ++i) {
        const std::size_t offset = random() % (flat.size() + 64u), length = random() % 20000u;
        const auto got = read_all(*view.archive, offset, length);
        const auto available = offset < flat.size() ? std::min<std::size_t>(length, flat.size() - offset) : 0u;
        require(got.size() == available && std::equal(got.begin(), got.end(), flat.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(offset, flat.size()))),
            "random read mismatch");
    }
    // A broken archive is refused, not served.
    auto broken = bytes;
    broken[broken.size() - 6u] ^= 0x40u;
    write_file(root / "broken.naz", broken);
    bool refused = false;
    try { chili::vfs::HostFileSource source(root / "broken.naz"); (void)chili::naz::read_directory(source); }
    catch (const std::runtime_error &) { refused = true; }
    require(refused, "broken end record accepted");
}

void game_config() {
    const std::string stock = "scenefile = /data/x.scene\r\nfilesystem = \"assets00.naz\"\r\nshowdefaultlogs = true\r\n";
    require(chili::apply_game_config(stock, {{"frame_lock", "1"}}) == stock + "frame_lock = 1\r\n", "new key is not appended");
    require(chili::apply_game_config(stock, {{"FileSystem", "\"mod.naz, assets00.naz\""}}) ==
        "scenefile = /data/x.scene\r\nfilesystem = \"mod.naz, assets00.naz\"\r\nshowdefaultlogs = true\r\n", "existing key is not replaced in place");
    require(chili::apply_game_config("a = 1\nb = 2", {{"b", "3"}, {"c", "4"}}) == "a = 1\nb = 3\nc = 4\n", "LF text or missing last newline");
    require(chili::apply_game_config("", {{"frame_lock", "2"}}) == "frame_lock = 2\r\n", "empty text");
    require(chili::apply_game_config(stock, {}) == stock, "no settings changed the text");
}

void ini_and_selection(const std::filesystem::path &root) {
    const auto mods = root / "mods";
    for (const char *name : {"zeta", "Alpha", "beta", ".git"}) std::filesystem::create_directories(mods / name);
    write_text(mods / "note.txt", "not a mod");
    write_text(root / "all.ini", "[Input]\nScheme=psp\n[Game]\nFrameLock = 1\n[Game.Config]\nshow_hud = false ; comment\nframe_lock = 2\n");
    auto setup = chili::ModSetup::load(root / "all.ini");
    require(setup.problems.empty() && setup.mods.size() == 3u && setup.mods[0].filename() == "Alpha" && setup.mods[1].filename() == "beta" &&
        setup.mods[2].filename() == "zeta", "default selection is not every folder in alphabetical order");
    require(setup.game_config == std::vector<std::pair<std::string, std::string>>{{"show_hud", "false"}, {"frame_lock", "2"}}, "game config order or override");
    write_text(root / "listed.ini", "[Mods]\nEnabled = zeta, missing, alpha, ..\\x\nLogFiles = true\nBogus = 1\n[Game]\nFrameLock = 3\n");
    setup = chili::ModSetup::load(root / "listed.ini");
    require(setup.mods.size() == 2u && setup.mods[0].filename() == "zeta" && setup.mods[1].filename() == "alpha" && setup.log_files, "Enabled order");
    require(setup.problems.size() == 4u && setup.game_config.empty(), "problems for a missing mod, an unsafe name, an unknown key and a bad FrameLock");
    write_text(root / "none.ini", "[Mods]\nEnabled =\n");
    require(chili::ModSetup::load(root / "none.ini").mods.empty(), "empty Enabled list still loads mods");
    std::filesystem::create_directories(root / "elsewhere" / "only");
    write_text(root / "folder.ini", "[Mods]\nFolder = elsewhere\n");
    setup = chili::ModSetup::load(root / "folder.ini");
    require(setup.mods.size() == 1u && setup.mods[0].filename() == "only" && setup.problems.empty(), "[Mods] Folder");
    setup = chili::ModSetup::load(root / "does-not-exist" / "ChiliNative.ini");
    require(setup.mods.empty() && setup.problems.empty() && setup.game_config.empty(), "missing ini and mods folder");
    // The template added to a fresh ini is all comments.
    chili::ModSetup blank;
    std::string folder; std::vector<std::string> enabled; bool listed = false;
    blank.apply(chili::default_mods_ini(), folder, enabled, listed);
    require(blank.problems.empty() && blank.game_config.empty() && folder.empty() && !listed && blank.settings_in_game, "default ini text is not inert");
    // [Game] SettingsInGame: the switch for the Settings lines and the page; on unless the file says false.
    {
        chili::ModSetup off, on, bad;
        off.apply("[Game]\nSettingsInGame = false\n", folder, enabled, listed);
        on.apply("[Game]\nsettingsingame=1\n", folder, enabled, listed);
        bad.apply("[Game]\nSettingsInGame=maybe\n", folder, enabled, listed);
        require(!off.settings_in_game && off.problems.empty() && on.settings_in_game && on.problems.empty(), "SettingsInGame is not read");
        require(bad.settings_in_game && bad.problems.size() == 1u, "a wrong SettingsInGame is not a problem line with the default kept");
        require(std::string_view(chili::default_mods_ini()).find("; SettingsInGame=true\n") != std::string_view::npos, "the default ini text does not name SettingsInGame");
    }
    // [Mods] PictureStoreMB: the memory for the pictures of texture packs; auto unless the file names a number.
    {
        chili::ModSetup absent, automatic, number, low, high, word, fraction;
        absent.apply("[Mods]\nDumpTextures=false\n", folder, enabled, listed);
        automatic.apply("[Mods]\nPictureStoreMB = Auto\n", folder, enabled, listed);
        number.apply("[Mods]\npicturestoremb=512\n", folder, enabled, listed);
        low.apply("[Mods]\nPictureStoreMB=63\n", folder, enabled, listed);
        high.apply("[Mods]\nPictureStoreMB=8193\n", folder, enabled, listed);
        word.apply("[Mods]\nPictureStoreMB=abc\n", folder, enabled, listed);
        fraction.apply("[Mods]\nPictureStoreMB=512.5\n", folder, enabled, listed);
        require(absent.picture_store_mb == 0u && automatic.picture_store_mb == 0u && automatic.problems.empty() && number.picture_store_mb == 512u && number.problems.empty(), "PictureStoreMB is not read");
        for (const auto *bad : {&low, &high, &word, &fraction})
            require(bad->picture_store_mb == 0u && bad->problems.size() == 1u && bad->problems.front().find("PictureStoreMB must be auto or a number of MB") != std::string::npos,
                "a wrong PictureStoreMB is not a problem line with auto kept");
        require(std::string_view(chili::default_mods_ini()).find("; PictureStoreMB=auto\n") != std::string_view::npos, "the default ini text does not name PictureStoreMB");
        constexpr std::uint64_t mib = 1024ull * 1024ull, gib = 1024ull * mib;
        // auto: a sixteenth of the memory, 256 MiB on a small PC (the store's size before the key), 2048 at most.
        require(chili::picture_store_bytes(0u, 4u * gib) == 256u * mib && chili::picture_store_bytes(0u, 2u * gib) == 256u * mib && chili::picture_store_bytes(0u, 0u) == 256u * mib &&
                chili::picture_store_bytes(0u, 8u * gib) == 512u * mib && chili::picture_store_bytes(0u, 16u * gib) == 1024u * mib && chili::picture_store_bytes(0u, 28u * gib) == 1792u * mib &&
                chili::picture_store_bytes(0u, 32u * gib) == 2048u * mib && chili::picture_store_bytes(0u, 128u * gib) == 2048u * mib, "the automatic size of the picture store");
        require(chili::picture_store_bytes(64u, 128u * gib) == 64u * mib && chili::picture_store_bytes(8192u, 4u * gib) == 8192u * mib, "a number in the file is taken as written");
    }
    write_text(root / "fresh.ini", "[Input]\nScheme=psp\n");
    chili::add_default_mod_settings(root / "fresh.ini");
    const auto once = file_bytes(root / "fresh.ini");
    chili::add_default_mod_settings(root / "fresh.ini");
    require(once.size() > 30u && file_bytes(root / "fresh.ini") == once, "template is added once");
}

void guest_view(const std::filesystem::path &root) {
    const auto disc = root / "disc0", usrdir = disc / "PSP_GAME" / "USRDIR", mods = root / "mods";
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> files{
        {"facecoll.mat", pattern(224u, 21u)}, {"data_psp/textures/boot.texture_psp", pattern(70000u, 22u)}, {"data_psp/maps/m1.map", pattern(150000u, 23u)}};
    std::vector<Entry> entries;
    const auto archive = make_archive(files, entries);
    write_file(usrdir / "assets00.naz", archive);
    const std::string stock = "filesystem = \"assets00.naz\"\r\nloadblocks = true\r\n";
    write_text(usrdir / "configpsp.txt", stock);
    write_text(usrdir / "plain.txt", "disc");
    write_text(usrdir / "shadowed.txt", "disc");
    // low: lowest priority. high: listed last, so it wins.
    const auto low_boot = pattern(80000u, 31u), high_boot = pattern(60000u, 32u), new_file = pattern(1234u, 33u), mat = pattern(300u, 34u);
    write_file(mods / "low" / "data_psp" / "textures" / "boot.texture_psp", low_boot);
    write_file(mods / "low" / "data_psp" / "extra" / "new.bin", new_file);
    write_text(mods / "low" / "disc0" / "PSP_GAME" / "USRDIR" / "shadowed.txt", "low");
    write_text(mods / "low" / "disc0" / "PSP_GAME" / "USRDIR" / "low_only.txt", "low only");
    write_file(mods / "high" / "DATA_PSP" / "Textures" / "Boot.texture_psp", high_boot);
    write_file(mods / "high" / "facecoll.mat", mat);
    write_text(mods / "high" / "readme.txt", "not in the archive, so not added");
    write_text(mods / "high" / "disc0" / "PSP_GAME" / "USRDIR" / "shadowed.txt", "high");
    write_text(mods / "high" / "data_psp" / "empty.bin", "");
    write_text(root / "ChiliNative.ini", "[Mods]\nEnabled = low, high\n[Game]\nFrameLock = 1\n");

    psprecomp::Runtime rt; rt.set_game_root(disc); chili::FileRuntime runtime(rt, root / "ms0");
    auto read_text = [&](const std::string &path) {
        GuestFile file(rt, path);
        const auto bytes = read_all(file, 0u, static_cast<std::size_t>(file.size()));
        return std::string(bytes.begin(), bytes.end());
    };
    // Before any mod is mounted the disc is served as it is.
    require(read_text("disc0:/PSP_GAME/USRDIR/shadowed.txt") == "disc" && read_text("disc0:/PSP_GAME/USRDIR/configpsp.txt") == stock &&
        stat_size(rt, "disc0:/PSP_GAME/USRDIR/assets00.naz") == archive.size(), "unmodded disc view");
    runtime.mount_mods(chili::ModSetup::load(root / "ChiliNative.ini"));

    require(read_text("disc0:/PSP_GAME/USRDIR/shadowed.txt") == "high" && read_text("umd0:/PSP_GAME/USRDIR/shadowed.txt") == "high", "the last listed mod does not win");
    require(read_text("disc0:/PSP_GAME/USRDIR/low_only.txt") == "low only" && read_text("plain.txt") == "disc", "loose overlay lookup");
    require(stat_size(rt, "disc0:/PSP_GAME/USRDIR/shadowed.txt") == 4u, "stat of a shadowed file");
    require(open(rt, "disc0:/PSP_GAME/USRDIR/shadowed.txt", 2u) == 0x8001001Eu && open(rt, "disc0:/PSP_GAME/USRDIR/low_only.txt", 0x602u) == 0x8001001Eu &&
        open(rt, "disc0:/PSP_GAME/USRDIR/assets00.naz", 3u) == 0x8001001Eu, "a mod layer file opened for writing");
    require(open(rt, "disc0:/PSP_GAME/USRDIR/../../../high/readme.txt") == 0x8001000Du && open(rt, "disc0:/PSP_GAME/USRDIR/missing.txt") == 0x80010002u, "sandbox or missing file");
    string(rt, "disc0:/PSP_GAME/USRDIR/low_only.txt"); require(call(rt, 0xF27A9C51u, text) == 0x8001001Eu, "a mod layer file was removable");

    require(read_text("disc0:/PSP_GAME/USRDIR/configpsp.txt") == stock + "frame_lock = 1\r\n", "configpsp.txt overlay");
    require(stat_size(rt, "disc0:/PSP_GAME/USRDIR/configpsp.txt") == stock.size() + 16u, "stat of the made configpsp.txt");

    GuestFile view(rt, "disc0:/PSP_GAME/USRDIR/assets00.naz");
    require(stat_size(rt, "disc0:/PSP_GAME/USRDIR/assets00.naz") == view.size() && view.size() > archive.size(), "stat of the archive view");
    const auto listed = chili::naz::read_directory(view);
    require(listed.entries.size() == 4u, "archive view entry count");
    const std::vector<std::pair<std::string, const std::vector<std::uint8_t> *>> expected{
        {"facecoll.mat", &mat}, {"data_psp/textures/boot.texture_psp", &high_boot}, {"data_psp/maps/m1.map", &files[2].second}, {"data_psp/extra/new.bin", &new_file}};
    for (std::size_t i = 0; i < expected.size(); ++i)
        require(listed.entries[i].name == expected[i].first && read_all(view, listed.entries[i].data_offset(), listed.entries[i].size) == *expected[i].second,
            "archive view entry " + expected[i].first);
    require(listed.entries[2].header_offset == entries[2].header_offset, "an untouched entry moved in the guest view");
    // Seek semantics on a made file: relative, from the end, past the end, negative.
    {
        const auto uid = open(rt, "disc0:/PSP_GAME/USRDIR/configpsp.txt");
        require(seek(rt, uid, -5, 2u) == stock.size() + 11u && seek(rt, uid, 2, 1u) == stock.size() + 13u, "seek from end or current");
        require(call(rt, 0x6A638D83u, uid, buffer, 64u) == 3u && rt.memory().load8(buffer) == '1', "read after seek");
        require(call(rt, 0x6A638D83u, uid, buffer, 64u) == 0u && seek(rt, uid, 100000, 0u) == 100000u && call(rt, 0x6A638D83u, uid, buffer, 8u) == 0u, "read at or past the end");
        require(static_cast<std::uint32_t>(seek(rt, uid, -1, 0u)) == 0x80010016u && seek(rt, uid, 0, 1u) == 100000u, "negative seek moved the position");
        require(call(rt, 0x42EC03ACu, uid, buffer, 4u) == 0x80010009u && call(rt, 0x6A638D83u, uid, 0u, 4u) == 0x8001000Eu, "write or bad pointer on a made file");
        close(rt, uid);
        require(call(rt, 0x810C4BC3u, uid) == 0x80010009u, "double close of a made file");
    }
    // The folder listing is the union of the layers, with the made files' sizes.
    string(rt, "disc0:/PSP_GAME/USRDIR"); const auto dir = call(rt, 0xB29DDF9Cu, text);
    require(static_cast<std::int32_t>(dir) > 2, "directory open");
    std::map<std::string, std::uint64_t> names;
    for (;;) {
        rt.memory().zero(status, 352u);
        const auto more = call(rt, 0xE3EB004Cu, dir, status);
        require(more <= 1u, "directory read");
        if (!more) break;
        names[rt.memory().read_c_string(status + 88u)] = rt.memory().load32(status + 8u);
    }
    require(call(rt, 0xEB092469u, dir) == 0u, "directory close");
    require(names.size() == 5u && names["assets00.naz"] == view.size() && names["configpsp.txt"] == stock.size() + 16u && names["shadowed.txt"] == 4u &&
        names["low_only.txt"] == 8u && names["plain.txt"] == 4u, "directory union");

    // Remounting with nothing enabled restores the plain disc.
    write_text(root / "ChiliNative.ini", "[Mods]\nEnabled =\n");
    runtime.mount_mods(chili::ModSetup::load(root / "ChiliNative.ini"));
    require(read_text("disc0:/PSP_GAME/USRDIR/shadowed.txt") == "disc" && read_text("disc0:/PSP_GAME/USRDIR/configpsp.txt") == stock &&
        stat_size(rt, "disc0:/PSP_GAME/USRDIR/assets00.naz") == archive.size(), "unmount");
    require(file_bytes(usrdir / "assets00.naz") == archive && file_bytes(usrdir / "configpsp.txt").size() == stock.size(), "disc files changed");
}

// Builders for a block in the game's layout (block_props.hpp).
void put(std::vector<std::uint8_t> &out, std::uint32_t value) { for (unsigned i = 0; i < 4u; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8u * i))); }
std::uint32_t bits(float value) { std::uint32_t out; std::memcpy(&out, &value, 4u); return out; }
std::vector<std::uint32_t> text_words(const std::string &value, std::size_t words) {
    std::vector<std::uint32_t> out(1u + words);
    out[0] = static_cast<std::uint32_t>(words);
    for (std::size_t i = 0; i < value.size(); ++i) out[1u + i / 4u] |= std::uint32_t{static_cast<std::uint8_t>(value[i])} << (8u * (i % 4u));
    return out;
}
struct Records {
    std::vector<std::uint8_t> bytes;
    void create(std::uint32_t entity, const std::string &type) {
        put(bytes, entity); put(bytes, 0xFF000000u);
        for (const auto value : text_words(type, type.size() / 4u + 1u)) put(bytes, value);
    }
    // Returns the offset of the value inside this chunk's records.
    std::size_t set(std::uint32_t entity, std::uint32_t index, const std::vector<std::uint32_t> &value) {
        put(bytes, entity); put(bytes, index);
        const auto at = bytes.size();
        for (const auto item : value) put(bytes, item);
        return at;
    }
};
// A fragment of the given chunks; `starts` gets the offset of each chunk's records.
std::vector<std::uint8_t> make_fragment(const std::vector<Records> &chunks, std::vector<std::size_t> &starts) {
    std::vector<std::uint8_t> out;
    put(out, 24u); put(out, 0u); put(out, 0x80000000u); put(out, 0x2800u); put(out, 0u); put(out, 0u);
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        const auto size = chunks[i].bytes.size();
        put(out, i + 1u == chunks.size() ? 0x80000000u : static_cast<std::uint32_t>(12u + size));
        put(out, static_cast<std::uint32_t>(size)); put(out, 4u);
        starts.push_back(out.size());
        out.insert(out.end(), chunks[i].bytes.begin(), chunks[i].bytes.end());
    }
    return out;
}
struct BlockAsset { std::uint32_t type; std::string name; std::vector<std::uint8_t> payload; bool inside_record{}; };
// A block; `payloads` gets the offset of each asset's payload.
std::vector<std::uint8_t> make_block(bool map, const std::vector<BlockAsset> &assets, std::vector<std::size_t> &payloads) {
    std::vector<std::uint8_t> out;
    if (map) out = pattern(chili::props::map_base, 55u);
    const std::vector<std::uint8_t> stream = map ? pattern(1000u, 56u) : std::vector<std::uint8_t>{};
    const auto count = static_cast<std::uint32_t>(assets.size());
    put(out, 0x0DA24EFAu); put(out, 0x5E6233D4u); put(out, 0x27403369u); put(out, map ? 1u : 0u);
    std::vector<std::uint8_t> info;
    std::vector<std::size_t> inside;
    for (const auto &asset : assets) {
        const auto start = info.size();
        // Type, name pointer, then the fields the engine fills in at load time; the fourth is never initialised.
        put(info, asset.type); put(info, 44u); put(info, 0u); put(info, 0x6D696E61u); put(info, 0u); put(info, 0u); put(info, 0x69730000u); put(info, 24u);
        put(info, 0u); put(info, 0xFFFFFFDCu); put(info, 0u); put(info, 0u);
        info.insert(info.end(), asset.name.begin(), asset.name.end());
        info.resize(start + ((asset.name.size() + 48u + 4u) & ~std::size_t{3}));
        inside.push_back(info.size());
        if (asset.inside_record) {
            // A fragment without a payload: its records follow the name, behind the pointer at +36.
            chili::props::store(info, start + 36u, static_cast<std::uint32_t>(info.size() - start - 36u));
            info.insert(info.end(), asset.payload.begin(), asset.payload.end());
        }
        info.resize(start + std::max<std::size_t>(128u, (info.size() - start + 15u) & ~std::size_t{15}));
    }
    put(out, count); put(out, static_cast<std::uint32_t>(info.size())); put(out, 0u);
    out.push_back(1u); put(out, 4096u); put(out, 4096u + static_cast<std::uint32_t>(stream.size()));
    out.insert(out.end(), stream.begin(), stream.end());
    if (map) { put(out, 0x80000001u); put(out, count - 1u); } else put(out, count);
    const auto info_at = out.size();
    out.insert(out.end(), info.begin(), info.end());
    for (const auto &asset : assets) put(out, asset.inside_record ? 0u : static_cast<std::uint32_t>(asset.payload.size()));
    for (std::size_t i = 0; i < assets.size(); ++i) {
        payloads.push_back(assets[i].inside_record ? info_at + inside[i] : out.size());
        if (!assets[i].inside_record) out.insert(out.end(), assets[i].payload.begin(), assets[i].payload.end());
    }
    return out;
}
std::vector<std::uint8_t> make_database(const std::vector<chili::props::Property> &properties) {
    std::vector<std::uint8_t> out;
    put(out, static_cast<std::uint32_t>(properties.size()));
    for (const auto &property : properties) {
        const auto line = property.name + ':' + property.type;
        put(out, static_cast<std::uint32_t>(line.size() + 1u));
        out.insert(out.end(), line.begin(), line.end()); out.push_back(0u);
    }
    put(out, 1u); put(out, 8u);
    for (const char c : std::string("DoIt():")) out.push_back(static_cast<std::uint8_t>(c));
    out.push_back(0u);
    return out;
}

// properties.ini: the lines, the block and fragment walk, the values written in place, and all of it through a mounted mod.
void property_lines(const std::filesystem::path &root) {
    namespace props = chili::props;
    const std::vector<props::Property> declared{{"Speed", "number"}, {"Count", "integer"}, {"Enabled", "truth"}, {"Offset", "vector"},
        {"Turn", "quaternion"}, {"Label", "string"}, {"Owner", "Entity"}, {"Items", "list(integer)"}, {"Unused", "number"}};
    const auto database_bytes = make_database(declared);
    const auto database = props::read_database(database_bytes);
    require(database.size() == declared.size() && database[3].name == "Offset" && database[3].type == "vector" && database[7].type == "list(integer)", "database.bin properties");
    bool refused = false;
    try { (void)props::read_database(std::span(database_bytes).first(20u)); } catch (const std::runtime_error &) { refused = true; }
    require(refused, "a cut database.bin was accepted");

    const auto base = props::builtin_properties;
    const std::uint32_t first = 0x10000000u, second = 0x10000001u, third = 0x10000002u;
    Records creates, values;
    creates.create(first, "thing(Node)"); creates.create(second, "thing(Node)"); creates.create(third, "other(NodeLogical)");
    creates.set(first, 6u, {0u});
    creates.set(first, 13u, {bits(1.f), bits(2.f), bits(3.f)});
    creates.set(first, props::name_property, text_words("First", 2u));
    const auto speed1 = values.set(first, base + 0u, {bits(1.5f)});
    const auto count1 = values.set(first, base + 1u, {7u});
    const auto enabled1 = values.set(first, base + 2u, {1u});
    const auto offset1 = values.set(first, base + 3u, {bits(1.f), bits(2.f), bits(3.f)});
    const auto label1 = values.set(first, base + 5u, text_words("hello", 3u));
    values.set(first, base + 6u, {second});
    values.set(first, base + 7u, {3u, 11u, 12u, 13u});
    values.set(second, 14u, {0u, 0u, 0u, bits(1.f)});
    values.set(second, props::name_property, text_words("Second One", 3u));
    const auto speed2 = values.set(second, base + 0u, {bits(2.5f)});
    const auto turn2 = values.set(second, base + 4u, {0u, 0u, 0u, bits(1.f)});
    const auto speed3 = values.set(third, base + 0u, {bits(3.5f)});
    std::vector<std::size_t> starts, other_starts, payloads;
    const auto fragment = make_fragment({creates, values}, starts);
    Records lone; lone.create(first, "thing(Node)");
    const auto lone_speed = lone.set(first, base + 0u, {bits(9.f)});
    const auto other = make_fragment({lone}, other_starts);
    const std::vector<BlockAsset> assets{{0u, "/data/textures/a.bmp", pattern(300u, 60u)}, {4u, "/data/Scripts/fragments/Tuning.fragment", fragment},
        {4u, "/data/Scripts/fragments/Empty.fragment", {}}, {3u, "/data/models/b.model", pattern(77u, 61u)},
        {4u, "/data/Scripts/fragments/Sub/Other.fragment", other}};
    for (const bool map : {false, true}) {
        payloads.clear();
        const auto original = make_block(map, assets, payloads);
        const auto listed = props::read_block(original, map);
        require(listed.size() == 5u && listed[1].name == assets[1].name && listed[1].offset == payloads[1] && listed[1].size == fragment.size() &&
            listed[2].size == 0u && listed[4].offset == payloads[4] && listed[4].type == 4u, "block assets");
        const auto walked = props::read_fragment(original, payloads[1], fragment.size(), database);
        require(walked.entities == std::vector<std::uint32_t>{first, second, third} && walked.values.size() == 15u &&
            walked.names.at(first) == "first" && walked.names.at(second) == "second one" && !walked.names.contains(third), "fragment entities and names");
        require(walked.values[9].index == base + 7u && walked.values[9].words == 4u && walked.values[1].words == 3u, "a value of unknown size is not delimited by the next record");

        const auto fragment_at = payloads[1], records = fragment_at + starts[1];
        const std::string ini =
            "; a comment\n[Tuning]\nFirst.Speed = 4.25 ; faster\n#1.Speed = -2\nCount = 0x10\nFirst.Enabled = false\n"
            "First.Offset = 9, 8.5 7\nSecond One.Turn = 0,1,0,0\nFirst.Label = \"a;b\"\n"
            "[Sub/Other.fragment]\nSpeed = 12\n"
            "[Tuning]\nSpeed = oops\nFirst.Label = \"much too long for it\"\nFirst.Owner = 5\nFirst.Items = 1\nThird.Speed = 1\nFirst.Nope = 1\nFirst.Unused = 1\n"
            "[Missing]\nSpeed = 1\n[elsewhere.map : Tuning]\nSpeed = 1\nbroken line\n[]\nSpeed = 2\n";
        std::vector<std::string> problems, done;
        const auto block_name = std::string(map ? "level.map" : props::default_block);
        auto lines = ini;
        if (map) for (std::size_t at = 0; (at = lines.find("[", at)) != std::string::npos; ++at)
            if (lines[at + 1u] != ']' && lines.compare(at, 10u, "[elsewhere") != 0) lines.insert(at + 1u, "Level.MAP : ");
        const auto overrides = props::parse(lines, "mod/properties.ini", problems);
        require(overrides.size() == 17u && problems.size() == 3u && overrides[0].entity == "first" && overrides[0].property == "Speed" &&
            overrides[0].value == "4.25" && overrides[0].block == block_name && overrides[0].fragment == "tuning" && overrides[1].entity == "#1" &&
            overrides[2].entity.empty() && overrides[5].entity == "second one" && overrides[6].value == "\"a;b\"" &&
            overrides[7].fragment == "sub/other" && overrides[0].origin == "mod/properties.ini line 3", "properties.ini lines: " + std::to_string(overrides.size()) + " " + std::to_string(problems.size()));
        problems.clear();
        auto patched = original;
        const auto written = props::apply(patched, block_name, database, overrides, done, problems);
        require(written == 8u && done.size() == 8u && problems.size() == 8u, "values written: " + std::to_string(written) + " done " + std::to_string(done.size()) + " problems " + std::to_string(problems.size()));
        auto expected = original;
        const auto poke = [&](std::size_t at, const std::vector<std::uint32_t> &words) { for (std::size_t i = 0; i < words.size(); ++i) props::store(expected, at + 4u * i, words[i]); };
        poke(records + speed1, {bits(4.25f)}); poke(records + speed2, {bits(-2.f)}); poke(records + count1, {16u}); poke(records + enabled1, {0u});
        poke(records + offset1, {bits(9.f), bits(8.5f), bits(7.f)}); poke(records + turn2, {0u, bits(1.f), 0u, 0u});
        poke(records + label1, text_words("a;b", 3u)); poke(payloads[4] + other_starts[0] + lone_speed, {bits(12.f)});
        require(patched == expected && patched.size() == original.size(), "the patched block differs from the expected bytes");
        require(props::word(patched, records + speed3) == bits(3.5f), "an entity that was not named changed");
        (void)props::read_block(patched, map);
        // Nothing for another block; a damaged block or fragment is left alone and reported.
        done.clear(); problems.clear();
        require(props::apply(patched, "nothing.main", database, overrides, done, problems) == 0u && done.empty() && problems.empty(), "lines for another block applied");
        auto damaged = original;
        props::store(damaged, (map ? props::map_base : 0u) + 16u, 6u);
        const auto before = damaged;
        require(props::apply(damaged, block_name, database, overrides, done, problems) == 0u && damaged == before && problems.size() == 16u, "a damaged block was patched");
        damaged = original; problems.clear();
        props::store(damaged, records + count1 - 8u, 0x10000009u);
        const auto broken = damaged;
        (void)props::apply(damaged, block_name, database, overrides, done, problems);
        for (std::size_t i = 0; i < fragment.size(); ++i) require(damaged[fragment_at + i] == broken[fragment_at + i], "a fragment with a stray record was patched");
    }
    // A fragment the block keeps inside its asset record (no payload) is read and written the same way.
    {
        payloads.clear();
        auto inside = assets;
        inside.push_back({4u, "/data/CustomizeFragments/Weapons/Inside.fragment", other, true});
        inside.push_back({0u, "/data/textures/z.bmp", pattern(40u, 62u)});
        const auto block = make_block(false, inside, payloads);
        const auto listed = props::read_block(block, false);
        require(listed.size() == 7u && listed[5].offset == payloads[5] && listed[5].size >= other.size() && listed[2].size == 0u &&
            listed[6].offset == payloads[6] && listed[6].size == 40u, "a fragment inside its record is not found");
        std::vector<std::string> problems, done;
        const auto overrides = props::parse("[Inside]\n#0.Speed = 3\n[Tuning]\nFirst.Count = 2\n", "mod/properties.ini", problems);
        auto patched = block;
        require(props::apply(patched, props::default_block, database, overrides, done, problems) == 2u && problems.empty(), "a line for a fragment inside its record");
        auto expected = block;
        props::store(expected, payloads[5] + other_starts[0] + lone_speed, bits(3.f));
        props::store(expected, payloads[1] + starts[1] + count1, 2u);
        require(patched == expected, "the block with a fragment inside its record differs from the expected bytes");
    }
    std::string why;
    require(props::encode("number", "1e3", 1u, why) == std::vector<std::uint32_t>{bits(1000.f)} && props::encode("integer", "-3", 1u, why) == std::vector<std::uint32_t>{0xFFFFFFFDu} &&
        props::encode("truth", "Yes", 1u, why) == std::vector<std::uint32_t>{1u} && props::encode("vector", "1 2", 3u, why).empty() &&
        props::encode("number", "1 2", 1u, why).empty() && props::encode("integer", "1.5", 1u, why).empty() && props::encode("truth", "maybe", 1u, why).empty() &&
        props::encode("string", "abcdefg", 3u, why) == text_words("abcdefg", 2u) && props::encode("string", "abcdefgh", 3u, why).empty(), "value text");

    // Through a mount: two mods, the later line wins, a block replaced by a mod file is patched from that file.
    const auto disc = root / "disc0", usrdir = disc / "PSP_GAME" / "USRDIR", mods = root / "mods";
    payloads.clear();
    const auto main_block = make_block(false, assets, payloads);
    const auto main_payloads = payloads;
    payloads.clear();
    const auto map_block = make_block(true, assets, payloads);
    std::vector<Entry> entries;
    const auto archive = make_archive({{"data_baked/scripts/stable/database.bin", database_bytes},
        {"data_psp/maps/g/shared/todpsp_game.main", main_block}, {"data_psp/maps/g/uk/todpsp_game.main", pattern(64u, 70u)},
        {"data_psp/maps/g/shared/level.map", map_block}, {"data_psp/x.bin", pattern(500u, 71u)}}, entries);
    write_file(usrdir / "assets00.naz", archive);
    write_text(mods / "a_first" / "properties.ini", "[Tuning]\nFirst.Speed = 5\nFirst.Count = 3\n[level.map : Tuning]\n#2.Speed = 8\n");
    write_text(mods / "b_second" / "properties.ini", "[Tuning]\nFirst.Speed = 6\nFirst.Nope = 1\n");
    auto replaced_map = map_block;
    replaced_map[10] ^= 0x55u;
    write_file(mods / "b_second" / "data_psp" / "maps" / "g" / "shared" / "level.map", replaced_map);
    write_text(root / "ChiliNative.ini", "");
    psprecomp::Runtime rt; rt.set_game_root(disc); chili::FileRuntime runtime(rt, root / "ms0");
    runtime.mount_mods(chili::ModSetup::load(root / "ChiliNative.ini"));
    GuestFile view(rt, "disc0:/PSP_GAME/USRDIR/assets00.naz");
    const auto listed = chili::naz::read_directory(view);
    require(listed.entries.size() == 5u, "archive view entry count with property lines");
    auto expected_main = main_block, expected_map = replaced_map;
    props::store(expected_main, main_payloads[1] + starts[1] + speed1, bits(6.f));
    props::store(expected_main, main_payloads[1] + starts[1] + count1, 3u);
    props::store(expected_map, payloads[1] + starts[1] + speed3, bits(8.f));
    const auto entry_bytes = [&](std::size_t i) { return read_all(view, listed.entries[i].data_offset(), listed.entries[i].size); };
    require(entry_bytes(1u) == expected_main, "todpsp_game.main in the view is not the patched block");
    require(entry_bytes(3u) == expected_map, "level.map in the view is not the mod's file with the property line written in");
    require(entry_bytes(0u) == database_bytes && entry_bytes(2u) == pattern(64u, 70u) && entry_bytes(4u) == pattern(500u, 71u) &&
        listed.entries[4].header_offset == entries[4].header_offset, "an untouched entry changed");
    require(file_bytes(usrdir / "assets00.naz") == archive && file_bytes(mods / "b_second" / "data_psp" / "maps" / "g" / "shared" / "level.map") == replaced_map,
        "property lines changed a file on disk");
}

// The letter spacing a pack carries for the text font (font_advances.hpp): which folder's file counts, and the
// block the game is served with it.
std::vector<std::uint8_t> font_tables() {
    // Two font records, each in front of its glyph table: 181 glyphs, the ten digits first, the last 30 pictures;
    // advance = cell width, x offset 0, as the game has them.
    std::vector<std::uint8_t> out;
    for (unsigned font = 0; font < 2u; ++font) {
        const auto record = out.size();
        out.resize(record + 0x80u);
        const auto table = out.size();
        chili::props::store(out, record + 4u, 30u);
        chili::props::store(out, record + 0x40u, static_cast<std::uint32_t>(table - record - 0x40u));
        chili::props::store(out, record + 0x44u, 181u);
        float x = 0.f, row = 0.f;
        for (unsigned i = 0; i < 181u; ++i) {
            const float width = static_cast<float>(10u + (i * 7u) % 23u);
            if (x + width > 256.f) { x = 0.f; row += 30.f; }
            const std::uint32_t code = i < 10u ? '0' + i : i < 36u ? 'A' + (i - 10u) : i < 62u ? 'a' + (i - 36u) : 0xC0u + (i - 62u);
            put(out, code | (116u << 16u)); put(out, bits(0.f)); put(out, bits(9.f)); put(out, bits(width));
            put(out, bits(x / 256.f)); put(out, bits(row / 512.f)); put(out, bits(width / 256.f)); put(out, i >= 151u ? 1u : 0u);
            x += width;
        }
        out.resize(out.size() + 0x40u);
    }
    return out;
}
// What the mount printed.
struct Captured {
    std::ostringstream text;
    std::streambuf *old;
    Captured() : old(std::cout.rdbuf(text.rdbuf())) {}
    ~Captured() { std::cout.rdbuf(old); }
    std::size_t lines(const std::string &part) const {
        std::size_t count = 0u;
        const auto all = text.str();
        for (std::size_t at = all.find(part); at != std::string::npos; at = all.find(part, at + 1u)) ++count;
        return count;
    }
};
void font_spacing(const std::filesystem::path &root) {
    namespace props = chili::props;
    namespace fa = chili::font_advances;
    const auto disc = root / "disc0", usrdir = disc / "PSP_GAME" / "USRDIR", mods = root / "mods";
    constexpr char sheet[] = "5d0c6d1a663a8515.png", sidecar[] = "5d0c6d1a663a8515.advances.txt";
    const auto file_text = [](const std::vector<std::uint8_t> &png, const std::string &glyphs) {
        return "chili-font-advances 1\nsheet 5d0c6d1a663a8515 " + fa::hex16(chili::xxh64(png)) + "\nface Test Face\n" + glyphs;
    };
    const auto setup_of = [&](const std::string &ini) { write_text(root / "ChiliNative.ini", ini); return chili::ModSetup::load(root / "ChiliNative.ini"); };

    // Which picture is in force, and which file goes with it.
    const auto menu_png = pattern(3000u, 91u), text_png = pattern(3100u, 92u);
    write_file(mods / "hd_menu" / "textures" / sheet, menu_png);
    write_file(mods / "hd_menu" / "textures" / "002d10b31e312e9d.png", pattern(100u, 93u));
    write_text(mods / "hd_menu" / "textures" / sidecar, file_text(menu_png, "glyph U+0066 20 -1\n"));
    write_file(mods / "hd_text" / "textures" / "5D0C6D1A663A8515.PNG", text_png);
    write_text(mods / "hd_text" / "textures" / sidecar, file_text(text_png, "glyph U+0066 11.5 -4.75\nglyph U+0030 16.25 -2.5\nglyph U+0416 14 -2\n"));
    write_file(mods / "hd_zz" / "textures" / "dc8cc6b510da1d80.png", pattern(50u, 94u));
    write_text(mods / "hd_zz" / "textures" / "5d0c6d1a663a8515.txt", "not a picture");
    write_text(mods / "hd_zz" / "textures" / "5d0c6d1a663a851.png", "a name that is no key");
    std::uint64_t key = 0u;
    require(chili::ModSetup::texture_key_of("x/5D0c6d1a663a8515.PnG", key) && key == chili::text_font_sheet_key && !chili::ModSetup::texture_key_of("5d0c6d1a663a8515.txt", key) &&
        !chili::ModSetup::texture_key_of("5d0c6d1a663a851.png", key) && !chili::ModSetup::texture_key_of("5d0c6d1a663a851g.png", key) &&
        !chili::ModSetup::texture_key_of("5d0c6d1a663a8515.advances.txt", key), "the name of a replacement picture");
    const auto same = [](const std::filesystem::path &a, const std::filesystem::path &b) { std::error_code failed; return std::filesystem::equivalent(a, b, failed); };
    auto setup = setup_of("");
    require(setup.mods.size() == 3u && setup.replace_textures, "three mods");
    require(same(setup.replacement_in_force(chili::text_font_sheet_key), mods / "hd_text" / "textures" / "5D0C6D1A663A8515.PNG") &&
        same(setup.replacement_in_force(0x002d10b31e312e9dull), mods / "hd_menu" / "textures" / "002d10b31e312e9d.png") &&
        same(setup.replacement_in_force(0xdc8cc6b510da1d80ull), mods / "hd_zz" / "textures" / "dc8cc6b510da1d80.png") &&
        setup.replacement_in_force(0x1111111111111111ull).empty(), "the replacement in force is the last folder's that has one");
    auto found = setup.font_advances_file();
    require(found.sheet.parent_path().parent_path().filename() == "hd_text" && same(found.file, mods / "hd_text" / "textures" / sidecar) &&
        found.ignored.size() == 1u && same(found.ignored[0], mods / "hd_menu" / "textures" / sidecar), "the file beside the sheet in force, the earlier folder's ignored");
    setup = setup_of("[Mods]\nEnabled = hd_text, hd_menu\n");
    found = setup.font_advances_file();
    require(same(found.file, mods / "hd_menu" / "textures" / sidecar) && found.ignored.size() == 1u && same(found.ignored[0], mods / "hd_text" / "textures" / sidecar), "Enabled turns the order round");
    setup = setup_of("[Mods]\nEnabled = hd_zz\n");
    found = setup.font_advances_file();
    require(found.sheet.empty() && found.file.empty() && found.ignored.empty(), "no sheet, no file");
    setup = setup_of("[Mods]\nEnabled = hd_menu, hd_zz\n");
    write_text(mods / "hd_zz" / "textures" / sidecar, "chili-font-advances 1\n");
    found = setup.font_advances_file();
    require(same(found.sheet, mods / "hd_menu" / "textures" / sheet) && same(found.file, mods / "hd_menu" / "textures" / sidecar) && found.ignored.size() == 1u &&
        same(found.ignored[0], mods / "hd_zz" / "textures" / sidecar), "a file without its sheet is ignored");
    std::filesystem::remove(mods / "hd_zz" / "textures" / sidecar);

    // Through a mount. The block: a fragment for a property line, and the two fonts inside an asset record.
    const std::uint32_t first = 0x10000000u;
    Records records;
    records.create(first, "thing(Node)");
    records.set(first, props::name_property, text_words("First", 2u));
    const auto speed = records.set(first, props::builtin_properties, {bits(1.5f)});
    std::vector<std::size_t> starts, payloads;
    const auto fragment = make_fragment({records}, starts);
    const auto fonts = font_tables();
    const std::vector<BlockAsset> assets{{0u, "/data/textures/a.bmp", pattern(300u, 60u)}, {4u, "/data/Scripts/fragments/Tuning.fragment", fragment},
        {7u, "/data/Fonts/ODMidText_24.font", fonts, true}, {3u, "/data/models/b.model", pattern(77u, 61u)}};
    const auto block = make_block(false, assets, payloads);
    constexpr std::size_t table_size = 181u * 32u;
    const std::size_t fill = payloads[2] + 0x80u, outline = fill + table_size + 0x40u + 0x80u;
    require(props::read_block(block, false).size() == 4u && props::word(block, fill) == (u'0' | (116u << 16u)) && props::word(block, outline + 32u) == (u'1' | (116u << 16u)), "the made-up block");
    const auto database = make_database({{"Speed", "number"}});
    std::vector<Entry> entries;
    const std::string block_file = "data_psp/maps/g/shared/todpsp_game.main";
    const auto archive = make_archive({{"data_baked/scripts/stable/database.bin", database}, {block_file, block},
        {"data_psp/maps/g/uk/todpsp_game.main", pattern(64u, 70u)}, {"data_psp/x.bin", pattern(500u, 71u)}}, entries);
    write_file(usrdir / "assets00.naz", archive);
    psprecomp::Runtime rt; rt.set_game_root(disc); chili::FileRuntime runtime(rt, root / "ms0");
    // The block the game reads after a mount; empty when the disc's own archive is served untouched.
    const auto served = [&](const chili::ModSetup &mounted, std::string &log) {
        Captured captured;
        runtime.mount_mods(mounted);
        log = captured.text.str();
        GuestFile view(rt, "disc0:/PSP_GAME/USRDIR/assets00.naz");
        if (view.size() == archive.size()) {
            require(read_all(view, 0u, archive.size()) == archive, "the untouched archive is not the file");
            return std::vector<std::uint8_t>{};
        }
        const auto listed = chili::naz::read_directory(view);
        require(listed.entries.size() == 4u && listed.entries[1].name == block_file && listed.entries[1].size == block.size(), "the block in the served archive");
        require(read_all(view, listed.entries[0].data_offset(), listed.entries[0].size) == database && listed.entries[3].header_offset == entries[3].header_offset, "an untouched entry changed");
        return read_all(view, listed.entries[1].data_offset(), listed.entries[1].size);
    };
    // The block with hd_text's file: 'f' and '0' in both tables, nothing else.
    const auto expected = [&](std::vector<std::uint8_t> bytes) {
        for (const std::size_t table : {fill, outline}) {
            props::store(bytes, table + 4u, bits(-2.5f + 2.5f)); props::store(bytes, table + 12u, bits(16.25f + 5.f));   // '0' is entry 0
            const std::size_t f = table + (36u + ('f' - 'a')) * 32u;
            props::store(bytes, f + 4u, bits(-4.75f + 2.5f)); props::store(bytes, f + 12u, bits(11.5f + 5.f));
        }
        return bytes;
    };
    std::string log;
    auto got = served(setup_of(""), log);
    require(got == expected(block), "the served block is not the game's with the two letters of hd_text's file in both tables");
    require(log.find("[mods] font advances: hd_text (Test Face): 2 glyphs in both font tables of todpsp_game.main, 1 unknown; pen steps x") != std::string::npos &&
        log.find("[mods] font advances: hd_menu's file ignored, the fill sheet in force is hd_text's\n") != std::string::npos, "the two log lines: " + log);
    require(runtime.font_spacing().applied && runtime.font_spacing().pack == "hd_text" && runtime.font_spacing().face == "Test Face" && runtime.font_spacing().glyphs == 2u &&
        runtime.font_spacing().unknown == 1u, "what the mount says it did");
    // With a property line for the same block: both changes, each in its place.
    write_text(mods / "hd_zz" / "properties.ini", "[Tuning]\nFirst.Speed = 6\n");
    got = served(setup_of(""), log);
    auto both = expected(block);
    props::store(both, payloads[1] + starts[0] + speed, bits(6.f));
    require(got == both && log.find("font advances: hd_text (Test Face): 2 glyphs") != std::string::npos, "the advances over a block with a property line written in");
    // The block as a mod's own file, tables untouched: edited from that file.
    auto own = block;
    own[payloads[0] + 5u] ^= 0x33u;
    write_file(mods / "hd_zz" / "data_psp" / "maps" / "g" / "shared" / "todpsp_game.main", own);
    got = served(setup_of(""), log);
    both = expected(own);
    props::store(both, payloads[1] + starts[0] + speed, bits(6.f));
    require(got == both, "the advances over a mod's own block file");
    // A mod's block whose table is not the game's own is served as the mod has it.
    props::store(own, fill + 12u, bits(40.f));
    write_file(mods / "hd_zz" / "data_psp" / "maps" / "g" / "shared" / "todpsp_game.main", own);
    got = served(setup_of(""), log);
    both = own;
    props::store(both, payloads[1] + starts[0] + speed, bits(6.f));
    require(got == both && !runtime.font_spacing().applied &&
        log.find("[mods] font advances: hd_text\\textures\\5d0c6d1a663a8515.advances.txt ignored: todpsp_game.main: the font tables are not the game's own") != std::string::npos,
        "a block with another table was edited: " + log);
    std::filesystem::remove_all(mods / "hd_zz" / "data_psp");
    std::filesystem::remove(mods / "hd_zz" / "properties.ini");

    // Nothing of it without the file, with a file that is not the sheet's, with a broken file, or with the software renderer:
    // the archive is the disc's own, byte for byte, and no pack means no line.
    const auto good = file_text(text_png, "glyph U+0066 11.5 -4.75\nglyph U+0030 16.25 -2.5\nglyph U+0416 14 -2\n");
    auto software = setup_of("");
    software.replace_textures = false;
    require(served(software, log).empty() && !runtime.font_spacing().applied && log.find("ignored: the software renderer draws the game's own font") != std::string::npos, "the software renderer");
    write_text(mods / "hd_text" / "textures" / sidecar, file_text(menu_png, "glyph U+0066 11.5 -4.75\n"));
    require(served(setup_of(""), log).empty() && !runtime.font_spacing().applied &&
        log.find("[mods] font advances: hd_text\\textures\\5d0c6d1a663a8515.advances.txt ignored: it was made for another picture") != std::string::npos, "a file made for another picture: " + log);
    write_text(mods / "hd_text" / "textures" / sidecar, good + "glyph U+0041 99 0\n");
    require(served(setup_of(""), log).empty() && log.find("advances.txt ignored: line 7: the step 99 is not between 3 and 48") != std::string::npos, "a file with a wrong number: " + log);
    std::filesystem::remove(mods / "hd_text" / "textures" / sidecar);
    require(served(setup_of(""), log).empty() && log.find("[mods] font advances: hd_menu's file ignored, the fill sheet in force is hd_text's\n") != std::string::npos &&
        log.find("advances.txt ignored:") == std::string::npos, "a pack without the file: " + log);
    std::filesystem::remove(mods / "hd_menu" / "textures" / sidecar);
    require(served(setup_of(""), log).empty() && log.find("font advances") == std::string::npos, "no file anywhere, and a line about it: " + log);
    // And again with the file: the same block as the first time (a mount starts from nothing).
    write_text(mods / "hd_text" / "textures" / sidecar, good);
    require(served(setup_of(""), log) == expected(block) && runtime.font_spacing().applied, "the file put back");
    require(served(setup_of("[Mods]\nEnabled =\n"), log).empty() && !runtime.font_spacing().applied && log.find("font advances") == std::string::npos, "no mods");
    require(file_bytes(usrdir / "assets00.naz") == archive && file_bytes(mods / "hd_text" / "textures" / "5D0C6D1A663A8515.PNG") == text_png, "a file on disk changed");
}

// Nothing reaches the disc folder or the archive, also when the archive has a second hard link.
void write_protection(const std::filesystem::path &root) {
    const auto disc = root / "disc0", linked = root / "disc0_fl1", stick = root / "ms0";
    std::vector<Entry> entries;
    const auto archive = make_archive({{"facecoll.mat", pattern(224u, 81u)}, {"data_psp/a.bin", pattern(4000u, 82u)}}, entries);
    write_file(disc / "PSP_GAME" / "USRDIR" / "assets00.naz", archive);
    write_text(disc / "PSP_GAME" / "USRDIR" / "configpsp.txt", "loadblocks = true\r\n");
    std::filesystem::create_directories(linked / "PSP_GAME" / "USRDIR");
    std::filesystem::create_hard_link(disc / "PSP_GAME" / "USRDIR" / "assets00.naz", linked / "PSP_GAME" / "USRDIR" / "assets00.naz");
    write_text(linked / "PSP_GAME" / "USRDIR" / "configpsp.txt", "loadblocks = true\r\nframe_lock = 1\r\n");
    write_file(root / "mods" / "m" / "data_psp" / "a.bin", pattern(10u, 83u));
    write_text(root / "mods" / "m" / "disc0" / "PSP_GAME" / "USRDIR" / "extra.txt", "mod");
    write_text(root / "ChiliNative.ini", "[Game]\nFrameLock = 2\n");
    const auto stamp = std::filesystem::last_write_time(disc / "PSP_GAME" / "USRDIR" / "assets00.naz");
    {
        psprecomp::Runtime rt; rt.set_game_root(linked); chili::FileRuntime runtime(rt, stick);
        const auto refused = 0x8001001Eu;
        for (const bool mounted : {false, true}) {
            if (mounted) runtime.mount_mods(chili::ModSetup::load(root / "ChiliNative.ini"));
            for (const char *device : {"disc0:", "umd0:", ""})
                for (const char *file : {"/PSP_GAME/USRDIR/assets00.naz", "/PSP_GAME/USRDIR/configpsp.txt", "/PSP_GAME/USRDIR/new.bin", "/PSP_GAME/USRDIR/extra.txt"})
                    for (const std::uint32_t flags : {2u, 3u, 0x602u, 0x102u, 0x201u, 0x401u})
                        require(open(rt, std::string(device) + file, flags) == refused, std::string("opened for writing: ") + device + file);
            for (const std::uint32_t nid : {0x06A70004u, 0x1117C65Fu, 0xF27A9C51u}) {
                string(rt, "disc0:/PSP_GAME/USRDIR/assets00.naz");
                require(call(rt, nid, text, 0777u) == refused, "mkdir, rmdir or remove reached the disc");
            }
            write_text(stick / "save.bin", "save");
            string(rt, "ms0:/save.bin"); string(rt, "disc0:/PSP_GAME/USRDIR/assets00.naz", text + 512u);
            require(call(rt, 0x779103A0u, text, text + 512u) == refused && call(rt, 0x779103A0u, text + 512u, text) == refused, "rename reached the disc");
            require(open(rt, "ms0:/../disc0_fl1/PSP_GAME/USRDIR/assets00.naz", 2u) == 0x8001000Du, "the memory stick path left its folder");
        }
        // A save file that is a hard link to the archive is read, not written.
        std::filesystem::create_hard_link(disc / "PSP_GAME" / "USRDIR" / "assets00.naz", stick / "linked.bin");
        require(open(rt, "ms0:/linked.bin", 2u) == 0x8001000Du && open(rt, "ms0:/linked.bin", 0x602u) == 0x8001000Du, "a hard link in the memory stick was opened for writing");
        const auto uid = open(rt, "ms0:/linked.bin", 1u);
        require(static_cast<std::int32_t>(uid) > 2, "a hard link in the memory stick cannot be read");
        close(rt, uid);
        const auto save = open(rt, "ms0:/save.bin", 0x602u);
        require(static_cast<std::int32_t>(save) > 2, "an ordinary save file cannot be written");
        close(rt, save);
    }
    require(file_bytes(disc / "PSP_GAME" / "USRDIR" / "assets00.naz") == archive && file_bytes(linked / "PSP_GAME" / "USRDIR" / "assets00.naz") == archive &&
        std::filesystem::last_write_time(disc / "PSP_GAME" / "USRDIR" / "assets00.naz") == stamp &&
        std::filesystem::hard_link_count(disc / "PSP_GAME" / "USRDIR" / "assets00.naz") == 3u, "the archive changed");
    require(!std::filesystem::exists(linked / "PSP_GAME" / "USRDIR" / "new.bin") && !std::filesystem::exists(disc / "PSP_GAME" / "USRDIR" / "new.bin") &&
        file_bytes(linked / "PSP_GAME" / "USRDIR" / "configpsp.txt").size() == 35u, "the disc folder changed");
    // A memory stick inside the disc folder, or the other way round, is refused before anything is created.
    for (const auto &[game, memory] : {std::pair{disc, disc / "ms0"}, std::pair{disc, disc}, std::pair{root / "outer" / "disc0", root / "outer"}}) {
        bool thrown = false;
        std::filesystem::create_directories(game);
        try { psprecomp::Runtime rt; rt.set_game_root(game); chili::FileRuntime runtime(rt, memory); }
        catch (const std::runtime_error &) { thrown = true; }
        require(thrown, "overlapping disc and memory stick folders were accepted");
    }
    require(!std::filesystem::exists(disc / "ms0"), "a folder was created inside the disc");
    require(chili::vfs::inside(disc / "PSP_GAME" / "x.png", disc) && chili::vfs::inside(disc, disc) && chili::vfs::inside(disc / "a" / ".." / "b", disc) &&
        !chili::vfs::inside(root / "disc0_other", disc) && !chili::vfs::inside(root, disc) && !chili::vfs::inside(disc / ".." / "ms0", disc), "inside()");
}

// The real archive through the mod layer, as the game reads it.
void actual_disc(const std::filesystem::path &disc, const std::filesystem::path &root) {
    const auto real_file = disc / "PSP_GAME" / "USRDIR" / "assets00.naz";
    chili::vfs::HostFileSource real(real_file);
    const auto original = chili::naz::read_directory(real);
    require(original.entries.size() == 395u, "the real archive does not list 395 entries");
    require(chili::naz::directory_bytes(original.entries, original.offset) ==
        read_all(real, original.offset, static_cast<std::size_t>(real.size() - original.offset)), "regenerated directory differs from the real one");

    // One replaced file and one new file, so the view really is spliced.
    const std::string target = "data_psp/textures/menu/loadscreens/generated/bootscreen.texture_psp";
    const auto replacement = pattern(557239u, 77u), addition = pattern(4096u, 78u);
    const auto mods = root / "actual-mods";
    write_file(mods / "swap" / std::filesystem::path(target), replacement);
    write_file(mods / "swap" / "data_psp" / "mod_files_test" / "added.bin", addition);
    // Two gameplay values: one in the block every mission loads, one in a level block.
    write_text(mods / "swap" / "properties.ini", "[DamageModDict]\nShellDamageMod.StructureDamMod = 7.5\n[mission01_1.map : Weapon_Gun_Light]\nGun_Light.Clipsize_m = 6\n");
    struct Patched { std::string name, fragment, entity, property; std::uint32_t was, now; };
    const std::vector<Patched> patched{
        {"data_psp/maps/todpsp_the_game/todpsp_game/shared/todpsp_game.main", "/data/Scripts/fragments/DamageModDict.fragment", "shelldamagemod", "StructureDamMod", bits(5.f), bits(7.5f)},
        {"data_psp/maps/todpsp_the_game/todpsp_game/shared/mission01_1.map", "/data/CustomizeFragments/Weapons/Weapon_Gun_Light.fragment", "gun_light", "Clipsize_m", 10u, 6u}};
    std::vector<chili::props::Property> database;
    write_text(root / "actual.ini", "[Mods]\nFolder = actual-mods\n");

    psprecomp::Runtime rt; rt.set_game_root(disc); chili::FileRuntime runtime(rt, root / "actual-ms0");
    runtime.mount_mods(chili::ModSetup::load(root / "actual.ini"));
    GuestFile view(rt, "disc0:/PSP_GAME/USRDIR/assets00.naz");
    const auto listed = chili::naz::read_directory(view);
    require(listed.entries.size() == 396u, "the view does not list 395 entries plus the new one");
    std::size_t compared = 0u, full = 0u, blocks = 0u;
    std::uint64_t bytes = 0u;
    for (const auto &entry : original.entries)
        if (entry.name == chili::props::database_path) database = chili::props::read_database(read_all(real, entry.data_offset(), entry.size));
    require(database.size() == 5710u, "database.bin does not declare 5,710 properties");
    for (std::size_t i = 0; i < original.entries.size(); ++i) {
        const auto &was = original.entries[i], &now = listed.entries[i];
        require(was.name == now.name && was.time == now.time && was.date == now.date, "entry " + std::to_string(i) + " name or time changed");
        if (const auto block = std::find_if(patched.begin(), patched.end(), [&](const auto &entry) { return entry.name == was.name; }); block != patched.end()) {
            // The block in the view is the real one with exactly one word changed: the value the line names.
            const auto before = read_all(real, was.data_offset(), was.size), after = read_all(view, now.data_offset(), now.size);
            require(now.size == was.size && now.header_offset >= original.offset && after.size() == before.size(), "a patched block changed size: " + was.name);
            std::size_t changed = 0u, lowest = before.size(), highest = 0u;
            for (std::size_t at = 0; at < before.size(); ++at) if (before[at] != after[at]) { ++changed; lowest = std::min(lowest, at); highest = at; }
            const bool map = was.name.ends_with(".map");
            const auto assets = chili::props::read_block(after, map);
            std::size_t value_at = 0u;
            for (const auto &asset : assets) {
                if (asset.name != block->fragment) continue;
                const auto fragment = chili::props::read_fragment(after, asset.offset, asset.size, database);
                for (const auto &value : fragment.values)
                    if (value.index >= chili::props::builtin_properties && database[value.index - chili::props::builtin_properties].name == block->property &&
                        fragment.names.contains(value.entity) && fragment.names.at(value.entity) == block->entity) value_at = value.offset;
            }
            require(changed >= 1u && value_at && lowest >= value_at && highest < value_at + 4u && chili::props::word(after, value_at) == block->now &&
                chili::props::word(before, value_at) == block->was, "the property line did not change exactly its value in " + was.name);
            ++blocks;
            continue;
        }
        if (was.name == target) {
            require(now.size == replacement.size() && now.header_offset >= original.offset &&
                read_all(view, now.data_offset(), now.size) == replacement, "the replaced entry does not serve the mod file");
            continue;
        }
        require(was.size == now.size && was.header_offset == now.header_offset, "untouched entry " + was.name + " moved");
        // Whole file for the first, the last and every 30th entry; both ends of all the others.
        const bool whole = i == 0u || i + 1u == original.entries.size() || i % 30u == 7u;
        const std::size_t edge = 65536u;
        if (whole || was.size <= 2u * edge) {
            require(read_all(view, now.data_offset(), now.size) == read_all(real, was.data_offset(), was.size), "file differs: " + was.name);
            bytes += was.size; ++full;
        } else {
            require(read_all(view, now.data_offset(), edge) == read_all(real, was.data_offset(), edge) &&
                read_all(view, now.data_offset() + now.size - edge, edge) == read_all(real, was.data_offset() + was.size - edge, edge), "file ends differ: " + was.name);
            bytes += 2u * edge;
        }
        ++compared;
    }
    require(full >= 10u && compared == 392u && blocks == 2u, "fewer than ten whole files compared, or the two patched blocks were not seen");
    require(listed.entries[395].name == "data_psp/mod_files_test/added.bin" && read_all(view, listed.entries[395].data_offset(), 4096u) == addition, "the added entry");
    require(file_bytes(mods / "swap" / std::filesystem::path(target)) == replacement && std::filesystem::file_size(real_file) == real.size(), "source files changed");
    std::cout << "PASS: real archive through the mod layer: 395 entries listed, " << full << " files compared whole and "
              << compared - full << " at both ends (" << bytes << " bytes), 1 replaced, 1 added, 2 blocks with a property line written in\n";

    // A pack's letter spacing over the same mods: the block every mission loads is the one of the first mount
    // (the property line written in) with other bytes only inside the two glyph tables, where the two letters
    // the file names carry its numbers.
    {
        const auto &name = patched[0].name;
        std::size_t index = 0u;
        while (original.entries[index].name != name) ++index;
        const auto before = read_all(view, listed.entries[index].data_offset(), listed.entries[index].size);
        const auto png = pattern(5000u, 79u);
        write_file(mods / "swap" / "textures" / "5d0c6d1a663a8515.png", png);
        write_text(mods / "swap" / "textures" / "5d0c6d1a663a8515.advances.txt", "chili-font-advances 1\nsheet 5d0c6d1a663a8515 " +
            chili::font_advances::hex16(chili::xxh64(png)) + "\nglyph U+0066 11.5 -4.75\nglyph U+0052 15.25 -2.5\n");
        runtime.mount_mods(chili::ModSetup::load(root / "actual.ini"));
        require(runtime.font_spacing().applied && runtime.font_spacing().pack == "swap" && runtime.font_spacing().glyphs == 2u && runtime.font_spacing().unknown == 0u,
            "the file was not applied to the real block");
        GuestFile spaced(rt, "disc0:/PSP_GAME/USRDIR/assets00.naz");
        const auto now = chili::naz::read_directory(spaced);
        require(now.entries.size() == 396u && now.entries[index].name == name && now.entries[index].size == before.size(), "the block in the view with the file");
        const auto after = read_all(spaced, now.entries[index].data_offset(), now.entries[index].size);
        constexpr std::size_t fill = 0x1643E9u, outline = 0x175BE9u, table_size = 181u * 32u;
        std::size_t changed = 0u;
        for (std::size_t at = 0; at < before.size(); ++at) {
            if (before[at] == after[at]) continue;
            ++changed;
            require((at >= fill && at < fill + table_size) || (at >= outline && at < outline + table_size), "a byte outside the two glyph tables changed");
        }
        unsigned letters = 0u;
        for (const std::size_t table : {fill, outline})
            for (std::size_t i = 0; i < 181u; ++i) {
                const auto entry = table + i * 32u;
                const auto code = chili::props::word(after, entry) & 0xFFFFu;
                if (code != u'f' && code != u'R') { require(std::equal(after.begin() + entry, after.begin() + entry + 32u, before.begin() + entry), "another glyph changed"); continue; }
                require(chili::props::word(after, entry + 4u) == bits(code == u'f' ? -2.25f : 0.f) && chili::props::word(after, entry + 12u) == bits(code == u'f' ? 16.5f : 20.25f) &&
                    chili::props::word(before, entry + 4u) == bits(0.f), "a named letter of the real table");
                ++letters;
            }
        require(changed > 0u && letters == 4u, "f and R in both tables");
        std::cout << "PASS: real archive with a letter spacing file: todpsp_game.main served with " << changed << " other bytes, all inside the two glyph tables, over the property line\n";
        std::filesystem::remove_all(mods / "swap" / "textures");
    }

    // With the test switch and no mods the game-side bytes are the file's own.
    write_text(root / "actual-none.ini", "[Mods]\nEnabled =\n");
    auto none = chili::ModSetup::load(root / "actual-none.ini");
    none.virtual_archive = true;
    runtime.mount_mods(none);
    GuestFile same(rt, "disc0:/PSP_GAME/USRDIR/assets00.naz");
    require(same.size() == real.size(), "forced view size");
    const auto tail = static_cast<std::size_t>(real.size() - original.offset) + 4096u;
    require(read_all(same, real.size() - tail, tail) == read_all(real, real.size() - tail, tail) && read_all(same, 0u, 1u << 20u) == read_all(real, 0u, 1u << 20u),
        "forced view differs from the file");
}

// [Rendering] of ChiliNative.ini and the CHILI_RENDER_* variables over it.
void render_settings(const std::filesystem::path &root) {
    using chili::RenderSettings;
    std::filesystem::create_directories(root);
    for (const wchar_t *name : {L"CHILI_RENDER_BACKEND", L"CHILI_RENDER_SCALE", L"CHILI_RENDER_MSAA", L"CHILI_RENDER_STRICT", L"CHILI_RENDER_GLOW"}) _wputenv_s(name, L"");
    const auto ini = root / "ChiliNative.ini";
    auto settings = RenderSettings::load(ini);
    require(settings.backend == RenderSettings::Backend::D3D12 && settings.internal_scale == 0u && settings.msaa == 1u && !settings.strict && !settings.fullscreen &&
            settings.glow == RenderSettings::Glow::Smooth && settings.problems.empty(), "render defaults without a file");
    { std::ofstream out(ini, std::ios::binary); out << "[Input]\r\nScheme=psp\r\n[Rendering]\r\n; note\r\nbackend = Software\r\nInternalScale=3 ; three\r\nMSAA=4\r\nFullscreen=True\r\n[Game]\r\nBackend=d3d12\r\n"; }
    settings = RenderSettings::load(ini);
    require(settings.backend == RenderSettings::Backend::Software && settings.internal_scale == 3u && settings.msaa == 4u && settings.fullscreen && settings.problems.empty(),
            "render settings from the file");
    { std::ofstream out(ini, std::ios::binary); out << "[Rendering]\nBackend=vulkan\nInternalScale=9\nMSAA=3\nShadows=on\nBackend\n"; }
    settings = RenderSettings::load(ini);
    require(settings.backend == RenderSettings::Backend::D3D12 && settings.internal_scale == 0u && settings.msaa == 1u && settings.problems.size() == 5u,
            "bad render settings are reported and the defaults kept");
    require(settings.resolved_scale(1080u, false) == 4u && settings.resolved_scale(768u, false) == 3u && settings.resolved_scale(272u, false) == 1u &&
            settings.resolved_scale(1440u, false) == 6u && settings.resolved_scale(2160u, false) == 8u && settings.resolved_scale(4320u, false) == 8u &&
            settings.resolved_scale(2160u, true) == 1u, "automatic scale follows the monitor up to 8, 1 when unattended");
    { std::ofstream out(ini, std::ios::binary); out << "[Rendering]\nPreset=low\n"; }
    settings = RenderSettings::load(ini);
    require(settings.problems.empty() && settings.resolved_scale(2160u, false) == 2u && settings.resolved_scale(272u, false) == 1u, "Preset=low limits the automatic scale to 2");
    { std::ofstream out(ini, std::ios::binary); out << "[Rendering]\nPreset=medium\nAntiAliasing=msaa4\nAnisotropy=8\nTextureFilter=nearest\nSSAO=low\nGeThread=false\n"; }
    settings = RenderSettings::load(ini);
    require(settings.problems.empty() && settings.resolved_scale(2160u, false) == 4u && settings.msaa == 4u && !settings.fxaa && settings.anisotropy == 8u &&
            settings.texture_filter == RenderSettings::TextureFilter::Nearest && settings.ssao == RenderSettings::AmbientOcclusion::Low && !settings.ge_thread,
            "Preset=medium limits the automatic scale to 4; anti-aliasing, filtering, occlusion and the list thread from the file");
    // The glow: smooth unless the file or the variable says game or off.
    { std::ofstream out(ini, std::ios::binary); out << "[Rendering]\nGlow=Game\n"; }
    settings = RenderSettings::load(ini);
    require(settings.problems.empty() && settings.glow == RenderSettings::Glow::Game, "Glow=game from the file");
    { std::ofstream out(ini, std::ios::binary); out << "[Rendering]\nGlow=smooth\nGlow=off\n"; }
    settings = RenderSettings::load(ini);
    require(settings.problems.empty() && settings.glow == RenderSettings::Glow::Off, "Glow=off from the file");
    _wputenv_s(L"CHILI_RENDER_GLOW", L"game");
    settings = RenderSettings::load(ini);
    require(settings.problems.empty() && settings.glow == RenderSettings::Glow::Game, "CHILI_RENDER_GLOW wins over the file");
    _wputenv_s(L"CHILI_RENDER_GLOW", L"");
    { std::ofstream out(ini, std::ios::binary); out << "[Rendering]\nGlow=bright\n"; }
    settings = RenderSettings::load(ini);
    require(settings.glow == RenderSettings::Glow::Smooth && settings.problems.size() == 1u && settings.problems[0] == "line 2: Glow must be smooth, game or off",
            "a glow that does not exist is reported with the text the launcher looks for, and the default kept");
    { std::ofstream out(ini, std::ios::binary); out << "[Rendering]\nPreset=low\nInternalScale=6\n"; }
    settings = RenderSettings::load(ini);
    require(settings.problems.empty() && settings.resolved_scale(1080u, false) == 6u, "a scale given in the file wins over the preset");
    { std::ofstream out(ini, std::ios::binary); out << "[Rendering]\nInternalScale=5\nInternalScale=Auto\n"; }
    settings = RenderSettings::load(ini);
    require(settings.internal_scale == 0u && settings.problems.empty(), "InternalScale=auto");
    { std::ofstream out(ini, std::ios::binary); out << "[Rendering]\nBackend=software\nInternalScale=2\n"; }
    settings = RenderSettings::load(ini);
    require(settings.resolved_scale(2160u, false) == 2u && settings.resolved_scale(2160u, true) == 2u, "a given scale is used as it is");
    _wputenv_s(L"CHILI_RENDER_BACKEND", L"d3d12"); _wputenv_s(L"CHILI_RENDER_SCALE", L"4"); _wputenv_s(L"CHILI_RENDER_MSAA", L"8"); _wputenv_s(L"CHILI_RENDER_STRICT", L"1");
    settings = RenderSettings::load(ini);
    require(settings.backend == RenderSettings::Backend::D3D12 && settings.internal_scale == 4u && settings.msaa == 8u && settings.strict && settings.problems.empty(),
            "the environment wins over the file");
    _wputenv_s(L"CHILI_RENDER_BACKEND", L"opengl");
    settings = RenderSettings::load(ini);
    require(settings.backend == RenderSettings::Backend::Software && settings.problems.size() == 1u && settings.problems[0].find("CHILI_RENDER_BACKEND") == 0u,
            "a bad variable is reported by name and the file's value kept");
    for (const wchar_t *name : {L"CHILI_RENDER_BACKEND", L"CHILI_RENDER_SCALE", L"CHILI_RENDER_MSAA", L"CHILI_RENDER_STRICT"}) _wputenv_s(name, L"");
    // The template is added once, to a file without the section.
    require(std::string(chili::default_rendering_ini()).find("\n; Glow=smooth\n") != std::string::npos, "the template names the glow and its default");
    { std::ofstream out(ini, std::ios::binary); out << "[Input]\nScheme=psp\n"; }
    chili::add_default_render_settings(ini);
    const auto size = std::filesystem::file_size(ini);
    chili::add_default_render_settings(ini);
    settings = RenderSettings::load(ini);
    require(size > 20u && std::filesystem::file_size(ini) == size && settings.backend == RenderSettings::Backend::D3D12 && settings.problems.empty(),
            "the commented template is added once and changes nothing");
}
}
int main(int argc, char **argv) {
    try {
        if (argc < 2 || argc > 3) throw std::runtime_error("Usage: chili_mod_files_test <scratch directory> [actual disc0]");
        const auto root = std::filesystem::absolute(argv[1]) / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(root);
        archive_format(root / "format");
        game_config();
        ini_and_selection(root / "ini");
        guest_view(root / "guest");
        property_lines(root / "props");
        font_spacing(root / "font");
        write_protection(root / "protect");
        render_settings(root / "render");
        if (argc == 3) actual_disc(argv[2], root);
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::cout << "PASS: NAZ directory round trip, spliced archive view, configpsp.txt overlay, mod selection, guest IO over mod layers, "
                     "property lines, a pack's letter spacing, write protection, render settings (" << checks << " checks)\n";
    } catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
