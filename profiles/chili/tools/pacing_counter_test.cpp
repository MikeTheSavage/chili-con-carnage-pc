#include "thread_runtime.hpp"
#include "audio_runtime.hpp"
#include "display_runtime.hpp"
#include "performance_telemetry.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

// No device, window, sleep, or host input: the audio output completes its blocks on the guest clock
// (the sink of deterministic runs), so every deadline below follows from the calls alone. The host
// ring and its feeder thread, which a device run adds behind this queue, are covered by audio_test.cpp.
namespace {
using Context = psprecomp::AllegrexContext;
using Telemetry = chili::PerformanceTelemetry;
unsigned checks{};
void require(bool condition, const char *why) {
    ++checks; if (!condition) throw std::runtime_error(why);
}
std::vector<std::uint64_t> words(const Context &c) {
    std::vector<std::uint64_t> out(c.gpr.begin(), c.gpr.end());
    out.insert(out.end(), {c.hi, c.lo, c.pc, c.fcr31});
    for (const auto f : c.fpr) out.push_back(std::bit_cast<std::uint32_t>(f));
    for (const auto f : c.vfpu) out.push_back(std::bit_cast<std::uint32_t>(f));
    out.insert(out.end(), c.vfpu_ctrl.begin(), c.vfpu_ctrl.end());
    return out;
}
struct Result {
    std::vector<std::uint64_t> trace;
    std::vector<std::uint8_t> memory;
    bool operator==(const Result &) const = default;
};
void seed(Context &c) {
    for (unsigned i = 1; i < 32; ++i) c.gpr[i] = 0x31415900u + i;
    for (unsigned i = 0; i < 32; ++i) c.set_fpr_bits(i, 0x7FC10000u + i);
    for (unsigned i = 0; i < 128; ++i) c.vfpu[i] = std::bit_cast<float>(0x80000000u + i);
    for (unsigned i = 0; i < 16; ++i) c.vfpu_ctrl[i] = 0xABCD0000u + i;
    c.hi = 0xCAFE1234; c.lo = 0xBEEF5678; c.fcr31 = 0x00800003;
    c.gpr[29] = 0x09FEF000; c.gpr[31] = 0x08804000; c.pc = 0x08801000;
}
void timer_work(psprecomp::Runtime &runtime, Context &c) {
    ++c.gpr[20]; c.pc = c.gpr[20] < 2300u ? 0x08806000u : 0u;
    if (c.gpr[20] == 2300u) runtime.stop("Fixture timer completed");
}

Result exercise(bool enabled) {
    Result result;
    std::ostringstream sink; Telemetry telemetry(sink, Telemetry::Clock::time_point{});
    {
        psprecomp::Runtime runtime; auto &c = runtime.cpu(); seed(c);
        chili::ThreadRuntime threads(runtime); threads.initialize_bootstrap();
        chili::AudioRuntime audio(runtime, &threads, true);
        chili::DisplayRuntime display(runtime, threads);
#ifndef PACING_BASELINE
        threads.set_pacing_observer(enabled ? &telemetry : nullptr);
        audio.set_pacing_observer(enabled ? &telemetry : nullptr);
        display.set_pacing_observer(enabled ? &telemetry : nullptr);
#else
        (void)enabled;
#endif
        const auto initial_generation = psprecomp::runtime_thread_switch_generation();
        auto checkpoint = [&] {
            auto cpu = words(c); result.trace.insert(result.trace.end(), cpu.begin(), cpu.end());
            result.trace.insert(result.trace.end(), {threads.now_us(),
                static_cast<std::uint64_t>(threads.current_uid()),
                static_cast<std::uint64_t>(psprecomp::runtime_thread_uid()),
                psprecomp::runtime_thread_switch_generation() - initial_generation,
                threads.interrupts_enabled(), threads.in_interrupt(), runtime.function_count(), runtime.stopped()});
        };
        auto call = [&](const char *library, unsigned nid) {
            runtime.invoke_import(library, nid, c);
            require(!runtime.stopped(), "Import stopped fixture"); checkpoint();
        };
        auto resume = [&](unsigned address) {
            require(runtime.invoke_isolated_aot(address, c), "Missing real continuation");
            require(!runtime.stopped(), "Continuation stopped fixture"); checkpoint();
        };
        for (unsigned i = 0; i < 7; ++i) call("ThreadManForUser", 0x369ED59D);
        require(threads.now_us() == 7 && c.gpr[2] == 7, "SystemTimeLow behavior changed");
        c.gpr[4] = 250; c.gpr[31] = 0x08804000;
        call("ThreadManForUser", 0xCEADEB47);
        require(threads.now_us() == 257 && c.pc == 0x08804000, "Idle deadline jump changed");
        // Interrupt-disabled waits must remain rejected and uncounted.
        call("Kernel_Library", 0x092968F4);
        c.gpr[31] = 0x08804000; call("sceDisplay", 0x46F186C3);
        require(c.gpr[2] == 0x800201A7 && threads.now_us() == 257, "Rejected VBlank wait advanced time");
        c.gpr[4] = 1; call("Kernel_Library", 0x5F10D406);
        c.gpr[31] = 0x08804000;
        call("sceDisplay", 0x46F186C3); resume(chili::DisplayRuntime::trampoline);
        require(threads.now_us() == 16683 && c.pc == c.gpr[31], "Callback VBlank timing changed");
        // Invalid SetMode size is also not an accepted VBlank wait.
        c.gpr[4] = 0; c.gpr[5] = 479; c.gpr[6] = 272;
        call("sceDisplay", 0x0E20F177); require(c.gpr[2] == 0x80000104, "SetMode validation changed");
        c.gpr[5] = 480; c.gpr[31] = 0x08804000;
        call("sceDisplay", 0x0E20F177); resume(chili::DisplayRuntime::trampoline);
        require(threads.now_us() == 33366 && display.snapshot().configured, "SetMode deadline changed");
        // Existing reserve / validation and two real queue submissions. A 17-frame block plays for
        // 385 guest microseconds, and a blocked writer sleeps exactly until the block in front is done.
        c.gpr[4] = 16; call("sceAudio", 0x01562BA3);
        require(c.gpr[2] == 0x80000104, "Invalid reserve accepted");
        c.gpr[4] = 17; call("sceAudio", 0x01562BA3); require(c.gpr[2] == 0, "Reserve failed");
        for (unsigned i = 0; i < 34; ++i) runtime.memory().store16(0x08802000 + i * 2, static_cast<std::uint16_t>(i * 123));
        c.gpr[4] = 0x8000; c.gpr[5] = 0x08802000; c.gpr[31] = 0x08804100;
        call("sceAudio", 0x2D53F36E); require(c.gpr[2] == 17 && threads.now_us() == 33366, "First completion marker changed");
        c.gpr[4] = 0x8000; c.gpr[5] = 0x08802000; c.gpr[31] = 0x08804100;
        auto saved = c; saved.set_gpr(2, 17); saved.pc = saved.gpr[31];
        call("sceAudio", 0x2D53F36E);
        require(c.pc == 0x0800000C && threads.now_us() == 33751, "Blocking output deadline changed");
        // The first block's time has passed on the guest clock: the writer goes on, nothing is retried.
        resume(0x0800000C);
        require(words(c) == words(saved) && threads.now_us() == 33751, "Completion failed to restore full CPU");
        c.gpr[4] = 0x8000; c.gpr[5] = 0; c.gpr[31] = 0x08804200;
        saved = c; saved.set_gpr(2, 0); saved.pc = saved.gpr[31];
        call("sceAudio", 0x2D53F36E);
        require(c.pc == 0x0800000C && threads.now_us() == 34136, "Drain deadline changed");
        resume(0x0800000C);
        require(words(c) == words(saved) && threads.now_us() == 34136, "Drain completion lost CPU");
        call("sceAudio", 0x647CEF33); require(c.gpr[2] == 0, "Finished queue not empty");
        call("sceAudio", 0x43196845); require(c.gpr[2] == 0, "Release changed");
        // Real outer dispatch starvation cadence, not a synthetic timer call.
        runtime.register_function(0x08806000, timer_work, "pacing_timer_work");
        c.gpr[20] = 0; runtime.run(0x08806000, 2400); checkpoint();
        require(runtime.stop_reason() == "Fixture timer completed" && c.gpr[20] == 2300 && threads.now_us() == 34236, "Timer cadence changed");
        require(sink.str().empty(), "HLE observer emitted output");
#ifndef PACING_BASELINE
        const auto &v = telemetry.counters();
        if (enabled) {
            require(v[Telemetry::IdleDeadlineJumps] == 5 && v[Telemetry::IdleDeadlineJumpUs] == 34129, "Idle jump attribution wrong");
            require(v[Telemetry::SchedulerTimerTicks] == 1 && v[Telemetry::SchedulerTimerUs] == 100, "Timer attribution wrong");
            require(v[Telemetry::SystemTimeLowReads] == 7 && v[Telemetry::SystemTimeLowReadUs] == 7, "Read attribution wrong");
            require(v[Telemetry::AudioResumeRetries] == 0 && v[Telemetry::AudioResumeCompletions] == 2, "Audio retry/completion attribution wrong");
            require(v[Telemetry::VblankWaits] == 2 && v[Telemetry::VblankRequestedDelayUs] == 33109, "VBlank attribution wrong");
            require(threads.now_us() == v[Telemetry::IdleDeadlineJumpUs] + v[Telemetry::SchedulerTimerUs] + v[Telemetry::SystemTimeLowReadUs], "Clock mutation totals do not reconcile");
            for (std::size_t i = 0; i <= Telemetry::TexturedSpriteRasterWallNs; ++i)
                require(v[i] == 0, "Pacing instrumentation affected prior counters");
            telemetry.poll(Telemetry::Clock::time_point{} + std::chrono::seconds(1), threads.now_us());
            require(sink.str().find("idle_jump_count=5 idle_jump_us=34129 timer_count=1 timer_us=100 read_count=7 read_us=7 audio_resume_retry_count=0 audio_resume_completion_count=2 vblank_wait_count=2 vblank_requested_delay_us=33109") != std::string::npos, "Output fields missing/wrong");
        } else {
            require(std::all_of(v.begin(), v.end(), [](auto n) { return n == 0; }), "Null observer mutated counters");
        }
#endif
        // Byte-for-byte entire mapped memory; includes all RAM, physical VRAM,
        // scratchpad and original PCM source. No hashes hide parity failures.
        result.memory.resize(runtime.memory().size() + runtime.memory().vram_size() + psprecomp::GuestMemory::kScratchpadSize);
        auto bytes = std::span<std::uint8_t>(result.memory);
        runtime.memory().copy_out(0x08000000, bytes.first(runtime.memory().size()));
        bytes = bytes.subspan(runtime.memory().size());
        runtime.memory().copy_out(0x04000000, bytes.first(runtime.memory().vram_size()));
        runtime.memory().copy_out(psprecomp::GuestMemory::kScratchpadBase, bytes.subspan(runtime.memory().vram_size()));
    }
    return result;
}
void write_result(const std::filesystem::path &path, const Result &r) {
    // The fixture folder is not in the repository; a fresh checkout does not have it yet.
    std::error_code status;
    std::filesystem::create_directories(path.parent_path(), status);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    auto vector = [&](const auto &values) {
        const std::uint64_t count = values.size(); file.write(reinterpret_cast<const char *>(&count), sizeof(count));
        file.write(reinterpret_cast<const char *>(values.data()), static_cast<std::streamsize>(values.size() * sizeof(values[0])));
    };
    vector(r.trace); vector(r.memory);
    require(static_cast<bool>(file), "Trace output failed");
}
}
int main(int argc, char **argv) {
    try {
        require(argc == 2, "Expected workspace build trace path");
        const auto path = std::filesystem::absolute(argv[1]).lexically_normal();
        const auto source = std::filesystem::path(__FILE__).parent_path().parent_path();
        require(path.parent_path() == source / "build" ||
            path.parent_path() == source / "analysis/chili-pacing-counter-fixtures",
            "Trace must stay in the prep build or official analysis fixture directory");
        auto null = exercise(false);
#ifndef PACING_BASELINE
        const auto enabled = exercise(true);
        require(null == enabled, "OFF/ON CPU/memory/imports/UID/deadline differ");
#endif
        write_result(path, null);
        std::cout << "PASS: " << checks << " assertions; full guest CPU/RAM/VRAM/scratch, UID/generation, continuations and virtual-time parity with guest-clocked audio; no device opened\n";
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
