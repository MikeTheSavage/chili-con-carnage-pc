#include "ge_runtime.hpp"
#include "texture_accessor_triangle_reference.hpp"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>



namespace {
using Bytes=std::vector<std::uint8_t>;using Clock=std::chrono::steady_clock;
unsigned checks{};
void require(bool value,const std::string &message){++checks;if(!value)throw std::runtime_error(message);}
Bytes read(const std::filesystem::path &path,std::size_t size){std::ifstream in(path,std::ios::binary);require(bool(in),"capture absent: "+path.string());in.seekg(0,std::ios::end);require(in.tellg()==static_cast<std::streamoff>(size),"capture size changed");in.seekg(0);Bytes bytes(size);in.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(size));require(bool(in),"capture read failed");return bytes;}
bool same_cpu(const psprecomp::AllegrexContext &a,const psprecomp::AllegrexContext &b){return a.gpr==b.gpr&&a.pc==b.pc&&a.hi==b.hi&&a.lo==b.lo&&a.fcr31==b.fcr31&&a.vfpu_ctrl==b.vfpu_ctrl&&std::memcmp(a.fpr.data(),b.fpr.data(),sizeof(a.fpr))==0&&std::memcmp(a.vfpu.data(),b.vfpu.data(),sizeof(a.vfpu))==0;}
struct Input {
 std::array<unsigned,256> regs{};std::array<float,12> world{},view{};std::array<float,16> projection{};
 unsigned vertices{},data{},texture{},palette{},committed{};std::string name;Bytes source,vram,texels,palette_bytes;
};
Input input(const std::filesystem::path &prefix,bool dxt){Input out;std::ifstream metadata(prefix.string()+".txt");require(bool(metadata),"capture metadata absent");std::string line;
 while(std::getline(metadata,line)){std::istringstream words(line);std::string kind;words>>kind;
  if(kind=="reg"){unsigned n{},v{};words>>std::hex>>n>>v;require(bool(words)&&n<256,"capture register malformed");out.regs[n]=v;}
  else if(kind=="world"||kind=="view"||kind=="projection"){auto parse=[&](auto &matrix){for(auto &v:matrix){words>>v;require(bool(words)&&std::isfinite(v),"capture matrix malformed");}};if(kind=="world")parse(out.world);else if(kind=="view")parse(out.view);else parse(out.projection);}
  else if(kind.starts_with("primitive=")){out.data=unsigned(std::stoul(kind.substr(10),nullptr,16));std::string address;words>>address;require(address.starts_with("vertices="),"capture address absent");out.vertices=unsigned(std::stoul(address.substr(9),nullptr,16));}}
 require(out.data==(dxt?0x400A0u:0x40056u)&&out.vertices==(dxt?0x084E9C04u:0x0850B1FCu)&&(out.regs[0x12]&0xFFFFFF)==(dxt?0xB5u:0x122u),"capture draw ABI changed");out.committed=dxt?163840u:4425u;out.name=dxt?"DXT1_clip160":"T4_mip_strip86";
 out.texture=(out.regs[0xA0]&0xFFFFF0)|((out.regs[0xA8]<<8)&0x0F000000);out.palette=(out.regs[0xB0]&0xFFFFF0)|((out.regs[0xB1]<<8)&0x0F000000);
 out.source=read(prefix.string()+"_vertices.bin",dxt?1600:1204);out.vram=read(prefix.string()+"_vram.bin",0x200000);out.texels=read(prefix.string()+"_texture.bin",dxt?128:4096);out.palette_bytes=read(prefix.string()+"_palette_source.bin",dxt?1024:64);return out;
}
struct Fixture {
 psprecomp::Runtime rt;chili::GeClutCache clut;unsigned cursor=0xDEADBEEF,index=0xBAADF00D,written=0xDEADF00D;std::string error;
 explicit Fixture(const Input &in){restore(in);require(clut.load(rt.memory(),in.regs[0xB0],in.regs[0xB1],in.regs[0xC4],error),"CLUT load failed");auto &cpu=rt.cpu();for(unsigned n=1;n<cpu.gpr.size();++n)cpu.gpr[n]=0x90000000+n;cpu.pc=0x08D98ED0;cpu.hi=0x12345678;cpu.lo=0x87654321;cpu.fcr31=0xACE0101;for(unsigned n=0;n<cpu.fpr.size();++n)cpu.fpr[n]=std::bit_cast<float>(0x7FC10000+n);for(unsigned n=0;n<cpu.vfpu.size();++n)cpu.vfpu[n]=std::bit_cast<float>(0xFFC10000+n);}
 void restore(const Input &in){auto &m=rt.memory();m.copy_in(in.vertices,in.source);m.copy_in(0x04000000,in.vram);m.copy_in(in.texture,in.texels);m.copy_in(in.palette,in.palette_bytes);cursor=0xDEADBEEF;index=0xBAADF00D;written=0xDEADF00D;error.clear();}
};
using Draw=bool(*)(psprecomp::GuestMemory&,const std::array<unsigned,256>&,const std::array<float,12>&,const std::array<float,12>&,const std::array<float,16>&,unsigned,unsigned,unsigned&,std::string&,const chili::GeClutCache&,unsigned*,unsigned,unsigned*,const chili::ge::DeformState*);
bool draw(Draw fn,Fixture &f,const Input &in){return fn(f.rt.memory(),in.regs,in.world,in.view,in.projection,in.vertices,in.data,f.cursor,f.error,f.clut,&f.written,0,&f.index,nullptr);}
struct CapturedCerr{std::ostringstream out;std::streambuf *previous=std::cerr.rdbuf(out.rdbuf());std::ios::fmtflags flags=std::cerr.flags();std::streamsize width=std::cerr.width();~CapturedCerr(){std::cerr.rdbuf(previous);std::cerr.flags(flags);std::cerr.width(width);}};
Bytes acceptance(const Input &in,bool watching){
 Fixture reference(in),candidate(in);const auto ram=reference.rt.memory().bytes();const auto cpu=reference.rt.cpu();std::array<std::uint8_t,psprecomp::GuestMemory::kScratchpadSize> scratch{};reference.rt.memory().copy_out(0x10000,scratch);
 std::string old_watch,new_watch;
 {CapturedCerr log;require(draw(chili_triangle_reference::render_ge_triangle_raster,reference,in),"A11 original-sampler capture rejected: "+reference.error);old_watch=log.out.str();}
 {CapturedCerr log;require(draw(chili::render_ge_triangle_raster,candidate,in),"candidate capture rejected: "+candidate.error);new_watch=log.out.str();}
 require(reference.written==in.committed&&candidate.written==reference.written,"capture committed count changed");require(reference.cursor==in.vertices+unsigned(in.source.size())&&candidate.cursor==reference.cursor&&candidate.index==0&&reference.index==0,"capture cursors changed");
 require(reference.rt.memory().vram_bytes()==candidate.rt.memory().vram_bytes(),"captured full2MiB differs");require(reference.rt.memory().bytes()==ram&&candidate.rt.memory().bytes()==ram&&same_cpu(reference.rt.cpu(),cpu)&&same_cpu(candidate.rt.cpu(),cpu),"capture source/CPU changed");
 std::array<std::uint8_t,psprecomp::GuestMemory::kScratchpadSize> actual{};candidate.rt.memory().copy_out(0x10000,actual);require(actual==scratch,"scratch changed");
 require(old_watch==new_watch,"actual watched depth/color operation/order/value/context differs");require((old_watch.find("[watch-write]")!=std::string::npos)==watching,"watch fixture has no genuine event or default emitted watch");
 const auto expected=candidate.rt.memory().vram_bytes();
 for(bool enabled:{false,true}){Fixture f(in);CapturedCerr log;chili::ThreadRuntime scheduler(f.rt);std::ostringstream output;chili::PerformanceTelemetry telemetry(output);chili::GeRuntime gpu(f.rt,&scheduler,0,nullptr,enabled?&telemetry:nullptr);gpu.commands=in.regs;gpu.clut=f.clut;gpu.world=in.world;gpu.view=in.view;gpu.projection=in.projection;gpu.vertex_address=in.vertices;
  const unsigned list=0x09F00000;f.rt.memory().store32(list,in.data|0x04000000);f.rt.memory().store32(list+4,0x0F000000);f.rt.memory().store32(list+8,0x0C000000);f.rt.cpu().gpr[4]=list;f.rt.cpu().gpr[5]=0;f.rt.cpu().gpr[6]=0xFFFFFFFF;f.rt.cpu().gpr[7]=0;const auto hle_ram=f.rt.memory().bytes();auto wanted_cpu=f.rt.cpu();wanted_cpu.gpr[2]=1;const auto uid=psprecomp::runtime_thread_uid();const auto generation=psprecomp::runtime_thread_switch_generation();const auto time=scheduler.now_us();f.rt.invoke_import("sceGe_user",0xAB49E76A,f.rt.cpu());
  auto commands=in.regs;commands[4]=in.data|0x04000000;commands[0x0F]=0x0F000000;require(!f.rt.stopped()&&gpu.completed.contains(1)&&gpu.commands==commands&&gpu.vertex_address==in.vertices+unsigned(in.source.size())&&gpu.index_address==0&&gpu.world==in.world&&gpu.view==in.view&&gpu.projection==in.projection,"native GE completion/state differs");require(f.rt.memory().vram_bytes()==expected&&f.rt.memory().bytes()==hle_ram&&same_cpu(f.rt.cpu(),wanted_cpu)&&scheduler.now_us()==time&&psprecomp::runtime_thread_uid()==uid&&psprecomp::runtime_thread_switch_generation()==generation,"native HLE fullVRAM/source/CPU/UID/time changed");
  chili::PerformanceTelemetry::Sample sample;require(telemetry.sample(chili::PerformanceTelemetry::Clock::now(),time,sample,true),"telemetry sample failed");if(enabled)require(sample.total[chili::PerformanceTelemetry::TriangleCommittedFragments]==in.committed&&sample.total[chili::PerformanceTelemetry::DrawSuccesses]==1,"enabled provenance changed");else for(auto counter:sample.total)require(counter==0,"null observer counted");}
 std::cout<<"PASS Authoritative A11 original-vs-AOT sampler "<<in.name<<" commits="<<in.committed<<" complete2MiB/source/CPU/cursors and realGE UID/deadline parity; watched="<<watching<<'\n';return expected;
}
double batch(Draw fn,Fixture &f,const Input &in,const Bytes &expected,unsigned repetitions){std::chrono::nanoseconds elapsed{};
 for(unsigned n=0;n<repetitions;++n){f.restore(in);const auto cpu=f.rt.cpu();const auto start=Clock::now();const bool ok=draw(fn,f,in);const auto stop=Clock::now();elapsed+=std::chrono::duration_cast<std::chrono::nanoseconds>(stop-start);require(ok&&f.cursor==in.vertices+unsigned(in.source.size())&&f.index==0&&f.written==in.committed,"timed draw output ABI differs");require(f.rt.memory().vram_bytes()==expected&&same_cpu(f.rt.cpu(),cpu),"timed fullVRAM/CPU differs");}
 return double(elapsed.count())/1e6/repetitions;
}
void benchmark(const Input &in,const Bytes &expected){Fixture old(in),candidate(in);(void)batch(chili_triangle_reference::render_ge_triangle_raster,old,in,expected,2);(void)batch(chili::render_ge_triangle_raster,candidate,in,expected,2);
 constexpr unsigned pairs=9;const unsigned repetitions=(in.data&65535)>100?4:16;std::vector<double> old_ms,new_ms,ratios;
 std::cout<<std::fixed<<std::setprecision(6)<<"Host-only paired captured draw; telemetry null; restore/full2MiB checks outside timed draw; default MSVC precise/O2; no gameFPS claim\n";
 for(unsigned pair=0;pair<pairs;++pair){double a,b;if(pair&1){b=batch(chili::render_ge_triangle_raster,candidate,in,expected,repetitions);a=batch(chili_triangle_reference::render_ge_triangle_raster,old,in,expected,repetitions);}else{a=batch(chili_triangle_reference::render_ge_triangle_raster,old,in,expected,repetitions);b=batch(chili::render_ge_triangle_raster,candidate,in,expected,repetitions);}old_ms.push_back(a);new_ms.push_back(b);ratios.push_back(a/b);std::cout<<"pair workload="<<in.name<<" index="<<pair<<" draws="<<repetitions<<" baseline_ms="<<a<<" candidate_ms="<<b<<" ratio="<<a/b<<'\n';}
 std::sort(old_ms.begin(),old_ms.end());std::sort(new_ms.begin(),new_ms.end());std::sort(ratios.begin(),ratios.end());std::cout<<"summary workload="<<in.name<<" baseline_median_ms="<<old_ms[pairs/2]<<" candidate_median_ms="<<new_ms[pairs/2]<<" paired_ratio_median="<<ratios[pairs/2]<<" ratio_min="<<ratios.front()<<" ratio_max="<<ratios.back()<<'\n';

}
}
int main(int argc,char **argv){try{require(argc>=3&&argc<=4,"Usage: texture_accessor_test T4-prefix DXT1-prefix [--benchmark]");const std::string option=argc==4?argv[3]:"";require(option.empty()||option=="--benchmark","unknown option");require(std::getenv("PSPRECOMP_WATCH_WRITE")==nullptr,"watch must be unset before texture timing/acceptance");for(unsigned n=0;n<2;++n){const auto in=input(argv[n+1],n==1);const auto expected=acceptance(in,false);if(option=="--benchmark")benchmark(in,expected);}std::cout<<"PASS "<<checks<<" shared texture-read checks; no operation/address/LOD/filter/format/feedback change\n";return 0;}catch(const std::exception &e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
