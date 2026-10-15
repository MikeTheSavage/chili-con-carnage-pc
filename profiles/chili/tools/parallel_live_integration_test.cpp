// Proposed source fixture only. Root must compile/run before qualification.
#include "ge_runtime.hpp"
#include <bit>
#include <cfenv>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace {
unsigned checks{};
void require(bool value,const char *message){++checks;if(!value)throw std::runtime_error(message);}
using Telemetry=chili::PerformanceTelemetry;
using Bytes=std::vector<std::uint8_t>;
bool same_cpu(const psprecomp::AllegrexContext &a,const psprecomp::AllegrexContext &b){
    return a.gpr==b.gpr&&a.pc==b.pc&&a.hi==b.hi&&a.lo==b.lo&&a.fcr31==b.fcr31&&a.vfpu_ctrl==b.vfpu_ctrl&&
        std::memcmp(a.fpr.data(),b.fpr.data(),sizeof(a.fpr))==0&&
        std::memcmp(a.vfpu.data(),b.vfpu.data(),sizeof(a.vfpu))==0;
}
class CoutRedirect {
public:
    explicit CoutRedirect(std::streambuf *buffer):previous_(std::cout.rdbuf(buffer)),flags_(std::cout.flags()),width_(std::cout.width()){}
    ~CoutRedirect(){std::cout.rdbuf(previous_);std::cout.flags(flags_);std::cout.width(width_);}
private:
    std::streambuf *previous_;std::ios::fmtflags flags_;std::streamsize width_;
};
enum class Case { Visible, Coincident, InvalidFragmentState, NoDraw };
struct Outcome {
    Bytes vram,ram;
    psprecomp::AllegrexContext cpu;
    std::array<unsigned,256> commands{};
    unsigned vertex{},index{},next_list{};
    std::unordered_set<unsigned> completed;
    std::string stop,log;
    chili::ParallelLiveCounts observation;
    Telemetry::Counters counters{};
    std::uint64_t time{},uid{},generation{};
    unsigned csr{};std::fenv_t fenv{};
};
Outcome native_case(Case test,bool attached){
    // Every invocation uses the actual enqueue HLE and current native renderer.
    // The null run is a caller-wiring comparator; V2's independent sequential
    // renderer oracle and broad ON/OFF/WATCH tests remain separate prerequisites.
    psprecomp::Runtime rt;chili::ThreadRuntime threads(rt);
    chili::ParallelLiveObserver observer;std::ostringstream telemetry_output,host_output;
    Telemetry telemetry(telemetry_output,Telemetry::Clock::time_point{});
    if(attached)telemetry.set_parallel_live_observer(&observer);
    chili::GeRuntime ge(rt,&threads,0,nullptr,&telemetry);
    if(attached)ge.set_parallel_live_observer(&observer);
    ge.commands[0x12]=test==Case::Coincident?0x19D:0x80019D;
    ge.commands[0xD2]=test==Case::InvalidFragmentState?0:3;
    ge.commands[0x9D]=128;ge.commands[0x9E]=0x80000;ge.commands[0x9F]=128;
    ge.commands[0xD5]=ge.commands[0x16]=95|(95<<10);ge.commands[0xD7]=65535;ge.commands[0x58]=255;
    ge.world=ge.view={1,0,0,0,1,0,0,0,1,0,0,0};
    ge.projection={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    ge.vertex_address=0x08804000;
    for(unsigned n=0;n<6;++n){
        const unsigned a=ge.vertex_address+n*20;
        const float x=test==Case::Coincident?0.0f:n%3==1?96.0f:0.0f;
        const float y=test==Case::Coincident?0.0f:n%3==2?96.0f:0.0f;
        rt.memory().store32(a,0);rt.memory().store32(a+4,n<3?0xC0875321u:0x91D56319u);
        rt.memory().store32(a+8,std::bit_cast<unsigned>(x));rt.memory().store32(a+12,std::bit_cast<unsigned>(y));
        rt.memory().store32(a+16,std::bit_cast<unsigned>(20.0f));
    }
    for(unsigned n=0;n<128*96;++n){rt.memory().store32(0x04000000+n*4,0xB1996655);rt.memory().store16(0x04080000+n*2,100);}
    const unsigned list=0x09F00000;
    unsigned cursor=list;
    if(test!=Case::NoDraw){rt.memory().store32(cursor,0x04030006);cursor+=4;}
    rt.memory().store32(cursor,0x0F000000);rt.memory().store32(cursor+4,0x0C000000);
    auto &cpu=rt.cpu();cpu.pc=0x08D98ED0;
    for(unsigned n=1;n<32;++n)cpu.gpr[n]=0x90000000+n;
    for(unsigned n=0;n<cpu.fpr.size();++n)cpu.fpr[n]=std::bit_cast<float>(0x7FC10000+n);
    for(unsigned n=0;n<cpu.vfpu.size();++n)cpu.vfpu[n]=std::bit_cast<float>(0xFFC10000+n);
    cpu.gpr[4]=list;cpu.gpr[5]=0;cpu.gpr[6]=0xFFFFFFFFu;cpu.gpr[7]=0;
    const auto ram=rt.memory().bytes(),image=rt.memory().vram_bytes();const auto before_cpu=cpu;
    const auto token=psprecomp::capture_runtime_execution_context();const auto time=threads.now_us();
    std::fenv_t saved{};require(std::fegetenv(&saved)==0,"save fixture FP environment");const auto saved_csr=_mm_getcsr();
    require(std::fesetenv(FE_DFL_ENV)==0,"fixture default FP environment");_mm_setcsr(_mm_getcsr()&~0x3Fu);
    {CoutRedirect redirect(host_output.rdbuf());rt.invoke_import("sceGe_user",0xAB49E76Au,cpu);}
    Outcome out;out.csr=_mm_getcsr();require(std::fegetenv(&out.fenv)==0,"capture renderer FP environment");
    (void)std::fesetenv(&saved);_mm_setcsr(saved_csr);
    require(rt.memory().bytes()==ram,"HLE observer changed RAM/source/list");
    require(psprecomp::runtime_execution_context_matches(token)&&threads.now_us()==time,"HLE observer changed UID/generation/time");
    const bool failed=test==Case::InvalidFragmentState;
    auto expected_cpu=before_cpu;if(!failed)expected_cpu.gpr[2]=1;
    require(same_cpu(cpu,expected_cpu),"HLE observer changed guest CPU beyond enqueue result");
    require(ge.completed.contains(1)==!failed&&ge.next_list==(failed?1u:2u),"strict failure or END publication changed");
    require(ge.vertex_address==0x08804000+(failed||test==Case::NoDraw?0u:120u)&&ge.index_address==0,"cursor transaction changed");
    require(rt.stopped()==failed,"strict draw stop changed");
    if(failed||test==Case::Coincident||test==Case::NoDraw)require(rt.memory().vram_bytes()==image,"zero-effect/failure draw wrote VRAM");
    else require(rt.memory().vram_bytes()!=image,"visible fixture has no writes");
    out.vram=rt.memory().vram_bytes();out.ram=rt.memory().bytes();out.cpu=cpu;out.commands=ge.commands;
    out.vertex=ge.vertex_address;out.index=ge.index_address;out.next_list=ge.next_list;out.completed=ge.completed;
    out.stop=rt.stop_reason();out.log=host_output.str();out.time=threads.now_us();out.uid=psprecomp::runtime_thread_uid();
    out.generation=psprecomp::runtime_thread_switch_generation();out.counters=telemetry.counters();out.observation=observer.snapshot();
    const bool observes=attached&&chili::kParallelShadeObservationCompiled;
    const bool raster=test==Case::Visible||failed;
    require(out.observation.raster_calls==(observes&&raster?1u:0u),"native raster denominator incorrect");
    require(out.observation.caller_coincident_draws==(observes&&test==Case::Coincident?1u:0u),"coincident denominator mixed with raster");
    if(observes&&test==Case::Visible){
        require(out.observation.routes[static_cast<std::size_t>(chili::ParallelShadeRoute::ParallelReady)]==1,"native visible route not observed");
        require(out.observation.known_commit_counts==1&&out.observation.committed_fragments==out.counters[Telemetry::TriangleCommittedFragments],"native written-count observation changed");
    }
    if(observes&&failed)require(out.observation.draw_failures==1&&out.observation.draw_successes==0&&out.observation.known_commit_counts==0,"failed draw silently counted success");
    if(!observes)require(out.observation==chili::ParallelLiveCounts{},"default/macro-OFF path recorded observations");
    return out;
}
class RejectParallelBuffer final:public std::stringbuf {
    std::streamsize xsputn(const char *text,std::streamsize length) override {
        if(std::string_view(text,static_cast<std::size_t>(length)).find("[parallel-shade]")!=std::string_view::npos)
            throw std::runtime_error("fixture rejects only outer parallel telemetry row");
        return std::stringbuf::xsputn(text,length);
    }
};
void telemetry_cases(){
    const auto epoch=Telemetry::Clock::time_point{};
    chili::ParallelLiveObserver observer;chili::ParallelShadeEvent event;
    event.route=chili::ParallelShadeRoute::ParallelReady;event.prepared_triangles=2;event.bbox_pixels=10000;
    event.attempted=event.pool_invoked=event.pool_completed=event.returned=event.rendered=event.committed_count_known=true;
    event.committed_fragments=8;observer.record(event);observer.note_caller_coincident();const auto counts=observer.snapshot();
    std::ostringstream null_output,active_output;Telemetry null_telemetry(null_output,epoch),active(active_output,epoch);
    active.set_parallel_live_observer(&observer);
    for(auto *stream:{&null_output,&active_output}){*stream<<std::hex<<std::boolalpha;stream->width(23);}
    const auto flags=active_output.flags();
    null_telemetry.poll(epoch+std::chrono::milliseconds(999),0);active.poll(epoch+std::chrono::milliseconds(999),0);
    require(null_output.str().empty()&&active_output.str().empty()&&active_output.width()==23,"observer changed not-due boundary");
    null_telemetry.poll(epoch+std::chrono::seconds(1),41);active.poll(epoch+std::chrono::seconds(1),41);
    const auto position=active_output.str().find("[parallel-shade]");
    require(position!=std::string::npos&&active_output.str().substr(0,position)==null_output.str(),"null perf output changed or existing row differs");
    require(active_output.flags()==flags&&active_output.width()==23&&null_output.width()==23,"success consumed caller flags/pending width");
    require(active_output.str().find("raster_calls=1")!=std::string::npos&&active_output.str().find("caller_coincident_draws=1")!=std::string::npos&&active_output.str().find("cumulative=1 saturated=0")!=std::string::npos,"cumulative separate integer output missing");
    const auto before_size=active_output.str().size();active.poll(epoch+std::chrono::milliseconds(1999),41);
    require(active_output.str().size()==before_size,"extra observer cadence introduced");
    active.poll(epoch+std::chrono::seconds(2),41,true);
    require(active_output.str().find("[parallel-shade]",position+1)!=std::string::npos&&observer.snapshot()==counts,"forced exit snapshot reset or mutated observer");
    require(!active.output_failed()&&active.counters()==null_telemetry.counters(),"healthy observation affects old counters");
    RejectParallelBuffer buffer;std::ostream failing(&buffer);failing.exceptions(std::ios::badbit|std::ios::failbit);
    failing<<std::hex<<std::boolalpha;failing.width(17);const auto fail_flags=failing.flags();
    Telemetry failed(failing,epoch);failed.set_parallel_live_observer(&observer);failed.poll(epoch+std::chrono::seconds(1),0);
    require(failed.output_failed()&&failing.bad(),"parallel row sink failure not contained");
    require(buffer.str().starts_with("[perf]")&&buffer.str().find("[parallel-shade]")==std::string::npos,"throwing fixture did not reach separate parallel row");
    require(failing.flags()==fail_flags&&failing.width()==17&&observer.snapshot()==counts,"sink exception changed formatting or observer");
}
}
int main(){try{
    require(std::getenv("PSPRECOMP_WATCH_WRITE")==nullptr,"integration fixture requires unwatched process; V2 separately qualifies WATCH");
    for(auto test:{Case::Visible,Case::Coincident,Case::InvalidFragmentState,Case::NoDraw}){
        const auto baseline=native_case(test,false),observed=native_case(test,true);
        require(baseline.vram==observed.vram&&baseline.ram==observed.ram&&same_cpu(baseline.cpu,observed.cpu),"native null/attached full memory/CPU differs");
        require(baseline.commands==observed.commands&&baseline.vertex==observed.vertex&&baseline.index==observed.index&&baseline.next_list==observed.next_list&&baseline.completed==observed.completed,"native command/cursor/END effects differ");
        require(baseline.stop==observed.stop&&baseline.log==observed.log&&baseline.time==observed.time,"native error/log/deadline differs");
        require(baseline.csr==observed.csr&&baseline.fenv._Fe_ctl==observed.fenv._Fe_ctl&&baseline.fenv._Fe_stat==observed.fenv._Fe_stat,"native FP result differs");
        // Clock durations are intentionally excluded; integer events must match.
        for(std::size_t i=0;i<Telemetry::Count;++i)if(i!=Telemetry::DrawWallNs&&i!=Telemetry::GeListWallNs)
            require(baseline.counters[i]==observed.counters[i],"old integer telemetry differs");
    }
    telemetry_cases();std::cout<<"PASS "<<checks<<" native caller/outer telemetry checks; compiled="<<unsigned(chili::kParallelShadeObservationCompiled)<<"; no speed/input/game acceptance\n";return 0;
}catch(const std::exception &error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}}
