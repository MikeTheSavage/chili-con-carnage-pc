#include "volatile_memory_runtime.hpp"
#include "thread_runtime.hpp"
#include "thread_lifecycle.hpp"
#include "psprecomp/elf32.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using Service=chili::VolatileMemoryRuntime;
constexpr unsigned lock_nid=0x3E0271D3u,unlock_nid=0xA569E425u,out_address=0x08870000u,out_size=0x08870004u;
unsigned assertions{};
void require(bool value,const char *message){++assertions;if(!value)throw std::runtime_error(message);}
void pattern(psprecomp::AllegrexContext &c,unsigned seed){
    for(unsigned i=1;i<c.gpr.size();++i)c.gpr[i]=0xE0000000u+seed*256u+i;
    c.gpr[28]=0x08D9AB00u+seed*16u;c.gpr[29]=0x09FE0000u-seed*4096u;
    c.hi=0x12345678u+seed;c.lo=0x89ABCDEFu;c.fcr31=0x87654321u;
    for(unsigned i=0;i<c.fpr.size();++i)c.fpr[i]=static_cast<float>(i)+seed*0.25f;
    for(unsigned i=0;i<c.vfpu.size();++i)c.vfpu[i]=-static_cast<float>(i)-seed*0.5f;
    c.fpr[0]=std::bit_cast<float>(0x7FC13370u+seed);c.fpr[1]=std::bit_cast<float>(0x80000000u);
    c.vfpu[3]=std::bit_cast<float>(0xFFC13370u+seed);c.vfpu[4]=std::bit_cast<float>(0x80000000u);
    for(unsigned i=0;i<c.vfpu_ctrl.size();++i)c.vfpu_ctrl[i]=0xF0000000u+i+seed*256u;
    c.gpr[31]=0x08860000u+seed*16u;c.pc=0x08D98A58u;
}
bool same_cpu(const psprecomp::AllegrexContext &a,const psprecomp::AllegrexContext &b){
    return a.gpr==b.gpr && a.pc==b.pc && a.hi==b.hi && a.lo==b.lo &&
        std::memcmp(a.fpr.data(),b.fpr.data(),a.fpr.size()*sizeof(float))==0 && a.fcr31==b.fcr31 &&
        std::memcmp(a.vfpu.data(),b.vfpu.data(),a.vfpu.size()*sizeof(float))==0 && a.vfpu_ctrl==b.vfpu_ctrl;
}
void invoke(psprecomp::Runtime &rt,unsigned nid){rt.invoke_import("sceSuspendForUser",nid,rt.cpu());}
struct Fixture{
    psprecomp::Runtime rt;
    std::map<unsigned,unsigned> stacks;
    unsigned stack_top=0x09FE0000u;
    chili::ThreadRuntime threads{rt,[this](unsigned size){
        const auto bottom=stack_top-size;require(rt.memory().contains(bottom,size),"Fixture stack has no real guest backing");
        stacks.emplace(bottom,size);stack_top=bottom;return bottom;
    }};
    std::unique_ptr<Service> service;
    Fixture(Service::ContextQuery query={},bool query_scheduler_state=false){
        if(query_scheduler_state)query=[this]{
            if(threads.in_interrupt())return Service::Context::Interrupt;
            if(!threads.interrupts_enabled())return Service::Context::DispatchDisabled;
            return Service::Context::Normal;
        };
        threads.initialize_bootstrap();pattern(rt.cpu(),1u);service=std::make_unique<Service>(rt,&threads,std::move(query));
    }
    unsigned lock(unsigned mode=0u,unsigned address=out_address,unsigned size=out_size){
        auto &c=rt.cpu();c.gpr[4]=mode;c.gpr[5]=address;c.gpr[6]=size;auto expected=c;
        const auto uid=threads.current_uid();const auto time=threads.now_us();
        invoke(rt,lock_nid);expected.gpr[2]=c.gpr[2];
        require(same_cpu(c,expected) && threads.current_uid()==uid && threads.now_us()==time,"Immediate lock changed context/UID/time");return c.gpr[2];
    }
    unsigned unlock(unsigned mode=0u){rt.cpu().gpr[4]=mode;invoke(rt,unlock_nid);return rt.cpu().gpr[2];}
    void delay(){rt.cpu().gpr[4]=1000u;rt.cpu().gpr[31]=0x08800080u;rt.invoke_import("ThreadManForUser",0xCEADEB47u,rt.cpu());require(!rt.stopped(),"Actual scheduler delay failed");}
    int spawn(unsigned priority){
        auto &c=rt.cpu();rt.memory().zero(0x08850000u,32u);rt.memory().store8(0x08850000u,'v');
        c.gpr[4]=0x08850000u;c.gpr[5]=0x08851000u;c.gpr[6]=priority;c.gpr[7]=4096u;c.gpr[8]=0u;
        rt.invoke_import("ThreadManForUser",0x446D8DE6u,c);const auto uid=static_cast<int>(c.gpr[2]);require(uid>1,"Cannot create real native fixture thread");
        c.gpr[4]=static_cast<unsigned>(uid);c.gpr[5]=0u;c.gpr[6]=0u;rt.invoke_import("ThreadManForUser",0xF475845Du,c);
        require(!rt.stopped() && threads.current_uid()==1,"Spawn unexpectedly switched away from fixture bootstrap");return uid;
    }
    psprecomp::AllegrexContext block_new(unsigned priority,int &uid){
        uid=spawn(priority);delay();require(threads.current_uid()==uid,"Scheduler failed to start expected waiter");
        auto &c=rt.cpu();pattern(c,static_cast<unsigned>(uid));c.gpr[4]=0u;
        c.gpr[5]=out_address+static_cast<unsigned>(uid)*16u;c.gpr[6]=c.gpr[5]+4u;const auto saved=c;
        rt.memory().store32(c.gpr[5],0xDEAD1337u);rt.memory().store32(c.gpr[6],0x1337u);
        invoke(rt,lock_nid);require(!rt.stopped() && threads.current_uid()==1,"Contended lock did not block/yield through real scheduler");
        require(rt.memory().load32(saved.gpr[5])==0xDEAD1337u && rt.memory().load32(saved.gpr[6])==0x1337u,"Contended lock published outputs before acquisition");return saved;
    }
    void resume(const psprecomp::AllegrexContext &saved,unsigned result){
        auto &c=rt.cpu();require(c.pc==Service::resume_pc,"Wait did not use reserved volatile continuation");
        require(rt.invoke_isolated_aot(Service::resume_pc,c),"No volatile continuation function");
        auto expected=saved;expected.gpr[2]=result;expected.pc=expected.gpr[31];
        require(!rt.stopped() && same_cpu(c,expected),"Wait wake/cancellation lost full CPU/FPU/VFPU/GP context");
    }
    void returned(){auto &c=rt.cpu();c.gpr[2]=0u;require(rt.invoke_isolated_aot(chili::ThreadRuntime::return_trampoline,c),"No native thread return");require(!rt.stopped(),"Native fixture thread return failed");}
};
void owned_elf_bounds(const char *path){
    const auto elf=psprecomp::Elf32Image::from_file(path);std::uint64_t arena_begin=psprecomp::kDefaultPspUserLoadBase;unsigned segments{};
    for(std::size_t i=0;i<elf.segments().size();++i){const auto &segment=elf.segments()[i];if(segment.type!=1u)continue;++segments;
        const auto begin=elf.segment_runtime_address(i);const auto end=std::uint64_t(begin)+segment.memory_size;
        require(end<=Service::address||begin>=std::uint64_t(Service::address)+Service::bytes,"Owned ELF load segment collides with fixed volatile memory");arena_begin=std::max(arena_begin,end);
    }
    require(segments!=0u&&arena_begin>=std::uint64_t(Service::address)+Service::bytes&&arena_begin<0x09FF0000u,"Actual main arena policy overlaps volatile memory or exceeds its bound");
    std::cout<<"Owned ELF: "<<segments<<" PT_LOAD segments and max(08804000,segments_end) partition arena do not overlap actual volatile backing PASS\n";
}
void actual_mission_call(){
    Fixture f;auto &m=f.rt.memory();
    constexpr unsigned actual_paddr=0x08DE3698u,actual_psize=0x09FE8810u,actual_ra=0x08844364u;
    std::vector<std::uint8_t> expected(m.size());for(std::size_t i=0;i<expected.size();++i)expected[i]=static_cast<std::uint8_t>((i*29u+17u)&255u);
    m.copy_in(psprecomp::GuestMemory::kPhysicalBase,expected);
    const auto vram=m.vram_bytes();std::vector<std::uint8_t> scratch(psprecomp::GuestMemory::kScratchpadSize);m.copy_out(psprecomp::GuestMemory::kScratchpadBase,scratch);
    auto &c=f.rt.cpu();c.gpr[31]=actual_ra;const auto uid=f.threads.current_uid(),identity=psprecomp::runtime_thread_uid();const auto time=f.threads.now_us(),generation=psprecomp::runtime_thread_switch_generation();
    require(f.lock(0u,actual_paddr,actual_psize)==0u&&!f.rt.stopped(),"Actual mission lock ABI failed");
    auto word=[&](unsigned address,unsigned value){const auto offset=address-psprecomp::GuestMemory::kPhysicalBase;for(unsigned i=0;i<4;++i)expected[offset+i]=static_cast<std::uint8_t>(value>>(i*8u));};
    word(actual_paddr,Service::address);word(actual_psize,Service::bytes);
    require(m.bytes()==expected,"Actual Lock changed guest bytes outside exactly paddr4+psize4");
    std::vector<std::uint8_t> scratch_after(scratch.size());m.copy_out(psprecomp::GuestMemory::kScratchpadBase,scratch_after);
    require(m.vram_bytes()==vram&&scratch_after==scratch,"Actual Lock touched VRAM/scratchpad");
    require(f.threads.current_uid()==uid&&psprecomp::runtime_thread_uid()==identity&&f.threads.now_us()==time&&psprecomp::runtime_thread_switch_generation()==generation,"Actual immediate Lock fabricated UID/time/context switch");
    require(f.service->locked()&&f.service->last_acquiring_uid()==uid&&f.service->waiting()==0u,"Actual Lock has no real exclusive ownership");
    const auto *backing=m.raw_pointer(Service::address,Service::bytes),*ram=m.raw_pointer(psprecomp::GuestMemory::kPhysicalBase,m.size());
    require(backing&&ram&&backing==ram+(Service::address-psprecomp::GuestMemory::kPhysicalBase),"Volatile backing is not the actual immutable RAM mapping");
    require(m.aot_fast_view().aot_load32(Service::address)==m.load32(Service::address)&&m.aot_load32(Service::address+Service::bytes-4u)==m.load32(Service::address+Service::bytes-4u),"AOT fast/slow views do not share real volatile RAM backing");
    auto before=c;before.gpr[4]=0u;before.pc=actual_ra;c.gpr[4]=0u;
    require(f.unlock()==0u&&!f.rt.stopped()&&!f.service->locked(),"Actual normal Unlock failed");before.gpr[2]=0u;
    require(same_cpu(c,before)&&m.bytes()==expected&&f.threads.now_us()==time&&psprecomp::runtime_thread_switch_generation()==generation,"Actual uncontended Unlock changed state outside v0/return PC");
    std::cout<<"Actual first-mission ABI: type0/paddr08DE3698/psize09FE8810/RA08844364, exact8-byte output, bitwise NaN CPU/UID/time preservation PASS\n";
}
void immediate(){
    Fixture f;auto &m=f.rt.memory();
    for(const auto mode:{0xFFFFFFFEu,0xFFFFFFFFu,1u,2u,3u,4u,5u,6u,7u,16u,256u,4096u,0x7FFFFFFFu}){
        m.store32(out_address,0xDEAD1337u);m.store32(out_size,0x1337u);
        require(f.lock(mode)==0x80000107u && !f.service->locked() && m.load32(out_address)==0xDEAD1337u && m.load32(out_size)==0x1337u,
            "Hardware invalid-mode vector failed");
    }
    for(const auto outputs:std::array{std::pair{out_address,out_size},std::pair{0u,out_size},std::pair{out_address,0u},std::pair{0u,0u}}){
        m.store32(out_address,0xDEAD1337u);m.store32(out_size,0x1337u);
        require(f.lock(0u,outputs.first,outputs.second)==0u && f.service->locked(),"Hardware nullable lock output vector failed");
        require(m.load32(out_address)==(outputs.first?Service::address:0xDEAD1337u) && m.load32(out_size)==(outputs.second?Service::bytes:0x1337u),"Lock output address/size mismatch");
        require(f.unlock(1u)==0x80000107u && f.service->locked(),"Invalid unlock mode released resource");
        require(f.unlock()==0u && !f.service->locked() && f.unlock()==0x800201AEu,"Unlock hardware vector failed");
    }
    require(f.lock()==0u,"Real backing lock failed");
    m.store8(Service::address-1u,0x7Bu);m.store8(Service::address+Service::bytes,0xE9u);
    std::vector<unsigned char> fill(Service::bytes,0xA5u);m.copy_in(Service::address,fill);
    require(m.load8(Service::address)==0xA5u && m.load8(Service::address+Service::bytes-1u)==0xA5u,"Returned volatile backing is not real writable guest memory");
    m.zero(m.load32(out_address),m.load32(out_size));m.copy_out(Service::address,fill);
    require(std::all_of(fill.begin(),fill.end(),[](auto x){return x==0u;}) && m.load8(Service::address-1u)==0x7Bu && m.load8(Service::address+Service::bytes)==0xE9u,
        "Actual caller 4-MiB memory clear escaped real volatile backing");
    require(f.unlock()==0u,"Real backing release failed");
    std::cout<<"Hardware immediate vectors: modes/null outputs/unlock errors/full context + real bounded 4-MiB backing PASS\n";
}
void fifo_waits(){
    Fixture f;require(f.lock()==0u,"Owner initial lock failed");std::map<int,psprecomp::AllegrexContext> expected;std::vector<int> order;
    for(const auto priority:{0x31u,0x33u,0x32u}){int uid{};auto saved=f.block_new(priority,uid);expected.emplace(uid,saved);}
    require(f.service->waiting()==3u && f.service->last_acquiring_uid()==1,"FIFO waiter ownership/state mismatch");
    require(f.unlock()==0u && f.service->last_acquiring_uid()==2,"Unlock did not grant first FIFO waiter");f.delay();
    while(f.threads.current_uid()!=1){
        const auto uid=f.threads.current_uid();
        if(f.rt.cpu().pc==Service::resume_pc){
            f.resume(expected.at(uid),0u);order.push_back(uid);
            require(f.service->last_acquiring_uid()==uid,"Waiter woke without actual resource handoff");
            require(f.rt.memory().load32(expected.at(uid).gpr[5])==Service::address && f.rt.memory().load32(expected.at(uid).gpr[6])==Service::bytes,"Waited acquisition did not publish outputs");
            f.rt.cpu().gpr[31]=chili::ThreadRuntime::return_trampoline;f.unlock();
        }else{require(f.rt.cpu().pc==chili::ThreadRuntime::return_trampoline,"Unexpected fixture continuation");f.returned();}
    }
    require(order==std::vector<int>{2,3,4} && !f.service->locked() && f.service->waiting()==0u,"Hardware FIFO order/full teardown failed");
    std::cout<<"Actual scheduler: three FIFO waiters with differing priorities, per-UID full-context resume and preemption PASS\n";
}
void canceled_wait(){
    Fixture f;require(f.lock()==0u,"Cancellation owner lock failed");int uid{};const auto saved=f.block_new(0x31u,uid);
    require(f.threads.wake(uid,0x800201A9u),"Cannot inject real native forced wait release");
    require(f.unlock()==0u && !f.service->locked() && f.service->waiting()==1u,"Unlock raced cancellation incorrectly");
    f.delay();require(f.threads.current_uid()==uid,"Canceled waiter did not run");f.resume(saved,0x800201A9u);
    require(f.service->waiting()==0u,"Canceled volatile ticket was retained");
    auto &c=f.rt.cpu();c.gpr[31]=0x08877774u;f.threads.wait(c,std::numeric_limits<unsigned long long>::max(),{});
    require(f.threads.current_uid()==1,"Generic following wait did not yield");
    require(f.lock()==0u && f.unlock()==0u,"Later owner acquire/release failed");
    require(f.threads.wake(uid,0x80100000u),"Old volatile ticket woke a later unrelated wait");
    f.delay();f.returned();
    require(!f.service->locked() && f.service->waiting()==0u,"Cancellation teardown failed");
    std::cout<<"Cancellation: unlock-before-canceled-continuation, real wake error, preserved context, later unrelated wait PASS\n";
}
void context_errors(){
    Fixture f({},true);auto &c=f.rt.cpu();
    require(f.threads.interrupts_enabled()&&!f.threads.in_interrupt()&&f.threads.can_wait(),"Actual ordinary scheduler context queries failed");
    for(const bool held:{false,true}){
        if(held)require(f.lock()==0u,"Held context setup failed");
        f.rt.invoke_import("Kernel_Library",0x092968F4u,c);const auto flags=c.gpr[2];
        require(!f.threads.interrupts_enabled()&&!f.threads.in_interrupt()&&!f.threads.can_wait(),"Actual disabled-interrupt scheduler queries failed");
        require(f.lock()==0x800201A7u && f.service->locked()==held && f.rt.memory().load32(out_address)==Service::address && f.rt.memory().load32(out_size)==Service::bytes,
            "Disabled-interrupt hardware output/error vector failed");
        c.gpr[4]=flags;f.rt.invoke_import("Kernel_Library",0x5F10D406u,c);
        require(f.threads.enter_interrupt(),"Cannot enter actual fixture interrupt context");
        require(f.threads.interrupts_enabled()&&f.threads.in_interrupt()&&!f.threads.can_wait(),"Actual interrupt scheduler queries failed");
        require(f.lock()==0x80020064u && f.service->locked()==held,"Interrupt hardware output/error vector failed");
        f.threads.leave_interrupt(c);
        require(f.threads.interrupts_enabled()&&!f.threads.in_interrupt()&&f.threads.can_wait(),"Actual interrupt exit scheduler queries failed");
        require(f.unlock()==(held?0u:0x800201AEu),"Context failure changed volatile ownership");
    }
    std::cout<<"Actual disabled-interrupt/interrupt contexts: hardware error codes and address/size outputs, no acquisition PASS\n";
}
void native_contention_and_delete(){
    Fixture f;chili::ThreadLifecycle lifecycle(f.rt,f.threads,[&](unsigned bottom){return f.stacks.erase(bottom)==1u;});
    require(f.service->try_acquire_native(123u),"Native ownership contention setup failed");
    int uid{};const auto saved=f.block_new(0x31u,uid);
    f.rt.cpu().gpr[4]=static_cast<unsigned>(uid);auto expected=f.rt.cpu();expected.gpr[2]=0x800201A4u;
    f.rt.invoke_import("ThreadManForUser",0x9FA03CD3u,f.rt.cpu());
    require(!f.rt.stopped() && same_cpu(f.rt.cpu(),expected) && f.service->waiting()==1u && f.stacks.size()==1u,"DeleteThread discarded an active resource waiter");
    require(!f.service->release_native(456u) && f.service->waiting()==1u && f.service->locked(),"Wrong native token released waiter");
    const auto prior=f.rt.cpu();const auto time=f.threads.now_us();
    require(f.service->release_native(123u) && same_cpu(f.rt.cpu(),prior) && f.threads.current_uid()==1 && f.threads.now_us()==time,"Native release fabricated guest outputs/context/scheduling");
    require(f.service->last_acquiring_uid()==uid && f.service->locked(),"Native release failed actual waiter handoff");
    f.delay();f.resume(saved,0u);require(f.unlock()==0u,"Native-acquired guest release failed");f.returned();
    f.rt.cpu().gpr[4]=static_cast<unsigned>(uid);f.rt.invoke_import("ThreadManForUser",0x9FA03CD3u,f.rt.cpu());
    require(!f.rt.stopped() && f.rt.cpu().gpr[2]==0u && f.stacks.empty() && !f.service->locked() && !f.service->waiting(),"Finished volatile waiter could not be cleanly deleted");
    std::cout<<"Native contention/lifecycle: token ownership, real guest handoff, active DeleteThread rejection, completed stack release PASS\n";
}
void strict_boundaries(){
    for(const auto pointer:{0xDEADBEEFu,out_address+1u,0x0A000000u,out_address|0x80000000u,0x04000000u}){
        Fixture f;f.rt.memory().store32(out_address,0xABCD1234u);f.lock(0u,out_address,pointer);
        require(f.rt.stopped() && !f.service->locked() && f.rt.memory().load32(out_address)==0xABCD1234u,"Unknown invalid output semantics caused partial output/acquisition");
    }
    {Fixture f;f.lock(0u,out_address,out_address);require(f.rt.stopped() && !f.service->locked(),"Aliased-output unknown case silently succeeded");}
    {Fixture f;f.rt.invoke_import("Kernel_Library",0x092968F4u,f.rt.cpu());f.rt.memory().store32(out_address,0xABCD1234u);f.lock();require(f.rt.stopped() && !f.service->locked() && f.rt.memory().load32(out_address)==0xABCD1234u,"Unknown cannot-wait context guessed PSP code/output");}
    {Fixture f;require(!f.service->try_acquire_native(0u) && f.service->try_acquire_native(123u) && !f.service->try_acquire_native(456u),"Native shared resource claim invariant failed");require(!f.service->release_native(456u) && f.service->locked() && f.service->release_native(123u) && !f.service->locked(),"Native token ownership release failed");}
    {Fixture f;require(f.lock()==0u,"Busy destruction setup failed");f.service.reset();require(f.rt.stopped(),"Busy service destruction silently discarded resource ownership");}
    {Fixture f;require(f.lock()==0u,"Stopped destruction setup failed");f.rt.stop("prior native failure");f.service.reset();require(f.rt.stop_reason()=="prior native failure","Busy destruction hid the original native failure");}
    {Fixture f;require(f.service->try_acquire_native(123u),"Pending destruction setup failed");int uid{};f.block_new(0x31u,uid);f.service.reset();require(f.rt.stopped(),"Pending continuation destruction silently succeeded");}
    {Fixture f;f.service.reset();invoke(f.rt,lock_nid);require(f.rt.stopped(),"Destroyed service HLE accessed stale native state");}
    {Fixture f;require(f.rt.invoke_isolated_aot(Service::resume_pc,f.rt.cpu()) && f.rt.stopped(),"Unmatched continuation silently fabricated success");}
    std::cout<<"Strict boundaries: invalid/aliased outputs atomic, unknown context, native owner tokens, busy destruction PASS\n";
}
}
int main(int argc,char **argv){try{if(argc>2)throw std::runtime_error("Usage: chili_volatile_prep_test [owned BOOT.BIN]");if(argc==2)owned_elf_bounds(argv[1]);actual_mission_call();immediate();fifo_waits();canceled_wait();context_errors();native_contention_and_delete();strict_boundaries();std::cout<<"PASS: isolated native volatile-memory foundation ("<<assertions<<" assertions)\n";return 0;}catch(const std::exception &error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}}
