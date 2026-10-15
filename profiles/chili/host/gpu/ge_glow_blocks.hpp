#pragma once
// Chili: where the game's full-screen glow is in a frame's draws. No graphics
// card needed here; ge_gpu_glow_dx12.hpp acts on what this finds, and
// tools/glow_blocks_test.cpp tests it on the draws of captured frames.
//
// The game makes its glow with sprites on a sheet of its own: the picture is
// copied onto the sheet at half size, then the sheet is copied, cut off below
// half brightness and added to itself some twenty times, and at the end one
// sprite adds a part of the sheet onto the picture, enlarged. Nothing of that
// is read from the game's numbers here. A glow block is a run of draws that
//
//   - are all through-mode sprites into one surface that is not the displayed
//     one, without depth test, not clears and not uploads,
//   - begins with a sprite that shrinks another surface (its texture rectangle
//     is at least 1.5 times its screen rectangle along both axes),
//   - has at least three draws, one of which samples the surface it draws into.
//
// The run ends before the first draw that is not such a sprite or goes to
// another surface. Menus, movies and loading screens have no such run.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vcs {

// What the rule needs to know of one draw.
struct GeGlowDraw {
    std::uint32_t target{};     // the surface drawn into (its VRAM offset)
    // A through-mode sprite, depth test off, no clear, no upload.
    bool sprite{};
    bool samples_surface{};     // its texture is a surface
    std::uint32_t source{};     // that surface
    // The texture rectangle is known (left, top, right, bottom in texels of the
    // source) and so is the screen rectangle (PSP pixels).
    bool rectangles_known{};
    std::array<float, 4> texels{};
    std::array<float, 4> pixels{};
};

struct GeGlowBlock {
    std::size_t first{};        // index of the shrinking sprite
    std::size_t last{};         // index of the run's last draw
    std::uint32_t target{};
};

inline constexpr float kGeGlowMinShrink = 1.5f;
inline constexpr std::size_t kGeGlowMinDraws = 3u;

// True when the draw copies another surface onto its own at 1.5 to 1 or smaller.
[[nodiscard]] inline bool ge_glow_shrinks(const GeGlowDraw &draw) noexcept {
    if (!draw.sprite || !draw.samples_surface || draw.source == draw.target || !draw.rectangles_known) return false;
    const float texels_x = draw.texels[2] - draw.texels[0], texels_y = draw.texels[3] - draw.texels[1];
    const float pixels_x = draw.pixels[2] - draw.pixels[0], pixels_y = draw.pixels[3] - draw.pixels[1];
    return pixels_x > 0.0f && pixels_y > 0.0f &&
           texels_x >= kGeGlowMinShrink * pixels_x && texels_y >= kGeGlowMinShrink * pixels_y;
}

// The glow blocks among `draws`, in order. `displayed` is the surface the
// window shows: a run into it is never a block.
[[nodiscard]] inline std::vector<GeGlowBlock> find_glow_blocks(std::span<const GeGlowDraw> draws,
                                                                 std::uint32_t displayed) {
    std::vector<GeGlowBlock> blocks;
    for (std::size_t first = 0u; first < draws.size();) {
        const GeGlowDraw &start = draws[first];
        if (start.target == displayed || !ge_glow_shrinks(start)) { ++first; continue; }
        std::size_t end = first + 1u;
        bool reads_itself = false;
        for (; end < draws.size() && draws[end].sprite && draws[end].target == start.target; ++end)
            reads_itself = reads_itself || (draws[end].samples_surface && draws[end].source == start.target);
        if (end - first >= kGeGlowMinDraws && reads_itself) {
            blocks.push_back({first, end - 1u, start.target});
            first = end;
        } else {
            ++first;
        }
    }
    return blocks;
}

} // namespace vcs
