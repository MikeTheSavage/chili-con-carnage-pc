#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace chili::savedata_storage {
using Bytes=std::vector<std::uint8_t>;
struct Slot {std::string game,save;};
struct File {std::string name;Bytes bytes;};
struct Limits {std::uint64_t file_bytes=32u*1024u*1024u,slot_bytes=64u*1024u*1024u;std::size_t files=16u,directory_entries=4096u;};
enum class ErrorCode {InvalidName,InvalidRoot,UnsafePath,NotFound,AlreadyExists,LimitExceeded,ConcurrentChange,NativeIo,InjectedFailure};
class Error:public std::runtime_error {
public:
    Error(ErrorCode code,std::string message,std::uint32_t native_error=0u):std::runtime_error(std::move(message)),code_(code),native_(native_error){}
    ErrorCode code()const noexcept{return code_;}
    std::uint32_t native_error()const noexcept{return native_;}
private:ErrorCode code_;std::uint32_t native_;
};
struct Failure {ErrorCode code;std::uint32_t native_error;std::string message;};
struct FileInfo {
    std::string name;
    std::uint64_t bytes{},allocated_bytes{},created_filetime{},accessed_filetime{},written_filetime{};
};
struct Metadata {bool exists{};std::vector<FileInfo> files;std::uint64_t bytes{};};
enum class Overwrite {Reject,ReplaceWithRollback};
enum class Outcome {Committed,FailedUnchanged,RecoveryRequired,CommittedCleanupRequired};
enum class Phase {StageCreated,FileStaged,StageComplete,OldMoved,Published,BeforeCleanup,BeforeRestore,
    // After the commit point of a replacement, before the generation it replaced is put into the kept place.
    BeforeKeep};
// Optional deterministic fault seam for standalone tests. Production callers
// normally omit it. Callback exceptions are handled as real failed operations.
using FaultHook=std::function<void(Phase)>;
struct CommitResult {
    Outcome outcome{Outcome::FailedUnchanged};
    std::optional<Failure> failure,rollback_failure;
    std::filesystem::path recovery_directory,backup_directory,staged_directory;
    // A committed replacement: the slot held a save before. That generation is
    // kept in previous_directory; when it could not be kept the save is committed
    // all the same and previous_failure says why.
    bool replaced{};
    std::filesystem::path previous_directory;
    std::optional<Failure> previous_failure;
};
// The one earlier generation of every slot lives in <ms0>/PSP/SAVEDATA_PREVIOUS/<game+save>:
// beside SAVEDATA, on the same volume, and outside the folder the game lists as save slots.
inline constexpr const char *previous_directory_name="SAVEDATA_PREVIOUS";
// What Store::sweep() did with one leftover transaction directory.
enum class SweepAction {Removed,RestoredSlot,KeptPrevious,RemovedPrevious,LeftAlone};
struct SweepReport {
    std::filesystem::path transaction;
    std::string slot_directory;
    SweepAction action{SweepAction::LeftAlone};
    std::filesystem::path destination;   // RestoredSlot, KeptPrevious
    std::optional<Failure> failure;      // LeftAlone, or a step that failed on the way
};
class Store {
public:
    // ms0 must already exist. Construction performs validation without writes.
    explicit Store(std::filesystem::path ms0,Limits limits={},std::shared_ptr<std::recursive_mutex> serialization={});
    const std::filesystem::path &ms0()const noexcept{return ms0_;}
    std::unique_lock<std::recursive_mutex> guard()const;
    std::shared_ptr<std::recursive_mutex> serialization()const{return serialization_;}
    Metadata query(const Slot &)const;
    Bytes read(const Slot &,const std::string &filename,std::uint64_t caller_limit)const;
    // Full slot replacement: every supplied file is already encoded/opaque.
    // Optional old files survive only if the caller includes them in this set.
    // ReplaceWithRollback keeps the replaced generation (CommitResult::previous_directory).
    CommitResult commit(const Slot &,std::span<const File>,Overwrite=Overwrite::Reject,FaultHook={});
    // Takes a slot out of SAVEDATA without erasing it: its folder becomes the kept
    // generation of that slot. Returns where it went. Throws Error; then the slot is as it was.
    std::filesystem::path retire(const Slot &);
    // Finishes what an interrupted or half-cleaned commit left in SAVEDATA: an old
    // generation whose slot is missing goes back into place, an old generation beside
    // a published slot goes to the kept place, unpublished staging is removed. Never
    // throws; a directory it cannot make sense of is left alone and reported. Directories
    // of another process that is still running are not touched.
    std::vector<SweepReport> sweep();
    std::filesystem::path previous_root()const;
    static void validate_slot(const Slot &);
    static void validate_filename(const std::string &);
private:
    std::filesystem::path ms0_;
    Limits limits_;
    std::shared_ptr<std::recursive_mutex> serialization_;
};
}
