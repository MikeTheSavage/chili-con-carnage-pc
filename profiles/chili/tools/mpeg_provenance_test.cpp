// The owned ICON1.PMF path is supplied at runtime; no media is embedded.
#include "mpeg_runtime.hpp"
#include "mpeg_provenance.hpp"
#include "av_runtime.hpp"
#include "thread_runtime.hpp"
#ifdef CHILI_MPEG_BASELINE_FIXTURE
#include "baseline_mpeg_runtime.hpp"
#endif
#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;
unsigned assertions{};
void require(bool good, const std::string &what) { ++assertions; if (!good) throw std::runtime_error(what); }
template<class T, std::size_t N> bool same(const std::array<T,N> &a, const std::array<T,N> &b) {
    return std::memcmp(a.data(),b.data(),sizeof(T)*N)==0;
}
bool same(const psprecomp::AllegrexContext &a,const psprecomp::AllegrexContext &b) {
    return same(a.gpr,b.gpr) && same(a.fpr,b.fpr) && same(a.vfpu,b.vfpu) && same(a.vfpu_ctrl,b.vfpu_ctrl) &&
        a.hi==b.hi && a.lo==b.lo && a.pc==b.pc && a.fcr31==b.fcr31;
}
Bytes read(const std::filesystem::path &path) {
    std::ifstream in(path,std::ios::binary);require(bool(in),"Cannot open owned fixture");
    return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}
unsigned be32(const Bytes &b,unsigned p) {
    require(p+4<=b.size(),"PSMF header truncated");
    return unsigned(b[p])<<24|unsigned(b[p+1])<<16|unsigned(b[p+2])<<8|b[p+3];
}
struct CaptureStdout {
    std::ostringstream data; std::streambuf *previous=std::cout.rdbuf(data.rdbuf());
    ~CaptureStdout(){std::cout.rdbuf(previous);}
};
constexpr unsigned ring=0x08801000,context=0x08801100,header=0x08802000,au=0x08804000;
constexpr unsigned status=0x08804100,pointer=0x08804104,detail=0x08804110;
constexpr unsigned work=0x08810000,ring_bytes=0x08830000,image=0x08900000;
constexpr unsigned callback=0x08000100,done=0x08000104,gp=0x089ABCDE;
struct Event {
    unsigned nid{}; psprecomp::AllegrexContext cpu;
    std::uint64_t guest_us{},switches{}; int uid{};
};
struct Outcome { std::vector<Event> events; Bytes frames,ram,vram,scratch; unsigned written{},drained{}; std::string log; };

template<class Mpeg> struct Fixture {
    psprecomp::Runtime runtime; chili::AvRuntime av{runtime}; chili::ThreadRuntime threads{runtime};
    Mpeg mpeg{runtime,av,&threads}; Bytes source; std::size_t cursor{};
    std::uint64_t initial_switches{};
    std::vector<Event> events;
    inline static Fixture *active{};
    explicit Fixture(const Bytes &file) {
        active=this;
        runtime.cpu().gpr[29]=0x09FFF000; threads.initialize_bootstrap();
        initial_switches=psprecomp::runtime_thread_switch_generation();
        runtime.register_function(callback,&supply,"provenance_owned_ring");
        runtime.register_function(done,[](auto &r,auto &){r.stop("fixture-return");},"provenance_fixture_done");
        for(unsigned module:{0u,3u}) {
            runtime.cpu().gpr[4]=module; runtime.invoke_import("sceUtility",0xC629AF26,runtime.cpu());
            require(runtime.cpu().gpr[2]==0,"AV module load failed");
        }
        require(call(0x682A619B)==0,"MPEG Init failed");
        require(file.size()>=2048,"Owned PSMF too small");
        runtime.memory().copy_in(header,std::span<const std::uint8_t>(file).first(2048));
        const auto offset=be32(file,8),size=be32(file,12);
        require(std::uint64_t(offset)+size<=file.size(),"Owned PSMF stream exceeds file");
        source.assign(file.begin()+offset,file.begin()+offset+size);source.resize((source.size()+2047)&~2047u);
        require(call(0x37295ED8,{ring,5,ring_bytes,5*2152,callback,0x12345678})==0,"Ring construct failed");
        require(call(0xD8C5F121,{context,work,0x10000,ring,160,0,0})==0,"MPEG Create failed");
        require(call(0x21FF80E4,{context,header,status})==0,"PSMF query failed");
        runtime.memory().store32(pointer,image);
    }
    ~Fixture(){active=nullptr;}
    static void supply(psprecomp::Runtime &r,psprecomp::AllegrexContext &c) {
        auto &f=*active;require(c.gpr[28]==gp && c.gpr[6]==0x12345678,"Callback ABI/GP changed");
        const auto count=std::min<std::size_t>(c.gpr[5],(f.source.size()-f.cursor)/2048);
        r.memory().copy_in(c.gpr[4],std::span<const std::uint8_t>(f.source).subspan(f.cursor,count*2048));
        f.cursor+=count*2048;
        const auto ra=c.gpr[31]; c.gpr.fill(0xDEADBEEF);c.fpr.fill(-9.5f);c.vfpu.fill(8.5f);c.vfpu_ctrl.fill(0xCC);
        c.hi=0xC0DE;c.lo=0xD00D;c.fcr31=0xE4;c.gpr[2]=static_cast<unsigned>(count);c.pc=ra;
    }
    unsigned call(unsigned nid,std::initializer_list<unsigned> args={}) {
        auto &c=runtime.cpu();
        for(unsigned i=1;i<32;++i)c.gpr[i]=0xA0000000u+i;
        c.gpr[28]=gp;c.gpr[29]=0x09FFF000;c.gpr[31]=done;c.pc=0;
        for(unsigned i=0;i<32;++i)c.set_fpr_bits(i,0x7FC00000u+i);
        for(unsigned i=0;i<128;++i)c.vfpu[i]=std::bit_cast<float>(0x7FC10000u+i);
        c.vfpu_ctrl.fill(0xABCD);c.hi=0xCAFEBABE;c.lo=0xDEAD0123;c.fcr31=0x123;
        for(unsigned i=4;i<=10;++i)c.gpr[i]=0;
        unsigned i=4;for(auto arg:args)c.gpr[i++]=arg;
        auto expected=c;
        runtime.invoke_import("sceMpeg",nid,c);
        if(c.pc){runtime.run(c.pc,100);require(runtime.stop_reason()=="fixture-return","Unexpected scheduler continuation stop");expected.pc=done;}
        expected.gpr[2]=c.gpr[2];require(same(c,expected),"MPEG changed full caller context/NaN bits");
        events.push_back({nid,c,threads.now_us(),psprecomp::runtime_thread_switch_generation()-initial_switches,threads.current_uid()});
        return c.gpr[2];
    }
    unsigned refill() {
        const auto free=call(0xB5F6DC87,{ring});
        return call(0xB240A59E,{ring,free,free});
    }
};

template<class Mpeg> Outcome run_video(const Bytes &file,bool diagnostic,bool drain_fixture=false) {
    require(_putenv_s("PSPRECOMP_MPEG_PROVENANCE",diagnostic?"1":"")==0,"Cannot set fixture diagnostic opt-in");
    CaptureStdout captured;
    Fixture<Mpeg> f(file);auto &m=f.runtime.memory();
    if(drain_fixture){
        // Encoded video is unchanged. A deliberately undersized declared width
        // makes the first real decoded frame fail output preflight and remain
        // queued; a valid Stop must then copy those genuine buffered pixels.
        unsigned descriptor{};
        const auto count=unsigned(file.at(0x80))*256u+file.at(0x81);
        for(unsigned n=0;n<count;++n)if((file.at(0x82+n*16)&0xF0u)==0xE0u){descriptor=0x82+n*16;break;}
        require(descriptor && file.at(descriptor+12)==9,"Owned fixture width descriptor differs");
        m.store8(header+descriptor+12,8);
        require(f.call(0x21FF80E4,{context,header,status})==0,"Drain fixture header analyze failed");
    }
    const auto stream=f.call(0x42560F23,{context,0,0});
    const auto es=f.call(0xA780CF7E,{context});
    require(es==1 && f.call(0x167AFD9E,{context,es,au})==0,"Video AU setup failed");
    constexpr unsigned stride=160,width=144,height=80;
    Bytes sentinel(stride*height*4,0xA5),output(sentinel.size());Outcome result;
    auto consume=[&](bool drain) {
        if(!m.load32(status))return;
        m.copy_out(image,output);result.frames.insert(result.frames.end(),output.begin(),output.end());
        for(unsigned y=0;y<height;++y)
            require(std::all_of(output.begin()+y*stride*4+width*4,output.begin()+(y+1)*stride*4,
                [](auto v){return v==0xA5;}),"Video write crossed stride padding");
        ++result.written;if(drain)++result.drained;
    };
    bool output_rejected=false;
    for(unsigned n=0;n<2000;++n){
        const auto got=f.call(0xFE246728,{context,stream,au,status});
        if(got==0x80618001u){if(f.cursor<f.source.size()){require(f.refill()>0,"Ring refill made no progress");continue;}break;}
        require(got==0,"Owned H264 AU failed");m.copy_in(image,sentinel);m.store32(status,0x777);
        const auto decoded=f.call(0x0E3C2E9D,{context,au,drain_fixture?128u:stride,pointer,status});
        if(drain_fixture && decoded==0x80610103u){
            m.copy_out(image,output);require(output==sentinel && m.load32(status)==0x777,"Rejected output changed guest pixels/status");
            output_rejected=true;break;
        }
        require(decoded==0,"Owned H264 decode failed");
        if(drain_fixture)require(m.load32(status)==0,"Undersized stride unexpectedly wrote pixels");
        else consume(false);
    }
    // Last buffered frames must be emitted by Stop, not counted as blank output.
    for(unsigned n=0;n<32;++n){m.copy_in(image,sentinel);require(f.call(0x740FCCD1,{context,stride,pointer,status})==0,"H264 Stop/drain failed");
        if(!m.load32(status))break;consume(true);}
    if(drain_fixture)require(output_rejected && result.written>0 && result.drained==result.written,"Native queued real video did not drain to valid output");
    else require(result.written>100,"Owned fixture did not exercise successful video writes");
    require(f.call(0x0F6C18D7,{context,detail})==0 && m.load32(detail+4)==result.written,"Guest frame count diverged");
    for(unsigned n=0;n<20;++n)require(f.call(0x740FCCD1,{context,stride,pointer,status})==0 && !m.load32(status),"Repeated empty Stop changed behavior");
    require(f.call(0x4571CC64,{context})==0,"Flush failed");
    require(f.call(0x0F6C18D7,{context,detail})==0 && m.load32(detail+4)==0,"Flush did not reset guest counters");
    // Replacing a previously analyzed header is an actual reset without byte dumps.
    m.store8(header+0x54,m.load8(header+0x54)^1u);
    require(f.call(0x21FF80E4,{context,header,status})!=0,"Malformed header unexpectedly succeeded");
    m.store8(header+0x54,m.load8(header+0x54)^1u);
    m.store8(header+7,'5');
    require(f.call(0x21FF80E4,{context,header,status})==0,"Valid replacement header failed");
    require(f.call(0xCEB870B1,{context,es})==0 && f.call(0x591A4AA2,{context,stream})==0,"Stream release failed");
    require(f.call(0x606A4649,{context})==0 && f.call(0x13407F13,{ring})==0 && f.call(0x874624D6)==0,"Native lifecycle cleanup failed");
    result.events=std::move(f.events);
    result.ram.resize(m.size());m.copy_out(psprecomp::GuestMemory::kPhysicalBase,result.ram);
    result.vram.resize(psprecomp::GuestMemory::kVramSize);m.copy_out(psprecomp::GuestMemory::kVramPhysicalBase,result.vram);
    result.scratch.resize(psprecomp::GuestMemory::kScratchpadSize);m.copy_out(psprecomp::GuestMemory::kScratchpadBase,result.scratch);
    result.log=captured.data.str();return result;
}
void compare(const Outcome &a,const Outcome &b,const char *label) {
    require(a.frames==b.frames && a.ram==b.ram && a.vram==b.vram && a.scratch==b.scratch,std::string(label)+" video/all guest bytes differ");
    require(a.written==b.written && a.drained==b.drained && a.events.size()==b.events.size(),std::string(label)+" result counts differ");
    for(std::size_t i=0;i<a.events.size();++i){const auto &x=a.events[i],&y=b.events[i];
        require(x.nid==y.nid && same(x.cpu,y.cpu) && x.guest_us==y.guest_us && x.switches==y.switches && x.uid==y.uid,
            std::string(label)+" full CPU/UID/time/switch parity failed");}
}
std::vector<std::string> diagnostic_lines(const std::string &text) {
    std::vector<std::string> lines;std::istringstream in(text);std::string line;
    while(std::getline(in,line))if(line.starts_with("[mpeg-video]"))lines.push_back(line);
    return lines;
}
void observer_test(){
    std::ostringstream text;text<<std::hex;text.width(9);const auto flags=text.flags();const auto width=text.width();
    chili::MpegProvenance observer(text);chili::MpegProvenance::Context c;observer.created(c,0x08801100,0x08810030);
    require(text.flags()==flags && text.width()==width,"Observer changed ostream formatting");
    for(unsigned i=1;i<=5000;++i){observer.decoded(c,1);observer.written(c,{0x08900000,512,480,272,0x088328AC,6,i*5400ull,i==1?chili::MpegProvenance::unknown_pts:i*3003ull,false});}
    require(c.decoded==5000 && c.written==5000,"Observer raw counters lost unsampled frames");
    observer.drained(c,{});observer.drained(c,{});observer.summary(c,"flush",{});observer.next_segment(c);
    require(c.segment==2 && c.segment_written==0 && c.written==5000,"Observer segment reset changed lifetime totals");
    observer.decoded(c,2);observer.written(c,{0x04000000,480,480,272,0x088328AC,6,27005400,1,true});
    observer.summary(c,"delete",{1,2,2048,1,1});observer.finished(0);
    const auto lines=diagnostic_lines(text.str());
    require(lines.size()==50,"Sparse observer emitted an unexpected number of lines");
    require(text.str().find("segment_written=4920")!=std::string::npos,"Diagnostic exhausted at startup4096 boundary");
    require(text.str().find("decoded_frame_pts90k=unknown")!=std::string::npos,"Unknown PTS was synthesized");
    require(text.str().find("global_decoded=5002 global_written=5001 retained_rings=0")!=std::string::npos,"Lifetime totals reset/lost queued decode");
    require(text.str().find("stride_pixels=512 stride_bytes=2048 format=RGBA8888 width=480 height=272 written_bytes=522240")!=std::string::npos,
        "Observer destination/stride/format footprint fields incorrect");
    struct Broken:std::streambuf{std::streamsize xsputn(const char *,std::streamsize)override{throw std::runtime_error("sink failure");}} buffer;
    std::ostream failure(&buffer);failure.exceptions(std::ios::badbit|std::ios::failbit);chili::MpegProvenance bad(failure);chili::MpegProvenance::Context d;
    bad.created(d,1,2);bad.decoded(d,1);bad.written(d,{});bad.finished(0);
    require(bad.output_failed() && d.written==1 && d.decoded==1,"Failing diagnostic sink escaped/disabled raw counters");
    std::cout<<"PASS observer: 5000 writes sampled beyond4096, segment/lifetime/unknownPTS/stride/failing-output semantics\n";
}
}
int main(int argc,char **argv){try {
    require(argc==2,"Usage: chili_mpeg_provenance_test <owned ICON1.PMF>");
    require(_putenv_s("PSPRECOMP_MPEG_TRACE","")==0 && _putenv_s("PSPRECOMP_MPEG_STOP_ON_VIDEO_ERROR","")==0,"Cannot clear unrelated diagnostics");
    observer_test();const auto file=read(argv[1]);
    const auto disabled=run_video<chili::MpegRuntime>(file,false);
    const auto enabled=run_video<chili::MpegRuntime>(file,true);compare(disabled,enabled,"enabled/disabled");
    const auto drain_disabled=run_video<chili::MpegRuntime>(file,false,true);
    const auto drain_enabled=run_video<chili::MpegRuntime>(file,true,true);compare(drain_disabled,drain_enabled,"Stop enabled/disabled");
    const auto lines=diagnostic_lines(enabled.log);
    require(diagnostic_lines(disabled.log).empty(),"Default path emitted provenance");
    require(lines.size()==6u+std::min(enabled.written,3u)+enabled.written/120u,"Actual owned movie diagnostic line count incorrect");
    require(enabled.log.find("destination=143654912 stride_pixels=160 stride_bytes=640 format=RGBA8888 width=144 height=80")!=std::string::npos,"Resolved guest destination/dimensions absent");
    const auto total="decoded="+std::to_string(enabled.written)+" written="+std::to_string(enabled.written);
    require(enabled.log.find("event=drain-empty")!=std::string::npos && enabled.log.find(total)!=std::string::npos &&
        enabled.log.find("event=flush")!=std::string::npos && enabled.log.find("event=delete")!=std::string::npos &&
        enabled.log.find("event=finish")!=std::string::npos && enabled.log.find("event=header-replaced")!=std::string::npos,
        "Successful output/drain/reset/lifecycle evidence incomplete");
#ifdef CHILI_MPEG_BASELINE_FIXTURE
    const auto baseline=run_video<chili_mpeg_baseline::MpegRuntime>(file,false);compare(baseline,disabled,"unchanged baseline/prepared");
    const auto drain_baseline=run_video<chili_mpeg_baseline::MpegRuntime>(file,false,true);compare(drain_baseline,drain_disabled,"unchanged Stop baseline/prepared");
    std::cout<<"PASS unchanged source baseline parity: guest video/full RAM/VRAM/scratchpad/full CPU NaN bits/UID/time/switches\n";
#endif
    require(_putenv_s("PSPRECOMP_MPEG_PROVENANCE","")==0,"Cannot restore diagnostic opt-in");
    std::cout<<"PASS runtime: "<<enabled.written<<" owned H264 writes, "<<enabled.drained<<" Stop writes, "<<enabled.events.size()
        <<" ABI events, "<<lines.size()<<" diagnostic lines; enabled/disabled guest bytes/full context/time equal\n";
    for(const auto &line:lines)std::cout<<line<<'\n';
    require(drain_enabled.log.find("drain=1")!=std::string::npos,"Successful Stop write provenance absent");
    std::cout<<"PASS Stop output: "<<drain_enabled.drained<<" genuine owned buffered frames after strict undersized-stride rejection; baseline/default/opt-in parity\n";
    for(const auto &line:diagnostic_lines(drain_enabled.log))std::cout<<line<<'\n';
    std::cout<<"PASS: "<<assertions<<" assertions\n";
}catch(const std::exception &error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}}
