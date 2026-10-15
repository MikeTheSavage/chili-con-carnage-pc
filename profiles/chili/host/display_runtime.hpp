#pragma once
#include "psprecomp/runtime.hpp"
#include "thread_runtime.hpp"
#include "performance_telemetry.hpp"
#include "host_log.hpp"
#include <functional>
#include <map>
#include <iostream>
namespace chili {
class DisplayRuntime {
    static inline DisplayRuntime *active_{};
    psprecomp::Runtime &runtime_;
    ThreadRuntime &threads_;
    PerformanceTelemetry *telemetry_{};
    PerformanceTelemetry *pacing_observer_{};
    std::map<int,psprecomp::AllegrexContext> waiting_;
    bool configured_{};
    struct Framebuffer {unsigned address{},stride{512},format{3};};
    Framebuffer current_,latched_;
    unsigned long long latch_time_{};
    bool pending_latch_{};
    const bool log_{log_enabled(LogChannel::Display)};
    unsigned frame_return_{};
    std::function<void(psprecomp::AllegrexContext &)> frame_observer_;
    std::function<void(unsigned)> framebuffer_observer_;
    void update_latch(){if(pending_latch_ && threads_.now_us()>=latch_time_){
        current_=latched_;pending_latch_=false;
        if(framebuffer_observer_)framebuffer_observer_(current_.address);
        if(telemetry_)telemetry_->display_apply({current_.address,current_.stride,current_.format},true,latch_time_,threads_.now_us());
    }}

    void wait(psprecomp::AllegrexContext &c,bool callbacks){
        if(!threads_.can_wait()){c.set_gpr(2,0x800201A7u);return;}
        const auto now=threads_.now_us(),deadline=next_vblank(now);
        if(pacing_observer_)pacing_observer_->vblank_wait(deadline-now);
        waiting_[threads_.current_uid()]=c;c.gpr[4]=static_cast<unsigned>(deadline-now);c.gpr[31]=trampoline;
        runtime_.invoke_import("ThreadManForUser",callbacks?0x68DA9E36u:0xCEADEB47u,c);
    }
    static void resumed(psprecomp::Runtime &r,psprecomp::AllegrexContext &c){
        auto &self=*active_;auto i=self.waiting_.find(self.threads_.current_uid());
        if(i==self.waiting_.end()){r.stop("Vblank return without pending wait");return;}
        self.update_latch();c=i->second;self.waiting_.erase(i);c.set_gpr(2,0);c.pc=c.gpr[31];
    }
public:
    // Setup-time observer of Display-layer wait requests; no added clock read.
    void set_pacing_observer(PerformanceTelemetry *observer) noexcept {pacing_observer_=observer;}
    // Game frame boundary: `observer` runs before each vblank wait whose guest
    // return address is `return_address` (the engine's once-per-frame call).
    void set_frame_boundary(unsigned return_address,std::function<void(psprecomp::AllegrexContext &)> observer){
        frame_return_=return_address;frame_observer_=std::move(observer);
    }
    // Runs whenever the displayed framebuffer address is applied (at once, or at
    // the vblank a latched request takes effect).
    void set_framebuffer_observer(std::function<void(unsigned address)> observer){framebuffer_observer_=std::move(observer);}
    struct Snapshot {unsigned address,stride,format;bool configured;};
    Snapshot snapshot(){update_latch();return {current_.address,current_.stride,current_.format,configured_};}
    unsigned framebuffer_address(){return snapshot().address;}
    static constexpr unsigned trampoline=0x08000010u;
    static unsigned long long next_vblank(unsigned long long now){
        auto n=now*60/1001000+1;auto deadline=n*1001000/60;
        if(deadline<=now)deadline=(n+1)*1001000/60;return deadline;
    }
    DisplayRuntime(psprecomp::Runtime &rt,ThreadRuntime &threads,PerformanceTelemetry *telemetry=nullptr):runtime_(rt),threads_(threads),telemetry_(telemetry){
        if(active_)throw std::runtime_error("Only one Chili display scheduler supported");active_=this;
        rt.register_function(trampoline,&resumed,"chili_vblank_return");
        rt.register_hle("sceDisplay",0x46F186C3u,[this](auto &r,auto &c){
            if(frame_observer_ && c.gpr[31]==frame_return_){frame_observer_(c);if(r.stopped())return;}
            wait(c,true);
        });
        rt.register_hle("sceDisplay",0x289D82FEu,[this](auto &r,auto &c){
            update_latch();const auto address=c.gpr[4],stride=c.gpr[5],format=c.gpr[6],sync=c.gpr[7];
            if(sync>1){c.set_gpr(2,0x80000107u);return;}
            const auto physical=psprecomp::GuestMemory::canonical(address);
            if((address&15) || (address && (!r.memory().contains(address,1) || physical<0x04000000u))){c.set_gpr(2,0x80000103u);return;}
            if((stride&63) || (!stride && address)){c.set_gpr(2,0x80000104u);return;}
            if(format>3){c.set_gpr(2,0x80000108u);return;}
            if(sync==0 && (stride!=latched_.stride || format!=latched_.format)){c.set_gpr(2,0x80000107u);return;}
            if(telemetry_)telemetry_->display_request(sync!=0,pending_latch_);
            latched_={address,stride,format};
            if(sync==0){current_=latched_;pending_latch_=false;
                if(framebuffer_observer_)framebuffer_observer_(current_.address);
                if(telemetry_)telemetry_->display_apply({current_.address,current_.stride,current_.format},false,threads_.now_us(),threads_.now_us());
            }
            else{current_.stride=stride;current_.format=format;pending_latch_=true;latch_time_=next_vblank(threads_.now_us());}
            if(log_)std::cout<<"[display] framebuffer="<<address<<" stride="<<stride<<" format="<<format<<" sync="<<sync<<'\n';
            c.set_gpr(2,0);
        });
        rt.register_hle("sceDisplay",0x0E20F177u,[this](auto &,auto &c){
            if(c.gpr[4]!=0){c.set_gpr(2,0x80000107u);return;}
            if(c.gpr[5]!=480 || c.gpr[6]!=272){c.set_gpr(2,0x80000104u);return;}
            configured_=true;if(log_)std::cout<<"[display] LCD mode480x272\n";wait(c,false);
        });
    }
    ~DisplayRuntime(){if(active_==this)active_=nullptr;}
};
}
