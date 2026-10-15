#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace chili::sfo {
using Bytes = std::vector<std::uint8_t>;
enum class Format : std::uint16_t { Blob=0x0004, Text=0x0204, Integer=0x0404 };
enum class ErrorCode {
    Truncated, InvalidMagic, UnsupportedVersion, InvalidTables, UnsupportedFormat,
    InvalidKey, DuplicateKey, InvalidLength, InvalidAlignment, Overlap,
    InvalidText, WrongType, LimitExceeded
};
class Error : public std::runtime_error {
public:
    Error(ErrorCode code, const std::string &message) : std::runtime_error(message), code_(code) {}
    ErrorCode code() const noexcept { return code_; }
private:
    ErrorCode code_;
};
// Native resource bounds, not claimed PSP format/firmware limits.
struct Limits {
    std::size_t file_bytes=16u*1024u*1024u;
    std::size_t entries=4096u;
};
struct Entry {
    std::string key;
    Format format{Format::Blob};
    std::uint32_t used_length{};
    // Exact reserved slot, including unused bytes. No opaque blob interpretation.
    Bytes storage;
    static Entry text(std::string key, std::string_view value, std::uint32_t capacity=0u, Limits limits={});
    static Entry integer(std::string key, std::uint32_t value);
    static Entry blob(std::string key, std::span<const std::uint8_t> value, std::uint32_t capacity=0u, Limits limits={});
    std::string_view text_value() const;
    std::uint32_t integer_value() const;
    std::span<const std::uint8_t> blob_value() const;
    bool operator==(const Entry &) const = default;
};
struct Document {
    std::vector<Entry> entries;
    const Entry *find(std::string_view key) const noexcept;
    Entry *find(std::string_view key) noexcept;
    bool operator==(const Document &) const = default;
};
// Pure byte transformations. Each result is complete or an exception is raised;
// neither API writes guest memory, files, secure metadata or previous documents.
Document parse(std::span<const std::uint8_t> bytes, Limits limits={});
Bytes serialize(const Document &document, Limits limits={});
}
