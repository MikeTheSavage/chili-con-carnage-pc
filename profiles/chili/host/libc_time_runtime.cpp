#include "libc_time_runtime.hpp"
#include "thread_runtime.hpp"
#include "psprecomp/common.hpp"
#include <chrono>
#include <limits>
#include <utility>

namespace chili {
namespace {
bool output_valid(const psprecomp::GuestMemory &memory, std::uint32_t address,
    std::size_t bytes) {
    // NULL outputs are optional. Reject kernel aliases before GuestMemory's
    // canonicalization, as the UtilsForUser firmware K1 pointer check does.
    // Native range/alignment checks also prevent partial/unaligned host writes.
    return address == 0u || ((address & 0x80000003u) == 0u && memory.contains(address, bytes));
}
}

std::uint64_t host_unix_microseconds() {
    const auto value = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (value < 0) throw psprecomp::Error("Native clock is earlier than the Unix epoch");
    return static_cast<std::uint64_t>(value);
}

UnixMicrosecondsSource make_scheduler_time_source(ThreadRuntime &threads,
    std::uint64_t initial_unix_microseconds) {
    const auto initial_virtual_microseconds = threads.now_us();
    return [&threads, initial_unix_microseconds, initial_virtual_microseconds] {
        const auto current = threads.now_us();
        if (current < initial_virtual_microseconds)
            throw psprecomp::Error("Chili scheduler clock moved before its wall-time anchor");
        const auto elapsed = current - initial_virtual_microseconds;
        if (elapsed > std::numeric_limits<std::uint64_t>::max() - initial_unix_microseconds)
            throw psprecomp::Error("Chili scheduler wall clock overflow");
        return initial_unix_microseconds + elapsed;
    };
}

LibcTimeRuntime::LibcTimeRuntime(psprecomp::Runtime &runtime, UnixMicrosecondsSource source)
    : source_(source ? std::move(source) : UnixMicrosecondsSource{&host_unix_microseconds}) {
    runtime.register_hle("UtilsForUser", 0x27CC57F0u,
        [this](auto &rt, auto &ctx) { time(rt, ctx); });
    runtime.register_hle("UtilsForUser", 0x91E4F6A7u,
        [this](auto &rt, auto &ctx) { clock(rt, ctx); });
    runtime.register_hle("UtilsForUser", 0x71EC4271u,
        [this](auto &rt, auto &ctx) { gettimeofday(rt, ctx); });
}

std::uint64_t LibcTimeRuntime::unix_microseconds() const {
    const auto value = source_();
    if (value > std::numeric_limits<std::uint64_t>::max() - rtc_unix_epoch_offset)
        throw psprecomp::Error("Native clock exceeds the PSP RTC tick range");
    return value;
}

std::uint64_t LibcTimeRuntime::rtc_tick() const {
    return unix_microseconds() + rtc_unix_epoch_offset;
}

bool LibcTimeRuntime::sample(psprecomp::Runtime &runtime, std::uint64_t &value) const {
    try {
        value = unix_microseconds();
        return true;
    } catch (const std::exception &error) {
        runtime.stop(std::string("Native libc clock failed: ") + error.what());
        return false;
    }
}

void LibcTimeRuntime::time(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const auto output = ctx.gpr[4];
    if (!output_valid(runtime.memory(), output, 4u)) {
        // Hardware autotest: sceKernelLibcTime((s32 *)0xDEADBEEF) returns 0.
        ctx.set_gpr(2u, 0u);
        return;
    }
    std::uint64_t value{};
    if (!sample(runtime, value)) return;
    const auto seconds = static_cast<std::uint32_t>(value / 1000000ull);
    if (output) runtime.memory().store32(output, seconds);
    ctx.set_gpr(2u, seconds);
}

void LibcTimeRuntime::clock(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    std::uint64_t value{};
    if (!sample(runtime, value)) return;
    // UOFW's RTC clock callback returns the low word of sceRtcGetCurrentTick.
    // Hardware libc tests confirm microsecond increments across thread sleeps.
    ctx.set_gpr(2u, static_cast<std::uint32_t>(value + rtc_unix_epoch_offset));
}

void LibcTimeRuntime::gettimeofday(psprecomp::Runtime &runtime,
    psprecomp::AllegrexContext &ctx) {
    const auto output = ctx.gpr[4], timezone = ctx.gpr[5];
    if (!output_valid(runtime.memory(), output, 8u) ||
        !output_valid(runtime.memory(), timezone, 8u)) {
        // The firmware checks both user pointers before invoking its RTC
        // callback and returns 1 on a failed pointer check, not a kernel NID error.
        ctx.set_gpr(2u, 1u);
        return;
    }
    std::uint64_t value{};
    if (!sample(runtime, value)) return;
    if (output) {
        runtime.memory().store32(output, static_cast<std::uint32_t>(value / 1000000ull));
        runtime.memory().store32(output + 4u, static_cast<std::uint32_t>(value % 1000000ull));
    }
    if (timezone) {
        // PSP compatibility references report both deprecated timezone fields
        // as zero. This is not the PC timezone; local RTC time is a separate API.
        // Non-NULL timezone behavior remains unverified by our hardware fixture.
        runtime.memory().store32(timezone, 0u);
        runtime.memory().store32(timezone + 4u, 0u);
    }
    ctx.set_gpr(2u, 0u);
}

void register_load_exec_exit(psprecomp::Runtime &runtime) {
    runtime.register_hle("LoadExecForUser", 0x05572A5Fu,
        [](auto &rt, auto &ctx) {
            ctx.pc = 0u;
            rt.stop(guest_exit_reason);
        });
}
} // namespace chili
