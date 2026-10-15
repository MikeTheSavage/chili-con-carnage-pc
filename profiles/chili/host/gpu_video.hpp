#pragma once
// The game's use of the hardware renderer: start-up from the [Rendering]
// settings and screenshots of the displayed surface. Runtime thread only.
#include "pc_window.hpp"
#include "render_settings.hpp"
#include "gpu/ge_gpu_host.hpp"
#include "gpu/ge_texture_mods.hpp"
namespace chili {
// Starts the renderer `settings` names. True when Direct3D 12 draws; false
// leaves the software rasterizer in charge. `report` says which one runs and,
// after a failed start, why. With settings.strict a failed start throws.
inline bool start_gpu_backend(const RenderSettings &settings,unsigned scale,std::string &report){
    vcs::GeGpuConfig config;
    config.backend=settings.backend==RenderSettings::Backend::D3D12?vcs::GeGpuConfig::Backend::DirectX12:vcs::GeGpuConfig::Backend::Software;
    config.internal_scale=scale;config.msaa=settings.msaa;config.strict=settings.strict;
    config.internal_scale_auto=settings.internal_scale==0u;
    config.anisotropic_filtering=settings.anisotropy;
    config.texture_filter=settings.texture_filter==RenderSettings::TextureFilter::Linear?vcs::GeGpuConfig::TextureFilter::Linear:
        settings.texture_filter==RenderSettings::TextureFilter::Nearest?vcs::GeGpuConfig::TextureFilter::Nearest:vcs::GeGpuConfig::TextureFilter::Game;
    config.post_aa=settings.fxaa?vcs::GeGpuConfig::PostAntiAliasing::Fxaa:vcs::GeGpuConfig::PostAntiAliasing::Off;
    config.ambient_occlusion=settings.ssao==RenderSettings::AmbientOcclusion::High?vcs::GeGpuConfig::AmbientOcclusion::High:
        settings.ssao==RenderSettings::AmbientOcclusion::Low?vcs::GeGpuConfig::AmbientOcclusion::Low:vcs::GeGpuConfig::AmbientOcclusion::Off;
    config.glow=settings.glow==RenderSettings::Glow::Off?vcs::GeGpuConfig::Glow::Off:
        settings.glow==RenderSettings::Glow::Game?vcs::GeGpuConfig::Glow::Game:vcs::GeGpuConfig::Glow::Smooth;
    // The game thread never waits for the display: frames are paced on guest time.
    config.present_sync_interval=0u;config.frame_readback=false;
    // The renderer's own PSPRECOMP_GE_* / PSPRECOMP_DX12_* switches stay usable for A/B runs.
    vcs::set_ge_gpu_config(vcs::ge_gpu_config_from_environment(config));
    std::string error;
    const bool started=vcs::initialize_ge_gpu_backend(error);
    report=vcs::ge_gpu_backend_report().message;
    if(!started)throw psprecomp::Error("Direct3D 12 renderer did not start (CHILI_RENDER_STRICT): "+error);
    if(vcs::ge_gpu_backend_active())
        report+=" scale="+std::to_string(vcs::ge_gpu_config().internal_scale)+" msaa="+std::to_string(vcs::ge_gpu_backend_report().dx12_msaa_samples)+
            (vcs::ge_gpu_config().post_aa==vcs::GeGpuConfig::PostAntiAliasing::Fxaa?" fxaa":"")+
            " anisotropy="+std::to_string(vcs::ge_gpu_config().anisotropic_filtering)+
            (vcs::ge_gpu_config().texture_filter==vcs::GeGpuConfig::TextureFilter::Linear?" filter=linear":
             vcs::ge_gpu_config().texture_filter==vcs::GeGpuConfig::TextureFilter::Nearest?" filter=nearest":"")+
            (vcs::ge_gpu_config().ambient_occlusion==vcs::GeGpuConfig::AmbientOcclusion::High?" ssao=high":
             vcs::ge_gpu_config().ambient_occlusion==vcs::GeGpuConfig::AmbientOcclusion::Low?" ssao=low":"")+
            (vcs::ge_gpu_config().glow==vcs::GeGpuConfig::Glow::Game?" glow=game":
             vcs::ge_gpu_config().glow==vcs::GeGpuConfig::Glow::Off?" glow=off":"");
    return vcs::ge_gpu_backend_active();
}
// Texture replacement and dump of the mods in `setup` (mods/TEXTURES.md). Returns the lines for the log.
// Direct3D 12 only: the software renderer draws from guest memory and never sees a replacement.
inline std::vector<std::string> start_texture_mods(const ModSetup &setup){
    vcs::GeTextureModsSetup textures;
    textures.replacement_folders=setup.texture_folders();
    textures.dump_folder=setup.texture_dump;
    MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);
    textures.store_bytes=picture_store_bytes(setup.picture_store_mb,GlobalMemoryStatusEx(&memory)?memory.ullTotalPhys:0u);
    vcs::ge_texture_mods_start(textures);
    std::vector<std::string> lines;
    const auto report=vcs::ge_texture_mods_report();
    if(report.replacement_files)lines.push_back("textures: "+std::to_string(report.replacement_files)+" replacement pictures in "+
        std::to_string(textures.replacement_folders.size())+" mod folder"+(textures.replacement_folders.size()==1u?"":"s"));
    if(vcs::ge_texture_mods_dumping())lines.push_back("textures: every texture drawn is written once to "+setup.texture_dump.string());
    return lines;
}
// What the texture mods did, for the end of the log; empty when they were not in use. With replacement pictures
// the first line says how many were decoded at the start, ahead of the draws, and what the store holds, and when
// the pictures of what the game draws from its start to the main menu were all there; the second how many the
// game drew with, and how many of those it asked for before they were decoded (the game's own picture was then
// drawn first), and, when the packs replace pictures of the start, how many of the late ones were those.
inline std::vector<std::string> texture_mods_summary(){
    std::vector<std::string> lines;
    if(!vcs::ge_texture_mods_keying())return lines;
    vcs::ge_texture_mods_wait_idle();
    const auto r=vcs::ge_texture_mods_report();
    const auto mib=[](std::uint64_t bytes){return std::to_string((bytes+524288u)/1048576u);};
    if(r.replacement_files)lines.push_back("textures: "+std::to_string(r.decoded_ahead)+" of "+std::to_string(r.replacement_files)+
        " pictures decoded ahead in "+std::to_string(r.decoded_ahead_ms)+" ms on "+std::to_string(r.workers)+" threads, "+
        mib(r.kept_bytes)+" MiB kept (store "+mib(r.store_bytes)+" MiB)"+
        (r.waiting_first_use?"; "+std::to_string(r.waiting_first_use)+" wait for their first use":"")+
        (r.start_files?"; the "+std::to_string(r.start_files)+" of the start to the main menu first, all there in "+
            std::to_string(r.start_decoded_ms)+" ms":""));
    lines.push_back("textures: "+std::to_string(r.replaced)+" replaced, "+std::to_string(r.failed)+" unreadable, "+
        std::to_string(r.dumped)+" dumped"+(r.late?", "+std::to_string(r.late)+" decoded on first use"+
            (r.start_files?" ("+std::to_string(r.start_late)+" of them of the start)":""):"")+
        (r.requeued?", "+std::to_string(r.requeued)+" decoded again for a screen picture's rows":"")+
        (r.unstable_addresses?", "+std::to_string(r.unstable_addresses)+" changing addresses left alone":""));
    return lines;
}
// The GPU surface at `address` (a 512x512 sheet at the internal scale), cut to
// the LCD's top-left 480x272 and returned as the 0xFFRRGGBB words the PNG and
// BMP writers take. `width` and `height` are 480x272 times the internal scale.
// Blocks on the GPU twice: screenshots only, never per frame.
inline bool read_gpu_display(unsigned address,std::vector<unsigned> &pixels,unsigned &width,unsigned &height){
    std::vector<std::byte> rgba;std::uint32_t sheet_width{},sheet_height{};
    // While a host page is set (the overlay pass, ge_gpu_backend.hpp) the picture is the surface with the page
    // drawn over it at the internal resolution: the page is on the window's back buffer, not on the surface.
    if(vcs::ge_gpu_backend_active() && vcs::ge_gpu_backend_overlay_set()){
        std::uint32_t w{},h{};
        if(!vcs::ge_gpu_backend_read_display_with_overlay(address,rgba,w,h))return false;
        width=w;height=h;pixels.resize(std::size_t(width)*height);
        for(std::size_t i=0;i<pixels.size();++i){
            const std::byte *p=rgba.data()+i*4u;
            pixels[i]=0xFF000000u|(std::to_integer<unsigned>(p[0])<<16)|(std::to_integer<unsigned>(p[1])<<8)|std::to_integer<unsigned>(p[2]);
        }
        return true;
    }
    if(!vcs::ge_gpu_backend_active() || !vcs::ge_gpu_backend_read_surface_rgba(address,rgba,sheet_width,sheet_height))return false;
    const unsigned scale=sheet_width/vcs::ge_gpu_config().surface_width;
    if(!scale || sheet_width<480u*scale || sheet_height<272u*scale)return false;
    width=480u*scale;height=272u*scale;pixels.resize(std::size_t(width)*height);
    for(unsigned y=0;y<height;++y){
        const std::byte *row=rgba.data()+std::size_t(y)*sheet_width*4u;
        for(unsigned x=0;x<width;++x)
            pixels[std::size_t(y)*width+x]=0xFF000000u|(std::to_integer<unsigned>(row[x*4u])<<16)|
                (std::to_integer<unsigned>(row[x*4u+1u])<<8)|std::to_integer<unsigned>(row[x*4u+2u]);
    }
    return true;
}
// Writes the displayed GPU surface. A ".png" gets the picture at the internal
// resolution; anything else the 480x272 BMP, box-filtered down when the scale
// is above 1.
inline bool save_gpu_display(unsigned address,const std::filesystem::path &path){
    std::vector<unsigned> pixels;unsigned width{},height{};
    if(!read_gpu_display(address,pixels,width,height))return false;
    auto extension=path.extension().wstring();
    for(auto &c:extension)if(c>=L'A' && c<=L'Z')c=static_cast<wchar_t>(c-L'A'+L'a');
    if(extension==L".png")return write_png(path,width,height,pixels);
    const unsigned scale=width/480u,area=scale*scale;
    if(scale>1u){
        std::vector<unsigned> reduced(480u*272u);
        for(unsigned y=0;y<272u;++y)for(unsigned x=0;x<480u;++x){
            unsigned sum[3]{};
            for(unsigned sy=0;sy<scale;++sy)for(unsigned sx=0;sx<scale;++sx){
                const unsigned p=pixels[std::size_t(y*scale+sy)*width+x*scale+sx];
                sum[0]+=(p>>16)&255u;sum[1]+=(p>>8)&255u;sum[2]+=p&255u;
            }
            reduced[y*480u+x]=0xFF000000u|(((sum[0]+area/2u)/area)<<16)|(((sum[1]+area/2u)/area)<<8)|((sum[2]+area/2u)/area);
        }
        pixels=std::move(reduced);
    }
    return save_display_pixels(path,pixels);
}
}
