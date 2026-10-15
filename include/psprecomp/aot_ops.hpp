#pragma once

// Helpers used only by automatically generated AOT units.
//
// Every unit is one very large function. MSVC compiles most of them without
// its optimizer (warning C4883), and there anything that is not __forceinline
// stays a call: std::array::operator[], std::bit_cast, std::isinf. The helpers
// here take raw pointers into AllegrexContext and call no library function, so
// a unit pays for them the same way at every optimization level.
//
// The *_np ("no prefix") forms are for sites where the generator has proved
// that the S, T and D prefix registers hold their defaults, because an earlier
// instruction of the same basic block consumed them. Nothing has to be applied
// there and nothing has to be reset afterwards.

#include "psprecomp/allegrex_context.hpp"

#include <type_traits>

namespace psprecomp {

template <typename T>
[[nodiscard]] PSPRECOMP_CONTEXT_FORCEINLINE std::uint32_t aot_bits(T value) noexcept {
    static_assert(std::is_same_v<T, float>);
#if defined(_MSC_VER) || defined(__clang__) || defined(__GNUC__)
    return __builtin_bit_cast(std::uint32_t, value);
#else
    return std::bit_cast<std::uint32_t>(value);
#endif
}

template <typename T>
[[nodiscard]] PSPRECOMP_CONTEXT_FORCEINLINE float aot_float(T bits) noexcept {
    static_assert(std::is_integral_v<T> && sizeof(T) == sizeof(float));
#if defined(_MSC_VER) || defined(__clang__) || defined(__GNUC__)
    return __builtin_bit_cast(float, bits);
#else
    return std::bit_cast<float>(bits);
#endif
}

template <typename T>
[[nodiscard]] PSPRECOMP_CONTEXT_FORCEINLINE bool aot_isnan(T value) noexcept {
    static_assert(std::is_same_v<T, float>);
    return value != value;
}

template <typename T>
[[nodiscard]] PSPRECOMP_CONTEXT_FORCEINLINE bool aot_isinf(T value) noexcept {
    return (aot_bits(value) & 0x7FFFFFFFu) == 0x7F800000u;
}

// MUL.S. The Allegrex FPU returns the positive quiet NaN for infinity times
// zero where the host returns the negative one. Only a NaN product can come
// from that case, so the operands are inspected on that path alone.
[[nodiscard]] PSPRECOMP_CONTEXT_FORCEINLINE float aot_mul_s(float fs, float ft) noexcept {
    const float product = fs * ft;
    if (product != product) {
        if ((aot_isinf(fs) && ft == 0.0f) || (aot_isinf(ft) && fs == 0.0f)) return aot_float(0x7FC00000u);
    }
    return product;
}

// ROUND/TRUNC/CEIL/FLOOR.W.S with a literal mode; same results as
// AllegrexContext::fpu_float_to_word_ct.
template <std::uint32_t Mode>
[[nodiscard]] PSPRECOMP_CONTEXT_FORCEINLINE std::uint32_t aot_float_to_word(float value) noexcept {
    static_assert(Mode <= 3u);
    const std::uint32_t bits = aot_bits(value);
    if ((bits & 0x7F800000u) == 0x7F800000u) return bits == 0xFF800000u ? 0x80000000u : 0x7FFFFFFFu;
    const double input = static_cast<double>(value);
    if constexpr (Mode == 1u) {
        if (input <= -2147483648.0) return 0x80000000u;
        if (input >= 2147483647.0) return 0x7FFFFFFFu;
        return static_cast<std::uint32_t>(static_cast<std::int32_t>(input));
    } else if constexpr (Mode == 0u) {
        return static_cast<std::uint32_t>(AllegrexContext::clamp_fpu_word(std::floor(input + 0.5)));
    } else if constexpr (Mode == 2u) {
        return static_cast<std::uint32_t>(AllegrexContext::clamp_fpu_word(std::ceil(input)));
    } else {
        return static_cast<std::uint32_t>(AllegrexContext::clamp_fpu_word(std::floor(input)));
    }
}

template <std::uint32_t ScalarRegister>
[[nodiscard]] PSPRECOMP_CONTEXT_FORCEINLINE std::uint32_t aot_vfpu_scalar_bits(
    const float *lanes, const std::uint32_t *control) noexcept {
    static_assert(ScalarRegister < 144u);
    if constexpr (ScalarRegister < 128u) {
        constexpr std::size_t index = AllegrexContext::vfpu_scalar_index(ScalarRegister);
        return aot_bits(lanes[index]);
    } else {
        return control[ScalarRegister - 128u];
    }
}

template <std::uint32_t ScalarRegister>
PSPRECOMP_CONTEXT_FORCEINLINE void aot_set_vfpu_scalar_bits(
    float *lanes, std::uint32_t *control, std::uint32_t value) noexcept {
    static_assert(ScalarRegister < 144u);
    if constexpr (ScalarRegister < 128u) {
        constexpr std::size_t index = AllegrexContext::vfpu_scalar_index(ScalarRegister);
        lanes[index] = aot_float(value);
    } else {
        control[ScalarRegister - 128u] = value;
    }
}

template <std::uint32_t VectorRegister, std::uint32_t Length>
PSPRECOMP_CONTEXT_FORCEINLINE void aot_vread(const float *lanes, float *destination) noexcept {
    static_assert(Length >= 1u && Length <= 4u);
    constexpr std::size_t i0 = AllegrexContext::vfpu_vector_lane_index(VectorRegister, Length, 0u);
    destination[0] = lanes[i0];
    if constexpr (Length >= 2u) {
        constexpr std::size_t i1 = AllegrexContext::vfpu_vector_lane_index(VectorRegister, Length, 1u);
        destination[1] = lanes[i1];
    }
    if constexpr (Length >= 3u) {
        constexpr std::size_t i2 = AllegrexContext::vfpu_vector_lane_index(VectorRegister, Length, 2u);
        destination[2] = lanes[i2];
    }
    if constexpr (Length >= 4u) {
        constexpr std::size_t i3 = AllegrexContext::vfpu_vector_lane_index(VectorRegister, Length, 3u);
        destination[3] = lanes[i3];
    }
}

template <std::uint32_t VectorRegister, std::uint32_t Length>
PSPRECOMP_CONTEXT_FORCEINLINE void aot_vwrite(float *lanes, const float *source) noexcept {
    static_assert(Length >= 1u && Length <= 4u);
    constexpr std::size_t i0 = AllegrexContext::vfpu_vector_lane_index(VectorRegister, Length, 0u);
    lanes[i0] = source[0];
    if constexpr (Length >= 2u) {
        constexpr std::size_t i1 = AllegrexContext::vfpu_vector_lane_index(VectorRegister, Length, 1u);
        lanes[i1] = source[1];
    }
    if constexpr (Length >= 3u) {
        constexpr std::size_t i2 = AllegrexContext::vfpu_vector_lane_index(VectorRegister, Length, 2u);
        lanes[i2] = source[2];
    }
    if constexpr (Length >= 4u) {
        constexpr std::size_t i3 = AllegrexContext::vfpu_vector_lane_index(VectorRegister, Length, 3u);
        lanes[i3] = source[3];
    }
}

// VDOT with default prefixes. The sum keeps all four terms, as the prefixed
// form does, so a short vector adds the same zero products.
template <std::uint32_t DestinationScalarRegister, std::uint32_t SourceRegister,
          std::uint32_t TargetRegister, std::uint32_t Length>
PSPRECOMP_CONTEXT_FORCEINLINE void aot_vdot_np(float *lanes) noexcept {
    float source[4]{};
    float target[4]{};
    aot_vread<SourceRegister, Length>(lanes, source);
    aot_vread<TargetRegister, Length>(lanes, target);
    const float result[1]{
        source[0] * target[0] + source[1] * target[1] +
        source[2] * target[2] + source[3] * target[3]
    };
    aot_vwrite<DestinationScalarRegister, 1u>(lanes, result);
}

// VSCL with default prefixes: every lane is scaled by the scalar register.
template <std::uint32_t DestinationRegister, std::uint32_t SourceRegister,
          std::uint32_t TargetScalarRegister, std::uint32_t Length>
PSPRECOMP_CONTEXT_FORCEINLINE void aot_vscl_np(float *lanes) noexcept {
    float source[4]{};
    aot_vread<SourceRegister, Length>(lanes, source);
    constexpr std::size_t scale_index = AllegrexContext::vfpu_scalar_index(TargetScalarRegister & 0x7Fu);
    const float scale = lanes[scale_index];
    float result[4]{};
    result[0] = source[0] * scale;
    if constexpr (Length >= 2u) result[1] = source[1] * scale;
    if constexpr (Length >= 3u) result[2] = source[2] * scale;
    if constexpr (Length >= 4u) result[3] = source[3] * scale;
    aot_vwrite<DestinationRegister, Length>(lanes, result);
}

template <std::uint32_t Condition>
[[nodiscard]] PSPRECOMP_CONTEXT_FORCEINLINE bool aot_vcmp_lane(float sv, float tv) noexcept {
    static_assert(Condition < 16u);
    if constexpr (Condition == 0u) return false;
    else if constexpr (Condition == 1u) return sv == tv;
    else if constexpr (Condition == 2u) return sv < tv;
    else if constexpr (Condition == 3u) return sv <= tv;
    else if constexpr (Condition == 4u) return true;
    else if constexpr (Condition == 5u) return sv != tv;
    else if constexpr (Condition == 6u) return sv >= tv;
    else if constexpr (Condition == 7u) return sv > tv;
    else if constexpr (Condition == 8u) return sv == 0.0f;
    else if constexpr (Condition == 9u) return aot_isnan(sv);
    else if constexpr (Condition == 10u) return aot_isinf(sv);
    else if constexpr (Condition == 11u) return aot_isnan(sv) || aot_isinf(sv);
    else if constexpr (Condition == 12u) return sv != 0.0f;
    else if constexpr (Condition == 13u) return !aot_isnan(sv);
    else if constexpr (Condition == 14u) return !aot_isinf(sv);
    else return !(aot_isnan(sv) || aot_isinf(sv));
}

template <std::uint32_t SourceRegister, std::uint32_t TargetRegister,
          std::uint32_t Length, std::uint32_t Condition>
PSPRECOMP_CONTEXT_FORCEINLINE void aot_vcmp_np(const float *lanes, std::uint32_t *control) noexcept {
    float source[4]{};
    float target[4]{};
    aot_vread<SourceRegister, Length>(lanes, source);
    aot_vread<TargetRegister, Length>(lanes, target);

    const bool r0 = aot_vcmp_lane<Condition>(source[0], target[0]);
    const bool r1 = Length >= 2u ? aot_vcmp_lane<Condition>(source[1], target[1]) : false;
    const bool r2 = Length >= 3u ? aot_vcmp_lane<Condition>(source[2], target[2]) : false;
    const bool r3 = Length >= 4u ? aot_vcmp_lane<Condition>(source[3], target[3]) : false;
    std::uint32_t lane_bits = static_cast<std::uint32_t>(r0);
    if constexpr (Length >= 2u) lane_bits |= static_cast<std::uint32_t>(r1) << 1u;
    if constexpr (Length >= 3u) lane_bits |= static_cast<std::uint32_t>(r2) << 2u;
    if constexpr (Length >= 4u) lane_bits |= static_cast<std::uint32_t>(r3) << 3u;
    bool any = r0;
    bool all = r0;
    if constexpr (Length >= 2u) { any = any || r1; all = all && r1; }
    if constexpr (Length >= 3u) { any = any || r2; all = all && r2; }
    if constexpr (Length >= 4u) { any = any || r3; all = all && r3; }

    constexpr std::uint32_t affected = ((1u << Length) - 1u) | (1u << 4u) | (1u << 5u);
    const std::uint32_t update = lane_bits | (static_cast<std::uint32_t>(any) << 4u) |
        (static_cast<std::uint32_t>(all) << 5u);
    control[3] = (control[3] & ~affected) | (update & affected);
}

template <std::uint32_t DestinationRegister, std::uint32_t SourceRegister,
          std::uint32_t Length, std::uint32_t ConditionIndex, bool MoveIfFalse>
PSPRECOMP_CONTEXT_FORCEINLINE void aot_vcmov_np(float *lanes, const std::uint32_t *control) noexcept {
    static_assert(ConditionIndex < 8u);
    float source[4]{};
    float destination[4]{};
    aot_vread<SourceRegister, Length>(lanes, source);
    aot_vread<DestinationRegister, Length>(lanes, destination);

    const std::uint32_t condition_code = control[3];
    if constexpr (ConditionIndex < 6u) {
        const bool cc = ((condition_code >> ConditionIndex) & 1u) != 0u;
        if (cc == !MoveIfFalse) {
            destination[0] = source[0];
            if constexpr (Length >= 2u) destination[1] = source[1];
            if constexpr (Length >= 3u) destination[2] = source[2];
            if constexpr (Length >= 4u) destination[3] = source[3];
        }
    } else if constexpr (ConditionIndex == 6u) {
        const bool want = !MoveIfFalse;
        if ((((condition_code >> 0u) & 1u) != 0u) == want) destination[0] = source[0];
        if constexpr (Length >= 2u)
            if ((((condition_code >> 1u) & 1u) != 0u) == want) destination[1] = source[1];
        if constexpr (Length >= 3u)
            if ((((condition_code >> 2u) & 1u) != 0u) == want) destination[2] = source[2];
        if constexpr (Length >= 4u)
            if ((((condition_code >> 3u) & 1u) != 0u) == want) destination[3] = source[3];
    }
    aot_vwrite<DestinationRegister, Length>(lanes, destination);
}

} // namespace psprecomp
