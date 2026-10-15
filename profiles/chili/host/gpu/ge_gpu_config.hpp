#pragma once

#include <cstdint>
#include <string>

namespace vcs {

// Chili's replacement for upstream's VcsConfiguration. It holds only what the
// lifted GE front end and D3D12 backend consume. The host fills it once at
// start-up (set_ge_gpu_config) before initialize_ge_gpu_backend(); the renderer
// reads plain fields afterwards and never touches the environment.
struct GeGpuConfig {
    enum class Backend : std::uint8_t { Software, DirectX12 };
    // How textures of 3D draws are filtered: as the game asks, or forced.
    enum class TextureFilter : std::uint8_t { Game, Linear, Nearest };
    // Post-process anti-aliasing of the 3D scene (multisampling is `msaa`).
    enum class PostAntiAliasing : std::uint8_t { Off, Fxaa };
    enum class AmbientOcclusion : std::uint8_t { Off, Low, High };
    // The game's full-screen glow (ge_gpu_glow_dx12.hpp). Smooth: its passes are
    // drawn at the PSP's own size and the result is enlarged smoothly. Game: as
    // the game's draws say, at the internal scale. Off: not added to the picture.
    enum class Glow : std::uint8_t { Smooth, Game, Off };

    // Software: the front end's own rasterizer writes guest VRAM.
    // DirectX12: draws go to GPU surfaces and guest VRAM is left alone.
    Backend backend{Backend::Software};
    // With the D3D12 backend active, also run the front end's CPU rasterizer
    // into guest VRAM. Debug aid only; it doubles the work.
    bool keep_software_raster{false};

    // GE surfaces are 512-pixel-stride VRAM sheets. Every GPU surface is one
    // sheet of this logical size; its physical size is sheet * internal_scale.
    // The LCD shows the top-left 480x272 of the displayed sheet.
    std::uint32_t surface_width{512u};
    std::uint32_t surface_height{512u};
    std::uint32_t internal_scale{1u};          // 1..8
    // The scale was chosen to fit the monitor, not by the player: a graphics
    // card with little memory then gets a smaller one.
    bool internal_scale_auto{false};
    std::uint32_t msaa{1u};                    // 1, 2, 4, 8
    // 1..16. Above 1, linearly filtered textures of 3D draws (not through-mode
    // sprites, not surfaces sampled as textures) are filtered anisotropically,
    // with the real texture footprint and without the game's negative LOD bias.
    std::uint32_t anisotropic_filtering{1u};
    TextureFilter texture_filter{TextureFilter::Game};
    // A/B switch: keep the game's LOD bias under anisotropic filtering.
    bool anisotropic_keep_bias{false};
    PostAntiAliasing post_aa{PostAntiAliasing::Off};
    AmbientOcclusion ambient_occlusion{AmbientOcclusion::Off};
    // Show the occlusion term alone (grey) instead of the scene.
    bool ambient_occlusion_debug{false};
    // How much of the occlusion term darkens the picture, in percent.
    std::uint32_t ambient_occlusion_strength{70u};
    // Direct3D 12 only. At internal scale 1 Smooth is Game: the game's passes
    // are the PSP's there. The software rasterizer always draws the game's own.
    Glow glow{Glow::Game};
    std::uint32_t depth_precision{24u};        // 16, 24 or 32 (float)
    std::uint32_t texture_cache_entries{8192u};
    // Budget of the GPU texture images. The game's own textures of a level are
    // a few tens of MB; replacement packs at several times the size need more.
    std::uint32_t texture_cache_mb{1024u};

    // 3D triangle draws: world/view/projection, viewport and clipping in the
    // vertex shader. Off sends CPU-clipped screen-space triangles instead.
    bool hardware_transform{true};
    bool hardware_lit{true};
    // Face culling of GPU-transformed draws. Upstream left it off for a defect
    // seen in VCS; Chili needs it, because the game draws two-sided glass as a
    // front and a back sheet in one strip and relies on the cull to drop one.
    bool hardware_cull{true};
    bool packed_0115{true};
    bool native_indexed_draw{false};
    bool direct_nonindexed_draw{false};
    bool batch_merge{true};
    bool texture_upload_ring{true};

    // Presentation.
    bool upscale_bilinear{true};
    bool preserve_aspect{true};
    bool integer_scale{false};
    std::uint32_t present_sync_interval{0u};   // 0 = immediate (tearing allowed)
    // Copy the displayed surface back to system memory after every frame.
    bool frame_readback{false};
    // Fail initialize_ge_gpu_backend() instead of falling back to software.
    bool strict{false};
    // Case-insensitive substring of the DXGI adapter description to use.
    // Empty picks the hardware adapter with the most dedicated video memory.
    std::string adapter{};

    // CPU-side worker tuning (texture decode, software raster, vertex decode).
    std::uint32_t raster_threads{0u};          // 0 = one per logical CPU, capped
    std::uint32_t raster_parallel_pixels{2048u};
    bool parallel_texture_decode{false};
    bool parallel_vertex_decode{false};
    std::uint32_t parallel_vertex_threshold{256u};
    std::uint32_t parallel_vertex_max_workers{6u};

    // Diagnostics.
    // Wait for the graphics card after every submission and add up how long it
    // took (GeGpuBackendReport::perf_finish_frame_*). Slows the game; for
    // measuring what a setting costs on the card.
    bool gpu_timing_diag{false};
    bool phase_diag{false};
    bool geometry_debug_colors{false};
    bool force_white_vertex{false};
};

namespace detail {
inline GeGpuConfig g_ge_gpu_config{};
}

[[nodiscard]] inline const GeGpuConfig &ge_gpu_config() noexcept {
    return detail::g_ge_gpu_config;
}

// Call before initialize_ge_gpu_backend() and before the first PRIM.
inline void set_ge_gpu_config(const GeGpuConfig &config) {
    detail::g_ge_gpu_config = config;
}

// Upstream's PSPRECOMP_GE_* / PSPRECOMP_DX12_* switches, applied on top of
// `base`. Reads the environment; call it once at start-up, never per draw.
[[nodiscard]] GeGpuConfig ge_gpu_config_from_environment(GeGpuConfig base = {});

} // namespace vcs
