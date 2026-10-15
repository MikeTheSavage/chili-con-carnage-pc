#pragma once
#include "input_device.hpp"
#include <cstddef>
#include <cstdint>
namespace chili {
// One gamepad's state, whatever kind of pad it is and however it was read (XInput for Xbox-style pads,
// Raw Input and the HID parser for PlayStation pads). Bindings, dead zones and the stick response apply
// to it unchanged.
//
// buttons: bit i is logical button i. The first fourteen are the buttons of PcInputConfig::pad_button_names
// in its order; bits 14 and 15 are the two triggers as buttons, set once a threshold has been applied
// (PadSample::with_triggers). A reader leaves them clear and fills lt and rt.
// lt, rt: trigger travel 0 to 1. lx, ly, rx, ry: stick travel -1 to 1, right and down positive.
// packet: a number that changes when the pad's state does (XInput's packet number, a HID pad's report count).
struct PadSample {
    std::uint16_t buttons{};
    float lt{}, rt{}, lx{}, ly{}, rx{}, ry{};
    std::uint32_t packet{};
    PadKind kind{PadKind::None};
    std::uint16_t vid{}, pid{};

    // Logical buttons by position on an Xbox-style pad. A PlayStation pad maps by position:
    // Cross = A, Circle = B, Square = X, Triangle = Y, L1 = LB, R1 = RB, L2 = LT, R2 = RT,
    // Share or Create = Back, Options = Start, L3 = LS, R3 = RS.
    enum Button : unsigned { DpadUp, DpadDown, DpadLeft, DpadRight, Start, Back, LS, RS, LB, RB, A, B, X, Y, LT, RT, ButtonCount };
    static constexpr std::uint16_t bit(unsigned button) noexcept { return static_cast<std::uint16_t>(1u << button); }
    static constexpr std::uint16_t trigger_bits = static_cast<std::uint16_t>((1u << LT) | (1u << RT));

    // All sixteen logical buttons: a trigger counts as pressed beyond `threshold` of its travel.
    constexpr std::uint16_t with_triggers(float threshold) const noexcept {
        return static_cast<std::uint16_t>((buttons & ~trigger_bits) | (lt > threshold ? bit(LT) : 0u) | (rt > threshold ? bit(RT) : 0u));
    }
};
inline const char *pad_kind_name(PadKind kind) noexcept {
    return kind == PadKind::Xbox ? "xbox" : kind == PadKind::PlayStation ? "playstation" : "none";
}
}
