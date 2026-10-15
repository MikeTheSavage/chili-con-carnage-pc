#include "../host/semaphores.hpp"
#include <stdexcept>
#include <iostream>
namespace {
constexpr auto name=0x08802000u, timeout=name+64u;
std::uint32_t uid;unsigned action,sequence;
void check(bool value,const char *s){if(!value)throw std::runtime_error(s);}
void call(psprecomp::Runtime &rt,std::uint32_t nid){rt.invoke_import("ThreadManForUser",nid,rt.cpu());}
std::uint32_t create(psprecomp::Runtime &rt,std::uint32_t initial=0u){
    rt.memory().store32(name,0x006D6573u);auto &c=rt.cpu();c.gpr[4]=name;c.gpr[5]=0;c.gpr[6]=initial;c.gpr[7]=3;c.gpr[8]=0;call(rt,0xD6DA4BA1u);check(static_cast<std::int32_t>(c.gpr[2])>0,"creation failed");return c.gpr[2];
}
void acquire(psprecomp::Runtime &rt,std::uint32_t amount,bool poll,std::uint32_t timer=0){auto &c=rt.cpu();c.gpr[4]=uid;c.gpr[5]=amount;c.gpr[6]=timer;call(rt,poll?0x58B1F937u:0x4E3A1105u);}
void basic(){
    psprecomp::Runtime rt;chili::ThreadRuntime threads(rt);threads.initialize_bootstrap();chili::Semaphores semas(rt,threads);uid=create(rt,2);
    acquire(rt,2,true);check(rt.cpu().gpr[2]==0,"counted acquisition failed");acquire(rt,1,true);check(rt.cpu().gpr[2]==0x800201ADu,"exhausted poll failed");
    rt.cpu().gpr[4]=uid;rt.cpu().gpr[5]=2;call(rt,0x3F53E640u);check(rt.cpu().gpr[2]==0,"counted signal failed");
    rt.cpu().gpr[4]=uid;rt.cpu().gpr[5]=0x7FFFFFFF;call(rt,0x3F53E640u);check(rt.cpu().gpr[2]==0x800201AEu,"overflow signal accepted");acquire(rt,2,true);check(rt.cpu().gpr[2]==0,"overflow modified count");
    acquire(rt,0,true);check(rt.cpu().gpr[2]==0x800201BDu,"zero count accepted");
    rt.memory().store32(timeout,100);rt.cpu().gpr[31]=0x08804000u;acquire(rt,1,false,timeout);check(rt.cpu().gpr[2]==0x800201A8u && rt.memory().load32(timeout)==0 && threads.now_us()==100,"timeout failed");
    rt.cpu().gpr[4]=uid;rt.cpu().gpr[5]=0xFFFFFFFFu;rt.cpu().gpr[6]=0;call(rt,0x8FFDF9A2u);acquire(rt,2,true);check(rt.cpu().gpr[2]==0,"negative cancel did not reset initial count");
    auto info=name+128u;rt.memory().store32(info,56);rt.cpu().gpr[4]=uid;rt.cpu().gpr[5]=info;call(rt,0xBC6FEBC5u);check(rt.memory().load32(info+40)==2 && rt.memory().load32(info+44)==0 && rt.memory().load32(info+48)==3 && rt.memory().load32(info+52)==0,"status failed");
    rt.cpu().gpr[4]=uid;call(rt,0x28B6489Cu);acquire(rt,1,true);check(rt.cpu().gpr[2]==0x80020199u,"deleted semaphore remained valid");
}
void resumed(psprecomp::Runtime &,psprecomp::AllegrexContext &c){check(c.gpr[2]==(action==0?0u:action==1?0x800201A9u:0x800201B5u),"wake result failed");check(sequence==1,"wake did not preempt");++sequence;c.pc=chili::ThreadRuntime::return_trampoline;}
void child(psprecomp::Runtime &rt,psprecomp::AllegrexContext &c){c.gpr[31]=0x08804200u;acquire(rt,2,false);}
void done(psprecomp::Runtime &,psprecomp::AllegrexContext &c){check(sequence==2,"parent continuation lost");++sequence;c.pc=chili::ThreadRuntime::return_trampoline;}
void release(psprecomp::Runtime &rt,psprecomp::AllegrexContext &c){sequence=1;c.gpr[31]=0x08804400u;c.gpr[4]=uid;c.gpr[5]=2;c.gpr[6]=0;call(rt,action==0?0x3F53E640u:action==1?0x8FFDF9A2u:0x28B6489Cu);}
void handoff(unsigned value){
    action=value;sequence=0;psprecomp::Runtime rt;chili::ThreadRuntime threads(rt);rt.cpu().gpr[31]=0x08804300u;threads.initialize_bootstrap();chili::Semaphores semas(rt,threads);uid=create(rt);
    rt.register_function(0x08804100u,child,"sema_child");rt.register_function(0x08804200u,resumed,"sema_resume");rt.register_function(0x08804300u,release,"sema_release");rt.register_function(0x08804400u,done,"sema_done");
    auto &c=rt.cpu();c.gpr[4]=name;c.gpr[5]=0x08804100u;c.gpr[6]=0x10;c.gpr[7]=0x1000;c.gpr[8]=0;call(rt,0x446D8DE6u);c.gpr[4]=c.gpr[2];c.gpr[5]=0;c.gpr[6]=0;call(rt,0xF475845Du);rt.run(c.pc,100);check(sequence==3,"handoff failed");
}
std::uint32_t second_uid;
void queued_first(psprecomp::Runtime &rt,psprecomp::AllegrexContext &c){
    c.gpr[16]=1;c.gpr[31]=0x08804700u;rt.memory().store32(timeout,100u);acquire(rt,3,false,timeout);
}
void queued_second(psprecomp::Runtime &rt,psprecomp::AllegrexContext &c){c.gpr[16]=2;c.gpr[31]=0x08804700u;acquire(rt,1,false);}
void queued_resume(psprecomp::Runtime &,psprecomp::AllegrexContext &c){
    check(c.gpr[2]==(c.gpr[16]==1?0x800201A8u:0u),"queued timeout/acquisition result failed");sequence|=c.gpr[16];c.pc=chili::ThreadRuntime::return_trampoline;
}
void start_second(psprecomp::Runtime &rt,psprecomp::AllegrexContext &c){c.gpr[31]=0x08804900u;c.gpr[4]=second_uid;c.gpr[5]=0;c.gpr[6]=0;call(rt,0xF475845Du);}
void parent_sleep(psprecomp::Runtime &rt,psprecomp::AllegrexContext &c){c.gpr[4]=200;c.gpr[31]=chili::ThreadRuntime::return_trampoline;call(rt,0xCEADEB47u);}
void timeout_queue(){
    sequence=0;psprecomp::Runtime rt;chili::ThreadRuntime threads(rt);rt.cpu().gpr[31]=0x08804800u;threads.initialize_bootstrap();chili::Semaphores semas(rt,threads);uid=create(rt,1);
    rt.register_function(0x08804500u,queued_first,"first");rt.register_function(0x08804600u,queued_second,"second");rt.register_function(0x08804700u,queued_resume,"queue_resume");rt.register_function(0x08804800u,start_second,"start_second");rt.register_function(0x08804900u,parent_sleep,"parent_sleep");
    auto make=[&](std::uint32_t entry,std::uint32_t priority){auto &c=rt.cpu();c.gpr[4]=name;c.gpr[5]=entry;c.gpr[6]=priority;c.gpr[7]=0x1000;c.gpr[8]=0;call(rt,0x446D8DE6u);return c.gpr[2];};
    const auto first=make(0x08804500u,0x10u);second_uid=make(0x08804600u,0x08u);
    auto &c=rt.cpu();c.gpr[4]=first;c.gpr[5]=0;c.gpr[6]=0;call(rt,0xF475845Du);rt.run(c.pc,100);
    check(sequence==3u && threads.now_us()==200u,"timeout failed to hand existing count to next queued waiter");
}
}
int main(){try{basic();handoff(0);handoff(1);handoff(2);timeout_queue();std::cout<<"PASS: counted acquisition/signal, overflow preservation, timeout/status/reset/delete, signal/cancel/delete continuations and mixed-count FIFO timeout handoff\n";}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
