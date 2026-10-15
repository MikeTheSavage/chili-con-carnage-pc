#include "native_savedata_backend.hpp"
#include "native_save_codec.hpp"
#include "param_sfo.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <limits>
#include <new>
#include <string_view>
#include <utility>

namespace chili {
namespace {
namespace storage = savedata_storage;
namespace codec = native_save;
constexpr std::uint32_t load_no_ms = 0x80110301u, load_access = 0x80110305u,
    load_broken = 0x80110306u, load_no_data = 0x80110307u,
    load_param = 0x80110308u, load_file_missing = 0x80110309u, load_internal = 0x8011030bu;
constexpr std::uint32_t save_no_ms = 0x80110381u, save_no_space = 0x80110383u,
    save_protected = 0x80110384u, save_access = 0x80110385u,
    save_param = 0x80110388u, save_internal = 0x8011038bu;
constexpr std::uint32_t delete_no_ms = 0x80110341u, delete_protected = 0x80110344u,
    delete_access = 0x80110345u, delete_no_data = 0x80110347u,
    delete_param = 0x80110348u, delete_internal = 0x8011034bu;
enum class Kind { Load, Save, Delete };
Kind kind_of(const save_request::Request& r) {
    return r.is_delete() ? Kind::Delete : r.is_save() ? Kind::Save : Kind::Load;
}
std::uint32_t param_error(Kind kind) { return kind == Kind::Delete ? delete_param : kind == Kind::Save ? save_param : 0x80110308u; }
std::uint32_t access_error(Kind kind) { return kind == Kind::Delete ? delete_access : kind == Kind::Save ? save_access : 0x80110305u; }
std::uint32_t internal_error(Kind kind) { return kind == Kind::Delete ? delete_internal : kind == Kind::Save ? save_internal : 0x8011030bu; }
constexpr std::size_t payload_limit = 8u * 1024u * 1024u, media_limit = 4u * 1024u * 1024u;
constexpr sfo::Limits sfo_limits{codec::max_sfo_bytes, 64};
constexpr std::array<const char*, 4> media_names{"ICON0.PNG", "ICON1.PMF", "PIC1.PNG", "SND0.AT3"};

[[noreturn]] void fail(std::uint32_t result, const char* message) {
    throw SavedataBackendError(result, message);
}

std::string folded(std::string text) {
    for (auto& c : text) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return text;
}

std::uint32_t storage_result(storage::ErrorCode code, std::uint32_t native, Kind kind) {
    using E = storage::ErrorCode;
    if (kind == Kind::Delete) {
        switch (code) {
        case E::InvalidName:
        case E::LimitExceeded: return delete_param;
        case E::InvalidRoot: return delete_no_ms;
        case E::NotFound: return delete_no_data;
        case E::InjectedFailure: return delete_internal;
        case E::NativeIo:
            if (native == 0) return delete_internal;
            return native == ERROR_WRITE_PROTECT || native == ERROR_ACCESS_DENIED ? delete_protected : delete_access;
        default: return delete_access;
        }
    }
    const bool save = kind == Kind::Save;
    switch (code) {
    case E::InvalidName: return save ? save_param : load_param;
    case E::InvalidRoot: return save ? save_no_ms : load_no_ms;
    case E::NotFound: return save ? save_access : load_file_missing;
    case E::AlreadyExists:
    case E::UnsafePath:
    case E::ConcurrentChange: return save ? save_access : load_access;
    case E::LimitExceeded: return save ? save_param : load_param;
    case E::InjectedFailure: return save ? save_internal : load_internal;
    case E::NativeIo:
        if (native == 0) return save ? save_internal : load_internal;
        if (save && (native == ERROR_DISK_FULL || native == ERROR_HANDLE_DISK_FULL ||
                     native == ERROR_DISK_QUOTA_EXCEEDED)) return save_no_space;
        if (save && (native == ERROR_WRITE_PROTECT || native == ERROR_ACCESS_DENIED)) return save_protected;
        return save ? save_access : load_access;
    }
    return save ? save_internal : load_internal;
}

[[noreturn]] void storage_failure(const storage::Error& e, Kind kind) {
    throw SavedataBackendError(storage_result(e.code(), e.native_error(), kind), e.what());
}
std::string utf8(const std::filesystem::path& p) {
    const auto bytes = p.u8string();
    return std::string(bytes.begin(), bytes.end());
}

storage::Store make_store(std::filesystem::path root, storage::Limits limits,
                         std::shared_ptr<std::recursive_mutex> serialization) {
    try { return storage::Store(std::move(root), limits, std::move(serialization)); }
    catch (const storage::Error& e) { storage_failure(e, Kind::Load); }
    catch (const std::filesystem::filesystem_error&) { fail(load_access, "Cannot access native savedata root"); }
}

void policy(const save_request::Request& r, Kind kind) {
    const auto error = param_error(kind);
    if (r.game != "ULUS10216" || r.file != "SECURE.BIN" || r.secure_version != 0)
        fail(error, "Native backend supports Chili ULUS10216/SECURE.BIN secureVersion0 only");
    if (r.mode != 0 && r.mode != 1 && r.mode != 4 && r.mode != 5 && r.mode != 7)
        fail(error, "Unsupported native savedata operation");
    // A delete moves a folder: the request's payload and media buffers play no part.
    if (kind == Kind::Delete) return;
    if (r.data.capacity == 0 || r.data.capacity > payload_limit || r.data.size > r.data.capacity)
        fail(error, "Native savedata payload capacity is invalid");
    for (const auto& b : r.media)
        if (b.capacity > media_limit || b.size > b.capacity)
            fail(error, "Native savedata media capacity is invalid");
    // Policy is intentionally stricter than the generic guest32 reader.
    // In particular, a system/media filename can never alias the payload.
    if (folded(r.file) == "PARAM.SFO" ||
        std::any_of(media_names.begin(), media_names.end(), [&](const auto* name) { return folded(r.file) == name; }))
        fail(error, "Native savedata payload collides with a managed filename");
}

void selection(const save_request::Request& r, const std::string& slot, Kind kind) {
    const auto error = param_error(kind);
    if ((r.interactive() && std::find(r.slots.begin(), r.slots.end(), slot) == r.slots.end()) ||
        (!r.interactive() && slot != r.slot))
        fail(error, "Native savedata selection is not in the requested slots");
    storage::Store::validate_slot({r.game, slot});
}

const storage::FileInfo* info(const storage::Metadata& m, const std::string& name) {
    const auto wanted = folded(name);
    const auto i = std::find_if(m.files.begin(), m.files.end(), [&](const auto& f) { return folded(f.name) == wanted; });
    return i == m.files.end() ? nullptr : &*i;
}

codec::Bytes make_sfo(const save_request::Request& r, const std::string& slot) {
    sfo::Document document;
    document.entries = {
        sfo::Entry::text("TITLE", r.title, 128, sfo_limits),
        sfo::Entry::text("SAVEDATA_TITLE", r.savedata_title, 128, sfo_limits),
        sfo::Entry::text("SAVEDATA_DETAIL", r.detail, 1024, sfo_limits),
        sfo::Entry::integer("PARENTAL_LEVEL", r.parental),
        sfo::Entry::text("SAVEDATA_DIRECTORY", r.game + slot, 32, sfo_limits),
        sfo::Entry::integer("CHILI_PC_FORMAT", 1),
        sfo::Entry::text("CHILI_PC_FILE", r.file, 13, sfo_limits)
    };
    return sfo::serialize(document, sfo_limits);
}

const sfo::Entry& required(const sfo::Document& doc, const char* key) {
    const auto* entry = doc.find(key);
    if (!entry) fail(load_broken, "Native savedata metadata is missing a required field");
    return *entry;
}

std::string native_text(const sfo::Document& doc, const char* key, std::size_t capacity) {
    const auto& entry = required(doc, key);
    if (entry.storage.size() != capacity)
        fail(load_broken, "Native savedata metadata has an unsupported text capacity");
    return std::string(entry.text_value());
}

SavedataLoadedProfile validate_sfo(std::span<const std::uint8_t> raw,
                                  const save_request::Request& r, const std::string& slot) {
    const auto doc = sfo::parse(raw, sfo_limits);
    if (doc.entries.size() != 7 || required(doc, "CHILI_PC_FORMAT").integer_value() != 1)
        fail(load_broken, "Foreign or unsupported native savedata metadata");
    if (native_text(doc, "SAVEDATA_DIRECTORY", 32) != r.game + slot ||
        native_text(doc, "CHILI_PC_FILE", 13) != r.file)
        fail(load_broken, "Native savedata directory/file identity does not match the request");
    SavedataLoadedProfile out;
    out.title = native_text(doc, "TITLE", 128);
    out.savedata_title = native_text(doc, "SAVEDATA_TITLE", 128);
    out.detail = native_text(doc, "SAVEDATA_DETAIL", 1024);
    const auto parental = required(doc, "PARENTAL_LEVEL").integer_value();
    if (parental > 255) fail(load_broken, "Native savedata parental value is invalid");
    out.parental = static_cast<std::uint8_t>(parental);
    return out;
}

std::string recovery_message(const storage::CommitResult& result) {
    auto utf8 = [](const std::filesystem::path& p) {
        const auto bytes = p.generic_u8string();
        return std::string(bytes.begin(), bytes.end());
    };
    return "Native savedata recovery required; transaction=" + utf8(result.recovery_directory) +
           "; backup=" + utf8(result.backup_directory) + "; staging=" + utf8(result.staged_directory);
}

} // namespace

NativeSavedataRecoveryError::NativeSavedataRecoveryError(const storage::CommitResult& result)
    : SavedataBackendError(save_internal, recovery_message(result)), recovery_(result) {}

NativeSavedataBackend::NativeSavedataBackend(psprecomp::GuestMemory& memory, std::filesystem::path ms0,
        DiagnosticSink diagnostic, storage::Limits limits, std::shared_ptr<std::recursive_mutex> serialization, LogSink log)
    : memory_(memory), store_(make_store(std::move(ms0), limits, std::move(serialization))),
      diagnostic_(std::move(diagnostic)), log_(std::move(log)) {}

void NativeSavedataBackend::log(const std::string& text) noexcept {
    try {
        if (log_) log_(text);
        else std::cout << "[savedata] " << text << std::endl;
    } catch (...) { /* a log line never fails an operation */ }
}

std::string NativeSavedataBackend::previous_place() const {
    return std::string("PSP\\") + storage::previous_directory_name;
}

void NativeSavedataBackend::sweep() noexcept {
    try {
        for (const auto& report : store_.sweep()) {
            const auto where = utf8(report.transaction);
            std::string text;
            switch (report.action) {
            case storage::SweepAction::Removed:
                text = "removed the work folder an unfinished save left behind: " + where; break;
            case storage::SweepAction::RestoredSlot:
                text = "an unfinished save had moved the save " + report.slot_directory +
                       " aside without putting a new one in its place: it is back in " + utf8(report.destination); break;
            case storage::SweepAction::KeptPrevious:
                text = "the save of " + report.slot_directory + " from before the last save was still in a work folder: it is now the earlier save kept in " +
                       utf8(report.destination); break;
            case storage::SweepAction::RemovedPrevious:
                text = "the save of " + report.slot_directory + " from before the last save was still in a work folder and could not be kept: removed"; break;
            case storage::SweepAction::LeftAlone:
                // Said once per run: the folder stays, and every operation looks at it again.
                if (!left_alone_.insert(report.transaction).second) continue;
                text = "a work folder of an unfinished save was left alone: " + where; break;
            }
            if (report.failure) text += " (" + report.failure->message + ")";
            log(text);
        }
    } catch (...) { /* sweeping is housekeeping: it never fails the operation it precedes */ }
}

std::optional<storage::CommitResult> NativeSavedataBackend::last_commit_result() const {
    auto guard = store_.guard();
    return last_commit_;
}

std::vector<SavedataSlotInfo> NativeSavedataBackend::inspect(const save_request::Request& r) {
    const auto kind = kind_of(r);
    try {
        policy(r, kind);
        auto guard = store_.guard();
        sweep();
        const auto names = r.interactive() ? r.slots : std::vector<std::string>{r.slot};
        if (names.empty()) fail(param_error(kind), "Native savedata slot list is empty");
        std::vector<SavedataSlotInfo> result;
        result.reserve(names.size());
        for (const auto& name : names) {
            selection(r, name, kind);
            const auto metadata = store_.query({r.game, name});
            SavedataSlotInfo item{{name, metadata.exists ? name : (r.new_title.empty() ? name : r.new_title), metadata.exists}, 0};
            for (const auto& f : metadata.files) item.written_filetime = std::max(item.written_filetime, f.written_filetime);
            if (const auto* sfo_info = info(metadata, "PARAM.SFO")) {
                item.written_filetime = sfo_info->written_filetime;
                try {
                    const auto raw = store_.read({r.game, name}, "PARAM.SFO", codec::max_sfo_bytes);
                    const auto doc = sfo::parse(raw, sfo_limits);
                    // Foreign valid SFO can supply a display title, but load
                    // separately requires every native marker and authentication.
                    for (const auto* key : {"SAVEDATA_TITLE", "TITLE"}) {
                        const auto* field = doc.find(key);
                        if (field && field->format == sfo::Format::Text && field->used_length <= 128) {
                            item.slot.display_title = std::string(field->text_value());
                            break;
                        }
                    }
                } catch (const sfo::Error&) { /* malformed metadata is an existing, unloadable slot */ }
                  catch (const storage::Error& e) {
                    if (e.code() != storage::ErrorCode::NotFound && e.code() != storage::ErrorCode::LimitExceeded) throw;
                  }
            }
            result.push_back(std::move(item));
        }
        return result;
    } catch (const storage::Error& e) { storage_failure(e, kind); }
      catch (const std::filesystem::filesystem_error&) { fail(access_error(kind), "Cannot inspect native savedata"); }
      catch (const std::bad_alloc&) { fail(internal_error(kind), "Native savedata inspection allocation failed"); }
      catch (const SavedataBackendError&) { throw; }
      catch (const std::exception& e) { fail(internal_error(kind), e.what()); }
}

void NativeSavedataBackend::report_committed_cleanup() noexcept {
    try {
        if (diagnostic_) diagnostic_(*last_commit_);
        else std::clog << "Native savedata committed; cleanup required; transaction=" << last_commit_->recovery_directory
                       << "; backup=" << last_commit_->backup_directory << "; staging=" << last_commit_->staged_directory << '\n';
    } catch (...) {
        std::fputs("Native savedata committed; cleanup/diagnostic requires attention; retained paths are in last_commit_result.\n", stderr);
    }
}

SavedataSaveReport NativeSavedataBackend::save(const save_request::Request& r, const std::string& slot, bool replace_existing) {
    try {
        policy(r, Kind::Save);
        if (!r.is_save()) fail(save_param, "Native save requires a save request");
        selection(r, slot, Kind::Save);
        // The request's word at offset 56 is not asked: on a PSP it is no permission
        // to write over data (the game's automatic save leaves it 0 and a PSP
        // replaces the save all the same). The caller's replace_existing decides.
        auto guard = store_.guard();
        sweep();
        last_commit_.reset();
        const auto metadata = store_.query({r.game, slot});
        const auto raw_sfo = make_sfo(r, slot);
        auto plaintext = save_request::bytes(memory_, r.data);
        struct Wipe {
            codec::Bytes& bytes;
            ~Wipe() { if (!bytes.empty()) SecureZeroMemory(bytes.data(), bytes.size()); }
        } wipe{plaintext};
        std::vector<storage::File> files;
        files.push_back({"PARAM.SFO", raw_sfo});
        files.push_back({r.file, codec::encode(plaintext, raw_sfo, r.key)});
        for (std::size_t i = 0; i < media_names.size(); ++i)
            if (r.media[i].size) files.push_back({media_names[i], save_request::bytes(memory_, r.media[i])});
        const auto overwrite_policy = replace_existing && metadata.exists ? storage::Overwrite::ReplaceWithRollback : storage::Overwrite::Reject;
#ifdef CHILI_NATIVE_SAVEDATA_BACKEND_TESTING
        auto commit = store_.commit({r.game, slot}, files, overwrite_policy, test_fault_);
#else
        auto commit = store_.commit({r.game, slot}, files, overwrite_policy);
#endif
        last_commit_.emplace(std::move(commit));
        SavedataSaveReport report;
        report.replaced = last_commit_->replaced;
        if (!last_commit_->previous_directory.empty()) report.previous = utf8(last_commit_->previous_directory);
        if (last_commit_->previous_failure)
            report.note = "the save before it could not be put in the kept place now (" + last_commit_->previous_failure->message + ")";
        switch (last_commit_->outcome) {
        case storage::Outcome::Committed: return report;
        case storage::Outcome::CommittedCleanupRequired:
            // The save itself is committed. What is left is taken up by sweep() at the next operation.
            report_committed_cleanup();
            if (!report.note.empty()) report.note += "; ";
            if (!last_commit_->backup_directory.empty())
                report.note += "its files are still in " + utf8(last_commit_->backup_directory) + " and are taken up at the next save or load";
            else
                report.note += "a work folder was left in " + utf8(last_commit_->recovery_directory) + " and is removed at the next save or load";
            return report;
        case storage::Outcome::RecoveryRequired: throw NativeSavedataRecoveryError(*last_commit_);
        case storage::Outcome::FailedUnchanged:
            if (const auto& failure = last_commit_->failure; failure)
                throw SavedataBackendError(storage_result(failure->code, failure->native_error, Kind::Save), failure->message);
            fail(save_internal, "Native save failed without a commit diagnostic");
        }
        fail(save_internal, "Unknown native save commit outcome");
    } catch (const storage::Error& e) { storage_failure(e, Kind::Save); }
      catch (const sfo::Error&) { fail(save_param, "Native save metadata cannot be encoded"); }
      catch (const codec::CodecError&) { fail(save_internal, "Native save encryption failed"); }
      catch (const save_request::Error&) { fail(save_param, "Native save guest source is invalid"); }
      catch (const std::filesystem::filesystem_error&) { fail(save_access, "Native save filesystem access failed"); }
      catch (const std::bad_alloc&) { fail(save_internal, "Native save allocation failed"); }
      catch (const SavedataBackendError&) { throw; }
      catch (const std::exception& e) { fail(save_internal, e.what()); }
}

std::string NativeSavedataBackend::remove(const save_request::Request& r, const std::string& slot) {
    try {
        policy(r, Kind::Delete);
        if (!r.is_delete()) fail(delete_param, "Native delete requires a delete request");
        selection(r, slot, Kind::Delete);
        auto guard = store_.guard();
        sweep();
        // Not erased: the folder becomes the kept earlier save of its slot.
        return utf8(store_.retire({r.game, slot}));
    } catch (const storage::Error& e) { storage_failure(e, Kind::Delete); }
      catch (const std::filesystem::filesystem_error&) { fail(delete_access, "Native delete filesystem access failed"); }
      catch (const std::bad_alloc&) { fail(delete_internal, "Native delete allocation failed"); }
      catch (const SavedataBackendError&) { throw; }
      catch (const std::exception& e) { fail(delete_internal, e.what()); }
}

SavedataLoadedProfile NativeSavedataBackend::load(const save_request::Request& r, const std::string& slot) {
    try {
        policy(r, Kind::Load);
        if (r.is_save() || r.is_delete()) fail(load_param, "Native load requires a load request");
        selection(r, slot, Kind::Load);
        auto guard = store_.guard();
        sweep();
        const auto metadata = store_.query({r.game, slot});
        if (!metadata.exists) fail(load_no_data, "Native savedata slot is absent");
        const auto* sfo_info = info(metadata, "PARAM.SFO");
        if (!sfo_info) fail(load_broken, "Native savedata metadata is absent");
        if (sfo_info->bytes > codec::max_sfo_bytes) fail(load_broken, "Native savedata metadata exceeds the format bound");
        const auto raw_sfo = store_.read({r.game, slot}, "PARAM.SFO", codec::max_sfo_bytes);
        auto result = validate_sfo(raw_sfo, r, slot);
        const auto* container_info = info(metadata, r.file);
        if (!container_info) fail(load_file_missing, "Native savedata payload file is absent");
        const auto container_cap = std::uint64_t(r.data.capacity) + codec::header_bytes + codec::tag_bytes;
        if (container_info->bytes > container_cap)
            fail(load_param, "Native savedata exceeds the guest payload capacity");
        const auto container = store_.read({r.game, slot}, r.file, container_cap);
        // Decode entirely into host staging. Any later failure also wipes it.
        auto plaintext = codec::decode(container, raw_sfo, r.key);
        struct Wipe {
            codec::Bytes& bytes;
            ~Wipe() { if (!bytes.empty()) SecureZeroMemory(bytes.data(), bytes.size()); }
        } wipe{plaintext};
        if (plaintext.size() > r.data.capacity) fail(load_param, "Native plaintext exceeds guest capacity");
        for (std::size_t i = 0; i < media_names.size(); ++i) {
            if (r.media[i].capacity == 0) continue;
            if (const auto* f = info(metadata, media_names[i])) {
                if (f->bytes > r.media[i].capacity) fail(load_param, "Native media exceeds guest capacity");
                result.media[i] = store_.read({r.game, slot}, media_names[i], r.media[i].capacity);
            }
        }
        result.payload = std::exchange(plaintext, {});
        return result;
    } catch (const storage::Error& e) { storage_failure(e, Kind::Load); }
      catch (const sfo::Error&) { fail(load_broken, "Native savedata metadata is malformed"); }
      catch (const codec::CodecError& e) {
        if (e.code() == codec::ErrorCode::CryptoFailure) fail(load_internal, "Native save decryption primitive failed");
        fail(load_broken, "Native savedata format or authentication is invalid");
      }
      catch (const std::filesystem::filesystem_error&) { fail(load_access, "Native load filesystem access failed"); }
      catch (const std::bad_alloc&) { fail(load_internal, "Native load allocation failed"); }
      catch (const SavedataBackendError&) { throw; }
      catch (const std::exception& e) { fail(load_internal, e.what()); }
}

#ifdef CHILI_NATIVE_SAVEDATA_BACKEND_TESTING
void NativeSavedataBackend::set_fault_hook_for_test(storage::FaultHook hook) {
    auto guard = store_.guard();
    test_fault_ = std::move(hook);
}
std::uint32_t NativeSavedataBackend::storage_result_for_test(storage::ErrorCode code, std::uint32_t native, bool saving) {
    return storage_result(code, native, saving ? Kind::Save : Kind::Load);
}
#endif
} // namespace chili
