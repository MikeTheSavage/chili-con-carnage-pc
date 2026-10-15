#pragma once
#include "psprecomp/guest_memory.hpp"
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <vector>

namespace chili::savedata_size_prep {
enum class Failure { InvalidAddress, InvalidParamSize, Unsupported, Filesystem, StaleRequest };
struct QueryFailure : std::runtime_error {
    Failure kind;
    QueryFailure(Failure type, const std::string &message) : std::runtime_error(message), kind(type) {}
};
struct Volume {
    std::uint64_t available_bytes{}, caller_capacity_bytes{}, total_free_bytes{}, host_cluster_bytes{};
};
struct Usage {
    std::uint64_t file_bytes{}, host_file_allocation_bytes{}, logical_clusters{};
    std::uint32_t file_count{};
};
struct Policy {
    // Logical MemoryStick geometry, independent of native NTFS allocation.
    std::uint32_t memory_stick_cluster_bytes{32768u};
    // Intentionally unknown by default. A profile must establish its actual
    // requested data-file overhead before utilityData can estimate that file.
    std::optional<std::uint32_t> requested_data_header_bytes;
    // Reference-only opt-in pending the profile's actual captured request.
    // Missing saves are admitted only when contested output fields start at
    // zero, making the researched references observationally agree.
    bool reference_missing_save_errors{};
};
struct Write { std::uint32_t address; std::vector<std::uint8_t> bytes; };
struct Plan {
    Volume volume;
    std::optional<Usage> existing_usage;
    std::vector<Write> writes, preconditions;
    // Check every input snapshot and output span before applying any output.
    void commit(psprecomp::GuestMemory &) const;
};
Volume native_volume(const std::filesystem::path &memory_stick_root);
// Deterministic runs report this volume instead of the host disk's free space.
void set_fixed_volume(const std::optional<Volume> &volume);
Usage native_usage(const std::filesystem::path &save_directory, std::uint32_t logical_cluster_bytes = 32768u);
// Supported preparation: SIZES8 free/real existing data/explicit estimate policy;
// GETSIZE22 zero-entry existing-directory query. Contested secure/nonzero-entry,
// missing-save output semantics and unknown secure estimates throw explicitly
// by default; the narrow reference policy above cannot overwrite unknown state.
// No HLE registrations or lifecycle changes; no actual save filesystem writes.
Plan prepare(const psprecomp::GuestMemory &, std::uint32_t params,
    const std::filesystem::path &memory_stick_root, const Policy & = {});
} // namespace chili::savedata_size_prep
