#pragma once
// The game's glyph table for a host page, taken from the running game's memory (page_draw.hpp, PageFont).
#include "page_draw.hpp"
#include "psprecomp/guest_memory.hpp"
namespace chili {
// The game's user memory, where its loaded blocks are.
inline constexpr std::uint32_t page_font_memory_base = 0x08800000u, page_font_memory_size = 0x01800000u;
// True once the table is found: the game has loaded its font by the time it draws its first text (frame 12
// of a start), so ask again later after a false. Reads guest memory and writes none; a few milliseconds.
inline bool load_page_font(const psprecomp::GuestMemory &memory, PageFont &font) {
    const std::uint8_t *ram = memory.raw_pointer(page_font_memory_base, page_font_memory_size);
    return ram != nullptr && font.scan(std::span<const std::uint8_t>(ram, page_font_memory_size), page_font_memory_base);
}
}
