#include "ge_gpu_config.hpp"
#include "ge_gpu_log.hpp"

#include "../host_log.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace vcs {

namespace {

constexpr unsigned kErrorsPerCategory = 8u;

std::atomic<GeGpuLogSink> g_sink{nullptr};
std::mutex g_log_mutex;
std::unordered_map<std::string, unsigned> g_error_counts;
std::unordered_set<std::string> g_once;

void default_sink(bool error, std::string_view category, std::string_view message) {
    std::FILE *out = error ? stderr : stdout;
    if (category.empty())
        std::fprintf(out, "[gpu] %.*s\n", static_cast<int>(message.size()), message.data());
    else
        std::fprintf(out, "[gpu] %.*s: %.*s\n", static_cast<int>(category.size()), category.data(),
                     static_cast<int>(message.size()), message.data());
    std::fflush(out);
}

void emit(bool error, std::string_view category, std::string_view message) {
    const GeGpuLogSink sink = g_sink.load(std::memory_order_acquire);
    (sink != nullptr ? sink : &default_sink)(error, category, message);
}

bool env_flag(const char *name, bool fallback) noexcept {
    const char *text = std::getenv(name);
    if (text == nullptr || *text == '\0') return fallback;
    return std::strcmp(text, "0") != 0 && std::strcmp(text, "false") != 0 &&
           std::strcmp(text, "FALSE") != 0 && std::strcmp(text, "off") != 0 &&
           std::strcmp(text, "OFF") != 0;
}

std::uint32_t env_number(const char *name, std::uint32_t fallback,
                         std::uint32_t minimum, std::uint32_t maximum) noexcept {
    const char *text = std::getenv(name);
    if (text == nullptr || *text == '\0') return fallback;
    char *end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0' || value < minimum || value > maximum) return fallback;
    return static_cast<std::uint32_t>(value);
}

} // namespace

void set_ge_gpu_log_sink(GeGpuLogSink sink) noexcept {
    g_sink.store(sink, std::memory_order_release);
}

void runtime_log_line(std::string_view line) {
    if (!chili::log_enabled(chili::LogChannel::Ge)) return;
    emit(false, {}, line);
}

void runtime_log_error(std::string_view category, std::string_view message) {
    unsigned count = 0u;
    {
        const std::lock_guard<std::mutex> lock(g_log_mutex);
        count = ++g_error_counts[std::string(category)];
    }
    if (count > kErrorsPerCategory) return;
    emit(true, category, message);
    if (count == kErrorsPerCategory) emit(true, category, "further errors in this category are not logged");
}

bool runtime_log_once(std::string_view what) {
    {
        const std::lock_guard<std::mutex> lock(g_log_mutex);
        if (!g_once.emplace(what).second) return false;
    }
    emit(true, "unsupported GE state", what);
    return true;
}

GeGpuConfig ge_gpu_config_from_environment(GeGpuConfig base) {
    if (const char *text = std::getenv("PSPRECOMP_GE_BACKEND"); text != nullptr && *text != '\0') {
        if (std::strcmp(text, "d3d12") == 0 || std::strcmp(text, "dx12") == 0 ||
            std::strcmp(text, "directx12") == 0)
            base.backend = GeGpuConfig::Backend::DirectX12;
        else if (std::strcmp(text, "software") == 0)
            base.backend = GeGpuConfig::Backend::Software;
    }
    // Upstream's switch is "skip the CPU raster"; Chili stores the opposite.
    base.keep_software_raster = !env_flag("PSPRECOMP_GE_GPU_SKIP_SOFTWARE_RASTER", !base.keep_software_raster);
    base.internal_scale = env_number("PSPRECOMP_GE_INTERNAL_SCALE", base.internal_scale, 1u, 8u);
    base.msaa = env_number("PSPRECOMP_GE_MSAA", base.msaa, 1u, 16u);
    base.anisotropic_filtering = env_number("PSPRECOMP_GE_ANISOTROPY", base.anisotropic_filtering, 1u, 16u);
    base.anisotropic_keep_bias = env_flag("PSPRECOMP_GE_ANISOTROPY_KEEP_BIAS", base.anisotropic_keep_bias);
    base.ambient_occlusion_debug = env_flag("PSPRECOMP_GE_AO_DEBUG", base.ambient_occlusion_debug);
    base.ambient_occlusion_strength = env_number("PSPRECOMP_GE_AO_STRENGTH", base.ambient_occlusion_strength, 0u, 100u);
    base.depth_precision = env_number("PSPRECOMP_GE_DEPTH_BITS", base.depth_precision, 16u, 32u);
    base.hardware_transform = env_flag("PSPRECOMP_GE_GPU_HW_TRANSFORM", base.hardware_transform);
    base.hardware_lit = env_flag("PSPRECOMP_GE_GPU_HW_LIT", base.hardware_lit);
    base.hardware_cull = env_flag("PSPRECOMP_GE_GPU_HW_CULL", base.hardware_cull);
    base.packed_0115 = env_flag("PSPRECOMP_DX12_PACKED_0115", base.packed_0115);
    base.native_indexed_draw = env_flag("PSPRECOMP_DX12_NATIVE_INDEXED_DRAW", base.native_indexed_draw);
    base.direct_nonindexed_draw = env_flag("PSPRECOMP_GE_DIRECT_NONINDEXED_DRAW", base.direct_nonindexed_draw);
    base.batch_merge = env_flag("PSPRECOMP_DX12_BATCH_MERGE", base.batch_merge);
    base.texture_upload_ring = env_flag("PSPRECOMP_DX12_TEXTURE_UPLOAD_RING", base.texture_upload_ring);
    base.frame_readback = env_flag("PSPRECOMP_DX12_GE_READBACK", base.frame_readback);
    base.strict = env_flag("PSPRECOMP_DX12_GE_STRICT", base.strict);
    if (const char *text = std::getenv("PSPRECOMP_GE_ADAPTER"); text != nullptr) base.adapter = text;
    base.raster_threads = env_number("PSPRECOMP_RASTER_THREADS", base.raster_threads, 1u, 64u);
    base.raster_parallel_pixels =
        env_number("PSPRECOMP_RASTER_PARALLEL_PIXELS", base.raster_parallel_pixels, 0u, 16'777'216u);
    base.parallel_texture_decode = env_flag("PSPRECOMP_GE_PARALLEL_TEXTURE_DECODE", base.parallel_texture_decode);
    base.parallel_vertex_decode = env_flag("PSPRECOMP_GE_PARALLEL_VERTEX_DECODE", base.parallel_vertex_decode);
    base.parallel_vertex_threshold =
        env_number("PSPRECOMP_GE_PARALLEL_VERTEX_THRESHOLD", base.parallel_vertex_threshold, 0u, 1'048'576u);
    base.parallel_vertex_max_workers =
        env_number("PSPRECOMP_GE_PARALLEL_VERTEX_MAX_WORKERS", base.parallel_vertex_max_workers, 1u, 32u);
    base.gpu_timing_diag = env_flag("PSPRECOMP_GPU_TIMING_DIAG", base.gpu_timing_diag);
    base.phase_diag = env_flag("PSPRECOMP_GE_PHASE_DIAG", base.phase_diag);
    base.geometry_debug_colors = env_flag("PSPRECOMP_GE_GPU_GEOMETRY_DEBUG_COLORS", base.geometry_debug_colors);
    base.force_white_vertex = env_flag("PSPRECOMP_GE_GPU_FORCE_WHITE_VERTEX", base.force_white_vertex);
    return base;
}

} // namespace vcs
