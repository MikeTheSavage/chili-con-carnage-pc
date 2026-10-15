#pragma once
#include "psprecomp/runtime.hpp"
#include <functional>
#include <limits>
#include <iostream>
#include <utility>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace chili {
class PowerRuntime {
public:
    using Inhibit = std::function<bool(bool)>;
    using Tick = std::function<bool(bool, bool)>;
    explicit PowerRuntime(psprecomp::Runtime &rt, Inhibit inhibit = {}, Tick tick = {})
        : inhibit_(std::move(inhibit)), tick_(std::move(tick)) {
        if (!inhibit_) inhibit_ = [](bool on) {
            return SetThreadExecutionState(ES_CONTINUOUS | (on ? ES_SYSTEM_REQUIRED : 0)) != 0;
        };
        if (!tick_) tick_ = [](bool system, bool display) {
            // Without ES_CONTINUOUS this resets idle timers and preserves the
            // separate, persistent suspend-lock execution requirement.
            return SetThreadExecutionState((system ? ES_SYSTEM_REQUIRED : 0) |
                                           (display ? ES_DISPLAY_REQUIRED : 0)) != 0;
        };
        rt.register_hle("sceSuspendForUser", 0xEADB1BD7u, [this](auto &r, auto &c) { change(r,c,1); });
        rt.register_hle("sceSuspendForUser", 0x3AEE7261u, [this](auto &r, auto &c) { change(r,c,-1); });
        rt.register_hle("sceSuspendForUser", 0x090CCB3Fu, [this](auto &r, auto &c) { power_tick(r,c); });
    }
    ~PowerRuntime() { if (balance_ > 0) inhibit_(false); }
    long long balance() const { return balance_; }
private:
    void power_tick(psprecomp::Runtime &r, psprecomp::AllegrexContext &c) {
        // The kernel import forwards to the power handler (uofw suspend.c).
        // PSPSDK documents ALL=0, SUSPEND=1, DISPLAY=6. Other bit patterns
        // lack a verified firmware oracle and are deliberately unsupported.
        const auto mode = c.gpr[4];
        if (mode != 0u && mode != 1u && mode != 6u) {
            r.stop("Unsupported sceKernelPowerTick mode " + std::to_string(mode));
            return;
        }
        if (!tick_(mode != 6u, mode != 1u)) {
            r.stop("Windows idle-timer reset failed");
            return;
        }
        c.set_gpr(2, 0u);
    }
    void change(psprecomp::Runtime &r, psprecomp::AllegrexContext &c, int delta) {
        if (c.gpr[4]) { c.set_gpr(2,0x80000107u); return; }
        if ((delta > 0 && balance_ == std::numeric_limits<long long>::max()) ||
            (delta < 0 && balance_ == std::numeric_limits<long long>::min())) {
            r.stop("Power lock balance overflow"); return;
        }
        const auto next = balance_ + delta;
        if ((next > 0) != (balance_ > 0) && !inhibit_(next > 0)) {
            r.stop("Windows system-sleep inhibition failed"); return;
        }
        balance_ = next; c.set_gpr(2,0);
        std::cout << "[power] suspend lock balance=" << balance_ << '\n';
    }
    Inhibit inhibit_;
    Tick tick_;
    long long balance_{};
};
}
