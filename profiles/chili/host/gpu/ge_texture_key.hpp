#pragma once
// The texture replacement key of mods/TEXTURES.md, computed from guest memory
// when a texture enters the GPU cache. It must give the number that
// tools/extract_textures.py prints for the same texture: XXH64 over a 16-byte
// header H, the level 0 texel bytes T without padding, and the palette entries
// P that occur, chained through the seed.
#include "../state_hash.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace vcs {

// GE state of the bound texture, level 0.
struct GeTextureKeyInput {
    std::uint32_t format{};        // TPSM & 15
    std::uint32_t width{};         // 1 << (TSIZE0 & 15)
    std::uint32_t height{};        // 1 << ((TSIZE0 >> 8) & 15)
    std::uint32_t buffer_width{};  // TBW0 & 0x7FF
    std::uint32_t address{};
    bool swizzled{};
    std::uint32_t clut_address{};
    std::uint32_t clut_format{};   // CMODE & 3
    std::uint32_t clut_shift{};
    std::uint32_t clut_mask{};
    std::uint32_t clut_start{};    // already shifted left by 4
};

// Rows of level 0 the key and a replacement picture cover: a 32-bit texture
// declared 512x512 is a screen image of which the game stores 272 rows.
[[nodiscard]] constexpr std::uint32_t ge_texture_key_rows(std::uint32_t format, std::uint32_t width,
                                                          std::uint32_t height) noexcept {
    return format == 3u && width == 512u && height == 512u ? 272u : height;
}

// `read(address, bytes)` returns a pointer to that many readable guest bytes or
// nullptr. Returns 0 when the texture has no key (unknown format, or a byte of
// T or P outside guest memory). `scratch` is reused between calls.
template <typename Read>
[[nodiscard]] std::uint64_t ge_texture_content_key(const GeTextureKeyInput &in, Read &&read,
                                                   std::vector<std::uint8_t> &scratch) {
    if (in.format > 10u || in.width == 0u || in.height == 0u || in.width > 1024u || in.height > 1024u)
        return 0u;
    const std::uint32_t rows = ge_texture_key_rows(in.format, in.width, in.height);
    const std::uint32_t buffer_width = std::max<std::uint32_t>(1u, in.buffer_width);
    scratch.clear();
    if (in.format >= 8u) {
        const std::uint32_t block = in.format == 8u ? 8u : 16u;
        const std::uint32_t per_row = std::max<std::uint32_t>(1u, (buffer_width + 3u) / 4u) * block;
        const std::uint32_t used = (in.width + 3u) / 4u * block;
        const std::uint32_t block_rows = (rows + 3u) / 4u;
        const std::size_t span = static_cast<std::size_t>(block_rows - 1u) * per_row + used;
        const std::uint8_t *data = read(in.address, span);
        if (data == nullptr) return 0u;
        scratch.resize(static_cast<std::size_t>(used) * block_rows);
        for (std::uint32_t r = 0u; r < block_rows; ++r)
            std::memcpy(scratch.data() + static_cast<std::size_t>(r) * used,
                        data + static_cast<std::size_t>(r) * per_row, used);
    } else {
        constexpr std::array<std::uint32_t, 8> kBits{16u, 16u, 16u, 32u, 4u, 8u, 16u, 32u};
        const std::uint32_t bits = kBits[in.format];
        const std::uint32_t pitch = (buffer_width * bits + 7u) / 8u;
        const std::uint32_t used = (in.width * bits + 7u) / 8u;
        scratch.resize(static_cast<std::size_t>(used) * rows);
        if (!in.swizzled) {
            const std::size_t span = static_cast<std::size_t>(rows - 1u) * pitch + used;
            const std::uint8_t *data = read(in.address, span);
            if (data == nullptr) return 0u;
            for (std::uint32_t y = 0u; y < rows; ++y)
                std::memcpy(scratch.data() + static_cast<std::size_t>(y) * used,
                            data + static_cast<std::size_t>(y) * pitch, used);
        } else {
            // 16-byte by 8-row blocks.
            const std::uint32_t blocks_per_row = (pitch + 15u) >> 4u;
            const std::size_t last =
                ((static_cast<std::size_t>((rows - 1u) >> 3u) * blocks_per_row + ((used - 1u) >> 4u)) << 7u) +
                (static_cast<std::size_t>((rows - 1u) & 7u) << 4u) + ((used - 1u) & 15u);
            const std::uint8_t *data = read(in.address, last + 1u);
            if (data == nullptr) return 0u;
            for (std::uint32_t y = 0u; y < rows; ++y) {
                const std::uint8_t *row = data +
                    ((static_cast<std::size_t>(y >> 3u) * blocks_per_row) << 7u) + (static_cast<std::size_t>(y & 7u) << 4u);
                std::uint8_t *out = scratch.data() + static_cast<std::size_t>(y) * used;
                for (std::uint32_t b = 0u; b < used; b += 16u)
                    std::memcpy(out + b, row + (static_cast<std::size_t>(b >> 4u) << 7u), std::min(16u, used - b));
            }
        }
        // The high nibble of a row's last byte is not a texel.
        if (in.format == 4u && (in.width & 1u) != 0u)
            for (std::size_t at = used - 1u; at < scratch.size(); at += used) scratch[at] &= 0x0Fu;
    }

    std::array<std::uint8_t, 16> header{};
    header[0] = 0x43u; header[1] = 0x54u; header[2] = 0x4Bu; header[3] = 0x31u;  // "CTK1"
    const bool paletted = in.format >= 4u && in.format <= 7u;
    header[4] = static_cast<std::uint8_t>(in.format);
    header[5] = static_cast<std::uint8_t>(paletted ? (in.clut_format & 3u) : 0u);
    header[8] = static_cast<std::uint8_t>(in.width & 0xFFu);
    header[9] = static_cast<std::uint8_t>(in.width >> 8u);
    header[10] = static_cast<std::uint8_t>(in.height & 0xFFu);
    header[11] = static_cast<std::uint8_t>(in.height >> 8u);
    std::uint64_t key = chili::xxh64(header);
    key = chili::xxh64(scratch, key);

    std::array<std::uint8_t, 256u * 4u> palette{};
    std::size_t palette_bytes = 0u;
    if (paletted) {
        const std::uint32_t entry_bytes = (in.clut_format & 3u) == 3u ? 4u : 2u;
        const std::uint32_t wrap = entry_bytes == 4u ? 0xFFu : 0x1FFu;
        const auto entry_of = [&](std::uint32_t index) noexcept {
            return (((index >> (in.clut_shift & 31u)) & (in.clut_mask & 0xFFu)) | in.clut_start) & wrap;
        };
        const auto append = [&](std::uint32_t index) noexcept {
            const std::uint8_t *entry = read(in.clut_address + entry_of(index) * entry_bytes, entry_bytes);
            if (entry == nullptr) return false;
            if (palette_bytes + entry_bytes > palette.size()) return false;
            std::memcpy(palette.data() + palette_bytes, entry, entry_bytes);
            palette_bytes += entry_bytes;
            return true;
        };
        if (in.format == 4u || in.format == 5u) {
            std::array<bool, 256> seen{};
            if (in.format == 5u) {
                for (const std::uint8_t value : scratch) seen[value] = true;
            } else {
                // `width` nibbles per row, low nibble first; an odd width's last
                // high nibble is zeroed above and must not count as index 0.
                const std::uint32_t used = (in.width + 1u) / 2u;
                for (std::uint32_t y = 0u; y < rows; ++y) {
                    const std::uint8_t *row = scratch.data() + static_cast<std::size_t>(y) * used;
                    for (std::uint32_t x = 0u; x < in.width; ++x)
                        seen[(row[x >> 1u] >> ((x & 1u) * 4u)) & 15u] = true;
                }
            }
            for (std::uint32_t index = 0u; index < 256u; ++index)
                if (seen[index] && !append(index)) return 0u;
        } else {
            // CLUT16 and CLUT32 (the game uses neither): the distinct index
            // words in ascending order. More than 256 of them has no key.
            const std::uint32_t word = in.format == 6u ? 2u : 4u;
            std::vector<std::uint32_t> indices;
            indices.reserve(scratch.size() / word);
            for (std::size_t at = 0u; at + word <= scratch.size(); at += word) {
                std::uint32_t value = 0u;
                std::memcpy(&value, scratch.data() + at, word);
                indices.push_back(value);
            }
            std::sort(indices.begin(), indices.end());
            indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
            if (indices.size() > 256u) return 0u;
            for (const std::uint32_t index : indices)
                if (!append(index)) return 0u;
        }
    }
    key = chili::xxh64({palette.data(), palette_bytes}, key);
    // 0 means "no key" to the callers.
    return key == 0u ? 1u : key;
}

} // namespace vcs
