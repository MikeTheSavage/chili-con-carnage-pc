#include "../host/atrac_runtime.hpp"
#include "../host/av_runtime.hpp"
#include "../host/thread_runtime.hpp"
#include "vcs_media_decoder.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
namespace {
constexpr unsigned buffer=0x08810000,pcm=0x08900000,counts=0x08802000,info=0x08802100,done=0x08000104;
void require(bool yes,const char *message){if(!yes)throw std::runtime_error(message);}
struct Fixture {
    psprecomp::Runtime rt;chili::AvRuntime av{rt};chili::ThreadRuntime threads{rt};chili::AtracRuntime atrac{rt,av,&threads};
    unsigned deferred{},busy_checks{};
    Fixture(){rt.cpu().gpr[29]=0x09FFF000;threads.initialize_bootstrap();rt.register_function(done,[](auto &r,auto &){r.stop("fixture-return");},"fixture_done");}
    unsigned call(unsigned nid,std::initializer_list<unsigned> arguments={}) {
        auto &c=rt.cpu();c.pc=0;c.gpr[31]=done;for(unsigned n=4;n<=10;++n)c.gpr[n]=0;
        unsigned n=4;for(auto a:arguments)c.gpr[n++]=a;auto expected=c;expected.pc=done;
        const auto before=threads.now_us();rt.invoke_import("sceAtrac3plus",nid,c);
        if(c.pc) {
            require(c.pc==chili::AtracRuntime::decode_resume_trampoline,"Unexpected ATRAC continuation");++deferred;
            auto other=expected;rt.invoke_import("sceAtrac3plus",0x61EB33F5,other);
            require(rt.stop_reason().find("decode return is pending")!=std::string::npos,"Pending ATRAC ID release was accepted");++busy_checks;
            rt.run(c.pc,100);require(rt.stop_reason()=="fixture-return","ATRAC continuation did not return to caller");
            expected.gpr[2]=c.gpr[2];require(c.gpr==expected.gpr && c.fpr==expected.fpr && c.vfpu==expected.vfpu && c.vfpu_ctrl==expected.vfpu_ctrl &&
                c.hi==expected.hi && c.lo==expected.lo && c.fcr31==expected.fcr31,"ATRAC scheduler delay lost caller CPU context");
            require(threads.now_us()>=before+2300,"ATRAC decode did not yield for 2300us");
        }
        return c.gpr[2];
    }
    void module(unsigned id,bool load){rt.cpu().gpr[4]=id;rt.invoke_import("sceUtility",load?0xC629AF26:0xF7D8D092,rt.cpu());}
    void load(){module(0,true);require(av.loaded(0),"Codec module load failed");module(2,true);require(av.loaded(2),"ATRAC module load failed");}
};
}
int main(int argc,char **argv){try {
    require(argc==2,"Usage: chili_atrac_runtime_stream_test <owned SND0.AT3>");
    std::ifstream file(argv[1],std::ios::binary);require(bool(file),"Cannot open owned media");
    const std::vector<std::uint8_t> source{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
    vcs::AudioStreamDecoder reference;require(reference.open(argv[1],44100,2,0),"Reference WAV decode unavailable");
    std::vector<std::uint8_t> expected,chunk(8192);
    for(;;){const auto n=reference.read(chunk);expected.insert(expected.end(),chunk.begin(),chunk.begin()+n);if(n<chunk.size())break;}
    constexpr unsigned trim=3874*4;require(expected.size()>trim,"Reference media too short");expected.erase(expected.begin(),expected.begin()+trim);expected.resize(1366238*4);
    Fixture f;auto &m=f.rt.memory();auto &c=f.rt.cpu();
    require(f.call(0x780F88D1,{123})==0x80630004,"Invalid codec ID accepted");f.call(0x780F88D1,{0x1000});
    require(f.rt.stopped() && f.rt.stop_reason().find("loaded ATRAC module")!=std::string::npos,"Disabled ATRAC allocation did not stop");
    f.load();const auto id=f.call(0x780F88D1,{0x1000});require(id==0 && f.call(0x780F88D1,{0x1000})==1 && f.call(0x780F88D1,{0x1000})==0x80630003,"ATRAC ID reservation/exhaustion failed");
    m.store32(counts,0xDEADBEEF);require(f.call(0x9AE849A7,{id,counts})==0x80630010 && m.load32(counts)==0xDEADBEEF,"Unset stream changed remaining output");
    require(f.call(0x61EB33F5,{1})==0,"Spare ID release failed");
    f.module(2,false);require(f.av.loaded(2) && f.rt.stop_reason().find("retains state")!=std::string::npos,"Allocated ATRAC ID was unloaded");
    m.copy_in(buffer,std::span<const std::uint8_t>(source).first(500));
    require(f.call(0x3F6E26B5,{id,buffer,500,1540})==0,"Native halfway-buffer attach failed");
    c.fpr.fill(1.25f);c.vfpu.fill(-3.5f);c.vfpu_ctrl.fill(0xE4);c.gpr[28]=0x08876540;c.gpr[16]=0x123ABC;c.hi=0xC0DE;c.lo=0xF00D;c.fcr31=0x123;
    std::vector<std::uint8_t> actual;unsigned refills{};
    for(unsigned attempt=0;attempt<5000;++attempt) {
        const auto result=f.call(0x6A8C3CD5,{id,pcm,counts,counts+4,counts+8});
        if(result==0x80630023) {
            require(f.call(0x5D268707,{id,info,info+4,info+8})==0,"Native stream data info failed");
            const auto count=m.load32(info+4),offset=m.load32(info+8);require(count && std::uint64_t(offset)+count<=source.size(),"Guest stream did not progress");
            m.copy_in(m.load32(info),std::span<const std::uint8_t>(source).subspan(offset,count));
            require(f.call(0x7DB31251,{id,count})==0,"Native guest stream data add failed");++refills;continue;
        }
        require(result==0,"Native streamed ATRAC decode failed");const auto bytes=m.load32(counts)*4;
        const auto old=actual.size();actual.resize(old+bytes);m.copy_out(pcm,std::span<std::uint8_t>(actual).subspan(old));
        require(f.call(0x9AE849A7,{id,info+12})==0 && m.load32(info+12)==m.load32(counts+8),"Bound remaining frames disagree with decoder");
        if(m.load32(counts+4)){require(f.call(0xE23E3A35,{id,info})==0x80630024,"Bound decode position did not report end");break;}
        require(f.call(0xE23E3A35,{id,info})==0 && m.load32(info)==actual.size()/4,"Bound position does not match consumed samples");
    }
    require(actual==expected && actual.size()==1366238*4,"Native HLE PCM differs from owned reference stream");
    require(f.call(0x2DD3E298,{id,123456,info})==0,"Bound reset info failed");
    for(unsigned i=16;i<32;i+=4)require(m.load32(info+i)==0,"Bound reset invented a second buffer");
    const auto min=m.load32(info+8),offset=m.load32(info+12);m.copy_in(m.load32(info),std::span<const std::uint8_t>(source).subspan(offset,min));
    require(f.call(0x644E5607,{id,123456,min,4})==0x80630017 && f.call(0x644E5607,{id,123456,min,0})==0,"Bound second-buffer/reset behavior incorrect");
    require(f.call(0x6A8C3CD5,{id,pcm,counts,counts+4,counts+8})==0,"Bound reset decode failed");
    std::vector<std::uint8_t> reset(m.load32(counts)*4);m.copy_out(pcm,reset);require(std::equal(reset.begin(),reset.end(),expected.begin()+123456*4),"Bound reset returned wrong real samples");
    require(f.call(0x2DD3E298,{id,0,info})==0,"Corruption reset info failed");
    const auto corrupted_size=m.load32(info+8);std::vector<std::uint8_t> corrupt(corrupted_size,0xFF),sentinel(8192,0xA5);m.copy_in(m.load32(info),corrupt);m.copy_in(pcm,sentinel);
    require(f.call(0x644E5607,{id,0,corrupted_size,0})==0 && f.call(0x6A8C3CD5,{id,pcm,counts,counts+4,counts+8})==0x80630002,"Bound corrupt decode succeeded");
    std::vector<std::uint8_t> untouched(8192);m.copy_out(pcm,untouched);require(untouched==sentinel,"Bound failed decode inserted audio");
    require(f.call(0xE88F759B,{id,info})==0 && static_cast<std::int32_t>(m.load32(info))<0,"Bound native error info missing");
    require(f.call(0x61EB33F5,{id})==0 && f.call(0x61EB33F5,{id})==0x80630005,"Bound release/double-release incorrect");
    f.module(2,false);require(!f.av.loaded(2),"Clean ATRAC unload failed");f.module(2,true);require(f.av.loaded(2),"Clean ATRAC reload failed");
    const auto again=f.call(0x780F88D1,{0x1000});require(again==id && f.call(0x61EB33F5,{again})==0,"Freed ID did not reinitialize cleanly");f.module(2,false);f.module(0,false);
    std::cout<<"PASS native ATRAC HLE: all 11 imported handlers; 1366238 reference-equal real samples, "<<refills<<" guest refills, "<<f.deferred
        <<" 2300us decode continuations/full CPU restoration, "<<f.busy_checks<<" pending-release guards, reset/second-buffer/corrupt PCM/unload/reload verified\n";
}catch(const std::exception &e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
