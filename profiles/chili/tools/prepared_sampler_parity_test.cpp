#include "ge_mip_texture.hpp"
#include "texture_accessor_mip_reference.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
unsigned checks{};
void require(bool ok,const char *why){++checks;if(!ok)throw std::runtime_error(why);}
const std::array<unsigned,12> addresses{0x08810000,0x48810000,0x88810000,0xA8810000,0x04001000,0x04201000,0x04401000,0xA4601000,0x041FFFF0,0x09FFFE00,0x00010000,0x40010000};
void seed(psprecomp::GuestMemory &memory,unsigned address,unsigned count){std::vector<std::uint8_t> bytes(count);for(unsigned n=0;n<count;++n)bytes[n]=std::uint8_t(n*13+29);memory.copy_in(address,bytes);}
void direct(){
 psprecomp::GuestMemory memory;std::string a_error,b_error;
 for(unsigned format:{0u,1u,2u,3u,8u,9u,10u})for(bool swizzled:{false,true}){
  if(format>=8&&swizzled)continue;
  for(unsigned address:addresses){seed(memory,address,512);chili::GeTextureFormatView candidate;chili_triangle_reference::GeTextureFormatView baseline;
   const bool a=baseline.configure(memory,address,16,8,8,format,swizzled,a_error),b=candidate.configure(memory,address,16,8,8,format,swizzled,b_error);require(a==b&&a_error==b_error&&a,"prepared format configure/address alias differs");
   require(baseline.address()==candidate.address()&&baseline.byte_pitch()==candidate.byte_pitch()&&baseline.storage_bytes()==candidate.storage_bytes(),"format storage metadata differs");
   for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){unsigned av=0xDEADBEEF,bv=0xDEADBEEF;require(baseline.sample(memory,x,y,av,a_error)&&candidate.sample(memory,x,y,bv,b_error)&&av==bv,"prepared direct/DXT read addressing/color differs");}
   unsigned av=0xDEADBEEF,bv=0xDEADBEEF;require(!baseline.sample(memory,8,0,av,a_error)&&!candidate.sample(memory,8,0,bv,b_error)&&av==bv&&av==0xDEADBEEF&&a_error==b_error,"invalid format sample altered sentinel/error");
   // Invalid reconfiguration retains exactly the same already prepared view.
   require(!baseline.configure(memory,0x09FFFFF0,16,8,8,format,swizzled,a_error)&&!candidate.configure(memory,0x09FFFFF0,16,8,8,format,swizzled,b_error)&&a_error==b_error,"invalid texture storage accepted/error differs");
   require(baseline.sample(memory,7,7,av,a_error)&&candidate.sample(memory,7,7,bv,b_error)&&av==bv,"failed configure changed retained view");
   a_error.clear();b_error.clear();}
 }
 std::cout<<"PASS direct16/8888 and DXT1/3/5 prepared linear/swizzled reads across RAM aliases, mirrored/wrapping VRAM, scratch and final RAM allocation; strict sentinel/retained-view parity\n";
}
void indexed(){
 psprecomp::GuestMemory memory;chili::GeClutCache clut;std::string error;
 for(unsigned n=0;n<256;++n)memory.store32(0x08850000+n*4,0xFF000000u|n|((255-n)<<8)|((n^85)<<16));
 require(clut.load(memory,0x00850000,0x00080000,32,error),"palette seed failed");
 for(unsigned format:{4u,5u})for(bool swizzled:{false,true})for(unsigned address:{0x08810000u,0x04001000u,0x04201000u,0x04601000u,0x041FFFF0u,0x00010000u}){
  seed(memory,address,512);std::array<unsigned,256> regs{};regs[0xC3]=format;regs[0xC2]=swizzled?1:0;regs[0xC5]=3|0xFF00;regs[0xC6]=0x101;regs[0xA0]=address&0xFFFFFF;regs[0xA8]=((address>>8)&0x0F0000)|32;regs[0xB8]=3|(3<<8);
  chili::GeMipTexture candidate;chili_triangle_reference::GeMipTexture baseline;std::string a_error,b_error;
  require(baseline.configure(memory,regs,&clut,a_error)&&candidate.configure(memory,regs,&clut,b_error)&&a_error==b_error,"indexed configure differs");
  for(unsigned clamp:{0u,0x101u}){regs[0xC7]=clamp;require(baseline.configure(memory,regs,&clut,a_error)&&candidate.configure(memory,regs,&clut,b_error),"indexed clamp configure differs");
   chili::GeMipSelection bpick;chili_triangle_reference::GeMipSelection apick;require(baseline.select(1,1,apick,a_error)&&candidate.select(1,1,bpick,b_error)&&apick.lower==bpick.lower&&apick.upper==bpick.upper&&apick.fraction16==bpick.fraction16&&apick.linear==bpick.linear&&apick.detail16==bpick.detail16,"indexed LOD/filter selection differs");
   for(float u:{-.3f,.125f,.5f,.875f,1.3f})for(float v:{-.3f,.125f,.5f,.875f,1.3f}){unsigned a=0xDEADBEEF,b=0xDEADBEEF;require(baseline.sample(memory,u,v,apick,a,a_error)&&candidate.sample(memory,u,v,bpick,b,b_error)&&a==b,"indexed prepared read/bilinear/wrap differs");}
  }
 }
 std::cout<<"PASS T4/T8 prepared reads, linear filtering/repeat/clamp over tile edges and RAM/VRAM mirrors/scratch, unchanged selections and color values\n";
}
}
int main(){try{direct();indexed();std::cout<<"PASS "<<checks<<" old-vs-public-AOT prepared sampler checks; no GPU/game/clock launched\n";return 0;}catch(const std::exception &e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
