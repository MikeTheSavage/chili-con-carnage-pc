#pragma once

#include "psprecomp/runtime.hpp"
#include "utility_dialog_owner.hpp"
#include "savedata_backend.hpp"
#include <filesystem>

namespace chili {
class ThreadRuntime;

// Native LIST/SIZES plus genuine slot selection and native backend save, load
// and delete. A user or sampled device edge supplies interactive choices.
// An automatic save (mode 1) replaces the save in its slot without a question,
// as a PSP does; a list save (mode 5) replaces one after the player's answer in
// the dialog; the request's word at offset 56 decides neither. A mode this port
// does not implement never stops the runtime: it runs through the utility's
// life cycle and ends with the result of a cancelled dialog, with one log line.
// memory_stick_root is the same ms0 root passed to FileRuntime.
class SavedataRuntime {
public:
    explicit SavedataRuntime(psprecomp::Runtime &runtime,
        std::filesystem::path memory_stick_root, ThreadRuntime *threads = nullptr,
        UtilityDialogOwner *owner = nullptr, SaveSlotPresenter *presenter = nullptr,
        SavedataBackend *backend = nullptr);
    ~SavedataRuntime();
    SavedataRuntime(const SavedataRuntime &) = delete;
    SavedataRuntime &operator=(const SavedataRuntime &) = delete;

    // The host post-dispatch pump calls this even when the guest does not poll
    // SavedataGetStatus. It changes only lifecycle/ownership after deadlines.
    void tick() { refresh_status(); }

    static constexpr std::uint32_t param_size = 1536u;
    static constexpr std::uint32_t id_entry_size = 72u;
    static constexpr std::uint32_t list_mode = 11u;
    static constexpr std::uint32_t sizes_mode = 8u;
    static constexpr std::uint64_t init_delay_us = 200000u;
    static constexpr std::uint64_t shutdown_delay_us = 2000u;

private:
    enum class Status : std::uint32_t { None, Init, Running, Finished, Shutdown };
    std::uint64_t now_us() const;
    void refresh_status();
    void init_start(psprecomp::AllegrexContext &);
    void update(psprecomp::AllegrexContext &);
    void get_status(psprecomp::AllegrexContext &);
    void shutdown_start(psprecomp::AllegrexContext &);
    bool enumerate(psprecomp::AllegrexContext &);
    bool query_sizes(psprecomp::AllegrexContext &);
    bool begin_operation(std::uint32_t,psprecomp::AllegrexContext &);
    bool begin_declined(std::uint32_t params,std::uint32_t mode,const std::string &why,psprecomp::AllegrexContext &);
    bool decline();
    bool operate(psprecomp::AllegrexContext &);
    void close_presenter() noexcept;
    void commit_loaded(const SavedataLoadedProfile &,const std::string &);

    psprecomp::Runtime &runtime_;
    std::filesystem::path memory_stick_root_;
    ThreadRuntime *threads_{};
    UtilityDialogOwner private_owner_;
    UtilityDialogOwner *owner_{};
    Status status_{Status::None};
    bool owns_slot_{};
    std::uint64_t deadline_{};
    std::uint32_t params_{}, id_list_{}, mode_{};
    std::string game_name_, save_pattern_;
    SaveSlotPresenter *presenter_{};
    SavedataBackend *backend_{};
    save_request::Request operation_;
    SaveSlotRequest slot_request_;
    std::uint64_t next_generation_{};
    std::uint32_t operation_error_{};
    bool opened_{};
    // The pending request is one this port does not carry out: Update ends it as cancelled.
    bool declined_{};
};
} // namespace chili
