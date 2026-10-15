#include "savedata_size_query.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <utility>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace chili::savedata_size_prep {
namespace {
bool span(const psprecomp::GuestMemory &m, std::uint32_t address, std::size_t length) {
    return address && !(address & 0x80000003u) && m.contains(address, length);
}
void need_span(const psprecomp::GuestMemory &m, std::uint32_t address, std::size_t length) {
    if (!span(m, address, length)) throw QueryFailure(Failure::InvalidAddress, "Invalid savedata size guest span");
}
bool intersects(std::uint32_t a, std::size_t as, std::uint32_t b, std::size_t bs) {
    const auto ca = std::uint64_t(psprecomp::GuestMemory::canonical(a));
    const auto cb = std::uint64_t(psprecomp::GuestMemory::canonical(b));
    return ca < cb + bs && cb < ca + as;
}
std::vector<std::uint8_t> bytes(const psprecomp::GuestMemory &m, std::uint32_t address, std::size_t length) {
    need_span(m, address, length); std::vector<std::uint8_t> result(length); m.copy_out(address, result); return result;
}
std::string string_at(const psprecomp::GuestMemory &m, std::uint32_t address, unsigned length) {
    std::string value;
    for (unsigned i = 0; i < length; ++i) {
        const auto c = m.load8(address + i); if (!c) break; value += static_cast<char>(c);
    }
    return value;
}
void valid_name(const std::string &value, bool nonempty) {
    if ((nonempty && value.empty()) || value == "." || value == "..")
        throw QueryFailure(Failure::Unsupported, "Unsupported savedata size name");
    for (const unsigned char c : value)
        if (c < 32u || c > 126u || std::string("/\\:*?\"<>|").find(static_cast<char>(c)) != std::string::npos)
            throw QueryFailure(Failure::Unsupported, "Savedata size wildcards/special/unsafe names need verified semantics");
}
bool directory(const std::filesystem::path &path) {
    const auto attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;
        throw QueryFailure(Failure::Filesystem, "Cannot inspect native savedata size directory");
    }
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
        throw QueryFailure(Failure::Unsupported, "Reparse/symlink savedata size paths are unsupported");
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY))
        throw QueryFailure(Failure::Filesystem, "Savedata size directory component is a file");
    return true;
}
std::uint64_t add(std::uint64_t a, std::uint64_t b) {
    if (b > std::numeric_limits<std::uint64_t>::max() - a)
        throw QueryFailure(Failure::Unsupported, "Savedata size arithmetic overflow");
    return a + b;
}
std::uint64_t clusters(std::uint64_t size, std::uint32_t cluster) {
    return size / cluster + (size % cluster != 0u);
}
std::uint32_t guest_int(std::uint64_t value) {
    if (value > 0x7FFFFFFFu)
        throw QueryFailure(Failure::Unsupported, "Native volume/save size exceeds signed32 PSP output range");
    return static_cast<std::uint32_t>(value);
}
void word(std::vector<std::uint8_t> &out, unsigned at, std::uint32_t value) {
    for (unsigned i = 0; i < 4u; ++i) out[at + i] = static_cast<std::uint8_t>(value >> (8u * i));
}
void size_string(std::vector<std::uint8_t> &out, unsigned at, std::uint64_t amount, bool round_up) {
    constexpr std::array units{"B", "KB", "MB", "GB", "TB", "PB", "EB"};
    unsigned unit{};
    while (amount >= 1024u && unit + 1u < units.size()) {
        amount = amount / 1024u + (round_up && amount % 1024u != 0u); ++unit;
    }
    const auto value = std::to_string(amount) + " " + units[unit];
    if (value.size() > 7u) throw QueryFailure(Failure::Unsupported, "Savedata size string exceeds eight-byte guest field");
    std::fill_n(out.begin() + at, 8u, 0u); std::copy(value.begin(), value.end(), out.begin() + at);
}
std::vector<std::uint8_t> used(std::uint64_t count, std::uint32_t cluster) {
    if (count > std::numeric_limits<std::uint64_t>::max() / cluster)
        throw QueryFailure(Failure::Unsupported, "Savedata allocated byte arithmetic overflow");
    const auto size = count * cluster;
    std::vector<std::uint8_t> out(28u);
    word(out, 0u, guest_int(count)); word(out, 4u, guest_int(size / 1024u)); size_string(out, 8u, size, true);
    word(out, 16u, guest_int(size / 1024u)); size_string(out, 20u, size, true);
    return out;
}
void output(Plan &plan, const psprecomp::GuestMemory &m, std::uint32_t address,
    std::vector<std::uint8_t> value, std::uint32_t params, unsigned params_size) {
    need_span(m, address, value.size());
    if (intersects(address, value.size(), params, params_size))
        throw QueryFailure(Failure::InvalidAddress, "Savedata size output overlaps request");
    for (const auto &other : plan.writes)
        if (intersects(address, value.size(), other.address, other.bytes.size()))
            throw QueryFailure(Failure::InvalidAddress, "Savedata size outputs overlap");
    plan.writes.push_back({address, std::move(value)});
}
std::optional<std::filesystem::path> save_path(const std::filesystem::path &root, const std::string &game, const std::string &save) {
    valid_name(game, true); valid_name(save, false);
    if (!directory(root)) throw QueryFailure(Failure::Filesystem, "Native MemoryStick root is not mounted");
    const auto psp = root / "PSP", saves = psp / "SAVEDATA";
    if (!directory(psp) || !directory(saves)) return std::nullopt;
    const auto path = saves / (game + save);
    if (!directory(path)) return std::nullopt;
    return path;
}
}

namespace { std::optional<Volume> fixed_volume; }
void set_fixed_volume(const std::optional<Volume> &volume) { fixed_volume = volume; }

Volume native_volume(const std::filesystem::path &root) {
    if (!directory(root)) throw QueryFailure(Failure::Filesystem, "Native MemoryStick root is not mounted");
    if (fixed_volume) return *fixed_volume;
    ULARGE_INTEGER available{}, total{}, free{};
    if (!GetDiskFreeSpaceExW(root.c_str(), &available, &total, &free))
        throw QueryFailure(Failure::Filesystem, "Native host volume space query failed");
    std::array<wchar_t, 32768> volume_path{};
    if (!GetVolumePathNameW(std::filesystem::absolute(root).c_str(), volume_path.data(), static_cast<DWORD>(volume_path.size())))
        throw QueryFailure(Failure::Filesystem, "Cannot identify native savedata volume");
    DWORD sectors{}, bytes{}, free_clusters{}, total_clusters{};
    if (!GetDiskFreeSpaceW(volume_path.data(), &sectors, &bytes, &free_clusters, &total_clusters) || !sectors || !bytes)
        throw QueryFailure(Failure::Filesystem, "Cannot query native filesystem allocation geometry");
    return {available.QuadPart, total.QuadPart, free.QuadPart, std::uint64_t(sectors) * bytes};
}

Usage native_usage(const std::filesystem::path &path, std::uint32_t cluster) {
    if (cluster != 32768u)
        throw QueryFailure(Failure::Unsupported, "Only hardware-tested 32768-byte MemoryStick geometry is prepared");
    if (!directory(path)) throw QueryFailure(Failure::Unsupported, "Requested save directory is absent");
    Usage result{}; result.logical_clusters = 1u; // logical directory record, not native host allocation
    try {
        for (const auto &item : std::filesystem::directory_iterator(path)) {
            const auto attrs = GetFileAttributesW(item.path().c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES) throw QueryFailure(Failure::Filesystem, "Cannot inspect savedata size file");
            if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
                throw QueryFailure(Failure::Unsupported, "Savedata size reparse files are unsupported");
            if (attrs & FILE_ATTRIBUTE_DIRECTORY)
                throw QueryFailure(Failure::Unsupported, "Nested savedata directory accounting needs verified semantics");
            const auto handle = CreateFileW(item.path().c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (handle == INVALID_HANDLE_VALUE) throw QueryFailure(Failure::Filesystem, "Cannot inspect savedata file allocation");
            FILE_STANDARD_INFO info{};
            const bool ok = GetFileInformationByHandleEx(handle, FileStandardInfo, &info, sizeof(info)) != 0;
            CloseHandle(handle);
            if (!ok || info.Directory || info.DeletePending || info.EndOfFile.QuadPart < 0 || info.AllocationSize.QuadPart < 0)
                throw QueryFailure(Failure::Filesystem, "Native savedata file size/allocation is unavailable");
            result.file_bytes = add(result.file_bytes, info.EndOfFile.QuadPart);
            result.host_file_allocation_bytes = add(result.host_file_allocation_bytes, info.AllocationSize.QuadPart);
            result.logical_clusters = add(result.logical_clusters, clusters(info.EndOfFile.QuadPart, cluster));
            if (result.file_count == 0xFFFFFFFFu) throw QueryFailure(Failure::Unsupported, "Savedata file count overflow");
            ++result.file_count;
        }
    } catch (const std::filesystem::filesystem_error &e) { throw QueryFailure(Failure::Filesystem, e.what()); }
    return result;
}

void Plan::commit(psprecomp::GuestMemory &memory) const {
    for (const auto &input : preconditions)
        if (bytes(memory, input.address, input.bytes.size()) != input.bytes)
            throw QueryFailure(Failure::StaleRequest, "Savedata size request changed before atomic commit");
    for (const auto &write : writes) need_span(memory, write.address, write.bytes.size());
    for (const auto &write : writes) memory.copy_in(write.address, write.bytes);
}

Plan prepare(const psprecomp::GuestMemory &memory, std::uint32_t params, const std::filesystem::path &root,
    const Policy &policy) {
    need_span(memory, params, 4u); const auto size = memory.load32(params);
    if (size != 0x5DCu && size != 0x600u)
        throw QueryFailure(Failure::InvalidParamSize, "Savedata size fields require supported complete parameter layout");
    need_span(memory, params, size);
    if (policy.memory_stick_cluster_bytes != 32768u)
        throw QueryFailure(Failure::Unsupported, "Only hardware-tested MemoryStick geometry is prepared");
    const auto mode = memory.load32(params + 48u);
    if (mode != 8u && mode != 22u) throw QueryFailure(Failure::Unsupported, "Not a prepared savedata size mode");
    if (mode == 22u && size != 0x600u)
        throw QueryFailure(Failure::InvalidParamSize, "GETSIZE requires its complete guest32 sizeInfo pointer");
    Plan plan; std::uint32_t common_result{};
    plan.preconditions.push_back({params, bytes(memory, params, size)});
    plan.volume = native_volume(root);
    const auto cluster = policy.memory_stick_cluster_bytes;
    const auto free_fields = [&] {
        std::vector<std::uint8_t> out(20u);
        word(out, 0u, cluster); word(out, 4u, guest_int(plan.volume.available_bytes / cluster));
        word(out, 8u, guest_int(plan.volume.available_bytes / 1024u));
        size_string(out, 12u, plan.volume.available_bytes, false);
        return out;
    };
    if (mode == 8u) {
        const auto free_info = memory.load32(params + 1488u), ms_info = memory.load32(params + 1492u),
            utility_info = memory.load32(params + 1496u);
        const std::array containers{std::pair{free_info, 20u}, std::pair{ms_info, 64u}, std::pair{utility_info, 28u}};
        for (unsigned i = 0; i < containers.size(); ++i) {
            const auto [address, length] = containers[i]; if (!address) continue;
            need_span(memory, address, length);
            if (intersects(address, length, params, size))
                throw QueryFailure(Failure::InvalidAddress, "Savedata size structure overlaps request");
            for (unsigned j = 0; j < i; ++j)
                if (containers[j].first && intersects(address, length, containers[j].first, containers[j].second))
                    throw QueryFailure(Failure::InvalidAddress, "Savedata size structures overlap");
        }
        if (free_info) {
            output(plan, memory, free_info, free_fields(), params, size);
        }
        if (ms_info) {
            auto input = bytes(memory, ms_info, 64u);
            plan.preconditions.push_back({ms_info, input});
            const auto game = string_at(memory, ms_info, 13u), save = string_at(memory, ms_info + 16u, 20u);
            const auto path = save_path(root, game, save);
            if (path) {
                plan.existing_usage = native_usage(*path, cluster);
                output(plan, memory, ms_info + 36u, used(plan.existing_usage->logical_clusters, cluster), params, size);
            } else {
                if (!policy.reference_missing_save_errors ||
                    !std::all_of(input.begin() + 36u, input.end(), [](auto byte) { return byte == 0u; }))
                    throw QueryFailure(Failure::Unsupported, "Missing SIZES save output semantics require initially-zero outputs and explicit reference policy");
                // The two references agree on SIZES_NO_DATA and their differing
                // clear/leave outputs are equivalent for this verified state.
                common_result = 0x801103C7u;
            }
        }
        if (utility_info) {
            std::uint64_t count = 2u; // directory record plus PARAM.SFO: hardware sizes fixture
            const auto filename = string_at(memory, params + 100u, 13u);
            if (!filename.empty()) {
                valid_name(filename, true);
                if (!policy.requested_data_header_bytes)
                    throw QueryFailure(Failure::Unsupported, "Requested data-file secure overhead is not established for this profile");
                if (*policy.requested_data_header_bytes != 0u && *policy.requested_data_header_bytes != 16u)
                    throw QueryFailure(Failure::Unsupported, "Requested secure overhead policy is outside researched cases");
                count = add(count, clusters(add(memory.load32(params + 124u), *policy.requested_data_header_bytes), cluster));
            }
            for (const auto offset : {1420u, 1436u, 1452u, 1468u}) count = add(count, clusters(memory.load32(params + offset), cluster));
            output(plan, memory, utility_info, used(count, cluster), params, size);
        }
    } else {
        const auto info = memory.load32(params + 1532u); const auto input = bytes(memory, info, 60u);
        if (intersects(info, 60u, params, size))
            throw QueryFailure(Failure::InvalidAddress, "GETSIZE structure overlaps request");
        if (memory.load32(info) || memory.load32(info + 4u))
            throw QueryFailure(Failure::Unsupported, "GETSIZE nonzero normal/secure-entry deficit and overwrite semantics lack hardware vectors");
        const auto path = save_path(root, string_at(memory, params + 60u, 13u), string_at(memory, params + 76u, 20u));
        if (!path) {
            if (!policy.reference_missing_save_errors || memory.load32(info + 48u) != 0u)
                throw QueryFailure(Failure::Unsupported, "Missing GETSIZE save requires initially-zero overwriteKB and explicit reference policy");
            common_result = 0x80110327u;
        }
        plan.preconditions.push_back({info, input});
        output(plan, memory, info + 16u, free_fields(), params, size);
        output(plan, memory, info + 36u, std::vector<std::uint8_t>(4u), params, size);
        output(plan, memory, info + 48u, std::vector<std::uint8_t>(4u), params, size);
        // Hardware getsize.expected leaves neededString/overwriteString untouched
        // when no additional space is needed. Input counts/pointers also survive.
    }
    // common.result belongs to the request, so append it after overlap-checked
    // external output fields rather than passing it to output().
    std::vector<std::uint8_t> result(4u); word(result, 0u, common_result);
    plan.writes.push_back({params + 28u, std::move(result)});
    return plan;
}
} // namespace chili::savedata_size_prep
