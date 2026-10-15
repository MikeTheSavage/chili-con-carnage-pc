#include "message_dialog_runtime.hpp"
#include <chrono>
#include <limits>
#include <sstream>

namespace chili {
namespace {
constexpr std::uint32_t invalid_status = 0x80110001u;
constexpr std::uint32_t invalid_address = 0x80110002u;
constexpr std::uint32_t invalid_param_size = 0x80110004u;
constexpr std::uint32_t wrong_type = 0x80110005u;
constexpr std::uint32_t bad_option = 0x80110501u;
constexpr std::uint32_t invalid_error_code = 0x80110502u;
constexpr std::uint32_t text_sound = 1u, no_sound = 2u, yes_no = 0x10u,
    okay = 0x20u, no_cancel = 0x80u, default_no = 0x100u;
constexpr std::uint32_t size_v1 = 572u, size_v2 = 580u, size_v3 = 708u;

bool guest_span(const psprecomp::GuestMemory &memory, std::uint32_t address, std::uint32_t bytes) {
    // Kernel aliases are not userspace utility requests. Alignment firmware
    // error fidelity is unverified; refuse misalignment as invalid address.
    return address && !(address & 0x80000003u) && memory.contains(address, bytes);
}
std::string fixed_string(const psprecomp::GuestMemory &memory, std::uint32_t address, unsigned bytes) {
    std::string value;
    for (unsigned i = 0; i < bytes; ++i) {
        const auto c = memory.load8(address + i);
        if (!c) break;
        value.push_back(static_cast<char>(c));
    }
    return value;
}
std::string error_text(std::uint32_t code) {
    std::ostringstream text;
    text << "The PSP game reported error 0x" << std::hex << std::uppercase;
    text.width(8); text.fill('0'); text << code << '.';
    return text.str();
}
std::uint64_t deadline_after(std::uint64_t now, std::uint64_t delay) {
    return now > std::numeric_limits<std::uint64_t>::max() - delay ?
        std::numeric_limits<std::uint64_t>::max() : now + delay;
}
}

MessageDialogRuntime::MessageDialogRuntime(psprecomp::Runtime &runtime, MessageDialogPresenter *presenter,
    Clock clock, UtilityDialogOwner *owner) : runtime_(runtime), presenter_(presenter),
    clock_(std::move(clock)), owner_(owner ? owner : &private_owner_) {
    runtime.register_hle("sceUtility", init_nid, [this](auto &, auto &ctx) { init_start(ctx); });
    runtime.register_hle("sceUtility", shutdown_nid, [this](auto &, auto &ctx) { shutdown_start(ctx); });
    runtime.register_hle("sceUtility", update_nid, [this](auto &, auto &ctx) { update(ctx); });
    runtime.register_hle("sceUtility", status_nid, [this](auto &, auto &ctx) { get_status(ctx); });
}
MessageDialogRuntime::~MessageDialogRuntime() {
    close_presenter();
    if (owns_slot_) owner_->release(UtilityDialogType::Message);
}
std::uint64_t MessageDialogRuntime::now_us() const {
    if (clock_) return clock_();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
void MessageDialogRuntime::tick() {
    if (status_ != Status::Init && status_ != Status::Shutdown) return;
    if (now_us() < deadline_) return;
    if (status_ == Status::Init) status_ = Status::Running;
    else {
        if (owns_slot_) owner_->release(UtilityDialogType::Message);
        owns_slot_ = false;
        status_ = Status::None;
        params_ = request_error_ = 0u;
        request_ = {};
    }
}
void MessageDialogRuntime::close_presenter() noexcept {
    if (opened_ && presenter_) presenter_->close(request_.generation);
    opened_ = false;
}
bool MessageDialogRuntime::valid_output_span() const {
    return guest_span(runtime_.memory(), params_, request_.size);
}

void MessageDialogRuntime::init_start(psprecomp::AllegrexContext &ctx) {
    tick();
    // Hardware status.expected: busy wins even against an invalid new pointer.
    if (status_ != Status::None || owner_->busy()) { ctx.set_gpr(2u, invalid_status); return; }
    auto &memory = runtime_.memory();
    const auto params = ctx.gpr[4];
    if (!guest_span(memory, params, 48u)) { ctx.set_gpr(2u, invalid_address); return; }
    const auto size = memory.load32(params);
    if (size != size_v1 && size != size_v2 && size != size_v3) {
        ctx.set_gpr(2u, invalid_param_size); return;
    }
    if (!guest_span(memory, params, size)) { ctx.set_gpr(2u, invalid_address); return; }
    MessageDialogRequest request;
    request.size = size;
    request.language = memory.load32(params + 4u);
    request.button_swap = memory.load32(params + 8u);
    request.mode = memory.load32(params + 52u);
    request.error_value = memory.load32(params + 56u);
    request.options = size >= size_v2 ? memory.load32(params + 572u) : 0u;
    if (request.mode > 1u || request.button_swap > 1u || request.language > 11u) {
        runtime_.stop("Native message dialog has an unsupported mode, language or confirm-button mapping");
        return;
    }
    std::uint32_t request_error{};
    if (request.mode == 0u && !(request.error_value & 0x80000000u)) request_error = invalid_error_code;
    else if (size == size_v2 && request.mode == 1u &&
        (request.options & ~(text_sound | yes_no | default_no))) request_error = bad_option;
    else if (size == size_v3 && ((request.options & ~0x1B3u) ||
        ((request.options & default_no) && !(request.options & yes_no)))) request_error = bad_option;

    request.yes_no = (request.options & yes_no) &&
        (size == size_v3 || (size == size_v2 && request.mode == 1u));
    request.okay = size == size_v3 && (request.options & okay);
    request.allow_back = !(size == size_v3 && (request.options & no_cancel));
    request.default_no = request.yes_no && (request.options & default_no);
    request.suppress_sound = (request.options & no_sound) != 0u;
    request.text_sound = (request.options & text_sound) != 0u;
    if (!request_error && ((request.yes_no && request.okay) ||
        (!request.allow_back && !request.yes_no && !request.okay))) {
        runtime_.stop("Native message dialog does not support combined YESNO/OK or a request with no actionable buttons");
        return;
    }
    request.text = request.mode == 0u ? error_text(request.error_value) : fixed_string(memory, params + 60u, 512u);
    if (size == size_v3) {
        request.okay_label = fixed_string(memory, params + 580u, 64u);
        request.back_label = fixed_string(memory, params + 644u, 64u);
    }
    if (!owner_->try_acquire(UtilityDialogType::Message)) { ctx.set_gpr(2u, invalid_status); return; }
    request.generation = ++next_generation_;
    request_ = std::move(request);
    request_error_ = request_error;
    params_ = params;
    owns_slot_ = true;
    opened_ = false;
    status_ = Status::Init;
    deadline_ = deadline_after(now_us(), init_delay_us);
    // Both common.result and MsgDialog.result are preserved during INIT.
    ctx.set_gpr(2u, 0u);
}
void MessageDialogRuntime::get_status(psprecomp::AllegrexContext &ctx) {
    tick();
    ctx.set_gpr(2u, owner_->is_current_type(UtilityDialogType::Message) ?
        static_cast<std::uint32_t>(status_) : wrong_type);
}
void MessageDialogRuntime::update(psprecomp::AllegrexContext &ctx) {
    tick();
    if (!owner_->is_current_type(UtilityDialogType::Message)) { ctx.set_gpr(2u, wrong_type); return; }
    if (status_ != Status::Running) { ctx.set_gpr(2u, invalid_status); return; }
    if (!valid_output_span()) { ctx.set_gpr(2u, invalid_address); return; }
    // animSpeed controls the firmware fade, which this native presenter does
    // not reproduce. It does not change native lifecycle or input selection.
    if (request_error_) {
        runtime_.memory().store32(params_ + 48u, request_error_);
        status_ = Status::Finished;
        ctx.set_gpr(2u, 0u);
        return;
    }
    if (!presenter_) { runtime_.stop("Native message dialog requires an actual user-input presenter"); return; }
    std::optional<MessageDialogSelection> selection;
    try {
        if (!opened_) {
            // close() also handles an open() that failed after allocating UI.
            opened_ = true;
            presenter_->open(request_);
        }
        selection = presenter_->poll(request_.generation);
    } catch (const std::exception &error) {
        close_presenter();
        runtime_.stop(std::string("Native message dialog presenter failed: ") + error.what());
        return;
    }
    std::uint32_t pressed{};
    const auto reject_selection = [this](const char *reason) {
        close_presenter();
        runtime_.stop(reason);
    };
    bool finished = selection && selection->generation == request_.generation;
    if (finished) {
        switch (selection->choice) {
        case MessageDialogChoice::Yes:
            if (!request_.yes_no) { reject_selection("Native message dialog received an unavailable YES action"); return; }
            pressed = 1u; break;
        case MessageDialogChoice::No:
            if (!request_.yes_no) { reject_selection("Native message dialog received an unavailable NO action"); return; }
            pressed = 2u; break;
        case MessageDialogChoice::Okay:
            if (!request_.okay) { reject_selection("Native message dialog received an unavailable OK action"); return; }
            pressed = 1u; break;
        case MessageDialogChoice::Back:
            if (!request_.allow_back) { reject_selection("Native message dialog received an unavailable BACK action"); return; }
            pressed = request_.size == size_v3 || (request_.size == size_v2 && request_.yes_no) ? 3u : 0u;
            break;
        default: reject_selection("Native message dialog received an unknown input action"); return;
        }
        close_presenter();
    }
    // Writes are restricted to the two documented output words. Source text,
    // options, priorities, common.result and bytes beyond the ABI stay intact.
    runtime_.memory().store32(params_ + 48u, 0u);
    if (finished && request_.size >= size_v2) runtime_.memory().store32(params_ + 576u, pressed);
    if (finished) status_ = Status::Finished;
    ctx.set_gpr(2u, 0u);
}
void MessageDialogRuntime::shutdown_start(psprecomp::AllegrexContext &ctx) {
    tick();
    if (!owner_->is_current_type(UtilityDialogType::Message)) { ctx.set_gpr(2u, wrong_type); return; }
    if (status_ != Status::Finished) { ctx.set_gpr(2u, invalid_status); return; }
    close_presenter();
    status_ = Status::Shutdown;
    deadline_ = deadline_after(now_us(), shutdown_delay_us);
    ctx.set_gpr(2u, 0u);
}
} // namespace chili
