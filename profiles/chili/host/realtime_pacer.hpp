#pragma once
#include "frame_timer.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <utility>
namespace chili {
// Keeps guest time from running ahead of the host clock. The scheduler calls
// idle_until() whenever every guest thread is asleep; if guest time is ahead of
// real time the host sleeps the difference, so vblank waits, delays and audio
// polls take as long as they would on a PSP. A host that is too slow does not
// wait at all, and what it has to catch up afterwards is capped at `slack`.
//
// The thread that owns the pacer is the game: it sleeps through every idle
// stretch, so with `raise_priority` it runs one step above normal priority
// while the pacer exists. Without that, other programs that keep every core
// busy hold it back 100 ms and more at a time (measured with compilers running:
// six such stalls in a 35 s movie, each one a hole in the sound; none at the
// raised priority). A player's run asks for it; a scripted run does not unless
// told to ([Game] Priority, CHILI_GAME_PRIORITY), so frame times measured by
// scripts stay comparable from build to build.
class RealTimePacer {
public:
    using Clock = std::chrono::steady_clock;
    // `pump` runs between slices of a long wait so the window stays responsive.
    explicit RealTimePacer(std::function<void()> pump = {}, bool raise_priority = false) : pump_(std::move(pump)) {
        // 0x2 = CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803 and later).
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0x2u, TIMER_ALL_ACCESS);
        if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0u, TIMER_ALL_ACCESS);
        if (!raise_priority) return;
        thread_ = GetCurrentThread();
        if (DuplicateHandle(GetCurrentProcess(), thread_, GetCurrentProcess(), &thread_, 0u, FALSE, DUPLICATE_SAME_ACCESS)) {
            priority_ = GetThreadPriority(thread_);
            if (priority_ != THREAD_PRIORITY_ERROR_RETURN && priority_ < THREAD_PRIORITY_ABOVE_NORMAL) SetThreadPriority(thread_, THREAD_PRIORITY_ABOVE_NORMAL);
        } else thread_ = nullptr;
    }
    ~RealTimePacer() {
        if (thread_) { if (priority_ != THREAD_PRIORITY_ERROR_RETURN) SetThreadPriority(thread_, priority_); CloseHandle(thread_); }
        if (timer_) CloseHandle(timer_);
    }
    RealTimePacer(const RealTimePacer &) = delete;
    RealTimePacer &operator=(const RealTimePacer &) = delete;
    void idle_until(std::uint64_t guest_us) {
        auto now = Clock::now();
        if (!anchored_ || guest_us < guest_anchor_) { anchor(now, guest_us); return; }
        const auto target = wall_anchor_ + std::chrono::microseconds(guest_us - guest_anchor_);
        if (now >= target) { if (now - target > slack) anchor(now - slack, guest_us); return; }
        if (target - now < shortest_wait) return;
        const auto started = now;
        while (now < target) {
            wait(std::min<Clock::duration>(target - now, slice));
            now = Clock::now();
            if (pump_ && now < target) pump_();
        }
        slept_ += now - started;
        FrameTimer::add_idle(now - started);
    }
    Clock::duration slept() const noexcept { return slept_; }
private:
    static constexpr Clock::duration slack = std::chrono::milliseconds(50);
    static constexpr Clock::duration shortest_wait = std::chrono::microseconds(200);
    static constexpr Clock::duration slice = std::chrono::milliseconds(8);
    void anchor(Clock::time_point wall, std::uint64_t guest_us) noexcept { wall_anchor_ = wall; guest_anchor_ = guest_us; anchored_ = true; }
    void wait(Clock::duration duration) {
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<long long>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count() / 100);
        if (timer_ && SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer_, INFINITE);
        else Sleep(static_cast<DWORD>(std::max<long long>(1, std::chrono::duration_cast<std::chrono::milliseconds>(duration).count())));
    }
    std::function<void()> pump_;
    HANDLE timer_{}, thread_{};
    int priority_{THREAD_PRIORITY_ERROR_RETURN};
    Clock::time_point wall_anchor_{};
    Clock::duration slept_{};
    std::uint64_t guest_anchor_{};
    bool anchored_{};
};
}
