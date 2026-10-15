#pragma once
// Independent attribute deformation, before world/view/projection and lighting.
// Hardware oracles and remaining precision limits are documented in deformation/README.md.
#include "ge_vertex_decoder.hpp"
#include <string>
#include <utility>

namespace chili::ge {
using BoneMatrix = std::array<float, 12>; // Four column vectors of three rows.
struct DeformState {
    std::array<float, 8> morph_weights{};
    std::array<BoneMatrix, 8> bones{};
};
struct DeformedVertex {
    std::uint32_t source_index{};
    Sample attributes; // Missing UV/color/normal remain missing; weights remain source data.
};
struct DeformedDraw {
    Layout source_layout;
    std::vector<DeformedVertex> vertices;
    std::uint32_t next_vertex_address{}, next_index_address{};
};
// GE register arguments contain the high 24 IEEE754 bits. The command byte is ignored.
inline std::array<float, 8> deformation_morph_weights(const std::array<std::uint32_t, 256> &commands) {
    std::array<float, 8> result{};
    for (unsigned n = 0; n < 8; ++n) result[n] = std::bit_cast<float>((commands[0x2C + n] & 0xFFFFFFu) << 8);
    return result;
}
namespace deform_detail {
inline float finite(float value) {
    if (!std::isfinite(value)) throw std::runtime_error("GE deformation has nonfinite input/result; hardware handling is unverified");
    return value;
}
inline float product(float a, float b) { return finite(finite(a) * finite(b)); }
inline float sum(float a, float b) { return finite(finite(a) + finite(b)); }
template<std::size_t N> inline void validate(const std::array<float, N> &values) {
    for (float value : values) finite(value);
}
inline void validate_sample(const Layout &layout, const Sample &sample) {
    if (sample.uv.has_value() != (layout.uv_type != 0) ||
        sample.color.has_value() != (layout.color_type != 0) ||
        sample.normal.has_value() != (layout.normal_type != 0))
        throw std::runtime_error("GE deformation source attribute presence disagrees with its layout");
    validate(sample.position);
    if (sample.uv) validate(*sample.uv);
    if (sample.normal) validate(*sample.normal);
    for (unsigned n = 0; n < layout.weight_count; ++n) finite(sample.weights[n]);
}
inline std::uint8_t channel(float value) {
    finite(value);
    // Clamp before conversion so large finite values never cause an out-of-range cast.
    return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 255.0f));
}
// Recover the original packed channel bits from the decoder's lossless bit replication.
// Morph RGB5551 uses a normalized 5-bit value, which differs from bit replication.
inline std::array<float, 4> morph_color(const Color &color, unsigned type) {
    if (type == 5) {
        std::array<float, 4> result{};
        for (unsigned n = 0; n < 3; ++n) {
            unsigned raw = color[n] >> 3;
            if (color[n] != ((raw << 3) | (raw >> 2)))
                throw std::runtime_error("GE packed morph color has lost its original source bits");
            result[n] = float(raw) * (255.0f / 31.0f);
        }
        if (color[3] != 0 && color[3] != 255)
            throw std::runtime_error("GE 5551 source alpha is not a packed endpoint");
        result[3] = float(color[3]);
        return result;
    }
    if (type == 6) {
        for (auto channel : color) if (channel % 17)
            throw std::runtime_error("GE 4444 morph color has lost its original source bits");
    }
    if (type != 6 && type != 7)
        throw std::runtime_error("GE 565 morph alpha awaits a hardware oracle");
    return {float(color[0]), float(color[1]), float(color[2]), float(color[3])};
}
inline Sample morph(const Layout &layout, const Vertex &vertex, const DeformState &state) {
    if (layout.morph_count == 1) return vertex.samples[0]; // MorphWeight0 is ignored by hardware.
    Sample result{};
    if (layout.uv_type) result.uv = Vec2{};
    if (layout.normal_type) result.normal = Vec3{};
    std::array<float, 4> color{};
    for (unsigned n = 0; n < layout.morph_count; ++n) {
        const auto &sample = vertex.samples[n];
        float weight = state.morph_weights[n];
        // PSP morphs declared bone weights before constructing the skin matrix,
        // as described by the original PSPSDK morphskin sample author. Preserve
        // their linear sum: no unit-sum normalization or extra clamping.
        if (layout.needs_skinning()) for (unsigned bone = 0; bone < layout.weight_count; ++bone)
            result.weights[bone] = sum(result.weights[bone], product(sample.weights[bone], weight));
        for (unsigned axis = 0; axis < 3; ++axis)
            result.position[axis] = sum(result.position[axis], product(sample.position[axis], weight));
        if (result.uv) for (unsigned axis = 0; axis < 2; ++axis)
            (*result.uv)[axis] = sum((*result.uv)[axis], product((*sample.uv)[axis], weight));
        if (result.normal) for (unsigned axis = 0; axis < 3; ++axis)
            (*result.normal)[axis] = sum((*result.normal)[axis], product((*sample.normal)[axis], weight));
        if (layout.color_type) {
            const auto source = morph_color(*sample.color, layout.color_type);
            for (unsigned channel = 0; channel < 4; ++channel)
                color[channel] = sum(color[channel], product(source[channel], weight));
        }
    }
    if (layout.color_type) result.color = Color{channel(color[0]), channel(color[1]), channel(color[2]), channel(color[3])};
    return result;
}
inline Sample skin(const Layout &layout, const Sample &source, const DeformState &state) {
    BoneMatrix combined{};
    for (unsigned bone = 0; bone < layout.weight_count; ++bone) {
        float weight = source.weights[bone];
        for (unsigned element = 0; element < 12; ++element)
            combined[element] = sum(combined[element], product(state.bones[bone][element], weight));
    }
    Sample result = source;
    for (unsigned row = 0; row < 3; ++row) {
        float p = product(combined[row], source.position[0]);
        p = sum(p, product(combined[3 + row], source.position[1]));
        p = sum(p, product(combined[6 + row], source.position[2]));
        result.position[row] = sum(p, combined[9 + row]);
        if (source.normal) {
            float normal = product(combined[row], (*source.normal)[0]);
            normal = sum(normal, product(combined[3 + row], (*source.normal)[1]));
            (*result.normal)[row] = sum(normal, product(combined[6 + row], (*source.normal)[2]));
        }
    }
    return result;
}
} // namespace deform_detail

// Performs no guest/framebuffer writes. A late malformed vertex throws before any
// returned draw/cursor can be committed. Callers still validate GE lighting/UV/raster state.
// Arithmetic uses ordinary binary32, matching the native bring-up renderer policy;
// PSP intermediate float24 rounding is not established by the morph image oracle.
inline DeformedDraw prepare_deformed_draw(const Draw &source, const DeformState &state) {
    const auto &layout = source.layout;
    auto expected = make_layout(layout.vtype);
    if (layout.morph_count != expected.morph_count || layout.weight_count != expected.weight_count ||
        layout.uv_type != expected.uv_type || layout.color_type != expected.color_type ||
        layout.normal_type != expected.normal_type || layout.position_type != expected.position_type ||
        layout.weight_type != expected.weight_type || layout.through != expected.through)
        throw std::runtime_error("GE deformation received an inconsistent source layout");
    if (layout.through && layout.needs_morphing())
        throw std::runtime_error("GE through+morph behavior awaits a hardware oracle");

    if (layout.needs_morphing()) {
        for (unsigned n = 0; n < layout.morph_count; ++n) deform_detail::finite(state.morph_weights[n]);
        if (layout.color_type == 4) throw std::runtime_error("GE 565 morph alpha awaits a hardware oracle");
    }
    if (layout.needs_skinning())
        for (unsigned n = 0; n < layout.weight_count; ++n) deform_detail::validate(state.bones[n]);
    DeformedDraw result{};
    result.source_layout = layout;
    result.next_vertex_address = source.next_vertex_address;
    result.next_index_address = source.next_index_address;
    result.vertices.reserve(source.vertices.size());
    for (const auto &vertex : source.vertices) {
        for (unsigned n = 0; n < layout.morph_count; ++n)
            deform_detail::validate_sample(layout, vertex.samples[n]);
        auto attributes = deform_detail::morph(layout, vertex, state);
        if (layout.needs_skinning()) attributes = deform_detail::skin(layout, attributes, state);
        result.vertices.push_back({vertex.source_index, std::move(attributes)});
    }
    return result;
}
// An adapter for bool/error renderers. Output is assigned only after the whole draw succeeds.
inline bool try_prepare_deformed_draw(const Draw &source, const DeformState &state,
                                     DeformedDraw &result, std::string &error) {
    try { auto prepared = prepare_deformed_draw(source, state); result = std::move(prepared); return true; }
    catch (const std::exception &failure) { error = failure.what(); return false; }
}
} // namespace chili::ge
