#include "controller_runtime.hpp"
#include "controller_sampling_provenance.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
using Context=psprecomp::AllegrexContext;
using Observer=chili::ControllerSamplingProvenance;
enum class SinkMode { Null, Good, Throwing };
constexpr unsigned output=0x08802000u,done=0x08804000u;
unsigned checks{};
void require(bool ok,const char *why){++checks;if(!ok)throw std::runtime_error(why);}
bool same_cpu(const Context &a,const Context &b){return a.gpr==b.gpr&&a.pc==b.pc&&a.hi==b.hi&&a.lo==b.lo&&a.fcr31==b.fcr31&&a.vfpu_ctrl==b.vfpu_ctrl&&std::memcmp(a.fpr.data(),b.fpr.data(),sizeof(a.fpr))==0&&std::memcmp(a.vfpu.data(),b.vfpu.data(),sizeof(a.vfpu))==0;}
void seed(Context &c,unsigned ordinal){for(unsigned i=1;i<c.gpr.size();++i)c.gpr[i]=0x90000000u+i+ordinal*0x100u;c.gpr[0]=0;c.hi=0xABC00000u+ordinal;c.lo=0x12300000u+ordinal;c.pc=0x08801234;c.fcr31=0x00100001u;for(unsigned i=0;i<c.fpr.size();++i)c.fpr[i]=std::bit_cast<float>(0x7FC10000u+i+ordinal*0x100u);for(unsigned i=0;i<c.vfpu.size();++i)c.vfpu[i]=std::bit_cast<float>(0xFFC10000u+i+ordinal*0x100u);for(unsigned i=0;i<c.vfpu_ctrl.size();++i)c.vfpu_ctrl[i]=0xE4u+i;c.gpr[31]=done;}
struct Result {std::vector<std::uint64_t> trace,provider;std::vector<std::uint8_t> memory;bool operator==(const Result &)const=default;};
class ThrowingBuffer:public std::streambuf{
public:unsigned attempts{};
protected:std::streamsize xsputn(const char *,std::streamsize)override{++attempts;throw std::runtime_error("fixture output failure");}
 int_type overflow(int_type)override{++attempts;throw std::runtime_error("fixture output failure");}
};
std::vector<std::uint8_t> whole_memory(const psprecomp::GuestMemory &m){auto bytes=m.bytes();const auto &vram=m.vram_bytes();bytes.insert(bytes.end(),vram.begin(),vram.end());std::array<std::uint8_t,psprecomp::GuestMemory::kScratchpadSize> scratch{};m.copy_out(0x10000,scratch);bytes.insert(bytes.end(),scratch.begin(),scratch.end());return bytes;}

Result exercise(SinkMode sink_mode){
    Result result;psprecomp::Runtime rt;chili::ThreadRuntime threads(rt);threads.initialize_bootstrap();
    auto &c=rt.cpu();auto &m=rt.memory();std::ostringstream log;ThrowingBuffer broken;std::ostream failed(&broken);failed.exceptions(std::ios::badbit|std::ios::failbit);
    std::ostream &sink=sink_mode==SinkMode::Throwing?failed:static_cast<std::ostream&>(log);Observer observer(sink);
    const std::vector<chili::ControllerInput> states{{0,128,128},{0xFFFFFFFF,17,240},{0xF3F9,99,111},{0xF3F9,17,240},{0xF3F9,17,240},{0,128,128},{0,128,0},{0,128,0},{0,128,255},{0,128,128},{0,0,0},{0,128,128}};
    unsigned polls{};auto provider=[&]{require(polls<states.size(),"extra provider poll");const auto value=states[polls++];result.provider.insert(result.provider.end(),{polls,value.buttons,value.x,value.y,threads.now_us(),unsigned(threads.current_uid())});return value;};
    auto controller=std::make_unique<chili::ControllerRuntime>(rt,threads,provider);
#ifndef CONTROLLER_ORIGINAL
    if(sink_mode!=SinkMode::Null)controller->set_sampling_observer(&observer);
#else
    require(sink_mode==SinkMode::Null,"original fixture cannot attach observer");
#endif
    const auto generation=psprecomp::runtime_thread_switch_generation();
    auto checkpoint=[&]{result.trace.insert(result.trace.end(),c.gpr.begin(),c.gpr.end());result.trace.insert(result.trace.end(),{c.hi,c.lo,c.pc,c.fcr31});for(auto f:c.fpr)result.trace.push_back(std::bit_cast<unsigned>(f));for(auto f:c.vfpu)result.trace.push_back(std::bit_cast<unsigned>(f));result.trace.insert(result.trace.end(),c.vfpu_ctrl.begin(),c.vfpu_ctrl.end());result.trace.insert(result.trace.end(),{threads.now_us(),unsigned(threads.current_uid()),unsigned(psprecomp::runtime_thread_uid()),psprecomp::runtime_thread_switch_generation()-generation,unsigned(threads.can_wait()),unsigned(threads.in_interrupt()),unsigned(threads.interrupts_enabled()),polls,unsigned(rt.stopped()),rt.stop_reason().size()});for(unsigned char ch:rt.stop_reason())result.trace.push_back(ch);std::array<std::uint8_t,16> bytes{};m.copy_out(output,bytes);for(auto value:bytes)result.trace.push_back(value);};
    auto call=[&](unsigned nid){result.trace.push_back(nid);rt.invoke_import("sceCtrl",nid,c);checkpoint();};
    auto invalid=[&](unsigned address,unsigned count,unsigned error){seed(c,20+count);c.gpr[4]=address;c.gpr[5]=count;auto wanted=c;wanted.set_gpr(2,error);const auto memory=whole_memory(m);const auto time=threads.now_us();const auto before=polls;call(0x1F803938);require(same_cpu(c,wanted)&&whole_memory(m)==memory&&threads.now_us()==time&&polls==before,"invalid read changed context/memory/time/provider");};
    invalid(output,0,0x80000104);invalid(output,65,0x80000104);invalid(0,1,0x800200D3);invalid(output+1,1,0x800200D3);invalid(0x09FFFFFC,1,0x800200D3);
    require(observer.successful_reads()==0&&log.str().empty(),"invalid request emitted sample");
    require(threads.enter_interrupt(),"fixture could not enter interrupt");invalid(output,1,0x800201A7);threads.leave_interrupt(c);checkpoint();
    rt.invoke_import("Kernel_Library",0x092968F4,c);checkpoint();invalid(output,1,0x800201A7);c.gpr[4]=1;rt.invoke_import("Kernel_Library",0x5F10D406,c);checkpoint();
    require(threads.can_wait()&&polls==0&&observer.successful_reads()==0,"wait rejection polled or observed");
    // Rejected second active instance must not replace the working singleton.
    bool rejected=false;try{chili::ControllerRuntime duplicate(rt,threads,provider);}catch(const psprecomp::Error&){rejected=true;}require(rejected&&polls==0,"duplicate runtime lifecycle changed");
    auto mode=[&](unsigned value,unsigned expected){seed(c,40+value);c.gpr[4]=value;auto wanted=c;wanted.set_gpr(2,expected);call(0x1F4011E6);require(same_cpu(c,wanted),"sampling mode context changed");};
    mode(2,0x80000107);mode(0,0);
    auto sample=[&](unsigned number,bool analog,unsigned address=output){seed(c,number);c.gpr[4]=address;c.gpr[5]=1;auto wanted=c;wanted.set_gpr(2,1);const auto before=whole_memory(m),vram=m.vram_bytes();const auto before_reads=observer.successful_reads();const auto before_polls=polls;call(0x1F803938);
        if(number){require(polls==before_polls&&observer.successful_reads()==before_reads&&c.pc==chili::ThreadRuntime::return_trampoline+8&&threads.now_us()==number*16667u,"blocked initial request polled/observed/deadline changed");require(rt.has_function(c.pc),"registered read continuation missing");rt.run(c.pc,1u);checkpoint();wanted.pc=done;require(rt.stopped()&&rt.stop_reason()=="Dispatch limit reached at 0x08804000","fixture bounded continuation did not complete once");}
        require(same_cpu(c,wanted)&&polls==before_polls+1&&threads.now_us()==number*16667u,"successful sample/context/provider/deadline changed");
        std::array<std::uint8_t,16> delivered{};m.copy_out(address,delivered);const auto &state=states[number];auto word=[&](unsigned at,unsigned value){for(unsigned i=0;i<4;++i)require(delivered[at+i]==std::uint8_t(value>>(i*8)),"sample word layout changed");};word(0,number*16667u);word(4,state.buttons&0xF3F9);require(delivered[8]==(analog?state.x:128)&&delivered[9]==(analog?state.y:128),"delivered axes changed");for(unsigned i=10;i<16;++i)require(delivered[i]==0,"reserved sample byte changed");
        auto wanted_memory=before;const auto offset=psprecomp::GuestMemory::canonical(address)-psprecomp::GuestMemory::kPhysicalBase;require(offset+delivered.size()<=m.bytes().size(),"fixture alias outsideRAM");std::copy(delivered.begin(),delivered.end(),wanted_memory.begin()+offset);require(whole_memory(m)==wanted_memory&&m.vram_bytes()==vram,"sample write footprint changed");
    };
    sample(0,false);sample(1,false);sample(2,false);mode(1,0);sample(3,true);sample(4,true);sample(5,true);sample(6,true);sample(7,true);sample(8,true);sample(9,true,output|0x40000000u);mode(0,1);sample(10,false);
#ifndef CONTROLLER_ORIGINAL
    if(sink_mode!=SinkMode::Null){require(observer.successful_reads()==11&&observer.state_changes()==8,"observer lost genuine sample or state change");if(sink_mode==SinkMode::Good){const auto text=log.str();require(std::count(text.begin(),text.end(),'\n')==8,"held unchanged input emitted repeated logs");for(unsigned ordinal:{1u,2u,4u,6u,7u,9u,10u,11u})require(text.find(" sample="+std::to_string(ordinal)+" ")!=std::string::npos,"changed sample ordinal absent");require(text.find("guest_us=100002 returned=1 buttons=0x0 x=128 y=0 mode=analog")!=std::string::npos,"forward sample metadata incorrect");require(text.find("output=0x48802000")!=std::string::npos&&text.find("read_kind=read-buffer-positive nid=0x1f803938")!=std::string::npos,"read kind/output provenance absent");require(!observer.output_failed(),"good sink failed");}else require(observer.output_failed()&&broken.attempts==1,"throwing sink was not isolated/latching");}
#endif
    if(sink_mode==SinkMode::Null)require(observer.successful_reads()==0&&observer.state_changes()==0&&log.str().empty(),"null observer called");
    // Destructor/constructor reset controller mode and sampling deadline only;
    // a fresh observer's initial sample does not fabricate an input edge.
    controller.reset();Observer fresh(sink);controller=std::make_unique<chili::ControllerRuntime>(rt,threads,provider);
#ifndef CONTROLLER_ORIGINAL
    if(sink_mode!=SinkMode::Null)controller->set_sampling_observer(&fresh);
#endif
    seed(c,11);c.gpr[4]=output;c.gpr[5]=1;auto wanted=c;wanted.set_gpr(2,1);const auto now=threads.now_us();call(0x1F803938);require(same_cpu(c,wanted)&&polls==12&&threads.now_us()==now&&m.load8(output+8)==128&&m.load8(output+9)==128,"recreated controller lifecycle changed");
#ifndef CONTROLLER_ORIGINAL
    if(sink_mode!=SinkMode::Null)require(fresh.successful_reads()==1&&fresh.state_changes()==1,"fresh observer did not observe actual initial read");
#endif
    const auto old_fresh_reads=fresh.successful_reads();invalid(output,65,0x80000104);require(fresh.successful_reads()==old_fresh_reads,"recreated controller invalid request emitted sample");const auto memory=whole_memory(m);const auto old_polls=polls;const auto old_reads=observer.successful_reads();seed(c,64);c.gpr[4]=output;c.gpr[5]=2;call(0x1F803938);require(rt.stopped()&&rt.stop_reason()=="Controller multi-sample ring reads require buffered sampling support"&&whole_memory(m)==memory&&polls==old_polls&&observer.successful_reads()==old_reads&&fresh.successful_reads()==old_fresh_reads,"strict unsupported ring read changed or emitted sample");
    result.memory=whole_memory(m);return result;
}
#ifndef CONTROLLER_ORIGINAL
void sink_contract(){std::ostringstream log;log<<std::hex;log.width(17);const auto flags=log.flags();Observer observer(log);Observer::Sample s;s.returned_count=1;s.output=output;s.ra=done;s.uid=1;s.guest_us=0;s.buttons=0x4000;observer.sampled(s);require(log.flags()==flags&&log.width()==17&&log.str().starts_with("[controller-sample]"),"enabled log failed flags/pending-width preservation");const auto before=log.str();s.guest_us=16667;s.output|=0x40000000;s.ra+=4;s.uid=2;observer.sampled(s);require(log.str()==before&&observer.successful_reads()==2&&observer.state_changes()==1,"metadata-only changes logged fake input state edge");ThrowingBuffer buffer;std::ostream fail(&buffer);fail<<std::hex;fail.width(13);fail.exceptions(std::ios::badbit|std::ios::failbit);const auto old=fail.flags();Observer failing(fail);failing.sampled(s);s.buttons=0;failing.sampled(s);require(failing.output_failed()&&buffer.attempts==1&&failing.successful_reads()==2&&failing.state_changes()==2&&fail.flags()==old&&fail.width()==13,"throwing observer changed formatting/retried/escaped");}
#endif
void write_result(const std::filesystem::path &path,const Result &r){std::ofstream file(path,std::ios::binary|std::ios::trunc);auto write=[&](const auto &v){const std::uint64_t n=v.size();file.write(reinterpret_cast<const char*>(&n),sizeof(n));file.write(reinterpret_cast<const char*>(v.data()),static_cast<std::streamsize>(v.size()*sizeof(v[0])));};write(r.trace);write(r.provider);write(r.memory);require(bool(file),"trace write failed");}
}
int main(int argc,char **argv){try{require(argc==2,"Expected fixture trace path");const auto path=std::filesystem::absolute(argv[1]).lexically_normal();const auto source=std::filesystem::path(__FILE__).parent_path();require(path.parent_path()==source/"build"||path.parent_path()==source.parent_path()/"analysis/chili-controller-sampling-fixtures","trace must stay in prep build or official analysis fixture root");const auto null=exercise(SinkMode::Null);
#ifndef CONTROLLER_ORIGINAL
    const auto enabled=exercise(SinkMode::Good),failed=exercise(SinkMode::Throwing);require(null==enabled&&null==failed,"null/enabled/failed observer whole trace/CPU/memory/provider/UID/time differs");sink_contract();
#endif
    write_result(path,null);std::cout<<"PASS "<<checks<<" passive controller assertions; real HLE/registered continuation with fixture provider, full CPU/RAM/VRAM/scratch/UID/time/provider parity; no host input/window/game\n";return 0;}catch(const std::exception &e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
