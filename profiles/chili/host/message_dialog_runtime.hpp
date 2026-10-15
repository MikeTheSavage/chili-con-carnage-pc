#pragma once

#include "psprecomp/runtime.hpp"
#include "utility_dialog_owner.hpp"
#include <functional>
#include <optional>
#include <string>

namespace chili {

enum class MessageDialogChoice { Yes, No, Okay, Back };
struct MessageDialogRequest {
    std::uint64_t generation{};
    std::uint32_t size{}, mode{}, error_value{}, options{};
    std::uint32_t language{}, button_swap{};
    std::string text, okay_label, back_label;
    bool yes_no{}, okay{}, allow_back{}, default_no{}, suppress_sound{}, text_sound{};
};
struct MessageDialogSelection {
    std::uint64_t generation{};
    MessageDialogChoice choice{};
};

// A presenter must only return selections caused by actual host input. Opening
// a window, selecting initial focus, or destroying a window is not a selection.
// Call all methods on the guest execution / native UI thread. A null presenter
// stops a valid running dialog explicitly rather than fabricating a response.
class MessageDialogPresenter {
public:
    virtual ~MessageDialogPresenter() = default;
    virtual void open(const MessageDialogRequest &) = 0;
    virtual std::optional<MessageDialogSelection> poll(std::uint64_t generation) = 0;
    virtual void close(std::uint64_t generation) noexcept = 0;
};

class MessageDialogRuntime {
public:
    using Clock = std::function<std::uint64_t()>;
    explicit MessageDialogRuntime(psprecomp::Runtime &, MessageDialogPresenter *presenter = nullptr,
        Clock clock = {}, UtilityDialogOwner *owner = nullptr);
    ~MessageDialogRuntime();
    MessageDialogRuntime(const MessageDialogRuntime &) = delete;
    MessageDialogRuntime &operator=(const MessageDialogRuntime &) = delete;

    // Refreshes timed status transitions and releases the shared owner after
    // teardown. This never writes guest memory, schedules, polls input or chooses.
    void tick();

    // Approximate native lifecycle delays, informed by PSP workshare.expected.
    // They do not reproduce PSP utility worker scheduling, fade or audio timing.
    static constexpr std::uint64_t init_delay_us = 650000u;
    static constexpr std::uint64_t shutdown_delay_us = 30000u;
    static constexpr std::uint32_t init_nid = 0x2AD8E239u;
    static constexpr std::uint32_t shutdown_nid = 0x67AF3428u;
    static constexpr std::uint32_t update_nid = 0x95FC253Bu;
    static constexpr std::uint32_t status_nid = 0x9A1C91D7u;
private:
    enum class Status : std::uint32_t { None, Init, Running, Finished, Shutdown };
    std::uint64_t now_us() const;
    void init_start(psprecomp::AllegrexContext &);
    void update(psprecomp::AllegrexContext &);
    void get_status(psprecomp::AllegrexContext &);
    void shutdown_start(psprecomp::AllegrexContext &);
    bool valid_output_span() const;
    void close_presenter() noexcept;

    psprecomp::Runtime &runtime_;
    MessageDialogPresenter *presenter_{};
    Clock clock_;
    UtilityDialogOwner private_owner_;
    UtilityDialogOwner *owner_{};
    Status status_{Status::None};
    bool owns_slot_{}, opened_{};
    std::uint64_t deadline_{}, next_generation_{};
    std::uint32_t params_{}, request_error_{};
    MessageDialogRequest request_;
};
} // namespace chili
