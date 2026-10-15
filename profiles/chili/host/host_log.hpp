#pragma once
#include <cstdlib>
#include <cstring>
namespace chili {
// Hot-path console tracing is off by default. PSPRECOMP_LOG enables channels by
// name ("ge", "display", "audio", "all"; comma separated). Read once per process.
enum class LogChannel : unsigned { Ge = 1u, Display = 2u, Audio = 4u };
inline unsigned log_mask() noexcept {
    static const unsigned mask = [] {
        const char *value = std::getenv("PSPRECOMP_LOG");
        if (!value || !*value) return 0u;
        unsigned bits = 0u;
        if (std::strstr(value, "all")) bits = ~0u;
        if (std::strstr(value, "ge")) bits |= static_cast<unsigned>(LogChannel::Ge);
        if (std::strstr(value, "display")) bits |= static_cast<unsigned>(LogChannel::Display);
        if (std::strstr(value, "audio")) bits |= static_cast<unsigned>(LogChannel::Audio);
        return bits;
    }();
    return mask;
}
inline bool log_enabled(LogChannel channel) noexcept {
    return (log_mask() & static_cast<unsigned>(channel)) != 0u;
}
}
