#include "ge_runtime.hpp"
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
namespace {
unsigned checks{};
void check(bool value,const std::string &why){++checks;if(!value)throw std::runtime_error(why);}
struct Fixture{
 psprecomp::Runtime rt;std::array<unsigned,256> r{};unsigned v=0x08804000,end=0xDEADBEEF;std::string error;
 Fixture(){r[0x12]=0x800102;r[0xD2]=3;r[0x9D]=8;r[0x9E]=0x80000;r[0x9F]=8;r[0xD5]=r[0x16]=7|(7<<10);r[0xD7]=65535;r[0x55]=0x191919;r[0x58]=255;r[0xE7]=1;reset();}
 void reset(unsigned color=0xA1406080){for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){rt.memory().store32(0x04000000+(y*8+x)*4,color);rt.memory().store16(0x04080000+(y*8+x)*2,8);}end=0xDEADBEEF;error.clear();}
 void vertex(unsigned n,int x,int y,unsigned z,unsigned color=0x80102040){const auto l=chili::ge::make_layout(r[0x12]);const unsigned a=v+n*l.vertex_stride;
  for(unsigned p=0;p<l.vertex_stride;++p)rt.memory().store8(a+p,0xCC);
  if(l.uv_type==3){rt.memory().store32(a+l.uv_offset,0x7FC12345);rt.memory().store32(a+l.uv_offset+4,0xFF800000);}
  if(l.color_type){if(l.color_type==7)rt.memory().store32(a+l.color_offset,color);else{unsigned c=l.color_type==4?((color&255)>>3)|(((color>>8)&255)>>2<<5)|(((color>>16)&255)>>3<<11):l.color_type==5?((color&255)>>3)|(((color>>8)&255)>>3<<5)|(((color>>16)&255)>>3<<10)|((color>>31)<<15):((color&255)>>4)|(((color>>8)&255)>>4<<4)|(((color>>16)&255)>>4<<8)|((color>>28)<<12);rt.memory().store16(a+l.color_offset,c);}}
  rt.memory().store16(a+l.position_offset,std::uint16_t(x));rt.memory().store16(a+l.position_offset+2,std::uint16_t(y));rt.memory().store16(a+l.position_offset+4,z);
 }
 bool draw(unsigned count=2){return chili::render_ge_sprites(rt.memory(),r,v,0x060000|count,end,error);}
 unsigned pixel(unsigned x,unsigned y){return rt.memory().load32(0x04000000+(y*8+x)*4);}
 void region(unsigned original,unsigned expected,unsigned x0=0,unsigned y0=0,unsigned x1=1,unsigned y1=1){for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)check(pixel(x,y)==((x>=x0&&x<=x1&&y>=y0&&y<=y1)?expected:original),"untextured coverage/material/stencil mismatch");}
};
bool cpu_equal(const psprecomp::AllegrexContext &a,const psprecomp::AllegrexContext &b){return a.gpr==b.gpr&&a.pc==b.pc&&a.hi==b.hi&&a.lo==b.lo&&a.fcr31==b.fcr31&&a.vfpu_ctrl==b.vfpu_ctrl&&std::memcmp(a.fpr.data(),b.fpr.data(),a.fpr.size()*4)==0&&std::memcmp(a.vfpu.data(),b.vfpu.data(),a.vfpu.size()*4)==0;}
void seed(Fixture &f){auto &c=f.rt.cpu();for(unsigned n=1;n<c.gpr.size();++n)c.gpr[n]=0x90000000+n;c.gpr[28]=0x08D9ABCD;c.gpr[29]=0x09FE8810;c.gpr[31]=0x08844364;c.pc=0x08D98A58;c.hi=0x12345678;c.lo=0x87654321;c.fcr31=0x13579BDF;
 for(unsigned n=0;n<c.fpr.size();++n)c.fpr[n]=std::bit_cast<float>(0x7FC10000+n);for(unsigned n=0;n<c.vfpu.size();++n)c.vfpu[n]=std::bit_cast<float>(0xFFC10000+n);for(unsigned n=0;n<c.vfpu_ctrl.size();++n)c.vfpu_ctrl[n]=0xE4000000+n;}
void layouts_and_stale(){
 for(unsigned uv:{0u,1u,2u,3u})for(unsigned color:{0u,4u,5u,6u,7u}){
  Fixture f;f.r[0x12]=0x800100|uv|(color<<2);f.r[0x55]=0x332211;f.r[0x58]=0x99;
  // Every texture mode/pointer is stale. UV source floats are NaN/Inf but never used.
  for(unsigned n=0xA0;n<=0xCC;++n)f.r[n]=0xFFFFFF;
  f.r[0x17]=f.r[0x1F]=1;f.r[0x50]=1;f.r[0xD6]=200;f.r[0xD7]=100;f.r[0x9F]=0;f.r[0x9E]=0x1FFFF0;f.r[0xE7]=0;
  f.vertex(0,0,0,9,0xFF0000FF);f.vertex(1,2,2,7,0xFF00FF00);const auto ram=f.rt.memory().bytes();seed(f);const auto cpu=f.rt.cpu();
  check(f.draw(),"untextured layout or unused texture rejected: "+f.error);f.region(0xA1406080,color?0xA100FF00:0xA1332211);
  check(f.end==f.v+2*chili::ge::make_layout(f.r[0x12]).vertex_stride&&f.rt.memory().bytes()==ram&&cpu_equal(f.rt.cpu(),cpu),"untextured source/context/cursor changed");
  for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)check(f.rt.memory().load16(0x04080000+(y*8+x)*2)==8,"disabled depth test wrote/read stale depth");
 }
 std::cout<<"PASS20 UV/color layouts: absent material ambient, flat second-endpoint color, unused NaN UV/texture/lighting/fog/depth state, stencil preserved\n";
}
unsigned expected_rgb(unsigned mode,unsigned src,unsigned dst,unsigned fa,unsigned fb){unsigned out=0;for(unsigned n=0;n<3;++n){const unsigned s=(src>>(n*8))&255,d=(dst>>(n*8))&255,a=mode==0xA2?src>>24:(fa>>(n*8))&255,b=(fb>>(n*8))&255;
 const int st=int(std::floor((s+0.5)*(a+0.5)/256)),dt=int(std::floor((d+0.5)*(b+0.5)/256));const int value=mode==0x1AA?st-dt:mode==0x2AA?dt-st:st+dt;out|=unsigned(std::clamp(value,0,255))<<(n*8);}return out;}
void blend_masks_tests(){
 for(unsigned mode:{0xAAu,0x1AAu,0x2AAu,0xA2u,0x32u})for(unsigned mask:{0u,0x0F55AAu,0xFFFFFFu})for(unsigned alpha_mask:{0u,0x55u,255u}){
  Fixture f;f.r[0x12]=0x80011C;f.r[0x21]=1;f.r[0xDF]=mode;f.r[0xE0]=0xF08020;f.r[0xE1]=0x40C010;f.r[0xE8]=mask;f.r[0xE9]=alpha_mask;f.vertex(0,0,0,9,0xFF0000FF);f.vertex(1,2,2,7,0x901020F0);
  unsigned rgb=expected_rgb(mode,0x901020F0,0xA1406080,f.r[0xE0],f.r[0xE1]);if(mode==0x32){rgb=0;for(unsigned n=0;n<3;++n)rgb|=((((0x901020F0u>>(n*8))&255)*144+((0xA1406080u>>(n*8))&255)*111+127)/255)<<(n*8);}
  check(f.draw(),"untextured blend rejected: "+f.error);f.region(0xA1406080,0xA1000000|(rgb&~mask)|(0x406080&mask));
 }
 for(bool reverse:{false,true}){Fixture f;f.r[0x12]=0x80011C;f.r[0xD4]=1|(1<<10);f.r[0x16]=2|(2<<10);f.r[0x23]=1;f.r[0xE7]=0;f.r[0xDE]=4;f.r[0x22]=1;f.r[0xDB]=0xFF7F06;
  f.vertex(0,reverse?4:-2,reverse?4:-1,9,0x010000FF);f.vertex(1,reverse?-2:4,reverse?-1:4,7,0x8000FF00);
  check(f.draw(),"clipped/reversed second endpoint depth/alpha draw rejected");f.region(0xA1406080,0xA100FF00,1,1,2,2);
  for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)check(f.rt.memory().load16(0x04080000+(y*8+x)*2)==(x>=1&&x<=2&&y>=1&&y<=2?7:8),"second endpoint depth/scissor mismatch");
  f.reset();f.r[0xDE]=6;check(f.draw(),"depth rejecting draw failed");f.region(0xA1406080,0xA1406080);
  f.reset();f.r[0xDE]=4;f.r[0xDB]=0xFF8004;check(f.draw(),"alpha rejecting draw failed");f.region(0xA1406080,0xA1406080);
 }
 std::cout<<"PASS45 FIX/FIX/SRC_ALPHA and legacy32 blend/mask cases, destination alpha, second endpoint depth/alpha tests, reversed/clipped rectangles/REGION2\n";
}
void failures(){for(unsigned kind=0;kind<12;++kind){Fixture f;f.vertex(0,0,0,9);f.vertex(1,2,2,7);
 if(kind==0)f.r[0x21]=1,f.r[0xDF]=0x3AA;
 if(kind==1)f.r[0x24]=1;if(kind==2)f.r[0x27]=1;if(kind==3)f.r[0x28]=1;if(kind==4)f.r[0x20]=1;
 if(kind==5)f.r[0x12]|=1<<18;if(kind==6)f.r[0x12]|=1<<11;if(kind==7)f.r[0x12]&=~0x800000;
 if(kind==8)f.r[0x9C]=0x1FFFF0;if(kind==9)f.r[0x23]=1,f.r[0x9F]=0;
 if(kind==10)f.r[0x23]=1,f.r[0x9E]=0;if(kind==11)f.v=0x09FFFFF6;
 const auto before=f.rt.memory().vram_bytes(),ram=f.rt.memory().bytes();check(!f.draw(),"unverified untextured case accepted");check(f.rt.memory().vram_bytes()==before&&f.rt.memory().bytes()==ram&&f.end==0xDEADBEEF,"failed untextured preflight wrote pixels/source/cursor");}
 Fixture f;f.vertex(0,0,0,9);f.vertex(1,2,2,7);const auto before=f.rt.memory().vram_bytes();check(!f.draw(3)&&f.rt.memory().vram_bytes()==before&&f.end==0xDEADBEEF,"odd count partial commit");
 std::cout<<"PASS Unsupported states, whole target/source bounds, overlap and odd-count failures atomic\n";
}
void runtime_draw(Fixture &f,unsigned count){chili::ThreadRuntime scheduler(f.rt);chili::GeRuntime gpu(f.rt,&scheduler);gpu.commands=f.r;
 const unsigned list=0x09E00000;const unsigned base=(f.v>>8)&0xFF0000;f.rt.memory().store32(list,0x10000000|base);f.rt.memory().store32(list+4,0x01000000|(f.v&0xFFFFFF));f.rt.memory().store32(list+8,0x04060000|count);f.rt.memory().store32(list+12,0x0F00BEEF);f.rt.memory().store32(list+16,0x0C000000);
 const auto before=f.rt.memory().vram_bytes();check(f.draw(count),"expected direct untextured failed: "+f.error);const auto expected=f.rt.memory().vram_bytes();f.rt.memory().copy_in(0x04000000,before);
 seed(f);f.rt.cpu().gpr[4]=list;f.rt.cpu().gpr[5]=0;f.rt.cpu().gpr[6]=0xFFFFFFFF;f.rt.cpu().gpr[7]=0;auto cpu=f.rt.cpu();cpu.gpr[2]=1;
 const auto ram=f.rt.memory().bytes();const auto uid=psprecomp::runtime_thread_uid();const auto generation=psprecomp::runtime_thread_switch_generation();const auto time=scheduler.now_us();
 f.rt.invoke_import("sceGe_user",0xAB49E76Au,f.rt.cpu());check(!f.rt.stopped()&&gpu.completed.contains(1)&&gpu.vertex_address==f.end&&gpu.commands[4]==(0x04060000|count),"actual native untextured GE list did not finish: "+f.rt.stop_reason());
 check(f.rt.memory().vram_bytes()==expected&&f.rt.memory().bytes()==ram&&cpu_equal(f.rt.cpu(),cpu)&&scheduler.now_us()==time&&psprecomp::runtime_thread_uid()==uid&&psprecomp::runtime_thread_switch_generation()==generation,"untextured runtime output/fullCPU/GP/UID/time changed");
}
std::vector<std::uint8_t> read(const std::filesystem::path &p,std::size_t size){std::ifstream s(p,std::ios::binary);check(bool(s),"capture input absent");s.seekg(0,std::ios::end);check(s.tellg()==static_cast<std::streamoff>(size),"capture input size mismatch");s.seekg(0);std::vector<std::uint8_t>b(size);s.read(reinterpret_cast<char*>(b.data()),static_cast<std::streamsize>(size));check(bool(s),"capture read failed");return b;}
void capture(const std::filesystem::path &prefix,const std::filesystem::path &independent={}){Fixture f;std::ifstream s(prefix.string()+".txt");check(bool(s),"capture metadata absent");std::string line;unsigned data{};
 while(std::getline(s,line)){std::istringstream in(line);std::string kind;in>>kind;if(kind=="reg"){unsigned n{},v{};in>>std::hex>>n>>v;check(n<256&&bool(in),"invalid captured reg");f.r[n]=v;}else if(kind.starts_with("primitive=")){data=unsigned(std::stoul(kind.substr(10),nullptr,16));std::string address;in>>address;check(address.starts_with("vertices="),"capture VADDR absent");f.v=unsigned(std::stoul(address.substr(9),nullptr,16));}}
 check(data==0x60010&&f.v==0x099400D0&&(f.r[0x12]&0xFFFFFF)==0x800102&&!(f.r[0x1E]&1)&&(f.r[0xDF]&0xFFFFFF)==0x2AA&&!(f.r[0x23]&1)&&(f.r[0xE7]&1),"actual sprite ABI/state changed");
 const auto vertices=read(prefix.string()+"_vertices.bin",160),vram=read(prefix.string()+"_vram.bin",psprecomp::GuestMemory::kVramSize);f.rt.memory().copy_in(f.v,vertices);f.rt.memory().copy_in(0x04000000,vram);
 // Independently form the exact eight disjoint captured rectangles and white
 // FIX reverse-subtract result. No production raster/blend helper forms oracle.
 auto expected=vram;constexpr unsigned framebuffer=0xD8000,stride=512;unsigned covered{},changed{};
 for(unsigned pair=0;pair<8;++pair){const auto a=pair*20,b=a+10;auto h=[&](unsigned offset){return unsigned(vertices[offset])|(unsigned(vertices[offset+1])<<8);};const unsigned x0=h(a+4),y0=h(a+6),x1=h(b+4),y1=h(b+6);
  check(x0==pair*64&&y0==0&&x1==(pair==7?480:(pair+1)*64)&&y1==272,"actual rectangle partition changed");
  for(unsigned y=y0;y<y1;++y)for(unsigned x=x0;x<x1;++x){const auto offset=framebuffer+(y*stride+x)*4;for(unsigned ch=0;ch<3;++ch){expected[offset+ch]=std::uint8_t(std::max(int(vram[offset+ch])-25,0));changed+=expected[offset+ch]!=vram[offset+ch];}++covered;}}
 if(!independent.empty())check(expected==read(independent,psprecomp::GuestMemory::kVramSize),"independent Frame-worker2MiB expected VRAM differs from coordinate oracle");
 seed(f);const auto cpu=f.rt.cpu();const auto ram=f.rt.memory().bytes();check(f.draw(16)&&f.end==0x09940170,"captured untextured draw failed: "+f.error);
 check(f.rt.memory().vram_bytes()==expected&&f.rt.memory().bytes()==ram&&cpu_equal(f.rt.cpu(),cpu),"capture fullVRAM/source/CPU differs from independent rectangles oracle");
 check(covered==130560&&changed==391680,"capture actual effect count differs");f.rt.memory().copy_in(0x04000000,vram);runtime_draw(f,16);
 // Poison all stale texture pointers, formats/filter/mips and UV bytes. The
 // exact same output must remain, even with invalid texture/framebuffer overlap.
 f.rt.memory().copy_in(0x04000000,vram);for(unsigned n=0xA0;n<=0xCC;++n)f.r[n]=0xFFFFFF;
 for(unsigned n=0;n<16;++n){f.rt.memory().store32(f.v+n*10,0xFFFFFFFF);}check(f.draw(16)&&f.rt.memory().vram_bytes()==expected,"poisoned unused capture texture/UV changed output");
 std::cout<<"PASS Actual captured8 untextured rectangles:130560 covered pixels/391680 changed RGBbytes; exact independent full2MiB oracle, no alpha/depth/padding changes, source160-byte cursor/runtime/context, poisoned stale texture ignored\n";
}
}
int main(int argc,char **argv){try{check(argc<=3,"Usage: ge_untextured_sprite_test [captured failure prefix [independent expected VRAM]]");layouts_and_stale();blend_masks_tests();failures();Fixture f;f.vertex(0,0,0,9);f.vertex(1,2,2,7);runtime_draw(f,2);if(argc>=2)capture(argv[1],argc==3?std::filesystem::path(argv[2]):std::filesystem::path{});std::cout<<"PASS "<<checks<<" native untextured sprite assertions; integer through formats only, PSP precision limits unchanged\n";}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
