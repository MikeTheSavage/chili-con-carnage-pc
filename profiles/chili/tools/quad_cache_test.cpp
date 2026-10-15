#include "ge_triangle_raster.hpp"
#include "psprecomp/runtime.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace quad_cache_fixture {
struct Trace {
 unsigned helpers{};std::vector<std::array<unsigned,2>> attempts;
 std::vector<std::array<unsigned,5>> covered;
 std::vector<std::array<unsigned,9>> selections;
};
inline thread_local Trace *sink{};
inline void attempt(int x,int y,bool helper){if(!sink)return;if(helper)++sink->helpers;else sink->attempts.push_back({unsigned(x),unsigned(y)});}
inline void uv(int x,int y,bool helper,float u,float v,float w){if(sink&&!helper)sink->covered.push_back({unsigned(x),unsigned(y),std::bit_cast<unsigned>(u),std::bit_cast<unsigned>(v),std::bit_cast<unsigned>(w)});}
inline void selected(int x,int y,float footprint,const chili::GeMipSelection &s){if(sink)sink->selections.push_back({unsigned(x),unsigned(y),std::bit_cast<unsigned>(footprint),s.lower,s.upper,s.fraction16,unsigned(s.linear),unsigned(s.detail16),0});}
}
#include "quad_cache_trace_reference.hpp"
#include "quad_cache_trace_candidate.hpp"

namespace {
unsigned checks{};
void require(bool ok,const std::string &why){++checks;if(!ok)throw std::runtime_error(why);}
bool same_cpu(const psprecomp::AllegrexContext &a,const psprecomp::AllegrexContext &b){return a.gpr==b.gpr&&a.pc==b.pc&&a.hi==b.hi&&a.lo==b.lo&&a.fcr31==b.fcr31&&a.vfpu_ctrl==b.vfpu_ctrl&&std::memcmp(a.fpr.data(),b.fpr.data(),sizeof(a.fpr))==0&&std::memcmp(a.vfpu.data(),b.vfpu.data(),sizeof(a.vfpu))==0;}
struct Fixture {
 psprecomp::Runtime rt;chili::GeClutCache clut;std::array<unsigned,256> regs{};
 std::array<float,12> world{1,0,0,0,1,0,0,0,1,0,0,0},view=world;
 std::array<float,16> projection{1,0,0,0,0,1,0,0,0,0,0,1,0,0,0,0};
 unsigned base=0x08804000,end=0xDEADBEEF,index=0xBAADF00D,written=0xDEADF00D;std::string error;
 Fixture(){regs[0x12]=0x19D;regs[0xD2]=3;regs[0x9D]=512;regs[0x9E]=0x88000;regs[0x9F]=512;regs[0xD5]=regs[0x16]=7|(7<<10);regs[0xD7]=65535;regs[0x1C]=1;regs[0x58]=255;regs[0x1E]=1;regs[0xC2]=0x30001;regs[0xC3]=4;regs[0xC6]=0x107;regs[0xC9]=0x103;regs[0xC5]=0xFF03;
  number(0x42,1);number(0x43,1);number(0x44,100);number(0x45,2048);number(0x46,2048);number(0x47,100);regs[0x4C]=regs[0x4D]=2048*16;number(0x48,1);number(0x49,1);
  for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)rt.memory().store32(0x04000000+(y*512+x)*4,0xAA112233);
  for(unsigned n=0;n<16;++n)rt.memory().store32(0x08820000+n*4,0xFF000000u|(n*17u)|((255-n*17u)<<8)|((n^10u)*17u<<16));
  require(clut.load(rt.memory(),0x820000,0x80000,2,error),"palette load failed");
  for(unsigned n=0;n<4;++n){const unsigned address=0x08810000+n*0x1000;regs[0xA0+n]=address&0xFFFFFF;regs[0xA8+n]=0x080000|32;regs[0xB8+n]=(3-n)|((3-n)<<8);std::array<std::uint8_t,128> bytes{};for(unsigned i=0;i<bytes.size();++i)bytes[i]=std::uint8_t(i*13+n*0x11);rt.memory().copy_in(address,bytes);}
  auto &cpu=rt.cpu();cpu.pc=0x08D98ED0;cpu.gpr[29]=0x09FE8810;for(unsigned n=0;n<cpu.fpr.size();++n)cpu.fpr[n]=std::bit_cast<float>(0x7FC10000+n);for(unsigned n=0;n<cpu.vfpu.size();++n)cpu.vfpu[n]=std::bit_cast<float>(0xFFC10000+n);
 }
 void number(unsigned n,float value){regs[n]=std::bit_cast<unsigned>(value)>>8;}
 void vertex(unsigned n,float x,float y,float w,unsigned u=0,unsigned v=0){auto &m=rt.memory();const unsigned a=base+n*20;m.store8(a,std::uint8_t(u));m.store8(a+1,std::uint8_t(v));m.store16(a+2,0xDEAD);m.store32(a+4,0xFFFFFFFF);m.store32(a+8,std::bit_cast<unsigned>(x));m.store32(a+12,std::bit_cast<unsigned>(y));m.store32(a+16,std::bit_cast<unsigned>(w));}
 void triangle(){vertex(0,0,0,1);vertex(1,8,0,1,128);vertex(2,0,8,1,0,128);}
 void reset(){end=0xDEADBEEF;index=0xBAADF00D;written=0xDEADF00D;error.clear();}
};
using Draw=bool(*)(psprecomp::GuestMemory&,const std::array<unsigned,256>&,const std::array<float,12>&,const std::array<float,12>&,const std::array<float,16>&,unsigned,unsigned,unsigned&,std::string&,const chili::GeClutCache&,unsigned*,unsigned,unsigned*,const chili::ge::DeformState*);
struct Outcome {bool success{};std::vector<std::uint8_t> vram;unsigned end{},index{},written{};std::string error;quad_cache_fixture::Trace trace;};
Outcome run(Fixture &f,Draw draw,unsigned count){Outcome out;quad_cache_fixture::sink=&out.trace;out.success=draw(f.rt.memory(),f.regs,f.world,f.view,f.projection,f.base,0x30000|count,f.end,f.error,f.clut,&f.written,0,&f.index,nullptr);quad_cache_fixture::sink=nullptr;out.vram=f.rt.memory().vram_bytes();out.end=f.end;out.index=f.index;out.written=f.written;out.error=f.error;return out;}
void compare(Fixture &f,unsigned count,bool expect_success,bool reuse,const char *why){f.reset();const auto before=f.rt.memory().vram_bytes(),ram=f.rt.memory().bytes();const auto cpu=f.rt.cpu();std::array<std::uint8_t,psprecomp::GuestMemory::kScratchpadSize> scratch{};f.rt.memory().copy_out(0x10000,scratch);
 const auto old=run(f,chili_quad_trace_reference::render_ge_triangle_raster,count);require(old.success==expect_success,std::string("baseline wrong expected outcome: ")+why+" error="+old.error);f.rt.memory().copy_in(0x04000000,before);f.reset();const auto candidate=run(f,chili_quad_trace_candidate::render_ge_triangle_raster,count);
 require(candidate.success==old.success&&candidate.error==old.error&&candidate.end==old.end&&candidate.index==old.index&&candidate.written==old.written&&candidate.vram==old.vram,std::string("wholeVRAM/error/cursor/count parity: ")+why);
 require(f.rt.memory().bytes()==ram&&same_cpu(f.rt.cpu(),cpu),std::string("source/NaN CPU changed: ")+why);std::array<std::uint8_t,psprecomp::GuestMemory::kScratchpadSize> now{};f.rt.memory().copy_out(0x10000,now);require(now==scratch,"scratch changed");
 require(old.trace.attempts==candidate.trace.attempts&&old.trace.covered==candidate.trace.covered&&old.trace.selections==candidate.trace.selections,std::string("covered UV/W/first-attempt/footprint/selection bit trace differs: ")+why);
 if(expect_success){if(f.regs[0x1E]&1)require(!candidate.trace.covered.empty(),"textured success has no covered evaluation");else require(candidate.trace.covered.empty()&&candidate.trace.selections.empty(),"untextured fallback evaluated UV/mip");require(old.trace.helpers==old.trace.covered.size()*3,"baseline did not evaluate all helpers");if(reuse)require(candidate.trace.helpers<old.trace.helpers,"memo fixture did not save helper calls");else require(candidate.trace.helpers==old.trace.helpers,"fallback/one-pixel incorrectly reused helpers");}
 else require(candidate.vram==before&&candidate.end==0xDEADBEEF&&candidate.index==0xBAADF00D&&candidate.written==0xDEADF00D,"late failed draw not atomic");
 // Test-only trace copies cannot establish uninstrumented production parity.
 f.rt.memory().copy_in(0x04000000,before);f.reset();const bool production=chili::render_ge_triangle_raster(f.rt.memory(),f.regs,f.world,f.view,f.projection,f.base,0x30000|count,f.end,f.error,f.clut,&f.written,0,&f.index);
 require(production==old.success&&f.error==old.error&&f.end==old.end&&f.index==old.index&&f.written==old.written&&f.rt.memory().vram_bytes()==old.vram&&f.rt.memory().bytes()==ram&&same_cpu(f.rt.cpu(),cpu),"untraced production differs from traced proof");
 std::cout<<"PASS "<<why<<" covered="<<old.trace.covered.size()<<" baseline_helpers="<<old.trace.helpers<<" candidate_helpers="<<candidate.trace.helpers<<" success="<<production<<'\n';
}
void successes(){
 for(unsigned variant=0;variant<5;++variant){Fixture f;f.triangle();if(variant==1)f.regs[0xD4]=1|(1<<10);if(variant==2){f.regs[0x50]=1;f.regs[0x23]=1;f.regs[0xDE]=1;f.regs[0x21]=1;f.regs[0xDF]=0x32;f.regs[0xE8]=0x550055;}if(variant==3){f.regs[0xD5]=f.regs[0x16]=1|(1<<10);}if(variant==4){f.regs[0xD6]=f.regs[0xD7]=100;f.regs[0x22]=1;f.regs[0xDB]=0x8004;for(unsigned n=0;n<8;++n)f.rt.memory().store32(0x08820000+n*4,0x00123456+n);require(f.clut.load(f.rt.memory(),0x820000,0x80000,2,f.error),"alpha palette reload failed");}compare(f,3,true,true,"covered/odd scissor/partial2x2/depth-blend-mask/alpha neighbors");}
 {Fixture f;f.vertex(0,0,0,1);f.vertex(1,1,0,1,128);f.vertex(2,0,1,1,0,128);compare(f,3,true,false,"single covered pixel");}
 {Fixture f;f.triangle();f.vertex(3,0,0,.5f);f.vertex(4,4,0,.5f,64);f.vertex(5,0,4,.5f,0,64);compare(f,6,true,true,"same quad keys reset across changed-perspective triangles");}
 for(unsigned mode:{1u,2u,3u}){Fixture f;f.triangle();f.regs[0xC8]=mode;f.number(0xD0,1);compare(f,3,true,false,"CONST/SLOPE fallback");}
 {Fixture f;f.triangle();f.regs[0x1E]=0;compare(f,3,true,false,"untextured fallback");}
 for(unsigned filter:{0u,1u,4u,5u,6u,7u})for(unsigned bias:{0u,15u,16u,255u}){Fixture f;f.triangle();f.regs[0xC6]=0x100|filter;f.regs[0xC8]=bias<<16;f.regs[0xC7]=0x101;compare(f,3,true,true,"nearest/linear/mip/fraction/bias/clamp selections");}
}
void strict(){
 for(unsigned later:{0u,1u}){Fixture f;const unsigned first=later?3:0;if(later){f.vertex(0,0,0,1);f.vertex(1,1,0,1,128);f.vertex(2,0,1,1,0,128);}f.vertex(first,7.f/32,0,.5f);f.vertex(first+1,15.f/16,0,1,128);f.vertex(first+2,7.f/32,.5f,.5f);compare(f,later?6:3,false,false,"exact helper zero pole / late pole atomicity");}
 for(unsigned negative:{0u,1u}){Fixture f;if(negative){f.vertex(0,1,0,1);f.vertex(1,.25f,0,.125f,128);f.vertex(2,1,1,1);}else{f.vertex(0,0,0,.125f);f.vertex(1,1,0,1,128);f.vertex(2,0,.125f,.125f);}compare(f,3,true,false,"finite negative helper denominator");f.number(0x48,std::numeric_limits<float>::max());compare(f,3,false,false,"nonfinite helper UV / overflow footprint atomicity");}
 {Fixture f;f.triangle();f.regs[0xC3]=6;compare(f,3,false,false,"unsupported texture format preflight");}
}
}
int main(){try{successes();strict();std::cout<<"PASS "<<checks<<" exact T4 AUTO quad memo assertions; no timing/game/FP precision claim\n";return 0;}catch(const std::exception &e){quad_cache_fixture::sink=nullptr;std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
