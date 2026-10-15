# Hardware GE renderer: origin and local changes

These files are a lift of the PSPRecomp upstream hardware GE renderer, taken
from `profiles/vcs/host` at commit `f6e7d415c7f447b934cc3865a31eb725f353d659`
(`f6e7d41`), with the VCS-specific parts removed and the Kapow-specific gaps
fixed. Upstream's `profiles/vcs` is not part of this repository; compare against an upstream checkout.

| File here | Upstream file | Lines up / here |
|---|---|---|
| `ge_renderer.hpp` | `profiles/vcs/host/ge_renderer.hpp` | 134 / 134 (unchanged) |
| `ge_renderer.cpp` | `profiles/vcs/host/ge_renderer.cpp` | 5040 / 4746 |
| `ge_gpu_backend.hpp` | `profiles/vcs/host/ge_gpu_backend.hpp` | 600 / 618 |
| `ge_gpu_backend_dx12.cpp` | `profiles/vcs/host/ge_gpu_backend_dx12.cpp` | 4265 / 3855 |
| `ge_gpu_config.hpp` | new, replaces `vcs_config.hpp` | |
| `ge_gpu_log.hpp`, `ge_gpu_support.cpp` | new, replace `vcs_runtime_log.*` and hold the config-from-environment reader | |

Names, namespace (`vcs`) and structure are kept so an upstream fix can be
carried over by diff. The lifted files use LF line endings, like the upstream
blobs; a checkout that converts line endings needs

    git diff --no-index --ignore-cr-at-eol <upstream>/profiles/vcs/host/ge_renderer.cpp profiles/chili/host/gpu/ge_renderer.cpp

Behaviour changes in the lifted files carry a comment starting `Chili:`.
Deletions and the one-line swaps from `vcs_configuration()` / `std::getenv`
to `ge_gpu_config()` are not marked; the diff above shows them.

## Removed

- **CloudWorks** (CC BY-NC-SA, not carried over in any form): `ge_cloud_camera_math.hpp`,
  `ge_cloudworks_present_shader.hpp`, the cloud camera observer
  (`ge_gpu_backend_observe_camera`), cloud render targets, root signature,
  three pipelines, the temporal pass and its injection into the frame loop.
  The present vertex and pixel shaders lived inside the CloudWorks shader
  source; the ones here are written new (`PresentVS` / `PresentPS` in
  `ge_gpu_backend_dx12.cpp`).
- **VCS game coupling in the front end**: `vcs_config.hpp`, `vcs_project2dfx.hpp`,
  `vcs_fps_overlay.hpp`, the `kVcsCameraPosition` (0x08BC87E0) read, the three
  observer calls, the widescreen HUD shrink block and `GeGpuWidescreenHud`.
- **Environment reads**: every `std::getenv` in both files. The switches are
  fields of `GeGpuConfig`, set once by the host. `ge_gpu_config_from_environment()`
  still honours the upstream variable names for A/B runs.
- **Per-surface CPU raster policy** (`PSPRECOMP_GE_GPU_SKIP_*`): with the D3D12
  backend active nothing is rasterized on the CPU (`software_raster_skipped`),
  unless `GeGpuConfig::keep_software_raster` is set.
- Dead code: the Stage 22 vertex staging relic (`ge_gpu_backend_stage_vertices`),
  `screen_to_d3d`, `model_to_d3d`, the non-Windows stub backend.

## Changed for Chili

Surface model (a first step, not the final one):

- Every GPU colour surface is one 512x512 logical sheet (`GeGpuConfig::surface_*`)
  at `sheet * internal_scale` physical pixels, keyed by framebuffer address.
  Upstream allocated every target at the 480x272-derived internal resolution
  and re-derived each target's logical extent per draw from the scissor and
  from the texture state that last sampled it. Feedback texture coordinates
  are normalised by the sheet, not by the texture registers. The swapchain
  shows the top-left 480x272 of the displayed sheet.
- Depth buffers are separate resources keyed by the GE depth address
  (`Dx12DepthTarget`), so the scene (0x040E8000) and composite (0x040D8000)
  surfaces share the buffer at 0x04088000. Upstream gave every colour target a
  private depth buffer.
- No implicit clears. A new resource is zeroed once; after that only the
  game's clear draws clear anything. Upstream cleared the displayed surface
  and every depth buffer at first use in each display interval.
- Clear-mode draws write depth (depth test forced to ALWAYS for them); D3D
  writes no depth while the test is disabled, which upstream relied on the
  implicit clear to hide.
- A texture address matches a surface only if it lies in VRAM
  (`find_feedback_target`). Upstream masked the address to 21 bits, so a
  main-RAM texture at 0x09288000 would have sampled the surface at 0x04088000.

Draw state:

- The whole PSP blend table (`resolve_blend`). Factors that depend only on
  the fragment (source colour or alpha, fixed colours) are evaluated in the
  pixel shader: the source factor is multiplied into the colour, the
  destination factor goes out as `SV_Target1` and is applied by dual-source
  blending. Upstream had five hard-coded combinations and drew everything else
  unblended. Not exact: ABS (drawn unblended), 2x destination alpha (drawn as
  1x), 2x source alpha above one half as a destination factor or with SUBTRACT
  (saturates). Those are reported, see below.
- Framebuffer alpha is written only in clear mode (or with the stencil test
  on, which is otherwise unimplemented). PSP alpha is the stencil plane.
- Register 0x1C is the GE's depth clamp. With it set, which is how the game keeps it, depth past the far
  plane is held at the end of the range and the geometry is drawn; the near plane still clips. Upstream
  clipped at both planes, which removed the hills on the horizon of the Story 1-1 intro (the game's far
  plane maps to depth -7.5). With the register clear both ends are clamped, as upstream did.
- A mip chain ends at the last level that is half the previous one. The game
  leaves stale size registers on top levels (16x256 texture, level 5 = 4x8).
- The adapter is chosen by vendor and dedicated memory, or by
  `GeGpuConfig::adapter`, instead of taking the first high-performance one.
- `Present` takes `GeGpuConfig::present_sync_interval`.

Added API (`ge_gpu_backend.hpp`, section "Chili additions"):

- `ge_gpu_backend_write_surface_rgba` / `_depth16`: CPU pixels into a GPU
  surface, queued in order with the draws. For movie frames and replay seeds.
- `ge_gpu_backend_flush`, `ge_gpu_backend_read_surface_rgba`: execute without
  presenting; blocking readback of one surface for screenshots and tools.
- `ge_gpu_backend_take_fallbacks`, `ge_gpu_fallback_names`,
  `GeGpuBackendReport::fallbacks_seen`: which unsupported or approximated GE
  state was drawn. The first sighting of each is logged once.
- `GeGpuDrawDescriptor` gained `depth_address`, `stencil_enabled`,
  `unsupported_state` and the host-only `surface_write`.

Presentation and screenshots:

- A flush (screenshot, tool) remembers that it drew on the displayed surface, so the next present still
  shows the picture (`display_pending`). Upstream lost that frame's present.

Known not to work with Chili: `GeGpuConfig::parallel_vertex_decode` (PSPRECOMP_GE_PARALLEL_VERTEX_DECODE=1)
crashes within seconds of the first mission. It is off by default; leave it off.

## Using it

All calls are made from the thread that executes GE lists.

    vcs::GeGpuConfig config;                       // or ge_gpu_config_from_environment()
    config.backend = vcs::GeGpuConfig::Backend::DirectX12;
    vcs::set_ge_gpu_config(config);
    vcs::initialize_ge_gpu_backend(error);         // false only when config.strict
    vcs::ge_gpu_backend_set_native_window(hwnd);
    vcs::ge_gpu_backend_set_display_framebuffer(0x04000000);
    // per PRIM: vcs::render_ge_primitive(memory, commands, transform, vaddr, iaddr, prim, stats, error, ...)
    // per frame: vcs::ge_gpu_backend_finish_color_frame(vblank)

With `Backend::Software`, or before `initialize_ge_gpu_backend`, the same
`render_ge_primitive` rasterizes into guest VRAM on the CPU.

The standalone build and the replay tool that exercises all of this are in
`profiles/chili/gpu_dev`.
