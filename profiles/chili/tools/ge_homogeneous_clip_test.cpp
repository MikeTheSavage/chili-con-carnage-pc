#include "ge_triangle_raster.hpp"
#include "psprecomp/runtime.hpp"
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {
unsigned assertions{};
void require(bool value,const std::string &why){++assertions;if(!value)throw std::runtime_error(why);}
struct Fixture {
    psprecomp::Runtime rt;chili::GeClutCache clut;std::array<unsigned,256> regs{};
    std::array<float,12> world{1,0,0,0,1,0,0,0,1,0,0,0},view=world;
    // W=model Z, near plane Zclip+W=2*model Z-2, hence W=1.
    std::array<float,16> projection{1,0,0,0,0,1,0,0,0,0,1,1,0,0,-2,0};
    unsigned vertex_base=0x08804000,end=0xDEADBEEF,iend=0xBAADF00D,written=0xDEADF00D;std::string error;
    Fixture(){regs[0x12]=0x19D;regs[0xD2]=3;regs[0x9D]=512;regs[0x9E]=0x80000;regs[0x9F]=512;regs[0xD5]=regs[0x16]=7|(7<<10);regs[0xD7]=65535;regs[0x1C]=1;regs[0x58]=255;
        regf(0x42,2);regf(0x43,2);regf(0x44,100);regf(0x45,2048);regf(0x46,2048);regf(0x47,100);regs[0x4C]=regs[0x4D]=2046*16;regf(0x48,1);regf(0x49,1);
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){rt.memory().store32(0x04000000+(y*512+x)*4,0xAA112233);rt.memory().store16(0x04080000+(y*512+x)*2,1000);}
        auto &c=rt.cpu();for(unsigned n=1;n<c.gpr.size();++n)c.gpr[n]=0x90000000u+n;c.gpr[28]=0x08D9ABCD;c.gpr[29]=0x09FE8810;c.gpr[31]=0x08844364;c.pc=0x08D98A58;c.hi=0x01234567;c.lo=0x89ABCDEF;c.fcr31=0x13579BDF;
        for(unsigned n=0;n<c.fpr.size();++n)c.fpr[n]=std::bit_cast<float>(0x7FC10000u+n);for(unsigned n=0;n<c.vfpu.size();++n)c.vfpu[n]=std::bit_cast<float>(0xFFC10000u+n);for(unsigned n=0;n<c.vfpu_ctrl.size();++n)c.vfpu_ctrl[n]=0xE4000000u+n;
    }
    void regf(unsigned index,float value){regs[index]=std::bit_cast<unsigned>(value)>>8;}
    void vertex(unsigned index,float x,float y,float z,unsigned color=0xFF0000FF,unsigned u=0,unsigned v=0){const auto address=vertex_base+index*20;rt.memory().store8(address,static_cast<std::uint8_t>(u));rt.memory().store8(address+1,static_cast<std::uint8_t>(v));rt.memory().store16(address+2,0x1234);rt.memory().store32(address+4,color);rt.memory().store32(address+8,std::bit_cast<unsigned>(x));rt.memory().store32(address+12,std::bit_cast<unsigned>(y));rt.memory().store32(address+16,std::bit_cast<unsigned>(z));}
    bool draw(unsigned count=3,unsigned primitive=3){return chili::render_ge_triangle_raster(rt.memory(),regs,world,view,projection,vertex_base,(primitive<<16)|count,end,error,clut,&written,0,&iend);}
    void texture(){regs[0x1E]=1;regs[0xC3]=3;regs[0xC9]=0x103;regs[0xC7]=0x101;regs[0xA0]=0x810000;regs[0xA8]=0x080004;regs[0xB8]=2;
        const unsigned colors[]={0xFF0000FF,0xFF00FF00,0xFFFF0000,0xFFFFFFFF};for(unsigned n=0;n<4;++n)rt.memory().store32(0x08810000+n*4,colors[n]);}
    void fog(){regs[0x1F]=1;regs[0xCF]=0xAA7733;regf(0xCD,0);regf(0xCE,0.25f);}
};
bool cpu_equal(const psprecomp::AllegrexContext &a,const psprecomp::AllegrexContext &b){return a.gpr==b.gpr&&a.pc==b.pc&&a.hi==b.hi&&a.lo==b.lo&&a.fcr31==b.fcr31&&a.vfpu_ctrl==b.vfpu_ctrl&&std::memcmp(a.fpr.data(),b.fpr.data(),a.fpr.size()*sizeof(float))==0&&std::memcmp(a.vfpu.data(),b.vfpu.data(),a.vfpu.size()*sizeof(float))==0;}
void exact_preclipped(){for(const auto behind:{-2.f,0.f})for(unsigned shade:{0u,1u})for(unsigned pipeline=0;pipeline<4;++pipeline){
    Fixture clipped,reference;const auto front=2.f-behind;clipped.regs[0x50]=reference.regs[0x50]=shade;
    if(pipeline&1){clipped.texture();reference.texture();}if(pipeline&2){clipped.fog();reference.fog();}clipped.regs[0x23]=reference.regs[0x23]=1;clipped.regs[0xDE]=reference.regs[0xDE]=4;
    const unsigned ca=0x80000000,cb=0x80000080,cc=0x80008000;
    clipped.vertex(0,-2,-2,behind,ca,0,0);clipped.vertex(1,2,-2,front,cb,128,0);clipped.vertex(2,-2,2,front,cc,0,128);
    // Exact source-edge t=1/2 intersections, independent of the renderer:
    // I20=(-2,0,1), I01=(0,-2,1), B=(2,-2,front), C=(-2,2,front).
    reference.vertex(0,-2,0,1,shade?0x80004000:cc,0,64);reference.vertex(1,0,-2,1,shade?0x80000040:cc,64,0);
    reference.vertex(2,2,-2,front,shade?cb:cc,128,0);reference.vertex(3,-2,2,front,cc,0,128);
    const auto ram=clipped.rt.memory().bytes();const auto cpu=clipped.rt.cpu();const auto identity=psprecomp::runtime_thread_uid();const auto generation=psprecomp::runtime_thread_switch_generation();
    require(reference.draw(4,5),"analytic preclipped reference failed: "+reference.error);require(clipped.draw(),"mixed/zero W draw failed: "+clipped.error);
    require(clipped.written>0&&clipped.written==reference.written,"clipping changed visible geometric coverage/count");require(clipped.rt.memory().vram_bytes()==reference.rt.memory().vram_bytes(),"clipped UV/fog/color/depth differs from independent exact preclipped polygon");
    require(clipped.end==clipped.vertex_base+60&&clipped.iend==0,"clipped source cursors incorrect");require(clipped.rt.memory().bytes()==ram&&cpu_equal(clipped.rt.cpu(),cpu)&&psprecomp::runtime_thread_uid()==identity&&psprecomp::runtime_thread_switch_generation()==generation,"native clipping touched source RAM or bitwise CPU/identity");
}
std::cout<<"PASS Mixed W and zero-W behind-near clipping versus exact dyadic preclipped geometry; flat/Gouraud, UV perspective, fog, depth, source cursors/full VRAM/NaN CPU\n";}
void retained_strict(){for(unsigned kind=0;kind<3;++kind){Fixture f;f.vertex(0,-1,-1,2);f.vertex(1,1,-1,2);f.vertex(2,-1,1,2);if(kind==0){f.projection[10]=f.projection[14]=0;f.vertex(2,-1,1,0);}else if(kind==1){f.projection[10]=f.projection[14]=0;f.vertex(2,-1,1,-1);}else f.projection[0]=std::numeric_limits<float>::quiet_NaN();const auto before=f.rt.memory().vram_bytes();require(!f.draw(),"unverified retained W/nonfinite case silently succeeded");require(f.rt.memory().vram_bytes()==before&&f.end==0xDEADBEEF&&f.iend==0xBAADF00D&&f.written==0xDEADF00D,"strict retained-W case changed output");}
    // A later invalid primitive cannot leave the preceding mixed-W triangle.
    Fixture f;f.vertex(0,-2,-2,-2);f.vertex(1,2,-2,4);f.vertex(2,-2,2,4);f.vertex(3,-1,-1,2);f.vertex(4,1,-1,2);f.vertex(5,-1,1,std::numeric_limits<float>::infinity());const auto before=f.rt.memory().vram_bytes();require(!f.draw(6)&&f.rt.memory().vram_bytes()==before&&f.end==0xDEADBEEF&&f.written==0xDEADF00D,"later invalid source left partial clipped draw/cursors");
    std::cout<<"PASS Retained/on-plane W<=0 and nonfinite transforms remain explicit, whole-draw atomic failure\n";
}
std::vector<std::uint8_t> read(const std::filesystem::path &path,std::size_t expected){std::ifstream file(path,std::ios::binary);require(bool(file),"capture file absent");file.seekg(0,std::ios::end);require(file.tellg()==static_cast<std::streamoff>(expected),"capture byte length mismatch");file.seekg(0);std::vector<std::uint8_t> bytes(expected);file.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));require(bool(file),"capture read failed");return bytes;}
void captured(const std::filesystem::path &prefix){Fixture f;std::ifstream metadata(prefix.string()+".txt");require(bool(metadata),"capture metadata absent");std::string line;unsigned data{},vertex_address{};
    while(std::getline(metadata,line)){std::istringstream words(line);std::string kind;words>>kind;if(kind=="reg"){unsigned index{},value{};words>>std::hex>>index>>value;require(index<256&&bool(words),"invalid captured register");f.regs[index]=value;}
        else if(kind=="world"||kind=="view"||kind=="projection"){auto parse=[&](auto &values){for(auto &value:values){words>>value;require(bool(words)&&std::isfinite(value),"invalid captured matrix");}};if(kind=="world")parse(f.world);else if(kind=="view")parse(f.view);else parse(f.projection);}
        else if(kind.starts_with("primitive=")){data=static_cast<unsigned>(std::stoul(kind.substr(10),nullptr,16));std::string address;words>>address;require(address.starts_with("vertices="),"captured vertex address absent");vertex_address=static_cast<unsigned>(std::stoul(address.substr(9),nullptr,16));}}
    require(data==0x400A0&&vertex_address==0x084E9C04&&(f.regs[0x12]&0xFFFFFF)==0xB5,"actual captured geometry ABI changed");f.vertex_base=vertex_address;
    f.rt.memory().copy_in(vertex_address,read(prefix.string()+"_vertices.bin",1600));f.rt.memory().copy_in(0x04000000,read(prefix.string()+"_vram.bin",psprecomp::GuestMemory::kVramSize));
    const auto tex=(f.regs[0xA0]&0xFFFFF0u)|((f.regs[0xA8]<<8)&0x0F000000u),pal=(f.regs[0xB0]&0xFFFFF0u)|((f.regs[0xB1]<<8)&0x0F000000u);
    f.rt.memory().copy_in(tex,read(prefix.string()+"_texture.bin",128));f.rt.memory().copy_in(pal,read(prefix.string()+"_palette_source.bin",1024));require(f.clut.load(f.rt.memory(),f.regs[0xB0],f.regs[0xB1],f.regs[0xC4],f.error),"captured palette load failed");
    const auto before=f.rt.memory().vram_bytes(),ram=f.rt.memory().bytes();const auto cpu=f.rt.cpu();require(f.draw(data&65535,(data>>16)&7),"actual captured mixed-W draw failed: "+f.error);require(f.end==vertex_address+1600&&f.iend==0&&f.written>0,"actual captured draw cursor/visible pixels missing");
    const unsigned fb=f.regs[0x9C]&0x1FFFF0,stride=f.regs[0x9D]&0x7FC;const unsigned sx0=f.regs[0xD4]&1023,sy0=(f.regs[0xD4]>>10)&1023,sx1=std::min(f.regs[0xD5]&1023,f.regs[0x16]&1023),sy1=std::min((f.regs[0xD5]>>10)&1023,(f.regs[0x16]>>10)&1023);unsigned changed{};
    const auto &after=f.rt.memory().vram_bytes();for(std::size_t offset=0;offset<before.size();++offset)if(before[offset]!=after[offset]){require(offset>=fb,"captured draw wrote before framebuffer");const auto pixel=(offset-fb)/4,y=pixel/stride,x=pixel%stride;require(x>=sx0&&x<=sx1&&y>=sy0&&y<=sy1,"captured draw escaped bounded framebuffer scissor");++changed;}
    require(changed>0&&f.rt.memory().bytes()==ram&&cpu_equal(f.rt.cpu(),cpu),"captured draw fabricated visible output or changed source/context");std::cout<<"PASS Actual captured B5 strip160/DXT1: committed pixels="<<f.written<<", changed framebuffer bytes="<<changed<<"; full2MiB bounds and source/CPU preserved\n";
}
}
int main(int argc,char **argv){try{require(argc<=2,"Usage: ge_homogeneous_clip_test [captured failure prefix]");exact_preclipped();retained_strict();if(argc==2)captured(argv[1]);std::cout<<"PASS "<<assertions<<" native homogeneous-clip assertions; PSP intermediate precision remains unverified\n";return 0;}catch(const std::exception &failure){std::cerr<<"FAIL "<<failure.what()<<'\n';return 1;}}
