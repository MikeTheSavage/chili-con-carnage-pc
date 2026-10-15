#include "ge_runtime.hpp"
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using Context=psprecomp::AllegrexContext;
using Bytes=std::vector<std::uint8_t>;
constexpr unsigned set_nid=0xA4FC06A4,unset_nid=0x05DB22CE,enqueue_nid=0xAB49E76A;
constexpr unsigned callback_data_address=0x08804000,list=0x08809000,old_entry=0x08806000,new_entry=0x08806100,done_entry=0x08807000,stack=0x08810000;
unsigned assertions=0;
void check(bool ok,const std::string &why){++assertions;if(!ok)throw std::runtime_error(why);}
void same_context(const Context &a,const Context &b,const std::string &why){
    check(a.gpr==b.gpr && a.hi==b.hi && a.lo==b.lo && a.pc==b.pc && a.fcr31==b.fcr31 && a.vfpu_ctrl==b.vfpu_ctrl,why+": integer/control context");
    check(std::memcmp(a.fpr.data(),b.fpr.data(),sizeof(a.fpr))==0,why+": FPU bits");
    check(std::memcmp(a.vfpu.data(),b.vfpu.data(),sizeof(a.vfpu))==0,why+": VFPU bits");
}
void seed(Context &c){
    for(unsigned i=0;i<c.gpr.size();++i)c.gpr[i]=0xC0010000u+i*0x10203u;
    c.gpr[0]=0;c.hi=0x89ABCDEF;c.lo=0xFEDCBA98;c.pc=0x0880F000;c.fcr31=0xA5800003;
    for(unsigned i=0;i<c.fpr.size();++i)c.set_fpr_bits(i,0x7FC00000u+i*73u);
    for(unsigned i=0;i<c.vfpu.size();++i)c.vfpu[i]=std::bit_cast<float>(0xFFC00000u+i*131u);
    for(unsigned i=0;i<c.vfpu_ctrl.size();++i)c.vfpu_ctrl[i]=0xA55A0000+i;
}
struct Memory {
    Bytes ram,vram,scratch;
    explicit Memory(const psprecomp::GuestMemory &m):ram(m.bytes()),vram(m.vram_bytes()),scratch(psprecomp::GuestMemory::kScratchpadSize){m.copy_out(psprecomp::GuestMemory::kScratchpadBase,scratch);}
    void unchanged(const psprecomp::GuestMemory &m,const std::string &why)const{
        check(ram==m.bytes(),why+": all RAM");check(vram==m.vram_bytes(),why+": all VRAM");
        Bytes current(scratch.size());m.copy_out(psprecomp::GuestMemory::kScratchpadBase,current);check(current==scratch,why+": all scratchpad");
    }
};
bool same_callbacks(const auto &a,const auto &b){
    for(unsigned i=0;i<a.size();++i){if(a[i].has_value()!=b[i].has_value())return false;
        if(a[i] && (a[i]->signal!=b[i]->signal || a[i]->signal_arg!=b[i]->signal_arg || a[i]->finish!=b[i]->finish || a[i]->finish_arg!=b[i]->finish_arg || a[i]->gp!=b[i]->gp))return false;}
    return true;
}
struct Fixture {
    psprecomp::Runtime rt;
    chili::ThreadRuntime threads{rt};
    unsigned sdk{};
    chili::GeRuntime ge;
    explicit Fixture(const unsigned *version=nullptr):ge(rt,&threads,stack,version){threads.initialize_bootstrap();seed(rt.cpu());
        rt.memory().store32(list,0x0F001234);rt.memory().store32(list+4,0x0C000000);
        rt.memory().store32(0x00010000,0x1234FEDC);rt.memory().store32(0x04123400,0x34567890);
    }
    void write_record(unsigned signal,unsigned finish,unsigned signal_arg=0x1357,unsigned finish_arg=0x5678){
        auto &m=rt.memory();m.store32(callback_data_address,signal);m.store32(callback_data_address+4,signal_arg);m.store32(callback_data_address+8,finish);m.store32(callback_data_address+12,finish_arg);
    }
    void api_on(Context &target,unsigned nid,unsigned expected,const std::string &why){
        const auto before=target;const auto memory=Memory(rt.memory());const auto time=threads.now_us();
        const auto uid=psprecomp::runtime_thread_uid();const auto generation=psprecomp::runtime_thread_switch_generation();
        const bool intr=threads.in_interrupt(),enabled=threads.interrupts_enabled(),stopped=rt.stopped();const auto reason=rt.stop_reason();
        rt.invoke_import("sceGe_user",nid,target);auto context=before;context.set_gpr(2,expected);
        same_context(target,context,why);memory.unchanged(rt.memory(),why);
        check(threads.now_us()==time && psprecomp::runtime_thread_uid()==uid && psprecomp::runtime_thread_switch_generation()==generation,why+": scheduler identity/time");
        check(threads.in_interrupt()==intr && threads.interrupts_enabled()==enabled,why+": interrupt state");
        check(rt.stopped()==stopped && rt.stop_reason()==reason,why+": stop state");
    }
    void api(unsigned nid,unsigned expected,const std::string &why){api_on(rt.cpu(),nid,expected,why);}
    void set(unsigned id,unsigned gp){rt.cpu().gpr[4]=callback_data_address;rt.cpu().gpr[28]=gp;api(set_nid,id,"SetCallback");}
    void unset(unsigned id,unsigned result=0){rt.cpu().gpr[4]=id;api(unset_nid,result,"UnsetCallback");}
};
void registry(){
    Fixture f;f.write_record(0x08805000,old_entry);
    for(unsigned id=0;id<16;++id){f.set(id,0xABCD0000+id);const auto &cb=f.ge.callbacks[id];check(cb && cb->signal==0x08805000 && cb->signal_arg==0x1357 && cb->finish==old_entry && cb->finish_arg==0x5678 && cb->gp==0xABCD0000+id,"16-slot registration/GP");}
    f.rt.cpu().gpr[4]=callback_data_address;const auto all=f.ge.callbacks;f.api(set_nid,0x80000022,"exhaustion");check(same_callbacks(all,f.ge.callbacks),"exhaustion changed registry");
    for(unsigned invalid:{16u,31u,0x7FFFFFFFu,0x80000000u,0xFFFFFFFFu}){f.unset(invalid,0x80000100);check(same_callbacks(all,f.ge.callbacks),"invalid ID changed registry");}
    f.unset(7);check(!f.ge.callbacks[7],"both handlers still registered");
    f.write_record(0,new_entry,0,0xAABB);f.set(7,0x11223344);check(f.ge.callbacks[7]->signal==0 && f.ge.callbacks[7]->finish==new_entry && f.ge.callbacks[7]->finish_arg==0xAABB && f.ge.callbacks[7]->gp==0x11223344,"reused ID retained removed handlers/GP");
    f.unset(15);f.unset(7);f.set(7,42);f.set(15,43);check(f.ge.callbacks[7]->gp==42 && f.ge.callbacks[15]->gp==43,"lowest-free allocation after removal");
    for(unsigned id=0;id<16;++id)f.unset(id);
    for(unsigned id=0;id<16;++id){f.unset(id);check(!f.ge.callbacks[id],"repeat removal recreated registry");}
    for(unsigned invalid_pointer:{0u,0x0FFFFFF8u}){
        f.rt.cpu().gpr[4]=invalid_pointer;const auto empty=f.ge.callbacks;
        f.api(set_nid,0x800200D3,"existing SetCallback invalid pointer");check(same_callbacks(empty,f.ge.callbacks),"invalid SetCallback allocated an ID");
    }
    f.write_record(0,0,0,0);f.set(0,77);check(f.ge.callbacks[0].has_value(),"zero-handler registration did not allocate ID0");f.unset(0);
    // No interrupted/pending callback should be created by a removed ID.
    f.rt.cpu().gpr[4]=list;f.rt.cpu().gpr[5]=0;f.rt.cpu().gpr[6]=0;f.rt.cpu().gpr[7]=0;
    const auto commands=f.ge.commands;f.api(enqueue_nid,0x80000100,"removed ID enqueue");
    check(f.ge.completed.empty() && f.ge.next_list==1 && f.ge.commands==commands && !f.ge.interrupt_resume,"removed ID acknowledged or left pending work");
    f.rt.cpu().gpr[6]=0xFFFFFFFF;f.api(enqueue_nid,1,"no-callback enqueue");check(f.ge.completed.contains(1) && !f.ge.interrupt_resume,"no-callback completion changed");
}
void disabled_context(){
    Fixture f;f.write_record(0,old_entry);f.set(0,123);
    f.rt.invoke_import("Kernel_Library",0x092968F4,f.rt.cpu());check(!f.threads.interrupts_enabled(),"fixture interrupts not disabled");
    f.unset(0);f.unset(0);f.unset(0xFFFFFFFF,0x80000100);check(!f.threads.interrupts_enabled() && !f.ge.callbacks[0],"Unset enabled interrupts or retained handler");
}
struct Active {
    Fixture *f{};Context resume{};std::optional<Memory> memory;
    bool self_remove{},reuse{},old_sdk{};unsigned old_calls{},new_calls{},done_calls{};
};
Active *active{};
void old_callback(psprecomp::Runtime &,Context &c){
    auto &a=*active;auto &f=*a.f;++a.old_calls;
    check(f.threads.in_interrupt() && !f.threads.can_wait(),"old callback outside actual interrupt");
    check(c.pc==old_entry && c.gpr[4]==0x1234 && c.gpr[5]==0x5678 && c.gpr[6]==(a.old_sdk?0:list+8) && c.gpr[28]==0xABC && c.gpr[29]==stack+16384-64 && c.gpr[31]==chili::GeRuntime::interrupt_return,"original finish ABI/registered GP");
    const unsigned back=c.gpr[31];
    if(a.self_remove){f.unset(0);check(f.ge.interrupt_resume.has_value(),"self removal discarded entered continuation");
        f.unset(0);f.unset(16,0x80000100);
        if(a.reuse){f.write_record(0,new_entry,0,0xABBA);f.set(0,0x123456);check(f.ge.callbacks[0]->finish==new_entry,"active ID0 reuse failed");a.memory.emplace(f.rt.memory());}
    }
    auto continuation=f.ge.interrupt_resume;check(continuation.has_value(),"entered continuation missing");same_context(*continuation,a.resume,"entered continuation mutation");
    // A callback may clobber every guest register; only its saved continuation
    // may determine the interrupted thread's state at the return trampoline.
    seed(c);c.gpr[0]=123;c.hi=0;c.lo=0;c.fcr31=0;c.fpr[0]=0;c.vfpu[0]=0;c.vfpu_ctrl[0]=0;c.pc=back;
}
void new_callback(psprecomp::Runtime &,Context &c){
    auto &a=*active;++a.new_calls;
    check(c.gpr[4]==0x1234 && c.gpr[5]==0xABBA && c.gpr[28]==0x123456 && c.gpr[31]==chili::GeRuntime::interrupt_return,"replacement callback used stale ABI/GP");
    c.pc=c.gpr[31];
}
void done(psprecomp::Runtime &rt,Context &c){
    auto &a=*active;++a.done_calls;same_context(c,a.resume,"finish full context restoration");
    check(!a.f->ge.interrupt_resume && !a.f->threads.in_interrupt() && a.f->threads.can_wait(),"finish left pending/entered interrupt state");
    a.memory->unchanged(rt.memory(),"finish lifecycle output footprint");rt.stop("callback fixture completed");
}
void active_removal(bool self_remove,bool reuse,const unsigned *sdk){
    Fixture f(sdk);Active a;a.f=&f;a.self_remove=self_remove;a.reuse=reuse;a.old_sdk=sdk && *sdk<=0x02000010;active=&a;
    f.rt.register_function(old_entry,old_callback,"old_finish");f.rt.register_function(new_entry,new_callback,"new_finish");f.rt.register_function(done_entry,done,"finish_continuation");
    f.write_record(0x08805000,old_entry);f.set(0,0xABC);
    seed(f.rt.cpu());auto &c=f.rt.cpu();c.gpr[4]=list;c.gpr[5]=0;c.gpr[6]=0;c.gpr[7]=0;c.gpr[28]=0xDEF;c.gpr[29]=0x09FFF000;c.gpr[31]=done_entry;
    a.resume=c;a.resume.set_gpr(2,1);a.resume.pc=done_entry;
    const auto time=f.threads.now_us(),generation=psprecomp::runtime_thread_switch_generation();const auto uid=psprecomp::runtime_thread_uid();
    f.rt.invoke_import("sceGe_user",enqueue_nid,c);
    check(f.ge.completed.contains(1) && f.ge.interrupt_resume && f.threads.in_interrupt(),"finish not entered synchronously");same_context(*f.ge.interrupt_resume,a.resume,"saved finish context");
    if(!self_remove){
        // Invoke the HLE with a separate supplied context to exercise registry
        // lifetime before dispatch without modifying the already entered IRQ.
        // This models the service boundary, not PSP interrupt preemption.
        Context remover;seed(remover);remover.gpr[4]=0;f.api_on(remover,unset_nid,0,"entered IRQ registry removal");
        check(f.ge.interrupt_resume.has_value() && c.pc==old_entry,"removal canceled already entered finish");
        f.api_on(remover,unset_nid,0,"entered IRQ repeated removal");
        if(reuse){f.write_record(0,new_entry,0,0xABBA);remover.gpr[4]=callback_data_address;remover.gpr[28]=0x123456;f.api_on(remover,set_nid,0,"entered IRQ ID reuse");}
    }
    a.memory.emplace(f.rt.memory());f.rt.run(c.pc,10);
    check(f.rt.stop_reason()=="callback fixture completed" && a.old_calls==1 && a.new_calls==0 && a.done_calls==1,"entered callback dispatched stale/replacement handler");
    check(f.threads.now_us()==time && psprecomp::runtime_thread_uid()==uid && psprecomp::runtime_thread_switch_generation()==generation,"finish teardown altered scheduler/time");
    if(reuse){
        check(f.ge.callbacks[0] && f.ge.callbacks[0]->finish==new_entry,"old IRQ return removed replacement registration");
        c.gpr[4]=list;c.gpr[5]=0;c.gpr[6]=0;c.gpr[7]=0;c.gpr[31]=done_entry;a.resume=c;a.resume.set_gpr(2,2);a.resume.pc=done_entry;
        f.rt.invoke_import("sceGe_user",enqueue_nid,c);a.memory.emplace(f.rt.memory());f.rt.run(c.pc,10);
        check(a.new_calls==1 && a.old_calls==1 && a.done_calls==2 && f.ge.completed.contains(2),"new submission did not use replacement exactly once");f.unset(0);
    }else check(!f.ge.callbacks[0],"removed active registration survived callback return");
    c.gpr[4]=list;c.gpr[5]=0;c.gpr[6]=0;c.gpr[7]=0;const auto count=f.ge.next_list;f.api(enqueue_nid,0x80000100,"post-IRQ stale ID enqueue");
    check(f.ge.next_list==count && !f.ge.interrupt_resume && a.old_calls==1 && a.new_calls==unsigned(reuse),"stale ID re-dispatched a removed callback");active=nullptr;
}
}
int main(){try{std::cout<<std::unitbuf;registry();disabled_context();
    const unsigned old_sdk=0x02000010,new_sdk=0x02000011;
    active_removal(false,false,nullptr);active_removal(false,true,&new_sdk);active_removal(true,false,&old_sdk);active_removal(true,true,&new_sdk);
    check(chili::GeRuntime::instances.empty(),"destroyed GE instance retained global runtime pointer");
    std::cout<<"PASS callback lifecycle: "<<assertions<<" assertions; 16 IDs, repeated/invalid removal, full CPU/FPU/VFPU/GP and RAM/VRAM/scratch preservation, entered IRQ removal/reuse, stale dispatch prevention\n";return 0;
}catch(const std::exception &e){std::cerr<<"FAIL callback lifecycle: "<<e.what()<<'\n';return 1;}}
