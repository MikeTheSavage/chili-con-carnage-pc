#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <io.h>
#include <memory>
#include <share.h>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <utility>
#include <vector>
namespace chili::vfs {
// True when `path` is `folder` or lies below it, links resolved.
inline bool inside(const std::filesystem::path &path, const std::filesystem::path &folder) {
    std::error_code status;
    const auto relative = std::filesystem::weakly_canonical(std::filesystem::absolute(path, status), status)
        .lexically_relative(std::filesystem::weakly_canonical(std::filesystem::absolute(folder, status), status));
    return !relative.empty() && *relative.begin() != "..";
}

// Read-only bytes addressed by offset: what a guest file handle reads from when
// the file is not simply a file on the host. Not thread safe; all guest file IO
// runs on the one host thread that runs the guest.
class Source {
public:
    virtual ~Source() = default;
    [[nodiscard]] virtual std::uint64_t size() const = 0;
    // Copies up to out.size() bytes starting at offset and returns how many; the
    // count is short only where the range runs past the end.
    virtual std::size_t read(std::uint64_t offset, std::span<std::uint8_t> out) = 0;
};

class MemorySource final : public Source {
public:
    explicit MemorySource(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}
    [[nodiscard]] std::uint64_t size() const override { return bytes_.size(); }
    std::size_t read(std::uint64_t offset, std::span<std::uint8_t> out) override {
        if (offset >= bytes_.size()) return 0u;
        const auto count = std::min<std::uint64_t>(out.size(), bytes_.size() - offset);
        std::memcpy(out.data(), bytes_.data() + offset, static_cast<std::size_t>(count));
        return static_cast<std::size_t>(count);
    }
private:
    std::vector<std::uint8_t> bytes_;
};

// A host file. Its size is fixed when the source is made, so whatever is built
// on top stays consistent; it is opened at the first read and kept open, and a
// part that has gone missing since reads as zero.
class HostFileSource final : public Source {
public:
    explicit HostFileSource(std::filesystem::path file) : file_(std::move(file)) {
        std::error_code status;
        size_ = std::filesystem::file_size(file_, status);
        if (status) throw std::runtime_error("Cannot read " + file_.string() + ": " + status.message());
    }
    ~HostFileSource() override { if (handle_ >= 0) _close(handle_); }
    HostFileSource(const HostFileSource &) = delete;
    HostFileSource &operator=(const HostFileSource &) = delete;
    [[nodiscard]] std::uint64_t size() const override { return size_; }
    std::size_t read(std::uint64_t offset, std::span<std::uint8_t> out) override {
        if (offset >= size_) return 0u;
        const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), size_ - offset));
        if (handle_ < 0 && !failed_) {
            // Other programs may read the file while the game has it, not rewrite it.
            failed_ = _wsopen_s(&handle_, file_.c_str(), _O_BINARY | _O_RDONLY, _SH_DENYWR, _S_IREAD) != 0;
            if (failed_) handle_ = -1;
        }
        std::size_t done = 0u;
        if (handle_ >= 0 && _lseeki64(handle_, static_cast<__int64>(offset), SEEK_SET) >= 0) {
            while (done < wanted) {
                const auto step = _read(handle_, out.data() + done, static_cast<unsigned>(std::min<std::size_t>(wanted - done, 0x40000000u)));
                if (step <= 0) break;
                done += static_cast<std::size_t>(step);
            }
        }
        std::memset(out.data() + done, 0, wanted - done);
        return wanted;
    }
private:
    std::filesystem::path file_;
    std::uint64_t size_{};
    int handle_{-1};
    bool failed_{};
};

// Pieces of other sources laid end to end.
class SegmentedSource final : public Source {
public:
    void append(std::shared_ptr<Source> source, std::uint64_t offset, std::uint64_t length) {
        if (!length) return;
        if (!segments_.empty() && segments_.back().source == source &&
            segments_.back().source_offset + segments_.back().length == offset) {
            segments_.back().length += length;
        } else segments_.push_back({size_, length, offset, std::move(source)});
        size_ += length;
    }
    [[nodiscard]] std::uint64_t size() const override { return size_; }
    [[nodiscard]] std::size_t segments() const { return segments_.size(); }
    std::size_t read(std::uint64_t offset, std::span<std::uint8_t> out) override {
        std::size_t done = 0u;
        // First segment that ends after offset.
        auto segment = std::upper_bound(segments_.begin(), segments_.end(), offset,
            [](std::uint64_t value, const Segment &entry) { return value < entry.start + entry.length; });
        for (; segment != segments_.end() && done < out.size(); ++segment) {
            const auto inside = offset + done - segment->start;
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(out.size() - done, segment->length - inside));
            const auto got = segment->source->read(segment->source_offset + inside, out.subspan(done, count));
            std::memset(out.data() + done + got, 0, count - got);
            done += count;
        }
        return done;
    }
private:
    struct Segment {
        std::uint64_t start, length, source_offset;
        std::shared_ptr<Source> source;
    };
    std::vector<Segment> segments_;
    std::uint64_t size_{};
};
}
