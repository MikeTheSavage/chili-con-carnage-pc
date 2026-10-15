// Chili: post-process passes of the Direct3D 12 renderer: anti-aliasing (FXAA)
// and ambient occlusion of the 3D scene. This file is part of
// ge_gpu_backend_dx12.cpp and is included inside its anonymous namespace,
// after the surface helpers and before the frame submission.
//
// The game draws a mission frame into a scene surface (sky, world, characters,
// then decals and particles), copies that surface into a second one through a
// handful of sprites, and draws bloom and HUD there. Both passes work on the
// scene surface while it holds nothing but the 3D picture:
//
//   ambient occlusion  after the last draw that writes depth (the end of the
//                      opaque world), so particles and glows drawn afterwards
//                      are not darkened. The game draws plants and fences as
//                      blended cut-outs whose nearly transparent fringe still
//                      writes depth; with ambient occlusion on, a depth-writing
//                      3D draw also writes its alpha into the surface's alpha
//                      plane (which the game leaves unused there). A pixel
//                      whose alpha is below one belongs to a cut-out: what
//                      lies behind it is unknown, so it is not darkened, and
//                      the darkening fades out towards it so that no edge
//                      shows where the fringe begins
//   FXAA               just before the first draw that samples the scene from
//                      another surface, so the HUD and the text, which go onto
//                      that other surface, are never touched
//
// A surface counts as a scene when it received at least kPostMinSceneDraws
// depth-writing, GPU-transformed draws before it is sampled; menus and movies
// never do, so they are left alone.

constexpr std::uint32_t kPostMinSceneDraws = 30u;
constexpr DXGI_FORMAT kAoFormat = DXGI_FORMAT_R16G16_FLOAT;
constexpr UINT kPostConstantWords = 24u;

struct Dx12PostEvent {
    std::size_t ao_after{};        // index of the last depth-writing scene draw
    std::size_t before{};          // index of the first draw that samples the scene
    std::uint32_t scene_address{};
    std::uint32_t depth_address{};
    GeGpuHardwareTransform camera{};
    D3D12_RECT region{};           // physical pixels of the scene surface
    bool ao_done{};
};

struct Dx12PostSurface {
    ComPtr<ID3D12Resource> image;
    std::uint32_t srv_index{};
    UINT rtv_index{};
    std::uint32_t width{};
    std::uint32_t height{};
    D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
};

struct Dx12Post {
    bool ready{};
    bool fxaa{};
    bool ao{};
    bool ao_high{};
    bool ao_debug{};
    std::uint32_t ao_step{1u};     // scene pixels per occlusion pixel
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> fxaa_pipeline;
    ComPtr<ID3D12PipelineState> prep_pipeline;
    ComPtr<ID3D12PipelineState> ao_pipeline;
    ComPtr<ID3D12PipelineState> blur_pipeline;
    ComPtr<ID3D12PipelineState> apply_pipeline;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    UINT rtv_stride{};
    Dx12PostSurface color_copy;
    // Distance from the camera per scene pixel: 1e6 for sky, negative for the
    // transparent fringe of a cut-out.
    Dx12PostSurface linear_depth;
    std::array<Dx12PostSurface, 2> ao_surface;
    std::vector<Dx12PostEvent> events;
    std::size_t next_event{};
    std::uint64_t fxaa_passes{};
    std::uint64_t ao_passes{};
};

Dx12Post &post_state() {
    static Dx12Post post;
    return post;
}

// True when the depth buffers must be readable by a shader.
bool post_reads_depth() noexcept {
    return ge_gpu_config().ambient_occlusion != GeGpuConfig::AmbientOcclusion::Off;
}

const char *post_shader_source() noexcept {
    return R"HLSL(
cbuffer PostConstants : register(b0) {
    float4 P0;
    float4 P1;
    float4 P2;
    float4 P3;
    float4 P4;
    float4 P5;
};
Texture2D<float4> Source : register(t0);
#ifdef DEPTH_MS
Texture2DMS<float> Depth : register(t1);
float RawDepth(int2 p) { return Depth.Load(p, 0).r; }
#else
Texture2D<float> Depth : register(t1);
float RawDepth(int2 p) { return Depth.Load(int3(p, 0)).r; }
#endif
// Distance from the camera, written by PrepPS.
Texture2D<float4> Linear : register(t2);
SamplerState LinearClamp : register(s0);

float4 PostVS(uint id : SV_VertexID) : SV_POSITION {
    float2 corner = float2(float((id << 1u) & 2u), float(id & 2u));
    return float4(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0);
}

// ---- FXAA: P0.xy = 1 / surface size, P4 = smallest and largest UV of the scene.
float Luma(float3 colour) { return dot(colour, float3(0.299, 0.587, 0.114)); }
float4 SceneTap(float2 uv) { return Source.SampleLevel(LinearClamp, clamp(uv, P4.xy, P4.zw), 0.0); }
float4 FxaaPS(float4 position : SV_POSITION) : SV_TARGET {
    float2 rcp = P0.xy;
    float2 uv = position.xy * rcp;
    float4 centre = SceneTap(uv);
    float lM = Luma(centre.rgb);
    float lN = Luma(SceneTap(uv + float2(0.0, -rcp.y)).rgb);
    float lS = Luma(SceneTap(uv + float2(0.0, rcp.y)).rgb);
    float lW = Luma(SceneTap(uv + float2(-rcp.x, 0.0)).rgb);
    float lE = Luma(SceneTap(uv + float2(rcp.x, 0.0)).rgb);
    float lMin = min(lM, min(min(lN, lS), min(lW, lE)));
    float lMax = max(lM, max(max(lN, lS), max(lW, lE)));
    float range = lMax - lMin;
    float4 result = centre;
    if (range >= max(0.0625, lMax * 0.166)) {
        float lNW = Luma(SceneTap(uv + float2(-rcp.x, -rcp.y)).rgb);
        float lNE = Luma(SceneTap(uv + float2(rcp.x, -rcp.y)).rgb);
        float lSW = Luma(SceneTap(uv + float2(-rcp.x, rcp.y)).rgb);
        float lSE = Luma(SceneTap(uv + float2(rcp.x, rcp.y)).rgb);
        float edgeHorz = abs(lNW + lSW - 2.0 * lW) + 2.0 * abs(lN + lS - 2.0 * lM) + abs(lNE + lSE - 2.0 * lE);
        float edgeVert = abs(lNW + lNE - 2.0 * lN) + 2.0 * abs(lW + lE - 2.0 * lM) + abs(lSW + lSE - 2.0 * lS);
        bool horizontal = edgeHorz >= edgeVert;
        float l1 = horizontal ? lN : lW;
        float l2 = horizontal ? lS : lE;
        float gradient1 = abs(l1 - lM);
        float gradient2 = abs(l2 - lM);
        bool steepest1 = gradient1 >= gradient2;
        float gradientScaled = 0.25 * max(gradient1, gradient2);
        float stepLength = horizontal ? rcp.y : rcp.x;
        float localAverage = 0.5 * ((steepest1 ? l1 : l2) + lM);
        if (steepest1) stepLength = -stepLength;
        float2 edgeUv = uv;
        if (horizontal) edgeUv.y += stepLength * 0.5; else edgeUv.x += stepLength * 0.5;
        float2 along = horizontal ? float2(rcp.x, 0.0) : float2(0.0, rcp.y);
        float2 uv1 = edgeUv - along;
        float2 uv2 = edgeUv + along;
        float end1 = Luma(SceneTap(uv1).rgb) - localAverage;
        float end2 = Luma(SceneTap(uv2).rgb) - localAverage;
        bool reached1 = abs(end1) >= gradientScaled;
        bool reached2 = abs(end2) >= gradientScaled;
        static const float kSteps[8] = {1.0, 1.5, 2.0, 2.0, 2.0, 4.0, 8.0, 8.0};
        [loop] for (int i = 0; i < 8; ++i) {
            if (!reached1) {
                uv1 -= along * kSteps[i];
                end1 = Luma(SceneTap(uv1).rgb) - localAverage;
                reached1 = abs(end1) >= gradientScaled;
            }
            if (!reached2) {
                uv2 += along * kSteps[i];
                end2 = Luma(SceneTap(uv2).rgb) - localAverage;
                reached2 = abs(end2) >= gradientScaled;
            }
        }
        float distance1 = horizontal ? uv.x - uv1.x : uv.y - uv1.y;
        float distance2 = horizontal ? uv2.x - uv.x : uv2.y - uv.y;
        float nearest = min(distance1, distance2);
        float span = max(distance1 + distance2, 1.0e-6);
        float endLuma = distance1 < distance2 ? end1 : end2;
        bool correctSide = (endLuma < 0.0) != (lM < localAverage);
        float offset = correctSide ? 0.5 - nearest / span : 0.0;
        // Single bright or dark pixels: blend a little with the neighbourhood.
        float average = (2.0 * (lN + lS + lW + lE) + lNW + lNE + lSW + lSE) / 12.0;
        float sub = saturate(abs(average - lM) / range);
        sub = (3.0 - 2.0 * sub) * sub * sub;
        offset = max(offset, sub * sub * 0.5);
        float2 finalUv = uv;
        if (horizontal) finalUv.y += offset * stepLength; else finalUv.x += offset * stepLength;
        result.rgb = SceneTap(finalUv).rgb;
    }
    return result;
}

// ---- Ambient occlusion.
// P0 = (65535 / viewport z scale, projection z term - viewport z centre / z scale,
//       projection w term, largest depth value that is still sky)
// P1 = (viewport centre x, y in logical pixels, 1 / (viewport x scale * projection x), same for y)
// P2 = (logical pixels per scene pixel, scene pixels per occlusion pixel, 0, 0)
// P4 = scene region in scene pixels (min x, min y, max x, max y), P5 = the same in occlusion pixels
// Source = the scene's colour, of which only the alpha plane is read: the alpha
// of the fragment that wrote the pixel's depth.
float4 PrepPS(float4 position : SV_POSITION) : SV_TARGET {
    int2 pixel = int2(position.xy);
    float raw = RawDepth(pixel);
    float dist = 1.0e6;
    if (raw > P0.w) {
        dist = P0.z / (raw * P0.x + P0.y);
        // A solid surface writes alpha 1. Anything less is a cut-out (a plant, a
        // fence): its blades are too thin and too see-through to shade or be shaded.
        if (Source.Load(int3(pixel, 0)).a < 0.99) dist = -dist;
    }
    return float4(dist, 0.0, 0.0, 1.0);
}
// xyz = position in view space (z = distance), w = 1 on a cut-out's fringe,
// which is treated as empty space.
float4 ViewPosition(int2 aoPixel) {
    int2 scenePixel = clamp(aoPixel * int(P2.y), int2(P4.xy), int2(P4.zw));
    float dist = Linear.Load(int3(scenePixel, 0)).r;
    float fringe = dist < 0.0 ? 1.0 : 0.0;
    if (dist < 0.0) dist = 1.0e6;
    float2 logical = (float2(scenePixel) + 0.5) * P2.x;
    return float4((logical - P1.xy) * P1.zw * dist, dist, fringe);
}
// P3 = (radius in world units, occlusion pixels per world unit at distance 1, taps, largest radius in pixels)
float4 AoPS(float4 position : SV_POSITION) : SV_TARGET {
    int2 a = int2(position.xy);
    float3 p = ViewPosition(a).xyz;
    float ao = 1.0;
    if (p.z < 1.0e5) {
        float3 pl = ViewPosition(a + int2(-1, 0)).xyz;
        float3 pr = ViewPosition(a + int2(1, 0)).xyz;
        float3 pu = ViewPosition(a + int2(0, -1)).xyz;
        float3 pd = ViewPosition(a + int2(0, 1)).xyz;
        float3 dx = abs(pl.z - p.z) < abs(pr.z - p.z) ? p - pl : pr - p;
        float3 dy = abs(pu.z - p.z) < abs(pd.z - p.z) ? p - pu : pd - p;
        float3 n = cross(dy, dx);
        float area = length(n);
        if (area > 1.0e-12) {
            n /= area;
            if (dot(n, p) > 0.0) n = -n;
            float radiusPixels = clamp(P3.x * P3.y / p.z, 3.0, P3.w);
            float radius = radiusPixels * p.z / P3.y;
            float r2 = radius * radius;
            float bias = 0.03 * radius + 0.0015 * p.z;
            int taps = int(P3.z);
            float turns = taps > 8 ? 7.0 : 3.0;
            float noise = frac(52.9829189 * frac(dot(float2(a), float2(0.06711056, 0.00583715))));
            float sum = 0.0;
            float fringe = 0.0;
            [loop] for (int i = 0; i < taps; ++i) {
                float t = (float(i) + 0.5) / float(taps);
                float angle = (t * turns + noise) * 6.2831853;
                float2 offset = float2(cos(angle), sin(angle)) * (t * radiusPixels);
                float4 q = ViewPosition(a + int2(round(offset)));
                float3 v = q.xyz - p;
                float vv = dot(v, v);
                float f = max(r2 - vv, 0.0);
                sum += f * f * f * max((dot(v, n) - bias) / (vv + 0.0001 * r2), 0.0);
                fringe += q.w;
            }
            ao = saturate(1.0 - sum * (5.0 / (r2 * r2 * r2)) * radius * P2.z / float(taps));
            // Next to a cut-out's fringe the darkening fades out.
            ao = lerp(ao, 1.0, saturate(fringe * 6.0 / float(taps)));
            // Nothing far away: depth is too coarse there and the fog takes over.
            ao = lerp(ao, 1.0, saturate((p.z - P2.w * 0.6) / (P2.w * 0.4)));
        }
    }
    return float4(ao, min(p.z, 60000.0), 0.0, 1.0);
}
// P3.xy = direction of the pass in occlusion pixels.
float4 BlurPS(float4 position : SV_POSITION) : SV_TARGET {
    int2 a = int2(position.xy);
    float2 centre = Source.Load(int3(a, 0)).rg;
    static const float kWeights[3] = {0.80, 0.45, 0.18};
    float sum = centre.x;
    float total = 1.0;
    for (int i = 1; i <= 3; ++i) {
        for (int side = -1; side <= 1; side += 2) {
            int2 q = clamp(a + int2(P3.xy) * (i * side), int2(P5.xy), int2(P5.zw));
            float2 tap = Source.Load(int3(q, 0)).rg;
            float weight = kWeights[i - 1] * saturate(1.0 - abs(tap.y - centre.y) / (0.04 * centre.y + 0.05));
            sum += tap.x * weight;
            total += weight;
        }
    }
    return float4(sum / total, centre.y, 0.0, 1.0);
}
// P3.x = strength. The result multiplies the scene (or replaces it in the debug view).
float4 ApplyPS(float4 position : SV_POSITION) : SV_TARGET {
    int2 pixel = int2(position.xy);
    float dist = Linear.Load(int3(pixel, 0)).r;
    float ao = 1.0;
    if (dist > 0.0 && dist < 1.0e5) {
        float2 at = float2(pixel) / P2.y;
        int2 base = int2(floor(at));
        float2 w = at - floor(at);
        float sum = 0.0;
        float total = 0.0;
        for (int j = 0; j < 4; ++j) {
            int2 o = int2(j & 1, j >> 1);
            int2 q = clamp(base + o, int2(P5.xy), int2(P5.zw));
            float2 tap = Source.Load(int3(q, 0)).rg;
            float weight = (o.x != 0 ? w.x : 1.0 - w.x) * (o.y != 0 ? w.y : 1.0 - w.y) *
                (saturate(1.0 - abs(tap.y - dist) / (0.04 * dist + 0.05)) + 0.01);
            sum += tap.x * weight;
            total += weight;
        }
        ao = sum / max(total, 1.0e-6);
    }
    float k = lerp(1.0, ao, P3.x);
    return float4(k, k, k, 1.0);
}
)HLSL";
}

bool compile_post_shader(const char *entry, const char *profile, bool depth_ms,
                         ComPtr<ID3DBlob> &blob, std::string &error) noexcept {
    const char *source = post_shader_source();
    const D3D_SHADER_MACRO macros[]{{depth_ms ? "DEPTH_MS" : "DEPTH_PLAIN", "1"}, {nullptr, nullptr}};
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(source, std::strlen(source), "ChiliPost", macros, nullptr, entry, profile,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0u, &blob, &errors);
    if (FAILED(hr)) {
        error = errors ? std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize())
                       : hr_text(hr, "D3DCompile(post)");
        return false;
    }
    return true;
}

ComPtr<ID3D12PipelineState> create_post_pipeline(Dx12GeState &s, Dx12Post &post, ID3DBlob *vs, ID3DBlob *ps,
                                                 DXGI_FORMAT format, UINT samples, UINT quality,
                                                 bool multiply, std::string &error) noexcept {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = post.root.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    D3D12_RENDER_TARGET_BLEND_DESC &blend = pso.BlendState.RenderTarget[0];
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (multiply) {
        // destination * source; the surface's alpha plane is left alone.
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ZERO;
        blend.DestBlend = D3D12_BLEND_SRC_COLOR;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
        blend.DestBlendAlpha = D3D12_BLEND_ONE;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN |
                                      D3D12_COLOR_WRITE_ENABLE_BLUE;
    }
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1u;
    pso.RTVFormats[0] = format;
    pso.SampleDesc.Count = samples;
    pso.SampleDesc.Quality = quality;
    ComPtr<ID3D12PipelineState> pipeline;
    const HRESULT hr = s.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline));
    if (FAILED(hr)) {
        error = hr_text(hr, "CreateGraphicsPipelineState(post)");
        return {};
    }
    return pipeline;
}

bool create_post_surface(Dx12GeState &s, Dx12Post &post, Dx12PostSurface &surface, std::uint32_t width,
                         std::uint32_t height, DXGI_FORMAT format, bool render_target, UINT rtv_index,
                         std::string &error) noexcept {
    if (s.next_srv >= kSrvCapacity) { error = "no SRV descriptor left for a post surface"; return false; }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1u;
    desc.MipLevels = 1u;
    desc.Format = format;
    desc.SampleDesc.Count = 1u;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = render_target ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
    const HRESULT hr = s.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                          IID_PPV_ARGS(&surface.image));
    if (FAILED(hr)) { error = hr_text(hr, "CreateCommittedResource(post surface)"); return false; }
    surface.width = width;
    surface.height = height;
    surface.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    surface.srv_index = s.next_srv++;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1u;
    s.device->CreateShaderResourceView(surface.image.Get(), &srv, srv_cpu(s, surface.srv_index));
    if (render_target) {
        surface.rtv_index = rtv_index;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = post.rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(rtv_index) * post.rtv_stride;
        s.device->CreateRenderTargetView(surface.image.Get(), nullptr, rtv);
    }
    return true;
}

void destroy_post() noexcept {
    post_state() = Dx12Post{};
}

// Creates what the enabled passes need. A failure leaves the passes off and
// the game drawing as without them; `error` says why.
bool create_post(Dx12GeState &s, std::string &error) noexcept {
    Dx12Post &post = post_state();
    post = Dx12Post{};
    const GeGpuConfig &config = ge_gpu_config();
    // Multisampling already smooths the edges; the two are alternatives.
    post.fxaa = config.post_aa == GeGpuConfig::PostAntiAliasing::Fxaa && s.sample_count == 1u;
    post.ao = config.ambient_occlusion != GeGpuConfig::AmbientOcclusion::Off;
    post.ao_high = config.ambient_occlusion == GeGpuConfig::AmbientOcclusion::High;
    post.ao_debug = post.ao && config.ambient_occlusion_debug;
    post.ao_step = post.ao_high ? 1u : 2u;
    if (!post.fxaa && !post.ao) return true;

    D3D12_DESCRIPTOR_RANGE ranges[3]{};
    for (UINT i = 0u; i < 3u; ++i) {
        ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[i].NumDescriptors = 1u;
        ranges[i].BaseShaderRegister = i;
    }
    std::array<D3D12_ROOT_PARAMETER, 4> parameters{};
    for (UINT i = 0u; i < 3u; ++i) {
        parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[i].DescriptorTable.NumDescriptorRanges = 1u;
        parameters[i].DescriptorTable.pDescriptorRanges = &ranges[i];
        parameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[3].Constants.ShaderRegister = 0u;
    parameters[3].Constants.Num32BitValues = kPostConstantWords;
    parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1u;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0u;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = static_cast<UINT>(parameters.size());
    desc.pParameters = parameters.data();
    desc.NumStaticSamplers = 1u;
    desc.pStaticSamplers = &sampler;
    ComPtr<ID3DBlob> blob, errors;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    if (FAILED(hr)) { error = hr_text(hr, "D3D12SerializeRootSignature(post)"); return false; }
    hr = s.device->CreateRootSignature(0u, blob->GetBufferPointer(), blob->GetBufferSize(),
                                       IID_PPV_ARGS(&post.root));
    if (FAILED(hr)) { error = hr_text(hr, "CreateRootSignature(post)"); return false; }

    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors = 3u;
    hr = s.device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&post.rtv_heap));
    if (FAILED(hr)) { error = hr_text(hr, "CreateDescriptorHeap(post RTV)"); return false; }
    post.rtv_stride = s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    const bool depth_ms = s.sample_count > 1u;
    ComPtr<ID3DBlob> vs, ps;
    if (!compile_post_shader("PostVS", "vs_5_1", depth_ms, vs, error)) return false;
    if (post.fxaa) {
        if (!compile_post_shader("FxaaPS", "ps_5_1", depth_ms, ps, error)) return false;
        post.fxaa_pipeline = create_post_pipeline(s, post, vs.Get(), ps.Get(), kColorFormat, 1u, 0u, false, error);
        if (!post.fxaa_pipeline) return false;
        if (!create_post_surface(s, post, post.color_copy, s.target_width, s.target_height, kColorFormat,
                                 false, 0u, error)) return false;
    }
    if (post.ao) {
        if (!compile_post_shader("PrepPS", "ps_5_1", depth_ms, ps, error)) return false;
        post.prep_pipeline = create_post_pipeline(s, post, vs.Get(), ps.Get(), DXGI_FORMAT_R32_FLOAT, 1u, 0u, false, error);
        if (!post.prep_pipeline) return false;
        if (!create_post_surface(s, post, post.linear_depth, s.target_width, s.target_height,
                                 DXGI_FORMAT_R32_FLOAT, true, 2u, error)) return false;
        if (!compile_post_shader("AoPS", "ps_5_1", depth_ms, ps, error)) return false;
        post.ao_pipeline = create_post_pipeline(s, post, vs.Get(), ps.Get(), kAoFormat, 1u, 0u, false, error);
        if (!post.ao_pipeline) return false;
        if (!compile_post_shader("BlurPS", "ps_5_1", depth_ms, ps, error)) return false;
        post.blur_pipeline = create_post_pipeline(s, post, vs.Get(), ps.Get(), kAoFormat, 1u, 0u, false, error);
        if (!post.blur_pipeline) return false;
        if (!compile_post_shader("ApplyPS", "ps_5_1", depth_ms, ps, error)) return false;
        post.apply_pipeline = create_post_pipeline(s, post, vs.Get(), ps.Get(), kColorFormat, s.sample_count,
                                                   s.sample_quality, !post.ao_debug, error);
        if (!post.apply_pipeline) return false;
        const std::uint32_t width = (s.target_width + post.ao_step - 1u) / post.ao_step;
        const std::uint32_t height = (s.target_height + post.ao_step - 1u) / post.ao_step;
        for (UINT i = 0u; i < 2u; ++i)
            if (!create_post_surface(s, post, post.ao_surface[i], width, height, kAoFormat, true, i, error))
                return false;
    }
    post.events.reserve(4u);
    post.ready = true;
    return true;
}

// Looks through the frame's batches for scene surfaces and where their passes go.
void plan_scene_post(Dx12GeState &s, Dx12Post &post) noexcept {
    post.events.clear();
    post.next_event = 0u;
    if (!post.ready) return;
    struct Scene {
        std::uint32_t address{};
        std::uint32_t depth_address{};
        std::uint32_t draws{};
        std::size_t last{};
        const Dx12Batch *camera{};
        std::int32_t x0{}, y0{}, x1{}, y1{};   // logical scissor union, inclusive
    };
    std::array<Scene, 4> scenes{};
    std::size_t scene_count = 0u;
    const auto find_scene = [&](std::uint32_t address) -> Scene * {
        for (std::size_t i = 0u; i < scene_count; ++i)
            if (scenes[i].address == address) return &scenes[i];
        return nullptr;
    };
    for (std::size_t index = 0u; index < s.batches.size(); ++index) {
        const Dx12Batch &batch = s.batches[index];
        const std::uint32_t address = batch.draw.framebuffer_address & kVramOffsetMask;
        if (batch.framebuffer_feedback && batch.feedback_address != address) {
            Scene *scene = find_scene(batch.feedback_address);
            if (scene != nullptr && scene->draws >= kPostMinSceneDraws && scene->camera != nullptr) {
                Dx12PostEvent event{};
                event.ao_after = scene->last;
                event.before = index;
                event.scene_address = scene->address;
                event.depth_address = scene->depth_address;
                event.camera = scene->camera->transform;
                const auto scale_x = [&](std::int32_t value) {
                    return static_cast<LONG>(std::clamp<std::int64_t>(
                        static_cast<std::int64_t>(value) * s.target_width / std::max<std::uint32_t>(1u, s.sheet_width),
                        0, static_cast<std::int64_t>(s.target_width)));
                };
                const auto scale_y = [&](std::int32_t value) {
                    return static_cast<LONG>(std::clamp<std::int64_t>(
                        static_cast<std::int64_t>(value) * s.target_height / std::max<std::uint32_t>(1u, s.sheet_height),
                        0, static_cast<std::int64_t>(s.target_height)));
                };
                event.region = {scale_x(scene->x0), scale_y(scene->y0), scale_x(scene->x1 + 1), scale_y(scene->y1 + 1)};
                if (!rect_empty(event.region) && post.events.size() < 4u) post.events.push_back(event);
            }
            if (scene != nullptr) scene->draws = 0u;
        }
        const GeGpuDrawDescriptor &draw = batch.draw;
        if (!batch.hardware_transform || draw.through || draw.clear_mode || !draw.depth_test_enabled ||
            !draw.depth_write_enabled || draw.surface_write != GeGpuSurfaceWrite::None) continue;
        Scene *scene = find_scene(address);
        if (scene == nullptr) {
            if (scene_count == scenes.size()) continue;
            scene = &scenes[scene_count++];
            *scene = Scene{};
            scene->address = address;
        }
        if (scene->draws == 0u) {
            scene->camera = nullptr;
            scene->x0 = draw.scissor_x0; scene->y0 = draw.scissor_y0;
            scene->x1 = draw.scissor_x1; scene->y1 = draw.scissor_y1;
        } else {
            scene->x0 = std::min(scene->x0, draw.scissor_x0); scene->y0 = std::min(scene->y0, draw.scissor_y0);
            scene->x1 = std::max(scene->x1, draw.scissor_x1); scene->y1 = std::max(scene->y1, draw.scissor_y1);
        }
        // The first draw with a perspective projection is the scene camera.
        if (scene->camera == nullptr && batch.transform.projection[3] != 0.0f) scene->camera = &batch;
        ++scene->draws;
        scene->last = index;
        scene->depth_address = draw.depth_address & kVramOffsetMask;
    }
}

void begin_post_pass(Dx12GeState &s, Dx12Post &post, ID3D12PipelineState *pipeline, std::uint32_t width,
                     std::uint32_t height, const D3D12_RECT &scissor, std::uint32_t source_srv,
                     std::uint32_t depth_srv, std::uint32_t linear_srv,
                     const std::array<float, kPostConstantWords> &constants) noexcept {
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    s.list->RSSetViewports(1u, &viewport);
    s.list->RSSetScissorRects(1u, &scissor);
    s.list->SetGraphicsRootSignature(post.root.Get());
    s.list->SetPipelineState(pipeline);
    s.list->SetGraphicsRootDescriptorTable(0u, srv_gpu(s, source_srv));
    s.list->SetGraphicsRootDescriptorTable(1u, srv_gpu(s, depth_srv));
    s.list->SetGraphicsRootDescriptorTable(2u, srv_gpu(s, linear_srv));
    s.list->SetGraphicsRoot32BitConstants(3u, kPostConstantWords, constants.data(), 0u);
    s.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s.list->DrawInstanced(3u, 1u, 0u, 0u);
}

void post_surface_state(Dx12GeState &s, Dx12PostSurface &surface, D3D12_RESOURCE_STATES state) noexcept {
    transition(s.list.Get(), surface.image.Get(), surface.state, state);
    surface.state = state;
}

D3D12_CPU_DESCRIPTOR_HANDLE post_rtv(Dx12Post &post, const Dx12PostSurface &surface) noexcept {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = post.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(surface.rtv_index) * post.rtv_stride;
    return rtv;
}

// Smooths the edges of the scene surface in place. False when nothing was drawn.
bool run_post_fxaa(Dx12GeState &s, Dx12Post &post, const Dx12PostEvent &event) noexcept {
    Dx12FramebufferTarget *scene = find_framebuffer_target(s, event.scene_address);
    if (!post.fxaa || scene == nullptr || !scene->color || scene->msaa_color) return false;
    const D3D12_RECT &region = event.region;
    transition(s.list.Get(), scene->color.Get(), scene->color_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    scene->color_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    post_surface_state(s, post.color_copy, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = post.color_copy.image.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = scene->color.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box{static_cast<UINT>(region.left), static_cast<UINT>(region.top), 0u,
                        static_cast<UINT>(region.right), static_cast<UINT>(region.bottom), 1u};
    s.list->CopyTextureRegion(&to, box.left, box.top, 0u, &from, &box);
    post_surface_state(s, post.color_copy, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(s.list.Get(), scene->color.Get(), scene->color_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    scene->color_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_cpu(s, scene->rtv_index);
    s.list->OMSetRenderTargets(1u, &rtv, FALSE, nullptr);
    const float w = static_cast<float>(s.target_width), h = static_cast<float>(s.target_height);
    std::array<float, kPostConstantWords> constants{};
    constants[0] = 1.0f / w;
    constants[1] = 1.0f / h;
    constants[16] = (static_cast<float>(region.left) + 0.5f) / w;
    constants[17] = (static_cast<float>(region.top) + 0.5f) / h;
    constants[18] = (static_cast<float>(region.right) - 0.5f) / w;
    constants[19] = (static_cast<float>(region.bottom) - 0.5f) / h;
    begin_post_pass(s, post, post.fxaa_pipeline.Get(), s.target_width, s.target_height, region,
                    post.color_copy.srv_index, 0u, 0u, constants);
    note_target_write(*scene, region);
    ++post.fxaa_passes;
    return true;
}

// Darkens corners and creases of the scene surface. False when nothing was drawn.
bool run_post_ao(Dx12GeState &s, Dx12Post &post, const Dx12PostEvent &event) noexcept {
    Dx12FramebufferTarget *scene = find_framebuffer_target(s, event.scene_address);
    const auto depth_found = s.depth_targets.find(event.depth_address);
    if (!post.ao || scene == nullptr || !scene->color || depth_found == s.depth_targets.end()) return false;
    Dx12DepthTarget &depth = depth_found->second;
    const GeGpuHardwareTransform &camera = event.camera;
    const float m0 = camera.projection[0], m5 = camera.projection[1];
    const float m10 = camera.projection[2], m14 = camera.projection[3];
    if (!depth.depth || depth.srv_index == 0u || m0 == 0.0f || m5 == 0.0f || m14 == 0.0f ||
        camera.viewport_scale_x == 0.0f || camera.viewport_scale_y == 0.0f || camera.viewport_scale_z == 0.0f)
        return false;
    const float scale = static_cast<float>(s.target_width) / static_cast<float>(s.sheet_width);
    const float step = static_cast<float>(post.ao_step);
    const D3D12_RECT &region = event.region;
    const D3D12_RECT ao_region{region.left / static_cast<LONG>(post.ao_step),
                               region.top / static_cast<LONG>(post.ao_step),
                               (region.right + static_cast<LONG>(post.ao_step) - 1) / static_cast<LONG>(post.ao_step),
                               (region.bottom + static_cast<LONG>(post.ao_step) - 1) / static_cast<LONG>(post.ao_step)};
    std::array<float, kPostConstantWords> constants{};
    constants[0] = 65535.0f / camera.viewport_scale_z;
    constants[1] = m10 - camera.viewport_center_z / camera.viewport_scale_z;
    constants[2] = m14;
    constants[3] = 1.5f / 65535.0f;
    constants[4] = camera.viewport_center_x - camera.viewport_offset_x;
    constants[5] = camera.viewport_center_y - camera.viewport_offset_y;
    constants[6] = 1.0f / (camera.viewport_scale_x * m0);
    constants[7] = 1.0f / (camera.viewport_scale_y * m5);
    constants[8] = 1.0f / scale;
    constants[9] = step;
    // Intensity: a right-angled corner comes out at about half brightness
    // before the strength is applied.
    constants[10] = 0.4f;
    constants[11] = 160.0f;                       // world distance at which the effect is gone
    constants[12] = 1.0f;                         // radius in world units (about metres)
    constants[13] = std::abs(camera.viewport_scale_x * m0) * scale / step;
    constants[14] = post.ao_high ? 16.0f : 8.0f;
    constants[15] = 48.0f * scale / step;         // at most 48 PSP pixels on screen
    constants[16] = static_cast<float>(region.left);
    constants[17] = static_cast<float>(region.top);
    constants[18] = static_cast<float>(region.right - 1);
    constants[19] = static_cast<float>(region.bottom - 1);
    constants[20] = static_cast<float>(ao_region.left);
    constants[21] = static_cast<float>(ao_region.top);
    constants[22] = static_cast<float>(ao_region.right - 1);
    constants[23] = static_cast<float>(ao_region.bottom - 1);

    // Distance per pixel, from the depth buffer and the alpha plane of the
    // (resolved) scene colour.
    resolve_target_for_sampling(s, *scene, false);
    transition(s.list.Get(), depth.depth.Get(), depth.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    depth.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Dx12PostSurface &linear = post.linear_depth;
    post_surface_state(s, linear, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = post_rtv(post, linear);
    s.list->OMSetRenderTargets(1u, &rtv, FALSE, nullptr);
    begin_post_pass(s, post, post.prep_pipeline.Get(), linear.width, linear.height, region,
                    scene->srv_index, depth.srv_index, 0u, constants);
    post_surface_state(s, linear, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(s.list.Get(), depth.depth.Get(), depth.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    depth.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    Dx12PostSurface &first = post.ao_surface[0];
    Dx12PostSurface &second = post.ao_surface[1];
    post_surface_state(s, first, D3D12_RESOURCE_STATE_RENDER_TARGET);
    rtv = post_rtv(post, first);
    s.list->OMSetRenderTargets(1u, &rtv, FALSE, nullptr);
    begin_post_pass(s, post, post.ao_pipeline.Get(), first.width, first.height, ao_region, 0u, 0u,
                    linear.srv_index, constants);
    post_surface_state(s, first, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    constants[12] = 1.0f; constants[13] = 0.0f;
    post_surface_state(s, second, D3D12_RESOURCE_STATE_RENDER_TARGET);
    rtv = post_rtv(post, second);
    s.list->OMSetRenderTargets(1u, &rtv, FALSE, nullptr);
    begin_post_pass(s, post, post.blur_pipeline.Get(), second.width, second.height, ao_region,
                    first.srv_index, 0u, 0u, constants);
    post_surface_state(s, second, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    constants[12] = 0.0f; constants[13] = 1.0f;
    post_surface_state(s, first, D3D12_RESOURCE_STATE_RENDER_TARGET);
    rtv = post_rtv(post, first);
    s.list->OMSetRenderTargets(1u, &rtv, FALSE, nullptr);
    begin_post_pass(s, post, post.blur_pipeline.Get(), first.width, first.height, ao_region,
                    second.srv_index, 0u, 0u, constants);
    post_surface_state(s, first, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    constants[12] = post.ao_debug ? 1.0f
        : static_cast<float>(std::min(ge_gpu_config().ambient_occlusion_strength, 100u)) / 100.0f;
    prepare_target_for_render(s, *scene);
    rtv = rtv_cpu(s, scene->rtv_index);
    s.list->OMSetRenderTargets(1u, &rtv, FALSE, nullptr);
    begin_post_pass(s, post, post.apply_pipeline.Get(), s.target_width, s.target_height, region,
                    first.srv_index, 0u, linear.srv_index, constants);
    note_target_write(*scene, region);
    ++post.ao_passes;
    return true;
}

// Runs the passes that are due before the batch with index `batch_index` is
// drawn. True when one ran: the caller's cached command-list state (pipeline,
// root signature and its arguments, render target, viewport, scissor,
// topology) is then no longer what the list has.
bool run_due_post(Dx12GeState &s, Dx12Post &post, std::size_t batch_index) noexcept {
    bool ran = false;
    while (post.next_event < post.events.size()) {
        Dx12PostEvent &event = post.events[post.next_event];
        if (!event.ao_done && batch_index > event.ao_after) {
            event.ao_done = true;
            ran = run_post_ao(s, post, event) || ran;
        }
        if (batch_index < event.before) break;
        ran = run_post_fxaa(s, post, event) || ran;
        ++post.next_event;
    }
    return ran;
}
