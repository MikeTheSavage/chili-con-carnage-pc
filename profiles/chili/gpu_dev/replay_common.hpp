#pragma once
// Shared pieces of the GE bring-up tools: dump loading, the three render
// paths, image comparison and PNG output.

#include "psprecomp/guest_memory.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gpu_dev {

constexpr std::uint32_t kVramBase = psprecomp::GuestMemory::kVramPhysicalBase;
constexpr std::uint32_t kVramEnd = kVramBase + psprecomp::GuestMemory::kVramSize;
constexpr std::uint32_t kSheetWidth = 512u;
constexpr std::uint32_t kSheetHeight = 512u;

// One GE draw: complete register file, matrix state and the guest memory it
// reads. Either parsed from a dump_draw capture or built by the self-test.
struct DrawCase {
    std::string name;
    std::uint32_t primitive_data{};
    std::uint32_t vertex_address{};
    std::uint32_t index_address{};
    std::array<std::uint32_t, 256> regs{};
    std::array<float, 12> world{};
    std::array<float, 12> view{};
    std::array<float, 16> projection{};
    std::array<float, 12> texture_matrix{};
    std::array<float, 96> bones{};
    std::array<float, 8> morph{};
    struct Blob {
        std::string kind;
        std::uint32_t address{};
        std::vector<std::uint8_t> bytes;
    };
    std::vector<Blob> blobs;

    [[nodiscard]] std::uint32_t reg(unsigned index) const noexcept { return regs[index] & 0x00FFFFFFu; }
    [[nodiscard]] std::uint32_t framebuffer() const noexcept { return kVramBase | (reg(0x9C) & 0x001FFFF0u); }
    [[nodiscard]] std::uint32_t depthbuffer() const noexcept { return kVramBase | (reg(0x9E) & 0x001FFFF0u); }
    [[nodiscard]] std::uint32_t texture() const noexcept {
        return (reg(0xA0) & 0x00FFFFF0u) | ((reg(0xA8) << 8u) & 0x0F000000u);
    }
    [[nodiscard]] bool textured() const noexcept { return (reg(0x1E) & 1u) != 0u; }
};

[[nodiscard]] bool load_dump(const std::filesystem::path &text_path, DrawCase &out, std::string &error);
void load_memory(psprecomp::GuestMemory &memory, const DrawCase &draw);

// A 512-pixel-wide RGBA8 view of VRAM starting at a surface address.
struct Image {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgba;
    [[nodiscard]] const std::uint8_t *pixel(std::uint32_t x, std::uint32_t y) const noexcept {
        return rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4u;
    }
};
[[nodiscard]] Image read_sheet(const psprecomp::GuestMemory &memory, std::uint32_t address);
[[nodiscard]] std::vector<std::uint16_t> read_depth_sheet(const psprecomp::GuestMemory &memory,
                                                           std::uint32_t address, std::uint32_t stride);

struct PathResult {
    bool ran{};
    bool ok{};
    std::string error;
    std::string notes;
    Image image;
    std::uint32_t fallbacks{};
};

// (a) Chili's frozen rasterizer, (b) the lifted front end's software path,
// (c) the lifted D3D12 backend, read back at its internal scale.
[[nodiscard]] PathResult run_reference(const DrawCase &draw);
[[nodiscard]] PathResult run_frontend_software(const DrawCase &draw);
struct GpuOptions {
    // Surfaces the game renders to; a texture at one of these samples the GPU
    // surface instead of being decoded from guest memory.
    std::vector<std::uint32_t> surfaces{0x04000000u, 0x04088000u, 0x040D8000u, 0x040E8000u};
    bool seed_depth{true};
};
[[nodiscard]] PathResult run_gpu(const DrawCase &draw, const GpuOptions &options);

// The same paths one step at a time, for sequences of draws on one memory.
bool reference_draw(psprecomp::GuestMemory &memory, const DrawCase &draw, std::string &error,
                    std::string *notes = nullptr);
bool gpu_start(std::string &error);
bool gpu_seed(const psprecomp::GuestMemory &memory, const DrawCase &draw,
              const GpuOptions &options, std::string &error, std::string *notes = nullptr);
bool gpu_draw(psprecomp::GuestMemory &memory, const DrawCase &draw, std::string &error,
              std::string *notes = nullptr);
bool gpu_read(std::uint32_t address, Image &out);

struct Rect {
    std::uint32_t x0{}, y0{}, x1{}, y1{};  // inclusive
};
[[nodiscard]] Rect scissor_rect(const DrawCase &draw, std::uint32_t rows);

struct DiffStats {
    std::uint64_t pixels{};
    std::uint64_t rgb_over_tolerance{};
    std::uint64_t alpha_over_tolerance{};
    std::uint32_t max_rgb{};
    std::uint32_t max_alpha{};
    double mean_abs_rgb{};
};
[[nodiscard]] DiffStats compare(const Image &a, const Image &b, const Rect &region, unsigned tolerance);
// Pixels of `after` inside `region` that differ from `before` in any channel.
[[nodiscard]] std::uint64_t changed_pixels(const Image &before, const Image &after, const Rect &region);

// PNG with alpha forced opaque, rows [0, rows). `scale` is the image's pixels
// per PSP pixel (the D3D12 path at internal scale > 1).
bool write_png(const std::filesystem::path &path, const Image &image, std::uint32_t rows);
bool write_diff_png(const std::filesystem::path &path, const Image &a, const Image &b, std::uint32_t rows);
// Box-filters an image rendered at `scale` back to PSP resolution.
[[nodiscard]] Image downscale(const Image &image, std::uint32_t scale);

[[nodiscard]] std::string json_escape(const std::string &text);

} // namespace gpu_dev
