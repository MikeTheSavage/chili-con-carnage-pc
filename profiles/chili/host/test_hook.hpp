#pragma once

// CHILI_TEST_HOOK=<guest address, hex>: registers a host function over that
// address which counts its calls and then runs the code it replaced; the count
// is printed when the run ends. It shows whether a host override sees every
// call of a guest function, including the calls made from inside the
// function's own generated unit. Install it after the generated functions.
//
// CHILI_TEST_HOOK_LOG=<n> also writes one line to stderr for each of the first
// n calls: the return address, a0-a3, f12, and the code addresses found in the
// first 96 words of the guest stack (the callers, roughly). With
// CHILI_TEST_HOOK_A0=<hex> only calls whose a0 is that value are counted and
// logged. CHILI_TEST_HOOK_SKIP=<n> leaves the first n matching calls out of the log.

#include "psprecomp/common.hpp"
#include "psprecomp/runtime.hpp"

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace chili {

class TestHook {
public:
    static void install(psprecomp::Runtime &runtime) {
        const char *text = std::getenv("CHILI_TEST_HOOK");
        if (text == nullptr || *text == 0) return;
        state().address = static_cast<std::uint32_t>(std::strtoul(text, nullptr, 16));
        state().original = runtime.function_at(state().address);
        if (state().original == nullptr) {
            std::cerr << "[test-hook] no generated code at " << psprecomp::hex32(state().address) << '\n';
            return;
        }
        if (const char *log = std::getenv("CHILI_TEST_HOOK_LOG")) state().log = std::strtoull(log, nullptr, 10);
        if (const char *skip = std::getenv("CHILI_TEST_HOOK_SKIP")) state().skip = std::strtoull(skip, nullptr, 10);
        if (const char *a0 = std::getenv("CHILI_TEST_HOOK_A0")) { state().a0 = static_cast<std::uint32_t>(std::strtoul(a0, nullptr, 16)); state().filter = *a0 != 0; }
        runtime.register_function(state().address, &hook, "chili_test_hook");
    }

    static void report() {
        if (state().original != nullptr)
            std::cout << "[test-hook] address=" << psprecomp::hex32(state().address) << " calls=" << state().calls << '\n';
    }
    // A marker in the log, so the calls can be told apart by controller sample.
    static void mark(std::uint64_t sample) {
        if (state().original != nullptr && state().log) std::cerr << "[test-hook] sample " << sample << '\n';
    }

private:
    struct State {
        std::uint32_t address{}, a0{};
        psprecomp::Runtime::RecompiledFunction original{};
        std::uint64_t calls{}, log{}, skip{};
        bool filter{};
    };
    static State &state() {
        static State instance;
        return instance;
    }
    // Entered with ctx.pc on the hooked address; the unit function runs it from there.
    static void hook(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        auto &s = state();
        if (!s.filter || ctx.gpr[4] == s.a0) {
            if (s.calls >= s.skip && s.calls < s.skip + s.log) {
                std::cerr << "[test-hook] call=" << s.calls << " ra=" << psprecomp::hex32(ctx.gpr[31]);
                for (unsigned r = 4; r < 8; ++r) std::cerr << " a" << (r - 4u) << '=' << psprecomp::hex32(ctx.gpr[r]);
                std::cerr << " f12=" << ctx.fpr[12];
                const auto &memory = rt.memory();
                if (memory.contains(ctx.gpr[5], 16u)) {
                    std::cerr << " [a1]=";
                    for (unsigned i = 0; i < 4u; ++i) std::cerr << (i ? "," : "") << std::bit_cast<float>(memory.load32(ctx.gpr[5] + i * 4u));
                }
                std::cerr << " stack:";
                for (std::uint32_t offset = 0; offset < 96u * 4u; offset += 4u) {
                    if (!memory.contains(ctx.gpr[29] + offset, 4u)) break;
                    const auto word = memory.load32(ctx.gpr[29] + offset);
                    if (word >= 0x08804000u && word < 0x08D99000u && !(word & 3u)) std::cerr << ' ' << psprecomp::hex32(word);
                }
                std::cerr << '\n';
            }
            ++s.calls;
        }
        s.original(rt, ctx);
    }
};

} // namespace chili
