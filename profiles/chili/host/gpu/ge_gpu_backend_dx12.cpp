#include "ge_gpu_backend.hpp"
#include "ge_gpu_config.hpp"
#include "ge_gpu_log.hpp"
#include "../frame_costs.hpp"
#include "../picture_rect.hpp"
#include "ge_texture_key.hpp"
#include "ge_texture_mods.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "ge_glow_blocks.hpp"
#include "ge_sampler_rule.hpp"

namespace vcs {

namespace {

using Microsoft::WRL::ComPtr;
// The PSP LCD: what the swapchain shows of the displayed surface.
constexpr std::uint32_t kReferenceWidth = 480u;
constexpr std::uint32_t kReferenceHeight = 272u;
// Framebuffer registers address VRAM by a 2 MiB offset. Surfaces are keyed by
// that offset; textures match a surface only when they lie in VRAM.
constexpr std::uint32_t kVramOffsetMask = 0x001FFFF0u;
constexpr std::size_t kGeometryUploadCapacity = 64u * 1024u * 1024u;
// Stage 45.2: persistent per-frame texture upload arena. The 44.7 path created,
// mapped and destroyed one committed upload resource for every decoded texture.
// Streaming bursts therefore paid kernel/D3D12 allocation overhead on the hot GE
// thread even though the bytes live only until the frame fence completes. Keep a
// mapped arena per frame slot instead; fence-slot reuse makes it safe without any
// extra synchronization. Oversized bursts retain a committed-resource fallback.
constexpr std::size_t kTextureUploadCapacity = 32u * 1024u * 1024u;
constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
constexpr UINT kFrameCount = 2u;
constexpr UINT kSrvCapacity = 65536u;
constexpr UINT kSamplerCapacity = 128u;
constexpr UINT kFramebufferTargetCapacity = 256u;
// Chili: decoded textures are placed in heaps of this size instead of getting a
// committed resource each. CreateCommittedResource took 0.4 to 0.6 ms per
// texture (a kernel allocation and a residency wait), which made the frame
// that first shows a mission's 80 textures 30 to 45 ms long;
// CreatePlacedResource in an existing heap is a few microseconds.
constexpr UINT64 kTextureHeapBytes = 32ull * 1024ull * 1024ull;
constexpr std::uint32_t kNoTextureHeap = 0xFFFFFFFFu;
// Chili: a replacement texture carries a full chain (8192 wide at most).
constexpr std::uint32_t kMaxTextureLevels = 14u;
// Replacement pictures uploaded per submission once they arrive from the
// decoder, so a mission start with many of them does not stall one frame.
constexpr std::uint64_t kReplacementUploadBudget = 24u * 1024u * 1024u;

struct Dx12Batch {
    GeGpuDrawDescriptor draw{};
    std::uint32_t first_vertex{};
    std::uint32_t vertex_count{};
    std::uint32_t first_index{};
    std::uint32_t index_count{};
    bool indexed{};
    // Dominant VCS 0x000115 stream kept in its original 10-byte PSP encoding.
    // A separate IA layout/VS decodes UV/5551/s16 directly on the GPU.
    bool packed_0115{};
    std::uint32_t logical_draw_count{1u};
    bool framebuffer_feedback{};
    std::uint32_t feedback_address{};
    // Chili: the part of the surface a screen-space draw samples, in logical
    // pixels (left, top, right, bottom). A draw that samples the surface it
    // draws into needs a copy of what it reads: with the region known only
    // that part is copied, not the whole sheet (25 such draws per game frame in
    // a mission, 16 MB each at scale 4).
    bool feedback_region_known{};
    std::array<float, 4> feedback_region{};
    // Stage 44.7: keep model-space vertices model-space. The 44.6 backend
    // multiplied every hardware-transform vertex on the CPU before upload,
    // throwing away the main reason for having a native GPU frontend.
    bool hardware_transform{};
    GeGpuHardwareTransform transform{};
};

struct Dx12TransformConstants {
    std::array<float, 4> row0{};
    std::array<float, 4> row1{};
    std::array<float, 4> row2{};
    std::array<float, 4> row3{};
    std::array<float, 4> view_z{};
    std::array<float, 4> uv{1.0f, 1.0f, 0.0f, 0.0f};
    std::array<float, 4> fog{};
    std::array<std::uint32_t, 4> control{};
    // Stage 45.5: affine per-draw vertex lighting for the packed 0x0115 path.
    // This replaces tens of thousands of CPU lighting evaluations with eight
    // root constants and one VS MAD.
    std::array<float, 4> color_mul{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> color_add{};
};
static_assert(sizeof(Dx12TransformConstants) == 40u * sizeof(std::uint32_t));

// Stage 45.4: the native DX12 upload stream now carries only attributes that
// actually vary per vertex.  Alpha/texture/fog/framebuffer state is constant
// for a PSP draw and is sent once as root constants.  The old stream was 56
// bytes/vertex; this one is 36 bytes (-35.7% CPU memcpy + upload traffic).
struct Dx12UploadVertex {
    float x{};
    float y{};
    float z{};
    float w{1.0f};
    std::uint32_t rgba{0xFFFFFFFFu};
    float u{};
    float v{};
    float fog_factor{1.0f};
    float q{1.0f};
};
static_assert(sizeof(Dx12UploadVertex) == 36u);

struct Dx12PixelConstants {
    std::uint32_t alpha_control{};
    std::uint32_t texture_control{};
    std::uint32_t texture_env{};
    std::uint32_t fog_control{};
    std::uint32_t framebuffer_format{};
    // Low nibble: factor multiplied into the source colour. Next nibble: factor
    // written to SV_Target1 for the destination (dual-source blending).
    std::uint32_t blend_control{};
    std::uint32_t blend_fix_source{};
    std::uint32_t blend_fix_dest{};
};
constexpr UINT kPixelConstantWords = 8u;
static_assert(sizeof(Dx12PixelConstants) == kPixelConstantWords * sizeof(std::uint32_t));

struct Dx12FrameResources {
    // A pair of timestamps was resolved for this slot's last submission.
    bool timed{};
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12Resource> upload_buffer;
    std::byte *mapped_upload{};
    ComPtr<ID3D12Resource> texture_upload_buffer;
    std::byte *mapped_texture_upload{};
    std::size_t texture_upload_cursor{};
    UINT64 fence_value{};
    std::vector<ComPtr<ID3D12Resource>> transient_resources;
};

// A range of a texture heap. `heap` is kNoTextureHeap for a committed resource.
struct Dx12TextureBlock {
    std::uint32_t heap{kNoTextureHeap};
    UINT64 offset{};
    UINT64 bytes{};
    UINT64 alignment{};
};

struct Dx12Texture {
    GeGpuDrawDescriptor descriptor{};
    ComPtr<ID3D12Resource> image;
    Dx12TextureBlock block{};
    ComPtr<ID3D12Resource> pending_upload;
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t mip_levels{1u};
    std::uint32_t srv_index{};
    std::uint32_t sampler_index{};
    // Chili: the sampler of 3D draws, which [Rendering] Anisotropy and
    // TextureFilter change; through-mode draws keep `sampler_index`.
    std::uint32_t scene_sampler_index{};
    std::uint64_t checksum{};
    std::uint64_t signature_epoch{};
    std::uint64_t last_used_epoch{};
    // The decoded chain until it has been copied to the GPU, then empty.
    std::vector<std::byte> rgba8;
    // Chili: bytes of the GPU image, which is what the cache budget counts.
    std::uint64_t gpu_bytes{};
    bool upload_pending{};
    // Replacement key of the content (0: none) and, once a mod's picture is
    // drawn in its place, that picture until it has been uploaded.
    std::uint64_t content_key{};
    bool replaced{};
    // log2 of how much larger than the original the replacement is.
    float level_shift{};
    GeTextureReplacementPtr replacement;
};

struct Dx12FramebufferTarget {
    std::uint32_t address{};
    // Logical PSP coordinate/texture extent represented by this target. The
    // Stage 44.6 backend treated every target as 480x272 even though VCS uses
    // larger offscreen surfaces (the world surface is sampled back through a
    // larger texture). That cropped the offscreen image before composition and
    // appeared as a large zoom in the first physical DX12 run.
    std::uint32_t logical_width{};
    std::uint32_t logical_height{};
    // Chili: the size of its images in pixels: the logical extent times the
    // internal scale, or the logical extent itself for a glow sheet
    // (ge_gpu_glow_dx12.hpp), which is drawn at the PSP's own size.
    std::uint32_t physical_width{};
    std::uint32_t physical_height{};
    // `color` is always a single-sample shader-readable image. With MSAA=1 it
    // is also the render target. With MSAA>1, `msaa_color` receives rasterized
    // samples and is resolved into `color` before feedback/presentation.
    ComPtr<ID3D12Resource> color;
    ComPtr<ID3D12Resource> msaa_color;
    ComPtr<ID3D12Resource> feedback_copy;
    std::uint32_t rtv_index{};
    std::uint32_t srv_index{};
    std::uint32_t feedback_srv_index{};
    D3D12_RESOURCE_STATES color_state{D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
    D3D12_RESOURCE_STATES msaa_state{D3D12_RESOURCE_STATE_RENDER_TARGET};
    D3D12_RESOURCE_STATES feedback_state{D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
    std::uint64_t last_render_epoch{};
    // Chili: what has been drawn (in physical pixels) since the multisampled
    // image was last resolved into `color`, and since `feedback_copy` was last
    // brought up to date. Only these rectangles are resolved and copied; the
    // game's bloom samples its own target two dozen times a frame, and a whole
    // sheet each time is most of what multisampling used to cost.
    D3D12_RECT resolve_dirty{};
    D3D12_RECT snapshot_dirty{};
};

// Chili: one depth image per GE depth-buffer address. Every surface has the
// same physical size and sample count, so any colour surface can bind any of
// them; the scene and composite surfaces share the buffer at 0x04088000.
struct Dx12DepthTarget {
    ComPtr<ID3D12Resource> depth;
    std::uint32_t dsv_index{};
    bool cleared{};
    // With ambient occlusion on, the buffer is also readable by a shader. It is
    // in DEPTH_WRITE whenever the game draws; a post pass that reads it puts it
    // back before it returns.
    std::uint32_t srv_index{};
    D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_DEPTH_WRITE};
};

struct Dx12RetiredSrv {
    std::uint32_t index{};
    UINT64 fence_value{};
};

struct Dx12RetiredBlock {
    Dx12TextureBlock block{};
    UINT64 fence_value{};
};

struct Dx12GeState {
    GeGpuBackendReport report{};
    bool enabled{};
    std::uint32_t display_framebuffer{};
    std::uint32_t target_width{480u};
    std::uint32_t target_height{272u};
    UINT sample_count{1u};
    UINT sample_quality{};
    DXGI_FORMAT depth_format{kDepthFormat};
    std::uint32_t depth_bits{32u};
    std::vector<Dx12UploadVertex> vertices;
    std::vector<std::byte> packed_0115_vertices;
    std::vector<std::uint32_t> indices;
    std::vector<Dx12Batch> batches;
    std::vector<std::byte> frame_rgba;
    std::vector<std::byte> last_texture_rgba;

    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    std::array<Dx12FrameResources, kFrameCount> frames;
    UINT frame_cursor{};
    ComPtr<ID3D12GraphicsCommandList> list;
    // Chili: the same list, for ResolveSubresourceRegion; null on a system without it.
    ComPtr<ID3D12GraphicsCommandList1> list1;
    ComPtr<ID3D12CommandAllocator> texture_allocator;
    ComPtr<ID3D12GraphicsCommandList> texture_list;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12DescriptorHeap> dsv_heap;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    ComPtr<ID3D12DescriptorHeap> sampler_heap;
    UINT rtv_stride{};
    UINT dsv_stride{};
    UINT srv_stride{};
    UINT sampler_stride{};
    UINT next_rtv{};
    UINT next_dsv{};
    UINT next_srv{1u};
    UINT next_sampler{1u};
    std::unordered_map<std::uint32_t, Dx12FramebufferTarget> frame_targets;
    std::unordered_map<std::uint32_t, Dx12DepthTarget> depth_targets;
    ComPtr<ID3D12Resource> readback_buffer;
    // Chili: ge_gpu_backend_read_surface_rgba(), created on first use.
    ComPtr<ID3D12Resource> surface_readback_buffer;
    UINT64 surface_readback_bytes{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT readback_footprint{};
    UINT readback_rows{};
    UINT64 readback_row_size{};
    UINT64 readback_bytes{};
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event{};
    UINT64 next_fence{1u};
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3DBlob> vertex_shader;
    ComPtr<ID3DBlob> packed_0115_vertex_shader;
    ComPtr<ID3DBlob> pixel_shader;
    // Chili: the same shader reading its texture through a cubic filter, for
    // the draws that enlarge a glow sheet; null when no draw can need it.
    ComPtr<ID3DBlob> cubic_pixel_shader;
    ComPtr<ID3DBlob> depth_write_pixel_shader;
    std::unordered_map<std::uint64_t, ComPtr<ID3D12PipelineState>> pipelines;
    std::unordered_map<std::uint64_t, Dx12Texture> textures;
    // Hot draw streams reuse the same texture for long runs. Avoid repeating an
    // unordered_map probe in texture_available() and again while recording the
    // same batch sequence. unordered_map node addresses survive rehash; the
    // cache is explicitly cleared before erasing any texture node.
    std::uint64_t last_texture_lookup_key{};
    Dx12Texture *last_texture_lookup{};
    std::vector<std::uint64_t> pending_texture_keys;
    std::vector<std::uint32_t> free_texture_srvs;
    std::vector<Dx12RetiredSrv> retired_texture_srvs;
    // Chili: GPU time per submission. Two timestamps per frame slot, resolved
    // into a readback buffer and read when the slot comes round again.
    ComPtr<ID3D12QueryHeap> timestamp_heap;
    ComPtr<ID3D12Resource> timestamp_readback;
    UINT64 timestamp_frequency{};
    std::uint64_t gpu_time_count{};
    double gpu_time_total_ms{};
    double gpu_time_max_ms{};
    // 0.05 ms steps; the last bin takes everything from 50 ms up.
    std::array<std::uint32_t, 1001> gpu_time_bins{};
    // Texture heaps: filled front to back; a released range is used again for
    // a texture of the same size and alignment once no submitted frame can
    // still sample the old one.
    std::vector<ComPtr<ID3D12Heap>> texture_heaps;
    UINT64 texture_heap_cursor{};
    std::unordered_map<UINT64, std::vector<Dx12TextureBlock>> free_texture_blocks;
    std::vector<Dx12RetiredBlock> retired_texture_blocks;
    std::unordered_map<std::uint64_t, std::uint32_t> sampler_cache;
    std::unordered_set<std::uint32_t> known_frame_targets;
    std::uint32_t last_registered_framebuffer_target{0xFFFFFFFFu};
    std::uint64_t texture_cache_bytes{};
    std::uint64_t frame_epoch{1u};

    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D12DescriptorHeap> swap_rtv_heap;
    std::array<ComPtr<ID3D12Resource>, kFrameCount> backbuffers;
    UINT swap_rtv_stride{};
    std::uint32_t swap_width{};
    std::uint32_t swap_height{};
    ComPtr<ID3D12PipelineState> present_pipeline;
    ComPtr<ID3DBlob> present_vertex_shader;
    ComPtr<ID3DBlob> present_pixel_shader;
    bool direct_present_ok{};
    std::uint32_t presented_framebuffer{};
    std::uint32_t missed_display_intervals{};
    // Chili: a flush (screenshot, tool) executed draws on the displayed surface
    // that no present has shown yet. The next present must not skip them.
    bool display_pending{};
    bool swapchain_tearing{};
    HWND native_window{};
    bool readback_enabled{};
    bool texture_upload_ring_enabled{true};
    std::string adapter_name;
    // Chili: logical size of every surface sheet (GeGpuConfig::surface_*).
    std::uint32_t sheet_width{512u};
    std::uint32_t sheet_height{512u};
    std::uint32_t pending_fallbacks{};
    // Surface writes queued in the current frame, to give each its own texture.
    std::uint32_t surface_write_serial{};
    // Decoded replacement pictures not yet swapped in (upload budget).
    std::vector<GeTextureReplacementReady> ready_replacements;
    // Content keys whose overlay changed (ge_texture_mods.hpp): their cached
    // textures leave the cache at the end of the submission.
    std::vector<std::uint64_t> overlay_changes;
};

Dx12GeState &state() {
    static Dx12GeState s;
    return s;
}

std::uint64_t hash_mix(std::uint64_t hash, std::uint64_t value) noexcept {
    hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6u) + (hash >> 2u);
    return hash;
}

std::uint64_t texture_key(const GeGpuDrawDescriptor &draw) noexcept {
    if (draw.texture_cache_key_hint != 0u) return draw.texture_cache_key_hint;
    std::uint64_t key = 0xCBF29CE484222325ull;
    const std::uint32_t levels = draw.texture_level_addresses[0] != 0u && draw.texture_mipmap_enabled
        ? std::min<std::uint32_t>(8u, draw.texture_max_level + 1u) : 1u;
    key = hash_mix(key, levels);
    for (std::uint32_t level = 0u; level < levels; ++level) {
        key = hash_mix(key, draw.texture_level_addresses[level] != 0u
            ? draw.texture_level_addresses[level] : draw.texture_address);
        key = hash_mix(key, draw.texture_level_buffer_widths[level] != 0u
            ? draw.texture_level_buffer_widths[level] : draw.texture_buffer_width);
        key = hash_mix(key, draw.texture_level_widths[level] != 0u
            ? draw.texture_level_widths[level] : draw.texture_width);
        key = hash_mix(key, draw.texture_level_heights[level] != 0u
            ? draw.texture_level_heights[level] : draw.texture_height);
    }
    key = hash_mix(key, draw.texture_format);
    key = hash_mix(key, draw.clut_address);
    key = hash_mix(key, draw.clut_format);
    key = hash_mix(key, draw.clut_shift);
    key = hash_mix(key, draw.clut_mask);
    key = hash_mix(key, draw.clut_start);
    key = hash_mix(key, draw.clut_checksum);
    key = hash_mix(key, static_cast<std::uint64_t>(draw.texture_swizzled));
    key = hash_mix(key, static_cast<std::uint64_t>(draw.texture_min_linear));
    key = hash_mix(key, static_cast<std::uint64_t>(draw.texture_mag_linear));
    key = hash_mix(key, static_cast<std::uint64_t>(draw.texture_mipmap_enabled));
    key = hash_mix(key, static_cast<std::uint64_t>(draw.texture_mipmap_linear));
    key = hash_mix(key, draw.texture_max_level);
    key = hash_mix(key, draw.texture_level_mode);
    key = hash_mix(key, static_cast<std::uint32_t>(draw.texture_level_offset16));
    key = hash_mix(key, draw.texture_selected_level);
    key = hash_mix(key, static_cast<std::uint64_t>(draw.texture_clamp_u));
    key = hash_mix(key, static_cast<std::uint64_t>(draw.texture_clamp_v));
    return key;
}

D3D12_CPU_DESCRIPTOR_HANDLE rtv_cpu(Dx12GeState &s, UINT index) noexcept {
    D3D12_CPU_DESCRIPTOR_HANDLE h = s.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(index) * s.rtv_stride;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE dsv_cpu(Dx12GeState &s, UINT index) noexcept {
    D3D12_CPU_DESCRIPTOR_HANDLE h = s.dsv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(index) * s.dsv_stride;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu(Dx12GeState &s, UINT index) noexcept {
    D3D12_CPU_DESCRIPTOR_HANDLE h = s.srv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(index) * s.srv_stride;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu(Dx12GeState &s, UINT index) noexcept {
    D3D12_GPU_DESCRIPTOR_HANDLE h = s.srv_heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(index) * s.srv_stride;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE sampler_cpu(Dx12GeState &s, UINT index) noexcept {
    D3D12_CPU_DESCRIPTOR_HANDLE h = s.sampler_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(index) * s.sampler_stride;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE sampler_gpu(Dx12GeState &s, UINT index) noexcept {
    D3D12_GPU_DESCRIPTOR_HANDLE h = s.sampler_heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(index) * s.sampler_stride;
    return h;
}

void reap_retired_texture_srvs(Dx12GeState &s) noexcept {
    if (!s.fence || s.retired_texture_srvs.empty()) return;
    const UINT64 completed = s.fence->GetCompletedValue();
    std::size_t write = 0u;
    for (std::size_t i = 0u; i < s.retired_texture_srvs.size(); ++i) {
        const Dx12RetiredSrv retired = s.retired_texture_srvs[i];
        if (retired.fence_value == 0u || completed >= retired.fence_value) {
            s.free_texture_srvs.push_back(retired.index);
        } else {
            if (write != i) s.retired_texture_srvs[write] = retired;
            ++write;
        }
    }
    s.retired_texture_srvs.resize(write);
}

std::uint32_t allocate_texture_srv(Dx12GeState &s) noexcept {
    reap_retired_texture_srvs(s);
    if (!s.free_texture_srvs.empty()) {
        const std::uint32_t index = s.free_texture_srvs.back();
        s.free_texture_srvs.pop_back();
        ++s.report.recycled_texture_descriptor_sets;
        return index;
    }
    if (s.next_srv >= kSrvCapacity) return 0u;
    return s.next_srv++;
}

void retire_texture_srv(Dx12GeState &s, std::uint32_t index) noexcept {
    if (index == 0u) return;
    UINT64 retire_after = 0u;
    for (const Dx12FrameResources &frame : s.frames)
        retire_after = std::max(retire_after, frame.fence_value);
    if (!s.fence || retire_after == 0u || s.fence->GetCompletedValue() >= retire_after)
        s.free_texture_srvs.push_back(index);
    else
        s.retired_texture_srvs.push_back({index, retire_after});
}

UINT64 texture_block_class(const Dx12TextureBlock &block) noexcept {
    return (block.bytes << 1u) | (block.alignment < D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT ? 1u : 0u);
}

void reap_retired_texture_blocks(Dx12GeState &s) noexcept {
    if (!s.fence || s.retired_texture_blocks.empty()) return;
    const UINT64 completed = s.fence->GetCompletedValue();
    std::size_t write = 0u;
    for (std::size_t i = 0u; i < s.retired_texture_blocks.size(); ++i) {
        const Dx12RetiredBlock retired = s.retired_texture_blocks[i];
        if (retired.fence_value == 0u || completed >= retired.fence_value) {
            s.free_texture_blocks[texture_block_class(retired.block)].push_back(retired.block);
        } else {
            if (write != i) s.retired_texture_blocks[write] = retired;
            ++write;
        }
    }
    s.retired_texture_blocks.resize(write);
}

void retire_texture_block(Dx12GeState &s, const Dx12TextureBlock &block) noexcept {
    if (block.heap == kNoTextureHeap) return;
    UINT64 retire_after = 0u;
    for (const Dx12FrameResources &frame : s.frames)
        retire_after = std::max(retire_after, frame.fence_value);
    try {
        if (!s.fence || retire_after == 0u || s.fence->GetCompletedValue() >= retire_after)
            s.free_texture_blocks[texture_block_class(block)].push_back(block);
        else
            s.retired_texture_blocks.push_back({block, retire_after});
    } catch (...) {
        // The range is lost until the backend is destroyed; nothing else depends on it.
    }
}

bool add_texture_heap(Dx12GeState &s) noexcept {
    D3D12_HEAP_DESC desc{};
    desc.SizeInBytes = kTextureHeapBytes;
    desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
    ComPtr<ID3D12Heap> heap;
    if (FAILED(s.device->CreateHeap(&desc, IID_PPV_ARGS(&heap)))) return false;
    try { s.texture_heaps.push_back(std::move(heap)); } catch (...) { return false; }
    s.texture_heap_cursor = 0u;
    return true;
}

// Creates the image of a decoded texture in COPY_DEST state: placed in a
// texture heap when it fits, a committed resource otherwise.
HRESULT place_texture_image(Dx12GeState &s, const D3D12_RESOURCE_DESC &desc,
                            Dx12Texture &texture) noexcept {
    D3D12_RESOURCE_DESC placed = desc;
    // Most PSP textures are far smaller than the 64 KiB default alignment.
    placed.Alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
    D3D12_RESOURCE_ALLOCATION_INFO info = s.device->GetResourceAllocationInfo(0u, 1u, &placed);
    if (info.SizeInBytes == UINT64_MAX || info.Alignment != D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT) {
        placed.Alignment = 0u;
        info = s.device->GetResourceAllocationInfo(0u, 1u, &placed);
    }
    if (info.SizeInBytes != UINT64_MAX && info.SizeInBytes != 0u && info.Alignment != 0u &&
        info.SizeInBytes <= kTextureHeapBytes / 4u) {
        reap_retired_texture_blocks(s);
        Dx12TextureBlock block{kNoTextureHeap, 0u, info.SizeInBytes, info.Alignment};
        if (const auto free_list = s.free_texture_blocks.find(texture_block_class(block));
            free_list != s.free_texture_blocks.end() && !free_list->second.empty()) {
            block = free_list->second.back();
            free_list->second.pop_back();
        } else {
            UINT64 offset = (s.texture_heap_cursor + info.Alignment - 1u) & ~(info.Alignment - 1u);
            if (s.texture_heaps.empty() || offset + info.SizeInBytes > kTextureHeapBytes) {
                if (add_texture_heap(s)) offset = 0u;
                else offset = UINT64_MAX;
            }
            if (offset != UINT64_MAX) {
                block.heap = static_cast<std::uint32_t>(s.texture_heaps.size() - 1u);
                block.offset = offset;
                s.texture_heap_cursor = offset + info.SizeInBytes;
            }
        }
        if (block.heap != kNoTextureHeap) {
            const HRESULT hr = s.device->CreatePlacedResource(
                s.texture_heaps[block.heap].Get(), block.offset, &placed,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture.image));
            if (SUCCEEDED(hr)) {
                texture.block = block;
                return hr;
            }
            texture.image.Reset();
            try { s.free_texture_blocks[texture_block_class(block)].push_back(block); } catch (...) {}
        }
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    texture.block = {};
    return s.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&texture.image));
}

D3D12_CPU_DESCRIPTOR_HANDLE swap_rtv(Dx12GeState &s, UINT index) noexcept {
    D3D12_CPU_DESCRIPTOR_HANDLE h = s.swap_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(index) * s.swap_rtv_stride;
    return h;
}

// Content checksum of a decoded texture: process-local, compared only with
// itself. Eight bytes per step; the byte-at-a-time FNV it replaces cost a
// millisecond per megabyte in the frames that upload textures.
std::uint64_t fnv1a64(std::span<const std::byte> bytes) noexcept {
    std::uint64_t hash = 1469598103934665603ull ^ bytes.size();
    std::size_t offset = 0u;
    for (; offset + 8u <= bytes.size(); offset += 8u) {
        std::uint64_t word;
        std::memcpy(&word, bytes.data() + offset, sizeof(word));
        hash = (hash ^ word) * 0x9E3779B97F4A7C15ull;
        hash ^= hash >> 29u;
    }
    for (; offset < bytes.size(); ++offset) {
        hash ^= static_cast<std::uint8_t>(bytes[offset]);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint32_t packed_texture_control(const GeGpuDrawDescriptor &draw, bool enabled) noexcept {
    return (draw.texture_function & 0xFFu) |
           (static_cast<std::uint32_t>(draw.texture_use_alpha ? 1u : 0u) << 8u) |
           (static_cast<std::uint32_t>(draw.texture_double_color ? 1u : 0u) << 16u) |
           (static_cast<std::uint32_t>(enabled ? 1u : 0u) << 24u);
}

std::uint32_t packed_alpha_control(const GeGpuDrawDescriptor &draw) noexcept {
    return static_cast<std::uint32_t>(draw.alpha_test_enabled ? 1u : 0u) |
           ((draw.alpha_function & 7u) << 8u) |
           ((draw.alpha_reference & 0xFFu) << 16u) |
           ((draw.alpha_mask & 0xFFu) << 24u);
}

Dx12UploadVertex make_upload_vertex(const GeGpuVertex &source) noexcept {
    return {source.x, source.y, source.z, source.w, source.rgba, source.u, source.v,
            source.fog_factor, source.q};
}

bool native_indexed_draw_enabled() noexcept { return ge_gpu_config().native_indexed_draw; }

std::string hr_text(HRESULT hr, const char *where) {
    std::ostringstream out;
    out << where << " failed (HRESULT=0x" << std::hex << std::uppercase
        << static_cast<unsigned long>(hr) << ')';
    LPSTR message = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    if (FormatMessageA(flags, nullptr, static_cast<DWORD>(hr),
                       MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       reinterpret_cast<LPSTR>(&message), 0u, nullptr) != 0u && message != nullptr) {
        std::string text(message);
        LocalFree(message);
        while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) text.pop_back();
        if (!text.empty()) out << ": " << text;
    }
    return out.str();
}

void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept {
    if (before == after || list == nullptr || resource == nullptr) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1u, &barrier);
}

bool rect_empty(const D3D12_RECT &rect) noexcept {
    return rect.right <= rect.left || rect.bottom <= rect.top;
}

void rect_include(D3D12_RECT &into, const D3D12_RECT &rect) noexcept {
    if (rect_empty(rect)) return;
    if (rect_empty(into)) { into = rect; return; }
    into.left = std::min(into.left, rect.left);
    into.top = std::min(into.top, rect.top);
    into.right = std::max(into.right, rect.right);
    into.bottom = std::max(into.bottom, rect.bottom);
}

// Chili: records that `rect` of the target is about to be drawn into.
void note_target_write(Dx12FramebufferTarget &target, const D3D12_RECT &rect) noexcept {
    rect_include(target.resolve_dirty, rect);
    rect_include(target.snapshot_dirty, rect);
}

void prepare_target_for_render(Dx12GeState &s, Dx12FramebufferTarget &target) noexcept {
    if (target.msaa_color) {
        transition(s.list.Get(), target.msaa_color.Get(), target.msaa_state,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
        target.msaa_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    } else {
        transition(s.list.Get(), target.color.Get(), target.color_state,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
        target.color_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
}

void resolve_target_for_sampling(Dx12GeState &s, Dx12FramebufferTarget &target,
                                 bool resume_render) noexcept {
    if (target.msaa_color && rect_empty(target.resolve_dirty)) {
        // Nothing drawn since the last resolve: `color` is current.
        transition(s.list.Get(), target.color.Get(), target.color_state,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        target.color_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        if (resume_render) prepare_target_for_render(s, target);
    } else if (target.msaa_color) {
        transition(s.list.Get(), target.msaa_color.Get(), target.msaa_state,
                   D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        target.msaa_state = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
        transition(s.list.Get(), target.color.Get(), target.color_state,
                   D3D12_RESOURCE_STATE_RESOLVE_DEST);
        target.color_state = D3D12_RESOURCE_STATE_RESOLVE_DEST;
        if (s.list1) {
            D3D12_RECT region = target.resolve_dirty;
            s.list1->ResolveSubresourceRegion(target.color.Get(), 0u, static_cast<UINT>(region.left),
                                              static_cast<UINT>(region.top), target.msaa_color.Get(), 0u,
                                              &region, kColorFormat, D3D12_RESOLVE_MODE_AVERAGE);
        } else {
            s.list->ResolveSubresource(target.color.Get(), 0u, target.msaa_color.Get(), 0u,
                                       kColorFormat);
        }
        target.resolve_dirty = {};
        ++s.report.dx12_resolves;
        transition(s.list.Get(), target.color.Get(), target.color_state,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        target.color_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        if (resume_render) prepare_target_for_render(s, target);
    } else {
        transition(s.list.Get(), target.color.Get(), target.color_state,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        target.color_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
}

bool wait_for_fence(Dx12GeState &s, UINT64 value, std::string &error) noexcept {
    if (value == 0u || !s.fence || s.fence->GetCompletedValue() >= value) return true;
    HRESULT hr = s.fence->SetEventOnCompletion(value, s.fence_event);
    if (FAILED(hr)) {
        error = hr_text(hr, "ID3D12Fence::SetEventOnCompletion(GE frame)");
        return false;
    }
    const DWORD result = WaitForSingleObject(s.fence_event, 5000u);
    if (result != WAIT_OBJECT_0) {
        std::ostringstream out;
        out << "DirectX 12 frame fence wait failed/timed out (wait=" << result << ')';
        if (s.device) {
            const HRESULT removed = s.device->GetDeviceRemovedReason();
            if (FAILED(removed)) out << "; device removed reason=0x" << std::hex
                                     << static_cast<unsigned long>(removed);
        }
        error = out.str();
        return false;
    }
    return true;
}

bool wait_for_gpu(Dx12GeState &s, std::string &error) noexcept {
    if (!s.queue || !s.fence) return true;
    const UINT64 value = s.next_fence++;
    HRESULT hr = s.queue->Signal(s.fence.Get(), value);
    if (FAILED(hr)) {
        error = hr_text(hr, "ID3D12CommandQueue::Signal(GE)");
        return false;
    }
    if (s.fence->GetCompletedValue() >= value) return true;
    hr = s.fence->SetEventOnCompletion(value, s.fence_event);
    if (FAILED(hr)) {
        error = hr_text(hr, "ID3D12Fence::SetEventOnCompletion(GE)");
        return false;
    }
    const DWORD result = WaitForSingleObject(s.fence_event, 5000u);
    if (result != WAIT_OBJECT_0) {
        std::ostringstream out;
        out << "DirectX 12 GE fence wait failed/timed out (wait=" << result << ')';
        if (s.device) {
            const HRESULT removed = s.device->GetDeviceRemovedReason();
            if (FAILED(removed)) out << "; device removed reason=0x" << std::hex
                                     << static_cast<unsigned long>(removed);
        }
        error = out.str();
        return false;
    }
    return true;
}

std::string lower_ascii(std::string text) {
    for (char &c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

// Chili: the first high-performance adapter is not always the real GPU. On a
// cloud or virtual machine DXGI also lists the hypervisor's display adapter
// (this machine has a Red Hat QXL controller next to the RTX A4500), and the
// window may sit on that one. Rank every Direct3D 12 capable adapter: a name
// the configuration asks for first, then real GPU vendors, then dedicated
// video memory.
bool select_adapter(Dx12GeState &s, std::string &error) noexcept {
    const std::string wanted = lower_ascii(ge_gpu_config().adapter);
    ComPtr<IDXGIAdapter1> best;
    std::string best_name;
    std::uint64_t best_score = 0u;
    for (UINT index = 0u;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        HRESULT hr = s.factory->EnumAdapterByGpuPreference(
            index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate));
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) continue;
        DXGI_ADAPTER_DESC1 desc{};
        candidate->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0u) continue;
        if (FAILED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0,
                                     __uuidof(ID3D12Device), nullptr))) continue;
        char utf8[512]{};
        const int count = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                                               utf8, static_cast<int>(sizeof(utf8)), nullptr, nullptr);
        const std::string name = count > 0 ? utf8 : "Direct3D 12 adapter";
        std::uint64_t vendor_rank = 1u;
        switch (desc.VendorId) {
        case 0x10DEu: case 0x1002u: case 0x1022u: vendor_rank = 3u; break;  // NVIDIA, AMD
        case 0x8086u: case 0x5143u: vendor_rank = 2u; break;                // Intel, Qualcomm
        default: break;  // hypervisor and indirect display adapters
        }
        const std::uint64_t memory_mb = std::min<std::uint64_t>(
            static_cast<std::uint64_t>(desc.DedicatedVideoMemory) >> 20u, 0xFFFFFFu);
        // Enumeration order is the last tie-break: +1 keeps every score nonzero
        // and the strict comparison below keeps the earlier adapter.
        std::uint64_t score = (vendor_rank << 32u) | (memory_mb << 8u) | 1u;
        if (!wanted.empty() && lower_ascii(name).find(wanted) != std::string::npos)
            score |= std::uint64_t{1} << 48u;
        runtime_log_line("dx12 ge adapter candidate \"" + name + "\" vendor=0x" +
                         [&] { std::ostringstream hex; hex << std::hex << desc.VendorId; return hex.str(); }() +
                         " dedicated_mb=" + std::to_string(memory_mb));
        if (score > best_score) {
            best_score = score;
            best = candidate;
            best_name = name;
        }
    }
    if (!best) {
        error = "No Direct3D 12-capable hardware adapter was found for the GE renderer";
        return false;
    }
    s.adapter = best;
    s.adapter_name = best_name;
    return true;
}

DXGI_FORMAT requested_depth_format(std::uint32_t bits) noexcept {
    if (bits <= 16u) return DXGI_FORMAT_D16_UNORM;
    if (bits <= 24u) return DXGI_FORMAT_D24_UNORM_S8_UINT;
    return DXGI_FORMAT_D32_FLOAT;
}

bool format_supports_depth(ID3D12Device *device, DXGI_FORMAT format) noexcept {
    if (device == nullptr) return false;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
    support.Format = format;
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))))
        return false;
    return (support.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL) != 0u;
}

void select_depth_and_msaa(Dx12GeState &s) noexcept {
    const GeGpuConfig &rendering = ge_gpu_config();
    s.depth_bits = rendering.depth_precision;
    // Chili: 16 bits cannot tell surfaces apart beyond a few metres, which
    // ambient occlusion needs.
    if (rendering.ambient_occlusion != GeGpuConfig::AmbientOcclusion::Off && s.depth_bits < 24u)
        s.depth_bits = 24u;
    s.depth_format = requested_depth_format(s.depth_bits);
    if (!format_supports_depth(s.device.Get(), s.depth_format)) {
        s.depth_format = DXGI_FORMAT_D32_FLOAT;
        s.depth_bits = 32u;
    }

    UINT requested = static_cast<UINT>(std::clamp(rendering.msaa, 1u, 16u));
    if (requested != 1u && requested != 2u && requested != 4u &&
        requested != 8u && requested != 16u) requested = 1u;
    s.sample_count = 1u;
    s.sample_quality = 0u;
    for (UINT samples = requested; samples >= 2u; samples >>= 1u) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS color{};
        color.Format = kColorFormat;
        color.SampleCount = samples;
        color.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS depth{};
        depth.Format = s.depth_format;
        depth.SampleCount = samples;
        depth.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
        const bool color_ok = SUCCEEDED(s.device->CheckFeatureSupport(
            D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &color, sizeof(color))) &&
            color.NumQualityLevels != 0u;
        const bool depth_ok = SUCCEEDED(s.device->CheckFeatureSupport(
            D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &depth, sizeof(depth))) &&
            depth.NumQualityLevels != 0u;
        if (color_ok && depth_ok) {
            s.sample_count = samples;
            // Quality 0 is universally valid whenever NumQualityLevels > 0 and
            // avoids vendor-specific quality-mode assumptions.
            s.sample_quality = 0u;
            break;
        }
    }
    s.report.dx12_msaa_samples = s.sample_count;
    s.report.dx12_depth_bits = s.depth_bits;
}

D3D12_COMPARISON_FUNC depth_compare(std::uint32_t function) noexcept {
    switch (function & 7u) {
    case 0u: return D3D12_COMPARISON_FUNC_NEVER;
    case 1u: return D3D12_COMPARISON_FUNC_ALWAYS;
    case 2u: return D3D12_COMPARISON_FUNC_EQUAL;
    case 3u: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case 4u: return D3D12_COMPARISON_FUNC_LESS;
    case 5u: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case 6u: return D3D12_COMPARISON_FUNC_GREATER;
    case 7u: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    }
    return D3D12_COMPARISON_FUNC_ALWAYS;
}

// Chili: the complete PSP blend table.
//
// result = equation(Cs * A, Cd * B). Factors that depend only on the fragment
// (source colour or alpha, a fixed colour) are evaluated in the pixel shader:
// the source one is multiplied into the output colour and the destination one
// is emitted as SV_Target1 and applied by dual-source blending. That is what
// lets two independent fixed colours work with D3D's single blend constant.
// Factors that depend on the framebuffer stay with the output merger.
enum Dx12ShaderFactor : std::uint32_t {
    kShaderFactorOne = 0u,
    kShaderFactorSourceColor = 1u,
    kShaderFactorInverseSourceColor = 2u,
    kShaderFactorSourceAlpha = 3u,
    kShaderFactorInverseSourceAlpha = 4u,
    kShaderFactorDoubleSourceAlpha = 5u,
    kShaderFactorInverseDoubleSourceAlpha = 6u,
    kShaderFactorFixed = 7u,
};

struct Dx12BlendState {
    bool enabled{};
    D3D12_BLEND source{D3D12_BLEND_ONE};
    D3D12_BLEND destination{D3D12_BLEND_ZERO};
    D3D12_BLEND_OP operation{D3D12_BLEND_OP_ADD};
    std::uint32_t shader_source{kShaderFactorOne};
    std::uint32_t shader_destination{kShaderFactorOne};
    std::uint32_t fallbacks{};
};

Dx12BlendState resolve_blend(const GeGpuDrawDescriptor &draw) noexcept {
    Dx12BlendState out{};
    if (!draw.blend_enabled || draw.clear_mode ||
        draw.surface_write != GeGpuSurfaceWrite::None) return out;
    const std::uint32_t equation = draw.blend_equation & 7u;
    if (equation == 5u) {
        // |Cs - Cd| needs the framebuffer in the shader. Not used by the game
        // so far; drawn unblended and reported.
        out.fallbacks |= kGeGpuFallbackBlendAbs;
        return out;
    }
    out.enabled = true;
    if (equation == 3u || equation == 4u) {
        // MIN and MAX ignore both factors on the PSP as well.
        out.source = D3D12_BLEND_ONE;
        out.destination = D3D12_BLEND_ONE;
        out.operation = equation == 3u ? D3D12_BLEND_OP_MIN : D3D12_BLEND_OP_MAX;
        return out;
    }
    // D3D: SUBTRACT = source - destination, REV_SUBTRACT = destination - source.
    out.operation = equation == 1u ? D3D12_BLEND_OP_SUBTRACT
                  : equation == 2u ? D3D12_BLEND_OP_REV_SUBTRACT
                                   : D3D12_BLEND_OP_ADD;

    out.source = D3D12_BLEND_ONE;
    switch (draw.blend_source_factor & 0xFu) {
    case 0u: out.source = D3D12_BLEND_DEST_COLOR; break;
    case 1u: out.source = D3D12_BLEND_INV_DEST_COLOR; break;
    case 2u: out.shader_source = kShaderFactorSourceAlpha; break;
    case 3u: out.shader_source = kShaderFactorInverseSourceAlpha; break;
    case 4u: out.source = D3D12_BLEND_DEST_ALPHA; break;
    case 5u: out.source = D3D12_BLEND_INV_DEST_ALPHA; break;
    case 6u: out.shader_source = kShaderFactorDoubleSourceAlpha;
             // Cs * 2As may exceed 1 on the PSP; the output merger clamps it.
             // ADD and REVERSE_SUBTRACT clamp to the same result, SUBTRACT does not.
             if (equation == 1u) out.fallbacks |= kGeGpuFallbackBlendDoubleAlpha;
             break;
    case 7u: out.shader_source = kShaderFactorInverseDoubleSourceAlpha; break;
    // 2 x destination alpha has no output-merger equivalent: drawn with 1 x.
    case 8u: out.source = D3D12_BLEND_DEST_ALPHA;
             out.fallbacks |= kGeGpuFallbackBlendDoubleAlpha; break;
    case 9u: out.source = D3D12_BLEND_INV_DEST_ALPHA;
             out.fallbacks |= kGeGpuFallbackBlendDoubleAlpha; break;
    default: out.shader_source = kShaderFactorFixed; break;
    }

    out.destination = D3D12_BLEND_SRC1_COLOR;
    switch (draw.blend_dest_factor & 0xFu) {
    case 0u: out.shader_destination = kShaderFactorSourceColor; break;
    case 1u: out.shader_destination = kShaderFactorInverseSourceColor; break;
    case 2u: out.shader_destination = kShaderFactorSourceAlpha; break;
    case 3u: out.shader_destination = kShaderFactorInverseSourceAlpha; break;
    case 4u: out.destination = D3D12_BLEND_DEST_ALPHA; break;
    case 5u: out.destination = D3D12_BLEND_INV_DEST_ALPHA; break;
    case 6u: out.shader_destination = kShaderFactorDoubleSourceAlpha;
             // A blend factor above 1 does not exist for a UNORM target.
             out.fallbacks |= kGeGpuFallbackBlendDoubleAlpha;
             break;
    case 7u: out.shader_destination = kShaderFactorInverseDoubleSourceAlpha; break;
    case 8u: out.destination = D3D12_BLEND_DEST_ALPHA;
             out.fallbacks |= kGeGpuFallbackBlendDoubleAlpha; break;
    case 9u: out.destination = D3D12_BLEND_INV_DEST_ALPHA;
             out.fallbacks |= kGeGpuFallbackBlendDoubleAlpha; break;
    default: out.shader_destination = kShaderFactorFixed; break;
    }
    return out;
}

// True when a clear-mode draw has to write depth. Clear mode disables the
// depth test, and D3D only writes depth while the test is enabled, so these
// draws run the test with ALWAYS.
bool clear_writes_depth(const GeGpuDrawDescriptor &draw) noexcept {
    return draw.clear_mode && draw.depth_write_enabled;
}

std::uint8_t color_write_mask(const GeGpuDrawDescriptor &draw) noexcept {
    if (draw.surface_write == GeGpuSurfaceWrite::Color) return D3D12_COLOR_WRITE_ENABLE_ALL;
    if (draw.surface_write == GeGpuSurfaceWrite::Depth) return 0u;
    std::uint8_t mask = 0u;
    for (std::uint32_t channel = 0u; channel < 4u; ++channel) {
        const std::uint32_t byte = (draw.color_write_mask >> (channel * 8u)) & 0xFFu;
        if (byte != 0xFFu) mask |= static_cast<std::uint8_t>(1u << channel);
    }
    // PSP framebuffer alpha is the stencil plane. Outside clear mode it only
    // changes through the stencil operation, so with the stencil test off the
    // destination alpha is preserved whatever PMSKA says.
    if (!draw.clear_mode && !draw.stencil_enabled)
        mask &= static_cast<std::uint8_t>(~D3D12_COLOR_WRITE_ENABLE_ALPHA);
    // Chili: with ambient occlusion on, a 3D draw that writes depth also leaves
    // its alpha in the surface, so the pass can tell a cut-out's transparent
    // fringe from a solid pixel (ge_gpu_post_dx12.hpp).
    if (ge_gpu_config().ambient_occlusion != GeGpuConfig::AmbientOcclusion::Off && !draw.through &&
        !draw.clear_mode && draw.depth_test_enabled && draw.depth_write_enabled)
        mask |= D3D12_COLOR_WRITE_ENABLE_ALPHA;
    return mask;
}

std::uint32_t draw_fallbacks(const GeGpuDrawDescriptor &draw) noexcept {
    if (draw.surface_write != GeGpuSurfaceWrite::None) return 0u;
    std::uint32_t fallbacks = draw.unsupported_state | resolve_blend(draw).fallbacks;
    for (std::uint32_t channel = 0u; channel < 4u; ++channel) {
        const std::uint32_t byte = (draw.color_write_mask >> (channel * 8u)) & 0xFFu;
        if (byte != 0u && byte != 0xFFu && !draw.clear_mode)
            fallbacks |= kGeGpuFallbackPartialColorMask;
    }
    return fallbacks;
}

// Chili: bits of a pipeline key above the draw's own (pipeline_key()), beside
// the three pipeline_for() adds at 61 to 63. They say what the draw is drawn
// into and read through, which is not in its GE state.
// A glow sheet is a single-sample surface: with multisampling on, a draw into
// it needs a state of its own.
constexpr std::uint64_t kPipelineSingleSample = std::uint64_t{1} << 60u;
// The draw enlarges a glow sheet: the pixel shader with the cubic filter.
constexpr std::uint64_t kPipelineCubic = std::uint64_t{1} << 59u;

std::uint64_t pipeline_key(const GeGpuDrawDescriptor &draw) noexcept {
    const Dx12BlendState blend = resolve_blend(draw);
    std::uint64_t key = static_cast<std::uint64_t>(draw.depth_test_enabled ? 1u : 0u);
    key |= static_cast<std::uint64_t>(draw.depth_write_enabled ? 1u : 0u) << 1u;
    key |= static_cast<std::uint64_t>(draw.depth_function & 7u) << 2u;
    key |= static_cast<std::uint64_t>(clear_writes_depth(draw) ? 1u : 0u) << 5u;
    key |= static_cast<std::uint64_t>(draw.surface_write) << 6u;
    key |= static_cast<std::uint64_t>(color_write_mask(draw) & 0xFu) << 8u;
    key |= static_cast<std::uint64_t>(blend.enabled ? 1u : 0u) << 12u;
    key |= static_cast<std::uint64_t>(static_cast<std::uint32_t>(blend.source) & 0x1Fu) << 13u;
    key |= static_cast<std::uint64_t>(static_cast<std::uint32_t>(blend.destination) & 0x1Fu) << 18u;
    key |= static_cast<std::uint64_t>(static_cast<std::uint32_t>(blend.operation) & 0x7u) << 23u;
    return key;
}

// Chili: a 3D draw whose texture filtering the settings change. Surfaces
// sampled as textures (screen copy, bloom) and through-mode sprites never are.
bool scene_filtered(const GeGpuDrawDescriptor &draw) noexcept {
    const GeGpuConfig &config = ge_gpu_config();
    return !draw.through && draw.texture_enabled && draw.surface_write == GeGpuSurfaceWrite::None &&
           (config.anisotropic_filtering > 1u || config.texture_filter != GeGpuConfig::TextureFilter::Game);
}

// Bits above the framebuffer format in Dx12PixelConstants::framebuffer_format.
// 0x100: sample with the texture's real footprint (anisotropic filtering)
// instead of the GE's axis-only level rule. 0x200: the draw uses the scene
// sampler, which also keeps it from merging with a through-mode neighbour.
D3D12_FILTER texture_filter(const GeGpuDrawDescriptor &draw, bool scene) noexcept;
std::uint32_t scene_filter_flags(const GeGpuDrawDescriptor &draw) noexcept {
    if (!scene_filtered(draw)) return 0u;
    return 0x200u | (texture_filter(draw, true) == D3D12_FILTER_ANISOTROPIC ? 0x100u : 0u);
}

Dx12PixelConstants make_pixel_constants(const GeGpuDrawDescriptor &draw,
                                        bool sampled_texture) noexcept {
    Dx12PixelConstants out{};
    out.alpha_control = packed_alpha_control(draw);
    out.texture_control = packed_texture_control(draw, sampled_texture);
    out.texture_env = draw.texture_env & 0x00FFFFFFu;
    out.fog_control = (draw.fog_color & 0x00FFFFFFu) |
        (static_cast<std::uint32_t>(draw.fog_enabled ? 0xFFu : 0u) << 24u);
    out.framebuffer_format = (draw.framebuffer_format & 3u) | scene_filter_flags(draw);
    const Dx12BlendState blend = resolve_blend(draw);
    if (blend.enabled) {
        out.blend_control = blend.shader_source | (blend.shader_destination << 4u);
        if (blend.shader_source == kShaderFactorFixed)
            out.blend_fix_source = draw.blend_fix_source & 0x00FFFFFFu;
        if (blend.shader_destination == kShaderFactorFixed)
            out.blend_fix_dest = draw.blend_fix_dest & 0x00FFFFFFu;
    }
    return out;
}

bool hardware_transform_equal(const GeGpuHardwareTransform &a,
                              const GeGpuHardwareTransform &b) noexcept {
    // Only fields consumed by make_transform_constants() participate.  The
    // trailing accounting counters deliberately do not: two adjacent PRIMs can
    // have different logical batching statistics while still sharing identical
    // shader constants.  Explicit field comparison avoids relying on struct
    // padding with memcmp.
    return a.model_to_clip == b.model_to_clip &&
           a.model_to_view_z == b.model_to_view_z &&
           a.viewport_scale_x == b.viewport_scale_x &&
           a.viewport_scale_y == b.viewport_scale_y &&
           a.viewport_scale_z == b.viewport_scale_z &&
           a.viewport_center_x == b.viewport_center_x &&
           a.viewport_center_y == b.viewport_center_y &&
           a.viewport_center_z == b.viewport_center_z &&
           a.viewport_offset_x == b.viewport_offset_x &&
           a.viewport_offset_y == b.viewport_offset_y &&
           a.uv_scale_u == b.uv_scale_u && a.uv_scale_v == b.uv_scale_v &&
           a.uv_offset_u == b.uv_offset_u && a.uv_offset_v == b.uv_offset_v &&
           a.fog_end == b.fog_end && a.fog_slope == b.fog_slope &&
           a.depth_clip_enabled == b.depth_clip_enabled &&
           a.cull_enabled == b.cull_enabled &&
           a.accept_counter_clockwise == b.accept_counter_clockwise &&
           a.primitive == b.primitive &&
           a.vertex_color_affine == b.vertex_color_affine &&
           a.vertex_color_mul == b.vertex_color_mul &&
           a.vertex_color_add == b.vertex_color_add;
}

bool adjacent_batch_merge_compatible(const Dx12Batch &a, const Dx12Batch &b) noexcept {
    // Framebuffer-feedback draws have an exact snapshot boundary. Indexed lists
    // are mergeable too: append_or_merge_batch() rebases the new local indices
    // onto the previous batch's base vertex before extending the draw.
    if (a.framebuffer_feedback || b.framebuffer_feedback) return false;
    if (a.indexed != b.indexed) return false;
    if (a.packed_0115 != b.packed_0115) return false;
    if (a.first_vertex + a.vertex_count != b.first_vertex) return false;
    if (a.indexed && a.first_index + a.index_count != b.first_index) return false;
    if ((a.draw.framebuffer_address & kVramOffsetMask) !=
        (b.draw.framebuffer_address & kVramOffsetMask)) return false;
    if (a.hardware_transform != b.hardware_transform) return false;
    // Concatenating independent triangle strips would synthesize bridge
    // triangles between PRIMs. Only triangle-list batches may merge.
    if ((a.hardware_transform && a.transform.primitive != 3u) ||
        (b.hardware_transform && b.transform.primitive != 3u)) return false;
    if (pipeline_key(a.draw) != pipeline_key(b.draw)) return false;
    if (a.draw.texture_enabled != b.draw.texture_enabled) return false;
    if (a.draw.texture_enabled && texture_key(a.draw) != texture_key(b.draw)) return false;
    if (a.draw.scissor_x0 != b.draw.scissor_x0 || a.draw.scissor_y0 != b.draw.scissor_y0 ||
        a.draw.scissor_x1 != b.draw.scissor_x1 || a.draw.scissor_y1 != b.draw.scissor_y1)
        return false;
    if ((a.draw.depth_address & kVramOffsetMask) !=
        (b.draw.depth_address & kVramOffsetMask)) return false;
    if (a.draw.surface_write != GeGpuSurfaceWrite::None) return false;
    // Pixel state moved out of the vertex stream in 45.4, so merged draws must
    // share the exact root-constant state rather than merely a compatible PSO.
    const Dx12PixelConstants pa = make_pixel_constants(a.draw, a.draw.texture_enabled);
    const Dx12PixelConstants pb = make_pixel_constants(b.draw, b.draw.texture_enabled);
    if (std::memcmp(&pa, &pb, sizeof(pa)) != 0) return false;
    if (a.draw.texture_mipmap_enabled != b.draw.texture_mipmap_enabled ||
        a.draw.texture_mipmap_linear != b.draw.texture_mipmap_linear) return false;
    return !a.hardware_transform || hardware_transform_equal(a.transform, b.transform);
}

bool append_or_merge_batch(Dx12GeState &s, Dx12Batch batch) {
    const bool merge_enabled = ge_gpu_config().batch_merge;
    if (merge_enabled && !s.batches.empty() && adjacent_batch_merge_compatible(s.batches.back(), batch)) {
        Dx12Batch &previous = s.batches.back();
        const bool counts_fit =
            batch.vertex_count <= std::numeric_limits<std::uint32_t>::max() - previous.vertex_count &&
            (!batch.indexed || batch.index_count <=
                std::numeric_limits<std::uint32_t>::max() - previous.index_count);
        bool indices_fit = counts_fit;
        std::size_t begin = 0u, end = 0u;
        std::uint32_t base_delta = 0u;
        if (indices_fit && batch.indexed) {
            base_delta = batch.first_vertex - previous.first_vertex;
            begin = batch.first_index;
            end = begin + batch.index_count;
            indices_fit = end <= s.indices.size();
            for (std::size_t i = begin; indices_fit && i < end; ++i)
                indices_fit = s.indices[i] <= std::numeric_limits<std::uint32_t>::max() - base_delta;
        }
        if (indices_fit) {
            if (batch.indexed) {
                for (std::size_t i = begin; i < end; ++i) s.indices[i] += base_delta;
                previous.index_count += batch.index_count;
            }
            previous.vertex_count += batch.vertex_count;
            previous.logical_draw_count += batch.logical_draw_count;
            return true;
        }
    }
    s.batches.push_back(std::move(batch));
    return false;
}

bool compile_shaders(Dx12GeState &s, std::string &error) noexcept {
    const char *shader = R"HLSL(
Texture2D<float4> SourceTexture : register(t0);
SamplerState SourceSampler : register(s0);
cbuffer DrawTransform : register(b0) {
    float4 TransformRow0;
    float4 TransformRow1;
    float4 TransformRow2;
    float4 TransformRow3;
    float4 ModelToViewZ;
    float4 UvScaleOffset;
    float4 FogParameters;
    uint4 TransformControl;
    float4 VertexColorMul;
    float4 VertexColorAdd;
};
cbuffer DrawPixelState : register(b1) {
    uint AlphaControlPacked;
    uint TextureControlPacked;
    uint TextureEnvPacked;
    uint FogControlPacked;
    uint FramebufferFormat;
    uint BlendControl;
    uint BlendFixSource;
    uint BlendFixDest;
};
struct VSIn {
    float4 position : POSITION;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
    float q : TEXCOORD1;
    float fogFactor : FOG0;
};
// Chili: the GE interpolates colour and fog with screen-space weights; only
// the texture coordinates are perspective-correct.
struct VSOut {
    float4 position : SV_POSITION;
    noperspective float4 color : COLOR0;
    float2 uv : TEXCOORD0;
    float q : TEXCOORD1;
    noperspective float fogFactor : FOG0;
};
// Chili: at native resolution, put a transformed vertex on the GE's 1/16 pixel
// grid. FogParameters.zw is the number of grid steps per clip-space unit; the
// sample-point shift folded into the transform is exactly one step, and the
// GE rounds a projected coordinate up from 5/8 of a step.
float2 SnapToGrid(float2 clipXY, float clipW) {
    float2 result = clipXY;
    if (TransformControl.w != 0u && clipW > 0.0) {
        float2 steps = floor((clipXY / clipW + 1.0) * FogParameters.zw + 0.375);
        result = (steps / FogParameters.zw - 1.0) * clipW;
    }
    return result;
}
// Chili: register 0x1C is the GE's depth clamp, and the game keeps it set.
// Depth past the far plane is then held at the end of the range instead of
// being clipped (the horizon's hills sit there); which end that is follows the
// sign of the viewport's Z scale. The near side keeps D3D's clip. With the
// register clear both ends are clamped, as upstream did.
float ClampDepth(float clipZ, float clipW) {
    float result = clipZ;
    if (TransformControl.y == 0u) result = clamp(clipZ, 0.0, clipW);
    else if (clipW > 0.0) result = TransformControl.y == 1u ? max(clipZ, 0.0) : min(clipZ, clipW);
    return result;
}
VSOut VSMain(VSIn input) {
    VSOut o;
    if (TransformControl.x == 1u) {
        float4 p = input.position;
        float clipW = dot(TransformRow3, p);
        if (abs(clipW) < 1.0e-12) clipW = 1.0;
        float clipZ = dot(TransformRow2, p);
        clipZ = ClampDepth(clipZ, clipW);
        o.position = float4(dot(TransformRow0, p), dot(TransformRow1, p), clipZ, clipW);
        o.position.xy = SnapToGrid(o.position.xy, clipW);
        o.uv = input.uv * UvScaleOffset.xy + UvScaleOffset.zw;
        float viewZ = dot(ModelToViewZ, p);
        o.fogFactor = saturate((viewZ + FogParameters.x) * FogParameters.y);
    } else if (TransformControl.x == 2u) {
        float clipW = input.position.w;
        if (abs(clipW) < 1.0e-12) clipW = 1.0;
        float2 screen = input.position.xy;
        if (TransformControl.w != 0u) screen = floor(screen * 16.0) * (1.0 / 16.0);
        o.position = float4(
            ((screen.x + UvScaleOffset.w) * UvScaleOffset.x - 1.0) * clipW,
            (1.0 - (screen.y + UvScaleOffset.w) * UvScaleOffset.y) * clipW,
            saturate(input.position.z * UvScaleOffset.z) * clipW,
            clipW);
        o.uv = input.uv;
        o.fogFactor = input.fogFactor;
    } else {
        o.position = input.position;
        o.uv = input.uv;
        o.fogFactor = input.fogFactor;
    }
    o.color = input.color;
    if (TransformControl.z != 0u) {
        float4 lit = saturate(o.color * VertexColorMul + VertexColorAdd);
        o.color = floor(lit * 255.0) * (1.0 / 255.0);
    }
    o.q = input.q;
    return o;
}

// Stage 45.4: direct GPU decode for VCS's dominant 10-byte PSP world vertex
// (vtype 0x000115). The CPU only snapshots the original bytes; conversion to
// float UV / RGBA8 / normalized XYZ is performed by the vertex shader.
struct VSInPacked0115 {
    uint2 uv8 : TEXCOORD2;
    uint color5551 : COLOR1;
    int2 positionXY : POSITION1;
    int positionZ : POSITION2;
};
float Expand5ToFloat(uint value) {
    value &= 31u;
    uint expanded = (value << 3u) | (value >> 2u);
    return float(expanded) * (1.0 / 255.0);
}
VSOut VSMainPacked0115(VSInPacked0115 input) {
    VSOut o;
    float3 model = float3(float2(input.positionXY), float(input.positionZ)) * (1.0 / 32768.0);
    float4 p = float4(model, 1.0);
    float clipW = dot(TransformRow3, p);
    if (abs(clipW) < 1.0e-12) clipW = 1.0;
    float clipZ = dot(TransformRow2, p);
    clipZ = ClampDepth(clipZ, clipW);
    o.position = float4(dot(TransformRow0, p), dot(TransformRow1, p), clipZ, clipW);
    o.position.xy = SnapToGrid(o.position.xy, clipW);
    float2 rawUv = float2(input.uv8) * (1.0 / 128.0);
    o.uv = rawUv * UvScaleOffset.xy + UvScaleOffset.zw;
    float viewZ = dot(ModelToViewZ, p);
    o.fogFactor = saturate((viewZ + FogParameters.x) * FogParameters.y);
    uint packed = input.color5551;
    o.color = float4(
        Expand5ToFloat(packed),
        Expand5ToFloat(packed >> 5u),
        Expand5ToFloat(packed >> 10u),
        (packed & 0x8000u) != 0u ? 1.0 : 0.0);
    if (TransformControl.z != 0u) {
        // Match the CPU path's per-channel RGBA8 clamp/truncation boundary.
        float4 lit = saturate(o.color * VertexColorMul + VertexColorAdd);
        o.color = floor(lit * 255.0) * (1.0 / 255.0);
    }
    o.q = 1.0;
    return o;
}

bool AlphaPass(uint fn, uint lhs, uint rhs) {
    switch (fn & 7u) {
        case 0u: return false;
        case 1u: return true;
        case 2u: return lhs == rhs;
        case 3u: return lhs != rhs;
        case 4u: return lhs < rhs;
        case 5u: return lhs <= rhs;
        case 6u: return lhs > rhs;
        case 7u: return lhs >= rhs;
    }
    return true;
}
float4 ApplyTextureFunction(float4 vertex, float4 textureValue, uint4 control, uint4 envBytes) {
    uint fn = control.x & 7u;
    bool useAlpha = control.y != 0u;
    bool doubleColor = control.z != 0u;
    float4 outColor = vertex;
    float3 env = float3(envBytes.xyz) / 255.0;
    if (fn == 0u) { // MODULATE
        outColor.rgb = vertex.rgb * textureValue.rgb;
        outColor.a = useAlpha ? vertex.a * textureValue.a : vertex.a;
    } else if (fn == 1u) { // DECAL
        float a = useAlpha ? textureValue.a : 1.0;
        outColor.rgb = lerp(vertex.rgb, textureValue.rgb, a);
        outColor.a = vertex.a;
    } else if (fn == 2u) { // BLEND
        outColor.rgb = lerp(vertex.rgb, env, textureValue.rgb);
        outColor.a = useAlpha ? vertex.a * textureValue.a : vertex.a;
    } else if (fn == 3u) { // REPLACE
        outColor = textureValue;
        if (!useAlpha) outColor.a = vertex.a;
    } else if (fn == 4u) { // ADD
        outColor.rgb = saturate(vertex.rgb + textureValue.rgb);
        outColor.a = useAlpha ? vertex.a * textureValue.a : vertex.a;
    }
    if (doubleColor) outColor.rgb = saturate(outColor.rgb * 2.0);
    return outColor;
}
float Quantize(float value, float levels) {
    return floor(saturate(value) * levels + 0.5) / levels;
}
float4 QuantizeFramebuffer(float4 color, uint format) {
    color = saturate(color);
    if ((format & 3u) == 0u) { // PSP GU_PSM_5650
        color.r = Quantize(color.r, 31.0);
        color.g = Quantize(color.g, 63.0);
        color.b = Quantize(color.b, 31.0);
        color.a = 1.0;
    } else if ((format & 3u) == 1u) { // GU_PSM_5551
        color.rgb = float3(Quantize(color.r,31.0), Quantize(color.g,31.0), Quantize(color.b,31.0));
        color.a = color.a >= 0.5 ? 1.0 : 0.0;
    } else if ((format & 3u) == 2u) { // GU_PSM_4444
        color = float4(Quantize(color.r,15.0), Quantize(color.g,15.0),
                       Quantize(color.b,15.0), Quantize(color.a,15.0));
    }
    return color;
}
// PSP blend factors that depend only on the fragment. Mode numbers match
// Dx12ShaderFactor in the C++ above.
float3 BlendFactor(uint mode, float4 color, uint fixedColor) {
    mode &= 15u;
    float3 factor = float3(1.0, 1.0, 1.0);
    if (mode == 1u) factor = color.rgb;
    else if (mode == 2u) factor = 1.0 - color.rgb;
    else if (mode == 3u) factor = color.aaa;
    else if (mode == 4u) factor = 1.0 - color.aaa;
    else if (mode == 5u) factor = 2.0 * color.aaa;
    else if (mode == 6u) factor = 1.0 - saturate(2.0 * color.aaa);
    else if (mode == 7u) factor = float3(float(fixedColor & 0xFFu), float((fixedColor >> 8u) & 0xFFu),
                                         float((fixedColor >> 16u) & 0xFFu)) / 255.0;
    return factor;
}
// Chili: the GE scales a blend term in 8-bit integers and drops the fraction:
// ((2c + 1) * (2f + 1)) >> 10. The source term is known here, so it is cut the
// same way. Rounding it instead brightens every pass of an accumulating effect
// by half a step; the game's bloom adds 21 such passes.
float3 BlendSourceTerm(float3 color, float3 factor) {
    float3 c = floor(saturate(color) * 255.0 + 0.5);
    float3 f = floor(max(factor, 0.0) * 255.0 + 0.5);
    return floor((2.0 * c + 1.0) * (2.0 * f + 1.0) * (1.0 / 1024.0)) * (1.0 / 255.0);
}
struct PSOut {
    float4 color : SV_TARGET0;
    // Destination blend factor for dual-source blending; ignored by pipelines
    // that do not name SRC1_COLOR.
    float4 factor : SV_TARGET1;
};
#ifdef GLOW_CUBIC
// Chili: the texture is a glow sheet, the game's glow at the PSP's own size,
// and the draw enlarges it by four times the internal scale. Read with the
// game's bilinear filter every texel would show as a diamond of that size;
// a cubic B-spline (the sixteen texels around the point, taken as four
// bilinear taps) has no such edges and never leaves the texels' range.
float4 GlowTexel(float2 uv) {
    float2 size;
    SourceTexture.GetDimensions(size.x, size.y);
    float2 t = uv * size - 0.5;
    float2 i = floor(t);
    float2 f = t - i;
    float2 f2 = f * f;
    float2 f3 = f2 * f;
    float2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
    float2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    float2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
    float2 w3 = f3 / 6.0;
    float2 g0 = w0 + w1;
    float2 g1 = w2 + w3;
    float2 h0 = (i - 0.5 + w1 / g0) / size;
    float2 h1 = (i + 1.5 + w3 / g1) / size;
    return g0.y * (g0.x * SourceTexture.SampleLevel(SourceSampler, float2(h0.x, h0.y), 0.0) +
                   g1.x * SourceTexture.SampleLevel(SourceSampler, float2(h1.x, h0.y), 0.0)) +
           g1.y * (g0.x * SourceTexture.SampleLevel(SourceSampler, float2(h0.x, h1.y), 0.0) +
                   g1.x * SourceTexture.SampleLevel(SourceSampler, float2(h1.x, h1.y), 0.0));
}
#endif
PSOut PSMain(VSOut input) {
    const uint4 textureControl = uint4(TextureControlPacked & 0xFFu,
        (TextureControlPacked >> 8u) & 0xFFu, (TextureControlPacked >> 16u) & 0xFFu,
        (TextureControlPacked >> 24u) & 0xFFu);
    const uint4 textureEnv = uint4(TextureEnvPacked & 0xFFu,
        (TextureEnvPacked >> 8u) & 0xFFu, (TextureEnvPacked >> 16u) & 0xFFu, 0u);
    const uint4 fogControl = uint4(FogControlPacked & 0xFFu,
        (FogControlPacked >> 8u) & 0xFFu, (FogControlPacked >> 16u) & 0xFFu,
        (FogControlPacked >> 24u) & 0xFFu);
    const uint4 alphaControl = uint4(AlphaControlPacked & 0xFFu,
        (AlphaControlPacked >> 8u) & 0xFFu, (AlphaControlPacked >> 16u) & 0xFFu,
        (AlphaControlPacked >> 24u) & 0xFFu);
    float4 color = saturate(input.color);
    if (textureControl.w != 0u) {
        float q = abs(input.q) < 1.0e-20 ? 1.0 : input.q;
        float2 uv = input.uv / q;
        // Chili: the GE picks the mip level from the U step along X and the V
        // step along Y only, so a rotated or stretched quad stays sharper than
        // the full-footprint rule would make it. Anisotropic filtering (bit
        // 0x100) needs the real footprint.
        float2 gradX = ddx(uv);
        float2 gradY = ddy(uv);
        if ((FramebufferFormat & 0x100u) == 0u) {
            gradX = float2(gradX.x, 0.0);
            gradY = float2(0.0, gradY.y);
        }
#ifdef GLOW_CUBIC
        float4 texel = GlowTexel(uv);
#else
        float4 texel = SourceTexture.SampleGrad(SourceSampler, uv, gradX, gradY);
#endif
        color = ApplyTextureFunction(color, texel, textureControl, textureEnv);
    }
    if (fogControl.w != 0u) {
        float3 fog = float3(fogControl.xyz) / 255.0;
        color.rgb = lerp(fog, color.rgb, saturate(input.fogFactor));
    }
    if (alphaControl.x != 0u) {
        uint a = (uint)floor(saturate(color.a) * 255.0 + 0.5);
        uint mask = alphaControl.w;
        if (!AlphaPass(alphaControl.y, a & mask, alphaControl.z & mask)) discard;
    }
    color = QuantizeFramebuffer(color, FramebufferFormat);
    PSOut o;
    o.factor = float4(BlendFactor(BlendControl >> 4u, color, BlendFixDest), 1.0);
    if ((BlendControl & 15u) != 0u)
        o.color = float4(BlendSourceTerm(color.rgb, BlendFactor(BlendControl, color, BlendFixSource)), color.a);
    else
        o.color = color;
    return o;
}

// Chili: ge_gpu_backend_write_surface_depth16(). The 16-bit PSP depth value
// arrives split over the red (low byte) and green (high byte) channels.
float DepthWritePS(VSOut input) : SV_DEPTH {
    float2 packed = SourceTexture.Sample(SourceSampler, input.uv).rg;
    // Same arithmetic as the vertex shader's through-mode depth (z * 1/65535),
    // so an equal 16-bit value compares equal against a seeded buffer.
    return (round(packed.r * 255.0) + round(packed.g * 255.0) * 256.0) * (1.0 / 65535.0);
}
)HLSL";

    // Chili: written for this port. Upstream's present shaders lived inside its
    // CloudWorks shader source (CC BY-NC-SA), which is not carried over.
    // PresentUvScale selects the LCD rectangle of the displayed surface sheet.
    const char *present = R"HLSL(
Texture2D<float4> SourceTexture : register(t0);
SamplerState SourceSampler : register(s0);
cbuffer PresentState : register(b1) {
    float2 PresentUvScale;
    // Size of one window pixel in UV, the largest UV of the picture, and how
    // many samples per axis cover a window pixel.
    float2 PresentFootprint;
    float2 PresentUvMax;
    uint PresentTaps;
};
struct PresentVSOut {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};
PresentVSOut PresentVS(uint vertexId : SV_VertexID) {
    // One triangle covering the viewport: (0,0) (2,0) (0,2) in UV space.
    float2 corner = float2(float((vertexId << 1u) & 2u), float(vertexId & 2u));
    PresentVSOut o;
    o.position = float4(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0);
    o.uv = corner;
    return o;
}
// The scale is applied here because the root constants are pixel-visible only.
// Chili: one bilinear sample reaches two source pixels across. When the
// picture is more than twice the window's size, a grid of samples covers the
// whole window pixel, so a large internal resolution shown in a small window
// is averaged down instead of skipping pixels.
float4 PresentPS(PresentVSOut input) : SV_TARGET {
    float2 uv = input.uv * PresentUvScale;
    float3 sum = float3(0.0, 0.0, 0.0);
    for (uint y = 0u; y < PresentTaps; ++y) {
        for (uint x = 0u; x < PresentTaps; ++x) {
            float2 offset = (float2(float(x), float(y)) + 0.5) / float(PresentTaps) - 0.5;
            float2 at = uv + offset * PresentFootprint;
            if (PresentTaps > 1u) at = min(at, PresentUvMax);
            sum += SourceTexture.SampleLevel(SourceSampler, at, 0.0).rgb;
        }
    }
    return float4(sum / float(PresentTaps * PresentTaps), 1.0);
}
)HLSL";
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS;
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(shader, std::strlen(shader), "VCSNativeDX12GE", nullptr, nullptr,
                            "VSMain", "vs_5_1", flags, 0u, &s.vertex_shader, &errors);
    if (FAILED(hr)) {
        error = errors ? std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize())
                       : hr_text(hr, "D3DCompile(DX12 GE VS)");
        return false;
    }
    errors.Reset();
    hr = D3DCompile(shader, std::strlen(shader), "VCSNativeDX12GE", nullptr, nullptr,
                    "VSMainPacked0115", "vs_5_1", flags, 0u,
                    &s.packed_0115_vertex_shader, &errors);
    if (FAILED(hr)) {
        error = errors ? std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize())
                       : hr_text(hr, "D3DCompile(DX12 GE packed 0115 VS)");
        return false;
    }
    errors.Reset();
    hr = D3DCompile(shader, std::strlen(shader), "VCSNativeDX12GE", nullptr, nullptr,
                    "PSMain", "ps_5_1", flags, 0u, &s.pixel_shader, &errors);
    if (FAILED(hr)) {
        error = errors ? std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize())
                       : hr_text(hr, "D3DCompile(DX12 GE PS)");
        return false;
    }
    // Chili: the pixel shader once more with the cubic filter, when the glow is
    // drawn small and enlarged (ge_gpu_glow_dx12.hpp). Without it the game's
    // own glow is drawn, so a failure here is reported and nothing else.
    if (ge_gpu_config().glow != GeGpuConfig::Glow::Game && ge_gpu_config().internal_scale > 1u) {
        const D3D_SHADER_MACRO cubic[]{{"GLOW_CUBIC", "1"}, {nullptr, nullptr}};
        errors.Reset();
        hr = D3DCompile(shader, std::strlen(shader), "VCSNativeDX12GE", cubic, nullptr,
                        "PSMain", "ps_5_1", flags, 0u, &s.cubic_pixel_shader, &errors);
        if (FAILED(hr)) {
            runtime_log_error("dx12 glow shader", errors
                ? std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize())
                : hr_text(hr, "D3DCompile(DX12 GE PS, cubic)"));
            s.cubic_pixel_shader.Reset();
        }
    }
    errors.Reset();
    hr = D3DCompile(shader, std::strlen(shader), "VCSNativeDX12GE", nullptr, nullptr,
                    "DepthWritePS", "ps_5_1", flags, 0u, &s.depth_write_pixel_shader, &errors);
    if (FAILED(hr)) {
        error = errors ? std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize())
                       : hr_text(hr, "D3DCompile(DX12 GE depth write PS)");
        return false;
    }

    errors.Reset();
    hr = D3DCompile(present, std::strlen(present), "VCSNativeDX12GEPresent", nullptr, nullptr,
                    "PresentVS", "vs_5_1", flags, 0u, &s.present_vertex_shader, &errors);
    if (FAILED(hr)) {
        error = errors ? std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize())
                       : hr_text(hr, "D3DCompile(DX12 GE Present VS)");
        return false;
    }
    errors.Reset();
    hr = D3DCompile(present, std::strlen(present), "VCSNativeDX12GEPresent", nullptr, nullptr,
                    "PresentPS", "ps_5_1", flags, 0u, &s.present_pixel_shader, &errors);
    if (FAILED(hr)) {
        error = errors ? std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize())
                       : hr_text(hr, "D3DCompile(DX12 GE Present PS)");
        return false;
    }
    return true;
}

bool create_root_signature(Dx12GeState &s, std::string &error) noexcept {
    D3D12_DESCRIPTOR_RANGE srv_range{};
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = 1u;
    srv_range.BaseShaderRegister = 0u;
    srv_range.RegisterSpace = 0u;
    srv_range.OffsetInDescriptorsFromTableStart = 0u;
    D3D12_DESCRIPTOR_RANGE sampler_range{};
    sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    sampler_range.NumDescriptors = 1u;
    sampler_range.BaseShaderRegister = 0u;
    sampler_range.RegisterSpace = 0u;
    sampler_range.OffsetInDescriptorsFromTableStart = 0u;
    std::array<D3D12_ROOT_PARAMETER, 4> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 1u;
    parameters[0].DescriptorTable.pDescriptorRanges = &srv_range;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1u;
    parameters[1].DescriptorTable.pDescriptorRanges = &sampler_range;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // 40 DWORD root constants = four final clip rows + view-Z + UV + fog +
    // control + affine vertex-colour lighting. Still below the D3D12 root
    // signature 64-DWORD budget together with the descriptor tables/pixel state.
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[2].Constants.ShaderRegister = 0u;
    parameters[2].Constants.RegisterSpace = 0u;
    parameters[2].Constants.Num32BitValues = 40u;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[3].Constants.ShaderRegister = 1u;
    parameters[3].Constants.RegisterSpace = 0u;
    // Dx12PixelConstants; the present pass reuses the first two words for its
    // UV scale.
    parameters[3].Constants.Num32BitValues = kPixelConstantWords;
    parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = static_cast<UINT>(parameters.size());
    desc.pParameters = parameters.data();
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> blob, errors;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                              &blob, &errors);
    if (FAILED(hr)) {
        error = errors ? std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize())
                       : hr_text(hr, "D3D12SerializeRootSignature(DX12 GE)");
        return false;
    }
    hr = s.device->CreateRootSignature(0u, blob->GetBufferPointer(), blob->GetBufferSize(),
                                       IID_PPV_ARGS(&s.root_signature));
    if (FAILED(hr)) {
        error = hr_text(hr, "CreateRootSignature(DX12 GE)");
        return false;
    }
    return true;
}

bool create_targets(Dx12GeState &s, std::string &error) noexcept {
    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors = kFramebufferTargetCapacity;
    HRESULT hr = s.device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&s.rtv_heap));
    if (FAILED(hr)) { error = hr_text(hr, "CreateDescriptorHeap(DX12 GE RTV)"); return false; }
    s.rtv_stride = s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC dsv_desc{};
    dsv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsv_desc.NumDescriptors = kFramebufferTargetCapacity;
    hr = s.device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&s.dsv_heap));
    if (FAILED(hr)) { error = hr_text(hr, "CreateDescriptorHeap(DX12 GE DSV)"); return false; }
    s.dsv_stride = s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
    srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_desc.NumDescriptors = kSrvCapacity;
    srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = s.device->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&s.srv_heap));
    if (FAILED(hr)) { error = hr_text(hr, "CreateDescriptorHeap(DX12 GE SRV)"); return false; }
    s.srv_stride = s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC sampler_desc{};
    sampler_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    sampler_desc.NumDescriptors = kSamplerCapacity;
    sampler_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = s.device->CreateDescriptorHeap(&sampler_desc, IID_PPV_ARGS(&s.sampler_heap));
    if (FAILED(hr)) { error = hr_text(hr, "CreateDescriptorHeap(DX12 GE sampler)"); return false; }
    s.sampler_stride = s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

    // Descriptor zero is a valid black/null texture. Persistent framebuffer
    // targets and decoded PSP textures share the rest of the SRV heap.
    D3D12_SHADER_RESOURCE_VIEW_DESC null_srv{};
    null_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    null_srv.Format = kColorFormat;
    null_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    null_srv.Texture2D.MipLevels = 1u;
    s.device->CreateShaderResourceView(nullptr, &null_srv, srv_cpu(s, 0u));

    D3D12_SAMPLER_DESC default_sampler{};
    default_sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    default_sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    default_sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    default_sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    default_sampler.MinLOD = 0.0f;
    default_sampler.MaxLOD = D3D12_FLOAT32_MAX;
    default_sampler.MaxAnisotropy = 1u;
    default_sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    s.device->CreateSampler(&default_sampler, sampler_cpu(s, 0u));

    // Readback exists only for the validation probe / explicit diagnostics.
    // Normal gameplay goes render-target -> SRV -> swapchain entirely on GPU.
    if (s.readback_enabled) {
        D3D12_RESOURCE_DESC color{};
        color.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        color.Width = s.target_width;
        color.Height = s.target_height;
        color.DepthOrArraySize = 1u;
        color.MipLevels = 1u;
        color.Format = kColorFormat;
        color.SampleDesc.Count = 1u;
        color.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        color.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        s.device->GetCopyableFootprints(&color, 0u, 1u, 0u, &s.readback_footprint,
                                         &s.readback_rows, &s.readback_row_size, &s.readback_bytes);
        D3D12_RESOURCE_DESC readback{};
        readback.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        readback.Width = std::max<UINT64>(s.readback_bytes, 256u);
        readback.Height = 1u;
        readback.DepthOrArraySize = 1u;
        readback.MipLevels = 1u;
        readback.Format = DXGI_FORMAT_UNKNOWN;
        readback.SampleDesc.Count = 1u;
        readback.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES readback_heap{};
        readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
        hr = s.device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&s.readback_buffer));
        if (FAILED(hr)) { error = hr_text(hr, "CreateCommittedResource(DX12 GE readback)"); return false; }
        s.frame_rgba.resize(static_cast<std::size_t>(s.target_width) * s.target_height * 4u);
    } else {
        s.frame_rgba.clear();
        s.readback_bytes = 0u;
    }

    return true;
}

Dx12FramebufferTarget *find_framebuffer_target(Dx12GeState &s, std::uint32_t address) noexcept {
    const auto found = s.frame_targets.find(address & kVramOffsetMask);
    return found == s.frame_targets.end() ? nullptr : &found->second;
}

const Dx12FramebufferTarget *find_framebuffer_target(const Dx12GeState &s,
                                                      std::uint32_t address) noexcept {
    const auto found = s.frame_targets.find(address & kVramOffsetMask);
    return found == s.frame_targets.end() ? nullptr : &found->second;
}

// Chili: a texture address names a surface only when it lies in VRAM. Texture
// registers carry full guest addresses, and upstream's masked lookup also
// matched main-RAM textures whose low 21 bits happened to equal a surface
// offset (0x09288000 would have sampled the surface at 0x04088000).
bool is_vram_address(std::uint32_t address) noexcept {
    return (address & 0x0F800000u) == 0x04000000u;
}

Dx12FramebufferTarget *find_feedback_target(Dx12GeState &s, std::uint32_t texture_address) noexcept {
    return is_vram_address(texture_address) ? find_framebuffer_target(s, texture_address) : nullptr;
}

const Dx12FramebufferTarget *find_feedback_target(const Dx12GeState &s,
                                                   std::uint32_t texture_address) noexcept {
    return is_vram_address(texture_address) ? find_framebuffer_target(s, texture_address) : nullptr;
}

bool ensure_framebuffer_target(Dx12GeState &s, std::uint32_t address,
                               std::string &error) noexcept {
    address &= kVramOffsetMask;
    if (auto *existing = find_framebuffer_target(s, address)) return existing->color != nullptr;
    if (!s.device || !s.rtv_heap || !s.srv_heap ||
        s.next_rtv >= kFramebufferTargetCapacity || s.next_srv >= kSrvCapacity) {
        error = "DX12 framebuffer target/descriptor capacity exhausted";
        return false;
    }

    chili::CostTimer cost(chili::frame_costs.surface_ns);
    ++chili::frame_costs.surfaces;
    Dx12FramebufferTarget target{};
    target.address = address;
    target.rtv_index = s.next_rtv++;
    target.srv_index = s.next_srv++;
    target.logical_width = s.sheet_width;
    target.logical_height = s.sheet_height;
    target.physical_width = s.target_width;
    target.physical_height = s.target_height;

    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    // The shader-readable color image is always single-sampled so texture
    // feedback and swapchain presentation never need Texture2DMS shaders.
    D3D12_RESOURCE_DESC color{};
    color.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    color.Width = s.target_width;
    color.Height = s.target_height;
    color.DepthOrArraySize = 1u;
    color.MipLevels = 1u;
    color.Format = kColorFormat;
    color.SampleDesc.Count = 1u;
    color.SampleDesc.Quality = 0u;
    color.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    color.Flags = s.sample_count == 1u ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
                                       : D3D12_RESOURCE_FLAG_NONE;
    D3D12_CLEAR_VALUE color_clear{};
    color_clear.Format = kColorFormat;
    // Chili: VRAM starts out zeroed, alpha included.
    HRESULT hr = s.device->CreateCommittedResource(
        &default_heap, D3D12_HEAP_FLAG_NONE, &color,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        s.sample_count == 1u ? &color_clear : nullptr,
        IID_PPV_ARGS(&target.color));
    if (FAILED(hr)) { error = hr_text(hr, "CreateCommittedResource(DX12 framebuffer color)"); return false; }

    ID3D12Resource *rtv_resource = target.color.Get();
    if (s.sample_count > 1u) {
        D3D12_RESOURCE_DESC msaa = color;
        msaa.SampleDesc.Count = s.sample_count;
        msaa.SampleDesc.Quality = s.sample_quality;
        msaa.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        hr = s.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &msaa,
                                                D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                &color_clear, IID_PPV_ARGS(&target.msaa_color));
        if (FAILED(hr)) { error = hr_text(hr, "CreateCommittedResource(DX12 framebuffer MSAA color)"); return false; }
        target.msaa_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        rtv_resource = target.msaa_color.Get();
    }
    s.device->CreateRenderTargetView(rtv_resource, nullptr, rtv_cpu(s, target.rtv_index));

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = kColorFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1u;
    s.device->CreateShaderResourceView(target.color.Get(), &srv, srv_cpu(s, target.srv_index));

    target.color_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    target.resolve_dirty = {0, 0, static_cast<LONG>(s.target_width), static_cast<LONG>(s.target_height)};
    target.snapshot_dirty = target.resolve_dirty;
    s.frame_targets.emplace(address, std::move(target));
    s.known_frame_targets.insert(address);
    s.report.framebuffer_targets_observed = s.known_frame_targets.size();
    s.report.dx12_native_framebuffer_targets = s.frame_targets.size();
    {
        std::ostringstream log;
        const auto created = s.frame_targets.find(address);
        log << "dx12 framebuffer target created address=0x" << std::hex << address
            << std::dec << " size=" << s.target_width << 'x' << s.target_height
            << " msaa=" << s.sample_count << " depth=" << s.depth_bits
            << " srv=" << (created != s.frame_targets.end() ? created->second.srv_index : 0u);
        runtime_log_line(log.str());
    }
    return true;
}

Dx12DepthTarget *ensure_depth_target(Dx12GeState &s, std::uint32_t address,
                                     std::string &error) noexcept {
    address &= kVramOffsetMask;
    if (const auto found = s.depth_targets.find(address); found != s.depth_targets.end())
        return found->second.depth ? &found->second : nullptr;
    if (!s.device || !s.dsv_heap || s.next_dsv >= kFramebufferTargetCapacity) {
        error = "DX12 depth target/descriptor capacity exhausted";
        return nullptr;
    }
    chili::CostTimer cost(chili::frame_costs.surface_ns);
    ++chili::frame_costs.surfaces;
    Dx12DepthTarget target{};
    target.dsv_index = s.next_dsv++;
    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC depth{};
    depth.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depth.Width = s.target_width;
    depth.Height = s.target_height;
    depth.DepthOrArraySize = 1u;
    depth.MipLevels = 1u;
    depth.Format = s.depth_format;
    // Chili: a depth buffer that a shader reads (ambient occlusion) is created
    // without a fixed format and viewed as depth and as a texture.
    DXGI_FORMAT depth_srv_format = DXGI_FORMAT_UNKNOWN;
    if (ge_gpu_config().ambient_occlusion != GeGpuConfig::AmbientOcclusion::Off) {
        if (s.depth_format == DXGI_FORMAT_D32_FLOAT) {
            depth.Format = DXGI_FORMAT_R32_TYPELESS;
            depth_srv_format = DXGI_FORMAT_R32_FLOAT;
        } else if (s.depth_format == DXGI_FORMAT_D24_UNORM_S8_UINT) {
            depth.Format = DXGI_FORMAT_R24G8_TYPELESS;
            depth_srv_format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        } else if (s.depth_format == DXGI_FORMAT_D16_UNORM) {
            depth.Format = DXGI_FORMAT_R16_TYPELESS;
            depth_srv_format = DXGI_FORMAT_R16_UNORM;
        }
    }
    depth.SampleDesc.Count = s.sample_count;
    depth.SampleDesc.Quality = s.sample_quality;
    depth.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depth.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE depth_clear{};
    depth_clear.Format = s.depth_format;
    depth_clear.DepthStencil.Depth = 0.0f;
    depth_clear.DepthStencil.Stencil = 0u;
    const HRESULT hr = s.device->CreateCommittedResource(
        &default_heap, D3D12_HEAP_FLAG_NONE, &depth, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &depth_clear, IID_PPV_ARGS(&target.depth));
    if (FAILED(hr)) {
        error = hr_text(hr, "CreateCommittedResource(DX12 depth target)");
        // Remember the failure so the next draw does not retry the allocation.
        s.depth_targets.emplace(address, Dx12DepthTarget{});
        return nullptr;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = s.depth_format;
    dsv.ViewDimension = s.sample_count > 1u ? D3D12_DSV_DIMENSION_TEXTURE2DMS
                                            : D3D12_DSV_DIMENSION_TEXTURE2D;
    s.device->CreateDepthStencilView(target.depth.Get(), &dsv, dsv_cpu(s, target.dsv_index));
    if (depth_srv_format != DXGI_FORMAT_UNKNOWN && s.next_srv < kSrvCapacity) {
        target.srv_index = s.next_srv++;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = depth_srv_format;
        if (s.sample_count > 1u) {
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        } else {
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = 1u;
        }
        s.device->CreateShaderResourceView(target.depth.Get(), &srv, srv_cpu(s, target.srv_index));
    }
    {
        std::ostringstream log;
        log << "dx12 depth target created address=0x" << std::hex << address
            << std::dec << " size=" << s.target_width << 'x' << s.target_height
            << " msaa=" << s.sample_count << " depth=" << s.depth_bits;
        runtime_log_line(log.str());
    }
    return &s.depth_targets.emplace(address, std::move(target)).first->second;
}

bool ensure_feedback_copy(Dx12GeState &s, Dx12FramebufferTarget &target,
                          std::string &error) noexcept {
    if (target.feedback_copy) return true;
    if (s.next_srv >= kSrvCapacity) {
        error = "DX12 feedback SRV descriptor capacity exhausted";
        return false;
    }
    chili::CostTimer cost(chili::frame_costs.surface_ns);
    ++chili::frame_costs.surfaces;
    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC color{};
    color.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    color.Width = target.physical_width;
    color.Height = target.physical_height;
    color.DepthOrArraySize = 1u;
    color.MipLevels = 1u;
    color.Format = kColorFormat;
    color.SampleDesc.Count = 1u;
    color.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    color.Flags = D3D12_RESOURCE_FLAG_NONE;
    HRESULT hr = s.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &color,
                                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                    nullptr, IID_PPV_ARGS(&target.feedback_copy));
    if (FAILED(hr)) { error = hr_text(hr, "CreateCommittedResource(DX12 feedback snapshot)"); return false; }
    target.feedback_srv_index = s.next_srv++;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = kColorFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1u;
    s.device->CreateShaderResourceView(target.feedback_copy.Get(), &srv,
                                       srv_cpu(s, target.feedback_srv_index));
    target.feedback_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    target.snapshot_dirty = {0, 0, static_cast<LONG>(target.physical_width), static_cast<LONG>(target.physical_height)};
    return true;
}

// Chili: a pipeline state is built from its key alone (pipeline_key() plus the
// top bits pipeline_for() adds), so the keys of an earlier run can be
// created again at start-up (ge_gpu_backend_warm_up).
ComPtr<ID3D12PipelineState> create_pipeline(Dx12GeState &s, std::uint64_t key,
                                            std::string &error) noexcept {
    const bool single_sample = (key & kPipelineSingleSample) != 0u;
    const bool cubic = (key & kPipelineCubic) != 0u;
    if (cubic && !s.cubic_pixel_shader) {
        error = "the cubic pixel shader was not compiled";
        return {};
    }
    const bool packed_0115 = ((key >> 63u) & 1u) != 0u;
    const bool cull_enabled = ((key >> 62u) & 1u) != 0u;
    const bool accept_counter_clockwise = ((key >> 61u) & 1u) != 0u;
    const bool depth_test_enabled = (key & 1u) != 0u;
    const bool depth_write_enabled = ((key >> 1u) & 1u) != 0u;
    const std::uint32_t depth_function = static_cast<std::uint32_t>((key >> 2u) & 7u);
    const bool clear_depth = ((key >> 5u) & 1u) != 0u;
    const GeGpuSurfaceWrite surface_write = static_cast<GeGpuSurfaceWrite>((key >> 6u) & 3u);
    static const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0u, DXGI_FORMAT_R32G32B32A32_FLOAT, 0u,
         static_cast<UINT>(offsetof(Dx12UploadVertex, x)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0u},
        {"COLOR", 0u, DXGI_FORMAT_R8G8B8A8_UNORM, 0u,
         static_cast<UINT>(offsetof(Dx12UploadVertex, rgba)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0u},
        {"TEXCOORD", 0u, DXGI_FORMAT_R32G32_FLOAT, 0u,
         static_cast<UINT>(offsetof(Dx12UploadVertex, u)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0u},
        {"TEXCOORD", 1u, DXGI_FORMAT_R32_FLOAT, 0u,
         static_cast<UINT>(offsetof(Dx12UploadVertex, q)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0u},
        {"FOG", 0u, DXGI_FORMAT_R32_FLOAT, 0u,
         static_cast<UINT>(offsetof(Dx12UploadVertex, fog_factor)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0u},
    };
    static const D3D12_INPUT_ELEMENT_DESC packed_layout[] = {
        {"TEXCOORD", 2u, DXGI_FORMAT_R8G8_UINT, 0u, 0u,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0u},
        {"COLOR", 1u, DXGI_FORMAT_R16_UINT, 0u, 2u,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0u},
        {"POSITION", 1u, DXGI_FORMAT_R16G16_SINT, 0u, 4u,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0u},
        {"POSITION", 2u, DXGI_FORMAT_R16_SINT, 0u, 8u,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0u},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = s.root_signature.Get();
    ID3DBlob *vs = packed_0115 ? s.packed_0115_vertex_shader.Get() : s.vertex_shader.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    ID3DBlob *ps = surface_write == GeGpuSurfaceWrite::Depth ? s.depth_write_pixel_shader.Get()
        : cubic ? s.cubic_pixel_shader.Get() : s.pixel_shader.Get();
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.InputLayout = packed_0115
        ? D3D12_INPUT_LAYOUT_DESC{packed_layout, static_cast<UINT>(std::size(packed_layout))}
        : D3D12_INPUT_LAYOUT_DESC{layout, static_cast<UINT>(std::size(layout))};
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = cull_enabled ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    // The transform's Y row is negated to convert the PSP framebuffer origin to
    // D3D's (see the row1 construction in the constant setup).  Mirroring an axis
    // reverses the screen-space winding of every triangle, so the guest's
    // front-face bit from GE command 0x9B must be inverted here.  The software
    // rasterizer classifies winding on the CPU, before that matrix, which is why
    // only the hardware path was affected: it culled the visible faces and kept
    // the hidden ones, dropping parts of models.
    pso.RasterizerState.FrontCounterClockwise = accept_counter_clockwise ? FALSE : TRUE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.BlendState.AlphaToCoverageEnable = FALSE;
    pso.BlendState.IndependentBlendEnable = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC blend{};
    blend.RenderTargetWriteMask = static_cast<UINT8>((key >> 8u) & 0xFu);
    blend.BlendEnable = ((key >> 12u) & 1u) != 0u ? TRUE : FALSE;
    blend.SrcBlend = static_cast<D3D12_BLEND>((key >> 13u) & 0x1Fu);
    blend.DestBlend = static_cast<D3D12_BLEND>((key >> 18u) & 0x1Fu);
    blend.BlendOp = static_cast<D3D12_BLEND_OP>((key >> 23u) & 7u);
    // Alpha is only written by clear mode and surface uploads, which never
    // blend, or with the (unimplemented) stencil test on. Keep the alpha half
    // of the equation valid and neutral.
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0] = blend;
    if (surface_write == GeGpuSurfaceWrite::Depth) {
        pso.DepthStencilState.DepthEnable = TRUE;
        pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    } else if (surface_write == GeGpuSurfaceWrite::Color) {
        pso.DepthStencilState.DepthEnable = FALSE;
        pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    } else {
        // Chili: D3D writes depth only while the depth test is enabled. A
        // clear-mode draw has the test off and still has to write depth, so
        // it runs with ALWAYS. Ordinary draws with the test off write nothing,
        // on the PSP as well.
        pso.DepthStencilState.DepthEnable = (depth_test_enabled || clear_depth) ? TRUE : FALSE;
        pso.DepthStencilState.DepthWriteMask =
            depth_write_enabled && (depth_test_enabled || clear_depth)
                ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        pso.DepthStencilState.DepthFunc = depth_test_enabled
            ? depth_compare(depth_function) : D3D12_COMPARISON_FUNC_ALWAYS;
    }
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1u;
    pso.RTVFormats[0] = kColorFormat;
    pso.DSVFormat = s.depth_format;
    pso.SampleDesc.Count = single_sample ? 1u : s.sample_count;
    pso.SampleDesc.Quality = single_sample ? 0u : s.sample_quality;
    ComPtr<ID3D12PipelineState> pipeline;
    const HRESULT hr = s.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline));
    if (FAILED(hr)) {
        error = hr_text(hr, "CreateGraphicsPipelineState(DX12 GE)");
        return {};
    }
    return pipeline;
}

ID3D12PipelineState *pipeline_for(Dx12GeState &s, const GeGpuDrawDescriptor &draw,
                                  bool packed_0115, bool cull_enabled,
                                  bool accept_counter_clockwise, std::uint64_t variant,
                                  std::string &error) noexcept {
    const std::uint64_t key = pipeline_key(draw) | variant |
        (packed_0115 ? (std::uint64_t{1} << 63u) : 0u) |
        (cull_enabled ? (std::uint64_t{1} << 62u) : 0u) |
        (accept_counter_clockwise ? (std::uint64_t{1} << 61u) : 0u);
    const auto found = s.pipelines.find(key);
    if (found != s.pipelines.end()) return found->second.Get();
    chili::CostTimer cost(chili::frame_costs.pipeline_ns);
    ++chili::frame_costs.pipelines;
    ComPtr<ID3D12PipelineState> pipeline = create_pipeline(s, key, error);
    if (!pipeline) return nullptr;
    ID3D12PipelineState *raw = pipeline.Get();
    s.pipelines.emplace(key, std::move(pipeline));
    s.report.unique_pipeline_keys = s.pipelines.size();
    s.report.graphics_pipeline_created = true;
    return raw;
}

// Chili: `scene` selects the sampler of a 3D draw, which the settings may
// force; everything else is filtered exactly as the game asks.
D3D12_FILTER texture_filter(const GeGpuDrawDescriptor &draw, bool scene) noexcept {
    bool min_linear = draw.texture_min_linear;
    bool mag_linear = draw.texture_mag_linear;
    if (scene) {
        const GeGpuConfig &config = ge_gpu_config();
        if (config.texture_filter == GeGpuConfig::TextureFilter::Linear) min_linear = mag_linear = true;
        else if (config.texture_filter == GeGpuConfig::TextureFilter::Nearest) min_linear = mag_linear = false;
        if (config.anisotropic_filtering > 1u && min_linear && mag_linear) return D3D12_FILTER_ANISOTROPIC;
    }
    const bool mip_linear = draw.texture_mipmap_enabled && draw.texture_mipmap_linear;
    if (!mip_linear) {
        if (min_linear && mag_linear) return D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        if (min_linear) return D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT;
        if (mag_linear) return D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
        return D3D12_FILTER_MIN_MAG_MIP_POINT;
    }
    if (min_linear && mag_linear) return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    if (min_linear) return D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
    if (mag_linear) return D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR;
    return D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;
}

// Chili: `level_shift` is log2(replacement size / original size) of a replaced
// texture, negative for "not replaced". A replaced texture has a full chain
// whatever the game's had, so it is sampled across levels smoothly and its
// fixed level moves with the size. `exact_levels` (GeTextureReplacement): the
// levels are the game's picture at exact sizes, sampled without the game's
// level bias. `mod_scene`: a mod's picture (GeTextureReplacement::mod_picture)
// in a 3D draw, sampled from the levels with about one texel per screen
// pixel; in a sprite it keeps the sampler of a replaced texture
// (ge_sampler_rule.hpp).
std::uint64_t sampler_key(const GeGpuDrawDescriptor &draw, float level_shift, bool scene, bool exact_levels,
                          bool mod_scene) noexcept {
    std::uint64_t key = static_cast<std::uint64_t>(texture_filter(draw, scene));
    key = hash_mix(key, (scene ? 1u : 0u) | (exact_levels ? 2u : 0u) | (mod_scene ? 4u : 0u));
    key = hash_mix(key, std::bit_cast<std::uint32_t>(level_shift));
    key = hash_mix(key, draw.texture_clamp_u ? 1u : 0u);
    key = hash_mix(key, draw.texture_clamp_v ? 1u : 0u);
    key = hash_mix(key, draw.texture_level_mode);
    key = hash_mix(key, static_cast<std::uint32_t>(draw.texture_level_offset16));
    key = hash_mix(key, draw.texture_selected_level);
    key = hash_mix(key, draw.texture_max_level);
    return key;
}

std::uint32_t ensure_sampler(Dx12GeState &s, const GeGpuDrawDescriptor &draw,
                             float level_shift = -1.0f, bool scene = false, bool exact_levels = false,
                             bool mod_picture = false) noexcept {
    // The sampler of 3D draws differs from the sprites' by the settings, and for a mod's picture always.
    const bool scene_draws = scene;
    scene = scene && (ge_gpu_config().anisotropic_filtering > 1u ||
                      ge_gpu_config().texture_filter != GeGpuConfig::TextureFilter::Game);
    const std::uint64_t key = sampler_key(draw, level_shift, scene, exact_levels, mod_picture && scene_draws);
    if (const auto found = s.sampler_cache.find(key); found != s.sampler_cache.end())
        return found->second;
    if (s.next_sampler >= kSamplerCapacity) return 0u;
    const std::uint32_t index = s.next_sampler++;
    D3D12_SAMPLER_DESC sampler{};
    const GeSamplerRule rule = ge_sampler_rule(texture_filter(draw, scene),
                                               static_cast<float>(draw.texture_level_offset16) / 16.0f,
                                               level_shift >= 0.0f, exact_levels, mod_picture, scene_draws,
                                               ge_gpu_config().anisotropic_keep_bias);
    sampler.Filter = rule.filter;
    sampler.AddressU = draw.texture_clamp_u ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = draw.texture_clamp_v ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MipLODBias = rule.bias;
    sampler.MaxAnisotropy = 1u;
    if (sampler.Filter == D3D12_FILTER_ANISOTROPIC)
        sampler.MaxAnisotropy = std::clamp(ge_gpu_config().anisotropic_filtering, 1u, 16u);
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MinLOD = 0.0f;
    // The image's own level count bounds the level.
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    if (draw.texture_level_mode == 1u) {
        const float level = static_cast<float>(draw.texture_selected_level) + std::max(0.0f, level_shift);
        sampler.MinLOD = level;
        sampler.MaxLOD = level;
    }
    s.device->CreateSampler(&sampler, sampler_cpu(s, index));
    s.sampler_cache.emplace(key, index);
    s.report.texture_samplers_created = s.sampler_cache.size() + 1u;
    return index;
}

bool create_present_pipeline(Dx12GeState &s, std::string &error) noexcept {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = s.root_signature.Get();
    pso.VS = {s.present_vertex_shader->GetBufferPointer(), s.present_vertex_shader->GetBufferSize()};
    pso.PS = {s.present_pixel_shader->GetBufferPointer(), s.present_pixel_shader->GetBufferSize()};
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1u;
    pso.RTVFormats[0] = kColorFormat;
    pso.SampleDesc.Count = 1u;
    const HRESULT hr = s.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&s.present_pipeline));
    if (FAILED(hr)) {
        error = hr_text(hr, "CreateGraphicsPipelineState(DX12 GE present)");
        return false;
    }
    return true;
}

void release_swapchain_buffers(Dx12GeState &s) noexcept {
    for (auto &buffer : s.backbuffers) buffer.Reset();
}

bool create_swapchain_buffers(Dx12GeState &s, std::string &error) noexcept {
    for (UINT i = 0u; i < kFrameCount; ++i) {
        HRESULT hr = s.swapchain->GetBuffer(i, IID_PPV_ARGS(&s.backbuffers[i]));
        if (FAILED(hr)) { error = hr_text(hr, "IDXGISwapChain::GetBuffer(DX12 GE)"); return false; }
        s.device->CreateRenderTargetView(s.backbuffers[i].Get(), nullptr, swap_rtv(s, i));
    }
    return true;
}

bool ensure_swapchain(Dx12GeState &s, std::string &error) noexcept {
    if (s.native_window == nullptr) {
        error = "DX12 GE direct present has no active display window";
        return false;
    }
    RECT client{};
    if (!GetClientRect(s.native_window, &client)) {
        error = "GetClientRect failed for DX12 GE direct present";
        return false;
    }
    const std::uint32_t surface_width = static_cast<std::uint32_t>(std::max<LONG>(1, client.right - client.left));
    const std::uint32_t surface_height = static_cast<std::uint32_t>(std::max<LONG>(1, client.bottom - client.top));
    if (s.swapchain && s.swap_width == surface_width && s.swap_height == surface_height)
        return true;
    if (s.swapchain) {
        if (!wait_for_gpu(s, error)) return false;
        release_swapchain_buffers(s);
        const UINT resize_flags = s.swapchain_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;
        const HRESULT hr = s.swapchain->ResizeBuffers(kFrameCount, surface_width, surface_height,
                                                       kColorFormat, resize_flags);
        if (FAILED(hr)) { error = hr_text(hr, "IDXGISwapChain::ResizeBuffers(DX12 GE)"); return false; }
        s.swap_width = surface_width;
        s.swap_height = surface_height;
        return create_swapchain_buffers(s, error);
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap.NumDescriptors = kFrameCount;
    HRESULT hr = s.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&s.swap_rtv_heap));
    if (FAILED(hr)) { error = hr_text(hr, "CreateDescriptorHeap(DX12 GE swap RTV)"); return false; }
    s.swap_rtv_stride = s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = surface_width;
    desc.Height = surface_height;
    desc.Format = kColorFormat;
    desc.SampleDesc.Count = 1u;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = kFrameCount;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    ComPtr<IDXGISwapChain1> swap1;
    hr = s.factory->CreateSwapChainForHwnd(s.queue.Get(), s.native_window,
                                            &desc, nullptr, nullptr, &swap1);
    s.swapchain_tearing = SUCCEEDED(hr);
    if (FAILED(hr)) {
        // A driver may reject tearing on a particular setup; retry without it.
        desc.Flags = 0u;
        hr = s.factory->CreateSwapChainForHwnd(s.queue.Get(), s.native_window,
                                                &desc, nullptr, nullptr, &swap1);
        s.swapchain_tearing = false;
    }
    if (FAILED(hr)) { error = hr_text(hr, "CreateSwapChainForHwnd(DX12 GE)"); return false; }
    (void)s.factory->MakeWindowAssociation(s.native_window, DXGI_MWA_NO_ALT_ENTER);
    hr = swap1.As(&s.swapchain);
    if (FAILED(hr)) { error = hr_text(hr, "Query IDXGISwapChain3(DX12 GE)"); return false; }
    s.swap_width = surface_width;
    s.swap_height = surface_height;
    if (!create_swapchain_buffers(s, error)) return false;
    s.report.swapchain_active = true;
    runtime_log_line("dx12 ge direct swapchain created " + std::to_string(surface_width) + "x" +
                     std::to_string(surface_height));
    return true;
}

std::uint32_t present_sampler(Dx12GeState &s) noexcept {
    GeGpuDrawDescriptor draw{};
    const bool linear = ge_gpu_config().upscale_bilinear;
    draw.texture_min_linear = linear;
    draw.texture_mag_linear = linear;
    draw.texture_clamp_u = true;
    draw.texture_clamp_v = true;
    return ensure_sampler(s, draw);
}

#include "ge_gpu_overlay_dx12.hpp"

bool record_direct_present(Dx12GeState &s, Dx12FramebufferTarget &source,
                           std::string &error) noexcept {
    if (!ensure_swapchain(s, error)) return false;
    const UINT index = s.swapchain->GetCurrentBackBufferIndex();
    ID3D12Resource *backbuffer = s.backbuffers[index].Get();
    resolve_target_for_sampling(s, source, false);
    transition(s.list.Get(), backbuffer, D3D12_RESOURCE_STATE_PRESENT,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = swap_rtv(s, index);
    s.list->OMSetRenderTargets(1u, &rtv, FALSE, nullptr);
    constexpr float black[4]{0.0f, 0.0f, 0.0f, 1.0f};
    s.list->ClearRenderTargetView(rtv, black, 0u, nullptr);
    // Where the LCD image lands in the window: letterboxed to the PSP aspect, or
    // stretched over the whole client area (host/picture_rect.hpp, shared with
    // the overlay pass and the mouse).
    const chili::PictureRect rect = chili::picture_rect(
        s.swap_width, s.swap_height, ge_gpu_config().preserve_aspect, ge_gpu_config().integer_scale,
        kReferenceWidth, kReferenceHeight);
    D3D12_VIEWPORT viewport{static_cast<float>(rect.x), static_cast<float>(rect.y),
                            static_cast<float>(std::max(1, rect.width)),
                            static_cast<float>(std::max(1, rect.height)), 0.0f, 1.0f};
    D3D12_RECT scissor{rect.x, rect.y, rect.x + std::max(1, rect.width),
                       rect.y + std::max(1, rect.height)};
    s.list->RSSetViewports(1u, &viewport);
    s.list->RSSetScissorRects(1u, &scissor);
    s.list->SetPipelineState(s.present_pipeline.Get());
    s.list->SetGraphicsRootSignature(s.root_signature.Get());
    ID3D12DescriptorHeap *heaps[]{s.srv_heap.Get(), s.sampler_heap.Get()};
    s.list->SetDescriptorHeaps(2u, heaps);
    s.list->SetGraphicsRootDescriptorTable(0u, srv_gpu(s, source.srv_index));
    s.list->SetGraphicsRootDescriptorTable(1u, sampler_gpu(s, present_sampler(s)));
    // The LCD is the top-left 480x272 of the displayed surface sheet.
    const std::array<float, 2> uv_scale{
        static_cast<float>(kReferenceWidth) / static_cast<float>(std::max<std::uint32_t>(1u, source.logical_width)),
        static_cast<float>(kReferenceHeight) / static_cast<float>(std::max<std::uint32_t>(1u, source.logical_height))};
    std::array<std::uint32_t, kPixelConstantWords> present_constants{};
    std::memcpy(present_constants.data(), uv_scale.data(), sizeof(uv_scale));
    // Source pixels per window pixel decide how many samples the shader takes.
    const float picture_width = static_cast<float>(kReferenceWidth) * static_cast<float>(s.target_width) /
                                static_cast<float>(std::max<std::uint32_t>(1u, s.sheet_width));
    const float ratio = picture_width / static_cast<float>(std::max(1, rect.width));
    const std::uint32_t taps = ge_gpu_config().upscale_bilinear
        ? static_cast<std::uint32_t>(std::clamp(std::ceil(ratio * 0.5f - 0.01f), 1.0f, 4.0f)) : 1u;
    const std::array<float, 4> footprint{
        uv_scale[0] / static_cast<float>(std::max(1, rect.width)),
        uv_scale[1] / static_cast<float>(std::max(1, rect.height)),
        uv_scale[0] - 0.5f / static_cast<float>(s.target_width),
        uv_scale[1] - 0.5f / static_cast<float>(s.target_height)};
    std::memcpy(present_constants.data() + 2u, footprint.data(), sizeof(footprint));
    present_constants[6] = taps;
    s.list->SetGraphicsRoot32BitConstants(
        3u, static_cast<UINT>(present_constants.size()), present_constants.data(), 0u);
    s.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s.list->DrawInstanced(3u, 1u, 0u, 0u);
    // Chili: a host page over the picture, and the window's picture read back
    // when a test asked for it (ge_gpu_overlay_dx12.hpp). Neither: nothing.
    D3D12_RESOURCE_STATES backbuffer_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    record_overlay(s, backbuffer, rect, backbuffer_state);
    transition(s.list.Get(), backbuffer, backbuffer_state, D3D12_RESOURCE_STATE_PRESENT);
    // Keep persistent framebuffer targets shader-readable between display
    // intervals. The next render pass transitions only the target it writes.
    return true;
}

Dx12Texture *find_cached_texture(Dx12GeState &s, std::uint64_t key) noexcept {
    if (s.last_texture_lookup != nullptr && s.last_texture_lookup_key == key)
        return s.last_texture_lookup;
    const auto found = s.textures.find(key);
    if (found == s.textures.end()) {
        s.last_texture_lookup = nullptr;
        s.last_texture_lookup_key = key;
        return nullptr;
    }
    s.last_texture_lookup_key = key;
    s.last_texture_lookup = &found->second;
    return s.last_texture_lookup;
}

void clear_texture_lookup_cache(Dx12GeState &s) noexcept {
    s.last_texture_lookup = nullptr;
    s.last_texture_lookup_key = 0u;
}

std::uint64_t texture_image_bytes(std::uint32_t width, std::uint32_t height,
                                  std::uint32_t mip_levels) noexcept {
    std::uint64_t bytes = 0u;
    for (std::uint32_t level = 0u; level < mip_levels; ++level) {
        bytes += static_cast<std::uint64_t>(width) * height * 4ull;
        width = std::max(1u, width >> 1u);
        height = std::max(1u, height >> 1u);
    }
    return bytes;
}

float replacement_level_shift(const GeTextureReplacement &picture, std::uint32_t base_width) noexcept {
    return std::max(0.0f, std::log2(static_cast<float>(picture.width) /
                                    static_cast<float>(std::max<std::uint32_t>(1u, base_width))));
}

bool create_texture_image(Dx12GeState &s, Dx12Texture &texture) noexcept {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = texture.width;
    desc.Height = texture.height;
    desc.DepthOrArraySize = 1u;
    desc.MipLevels = static_cast<UINT16>(texture.mip_levels);
    desc.Format = kColorFormat;
    desc.SampleDesc.Count = 1u;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // In a shared heap when it fits (a mod's large picture gets a resource of its own).
    const HRESULT hr = place_texture_image(s, desc, texture);
    if (FAILED(hr)) {
        runtime_log_error("dx12 texture create", hr_text(hr, "CreateCommittedResource(texture)"));
        return false;
    }
    texture.srv_index = allocate_texture_srv(s);
    if (texture.srv_index == 0u) {
        ++s.report.rejected_texture_decodes;
        runtime_log_error("dx12 texture", "SRV descriptor heap exhausted");
        texture.image.Reset();
        retire_texture_block(s, texture.block);
        texture.block = {};
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = kColorFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = texture.mip_levels;
    s.device->CreateShaderResourceView(texture.image.Get(), &srv, srv_cpu(s, texture.srv_index));
    texture.gpu_bytes = texture_image_bytes(texture.width, texture.height, texture.mip_levels);
    texture.upload_pending = true;
    return true;
}

// Chili: puts a mod's picture in place of a cached texture's image. The old
// image stays alive until the frames that may still sample it are done.
void swap_texture_image(Dx12GeState &s, std::uint64_t cache_key, Dx12Texture &texture,
                        const GeTextureReplacementPtr &picture) noexcept {
    const std::uint32_t base_width = texture.width;
    Dx12Texture replaced = texture;
    replaced.image.Reset();
    replaced.block = {};
    replaced.width = picture->width;
    replaced.height = picture->height;
    replaced.mip_levels = std::min(picture->mip_levels, kMaxTextureLevels);
    if (!create_texture_image(s, replaced)) return;
    for (Dx12FrameResources &retire : s.frames)
        retire.transient_resources.push_back(texture.image);
    retire_texture_srv(s, texture.srv_index);
    retire_texture_block(s, texture.block);
    s.texture_cache_bytes -= std::min<std::uint64_t>(s.texture_cache_bytes, texture.gpu_bytes);
    const bool was_pending = texture.upload_pending;
    replaced.rgba8.clear();
    replaced.rgba8.shrink_to_fit();
    replaced.replacement = picture;
    replaced.replaced = true;
    replaced.level_shift = replacement_level_shift(*picture, base_width);
    replaced.sampler_index = ensure_sampler(s, replaced.descriptor, replaced.level_shift, false,
                                            picture->exact_levels, picture->mod_picture);
    replaced.scene_sampler_index = ensure_sampler(s, replaced.descriptor, replaced.level_shift, true,
                                                  picture->exact_levels, picture->mod_picture);
    texture = std::move(replaced);
    s.texture_cache_bytes += texture.gpu_bytes;
    if (!was_pending) s.pending_texture_keys.push_back(cache_key);
}

// Chili: takes a texture out of the cache. The frames that may still sample
// its image keep it alive. The next draw with it decodes and uploads again.
void evict_texture(Dx12GeState &s, std::unordered_map<std::uint64_t, Dx12Texture>::iterator victim) noexcept {
    for (Dx12FrameResources &retire : s.frames)
        retire.transient_resources.push_back(victim->second.image);
    retire_texture_srv(s, victim->second.srv_index);
    retire_texture_block(s, victim->second.block);
    s.texture_cache_bytes -= std::min<std::uint64_t>(s.texture_cache_bytes, victim->second.gpu_bytes);
    clear_texture_lookup_cache(s);
    overlay_note_texture(victim->second.content_key, -1);
    s.textures.erase(victim);
}

// Chili: the overlays of some textures changed (the button prompts took
// another style): the cached textures with those contents leave the cache, the
// way the cache limit takes one out, and the next draw uploads them with the
// new pictures. Only between two submissions, when no accumulated draw refers
// to a texture: a draw looks its texture up when the submission is recorded.
void evict_changed_overlays(Dx12GeState &s) noexcept {
    (void)ge_texture_mods_take_overlay_changes(s.overlay_changes);
    if (s.overlay_changes.empty() || !s.batches.empty()) return;
    std::uint64_t evicted = 0u;
    for (auto it = s.textures.begin(); it != s.textures.end();) {
        const auto texture = it++;
        if (texture->second.content_key == 0u ||
            std::find(s.overlay_changes.begin(), s.overlay_changes.end(), texture->second.content_key) ==
                s.overlay_changes.end()) continue;
        evict_texture(s, texture);
        ++evicted;
    }
    s.overlay_changes.clear();
    if (evicted != 0u) ge_texture_mods_note_overlay_evictions(evicted);
}

// Chili: pictures the decoder finished since the last submission replace the
// images of every cached texture with that content, within the upload budget.
void apply_ready_replacements(Dx12GeState &s) noexcept {
    ge_texture_mods_take_ready(s.ready_replacements);
    if (s.ready_replacements.empty()) return;
    std::uint64_t uploaded = 0u;
    std::size_t done = 0u;
    for (; done < s.ready_replacements.size(); ++done) {
        const GeTextureReplacementReady &ready = s.ready_replacements[done];
        if (!ready.picture) continue;
        // A texture with an overlay was uploaded with its cells over the game's
        // picture while the mod's was being decoded: it is uploaded again, over
        // the mod's picture.
        if (ge_texture_mods_has_overlay(ready.key)) {
            s.overlay_changes.push_back(ready.key);
            continue;
        }
        if (uploaded != 0u && uploaded + ready.picture->rgba8.size() > kReplacementUploadBudget) break;
        for (auto &[cache_key, texture] : s.textures) {
            if (texture.content_key != ready.key || texture.replaced || !texture.image) continue;
            swap_texture_image(s, cache_key, texture, ready.picture);
            uploaded += ready.picture->rgba8.size();
        }
    }
    s.ready_replacements.erase(s.ready_replacements.begin(),
                               s.ready_replacements.begin() + static_cast<std::ptrdiff_t>(done));
}

bool prepare_texture_upload(Dx12GeState &s, const GeGpuDrawDescriptor &draw,
                            std::uint32_t base_width, std::uint32_t base_height,
                            std::uint32_t mip_levels, std::vector<std::byte> packed,
                            std::uint64_t content_key = 0u) noexcept {
    if (!s.enabled || !draw.texture_enabled || base_width == 0u || base_height == 0u ||
        mip_levels == 0u || mip_levels > 8u) return false;
    std::size_t expected = 0u;
    std::uint32_t w = base_width, h = base_height;
    for (std::uint32_t level = 0u; level < mip_levels; ++level) {
        const std::uint64_t bytes = static_cast<std::uint64_t>(w) * h * 4ull;
        if (bytes > std::numeric_limits<std::size_t>::max() - expected) return false;
        expected += static_cast<std::size_t>(bytes);
        w = std::max(1u, w >> 1u);
        h = std::max(1u, h >> 1u);
    }
    if (packed.size() != expected) return false;
    const std::uint64_t key = texture_key(draw);
    const std::uint64_t checksum = fnv1a64(packed);
    if (auto found = s.textures.find(key); found != s.textures.end()) {
        found->second.signature_epoch = s.frame_epoch;
        if (found->second.checksum == checksum) {
            found->second.descriptor = draw;
            ++s.report.texture_cache_hits;
            return true;
        }
        // Do not overwrite a descriptor/resource that an in-flight frame can still reference.
        // Keep one ComPtr in *every* frame slot. Each slot drops its reference only
        // after its own fence completes, so replacing a palette/streamed texture can
        // never free the old D3D12 resource while either of the two submitted frames
        // is still sampling it.
        for (Dx12FrameResources &retire : s.frames)
            retire.transient_resources.push_back(found->second.image);
        retire_texture_srv(s, found->second.srv_index);
        retire_texture_block(s, found->second.block);
        s.texture_cache_bytes -= std::min<std::uint64_t>(s.texture_cache_bytes, found->second.gpu_bytes);
        clear_texture_lookup_cache(s);
        overlay_note_texture(found->second.content_key, -1);
        s.textures.erase(found);
    }
    // Chili: dump the game's picture once, and look for a mod's. A picture
    // already decoded is uploaded in place of the original at once; otherwise
    // the original is shown until the decoder delivers (apply_ready_replacements).
    GeTextureReplacementPtr replacement;
    if (content_key != 0u && !ge_texture_mods_admit(draw.texture_address, content_key, s.frame_epoch))
        content_key = 0u;
    if (content_key != 0u) {
        const std::uint32_t rows = ge_texture_key_rows(draw.texture_format, base_width, base_height);
        if (ge_texture_mods_dumping())
            ge_texture_mods_dump(content_key, base_width, rows, std::span<const std::byte>(packed).first(
                static_cast<std::size_t>(base_width) * rows * 4u));
        replacement = ge_texture_mods_find(content_key, rows, base_height);
        // The button prompts' pictures go over whichever picture is in force,
        // before its smaller levels are made.
        replacement = ge_texture_mods_compose_overlay(content_key, replacement, base_width, base_height,
            std::span<const std::byte>(packed).first(static_cast<std::size_t>(base_width) * base_height * 4u));
    }
    const std::uint64_t incoming_bytes = replacement
        ? texture_image_bytes(replacement->width, replacement->height, replacement->mip_levels)
        : packed.size();
    const std::uint32_t entry_limit = std::max<std::uint32_t>(16u, ge_gpu_config().texture_cache_entries);
    const std::uint64_t byte_limit = static_cast<std::uint64_t>(
        std::max<std::uint32_t>(16u, ge_gpu_config().texture_cache_mb)) * 1024ull * 1024ull;
    while (s.textures.size() >= entry_limit || s.texture_cache_bytes + incoming_bytes > byte_limit) {
        auto victim = s.textures.end();
        for (auto it = s.textures.begin(); it != s.textures.end(); ++it) {
            if (it->second.last_used_epoch == s.frame_epoch) continue;
            if (victim == s.textures.end() ||
                it->second.last_used_epoch < victim->second.last_used_epoch)
                victim = it;
        }
        if (victim == s.textures.end()) {
            ++s.report.rejected_texture_decodes;
            return false;
        }
        evict_texture(s, victim);
        ++s.report.evicted_textures;
    }

    Dx12Texture texture{};
    texture.descriptor = draw;
    texture.width = base_width;
    texture.height = base_height;
    texture.mip_levels = mip_levels;
    texture.checksum = checksum;
    texture.signature_epoch = s.frame_epoch;
    texture.last_used_epoch = s.frame_epoch;
    texture.content_key = content_key;
    // Proof tooling only (ge_gpu_backend_copy_last_texture_rgba): not worth a copy per texture in the game.
    if (s.readback_enabled)
        s.last_texture_rgba.assign(packed.begin(), packed.begin() + static_cast<std::size_t>(base_width) * base_height * 4u);
    bool exact_levels = false, mod_picture = false;
    if (replacement) {
        texture.width = replacement->width;
        texture.height = replacement->height;
        texture.mip_levels = std::min(replacement->mip_levels, kMaxTextureLevels);
        texture.level_shift = replacement_level_shift(*replacement, base_width);
        exact_levels = replacement->exact_levels;
        mod_picture = replacement->mod_picture;
        texture.replacement = std::move(replacement);
        texture.replaced = true;
    } else {
        texture.rgba8 = std::move(packed);
    }
    // Stage 45.2 defers the CPU copy until the frame slot is fence-safe.
    // `rgba8` already owns the decoded mip chain, so allocating/mapping a D3D12
    // upload resource here is pure churn. record_pending_texture_uploads() copies
    // it into the persistent mapped arena of the frame being submitted.
    if (!create_texture_image(s, texture)) return false;
    texture.sampler_index = ensure_sampler(s, draw, texture.replaced ? texture.level_shift : -1.0f, false,
                                           exact_levels, mod_picture);
    texture.scene_sampler_index =
        ensure_sampler(s, draw, texture.replaced ? texture.level_shift : -1.0f, true, exact_levels, mod_picture);
    const std::uint32_t srv_index = texture.srv_index;
    s.texture_cache_bytes += texture.gpu_bytes;
    s.pending_texture_keys.push_back(key);
    overlay_note_texture(texture.content_key, 1);
    s.textures.emplace(key, std::move(texture));
    ++s.report.decoded_texture_uploads;
    ++chili::frame_costs.textures;
    chili::frame_costs.texture_bytes += expected;
    s.report.decoded_texture_bytes += expected;
    s.report.texture_images_created = s.textures.size();
    s.report.texture_image_uploads = s.report.decoded_texture_uploads;
    s.report.texture_image_upload_bytes += expected;
    s.report.last_texture_key = key;
    s.report.last_texture_checksum = checksum;
    s.report.last_texture_width = base_width;
    s.report.last_texture_height = base_height;
    s.report.last_texture_format = draw.texture_format;
    if (draw.texture_format == 4u) ++s.report.decoded_t4_textures;
    if (draw.texture_format == 5u) ++s.report.decoded_t8_textures;
    if (draw.texture_format <= 2u) ++s.report.decoded_direct16_textures;
    if (draw.texture_format == 3u) ++s.report.decoded_direct32_textures;
    if (draw.texture_format == 6u) ++s.report.decoded_indexed16_textures;
    if (draw.texture_format == 7u) ++s.report.decoded_indexed32_textures;
    if (draw.texture_format == 8u) ++s.report.decoded_dxt1_textures;
    if (draw.texture_format == 9u) ++s.report.decoded_dxt3_textures;
    if (draw.texture_format == 10u) ++s.report.decoded_dxt5_textures;
    if (draw.texture_format >= 8u && draw.texture_format <= 10u)
        s.report.compressed_texture_formats_active = true;
    s.report.uploaded_mip_levels += mip_levels;
    s.report.texture_descriptor_layout_created = true;
    s.report.texture_descriptor_pool_created = true;
    s.report.texture_descriptor_sets_allocated = s.textures.size();
    (void)srv_index;
    return true;
}

void record_pending_texture_uploads(Dx12GeState &s, Dx12FrameResources &frame) noexcept {
    constexpr UINT64 kPlacementAlignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    const auto align_up = [](UINT64 value, UINT64 alignment) noexcept {
        return (value + alignment - 1u) & ~(alignment - 1u);
    };

    for (const std::uint64_t key : s.pending_texture_keys) {
        auto found = s.textures.find(key);
        if (found == s.textures.end()) continue;
        Dx12Texture &texture = found->second;
        // A texture swapped for its replacement before the original went up is
        // listed twice; the first visit uploads it.
        if (!texture.image || !texture.upload_pending) continue;
        const std::span<const std::byte> source_bytes = texture.replacement
            ? std::span<const std::byte>(texture.replacement->rgba8)
            : std::span<const std::byte>(texture.rgba8);
        if (source_bytes.size() < texture.gpu_bytes) continue;

        const D3D12_RESOURCE_DESC desc = texture.image->GetDesc();
        std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, kMaxTextureLevels> footprints{};
        std::array<UINT, kMaxTextureLevels> rows{};
        std::array<UINT64, kMaxTextureLevels> row_sizes{};
        UINT64 upload_bytes = 0u;
        s.device->GetCopyableFootprints(&desc, 0u, texture.mip_levels, 0u,
                                         footprints.data(), rows.data(), row_sizes.data(),
                                         &upload_bytes);

        const UINT64 arena_offset = align_up(
            static_cast<UINT64>(frame.texture_upload_cursor), kPlacementAlignment);
        const bool use_arena = s.texture_upload_ring_enabled &&
            frame.texture_upload_buffer && frame.mapped_texture_upload &&
            arena_offset <= kTextureUploadCapacity &&
            upload_bytes <= kTextureUploadCapacity - arena_offset;

        ID3D12Resource *upload_resource = nullptr;
        std::byte *mapped_base = nullptr;
        UINT64 footprint_base = 0u;
        ComPtr<ID3D12Resource> fallback_upload;

        if (use_arena) {
            footprint_base = arena_offset;
            s.device->GetCopyableFootprints(&desc, 0u, texture.mip_levels, footprint_base,
                                             footprints.data(), rows.data(), row_sizes.data(),
                                             nullptr);
            upload_resource = frame.texture_upload_buffer.Get();
            mapped_base = frame.mapped_texture_upload;
            frame.texture_upload_cursor = static_cast<std::size_t>(footprint_base + upload_bytes);
        } else {
            // Extremely large streaming bursts keep the old safe behavior rather
            // than dropping a texture. This path should be rare; the normal path
            // performs zero upload-resource allocations per texture.
            D3D12_RESOURCE_DESC upload{};
            upload.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            upload.Width = std::max<UINT64>(upload_bytes, 256u);
            upload.Height = 1u;
            upload.DepthOrArraySize = 1u;
            upload.MipLevels = 1u;
            upload.Format = DXGI_FORMAT_UNKNOWN;
            upload.SampleDesc.Count = 1u;
            upload.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES up_heap{};
            up_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
            const HRESULT create_hr = s.device->CreateCommittedResource(
                &up_heap, D3D12_HEAP_FLAG_NONE, &upload,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&fallback_upload));
            if (FAILED(create_hr)) {
                runtime_log_error("dx12 texture upload",
                    hr_text(create_hr, "CreateCommittedResource(texture upload fallback)"));
                continue;
            }
            void *mapped = nullptr;
            const D3D12_RANGE no_read{0u, 0u};
            const HRESULT map_hr = fallback_upload->Map(0u, &no_read, &mapped);
            if (FAILED(map_hr) || mapped == nullptr) {
                runtime_log_error("dx12 texture upload",
                    hr_text(map_hr, "Map(texture upload fallback)"));
                continue;
            }
            upload_resource = fallback_upload.Get();
            mapped_base = static_cast<std::byte *>(mapped);
        }

        std::size_t source_offset = 0u;
        std::uint32_t w = texture.width;
        std::uint32_t h = texture.height;
        for (std::uint32_t level = 0u; level < texture.mip_levels; ++level) {
            const std::size_t row_bytes = static_cast<std::size_t>(w) * 4u;
            for (std::uint32_t y = 0u; y < h; ++y) {
                std::memcpy(mapped_base + footprints[level].Offset +
                                static_cast<std::size_t>(y) * footprints[level].Footprint.RowPitch,
                            source_bytes.data() + source_offset +
                                static_cast<std::size_t>(y) * row_bytes,
                            row_bytes);
            }
            source_offset += row_bytes * h;
            w = std::max(1u, w >> 1u);
            h = std::max(1u, h >> 1u);
        }

        if (fallback_upload) {
            fallback_upload->Unmap(0u, nullptr);
            frame.transient_resources.push_back(fallback_upload);
        }

        if (texture.block.heap != kNoTextureHeap) {
            // The heap range may have held another texture: this one is the live one now.
            D3D12_RESOURCE_BARRIER alias{};
            alias.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
            alias.Aliasing.pResourceAfter = texture.image.Get();
            s.list->ResourceBarrier(1u, &alias);
        }
        for (std::uint32_t level = 0u; level < texture.mip_levels; ++level) {
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = texture.image.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = level;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = upload_resource;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = footprints[level];
            s.list->CopyTextureRegion(&dst, 0u, 0u, 0u, &src, nullptr);
        }
        transition(s.list.Get(), texture.image.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        texture.pending_upload.Reset();
        // Chili: the GPU has the pixels now. Only the checksum is needed to
        // recognise the same content again.
        texture.upload_pending = false;
        texture.replacement.reset();
        texture.rgba8.clear();
        texture.rgba8.shrink_to_fit();
    }
    s.pending_texture_keys.clear();
}

Dx12TransformConstants make_transform_constants(const Dx12Batch &batch,
                                                 std::uint32_t logical_width,
                                                 std::uint32_t logical_height,
                                                 bool snap_vertices) noexcept {
    Dx12TransformConstants constants{};
    logical_width = std::max<std::uint32_t>(1u, logical_width);
    logical_height = std::max<std::uint32_t>(1u, logical_height);
    // Chili: the GE samples a triangle 7/16 of a pixel into each pixel, D3D at
    // the centre. Moving triangles by the difference makes edges and texel
    // weights land where they do on the PSP. Rectangles are sampled at the
    // centre there too.
    const float center_shift = batch.draw.primitive == 6u ? 0.0f : 1.0f / 16.0f;
    // At native resolution triangle vertices also land on the GE's 1/16 pixel
    // grid, which decides which row or column a shallow edge covers.
    const bool snap = snap_vertices && batch.draw.primitive != 6u;
    if (!batch.hardware_transform) {
        // Mode 2 converts already viewport-transformed PSP screen coordinates
        // into D3D clip space using this framebuffer's *logical* extent.
        constants.uv = {2.0f / static_cast<float>(logical_width),
                        2.0f / static_cast<float>(logical_height),
                        1.0f / 65535.0f, center_shift};
        constants.control = {2u, 0u, 0u, snap ? 1u : 0u};
        return constants;
    }
    const GeGpuHardwareTransform &hw = batch.transform;
    const auto row = [&](std::size_t r) {
        return std::array<float, 4>{
            hw.model_to_clip[r], hw.model_to_clip[4u + r],
            hw.model_to_clip[8u + r], hw.model_to_clip[12u + r]};
    };
    const auto add_scaled = [](const std::array<float, 4> &a, float sa,
                               const std::array<float, 4> &b, float sb) {
        return std::array<float, 4>{a[0] * sa + b[0] * sb,
                                    a[1] * sa + b[1] * sb,
                                    a[2] * sa + b[2] * sb,
                                    a[3] * sa + b[3] * sb};
    };
    const auto clip_x = row(0u);
    const auto clip_y = row(1u);
    const auto clip_z = row(2u);
    const auto clip_w = row(3u);
    const float x_a = hw.viewport_scale_x * (2.0f / static_cast<float>(logical_width));
    const float x_b = (hw.viewport_center_x - hw.viewport_offset_x + center_shift) *
                      (2.0f / static_cast<float>(logical_width)) - 1.0f;
    const float y_a = hw.viewport_scale_y * (2.0f / static_cast<float>(logical_height));
    const float y_b = (hw.viewport_center_y - hw.viewport_offset_y + center_shift) *
                      (2.0f / static_cast<float>(logical_height)) - 1.0f;
    constexpr float inv_depth = 1.0f / 65535.0f;
    const float z_a = hw.viewport_scale_z * inv_depth;
    const float z_b = hw.viewport_center_z * inv_depth;
    constants.row0 = add_scaled(clip_x, x_a, clip_w, x_b);
    constants.row1 = add_scaled(clip_y, -y_a, clip_w, -y_b);
    constants.row2 = add_scaled(clip_z, z_a, clip_w, z_b);
    constants.row3 = clip_w;
    constants.view_z = hw.model_to_view_z;
    constants.uv = {hw.uv_scale_u, hw.uv_scale_v, hw.uv_offset_u, hw.uv_offset_v};
    constants.fog = {hw.fog_end, hw.fog_slope,
                     8.0f * static_cast<float>(logical_width), 8.0f * static_cast<float>(logical_height)};
    // 1: the far plane maps to depth 0 (negative Z scale), 2: to the top of the range.
    constants.control = {1u, hw.depth_clip_enabled ? (hw.viewport_scale_z < 0.0f ? 1u : 2u) : 0u,
                         hw.vertex_color_affine ? 1u : 0u, snap ? 1u : 0u};
    constants.color_mul = hw.vertex_color_mul;
    constants.color_add = hw.vertex_color_add;
    return constants;
}

void clear_accumulation(Dx12GeState &s) noexcept {
    s.vertices.clear();
    s.packed_0115_vertices.clear();
    s.indices.clear();
    s.batches.clear();
    s.surface_write_serial = 0u;
}

void note_fallbacks(Dx12GeState &s, std::uint32_t fallbacks) noexcept {
    if (fallbacks == 0u) return;
    const std::uint32_t fresh = fallbacks & ~s.report.fallbacks_seen;
    s.pending_fallbacks |= fallbacks;
    s.report.fallbacks_seen |= fallbacks;
    // First sighting only; the names come from ge_gpu_fallback_names().
    for (std::uint32_t bit = 1u; fresh != 0u && bit != 0u; bit <<= 1u)
        if ((fresh & bit) != 0u) (void)runtime_log_once(ge_gpu_fallback_names(bit));
}

#include "ge_gpu_post_dx12.hpp"
#include "ge_gpu_glow_dx12.hpp"

bool create_backend(Dx12GeState &s, std::string &error) noexcept {
    const GeGpuConfig &config = ge_gpu_config();
    const std::uint32_t scale = std::clamp(config.internal_scale, 1u, 8u);
    s.sheet_width = std::clamp(config.surface_width, kReferenceWidth, 1024u);
    s.sheet_height = std::clamp(config.surface_height, kReferenceHeight, 1024u);
    s.target_width = s.sheet_width * scale;
    s.target_height = s.sheet_height * scale;
    s.readback_enabled = config.frame_readback;
    s.texture_upload_ring_enabled = config.texture_upload_ring;
    HRESULT hr = CreateDXGIFactory2(0u, IID_PPV_ARGS(&s.factory));
    if (FAILED(hr)) { error = hr_text(hr, "CreateDXGIFactory2(DX12 GE)"); return false; }
    if (!select_adapter(s, error)) return false;
    // Chili: a scale that was picked to fit the monitor is cut down on a card
    // with little memory. A sheet is 512 x 512 x scale^2 pixels, and the game's
    // four surfaces, their snapshots and the depth buffer make about ten.
    if (config.internal_scale_auto && scale > 2u) {
        DXGI_ADAPTER_DESC1 adapter_desc{};
        if (SUCCEEDED(s.adapter->GetDesc1(&adapter_desc))) {
            const std::uint64_t megabytes = static_cast<std::uint64_t>(adapter_desc.DedicatedVideoMemory) >> 20u;
            const std::uint32_t limit = megabytes < 1024u ? 2u : megabytes < 3072u ? 4u : megabytes < 6144u ? 6u : 8u;
            if (scale > limit) {
                GeGpuConfig adjusted = config;
                adjusted.internal_scale = limit;
                set_ge_gpu_config(adjusted);
                s.target_width = s.sheet_width * limit;
                s.target_height = s.sheet_height * limit;
            }
        }
    }
    hr = D3D12CreateDevice(s.adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&s.device));
    if (FAILED(hr)) { error = hr_text(hr, "D3D12CreateDevice(DX12 GE)"); return false; }
    select_depth_and_msaa(s);
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = s.device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&s.queue));
    if (FAILED(hr)) { error = hr_text(hr, "CreateCommandQueue(DX12 GE)"); return false; }

    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC upload{};
    upload.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upload.Width = kGeometryUploadCapacity;
    upload.Height = 1u;
    upload.DepthOrArraySize = 1u;
    upload.MipLevels = 1u;
    upload.Format = DXGI_FORMAT_UNKNOWN;
    upload.SampleDesc.Count = 1u;
    upload.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (Dx12FrameResources &frame : s.frames) {
        hr = s.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator));
        if (FAILED(hr)) { error = hr_text(hr, "CreateCommandAllocator(DX12 GE frame)"); return false; }
        hr = s.device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                IID_PPV_ARGS(&frame.upload_buffer));
        if (FAILED(hr)) { error = hr_text(hr, "CreateCommittedResource(DX12 GE geometry upload)"); return false; }
        void *mapped = nullptr;
        const D3D12_RANGE no_read{0u, 0u};
        hr = frame.upload_buffer->Map(0u, &no_read, &mapped);
        if (FAILED(hr) || mapped == nullptr) { error = hr_text(hr, "Map(DX12 GE geometry upload)"); return false; }
        frame.mapped_upload = static_cast<std::byte *>(mapped);

        if (s.texture_upload_ring_enabled) {
            D3D12_RESOURCE_DESC texture_upload = upload;
            texture_upload.Width = kTextureUploadCapacity;
            hr = s.device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &texture_upload,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&frame.texture_upload_buffer));
            if (FAILED(hr)) { error = hr_text(hr, "CreateCommittedResource(DX12 texture upload arena)"); return false; }
            mapped = nullptr;
            hr = frame.texture_upload_buffer->Map(0u, &no_read, &mapped);
            if (FAILED(hr) || mapped == nullptr) { error = hr_text(hr, "Map(DX12 texture upload arena)"); return false; }
            frame.mapped_texture_upload = static_cast<std::byte *>(mapped);
        }
        frame.texture_upload_cursor = 0u;
        frame.transient_resources.reserve(128u);
    }
    hr = s.device->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, s.frames[0].allocator.Get(), nullptr,
                                     IID_PPV_ARGS(&s.list));
    if (FAILED(hr)) { error = hr_text(hr, "CreateCommandList(DX12 GE)"); return false; }
    s.list->Close();
    (void)s.list.As(&s.list1);

    hr = s.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.texture_allocator));
    if (FAILED(hr)) { error = hr_text(hr, "CreateCommandAllocator(DX12 GE texture)"); return false; }
    hr = s.device->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, s.texture_allocator.Get(), nullptr,
                                     IID_PPV_ARGS(&s.texture_list));
    if (FAILED(hr)) { error = hr_text(hr, "CreateCommandList(DX12 GE texture)"); return false; }
    s.texture_list->Close();

    hr = s.device->CreateFence(0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence));
    if (FAILED(hr)) { error = hr_text(hr, "CreateFence(DX12 GE)"); return false; }
    s.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (s.fence_event == nullptr) { error = "CreateEventW failed for DX12 GE fence"; return false; }
    if (!compile_shaders(s, error)) return false;
    if (!create_root_signature(s, error)) return false;
    if (!create_targets(s, error)) return false;
    if (!create_present_pipeline(s, error)) return false;
    // Chili: anti-aliasing and ambient occlusion passes. Without them the game
    // still draws, so a failure here is reported and they stay off.
    if (std::string post_error; !create_post(s, post_error)) {
        runtime_log_error("dx12 post-process passes are off", post_error);
        destroy_post();
    }
    // Chili: the glow at the PSP's size ([Rendering] Glow). Without it the
    // game's own glow is drawn.
    if (std::string glow_error; !create_glow(s, glow_error)) {
        runtime_log_error("dx12 smooth glow is off", glow_error);
        destroy_glow();
    }
    s.vertices.reserve(262144u);
    s.packed_0115_vertices.reserve(2621440u);
    s.indices.reserve(524288u);
    s.batches.reserve(4096u);
    // Stage 45.2: reserve hot node maps up front. City streaming otherwise can
    // grow buckets while the GE thread is preparing a frame. References to
    // unordered_map nodes remain valid across rehash, but avoiding rehash also
    // reduces allocator/driver-thread noise.
    s.textures.reserve(std::max<std::uint32_t>(256u, ge_gpu_config().texture_cache_entries));
    s.frame_targets.reserve(64u);
    s.pipelines.reserve(512u);
    s.sampler_cache.reserve(64u);
    s.pending_texture_keys.reserve(256u);
    s.free_texture_srvs.reserve(1024u);
    s.retired_texture_srvs.reserve(256u);
    s.retired_texture_blocks.reserve(256u);
    // The first texture heap exists before the first frame; later ones are added as it fills.
    (void)add_texture_heap(s);
    // GPU timing is a diagnostic: without it the renderer runs the same.
    if (SUCCEEDED(s.queue->GetTimestampFrequency(&s.timestamp_frequency)) && s.timestamp_frequency != 0u) {
        D3D12_QUERY_HEAP_DESC query_heap{};
        query_heap.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        query_heap.Count = kFrameCount * 2u;
        D3D12_RESOURCE_DESC readback = upload;
        readback.Width = kFrameCount * 2u * sizeof(UINT64);
        D3D12_HEAP_PROPERTIES readback_heap{};
        readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
        if (FAILED(s.device->CreateQueryHeap(&query_heap, IID_PPV_ARGS(&s.timestamp_heap))) ||
            FAILED(s.device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&s.timestamp_readback)))) {
            s.timestamp_heap.Reset();
            s.timestamp_readback.Reset();
        }
    }
    // Chili: the overlay pass (a host page over the picture). Created now, so
    // the first page costs no frame; without it the game draws the same.
    if (std::string overlay_error; !create_overlay(s, overlay_error)) {
        runtime_log_error("dx12 overlay pass is off", overlay_error);
        destroy_overlay();
    }
    return true;
}

void destroy_backend(Dx12GeState &s) noexcept {
    std::string ignored;
    if (s.queue && s.fence) (void)wait_for_gpu(s, ignored);
    for (Dx12FrameResources &frame : s.frames) {
        if (frame.upload_buffer && frame.mapped_upload != nullptr)
            frame.upload_buffer->Unmap(0u, nullptr);
        if (frame.texture_upload_buffer && frame.mapped_texture_upload != nullptr)
            frame.texture_upload_buffer->Unmap(0u, nullptr);
        frame.mapped_upload = nullptr;
        frame.mapped_texture_upload = nullptr;
        frame.transient_resources.clear();
        frame.texture_upload_buffer.Reset();
        frame.upload_buffer.Reset();
        frame.texture_upload_cursor = 0u;
        frame.allocator.Reset();
        frame.fence_value = 0u;
        frame.timed = false;
    }
    release_swapchain_buffers(s);
    destroy_post();
    destroy_glow();
    destroy_overlay();
    s.swapchain.Reset();
    s.swap_rtv_heap.Reset();
    s.present_pipeline.Reset();
    s.present_pixel_shader.Reset();
    s.present_vertex_shader.Reset();
    s.pipelines.clear();
    s.textures.clear();
    s.pending_texture_keys.clear();
    s.free_texture_srvs.clear();
    s.retired_texture_srvs.clear();
    s.free_texture_blocks.clear();
    s.retired_texture_blocks.clear();
    s.texture_heaps.clear();
    s.texture_heap_cursor = 0u;
    s.timestamp_heap.Reset();
    s.timestamp_readback.Reset();
    s.timestamp_frequency = 0u;
    s.gpu_time_count = 0u;
    s.gpu_time_total_ms = 0.0;
    s.gpu_time_max_ms = 0.0;
    s.gpu_time_bins.fill(0u);
    s.sampler_cache.clear();
    s.known_frame_targets.clear();
    s.last_registered_framebuffer_target = 0xFFFFFFFFu;
    s.pixel_shader.Reset();
    s.cubic_pixel_shader.Reset();
    s.depth_write_pixel_shader.Reset();
    s.packed_0115_vertex_shader.Reset();
    s.vertex_shader.Reset();
    s.root_signature.Reset();
    s.readback_buffer.Reset();
    s.surface_readback_buffer.Reset();
    s.surface_readback_bytes = 0u;
    s.frame_targets.clear();
    s.depth_targets.clear();
    s.sampler_heap.Reset();
    s.srv_heap.Reset();
    s.dsv_heap.Reset();
    s.rtv_heap.Reset();
    s.texture_list.Reset();
    s.texture_allocator.Reset();
    s.list1.Reset();
    s.list.Reset();
    if (s.fence_event != nullptr) CloseHandle(s.fence_event);
    s.fence_event = nullptr;
    s.fence.Reset();
    s.queue.Reset();
    s.device.Reset();
    s.adapter.Reset();
    s.factory.Reset();
    s.frame_rgba.clear();
    s.last_texture_rgba.clear();
    s.ready_replacements.clear();
    s.overlay_changes.clear();
    s.texture_cache_bytes = 0u;
    s.next_rtv = 0u;
    s.next_dsv = 0u;
    s.next_srv = 1u;
    s.next_sampler = 1u;
    s.frame_cursor = 0u;
    s.frame_epoch = 1u;
    s.sample_count = 1u;
    s.sample_quality = 0u;
    s.depth_format = kDepthFormat;
    s.depth_bits = 32u;
    s.swap_width = s.swap_height = 0u;
    s.native_window = nullptr;
    s.direct_present_ok = false;
    s.presented_framebuffer = 0u;
    s.missed_display_intervals = 0u;
    s.display_pending = false;
    s.swapchain_tearing = false;
    s.pending_fallbacks = 0u;
    clear_accumulation(s);
    s.enabled = false;
}

} // namespace

bool initialize_ge_gpu_backend(std::string &error) {
    Dx12GeState &s = state();
    destroy_backend(s);
    s.report = {};
    s.display_framebuffer = 0u;
    const GeGpuConfig &rendering = ge_gpu_config();
    s.report.requested = rendering.backend == GeGpuConfig::Backend::DirectX12
        ? GeGpuBackendKind::DirectX12 : GeGpuBackendKind::Software;
    s.report.active = GeGpuBackendKind::Software;
    s.report.frames_in_flight_capacity = kFrameCount;
    if (rendering.backend != GeGpuConfig::Backend::DirectX12) {
        s.report.message = "Software GE backend active";
        error.clear();
        return true;
    }
    if (!create_backend(s, error)) {
        const std::string native_error = error;
        runtime_log_error("dx12 ge initialize", native_error);
        destroy_backend(s);
        const bool strict_mode = rendering.strict;
        s.report.requested = GeGpuBackendKind::DirectX12;
        s.report.active = GeGpuBackendKind::Software;
        s.report.frames_in_flight_capacity = kFrameCount;
        s.report.message = "Native DirectX 12 GE failed; software GE stays active: " + native_error;
        error = native_error;
        return !strict_mode;
    }
    s.enabled = true;
    s.report.active = GeGpuBackendKind::DirectX12;
    s.report.loader_opened = true;
    s.report.instance_created = true;
    s.report.device_created = true;
    s.report.transfer_buffer_created = true;
    s.report.transfer_memory_mapped = true;
    s.report.command_pool_created = true;
    s.report.transfer_self_test_passed = true;
    s.report.offscreen_image_created = true;
    s.report.offscreen_image_memory_bound = true;
    s.report.offscreen_image_view_created = true;
    s.report.render_pass_created = true;
    s.report.framebuffer_created = true;
    s.report.shader_modules_created = true;
    s.report.graphics_pipeline_created = false;
    s.report.offscreen_self_test_passed = true;
    s.report.physical_device_count = 1u;
    s.report.graphics_queue_family = 0u;
    s.report.memory_type_index = 0u;
    s.report.upload_capacity_bytes = kGeometryUploadCapacity;
    s.report.offscreen_width = s.target_width;
    s.report.offscreen_height = s.target_height;
    s.report.game_frame_readback_bytes = s.readback_enabled ? s.frame_rgba.size() : 0u;
    s.report.frames_in_flight_capacity = kFrameCount;
    s.report.texture_descriptor_layout_created = true;
    s.report.texture_descriptor_pool_created = true;
    s.report.textured_shader_modules_created = true;
    s.report.textured_pipeline_created = true;
    s.report.full_mip_chain_active = true;
    s.report.mipmap_state_active = true;
    s.report.base_texture_formats_active = true;
    s.report.depth_image_created = true;
    s.report.depth_image_memory_bound = true;
    s.report.depth_image_view_created = true;
    s.report.depth_attachment_active = true;
    s.report.alpha_test_shader_active = true;
    s.report.standard_alpha_blend_pipeline_active = true;
    s.report.observed_blend_modes_pipeline_active = true;
    s.report.color_write_mask_pipeline_active = true;
    s.report.fog_shader_active = true;
    s.report.message = "Direct3D 12 GE backend active on " + s.adapter_name;
    runtime_log_line(std::string("dx12 ge initialized adapter=") + s.adapter_name +
                     " target=" + std::to_string(s.target_width) + "x" +
                     std::to_string(s.target_height) +
                     " msaa=" + std::to_string(s.sample_count) +
                     " depth=" + std::to_string(s.depth_bits));
    error.clear();
    return true;
}

void shutdown_ge_gpu_backend() noexcept {
    Dx12GeState &s = state();
    if (s.enabled) runtime_log_line("dx12 ge shutdown");
    destroy_backend(s);
    s.report = {};
    s.display_framebuffer = 0u;
}

bool ge_gpu_backend_active() noexcept { return state().enabled; }
bool ge_gpu_backend_transfer_ready() noexcept { return state().enabled; }
bool ge_gpu_backend_graphics_ready() noexcept { return state().enabled; }

void ge_gpu_backend_record_draw(const GeGpuDrawDescriptor &draw) noexcept {
    Dx12GeState &s = state();
    if (!s.enabled) return;
    ++s.report.draw_calls;
    s.report.vertices += draw.vertex_count;
    if (draw.texture_enabled) ++s.report.textured_draw_calls;
    note_fallbacks(s, draw_fallbacks(draw));
    const std::uint32_t target = draw.framebuffer_address & kVramOffsetMask;
    // Register/create render targets as soon as the GE identifies them. That
    // makes a target produced by an earlier PRIM immediately available as a
    // GPU SRV to a later framebuffer-feedback PRIM in the same display interval.
    //
    // Chili: upstream also re-derived each target's logical extent here from
    // the scissor and from whichever texture state last sampled it. Every
    // surface is a fixed sheet now (GeGpuConfig::surface_*), so nothing about a
    // target changes after it is created.
    if (draw.framebuffer_stride != 0u || target == s.display_framebuffer) {
        if (target != s.last_registered_framebuffer_target ||
            find_framebuffer_target(s, target) == nullptr) {
            s.last_registered_framebuffer_target = target;
            s.known_frame_targets.insert(target);
            s.report.framebuffer_targets_observed = s.known_frame_targets.size();
            std::string error;
            if (!ensure_framebuffer_target(s, target, error) && !error.empty())
                runtime_log_error("dx12 framebuffer target", error);
        }
    }
}

bool ge_gpu_backend_texture_needed(const GeGpuDrawDescriptor &draw) noexcept {
    Dx12GeState &s = state();
    if (!s.enabled || !draw.texture_enabled || draw.texture_format > 10u ||
        draw.texture_width == 0u || draw.texture_height == 0u) return false;
    if (find_feedback_target(s, draw.texture_address) != nullptr) {
        ++s.report.texture_cache_hits;
        return false; // native GPU->GPU framebuffer feedback: no CPU decode
    }
    ++s.report.texture_decode_requests;
    const std::uint64_t key = texture_key(draw);
    Dx12Texture *found = find_cached_texture(s, key);
    if (found == nullptr) return true;
    found->signature_epoch = s.frame_epoch;
    found->last_used_epoch = s.frame_epoch;
    if (draw.texture_content_signature != 0u &&
        found->descriptor.texture_content_signature != draw.texture_content_signature)
        return true;
    ++s.report.texture_cache_hits;
    return false;
}

void ge_gpu_backend_prepare_texture_keys(GeGpuDrawDescriptor &draw) noexcept {
    if (!draw.texture_enabled) {
        draw.texture_cache_key_hint = 0u;
        draw.texture_image_key_hint = 0u;
        return;
    }
    draw.texture_cache_key_hint = 0u;
    draw.texture_image_key_hint = 0u;
    const std::uint64_t key = texture_key(draw);
    draw.texture_cache_key_hint = key;
    draw.texture_image_key_hint = key;
}

bool ge_gpu_backend_texture_signature_needed(const GeGpuDrawDescriptor &draw) noexcept {
    Dx12GeState &s = state();
    if (!s.enabled || !draw.texture_enabled || draw.texture_width == 0u || draw.texture_height == 0u)
        return false;
    if (find_feedback_target(s, draw.texture_address) != nullptr)
        return false; // framebuffer target already has authoritative content on the GPU
    Dx12Texture *found = find_cached_texture(s, texture_key(draw));
    return found == nullptr || found->signature_epoch != s.frame_epoch;
}

bool ge_gpu_backend_is_framebuffer_feedback_texture(const GeGpuDrawDescriptor &draw) noexcept {
    const Dx12GeState &s = state();
    return s.enabled && draw.texture_enabled &&
           find_feedback_target(s, draw.texture_address) != nullptr;
}
bool ge_gpu_backend_adopt_shared_texture(const GeGpuDrawDescriptor &) noexcept { return false; }

bool ge_gpu_backend_texture_available(const GeGpuDrawDescriptor &draw) noexcept {
    Dx12GeState &s = state();
    if (!s.enabled || !draw.texture_enabled) return false;
    if (const auto *target = find_feedback_target(s, draw.texture_address))
        return target->color != nullptr;
    Dx12Texture *found = find_cached_texture(s, texture_key(draw));
    if (found == nullptr || !found->image) return false;
    found->last_used_epoch = s.frame_epoch;
    return true;
}

bool ge_gpu_backend_upload_decoded_texture(const GeGpuDrawDescriptor &draw, std::uint32_t width,
                                           std::uint32_t height,
                                           std::span<const std::byte> rgba) noexcept {
    if (rgba.empty()) return false;
    std::vector<std::byte> packed(rgba.begin(), rgba.end());
    return prepare_texture_upload(state(), draw, width, height, 1u, std::move(packed));
}

bool ge_gpu_backend_upload_decoded_texture_chain(const GeGpuDrawDescriptor &draw,
                                                 std::span<const GeGpuDecodedMipLevel> levels) noexcept {
    if (levels.empty() || levels.size() > 8u) return false;
    std::size_t total = 0u;
    std::uint32_t w = levels.front().width;
    std::uint32_t h = levels.front().height;
    if (w == 0u || h == 0u) return false;
    for (const auto &level : levels) {
        const std::size_t bytes = static_cast<std::size_t>(level.width) * level.height * 4u;
        if (level.width != w || level.height != h || level.rgba8.size() != bytes) return false;
        total += bytes;
        w = std::max(1u, w >> 1u);
        h = std::max(1u, h >> 1u);
    }
    std::vector<std::byte> packed;
    try {
        packed.reserve(total);
        for (const auto &level : levels) packed.insert(packed.end(), level.rgba8.begin(), level.rgba8.end());
    } catch (...) { return false; }
    return prepare_texture_upload(state(), draw, levels.front().width, levels.front().height,
                                  static_cast<std::uint32_t>(levels.size()), std::move(packed));
}

bool ge_gpu_backend_upload_decoded_texture_chain_packed(const GeGpuDrawDescriptor &draw,
                                                        std::uint32_t width, std::uint32_t height,
                                                        std::uint32_t mip_levels,
                                                        std::vector<std::byte> packed,
                                                        std::uint64_t content_key) noexcept {
    return prepare_texture_upload(state(), draw, width, height, mip_levels, std::move(packed), content_key);
}
bool ge_gpu_backend_copy_last_texture_rgba(std::span<std::byte> destination) noexcept {
    const Dx12GeState &s = state();
    if (s.last_texture_rgba.empty() || destination.size() < s.last_texture_rgba.size()) return false;
    std::memcpy(destination.data(), s.last_texture_rgba.data(), s.last_texture_rgba.size());
    return true;
}

void ge_gpu_backend_accumulate_color_triangles(
    const GeGpuDrawDescriptor &draw,
    std::span<const GeGpuVertex> triangle_vertices) noexcept {
    Dx12GeState &s = state();
    if (!s.enabled || triangle_vertices.empty() || triangle_vertices.size() % 3u != 0u) return;
    const std::size_t required =
        (s.vertices.size() + triangle_vertices.size()) * sizeof(Dx12UploadVertex) +
        s.packed_0115_vertices.size() +
        s.indices.size() * sizeof(std::uint32_t) + 16u;
    if (required > kGeometryUploadCapacity ||
        s.vertices.size() > std::numeric_limits<std::uint32_t>::max()) {
        ++s.report.game_vertex_overflows;
        note_fallbacks(s, kGeGpuFallbackGeometryOverflow);
        return;
    }
    try {
        const bool sampled_texture = draw.texture_enabled && ge_gpu_backend_texture_available(draw);
        const std::uint32_t first = static_cast<std::uint32_t>(s.vertices.size());
        // U and V arrive in texels. Chili: a surface is sampled through its
        // whole sheet, whatever size the texture registers claim for the view.
        float u_extent = static_cast<float>(draw.texture_width);
        float v_extent = static_cast<float>(draw.texture_height);
        if (const Dx12FramebufferTarget *feedback = draw.texture_enabled
                ? find_feedback_target(s, draw.texture_address) : nullptr) {
            u_extent = static_cast<float>(feedback->logical_width);
            v_extent = static_cast<float>(feedback->logical_height);
        }
        std::array<float, 4> region{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                                    std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
        bool region_known = true;
        for (const GeGpuVertex &source : triangle_vertices) {
            Dx12UploadVertex vertex = make_upload_vertex(source);
            // In texels here: for a surface these are its logical pixels.
            region[0] = std::min(region[0], source.u);
            region[1] = std::min(region[1], source.v);
            region[2] = std::max(region[2], source.u);
            region[3] = std::max(region[3], source.v);
            if (source.q != 1.0f) region_known = false;
            if (sampled_texture && u_extent > 0.0f && v_extent > 0.0f) {
                vertex.u /= u_extent;
                vertex.v /= v_extent;
            }
            s.vertices.push_back(vertex);
        }
        const std::uint32_t feedback_address = draw.texture_address & kVramOffsetMask;
        const bool framebuffer_feedback = draw.texture_enabled &&
            find_feedback_target(s, draw.texture_address) != nullptr;
        Dx12Batch batch{};
        batch.draw = draw;
        batch.first_vertex = first;
        batch.vertex_count = static_cast<std::uint32_t>(triangle_vertices.size());
        batch.logical_draw_count = 1u;
        batch.framebuffer_feedback = framebuffer_feedback;
        batch.feedback_address = feedback_address;
        // Inside the sheet only: outside it the sampler wraps or clamps to texels the region would miss.
        batch.feedback_region_known = framebuffer_feedback && sampled_texture && region_known &&
            region[0] >= 0.0f && region[1] >= 0.0f && region[2] <= u_extent && region[3] <= v_extent &&
            region[2] >= region[0] && region[3] >= region[1];
        batch.feedback_region = region;
        (void)append_or_merge_batch(s, std::move(batch));
        ++s.report.game_draw_calls;
        s.report.game_triangles += triangle_vertices.size() / 3u;
        s.report.game_vertices += triangle_vertices.size();
        if (draw.texture_enabled && sampled_texture) ++s.report.textured_game_draw_calls;
        else if (draw.texture_enabled) {
            ++s.report.game_textured_draws_without_texture;
            note_fallbacks(s, kGeGpuFallbackTextureMissing);
        }
    } catch (...) {
        ++s.report.game_vertex_overflows;
        note_fallbacks(s, kGeGpuFallbackGeometryOverflow);
    }
}

void ge_gpu_backend_accumulate_hardware_triangles(
    const GeGpuDrawDescriptor &draw,
    const GeGpuHardwareTransform &transform,
    std::span<const GeGpuVertex> vertices,
    std::span<const std::uint32_t> triangle_indices) noexcept {
    Dx12GeState &s = state();
    if (!s.enabled || vertices.empty()) return;

    // Native indices are deliberately isolated behind one DX12-only switch.
    // Stage 44.8 changed indexed submission *and* auto-enabled parallel decode
    // in the same regression window, so the original crash never proved the
    // index buffer itself guilty. 45.4 reintroduces only this one optimization;
    // the compatibility launcher can turn it off without rebuilding.
    const bool indexed = native_indexed_draw_enabled() && !triangle_indices.empty();
    const std::size_t emitted_count = triangle_indices.empty() ? vertices.size()
                                                               : triangle_indices.size();
    if (emitted_count == 0u ||
        (transform.primitive == 4u ? emitted_count < 3u : (emitted_count % 3u) != 0u)) return;
    if (s.vertices.size() > std::numeric_limits<std::uint32_t>::max() ||
        s.indices.size() > std::numeric_limits<std::uint32_t>::max()) {
        ++s.report.game_vertex_overflows;
        return;
    }

    const std::size_t vertices_to_append = indexed ? vertices.size() : emitted_count;
    const std::size_t indices_to_append = indexed ? triangle_indices.size() : 0u;
    const std::size_t required =
        (s.vertices.size() + vertices_to_append) * sizeof(Dx12UploadVertex) +
        s.packed_0115_vertices.size() +
        (s.indices.size() + indices_to_append) * sizeof(std::uint32_t) + 16u;
    if (required > kGeometryUploadCapacity) {
        ++s.report.game_vertex_overflows;
        return;
    }

    try {
        const bool sampled_texture = draw.texture_enabled && ge_gpu_backend_texture_available(draw);
        const std::uint32_t first_vertex = static_cast<std::uint32_t>(s.vertices.size());
        const std::uint32_t first_index = static_cast<std::uint32_t>(s.indices.size());

        if (indexed || triangle_indices.empty()) {
            for (const GeGpuVertex &source : vertices)
                s.vertices.push_back(make_upload_vertex(source));
        } else {
            // Exact Stage 44.7 fallback: expand the index stream on the CPU.
            for (std::uint32_t index : triangle_indices) {
                if (static_cast<std::size_t>(index) >= vertices.size()) {
                    ++s.report.game_vertex_overflows;
                    s.vertices.resize(first_vertex);
                    return;
                }
                s.vertices.push_back(make_upload_vertex(vertices[index]));
            }
        }

        if (indexed) {
            for (std::uint32_t index : triangle_indices) {
                if (static_cast<std::size_t>(index) >= vertices.size()) {
                    ++s.report.game_vertex_overflows;
                    s.vertices.resize(first_vertex);
                    s.indices.resize(first_index);
                    return;
                }
                s.indices.push_back(index);
            }
        }

        const std::uint32_t logical = std::max<std::uint32_t>(1u, transform.logical_prim_batches);
        const std::uint32_t feedback_address = draw.texture_address & kVramOffsetMask;
        const bool framebuffer_feedback = draw.texture_enabled &&
            find_feedback_target(s, draw.texture_address) != nullptr;
        Dx12Batch batch{};
        batch.draw = draw;
        batch.first_vertex = first_vertex;
        batch.vertex_count = static_cast<std::uint32_t>(vertices_to_append);
        batch.first_index = first_index;
        batch.index_count = indexed ? static_cast<std::uint32_t>(triangle_indices.size()) : 0u;
        batch.indexed = indexed;
        batch.logical_draw_count = logical;
        batch.framebuffer_feedback = framebuffer_feedback;
        batch.feedback_address = feedback_address;
        batch.hardware_transform = true;
        batch.transform = transform;
        (void)append_or_merge_batch(s, std::move(batch));

        s.report.game_draw_calls += logical;
        s.report.game_triangles += transform.primitive == 4u
            ? (emitted_count > 2u ? emitted_count - 2u : 0u) : emitted_count / 3u;
        s.report.game_vertices += emitted_count;
        s.report.hw_transform_draw_calls += logical;
        s.report.hw_transform_vertices += vertices.size();
        s.report.hw_transform_prim_batches += logical;
        s.report.hw_transform_unique_vertices_decoded += transform.unique_vertices_decoded;
        s.report.hw_transform_index_reuses += transform.index_reuses;
        if (draw.texture_enabled && sampled_texture) s.report.textured_game_draw_calls += logical;
        else if (draw.texture_enabled) {
            s.report.game_textured_draws_without_texture += logical;
            note_fallbacks(s, kGeGpuFallbackTextureMissing);
        }
    } catch (...) {
        ++s.report.game_vertex_overflows;
    }
}

bool ge_gpu_backend_accumulate_hardware_packed_0115(
    const GeGpuDrawDescriptor &draw,
    const GeGpuHardwareTransform &transform,
    std::span<const std::byte> packed_vertices,
    std::uint32_t vertex_count,
    std::span<const std::uint32_t> triangle_indices) noexcept {
    Dx12GeState &s = state();
    constexpr std::size_t kPackedStride = 10u;
    if (!s.enabled || vertex_count == 0u ||
        packed_vertices.size() != static_cast<std::size_t>(vertex_count) * kPackedStride)
        return false;

    const bool indexed = native_indexed_draw_enabled() && !triangle_indices.empty();
    const std::size_t emitted_count = triangle_indices.empty()
        ? static_cast<std::size_t>(vertex_count) : triangle_indices.size();
    if (emitted_count == 0u ||
        (transform.primitive == 4u ? emitted_count < 3u : (emitted_count % 3u) != 0u)) return false;

    const std::size_t first_packed_byte = s.packed_0115_vertices.size();
    if ((first_packed_byte % kPackedStride) != 0u) return false;
    const std::size_t first_vertex64 = first_packed_byte / kPackedStride;
    if (first_vertex64 > std::numeric_limits<std::uint32_t>::max() ||
        s.indices.size() > std::numeric_limits<std::uint32_t>::max())
        return false;

    const std::size_t packed_append_bytes = indexed || triangle_indices.empty()
        ? packed_vertices.size() : emitted_count * kPackedStride;
    const std::size_t index_append_count = indexed ? triangle_indices.size() : 0u;
    const std::size_t required = s.vertices.size() * sizeof(Dx12UploadVertex) +
        s.packed_0115_vertices.size() + packed_append_bytes +
        (s.indices.size() + index_append_count) * sizeof(std::uint32_t) + 16u;
    if (required > kGeometryUploadCapacity) {
        ++s.report.game_vertex_overflows;
        return false;
    }

    const std::uint32_t first_vertex = static_cast<std::uint32_t>(first_vertex64);
    const std::uint32_t first_index = static_cast<std::uint32_t>(s.indices.size());
    try {
        if (indexed || triangle_indices.empty()) {
            s.packed_0115_vertices.insert(s.packed_0115_vertices.end(),
                                          packed_vertices.begin(), packed_vertices.end());
        } else {
            // Compatibility mode keeps Stage 44.7 DrawInstanced semantics while
            // still avoiding CPU vertex conversion: duplicate only the original
            // 10-byte PSP records according to the triangle stream.
            for (std::uint32_t index : triangle_indices) {
                if (index >= vertex_count) {
                    ++s.report.game_vertex_overflows;
                    s.packed_0115_vertices.resize(first_packed_byte);
                    return false;
                }
                const std::byte *source = packed_vertices.data() +
                    static_cast<std::size_t>(index) * kPackedStride;
                s.packed_0115_vertices.insert(s.packed_0115_vertices.end(),
                                               source, source + kPackedStride);
            }
        }
        if (indexed) {
            for (std::uint32_t index : triangle_indices) {
                if (index >= vertex_count) {
                    ++s.report.game_vertex_overflows;
                    s.packed_0115_vertices.resize(first_packed_byte);
                    s.indices.resize(first_index);
                    return false;
                }
                s.indices.push_back(index);
            }
        }

        const std::uint32_t logical = std::max<std::uint32_t>(1u, transform.logical_prim_batches);
        const std::uint32_t feedback_address = draw.texture_address & kVramOffsetMask;
        const bool framebuffer_feedback = draw.texture_enabled &&
            find_feedback_target(s, draw.texture_address) != nullptr;
        Dx12Batch batch{};
        batch.draw = draw;
        batch.first_vertex = first_vertex;
        batch.vertex_count = static_cast<std::uint32_t>(indexed || triangle_indices.empty()
            ? vertex_count : emitted_count);
        batch.first_index = first_index;
        batch.index_count = indexed ? static_cast<std::uint32_t>(triangle_indices.size()) : 0u;
        batch.indexed = indexed;
        batch.packed_0115 = true;
        batch.logical_draw_count = logical;
        batch.framebuffer_feedback = framebuffer_feedback;
        batch.feedback_address = feedback_address;
        batch.hardware_transform = true;
        batch.transform = transform;
        (void)append_or_merge_batch(s, std::move(batch));

        s.report.game_draw_calls += logical;
        s.report.game_triangles += transform.primitive == 4u
            ? (emitted_count > 2u ? emitted_count - 2u : 0u) : emitted_count / 3u;
        s.report.game_vertices += emitted_count;
        s.report.hw_transform_draw_calls += logical;
        s.report.hw_transform_vertices += vertex_count;
        s.report.hw_transform_prim_batches += logical;
        s.report.hw_transform_unique_vertices_decoded += transform.unique_vertices_decoded;
        s.report.hw_transform_index_reuses += transform.index_reuses;
        if (draw.texture_enabled && ge_gpu_backend_texture_available(draw))
            s.report.textured_game_draw_calls += logical;
        else if (draw.texture_enabled)
            s.report.game_textured_draws_without_texture += logical;
        return true;
    } catch (...) {
        s.packed_0115_vertices.resize(first_packed_byte);
        s.indices.resize(first_index);
        ++s.report.game_vertex_overflows;
        return false;
    }
}

void ge_gpu_backend_set_native_window(void *native_window) noexcept {
    Dx12GeState &s = state();
    const HWND window = static_cast<HWND>(native_window);
    if (s.native_window == window) return;
    if (s.swapchain) {
        std::string ignored;
        (void)wait_for_gpu(s, ignored);
        release_swapchain_buffers(s);
        s.swapchain.Reset();
        s.swap_rtv_heap.Reset();
        s.swap_width = s.swap_height = 0u;
        s.direct_present_ok = false;
        s.presented_framebuffer = 0u;
        s.missed_display_intervals = 0u;
        s.report.swapchain_active = false;
    }
    s.native_window = window;
}

void ge_gpu_backend_set_display_framebuffer(std::uint32_t address) noexcept {
    Dx12GeState &s = state();
    s.display_framebuffer = address & kVramOffsetMask;
    if (!s.enabled || !s.device) return;
    std::string error;
    if (!ensure_framebuffer_target(s, s.display_framebuffer, error) && !error.empty())
        runtime_log_error("dx12 display framebuffer", error);
}

void ge_gpu_backend_warm_up(const GeGpuWarmUp &items) noexcept {
    Dx12GeState &s = state();
    if (!s.enabled || !s.device) return;
    std::string error;
    for (const std::uint32_t address : items.color_surfaces)
        if (!ensure_framebuffer_target(s, address, error) && !error.empty())
            runtime_log_error("dx12 warm-up surface", error);
    for (const std::uint32_t address : items.depth_surfaces)
        if (ensure_depth_target(s, address, error) == nullptr && !error.empty())
            runtime_log_error("dx12 warm-up depth surface", error);
    for (const std::uint32_t address : items.feedback_copies) {
        Dx12FramebufferTarget *target = find_framebuffer_target(s, address);
        if (target != nullptr && target->color && !ensure_feedback_copy(s, *target, error) && !error.empty())
            runtime_log_error("dx12 warm-up surface copy", error);
    }
    // Chili: a surface that reads itself is where the game makes its glow: its
    // small sheet is made now as well (two at most; a frame makes any other).
    if (Dx12Glow &glow = glow_state(); glow.ready)
        for (const std::uint32_t address : items.feedback_copies)
            if (glow.sheet_count < 2u) (void)ensure_glow_sheet(s, glow, address & kVramOffsetMask);
    for (const std::uint64_t key : items.pipelines) {
        if (s.pipelines.find(key) != s.pipelines.end()) continue;
        // Chili: a state of the glow sheets that this run cannot need (no
        // cubic shader, no multisampling, or no sheets at all: Glow=game).
        if (((key & kPipelineCubic) != 0u && !s.cubic_pixel_shader) ||
            ((key & kPipelineSingleSample) != 0u && (s.sample_count == 1u || !glow_state().ready))) continue;
        // A key from another build of the renderer may describe a state Direct3D refuses.
        ComPtr<ID3D12PipelineState> pipeline = create_pipeline(s, key, error);
        if (!pipeline) continue;
        s.pipelines.emplace(key, std::move(pipeline));
    }
    s.report.unique_pipeline_keys = s.pipelines.size();
}

GeGpuWarmUp ge_gpu_backend_created() {
    const Dx12GeState &s = state();
    GeGpuWarmUp out;
    for (const auto &[key, pipeline] : s.pipelines)
        if (pipeline) out.pipelines.push_back(key);
    for (const auto &[address, target] : s.frame_targets) {
        if (!target.color) continue;
        out.color_surfaces.push_back(address);
        if (target.feedback_copy) out.feedback_copies.push_back(address);
    }
    for (const auto &[address, target] : s.depth_targets)
        if (target.depth) out.depth_surfaces.push_back(address);
    std::sort(out.pipelines.begin(), out.pipelines.end());
    std::sort(out.color_surfaces.begin(), out.color_surfaces.end());
    std::sort(out.depth_surfaces.begin(), out.depth_surfaces.end());
    std::sort(out.feedback_copies.begin(), out.feedback_copies.end());
    return out;
}

namespace {

enum class SubmitMode { Present, Flush };

// Records and executes everything accumulated since the last submission.
// Present mode is the end of a guest frame: it advances the frame epoch and
// puts the displayed surface on the swapchain when a window is attached.
// Flush mode only executes, for readbacks and offline tools.
bool submit_accumulated(Dx12GeState &s, std::uint64_t vblank, SubmitMode mode) noexcept {
    const bool presenting = mode == SubmitMode::Present;
    if (!s.enabled || !s.device) {
        clear_accumulation(s);
        if (presenting) ++s.frame_epoch;
        return false;
    }
    const bool pending_only = presenting && s.display_pending &&
        ((s.vertices.empty() && s.packed_0115_vertices.empty()) || s.batches.empty());
    if (pending_only) clear_accumulation(s);
    if (!pending_only &&
        ((s.vertices.empty() && s.packed_0115_vertices.empty()) || s.batches.empty())) {
        if (presenting && ++s.missed_display_intervals > 4u) {
            s.direct_present_ok = false;
            s.presented_framebuffer = 0u;
        }
        clear_accumulation(s);
        if (presenting) ++s.frame_epoch;
        return false;
    }

    const std::size_t vertex_bytes = s.vertices.size() * sizeof(Dx12UploadVertex);
    const std::size_t packed_offset = (vertex_bytes + 3u) & ~std::size_t{3u};
    const std::size_t packed_bytes = s.packed_0115_vertices.size();
    const std::size_t index_offset = (packed_offset + packed_bytes + 3u) & ~std::size_t{3u};
    const std::size_t index_bytes = s.indices.size() * sizeof(std::uint32_t);
    const std::size_t bytes = index_offset + index_bytes;
    if (bytes > kGeometryUploadCapacity) {
        ++s.report.game_vertex_overflows;
        note_fallbacks(s, kGeGpuFallbackGeometryOverflow);
        clear_accumulation(s);
        if (presenting) ++s.frame_epoch;
        return false;
    }

    Dx12FrameResources &frame = s.frames[s.frame_cursor];
    std::string error;
    bool frame_free;
    {
        chili::CostTimer cost(chili::frame_costs.gpu_wait_ns);
        frame_free = wait_for_fence(s, frame.fence_value, error);
    }
    if (!frame_free) {
        runtime_log_error("dx12 ge frame wait", error);
        clear_accumulation(s);
        return false;
    }
    ++chili::frame_costs.submits;
    if (frame.timed && s.timestamp_readback) {
        // The slot's last submission has finished (the fence above): its two timestamps are readable.
        frame.timed = false;
        const SIZE_T offset = static_cast<SIZE_T>(s.frame_cursor) * 2u * sizeof(UINT64);
        const D3D12_RANGE read{offset, offset + 2u * sizeof(UINT64)};
        void *mapped = nullptr;
        if (SUCCEEDED(s.timestamp_readback->Map(0u, &read, &mapped)) && mapped != nullptr) {
            UINT64 ticks[2];
            std::memcpy(ticks, static_cast<const std::byte *>(mapped) + offset, sizeof(ticks));
            const D3D12_RANGE none{0u, 0u};
            s.timestamp_readback->Unmap(0u, &none);
            if (ticks[1] >= ticks[0]) {
                const double ms = static_cast<double>(ticks[1] - ticks[0]) * 1000.0 /
                                  static_cast<double>(s.timestamp_frequency);
                ++s.gpu_time_count;
                s.gpu_time_total_ms += ms;
                s.gpu_time_max_ms = std::max(s.gpu_time_max_ms, ms);
                ++s.gpu_time_bins[std::min<std::size_t>(s.gpu_time_bins.size() - 1u,
                                                        static_cast<std::size_t>(ms * 20.0))];
            }
        }
    }
    frame.transient_resources.clear();
    frame.texture_upload_cursor = 0u;
    if (frame.mapped_upload == nullptr ||
        (s.texture_upload_ring_enabled && frame.mapped_texture_upload == nullptr)) {
        runtime_log_error("dx12 ge", "frame upload arena is not mapped");
        clear_accumulation(s);
        return false;
    }
    if (vertex_bytes != 0u)
        std::memcpy(frame.mapped_upload, s.vertices.data(), vertex_bytes);
    if (packed_bytes != 0u)
        std::memcpy(frame.mapped_upload + packed_offset, s.packed_0115_vertices.data(), packed_bytes);
    if (index_bytes != 0u)
        std::memcpy(frame.mapped_upload + index_offset, s.indices.data(), index_bytes);

    bool direct_possible = false;
    if (presenting && s.native_window != nullptr) {
        std::string present_error;
        direct_possible = ensure_swapchain(s, present_error);
        if (!direct_possible && !present_error.empty())
            runtime_log_error("dx12 ge swapchain", present_error);
    }

    HRESULT hr = frame.allocator->Reset();
    if (FAILED(hr)) {
        runtime_log_error("dx12 ge", hr_text(hr, "CommandAllocator::Reset"));
        clear_accumulation(s);
        return false;
    }
    hr = s.list->Reset(frame.allocator.Get(), nullptr);
    if (FAILED(hr)) {
        runtime_log_error("dx12 ge", hr_text(hr, "CommandList::Reset"));
        clear_accumulation(s);
        return false;
    }

    if (s.timestamp_heap)
        s.list->EndQuery(s.timestamp_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, s.frame_cursor * 2u);
    apply_ready_replacements(s);
    record_pending_texture_uploads(s, frame);

    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(s.target_width),
                                  static_cast<float>(s.target_height), 0.0f, 1.0f};
    s.list->RSSetViewports(1u, &viewport);
    const D3D12_VERTEX_BUFFER_VIEW vb{
        frame.upload_buffer->GetGPUVirtualAddress(), static_cast<UINT>(vertex_bytes),
        static_cast<UINT>(sizeof(Dx12UploadVertex))};
    const D3D12_VERTEX_BUFFER_VIEW packed_vb{
        frame.upload_buffer->GetGPUVirtualAddress() + packed_offset,
        static_cast<UINT>(packed_bytes), 10u};
    D3D12_INDEX_BUFFER_VIEW ib{};
    if (index_bytes != 0u) {
        ib.BufferLocation = frame.upload_buffer->GetGPUVirtualAddress() + index_offset;
        ib.SizeInBytes = static_cast<UINT>(index_bytes);
        ib.Format = DXGI_FORMAT_R32_UINT;
        s.list->IASetIndexBuffer(&ib);
    }
    s.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s.list->SetGraphicsRootSignature(s.root_signature.Get());
    ID3D12DescriptorHeap *descriptor_heaps[]{s.srv_heap.Get(), s.sampler_heap.Get()};
    s.list->SetDescriptorHeaps(2u, descriptor_heaps);

    ID3D12PipelineState *active_pipeline = nullptr;
    std::uint64_t active_pipeline_key = std::numeric_limits<std::uint64_t>::max();
    Dx12FramebufferTarget *current_target = nullptr;
    std::uint32_t current_address = 0xFFFFFFFFu;
    Dx12DepthTarget *current_depth = nullptr;
    std::uint32_t current_depth_address = 0xFFFFFFFFu;
    std::uint32_t executed_batches = 0u;
    std::uint32_t bound_srv = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t bound_sampler = std::numeric_limits<std::uint32_t>::max();
    Dx12TransformConstants active_transform{};
    bool active_transform_valid = false;
    Dx12PixelConstants active_pixel{};
    bool active_pixel_valid = false;
    D3D12_RECT active_scissor{};
    bool active_scissor_valid = false;
    bool active_packed_0115 = false;
    bool active_vertex_layout_valid = false;
    D3D12_PRIMITIVE_TOPOLOGY active_topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    bool touched_display = false;
    constexpr float zero_color[4]{0.0f, 0.0f, 0.0f, 0.0f};
    // Chili: where this frame's post-process passes go (ge_gpu_post_dx12.hpp).
    Dx12Post &post = post_state();
    plan_scene_post(s, post);
    // Chili: where this frame's glow is drawn small (ge_gpu_glow_dx12.hpp), and
    // the size of the viewport the list has: a glow sheet has another.
    Dx12Glow &glow = glow_state();
    plan_glow(s, glow);
    std::uint32_t viewport_width = s.target_width;
    std::uint32_t viewport_height = s.target_height;
    for (const Dx12Batch &batch : s.batches) {
        if (post.next_event < post.events.size()) {
            // The surface a pass works on must hold everything drawn so far.
            if (run_due_post(s, post, static_cast<std::size_t>(&batch - s.batches.data()))) {
                if (current_target != nullptr) resolve_target_for_sampling(s, *current_target, false);
                s.list->RSSetViewports(1u, &viewport);
                viewport_width = s.target_width;
                viewport_height = s.target_height;
                // A pass changed a surface in place: an average of it is of the picture before.
                glow.averaged = kGlowNoSurface;
                s.list->SetGraphicsRootSignature(s.root_signature.Get());
                active_pipeline = nullptr;
                current_target = nullptr;
                current_address = 0xFFFFFFFFu;
                current_depth = nullptr;
                current_depth_address = 0xFFFFFFFFu;
                bound_srv = std::numeric_limits<std::uint32_t>::max();
                bound_sampler = std::numeric_limits<std::uint32_t>::max();
                active_transform_valid = false;
                active_pixel_valid = false;
                active_scissor_valid = false;
                active_topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
            }
        }
        const std::uint32_t address = batch.draw.framebuffer_address & kVramOffsetMask;
        // Chili: the glow. `sheet`: the draw goes to this small sheet in place
        // of its surface. `sampled_sheet`: it reads this one in place of the
        // surface its texture names. `glow_boxed`: it reads a full-size surface
        // averaged down to the sheet's size.
        Dx12GlowSheet *sheet = nullptr;
        Dx12GlowSheet *sampled_sheet = nullptr;
        std::uint64_t pipeline_variant = 0u;
        bool glow_boxed = false;
        if (glow.ready) {
            if (start_due_glow(s, glow, static_cast<std::size_t>(&batch - s.batches.data())))
                current_address = 0xFFFFFFFFu;
            sheet = glow_sheet(glow, address);
            // A draw into the surface the glow's source is the average of: the average is of the picture before.
            if (address == glow.averaged && sheet == nullptr) glow.averaged = kGlowNoSurface;
            if (sheet != nullptr && !glow_drawable(batch)) {
                // Not a sprite of the effect: the surface is drawn at full size again.
                sheet->active = false;
                sheet = nullptr;
                current_address = 0xFFFFFFFFu;
            }
            const bool samples_surface = batch.framebuffer_feedback && batch.draw.texture_enabled;
            if (samples_surface) sampled_sheet = glow_sheet(glow, batch.feedback_address);
            if (sampled_sheet != nullptr && sheet == nullptr) {
                // The draw puts a sheet onto another surface: the glow's last
                // sprite, which adds it to the picture.
                if (glow.off && batch.draw.blend_enabled && !batch.draw.clear_mode) {
                    ++glow.left_out;
                    continue;
                }
                if (glow.magnify) pipeline_variant |= kPipelineCubic;
                ++glow.readers;
            } else if (sheet != nullptr && sampled_sheet == nullptr && samples_surface) {
                // The draw copies a full-size surface into a sheet: it reads the
                // surface averaged down, which one pass makes for all the sprites of
                // a block.
                Dx12FramebufferTarget *from = find_framebuffer_target(s, batch.feedback_address);
                glow_boxed = from != nullptr && from->color;
                if (glow_boxed && !glow_averaged(glow, batch.feedback_address, batch)) {
                    if (current_target != nullptr) resolve_target_for_sampling(s, *current_target, false);
                    average_for_glow(s, glow, *from, batch.feedback_address, batch);
                    viewport_width = s.sheet_width;
                    viewport_height = s.sheet_height;
                    active_pipeline = nullptr;
                    current_target = nullptr;
                    current_address = 0xFFFFFFFFu;
                    current_depth = nullptr;
                    current_depth_address = 0xFFFFFFFFu;
                    bound_srv = std::numeric_limits<std::uint32_t>::max();
                    active_pixel_valid = false;
                    active_scissor_valid = false;
                    active_topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
                }
            }
            if (sheet != nullptr && s.sample_count > 1u) pipeline_variant |= kPipelineSingleSample;
        }
        Dx12FramebufferTarget *target = address == current_address ? current_target
            : sheet != nullptr ? &sheet->target : find_framebuffer_target(s, address);
        // A colour surface write never touches depth: it keeps the bound depth
        // buffer, or borrows any existing one, instead of creating its own.
        std::uint32_t depth_address = sheet != nullptr ? kGlowDepthAddress
                                                       : batch.draw.depth_address & kVramOffsetMask;
        if (batch.draw.surface_write == GeGpuSurfaceWrite::Color) {
            if (current_depth != nullptr && current_depth_address != kGlowDepthAddress)
                depth_address = current_depth_address;
            else if (!s.depth_targets.empty()) depth_address = s.depth_targets.begin()->first;
        }
        Dx12DepthTarget *depth = depth_address == current_depth_address ? current_depth
            : sheet != nullptr ? &glow.depth : ensure_depth_target(s, depth_address, error);
        if (target == nullptr || !target->color || depth == nullptr) {
            note_fallbacks(s, kGeGpuFallbackTargetUnavailable);
            if (depth == nullptr && !error.empty()) runtime_log_error("dx12 depth target", error);
            continue;
        }

        if (current_target != target || current_depth != depth) {
            if (current_target != nullptr && current_target != target)
                resolve_target_for_sampling(s, *current_target, false);
            prepare_target_for_render(s, *target);
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_cpu(s, target->rtv_index);
            const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_cpu(s, depth->dsv_index);
            s.list->OMSetRenderTargets(1u, &rtv, FALSE, &dsv);
            if (target->physical_width != viewport_width || target->physical_height != viewport_height) {
                viewport_width = target->physical_width;
                viewport_height = target->physical_height;
                const D3D12_VIEWPORT target_viewport{0.0f, 0.0f, static_cast<float>(viewport_width),
                                                     static_cast<float>(viewport_height), 0.0f, 1.0f};
                s.list->RSSetViewports(1u, &target_viewport);
            }

            // Chili: PSP EDRAM is persistent and so are these surfaces. A new
            // resource is zeroed once, like power-on VRAM; after that only the
            // game's own clear draws clear colour or depth. Upstream cleared
            // the displayed surface and every depth buffer once per interval.
            if (target->last_render_epoch == 0u)
                s.list->ClearRenderTargetView(rtv, zero_color, 0u, nullptr);
            target->last_render_epoch = s.frame_epoch;
            if (!depth->cleared) {
                s.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0u, 0u, nullptr);
                depth->cleared = true;
            }
            if (current_target != target) {
                bound_srv = std::numeric_limits<std::uint32_t>::max();
                bound_sampler = std::numeric_limits<std::uint32_t>::max();
            }
            current_target = target;
            current_address = address;
            current_depth = depth;
            current_depth_address = depth_address;
        }

        if (address == s.display_framebuffer) touched_display = true;

        const D3D12_PRIMITIVE_TOPOLOGY topology =
            batch.hardware_transform && batch.transform.primitive == 4u
                ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP
                : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        if (topology != active_topology) {
            s.list->IASetPrimitiveTopology(topology);
            active_topology = topology;
        }

        if (!active_vertex_layout_valid || active_packed_0115 != batch.packed_0115) {
            const D3D12_VERTEX_BUFFER_VIEW &active_vb = batch.packed_0115 ? packed_vb : vb;
            s.list->IASetVertexBuffers(0u, 1u, &active_vb);
            active_packed_0115 = batch.packed_0115;
            active_vertex_layout_valid = true;
        }

        const bool batch_cull = batch.hardware_transform && batch.transform.cull_enabled;
        const bool batch_accept_ccw = batch_cull && batch.transform.accept_counter_clockwise;
        const std::uint64_t batch_pipeline_key = pipeline_key(batch.draw) | pipeline_variant |
            (batch.packed_0115 ? (std::uint64_t{1} << 63u) : 0u) |
            (batch_cull ? (std::uint64_t{1} << 62u) : 0u) |
            (batch_accept_ccw ? (std::uint64_t{1} << 61u) : 0u);
        if (active_pipeline == nullptr || batch_pipeline_key != active_pipeline_key) {
            ID3D12PipelineState *pipeline = pipeline_for(
                s, batch.draw, batch.packed_0115, batch_cull, batch_accept_ccw, pipeline_variant, error);
            if (pipeline == nullptr) {
                runtime_log_error("dx12 ge pipeline", error);
                note_fallbacks(s, kGeGpuFallbackPipeline);
                continue;
            }
            if (pipeline != active_pipeline)
                s.list->SetPipelineState(pipeline);
            active_pipeline = pipeline;
            active_pipeline_key = batch_pipeline_key;
        }

        std::uint32_t srv_index = 0u;
        std::uint32_t sampler_index = 0u;
        // Hardware-transformed draws carry UVs normalised to the texture the
        // registers describe; a surface is sampled through its whole sheet.
        float feedback_u = 1.0f;
        float feedback_v = 1.0f;
        if (batch.draw.texture_enabled) {
            const std::uint32_t feedback_address = batch.feedback_address;
            Dx12FramebufferTarget *feedback = !batch.framebuffer_feedback ? nullptr
                : sampled_sheet != nullptr ? &sampled_sheet->target : find_framebuffer_target(s, feedback_address);
            if (feedback != nullptr && feedback->color) {
                sampler_index = ensure_sampler(s, batch.draw);
                if (feedback->logical_width != 0u && feedback->logical_height != 0u) {
                    feedback_u = static_cast<float>(batch.draw.texture_width) /
                                 static_cast<float>(feedback->logical_width);
                    feedback_v = static_cast<float>(batch.draw.texture_height) /
                                 static_cast<float>(feedback->logical_height);
                }
                if (feedback == current_target) {
                    // D3D12 cannot sample the target while it is bound for
                    // writing. Resolve MSAA first (if active), snapshot the
                    // single-sample image GPU->GPU, then resume rendering.
                    std::string feedback_error;
                    if (ensure_feedback_copy(s, *feedback, feedback_error)) {
                        // Chili: the copy is current outside `snapshot_dirty`, the
                        // rectangle drawn into since it was last brought up to date.
                        // Of that, a draw whose texture rectangle is known needs only
                        // the part it reads; the rest stays marked for a later draw.
                        D3D12_RECT need = feedback->snapshot_dirty;
                        bool all_copied = true;
                        if (!rect_empty(need) && batch.feedback_region_known &&
                            feedback->logical_width != 0u && feedback->logical_height != 0u) {
                            // Two physical pixels around it: a linear filter reads one beyond the edge.
                            const float scale_u = static_cast<float>(feedback->physical_width) /
                                                  static_cast<float>(feedback->logical_width);
                            const float scale_v = static_cast<float>(feedback->physical_height) /
                                                  static_cast<float>(feedback->logical_height);
                            const auto bound = [](float value, std::uint32_t limit) {
                                return static_cast<LONG>(std::clamp(value, 0.0f, static_cast<float>(limit)));
                            };
                            const D3D12_RECT read{
                                bound(std::floor(batch.feedback_region[0] * scale_u) - 2.0f, feedback->physical_width),
                                bound(std::floor(batch.feedback_region[1] * scale_v) - 2.0f, feedback->physical_height),
                                bound(std::ceil(batch.feedback_region[2] * scale_u) + 2.0f, feedback->physical_width),
                                bound(std::ceil(batch.feedback_region[3] * scale_v) + 2.0f, feedback->physical_height)};
                            if (!rect_empty(read)) {
                                const D3D12_RECT dirty = need;
                                need.left = std::max(dirty.left, read.left);
                                need.top = std::max(dirty.top, read.top);
                                need.right = std::min(dirty.right, read.right);
                                need.bottom = std::min(dirty.bottom, read.bottom);
                                all_copied = need.left == dirty.left && need.top == dirty.top &&
                                             need.right == dirty.right && need.bottom == dirty.bottom;
                            }
                        }
                        if (!rect_empty(need)) {
                            resolve_target_for_sampling(s, *feedback, false);
                            transition(s.list.Get(), feedback->color.Get(), feedback->color_state,
                                       D3D12_RESOURCE_STATE_COPY_SOURCE);
                            feedback->color_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
                            transition(s.list.Get(), feedback->feedback_copy.Get(), feedback->feedback_state,
                                       D3D12_RESOURCE_STATE_COPY_DEST);
                            feedback->feedback_state = D3D12_RESOURCE_STATE_COPY_DEST;
                            D3D12_TEXTURE_COPY_LOCATION copy_to{};
                            copy_to.pResource = feedback->feedback_copy.Get();
                            copy_to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                            D3D12_TEXTURE_COPY_LOCATION copy_from{};
                            copy_from.pResource = feedback->color.Get();
                            copy_from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                            const D3D12_BOX box{static_cast<UINT>(need.left), static_cast<UINT>(need.top), 0u,
                                                static_cast<UINT>(need.right), static_cast<UINT>(need.bottom), 1u};
                            s.list->CopyTextureRegion(&copy_to, box.left, box.top, 0u, &copy_from, &box);
                            if (all_copied) feedback->snapshot_dirty = {};
                            transition(s.list.Get(), feedback->feedback_copy.Get(), feedback->feedback_state,
                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                            feedback->feedback_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                            transition(s.list.Get(), feedback->color.Get(), feedback->color_state,
                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                            feedback->color_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                            prepare_target_for_render(s, *feedback);
                            const D3D12_CPU_DESCRIPTOR_HANDLE self_rtv = rtv_cpu(s, feedback->rtv_index);
                            const D3D12_CPU_DESCRIPTOR_HANDLE self_dsv = dsv_cpu(s, depth->dsv_index);
                            s.list->OMSetRenderTargets(1u, &self_rtv, FALSE, &self_dsv);
                        }
                        srv_index = feedback->feedback_srv_index;
                        ++s.report.vram_feedback_refreshes;
                        ++s.report.dx12_gpu_feedback_draws;
                        ++s.report.dx12_self_feedback_snapshots;
                    } else if (!feedback_error.empty()) {
                        runtime_log_error("dx12 self-feedback", feedback_error);
                    }
                } else if (glow_boxed) {
                    srv_index = glow.source.srv_index;
                    ++s.report.vram_feedback_refreshes;
                    ++s.report.dx12_gpu_feedback_draws;
                } else {
                    resolve_target_for_sampling(s, *feedback, false);
                    srv_index = feedback->srv_index;
                    ++s.report.vram_feedback_refreshes;
                    ++s.report.dx12_gpu_feedback_draws;
                }
                if (feedback_address == s.display_framebuffer)
                    ++s.report.display_framebuffer_sampled_draws;
            } else {
                if (Dx12Texture *texture = find_cached_texture(s, texture_key(batch.draw));
                    texture != nullptr && texture->image) {
                    srv_index = texture->srv_index;
                    sampler_index = batch.draw.through ? texture->sampler_index
                                                       : texture->scene_sampler_index;
                }
            }
        }
        if (srv_index != bound_srv) {
            s.list->SetGraphicsRootDescriptorTable(0u, srv_gpu(s, srv_index));
            bound_srv = srv_index;
        }
        if (sampler_index != bound_sampler) {
            s.list->SetGraphicsRootDescriptorTable(1u, sampler_gpu(s, sampler_index));
            bound_sampler = sampler_index;
        }

        const std::uint32_t logical_width = current_target->logical_width != 0u
            ? current_target->logical_width : s.sheet_width;
        const std::uint32_t logical_height = current_target->logical_height != 0u
            ? current_target->logical_height : s.sheet_height;
        Dx12TransformConstants draw_transform =
            make_transform_constants(batch, logical_width, logical_height,
                                     s.target_width == s.sheet_width);
        if (batch.hardware_transform) {
            draw_transform.uv[0] *= feedback_u;
            draw_transform.uv[1] *= feedback_v;
            draw_transform.uv[2] *= feedback_u;
            draw_transform.uv[3] *= feedback_v;
        }
        if (!active_transform_valid ||
            std::memcmp(&draw_transform, &active_transform, sizeof(draw_transform)) != 0) {
            s.list->SetGraphicsRoot32BitConstants(
                2u, 40u, &draw_transform, 0u);
            active_transform = draw_transform;
            active_transform_valid = true;
        }

        const Dx12PixelConstants pixel_state = make_pixel_constants(batch.draw, srv_index != 0u);
        if (!active_pixel_valid ||
            std::memcmp(&pixel_state, &active_pixel, sizeof(pixel_state)) != 0) {
            s.list->SetGraphicsRoot32BitConstants(3u, kPixelConstantWords, &pixel_state, 0u);
            active_pixel = pixel_state;
            active_pixel_valid = true;
        }

        const auto scale_x = [&](std::int32_t value) {
            return static_cast<LONG>(std::clamp<std::int64_t>(
                static_cast<std::int64_t>(value) * current_target->physical_width /
                    std::max<std::uint32_t>(1u, logical_width),
                0, static_cast<std::int64_t>(current_target->physical_width)));
        };
        const auto scale_y = [&](std::int32_t value) {
            return static_cast<LONG>(std::clamp<std::int64_t>(
                static_cast<std::int64_t>(value) * current_target->physical_height /
                    std::max<std::uint32_t>(1u, logical_height),
                0, static_cast<std::int64_t>(current_target->physical_height)));
        };
        D3D12_RECT scissor{
            scale_x(batch.draw.scissor_x0), scale_y(batch.draw.scissor_y0),
            scale_x(batch.draw.scissor_x1 + 1), scale_y(batch.draw.scissor_y1 + 1)};
        if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) {
            note_fallbacks(s, kGeGpuFallbackEmptyScissor);
            continue;
        }
        if (!active_scissor_valid ||
            std::memcmp(&scissor, &active_scissor, sizeof(scissor)) != 0) {
            s.list->RSSetScissorRects(1u, &scissor);
            active_scissor = scissor;
            active_scissor_valid = true;
        }
        note_target_write(*current_target, scissor);
        if (batch.indexed)
            s.list->DrawIndexedInstanced(batch.index_count, 1u, batch.first_index,
                                         static_cast<INT>(batch.first_vertex), 0u);
        else
            s.list->DrawInstanced(batch.vertex_count, 1u, batch.first_vertex, 0u);
        ++executed_batches;

        if (batch.draw.surface_write != GeGpuSurfaceWrite::None) continue;
        if (batch.draw.depth_test_enabled) s.report.depth_tested_game_draw_calls += batch.logical_draw_count;
        if (batch.draw.depth_write_enabled) s.report.depth_writing_game_draw_calls += batch.logical_draw_count;
        if (batch.draw.alpha_test_enabled) s.report.alpha_tested_game_draw_calls += batch.logical_draw_count;
        if (batch.draw.blend_enabled && !batch.draw.clear_mode) {
            const std::uint32_t mode = (batch.draw.blend_equation & 7u) << 8u |
                (batch.draw.blend_dest_factor & 0xFu) << 4u | (batch.draw.blend_source_factor & 0xFu);
            if (mode == 0x032u) s.report.standard_alpha_blended_game_draw_calls += batch.logical_draw_count;
            else if (mode == 0x0AAu) s.report.additive_blended_game_draw_calls += batch.logical_draw_count;
            if (resolve_blend(batch.draw).fallbacks != 0u)
                s.report.unsupported_blend_game_draw_calls += batch.logical_draw_count;
        }
        if (batch.draw.fog_enabled) s.report.fogged_game_draw_calls += batch.logical_draw_count;
        if (srv_index != 0u) {
            const std::uint32_t submitted_vertices = batch.indexed ? batch.index_count : batch.vertex_count;
            s.report.textured_game_triangles +=
                batch.hardware_transform && batch.transform.primitive == 4u
                    ? (submitted_vertices > 2u ? submitted_vertices - 2u : 0u)
                    : submitted_vertices / 3u;
            s.report.textured_game_vertices += submitted_vertices;
            switch (batch.draw.texture_function & 7u) {
            case 0u: s.report.modulate_texture_game_draw_calls += batch.logical_draw_count; break;
            case 1u: s.report.decal_texture_game_draw_calls += batch.logical_draw_count; break;
            case 2u: s.report.blend_texture_game_draw_calls += batch.logical_draw_count; break;
            case 3u: s.report.replace_texture_game_draw_calls += batch.logical_draw_count; break;
            case 4u: s.report.add_texture_game_draw_calls += batch.logical_draw_count; break;
            default: ++s.report.unsupported_texture_function_game_draw_calls; break;
            }
            if (batch.draw.texture_double_color)
                s.report.double_color_texture_game_draw_calls += batch.logical_draw_count;
            if (batch.draw.texture_mipmap_enabled) {
                s.report.mipmapped_game_draw_calls += batch.logical_draw_count;
                if (batch.draw.texture_mipmap_linear)
                    s.report.mip_linear_game_draw_calls += batch.logical_draw_count;
            }
        }
    }

    if (current_target != nullptr)
        resolve_target_for_sampling(s, *current_target, false);

    // Chili: a flush consumes the draws, so what it did to the displayed surface
    // is remembered until a present shows it.
    if (presenting) {
        touched_display = touched_display || s.display_pending;
        s.display_pending = false;
    } else if (touched_display) {
        s.display_pending = true;
    }

    if (executed_batches == 0u && !(presenting && touched_display)) {
        hr = s.list->Close();
        if (SUCCEEDED(hr)) {
            ID3D12CommandList *upload_lists[]{s.list.Get()};
            s.queue->ExecuteCommandLists(1u, upload_lists);
            frame.fence_value = s.next_fence++;
            hr = s.queue->Signal(s.fence.Get(), frame.fence_value);
            if (FAILED(hr))
                runtime_log_error("dx12 ge", hr_text(hr, "ID3D12CommandQueue::Signal(upload-only)"));
            s.frame_cursor = (s.frame_cursor + 1u) % kFrameCount;
        } else {
            runtime_log_error("dx12 ge", hr_text(hr, "CommandList::Close(upload-only)"));
        }
        clear_accumulation(s);
        if (presenting) ++s.frame_epoch;
        return false;
    }

    Dx12FramebufferTarget *display_target = find_framebuffer_target(s, s.display_framebuffer);
    const bool display_ready = presenting && touched_display &&
        display_target != nullptr && display_target->color;
    if (presenting && !display_ready) {
        ++s.report.frames_without_displayed_target;
        if (++s.missed_display_intervals > 4u) {
            s.direct_present_ok = false;
            s.presented_framebuffer = 0u;
        }
    }

    if (s.readback_enabled && s.readback_buffer && display_ready) {
        transition(s.list.Get(), display_target->color.Get(), display_target->color_state,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        display_target->color_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = display_target->color.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = s.readback_buffer.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = s.readback_footprint;
        s.list->CopyTextureRegion(&dst, 0u, 0u, 0u, &src, nullptr);
        transition(s.list.Get(), display_target->color.Get(), display_target->color_state,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        display_target->color_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    bool recorded_present = false;
    if (direct_possible && display_ready) {
        std::string present_error;
        recorded_present = record_direct_present(s, *display_target, present_error);
        if (!recorded_present && !present_error.empty())
            runtime_log_error("dx12 ge direct present", present_error);
    }

    if (s.timestamp_heap) {
        s.list->EndQuery(s.timestamp_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, s.frame_cursor * 2u + 1u);
        s.list->ResolveQueryData(s.timestamp_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, s.frame_cursor * 2u, 2u,
                                 s.timestamp_readback.Get(), static_cast<UINT64>(s.frame_cursor) * 2u * sizeof(UINT64));
        frame.timed = true;
    }
    hr = s.list->Close();
    if (FAILED(hr)) {
        runtime_log_error("dx12 ge", hr_text(hr, "CommandList::Close"));
        clear_accumulation(s);
        return false;
    }
    ID3D12CommandList *lists[]{s.list.Get()};
    s.queue->ExecuteCommandLists(1u, lists);

    bool presented = false;
    if (recorded_present && s.swapchain) {
        // Tearing is only legal with sync interval 0.
        const UINT interval = std::min<std::uint32_t>(ge_gpu_config().present_sync_interval, 4u);
        {
            chili::CostTimer cost(chili::frame_costs.present_ns);
            hr = s.swapchain->Present(interval, interval == 0u && s.swapchain_tearing
                                                    ? DXGI_PRESENT_ALLOW_TEARING : 0u);
        }
        if (SUCCEEDED(hr)) {
            presented = true;
            s.direct_present_ok = true;
            s.presented_framebuffer = s.display_framebuffer;
            s.missed_display_intervals = 0u;
            s.report.gpu_frame_presented_to_window = true;
        } else {
            runtime_log_error("dx12 ge present", hr_text(hr, "IDXGISwapChain::Present"));
            s.direct_present_ok = false;
            s.presented_framebuffer = 0u;
        }
    }

    frame.fence_value = s.next_fence++;
    hr = s.queue->Signal(s.fence.Get(), frame.fence_value);
    if (FAILED(hr)) {
        runtime_log_error("dx12 ge", hr_text(hr, "ID3D12CommandQueue::Signal(frame)"));
        clear_accumulation(s);
        return false;
    }
    // Chili: measuring run only. How long the card takes for the frame just
    // submitted, by waiting for it.
    if (ge_gpu_config().gpu_timing_diag) {
        LARGE_INTEGER before{}, after{}, frequency{};
        QueryPerformanceCounter(&before);
        (void)wait_for_fence(s, frame.fence_value, error);
        QueryPerformanceCounter(&after);
        QueryPerformanceFrequency(&frequency);
        s.report.perf_finish_frame_ns += static_cast<std::uint64_t>(
            (after.QuadPart - before.QuadPart) * 1000000000ll / std::max<LONGLONG>(1, frequency.QuadPart));
        ++s.report.perf_finish_frame_calls;
    }

    if (s.readback_enabled && s.readback_buffer && display_ready) {
        if (!wait_for_fence(s, frame.fence_value, error)) {
            runtime_log_error("dx12 ge readback wait", error);
        } else {
            void *mapped = nullptr;
            const D3D12_RANGE read_range{0u, static_cast<SIZE_T>(s.readback_bytes)};
            hr = s.readback_buffer->Map(0u, &read_range, &mapped);
            if (SUCCEEDED(hr) && mapped != nullptr) {
                const auto *source = static_cast<const std::byte *>(mapped) + s.readback_footprint.Offset;
                const std::size_t row_bytes = static_cast<std::size_t>(s.target_width) * 4u;
                for (std::uint32_t y = 0u; y < s.target_height; ++y)
                    std::memcpy(s.frame_rgba.data() + static_cast<std::size_t>(y) * row_bytes,
                                source + static_cast<std::size_t>(y) * s.readback_footprint.Footprint.RowPitch,
                                row_bytes);
                const D3D12_RANGE no_write{0u, 0u};
                s.readback_buffer->Unmap(0u, &no_write);
            }
        }
    }

    s.report.dx12_fxaa_passes = post.fxaa_passes;
    s.report.dx12_ao_passes = post.ao_passes;
    s.report.dx12_glow_blocks = glow.blocks;
    s.report.dx12_glow_readers = glow.readers;
    s.report.dx12_glow_left_out = glow.left_out;
    ++s.report.transfer_submissions;
    s.report.transfer_bytes += bytes;
    s.report.swapchain_active = s.swapchain != nullptr;
    clear_accumulation(s);
    s.frame_cursor = (s.frame_cursor + 1u) % kFrameCount;
    if (!presenting) return true;

    ++s.report.game_frames;
    s.report.game_frame_vblank = vblank;
    s.report.game_frame_readback_bytes = (s.readback_enabled && display_ready) ? s.frame_rgba.size() : 0u;
    s.report.presented_framebuffer_target = display_ready ? s.display_framebuffer : 0u;
    s.report.release_candidate_ready = s.enabled && display_ready &&
        (presented || s.readback_enabled) && s.report.transfer_self_test_passed &&
        s.report.offscreen_self_test_passed && s.report.texture_descriptor_layout_created &&
        s.report.depth_attachment_active && s.report.alpha_test_shader_active &&
        s.report.observed_blend_modes_pipeline_active && s.report.fog_shader_active;
    ++s.frame_epoch;
    return presented || (s.readback_enabled && display_ready);
}

// Chili: a submission, and after it, with nothing accumulated, the textures
// whose overlay changed leave the cache (evict_changed_overlays).
bool submit_frame(Dx12GeState &s, std::uint64_t vblank, SubmitMode mode) noexcept {
    const bool submitted = submit_accumulated(s, vblank, mode);
    evict_changed_overlays(s);
    return submitted;
}

// Queues a full-sheet textured rectangle that replaces a surface's colour, or
// its depth, with CPU-supplied pixels. It goes through the ordinary texture
// cache and batch stream, so it executes in order with the game's draws.
bool queue_surface_write(Dx12GeState &s, GeGpuSurfaceWrite kind,
                         std::uint32_t color_address, std::uint32_t depth_address,
                         std::uint32_t width, std::uint32_t height,
                         std::vector<std::byte> rgba8, bool smooth) noexcept {
    GeGpuDrawDescriptor draw{};
    draw.surface_write = kind;
    draw.primitive = 6u;
    draw.vertex_count = 2u;
    draw.framebuffer_address = 0x04000000u | (color_address & kVramOffsetMask);
    draw.framebuffer_stride = s.sheet_width;
    draw.framebuffer_format = 3u;
    draw.depth_address = 0x04000000u | (depth_address & kVramOffsetMask);
    draw.texture_enabled = true;
    draw.texture_format = 3u;
    draw.texture_address = 0u;  // not a VRAM address, so never mistaken for feedback
    draw.texture_width = width;
    draw.texture_height = height;
    draw.texture_buffer_width = width;
    draw.texture_function = 3u;
    draw.texture_use_alpha = true;
    draw.texture_clamp_u = true;
    draw.texture_clamp_v = true;
    // Depth must stay exact per texel; colour may be smoothed when upscaled.
    const bool linear = smooth && kind == GeGpuSurfaceWrite::Color &&
                        s.target_width != s.sheet_width;
    draw.texture_min_linear = linear;
    draw.texture_mag_linear = linear;
    draw.texture_linear = linear;
    draw.through = true;
    draw.scissor_x0 = 0;
    draw.scissor_y0 = 0;
    draw.scissor_x1 = static_cast<std::int32_t>(s.sheet_width) - 1;
    draw.scissor_y1 = static_cast<std::int32_t>(s.sheet_height) - 1;
    // One cache entry per write of the frame. A write that repeats every frame
    // (a movie) therefore reuses its key and replaces the previous image.
    const std::uint64_t key = hash_mix(hash_mix(0x4368696C69537572ull,
        (static_cast<std::uint64_t>(kind) << 32u) | (linear ? 1u : 0u)), s.surface_write_serial++);
    draw.texture_cache_key_hint = key;
    draw.texture_image_key_hint = key;
    if (!prepare_texture_upload(s, draw, width, height, 1u, std::move(rgba8))) return false;

    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    const auto vertex = [](float x, float y) {
        GeGpuVertex out{};
        out.x = x;
        out.y = y;
        out.u = x;
        out.v = y;
        return out;
    };
    const std::array<GeGpuVertex, 6> quad{{
        vertex(0.0f, 0.0f), vertex(w, 0.0f), vertex(w, h),
        vertex(0.0f, 0.0f), vertex(w, h), vertex(0.0f, h)}};
    const std::size_t before = s.batches.size();
    ge_gpu_backend_accumulate_color_triangles(draw, quad);
    return s.batches.size() != before;
}

} // namespace

bool ge_gpu_backend_finish_color_frame(std::uint64_t vblank) noexcept {
    return submit_frame(state(), vblank, SubmitMode::Present);
}

bool ge_gpu_backend_flush() noexcept {
    return submit_frame(state(), 0u, SubmitMode::Flush);
}

bool ge_gpu_backend_write_surface_rgba(std::uint32_t address, std::uint32_t width,
                                       std::uint32_t height,
                                       std::span<const std::byte> rgba8, bool smooth) noexcept {
    Dx12GeState &s = state();
    if (!s.enabled || width == 0u || height == 0u ||
        width > s.sheet_width || height > s.sheet_height ||
        rgba8.size() != static_cast<std::size_t>(width) * height * 4u) return false;
    try {
        std::string error;
        if (!ensure_framebuffer_target(s, address, error)) {
            if (!error.empty()) runtime_log_error("dx12 surface write", error);
            return false;
        }
        // The colour write binds whichever depth buffer was used last with this
        // surface; it never touches it. Address 0 is as good as any here.
        return queue_surface_write(s, GeGpuSurfaceWrite::Color, address, 0u, width, height,
                                   std::vector<std::byte>(rgba8.begin(), rgba8.end()), smooth);
    } catch (...) {
        return false;
    }
}

bool ge_gpu_backend_write_surface_depth16(std::uint32_t depth_address, std::uint32_t color_address,
                                          std::uint32_t width, std::uint32_t height,
                                          std::span<const std::uint16_t> depth) noexcept {
    Dx12GeState &s = state();
    if (!s.enabled || width == 0u || height == 0u ||
        width > s.sheet_width || height > s.sheet_height ||
        depth.size() != static_cast<std::size_t>(width) * height) return false;
    try {
        std::string error;
        if (!ensure_framebuffer_target(s, color_address, error) ||
            ensure_depth_target(s, depth_address, error) == nullptr) {
            if (!error.empty()) runtime_log_error("dx12 surface depth write", error);
            return false;
        }
        std::vector<std::byte> packed(depth.size() * 4u);
        for (std::size_t i = 0u; i < depth.size(); ++i) {
            packed[i * 4u + 0u] = static_cast<std::byte>(depth[i] & 0xFFu);
            packed[i * 4u + 1u] = static_cast<std::byte>(depth[i] >> 8u);
            packed[i * 4u + 2u] = std::byte{0};
            packed[i * 4u + 3u] = std::byte{0xFF};
        }
        return queue_surface_write(s, GeGpuSurfaceWrite::Depth, color_address, depth_address,
                                   width, height, std::move(packed), false);
    } catch (...) {
        return false;
    }
}

bool ge_gpu_backend_read_surface_rgba(std::uint32_t address, std::vector<std::byte> &rgba8,
                                      std::uint32_t &width, std::uint32_t &height) noexcept {
    Dx12GeState &s = state();
    if (!s.enabled || !s.device) return false;
    (void)submit_frame(s, 0u, SubmitMode::Flush);
    Dx12FramebufferTarget *target = find_framebuffer_target(s, address);
    if (target == nullptr || !target->color) return false;
    std::string error;
    if (!wait_for_gpu(s, error)) {
        runtime_log_error("dx12 surface read", error);
        return false;
    }

    const D3D12_RESOURCE_DESC desc = target->color->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0u;
    UINT64 row_size = 0u;
    UINT64 total = 0u;
    s.device->GetCopyableFootprints(&desc, 0u, 1u, 0u, &footprint, &rows, &row_size, &total);
    if (!s.surface_readback_buffer || s.surface_readback_bytes < total) {
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = std::max<UINT64>(total, 256u);
        buffer.Height = 1u;
        buffer.DepthOrArraySize = 1u;
        buffer.MipLevels = 1u;
        buffer.Format = DXGI_FORMAT_UNKNOWN;
        buffer.SampleDesc.Count = 1u;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        s.surface_readback_buffer.Reset();
        const HRESULT create_hr = s.device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&s.surface_readback_buffer));
        if (FAILED(create_hr)) {
            runtime_log_error("dx12 surface read", hr_text(create_hr, "CreateCommittedResource(readback)"));
            s.surface_readback_bytes = 0u;
            return false;
        }
        s.surface_readback_bytes = buffer.Width;
    }

    HRESULT hr = s.texture_allocator->Reset();
    if (SUCCEEDED(hr)) hr = s.texture_list->Reset(s.texture_allocator.Get(), nullptr);
    if (FAILED(hr)) {
        runtime_log_error("dx12 surface read", hr_text(hr, "CommandList::Reset"));
        return false;
    }
    // After a submission every surface's single-sample image is resolved and
    // shader-readable, MSAA or not.
    transition(s.texture_list.Get(), target->color.Get(), target->color_state,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = target->color.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = s.surface_readback_buffer.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    s.texture_list->CopyTextureRegion(&dst, 0u, 0u, 0u, &src, nullptr);
    transition(s.texture_list.Get(), target->color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
               target->color_state);
    hr = s.texture_list->Close();
    if (FAILED(hr)) {
        runtime_log_error("dx12 surface read", hr_text(hr, "CommandList::Close"));
        return false;
    }
    ID3D12CommandList *lists[]{s.texture_list.Get()};
    s.queue->ExecuteCommandLists(1u, lists);
    if (!wait_for_gpu(s, error)) {
        runtime_log_error("dx12 surface read", error);
        return false;
    }

    void *mapped = nullptr;
    const D3D12_RANGE read_range{0u, static_cast<SIZE_T>(total)};
    hr = s.surface_readback_buffer->Map(0u, &read_range, &mapped);
    if (FAILED(hr) || mapped == nullptr) {
        runtime_log_error("dx12 surface read", hr_text(hr, "Map(readback)"));
        return false;
    }
    width = static_cast<std::uint32_t>(desc.Width);
    height = desc.Height;
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4u;
    try {
        rgba8.resize(row_bytes * height);
    } catch (...) {
        const D3D12_RANGE none{0u, 0u};
        s.surface_readback_buffer->Unmap(0u, &none);
        return false;
    }
    const auto *source = static_cast<const std::byte *>(mapped) + footprint.Offset;
    for (std::uint32_t y = 0u; y < height; ++y)
        std::memcpy(rgba8.data() + static_cast<std::size_t>(y) * row_bytes,
                    source + static_cast<std::size_t>(y) * footprint.Footprint.RowPitch, row_bytes);
    const D3D12_RANGE no_write{0u, 0u};
    s.surface_readback_buffer->Unmap(0u, &no_write);
    return true;
}

// ---- Chili: the overlay pass (ge_gpu_overlay_dx12.hpp) ---------------------

void ge_gpu_backend_set_overlay(std::shared_ptr<const GeGpuOverlay> overlay) noexcept {
    Dx12OverlayShared &shared = overlay_shared();
    std::shared_ptr<const GeGpuOverlay> previous;   // let go of outside the lock
    {
        const std::lock_guard<std::mutex> lock(shared.mutex);
        previous = std::move(shared.list);
        shared.list = std::move(overlay);
        shared.set.store(shared.list != nullptr, std::memory_order_release);
    }
}

bool ge_gpu_backend_overlay_set() noexcept {
    return overlay_shared().set.load(std::memory_order_acquire);
}

void ge_gpu_backend_watch_overlay_textures(std::span<const GeGpuOverlayTexture> textures) {
    Dx12OverlayShared &shared = overlay_shared();
    shared.watched_count.store(0u, std::memory_order_release);
    const std::uint32_t count = static_cast<std::uint32_t>(std::min(textures.size(), shared.watched.size()));
    std::array<GeTextureOverlayKey, kOverlayTextureCapacity> keys{};
    for (std::uint32_t i = 0u; i < count; ++i) {
        shared.watched[i] = textures[i].key;
        shared.cached[i].store(0, std::memory_order_relaxed);
        keys[i] = {textures[i].key, textures[i].format, textures[i].width, textures[i].height};
    }
    shared.watched_count.store(count, std::memory_order_release);
    ge_texture_mods_watch_keys(std::span<const GeTextureOverlayKey>(keys.data(), count));
}

bool ge_gpu_backend_overlay_texture_ready(std::uint64_t key) noexcept {
    const Dx12OverlayShared &shared = overlay_shared();
    const std::uint32_t count = shared.watched_count.load(std::memory_order_acquire);
    for (std::uint32_t i = 0u; i < count; ++i)
        if (shared.watched[i] == key) return shared.cached[i].load(std::memory_order_relaxed) > 0;
    return false;
}

bool ge_gpu_backend_read_display_with_overlay(std::uint32_t address, std::vector<std::byte> &rgba8,
                                              std::uint32_t &width, std::uint32_t &height) noexcept {
    Dx12GeState &s = state();
    if (!s.enabled || !s.device) return false;
    (void)submit_frame(s, 0u, SubmitMode::Flush);
    Dx12FramebufferTarget *target = find_framebuffer_target(s, address);
    if (target == nullptr || !target->color) return false;
    std::string error;
    if (!wait_for_gpu(s, error)) {
        runtime_log_error("dx12 overlay", error);
        return false;
    }
    return overlay_read_display(s, *target, rgba8, width, height);
}

void ge_gpu_backend_request_window_capture() noexcept {
    Dx12Overlay &overlay = overlay_state();
    overlay.window_requested = true;
    overlay.window_recorded = false;
}

bool ge_gpu_backend_take_window_capture(GeGpuWindowCapture &capture) noexcept {
    Dx12GeState &s = state();
    Dx12Overlay &overlay = overlay_state();
    if (!s.enabled || !overlay.window_recorded || !overlay.window_readback) return false;
    std::string error;
    if (!wait_for_gpu(s, error)) {
        runtime_log_error("dx12 overlay", error);
        return false;
    }
    overlay.window_recorded = false;
    capture.width = overlay.window_footprint.Footprint.Width;
    capture.height = overlay.window_footprint.Footprint.Height;
    capture.picture_x = overlay.window_picture.x;
    capture.picture_y = overlay.window_picture.y;
    capture.picture_width = overlay.window_picture.width;
    capture.picture_height = overlay.window_picture.height;
    return overlay_read_rows(overlay.window_readback.Get(), overlay.window_footprint, overlay.window_readback_bytes,
                             capture.width, capture.height, capture.rgba8);
}

GeGpuOverlayStats ge_gpu_backend_overlay_stats() noexcept {
    return overlay_state().stats;
}

GeGpuTimeStats ge_gpu_backend_gpu_time() noexcept {
    const Dx12GeState &s = state();
    GeGpuTimeStats out;
    out.submissions = s.gpu_time_count;
    if (s.gpu_time_count == 0u) return out;
    out.mean_ms = s.gpu_time_total_ms / static_cast<double>(s.gpu_time_count);
    out.max_ms = s.gpu_time_max_ms;
    const auto rank = [&](double share) {
        const std::uint64_t wanted = static_cast<std::uint64_t>(share * static_cast<double>(s.gpu_time_count));
        std::uint64_t seen = 0u;
        for (std::size_t bin = 0u; bin < s.gpu_time_bins.size(); ++bin) {
            seen += s.gpu_time_bins[bin];
            if (seen > wanted) return static_cast<double>(bin + 1u) * 0.05;
        }
        return static_cast<double>(s.gpu_time_bins.size()) * 0.05;
    };
    out.p50_ms = rank(0.5);
    out.p99_ms = rank(0.99);
    return out;
}

std::uint32_t ge_gpu_backend_take_fallbacks() noexcept {
    Dx12GeState &s = state();
    const std::uint32_t fallbacks = s.pending_fallbacks;
    s.pending_fallbacks = 0u;
    return fallbacks;
}

std::string ge_gpu_fallback_names(std::uint32_t fallbacks) {
    static constexpr struct { std::uint32_t bit; const char *name; } kNames[] = {
        {kGeGpuFallbackBlendAbs, "blend equation ABS drawn unblended"},
        {kGeGpuFallbackBlendDoubleAlpha, "doubled alpha blend factor saturated at 1"},
        {kGeGpuFallbackStencil, "stencil test ignored"},
        {kGeGpuFallbackColorTest, "colour test ignored"},
        {kGeGpuFallbackLogicOp, "logic op ignored"},
        {kGeGpuFallbackPartialColorMask, "per-bit colour mask written per channel"},
        {kGeGpuFallbackTextureMissing, "texture unavailable, flat colour drawn"},
        {kGeGpuFallbackGeometryOverflow, "geometry buffer full, draw dropped"},
        {kGeGpuFallbackTargetUnavailable, "render target unavailable, draw dropped"},
        {kGeGpuFallbackPipeline, "pipeline state failed, draw dropped"},
        {kGeGpuFallbackEmptyScissor, "empty scissor, draw dropped"},
        {kGeGpuFallbackFramebufferFormat, "16-bit framebuffer drawn into an 8888 surface"},
    };
    std::string out;
    for (const auto &entry : kNames) {
        if ((fallbacks & entry.bit) == 0u) continue;
        if (!out.empty()) out += "; ";
        out += entry.name;
    }
    return out;
}

std::string ge_gpu_backend_adapter_name() {
    const Dx12GeState &s = state();
    return s.enabled ? s.adapter_name : std::string{};
}

bool ge_gpu_backend_copy_game_frame_rgba(std::span<std::byte> destination) noexcept {
    const Dx12GeState &s = state();
    if (s.frame_rgba.empty() || destination.size() < s.frame_rgba.size()) return false;
    std::memcpy(destination.data(), s.frame_rgba.data(), s.frame_rgba.size());
    return true;
}

bool ge_gpu_backend_presents_directly() noexcept {
    const Dx12GeState &s = state();
    return s.enabled && s.swapchain != nullptr && s.direct_present_ok;
}
std::uint32_t ge_gpu_backend_owned_framebuffer() noexcept {
    const Dx12GeState &s = state();
    return s.enabled && s.swapchain != nullptr && s.direct_present_ok
        ? s.presented_framebuffer : 0u;
}
std::uint32_t ge_gpu_backend_display_framebuffer() noexcept { return state().display_framebuffer; }
std::span<const std::byte> ge_gpu_backend_game_frame_rgba() noexcept {
    const Dx12GeState &s = state();
    return s.frame_rgba.empty() ? std::span<const std::byte>{}
                                : std::span<const std::byte>(s.frame_rgba.data(), s.frame_rgba.size());
}
bool ge_gpu_backend_copy_offscreen_rgba(std::span<std::byte> destination) noexcept {
    return ge_gpu_backend_copy_game_frame_rgba(destination);
}
void ge_gpu_backend_mark_window_presented() noexcept { state().report.gpu_frame_presented_to_window = true; }
GeGpuBackendReport ge_gpu_backend_report() { return state().report; }

const char *ge_gpu_backend_name(GeGpuBackendKind kind) noexcept {
    switch (kind) {
    case GeGpuBackendKind::Software: return "software";
    case GeGpuBackendKind::DirectX12: return "directx12";
    }
    return "unknown";
}

} // namespace vcs
