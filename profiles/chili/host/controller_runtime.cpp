#include "controller_runtime.hpp"
#include "controller_sampling_provenance.hpp"
#include "pc_input.hpp"
#include "psprecomp/common.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <Xinput.h>
#include <algorithm>
#include <iostream>
namespace chili {
ControllerRuntime *ControllerRuntime::active_{};
ControllerInput poll_host_controller(){return PcInput::instance().sample();}
ControllerRuntime::ControllerRuntime(psprecomp::Runtime &rt,ThreadRuntime &threads,std::function<ControllerInput()> input):runtime_(rt),threads_(threads),input_(std::move(input)){
    if(active_)throw psprecomp::Error("Only one Chili controller runtime may be active");active_=this;
    rt.register_hle("sceCtrl",0x1F4011E6u,[this](auto &,auto &c){if(c.gpr[4]>1u){c.set_gpr(2,0x80000107u);return;}const auto old=mode_;mode_=c.gpr[4];c.set_gpr(2,old);std::cout<<"[ctrl] mode="<<mode_<<'\n';});
    rt.register_hle("sceCtrl",0x1F803938u,[this](auto &,auto &c){read(c);});
    rt.register_function(ThreadRuntime::return_trampoline+8u,&resumed,"chili_controller_resume");
}
ControllerRuntime::~ControllerRuntime(){active_=nullptr;}
void ControllerRuntime::read(psprecomp::AllegrexContext &c){
    const auto output=c.gpr[4],count=c.gpr[5];
    if(!count || count>64u){c.set_gpr(2,0x80000104u);return;}
    if(count!=1u){runtime_.stop("Controller multi-sample ring reads require buffered sampling support");return;}
    if(!output || (output&3u) || !runtime_.memory().contains(output,16u)){c.set_gpr(2,0x800200D3u);return;}
    if(!threads_.can_wait()){c.set_gpr(2,0x800201A7u);return;}
    const auto now=threads_.now_us();
    // The 59.94 Hz vblank grid of DisplayRuntime::next_vblank.
    const auto vblank_time=[](std::uint64_t index){return index*1001000u/60u;};
    auto interval=now*60u/1001000u;
    if(vblank_time(interval+1u)<=now)++interval;
    const auto wake=vblank_latch_?(sampled_ && interval<=sampled_vblank_?vblank_time(sampled_vblank_+1u):0u):next_sample_;
    if(now<wake){
        pending_[threads_.current_uid()]=c;c.gpr[4]=static_cast<std::uint32_t>(wake-now);c.gpr[31]=ThreadRuntime::return_trampoline+8u;
        runtime_.invoke_import("ThreadManForUser",0xCEADEB47u,c);return;
    }
    const auto input=input_();std::array<std::uint8_t,16> sample{};
    auto word=[&](unsigned offset,std::uint32_t value){for(unsigned i=0;i<4;++i)sample[offset+i]=static_cast<std::uint8_t>(value>>(8*i));};
    word(0,static_cast<std::uint32_t>(now));word(4,input.buttons&0xF3F9u);sample[8]=mode_?input.x:128u;sample[9]=mode_?input.y:128u;
    runtime_.memory().copy_in(output,sample);next_sample_=now+16667u;sampled_=true;sampled_vblank_=interval;c.set_gpr(2,1u);
    if(sampling_observer_)sampling_observer_->sampled({ControllerSamplingProvenance::ReadKind::ReadBufferPositive,
        input.buttons&0xF3F9u,1u,output,c.gpr[31],sample[8],sample[9],threads_.current_uid(),now,mode_!=0u});
}
void ControllerRuntime::resumed(psprecomp::Runtime &,psprecomp::AllegrexContext &c){
    auto &self=*active_;auto found=self.pending_.find(self.threads_.current_uid());
    if(found==self.pending_.end()){self.runtime_.stop("Controller return without pending read");return;}
    c=found->second;self.pending_.erase(found);c.pc=c.gpr[31];self.read(c);
}
}
