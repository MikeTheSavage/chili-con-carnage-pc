#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace chili::ge::blend {

// These are GE register payloads, not complete DF/E0/E1 command words.
// This helper deliberately does not own framebuffer alpha/stencil or write masks.
class PreparedBlend;
inline PreparedBlend prepare(bool enabled, std::uint32_t mode,
                             std::uint32_t fixed_a, std::uint32_t fixed_b);
inline std::uint32_t rgb(const PreparedBlend &state, std::uint32_t source_rgba,
                         std::uint32_t destination_rgba) noexcept;

class PreparedBlend {
public:
    bool enabled() const noexcept { return enabled_; }
    std::uint32_t mode() const noexcept { return mode_; }

private:
    PreparedBlend(bool enabled, std::uint32_t mode, std::uint32_t fixed_a,
                  std::uint32_t fixed_b) noexcept
        : enabled_(enabled), mode_(mode), fixed_a_(fixed_a & 0xFFFFFFu),
          fixed_b_(fixed_b & 0xFFFFFFu) {}

    bool enabled_;
    std::uint32_t mode_;
    std::uint32_t fixed_a_;
    std::uint32_t fixed_b_;
    friend PreparedBlend prepare(bool, std::uint32_t, std::uint32_t, std::uint32_t);
    friend std::uint32_t rgb(const PreparedBlend &, std::uint32_t, std::uint32_t) noexcept;
};

// Decode once during draw preflight, before preparing or committing any pixels.
// Existing mode 0x32 remains the caller's legacy path and is rejected here.
inline PreparedBlend prepare(bool enabled, std::uint32_t mode,
                             std::uint32_t fixed_a, std::uint32_t fixed_b) {
    if (enabled && ((mode & ~0x7FFu) || ((mode >> 8) & 7u) > 2u ||
                    (mode & 15u) > 10u || ((mode >> 4) & 15u) > 10u ||
                    mode == 0x32u)) {
        throw std::runtime_error("unsupported native GE blend mode");
    }
    return PreparedBlend(enabled, mode, fixed_a, fixed_b);
}

// Independently written from the documented integer arithmetic and recorded
// PSP autotest values. Each scaled term is truncated before the equation.
inline std::uint32_t rgb(const PreparedBlend &state, std::uint32_t source_rgba,
                         std::uint32_t destination_rgba) noexcept {
    if (!state.enabled_) return source_rgba & 0xFFFFFFu;

    std::uint32_t result = 0;
    for (unsigned channel = 0; channel != 3; ++channel) {
        const unsigned shift = channel * 8;
        const std::uint32_t source = (source_rgba >> shift) & 255u;
        const std::uint32_t destination = (destination_rgba >> shift) & 255u;
        // Independently stated PSPSDK factor meanings. Doubling retains values
        // above 255 until the scaled term/equation; inverse doubling clips at 0.
        const auto factor = [&](unsigned which, unsigned other, unsigned fixed) {
            const unsigned source_alpha = source_rgba >> 24;
            const unsigned destination_alpha = destination_rgba >> 24;
            switch (which) {
            case 0: return other;
            case 1: return 255u - other;
            case 2: return source_alpha;
            case 3: return 255u - source_alpha;
            case 4: return destination_alpha;
            case 5: return 255u - destination_alpha;
            case 6: return source_alpha * 2u;
            case 7: return source_alpha < 128u ? 255u - source_alpha * 2u : 0u;
            case 8: return destination_alpha * 2u;
            case 9: return destination_alpha < 128u ? 255u - destination_alpha * 2u : 0u;
            default: return fixed; // Only factor 10 can remain after prepare.
            }
        };
        const std::uint32_t source_factor = factor(state.mode_ & 15u, destination,
                                                   (state.fixed_a_ >> shift) & 255u);
        const std::uint32_t destination_factor = factor((state.mode_ >> 4) & 15u, source,
                                                        (state.fixed_b_ >> shift) & 255u);
        const int source_term = static_cast<int>(((source * 2u + 1u) *
                                                   (source_factor * 2u + 1u)) >> 10);
        const int destination_term = static_cast<int>(((destination * 2u + 1u) *
                                                        (destination_factor * 2u + 1u)) >> 10);
        int combined = source_term + destination_term;
        if (((state.mode_ >> 8) & 7u) == 1u) combined = source_term - destination_term;
        if (((state.mode_ >> 8) & 7u) == 2u) combined = destination_term - source_term;
        result |= static_cast<std::uint32_t>(std::clamp(combined, 0, 255)) << shift;
    }
    return result;
}

} // namespace chili::ge::blend
