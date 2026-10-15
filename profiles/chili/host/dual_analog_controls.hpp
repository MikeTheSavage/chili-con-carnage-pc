#pragma once
#include "controller_runtime.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>
namespace chili {
// The engine's own two-stick control scheme.
//
// The game's player script asks its control setup for run_left, run_right,
// look_up and look_down, and only falls back to the one-stick PSP behaviour
// (stick X turns, L strafes, Circle plus stick Y looks) because the PSP game
// data does not define those controls. Here the host defines them, and takes
// look_left and look_right away from the left stick:
//
//   run_left, run_right     left stick X      strafe
//   look_left, look_right   right stick X     turn
//   look_up, look_down      right stick Y     pitch
//
// A control is a pressure with the engine's own stick dead zone (the first 30%
// of travel), computed as Kapow's Control does for a stick direction. The run
// controls go from 0 to 1 like every control the game has. The look controls
// are a rate: the player script asks for a turn of 180 degrees per second
// times the pressure squared, of which the player turns 95% (measured: 5.54
// degrees in a 33 ms frame for 176.8), and pitches the view at one radian per
// second times the pressure cubed. It does not clamp the pressure. The PSP
// stick stops at 1; the second stick is the host's, and its full travel stands
// for look_gain, which lets a mouse turn faster than a thumb stick. Everything
// else in the game's control data is untouched.
struct DualAnalogControls {
    enum Control : unsigned { RunLeft, RunRight, LookUp, LookDown, LookLeft, LookRight, Count };
    // Ids the game is given for these controls; its own ids are small list indices.
    static constexpr std::uint32_t first_id = 0x7F00u;
    static constexpr float dead_zone = 0.3f;
    static constexpr float turn_degrees_per_second = 171.0f, pitch_degrees_per_second = 57.29578f;
    static constexpr float look_gain = 3.0f;
    struct Answer { float pressure{}, real_pressure{}; bool pressed{}, released{}; };

    // Index of a host-defined control, or Count. Control names in the game data
    // are matched without regard to case.
    static unsigned from_name(std::string_view name) noexcept {
        static constexpr std::array<std::string_view, Count> names{"run_left", "run_right", "look_up", "look_down", "look_left", "look_right"};
        for (unsigned i = 0; i < Count; ++i) {
            if (names[i].size() != name.size()) continue;
            bool same = true;
            for (std::size_t c = 0; c < name.size() && same; ++c)
                same = (name[c] >= 'A' && name[c] <= 'Z' ? static_cast<char>(name[c] - 'A' + 'a') : name[c]) == names[i][c];
            if (same) return i;
        }
        return Count;
    }
    static bool is_id(std::uint32_t id) noexcept { return id - first_id < Count; }

    // The engine's stick byte to axis value: -1 to +0.992.
    static float raw(std::uint8_t byte) noexcept { return static_cast<float>(byte) * 0.0078125f - 1.0f; }
    static float pressure_of(float force) noexcept { return std::max(0.0f, (force - dead_zone) / (1.0f - dead_zone)); }
    // Degrees per second the game turns (positive: right) or pitches the view
    // (positive: down) for a second-stick byte.
    static float turn_of(std::uint8_t byte) noexcept {
        const float pressure = pressure_of(std::fabs(raw(byte))) * look_gain;
        return std::copysign(turn_degrees_per_second * pressure * pressure, raw(byte));
    }
    static float pitch_of(std::uint8_t byte) noexcept {
        const float pressure = pressure_of(std::fabs(raw(byte))) * look_gain;
        return std::copysign(pitch_degrees_per_second * pressure * pressure * pressure, raw(byte));
    }
    // The second-stick byte whose rate is nearest to the wanted one.
    static std::uint8_t byte_for_turn(float degrees_per_second) noexcept {
        return byte_for(std::sqrt(std::fabs(degrees_per_second) / turn_degrees_per_second), degrees_per_second);
    }
    static std::uint8_t byte_for_pitch(float degrees_per_second) noexcept {
        return byte_for(std::cbrt(std::fabs(degrees_per_second) / pitch_degrees_per_second), degrees_per_second);
    }

    // The pad state the guest was just handed; one call per controller sample.
    void sampled(ControllerInput input) noexcept {
        input_ = input;
        if (look_queried_) { look_idle_ = 0u; if (look_run_ < look_run_needed) ++look_run_; }
        else if (look_idle_ + 1u >= look_idle_limit) { look_idle_ = look_idle_limit; look_run_ = 0u; }
        else ++look_idle_;
        look_queried_ = false;
    }
    // Whether the game has asked for a look control since the last sample.
    bool look_queried() const noexcept { return look_queried_; }
    ControllerInput input() const noexcept { return input_; }
    // The right stick as the engine's pad object would report a second axis.
    float axis_x() const noexcept { return raw(input_.rx); }
    float axis_y() const noexcept { return raw(input_.ry); }
    // How far the stick is pushed in a control's direction, 0 to 1.
    float force(unsigned control) const noexcept {
        switch (control) {
        case RunLeft: return std::max(0.0f, -raw(input_.x));
        case RunRight: return std::max(0.0f, raw(input_.x));
        case LookUp: return std::max(0.0f, -raw(input_.ry));
        case LookDown: return std::max(0.0f, raw(input_.ry));
        case LookLeft: return std::max(0.0f, -raw(input_.rx));
        case LookRight: return std::max(0.0f, raw(input_.rx));
        default: return 0.0f;
        }
    }
    // One query by the game, with the engine's edge rule: "pressed" is the
    // first query that finds the control down, "released" the first that finds
    // it up again.
    Answer query(unsigned control, bool enabled = true) noexcept {
        Answer answer;
        if (control >= Count) return answer;
        const bool look = control >= LookUp;
        if (look) look_queried_ = true;
        if (enabled) {
            const float gain = look ? look_gain : 1.0f;
            answer.real_pressure = force(control) * gain;
            answer.pressure = pressure_of(force(control)) * gain;
        }
        const bool down = answer.pressure > 0.0f;
        answer.pressed = down && !down_[control];
        answer.released = !down && down_[control];
        down_[control] = down;
        return answer;
    }
    // True while the game keeps asking for the look controls. It has to ask in
    // four samples before that counts: while it starts, and when a mission has
    // just been loaded, it asks for a sample or two without anybody playing.
    // False again once it has stopped asking for eight samples: menus,
    // cutscenes, the pause screen.
    static constexpr unsigned look_run_needed = 4u, look_idle_limit = 8u;
    bool look_active() const noexcept { return look_idle_ < look_idle_limit && look_run_ >= look_run_needed; }

private:
    // Stick travel is counted in 128ths; the first step past the dead zone is 39.
    static std::uint8_t byte_for(float pressure, float sign) noexcept {
        constexpr int first = 39, last = 127;
        const float wanted = std::min(pressure / look_gain, 1.0f);
        const float lowest = pressure_of(static_cast<float>(first) * 0.0078125f);
        if (!(wanted > lowest * 0.5f)) return 128u;
        const int steps = std::clamp(static_cast<int>(std::lround(128.0f * (dead_zone + (1.0f - dead_zone) * wanted))), first, last);
        return static_cast<std::uint8_t>(sign < 0.0f ? 128 - steps : 128 + steps);
    }
    ControllerInput input_{};
    std::array<bool, Count> down_{};
    unsigned look_idle_{look_idle_limit}, look_run_{};
    bool look_queried_{};
};
}
