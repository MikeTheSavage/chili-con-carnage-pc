#pragma once

#include "psprecomp/runtime.hpp"
#include <functional>

namespace chili {
class ThreadRuntime;

// A single sampled Unix microsecond clock can also be shared with the RTC HLE.
// Guest time_t, clock_t, timeval and timezone remain 32-bit, irrespective of
// the host C runtime's types. These imports never schedule or advance time.
using UnixMicrosecondsSource = std::function<std::uint64_t()>;
inline constexpr std::uint64_t rtc_unix_epoch_offset = 62135596800000000ull;
std::uint64_t host_unix_microseconds();

// Anchor wall time to the existing scheduler without changing its clock or
// current guest. The scheduler must outlive the returned callback.
UnixMicrosecondsSource make_scheduler_time_source(ThreadRuntime &threads,
    std::uint64_t initial_unix_microseconds);

class LibcTimeRuntime {
public:
    explicit LibcTimeRuntime(psprecomp::Runtime &runtime,
        UnixMicrosecondsSource source = {});
    LibcTimeRuntime(const LibcTimeRuntime &) = delete;
    LibcTimeRuntime &operator=(const LibcTimeRuntime &) = delete;

    [[nodiscard]] std::uint64_t unix_microseconds() const;
    [[nodiscard]] std::uint64_t rtc_tick() const;

private:
    bool sample(psprecomp::Runtime &, std::uint64_t &) const;
    void time(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    void clock(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    void gettimeofday(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    UnixMicrosecondsSource source_;
};

// Bind only ExitGame. ThreadRuntime owns RegisterExitCallback and delivery of
// the player's actual window-close request. No input or exit is synthesized here.
inline constexpr char guest_exit_reason[] = "Guest requested exit through sceKernelExitGame";
void register_load_exec_exit(psprecomp::Runtime &runtime);
} // namespace chili
