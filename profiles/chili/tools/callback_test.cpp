#include "../host/thread_runtime.hpp"
#include <iostream>
#include <stdexcept>
namespace {
constexpr auto name=0x08802000u, entry=0x08804100u, done=0x08804200u;
std::uint32_t uid; unsigned calls; bool delete_on_return;
void require(bool ok,const char *s){if(!ok)throw std::runtime_error(s);}
void call(psprecomp::Runtime &rt,std::uint32_t nid){rt.invoke_import("ThreadManForUser",nid,rt.cpu());}
void callback(psprecomp::Runtime &,psprecomp::AllegrexContext &c){
    require(c.gpr[4]==2 && c.gpr[5]==42 && c.gpr[6]==0x1234u,"callback arguments mismatch");
    require(c.gpr[28]==0x5678u,"callback GP mismatch");++calls;
    c.gpr[16]=0xFFFFFFFFu;c.fpr[2]=0xFFu;c.set_gpr(2,delete_on_return?1u:0u);c.pc=c.gpr[31];
}
void completed(psprecomp::Runtime &,psprecomp::AllegrexContext &c){
    require(c.gpr[2]==1u && c.gpr[16]==7u && c.fpr[2]==9u && c.gpr[28]==0xABCDu && c.gpr[29]==0x09FFF000u,"callback context not restored");c.pc=0;
}
void check_callback(bool erase){
    calls=0;delete_on_return=erase;psprecomp::Runtime rt;chili::ThreadRuntime threads(rt);
    rt.cpu().gpr[29]=0x09FFF000u;threads.initialize_bootstrap();rt.memory().store32(name,0x006263u);
    rt.register_function(entry,callback,"test_callback");rt.register_function(done,completed,"callback_done");
    auto &c=rt.cpu();c.gpr[4]=name;c.gpr[5]=entry;c.gpr[6]=0x1234u;c.gpr[28]=0x5678u;call(rt,0xE81CAF8Fu);uid=c.gpr[2];
    c.gpr[4]=uid;c.gpr[5]=20u;call(rt,0xC11BA8C4u);c.gpr[4]=uid;c.gpr[5]=42u;call(rt,0xC11BA8C4u);
    c.gpr[4]=uid;call(rt,0x2A3D44FFu);require(c.gpr[2]==2,"notification count mismatch");
    const auto info=name+64u;rt.memory().store32(info,56u);c.gpr[4]=uid;c.gpr[5]=info;call(rt,0x730ED8BCu);
    require(rt.memory().load32(info+36)==1u && rt.memory().load32(info+48)==2u && rt.memory().load32(info+52)==42u,"callback status mismatch");
    c.gpr[16]=7;c.fpr[2]=9;c.gpr[28]=0xABCDu;c.gpr[31]=done;call(rt,0x349D6D6Cu);rt.run(c.pc,100u);require(calls==1u,"callback not delivered once");
    c.gpr[4]=uid;call(rt,0x2A3D44FFu);require(c.gpr[2]==(erase?0x800201A1u:0u),"callback return deletion/count failed");
}
unsigned stage;
void delay_callback(psprecomp::Runtime &,psprecomp::AllegrexContext &c){require(c.gpr[4]==1 && c.gpr[5]==0 && c.gpr[6]==0,"exit callback args failed");++stage;c.gpr[2]=0;c.pc=c.gpr[31];}
void child_done(psprecomp::Runtime &,psprecomp::AllegrexContext &c){require(stage==2u && c.gpr[2]==0u,"delay continuation failed");++stage;c.pc=chili::ThreadRuntime::return_trampoline;}
void child(psprecomp::Runtime &rt,psprecomp::AllegrexContext &c){
    c.gpr[4]=name;c.gpr[5]=0x08804500u;c.gpr[6]=0;call(rt,0xE81CAF8Fu);uid=c.gpr[2];c.gpr[4]=uid;rt.invoke_import("LoadExecForUser",0x4AC57943u,c);
    c.gpr[4]=1000;c.gpr[31]=0x08804600u;call(rt,0x68DA9E36u);
}
chili::ThreadRuntime *scheduler;
void parent(psprecomp::Runtime &,psprecomp::AllegrexContext &c){stage=1;scheduler->request_exit_callback();c.gpr[31]=0x08804800u;scheduler->dispatch_ready(c);}
void parent_delay(psprecomp::Runtime &rt,psprecomp::AllegrexContext &c){c.gpr[4]=2000;c.gpr[31]=chili::ThreadRuntime::return_trampoline;call(rt,0xCEADEB47u);}
void check_delay(){
    stage=0;psprecomp::Runtime rt;chili::ThreadRuntime threads(rt);scheduler=&threads;rt.cpu().gpr[31]=0x08804700u;threads.initialize_bootstrap();rt.memory().store32(name,0x006263u);
    rt.register_function(0x08804400u,child,"child");rt.register_function(0x08804500u,delay_callback,"exit_cb");rt.register_function(0x08804600u,child_done,"child_done");rt.register_function(0x08804700u,parent,"parent");rt.register_function(0x08804800u,parent_delay,"parent_delay");
    auto &c=rt.cpu();c.gpr[4]=name;c.gpr[5]=0x08804400u;c.gpr[6]=0x10u;c.gpr[7]=0x1000u;c.gpr[8]=0;call(rt,0x446D8DE6u);c.gpr[4]=c.gpr[2];c.gpr[5]=0;c.gpr[6]=0;call(rt,0xF475845Du);rt.run(c.pc,100u);require(stage==3u && threads.now_us()==2000u,"callback delay deadline not preserved");
}
void check_power_registration(){
    psprecomp::Runtime rt;chili::ThreadRuntime threads(rt);threads.initialize_bootstrap();rt.memory().store32(name,0x006263u);
    auto &c=rt.cpu();c.gpr[4]=name;c.gpr[5]=entry;c.gpr[6]=0;call(rt,0xE81CAF8Fu);const auto cb=c.gpr[2];
    c.gpr[4]=0xFFFFFFFFu;c.gpr[5]=0;rt.invoke_import("scePower",0x04B7766Eu,c);require(c.gpr[2]==0x80000100u,"invalid power callback accepted");
    c.gpr[4]=0xFFFFFFFFu;c.gpr[5]=cb;rt.invoke_import("scePower",0x04B7766Eu,c);require(c.gpr[2]==0,"first automatic slot mismatch");
    c.gpr[4]=cb;c.gpr[5]=name+64u;rt.memory().store32(c.gpr[5],56u);call(rt,0x730ED8BCu);
    require(rt.memory().load32(name+64u+48u)==1u && rt.memory().load32(name+64u+52u)==0x10E4u,"initial power status not queued");
    c.gpr[4]=0;c.gpr[5]=cb;rt.invoke_import("scePower",0x04B7766Eu,c);require(c.gpr[2]==0x80000020u,"occupied slot accepted");
    for(unsigned i=1;i<16;++i){c.gpr[4]=0xFFFFFFFFu;c.gpr[5]=cb;rt.invoke_import("scePower",0x04B7766Eu,c);require(c.gpr[2]==i,"automatic slot assignment mismatch");}
    c.gpr[4]=0xFFFFFFFFu;c.gpr[5]=cb;rt.invoke_import("scePower",0x04B7766Eu,c);require(c.gpr[2]==0x80000022u,"full power slot table accepted registration");
}
}
int main(){try{check_callback(false);check_callback(true);check_delay();check_power_registration();std::cout<<"PASS: callback ownership/count/status/arguments/context/deletion, exit notification during callback-aware delay and power slot/status registration\n";}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
