#include "thread_runtime.hpp"
#include "performance_telemetry.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <iostream>
#include <limits>

namespace chili {
ThreadRuntime *ThreadRuntime::active_ = nullptr;

ThreadRuntime::ThreadRuntime(psprecomp::Runtime &runtime,
    std::function<std::uint32_t(std::uint32_t)> stack_allocator)
    : runtime_(runtime), stack_allocator_(std::move(stack_allocator)) {
    if (active_) throw psprecomp::Error("Only one Chili scheduler may be active");
    active_ = this;
    runtime.register_hle("Kernel_Library", 0x092968F4u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2u, interrupts_enabled_ ? 1u : 0u);
            interrupts_enabled_ = false;
        });
    auto resume = [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
        interrupts_enabled_ = (ctx.gpr[4] & 1u) != 0u;
        if (interrupts_enabled_ && !in_interrupt_) {
            ctx.pc = ctx.gpr[31];
            const bool pending = timer_pending_;
            timer_pending_ = false;
            reschedule(ctx, false, pending);
        }
    };
    runtime.register_hle("Kernel_Library", 0x5F10D406u, resume);
    runtime.register_hle("Kernel_Library", 0x3B84732Du, resume);
    runtime.register_function(return_trampoline, &returned, "chili_thread_return");
    auto bind = [&](std::uint32_t nid, auto method) {
        runtime.register_hle("ThreadManForUser", nid,
            [this, method](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { (this->*method)(ctx); });
    };
    bind(0x446D8DE6u, &ThreadRuntime::create);
    bind(0xF475845Du, &ThreadRuntime::start);
    bind(0xCEADEB47u, &ThreadRuntime::delay);
    bind(0x68DA9E36u, &ThreadRuntime::delay_callback);
    runtime.register_function(return_trampoline + 4u, &callback_returned, "chili_callback_return");
    const std::uint32_t callback_nids[]{0xE81CAF8Fu,0xEDBA5844u,0xC11BA8C4u,0xBA4051D6u,0x2A3D44FFu,0x349D6D6Cu,0x730ED8BCu};
    for (unsigned op=0;op<7;++op) runtime.register_hle("ThreadManForUser",callback_nids[op],
        [this,op](auto &,auto &c) { callback_operation(c,op); });
    runtime.register_hle("LoadExecForUser",0x4AC57943u,[this](auto &,auto &c) {
        if (!callbacks_.contains(c.gpr[4])) { c.set_gpr(2,0x800201A1u); return; }
        exit_callback_=c.gpr[4]; c.set_gpr(2,0u);
    });
    runtime.register_hle("scePower",0x04B7766Eu,[this](auto &,auto &c) {
        const auto requested=static_cast<std::int32_t>(c.gpr[4]);const auto callback=c.gpr[5];
        if(requested<-1 || requested>=16){c.set_gpr(2,0x80000102u);return;}
        if(!callbacks_.contains(callback)){c.set_gpr(2,0x80000100u);return;}
        unsigned slot=0;
        if(requested==-1){while(slot<power_callbacks_.size() && power_callbacks_[slot])++slot;
            if(slot==power_callbacks_.size()){c.set_gpr(2,0x80000022u);return;}}
        else {slot=static_cast<unsigned>(requested);if(power_callbacks_[slot]){c.set_gpr(2,0x80000020u);return;}}
        power_callbacks_[slot]=callback;
        // Virtual PC power source: AC, battery present, 100 percent charge.
        // Queue the initial status in the callback's owning thread.
        notify_callback(callback,0x10E4u);
        c.set_gpr(2,requested==-1?slot:0u);
    });
    // Callback-aware event/pool waits remain unbound until they can preserve
    // their resource wait across callback delivery. DelayThreadCB is supported.
    runtime.register_hle("ThreadManForUser", 0x293B45B8u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2u, static_cast<std::uint32_t>(current_)); });
    runtime.register_hle("ThreadManForUser", 0x369ED59Du,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ++clock_us_;
            if (pacing_observer_) pacing_observer_->system_time_low_read();
            ctx.set_gpr(2u, static_cast<std::uint32_t>(clock_us_));
        });
    psprecomp::set_runtime_starvation_hook(&timer, 2048u);
}

ThreadRuntime::~ThreadRuntime() {
    psprecomp::set_runtime_starvation_hook(nullptr, 0u);
    active_ = nullptr;
}

void ThreadRuntime::initialize_bootstrap() {
    Thread thread;
    thread.name = "module_start";
    thread.priority = 0x20u;
    thread.state = State::Running;
    thread.context = runtime_.cpu();
    threads_.emplace(current_, std::move(thread));
    psprecomp::set_runtime_thread_identity(current_, "module_start");
}

void ThreadRuntime::create(psprecomp::AllegrexContext &ctx) {
    const auto requested = ctx.gpr[7];
    if (requested < 0x200u || requested > 0x01000000u) { ctx.set_gpr(2u, 0x80020194u); return; }
    if (ctx.gpr[6] < 8u || ctx.gpr[6] > 119u) { ctx.set_gpr(2u, 0x80020193u); return; }
    if (!ctx.gpr[4] || !runtime_.memory().contains(ctx.gpr[4], 1u) || (ctx.gpr[5] & 3u)) {
        ctx.set_gpr(2u, 0x800200D3u); return;
    }
    const auto size = (requested + 0xFFu) & ~0xFFu;
    const auto bottom = stack_allocator_ ? stack_allocator_(size) :
        (next_stack_top_ >= 0x09000000u + size ? next_stack_top_ - size : 0u);
    if (!bottom) { ctx.set_gpr(2u, 0x80020190u); return; }
    Thread thread;
    thread.name = runtime_.memory().read_c_string(ctx.gpr[4], 32u);
    thread.entry = ctx.gpr[5];
    thread.priority = ctx.gpr[6];
    thread.attributes = ctx.gpr[8];
    thread.top = bottom + size;
    thread.bottom = bottom;
    thread.initial_gp = ctx.gpr[28];
    thread.context.gpr[28] = thread.initial_gp;
    thread.context.vfpu_ctrl[0] = 0xE4u;
    thread.context.vfpu_ctrl[1] = 0xE4u;
    if ((thread.attributes & 0x00100000u) == 0u) {
        const std::vector<std::uint8_t> fill(size, 0xFFu);
        runtime_.memory().copy_in(thread.bottom, fill);
    }
    next_stack_top_ = thread.bottom;
    const auto uid = next_uid_++;
    std::cout << "[thread] create uid=" << uid << " name=" << thread.name
              << " entry=" << psprecomp::hex32(thread.entry) << " stack=" << size << " priority=" << thread.priority << '\n';
    threads_.emplace(uid, std::move(thread));
    ctx.set_gpr(2u, static_cast<std::uint32_t>(uid));
}

void ThreadRuntime::start(psprecomp::AllegrexContext &ctx) {
    const auto found = threads_.find(static_cast<std::int32_t>(ctx.gpr[4]));
    if (found == threads_.end()) { ctx.set_gpr(2u, 0x80020198u); return; }
    auto &thread = found->second;
    if (thread.state != State::Dormant && thread.state != State::Ended) { ctx.set_gpr(2u, 0x800201A4u); return; }
    const auto length = ctx.gpr[5], pointer = ctx.gpr[6];
    if (length > thread.top - thread.bottom - 0x140u ||
        (length && !runtime_.memory().contains(pointer, length))) {
        ctx.set_gpr(2u, 0x800200D3u); return;
    }
    thread.context = {};
    thread.context.gpr[28] = thread.initial_gp;
    thread.context.vfpu_ctrl[0] = 0xE4u;
    thread.context.vfpu_ctrl[1] = 0xE4u;
    thread.exit_status = 0x800201A4u;
    thread.callback_resume.reset();
    thread.active_callback = 0u;
    thread.callback_delay = false;
    thread.on_timeout = {};
    thread.wake_time = 0u;
    std::uint32_t sp = thread.top - 0x100u;
    if (length) {
        sp -= (length + 15u) & ~15u;
        std::vector<std::uint8_t> arguments(length);
        runtime_.memory().copy_out(pointer, arguments);
        runtime_.memory().copy_in(sp, arguments);
        thread.context.gpr[4] = length;
        thread.context.gpr[5] = sp;
    }
    sp -= 64u;
    thread.context.gpr[26] = thread.top - 0x100u;
    thread.context.gpr[29] = sp;
    thread.context.gpr[30] = sp;
    thread.context.gpr[31] = return_trampoline;
    thread.context.pc = thread.entry;
    thread.state = State::Ready;
    thread.ready_order = ++order_;
    ctx.set_gpr(2u, 0u);
    ctx.pc = ctx.gpr[31];
    std::cout << "[thread] start " << thread.name << " args=" << length << '\n';
    reschedule(ctx, false);
}

void ThreadRuntime::delay(psprecomp::AllegrexContext &ctx) {
    if(!can_wait()){ctx.set_gpr(2u,0x800201A7u);return;}
    auto &thread = threads_.at(current_);
    thread.wake_time = clock_us_ + std::max(1u, ctx.gpr[4]);
    thread.state = State::Delayed;
    ctx.set_gpr(2u, 0u);
    ctx.pc = ctx.gpr[31];
    reschedule(ctx, true);
}

void ThreadRuntime::finish(psprecomp::AllegrexContext &ctx) {
    auto &thread = threads_.at(current_);
    thread.exit_status = ctx.gpr[2];
    thread.state = State::Ended;
    thread.context = ctx;
    thread.callback_resume.reset();
    thread.active_callback = 0u;
    thread.callback_delay = false;
    thread.on_timeout = {};
    thread.wake_time = 0u;
    if (lifecycle_end_hook_) lifecycle_end_hook_(current_,thread.exit_status);
    std::cout << "[thread] return " << thread.name << " status=" << thread.exit_status << '\n';
    reschedule(ctx, true);
}

void ThreadRuntime::reschedule(psprecomp::AllegrexContext &ctx, bool force, bool timeslice) {
    if(in_interrupt_)return;
    if (!force && !interrupts_enabled_) return;
    auto &caller = threads_.at(current_);
    auto best = threads_.end();
    auto find_ready = [&] {
        for (auto it = threads_.begin(); it != threads_.end(); ++it) {
            auto &thread = it->second;
            if (thread.state == State::Waiting && thread.wake_time <= clock_us_) {
                if (thread.on_timeout) thread.on_timeout();
                thread.on_timeout = {};
                thread.context.set_gpr(2u, 0x800201A8u);
                if (it->first == current_) ctx.set_gpr(2u, 0x800201A8u);
                thread.state = State::Ready;
                thread.ready_order = ++order_;
            }
            if (thread.state == State::Delayed && thread.wake_time <= clock_us_) {
                thread.callback_delay = false;
                thread.state = State::Ready;
                thread.ready_order = ++order_;
            }
            if (thread.state != State::Ready) continue;
            if (best == threads_.end() || thread.priority < best->second.priority ||
                (thread.priority == best->second.priority && thread.ready_order < best->second.ready_order)) best = it;
        }
    };
    find_ready();
    if (force && best == threads_.end()) {
        auto next_wake = std::numeric_limits<std::uint64_t>::max();
        for (const auto &[uid, thread] : threads_)
            if (thread.state == State::Delayed || thread.state == State::Waiting)
                next_wake = std::min(next_wake, thread.wake_time);
        if (next_wake != std::numeric_limits<std::uint64_t>::max()) {
            if (pacing_observer_) pacing_observer_->idle_deadline_jump(next_wake - clock_us_);
            if (idle_hook_) idle_hook_(next_wake);
            clock_us_ = next_wake;
            find_ready();
        }
    }
    if (best == threads_.end()) {
        if (force) runtime_.stop("No runnable Chili threads");
        return;
    }
    if (!force && (best->second.priority > caller.priority ||
                   (best->second.priority == caller.priority && !timeslice))) return;
    caller.context = ctx;
    if (caller.state == State::Running) {
        caller.state = State::Ready;
        caller.ready_order = ++order_;
    }
    current_ = best->first;
    best->second.state = State::Running;
    ctx = best->second.context;
    psprecomp::set_runtime_thread_identity(current_, best->second.name);
    if (best->second.callback_delay) deliver_callback(ctx);
}

void ThreadRuntime::wait(psprecomp::AllegrexContext &ctx, std::uint64_t deadline, std::function<void()> on_timeout) {
    auto &thread = threads_.at(current_);
    ctx.set_gpr(2u, 0u);
    ctx.pc = ctx.gpr[31];
    thread.context = ctx;
    thread.state = State::Waiting;
    thread.wake_time = deadline;
    thread.on_timeout = std::move(on_timeout);
    reschedule(ctx, true);
}

bool ThreadRuntime::wake(std::int32_t uid, std::uint32_t result) {
    auto found = threads_.find(uid);
    if (found == threads_.end() || found->second.state != State::Waiting) return false;
    auto &thread = found->second;
    thread.context.set_gpr(2u, result);
    thread.state = State::Ready;
    thread.ready_order = ++order_;
    thread.on_timeout = {};
    return true;
}

void ThreadRuntime::returned(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { active_->finish(ctx); }
void ThreadRuntime::notify_callback(std::uint32_t uid, std::uint32_t argument) {
    auto &cb=callbacks_.at(uid);
    if (cb.count < 0x7FFFFFFFu) ++cb.count;
    cb.argument=argument;
    auto &owner=threads_.at(cb.owner);
    if (owner.state==State::Delayed && owner.callback_delay) {
        owner.state=State::Ready; owner.ready_order=++order_;
    }
}
void ThreadRuntime::request_exit_callback() {
    if (callbacks_.contains(exit_callback_)) notify_callback(exit_callback_,0u);
}
bool ThreadRuntime::deliver_callback(psprecomp::AllegrexContext &ctx) {
    auto &thread=threads_.at(current_);
    if (thread.callback_resume) return false;
    for(auto &[uid,cb]:callbacks_) {
        if(cb.owner!=current_ || !cb.count) continue;
        const auto sp=ctx.gpr[29];
        if(sp<thread.bottom+64u || !runtime_.memory().contains(sp-64u,64u)) {
            runtime_.stop("Callback stack has insufficient space"); return false;
        }
        thread.callback_resume=ctx;thread.active_callback=uid;
        ctx.gpr[4]=cb.count;ctx.gpr[5]=cb.argument;ctx.gpr[6]=cb.common;
        cb.count=0;cb.argument=0;
        ctx.gpr[28]=cb.gp;ctx.gpr[29]=sp-64u;ctx.gpr[31]=return_trampoline+4u;ctx.pc=cb.entry;
        return true;
    }
    return false;
}
void ThreadRuntime::delay_callback(psprecomp::AllegrexContext &ctx) {
    if(!can_wait()) {ctx.set_gpr(2,0x800201A7u);return;}
    auto &thread=threads_.at(current_);
    if(thread.callback_resume) {runtime_.stop("Callback-aware delay inside an active callback requires nested wait support");return;}
    thread.wake_time=clock_us_+std::max(1u,ctx.gpr[4]);thread.callback_delay=true;
    ctx.set_gpr(2,0u);ctx.pc=ctx.gpr[31];
    if(deliver_callback(ctx)) return;
    thread.state=State::Delayed;reschedule(ctx,true);
}
void ThreadRuntime::callback_returned(psprecomp::Runtime &,psprecomp::AllegrexContext &ctx) {
    auto &self=*active_;auto &thread=self.threads_.at(self.current_);
    if(!thread.callback_resume) {self.runtime_.stop("Callback return without saved continuation");return;}
    if(ctx.gpr[2]) {self.callbacks_.erase(thread.active_callback);if(self.exit_callback_==thread.active_callback)self.exit_callback_=0;}
    ctx=*thread.callback_resume;thread.callback_resume.reset();thread.active_callback=0;
    if(self.deliver_callback(ctx)) return;
    if(thread.callback_delay) {
        if(thread.wake_time>self.clock_us_) {thread.state=State::Delayed;self.reschedule(ctx,true);}
        else thread.callback_delay=false;
    }
}
void ThreadRuntime::callback_operation(psprecomp::AllegrexContext &ctx,unsigned op) {
    if(op==0u) {
        if(!ctx.gpr[4] || !runtime_.memory().contains(ctx.gpr[4],1u) || (ctx.gpr[5]&3u) || !runtime_.memory().contains(ctx.gpr[5],4u)) {ctx.set_gpr(2,0x800200D3u);return;}
        auto uid=next_callback_++; callbacks_.emplace(uid,Callback{runtime_.memory().read_c_string(ctx.gpr[4],32u),current_,ctx.gpr[5],ctx.gpr[6],ctx.gpr[28]});ctx.set_gpr(2,uid);return;
    }
    if(op==5u) {ctx.set_gpr(2,1u);ctx.pc=ctx.gpr[31];if(!deliver_callback(ctx))ctx.set_gpr(2,0u);return;}
    auto found=callbacks_.find(ctx.gpr[4]);
    if(found==callbacks_.end()) {ctx.set_gpr(2,0x800201A1u);return;}
    const auto uid=found->first;auto &cb=found->second;
    if(op==1u) {callbacks_.erase(found);if(exit_callback_==uid)exit_callback_=0;ctx.set_gpr(2,0u);return;}
    if(op==2u) {notify_callback(uid,ctx.gpr[5]);ctx.set_gpr(2,0u);dispatch_ready(ctx);return;}
    if(op==3u) {cb.count=0;cb.argument=0;ctx.set_gpr(2,0u);return;}
    if(op==4u) {ctx.set_gpr(2,cb.count);return;}
    const auto output=ctx.gpr[5];
    if(!output || !runtime_.memory().contains(output,4u)) {ctx.set_gpr(2,0x800200D3u);return;}
    const auto bytes=std::min(56u,runtime_.memory().load32(output));
    if(!runtime_.memory().contains(output,bytes)) {ctx.set_gpr(2,0x800200D3u);return;}
    std::vector<std::uint8_t> data(56u,0u);
    auto word=[&](unsigned offset,std::uint32_t value){for(unsigned i=0;i<4;++i)data[offset+i]=static_cast<std::uint8_t>(value>>(i*8));};
    word(0,56u);for(std::size_t i=0;i<std::min<std::size_t>(31u,cb.name.size());++i)data[4+i]=cb.name[i];
    word(36,static_cast<std::uint32_t>(cb.owner));word(40,cb.entry);word(44,cb.common);word(48,cb.count);word(52,cb.argument);
    runtime_.memory().copy_in(output,std::span<const std::uint8_t>(data.data(),bytes));ctx.set_gpr(2,0u);
}
void ThreadRuntime::timer(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    active_->clock_us_ += 100u;
    if (active_->pacing_observer_) active_->pacing_observer_->scheduler_timer_tick();
    if (active_->tick_hook_) active_->tick_hook_();
    if (!active_->can_wait()) { active_->timer_pending_ = true; return; }
    active_->reschedule(ctx, false, true);
}
void ThreadRuntime::leave_interrupt(psprecomp::AllegrexContext &ctx){
    in_interrupt_=false;
    if(interrupts_enabled_){const bool pending=timer_pending_;timer_pending_=false;reschedule(ctx,false,pending);}
}

}
