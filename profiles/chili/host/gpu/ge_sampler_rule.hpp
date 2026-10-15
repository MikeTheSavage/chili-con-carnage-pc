#pragma once
// The filter and the level bias of a texture's sampler (ge_gpu_backend_dx12.cpp, ensure_sampler), kept
// apart from the device so that every case can be checked without one (tools/sampler_rule_test.cpp).
//
// The game asks for a level bias of -1.5 on every texture: it sharpened a 480x272 picture. Its own textures
// keep it. A mod's picture is several times the size of the texture it stands for, and with that bias it
// is read from its largest level until it is shrunk 2.8 times.
//
//   In a 3D draw a mod's picture is sampled without the game's bias, with a linear filter between levels:
//   from the levels that have about one texel per screen pixel. A coin that turns, or comes nearer, changes
//   its step from texel to texel all the time; with the bias an eight times picture at an internal scale
//   of 4 was read at every second texel. Its levels are a box filter with colour weighted by alpha
//   (ge_texture_build_mip_chain).
//   In a sprite (HUD, text, menus: through mode) a mod's picture keeps the sampler every replaced texture
//   had before this rule: the game's bias, so the full-size picture at every internal scale down to 2.8
//   texels per pixel. A sprite has one size and does not move across texels, and the pictures of a HUD or
//   text pack are chosen for their edges: read from the half-size level they were a third less steep on a
//   1920x1088 picture.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>

namespace vcs {

struct GeSamplerRule {
    D3D12_FILTER filter{};
    float bias{};
};

// `filter`: what the game asks, or the settings for a 3D draw (texture_filter()). `game_bias`: the game's
// level offset in levels. `replaced`: the image is a GeTextureReplacement with its own chain of levels.
// `exact_levels`, `mod_picture`: that picture's flags. `scene`: the sampler of 3D draws, not of sprites.
// `anisotropic_keep_bias`: the renderer's test switch.
constexpr GeSamplerRule ge_sampler_rule(D3D12_FILTER filter, float game_bias, bool replaced, bool exact_levels,
                                        bool mod_picture, bool scene, bool anisotropic_keep_bias) noexcept {
    GeSamplerRule rule{filter, game_bias};
    if (replaced && rule.filter == D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT) rule.filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    // The game sharpens every texture by 1.5 levels, which a 480x272 LCD hid and a large picture shows as
    // shimmer. Anisotropic filtering keeps slanted surfaces sharp without it.
    if (rule.filter == D3D12_FILTER_ANISOTROPIC && !anisotropic_keep_bias) rule.bias = rule.bias < 0.0f ? 0.0f : rule.bias;
    if (exact_levels) rule.bias = 0.0f;
    if (!replaced || !mod_picture || exact_levels || !scene) return rule;
    rule.bias = 0.0f;
    switch (rule.filter) {
    case D3D12_FILTER_MIN_MAG_MIP_POINT: rule.filter = D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR; break;
    case D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT: rule.filter = D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR; break;
    case D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT: rule.filter = D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR; break;
    default: break;   // linear between levels already, or anisotropic
    }
    return rule;
}

} // namespace vcs
