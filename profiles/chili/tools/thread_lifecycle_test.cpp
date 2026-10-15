#include "thread_lifecycle.hpp"
#include "partition_memory.hpp"
#include <bit>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
constexpr std::uint32_t create_nid=0x446D8DE6u,start_nid=0xF475845Du,join_nid=0x278C0DF5u,
    exit_nid=0xAA73C935u,delete_nid=0x9FA03CD3u,id_nid=0x293B45B8u;
constexpr std::uint32_t child_pc=0x08804000u,resume_pc=0x08804100u,timeout_ptr=0x08803000u;
psprecomp::AllegrexContext saved_parent;
std::uint32_t expected_result,worker_uid,worker_bottom,worker_mode;
std::uint32_t worker_callback;
unsigned resumed_count;
std::vector<unsigned> join_order;
void require(bool value,const char *text) { if(!value)throw std::runtime_error(text); }
void call(psprecomp::Runtime &rt,std::uint32_t nid) { rt.invoke_import("ThreadManForUser",nid,rt.cpu()); }
std::uint32_t create(psprecomp::Runtime &rt,std::uint32_t entry=child_pc,std::uint32_t priority=0x30u,std::uint32_t attr=0u) {
    const char name[]="lifecycle_fixture";
    for(unsigned i=0;i<sizeof(name);++i)rt.memory().store8(0x08801000u+i,name[i]);
    auto &c=rt.cpu();c.gpr[4]=0x08801000u;c.gpr[5]=entry;c.gpr[6]=priority;c.gpr[7]=0x1000u;c.gpr[8]=attr;
    call(rt,create_nid);require(c.gpr[2]<0x80000000u,"Create failed");return c.gpr[2];
}
void start(psprecomp::Runtime &rt,std::uint32_t uid) {
    auto &c=rt.cpu();c.gpr[4]=uid;c.gpr[5]=0u;c.gpr[6]=0u;call(rt,start_nid);
    require(c.gpr[2]==0u,"Start failed");
}
void set_parent_state(psprecomp::AllegrexContext &c) {
    for(unsigned i=1;i<32u;++i)c.gpr[i]=0x11000000u+i;
    for(unsigned i=0;i<32u;++i)c.set_fpr_bits(i,0x7FC00000u+i);
    for(unsigned i=0;i<128u;++i)c.vfpu[i]=std::bit_cast<float>(0x7FA00000u+i);
    for(unsigned i=0;i<16u;++i)c.vfpu_ctrl[i]=0xF0000000u+i;
    c.hi=0x13579BDFu;c.lo=0x2468ACE0u;c.fcr31=0xCAFEBABEu;c.gpr[31]=resume_pc;
}
void verify_parent(psprecomp::Runtime &rt,psprecomp::AllegrexContext &c) {
    require(psprecomp::runtime_thread_uid()==1,"Scheduler identity did not return to parent");
    require(c.pc==resume_pc,"Join resumed at wrong PC");
    require(c.gpr[2]==expected_result,"Join did not return target status");
    for(unsigned i=0;i<32u;++i)if(i!=2u)require(c.gpr[i]==saved_parent.gpr[i],"Join lost a parent GPR");
    require(c.hi==saved_parent.hi&&c.lo==saved_parent.lo&&c.fcr31==saved_parent.fcr31,"Join lost HI/LO/FCR31");
    for(unsigned i=0;i<32u;++i)require(c.fpr_bits(i)==saved_parent.fpr_bits(i),"Join lost FPU bits");
    for(unsigned i=0;i<128u;++i)require(std::bit_cast<std::uint32_t>(c.vfpu[i])==std::bit_cast<std::uint32_t>(saved_parent.vfpu[i]),"Join lost VFPU bits");
    require(c.vfpu_ctrl==saved_parent.vfpu_ctrl,"Join lost VFPU controls");
    ++resumed_count;rt.stop("fixture complete");
}
void worker(psprecomp::Runtime &rt,psprecomp::AllegrexContext &c) {
    require(c.gpr[4]==0u&&c.gpr[5]==0u,"Fresh/restarted child retained arguments");
    require(c.gpr[16]==0u&&c.hi==0u&&c.lo==0u&&c.fpr_bits(2)==0u&&std::bit_cast<std::uint32_t>(c.vfpu[127])==0u,"Fresh/restarted child retained prior CPU state");
    require(c.gpr[28]==0u&&c.vfpu_ctrl[0]==0xE4u&&c.vfpu_ctrl[1]==0xE4u&&c.vfpu_ctrl[2]==0u,"Fresh/restarted child GP/prefix state incorrect");
    rt.invoke_import("ThreadManForUser",id_nid,c);require(c.gpr[2]==worker_uid,"GetThreadId did not follow worker dispatch");
    if(worker_mode==5u) {
        c.gpr[4]=0x08801000u;c.gpr[5]=child_pc+24u;c.gpr[6]=0u;rt.invoke_import("ThreadManForUser",0xE81CAF8Fu,c);
        c.gpr[4]=c.gpr[2];c.gpr[5]=1u;rt.invoke_import("ThreadManForUser",0xC11BA8C4u,c);
        c.gpr[4]=50u;rt.invoke_import("ThreadManForUser",0x68DA9E36u,c);return;
    }
    if(worker_mode==4u) {
        c.gpr[4]=0x08801000u;c.gpr[5]=0x08805000u;c.gpr[6]=0u;
        rt.invoke_import("ThreadManForUser",0xE81CAF8Fu,c);worker_callback=c.gpr[2];
        require(worker_callback<0x80000000u,"Child callback create failed");
        c.gpr[4]=worker_callback;rt.invoke_import("LoadExecForUser",0x4AC57943u,c);require(c.gpr[2]==0u,"Child exit callback registration failed");
        c.gpr[4]=0u;c.gpr[5]=worker_callback;rt.invoke_import("scePower",0x04B7766Eu,c);require(c.gpr[2]==0u,"Child power callback registration failed");
    }
    if(worker_mode==2u) {
        c.gpr[4]=200u;c.gpr[31]=child_pc+4u;rt.invoke_import("ThreadManForUser",0xCEADEB47u,c);return;
    }
    const auto return_pc=c.gpr[31];
    for(unsigned i=1;i<32u;++i)c.gpr[i]=0xEE000000u+i;
    c.hi=0xEEEEEEEEu;c.lo=0xFFFFFFFFu;
    for(unsigned i=0;i<32u;++i)c.set_fpr_bits(i,0xDEAD0000u+i);
    for(unsigned i=0;i<128u;++i)c.vfpu[i]=std::bit_cast<float>(0xFEED0000u+i);
    c.vfpu_ctrl.fill(0xFACEFACEu);
    if(worker_mode==0u){c.gpr[2]=expected_result;c.pc=return_pc;}
    else {c.gpr[4]=worker_mode==3u?0xFFFFFFFFu:expected_result;rt.invoke_import("ThreadManForUser",exit_nid,c);}
}
void delayed_return(psprecomp::Runtime &,psprecomp::AllegrexContext &c) {c.gpr[2]=7u;c.pc=chili::ThreadRuntime::return_trampoline;}
void exit_callback(psprecomp::Runtime &rt,psprecomp::AllegrexContext &c) {
    require(c.gpr[4]==1u&&c.gpr[5]==1u,"Active callback context incorrect");
    c.gpr[4]=12u;rt.invoke_import("ThreadManForUser",exit_nid,c);
}
void timeout_resume(psprecomp::Runtime &rt,psprecomp::AllegrexContext &c) {
    require(c.gpr[2]==0x800201A8u,"Join timeout result wrong");
    require(rt.memory().load32(timeout_ptr)==0u,"Join timeout did not update pointer");
    c.gpr[4]=worker_uid;c.gpr[5]=0u;c.gpr[31]=resume_pc;
    saved_parent=c;saved_parent.pc=resume_pc;expected_result=7u;
    rt.invoke_import("ThreadManForUser",join_nid,c);
}
struct Fixture {
    psprecomp::Runtime rt;
    chili::PartitionMemory memory;
    chili::ThreadRuntime threads;
    std::uint32_t sdk=0x03080000u;
    chili::ThreadLifecycle lifecycle;
    Fixture():memory(rt,0x09000000u,0x09FF0000u),
        threads(rt,[this](std::uint32_t size){worker_bottom=memory.reserve_stack(size);return worker_bottom;}),
        lifecycle(rt,threads,[this](std::uint32_t base){return memory.release_region(base);},&sdk) {
        rt.cpu().gpr[31]=resume_pc;threads.initialize_bootstrap();
        rt.register_function(child_pc,worker,"worker");
        rt.register_function(child_pc+4u,delayed_return,"worker_delayed_return");
        rt.register_function(child_pc+24u,exit_callback,"callback_exit");
        rt.register_function(resume_pc,verify_parent,"join_resume");
        rt.register_function(resume_pc+4u,timeout_resume,"timeout_resume");
    }
};
void test_join(std::uint32_t mode,std::uint32_t result) {
    Fixture f;worker_mode=mode;expected_result=result;resumed_count=0u;
    worker_uid=create(f.rt);start(f.rt,worker_uid);
    set_parent_state(f.rt.cpu());f.rt.cpu().gpr[4]=worker_uid;f.rt.cpu().gpr[5]=timeout_ptr;
    f.rt.memory().store32(timeout_ptr,1000u);
    saved_parent=f.rt.cpu();saved_parent.pc=resume_pc;
    call(f.rt,join_nid);require(f.threads.current_uid()==2,"Join failed to dispatch worker");
    f.rt.run(f.rt.cpu().pc,100u);
    require(f.rt.stop_reason()=="fixture complete"&&resumed_count==1u,"Parent did not resume once");
    require(f.rt.memory().load32(timeout_ptr)==1000u,"Successful immediate worker consumed fabricated time");
    f.rt.cpu().gpr[4]=worker_uid;f.rt.cpu().gpr[5]=timeout_ptr;call(f.rt,join_nid);
    require(f.rt.cpu().gpr[2]==result,"Repeated join changed status");
    if((mode==0u&&result==5u)||mode==5u) {
        worker_mode=1u;expected_result=11u;resumed_count=0u;start(f.rt,worker_uid);
        set_parent_state(f.rt.cpu());f.rt.cpu().gpr[4]=worker_uid;f.rt.cpu().gpr[5]=0u;
        saved_parent=f.rt.cpu();saved_parent.pc=resume_pc;call(f.rt,join_nid);
        f.rt.run(f.rt.cpu().pc,100u);
        require(f.rt.stop_reason()=="fixture complete"&&resumed_count==1u,"Ended thread could not restart and rejoin");
    }
    f.rt.cpu().gpr[4]=worker_uid;call(f.rt,delete_nid);require(f.rt.cpu().gpr[2]==0u,"Ended delete failed");
    f.rt.cpu().gpr[4]=worker_uid;f.rt.cpu().gpr[5]=0u;call(f.rt,join_nid);
    require(f.rt.cpu().gpr[2]==0x80020198u,"Deleted ended thread remained joinable");
    if(mode==4u) {
        f.rt.cpu().gpr[4]=worker_callback;call(f.rt,0x2A3D44FFu);require(f.rt.cpu().gpr[2]==0x800201A1u,"Deleting thread retained owned callback");
        f.threads.request_exit_callback(); // No dangling callback-owner map access.
        auto &c=f.rt.cpu();c.gpr[4]=0x08801000u;c.gpr[5]=0x08805000u;c.gpr[6]=0u;call(f.rt,0xE81CAF8Fu);
        c.gpr[5]=c.gpr[2];c.gpr[4]=0u;f.rt.invoke_import("scePower",0x04B7766Eu,c);
        require(c.gpr[2]==0u,"Deleted thread retained power callback slot");
    }
}
void test_timeout() {
    Fixture f;worker_mode=2u;resumed_count=0u;worker_uid=create(f.rt);start(f.rt,worker_uid);
    set_parent_state(f.rt.cpu());f.rt.cpu().gpr[4]=worker_uid;f.rt.cpu().gpr[5]=timeout_ptr;f.rt.cpu().gpr[31]=resume_pc+4u;
    f.rt.memory().store32(timeout_ptr,100u);call(f.rt,join_nid);
    f.rt.run(f.rt.cpu().pc,100u);
    require(f.rt.stop_reason()=="fixture complete"&&resumed_count==1u,"Rejoin after timeout failed");
    require(f.rt.memory().load32(timeout_ptr)==0u,"Stale join changed expired timeout");
    require(f.threads.now_us()==200u,"Scheduler timeout/delay clock incorrect");
}
void test_errors_and_delete() {
    Fixture f;auto &c=f.rt.cpu();const auto dormant=create(f.rt,child_pc,0x30u,0x00200000u);const auto base=worker_bottom;
    c.gpr[4]=dormant;c.gpr[5]=timeout_ptr;f.rt.memory().store32(timeout_ptr,500u);call(f.rt,join_nid);
    require(c.gpr[2]==0x800201A2u&&f.rt.memory().load32(timeout_ptr)==500u,"Never-started join semantics incorrect");
    for(auto uid:{0u,1u}){c.gpr[4]=uid;call(f.rt,join_nid);require(c.gpr[2]==0x80020197u,"Self/zero join accepted");}
    c.gpr[4]=0xDEADBEEFu;call(f.rt,join_nid);require(c.gpr[2]==0x80020198u,"Invalid join accepted");
    for(auto uid:{0u,1u}){c.gpr[4]=uid;call(f.rt,delete_nid);require(c.gpr[2]==0x800201A4u,"Self/zero delete accepted");}
    c.gpr[4]=dormant;call(f.rt,delete_nid);require(c.gpr[2]==0u,"Dormant delete failed");
    for(unsigned i=0;i<0x1000u;++i)require(f.rt.memory().load8(base+i)==0u,"CLEAR_STACK did not zero deleted allocation");
    call(f.rt,delete_nid);require(c.gpr[2]==0x80020198u,"Deleted UID remained valid");
    const auto next=create(f.rt);require(next!=dormant&&worker_bottom==base,"Delete did not release/reuse stack with a fresh UID");
    start(f.rt,next);c.gpr[4]=next;call(f.rt,delete_nid);require(c.gpr[2]==0x800201A4u,"Ready thread deleted");
    c.gpr[4]=next;c.gpr[5]=timeout_ptr;f.rt.memory().store32(timeout_ptr,0u);call(f.rt,join_nid);
    require(c.gpr[2]==0x800201A8u&&f.threads.current_uid()==1,"Zero timeout blocked");
    c.gpr[5]=timeout_ptr+1u;call(f.rt,join_nid);require(c.gpr[2]==0x800200D3u,"Unaligned timeout accepted");
    c.gpr[5]=0x01000000u;call(f.rt,join_nid);require(c.gpr[2]==0x800200D3u,"Unmapped timeout accepted");
    call(f.rt,id_nid);require(c.gpr[2]==1u,"Existing GetThreadId regressed");
    f.rt.invoke_import("Kernel_Library",0x092968F4u,c);c.gpr[4]=next;c.gpr[5]=0u;call(f.rt,join_nid);
    require(c.gpr[2]==0x800201A7u&&f.threads.current_uid()==1,"Join blocked with interrupts disabled");
    c.gpr[4]=10u;call(f.rt,exit_nid);require(c.gpr[2]==0x800201A7u&&f.threads.current_uid()==1,"Exit switched with interrupts disabled");
    c.gpr[4]=1u;f.rt.invoke_import("Kernel_Library",0x5F10D406u,c);
    require(f.threads.enter_interrupt(),"Cannot enter fixture interrupt");c.gpr[4]=next;call(f.rt,join_nid);
    require(c.gpr[2]==0x80020064u,"Join interrupt context not rejected");call(f.rt,exit_nid);
    require(c.gpr[2]==0x80020064u,"Exit interrupt context not rejected");f.threads.leave_interrupt(c);
}
void joining_peer(psprecomp::Runtime &rt,psprecomp::AllegrexContext &c) {
    c.gpr[4]=worker_uid;c.gpr[5]=0u;c.gpr[31]=child_pc+16u;
    rt.invoke_import("ThreadManForUser",join_nid,c);
}
void peer_resumed(psprecomp::Runtime &,psprecomp::AllegrexContext &c) {
    require(c.gpr[2]==5u,"Multiple joiner status incorrect");join_order.push_back(static_cast<unsigned>(psprecomp::runtime_thread_uid()));
    c.gpr[2]=0u;c.pc=chili::ThreadRuntime::return_trampoline;
}
void all_joined(psprecomp::Runtime &rt,psprecomp::AllegrexContext &) {
    require(join_order==std::vector<unsigned>{3u,4u},"Joiners did not wake in priority order");rt.stop("all joined");
}
void test_multiple_joiners() {
    Fixture f;worker_mode=0u;expected_result=5u;join_order.clear();worker_uid=create(f.rt,child_pc,0x50u);
    const auto a=create(f.rt,child_pc+8u,0x30u),b=create(f.rt,child_pc+8u,0x40u);
    f.rt.register_function(child_pc+8u,joining_peer,"joining_peer");f.rt.register_function(child_pc+16u,peer_resumed,"peer_resumed");
    f.rt.register_function(resume_pc+8u,all_joined,"all_joined");
    start(f.rt,worker_uid);start(f.rt,a);start(f.rt,b);
    f.rt.cpu().gpr[4]=1u;f.rt.cpu().gpr[31]=resume_pc+8u;call(f.rt,0xCEADEB47u);
    f.rt.run(f.rt.cpu().pc,100u);require(f.rt.stop_reason()=="all joined","Multiple joiners did not complete");
}
}
int main() {
    try {
        test_join(0u,5u);test_join(1u,9u);test_join(3u,0x800200D2u);test_join(0u,0xFFFFFFFFu);test_join(4u,9u);test_join(5u,12u);
        test_timeout();test_errors_and_delete();test_multiple_joiners();
        std::cout<<"PASS: return/explicit exit, signed status, full GPR/HI/LO/FPU/VFPU context, repeated joins, Ended restart/reset, timeout/rejoin, multiple priority joiners, dormant/ready/self/invalid UIDs, stack reclamation and clear, owned callback cleanup, interrupt rejection, GetThreadId\n";
    } catch(const std::exception &e) {std::cerr<<e.what()<<'\n';return 1;}
}
