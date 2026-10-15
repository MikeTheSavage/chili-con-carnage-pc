// The disc as an .iso read in place (host/disc_image.hpp). The test writes a small ISO 9660 image,
// reads it through IsoImage and through the guest's file calls, lays a mod folder over it, and checks
// what is refused. Given a real image and the folder it was unpacked to, it compares the two file by file.
#include "../host/file_runtime.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace {
constexpr auto text = 0x09000000u, buffer = text + 2048u, status = buffer + 1024u, dirent = status + 256u;
constexpr std::size_t sector = 2048u;
void require(bool value, const std::string &message) { if (!value) throw std::runtime_error(message); }
void string(psprecomp::Runtime &rt, const std::string &value, std::uint32_t address = text) {
    for (std::size_t i = 0; i <= value.size(); ++i)
        rt.memory().store8(address + static_cast<std::uint32_t>(i), i == value.size() ? 0u : value[i]);
}
std::uint32_t call(psprecomp::Runtime &rt, std::uint32_t nid, std::uint32_t a0, std::uint32_t a1 = 0u, std::uint32_t a2 = 0u) {
    auto &ctx = rt.cpu(); ctx.gpr[4] = a0; ctx.gpr[5] = a1; ctx.gpr[6] = a2;
    rt.invoke_import("IoFileMgrForUser", nid, ctx); return ctx.gpr[2];
}
std::uint32_t open(psprecomp::Runtime &rt, const std::string &path, std::uint32_t flags) {
    string(rt, path); return call(rt, 0x109F50BCu, text, flags, 0777u);
}
std::string read_all(psprecomp::Runtime &rt, std::uint32_t uid) {
    std::string out;
    for (;;) {
        const auto got = call(rt, 0x6A638D83u, uid, buffer, 7u);
        require(got <= 7u, "read failed");
        if (!got) return out;
        for (unsigned i = 0; i < got; ++i) out.push_back(static_cast<char>(rt.memory().load8(buffer + i)));
    }
}
std::vector<std::string> list(psprecomp::Runtime &rt, const std::string &path) {
    string(rt, path);
    const auto uid = call(rt, 0xB29DDF9Cu, text);
    require(static_cast<std::int32_t>(uid) > 2, "directory open failed: " + path);
    std::vector<std::string> names;
    for (;;) {
        rt.memory().zero(dirent, 352u);
        const auto more = call(rt, 0xE3EB004Cu, uid, dirent);
        require(more <= 1u, "directory read failed");
        if (!more) break;
        names.push_back(rt.memory().read_c_string(dirent + 88u, 256u) + (rt.memory().load32(dirent) & 0x1000u ? "/" : ""));
    }
    require(call(rt, 0xEB092469u, uid) == 0u, "directory close failed");
    return names;
}

// ---- a small image ------------------------------------------------------------------------------
void le32(std::vector<std::uint8_t> &bytes, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4u; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (8u * i));
}
// One directory record: the name as stored, where its bytes are, and the directory flag.
std::vector<std::uint8_t> record(const std::string &name, std::uint32_t lba, std::uint32_t size, bool directory) {
    std::vector<std::uint8_t> out(33u + name.size() + ((name.size() & 1u) ? 0u : 1u));
    out[0] = static_cast<std::uint8_t>(out.size());
    le32(out, 2u, lba); le32(out, 10u, size);
    out[18] = 107u; out[19] = 1u; out[20] = 12u; out[21] = 12u; out[22] = 57u; out[23] = 1u; out[24] = 4u;   // 2007-01-12 12:57:01 +1h
    out[25] = directory ? 2u : 0u;
    out[32] = static_cast<std::uint8_t>(name.size());
    std::copy(name.begin(), name.end(), out.begin() + 33);
    return out;
}
void put(std::vector<std::uint8_t> &image, std::size_t lba, const std::vector<std::vector<std::uint8_t>> &records) {
    std::size_t at = lba * sector;
    for (const auto &item : records) { std::copy(item.begin(), item.end(), image.begin() + static_cast<std::ptrdiff_t>(at)); at += item.size(); }
}
void write(const std::filesystem::path &file, const std::vector<std::uint8_t> &bytes) {
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
// root { UMD_DATA.BIN, PSP_GAME/ { USRDIR/ { Mixed.Txt;1, data.bin } } }
std::vector<std::uint8_t> small_image() {
    std::vector<std::uint8_t> image(24u * sector);
    const std::string self(1u, '\0'), parent(1u, '\1'), umd = "TEST-00001|0123456789ABCDEF|0001|G", mixed = "mixed case name", data = "ABCDEFGHIJ";
    auto *volume = image.data() + 16u * sector;
    volume[0] = 1u; std::memcpy(volume + 1u, "CD001", 5u); volume[6] = 1u;
    volume[128] = 0u; volume[129] = 8u;                                           // 2048-byte blocks
    const auto root = record(self, 18u, sector, true);
    std::copy(root.begin(), root.end(), volume + 156u);
    image[17u * sector] = 255u; std::memcpy(image.data() + 17u * sector + 1u, "CD001", 5u);
    put(image, 18u, {record(self, 18u, sector, true), record(parent, 18u, sector, true), record("PSP_GAME", 19u, sector, true),
                     record("UMD_DATA.BIN", 21u, static_cast<std::uint32_t>(umd.size()), false)});
    put(image, 19u, {record(self, 19u, sector, true), record(parent, 18u, sector, true), record("USRDIR", 20u, sector, true)});
    put(image, 20u, {record(self, 20u, sector, true), record(parent, 19u, sector, true),
                     record("Mixed.Txt;1", 22u, static_cast<std::uint32_t>(mixed.size()), false),
                     record("data.bin", 23u, static_cast<std::uint32_t>(data.size()), false)});
    std::copy(umd.begin(), umd.end(), image.begin() + 21u * sector);
    std::copy(mixed.begin(), mixed.end(), image.begin() + 22u * sector);
    std::copy(data.begin(), data.end(), image.begin() + 23u * sector);
    image.resize(23u * sector + data.size());
    return image;
}
bool refused(const std::filesystem::path &file, const char *word) {
    try { chili::IsoImage image(file); }
    catch (const std::runtime_error &error) { return std::string(error.what()).find(word) != std::string::npos; }
    return false;
}

void image_reader(const std::filesystem::path &root) {
    std::filesystem::create_directories(root);
    const auto bytes = small_image();
    write(root / "small.iso", bytes);
    const chili::IsoImage image(root / "small.iso");
    require(image.files() == 3u, "file count");
    const auto *data = image.find("PSP_GAME\\usrdir/DATA.BIN");
    require(data && !data->directory && data->size == 10u && data->offset == 23u * sector && data->name == "data.bin", "file lookup in any case");
    require(image.file("psp_game/usrdir/mixed.txt") && image.find("psp_game/usrdir/mixed.txt")->name == "Mixed.Txt", "version suffix is dropped");
    require(image.find("psp_game") && image.find("psp_game")->directory && !image.file("psp_game"), "directory entry");
    require(!image.find("psp_game/usrdir/missing.bin") && !image.find("psp_game/usrdir/data.bin/x"), "missing entries");
    require(image.find("")->children == std::vector<std::string>({"psp_game", "umd_data.bin"}), "root children");
    require(image.find("psp_game/usrdir")->children == std::vector<std::string>({"psp_game/usrdir/data.bin", "psp_game/usrdir/mixed.txt"}), "children are sorted");
    const auto whole = image.bytes("PSP_GAME/USRDIR/data.bin", 64u);
    require(std::string(whole.begin(), whole.end()) == "ABCDEFGHIJ" && image.bytes("PSP_GAME/USRDIR/data.bin", 4u).empty(), "whole file, and the limit");
    std::array<std::uint8_t, 8> part{};
    require(image.open(*data)->size() == 10u && image.open(*data)->read(7u, part) == 3u && part[0] == 'H' && part[2] == 'J', "a file is its own range of the image");
    // 2007-01-12 12:57:01 at +1 h is 11:57:01 UTC.
    require(data->time == 1168603021, "record date");

    auto cut = bytes; cut.resize(23u * sector + 4u);
    write(root / "cut.iso", cut);
    require(refused(root / "cut.iso", "cut short"), "a file past the end of the image is refused");
    auto compressed = bytes; std::memcpy(compressed.data(), "CISO", 4u);
    write(root / "compressed.iso", compressed);
    require(refused(root / "compressed.iso", "compressed"), "a CSO is refused");
    write(root / "other.iso", std::vector<std::uint8_t>(40u * sector, 0x41u));
    require(refused(root / "other.iso", "ISO 9660"), "another kind of file is refused");
    write(root / "tiny.iso", std::vector<std::uint8_t>(100u));
    require(refused(root / "tiny.iso", "too small"), "a small file is refused");
    auto pieces = bytes; pieces[20u * sector + 34u + 34u + 25u] = 0x80u;   // the multi-extent flag of the first file record
    write(root / "pieces.iso", pieces);
    require(refused(root / "pieces.iso", "several pieces"), "a multi-extent file is refused");
}

void guest_view(const std::filesystem::path &root) {
    std::filesystem::create_directories(root);
    write(root / "small.iso", small_image());
    // A mod that shadows one file of the disc and adds one.
    const auto mod = root / "mods" / "m" / "disc0" / "PSP_GAME" / "USRDIR";
    std::filesystem::create_directories(mod);
    { std::ofstream out(mod / "DATA.BIN", std::ios::binary); out << "mod"; }
    { std::ofstream out(mod / "extra.bin", std::ios::binary); out << "added"; }
    for (const bool modded : {false, true}) {
        psprecomp::Runtime rt;
        rt.set_game_root(root / "small.iso");
        chili::disc_iso = std::make_shared<chili::IsoImage>(root / "small.iso");
        chili::FileRuntime files(rt, root / "ms0");
        if (modded) { chili::ModSetup setup; setup.folder = root / "mods"; setup.mods.push_back(root / "mods" / "m"); files.mount_mods(setup); }
        const auto uid = open(rt, "disc0:/PSP_GAME/USRDIR/data.bin", 1u);
        require(static_cast<std::int32_t>(uid) > 2, "open of a file in the image");
        require(read_all(rt, uid) == (modded ? "mod" : "ABCDEFGHIJ"), "bytes of the file, the mod's when one shadows it");
        require(call(rt, 0x810C4BC3u, uid) == 0u, "close");
        const auto relative = open(rt, "MIXED.TXT", 1u);
        require(static_cast<std::int32_t>(relative) > 2 && read_all(rt, relative) == "mixed case name", "relative path, any case");
        require(open(rt, "disc0:/PSP_GAME/USRDIR/missing.bin", 1u) == 0x80010002u, "a missing file");
        require(open(rt, "disc0:/PSP_GAME/USRDIR/data.bin", 0x602u) == 0x8001001Eu, "the disc is read-only");
        string(rt, "disc0:/PSP_GAME/USRDIR/new"); require(call(rt, 0x06A70004u, text, 0777u) == 0x8001001Eu, "no folder is made on the disc");
        string(rt, "disc0:/PSP_GAME/USRDIR/Mixed.Txt");
        require(call(rt, 0xACE946E8u, text, status) == 0u && rt.memory().load32(status + 8u) == 15u && (rt.memory().load32(status) & 0xF000u) == 0x2000u &&
            rt.memory().load16(status + 16u + 32u) == 2007u, "stat of a file in the image: size, kind, year");
        string(rt, "disc0:/PSP_GAME");
        require(call(rt, 0xACE946E8u, text, status) == 0u && (rt.memory().load32(status) & 0xF000u) == 0x1000u, "stat of a directory in the image");
        string(rt, "disc0:/PSP_GAME/nothing"); require(call(rt, 0xACE946E8u, text, status) == 0x80010002u, "stat of a missing entry");
        string(rt, "disc0:/PSP_GAME/USRDIR/data.bin"); require(call(rt, 0xB29DDF9Cu, text) == (modded ? 0x80010014u : 0x80010014u), "a file is not a directory");
        string(rt, "disc0:/nothing"); require(call(rt, 0xB29DDF9Cu, text) == 0x80010002u, "a missing directory");
        require(list(rt, "disc0:/") == std::vector<std::string>({"PSP_GAME/", "UMD_DATA.BIN"}), "root listing");
        const auto usrdir = list(rt, "disc0:/PSP_GAME/USRDIR");
        require(usrdir == (modded ? std::vector<std::string>({"DATA.BIN", "Mixed.Txt", "extra.bin"}) : std::vector<std::string>({"Mixed.Txt", "data.bin"})),
            "folder listing: one entry per name, the mod's spelling when it has the file");
        if (modded) {
            const auto added = open(rt, "disc0:/PSP_GAME/USRDIR/extra.bin", 1u);
            require(static_cast<std::int32_t>(added) > 2 && read_all(rt, added) == "added", "a file only the mod has");
        }
        chili::disc_iso.reset();
    }
}

// Every file of the image against the folder it was unpacked to.
void actual_disc(const std::filesystem::path &iso, const std::filesystem::path &folder) {
    const chili::IsoImage image(iso);
    std::size_t files = 0u;
    std::uint64_t bytes = 0u;
    for (const auto &item : std::filesystem::recursive_directory_iterator(folder)) {
        if (!item.is_regular_file()) continue;
        const auto inside = item.path().lexically_relative(folder).generic_string();
        const auto *entry = image.file(inside);
        require(entry && entry->size == item.file_size(), "the image and the folder differ at " + inside);
        const auto piece = image.open(*entry);
        std::ifstream host(item.path(), std::ios::binary);
        std::vector<std::uint8_t> a(1u << 20u), b(1u << 20u);
        for (std::uint64_t at = 0u; at < entry->size; at += a.size()) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(a.size(), entry->size - at));
            require(piece->read(at, std::span(a).first(count)) == count, "short read in the image at " + inside);
            host.read(reinterpret_cast<char *>(b.data()), static_cast<std::streamsize>(count));
            require(static_cast<std::size_t>(host.gcount()) == count && std::equal(a.begin(), a.begin() + static_cast<std::ptrdiff_t>(count), b.begin()),
                "bytes differ at " + inside);
        }
        ++files; bytes += entry->size;
    }
    require(files == image.files(), "the folder has " + std::to_string(files) + " files, the image " + std::to_string(image.files()));
    std::cout << "actual disc: " << files << " files, " << bytes << " bytes equal in the image and in the folder\n";
}
}
int main(int argc, char **argv) {
    try {
        if (argc != 2 && argc != 4) throw std::runtime_error("Usage: chili_disc_image_test <scratch directory> [<disc image> <unpacked disc folder>]");
        const auto root = std::filesystem::absolute(argv[1]) / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        image_reader(root / "reader");
        guest_view(root / "guest");
        if (argc == 4) actual_disc(argv[2], argv[3]);
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::cout << "PASS: image directory, names, ranges and dates; refusals; guest open, read, stat and listing on an image, with a mod folder over it\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
