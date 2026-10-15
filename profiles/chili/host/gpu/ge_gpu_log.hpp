#pragma once

#include <string_view>

namespace vcs {

// Chili's replacement for upstream's vcs_runtime_log. Info lines are dropped
// unless the "ge" channel of PSPRECOMP_LOG is on; errors always reach the sink
// but each category is capped so a per-frame failure cannot flood it.
using GeGpuLogSink = void (*)(bool error, std::string_view category, std::string_view message);

// nullptr restores the default sink (stderr for errors, stdout for info).
void set_ge_gpu_log_sink(GeGpuLogSink sink) noexcept;

void runtime_log_line(std::string_view line);
void runtime_log_error(std::string_view category, std::string_view message);

// Reports a GE state the renderer approximates or skips. The first use of each
// `what` is logged; later ones only count. Returns true on that first use.
bool runtime_log_once(std::string_view what);

} // namespace vcs
