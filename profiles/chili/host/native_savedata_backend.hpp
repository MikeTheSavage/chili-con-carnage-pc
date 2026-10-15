#pragma once
#include "savedata_backend.hpp"
#include "savedata_storage.hpp"

#include <functional>
#include <optional>
#include <set>

namespace chili {

// A recovery failure retains the exact transaction/backup/staging paths.
class NativeSavedataRecoveryError final : public SavedataBackendError {
public:
    explicit NativeSavedataRecoveryError(const savedata_storage::CommitResult&);
    const savedata_storage::CommitResult& recovery() const noexcept { return recovery_; }
private:
    savedata_storage::CommitResult recovery_;
};

class NativeSavedataBackend final : public SavedataBackend {
public:
    using DiagnosticSink = std::function<void(const savedata_storage::CommitResult&)>;
    // One line of the log, without its "[savedata] " prefix. Default: std::cout.
    using LogSink = std::function<void(const std::string&)>;
    // Construction validates an already existing ms0 root without writing it.
    NativeSavedataBackend(psprecomp::GuestMemory&, std::filesystem::path ms0,
                          DiagnosticSink diagnostic = {}, savedata_storage::Limits limits = {},
                          std::shared_ptr<std::recursive_mutex> serialization = {}, LogSink log = {});
    std::vector<SavedataSlotInfo> inspect(const save_request::Request&) override;
    // A save that is already in the slot is replaced when replace_existing is set,
    // whatever the request's word at offset 56 holds. The replaced generation goes
    // to <ms0>/PSP/SAVEDATA_PREVIOUS/<game+slot>; if it cannot be kept the save is
    // committed all the same and the report says why.
    SavedataSaveReport save(const save_request::Request&, const std::string& slot, bool replace_existing) override;
    SavedataLoadedProfile load(const save_request::Request&, const std::string& slot) override;
    std::string remove(const save_request::Request&, const std::string& slot) override;
    std::string previous_place() const override;
    std::optional<savedata_storage::CommitResult> last_commit_result() const;
    std::shared_ptr<std::recursive_mutex> serialization() const { return store_.serialization(); }

#ifdef CHILI_NATIVE_SAVEDATA_BACKEND_TESTING
    // Fixture-only seam. Production builds must not define this macro.
    void set_fault_hook_for_test(savedata_storage::FaultHook);
    static std::uint32_t storage_result_for_test(savedata_storage::ErrorCode,
                                                std::uint32_t native_error, bool save);
#endif
private:
    psprecomp::GuestMemory& memory_;
    savedata_storage::Store store_;
    DiagnosticSink diagnostic_;
    std::optional<savedata_storage::CommitResult> last_commit_;
#ifdef CHILI_NATIVE_SAVEDATA_BACKEND_TESTING
    savedata_storage::FaultHook test_fault_;
#endif
    LogSink log_;
    std::set<std::filesystem::path> left_alone_;
    void report_committed_cleanup() noexcept;
    // Finishes what an interrupted save left behind (Store::sweep) and logs it.
    void sweep() noexcept;
    void log(const std::string&) noexcept;
};
} // namespace chili
