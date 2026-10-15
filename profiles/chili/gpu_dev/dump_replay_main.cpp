// chili_ge_dump_replay: replays one captured GE draw (see dump_draw in
// host/ge_runtime.hpp) three ways into a copy of the captured VRAM and
// compares the results:
//   (a) Chili's frozen software rasterizer        -- the reference
//   (b) the lifted front end's own software path  -- isolates state decode
//   (c) the lifted D3D12 backend, read back       -- the path the game will use
//
//   chili_ge_dump_replay <dump.txt> [--out <dir>] [options]
//   chili_ge_dump_replay --selftest [--out <dir>] [options]
//   chili_ge_dump_replay <dump.txt> --present-test [--vram-from <other dump.txt>] [--frames N]
//   chili_ge_dump_replay <dump.txt> --bench N
//
// --present-test opens a window, replays the draw into the displayed surface
// every frame and presents it through the backend's swapchain, then saves a
// capture of the window. --bench queues the draw N times per frame and times
// the CPU side of the D3D12 path.
//
// Options: --scale N (internal scale, default 1)   --msaa N   --depth-bits N
//          --adapter <substring>   --no-hw-transform   --hw-cull
//          --no-seed-depth   --surface <hex address> (extra render surface)
//          --tolerance N (default 8)   --no-gpu

#include "replay_common.hpp"

#include "ge_gpu_backend.hpp"
#include "ge_gpu_config.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace gpu_dev {
int run_selftest(const std::filesystem::path &out_dir, unsigned tolerance);
}

namespace {

using namespace gpu_dev;

std::string diff_json(const DiffStats &d) {
    std::ostringstream out;
    out << "{\"pixels\":" << d.pixels << ",\"rgb_over_tolerance\":" << d.rgb_over_tolerance
        << ",\"alpha_over_tolerance\":" << d.alpha_over_tolerance << ",\"max_rgb\":" << d.max_rgb
        << ",\"max_alpha\":" << d.max_alpha << ",\"mean_abs_rgb\":" << d.mean_abs_rgb << "}";
    return out.str();
}

std::string path_json(const PathResult &r) {
    std::ostringstream out;
    out << "{\"ran\":" << (r.ran ? "true" : "false") << ",\"ok\":" << (r.ok ? "true" : "false")
        << ",\"error\":\"" << json_escape(r.error) << "\",\"notes\":\"" << json_escape(r.notes)
        << "\",\"fallbacks\":\"" << json_escape(vcs::ge_gpu_fallback_names(r.fallbacks)) << "\"}";
    return out.str();
}

std::string state_summary(const DrawCase &d) {
    std::ostringstream out;
    out << std::hex << "prim=" << ((d.primitive_data >> 16u) & 7u) << std::dec
        << " count=" << (d.primitive_data & 0xFFFFu) << std::hex
        << " vtype=" << d.reg(0x12) << " fb=" << d.framebuffer() << " depth=" << d.depthbuffer();
    if (d.textured())
        out << " tex=" << d.texture() << " tfmt=" << (d.reg(0xC3) & 15u) << std::dec
            << " tsize=" << (1u << (d.reg(0xB8) & 15u)) << "x" << (1u << ((d.reg(0xB8) >> 8u) & 15u))
            << std::hex << " tfunc=" << d.reg(0xC9) << " filter=" << d.reg(0xC6);
    else
        out << " untextured";
    if (d.reg(0x21) & 1u)
        out << " blend=" << d.reg(0xDF) << " fixA=" << d.reg(0xE0) << " fixB=" << d.reg(0xE1);
    if (d.reg(0x22) & 1u) out << " alphatest=" << d.reg(0xDB);
    if (d.reg(0x23) & 1u) out << " ztest=" << (d.reg(0xDE) & 7u) << (d.reg(0xE7) & 1u ? " zwrite=off" : " zwrite=on");
    if (d.reg(0xD3) & 1u) out << " clear=" << d.reg(0xD3);
    if (d.reg(0x17) & 1u)
        out << " lighting(" << (d.reg(0x18) & 1u) << (d.reg(0x19) & 1u) << (d.reg(0x1A) & 1u) << (d.reg(0x1B) & 1u) << ")";
    if (d.reg(0x1D) & 1u) out << " cull";
    if (d.reg(0x1F) & 1u) out << " fog";
    return out.str();
}

int replay_dump(const std::filesystem::path &dump_path, const std::filesystem::path &out_root,
                const GpuOptions &gpu_options, bool use_gpu, unsigned tolerance) {
    DrawCase draw;
    std::string error;
    if (!load_dump(dump_path, draw, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }
    const std::filesystem::path out_dir = out_root / draw.name;
    std::error_code ignored;
    std::filesystem::create_directories(out_dir, ignored);

    // Untouched view of the target, to tell how much each path actually drew.
    Image original;
    {
        psprecomp::GuestMemory memory;
        load_memory(memory, draw);
        original = read_sheet(memory, draw.framebuffer());
    }
    const std::uint32_t available_rows =
        std::min<std::uint32_t>(kSheetHeight, (kVramEnd - draw.framebuffer()) / (kSheetWidth * 4u));
    const Rect region = scissor_rect(draw, available_rows);
    const std::uint32_t rows = region.y1 + 1u;

    const PathResult reference = run_reference(draw);
    // Order matters: the front end's software path only runs while the D3D12
    // backend has not been started yet.
    const PathResult software = run_frontend_software(draw);
    PathResult gpu;
    Image gpu_native;
    const std::uint32_t scale = std::max(1u, vcs::ge_gpu_config().internal_scale);
    if (use_gpu) {
        gpu = run_gpu(draw, gpu_options);
        if (!gpu.image.rgba.empty()) {
            gpu_native = gpu.image;
            gpu.image = downscale(gpu.image, scale);
        }
    }

    write_png(out_dir / "before.png", original, rows);
    if (!reference.image.rgba.empty()) write_png(out_dir / "a_reference.png", reference.image, rows);
    if (!software.image.rgba.empty()) write_png(out_dir / "b_frontend_software.png", software.image, rows);
    if (!gpu.image.rgba.empty()) {
        write_png(out_dir / "c_d3d12.png", gpu.image, rows);
        if (scale > 1u) write_png(out_dir / "c_d3d12_native.png", gpu_native, rows * scale);
    }

    const bool have_reference = reference.ok && !reference.image.rgba.empty();
    DiffStats software_diff{}, gpu_diff{}, gpu_vs_software{};
    if (have_reference && !software.image.rgba.empty()) {
        software_diff = compare(reference.image, software.image, region, tolerance);
        write_diff_png(out_dir / "diff_b_vs_a.png", reference.image, software.image, rows);
    }
    if (have_reference && !gpu.image.rgba.empty()) {
        gpu_diff = compare(reference.image, gpu.image, region, tolerance);
        write_diff_png(out_dir / "diff_c_vs_a.png", reference.image, gpu.image, rows);
    }
    if (!software.image.rgba.empty() && !gpu.image.rgba.empty())
        gpu_vs_software = compare(software.image, gpu.image, region, tolerance);

    const std::uint64_t drawn_reference = have_reference ? changed_pixels(original, reference.image, region) : 0u;
    const std::uint64_t drawn_software = software.image.rgba.empty() ? 0u : changed_pixels(original, software.image, region);
    const std::uint64_t drawn_gpu = gpu.image.rgba.empty() ? 0u : changed_pixels(original, gpu.image, region);

    std::ostringstream json;
    json << "{\n"
         << "  \"dump\": \"" << json_escape(draw.name) << "\",\n"
         << "  \"state\": \"" << json_escape(state_summary(draw)) << "\",\n"
         << "  \"adapter\": \"" << json_escape(vcs::ge_gpu_backend_adapter_name()) << "\",\n"
         << "  \"internal_scale\": " << scale << ",\n"
         << "  \"tolerance\": " << tolerance << ",\n"
         << "  \"region\": [" << region.x0 << "," << region.y0 << "," << region.x1 << "," << region.y1 << "],\n"
         << "  \"reference\": " << path_json(reference) << ",\n"
         << "  \"frontend_software\": " << path_json(software) << ",\n"
         << "  \"d3d12\": " << path_json(gpu) << ",\n"
         << "  \"pixels_changed\": {\"reference\":" << drawn_reference << ",\"frontend_software\":"
         << drawn_software << ",\"d3d12\":" << drawn_gpu << "},\n"
         << "  \"frontend_software_vs_reference\": " << diff_json(software_diff) << ",\n"
         << "  \"d3d12_vs_reference\": " << diff_json(gpu_diff) << ",\n"
         << "  \"d3d12_vs_frontend_software\": " << diff_json(gpu_vs_software) << "\n"
         << "}\n";
    std::ofstream(out_dir / "report.json", std::ios::trunc) << json.str();

    std::printf("%s\n  %s\n", draw.name.c_str(), state_summary(draw).c_str());
    const auto line = [&](const char *label, const PathResult &r, std::uint64_t drawn, const DiffStats *d) {
        std::printf("  %-22s %s changed=%llu", label, !r.ran ? "skipped" : r.ok ? "ok" : "FAILED",
                    static_cast<unsigned long long>(drawn));
        if (d != nullptr && have_reference && r.ran && !r.image.rgba.empty())
            std::printf("  vs reference: rgb>%u=%llu alpha>%u=%llu max=%u mean=%.3f", tolerance,
                        static_cast<unsigned long long>(d->rgb_over_tolerance), tolerance,
                        static_cast<unsigned long long>(d->alpha_over_tolerance), d->max_rgb, d->mean_abs_rgb);
        if (!r.error.empty()) std::printf("  error: %s", r.error.c_str());
        if (r.fallbacks != 0u) std::printf("  fallbacks: %s", vcs::ge_gpu_fallback_names(r.fallbacks).c_str());
        std::printf("\n      %s\n", r.notes.c_str());
    };
    line("(a) chili reference", reference, drawn_reference, nullptr);
    line("(b) front end software", software, drawn_software, &software_diff);
    line("(c) d3d12", gpu, drawn_gpu, &gpu_diff);
    return 0;
}

void pump_messages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0u, 0u, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

// Grabs the client area of a window from the composed desktop.
bool capture_client(HWND window, Image &out) {
    RECT client{};
    POINT origin{0, 0};
    if (!GetClientRect(window, &client) || !ClientToScreen(window, &origin)) return false;
    const int width = client.right - client.left, height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return false;
    const HDC screen = GetDC(nullptr);
    const HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void *bits = nullptr;
    const HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0u);
    bool ok = false;
    if (bitmap != nullptr && bits != nullptr) {
        const HGDIOBJ previous = SelectObject(memory, bitmap);
        ok = BitBlt(memory, 0, 0, width, height, screen, origin.x, origin.y, SRCCOPY) != FALSE;
        GdiFlush();
        out.width = static_cast<std::uint32_t>(width);
        out.height = static_cast<std::uint32_t>(height);
        out.rgba.resize(static_cast<std::size_t>(width) * height * 4u);
        const auto *source = static_cast<const std::uint8_t *>(bits);
        for (std::size_t i = 0u; i < out.rgba.size(); i += 4u) {
            out.rgba[i] = source[i + 2u];
            out.rgba[i + 1u] = source[i + 1u];
            out.rgba[i + 2u] = source[i];
            out.rgba[i + 3u] = 0xFFu;
        }
        SelectObject(memory, previous);
    }
    if (bitmap != nullptr) DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return ok;
}

int present_test(DrawCase draw, const std::filesystem::path &out_root, const GpuOptions &options,
                 unsigned frames) {
    if ((draw.framebuffer() & 0x001FFFF0u) != 0u) {
        std::fprintf(stderr, "--present-test needs a draw into the displayed surface 0x04000000\n");
        return 2;
    }
    // Without this the window is DPI-virtualised: the client size, and so the
    // swapchain, would be in logical pixels and the capture in physical ones.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"ChiliGeDumpReplayPresent";
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassW(&window_class);
    RECT bounds{0, 0, 960, 544};
    AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE);
    const HWND window = CreateWindowExW(0u, window_class.lpszClassName, L"Chili GE present test",
                                        WS_OVERLAPPEDWINDOW, 60, 60, bounds.right - bounds.left,
                                        bounds.bottom - bounds.top, nullptr, nullptr,
                                        window_class.hInstance, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "CreateWindowExW failed\n");
        return 1;
    }
    ShowWindow(window, SW_SHOWNOACTIVATE);
    SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    pump_messages();

    std::string error;
    if (!gpu_start(error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        DestroyWindow(window);
        return 1;
    }
    vcs::ge_gpu_backend_set_native_window(window);
    psprecomp::GuestMemory memory;
    load_memory(memory, draw);
    unsigned presented = 0u;
    bool ok = gpu_seed(memory, draw, options, error);
    const auto start = std::chrono::steady_clock::now();
    for (unsigned frame = 0u; ok && frame < frames; ++frame) {
        ok = gpu_draw(memory, draw, error);
        if (vcs::ge_gpu_backend_finish_color_frame(frame + 1u)) ++presented;
        pump_messages();
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const vcs::GeGpuBackendReport report = vcs::ge_gpu_backend_report();

    // Let the compositor show the last frame, then grab it.
    Sleep(400u);
    pump_messages();
    Image capture;
    const bool captured = capture_client(window, capture);
    std::error_code ignored;
    std::filesystem::create_directories(out_root, ignored);
    if (captured) write_png(out_root / "present_test.png", capture, capture.height);

    vcs::ge_gpu_backend_set_native_window(nullptr);
    DestroyWindow(window);
    pump_messages();

    std::printf("present test: %u of %u frames presented, %.3f ms per frame, swapchain=%s, adapter=%s\n",
                presented, frames, frames != 0u ? seconds * 1000.0 / frames : 0.0,
                report.swapchain_active ? "yes" : "no", vcs::ge_gpu_backend_adapter_name().c_str());
    if (!error.empty()) std::printf("  error: %s\n", error.c_str());
    if (captured) {
        // A blank capture means the swapchain image never reached the desktop.
        std::uint64_t lit = 0u;
        for (std::size_t i = 0u; i < capture.rgba.size(); i += 4u)
            if (capture.rgba[i] > 24u || capture.rgba[i + 1u] > 24u || capture.rgba[i + 2u] > 24u) ++lit;
        std::printf("  window capture %ux%u saved, %llu%% of pixels non-black\n", capture.width,
                    capture.height, static_cast<unsigned long long>(lit * 100u / (capture.rgba.size() / 4u)));
    }
    return ok && presented == frames ? 0 : 1;
}

// CPU cost of the D3D12 path: the front end decoding and queueing the draw,
// and the backend recording and submitting the frame. No readback inside.
int bench(const DrawCase &draw, const GpuOptions &options, unsigned draws_per_frame) {
    std::string error;
    if (!gpu_start(error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    psprecomp::GuestMemory memory;
    load_memory(memory, draw);
    if (!gpu_seed(memory, draw, options, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    constexpr unsigned kFrames = 200u;
    double queue_seconds = 0.0, submit_seconds = 0.0;
    for (unsigned frame = 0u; frame < kFrames + 20u; ++frame) {
        const auto t0 = std::chrono::steady_clock::now();
        for (unsigned i = 0u; i < draws_per_frame; ++i)
            if (!gpu_draw(memory, draw, error)) {
                std::fprintf(stderr, "%s\n", error.c_str());
                return 1;
            }
        const auto t1 = std::chrono::steady_clock::now();
        (void)vcs::ge_gpu_backend_flush();
        // Flush never ends the epoch; do it here like a real frame would.
        (void)vcs::ge_gpu_backend_finish_color_frame(frame + 1u);
        const auto t2 = std::chrono::steady_clock::now();
        if (frame < 20u) continue;  // warm-up: pipelines, texture upload
        queue_seconds += std::chrono::duration<double>(t1 - t0).count();
        submit_seconds += std::chrono::duration<double>(t2 - t1).count();
    }
    Image ignored;
    (void)gpu_read(draw.framebuffer(), ignored);  // drains the GPU
    std::printf("%s\n  bench: %u draws per frame, %u frames: queue %.3f ms/frame (%.2f us/draw), "
                "submit %.3f ms/frame\n",
                draw.name.c_str(), draws_per_frame, kFrames, queue_seconds * 1000.0 / kFrames,
                queue_seconds * 1e6 / (static_cast<double>(kFrames) * draws_per_frame),
                submit_seconds * 1000.0 / kFrames);
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    std::filesystem::path dump, out_root = ".";
    vcs::GeGpuConfig config = vcs::ge_gpu_config_from_environment();
    config.backend = vcs::GeGpuConfig::Backend::DirectX12;
    GpuOptions gpu_options;
    bool use_gpu = true, selftest = false, present = false;
    unsigned tolerance = 8u, frames = 120u, bench_draws = 0u;
    std::filesystem::path vram_from;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> const char * { return i + 1 < argc ? argv[++i] : ""; };
        if (arg == "--out") out_root = value();
        else if (arg == "--scale") config.internal_scale = static_cast<std::uint32_t>(std::atoi(value()));
        else if (arg == "--msaa") config.msaa = static_cast<std::uint32_t>(std::atoi(value()));
        else if (arg == "--depth-bits") config.depth_precision = static_cast<std::uint32_t>(std::atoi(value()));
        else if (arg == "--adapter") config.adapter = value();
        else if (arg == "--no-hw-transform") config.hardware_transform = false;
        else if (arg == "--hw-cull") config.hardware_cull = true;
        else if (arg == "--no-seed-depth") gpu_options.seed_depth = false;
        else if (arg == "--surface") gpu_options.surfaces.push_back(static_cast<std::uint32_t>(std::strtoul(value(), nullptr, 16)));
        else if (arg == "--tolerance") tolerance = static_cast<unsigned>(std::atoi(value()));
        else if (arg == "--no-gpu") use_gpu = false;
        else if (arg == "--selftest") selftest = true;
        else if (arg == "--present-test") present = true;
        else if (arg == "--frames") frames = static_cast<unsigned>(std::atoi(value()));
        else if (arg == "--vram-from") vram_from = value();
        else if (arg == "--bench") bench_draws = static_cast<unsigned>(std::atoi(value()));
        else if (!arg.empty() && arg[0] != '-') dump = arg;
        else {
            std::fprintf(stderr, "unknown option %s\n", arg.c_str());
            return 2;
        }
    }
    if (dump.empty() && !selftest) {
        std::fprintf(stderr, "usage: chili_ge_dump_replay <dump.txt> [--out dir] [--scale N] [--msaa N]\n"
                             "       chili_ge_dump_replay --selftest [--out dir]\n");
        return 2;
    }
    config.strict = true;
    // Replay output must not depend on worker timing.
    config.parallel_vertex_decode = false;
    vcs::set_ge_gpu_config(config);

    int status = 0;
    if (selftest) {
        status = run_selftest(out_root, tolerance);
    } else if (present || bench_draws != 0u) {
        DrawCase draw;
        std::string error;
        if (!load_dump(dump, draw, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 2;
        }
        if (!vram_from.empty()) {
            // Same draw, another capture's VRAM: e.g. the LCD blit over a
            // mission frame.
            DrawCase other;
            if (!load_dump(vram_from, other, error)) {
                std::fprintf(stderr, "%s\n", error.c_str());
                return 2;
            }
            // Snapshots of VRAM ranges (a texture that is a surface) belong to
            // the original capture and would overwrite the borrowed image.
            std::erase_if(draw.blobs, [](const DrawCase::Blob &blob) {
                return blob.kind != "vram" && (blob.address & 0x0F800000u) == 0x04000000u;
            });
            for (auto &blob : draw.blobs)
                if (blob.kind == "vram")
                    for (const auto &replacement : other.blobs)
                        if (replacement.kind == "vram") blob.bytes = replacement.bytes;
        }
        status = present ? present_test(draw, out_root, gpu_options, frames)
                         : bench(draw, gpu_options, bench_draws);
    } else {
        status = replay_dump(dump, out_root, gpu_options, use_gpu, tolerance);
    }
    vcs::shutdown_ge_gpu_backend();
    return status;
}
