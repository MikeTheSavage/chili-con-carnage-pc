#include "ge_runtime.hpp"
#include "required_mip_triangle_reference.hpp"
#include "required_mip_sampler_reference.hpp"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
using Regs=std::array<unsigned,256>;
using Bytes=std::vector<std::uint8_t>;
unsigned checks{};
constexpr unsigned vertex=0x08808000,palette_address=0x08818000;
void require(bool value,const std::string &why){++checks;if(!value)throw std::runtime_error(why);}
Bytes read(const std::filesystem::path &path,std::size_t count){std::ifstream in(path,std::ios::binary);
    require(bool(in),"missing input "+path.string());in.seekg(0,std::ios::end);require(in.tellg()==std::streamoff(count),"wrong binary size");
    in.seekg(0);Bytes out(count);in.read(reinterpret_cast<char*>(out.data()),std::streamsize(count));require(bool(in),"input read failed");return out;}
bool same_cpu(const psprecomp::AllegrexContext &a,const psprecomp::AllegrexContext &b){return a.gpr==b.gpr && a.pc==b.pc && a.hi==b.hi && a.lo==b.lo && a.fcr31==b.fcr31 && a.vfpu_ctrl==b.vfpu_ctrl && std::memcmp(a.fpr.data(),b.fpr.data(),sizeof(a.fpr))==0 && std::memcmp(a.vfpu.data(),b.vfpu.data(),sizeof(a.vfpu))==0;}
void seed_cpu(psprecomp::AllegrexContext &c){for(unsigned n=1;n<32;++n)c.gpr[n]=0xA0000000+n;c.pc=0x08801000;c.hi=0x1357;c.lo=0x2468;c.fcr31=0x1E0001;
    for(unsigned n=0;n<c.fpr.size();++n)c.fpr[n]=std::bit_cast<float>(0x7FC10000+n);for(unsigned n=0;n<c.vfpu.size();++n)c.vfpu[n]=std::bit_cast<float>(0xFFC10000+n);}
struct MemoryState{Bytes ram,vram;std::array<std::uint8_t,psprecomp::GuestMemory::kScratchpadSize> scratch{};
    explicit MemoryState(const psprecomp::GuestMemory &m):ram(m.bytes()),vram(m.vram_bytes()){m.copy_out(0x10000,scratch);}
    bool same(const psprecomp::GuestMemory &m) const{std::array<std::uint8_t,psprecomp::GuestMemory::kScratchpadSize> s{};m.copy_out(0x10000,s);return ram==m.bytes()&&vram==m.vram_bytes()&&scratch==s;}
    bool source_same(const psprecomp::GuestMemory &m) const{std::array<std::uint8_t,psprecomp::GuestMemory::kScratchpadSize> s{};m.copy_out(0x10000,s);return ram==m.bytes()&&scratch==s;}};
struct Input{Regs regs{};std::array<float,12> world{},view{};std::array<float,16> projection{};unsigned vertices{},data{};Bytes source,texture,palette,vram;};
Input actual(const std::filesystem::path &prefix){Input out;std::ifstream in(prefix.string()+".txt");require(bool(in),"actual metadata missing");std::string line;
    while(std::getline(in,line)){std::istringstream w(line);std::string tag;w>>tag;
        if(tag=="reg"){unsigned index,value;w>>std::hex>>index>>value;require(bool(w)&&index<256,"register parse failed");out.regs[index]=value;}
        else if(tag=="world"||tag=="view"||tag=="projection"){auto parse=[&](auto &matrix){for(auto &f:matrix){w>>f;require(bool(w)&&std::isfinite(f),"matrix parse failed");}};if(tag=="world")parse(out.world);else if(tag=="view")parse(out.view);else parse(out.projection);}
        else if(tag.starts_with("primitive=")){out.data=unsigned(std::stoul(tag.substr(10),nullptr,16));w>>tag;require(tag.starts_with("vertices="),"vertices missing");out.vertices=unsigned(std::stoul(tag.substr(9),nullptr,16));}
        else if(tag.starts_with("vertex_stride=")){require(std::stoul(tag.substr(14),nullptr,16)==16,"actual hex stride changed");}}
    require(out.data==0x50004 && out.vertices==0x09943A58 && (out.regs[0x12]&0xFFFFFF)==0x80011E,"actual draw identity changed");
    require((out.regs[0xC2]&0xFFFFFF)==0x050001 && (out.regs[0xAD]&0x7FF)==0,"actual unused-level blocker changed");
    out.source=read(prefix.string()+"_vertices.bin",64);out.texture=read(prefix.string()+"_texture.bin",4096);out.palette=read(prefix.string()+"_palette_source.bin",64);out.vram=read(prefix.string()+"_vram.bin",0x200000);return out;}
struct Captured{psprecomp::Runtime rt;chili::GeClutCache clut;unsigned cursor=0xFEEDCAFE,index=0xBAADF00D,written=0xDEADF00D;std::string error;
    explicit Captured(const Input &in){auto &m=rt.memory();m.copy_in(in.vertices,in.source);m.copy_in(0x04000000,in.vram);
        const unsigned texture=(in.regs[0xA0]&0xFFFFF0)|((in.regs[0xA8]<<8)&0xF000000),palette=(in.regs[0xB0]&0xFFFFF0)|((in.regs[0xB1]<<8)&0xF000000);
        m.copy_in(texture,in.texture);m.copy_in(palette,in.palette);require(clut.load(m,in.regs[0xB0],in.regs[0xB1],in.regs[0xC4],error),error);seed_cpu(rt.cpu());}};
using Draw=bool(*)(psprecomp::GuestMemory&,const Regs&,const std::array<float,12>&,const std::array<float,12>&,const std::array<float,16>&,unsigned,unsigned,unsigned&,std::string&,const chili::GeClutCache&,unsigned*,unsigned,unsigned*,const chili::ge::DeformState*);
bool draw(Draw fn,Captured &f,const Input &in,const Regs &r){return fn(f.rt.memory(),r,in.world,in.view,in.projection,in.vertices,in.data,f.cursor,f.error,f.clut,&f.written,0,&f.index,nullptr);}
void actual_capture(const Input &in){Captured baseline(in),candidate(in),strict(in);const MemoryState before(candidate.rt.memory());const auto cpu=candidate.rt.cpu();
    require(!draw(chili_required_baseline::render_ge_triangle_raster,strict,in,in.regs)&&strict.error=="invalid mip texture stride","unmodified baseline did not reproduce actual blocker");
    require(strict.cursor==0xFEEDCAFE&&strict.index==0xBAADF00D&&strict.written==0xDEADF00D&&before.same(strict.rt.memory()),"old strict stop atomicity");
    Regs reference=in.regs;reference[0xC2]&=~(7u<<16);
    // Independent verify_usage.py proves all 1,140 covered fragments use only
    // L0/MAG, so this fixture-only max0 counterfactual has identical sampling.
    require(draw(chili_required_baseline::render_ge_triangle_raster,baseline,in,reference),"reference max0 failed: "+baseline.error);
    require(draw(chili::render_ge_triangle_raster,candidate,in,in.regs),"actual required-level render failed: "+candidate.error);
    require(candidate.rt.memory().vram_bytes()==baseline.rt.memory().vram_bytes(),"actual entire2MiB differs from independent max0 baseline");
    require(candidate.written==baseline.written&&candidate.written==1140,"actual committed fragment count differs");
    require(candidate.cursor==0x09943A98&&baseline.cursor==candidate.cursor&&candidate.index==0&&baseline.index==0,"actual dual cursors differ");
    require(before.source_same(candidate.rt.memory())&&before.source_same(baseline.rt.memory())&&same_cpu(candidate.rt.cpu(),cpu)&&same_cpu(baseline.rt.cpu(),cpu),"actual source/scratch/CPU changed");
    const Bytes expected=baseline.rt.memory().vram_bytes();
    for(bool enabled:{false,true}){Captured f(in);chili::ThreadRuntime scheduler(f.rt);std::ostringstream output;chili::PerformanceTelemetry telemetry(output);
        chili::GeRuntime ge(f.rt,&scheduler,0,nullptr,enabled?&telemetry:nullptr);ge.commands=in.regs;ge.clut=f.clut;ge.world=in.world;ge.view=in.view;ge.projection=in.projection;ge.vertex_address=in.vertices;
        constexpr unsigned list=0x09F00000;f.rt.memory().store32(list,0x04000000|in.data);f.rt.memory().store32(list+4,0x0F000000);f.rt.memory().store32(list+8,0x0C000000);
        f.rt.cpu().gpr[4]=list;f.rt.cpu().gpr[5]=0;f.rt.cpu().gpr[6]=0xFFFFFFFF;f.rt.cpu().gpr[7]=0;const MemoryState sources(f.rt.memory());auto wanted=f.rt.cpu();wanted.gpr[2]=1;
        const auto uid=psprecomp::runtime_thread_uid();const auto generation=psprecomp::runtime_thread_switch_generation();const auto time=scheduler.now_us();
        f.rt.invoke_import("sceGe_user",0xAB49E76A,f.rt.cpu());auto commands=in.regs;commands[4]=0x04000000|in.data;commands[0x0F]=0x0F000000;
        require(!f.rt.stopped()&&ge.completed.contains(1)&&ge.vertex_address==0x09943A98&&ge.index_address==0&&ge.commands==commands&&ge.world==in.world&&ge.view==in.view&&ge.projection==in.projection,"actual native HLE complete state changed");
        require(f.rt.memory().vram_bytes()==expected&&sources.source_same(f.rt.memory())&&same_cpu(f.rt.cpu(),wanted)&&scheduler.now_us()==time&&psprecomp::runtime_thread_uid()==uid&&psprecomp::runtime_thread_switch_generation()==generation,"actual native HLE fullmemory/CPU/thread/time parity failed");
        chili::PerformanceTelemetry::Sample sample;require(telemetry.sample(chili::PerformanceTelemetry::Clock::now(),time,sample,true),"telemetry sample failed");
        if(enabled)require(sample.total[chili::PerformanceTelemetry::DrawSuccesses]==1&&sample.total[chili::PerformanceTelemetry::TriangleCommittedFragments]==candidate.written,"actual enabled counter attribution changed");else for(auto counter:sample.total)require(counter==0,"null telemetry changed counters");}
    std::cout<<"PASS actual1140 coverage whole2MiB max0-baseline oracle; committed="<<candidate.written<<" dualcursors09943A98 nativeHLE null/enabled state parity\n";
}
struct Synthetic{psprecomp::Runtime rt;Regs regs{};chili::GeClutCache clut;std::array<float,12> world{1,0,0,0,1,0,0,0,1,0,0,0},view=world;std::array<float,16> projection{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    unsigned cursor=0xFEEDCAFE,index=0xBAADF00D,written=0xDEADF00D;std::string error;
    Synthetic(){auto &m=rt.memory();seed_cpu(rt.cpu());Bytes v(m.vram_size());for(unsigned n=0;n<v.size();++n)v[n]=std::uint8_t(n*17+23);m.copy_in(0x04000000,v);
        regs[0x12]=0x80011E;regs[0x1E]=1;regs[0xD2]=3;regs[0x9D]=512;regs[0x9E]=0x80000;regs[0x9F]=512;regs[0xD5]=regs[0x16]=63|(31<<10);regs[0xD7]=65535;regs[0xE7]=1;
        regs[0xC2]=5<<16;regs[0xC3]=3;regs[0xC6]=0x107;regs[0xC9]=0x103;regs[0x55]=0xFFFFFF;regs[0x58]=255;
        for(unsigned level=0;level<6;++level){const unsigned address=0x08820000+level*0x10000;regs[0xA0+level]=address&0xFFFFFF;regs[0xA8+level]=0x80000|16;regs[0xB8+level]=4|(4<<8);
            for(unsigned n=0;n<256;++n)m.store32(address+n*4,0xFF010203u+level*0x00102030u);}
        for(unsigned n=0;n<32;++n)m.store32(palette_address+n*4,0xFF000000u+n*0x00030507u);require(clut.load(m,palette_address&0xFFFFFF,(palette_address>>8)&0xF0000,4,error),error);}
    void vertex_at(unsigned n,unsigned u,unsigned v,int x,int y){const unsigned a=vertex+n*16;auto &m=rt.memory();m.store16(a,u);m.store16(a+2,v);m.store32(a+4,0xFFFFFFFF);m.store16(a+8,x);m.store16(a+10,y);m.store16(a+12,7);m.store16(a+14,0xBEEF);}
    void triangles(unsigned first_uv=16,unsigned last_uv=384){vertex_at(0,0,0,0,0);vertex_at(1,first_uv,0,16,0);vertex_at(2,0,first_uv,0,16);
        vertex_at(3,0,0,32,0);vertex_at(4,last_uv,0,48,0);vertex_at(5,0,last_uv,32,16);}
    bool triangle(unsigned count=6){return chili::render_ge_triangle_raster(rt.memory(),regs,world,view,projection,vertex,0x30000|count,cursor,error,clut,&written,0,&index);}
    bool sprites(unsigned count=4){return chili::render_ge_sprites(rt.memory(),regs,vertex,0x60000|count,cursor,error,&clut);}
    void rectangles(unsigned last_uv=384){vertex_at(0,0,0,0,0);vertex_at(1,16,16,16,16);vertex_at(2,0,0,32,0);vertex_at(3,last_uv,last_uv,48,16);}
};
void mask_api(){Synthetic f;auto &m=f.rt.memory();chili::GeMipTexture sampler;std::string error;unsigned color=0xDEADBEEF;chili::GeMipSelection selection;
    require(sampler.configure(m,f.regs,nullptr,error)&&sampler.prepared_levels()==63&&sampler.max_level()==5,"default full mask changed");
    f.regs[0xAD]=0;require(!sampler.configure(m,f.regs,nullptr,error),"default full validation weakened");require(sampler.prepared_levels()==63,"failed full configure mutated sampler");
    require(!sampler.configure(m,f.regs,nullptr,error,64)&&sampler.prepared_levels()==63,"outofrange mask accepted/mutated");
    require(sampler.configure(m,f.regs,nullptr,error,0)&&sampler.prepared_levels()==0&&sampler.max_level()==5&&sampler.level(0).width==16&&sampler.level(5).stride==0,"metadata-only global/base setup wrong");
    require(sampler.select(1,1,selection,error)&&selection.lower==0,"metadata selection failed");require(!sampler.sample(m,0,0,selection,color,error)&&color==0xDEADBEEF,"unprepared sample touched color");
    require(sampler.configure(m,f.regs,nullptr,error,1)&&sampler.sample(m,0,0,selection,color,error)&&color==0xFF010203,"selected0 sample failed");
    const auto prepared=sampler.prepared_levels();require(!sampler.configure(m,f.regs,nullptr,error,33)&&sampler.prepared_levels()==prepared,"required invalid level accepted/configuration atomicity broken");
    f.regs[0xC8]=0x480001;require(sampler.configure(m,f.regs,nullptr,error,16),"required4 setup failed");require(sampler.select(0,1,selection,error)&&selection.lower==4&&selection.upper==5&&selection.fraction16==8,"positive upper selector failed");
    color=0xDEADBEEF;require(!sampler.sample(m,0,0,selection,color,error)&&color==0xDEADBEEF,"unprepared positive upper accepted");
    selection.fraction16=0;require(sampler.sample(m,0,0,selection,color,error),"zero-weight unprepared upper rejected");
    f.regs[0xBD]=0xF0F;require(sampler.configure(m,f.regs,nullptr,error,1),"unused dimensions rejected");require(!sampler.configure(m,f.regs,nullptr,error,33),"required oversized dimensions accepted");
    f.regs[0xB8]=0xA04;require(!sampler.configure(m,f.regs,nullptr,error,0),"unsupported base dimensions accepted for normalization");
    f.regs[0xB8]=0x404;f.regs[0xC3]=6;require(!sampler.configure(m,f.regs,nullptr,error,0),"unknown global format accepted metadata-only");
    f.regs[0xC3]=4;f.regs[0xC2]|=0x100;f.regs[0xC5]=0xFF03;for(unsigned l=0;l<6;++l)f.regs[0xA8+l]=0x80020;f.regs[0xBD]=0x404;
    require(sampler.configure(m,f.regs,nullptr,error,0),"metadata-only indexed requires cache");require(!sampler.configure(m,f.regs,nullptr,error,1),"required indexed accepts absent cache");
    require(sampler.configure(m,f.regs,&f.clut,error,3),"multiclut prepared0/1 with32entries failed");require(!sampler.configure(m,f.regs,&f.clut,error,7),"required multiclut2 missing entries accepted");
    require(sampler.prepared_levels()==3,"failed palette setup mutated prepared mask");
    std::cout<<"PASS default-full/mask0/range/prepared-guard/palette/base-dimension contracts\n";
}
void atomic_failures(){for(unsigned kind=0;kind<5;++kind)for(bool sprite:{false,true}){Synthetic f;f.triangles();if(sprite)f.rectangles();
        if(kind==0)f.regs[0xAD]=0; // second footprint24 => L4/L5 fraction8
        if(kind==1){f.regs[0xA5]=0xFFFFF0;f.regs[0xAD]=0x90010;}
        if(kind==2){f.regs[0xA5]=0;f.regs[0xAD]=0x40200;} // actual selected reads revisit the first rectangle's rows/columns
        if(kind==3){f.regs[0xC3]=4;f.regs[0xC2]|=0x100;f.regs[0xC5]=0xFF03;for(unsigned n=0;n<6;++n)f.regs[0xA8+n]=0x80020;}
        if(kind==4)f.regs[0xBD]=0xA04;
        const MemoryState before(f.rt.memory());const auto cpu=f.rt.cpu();require(!(sprite?f.sprites():f.triangle()),"late required invalid accepted kind="+std::to_string(kind)+" sprite="+std::to_string(sprite));
        require(!f.error.empty()&&before.same(f.rt.memory())&&same_cpu(f.rt.cpu(),cpu)&&f.cursor==0xFEEDCAFE&&f.index==0xBAADF00D&&f.written==0xDEADF00D,"late selected failure changed guest/output");}
    // The early triangle alone stages/commits genuine visible pixels. Failure
    // above is specifically a later selected level, not an empty first draw.
    Synthetic early;early.triangles();early.regs[0xAD]=0;require(early.triangle(3)&&early.written>0,"early visible staging premise failed");
    for(bool sprite:{false,true}){Synthetic f;f.triangles(16,16);if(sprite)f.rectangles(16);f.regs[0xA5]=0;f.regs[0xAD]=0x40010;
        require(sprite?f.sprites():f.triangle(),"unselected feedback rejected");}
    std::cout<<"PASS late required stride/span/feedback/cache/dimension atomicity + unused-feedback success\n";
}
void filter_matrix(){for(unsigned filter=4;filter<=7;++filter)for(unsigned mode=0;mode<4;++mode)for(unsigned bias:{0u,8u,24u,64u,232u}){
    Synthetic f;f.regs[0xC6]=0x100|filter;f.regs[0xC8]=(bias<<16)|mode;f.regs[0xD0]=std::bit_cast<unsigned>(2.0f)>>8;
    chili::GeMipTexture full,partial;std::string error;require(full.configure(f.rt.memory(),f.regs,nullptr,error)&&partial.configure(f.rt.memory(),f.regs,nullptr,error,0),error);
    for(float footprint:{0.0f,0.5f,1.0f,2.5f,8.0f,24.0f,64.0f}){chili::GeMipSelection a,b;require(full.select(footprint,1,a,error)&&partial.select(footprint,1,b,error),error);
        require(a.lower==b.lower&&a.upper==b.upper&&a.fraction16==b.fraction16&&a.linear==b.linear&&a.detail16==b.detail16,"metadata changes selection/filter/bias");
        const unsigned required=(1u<<a.lower)|(a.fraction16?(1u<<a.upper):0u);require(partial.configure(f.rt.memory(),f.regs,nullptr,error,required),error);unsigned x{},y{};
        require(full.sample(f.rt.memory(),0.3125f,0.6875f,a,x,error)&&partial.sample(f.rt.memory(),0.3125f,0.6875f,b,y,error)&&x==y,"partial sampled filtering differs");}}
    std::cout<<"PASS allminfilters/allLOD modes/signedbias finite-footprint prepared/full sample parity\n";
}
void original_default_parity(){for(unsigned kind=0;kind<10;++kind){Synthetic f;chili_original_mip::GeMipTexture original;chili::GeMipTexture candidate;std::string a,b;
    require(original.configure(f.rt.memory(),f.regs,nullptr,a)&&candidate.configure(f.rt.memory(),f.regs,nullptr,b),"default original/candidate seed failed");
    chili_original_mip::GeMipSelection os;chili::GeMipSelection cs;require(original.select(24,1,os,a)&&candidate.select(24,1,cs,b),"default selection seed failed");
    unsigned expected{},actual{};require(original.sample(f.rt.memory(),.375f,.625f,os,expected,a)&&candidate.sample(f.rt.memory(),.375f,.625f,cs,actual,b)&&actual==expected,"default seeded sample differs");
    if(kind==0)f.regs[0xA8]=0;if(kind==1)f.regs[0xAD]=0;if(kind==2)f.regs[0xB8]=0xA04;if(kind==3)f.regs[0xBD]=0xA04;
    if(kind==4){f.regs[0xA5]=0xFFFFF0;f.regs[0xAD]=0x90010;}if(kind==5)f.regs[0xC3]=6;if(kind==6)f.regs[0xC6]=2;if(kind==7)f.regs[0xC6]=0x207;
    if(kind>=8){f.regs[0xC3]=4;f.regs[0xC5]=0xFF03;for(unsigned n=0;n<6;++n)f.regs[0xA8+n]=0x80020;if(kind==9)f.regs[0xC2]|=0x100;}
    chili::GeClutCache cold;const auto *cache=kind==8?&cold:kind==9?&f.clut:nullptr;const MemoryState before(f.rt.memory());const auto cpu=f.rt.cpu();
    a.clear();b.clear();require(!original.configure(f.rt.memory(),f.regs,cache,a)&&!candidate.configure(f.rt.memory(),f.regs,cache,b)&&a==b&&!a.empty(),"default full failure error differs from original F60 kind="+std::to_string(kind));
    require(before.same(f.rt.memory())&&same_cpu(f.rt.cpu(),cpu)&&candidate.prepared_levels()==63,"default failed configure guest/prepared state changed");
    require(original.sample(f.rt.memory(),.375f,.625f,os,expected,a)&&candidate.sample(f.rt.memory(),.375f,.625f,cs,actual,b)&&expected==actual,"pre-existing failed-reconfigure preservation changed");}
    std::cout<<"PASS unchanged F60 default-full failure errors/memory + pre-existing sampler transaction preservation\n";
}
}
int main(int argc,char **argv){try{require(argc==2,"Usage: ge_required_mip_levels_test actual-prefix");mask_api();atomic_failures();filter_matrix();original_default_parity();actual_capture(actual(argv[1]));std::cout<<"PASS "<<checks<<" required mip checks; no actual PSP numeric parity claim\n";return 0;}catch(const std::exception &e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
