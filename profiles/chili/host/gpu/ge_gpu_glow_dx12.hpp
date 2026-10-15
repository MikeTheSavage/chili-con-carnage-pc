// Chili: the game's full-screen glow drawn at the PSP's own size and enlarged
// smoothly ([Rendering] Glow). This file is part of ge_gpu_backend_dx12.cpp and
// is included inside its anonymous namespace, after the post-process passes
// and before the frame submission.
//
// The game makes the glow with 28 sprite draws on a sheet of its own
// (0x04088000 in a mission): the picture is copied there twice at half size
// (a working picture of 120x68 PSP pixels), cut off below half brightness,
// added to itself 21 times at offsets of one and two working pixels, and one
// last sprite adds the result onto the picture, enlarged four times with the
// bilinear filter. Drawn at internal scale N, as every other draw is, the
// working picture is N times finer than the PSP's, the 21 copies lie N pixels
// apart and the enlargement no longer joins them: at 1920x1088 the glow is a
// lattice of 16-pixel squares with ghost copies of every bright edge.
//
// Smooth: the sprites of a glow block (ge_glow_blocks.hpp finds one; nothing
// of the game's numbers is read) are drawn as they are, with the same shader
// and blend states, into a sheet of the PSP's size, a "glow sheet":
//
//   - a draw into the sheet that samples a full-size surface (the first one:
//     the picture) reads a copy of it averaged over N x N pixels: the picture
//     the PSP would have had. One pass makes that copy for all the sprites of
//     a block that read the surface;
//   - a draw into the sheet that samples the sheet goes through the ordinary
//     path for a surface that reads itself, on the small sheet;
//   - a draw elsewhere that samples the sheet (the game's last sprite; in the
//     intro's cutscenes also a sprite that paints the black bars from it) reads
//     the small sheet through a cubic filter (the pixel shader's GLOW_CUBIC
//     variant) and is otherwise the game's draw: its blend, colour and alpha.
//
// A surface stays a glow sheet from the first draw of a block until a draw
// into it that is not such a sprite (a 3D draw, a clear, an upload): from
// there it is drawn at full size again, as without this file, until the next
// block. Its full-size sheet and the small one do not share their content.
//
// Off: the same, and a draw elsewhere that samples the sheet and blends is not
// drawn. Game: none of this; every draw goes where it went before.

// The depth buffer bound with a glow sheet: no VRAM offset is this number.
constexpr std::uint32_t kGlowDepthAddress = 0xFFFFFFFEu;

struct Dx12GlowSheet {
    std::uint32_t address{};
    // In force: draws into the address go to `target` and draws that sample
    // the address read `target`.
    bool active{};
    Dx12FramebufferTarget target;
};

struct Dx12GlowStart {
    std::size_t batch{};       // the first draw of a block
    std::uint32_t address{};   // the surface it draws into
    // The surface the block's first sprite copies, and all that the block's
    // sprites read of it (logical pixels: left, top, right, bottom), which one
    // pass averages down. Not known when one of those sprites' rectangles is not.
    std::uint32_t source{};
    bool region_known{};
    std::array<float, 4> region{};
};

// No surface: a VRAM offset is never this number.
constexpr std::uint32_t kGlowNoSurface = 0xFFFFFFFFu;

struct Dx12Glow {
    bool ready{};
    bool off{};                // a draw that adds a sheet to another surface is left out
    bool magnify{};            // internal scale above 1: such a draw reads through the cubic filter
    std::uint32_t scale{1u};
    ComPtr<ID3D12PipelineState> box_pipeline;
    // A full-size surface averaged down to the PSP's size, for the draw that copies it into a sheet.
    Dx12FramebufferTarget source;
    // What `source` holds: the surface it is the average of (kGlowNoSurface:
    // of none) and the part of it; all of it when `averaged_whole`.
    std::uint32_t averaged{kGlowNoSurface};
    bool averaged_whole{};
    std::array<float, 4> averaged_region{};
    // Direct3D wants a depth buffer of the render target's size; the sprites neither test nor write it.
    Dx12DepthTarget depth;
    // The game has one such sheet. Pointers into the array are held while a frame is recorded.
    std::array<Dx12GlowSheet, 4> sheets;
    std::size_t sheet_count{};
    // The blocks of the submission being recorded.
    std::vector<GeGlowDraw> draws;
    std::vector<Dx12GlowStart> starts;
    std::size_t next_start{};
    // The block that began last.
    Dx12GlowStart block{};
    std::uint64_t blocks{};
    std::uint64_t readers{};
    std::uint64_t left_out{};
};

Dx12Glow &glow_state() {
    static Dx12Glow glow;
    return glow;
}

void destroy_glow() noexcept {
    glow_state() = Dx12Glow{};
}

// A draw that may go to a glow sheet: a through-mode sprite that neither tests
// nor writes depth. Anything else needs the game's own depth buffer or is not
// part of the effect.
bool glow_drawable(const Dx12Batch &batch) noexcept {
    const GeGpuDrawDescriptor &draw = batch.draw;
    return draw.through && draw.primitive == 6u && !batch.hardware_transform && !draw.depth_test_enabled &&
           !draw.clear_mode && draw.surface_write == GeGpuSurfaceWrite::None;
}

// The sheet in force for the surface at `address`, or null.
Dx12GlowSheet *glow_sheet(Dx12Glow &glow, std::uint32_t address) noexcept {
    for (std::size_t i = 0u; i < glow.sheet_count; ++i)
        if (glow.sheets[i].active && glow.sheets[i].address == address) return &glow.sheets[i];
    return nullptr;
}

// A single-sample colour surface of the sheet's logical size, readable and drawable.
bool create_glow_surface(Dx12GeState &s, Dx12FramebufferTarget &target, std::string &error) noexcept {
    if (s.next_rtv >= kFramebufferTargetCapacity || s.next_srv >= kSrvCapacity) {
        error = "no descriptor left for a glow sheet";
        return false;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC color{};
    color.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    color.Width = s.sheet_width;
    color.Height = s.sheet_height;
    color.DepthOrArraySize = 1u;
    color.MipLevels = 1u;
    color.Format = kColorFormat;
    color.SampleDesc.Count = 1u;
    color.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    color.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = kColorFormat;
    const HRESULT hr = s.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &color,
                                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                                                          IID_PPV_ARGS(&target.color));
    if (FAILED(hr)) { error = hr_text(hr, "CreateCommittedResource(glow sheet)"); return false; }
    target.rtv_index = s.next_rtv++;
    target.srv_index = s.next_srv++;
    s.device->CreateRenderTargetView(target.color.Get(), nullptr, rtv_cpu(s, target.rtv_index));
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = kColorFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1u;
    s.device->CreateShaderResourceView(target.color.Get(), &srv, srv_cpu(s, target.srv_index));
    target.logical_width = target.physical_width = s.sheet_width;
    target.logical_height = target.physical_height = s.sheet_height;
    target.color_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    target.resolve_dirty = {};
    target.snapshot_dirty = {0, 0, static_cast<LONG>(s.sheet_width), static_cast<LONG>(s.sheet_height)};
    return true;
}

// The sheet of the surface at `address`, created with its snapshot when it
// does not exist yet; null when it cannot be had (the surface is then drawn at
// full size, as the game's draws say).
Dx12GlowSheet *ensure_glow_sheet(Dx12GeState &s, Dx12Glow &glow, std::uint32_t address) noexcept {
    for (std::size_t i = 0u; i < glow.sheet_count; ++i)
        if (glow.sheets[i].address == address) return &glow.sheets[i];
    if (glow.sheet_count == glow.sheets.size()) return nullptr;
    chili::CostTimer cost(chili::frame_costs.surface_ns);
    ++chili::frame_costs.surfaces;
    Dx12GlowSheet &sheet = glow.sheets[glow.sheet_count];
    sheet = Dx12GlowSheet{};
    sheet.address = address;
    sheet.target.address = address;
    std::string error;
    if (!create_glow_surface(s, sheet.target, error) || !ensure_feedback_copy(s, sheet.target, error)) {
        runtime_log_error("dx12 glow sheet", error);
        sheet = Dx12GlowSheet{};
        return nullptr;
    }
    ++glow.sheet_count;
    runtime_log_line("dx12 glow sheet created address=0x" +
                     [&] { std::ostringstream hex; hex << std::hex << address; return hex.str(); }() +
                     " size=" + std::to_string(s.sheet_width) + 'x' + std::to_string(s.sheet_height));
    return &sheet;
}

// Creates what the smooth glow needs. Without it the game's own glow is drawn,
// so a failure leaves it off and `error` says why.
bool create_glow(Dx12GeState &s, std::string &error) noexcept {
    Dx12Glow &glow = glow_state();
    glow = Dx12Glow{};
    const GeGpuConfig::Glow mode = ge_gpu_config().glow;
    glow.scale = s.target_width / std::max<std::uint32_t>(1u, s.sheet_width);
    glow.magnify = glow.scale > 1u;
    glow.off = mode == GeGpuConfig::Glow::Off;
    // At scale 1 the game's passes are the PSP's own: nothing to do but leave the glow out when it is off.
    if (mode == GeGpuConfig::Glow::Game || (mode == GeGpuConfig::Glow::Smooth && !glow.magnify)) return true;
    if (glow.magnify && !s.cubic_pixel_shader) { error = "the cubic pixel shader is missing"; return false; }

    // The average of scale x scale pixels of the source for every pixel of the small surface.
    const char *source = R"HLSL(
Texture2D<float4> Source : register(t0);
cbuffer GlowBox : register(b1) {
    uint Scale;
};
float4 GlowBoxVS(uint id : SV_VertexID) : SV_POSITION {
    float2 corner = float2(float((id << 1u) & 2u), float(id & 2u));
    return float4(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0);
}
float4 GlowBoxPS(float4 position : SV_POSITION) : SV_TARGET {
    int2 base = int2(position.xy) * int(Scale);
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    [loop] for (uint y = 0u; y < Scale; ++y) {
        [loop] for (uint x = 0u; x < Scale; ++x)
            sum += Source.Load(int3(base + int2(int(x), int(y)), 0));
    }
    return sum / float(Scale * Scale);
}
)HLSL";
    ComPtr<ID3DBlob> vs, ps, errors;
    HRESULT hr = D3DCompile(source, std::strlen(source), "ChiliGlowBox", nullptr, nullptr, "GlowBoxVS", "vs_5_1",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0u, &vs, &errors);
    if (SUCCEEDED(hr)) {
        errors.Reset();
        hr = D3DCompile(source, std::strlen(source), "ChiliGlowBox", nullptr, nullptr, "GlowBoxPS", "ps_5_1",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0u, &ps, &errors);
    }
    if (FAILED(hr)) {
        error = errors ? std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize())
                       : hr_text(hr, "D3DCompile(glow box)");
        return false;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = s.root_signature.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
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
    hr = s.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&glow.box_pipeline));
    if (FAILED(hr)) { error = hr_text(hr, "CreateGraphicsPipelineState(glow box)"); return false; }

    if (!create_glow_surface(s, glow.source, error)) return false;

    if (s.next_dsv >= kFramebufferTargetCapacity) { error = "no descriptor left for the glow sheets' depth buffer"; return false; }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC depth{};
    depth.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depth.Width = s.sheet_width;
    depth.Height = s.sheet_height;
    depth.DepthOrArraySize = 1u;
    depth.MipLevels = 1u;
    depth.Format = s.depth_format;
    depth.SampleDesc.Count = 1u;
    depth.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depth.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE depth_clear{};
    depth_clear.Format = s.depth_format;
    hr = s.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &depth, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                           &depth_clear, IID_PPV_ARGS(&glow.depth.depth));
    if (FAILED(hr)) { error = hr_text(hr, "CreateCommittedResource(glow depth)"); return false; }
    glow.depth.dsv_index = s.next_dsv++;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = s.depth_format;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    s.device->CreateDepthStencilView(glow.depth.depth.Get(), &dsv, dsv_cpu(s, glow.depth.dsv_index));

    glow.draws.reserve(4096u);
    glow.starts.reserve(4u);
    glow.ready = true;
    return true;
}

// Looks through the submission's draws for glow blocks: where each begins, its
// surface becomes a glow sheet.
void plan_glow(Dx12GeState &s, Dx12Glow &glow) noexcept {
    glow.starts.clear();
    glow.next_start = 0u;
    glow.averaged = kGlowNoSurface;
    if (!glow.ready) return;
    try {
        glow.draws.clear();
        for (const Dx12Batch &batch : s.batches) {
            GeGlowDraw draw{};
            draw.target = batch.draw.framebuffer_address & kVramOffsetMask;
            draw.sprite = glow_drawable(batch);
            draw.samples_surface = batch.framebuffer_feedback && batch.draw.texture_enabled;
            draw.source = batch.feedback_address;
            // Only a sprite that copies another surface can begin a block: only its rectangles are needed.
            if (draw.sprite && draw.samples_surface && draw.source != draw.target && batch.feedback_region_known &&
                batch.vertex_count != 0u && !batch.indexed &&
                static_cast<std::size_t>(batch.first_vertex) + batch.vertex_count <= s.vertices.size()) {
                draw.rectangles_known = true;
                draw.texels = batch.feedback_region;
                draw.pixels = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                               std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
                for (std::uint32_t i = 0u; i < batch.vertex_count; ++i) {
                    const Dx12UploadVertex &vertex = s.vertices[batch.first_vertex + i];
                    draw.pixels[0] = std::min(draw.pixels[0], vertex.x);
                    draw.pixels[1] = std::min(draw.pixels[1], vertex.y);
                    draw.pixels[2] = std::max(draw.pixels[2], vertex.x);
                    draw.pixels[3] = std::max(draw.pixels[3], vertex.y);
                }
            }
            glow.draws.push_back(draw);
        }
        for (const GeGlowBlock &block : find_glow_blocks(glow.draws, s.display_framebuffer)) {
            Dx12GlowStart start{block.first, block.target, glow.draws[block.first].source, true,
                                glow.draws[block.first].texels};
            for (std::size_t i = block.first; i <= block.last && start.region_known; ++i) {
                const Dx12Batch &batch = s.batches[i];
                if (!batch.framebuffer_feedback || !batch.draw.texture_enabled ||
                    batch.feedback_address != start.source) continue;
                start.region_known = batch.feedback_region_known;
                start.region = {std::min(start.region[0], batch.feedback_region[0]),
                                std::min(start.region[1], batch.feedback_region[1]),
                                std::max(start.region[2], batch.feedback_region[2]),
                                std::max(start.region[3], batch.feedback_region[3])};
            }
            glow.starts.push_back(start);
            ++glow.blocks;
        }
    } catch (...) {
        glow.starts.clear();
    }
}

// Puts the sheets of the blocks that begin at or before the draw with index
// `batch_index` in force. True when a surface changed hands: the caller must
// look its render target up again.
bool start_due_glow(Dx12GeState &s, Dx12Glow &glow, std::size_t batch_index) noexcept {
    bool changed = false;
    for (; glow.next_start < glow.starts.size() && glow.starts[glow.next_start].batch <= batch_index; ++glow.next_start) {
        glow.block = glow.starts[glow.next_start];
        Dx12GlowSheet *sheet = ensure_glow_sheet(s, glow, glow.block.address);
        if (sheet == nullptr || sheet->active) continue;
        sheet->active = true;
        changed = true;
    }
    return changed;
}

// True when the glow's source surface holds the surface at `address` averaged
// down where the sprite `batch` reads it.
bool glow_averaged(const Dx12Glow &glow, std::uint32_t address, const Dx12Batch &batch) noexcept {
    const std::array<float, 4> &r = batch.feedback_region, &have = glow.averaged_region;
    return glow.averaged == address &&
           (glow.averaged_whole || (batch.feedback_region_known && r[0] >= have[0] && r[1] >= have[1] &&
                                    r[2] <= have[2] && r[3] <= have[3]));
}

void run_glow_box(Dx12GeState &s, Dx12Glow &glow, Dx12FramebufferTarget &from, bool region_known,
                  const std::array<float, 4> &region) noexcept;

// Averages `from` (the surface at `address`) down for the sprite `batch`: the
// part it reads, or, when it is one of the sprites of the block that began
// last, all that the block's sprites read of that surface. See run_glow_box
// for what the pass does to the command list.
void average_for_glow(Dx12GeState &s, Dx12Glow &glow, Dx12FramebufferTarget &from, std::uint32_t address,
                      const Dx12Batch &batch) noexcept {
    std::array<float, 4> region = batch.feedback_region;
    const std::array<float, 4> &all = glow.block.region;
    if (batch.feedback_region_known && glow.block.source == address && glow.block.region_known &&
        region[0] >= all[0] && region[1] >= all[1] && region[2] <= all[2] && region[3] <= all[3])
        region = all;
    run_glow_box(s, glow, from, batch.feedback_region_known, region);
    glow.averaged = address;
    glow.averaged_whole = !batch.feedback_region_known;
    glow.averaged_region = region;
}

// Fills the glow's source surface with `from` averaged over scale x scale
// pixels, inside `region` (logical pixels: left, top, right, bottom) and one
// pixel around it, which a linear filter reads at the edge. `from` must not be
// bound for drawing. Afterwards the command list has another render target,
// viewport (the sheet's size), scissor, pipeline, texture and pixel constants
// than the caller's cache says.
void run_glow_box(Dx12GeState &s, Dx12Glow &glow, Dx12FramebufferTarget &from, bool region_known,
                  const std::array<float, 4> &region) noexcept {
    resolve_target_for_sampling(s, from, false);
    transition(s.list.Get(), glow.source.color.Get(), glow.source.color_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    glow.source.color_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_cpu(s, glow.source.rtv_index);
    s.list->OMSetRenderTargets(1u, &rtv, FALSE, nullptr);
    const LONG width = static_cast<LONG>(s.sheet_width), height = static_cast<LONG>(s.sheet_height);
    D3D12_RECT scissor{0, 0, width, height};
    if (region_known) {
        const auto bound = [](float value, LONG limit) {
            return static_cast<LONG>(std::clamp(value, 0.0f, static_cast<float>(limit)));
        };
        scissor = {bound(std::floor(region[0]) - 1.0f, width), bound(std::floor(region[1]) - 1.0f, height),
                   bound(std::ceil(region[2]) + 1.0f, width), bound(std::ceil(region[3]) + 1.0f, height)};
        if (rect_empty(scissor)) scissor = {0, 0, width, height};
    }
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    s.list->RSSetViewports(1u, &viewport);
    s.list->RSSetScissorRects(1u, &scissor);
    s.list->SetPipelineState(glow.box_pipeline.Get());
    s.list->SetGraphicsRootDescriptorTable(0u, srv_gpu(s, from.srv_index));
    std::array<std::uint32_t, kPixelConstantWords> constants{};
    constants[0] = glow.scale;
    s.list->SetGraphicsRoot32BitConstants(3u, kPixelConstantWords, constants.data(), 0u);
    s.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s.list->DrawInstanced(3u, 1u, 0u, 0u);
    transition(s.list.Get(), glow.source.color.Get(), glow.source.color_state,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    glow.source.color_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}
