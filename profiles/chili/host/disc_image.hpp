#pragma once
#include "vfs.hpp"
#include <array>
#include <ctime>
#include <map>
#include <string_view>
namespace chili {
// The game disc as an .iso file, read in place: nothing is unpacked or copied.
//
// A UMD image is plain ISO 9660: one primary volume descriptor, 2048-byte
// blocks, every file in one extent. That is all this reads; a compressed image
// (.cso), an interleaved or a multi-extent file is refused with a reason. The
// directory tree is read once when the image is opened (14 files on this
// disc); file contents are read on demand from the image file, which stays
// open read-only and may be read, not rewritten, by other programs meanwhile.
class IsoImage {
public:
    struct Entry {
        std::string name;                   // as on the disc, without the ";1" version suffix
        bool directory{};
        std::uint64_t offset{}, size{};     // a file's bytes inside the image
        __time64_t time{};                  // the record's date
        std::vector<std::string> children;  // a directory's entries as keys, sorted
    };
    explicit IsoImage(const std::filesystem::path &file)
        : path_(file), source_(std::make_shared<vfs::HostFileSource>(file)) {
        std::array<std::uint8_t, block> descriptor{};
        if (source_->size() < 17u * block) fail("the file is too small to be a disc image");
        source_->read(0u, std::span(descriptor).first(4u));
        if (std::memcmp(descriptor.data(), "CISO", 4u) == 0)
            fail("it is a compressed image (CSO); convert it to a plain .iso first");
        for (std::uint64_t sector = 16u;; ++sector) {
            if (sector >= 64u || (sector + 1u) * block > source_->size()) fail("it has no ISO 9660 volume descriptor");
            source_->read(sector * block, descriptor);
            if (std::memcmp(descriptor.data() + 1u, "CD001", 5u) != 0) fail("it is not an ISO 9660 image");
            if (descriptor[0] == 1u) break;
            if (descriptor[0] == 255u) fail("it has no primary volume descriptor");
        }
        if (le16(descriptor.data() + 128u) != block) fail("its block size is not 2048 bytes");
        entries_[""] = Entry{{}, true, 0u, 0u, 0, {}};
        walk(descriptor.data() + 156u, "", 0u);
    }
    [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }
    [[nodiscard]] std::size_t files() const noexcept { return files_; }
    // Key of a path inside the disc: lower case, forward slashes, no leading or trailing slash.
    [[nodiscard]] static std::string key(std::string_view inside) {
        std::string out;
        for (const char c : inside) {
            if (c == '\\' || c == '/') { if (!out.empty() && out.back() != '/') out.push_back('/'); }
            else out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
        }
        while (!out.empty() && out.back() == '/') out.pop_back();
        return out;
    }
    // The file or directory at a path inside the disc, in any case; null when there is none.
    [[nodiscard]] const Entry *find(std::string_view inside) const {
        const auto found = entries_.find(key(inside));
        return found == entries_.end() ? nullptr : &found->second;
    }
    [[nodiscard]] const Entry *file(std::string_view inside) const {
        const auto *entry = find(inside);
        return entry && !entry->directory ? entry : nullptr;
    }
    [[nodiscard]] std::shared_ptr<vfs::Source> open(const Entry &entry) const {
        auto piece = std::make_shared<vfs::SegmentedSource>();
        piece->append(source_, entry.offset, entry.size);
        return piece;
    }
    // A whole file; empty when it is missing or larger than `limit`.
    [[nodiscard]] std::vector<std::uint8_t> bytes(std::string_view inside, std::uint64_t limit) const {
        std::vector<std::uint8_t> out;
        if (const auto *entry = file(inside); entry && entry->size <= limit) {
            out.resize(static_cast<std::size_t>(entry->size));
            source_->read(entry->offset, out);
        }
        return out;
    }
private:
    static constexpr std::size_t block = 2048u;
    static std::uint32_t le16(const std::uint8_t *p) { return p[0] | (std::uint32_t{p[1]} << 8u); }
    static std::uint32_t le32(const std::uint8_t *p) { return le16(p) | (le16(p + 2u) << 16u); }
    [[noreturn]] void fail(const std::string &what) const {
        const auto name = path_.filename().u8string();
        throw std::runtime_error(std::string(name.begin(), name.end()) + ": " + what);
    }
    // `record` is a directory's own record; its entries are added below `parent` (a key, "" for the root).
    void walk(const std::uint8_t *record, const std::string &parent, unsigned depth) {
        const std::uint64_t start = (std::uint64_t{le32(record + 2u)} + record[1]) * block, length = le32(record + 10u);
        if (depth > 8u || length > (4u << 20u) || start + length > source_->size()) fail("a directory lies outside the image");
        std::vector<std::uint8_t> data(static_cast<std::size_t>(length));
        source_->read(start, data);
        std::vector<std::string> children;
        for (std::size_t at = 0u; at < data.size();) {
            const std::size_t size = data[at];
            // Records do not cross a block; the rest of a block is zero.
            if (!size) { at = (at / block + 1u) * block; continue; }
            const auto *item = data.data() + at;
            if (size < 34u || at + size > data.size() || 33u + item[32] > size) fail("a directory record is damaged");
            at += size;
            std::string name(reinterpret_cast<const char *>(item + 33u), item[32]);
            if (name == std::string(1u, '\0') || name == std::string(1u, '\1')) continue;   // the directory itself, its parent
            if (const auto version = name.find(';'); version != std::string::npos) name.erase(version);
            if (!name.empty() && name.back() == '.') name.pop_back();
            const bool plain = !name.empty() && name != ".." && std::all_of(name.begin(), name.end(), [](char c) {
                return c >= 32 && c <= 126 && c != '/' && c != '\\' && c != ':'; });
            if (!plain) fail("a file name is not plain text");
            Entry entry;
            entry.name = name;
            entry.directory = (item[25] & 2u) != 0u;
            entry.offset = (std::uint64_t{le32(item + 2u)} + item[1]) * block;
            entry.size = le32(item + 10u);
            entry.time = record_time(item + 18u);
            const auto path = parent.empty() ? key(name) : parent + '/' + key(name);
            if (entries_.contains(path)) continue;
            if (!entry.directory) {
                if ((item[25] & 0x80u) || item[26]) fail(name + " is stored in several pieces, which is not supported");
                if (entry.offset + entry.size > source_->size()) fail(name + " runs past the end of the image (the file is cut short)");
                ++files_;
            }
            if (entries_.size() > 65536u) fail("it has too many files");
            children.push_back(path);
            entries_[path] = std::move(entry);
            if (item[25] & 2u) walk(item, path, depth + 1u);
        }
        std::sort(children.begin(), children.end());
        entries_[parent].children = std::move(children);
    }
    // Years since 1900, month, day, hour, minute, second, offset from GMT in quarter hours.
    static __time64_t record_time(const std::uint8_t *date) {
        std::tm parts{};
        parts.tm_year = date[0]; parts.tm_mon = date[1] ? date[1] - 1 : 0; parts.tm_mday = date[2] ? date[2] : 1;
        parts.tm_hour = date[3]; parts.tm_min = date[4]; parts.tm_sec = date[5];
        const auto utc = _mkgmtime64(&parts);
        return utc < 0 ? 0 : utc - static_cast<std::int8_t>(date[6]) * 900;
    }
    std::filesystem::path path_;
    std::shared_ptr<vfs::HostFileSource> source_;
    std::map<std::string, Entry> entries_;
    std::size_t files_{};
};

// The disc of this run when it is an .iso; null when it is a folder. Set once in main before the file
// runtime is made. The file runtime, the UMD service and the PSMF module loader read the disc through it.
inline std::shared_ptr<IsoImage> disc_iso;
}
