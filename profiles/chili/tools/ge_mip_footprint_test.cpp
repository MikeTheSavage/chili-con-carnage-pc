#include "ge_runtime.hpp"
#include "ge_triangle_raster_reference.hpp"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {
unsigned assertions{};
void require(bool value,const std::string &why){++assertions;if(!value)throw std::runtime_error(why);}
bool cpu_equal(const psprecomp::AllegrexContext &a,const psprecomp::AllegrexContext &b){return a.gpr==b.gpr&&a.pc==b.pc&&a.hi==b.hi&&a.lo==b.lo&&a.fcr31==b.fcr31&&a.vfpu_ctrl==b.vfpu_ctrl&&std::memcmp(a.fpr.data(),b.fpr.data(),sizeof(a.fpr))==0&&std::memcmp(a.vfpu.data(),b.vfpu.data(),sizeof(a.vfpu))==0;}
struct Fixture {
    psprecomp::Runtime rt;chili::GeClutCache clut;std::array<unsigned,256> regs{};
    std::array<float,12> world{1,0,0,0,1,0,0,0,1,0,0,0},view=world;
    std::array<float,16> projection{1,0,0,0,0,1,0,0,0,0,0,1,0,0,0,0};
    unsigned vertex_base=0x08804000,end=0xDEADBEEF,iend=0xBAADF00D,written=0xDEADF00D;std::string error;
    Fixture(){regs[0x12]=0x19D;regs[0xD2]=3;regs[0x9D]=512;regs[0x9E]=0x80000;regs[0x9F]=512;regs[0xD5]=regs[0x16]=7|(7<<10);regs[0xD7]=65535;regs[0x1C]=1;regs[0x58]=255;
        regf(0x42,1);regf(0x43,1);regf(0x44,100);regf(0x45,2048);regf(0x46,2048);regf(0x47,100);regs[0x4C]=regs[0x4D]=2048*16;regf(0x48,1);regf(0x49,1);
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)rt.memory().store32(0x04000000+(y*512+x)*4,0xAA112233);
        seed();}
    void seed(){auto &c=rt.cpu();for(unsigned n=1;n<c.gpr.size();++n)c.gpr[n]=0x90000000u+n;c.gpr[28]=0x08D9ABCD;c.gpr[29]=0x09FE8810;c.gpr[31]=0x08844364;c.pc=0x08D98A58;c.hi=0x1234567;c.lo=0x89ABCDEF;c.fcr31=0x13579BDF;for(unsigned n=0;n<c.fpr.size();++n)c.fpr[n]=std::bit_cast<float>(0x7FC10000u+n);for(unsigned n=0;n<c.vfpu.size();++n)c.vfpu[n]=std::bit_cast<float>(0xFFC10000u+n);}
    void regf(unsigned n,float value){regs[n]=std::bit_cast<unsigned>(value)>>8;}
    void vertex(unsigned index,float x,float y,float w,unsigned u=0,unsigned v=0){const unsigned a=vertex_base+index*20;rt.memory().store8(a,static_cast<std::uint8_t>(u));rt.memory().store8(a+1,static_cast<std::uint8_t>(v));rt.memory().store16(a+2,0xDEAD);rt.memory().store32(a+4,0xFFFFFFFF);rt.memory().store32(a+8,std::bit_cast<unsigned>(x));rt.memory().store32(a+12,std::bit_cast<unsigned>(y));rt.memory().store32(a+16,std::bit_cast<unsigned>(w));}
    void mips(){regs[0x1E]=1;regs[0xC2]=3<<16;regs[0xC3]=3;regs[0xC6]=0x107;regs[0xC7]=0x101;regs[0xC9]=0x103;
        const unsigned colors[]={0xFF0000FF,0xFF00FF00,0xFFFF0000,0xFFFFFFFF};for(unsigned n=0;n<4;++n){const unsigned size=8>>n,stride=std::max(size,4u),address=0x08810000+n*0x1000;regs[0xA0+n]=address&0xFFFFFF;regs[0xA8+n]=0x080000|stride;regs[0xB8+n]=(3-n)|((3-n)<<8);for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x)rt.memory().store32(address+(y*stride+x)*4,colors[n]);}}
    bool draw(unsigned count=3,unsigned primitive=3,bool baseline=false){if(baseline)return chili_mip_baseline::render_ge_triangle_raster(rt.memory(),regs,world,view,projection,vertex_base,(primitive<<16)|count,end,error,clut,&written,0,&iend);return chili::render_ge_triangle_raster(rt.memory(),regs,world,view,projection,vertex_base,(primitive<<16)|count,end,error,clut,&written,0,&iend);}
    void outputs_reset(){end=0xDEADBEEF;iend=0xBAADF00D;written=0xDEADF00D;error.clear();}
};
void unchanged_failure(Fixture &f,unsigned count,const char *why,bool baseline=false){const auto vram=f.rt.memory().vram_bytes(),ram=f.rt.memory().bytes();const auto cpu=f.rt.cpu();require(!f.draw(count,3,baseline),std::string("unsupported input accepted: ")+why);require(f.rt.memory().vram_bytes()==vram&&f.rt.memory().bytes()==ram&&cpu_equal(f.rt.cpu(),cpu)&&f.end==0xDEADBEEF&&f.iend==0xBAADF00D&&f.written==0xDEADF00D,std::string("failure changed VRAM/source/CPU/cursors: ")+why);}
void finite_helpers(){
    {Fixture f;f.mips();f.vertex(0,0,0,1);f.vertex(1,1,0,1,128);f.vertex(2,0,1,1,0,128);const auto before=f.rt.memory().vram_bytes();require(f.draw(3,3,true),"ordinary positive helper baseline failed");const auto expected=f.rt.memory().vram_bytes();const unsigned committed=f.written;require(committed==1&&f.rt.memory().load32(0x04000000)==0xAAFFFFFF,"positive helper exact mip3 pixel incorrect");f.rt.memory().copy_in(0x04000000,before);f.outputs_reset();require(f.draw()&&f.rt.memory().vram_bytes()==expected&&f.written==committed&&f.end==f.vertex_base+60&&f.iend==0,"ordinary positive helper baseline/candidate full VRAM/cursor parity failed");}
    for(unsigned variant=0;variant<4;++variant){Fixture f;f.mips();const bool y_axis=variant&2,negative_base=variant&1;
        if(negative_base){if(y_axis){f.vertex(0,0,1,1);f.vertex(1,0,.25f,.125f,0,128);f.vertex(2,1,1,1);}else{f.vertex(0,1,0,1);f.vertex(1,.25f,0,.125f,128);f.vertex(2,1,1,1);}}
        else if(y_axis){f.vertex(0,0,0,.125f);f.vertex(1,0,1,1,0,128);f.vertex(2,.125f,0,.125f);}else{f.vertex(0,0,0,.125f);f.vertex(1,1,0,1,128);f.vertex(2,0,.125f,.125f);}
        // Exact subpixel vertices yield one covered sample. Independent rational
        // arithmetic gives footprint(negative base)=8*|56/65-72/47|,
        // or footprint(negative +axis)=8*|7/79+23/33|. Exponent/top4
        // mantissa LOD gives37 or41, retaining trilinear fractions5 or9.
        const unsigned expected=negative_base?0xAAFF4F4F:0xAAFF8F8F;
        unchanged_failure(f,3,"finite negative helper",true);f.outputs_reset();const auto before=f.rt.memory().vram_bytes(),ram=f.rt.memory().bytes();const auto cpu=f.rt.cpu();
        require(f.draw(),"finite signed helper draw failed: "+f.error);require(f.written==1&&f.end==f.vertex_base+60&&f.iend==0,"thin helper source cursor/coverage mismatch");
        const unsigned pixel_x=negative_base&&!y_axis?1:0,pixel_y=negative_base&&y_axis?1:0,offset=(pixel_y*512+pixel_x)*4;auto wanted=before;
        for(unsigned ch=0;ch<4;++ch)wanted[offset+ch]=static_cast<std::uint8_t>(expected>>(ch*8));
        require(f.rt.memory().vram_bytes()==wanted,"signed helper lost exact trilinear levels/fraction/full VRAM");require(f.rt.memory().bytes()==ram&&cpu_equal(f.rt.cpu(),cpu),"signed helper changed source or CPU");
    }
    std::cout<<"PASS Finite negative base/+X/+Y helper denominators retain independent rational AUTO LOD and exact trilinear pixels\n";
}
void singular_strict(){
    for(unsigned later=0;later<2;++later){Fixture f;f.mips();const unsigned first=later?3:0;if(later){f.vertex(0,0,0,1);f.vertex(1,1,0,1,128);f.vertex(2,0,1,1,0,128);}
        // Exact q helper pole: vertices screenX7/16 and15/16, reciprocal
        // W2 and1. At quad +X sample23/16 q=2+2*(1-2)=0.
        f.vertex(first,7.f/32,0,.5f);f.vertex(first+1,15.f/16,0,1,128);f.vertex(first+2,7.f/32,.5f,.5f);
        unchanged_failure(f,later?6:3,"zero helper pole");require(f.error=="native triangle mip interpolation footprint unsupported","zero helper pole failure classification changed");}
    {Fixture f;f.mips();f.vertex(0,0,0,.125f);f.vertex(1,1,0,1,128);f.vertex(2,0,.125f,.125f);f.regf(0x48,std::numeric_limits<float>::max());unchanged_failure(f,3,"finite helper UV produces overflowing footprint");require(f.error=="invalid mip derivative footprint","overflowing footprint was not rejected by unchanged sampler");}
    {Fixture f;f.mips();f.vertex(0,1,0,1);f.vertex(1,.25f,0,.125f,128);f.vertex(2,1,1,1);f.regf(0x48,std::numeric_limits<float>::max());unchanged_failure(f,3,"nonfinite extrapolated UV");require(f.error=="native triangle mip interpolation footprint unsupported","nonfinite helper UV was not rejected before sampler");}
    std::cout<<"PASS Exact zero helper poles, late pole, nonfinite extrapolated UV and overflowing footprint retain whole-draw atomicity\n";
}
std::vector<std::uint8_t> read(const std::filesystem::path &p,std::size_t expected){std::ifstream f(p,std::ios::binary);require(bool(f),"capture absent: "+p.string());f.seekg(0,std::ios::end);require(f.tellg()==static_cast<std::streamoff>(expected),"capture size changed");f.seekg(0);std::vector<std::uint8_t> b(expected);f.read(reinterpret_cast<char*>(b.data()),static_cast<std::streamsize>(b.size()));require(bool(f),"capture read failed");return b;}
std::vector<std::uint8_t> baseline_composite(Fixture &f,const std::vector<std::uint8_t> &before){
    const auto original=f.regs;f.rt.memory().copy_in(0x04000000,before);
    // Independent old implementation succeeds in every disjoint region other
    // than the one proved helper witness. Keep original per-pixel primitive
    // order; no textures alias framebuffer/depth, so regions cannot feed back.
    const std::array<std::array<unsigned,4>,4> rectangles{{{0,0,511,189},{0,191,511,319},{0,190,135,190},{137,190,511,190}}};
    for(const auto &r:rectangles){f.regs[0xD4]=r[0]|(r[1]<<10);f.regs[0xD5]=r[2]|(r[3]<<10);f.outputs_reset();require(f.draw(86,4,true),"unchanged baseline disjoint region failed: "+f.error);require(f.end==f.vertex_base+1204&&f.iend==0,"baseline partition cursor failed");}
    auto expected=f.rt.memory().vram_bytes();f.regs=original;f.outputs_reset();f.rt.memory().copy_in(0x04000000,before);return expected;
}
std::array<unsigned,2> independent_witness(Fixture &source,const std::vector<std::uint8_t> &before){
    Fixture reference;reference.regs=source.regs;reference.world=source.world;reference.view=source.view;reference.projection=source.projection;reference.clut=source.clut;reference.regs[0xD4]=reference.regs[0xD5]=136|(190<<10);reference.vertex_base=0x08870000;reference.rt.memory().copy_in(0x04000000,before);
    const unsigned color_address=0x04147220,depth_address=0x040B7910;unsigned failed{},baseline_commits{},winning_strip{};
    for(unsigned last=2;last<86;++last){const std::array<unsigned,3> order{(last&1)?last-1:last-2,(last&1)?last-2:last-1,last};for(unsigned n=0;n<3;++n){std::array<std::uint8_t,14> record{};source.rt.memory().copy_out(source.vertex_base+order[n]*14,record);reference.rt.memory().copy_in(reference.vertex_base+n*14,record);}
        reference.outputs_reset();const auto saved=reference.rt.memory().vram_bytes();const bool success=reference.draw(3,3,true);
        if(success){baseline_commits+=reference.written;if(reference.written)winning_strip=last;continue;}
        require(last==50&&reference.error=="native triangle mip interpolation footprint unsupported"&&reference.rt.memory().vram_bytes()==saved,"unexpected baseline witness triangle failure");++failed;
        // The primary independent mathematical witness below supplies only the
        // failing triangle's post-fog color and projected depth. All other
        // triangles use unchanged baseline rendering at this exact pixel.
        require((source.regs[0xDE]&7)==7&&(source.regs[0xE7]&1)==0&&(source.regs[0xDF]&0xFFFFFF)==0x32&&(source.regs[0xE8]&0xFFFFFF)==0,"witness commit state changed");
        const unsigned old_depth=reference.rt.memory().load16(depth_address),old_color=reference.rt.memory().load32(color_address);if(430>=old_depth){reference.rt.memory().store16(depth_address,430);reference.rt.memory().store32(color_address,(old_color&0xFF000000)|0x006E91B0);}
    }
    require(failed==1&&baseline_commits==1&&winning_strip==52,"independent witness coverage/order changed");
    const std::array<unsigned,2> result{reference.rt.memory().load32(color_address),reference.rt.memory().load16(depth_address)};
    std::cout<<"PASS Independent witness old-baseline other-triangle commits="<<baseline_commits<<" at strip="<<winning_strip<<", one rational helper triangle inserted before exact depth/legacy blend; color="<<std::hex<<result[0]<<" depth="<<result[1]<<std::dec<<'\n';return result;
}
void capture(const std::filesystem::path &prefix){Fixture f;std::ifstream metadata(prefix.string()+".txt");require(bool(metadata),"capture metadata absent");std::string line;unsigned data{};
    while(std::getline(metadata,line)){std::istringstream s(line);std::string kind;s>>kind;if(kind=="reg"){unsigned n{},v{};s>>std::hex>>n>>v;require(n<256&&bool(s),"capture register malformed");f.regs[n]=v;}else if(kind=="world"||kind=="view"||kind=="projection"){auto parse=[&](auto &a){for(auto &v:a){s>>v;require(bool(s)&&std::isfinite(v),"capture matrix malformed");}};if(kind=="world")parse(f.world);else if(kind=="view")parse(f.view);else parse(f.projection);}else if(kind.starts_with("primitive=")){data=static_cast<unsigned>(std::stoul(kind.substr(10),nullptr,16));std::string address;s>>address;require(address.starts_with("vertices="),"capture address absent");f.vertex_base=static_cast<unsigned>(std::stoul(address.substr(9),nullptr,16));}}
    require(data==0x40056&&f.vertex_base==0x0850B1FC&&(f.regs[0x12]&0xFFFFFF)==0x122,"capture draw ABI changed");f.rt.memory().copy_in(f.vertex_base,read(prefix.string()+"_vertices.bin",1204));f.rt.memory().copy_in(0x04000000,read(prefix.string()+"_vram.bin",0x200000));
    const unsigned tex=(f.regs[0xA0]&0xFFFFF0u)|((f.regs[0xA8]<<8)&0x0F000000u),pal=(f.regs[0xB0]&0xFFFFF0u)|((f.regs[0xB1]<<8)&0x0F000000u);
    f.rt.memory().copy_in(tex,read(prefix.string()+"_texture.bin",4096));f.rt.memory().copy_in(pal,read(prefix.string()+"_palette_source.bin",64));require(f.clut.load(f.rt.memory(),f.regs[0xB0],f.regs[0xB1],f.regs[0xC4],f.error),"capture CLUT load failed");
    const auto before=f.rt.memory().vram_bytes(),ram=f.rt.memory().bytes();const auto cpu=f.rt.cpu();require(!f.draw(86,4,true)&&f.error=="native triangle mip interpolation footprint unsupported","baseline does not reproduce exact failure");require(f.rt.memory().vram_bytes()==before&&f.rt.memory().bytes()==ram&&cpu_equal(f.rt.cpu(),cpu)&&f.end==0xDEADBEEF&&f.iend==0xBAADF00D&&f.written==0xDEADF00D,"baseline failure changed inputs/outputs");f.outputs_reset();
    auto independent=baseline_composite(f,before);
    require(f.draw(86,4),"captured signed helper draw failed: "+f.error);require(f.end==f.vertex_base+1204&&f.iend==0,"capture both source cursors changed");require(f.rt.memory().bytes()==ram&&cpu_equal(f.rt.cpu(),cpu),"capture changed full source/CPU");const auto expected=f.rt.memory().vram_bytes();const unsigned committed=f.written;unsigned changed{};
    const unsigned witness_color=(0xE8000+(190*512+136)*4),witness_depth=(0x88000+(190*512+136)*2);
    // Independently derived failing triangle48/49/50 at the captured witness,
    // mip4 sampleFF7692B0, affine primaryFFECFCFF -> MODULATE FF6D90B0,
    // fog byte252 -> FF6E91B0, alpha>=2, projected depth430 >= old384,
    // alpha255 legacy blend32 and zero masks preserve FF destination alpha.
    // Other triangles at the same pixel are retained in original strip order
    // by independent_witness's unchanged old-baseline rendering. These are
    // binary32/native arithmetic expectations, not new PSP goldens.
    auto little=[&](unsigned address,unsigned size){unsigned value=0;for(unsigned ch=0;ch<size;++ch)value|=unsigned(independent[address+ch])<<(ch*8);return value;};
    require(little(witness_color,4)==0xFF3B618A&&little(witness_depth,2)==384,"old baseline partitions changed independent witness or capture changed");
    const auto witness=independent_witness(f,before);for(unsigned ch=0;ch<4;++ch)independent[witness_color+ch]=static_cast<std::uint8_t>(witness[0]>>(ch*8));for(unsigned ch=0;ch<2;++ch)independent[witness_depth+ch]=static_cast<std::uint8_t>(witness[1]>>(ch*8));
    for(std::size_t n=0;n<expected.size();++n)require(expected[n]==independent[n],"candidate differs from unchanged baseline composite plus independent witness at byte "+std::to_string(n)+" actual="+std::to_string(expected[n])+" independent="+std::to_string(independent[n])+" color="+std::to_string(f.rt.memory().load32(0x04000000+witness_color))+" depth="+std::to_string(f.rt.memory().load16(0x04000000+witness_depth)));
    const unsigned fb=f.regs[0x9C]&0x1FFFF0,db=f.regs[0x9E]&0x1FFFF0,stride=f.regs[0x9D]&0x7FC,ds=f.regs[0x9F]&0x7FC,sx0=f.regs[0xD4]&1023,sy0=(f.regs[0xD4]>>10)&1023,sx1=std::min(f.regs[0xD5]&1023,f.regs[0x16]&1023),sy1=std::min((f.regs[0xD5]>>10)&1023,(f.regs[0x16]>>10)&1023);
    for(std::size_t n=0;n<before.size();++n)if(before[n]!=expected[n]){auto inside=[&](unsigned base,unsigned pitch,unsigned size){if(n<base)return false;const auto pixel=(n-base)/size;return pixel%pitch>=sx0&&pixel%pitch<=sx1&&pixel/pitch>=sy0&&pixel/pitch<=sy1;};require(inside(fb,stride,4)||inside(db,ds,2),"capture write escaped color/depth/scissor");++changed;}
    require(changed>0&&committed>0,"capture produced no committed color/depth output");
    f.rt.memory().copy_in(0x04000000,before);f.outputs_reset();require(f.draw(86,4)&&f.rt.memory().vram_bytes()==expected&&f.written==committed,"capture replay differs");
    // Genuine native GE HLE consumes actual PRIM and completion without any
    // helper bypass, import-result patch or guest scheduler time advance.
    chili::ThreadRuntime scheduler(f.rt);
    for(bool enabled:{false,true}){f.rt.memory().copy_in(0x04000000,before);std::ostringstream output;chili::PerformanceTelemetry telemetry(output);chili::GeRuntime gpu(f.rt,&scheduler,0,nullptr,enabled?&telemetry:nullptr);gpu.commands=f.regs;gpu.clut=f.clut;gpu.world=f.world;gpu.view=f.view;gpu.projection=f.projection;gpu.vertex_address=f.vertex_base;
        const unsigned list=0x09F00000;f.rt.memory().store32(list,0x04040056);f.rt.memory().store32(list+4,0x0F000000);f.rt.memory().store32(list+8,0x0C000000);f.rt.cpu().gpr[4]=list;f.rt.cpu().gpr[5]=0;f.rt.cpu().gpr[6]=0xFFFFFFFF;f.rt.cpu().gpr[7]=0;auto hle_cpu=f.rt.cpu();hle_cpu.gpr[2]=1;const auto hle_ram=f.rt.memory().bytes();const auto uid=psprecomp::runtime_thread_uid();const auto generation=psprecomp::runtime_thread_switch_generation();const auto time=scheduler.now_us();f.rt.invoke_import("sceGe_user",0xAB49E76Au,f.rt.cpu());
        auto commands=f.regs;commands[4]=0x04040056;commands[0x0F]=0x0F000000;require(!f.rt.stopped()&&gpu.completed.contains(1)&&gpu.commands==commands&&gpu.vertex_address==f.vertex_base+1204&&gpu.index_address==0&&gpu.world==f.world&&gpu.view==f.view&&gpu.projection==f.projection,"capture genuine GE HLE full state/completion/cursors failed: "+f.rt.stop_reason());require(f.rt.memory().vram_bytes()==expected&&f.rt.memory().bytes()==hle_ram&&cpu_equal(f.rt.cpu(),hle_cpu)&&scheduler.now_us()==time&&psprecomp::runtime_thread_uid()==uid&&psprecomp::runtime_thread_switch_generation()==generation,"capture native HLE differs or changes source/CPU/UID/time");
        chili::PerformanceTelemetry::Sample sample;require(telemetry.sample(chili::PerformanceTelemetry::Clock::now(),time,sample,true),"observer sample failed");if(enabled){require(sample.total[chili::PerformanceTelemetry::DrawAttempts]==1&&sample.total[chili::PerformanceTelemetry::DrawSuccesses]==1&&sample.total[chili::PerformanceTelemetry::DrawFailures]==0&&sample.total[chili::PerformanceTelemetry::TriangleVertices]==86&&sample.total[chili::PerformanceTelemetry::TriangleCommittedFragments]==committed&&sample.total[chili::PerformanceTelemetry::GeListsCompleted]==1&&sample.total[chili::PerformanceTelemetry::GeWordsRead]==3,"observer provenance changed");}else for(auto count:sample.total)require(count==0,"default null renderer updated observer");}
    std::cout<<"PASS Actual captured0122 strip86: committed="<<committed<<", changed color/depth bytes="<<changed<<"; exact repeated/native HLE full2MiB, source/NaN CPU/cursors/UID/time preserved\n";
}
}
int main(int argc,char **argv){try{require(argc==2,"Usage: ge_mip_footprint_test actualcaptureprefix");finite_helpers();singular_strict();capture(argv[1]);std::cout<<"PASS "<<assertions<<" signed mip-helper assertions; no new PSP hardware/float24/pole/FPS/gameplay proof\n";return 0;}catch(const std::exception &e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
