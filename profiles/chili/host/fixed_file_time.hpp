#pragma once
#include <ctime>
#include <optional>
namespace chili {
// Deterministic runs hand the guest this one timestamp for every host file, so
// a save written during a run cannot make a later listing differ between runs.
inline std::optional<__time64_t> fixed_file_time;
}
