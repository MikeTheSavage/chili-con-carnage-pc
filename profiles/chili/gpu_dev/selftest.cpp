// Synthetic checks for the Chili changes to the lifted D3D12 backend. The
// captured dumps only cover the states the first mission happened to hit when
// the old rasterizer stopped; these cover the rest of each fix:
//   - the whole PSP blend table against Chili's reference arithmetic
//   - clear-mode depth writes, and depth/colour surviving a frame boundary
//   - framebuffer alpha preserved outside clear mode, written inside it
//   - surface lookups limited to VRAM texture addresses
//   - the adapter the backend picked
// Every case draws through-mode sprites into a seeded 512x320 surface.

#include "replay_common.hpp"

#include "ge_gpu_backend.hpp"
#include "ge_gpu_config.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace gpu_dev {

namespace {

constexpr std::uint32_t kScene = 0x040D8000u;   // composite surface in the game
constexpr std::uint32_t kLcd = 0x04000000u;
constexpr std::uint32_t kDepth = 0x04088000u;
constexpr std::uint32_t kVertices = 0x08900000u;
constexpr std::uint32_t kRamTexture = 0x09288000u;  // low 21 bits equal the depth/scratch surface offset

struct Check {
    std::string name;
    bool pass{};
    std::string detail;
};

void set_reg(DrawCase &draw, unsigned reg, std::uint32_t value) {
    draw.regs[reg] = (static_cast<std::uint32_t>(reg) << 24u) | (value & 0x00FFFFFFu);
}

std::uint32_t pattern(std::uint32_t x, std::uint32_t y) {
    const std::uint32_t r = (x / 2u) & 0xFFu, g = (y * 4u / 5u) & 0xFFu;
    const std::uint32_t b = (x * 7u + y * 3u) & 0xFFu, a = (x + y * 2u) & 0xFFu;
    return r | (g << 8u) | (b << 16u) | (a << 24u);
}

void fill_surface(std::vector<std::uint8_t> &vram, std::uint32_t address, std::uint32_t rows) {
    for (std::uint32_t y = 0u; y < rows; ++y)
        for (std::uint32_t x = 0u; x < kSheetWidth; ++x) {
            const std::uint32_t value = pattern(x, y);
            std::memcpy(vram.data() + (address - kVramBase) + (static_cast<std::size_t>(y) * kSheetWidth + x) * 4u,
                        &value, 4u);
        }
}

// Target `fb`, depth buffer at 0x04088000 holding `depth` everywhere.
DrawCase base_case(const std::string &name, std::uint32_t fb, std::uint16_t depth) {
    DrawCase draw;
    draw.name = name;
    const std::uint32_t rows = fb == kLcd ? 272u : 320u;
    set_reg(draw, 0x9C, fb & 0x00FFFFFFu);
    set_reg(draw, 0x9D, 0x040000u | kSheetWidth);
    set_reg(draw, 0x9E, kDepth & 0x00FFFFFFu);
    set_reg(draw, 0x9F, 0x040000u | kSheetWidth);
    set_reg(draw, 0xD2, 3u);
    const std::uint32_t corner = ((rows - 1u) << 10u) | (fb == kLcd ? 479u : 511u);
    set_reg(draw, 0xD4, 0u);
    set_reg(draw, 0xD5, corner);
    set_reg(draw, 0x16, corner);
    set_reg(draw, 0x55, 0xFFFFFFu);
    set_reg(draw, 0x58, 0xFFu);
    draw.world = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    draw.view = draw.world;
    draw.projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    DrawCase::Blob vram;
    vram.kind = "vram";
    vram.address = kVramBase;
    vram.bytes.assign(psprecomp::GuestMemory::kVramSize, 0u);
    fill_surface(vram.bytes, fb, rows);
    // 512x320 16-bit depth, as the game lays it out.
    for (std::uint32_t i = 0u; i < kSheetWidth * 320u; ++i)
        std::memcpy(vram.bytes.data() + (kDepth - kVramBase) + static_cast<std::size_t>(i) * 2u, &depth, 2u);
    draw.blobs.push_back(std::move(vram));
    return draw;
}

// Untextured colour sprite: vtype 0x80011C (through, colour 8888, s16 position).
void color_sprite(DrawCase &draw, std::uint32_t vertex_address, Rect rect, std::uint16_t z,
                  std::uint32_t rgba) {
    set_reg(draw, 0x12, 0x80011Cu);
    draw.vertex_address = vertex_address;
    draw.primitive_data = (6u << 16u) | 2u;
    DrawCase::Blob blob;
    blob.kind = "vertices";
    blob.address = vertex_address;
    blob.bytes.resize(24u);
    const auto put = [&](std::size_t offset, std::uint32_t x, std::uint32_t y) {
        const std::int16_t sx = static_cast<std::int16_t>(x), sy = static_cast<std::int16_t>(y);
        std::memcpy(blob.bytes.data() + offset, &rgba, 4u);
        std::memcpy(blob.bytes.data() + offset + 4u, &sx, 2u);
        std::memcpy(blob.bytes.data() + offset + 6u, &sy, 2u);
        std::memcpy(blob.bytes.data() + offset + 8u, &z, 2u);
    };
    // Rect is inclusive; the sprite's far corner is exclusive.
    put(0u, rect.x0, rect.y0);
    put(12u, rect.x1 + 1u, rect.y1 + 1u);
    draw.blobs.push_back(std::move(blob));
}

std::string hex(std::uint32_t value, int digits) {
    char text[16];
    std::snprintf(text, sizeof(text), "%0*x", digits, value);
    return text;
}

// ------------------------------------------------------------- blend table

void check_blend_table(std::vector<Check> &checks, unsigned tolerance, std::string &table_text) {
    const Rect rect{16u, 16u, 303u, 207u};
    const GpuOptions options;
    std::ostringstream table;
    // Source alpha below and above one half: the second exercises the doubled
    // factors where the PSP keeps values above 1 and a UNORM target cannot.
    for (const std::uint32_t source : {0x60C84090u, 0xC0C84090u}) {
        unsigned exact = 0u, approximate = 0u, failed = 0u, no_reference = 0u;
        std::uint32_t worst = 0u;
        std::string failures;
        for (unsigned equation = 0u; equation < 3u; ++equation)
            for (unsigned a = 0u; a <= 10u; ++a)
                for (unsigned b = 0u; b <= 10u; ++b) {
                    const std::uint32_t mode = (equation << 8u) | (b << 4u) | a;
                    DrawCase draw = base_case("blend_" + hex(mode, 3), kScene, 0x1111u);
                    set_reg(draw, 0x21, 1u);
                    set_reg(draw, 0xDF, mode);
                    set_reg(draw, 0xE0, 0xC08040u);
                    set_reg(draw, 0xE1, 0x50A030u);
                    color_sprite(draw, kVertices, rect, 0u, source);
                    const PathResult reference = run_reference(draw);
                    const PathResult gpu = run_gpu(draw, options);
                    if (!reference.ok) {
                        ++no_reference;
                        continue;
                    }
                    const DiffStats diff = compare(reference.image, gpu.image, rect, tolerance);
                    const bool matches = gpu.ok && diff.rgb_over_tolerance == 0u &&
                                         diff.alpha_over_tolerance == 0u;
                    if (matches) worst = std::max(worst, diff.max_rgb);
                    table << "source=" << hex(source, 8) << " mode=" << hex(mode, 3)
                          << " max_rgb=" << diff.max_rgb << " over=" << diff.rgb_over_tolerance
                          << " alpha_over=" << diff.alpha_over_tolerance
                          << (gpu.fallbacks != 0u ? " flagged" : "") << (matches ? "" : " MISMATCH") << "\n";
                    if (matches) ++exact;
                    else if (gpu.fallbacks != 0u) ++approximate;
                    else {
                        ++failed;
                        if (failures.size() < 400u)
                            failures += " " + hex(mode, 3) + "(max " + std::to_string(diff.max_rgb) + ")";
                    }
                }
        Check check;
        check.name = "blend table ADD/SUB/REVSUB x 11 x 11, source alpha 0x" + hex(source >> 24u, 2);
        check.pass = failed == 0u && no_reference == 0u;
        std::ostringstream detail;
        detail << exact << " match within " << tolerance << "/255 (worst " << worst << "), "
               << approximate << " differ and are flagged as approximated, " << failed
               << " differ unflagged, " << no_reference << " without reference";
        if (!failures.empty()) detail << ". Unflagged:" << failures;
        check.detail = detail.str();
        checks.push_back(check);
    }

    // MIN and MAX ignore the factors. Chili's reference rejects them, so the
    // expected image is computed here.
    for (const unsigned equation : {3u, 4u}) {
        const std::uint32_t source = 0x60C84090u;
        DrawCase draw = base_case(equation == 3u ? "blend_min" : "blend_max", kScene, 0x1111u);
        set_reg(draw, 0x21, 1u);
        set_reg(draw, 0xDF, (equation << 8u) | 0xAAu);
        set_reg(draw, 0xE0, 0x102030u);
        set_reg(draw, 0xE1, 0x405060u);
        color_sprite(draw, kVertices, rect, 0u, source);
        psprecomp::GuestMemory memory;
        load_memory(memory, draw);
        Image expected = read_sheet(memory, kScene);
        for (std::uint32_t y = rect.y0; y <= rect.y1; ++y)
            for (std::uint32_t x = rect.x0; x <= rect.x1; ++x) {
                std::uint8_t *p = expected.rgba.data() + (static_cast<std::size_t>(y) * expected.width + x) * 4u;
                for (unsigned c = 0u; c < 3u; ++c) {
                    const std::uint8_t s = static_cast<std::uint8_t>(source >> (c * 8u));
                    p[c] = equation == 3u ? std::min(p[c], s) : std::max(p[c], s);
                }
            }
        const PathResult gpu = run_gpu(draw, options);
        const DiffStats diff = compare(expected, gpu.image, rect, tolerance);
        Check check;
        check.name = equation == 3u ? "blend equation MIN" : "blend equation MAX";
        check.pass = gpu.ok && diff.rgb_over_tolerance == 0u && diff.alpha_over_tolerance == 0u;
        check.detail = "max_rgb=" + std::to_string(diff.max_rgb) + " over=" + std::to_string(diff.rgb_over_tolerance);
        checks.push_back(check);
    }

    {
        DrawCase draw = base_case("blend_abs", kScene, 0x1111u);
        set_reg(draw, 0x21, 1u);
        set_reg(draw, 0xDF, 0x5AAu);
        color_sprite(draw, kVertices, rect, 0u, 0x60C84090u);
        const PathResult gpu = run_gpu(draw, options);
        Check check;
        check.name = "blend equation ABS is reported, not silently unblended";
        check.pass = gpu.ok && (gpu.fallbacks & vcs::kGeGpuFallbackBlendAbs) != 0u;
        check.detail = vcs::ge_gpu_fallback_names(gpu.fallbacks);
        checks.push_back(check);
    }
    table_text = table.str();
}

// ------------------------------------------------- depth and persistence

struct Sequence {
    std::vector<DrawCase> draws;
    // Index after which the GPU path ends the frame (twice); -1 for never.
    int frame_end_after{-1};
};

// Runs the sequence on Chili's reference and on the GPU and compares `region`.
Check run_sequence(const std::string &name, const Sequence &sequence, const Rect &region,
                   unsigned tolerance, std::uint64_t expect_changed_min) {
    Check check;
    check.name = name;
    const DrawCase &first = sequence.draws.front();
    psprecomp::GuestMemory reference_memory, gpu_memory;
    for (const DrawCase &draw : sequence.draws) {
        load_memory(reference_memory, draw);
        load_memory(gpu_memory, draw);
    }
    const Image original = read_sheet(reference_memory, first.framebuffer());
    std::string error;
    for (const DrawCase &draw : sequence.draws)
        if (!reference_draw(reference_memory, draw, error)) {
            check.detail = "reference failed on " + draw.name + ": " + error;
            return check;
        }
    const Image expected = read_sheet(reference_memory, first.framebuffer());

    if (!gpu_start(error) || !gpu_seed(gpu_memory, first, GpuOptions{}, error)) {
        check.detail = error;
        return check;
    }
    (void)vcs::ge_gpu_backend_take_fallbacks();
    for (std::size_t i = 0u; i < sequence.draws.size(); ++i) {
        if (!gpu_draw(gpu_memory, sequence.draws[i], error)) {
            check.detail = "gpu draw failed on " + sequence.draws[i].name + ": " + error;
            return check;
        }
        if (static_cast<int>(i) == sequence.frame_end_after) {
            (void)vcs::ge_gpu_backend_finish_color_frame(1u);
            (void)vcs::ge_gpu_backend_finish_color_frame(2u);
        }
    }
    Image actual;
    const bool read = gpu_read(first.framebuffer(), actual);
    const std::uint32_t fallbacks = vcs::ge_gpu_backend_take_fallbacks();
    (void)vcs::ge_gpu_backend_finish_color_frame(0u);
    if (!read) {
        check.detail = "readback failed";
        return check;
    }
    const DiffStats diff = compare(expected, actual, region, tolerance);
    const std::uint64_t changed = changed_pixels(original, expected, region);
    check.pass = diff.rgb_over_tolerance == 0u && diff.alpha_over_tolerance == 0u &&
                 changed >= expect_changed_min;
    std::ostringstream detail;
    detail << "reference changed " << changed << " pixels; gpu differs in " << diff.rgb_over_tolerance
           << " (rgb) / " << diff.alpha_over_tolerance << " (alpha), max " << diff.max_rgb;
    if (fallbacks != 0u) detail << "; flagged: " << vcs::ge_gpu_fallback_names(fallbacks);
    check.detail = detail.str();
    return check;
}

void check_depth_and_persistence(std::vector<Check> &checks, unsigned tolerance) {
    const Rect cleared{64u, 48u, 199u, 159u};
    const Rect tested{32u, 24u, 259u, 199u};
    const Rect marker{300u, 40u, 399u, 99u};
    const auto clear_depth = [&](std::uint32_t fb) {
        DrawCase draw = base_case("clear depth", fb, 0x1111u);
        set_reg(draw, 0xD3, 0x0401u);  // clear mode, depth only
        color_sprite(draw, kVertices, cleared, 0x8000u, 0xFF000000u);
        return draw;
    };
    const auto depth_equal = [&](std::uint32_t fb) {
        DrawCase draw = base_case("depth-equal red", fb, 0x1111u);
        set_reg(draw, 0x23, 1u);   // depth test
        set_reg(draw, 0xDE, 2u);   // EQUAL
        set_reg(draw, 0xE7, 1u);   // no depth write
        color_sprite(draw, kVertices + 0x100u, tested, 0x8000u, 0xFF0000FFu);
        return draw;
    };
    const auto green_marker = [&](std::uint32_t fb) {
        DrawCase draw = base_case("green marker", fb, 0x1111u);
        color_sprite(draw, kVertices + 0x200u, marker, 0u, 0xFF00FF00u);
        return draw;
    };
    const Rect whole{0u, 0u, 479u, 271u};
    const std::uint64_t cleared_pixels =
        static_cast<std::uint64_t>(cleared.x1 - cleared.x0 + 1u) * (cleared.y1 - cleared.y0 + 1u);

    // A clear-mode draw must write depth: only the cleared rectangle then
    // passes a depth-EQUAL draw that covers a larger one.
    {
        Sequence sequence;
        sequence.draws = {clear_depth(kScene), depth_equal(kScene)};
        checks.push_back(run_sequence("clear mode writes depth", sequence, whole, tolerance, cleared_pixels));
    }
    // Depth and colour must survive a frame boundary: upstream cleared every
    // depth buffer, and the displayed surface, at the first use of an interval.
    for (const std::uint32_t fb : {kScene, kLcd}) {
        Sequence sequence;
        sequence.draws = {clear_depth(fb), green_marker(fb), depth_equal(fb)};
        sequence.frame_end_after = 1;
        checks.push_back(run_sequence(
            std::string("no implicit clear across frames, target ") +
                (fb == kLcd ? "0x04000000 (displayed)" : "0x040D8000"),
            sequence, whole, tolerance, cleared_pixels));
    }
    // Ordinary draws with the depth test off must not write depth.
    {
        Sequence sequence;
        DrawCase writer = base_case("depth write requested, test off", kScene, 0x1111u);
        set_reg(writer, 0xE7, 0u);
        color_sprite(writer, kVertices, cleared, 0x8000u, 0xFF00FF00u);
        sequence.draws = {writer, depth_equal(kScene)};
        checks.push_back(run_sequence("depth test off writes no depth", sequence, whole, tolerance, cleared_pixels));
    }
}

void check_alpha(std::vector<Check> &checks, unsigned tolerance) {
    const Rect rect{40u, 40u, 239u, 159u};
    const Rect whole{0u, 0u, 479u, 271u};
    const std::uint64_t pixels = static_cast<std::uint64_t>(200u) * 120u;
    {
        Sequence sequence;
        DrawCase draw = base_case("opaque sprite", kScene, 0x1111u);
        color_sprite(draw, kVertices, rect, 0u, 0xFF3060C0u);
        sequence.draws = {draw};
        checks.push_back(run_sequence("alpha preserved with stencil off", sequence, whole, tolerance, pixels));
    }
    {
        Sequence sequence;
        DrawCase draw = base_case("clear colour and alpha", kScene, 0x1111u);
        set_reg(draw, 0xD3, 0x0301u);
        color_sprite(draw, kVertices, rect, 0u, 0x80112233u);
        sequence.draws = {draw};
        checks.push_back(run_sequence("clear mode writes colour and alpha", sequence, whole, tolerance, pixels));
    }
    {
        Sequence sequence;
        DrawCase draw = base_case("clear colour only", kScene, 0x1111u);
        set_reg(draw, 0xD3, 0x0101u);
        color_sprite(draw, kVertices, rect, 0u, 0x80112233u);
        sequence.draws = {draw};
        checks.push_back(run_sequence("clear mode colour-only keeps alpha", sequence, whole, tolerance, pixels));
    }
}

// A texture in main RAM whose address has the same low 21 bits as a surface
// must be decoded from RAM, not taken from that surface.
void check_feedback_restriction(std::vector<Check> &checks, unsigned tolerance) {
    Check check;
    check.name = "main-RAM texture is not mistaken for a surface";
    std::string error;
    if (!gpu_start(error)) {
        check.detail = error;
        checks.push_back(check);
        return;
    }
    // Make sure a surface exists at 0x04088000, filled with green.
    std::vector<std::byte> green(static_cast<std::size_t>(kSheetWidth) * kSheetHeight * 4u);
    for (std::size_t i = 0u; i < green.size(); i += 4u) {
        green[i] = std::byte{0};
        green[i + 1u] = std::byte{0xFF};
        green[i + 2u] = std::byte{0};
        green[i + 3u] = std::byte{0xFF};
    }
    if (!vcs::ge_gpu_backend_write_surface_rgba(kDepth, kSheetWidth, kSheetHeight, green)) {
        check.detail = "could not create the surface at 0x04088000";
        checks.push_back(check);
        return;
    }

    DrawCase draw = base_case("ram texture", kScene, 0x1111u);
    set_reg(draw, 0x12, 0x800102u);      // through, UV 16-bit, position 16-bit
    set_reg(draw, 0x1E, 1u);
    set_reg(draw, 0xA0, kRamTexture & 0x00FFFFFFu);
    set_reg(draw, 0xA8, ((kRamTexture >> 24u) << 16u) | 64u);
    set_reg(draw, 0xB8, (6u << 8u) | 6u);  // 64x64
    set_reg(draw, 0xC3, 3u);               // 8888
    set_reg(draw, 0xC7, 0x000101u);        // clamp
    set_reg(draw, 0xC9, 0x000103u);        // REPLACE, texture alpha
    set_reg(draw, 0xE7, 1u);               // no depth write
    DrawCase::Blob texture;
    texture.kind = "texture";
    texture.address = kRamTexture;
    texture.bytes.resize(64u * 64u * 4u);
    for (std::uint32_t y = 0u; y < 64u; ++y)
        for (std::uint32_t x = 0u; x < 64u; ++x) {
            const std::uint32_t value = 0xFF000000u | (0x00FF00FFu ^ ((x * 3u) << 8u) ^ (y << 17u));
            std::memcpy(texture.bytes.data() + (static_cast<std::size_t>(y) * 64u + x) * 4u, &value, 4u);
        }
    draw.blobs.push_back(std::move(texture));
    DrawCase::Blob vertices;
    vertices.kind = "vertices";
    vertices.address = kVertices;
    const std::uint16_t words[10]{0u, 0u, 16u, 16u, 0u, 64u, 64u, 80u, 80u, 0u};
    vertices.bytes.resize(sizeof(words));
    std::memcpy(vertices.bytes.data(), words, sizeof(words));
    draw.blobs.push_back(std::move(vertices));
    draw.vertex_address = kVertices;
    draw.primitive_data = (6u << 16u) | 2u;

    const Rect rect{16u, 16u, 79u, 79u};
    const PathResult reference = run_reference(draw);
    const PathResult gpu = run_gpu(draw, GpuOptions{});
    if (!reference.ok || !gpu.ok) {
        check.detail = "reference: " + reference.error + " gpu: " + gpu.error;
        checks.push_back(check);
        return;
    }
    const DiffStats diff = compare(reference.image, gpu.image, rect, tolerance);
    check.pass = diff.rgb_over_tolerance == 0u;
    check.detail = "differs in " + std::to_string(diff.rgb_over_tolerance) + " of " +
                   std::to_string(diff.pixels) + " pixels, max " + std::to_string(diff.max_rgb) + "; " + gpu.notes;
    checks.push_back(check);
}

void check_adapter(std::vector<Check> &checks) {
    using Microsoft::WRL::ComPtr;
    std::string listing;
    bool hardware_gpu_present = false;
    ComPtr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        for (UINT index = 0u;; ++index) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            char name[256]{};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
            char line[512];
            std::snprintf(line, sizeof(line), "[%u] %s vendor=0x%04x dedicated=%lluMB%s; ", index, name,
                          desc.VendorId, static_cast<unsigned long long>(desc.DedicatedVideoMemory >> 20u),
                          (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0u ? " software" : "");
            listing += line;
            if (desc.VendorId == 0x10DEu || desc.VendorId == 0x1002u) hardware_gpu_present = true;
        }
    }
    std::string error;
    Check check;
    check.name = "backend runs on the discrete GPU";
    if (!gpu_start(error)) {
        check.detail = error;
    } else {
        const std::string chosen = vcs::ge_gpu_backend_adapter_name();
        const bool discrete = chosen.find("NVIDIA") != std::string::npos ||
                              chosen.find("AMD") != std::string::npos ||
                              chosen.find("Radeon") != std::string::npos;
        check.pass = discrete || !hardware_gpu_present;
        check.detail = "chosen: " + chosen + ". DXGI lists: " + listing;
    }
    checks.push_back(check);
}

} // namespace

int run_selftest(const std::filesystem::path &out_dir, unsigned tolerance) {
    // Blend arithmetic is exact to a rounding step; anything above 3/255 is a
    // real mismatch whatever tolerance the dump comparison uses.
    tolerance = std::min(tolerance, 3u);
    std::vector<Check> checks;
    std::string blend_table;
    check_adapter(checks);
    check_blend_table(checks, tolerance, blend_table);
    check_depth_and_persistence(checks, tolerance);
    check_alpha(checks, tolerance);
    check_feedback_restriction(checks, tolerance);

    std::error_code ignored;
    std::filesystem::create_directories(out_dir, ignored);
    std::ofstream(out_dir / "selftest_blend_table.txt", std::ios::trunc) << blend_table;
    std::ostringstream json;
    json << "[\n";
    unsigned failed = 0u;
    for (std::size_t i = 0u; i < checks.size(); ++i) {
        const Check &check = checks[i];
        if (!check.pass) ++failed;
        std::printf("%s  %s\n      %s\n", check.pass ? "PASS" : "FAIL", check.name.c_str(), check.detail.c_str());
        json << "  {\"check\":\"" << json_escape(check.name) << "\",\"pass\":" << (check.pass ? "true" : "false")
             << ",\"detail\":\"" << json_escape(check.detail) << "\"}" << (i + 1u < checks.size() ? "," : "") << "\n";
    }
    json << "]\n";
    std::ofstream(out_dir / "selftest.json", std::ios::trunc) << json.str();
    std::printf("%u of %zu checks failed\n", failed, checks.size());
    return failed == 0u ? 0 : 1;
}

} // namespace gpu_dev
