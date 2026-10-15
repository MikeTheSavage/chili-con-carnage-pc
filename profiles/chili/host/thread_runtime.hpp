#pragma once
#include "psprecomp/runtime.hpp"
#include <map>
#include <functional>
#include <optional>

namespace chili {
class ThreadLifecycle;
class PerformanceTelemetry;
class ThreadRuntime {
public:
    static constexpr std::uint32_t return_trampoline = 0x08000000u;
    explicit ThreadRuntime(psprecomp::Runtime &runtime,
        std::function<std::uint32_t(std::uint32_t)> stack_allocator = {});
    ~ThreadRuntime();
    void initialize_bootstrap();
    // Setup-time optional observer; caller owns its lifetime. No timing policy.
    void set_pacing_observer(PerformanceTelemetry *observer) noexcept { pacing_observer_ = observer; }
    // Runs when every thread is asleep, before the clock jumps to the earliest
    // wake time (the argument). A real-time pacer waits here; unset, guest time
    // never waits for the host.
    void set_idle_hook(std::function<void(std::uint64_t)> hook) { idle_hook_ = std::move(hook); }
    // Runs at every timer tick (2048 dispatches of guest code), before the tick's
    // scheduling point. For a host device that must not wait for the frame's idle.
    void set_tick_hook(std::function<void()> hook) { tick_hook_ = std::move(hook); }
    // A host event has made `uid`'s delay moot: it is ready at the next scheduling point.
    void expedite(std::int32_t uid) {
        const auto found = threads_.find(uid);
        if (found != threads_.end() && found->second.state == State::Delayed && found->second.wake_time > clock_us_) found->second.wake_time = clock_us_;
    }
    std::int32_t current_uid() const { return current_; }
    std::uint32_t priority(std::int32_t uid) const { return threads_.at(uid).priority; }
    std::uint64_t now_us() const { return clock_us_; }
    bool can_wait() const { return interrupts_enabled_ && !in_interrupt_; }
    bool interrupts_enabled() const { return interrupts_enabled_; }
    bool in_interrupt() const { return in_interrupt_; }
    void wait(psprecomp::AllegrexContext &, std::uint64_t deadline, std::function<void()> on_timeout);
    bool wake(std::int32_t uid, std::uint32_t result);
    void dispatch_ready(psprecomp::AllegrexContext &ctx) { ctx.pc = ctx.gpr[31]; reschedule(ctx, false); }
    void request_exit_callback();
    bool enter_interrupt() { if(!can_wait())return false;in_interrupt_=true;return true; }
    void leave_interrupt(psprecomp::AllegrexContext &ctx);

private:
    friend class ThreadLifecycle;
    std::function<void(std::int32_t,std::uint32_t)> lifecycle_end_hook_;
    enum class State { Dormant, Ready, Running, Delayed, Waiting, Ended };
    struct Thread {
        std::string name;
        std::uint32_t entry{}, priority{}, bottom{}, top{}, attributes{}, initial_gp{};
        psprecomp::AllegrexContext context{};
        State state{State::Dormant};
        std::uint64_t ready_order{}, wake_time{};
        std::uint32_t exit_status{};
        std::function<void()> on_timeout;
        bool callback_delay{};
        std::optional<psprecomp::AllegrexContext> callback_resume;
        std::uint32_t active_callback{};
    };
    static ThreadRuntime *active_;
    static void returned(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    static void timer(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    void create(psprecomp::AllegrexContext &);
    void start(psprecomp::AllegrexContext &);
    void delay(psprecomp::AllegrexContext &);
    void delay_callback(psprecomp::AllegrexContext &);
    void callback_operation(psprecomp::AllegrexContext &, unsigned operation);
    bool deliver_callback(psprecomp::AllegrexContext &);
    static void callback_returned(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    void notify_callback(std::uint32_t uid, std::uint32_t argument);
    void finish(psprecomp::AllegrexContext &);
    void reschedule(psprecomp::AllegrexContext &, bool force, bool timeslice = false);
    psprecomp::Runtime &runtime_;
    std::function<std::uint32_t(std::uint32_t)> stack_allocator_;
    std::map<std::int32_t, Thread> threads_;
    std::int32_t current_{1}, next_uid_{2};
    std::uint32_t next_stack_top_{0x09FF0000u};
    std::uint64_t clock_us_{}, order_{};
    PerformanceTelemetry *pacing_observer_{};
    std::function<void(std::uint64_t)> idle_hook_;
    std::function<void()> tick_hook_;
    bool interrupts_enabled_{true}, timer_pending_{}, in_interrupt_{};
    struct Callback { std::string name; std::int32_t owner; std::uint32_t entry, common, gp, count{}, argument{}; };
    std::map<std::uint32_t, Callback> callbacks_;
    std::uint32_t next_callback_{0x50000u}, exit_callback_{};
    std::array<std::uint32_t,16> power_callbacks_{};
};
}
