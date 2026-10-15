#include "ge_blend.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

using chili::ge::blend::prepare;
using chili::ge::blend::rgb;

namespace {
unsigned checks = 0;
void check(bool condition, const char *label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
}

struct Vector {
    const char *label;
    std::uint32_t mode, destination, source, fixed_a, fixed_b, expected;
};

// Numeric facts transcribed from pspautotests gpu/commands/blend.expected.
// The upstream test replaces stencil with AA. Compare RGB only: this helper
// cannot claim to implement or validate that separate stencil operation.
constexpr std::array<Vector, 10> recorded {{
    {"One + Zero", 0xAA, 0x44444444, 0xEEDDCCBB, 0xFFFFFF, 0, 0xDDCCBB},
    {"Zero + One", 0xAA, 0x44444444, 0xEEDDCCBB, 0, 0xFFFFFF, 0x444444},
    {"Fix + Zero", 0xAA, 0x50404040, 0x90808080, 0x808080, 0, 0x404040},
    {"Src alpha + Zero", 0xA2, 0x50404040, 0x90808080, 0, 0, 0x484848},
    {"F0 + 0F", 0xAA, 0xFFFFFFFF, 0xFFFFFFFF, 0xF0F0F0, 0x0F0F0F, 0xFFFFFF},
    {"0F + F0", 0xAA, 0xFFFFFFFF, 0xFFFFFFFF, 0x0F0F0F, 0xF0F0F0, 0xFFFFFF},
    {"F0 - 0F", 0x1AA, 0xFFFFFFFF, 0xFFFFFFFF, 0xF0F0F0, 0x0F0F0F, 0xE1E1E1},
    {"0F - F0", 0x1AA, 0xFFFFFFFF, 0xFFFFFFFF, 0x0F0F0F, 0xF0F0F0, 0},
    {"Reverse F0 - 0F", 0x2AA, 0xFFFFFFFF, 0xFFFFFFFF, 0xF0F0F0, 0x0F0F0F, 0},
    {"Reverse 0F - F0", 0x2AA, 0xFFFFFFFF, 0xFFFFFFFF, 0x0F0F0F, 0xF0F0F0, 0xE1E1E1},
}};

void recorded_oracle() {
    for (const auto &v : recorded) {
        const auto state = prepare(true, v.mode, v.fixed_a, v.fixed_b);
        check(rgb(state, v.source, v.destination) == v.expected, v.label);
    }
    std::cout << "Recorded upstream PSP autotest oracle: 10 RGB vectors passed\n";
}

void boundaries() {
    // Reference arithmetic regressions, not additional captured hardware values.
    const auto half = prepare(true, 0xAA, 0x7F7F7F, 0);
    check(rgb(half, 0x00010101, 0) == 0, "half factor tiny source truncates");
    check(rgb(half, 0x00020202, 0) == 0x010101, "half factor next source step");
    check(rgb(half, 0x00FFFFFF, 0) == 0x7F7F7F, "127 factor endpoint");
    check(rgb(prepare(true, 0xAA, 0x808080, 0), 0x00FFFFFF, 0) == 0x808080,
          "128 factor endpoint");
    check(rgb(prepare(true, 0xAA, 0x7F7F7F, 0x7F7F7F), 0x00010101, 0x00010101) == 0,
          "truncate each term before add");
    check(rgb(prepare(true, 0xAA, 0x402010, 0), 0x00FFFFFF, 0) == 0x402010,
          "independent RGB fixed channels");
    check(rgb(prepare(true, 0x2AA, 0, 0x102040), 0, 0x00FFFFFF) == 0x102040,
          "independent RGB reverse channels");
    const auto alpha = prepare(true, 0xA2, 0x123456, 0);
    check(rgb(alpha, 0x00ABCDEF, 0xFF112233) == 0, "zero source alpha");
    check(rgb(alpha, 0xFFABCDEF, 0x00112233) == 0xABCDEF, "full source alpha");
    check(rgb(prepare(true, 0xA2, 0, 0xFFFFFF), 0xFF100101, 0xAA01FF80) == 0x11FF81,
          "A2 white destination factor and channel saturation");
    check(rgb(prepare(true, 0x1AA, 0xFFFFFF, 0xFFFFFF), 0xAA807F00, 0xBB017F01) == 0x7F0000,
          "signed subtraction clamps each channel");
    check(rgb(prepare(true, 0x2AA, 0xFFFFFF, 0xFFFFFF), 0xAA807F00, 0xBB017F01) == 1,
          "signed reverse subtraction clamps each channel");

    // Exhaustive endpoint identities are algebraic checks, not hardware sweep claims.
    for (unsigned c = 0; c != 256; ++c) {
        const auto color = c * 0x010101u;
        check(rgb(prepare(true, 0xAA, 0xFFFFFF, 0), 0xAA000000u | color, 0xBBFFFFFF) == color,
              "all channel values times one");
        check(rgb(prepare(true, 0xAA, 0, 0), 0xAA000000u | color, 0xBB000000u | color) == 0,
              "all channel values times zero");
    }
    std::cout << "Reference boundary and identity checks passed\n";
}

void ownership_and_decode() {
    const auto off = prepare(false, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF);
    check(!off.enabled(), "disabled state");
    check(rgb(off, 0xABCDEF01, 0x12345678) == 0xCDEF01, "OFF returns source RGB only");
    const auto fixed = prepare(true, 0xAA, 0xAAFFFFFF, 0);
    check(fixed.enabled() && fixed.mode() == 0xAA, "decoded known state");
    check(rgb(fixed, 0x01ABCDEF, 0x12123456) == rgb(fixed, 0xFEABCDEF, 0xFF123456),
          "FIX ignores source and destination alpha");
    check((rgb(fixed, 0xFEABCDEF, 0xFF123456) & 0xFF000000u) == 0, "no alpha output");

    // Staging failure leaves the caller's prior prepared state intact.
    auto state = prepare(true, 0xAA, 0xFFFFFF, 0);
    const auto original = rgb(state, 0x22ABCDEF, 0x99887766);
    for (const std::uint32_t mode : {0x32u, 0xBAu, 0x3AAu, 0x4AAu, 0x5AAu,
                                   0x1000AAu, 0xDF0000AAu, 0xFFFFFFFFu}) {
        bool threw = false;
        try { state = prepare(true, mode, 0, 0); }
        catch (const std::runtime_error &) { threw = true; }
        check(threw, "unsupported mode must fail preflight");
        check(state.mode() == 0xAA && rgb(state, 0x22ABCDEF, 0x99887766) == original,
              "failed decode preserves staged state");
    }
    std::cout << "Strict decode, OFF, legacy32 separation and RGB ownership passed\n";
}
} // namespace

int main() {
    try {
        recorded_oracle();
        boundaries();
        ownership_and_decode();
        std::cout << "GE blend preparation passed: " << checks << " checks\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "GE blend test failed: " << error.what() << '\n';
        return 1;
    }
}
