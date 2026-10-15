#include "../host/event_flags.hpp"
#include <iostream>
#include <stdexcept>
namespace {
constexpr auto name=0x08802000u, out=name+64u, timeout=out+4u;
std::uint32_t uid; unsigned action, sequence;
void check(bool ok,const char *text) { if(!ok) throw std::runtime_error(text); }
void call(psprecomp::Runtime &rt,std::uint32_t nid) { rt.invoke_import("ThreadManForUser",nid,rt.cpu()); }
std::uint32_t create(psprecomp::Runtime &rt,std::uint32_t bits=0u) {
    rt.memory().store32(name,0x00667665u);
    auto &c=rt.cpu(); c.gpr[4]=name;c.gpr[5]=0x200u;c.gpr[6]=bits;c.gpr[7]=0u;
    call(rt,0x55C20A00u); check(static_cast<std::int32_t>(c.gpr[2])>0,"create failed");return c.gpr[2];
}
void wait(psprecomp::Runtime &rt,std::uint32_t bits,std::uint32_t mode,bool poll,std::uint32_t timer=0u) {
    auto &c=rt.cpu();c.gpr[4]=uid;c.gpr[5]=bits;c.gpr[6]=mode;c.gpr[7]=out;c.gpr[8]=timer;
    call(rt,poll?0x30FD48F0u:0x402FCF22u);
}
void basic() {
    psprecomp::Runtime rt;chili::ThreadRuntime threads(rt);threads.initialize_bootstrap();chili::EventFlags events(rt,threads);
    uid=create(rt,7u); wait(rt,3u,0x20u,true);
    check(rt.cpu().gpr[2]==0 && rt.memory().load32(out)==7u,"AND/CLEAR did not return original pattern");
    wait(rt,3u,0u,true);check(rt.cpu().gpr[2]==0x800201AFu && rt.memory().load32(out)==4u,"failed poll pattern mismatch");
    wait(rt,6u,0x11u,true);check(rt.cpu().gpr[2]==0 && rt.memory().load32(out)==4u,"OR/CLEARALL failed");
    wait(rt,1u,0x30u,true);check(rt.cpu().gpr[2]==0x80020195u,"invalid mode accepted");
    wait(rt,0u,0u,true);check(rt.cpu().gpr[2]==0x800201B1u,"zero pattern accepted");
    rt.cpu().gpr[4]=uid;rt.cpu().gpr[5]=15u;call(rt,0x1FB15A32u);
    rt.cpu().gpr[4]=uid;rt.cpu().gpr[5]=5u;call(rt,0x812346E4u);
    wait(rt,5u,0u,true);check(rt.cpu().gpr[2]==0 && rt.memory().load32(out)==5u,"clear did not retain mask");
    rt.memory().store32(timeout,100u);rt.cpu().gpr[31]=0x08804000u;wait(rt,8u,0u,false,timeout);
    check(rt.cpu().gpr[2]==0x800201A8u && rt.memory().load32(timeout)==0u && threads.now_us()==100u,"timeout failed");
    const auto info=out+64u;rt.memory().store32(info,52u);rt.cpu().gpr[4]=uid;rt.cpu().gpr[5]=info;call(rt,0xA66B0120u);
    check(rt.memory().load32(info+40)==7u && rt.memory().load32(info+44)==5u && rt.memory().load32(info+48)==0u,"status failed");
    rt.cpu().gpr[4]=uid;call(rt,0xEF9E4C70u);wait(rt,1u,0u,true);check(rt.cpu().gpr[2]==0x8002019Au,"deleted flag survived");
}
void resume(psprecomp::Runtime &rt,psprecomp::AllegrexContext &c) {
    check(c.gpr[2]==(action==0?0u:action==1?0x800201A9u:0x800201B5u),"wake result failed");
    check(sequence==1u,"priority wake did not preempt");++sequence;
    if(!action) check(rt.memory().load32(out)==3u,"wake pattern failed");
    c.pc=chili::ThreadRuntime::return_trampoline;
}
void child(psprecomp::Runtime &rt,psprecomp::AllegrexContext &c) { c.gpr[31]=0x08804200u;wait(rt,3u,0x20u,false); }
void parent_done(psprecomp::Runtime &,psprecomp::AllegrexContext &c) { check(sequence==2u,"parent continuation lost");++sequence;c.pc=chili::ThreadRuntime::return_trampoline; }
void release(psprecomp::Runtime &rt,psprecomp::AllegrexContext &c) {
    sequence=1u;c.gpr[31]=0x08804400u;c.gpr[4]=uid;c.gpr[5]=3u;c.gpr[6]=0u;
    call(rt,action==0?0x1FB15A32u:action==1?0xCD203292u:0xEF9E4C70u);
}
void handoff(unsigned value) {
    action=value;sequence=0;psprecomp::Runtime rt;chili::ThreadRuntime threads(rt);
    rt.cpu().gpr[31]=0x08804300u;threads.initialize_bootstrap();chili::EventFlags events(rt,threads);uid=create(rt);
    rt.register_function(0x08804100u,child,"event_child");rt.register_function(0x08804200u,resume,"event_resume");
    rt.register_function(0x08804300u,release,"event_release");rt.register_function(0x08804400u,parent_done,"event_done");
    auto &c=rt.cpu();c.gpr[4]=name;c.gpr[5]=0x08804100u;c.gpr[6]=0x10u;c.gpr[7]=0x1000u;c.gpr[8]=0u;call(rt,0x446D8DE6u);
    c.gpr[4]=c.gpr[2];c.gpr[5]=0;c.gpr[6]=0;call(rt,0xF475845Du);rt.run(c.pc,100u);check(sequence==3u,"handoff unfinished");
}
}
int main(){try{basic();handoff(0);handoff(1);handoff(2);std::cout<<"PASS: event patterns, clear modes, timeout, status, delete and set/cancel/delete continuation wakeups\n";}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
