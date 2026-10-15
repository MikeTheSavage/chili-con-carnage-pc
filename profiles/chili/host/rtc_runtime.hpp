#pragma once
#include "psprecomp/runtime.hpp"
#include "libc_time_runtime.hpp"
namespace chili {
// With no source the RTC reads the host system clock and local time zone. A
// source (Unix microseconds, for instance make_scheduler_time_source) replaces
// both; local time is then reported as UTC so nothing depends on the host.
void register_rtc(psprecomp::Runtime &, UnixMicrosecondsSource source = {});
}
