#pragma once
// Independent GE source-record preparation. No renderer state or guest writes.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace chili::ge {
struct Layout {
    std::uint32_t vtype{};
    unsigned uv_type{}, color_type{}, normal_type{}, position_type{}, weight_type{}, index_type{};
    unsigned weight_count{}, morph_count{}, index_bytes{}, alignment{1};
    unsigned weight_offset{}, uv_offset{}, color_offset{}, normal_offset{}, position_offset{};
    unsigned sample_stride{}, vertex_stride{};
    bool through{};
    bool needs_skinning() const { return weight_type != 0 && !through; }
    bool needs_morphing() const { return morph_count > 1; }
};
inline unsigned align_up(unsigned size, unsigned alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}
inline Layout make_layout(std::uint32_t vtype) {
    constexpr std::uint32_t allowed = 0x1FFFu | (7u << 14) | (7u << 18) | (1u << 23);
    if (vtype & ~allowed) throw std::runtime_error("GE vertex type contains reserved bits");
    Layout l{}; l.vtype = vtype;
    l.uv_type = vtype & 3; l.color_type = (vtype >> 2) & 7;
    l.normal_type = (vtype >> 5) & 3; l.position_type = (vtype >> 7) & 3;
    l.weight_type = (vtype >> 9) & 3; l.index_type = (vtype >> 11) & 3;
    l.weight_count = l.weight_type ? ((vtype >> 14) & 7) + 1 : 0;
    l.morph_count = ((vtype >> 18) & 7) + 1; l.through = (vtype & (1u << 23)) != 0;
    if (!l.position_type) throw std::runtime_error("GE vertex position encoding is absent");
    if (l.color_type > 0 && l.color_type < 4) throw std::runtime_error("GE color encoding is undocumented");
    if (l.index_type == 3) throw std::runtime_error("GE 32-bit index encoding is not prepared");
    l.index_bytes = l.index_type ? (1u << (l.index_type - 1)) : 0;
    constexpr unsigned bytes[] = {0, 1, 2, 4};
    unsigned cursor = 0;
    auto member = [&](unsigned alignment, unsigned size, unsigned &offset) {
        if (!size) return;
        cursor = align_up(cursor, alignment); offset = cursor; cursor += size;
        l.alignment = std::max(l.alignment, alignment);
    };
    member(bytes[l.weight_type], bytes[l.weight_type] * l.weight_count, l.weight_offset);
    member(bytes[l.uv_type], bytes[l.uv_type] * 2, l.uv_offset);
    member(l.color_type == 7 ? 4 : 2, l.color_type ? (l.color_type == 7 ? 4 : 2) : 0, l.color_offset);
    member(bytes[l.normal_type], bytes[l.normal_type] * 3, l.normal_offset);
    member(bytes[l.position_type], bytes[l.position_type] * 3, l.position_offset);
    l.sample_stride = align_up(cursor, l.alignment);
    l.vertex_stride = l.sample_stride * l.morph_count;
    return l;
}
using Vec2 = std::array<float, 2>;
using Vec3 = std::array<float, 3>;
using Color = std::array<std::uint8_t, 4>; // RGBA, source low byte is R.
struct Sample {
    std::optional<Vec2> uv;
    std::optional<Color> color;
    std::optional<Vec3> normal;
    Vec3 position{};
    std::array<float, 8> weights{};
};
struct Vertex {
    std::uint32_t source_index{};
    std::array<Sample, 8> samples{};
};
struct Draw {
    Layout layout;
    std::vector<Vertex> vertices; // In PRIM/index order, including duplicates.
    std::uint32_t next_vertex_address{}, next_index_address{};
    // Readiness refers to source attribute conversion only. Matrices, UV generation,
    // lighting/material defaults, clipping, and rasterization still belong to GE state.
    bool source_attributes_ready() const {
        return !layout.needs_skinning() && !layout.needs_morphing();
    }
};
inline std::uint32_t uread(std::span<const std::uint8_t> bytes, unsigned offset, unsigned count) {
    if (offset > bytes.size() || count > bytes.size() - offset) throw std::runtime_error("Truncated GE vertex member");
    std::uint32_t value = 0;
    for (unsigned i = 0; i < count; ++i) value |= std::uint32_t(bytes[offset + i]) << (8 * i);
    return value;
}
inline float scalar(std::span<const std::uint8_t> bytes, unsigned offset, unsigned type, bool sign, bool normalize) {
    constexpr unsigned sizes[] = {0, 1, 2, 4};
    auto bits = uread(bytes, offset, sizes[type]);
    if (type == 3) {
        auto value = std::bit_cast<float>(bits);
        if (!std::isfinite(value)) throw std::runtime_error("Nonfinite GE source attribute requires verified hardware handling");
        return value;
    }
    auto value = sign ? (type == 1 ? int(std::bit_cast<std::int8_t>(std::uint8_t(bits))) : int(std::bit_cast<std::int16_t>(std::uint16_t(bits)))) : int(bits);
    return float(value) * (normalize ? (type == 1 ? 1.0f / 128.0f : 1.0f / 32768.0f) : 1.0f);
}
inline Color decode_color(std::span<const std::uint8_t> bytes, unsigned at, unsigned type) {
    auto value = uread(bytes, at, type == 7 ? 4 : 2);
    auto c5 = [](unsigned x) { return std::uint8_t((x << 3) | (x >> 2)); };
    auto c6 = [](unsigned x) { return std::uint8_t((x << 2) | (x >> 4)); };
    switch (type) {
    case 4: return {c5(value & 31), c6((value >> 5) & 63), c5((value >> 11) & 31), 255};
    case 5: return {c5(value & 31), c5((value >> 5) & 31), c5((value >> 10) & 31), std::uint8_t((value >> 15) * 255)};
    case 6: return {std::uint8_t((value & 15) * 17), std::uint8_t(((value >> 4) & 15) * 17), std::uint8_t(((value >> 8) & 15) * 17), std::uint8_t((value >> 12) * 17)};
    case 7: return {std::uint8_t(value), std::uint8_t(value >> 8), std::uint8_t(value >> 16), std::uint8_t(value >> 24)};
    default: throw std::runtime_error("Absent GE color has no implicit source value");
    }
}
inline Sample decode_sample(const Layout &l, std::span<const std::uint8_t> bytes) {
    if (bytes.size() != l.sample_stride) throw std::runtime_error("Incorrect GE sample record size");
    if (l.through && l.position_type == 1) throw std::runtime_error("Through 8-bit position conversion awaits a hardware oracle");
    Sample s{}; constexpr unsigned sizes[] = {0, 1, 2, 4};
    for (unsigned i = 0; i < l.weight_count; ++i) s.weights[i] = scalar(bytes, l.weight_offset + sizes[l.weight_type] * i, l.weight_type, false, true);
    if (l.uv_type) {
        Vec2 uv{};
        for (unsigned i = 0; i < 2; ++i) uv[i] = scalar(bytes, l.uv_offset + sizes[l.uv_type] * i, l.uv_type, false, !l.through);
        s.uv = uv;
    }
    if (l.color_type) s.color = decode_color(bytes, l.color_offset, l.color_type);
    if (l.normal_type) {
        Vec3 normal{};
        for (unsigned i = 0; i < 3; ++i) normal[i] = scalar(bytes, l.normal_offset + sizes[l.normal_type] * i, l.normal_type, true, true);
        s.normal = normal; // No normalization to unit length and no default normal.
    }
    for (unsigned i = 0; i < 3; ++i) {
        bool sign = !(l.through && l.position_type == 2 && i == 2);
        s.position[i] = scalar(bytes, l.position_offset + sizes[l.position_type] * i, l.position_type, sign, !l.through);
    }
    if (l.through && l.position_type == 3) {
        float z = std::clamp(s.position[2], 0.0f, 65535.0f);
        s.position[2] = float(static_cast<std::uint32_t>(z));
    }
    return s;
}
inline std::uint32_t add_address(std::uint32_t base, std::uint64_t delta) {
    auto result = std::uint64_t(base) + delta;
    if (result > std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("GE address cursor would wrap");
    return std::uint32_t(result);
}
// Reader signature: bool reader(uint32_t address, span<uint8_t> destination).
// The caller resolves PSP aliases and checks mapped ranges. It must not write guest state.
// A failure throws before any Draw is returned. The caller commits cursors after it accepts
// all remaining GE state/primitive semantics and successfully executes the draw.
template<class Reader>
Draw prepare_draw(Reader &&reader, std::uint32_t vtype, std::uint32_t vertex_address,
                  std::uint32_t index_address, std::uint32_t count) {
    Draw out{}; out.layout = make_layout(vtype);
    out.next_vertex_address = vertex_address; out.next_index_address = index_address;
    if (count > 65535) throw std::runtime_error("GE PRIM count exceeds its 16-bit field");
    if (!count) return out;
    const auto &l = out.layout;
    if (vertex_address % l.alignment || (l.index_bytes && index_address % l.index_bytes)) throw std::runtime_error("Misaligned GE source address");
    std::vector<std::uint8_t> indices(count * l.index_bytes);
    if (l.index_bytes) {
        out.next_index_address = add_address(index_address, indices.size());
        if (!reader(index_address, std::span<std::uint8_t>(indices))) throw std::runtime_error("GE index stream is outside mapped memory");
    } else {
        out.next_vertex_address = add_address(vertex_address, std::uint64_t(count) * l.vertex_stride);
    }
    std::vector<std::uint8_t> record(l.vertex_stride); out.vertices.reserve(count);
    for (std::uint32_t n = 0; n < count; ++n) {
        auto index = l.index_bytes ? uread(indices, n * l.index_bytes, l.index_bytes) : n;
        auto address = add_address(vertex_address, std::uint64_t(index) * l.vertex_stride);
        // The exclusive end is intentionally required to be representable too.
        add_address(address, l.vertex_stride);
        if (!reader(address, std::span<std::uint8_t>(record))) throw std::runtime_error("GE vertex record is outside mapped memory");
        Vertex v{}; v.source_index = index;
        for (unsigned m = 0; m < l.morph_count; ++m) v.samples[m] = decode_sample(l, std::span<const std::uint8_t>(record).subspan(m * l.sample_stride, l.sample_stride));
        out.vertices.push_back(v);
    }
    return out;
}
} // namespace chili::ge
