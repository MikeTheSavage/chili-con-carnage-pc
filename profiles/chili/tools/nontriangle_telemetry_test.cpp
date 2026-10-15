#include "ge_runtime.hpp"
#include "nontriangle_telemetry_baseline/ge_sprites_reference.hpp"
#include <bit>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
using Telemetry = chili::PerformanceTelemetry;
unsigned checks{};
void require(bool value,const std::string &why){++checks;if(!value)throw std::runtime_error(why);}
using Bytes = std::vector<std::uint8_t>;
Bytes read(const std::filesystem::path &path,std::size_t size){
    std::ifstream file(path,std::ios::binary);require(bool(file),"capture absent: "+path.string());
    file.seekg(0,std::ios::end);require(file.tellg()==static_cast<std::streamoff>(size),"capture length changed");file.seekg(0);
    Bytes bytes(size);file.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(size));require(bool(file),"capture read failed");return bytes;
}
struct Input{std::array<unsigned,256> regs{};unsigned vertices{},data{};Bytes source,vram;};
Input captured(const std::filesystem::path &prefix,bool untextured){
    Input in;std::ifstream file(prefix.string()+".txt");require(bool(file),"capture metadata absent");std::string line;
    while(std::getline(file,line)){std::istringstream words(line);std::string kind;words>>kind;
        if(kind=="reg"){unsigned reg{},value{};words>>std::hex>>reg>>value;require(bool(words)&&reg<256,"invalid captured register");in.regs[reg]=value;}
        else if(kind.starts_with("primitive=")){in.data=unsigned(std::stoul(kind.substr(10),nullptr,16));std::string address;words>>address;
            require(address.starts_with("vertices="),"captured VADDR absent");in.vertices=unsigned(std::stoul(address.substr(9),nullptr,16));}}
    if(untextured)require(in.data==0x60010&&in.vertices==0x099400D0&&(in.regs[0x12]&0xFFFFFF)==0x800102&&!(in.regs[0x1E]&1)&&(in.regs[0xDF]&0xFFFFFF)==0x2AA,"untextured actual capture changed");
    else require(in.data==0x6001E&&in.vertices==0x09945880&&(in.regs[0x12]&0xFFFFFF)==0x800102&&(in.regs[0x1E]&1)&&(in.regs[0xC3]&0xFFFFFF)==3,"textured actual capture changed");
    in.source=read(prefix.string()+"_vertices.bin",untextured?160u:300u);in.vram=read(prefix.string()+"_vram.bin",psprecomp::GuestMemory::kVramSize);
    if(!untextured){const auto texture=read(prefix.string()+"_texture.bin",1048576);require(std::equal(texture.begin(),texture.end(),in.vram.begin()+0xE8000),"captured texture differs from captured VRAM backing");}
    return in;
}
bool equal_cpu(const psprecomp::AllegrexContext &a,const psprecomp::AllegrexContext &b){
    return a.gpr==b.gpr&&a.hi==b.hi&&a.lo==b.lo&&a.pc==b.pc&&a.fcr31==b.fcr31&&a.vfpu_ctrl==b.vfpu_ctrl&&
        std::memcmp(a.fpr.data(),b.fpr.data(),sizeof(a.fpr))==0&&std::memcmp(a.vfpu.data(),b.vfpu.data(),sizeof(a.vfpu))==0;
}
void seed(psprecomp::AllegrexContext &c){
    for(unsigned n=1;n<32;++n)c.gpr[n]=0x90000000+n;c.gpr[28]=0x08D9ABCD;c.gpr[29]=0x09FE8810;c.gpr[31]=0x08844364;c.pc=0x08D98A58;
    c.hi=0x12345678;c.lo=0x87654321;c.fcr31=0x13579BDF;
    for(unsigned n=0;n<32;++n)c.fpr[n]=std::bit_cast<float>(0x7FC10000+n);
    for(unsigned n=0;n<128;++n)c.vfpu[n]=std::bit_cast<float>(0xFFC10000+n);
    for(unsigned n=0;n<16;++n)c.vfpu_ctrl[n]=0xE4000000+n;
}
void load(psprecomp::Runtime &runtime,const Input &in){runtime.memory().copy_in(in.vertices,in.source);runtime.memory().copy_in(0x04000000,in.vram);seed(runtime.cpu());}
unsigned end(const Input &in){return in.vertices+unsigned(in.source.size());}
void phases(const Telemetry &observer,bool clear,bool untextured,bool feedback,bool raster){
    const auto &c=observer.counters();
    require(c[Telemetry::ClearSpriteAttempts]==unsigned(clear)&&c[Telemetry::UntexturedSpriteAttempts]==unsigned(untextured)&&c[Telemetry::TexturedSpriteAttempts]==unsigned(!clear&&!untextured),"sprite dispatch classification incorrect");
    require(c[Telemetry::SpriteFeedbackAttempts]==unsigned(feedback),"feedback pass classification incorrect");
    const auto pre=clear?Telemetry::ClearSpritePreflightWallNs:untextured?Telemetry::UntexturedSpritePreflightWallNs:Telemetry::TexturedSpritePreflightWallNs;
    const auto draw=clear?Telemetry::ClearSpriteRasterWallNs:untextured?Telemetry::UntexturedSpriteRasterWallNs:Telemetry::TexturedSpriteRasterWallNs;
    require(c[pre]>0,"reached preflight timing absent");require((c[draw]>0)==raster,"unreached raster timing charged or reached raster absent");
    require((c[Telemetry::SpriteFeedbackWallNs]>0)==feedback,"unreached feedback timing charged or reached feedback absent");
    for(auto slot:{Telemetry::ClearSpritePreflightWallNs,Telemetry::UntexturedSpritePreflightWallNs,Telemetry::TexturedSpritePreflightWallNs})if(slot!=pre)require(c[slot]==0,"another sprite preflight class charged");
    for(auto slot:{Telemetry::ClearSpriteRasterWallNs,Telemetry::UntexturedSpriteRasterWallNs,Telemetry::TexturedSpriteRasterWallNs})if(slot!=draw)require(c[slot]==0,"another sprite raster class charged");
}
Bytes baseline(const Input &in){
    psprecomp::Runtime runtime;load(runtime,in);const auto cpu=runtime.cpu();const auto ram=runtime.memory().bytes();unsigned cursor=0xDEADBEEF;std::string error;
    require(chili_sprite_baseline::render_ge_sprites(runtime.memory(),in.regs,in.vertices,in.data,cursor,error),"frozen baseline rejected actual draw: "+error);
    require(cursor==end(in)&&equal_cpu(cpu,runtime.cpu())&&ram==runtime.memory().bytes(),"frozen baseline source/context/cursor changed");return runtime.memory().vram_bytes();
}
void direct(const Input &in,const Bytes &expected,bool enabled,bool clear,bool untextured,bool feedback){
    std::ostringstream output;Telemetry observer(output);psprecomp::Runtime runtime;load(runtime,in);const auto cpu=runtime.cpu();const auto ram=runtime.memory().bytes();unsigned cursor=0xDEADBEEF;std::string error;
    require(chili::render_ge_sprites(runtime.memory(),in.regs,in.vertices,in.data,cursor,error,nullptr,enabled?&observer:nullptr),"modified direct draw rejected: "+error);
    require(runtime.memory().vram_bytes()==expected&&cursor==end(in),"default/enabled complete2MiB VRAM/cursor differs from frozen baseline");
    require(equal_cpu(cpu,runtime.cpu())&&ram==runtime.memory().bytes(),"observer changed full CPU/GP/FPU/VFPU/source RAM");
    require(output.str().empty(),"draw instrumentation emitted output inside renderer");
    if(enabled)phases(observer,clear,untextured,feedback,true);
    else require(observer.counters()==Telemetry::Counters{},"null path updated observer counters");
}
void streamed(const Input &in,const Bytes &expected,bool enabled,bool clear,bool untextured,bool feedback){
    std::ostringstream output;Telemetry observer(output);psprecomp::Runtime runtime;load(runtime,in);chili::ThreadRuntime scheduler(runtime);
    chili::GeRuntime gpu(runtime,&scheduler,0,nullptr,enabled?&observer:nullptr);gpu.commands=in.regs;
    constexpr unsigned list=0x09E00000;runtime.memory().store32(list,0x10000000|((in.vertices>>8)&0xFF0000));runtime.memory().store32(list+4,0x01000000|(in.vertices&0xFFFFFF));
    runtime.memory().store32(list+8,0x04000000|in.data);runtime.memory().store32(list+12,0x0F00BEEF);runtime.memory().store32(list+16,0x0C000000);
    runtime.cpu().gpr[4]=list;runtime.cpu().gpr[5]=0;runtime.cpu().gpr[6]=0xFFFFFFFF;runtime.cpu().gpr[7]=0;auto expected_cpu=runtime.cpu();expected_cpu.gpr[2]=1;
    const auto ram=runtime.memory().bytes();const auto time=scheduler.now_us(),generation=psprecomp::runtime_thread_switch_generation();const auto uid=psprecomp::runtime_thread_uid();
    runtime.invoke_import("sceGe_user",0xAB49E76Au,runtime.cpu());
    require(!runtime.stopped()&&gpu.completed.contains(1)&&gpu.vertex_address==end(in)&&gpu.commands[4]==(0x04000000|in.data),"streamed default/enabled draw/cursor/completion changed");
    require(runtime.memory().vram_bytes()==expected&&runtime.memory().bytes()==ram&&equal_cpu(runtime.cpu(),expected_cpu),"streamed complete2MiB output/fullCPU/source differs from baseline");
    require(scheduler.now_us()==time&&psprecomp::runtime_thread_uid()==uid&&psprecomp::runtime_thread_switch_generation()==generation,"observer changed guest time/thread scheduling");
    require(output.str().empty(),"streamed instrumentation emitted output inside HLE");
    if(enabled){phases(observer,clear,untextured,feedback,true);const auto &c=observer.counters();
        require(c[Telemetry::DrawAttempts]==1&&c[Telemetry::DrawSuccesses]==1&&c[Telemetry::DrawFailures]==0&&c[Telemetry::GeListsCompleted]==1,"existing whole-draw/list metrics changed");
        const auto phases_ns=c[Telemetry::ClearSpritePreflightWallNs]+c[Telemetry::UntexturedSpritePreflightWallNs]+c[Telemetry::TexturedSpritePreflightWallNs]+c[Telemetry::SpriteFeedbackWallNs]+c[Telemetry::ClearSpriteRasterWallNs]+c[Telemetry::UntexturedSpriteRasterWallNs]+c[Telemetry::TexturedSpriteRasterWallNs];
        require(phases_ns<=c[Telemetry::DrawWallNs],"disjoint sprite phases exceeded enclosing draw scope");
    }else require(observer.counters()==Telemetry::Counters{},"default streamed path changed counters");
}
void actual(const Input &in,bool untextured){const auto expected=baseline(in);
    if(untextured){auto oracle=in.vram;unsigned changed{};for(unsigned y=0;y<272;++y)for(unsigned x=0;x<480;++x)for(unsigned ch=0;ch<3;++ch){const auto off=0xD8000+(y*512+x)*4+ch;oracle[off]=std::uint8_t(std::max(int(in.vram[off])-25,0));changed+=oracle[off]!=in.vram[off];}
        require(expected==oracle&&changed==391680,"captured untextured output differs from independent exact RGB/alpha/depth oracle");}
    for(bool enabled:{false,true}){direct(in,expected,enabled,false,untextured,!untextured);streamed(in,expected,enabled,false,untextured,!untextured);}
    std::cout<<"PASS Actual "<<(untextured?"untextured16/2AA":"textured30/overlapping-copy")<<": frozen baseline, default-null and enabled direct/realGE full2MiB VRAM, cursor, fullCPU, sourceRAM and guest-time parity\n";
}
void clear(){Input in;in.vertices=0x08804000;in.data=0x60002;in.source.resize(24);in.vram.resize(psprecomp::GuestMemory::kVramSize,0xA7);
    auto word=[&](unsigned off,unsigned value,unsigned size){for(unsigned n=0;n<size;++n)in.source[off+n]=std::uint8_t(value>>(n*8));};word(0,0x80102040,4);word(4,0,2);word(6,0,2);word(8,7,2);word(12,0xAA332211,4);word(16,480,2);word(18,272,2);word(20,9,2);
    in.regs[0x12]=0x80011C;in.regs[0xD3]=0x701;in.regs[0xD2]=3;in.regs[0x9D]=512;in.regs[0x9E]=0x100000;in.regs[0x9F]=512;in.regs[0x16]=in.regs[0xD5]=479|(271<<10);
    const auto expected=baseline(in);for(bool enabled:{false,true}){direct(in,expected,enabled,true,false,false);streamed(in,expected,enabled,true,false,false);}
    std::cout<<"PASS Clear path: phase classification, default/enabled baseline output/cursor/context and realGE parity\n";
}
void failures(const Input &actual_texture){for(bool feedback:{false,true})for(bool enabled:{false,true}){
    auto in=actual_texture;if(feedback){in.regs[0x9C]=0xE8000;in.regs[0xA0]=0x040D8000;}else in.regs[0x12]=0;
    std::ostringstream output;Telemetry observer(output);psprecomp::Runtime runtime;load(runtime,in);const auto cpu=runtime.cpu();const auto ram=runtime.memory().bytes();unsigned cursor=0xDEADBEEF;std::string error;
    require(!chili::render_ge_sprites(runtime.memory(),in.regs,in.vertices,in.data,cursor,error,nullptr,enabled?&observer:nullptr),"strict failing preflight/feedback accepted");
    require(error==(feedback?"sprite feedback has a read-after-write dependency":"sprite vertex format/primitive unsupported"),"existing strict rejection reason changed");
    require(runtime.memory().vram_bytes()==in.vram&&cursor==0xDEADBEEF&&equal_cpu(cpu,runtime.cpu())&&ram==runtime.memory().bytes(),"failed observer path partially committed VRAM/cursor/source/context");
    require(output.str().empty(),"failed render instrumentation emitted output");if(enabled)phases(observer,false,false,feedback,false);else require(observer.counters()==Telemetry::Counters{},"null failure path updated metrics");
    }
    std::cout<<"PASS Invalid format and dependent feedback: failure-phase accounting, exact strict errors, fullVRAM/cursor/context atomicity\n";
}
void output(){std::ostringstream out;const auto epoch=Telemetry::Clock::now();Telemetry observer(out,epoch);
    {Telemetry::SpriteScope scope(&observer,Telemetry::SpritePath::Textured);scope.feedback();scope.raster();}
    require(out.str().empty(),"scope emitted output");out<<std::hex;out.width(13);const auto flags=out.flags();observer.poll(epoch+std::chrono::seconds(1),17);
    require(out.str().starts_with("[perf] wall_us=1000000 guest_us=17"),"existing raw-unit row formatting changed");
    require(out.flags()==flags&&out.width()==13,"new fields altered caller stream state");
    require(out.str().find(" sprite_textured_attempts=1")!=std::string::npos&&out.str().find(" sprite_feedback_attempts=1")!=std::string::npos&&out.str().find(" sprite_textured_raster_ns=")!=std::string::npos,"new coarse fields missing");
    require(out.str().find("fps")==std::string::npos,"instrumentation invented game FPS");const auto size=out.str().size();observer.poll(epoch+std::chrono::milliseconds(1999),17);require(out.str().size()==size,"row volume increased above existing cadence");
}
}
int main(int argc,char **argv){try{require(argc==3,"Usage: nontriangle_telemetry_test <untextured capture prefix> <textured capture prefix>");
    const auto untextured=captured(argv[1],true),texture=captured(argv[2],false);actual(untextured,true);actual(texture,false);clear();failures(texture);output();
    std::cout<<"PASS "<<checks<<" optional nontriangle telemetry assertions; no game/window/input launched, no optimization or FPS claim\n";
}catch(const std::exception &e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
