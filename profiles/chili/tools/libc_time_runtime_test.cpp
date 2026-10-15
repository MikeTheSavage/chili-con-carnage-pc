#include "../host/libc_time_runtime.hpp"
#include "../host/rtc_runtime.hpp"
#include "../host/thread_runtime.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
constexpr unsigned time_nid = 0x27CC57F0u, clock_nid = 0x91E4F6A7u,
    tod_nid = 0x71EC4271u, exit_nid = 0x05572A5Fu;
constexpr unsigned output = 0x09000020u, timezone_output = 0x09000040u;
bool exit_wrapper_returned{};
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void pattern(psprecomp::AllegrexContext &c) {
    for (unsigned i = 1; i < c.gpr.size(); ++i) c.gpr[i] = 0xF0000000u + i;
    c.gpr[29] = 0x09FFF000u; c.gpr[28] = 0x08D9AB00u; c.gpr[31] = 0x08812340u;
    c.pc = 0x08D98A78u; c.hi = 0x12345678u; c.lo = 0x89ABCDEFu; c.fcr31 = 0x87654321u;
    for (unsigned i = 0; i < c.fpr.size(); ++i) c.fpr[i] = i * 1.25f;
    for (unsigned i = 0; i < c.vfpu.size(); ++i) c.vfpu[i] = -static_cast<float>(i);
    for (unsigned i = 0; i < c.vfpu_ctrl.size(); ++i) c.vfpu_ctrl[i] = 0xF0E0D000u + i;
}
bool same_cpu(const psprecomp::AllegrexContext &a, const psprecomp::AllegrexContext &b) {
    return a.gpr == b.gpr && a.pc == b.pc && a.hi == b.hi && a.lo == b.lo &&
        a.fpr == b.fpr && a.fcr31 == b.fcr31 && a.vfpu == b.vfpu && a.vfpu_ctrl == b.vfpu_ctrl;
}
unsigned call(psprecomp::Runtime &rt, unsigned nid, unsigned a0 = 0u, unsigned a1 = 0u) {
    auto &c = rt.cpu(); c.gpr[4] = a0; c.gpr[5] = a1;
    auto expected = c;
    const auto uid = psprecomp::runtime_thread_uid();
    const auto generation = psprecomp::runtime_thread_switch_generation();
    rt.invoke_import("UtilsForUser", nid, c);
    expected.gpr[2] = c.gpr[2];
    require(!rt.stopped(), "Valid libc time call stopped runtime");
    require(same_cpu(c, expected), "Libc time import changed caller CPU state outside V0");
    require(psprecomp::runtime_thread_uid() == uid && psprecomp::runtime_thread_switch_generation() == generation,
        "Libc time import changed guest scheduling ownership");
    return c.gpr[2];
}
void fill(psprecomp::GuestMemory &m, unsigned address, unsigned bytes, unsigned value = 0xA5u) {
    for (unsigned i = 0; i < bytes; ++i) m.store8(address + i, static_cast<std::uint8_t>(value));
}
void guard(psprecomp::GuestMemory &m, unsigned address, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) require(m.load8(address + i) == 0xA5u, "Time call changed adjacent/invalid output bytes");
}

void fixed_abi_and_errors() {
    psprecomp::Runtime rt;
    std::uint64_t now = 1700000000123456ull;
    unsigned samples{};
    chili::LibcTimeRuntime libc(rt, [&] { ++samples; return now; });
    pattern(rt.cpu()); auto &m = rt.memory();
    fill(m, output - 4u, 20u); fill(m, timezone_output - 4u, 20u);
    require(call(rt, time_nid, output) == 1700000000u && m.load32(output) == 1700000000u,
        "Guest time_t layout or return differs from Unix seconds");
    guard(m, output - 4u, 4u); guard(m, output + 4u, 12u);
    require(call(rt, time_nid) == 1700000000u, "Time(NULL) did not return time");
    require(call(rt, tod_nid, output, timezone_output) == 0u && m.load32(output) == 1700000000u &&
        m.load32(output + 4u) == 123456u, "Guest timeval is not two 32-bit seconds/microseconds fields");
    require(m.load32(timezone_output) == 0u && m.load32(timezone_output + 4u) == 0u,
        "PSP timezone compatibility fields differ from zero");
    guard(m, output - 4u, 4u); guard(m, output + 8u, 8u);
    guard(m, timezone_output - 4u, 4u); guard(m, timezone_output + 8u, 8u);
    require(call(rt, clock_nid) == static_cast<unsigned>(now + chili::rtc_unix_epoch_offset),
        "Clock is not the low 32 bits of the RTC microsecond tick");
    require(samples == 4u, "Libc imports sampled their source more than once");

    fill(m, output, 8u); fill(m, timezone_output, 8u);
    for (const auto invalid : {0xDEADBEEFu, 0x80010000u, output | 0x80000000u, output + 1u}) {
        require(call(rt, time_nid, invalid) == 0u, "Invalid Time output did not return the hardware failure value");
        require(call(rt, tod_nid, output, invalid) == 1u, "Invalid Gettimeofday timezone pointer did not fail");
        require(call(rt, tod_nid, invalid, timezone_output) == 1u, "Invalid Gettimeofday time pointer did not fail");
        guard(m, output, 8u); guard(m, timezone_output, 8u);
    }
    require(samples == 4u, "Invalid output validation sampled or modified the clock");
    constexpr auto ram_tail = 0x09FFFFFCu;
    fill(m, ram_tail, 4u);
    require(call(rt, tod_nid, ram_tail, timezone_output) == 1u, "Cross-boundary timeval was partially accepted");
    guard(m, ram_tail, 4u); guard(m, timezone_output, 8u);
    require(call(rt, time_nid, ram_tail) == 1700000000u && m.load32(ram_tail) == 1700000000u,
        "Last complete guest32 time_t was rejected");
    require(call(rt, time_nid, output | 0x40000000u) == 1700000000u && m.load32(output) == 1700000000u,
        "Uncached PSP user RAM alias was rejected");
    fill(m, timezone_output, 8u);
    require(call(rt, tod_nid, 0u, timezone_output) == 0u && m.load32(timezone_output) == 0u &&
        m.load32(timezone_output + 4u) == 0u, "Timezone-only Gettimeofday failed");
    require(call(rt, tod_nid, output, 0u) == 0u && m.load32(output + 4u) == 123456u,
        "Timeval-only Gettimeofday failed");
    require(call(rt, tod_nid, 0u, 0u) == 0u, "Gettimeofday(NULL, NULL) failed");
    now = 2147483648ull * 1000000ull + 999999u;
    require(call(rt, time_nid, output) == 0x80000000u && m.load32(output) == 0x80000000u,
        "Host-width time_t leaked into the PSP signed32 boundary");
    require(call(rt, tod_nid, output) == 0u && m.load32(output) == 0x80000000u &&
        m.load32(output + 4u) == 999999u, "Guest32 timeval sign boundary/layout mismatch");
}

void scheduler_clock() {
    psprecomp::Runtime rt; pattern(rt.cpu());
    chili::ThreadRuntime threads(rt); threads.initialize_bootstrap();
    const auto source = chili::make_scheduler_time_source(threads, 1700000000999500ull);
    chili::LibcTimeRuntime libc(rt, source);
    const auto uid = threads.current_uid();
    const auto virtual_start = threads.now_us();
    const auto first = call(rt, clock_nid);
    require(call(rt, time_nid) == 1700000000u && threads.now_us() == virtual_start,
        "Clock reads advanced scheduler time");
    auto &c = rt.cpu(); c.gpr[4] = 1000u; c.gpr[31] = 0x08812340u;
    rt.invoke_import("ThreadManForUser", 0xCEADEB47u, c);
    require(!rt.stopped() && threads.current_uid() == uid && threads.now_us() == virtual_start + 1000u,
        "Actual DelayThread fixture did not advance the native scheduler");
    const auto second = call(rt, clock_nid);
    require(second - first == 1000u, "LibcClock did not follow the real scheduler's 1000us delay");
    require(call(rt, tod_nid, output) == 0u && rt.memory().load32(output) == 1700000001u &&
        rt.memory().load32(output + 4u) == 500u && call(rt, time_nid) == 1700000001u,
        "Scheduler time crossed a second inconsistently across libc APIs");
    const auto elapsed = threads.now_us();
    for (unsigned n = 0; n < 100u; ++n) require(call(rt, clock_nid) == second, "Reading time fabricated clock progress");
    require(threads.now_us() == elapsed && threads.current_uid() == uid, "Time polling rescheduled guest execution");
    const auto late_anchor = chili::make_scheduler_time_source(threads, 1800000000000000ull);
    require(late_anchor() == 1800000000000000ull, "Wall anchor counted scheduler time from before its creation");
}

void clock_wrap() {
    psprecomp::Runtime rt; pattern(rt.cpu());
    chili::ThreadRuntime threads(rt); threads.initialize_bootstrap();
    constexpr std::uint64_t base = 1700000000000000ull;
    const auto adjust = static_cast<unsigned>(0xFFFFFFF0u - static_cast<unsigned>(base + chili::rtc_unix_epoch_offset));
    chili::LibcTimeRuntime libc(rt, chili::make_scheduler_time_source(threads, base + adjust));
    require(call(rt, clock_nid) == 0xFFFFFFF0u, "Clock wrap setup failed");
    rt.cpu().gpr[4] = 64u;
    rt.invoke_import("ThreadManForUser", 0xCEADEB47u, rt.cpu());
    require(call(rt, clock_nid) == 0x30u, "LibcClock did not wrap as guest32 microseconds");
    require(libc.rtc_tick() == base + adjust + 64u + chili::rtc_unix_epoch_offset,
        "Clock wrap corrupted the wide RTC clock");
}

void default_rtc_consistency() {
    psprecomp::Runtime rt; pattern(rt.cpu());
    chili::register_rtc(rt); chili::LibcTimeRuntime libc(rt);
    rt.cpu().gpr[4] = output;
    rt.invoke_import("sceRtc", 0x3F7AD767u, rt.cpu());
    const auto rtc = rt.memory().load32(output) | (static_cast<std::uint64_t>(rt.memory().load32(output + 4u)) << 32u);
    require(call(rt, tod_nid, output) == 0u, "Default Gettimeofday failed alongside existing RTC");
    const auto unix_us = static_cast<std::uint64_t>(rt.memory().load32(output)) * 1000000ull + rt.memory().load32(output + 4u);
    require(unix_us + chili::rtc_unix_epoch_offset >= rtc && unix_us + chili::rtc_unix_epoch_offset - rtc < 5000000ull,
        "Default libc clock disagrees with existing RTC epoch/units");
    const auto clock = call(rt, clock_nid);
    require(clock - static_cast<unsigned>(unix_us + chili::rtc_unix_epoch_offset) < 5000000u,
        "Default LibcClock disagrees with existing RTC tick");
}

void clock_failure() {
    psprecomp::Runtime rt; pattern(rt.cpu()); fill(rt.memory(), output, 8u);
    chili::LibcTimeRuntime libc(rt, [] { return std::numeric_limits<std::uint64_t>::max(); });
    rt.cpu().gpr[4] = output; rt.cpu().gpr[5] = 0u; const auto before = rt.cpu();
    rt.invoke_import("UtilsForUser", tod_nid, rt.cpu());
    require(rt.stopped() && rt.stop_reason().find("exceeds the PSP RTC tick range") != std::string::npos,
        "Invalid native time source silently succeeded");
    require(same_cpu(before, rt.cpu()), "Failed clock fabricated a return or CPU state");
    guard(rt.memory(), output, 8u);
}

void guest_exit() {
    psprecomp::Runtime rt; pattern(rt.cpu());
    chili::ThreadRuntime threads(rt); threads.initialize_bootstrap();
    chili::register_load_exec_exit(rt);
    require(!rt.stopped(), "Registering ExitGame activated exit without a guest call");
    rt.cpu().gpr[4] = 0u;
    rt.invoke_import("LoadExecForUser", 0x4AC57943u, rt.cpu());
    require(!rt.stopped() && rt.cpu().gpr[2] == 0x800201A1u,
        "ExitGame binding replaced existing RegisterExitCallback behavior");
    const auto uid = threads.current_uid();
    const auto ticks = threads.now_us();
    auto expected = rt.cpu(); expected.pc = 0u;
    rt.invoke_import("LoadExecForUser", exit_nid, rt.cpu());
    require(rt.stopped() && rt.stop_reason() == chili::guest_exit_reason && same_cpu(expected, rt.cpu()),
        "Explicit guest exit did not stop at PC0 with all remaining CPU state intact");
    require(threads.current_uid() == uid && threads.now_us() == ticks, "Guest ExitGame changed scheduler context");
}

void actual_exit_wrapper_abi() {
    // Replay the verified game wrapper 0882AEC0/0882AED0 (unit_0009), using
    // the same import thunk normalization as generated_registry import_100.
    // This checks the actual non-returning call ABI, not the full game corpus.
    psprecomp::Runtime rt; pattern(rt.cpu()); chili::register_load_exec_exit(rt);
    exit_wrapper_returned = false;
    rt.register_function(0x0882AEC0u, [](auto &r, auto &c) {
        c.set_gpr(29u, c.gpr[29] - 16u); r.memory().store32(c.gpr[29], c.gpr[31]);
        c.set_gpr(31u, 0x0882AED0u); c.pc = 0x08D98BF8u;
    }, "chili_exit_wrapper_fixture");
    rt.register_function(0x08D98BF8u, [](auto &r, auto &c) {
        const auto owner = psprecomp::capture_runtime_execution_context();
        const auto import_pc = c.pc, return_address = c.gpr[31];
        r.invoke_import_cached(100u, "LoadExecForUser", exit_nid, c);
        if (!r.stopped() && psprecomp::runtime_execution_context_matches(owner) && c.pc == import_pc) c.pc = return_address;
    }, "chili_exit_import_fixture");
    rt.register_function(0x0882AED0u, [](auto &, auto &) { exit_wrapper_returned = true; }, "chili_exit_return_fixture");
    rt.run(0x0882AEC0u, 10u);
    require(rt.stopped() && rt.stop_reason() == chili::guest_exit_reason && rt.cpu().pc == 0u && !exit_wrapper_returned,
        "Generated import normalization revived non-returning ExitGame");
}
} // namespace

int main() {
    try {
        fixed_abi_and_errors(); scheduler_clock(); clock_wrap(); default_rtc_consistency();
        clock_failure(); guest_exit(); actual_exit_wrapper_abi();
        std::cout << "PASS native libc/exit: all four imported NIDs; guest32 time/timeval/timezone layouts, NULL/user aliases, "
            "atomic invalid outputs, full CPU/UID preservation, actual scheduler 1000us increments and clock wrap, "
            "existing RTC consistency, strict clock-source failure, explicit PC0 exit and game-wrapper ABI verified\n";
        std::cout << "LIMIT: timezone-zero output and safe alignment/range rejection are compatibility assumptions; "
            "the PSP hardware libc fixture verifies Time NULL/invalid and microsecond clock differences, not those cases\n";
    } catch (const std::exception &error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
