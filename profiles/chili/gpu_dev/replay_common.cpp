#include "replay_common.hpp"

#include "ge_gpu_backend.hpp"
#include "ge_gpu_config.hpp"
#include "ge_renderer.hpp"

// Chili's frozen software rasterizer, header-only, exactly as the game uses it.
#include "ge_sprites.hpp"
#include "ge_triangle_raster.hpp"
#include "ge_triangles.hpp"

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
#include <wincodec.h>
#include <wrl/client.h>

namespace gpu_dev {

namespace {

bool read_file(const std::filesystem::path &path, std::vector<std::uint8_t> &out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char *>(out.data()), size);
    return static_cast<bool>(in);
}

template <std::size_t N>
bool parse_floats(std::istringstream &line, std::array<float, N> &out) {
    for (float &value : out)
        if (!(line >> value)) return false;
    return true;
}

// "key=hex" fields of the dump header and range lines.
bool field(const std::string &line, const char *key, std::uint32_t &out) {
    const std::string needle = std::string(key) + "=";
    std::size_t at = 0u;
    while ((at = line.find(needle, at)) != std::string::npos) {
        if (at == 0u || line[at - 1u] == ' ') {
            out = static_cast<std::uint32_t>(std::strtoul(line.c_str() + at + needle.size(), nullptr, 16));
            return true;
        }
        at += needle.size();
    }
    return false;
}

} // namespace

bool load_dump(const std::filesystem::path &text_path, DrawCase &out, std::string &error) {
    std::ifstream in(text_path);
    if (!in) {
        error = "cannot open " + text_path.string();
        return false;
    }
    out = {};
    out.name = text_path.stem().string();
    const std::filesystem::path root = text_path.parent_path() / text_path.stem();
    unsigned registers = 0u;
    bool have_primitive = false;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream words(line);
        std::string head;
        if (!(words >> head)) continue;
        if (head == "reg") {
            std::string index, value;
            if (!(words >> index >> value)) continue;
            const unsigned long i = std::strtoul(index.c_str(), nullptr, 16);
            if (i < out.regs.size()) {
                out.regs[i] = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 16));
                ++registers;
            }
        } else if (head == "world") {
            parse_floats(words, out.world);
        } else if (head == "view") {
            parse_floats(words, out.view);
        } else if (head == "projection") {
            parse_floats(words, out.projection);
        } else if (head == "texture_matrix") {
            parse_floats(words, out.texture_matrix);
        } else if (head == "bones") {
            parse_floats(words, out.bones);
        } else if (head == "morph") {
            parse_floats(words, out.morph);
        } else if (head.rfind("primitive=", 0u) == 0u) {
            have_primitive = field(line, "primitive", out.primitive_data) &&
                             field(line, "vertices", out.vertex_address);
        } else if (head.rfind("vertex_stride=", 0u) == 0u) {
            field(line, "indices", out.index_address);
        } else if (head == "indices" || head == "vertices" || head == "vram" ||
                   head == "texture" || head == "palette_source") {
            std::uint32_t address = 0u, length = 0u, saved = 0u;
            if (!field(line, "address", address) || !field(line, "length", length) ||
                !field(line, "saved", saved) || saved == 0u)
                continue;  // "<kind> unavailable ..." lines carry no file
            DrawCase::Blob blob;
            blob.kind = head;
            blob.address = address;
            const std::filesystem::path file = root.string() + "_" + head + ".bin";
            if (!read_file(file, blob.bytes)) {
                error = "cannot read " + file.string();
                return false;
            }
            if (blob.bytes.size() != length) {
                error = file.string() + " has " + std::to_string(blob.bytes.size()) +
                        " bytes, the dump says " + std::to_string(length);
                return false;
            }
            out.blobs.push_back(std::move(blob));
        }
    }
    if (registers != 256u || !have_primitive) {
        error = text_path.string() + " is not a GE draw dump (registers=" + std::to_string(registers) + ")";
        return false;
    }
    return true;
}

void load_memory(psprecomp::GuestMemory &memory, const DrawCase &draw) {
    // VRAM first: a texture or palette snapshot taken from VRAM is the same
    // bytes, and anything else must not be overwritten by it.
    for (const auto &blob : draw.blobs)
        if (blob.kind == "vram") memory.copy_in(blob.address, blob.bytes);
    for (const auto &blob : draw.blobs)
        if (blob.kind != "vram") memory.copy_in(blob.address, blob.bytes);
}

Image read_sheet(const psprecomp::GuestMemory &memory, std::uint32_t address) {
    Image image;
    image.width = kSheetWidth;
    image.height = kSheetHeight;
    image.rgba.assign(static_cast<std::size_t>(kSheetWidth) * kSheetHeight * 4u, 0u);
    const std::uint32_t base = kVramBase | (address & 0x001FFFF0u);
    const std::uint32_t rows = std::min<std::uint32_t>(kSheetHeight, (kVramEnd - base) / (kSheetWidth * 4u));
    if (rows != 0u)
        memory.copy_out(base, std::span<std::uint8_t>(image.rgba.data(),
                                                      static_cast<std::size_t>(rows) * kSheetWidth * 4u));
    return image;
}

std::vector<std::uint16_t> read_depth_sheet(const psprecomp::GuestMemory &memory,
                                            std::uint32_t address, std::uint32_t stride) {
    std::vector<std::uint16_t> depth(static_cast<std::size_t>(kSheetWidth) * kSheetHeight, 0u);
    const std::uint32_t base = kVramBase | (address & 0x001FFFF0u);
    if (stride == 0u) stride = kSheetWidth;
    for (std::uint32_t y = 0u; y < kSheetHeight; ++y) {
        const std::uint32_t row = base + y * stride * 2u;
        const std::uint32_t columns = std::min(stride, kSheetWidth);
        if (row + columns * 2u > kVramEnd) break;
        std::array<std::uint8_t, kSheetWidth * 2u> bytes{};
        memory.copy_out(row, std::span<std::uint8_t>(bytes.data(), columns * 2u));
        for (std::uint32_t x = 0u; x < columns; ++x)
            depth[static_cast<std::size_t>(y) * kSheetWidth + x] =
                static_cast<std::uint16_t>(bytes[x * 2u] | (bytes[x * 2u + 1u] << 8u));
    }
    return depth;
}

bool reference_draw(psprecomp::GuestMemory &memory, const DrawCase &draw, std::string &error,
                    std::string *notes) {
    try {
        std::array<unsigned, 256> regs{};
        for (std::size_t i = 0u; i < regs.size(); ++i) regs[i] = draw.regs[i];

        // The game latches the palette at the CLOAD command; the dump holds
        // the bytes that command read.
        chili::GeClutCache clut;
        std::string clut_error;
        if (!clut.load(memory, regs[0xB0] & 0xFFFFFFu, regs[0xB1] & 0xFFFFFFu,
                       regs[0xC4] & 0xFFFFFFu, clut_error) && notes != nullptr)
            *notes += "CLUT load: " + clut_error + ". ";

        // Same dispatch as the PRIM handler in host/ge_runtime.hpp.
        const unsigned data = draw.primitive_data;
        const unsigned primitive = (data >> 16u) & 7u;
        const bool triangles = primitive >= 3u && primitive <= 5u && !(regs[0xD3] & 1u);
        unsigned advanced = 0u, advanced_indices = draw.index_address, written = 0u;
        bool rendered = false;
        if (triangles) {
            std::string geometry_error;
            rendered = primitive == 3u &&
                chili::render_ge_triangles(memory, regs, draw.world, draw.view, draw.projection,
                                           draw.vertex_address, data, advanced, geometry_error);
            if (rendered && notes != nullptr) *notes += "coincident-geometry shortcut (no pixels). ";
            if (!rendered) {
                chili::ge::DeformState deformation;
                const chili::ge::DeformState *deformation_state = nullptr;
                const unsigned vtype = regs[0x12] & 0xFFFFFFu;
                if ((vtype & 0x1C0000u) || ((vtype & 0x600u) && !(vtype & 0x800000u))) {
                    deformation.morph_weights = draw.morph;
                    for (unsigned bone = 0u; bone < 8u; ++bone)
                        std::copy_n(draw.bones.begin() + bone * 12u, 12u, deformation.bones[bone].begin());
                    deformation_state = &deformation;
                }
                rendered = chili::render_ge_triangle_raster(
                    memory, regs, draw.world, draw.view, draw.projection, draw.vertex_address, data,
                    advanced, error, clut, &written, draw.index_address, &advanced_indices,
                    deformation_state);
            }
            if (rendered && notes != nullptr) *notes += "written_pixels=" + std::to_string(written) + ". ";
        } else {
            rendered = chili::render_ge_sprites(memory, regs, draw.vertex_address, data, advanced,
                                                error, &clut, nullptr);
        }
        return rendered;
    } catch (const std::exception &failure) {
        error = std::string("exception: ") + failure.what();
        return false;
    }
}

PathResult run_reference(const DrawCase &draw) {
    PathResult result;
    result.ran = true;
    try {
        psprecomp::GuestMemory memory;
        load_memory(memory, draw);
        result.ok = reference_draw(memory, draw, result.error, &result.notes);
        result.image = read_sheet(memory, draw.framebuffer());
    } catch (const std::exception &failure) {
        result.ok = false;
        result.error = std::string("exception: ") + failure.what();
    }
    return result;
}

namespace {

vcs::GeTransformState make_transform(const DrawCase &draw) {
    vcs::GeTransformState state{};
    state.bones = draw.bones;
    state.world = draw.world;
    state.view = draw.view;
    state.projection = draw.projection;
    state.texture = draw.texture_matrix;
    state.morph_weights = draw.morph;
    return state;
}

std::string stats_notes(const vcs::GeRenderStats &stats) {
    std::ostringstream out;
    out << "triangles=" << stats.triangles << " rectangles=" << stats.rectangles
        << " culled=" << stats.culled_triangles << " pixels_written=" << stats.pixels_written;
    if (stats.unsupported_primitives != 0u) out << " unsupported_primitives=" << stats.unsupported_primitives;
    if (stats.skinned_vertices != 0u) out << " skinned=" << stats.skinned_vertices;
    if (stats.morphed_vertices != 0u) out << " morphed=" << stats.morphed_vertices;
    if (stats.lit_vertices != 0u) out << " lit=" << stats.lit_vertices;
    out << ". ";
    return out.str();
}

bool g_backend_started = false;
std::string g_backend_error;

bool ensure_backend() {
    if (g_backend_started) return vcs::ge_gpu_backend_active();
    g_backend_started = true;
    if (!vcs::initialize_ge_gpu_backend(g_backend_error) || !vcs::ge_gpu_backend_active()) {
        if (g_backend_error.empty()) g_backend_error = vcs::ge_gpu_backend_report().message;
        return false;
    }
    vcs::ge_gpu_backend_set_display_framebuffer(kVramBase);
    return true;
}

} // namespace

PathResult run_frontend_software(const DrawCase &draw) {
    PathResult result;
    result.ran = true;
    if (vcs::ge_gpu_backend_active()) {
        result.error = "the front end's software path only runs while the GPU backend is inactive";
        return result;
    }
    try {
        psprecomp::GuestMemory memory;
        load_memory(memory, draw);
        const vcs::GeTransformState transform = make_transform(draw);
        vcs::GeRenderStats stats{};
        result.ok = vcs::render_ge_primitive(memory, draw.regs, transform, draw.vertex_address,
                                             draw.index_address, draw.primitive_data, stats, result.error);
        result.notes = stats_notes(stats);
        result.image = read_sheet(memory, draw.framebuffer());
    } catch (const std::exception &failure) {
        result.ok = false;
        result.error = std::string("exception: ") + failure.what();
    }
    return result;
}

bool gpu_start(std::string &error) {
    if (ensure_backend()) return true;
    error = "D3D12 backend unavailable: " + g_backend_error;
    return false;
}

bool gpu_seed(const psprecomp::GuestMemory &memory, const DrawCase &draw,
              const GpuOptions &options, std::string &error, std::string *notes) {
    const auto as_bytes = [](const Image &image) {
        return std::span<const std::byte>(reinterpret_cast<const std::byte *>(image.rgba.data()),
                                          image.rgba.size());
    };
    const auto is_surface = [&](std::uint32_t address) {
        return (address & 0x0F800000u) == 0x04000000u &&
               std::find(options.surfaces.begin(), options.surfaces.end(),
                         kVramBase | (address & 0x001FFFF0u)) != options.surfaces.end();
    };
    // The GPU owns its surfaces, so the captured VRAM has to be put there
    // before the draw: the target, the depth buffer and a sampled surface.
    const std::uint32_t target = draw.framebuffer();
    const Image seed = read_sheet(memory, target);
    if (!vcs::ge_gpu_backend_write_surface_rgba(target, seed.width, seed.height, as_bytes(seed))) {
        error = "could not seed the target surface";
        return false;
    }
    if (draw.textured() && is_surface(draw.texture()) &&
        (draw.texture() & 0x001FFFF0u) != (target & 0x001FFFF0u)) {
        const Image source = read_sheet(memory, draw.texture());
        if (!vcs::ge_gpu_backend_write_surface_rgba(draw.texture(), source.width, source.height,
                                                    as_bytes(source))) {
            error = "could not seed the sampled surface";
            return false;
        }
        if (notes != nullptr) *notes += "sampled surface seeded. ";
    }
    if (options.seed_depth) {
        const std::vector<std::uint16_t> depth =
            read_depth_sheet(memory, draw.depthbuffer(), draw.reg(0x9F) & 0x7FCu);
        if (!vcs::ge_gpu_backend_write_surface_depth16(draw.depthbuffer(), target,
                                                       kSheetWidth, kSheetHeight, depth)) {
            error = "could not seed the depth surface";
            return false;
        }
    }
    return true;
}

bool gpu_draw(psprecomp::GuestMemory &memory, const DrawCase &draw, std::string &error,
              std::string *notes) {
    const vcs::GeGpuBackendReport before = vcs::ge_gpu_backend_report();
    const vcs::GeTransformState transform = make_transform(draw);
    vcs::GeRenderStats stats{};
    const bool rendered = vcs::render_ge_primitive(
        memory, draw.regs, transform, draw.vertex_address, draw.index_address,
        draw.primitive_data, stats, error);
    if (notes != nullptr) {
        const vcs::GeGpuBackendReport after = vcs::ge_gpu_backend_report();
        std::ostringstream text;
        text << stats_notes(stats);
        const std::uint64_t hw = after.hw_transform_draw_calls - before.hw_transform_draw_calls;
        text << (hw != 0u ? "hardware transform. " : "screen-space triangles. ");
        if (after.game_textured_draws_without_texture != before.game_textured_draws_without_texture)
            text << "texture missing. ";
        if (after.game_vertex_overflows != before.game_vertex_overflows) text << "geometry overflow. ";
        *notes += text.str();
    }
    return rendered;
}

bool gpu_read(std::uint32_t address, Image &out) {
    std::vector<std::byte> pixels;
    std::uint32_t width = 0u, height = 0u;
    if (!vcs::ge_gpu_backend_read_surface_rgba(address, pixels, width, height)) return false;
    out.width = width;
    out.height = height;
    out.rgba.resize(pixels.size());
    std::memcpy(out.rgba.data(), pixels.data(), pixels.size());
    return true;
}

PathResult run_gpu(const DrawCase &draw, const GpuOptions &options) {
    PathResult result;
    result.ran = true;
    if (!gpu_start(result.error)) return result;
    try {
        psprecomp::GuestMemory memory;
        load_memory(memory, draw);
        if (!gpu_seed(memory, draw, options, result.error, &result.notes)) return result;
        (void)vcs::ge_gpu_backend_take_fallbacks();
        const vcs::GeGpuBackendReport before = vcs::ge_gpu_backend_report();
        const bool rendered = gpu_draw(memory, draw, result.error, &result.notes);
        const bool read = gpu_read(draw.framebuffer(), result.image);
        result.fallbacks = vcs::ge_gpu_backend_take_fallbacks();
        const vcs::GeGpuBackendReport after = vcs::ge_gpu_backend_report();
        // End the frame so the next case revalidates cached textures.
        (void)vcs::ge_gpu_backend_finish_color_frame(0u);
        if (!read) {
            if (result.error.empty()) result.error = "surface readback failed";
            return result;
        }
        if (after.dx12_self_feedback_snapshots != before.dx12_self_feedback_snapshots)
            result.notes += "self-feedback snapshot. ";
        else if (after.dx12_gpu_feedback_draws != before.dx12_gpu_feedback_draws)
            result.notes += "surface sampled GPU to GPU. ";
        result.ok = rendered;
    } catch (const std::exception &failure) {
        result.ok = false;
        result.error = std::string("exception: ") + failure.what();
    }
    return result;
}

Rect scissor_rect(const DrawCase &draw, std::uint32_t rows) {
    const std::uint32_t s1 = draw.reg(0xD4), s2 = draw.reg(0xD5);
    Rect rect{};
    rect.x0 = s1 & 0x3FFu;
    rect.y0 = (s1 >> 10u) & 0x3FFu;
    // An all-zero second corner means "whole framebuffer" to the front end.
    rect.x1 = s2 == 0u ? kSheetWidth - 1u : s2 & 0x3FFu;
    rect.y1 = s2 == 0u ? 271u : (s2 >> 10u) & 0x3FFu;
    rect.x1 = std::min(rect.x1, kSheetWidth - 1u);
    rect.y1 = std::min(rect.y1, std::max(rows, 1u) - 1u);
    rect.x0 = std::min(rect.x0, rect.x1);
    rect.y0 = std::min(rect.y0, rect.y1);
    return rect;
}

DiffStats compare(const Image &a, const Image &b, const Rect &region, unsigned tolerance) {
    DiffStats stats{};
    if (a.width != b.width || a.rgba.empty() || b.rgba.empty()) return stats;
    const std::uint32_t y1 = std::min({region.y1, a.height - 1u, b.height - 1u});
    const std::uint32_t x1 = std::min(region.x1, a.width - 1u);
    std::uint64_t sum = 0u;
    for (std::uint32_t y = region.y0; y <= y1; ++y) {
        for (std::uint32_t x = region.x0; x <= x1; ++x) {
            const std::uint8_t *pa = a.pixel(x, y), *pb = b.pixel(x, y);
            std::uint32_t worst = 0u;
            for (unsigned c = 0u; c < 3u; ++c) {
                const std::uint32_t d = static_cast<std::uint32_t>(std::abs(int(pa[c]) - int(pb[c])));
                worst = std::max(worst, d);
                sum += d;
            }
            const std::uint32_t alpha = static_cast<std::uint32_t>(std::abs(int(pa[3]) - int(pb[3])));
            ++stats.pixels;
            if (worst > tolerance) ++stats.rgb_over_tolerance;
            if (alpha > tolerance) ++stats.alpha_over_tolerance;
            stats.max_rgb = std::max(stats.max_rgb, worst);
            stats.max_alpha = std::max(stats.max_alpha, alpha);
        }
    }
    if (stats.pixels != 0u) stats.mean_abs_rgb = static_cast<double>(sum) / (3.0 * static_cast<double>(stats.pixels));
    return stats;
}

std::uint64_t changed_pixels(const Image &before, const Image &after, const Rect &region) {
    if (before.width != after.width || before.rgba.empty() || after.rgba.empty()) return 0u;
    const std::uint32_t y1 = std::min({region.y1, before.height - 1u, after.height - 1u});
    std::uint64_t count = 0u;
    for (std::uint32_t y = region.y0; y <= y1; ++y)
        for (std::uint32_t x = region.x0; x <= std::min(region.x1, before.width - 1u); ++x)
            if (std::memcmp(before.pixel(x, y), after.pixel(x, y), 4u) != 0) ++count;
    return count;
}

Image downscale(const Image &image, std::uint32_t scale) {
    if (scale <= 1u || image.width % scale != 0u || image.height % scale != 0u) return image;
    Image out;
    out.width = image.width / scale;
    out.height = image.height / scale;
    out.rgba.resize(static_cast<std::size_t>(out.width) * out.height * 4u);
    const std::uint32_t samples = scale * scale;
    for (std::uint32_t y = 0u; y < out.height; ++y)
        for (std::uint32_t x = 0u; x < out.width; ++x)
            for (unsigned c = 0u; c < 4u; ++c) {
                std::uint32_t sum = 0u;
                for (std::uint32_t sy = 0u; sy < scale; ++sy)
                    for (std::uint32_t sx = 0u; sx < scale; ++sx)
                        sum += image.pixel(x * scale + sx, y * scale + sy)[c];
                out.rgba[(static_cast<std::size_t>(y) * out.width + x) * 4u + c] =
                    static_cast<std::uint8_t>((sum + samples / 2u) / samples);
            }
    return out;
}

namespace {

using Microsoft::WRL::ComPtr;

bool write_png_bgr(const std::filesystem::path &path, std::uint32_t width, std::uint32_t height,
                   std::vector<std::uint8_t> &bgr) {
    if (width == 0u || height == 0u) return false;
    static const bool com_ready = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    (void)com_ready;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(width, height))) return false;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetPixelFormat(&format)) || !IsEqualGUID(format, GUID_WICPixelFormat24bppBGR))
        return false;
    return SUCCEEDED(frame->WritePixels(height, width * 3u, static_cast<UINT>(bgr.size()), bgr.data())) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

} // namespace

bool write_png(const std::filesystem::path &path, const Image &image, std::uint32_t rows) {
    rows = std::min(rows, image.height);
    std::vector<std::uint8_t> bgr(static_cast<std::size_t>(image.width) * rows * 3u);
    for (std::uint32_t y = 0u; y < rows; ++y)
        for (std::uint32_t x = 0u; x < image.width; ++x) {
            const std::uint8_t *p = image.pixel(x, y);
            std::uint8_t *q = bgr.data() + (static_cast<std::size_t>(y) * image.width + x) * 3u;
            q[0] = p[2];
            q[1] = p[1];
            q[2] = p[0];
        }
    return write_png_bgr(path, image.width, rows, bgr);
}

bool write_diff_png(const std::filesystem::path &path, const Image &a, const Image &b, std::uint32_t rows) {
    if (a.width != b.width) return false;
    rows = std::min({rows, a.height, b.height});
    std::vector<std::uint8_t> bgr(static_cast<std::size_t>(a.width) * rows * 3u);
    for (std::uint32_t y = 0u; y < rows; ++y)
        for (std::uint32_t x = 0u; x < a.width; ++x) {
            const std::uint8_t *p = a.pixel(x, y), *q = b.pixel(x, y);
            std::uint8_t *o = bgr.data() + (static_cast<std::size_t>(y) * a.width + x) * 3u;
            // Absolute difference, amplified so a tolerance-sized error is visible.
            for (unsigned c = 0u; c < 3u; ++c)
                o[2u - c] = static_cast<std::uint8_t>(std::min(255, std::abs(int(p[c]) - int(q[c])) * 8));
        }
    return write_png_bgr(path, a.width, rows, bgr);
}

std::string json_escape(const std::string &text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': break;
        case '\t': out += ' '; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20u) out += ' ';
            else out += c;
        }
    }
    return out;
}

} // namespace gpu_dev
