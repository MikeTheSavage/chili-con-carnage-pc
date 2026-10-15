// The filter and level bias of a texture's sampler (host/gpu/ge_sampler_rule.hpp), without a graphics card:
//
//   a texture of the game keeps what the game asks, to the bit: the filter, the bias of -1.5, the bias
//       clamped to 0 under anisotropic filtering and kept with the test switch
//   cells painted over the game's own picture (the button prompts) keep what they had: a linear filter
//       between levels where the game asked for smooth texels, the game's bias, or none with exact levels
//   a mod's picture in a 3D draw has no bias and a linear filter between levels whatever the game asked,
//       and within a level its texels are filtered as the game or the setting says; in a sprite (HUD, text,
//       menus) it has, to the bit, the sampler every replaced texture had before the rule: the game's bias
//   which level that reads: an eight times picture at internal scales 8, 4, 2 and 1 in a 3D draw before
//       and after, and in a sprite the same level as before at every size, the full-size picture down to
//       2.8 texels per pixel
#include "gpu/ge_sampler_rule.hpp"
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using vcs::ge_sampler_rule;
using vcs::GeSamplerRule;
unsigned checks, failures;
const bool keep_going = [] { const char *value = std::getenv("CHILI_TEST_KEEP_GOING"); return value && *value == '1'; }();
void require(bool ok, const std::string &what) {
    ++checks;
    if (ok) return;
    if (!keep_going) throw std::runtime_error(what);
    ++failures;
    std::cerr << "FAILED CHECK: " << what << '\n';
}

// The eight filters texture_filter() can give and the anisotropic one.
constexpr std::array<D3D12_FILTER, 9> filters{
    D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR, D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT,
    D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR, D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT, D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR,
    D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_FILTER_ANISOTROPIC};
constexpr std::array<float, 5> biases{-1.5f, 0.0f, -0.5f, 1.0f, 2.25f};

// Bits of a D3D12_FILTER: 0x01 linear between levels, 0x04 linear when enlarged, 0x10 linear when shrunk.
bool mip_linear(D3D12_FILTER filter) { return filter == D3D12_FILTER_ANISOTROPIC || (filter & 0x01) != 0; }
unsigned texel_bits(D3D12_FILTER filter) { return static_cast<unsigned>(filter) & 0x14u; }
std::string name(D3D12_FILTER filter, float bias) { return "filter " + std::to_string(static_cast<int>(filter)) + " bias " + std::to_string(bias); }

// The level a sampler reads at `texels_per_pixel` of level 0, as Direct3D computes it: log2 of the step,
// plus the bias, not below 0. A whole number is one level; a fraction is a blend with a linear filter
// between levels and the nearer level without.
float level_read(const GeSamplerRule &rule, float texels_per_pixel) {
    const float level = std::max(0.0f, std::log2(texels_per_pixel) + rule.bias);
    return mip_linear(rule.filter) ? level : std::floor(level + 0.5f);
}

void game_textures() {
    for (const D3D12_FILTER filter : filters)
        for (const float bias : biases)
            for (const bool keep : {false, true})
                for (const bool scene : {false, true})
                    for (const bool mod : {false, true}) {
                        // Not replaced: `mod_picture` cannot be set, and if it were it would change nothing.
                        const auto rule = ge_sampler_rule(filter, bias, false, false, mod, scene, keep);
                        const float expected = filter == D3D12_FILTER_ANISOTROPIC && !keep ? std::max(0.0f, bias) : bias;
                        require(rule.filter == filter && rule.bias == expected, "a texture of the game keeps its sampler: " + name(filter, bias));
                    }
    // The one every draw of the menus, the HUD and the world asks for.
    const auto usual = ge_sampler_rule(D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, -1.5f, false, false, false, false, false);
    require(usual.filter == D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT && usual.bias == -1.5f, "the game's usual sampler");
}

void painted_over_the_game() {
    for (const D3D12_FILTER filter : filters)
        for (const float bias : biases)
            for (const bool keep : {false, true})
                for (const bool scene : {false, true}) {
                    // Cells over the game's own picture at a size that is not a power of two times it: filtered levels.
                    const auto plain = ge_sampler_rule(filter, bias, true, false, false, scene, keep);
                    const D3D12_FILTER smooth = filter == D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : filter;
                    const float expected = filter == D3D12_FILTER_ANISOTROPIC && !keep ? std::max(0.0f, bias) : bias;
                    require(plain.filter == smooth && plain.bias == expected, "cells over the game's picture keep the game's bias: " + name(filter, bias));
                    // The same with exact levels: no bias, the filter as before.
                    for (const bool mod : {false, true}) {
                        const auto exact = ge_sampler_rule(filter, bias, true, true, mod, scene, keep);
                        require(exact.filter == smooth && exact.bias == 0.0f, "exact levels are read without a bias and with the filter they had: " + name(filter, bias));
                    }
                }
}

void mod_pictures() {
    for (const D3D12_FILTER filter : filters)
        for (const float bias : biases)
            for (const bool keep : {false, true}) {
                // A 3D draw.
                const auto scene = ge_sampler_rule(filter, bias, true, false, true, true, keep);
                require(scene.bias == 0.0f, "a mod's picture in a 3D draw has no level bias: " + name(filter, bias));
                require(mip_linear(scene.filter), "and is filtered between levels: " + name(filter, bias));
                require(filter == D3D12_FILTER_ANISOTROPIC ? scene.filter == D3D12_FILTER_ANISOTROPIC
                                                           : scene.filter != D3D12_FILTER_ANISOTROPIC && texel_bits(scene.filter) == texel_bits(filter),
                        "within a level its texels are filtered as the game or the setting says: " + name(filter, bias));
                // A sprite: the sampler of a replaced texture that is no mod's picture, which is the one
                // every replaced texture had before the rule (the filter the game asks, smooth between
                // levels where it asks for smooth texels, and the game's bias).
                const auto sprite = ge_sampler_rule(filter, bias, true, false, true, false, keep);
                const auto plain = ge_sampler_rule(filter, bias, true, false, false, false, keep);
                const D3D12_FILTER smooth = filter == D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : filter;
                const float expected = filter == D3D12_FILTER_ANISOTROPIC && !keep ? std::max(0.0f, bias) : bias;
                require(sprite.filter == plain.filter && sprite.bias == plain.bias, "a mod's picture in a sprite has the sampler of a replaced texture: " + name(filter, bias));
                require(sprite.filter == smooth && sprite.bias == expected, "which is the game's filter and the game's bias: " + name(filter, bias));
            }
    const auto point = ge_sampler_rule(D3D12_FILTER_MIN_MAG_MIP_POINT, -1.5f, true, false, true, true, false);
    require(point.filter == D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR, "TextureFilter=nearest stays blocky within a level");
    const auto usual_scene = ge_sampler_rule(D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, -1.5f, true, false, true, true, false);
    require(usual_scene.filter == D3D12_FILTER_MIN_MAG_MIP_LINEAR && usual_scene.bias == 0.0f, "the game's usual sampler on a mod's picture in the world");
    const auto usual_sprite = ge_sampler_rule(D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, -1.5f, true, false, true, false, false);
    require(usual_sprite.filter == D3D12_FILTER_MIN_MAG_MIP_LINEAR && usual_sprite.bias == -1.5f, "the game's usual sampler on a mod's picture in the HUD: as before the rule");
}

void levels() {
    // An eight times picture drawn at the size the game draws its texture: 8 texels of it per PSP pixel,
    // so 8 / scale per screen pixel.
    const auto before = ge_sampler_rule(D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, -1.5f, true, false, false, false, false);   // every replaced texture before the rule
    const auto sprite = ge_sampler_rule(D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, -1.5f, true, false, true, false, false);
    const auto scene = ge_sampler_rule(D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, -1.5f, true, false, true, true, false);
    const struct { float scale, level_before, level_scene; } cases[]{{8.0f, 0.0f, 0.0f}, {4.0f, 0.0f, 1.0f}, {2.0f, 0.5f, 2.0f}, {1.0f, 1.5f, 3.0f}};
    for (const auto &c : cases) {
        const float step = 8.0f / c.scale;
        require(level_read(before, step) == c.level_before, "with the game's bias an 8x picture at scale " + std::to_string(c.scale) + " was read from level " + std::to_string(c.level_before));
        require(level_read(scene, step) == c.level_scene, "in a 3D draw, without it, from level " + std::to_string(c.level_scene));
        // Texels of the level read per screen pixel in a 3D draw: 1 after, up to 2 before.
        require(step / std::exp2(level_read(scene, step)) == 1.0f, "one texel per screen pixel in a 3D draw at scale " + std::to_string(c.scale));
        require(level_read(sprite, step) == c.level_before, "a sprite is read from the level it was read from, at scale " + std::to_string(c.scale));
    }
    require(8.0f / 4.0f / std::exp2(level_read(before, 2.0f)) == 2.0f, "at scale 4 every second texel was read");
    // Enlarged pictures and pictures at one texel per pixel are read from level 0 either way.
    for (const float step : {0.25f, 0.5f, 0.7f, 1.0f})
        require(level_read(before, step) == 0.0f && level_read(sprite, step) == 0.0f && level_read(scene, step) == 0.0f, "level 0 at " + std::to_string(step) + " texels per pixel");

    // A sprite at every size: the level, and the blend of two, that a replaced texture was read from
    // before the rule; the full-size picture alone down to 2.8 texels per pixel.
    float most_full_size = 0.0f;
    for (float step = 0.25f; step <= 64.0f; step *= 1.01f) {
        require(level_read(sprite, step) == level_read(before, step), "a sprite at " + std::to_string(step) + " texels per pixel is read as before");
        if (level_read(sprite, step) == 0.0f) most_full_size = std::max(most_full_size, step);
    }
    require(most_full_size > 2.8f && most_full_size < 2.83f, "a sprite is read from the full-size picture until 2.8 texels per pixel (" + std::to_string(most_full_size) + ")");
    // Text on an eight times sheet at the sizes the game draws it: on a 3840x2176 picture (scale 8) every
    // size, on a 1920x1088 picture (scale 4) the sizes from 0.8 up are read from the full-size sheet.
    for (const float size : {0.41f, 0.6f, 0.7f, 0.8f, 1.0f, 1.21f, 1.5f})
        require(level_read(sprite, 1.0f / size) == 0.0f, "text of size " + std::to_string(size) + " at scale 8 is read from the full-size sheet");
    for (const float size : {0.8f, 1.0f, 1.21f, 1.5f})
        require(level_read(sprite, 2.0f / size) == 0.0f, "text of size " + std::to_string(size) + " at scale 4 is read from the full-size sheet");
    // The HUD's pictures (drawn at the game's size, 2 texels per pixel at scale 4) with them.
    require(level_read(sprite, 2.0f) == 0.0f && level_read(scene, 2.0f) == 1.0f, "at scale 4 a HUD picture is read from the full-size picture, a picture in the world from the half-size level");
}
}

int main() {
    try {
        game_textures();
        painted_over_the_game();
        mod_pictures();
        levels();
        if (failures) { std::cerr << failures << " of " << checks << " checks FAILED\n"; return 1; }
        std::cout << "sampler rule: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
