// chili_ge_replay: re-executes captured GE display lists offline.
//
// A capture (host/ge_frame_capture.hpp, written by the game when
// CHILI_GE_CAPTURE_DIR is set) holds guest RAM, VRAM, scratchpad and the GE
// executor state at sceGeListEnQueue. This tool loads it into a fresh runtime,
// runs the list through the same executor the game uses, writes each surface as
// a PNG, writes the per-draw trace, and with --compare checks the result.
//
// --backend software replays through the CPU rasterizer and compares with the
// VRAM the game had when the list returned (byte-exact by default).
// --backend d3d12 replays through the hardware renderer. Several captures given
// in order run on one backend instance, so GPU surfaces carry over from list to
// list as they do in the game; --compare then replays each capture through the
// software rasterizer as well and compares the surfaces with a tolerance.
#include "../host/ge_runtime.hpp"
#include "../host/gpu/ge_gpu_host.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>

namespace {
using chili::GeDrawInfo;
using Bytes=std::vector<std::uint8_t>;
constexpr unsigned kVramBase=psprecomp::GuestMemory::kVramPhysicalBase,kVramSize=psprecomp::GuestMemory::kVramSize;

struct Surface {std::string name;unsigned address{},width{},height{},stride{};bool depth{};};
struct Image {unsigned width{},height{};Bytes rgb;};

std::string hex(unsigned value){char buffer[16];std::snprintf(buffer,sizeof buffer,"0x%08X",value);return buffer;}

// 8-bit RGB PNG with stored (uncompressed) deflate blocks; no library needed.
bool write_png(const std::filesystem::path &path,const Image &image){
    static const auto table=[]{
        std::array<std::uint32_t,256> values{};
        for(std::uint32_t n=0;n<256;++n){std::uint32_t c=n;for(int k=0;k<8;++k)c=(c&1u)?0xEDB88320u^(c>>1):c>>1;values[n]=c;}
        return values;
    }();
    auto be32=[](Bytes &out,std::uint32_t value){for(int shift=24;shift>=0;shift-=8)out.push_back(static_cast<std::uint8_t>(value>>shift));};
    Bytes file{0x89,'P','N','G','\r','\n',0x1A,'\n'};
    auto chunk=[&](const char *type,const Bytes &payload){
        be32(file,static_cast<std::uint32_t>(payload.size()));
        const std::size_t start=file.size();
        file.insert(file.end(),type,type+4);file.insert(file.end(),payload.begin(),payload.end());
        std::uint32_t crc=0xFFFFFFFFu;
        for(std::size_t i=start;i<file.size();++i)crc=table[(crc^file[i])&255u]^(crc>>8);
        be32(file,~crc);
    };
    Bytes header;be32(header,image.width);be32(header,image.height);
    header.insert(header.end(),{8,2,0,0,0});
    chunk("IHDR",header);
    const std::size_t row=std::size_t(image.width)*3u;
    Bytes raw;raw.reserve((row+1u)*image.height);
    for(unsigned y=0;y<image.height;++y){raw.push_back(0);raw.insert(raw.end(),image.rgb.begin()+y*row,image.rgb.begin()+(y+1u)*row);}
    Bytes data{0x78,0x01};
    std::uint32_t a=1,b=0;
    for(const auto byte:raw){a=(a+byte)%65521u;b=(b+a)%65521u;}
    for(std::size_t offset=0;offset<raw.size() || offset==0;){
        const std::size_t length=std::min<std::size_t>(65535u,raw.size()-offset);
        data.push_back(static_cast<std::uint8_t>(offset+length>=raw.size()));
        data.push_back(static_cast<std::uint8_t>(length));data.push_back(static_cast<std::uint8_t>(length>>8));
        data.push_back(static_cast<std::uint8_t>(~length));data.push_back(static_cast<std::uint8_t>((~length)>>8));
        data.insert(data.end(),raw.begin()+offset,raw.begin()+offset+length);
        offset+=length;
        if(!length)break;
    }
    be32(data,(b<<16)|a);
    chunk("IDAT",data);chunk("IEND",{});
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    out.write(reinterpret_cast<const char*>(file.data()),static_cast<std::streamsize>(file.size()));
    return static_cast<bool>(out);
}

// One surface out of a raw 2 MiB VRAM image. Colour is 8888 with alpha dropped
// (the PSP keeps stencil there); depth is 16-bit shown as grey.
bool decode_surface(std::span<const std::uint8_t> vram,const Surface &surface,Image &out){
    const unsigned offset=(surface.address-kVramBase)&(kVramSize-1u),texel=surface.depth?2u:4u;
    if(!surface.width || !surface.height || surface.stride<surface.width)return false;
    const std::uint64_t last=std::uint64_t(offset)+(std::uint64_t(surface.height-1u)*surface.stride+surface.width)*texel;
    if(last>vram.size())return false;
    out.width=surface.width;out.height=surface.height;out.rgb.resize(std::size_t(surface.width)*surface.height*3u);
    for(unsigned y=0;y<surface.height;++y)for(unsigned x=0;x<surface.width;++x){
        const std::uint8_t *source=vram.data()+offset+(std::size_t(y)*surface.stride+x)*texel;
        std::uint8_t *target=out.rgb.data()+(std::size_t(y)*surface.width+x)*3u;
        if(surface.depth)target[0]=target[1]=target[2]=source[1];
        else{target[0]=source[0];target[1]=source[1];target[2]=source[2];}
    }
    return true;
}

// Box-filters an image rendered at `scale` pixels per PSP pixel back to 1x.
Image downscale(const Image &image,unsigned scale){
    if(scale<=1u)return image;
    Image out;out.width=image.width/scale;out.height=image.height/scale;out.rgb.resize(std::size_t(out.width)*out.height*3u);
    const unsigned area=scale*scale;
    for(unsigned y=0;y<out.height;++y)for(unsigned x=0;x<out.width;++x)for(unsigned c=0;c<3;++c){
        unsigned sum=0;
        for(unsigned sy=0;sy<scale;++sy)for(unsigned sx=0;sx<scale;++sx)
            sum+=image.rgb[(std::size_t(y*scale+sy)*image.width+x*scale+sx)*3u+c];
        out.rgb[(std::size_t(y)*out.width+x)*3u+c]=static_cast<std::uint8_t>((sum+area/2u)/area);
    }
    return out;
}

// Images of equal height next to each other with a grey gap.
Image side_by_side(std::initializer_list<const Image*> images){
    Image out;const unsigned gap=4u;
    for(const Image *image:images){out.width+=image->width+(out.width?gap:0u);out.height=std::max(out.height,image->height);}
    out.rgb.assign(std::size_t(out.width)*out.height*3u,0x40);
    unsigned left=0;
    for(const Image *image:images){
        for(unsigned y=0;y<image->height;++y)
            std::memcpy(out.rgb.data()+(std::size_t(y)*out.width+left)*3u,image->rgb.data()+std::size_t(y)*image->width*3u,std::size_t(image->width)*3u);
        left+=image->width+gap;
    }
    return out;
}

// The seam a GPU renderer plugs into. execute() runs one list from the state the
// capture restored; read_surface() returns what that renderer drew, which for a
// GPU backend is its own target rather than guest VRAM.
class ReplayBackend {
public:
    virtual ~ReplayBackend()=default;
    virtual const char *name() const=0;
    // True when results land in guest VRAM, so a byte comparison is meaningful.
    virtual bool writes_guest_vram() const=0;
    virtual bool execute(psprecomp::Runtime &rt,chili::GeRuntime &ge,unsigned list,chili::GeRuntime::ListRun &run)=0;
    // `native` receives the image at the renderer's own resolution when that is
    // not the PSP's.
    virtual bool read_surface(const psprecomp::Runtime &rt,const Surface &surface,Image &out,Image *native=nullptr)=0;
    // Copies the surfaces of a raw VRAM image into the renderer's own targets.
    virtual void seed(std::span<const std::uint8_t>){}
    // Approximated or dropped draws since the last call; empty when clean.
    virtual std::string take_exceptions(){return {};}
    virtual void print_report() const {}
    double submit_ms{};
};
class SoftwareBackend final : public ReplayBackend {
public:
    const char *name() const override {return "software";}
    bool writes_guest_vram() const override {return true;}
    bool execute(psprecomp::Runtime &rt,chili::GeRuntime &ge,unsigned list,chili::GeRuntime::ListRun &run) override {
        return ge.execute_list(rt,list,run);
    }
    bool read_surface(const psprecomp::Runtime &rt,const Surface &surface,Image &out,Image *) override {
        return decode_surface(rt.memory().vram_bytes(),surface,out);
    }
};
class D3D12Backend final : public ReplayBackend {
    unsigned scale_;
    vcs::GeGpuBackendReport last_{};
public:
    D3D12Backend(unsigned scale,unsigned msaa,bool hardware_cull,bool hardware_transform,vcs::GeGpuConfig::Glow glow,std::string &error):scale_(scale){
        vcs::GeGpuConfig config;
        config.backend=vcs::GeGpuConfig::Backend::DirectX12;
        config.internal_scale=scale;config.msaa=msaa;config.hardware_cull=hardware_cull;config.hardware_transform=hardware_transform;config.strict=true;
        config.glow=glow;
        vcs::set_ge_gpu_config(config);
        if(!vcs::initialize_ge_gpu_backend(error) || !vcs::ge_gpu_backend_active()){
            if(error.empty())error="the D3D12 backend did not start";
            return;
        }
        vcs::ge_gpu_backend_set_display_framebuffer(kVramBase);
        std::cout<<"d3d12 adapter: "<<vcs::ge_gpu_backend_adapter_name()<<" scale="<<scale<<" msaa="<<msaa<<'\n';
    }
    ~D3D12Backend() override {vcs::shutdown_ge_gpu_backend();}
    const char *name() const override {return "d3d12";}
    bool writes_guest_vram() const override {return false;}
    bool execute(psprecomp::Runtime &rt,chili::GeRuntime &ge,unsigned list,chili::GeRuntime::ListRun &run) override {
        chili::attach_gpu_draw_backend(ge);
        const bool ok=ge.execute_list(rt,list,run);
        const auto started=std::chrono::steady_clock::now();
        (void)vcs::ge_gpu_backend_flush();
        submit_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        return ok;
    }
    bool read_surface(const psprecomp::Runtime &,const Surface &surface,Image &out,Image *native) override {
        if(surface.depth)return false;
        std::vector<std::byte> rgba;std::uint32_t width{},height{};
        if(!vcs::ge_gpu_backend_read_surface_rgba(surface.address,rgba,width,height))return false;
        Image full;full.width=std::min(surface.width*scale_,width);full.height=std::min(surface.height*scale_,height);
        full.rgb.resize(std::size_t(full.width)*full.height*3u);
        for(unsigned y=0;y<full.height;++y)for(unsigned x=0;x<full.width;++x)for(unsigned c=0;c<3;++c)
            full.rgb[(std::size_t(y)*full.width+x)*3u+c]=static_cast<std::uint8_t>(rgba[(std::size_t(y)*width+x)*4u+c]);
        out=downscale(full,scale_);
        if(native && scale_>1u)*native=std::move(full);
        return true;
    }
    void seed(std::span<const std::uint8_t> vram) override {
        struct Sheet {unsigned address,rows;};
        for(const Sheet sheet:{Sheet{0x04000000u,272u},Sheet{0x040D8000u,320u},Sheet{0x040E8000u,320u},Sheet{0x04088000u,320u}}){
            const std::size_t offset=sheet.address-kVramBase,bytes=std::size_t(sheet.rows)*512u*4u;
            if(offset+bytes>vram.size())continue;
            (void)vcs::ge_gpu_backend_write_surface_rgba(sheet.address,512u,sheet.rows,std::as_bytes(vram.subspan(offset,bytes)));
        }
        const std::size_t offset=0x04088000u-kVramBase;
        std::vector<std::uint16_t> depth(512u*320u);
        if(offset+depth.size()*2u<=vram.size()){
            std::memcpy(depth.data(),vram.data()+offset,depth.size()*2u);
            (void)vcs::ge_gpu_backend_write_surface_depth16(0x04088000u,0x040E8000u,512u,320u,depth);
        }
        (void)vcs::ge_gpu_backend_flush();
        (void)vcs::ge_gpu_backend_take_fallbacks();
    }
    std::string take_exceptions() override {
        const auto report=vcs::ge_gpu_backend_report();
        std::string text=vcs::ge_gpu_fallback_names(vcs::ge_gpu_backend_take_fallbacks());
        auto count=[&](const char *what,std::uint64_t now,std::uint64_t before){
            if(now==before)return;
            if(!text.empty())text+="; ";
            text+=std::to_string(now-before)+" "+what;
        };
        count("textured draws without a texture",report.game_textured_draws_without_texture,last_.game_textured_draws_without_texture);
        count("draws with an approximated blend",report.unsupported_blend_game_draw_calls,last_.unsupported_blend_game_draw_calls);
        count("rejected draws",report.rejected_gpu_draws,last_.rejected_gpu_draws);
        count("geometry overflows",report.game_vertex_overflows,last_.game_vertex_overflows);
        count("rejected texture decodes",report.rejected_texture_decodes,last_.rejected_texture_decodes);
        last_=report;
        return text;
    }
    void print_report() const override {
        const auto r=vcs::ge_gpu_backend_report();
        std::cout<<"d3d12 totals: draws="<<r.game_draw_calls<<" hw_transform_draws="<<r.hw_transform_draw_calls
                 <<" triangles="<<r.game_triangles<<" pipelines="<<r.unique_pipeline_keys<<" textures="<<r.unique_texture_keys
                 <<" texture_uploads="<<r.texture_image_uploads<<" surfaces="<<r.dx12_native_framebuffer_targets
                 <<" feedback_draws="<<r.dx12_gpu_feedback_draws<<" self_feedback_snapshots="<<r.dx12_self_feedback_snapshots
                 <<" submissions="<<r.transfer_submissions<<" glow_blocks="<<r.dx12_glow_blocks<<" glow_readers="<<r.dx12_glow_readers
                 <<" glow_left_out="<<r.dx12_glow_left_out<<'\n';
        // The card's own time per submission (timestamp queries): with --repeat, of the same list again and again.
        const auto gpu=vcs::ge_gpu_backend_gpu_time();
        if(gpu.submissions)std::cout<<"d3d12 gpu time per submission: count="<<gpu.submissions<<" mean_ms="<<gpu.mean_ms<<" p50_ms="<<gpu.p50_ms
                                    <<" p99_ms="<<gpu.p99_ms<<" max_ms="<<gpu.max_ms<<'\n';
    }
};

struct Summary {
    unsigned draws{},failed{},triangle_draws{},sprite_draws{},clear_draws{},other_draws{};
    std::uint64_t vertices{},pixels{};
    std::map<std::string,unsigned> targets,formats,blends,vertex_types,vram_textures,failures;
    void add(const GeDrawInfo &d){
        ++draws;vertices+=d.count;
        if(!d.rendered){++failed;++failures[d.error];}
        if(d.clear)++clear_draws;else if(d.triangles())++triangle_draws;else if(d.primitive==6u)++sprite_draws;else ++other_draws;
        if(d.rendered && d.triangles() && !d.clear)pixels+=d.written_pixels;
        const std::string kind=d.clear?"clear":GeDrawInfo::primitive_name(d.primitive);
        ++targets["fb="+hex(d.framebuffer)+" stride="+std::to_string(d.framebuffer_stride)+" zb="+hex(d.depthbuffer)+
            (d.clear?"":(d.depth_test?(d.depth_write?" ztest+zwrite ":" ztest "):" nodepth "))+kind];
        const std::string blend=!d.blend?"off":std::string(GeDrawInfo::blend_equation_name(d.blend_equation))+" src="+
            GeDrawInfo::blend_factor_name(d.blend_source,true)+" dst="+GeDrawInfo::blend_factor_name(d.blend_destination,false)+
            (d.blend_source==10u?" fix_a="+hex(d.blend_fix_a):"")+(d.blend_destination==10u?" fix_b="+hex(d.blend_fix_b):"");
        if(!d.clear)++blends[blend];
        ++formats[d.clear?"(clear)":!d.textured?"(untextured)":
            std::string(GeDrawInfo::texture_format_name(d.texture_format))+(d.swizzled?" swizzled":"")+
            (d.texture_levels>1u && (d.min_filter&4u)?" mips="+std::to_string(d.texture_levels):"")+
            " func="+GeDrawInfo::texture_function_name(d.texture_function)];
        char vtype[96];
        std::snprintf(vtype,sizeof vtype,"0x%06X %s uv=%u col=%u nrm=%u pos=%u weights=%u morphs=%u index=%u",d.vtype,
            d.through?"through":"transform",d.uv_type,d.color_type,d.normal_type,d.position_type,d.weight_count,d.morph_count,d.index_type);
        ++vertex_types[vtype];
        if(d.textured && d.texture_in_vram && !d.clear)
            ++vram_textures["tex="+hex(d.texture_address)+" "+std::to_string(d.texture_width)+"x"+std::to_string(d.texture_height)+
                " "+GeDrawInfo::texture_format_name(d.texture_format)+" -> fb="+hex(d.framebuffer)+" "+kind+" blend="+blend];
    }
    void print() const {
        std::cout<<"draws="<<draws<<" failed="<<failed<<" triangles="<<triangle_draws<<" sprites="<<sprite_draws
                 <<" clears="<<clear_draws<<" other="<<other_draws<<" vertices="<<vertices<<" triangle_pixels="<<pixels<<'\n';
        auto section=[](const char *title,const std::map<std::string,unsigned> &rows){
            if(rows.empty())return;
            std::cout<<title<<":\n";
            for(const auto &[key,count]:rows)std::cout<<"  "<<count<<"  "<<key<<'\n';
        };
        section("targets",targets);section("texture formats",formats);section("blend modes",blends);
        section("vertex types",vertex_types);section("textures in VRAM",vram_textures);section("failed draws",failures);
    }
};

struct Difference {std::uint64_t pixels{},different{},outside{};unsigned largest{};};
Difference compare_images(const Image &actual,const Image &expected,unsigned tolerance,Image &visual){
    Difference result;result.pixels=std::uint64_t(actual.width)*actual.height;
    visual.width=actual.width;visual.height=actual.height;visual.rgb.assign(actual.rgb.size(),0);
    for(std::size_t pixel=0;pixel<result.pixels;++pixel){
        unsigned delta=0;
        for(unsigned channel=0;channel<3;++channel){
            const int d=int(actual.rgb[pixel*3u+channel])-int(expected.rgb[pixel*3u+channel]);
            const unsigned magnitude=unsigned(d<0?-d:d);
            delta=std::max(delta,magnitude);
            visual.rgb[pixel*3u+channel]=static_cast<std::uint8_t>(std::min(255u,magnitude*8u));
        }
        if(delta){++result.different;result.largest=std::max(result.largest,delta);}
        if(delta>tolerance)++result.outside;
    }
    return result;
}

struct Options {
    std::vector<std::filesystem::path> captures;
    std::filesystem::path out,trace,frames;
    std::string backend{"software"},seed{"first"};
    std::vector<Surface> surfaces{
        {"lcd",0x04000000u,480,272,512,false},
        {"scene",0x040E8000u,512,320,512,false},
        {"composite",0x040D8000u,512,320,512,false},
        {"scratch",0x04088000u,512,160,512,false},
        {"depth",0x04088000u,512,320,512,true},
    };
    bool images{true},tracing{true},compare{},write_expected{},vram_sheet{},info{},summary{true},hardware_cull{true},hardware_transform{true};
    bool tolerance_set{},max_diff_set{};
    unsigned tolerance{},draws{0xFFFFFFFFu},repeat{1},scale{1},msaa{1},regs_draw{0xFFFFFFFFu};
    vcs::GeGpuConfig::Glow glow{vcs::GeGpuConfig::Glow::Game};
    double max_diff_percent{};
};

std::unique_ptr<ReplayBackend> make_backend(const Options &options,std::string &error){
    if(options.backend=="software")return std::make_unique<SoftwareBackend>();
    if(options.backend=="d3d12"){
        auto backend=std::make_unique<D3D12Backend>(options.scale,options.msaa,options.hardware_cull,options.hardware_transform,options.glow,error);
        if(!error.empty())return nullptr;
        return backend;
    }
    error="unknown backend \""+options.backend+"\" (software, d3d12)";return nullptr;
}

void usage(){
    std::cerr<<
        "usage: chili_ge_replay <capture.gecap> [more captures, in order] [options]\n"
        "  --backend <software|d3d12>  renderer to replay through (default software)\n"
        "  --out <dir>                 output directory (default: next to the capture)\n"
        "  --no-images                 do not write surface images\n"
        "  --surface <name>=<addr>,<w>,<h>[,<stride>[,depth]]  add or replace a surface\n"
        "  --vram-sheet                also write all of VRAM as a 512x1024 image\n"
        "  --trace <file>              per-draw trace (default <out>/<capture>.trace.jsonl)\n"
        "  --no-trace                  do not write the trace\n"
        "  --no-summary                do not print the per-list draw summary\n"
        "  --compare                   software: compare with the VRAM the game had after the list\n"
        "                              d3d12: compare lcd and the list's last target with a software\n"
        "                              replay of the same capture (default tolerance 8, 2% allowed)\n"
        "  --tolerance <n>             per-channel tolerance for --compare\n"
        "  --max-diff-percent <p>      share of a surface allowed outside tolerance\n"
        "  --write-expected            also write the reference surfaces\n"
        "  --frames <dir>              d3d12 --compare: reference | d3d12 | difference PNGs\n"
        "  --draws <n>                 stop every list after n draws\n"
        "  --repeat <n>                run n times and report timing\n"
        "  --scale <n> --msaa <n>      d3d12 internal scale and sample count (default 1, 1)\n"
        "  --glow <smooth|game|off>    d3d12: the game's glow drawn at the PSP's size and enlarged smoothly,\n"
        "                              as the game's draws say (default), or left out\n"
        "  --no-hw-cull                d3d12: do not cull GPU-transformed draws (diagnostic)\n"
        "  --no-hw-transform           d3d12: transform and clip on the CPU\n"
        "  --seed <first|all|none>     d3d12: copy the capture's VRAM surfaces to the GPU before the\n"
        "                              first list, every list, or never (default first)\n"
        "  --regs <n>                  print the GE registers and matrices as draw n starts\n"
        "  --info                      print the capture header and exit\n"
        "exit: 0 ok, 1 usage or I/O error, 2 list did not complete, 3 comparison failed\n";
}

bool parse_unsigned(std::string_view text,unsigned &value){
    int base=10;
    if(text.starts_with("0x") || text.starts_with("0X")){text.remove_prefix(2);base=16;}
    const auto result=std::from_chars(text.data(),text.data()+text.size(),value,base);
    return result.ec==std::errc{} && result.ptr==text.data()+text.size();
}

bool parse_surface(std::string_view text,Surface &surface){
    const auto equals=text.find('=');
    if(equals==std::string_view::npos || !equals)return false;
    surface.name=std::string(text.substr(0,equals));text.remove_prefix(equals+1);
    std::vector<std::string_view> fields;
    while(true){
        const auto comma=text.find(',');fields.push_back(text.substr(0,comma));
        if(comma==std::string_view::npos)break;
        text.remove_prefix(comma+1);
    }
    if(fields.size()<3 || fields.size()>5 || !parse_unsigned(fields[0],surface.address) ||
       !parse_unsigned(fields[1],surface.width) || !parse_unsigned(fields[2],surface.height))return false;
    surface.stride=512;surface.depth=false;
    if(fields.size()>=4 && !parse_unsigned(fields[3],surface.stride))return false;
    if(fields.size()==5){if(fields[4]!="depth")return false;surface.depth=true;}
    return true;
}

bool parse_options(int argc,char **argv,Options &options){
    for(int i=1;i<argc;++i){
        const std::string_view argument=argv[i];
        auto value=[&](std::string_view &out){if(i+1>=argc)return false;out=argv[++i];return true;};
        std::string_view text;
        if(argument=="--backend"){if(!value(text))return false;options.backend=text;}
        else if(argument=="--out"){if(!value(text))return false;options.out=text;}
        else if(argument=="--trace"){if(!value(text))return false;options.trace=text;}
        else if(argument=="--frames"){if(!value(text))return false;options.frames=text;}
        else if(argument=="--seed"){if(!value(text) || (text!="first" && text!="all" && text!="none"))return false;options.seed=text;}
        else if(argument=="--surface"){
            Surface surface;
            if(!value(text) || !parse_surface(text,surface))return false;
            const auto existing=std::find_if(options.surfaces.begin(),options.surfaces.end(),[&](const Surface &s){return s.name==surface.name;});
            if(existing!=options.surfaces.end())*existing=surface;else options.surfaces.push_back(surface);
        }
        else if(argument=="--tolerance"){if(!value(text) || !parse_unsigned(text,options.tolerance))return false;options.tolerance_set=true;}
        else if(argument=="--draws"){if(!value(text) || !parse_unsigned(text,options.draws))return false;}
        else if(argument=="--regs"){if(!value(text) || !parse_unsigned(text,options.regs_draw))return false;}
        else if(argument=="--repeat"){if(!value(text) || !parse_unsigned(text,options.repeat) || !options.repeat)return false;}
        else if(argument=="--scale"){if(!value(text) || !parse_unsigned(text,options.scale) || !options.scale || options.scale>8u)return false;}
        else if(argument=="--msaa"){if(!value(text) || !parse_unsigned(text,options.msaa) || !options.msaa || options.msaa>8u)return false;}
        else if(argument=="--glow"){
            if(!value(text))return false;
            if(text=="smooth")options.glow=vcs::GeGpuConfig::Glow::Smooth;
            else if(text=="game")options.glow=vcs::GeGpuConfig::Glow::Game;
            else if(text=="off")options.glow=vcs::GeGpuConfig::Glow::Off;
            else return false;
        }
        else if(argument=="--max-diff-percent"){
            if(!value(text))return false;
            char *end{};const std::string copy(text);options.max_diff_percent=std::strtod(copy.c_str(),&end);
            if(end==copy.c_str() || *end || options.max_diff_percent<0)return false;
            options.max_diff_set=true;
        }
        else if(argument=="--no-images")options.images=false;
        else if(argument=="--no-trace")options.tracing=false;
        else if(argument=="--no-summary")options.summary=false;
        else if(argument=="--compare")options.compare=true;
        else if(argument=="--write-expected")options.write_expected=true;
        else if(argument=="--vram-sheet")options.vram_sheet=true;
        else if(argument=="--no-hw-cull")options.hardware_cull=false;
        else if(argument=="--no-hw-transform")options.hardware_transform=false;
        else if(argument=="--info")options.info=true;
        else if(!argument.starts_with("--"))options.captures.emplace_back(argument);
        else return false;
    }
    return !options.captures.empty();
}

void print_info(const chili::ge_capture::File &capture){
    using namespace chili::ge_capture;
    const auto &s=capture.scalars;
    auto r=[&](unsigned i){return capture.state.commands[i]&0xFFFFFFu;};
    std::cout<<"ordinal="<<s[Ordinal]<<" list="<<hex(s[ListAddress])<<" callback="<<hex(s[CallbackId])
             <<" finish="<<hex(s[CallbackFinish])<<" sdk="<<hex(s[SdkVersion])<<'\n'
             <<"vertices="<<hex(s[VertexAddress])<<" indices="<<hex(s[IndexAddress])<<" offset="<<hex(s[OffsetAddress])
             <<" clut_source="<<hex(s[ClutAddress])<<" next_list="<<s[NextList]<<'\n'
             <<"framebuffer="<<hex(0x04000000u|(r(0x9C)&0x1FFFF0u))<<" stride="<<(r(0x9D)&0x7FCu)
             <<" depthbuffer="<<hex(0x04000000u|(r(0x9E)&0x1FFFF0u))<<'\n'
             <<"ram="<<capture.ram.size()<<" vram="<<capture.vram.size()<<" scratchpad="<<capture.scratchpad.size()
             <<" host_mxcsr="<<hex(s[HostMxcsr])<<'\n';
    if(capture.has_after){
        const auto &a=capture.after_scalars;
        std::cout<<"after: completed="<<a[Completed]<<" id="<<a[ListId]<<" commands="<<a[Commands]<<" draws="<<a[Draws]
                 <<" end="<<hex(a[EndCursor])<<'\n';
    }else std::cout<<"after: not recorded\n";
    for(const auto &slot:capture.slots)std::cout<<"chunk "<<tag_text(slot)<<" offset="<<slot.offset<<" size="<<slot.size<<'\n';
}

// Registers and cursors after the replay against what the game had.
unsigned compare_state(const chili::GeExecutorState &actual,const chili::GeExecutorState &expected){
    unsigned mismatches=0;
    auto check=[&](const char *what,bool same){if(!same){++mismatches;std::cout<<"state mismatch: "<<what<<'\n';}};
    check("registers",actual.commands==expected.commands);
    check("world matrix",std::memcmp(actual.world.data(),expected.world.data(),sizeof actual.world)==0);
    check("view matrix",std::memcmp(actual.view.data(),expected.view.data(),sizeof actual.view)==0);
    check("projection matrix",std::memcmp(actual.projection.data(),expected.projection.data(),sizeof actual.projection)==0);
    check("texture matrix",std::memcmp(actual.texture.data(),expected.texture.data(),sizeof actual.texture)==0);
    check("bone matrices",std::memcmp(actual.bones.data(),expected.bones.data(),sizeof actual.bones)==0);
    check("morph weights",std::memcmp(actual.morph.data(),expected.morph.data(),sizeof actual.morph)==0);
    check("matrix cursors",actual.bone_cursor==expected.bone_cursor && actual.world_cursor==expected.world_cursor &&
        actual.view_cursor==expected.view_cursor && actual.texture_cursor==expected.texture_cursor &&
        actual.projection_cursor==expected.projection_cursor);
    check("vertex/index/offset cursors",actual.vertex_address==expected.vertex_address &&
        actual.index_address==expected.index_address && actual.offset_address==expected.offset_address);
    check("CLUT latch",actual.clut_bytes==expected.clut_bytes && actual.clut_loaded==expected.clut_loaded);
    return mismatches;
}

void restore_capture(psprecomp::Runtime &rt,chili::GeRuntime &ge,const chili::ge_capture::File &capture){
    rt.memory().copy_in(psprecomp::GuestMemory::kPhysicalBase,capture.ram);
    rt.memory().copy_in(kVramBase,capture.vram);
    rt.memory().copy_in(psprecomp::GuestMemory::kScratchpadBase,capture.scratchpad);
    ge.restore(capture.state);ge.enqueued_lists=capture.scalars[chili::ge_capture::Ordinal];
}
}

int main(int argc,char **argv){
    try{
        Options options;
        if(!parse_options(argc,argv,options)){usage();return 1;}
        using namespace chili::ge_capture;
        std::string error;
        if(options.info){
            for(const auto &path:options.captures){
                File capture;
                if(!capture.load(path,error)){std::cerr<<"chili_ge_replay: "<<error<<'\n';return 1;}
                print_info(capture);
            }
            return 0;
        }
        if(options.out.empty())options.out=options.captures.front().parent_path();
        if(options.out.empty())options.out=".";
        std::filesystem::create_directories(options.out);
        if(!options.frames.empty())std::filesystem::create_directories(options.frames);
        if(!options.trace.empty() && options.captures.size()>1u){std::cerr<<"chili_ge_replay: --trace names one file; omit it with several captures\n";return 1;}

        // The game-side capture and dump switches must not act on the replay itself.
        for(const char *name:{"CHILI_GE_CAPTURE_DIR","CHILI_GE_CAPTURE_LISTS","CHILI_GE_TRACE","CHILI_GE_TRACE_LISTS",
                              "PSPRECOMP_GE_DUMP","PSPRECOMP_GE_DUMP_FAILURE"})_putenv_s(name,"");

        auto backend=make_backend(options,error);
        if(!backend){std::cerr<<"chili_ge_replay: "<<error<<'\n';return 1;}
        const bool gpu=!backend->writes_guest_vram();
        if(gpu){
            // A hardware renderer is compared by eye-level tolerance, not by bytes.
            if(!options.tolerance_set)options.tolerance=8u;
            if(!options.max_diff_set)options.max_diff_percent=2.0;
        }

        std::unique_ptr<psprecomp::Runtime> rt,reference_rt;
        std::unique_ptr<chili::GeRuntime> ge,reference_ge;
        unsigned ram_size=0;
        int exit_code=0;
        for(std::size_t index=0;index<options.captures.size();++index){
            const auto &path=options.captures[index];
            File capture;
            if(!capture.load(path,error)){std::cerr<<"chili_ge_replay: "<<error<<'\n';return 1;}
            if(options.compare && !gpu && !capture.has_after){std::cerr<<"chili_ge_replay: capture has no post-list state to compare with\n";return 1;}
            if(!rt){
                ram_size=capture.scalars[RamSize];
                rt=std::make_unique<psprecomp::Runtime>(ram_size);ge=std::make_unique<chili::GeRuntime>(*rt);
                ge->draw_limit=options.draws;
            }else if(capture.scalars[RamSize]!=ram_size){std::cerr<<"chili_ge_replay: captures differ in RAM size\n";return 1;}
            const std::string stem=path.stem().string();
            const auto trace_path=!options.tracing?std::filesystem::path():
                options.trace.empty()?options.out/(stem+".trace.jsonl"):options.trace;

            if(gpu && (options.seed=="all" || (options.seed=="first" && index==0u)))backend->seed(capture.vram);

            Summary summary;
            chili::GeRuntime::ListRun run;bool executed=false;
            std::vector<double> pass_ms;
            for(unsigned pass=0;pass<options.repeat;++pass){
                restore_capture(*rt,*ge,capture);
                const bool first=pass==0;
                if(first){
                    if(options.tracing && !ge->trace().open(trace_path)){std::cerr<<"chili_ge_replay: cannot open "<<trace_path.string()<<'\n';return 1;}
                    ge->trace().set_sink([&](const GeDrawInfo &draw){
                        summary.add(draw);
                        if(draw.index!=options.regs_draw)return;
                        std::cout<<"registers at draw "<<draw.index<<":\n";
                        for(unsigned reg=0;reg<256;++reg)if(ge->commands[reg]&0xFFFFFFu)
                            std::printf("  %02X %06X %g\n",reg,ge->commands[reg]&0xFFFFFFu,double(std::bit_cast<float>(ge->commands[reg]<<8)));
                        auto matrix=[](const char *name,const auto &values,unsigned count){
                            std::printf("  %s",name);for(unsigned i=0;i<count;++i)std::printf(" %g",double(values[i]));std::printf("\n");
                        };
                        matrix("world",ge->world,12);matrix("view",ge->view,12);matrix("projection",ge->projection,16);matrix("texture",ge->texture,12);
                        std::fflush(stdout);
                    });
                }
                ge->set_trace_all(first);
                run={};
                const auto started=std::chrono::steady_clock::now();
                executed=backend->execute(*rt,*ge,capture.scalars[ListAddress],run);
                pass_ms.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count());
                if(!executed)break;
            }
            ge->trace().set_sink({});
            std::cout<<"backend="<<backend->name()<<" list="<<hex(capture.scalars[ListAddress])<<" ordinal="<<capture.scalars[Ordinal]
                     <<" completed="<<run.completed<<" commands="<<run.commands<<" draws="<<run.draws<<" end="<<hex(run.end);
            if(run.truncated)std::cout<<" (stopped at the draw limit)";
            std::cout<<'\n';
            if(!executed)std::cout<<"stopped: "<<rt->stop_reason()<<'\n';
            // The first pass feeds the trace and summary; later passes time the list alone.
            // For a GPU backend the time includes recording and submitting the frame.
            std::cout<<"time_ms traced_pass="<<pass_ms.front();
            if(pass_ms.size()>1){
                double best=pass_ms[1],total=0;
                for(std::size_t i=1;i<pass_ms.size();++i){best=std::min(best,pass_ms[i]);total+=pass_ms[i];}
                std::cout<<" untraced_best="<<best<<" untraced_mean="<<total/double(pass_ms.size()-1u)<<" untraced_passes="<<pass_ms.size()-1u;
            }
            if(gpu)std::cout<<" last_submit="<<backend->submit_ms;
            std::cout<<'\n';
            if(options.summary)summary.print();
            if(options.tracing)std::cout<<"trace: "<<trace_path.string()<<'\n';
            bool comparison_failed=false;
            if(gpu){
                const std::string exceptions=backend->take_exceptions();
                std::cout<<"exceptions: "<<(exceptions.empty()?"none":exceptions)<<'\n';
                if(summary.failed || !exceptions.empty())comparison_failed=options.compare;
            }

            // What the list should have produced: the game's own VRAM for the
            // software backend, a software replay of the capture for a GPU one.
            std::span<const std::uint8_t> expected_vram;
            if(gpu && (options.compare || options.write_expected)){
                if(!reference_rt){
                    reference_rt=std::make_unique<psprecomp::Runtime>(ram_size);
                    reference_ge=std::make_unique<chili::GeRuntime>(*reference_rt);
                    reference_ge->draw_limit=options.draws;
                }
                restore_capture(*reference_rt,*reference_ge,capture);
                chili::GeRuntime::ListRun reference_run;
                if(!reference_ge->execute_list(*reference_rt,capture.scalars[ListAddress],reference_run))
                    std::cout<<"reference replay stopped: "<<reference_rt->stop_reason()<<'\n';
                expected_vram=reference_rt->memory().vram_bytes();
            }else if(capture.has_after)expected_vram=capture.after_vram;

            auto surfaces=options.surfaces;
            if(options.vram_sheet)surfaces.push_back({"vram_sheet",kVramBase,512,1024,512,false});
            // A GPU backend is judged on the picture: the LCD and the surface the
            // list drew last, both over the visible 480x272. Other surfaces alias
            // each other in VRAM (depth under the scratch sheet, the scene under
            // the composite) and are reported without counting.
            const unsigned last_target=kVramBase|(ge->commands[0x9C]&0x1FFFF0u);
            if(gpu && last_target!=kVramBase)surfaces.insert(surfaces.begin()+1,{"target",last_target,480,272,512,false});
            for(const auto &surface:surfaces){
                Image actual,native;
                if(!backend->read_surface(*rt,surface,actual,&native)){
                    if(!gpu)std::cerr<<"surface "<<surface.name<<": outside VRAM, skipped\n";
                    continue;
                }
                const auto base=options.out/(stem+"_"+surface.name);
                if(options.images && !write_png(base.string()+".png",actual)){std::cerr<<"cannot write "<<base.string()<<".png\n";return 1;}
                if(options.images && native.width)write_png(base.string()+"_native.png",native);
                if(expected_vram.empty() || (!options.compare && !options.write_expected))continue;
                Image expected;
                if(!decode_surface(expected_vram,surface,expected))continue;
                if(options.write_expected && !write_png(base.string()+"_expected.png",expected)){std::cerr<<"cannot write "<<base.string()<<"_expected.png\n";return 1;}
                if(!options.compare)continue;
                if(actual.width!=expected.width || actual.height!=expected.height){
                    std::cout<<"compare "<<surface.name<<": backend surface is "<<actual.width<<"x"<<actual.height<<", not compared\n";continue;
                }
                Image visual;
                const auto difference=compare_images(actual,expected,options.tolerance,visual);
                const double percent=difference.pixels?100.0*double(difference.outside)/double(difference.pixels):0.0;
                const bool counted=!gpu || surface.name=="lcd" || surface.name=="target";
                const bool failed=counted && percent>options.max_diff_percent;
                std::cout<<"compare "<<surface.name<<": pixels="<<difference.pixels<<" different="<<difference.different
                         <<" outside_tolerance="<<difference.outside<<" ("<<percent<<"%) largest_delta="<<difference.largest
                         <<(!counted?" (not counted)":failed?" FAIL":" ok")<<'\n';
                if(difference.different && options.images){
                    write_png(base.string()+"_diff.png",visual);
                    if(!options.write_expected)write_png(base.string()+"_expected.png",expected);
                }
                if(gpu && counted && !options.frames.empty())
                    write_png(options.frames/(stem+"_"+surface.name+".png"),side_by_side({&expected,&actual,&visual}));
                comparison_failed=comparison_failed || failed;
            }
            if(options.compare && !gpu){
                const auto &vram=rt->memory().vram_bytes();
                std::size_t different=0,first_difference=0;
                for(std::size_t i=0;i<vram.size();++i)if(vram[i]!=capture.after_vram[i]){if(!different)first_difference=i;++different;}
                std::cout<<"compare vram_bytes: different="<<different;
                if(different)std::cout<<" first="<<hex(kVramBase+static_cast<unsigned>(first_difference));
                std::cout<<'\n';
                if(different && !options.tolerance && options.max_diff_percent==0)comparison_failed=true;
            }
            if(options.compare && capture.has_after){
                const auto &after=capture.after_scalars;
                if(!run.truncated && (run.completed!=(after[Completed]!=0) || run.commands!=after[Commands] || run.draws!=after[Draws])){
                    std::cout<<"compare list: game completed="<<after[Completed]<<" commands="<<after[Commands]<<" draws="<<after[Draws]<<" FAIL\n";
                    comparison_failed=true;
                }
                if(!run.truncated && compare_state(ge->snapshot(),capture.after_state))comparison_failed=true;
            }
            if(options.compare)std::cout<<"compare: "<<(comparison_failed?"FAIL":"ok")<<'\n';
            if(!executed || (!run.completed && !run.truncated))exit_code=std::max(exit_code,2);
            else if(comparison_failed)exit_code=std::max(exit_code,3);
            if(exit_code==2)break;
        }
        backend->print_report();
        // The draw backend must not outlive the renderer it calls.
        ge.reset();reference_ge.reset();backend.reset();
        return exit_code;
    }catch(const std::exception &failure){
        std::cerr<<"chili_ge_replay: "<<failure.what()<<'\n';return 1;
    }
}
