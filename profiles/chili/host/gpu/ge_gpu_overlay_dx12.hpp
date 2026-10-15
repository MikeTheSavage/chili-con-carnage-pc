// Chili: the overlay pass of the Direct3D 12 renderer: a host page (settings
// inside the game) drawn over the game's picture. This file is part of
// ge_gpu_backend_dx12.cpp and is included inside its anonymous namespace,
// before the direct present. What it is and what the host hands over is in
// ge_gpu_backend.hpp ("the overlay pass").
//
// The page is a list of triangles in PSP pixels. They are drawn on the swap
// chain's back buffer right after the game's picture was put there, with the
// viewport on the picture's rectangle, so a PSP pixel of the page is the same
// window pixels as that PSP pixel of the game's picture at any window size and
// any internal scale. The back buffer always has one sample, so multisampling
// of the game's surfaces makes no difference here.
//
// Textures are the renderer's own cache entries, found by content key: the
// font sheets, the menu's sprites and backgrounds as they are in force (a
// mod's picture, the button prompts' cells). An entry a page draws with is
// marked as used, so the cache limit does not take it; an image that leaves the
// cache for another reason stays alive until the frames that sample it are
// done, like any texture a game draw used.
//
// With no page set and no window capture asked for, record_overlay() is one
// atomic load and one test, and the command list is what it was before.

constexpr std::size_t kOverlayTextureCapacity = 16u;
constexpr std::uint32_t kOverlayInitialVertices = 16384u;     // 320 KB
constexpr std::uint32_t kOverlayVertexLimit = 1u << 20u;      // 20 MB: far above any page

// What crosses threads: the page (set by the game thread, taken at a present)
// and which watched textures are in the cache (kept by the thread that runs
// the lists, read by the game thread).
struct Dx12OverlayShared {
    std::mutex mutex;
    std::shared_ptr<const GeGpuOverlay> list;
    std::atomic<bool> set{false};
    // Written before the first draw, then only read.
    std::array<std::uint64_t, kOverlayTextureCapacity> watched{};
    std::atomic<std::uint32_t> watched_count{0u};
    // Cache entries with that content, per watched key.
    std::array<std::atomic<std::int32_t>, kOverlayTextureCapacity> cached{};
};

Dx12OverlayShared &overlay_shared() {
    static Dx12OverlayShared shared;
    return shared;
}

struct Dx12OverlayBuffer {
    ComPtr<ID3D12Resource> buffer;
    std::byte *mapped{};
    std::uint32_t capacity{};   // vertices
};

struct Dx12Overlay {
    bool ready{};
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    // One per frame in flight, and one for the read-back, which waits for the card.
    std::array<Dx12OverlayBuffer, kFrameCount> vertices;
    Dx12OverlayBuffer capture_vertices;
    // The displayed surface with the page, at the internal resolution.
    ComPtr<ID3D12Resource> capture_target;
    ComPtr<ID3D12DescriptorHeap> capture_rtv;
    std::uint32_t capture_width{}, capture_height{};
    ComPtr<ID3D12Resource> capture_readback;
    UINT64 capture_readback_bytes{};
    // The back buffer of one present.
    bool window_requested{};
    bool window_recorded{};
    ComPtr<ID3D12Resource> window_readback;
    UINT64 window_readback_bytes{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT window_footprint{};
    chili::PictureRect window_picture{};
    // Time the card spends on the page: two timestamps per frame slot.
    ComPtr<ID3D12QueryHeap> query_heap;
    ComPtr<ID3D12Resource> query_readback;
    std::array<bool, kFrameCount> timed{};
    double gpu_total_ms{};
    GeGpuOverlayStats stats{};
    // The textures the last page named and where the cache has them (0: nowhere). The cache is read
    // through again only when a keyed texture entered or left it, or the page names other textures; in
    // between each entry is looked up by its cache key and checked.
    struct Found {
        std::uint64_t key{};
        std::uint64_t cache_key{};
        Dx12Texture *texture{};   // of this recording only
    };
    std::vector<Found> found;
    std::uint64_t cache_generation{1u};
    std::uint64_t found_generation{};
};

Dx12Overlay &overlay_state() {
    static Dx12Overlay overlay;
    return overlay;
}

// A texture with a content key entered (+1) or left (-1) the cache.
void overlay_note_texture(std::uint64_t content_key, std::int32_t change) noexcept {
    if (content_key == 0u) return;
    ++overlay_state().cache_generation;
    Dx12OverlayShared &shared = overlay_shared();
    const std::uint32_t count = shared.watched_count.load(std::memory_order_relaxed);
    for (std::uint32_t i = 0u; i < count; ++i) {
        if (shared.watched[i] != content_key) continue;
        shared.cached[i].fetch_add(change, std::memory_order_relaxed);
        return;
    }
}

const char *overlay_shader_source() noexcept {
    return R"HLSL(
cbuffer OverlayConstants : register(b0) {
    uint Flags;             // 1 textured, 2 nearest texel, 4 the texture's alpha only
    float AlphaReference;   // texels with alpha at or below it are not drawn
};
Texture2D<float4> Picture : register(t0);
SamplerState Smooth : register(s0);
SamplerState Nearest : register(s1);
struct OverlayVertex {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 colour : COLOR0;
};
// PSP pixels to the viewport, which is the picture's rectangle.
OverlayVertex OverlayVS(float2 position : POSITION, float2 uv : TEXCOORD0, float4 colour : COLOR0) {
    OverlayVertex output;
    output.position = float4(position.x / 240.0 - 1.0, 1.0 - position.y / 136.0, 0.0, 1.0);
    output.uv = uv;
    output.colour = colour;
    return output;
}
float4 OverlayPS(OverlayVertex input) : SV_TARGET {
    float4 colour = input.colour;
    if ((Flags & 1u) != 0u) {
        float4 smooth = Picture.Sample(Smooth, input.uv);
        float4 nearest = Picture.Sample(Nearest, input.uv);
        float4 texel = (Flags & 2u) != 0u ? nearest : smooth;
        if (texel.a <= AlphaReference) discard;
        colour = (Flags & 4u) != 0u ? float4(colour.rgb, colour.a * texel.a) : colour * texel;
    }
    return colour;
}
)HLSL";
}

void destroy_overlay() noexcept {
    Dx12Overlay &overlay = overlay_state();
    const GeGpuOverlayStats kept = overlay.stats;
    const double gpu_total = overlay.gpu_total_ms;
    overlay = Dx12Overlay{};
    // The numbers of a run are read after the renderer has shut down.
    overlay.stats = kept;
    overlay.stats.available = false;
    overlay.gpu_total_ms = gpu_total;
    Dx12OverlayShared &shared = overlay_shared();
    for (auto &count : shared.cached) count.store(0, std::memory_order_relaxed);
}

// Creates the pass. A failure leaves it off and the game drawing as without
// it; `error` says why.
bool create_overlay(Dx12GeState &s, std::string &error) noexcept {
    Dx12Overlay &overlay = overlay_state();
    overlay = Dx12Overlay{};

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1u;
    range.BaseShaderRegister = 0u;
    std::array<D3D12_ROOT_PARAMETER, 2> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 1u;
    parameters[0].DescriptorTable.pDescriptorRanges = &range;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.ShaderRegister = 0u;
    parameters[1].Constants.Num32BitValues = 2u;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    std::array<D3D12_STATIC_SAMPLER_DESC, 2> samplers{};
    for (UINT i = 0u; i < 2u; ++i) {
        samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].MaxAnisotropy = 1u;
        samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = i;
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    // Between two levels of a sheet the sharper one counts for more: text.
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].MipLODBias = -0.5f;
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = static_cast<UINT>(parameters.size());
    desc.pParameters = parameters.data();
    desc.NumStaticSamplers = static_cast<UINT>(samplers.size());
    desc.pStaticSamplers = samplers.data();
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> blob, errors;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    if (FAILED(hr)) { error = hr_text(hr, "D3D12SerializeRootSignature(overlay)"); return false; }
    hr = s.device->CreateRootSignature(0u, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&overlay.root));
    if (FAILED(hr)) { error = hr_text(hr, "CreateRootSignature(overlay)"); return false; }

    const char *source = overlay_shader_source();
    ComPtr<ID3DBlob> vs, ps;
    const auto compile = [&](const char *entry, const char *profile, ComPtr<ID3DBlob> &out) {
        ComPtr<ID3DBlob> messages;
        const HRESULT compiled = D3DCompile(source, std::strlen(source), "ChiliOverlay", nullptr, nullptr, entry, profile,
                                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0u, &out, &messages);
        if (SUCCEEDED(compiled)) return true;
        error = messages ? std::string(static_cast<const char *>(messages->GetBufferPointer()), messages->GetBufferSize())
                         : hr_text(compiled, "D3DCompile(overlay)");
        return false;
    };
    if (!compile("OverlayVS", "vs_5_0", vs) || !compile("OverlayPS", "ps_5_0", ps)) return false;

    static const D3D12_INPUT_ELEMENT_DESC layout[]{
        {"POSITION", 0u, DXGI_FORMAT_R32G32_FLOAT, 0u, 0u, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0u},
        {"TEXCOORD", 0u, DXGI_FORMAT_R32G32_FLOAT, 0u, 8u, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0u},
        {"COLOR", 0u, DXGI_FORMAT_R8G8B8A8_UNORM, 0u, 16u, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0u}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = overlay.root.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.InputLayout = {layout, static_cast<UINT>(std::size(layout))};
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    // Straight alpha over the picture, as the game blends its menu text.
    D3D12_RENDER_TARGET_BLEND_DESC &blend = pso.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1u;
    pso.RTVFormats[0] = kColorFormat;
    pso.SampleDesc.Count = 1u;
    hr = s.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&overlay.pipeline));
    if (FAILED(hr)) { error = hr_text(hr, "CreateGraphicsPipelineState(overlay)"); return false; }

    // Timing is a diagnostic: without it the pass draws the same.
    if (s.timestamp_frequency != 0u) {
        D3D12_QUERY_HEAP_DESC heap{};
        heap.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        heap.Count = kFrameCount * 2u;
        D3D12_RESOURCE_DESC readback{};
        readback.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        readback.Width = kFrameCount * 2u * sizeof(UINT64);
        readback.Height = 1u;
        readback.DepthOrArraySize = 1u;
        readback.MipLevels = 1u;
        readback.SampleDesc.Count = 1u;
        readback.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES readback_heap{};
        readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
        if (FAILED(s.device->CreateQueryHeap(&heap, IID_PPV_ARGS(&overlay.query_heap))) ||
            FAILED(s.device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback, D3D12_RESOURCE_STATE_COPY_DEST,
                                                     nullptr, IID_PPV_ARGS(&overlay.query_readback)))) {
            overlay.query_heap.Reset();
            overlay.query_readback.Reset();
        }
    }
    overlay.found.reserve(2u * kOverlayTextureCapacity);
    overlay.ready = true;
    overlay.stats.available = true;
    return true;
}

// Room for `count` vertices in `buffer`. A buffer that is replaced stays alive
// until the frames that may still read it are done.
bool overlay_reserve(Dx12GeState &s, Dx12OverlayBuffer &buffer, std::size_t count) noexcept {
    if (buffer.buffer && buffer.capacity >= count) return true;
    if (count > kOverlayVertexLimit) return false;
    const std::uint32_t capacity = std::max<std::uint32_t>(kOverlayInitialVertices, std::bit_ceil(static_cast<std::uint32_t>(count)));
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<UINT64>(capacity) * sizeof(GeGpuOverlayVertex);
    desc.Height = 1u;
    desc.DepthOrArraySize = 1u;
    desc.MipLevels = 1u;
    desc.SampleDesc.Count = 1u;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    ComPtr<ID3D12Resource> created;
    HRESULT hr = s.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&created));
    void *mapped = nullptr;
    const D3D12_RANGE no_read{0u, 0u};
    if (SUCCEEDED(hr)) hr = created->Map(0u, &no_read, &mapped);
    if (FAILED(hr) || mapped == nullptr) {
        runtime_log_error("dx12 overlay", hr_text(hr, "vertex buffer"));
        return false;
    }
    if (buffer.buffer)
        for (Dx12FrameResources &frame : s.frames) frame.transient_resources.push_back(buffer.buffer);
    buffer.buffer = std::move(created);
    buffer.mapped = static_cast<std::byte *>(mapped);
    buffer.capacity = capacity;
    return true;
}

// The cache entries of the textures `page` names, in `overlay.found`. Reading through the whole cache
// costs a memory miss per texture (tens of microseconds in a mission), so it is done only when a keyed
// texture entered or left the cache or the page names another set; the number of entries per watched key
// is put right on the way. Of several entries with one content (the game draws a texture with two sampler
// states) the one used last is taken. An image still waiting for its pixels cannot be sampled yet.
void overlay_find_textures(Dx12GeState &s, Dx12Overlay &overlay, const GeGpuOverlay &page) noexcept {
    // The textures the page names, each once, in the order they come.
    std::array<std::uint64_t, 2u * kOverlayTextureCapacity> keys{};
    std::size_t named = 0u;
    for (const GeGpuOverlayRun &run : page.runs) {
        if (run.texture == 0u || std::find(keys.begin(), keys.begin() + named, run.texture) != keys.begin() + named) continue;
        if (named == keys.size()) break;   // more than a page has: the runs of the rest are left out
        keys[named++] = run.texture;
    }
    bool same = overlay.found_generation == overlay.cache_generation && overlay.found.size() == named;
    for (std::size_t i = 0u; same && i < named; ++i) same = overlay.found[i].key == keys[i];
    const auto usable = [](const Dx12Texture &texture) { return texture.image && !texture.upload_pending; };
    if (same) {
        for (Dx12Overlay::Found &entry : overlay.found) {
            entry.texture = nullptr;
            if (entry.cache_key == 0u) continue;
            const auto cached = s.textures.find(entry.cache_key);
            if (cached != s.textures.end() && cached->second.content_key == entry.key && usable(cached->second)) entry.texture = &cached->second;
        }
        return;
    }
    overlay.found.clear();
    for (std::size_t i = 0u; i < named; ++i) overlay.found.push_back({keys[i], 0u, nullptr});
    Dx12OverlayShared &shared = overlay_shared();
    const std::uint32_t watched = shared.watched_count.load(std::memory_order_relaxed);
    std::array<std::int32_t, kOverlayTextureCapacity> counts{};
    for (auto &[cache_key, texture] : s.textures) {
        if (texture.content_key == 0u) continue;
        for (std::uint32_t i = 0u; i < watched; ++i)
            if (shared.watched[i] == texture.content_key) { ++counts[i]; break; }
        for (Dx12Overlay::Found &entry : overlay.found) {
            if (entry.key != texture.content_key) continue;
            if (usable(texture)) {
                if (entry.texture == nullptr || texture.last_used_epoch > entry.texture->last_used_epoch) {
                    entry.texture = &texture;
                    entry.cache_key = cache_key;
                }
            } else if (entry.cache_key == 0u) {
                // Remembered: it gets its pixels without the cache changing.
                entry.cache_key = cache_key;
            }
            break;
        }
    }
    for (std::uint32_t i = 0u; i < watched; ++i) shared.cached[i].store(counts[i], std::memory_order_relaxed);
    overlay.found_generation = overlay.cache_generation;
}

// Records `page` into `list`, whose render target is bound: `picture` is the
// picture's rectangle in that target, in its pixels. The textures it draws
// with count as used in this frame.
void draw_overlay(Dx12GeState &s, Dx12Overlay &overlay, ID3D12GraphicsCommandList *list, const GeGpuOverlay &page,
                  Dx12OverlayBuffer &buffer, const chili::PictureRect &picture) noexcept {
    if (page.runs.empty() || page.vertices.empty() || picture.empty()) return;
    if (!overlay_reserve(s, buffer, page.vertices.size())) return;
    std::memcpy(buffer.mapped, page.vertices.data(), page.vertices.size() * sizeof(GeGpuOverlayVertex));
    overlay_find_textures(s, overlay, page);

    const D3D12_VIEWPORT viewport{static_cast<float>(picture.x), static_cast<float>(picture.y), static_cast<float>(picture.width),
                                  static_cast<float>(picture.height), 0.0f, 1.0f};
    const D3D12_RECT whole{picture.x, picture.y, picture.x + picture.width, picture.y + picture.height};
    list->RSSetViewports(1u, &viewport);
    list->RSSetScissorRects(1u, &whole);
    list->SetPipelineState(overlay.pipeline.Get());
    list->SetGraphicsRootSignature(overlay.root.Get());
    ID3D12DescriptorHeap *heaps[]{s.srv_heap.Get(), s.sampler_heap.Get()};
    list->SetDescriptorHeaps(2u, heaps);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VERTEX_BUFFER_VIEW view{buffer.buffer->GetGPUVirtualAddress(),
                                        static_cast<UINT>(page.vertices.size() * sizeof(GeGpuOverlayVertex)),
                                        static_cast<UINT>(sizeof(GeGpuOverlayVertex))};
    list->IASetVertexBuffers(0u, 1u, &view);

    std::uint32_t bound_srv = 0xFFFFFFFFu;
    std::array<std::uint32_t, 2> bound_constants{0xFFFFFFFFu, 0u};
    bool clipped = false;
    for (const GeGpuOverlayRun &run : page.runs) {
        if (run.vertex_count < 3u || run.first_vertex > page.vertices.size() ||
            run.vertex_count > page.vertices.size() - run.first_vertex) continue;
        std::uint32_t srv = 0u;   // descriptor 0 is the null texture
        std::uint32_t flags = 0u;
        if (run.texture != 0u) {
            const auto entry = std::find_if(overlay.found.begin(), overlay.found.end(),
                                            [&](const Dx12Overlay::Found &item) { return item.key == run.texture; });
            if (entry == overlay.found.end() || entry->texture == nullptr) {
                ++overlay.stats.runs_without_texture;
                continue;
            }
            entry->texture->last_used_epoch = s.frame_epoch;
            srv = entry->texture->srv_index;
            flags = 1u | ((run.flags & kGeGpuOverlayNearest) != 0u ? 2u : 0u) | ((run.flags & kGeGpuOverlaySilhouette) != 0u ? 4u : 0u);
        }
        if (run.clipped || clipped) {
            D3D12_RECT scissor = whole;
            if (run.clipped) {
                // Whole pixels whose middle is inside the clip, as the rasterizer counts them.
                const float sx = picture.scale_x(), sy = picture.scale_y();
                scissor.left = std::max<LONG>(whole.left, picture.x + static_cast<LONG>(std::lround(run.clip[0] * sx)));
                scissor.top = std::max<LONG>(whole.top, picture.y + static_cast<LONG>(std::lround(run.clip[1] * sy)));
                scissor.right = std::min<LONG>(whole.right, picture.x + static_cast<LONG>(std::lround(run.clip[2] * sx)));
                scissor.bottom = std::min<LONG>(whole.bottom, picture.y + static_cast<LONG>(std::lround(run.clip[3] * sy)));
                if (rect_empty(scissor)) continue;
            }
            list->RSSetScissorRects(1u, &scissor);
            clipped = run.clipped;
        }
        if (srv != bound_srv) {
            list->SetGraphicsRootDescriptorTable(0u, srv_gpu(s, srv));
            bound_srv = srv;
        }
        const float reference = run.alpha_reference != 0u ? static_cast<float>(run.alpha_reference) / 255.0f : -1.0f;
        const std::array<std::uint32_t, 2> constants{flags, std::bit_cast<std::uint32_t>(reference)};
        if (constants != bound_constants) {
            list->SetGraphicsRoot32BitConstants(1u, 2u, constants.data(), 0u);
            bound_constants = constants;
        }
        list->DrawInstanced(run.vertex_count, 1u, run.first_vertex, 0u);
        ++overlay.stats.runs;
        overlay.stats.vertices += run.vertex_count;
    }
}

std::shared_ptr<const GeGpuOverlay> overlay_take_page() {
    Dx12OverlayShared &shared = overlay_shared();
    if (!shared.set.load(std::memory_order_acquire)) return {};
    const std::lock_guard<std::mutex> lock(shared.mutex);
    return shared.list;
}

// The timestamps of this slot's last page, readable now that its frame is done.
void overlay_read_gpu_time(Dx12GeState &s, Dx12Overlay &overlay) noexcept {
    if (!overlay.query_readback || !overlay.timed[s.frame_cursor]) return;
    overlay.timed[s.frame_cursor] = false;
    const SIZE_T offset = static_cast<SIZE_T>(s.frame_cursor) * 2u * sizeof(UINT64);
    const D3D12_RANGE read{offset, offset + 2u * sizeof(UINT64)};
    void *mapped = nullptr;
    if (FAILED(overlay.query_readback->Map(0u, &read, &mapped)) || mapped == nullptr) return;
    UINT64 ticks[2];
    std::memcpy(ticks, static_cast<const std::byte *>(mapped) + offset, sizeof(ticks));
    const D3D12_RANGE none{0u, 0u};
    overlay.query_readback->Unmap(0u, &none);
    if (ticks[1] < ticks[0]) return;
    const double ms = static_cast<double>(ticks[1] - ticks[0]) * 1000.0 / static_cast<double>(s.timestamp_frequency);
    ++overlay.stats.gpu_samples;
    overlay.gpu_total_ms += ms;
    overlay.stats.gpu_mean_ms = overlay.gpu_total_ms / static_cast<double>(overlay.stats.gpu_samples);
    overlay.stats.gpu_max_ms = std::max(overlay.stats.gpu_max_ms, ms);
}

// In the direct present, after the game's picture is on the back buffer: the
// page, and a copy of the back buffer when the window's picture was asked for.
// `state` is the state the back buffer is in, on entry and on return.
void record_overlay(Dx12GeState &s, ID3D12Resource *backbuffer, const chili::PictureRect &picture,
                    D3D12_RESOURCE_STATES &state) noexcept {
    Dx12Overlay &overlay = overlay_state();
    if (!overlay_shared().set.load(std::memory_order_acquire) && !overlay.window_requested) return;
    if (overlay.ready) {
        overlay_read_gpu_time(s, overlay);
        if (const std::shared_ptr<const GeGpuOverlay> page = overlay_take_page()) {
            const auto started = std::chrono::steady_clock::now();
            const std::uint64_t runs_before = overlay.stats.runs;
            if (overlay.query_heap) s.list->EndQuery(overlay.query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, s.frame_cursor * 2u);
            draw_overlay(s, overlay, s.list.Get(), *page, overlay.vertices[s.frame_cursor], picture);
            if (overlay.query_heap) {
                s.list->EndQuery(overlay.query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, s.frame_cursor * 2u + 1u);
                s.list->ResolveQueryData(overlay.query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, s.frame_cursor * 2u, 2u,
                                         overlay.query_readback.Get(), static_cast<UINT64>(s.frame_cursor) * 2u * sizeof(UINT64));
                overlay.timed[s.frame_cursor] = overlay.stats.runs != runs_before;
            }
            const std::uint64_t ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
            ++overlay.stats.presents;
            overlay.stats.record_ns += ns;
            overlay.stats.record_max_ns = std::max(overlay.stats.record_max_ns, ns);
        }
    }
    if (!overlay.window_requested) return;
    // The back buffer as the window is about to get it.
    const D3D12_RESOURCE_DESC desc = backbuffer->GetDesc();
    UINT64 total = 0u;
    s.device->GetCopyableFootprints(&desc, 0u, 1u, 0u, &overlay.window_footprint, nullptr, nullptr, &total);
    if (!overlay.window_readback || overlay.window_readback_bytes < total) {
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = std::max<UINT64>(total, 256u);
        buffer.Height = 1u;
        buffer.DepthOrArraySize = 1u;
        buffer.MipLevels = 1u;
        buffer.SampleDesc.Count = 1u;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        overlay.window_readback.Reset();
        overlay.window_readback_bytes = 0u;
        const HRESULT hr = s.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                                             nullptr, IID_PPV_ARGS(&overlay.window_readback));
        if (FAILED(hr)) {
            runtime_log_error("dx12 overlay", hr_text(hr, "window capture buffer"));
            overlay.window_requested = false;
            return;
        }
        overlay.window_readback_bytes = buffer.Width;
    }
    transition(s.list.Get(), backbuffer, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = backbuffer;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = overlay.window_readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = overlay.window_footprint;
    s.list->CopyTextureRegion(&destination, 0u, 0u, 0u, &source, nullptr);
    overlay.window_picture = picture;
    overlay.window_requested = false;
    overlay.window_recorded = true;
}

// Tightly packed RGBA8 rows out of a read-back buffer.
bool overlay_read_rows(ID3D12Resource *buffer, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint, UINT64 bytes,
                       std::uint32_t width, std::uint32_t height, std::vector<std::byte> &rgba8) noexcept {
    void *mapped = nullptr;
    const D3D12_RANGE read{0u, static_cast<SIZE_T>(bytes)};
    const HRESULT hr = buffer->Map(0u, &read, &mapped);
    if (FAILED(hr) || mapped == nullptr) {
        runtime_log_error("dx12 overlay", hr_text(hr, "Map(read-back)"));
        return false;
    }
    const D3D12_RANGE none{0u, 0u};
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4u;
    try {
        rgba8.resize(row_bytes * height);
    } catch (...) {
        buffer->Unmap(0u, &none);
        return false;
    }
    const auto *source = static_cast<const std::byte *>(mapped) + footprint.Offset;
    for (std::uint32_t y = 0u; y < height; ++y)
        std::memcpy(rgba8.data() + static_cast<std::size_t>(y) * row_bytes,
                    source + static_cast<std::size_t>(y) * footprint.Footprint.RowPitch, row_bytes);
    buffer->Unmap(0u, &none);
    return true;
}

// The LCD's part of `target` with the page over it, at the internal
// resolution. Nothing may be queued and the card must be idle (the caller
// flushed and waited).
bool overlay_read_display(Dx12GeState &s, Dx12FramebufferTarget &target, std::vector<std::byte> &rgba8,
                          std::uint32_t &width, std::uint32_t &height) noexcept {
    Dx12Overlay &overlay = overlay_state();
    const std::uint32_t scale = std::max<std::uint32_t>(1u, s.target_width / std::max<std::uint32_t>(1u, s.sheet_width));
    width = std::min<std::uint32_t>(kReferenceWidth * scale, s.target_width);
    height = std::min<std::uint32_t>(kReferenceHeight * scale, s.target_height);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1u;
    desc.MipLevels = 1u;
    desc.Format = kColorFormat;
    desc.SampleDesc.Count = 1u;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 total = 0u;
    s.device->GetCopyableFootprints(&desc, 0u, 1u, 0u, &footprint, nullptr, nullptr, &total);
    HRESULT hr = S_OK;
    if (!overlay.capture_target || overlay.capture_width != width || overlay.capture_height != height) {
        overlay.capture_target.Reset();
        overlay.capture_readback.Reset();
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        hr = s.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&overlay.capture_target));
        if (SUCCEEDED(hr) && !overlay.capture_rtv) {
            D3D12_DESCRIPTOR_HEAP_DESC rtv{};
            rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            rtv.NumDescriptors = 1u;
            hr = s.device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&overlay.capture_rtv));
        }
        if (SUCCEEDED(hr)) {
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = std::max<UINT64>(total, 256u);
            buffer.Height = 1u;
            buffer.DepthOrArraySize = 1u;
            buffer.MipLevels = 1u;
            buffer.SampleDesc.Count = 1u;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES readback{};
            readback.Type = D3D12_HEAP_TYPE_READBACK;
            hr = s.device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&overlay.capture_readback));
            overlay.capture_readback_bytes = buffer.Width;
        }
        if (FAILED(hr)) {
            runtime_log_error("dx12 overlay", hr_text(hr, "read-back target"));
            overlay.capture_target.Reset();
            overlay.capture_readback.Reset();
            return false;
        }
        overlay.capture_width = width;
        overlay.capture_height = height;
        s.device->CreateRenderTargetView(overlay.capture_target.Get(), nullptr,
                                         overlay.capture_rtv->GetCPUDescriptorHandleForHeapStart());
    }

    hr = s.texture_allocator->Reset();
    if (SUCCEEDED(hr)) hr = s.texture_list->Reset(s.texture_allocator.Get(), nullptr);
    if (FAILED(hr)) {
        runtime_log_error("dx12 overlay", hr_text(hr, "CommandList::Reset(read-back)"));
        return false;
    }
    ID3D12GraphicsCommandList *list = s.texture_list.Get();
    // After a submission every surface's single-sample image is resolved and
    // shader-readable, multisampled or not.
    transition(list, target.color.Get(), target.color_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION surface{};
    surface.pResource = target.color.Get();
    surface.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION picture{};
    picture.pResource = overlay.capture_target.Get();
    picture.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box{0u, 0u, 0u, width, height, 1u};
    list->CopyTextureRegion(&picture, 0u, 0u, 0u, &surface, &box);
    transition(list, target.color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, target.color_state);
    transition(list, overlay.capture_target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (overlay.ready) {
        if (const std::shared_ptr<const GeGpuOverlay> page = overlay_take_page()) {
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv = overlay.capture_rtv->GetCPUDescriptorHandleForHeapStart();
            list->OMSetRenderTargets(1u, &rtv, FALSE, nullptr);
            draw_overlay(s, overlay, list, *page, overlay.capture_vertices,
                         chili::PictureRect{0, 0, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)});
        }
    }
    transition(list, overlay.capture_target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = overlay.capture_readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    list->CopyTextureRegion(&destination, 0u, 0u, 0u, &picture, nullptr);
    transition(list, overlay.capture_target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    hr = list->Close();
    if (FAILED(hr)) {
        runtime_log_error("dx12 overlay", hr_text(hr, "CommandList::Close(read-back)"));
        return false;
    }
    ID3D12CommandList *lists[]{list};
    s.queue->ExecuteCommandLists(1u, lists);
    std::string error;
    if (!wait_for_gpu(s, error)) {
        runtime_log_error("dx12 overlay", error);
        return false;
    }
    return overlay_read_rows(overlay.capture_readback.Get(), footprint, total, width, height, rgba8);
}
