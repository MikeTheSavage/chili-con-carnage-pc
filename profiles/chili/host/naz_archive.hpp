#pragma once
#include "vfs.hpp"
#include <array>
#include <map>
#include <string_view>
namespace chili::naz {
// The game's asset archive (assets00.naz), the same container as Total Overdose
// PC: a ZIP variant with every file stored uncompressed.
//
//   end record, last 22 bytes   magic 0x16ED5B50, then the ZIP end-of-directory fields
//   directory record, 46 bytes  magic 0x0406F370, time, date, crc (0), packed size, size, name length,
//                               extra length (1), comment length, disk, attributes, local header offset,
//                               then the words 10, 20, 8, 0; the name with every byte rotated right by
//                               two bits; one extra byte 0x88
//   entry                       30 + name length + 1 bytes of local header the engine never reads,
//                               the file's bytes, 12 unused bytes
//
// The engine reads the end record and the directory once and from then on seeks
// straight to local header offset + 30 + name length + extra length.
inline constexpr std::uint32_t end_magic = 0x16ED5B50u, record_magic = 0x0406F370u;
inline constexpr std::size_t end_size = 22u, record_size = 46u, local_header_size = 30u, trailer_size = 12u;
inline constexpr std::uint8_t extra_byte = 0x88u;
// Offsets stay below 2 GiB so they are safe in the engine's signed arithmetic.
inline constexpr std::uint64_t size_limit = 0x7FFFFFFFull;

struct Entry {
    std::string name;   // as stored: lower case, forward slashes
    std::uint32_t header_offset{}, size{};
    std::uint16_t time{}, date{}, extra_length{1};
    [[nodiscard]] std::uint64_t data_offset() const { return std::uint64_t{header_offset} + local_header_size + name.size() + extra_length; }
};
struct Directory {
    std::vector<Entry> entries;
    std::uint64_t offset{}, size{};   // of the directory records inside the archive
};

inline std::uint16_t get16(const std::uint8_t *p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8u)); }
inline std::uint32_t get32(const std::uint8_t *p) { return get16(p) | (static_cast<std::uint32_t>(get16(p + 2)) << 16u); }
inline void put16(std::vector<std::uint8_t> &out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value)); out.push_back(static_cast<std::uint8_t>(value >> 8u));
}
inline void put32(std::vector<std::uint8_t> &out, std::uint32_t value) { put16(out, value); put16(out, value >> 16u); }

// Archive spelling of a path: lower case, forward slashes, no leading slash.
inline std::string normal_name(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '\\' || c == '/') { if (!out.empty() && out.back() != '/') out.push_back('/'); }
        else out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }
    while (!out.empty() && out.back() == '/') out.pop_back();
    return out;
}

inline Directory read_directory(vfs::Source &archive) {
    const auto fail = [](const std::string &what) -> Directory { throw std::runtime_error("NAZ archive: " + what); };
    const auto total = archive.size();
    std::array<std::uint8_t, end_size> end{};
    if (total < end_size || archive.read(total - end_size, end) != end_size) return fail("too small");
    if (get32(end.data()) != end_magic) return fail("no end record");
    const std::uint32_t count = get16(end.data() + 8u);
    Directory directory;
    directory.size = get32(end.data() + 12u);
    directory.offset = get32(end.data() + 16u);
    if (get16(end.data() + 10u) != count || get16(end.data() + 20u) || directory.offset + directory.size + end_size != total)
        return fail("the end record does not describe this file");
    std::vector<std::uint8_t> records(static_cast<std::size_t>(directory.size));
    if (archive.read(directory.offset, records) != records.size()) return fail("cannot read the directory");
    std::size_t at = 0u;
    for (std::uint32_t index = 0; index < count; ++index) {
        if (at + record_size > records.size()) return fail("a directory record runs past the directory");
        const auto *record = records.data() + at;
        if (get32(record) != record_magic) return fail("bad directory record " + std::to_string(index));
        Entry entry;
        entry.time = get16(record + 4u);
        entry.date = get16(record + 6u);
        entry.size = get32(record + 16u);
        const std::size_t name_length = get16(record + 20u), comment_length = get16(record + 24u);
        entry.extra_length = get16(record + 22u);
        entry.header_offset = get32(record + 34u);
        if (get32(record + 12u) != entry.size) return fail("entry " + std::to_string(index) + " is compressed");
        if (at + record_size + name_length + entry.extra_length + comment_length > records.size())
            return fail("a directory record runs past the directory");
        for (std::size_t i = 0; i < name_length; ++i) {
            const auto stored = record[record_size + i];
            entry.name.push_back(static_cast<char>(static_cast<std::uint8_t>((stored << 2u) | (stored >> 6u))));
        }
        if (entry.data_offset() + entry.size > directory.offset) return fail("entry " + entry.name + " runs into the directory");
        at += record_size + name_length + entry.extra_length + comment_length;
        directory.entries.push_back(std::move(entry));
    }
    if (at != records.size()) return fail("stray bytes after the last directory record");
    return directory;
}

// The directory records and the end record for entries whose data sits before directory_offset.
inline std::vector<std::uint8_t> directory_bytes(const std::vector<Entry> &entries, std::uint64_t directory_offset) {
    if (entries.size() > 0xFFFFu) throw std::runtime_error("NAZ archive: more than 65535 files");
    std::vector<std::uint8_t> out;
    for (const auto &entry : entries) {
        if (entry.name.size() > 0xFFFFu) throw std::runtime_error("NAZ archive: name too long");
        put32(out, record_magic); put16(out, entry.time); put16(out, entry.date); put32(out, 0u);
        put32(out, entry.size); put32(out, entry.size);
        put16(out, static_cast<std::uint32_t>(entry.name.size())); put16(out, entry.extra_length); put16(out, 0u);
        put16(out, 0u); put16(out, 0u); put32(out, 0u); put32(out, entry.header_offset);
        put16(out, 10u); put16(out, 20u); put16(out, 8u); put16(out, 0u);
        for (const char c : entry.name) {
            const auto plain = static_cast<std::uint8_t>(c);
            out.push_back(static_cast<std::uint8_t>((plain >> 2u) | (plain << 6u)));
        }
        out.insert(out.end(), entry.extra_length, extra_byte);
    }
    const auto size = out.size();
    if (directory_offset + size + end_size > size_limit) throw std::runtime_error("NAZ archive: larger than 2 GiB");
    put32(out, end_magic); put16(out, 0u); put16(out, 0u);
    put16(out, static_cast<std::uint32_t>(entries.size())); put16(out, static_cast<std::uint32_t>(entries.size()));
    put32(out, static_cast<std::uint32_t>(size)); put32(out, static_cast<std::uint32_t>(directory_offset)); put16(out, 0u);
    return out;
}

// One replacement or new file for the archive view.
struct Replacement {
    std::string name;             // archive path, any spelling
    std::filesystem::path file;
    std::shared_ptr<vfs::Source> source;   // set for bytes made in memory; `file` then only names their origin
};
struct Overlay {
    std::shared_ptr<vfs::Source> archive;
    std::vector<Entry> entries;   // the directory the game sees
    std::size_t replaced{}, added{};
};

// The archive the game sees: the real archive's data region as it is on disk,
// then one entry per replacement file, then a new directory. A replaced file
// keeps its place in the directory and points at the appended copy; every
// other file keeps its offset, and nothing is copied.
inline Overlay overlay(std::shared_ptr<vfs::Source> base, const std::vector<Replacement> &files) {
    auto directory = read_directory(*base);
    Overlay result;
    result.entries = std::move(directory.entries);
    std::map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < result.entries.size(); ++i) index[normal_name(result.entries[i].name)] = i;
    auto archive = std::make_shared<vfs::SegmentedSource>();
    archive->append(base, 0u, directory.offset);
    std::size_t longest = 0u;
    for (const auto &file : files) longest = std::max(longest, file.name.size());
    const auto zeros = std::make_shared<vfs::MemorySource>(std::vector<std::uint8_t>(local_header_size + longest + 1u + trailer_size));
    for (const auto &file : files) {
        const auto name = normal_name(file.name);
        if (name.empty() || name.size() > 0xFFFFu) throw std::runtime_error("NAZ archive: bad entry name '" + file.name + "'");
        std::shared_ptr<vfs::Source> data = file.source;
        if (!data) data = std::make_shared<vfs::HostFileSource>(file.file);
        const auto header = archive->size(), header_bytes = std::uint64_t{local_header_size} + name.size() + 1u;
        if (header + header_bytes + data->size() + trailer_size > size_limit)
            throw std::runtime_error("NAZ archive: the mods take the archive past 2 GiB at " + file.file.string());
        Entry entry;
        entry.name = name;
        entry.header_offset = static_cast<std::uint32_t>(header);
        entry.size = static_cast<std::uint32_t>(data->size());
        entry.date = 0x21u;   // 1980-01-01; host file times stay out of the guest
        archive->append(zeros, 0u, header_bytes);
        archive->append(std::move(data), 0u, entry.size);
        archive->append(zeros, 0u, trailer_size);
        if (const auto found = index.find(name); found != index.end()) {
            auto &old = result.entries[found->second];
            if (old.header_offset < directory.offset) ++result.replaced;
            entry.name = old.name; entry.time = old.time; entry.date = old.date;
            old = std::move(entry);
        } else {
            index[name] = result.entries.size();
            result.entries.push_back(std::move(entry));
            ++result.added;
        }
    }
    auto records = directory_bytes(result.entries, archive->size());
    const auto records_size = records.size();
    archive->append(std::make_shared<vfs::MemorySource>(std::move(records)), 0u, records_size);
    result.archive = std::move(archive);
    return result;
}
}
