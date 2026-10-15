#include "volatile_memory_runtime.hpp"
#include "thread_runtime.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <unordered_map>

namespace chili {
struct VolatileMemoryRuntime::State {
    static constexpr unsigned invalid_mode = 0x80000107u;
    static constexpr unsigned not_locked = 0x800201AEu;
    static constexpr unsigned cannot_wait = 0x800201A7u;
    static constexpr unsigned illegal_context = 0x80020064u;
    enum class Holder { None, Guest, Native };
    struct Wait {
        std::uint64_t ticket;
        std::uint32_t out_address, out_size;
        psprecomp::AllegrexContext saved;
        bool granted{};
    };
    psprecomp::Runtime &runtime;
    ThreadRuntime *threads;
    ContextQuery context_query;
    Holder holder{Holder::None};
    std::int32_t acquiring_uid{};
    std::uint64_t native_token{}, next_ticket{1};
    std::map<std::int32_t, Wait> waits;
    std::deque<std::pair<std::int32_t,std::uint64_t>> fifo;
    inline static std::unordered_map<psprecomp::Runtime *,std::weak_ptr<State>> instances;

    State(psprecomp::Runtime &rt, ThreadRuntime *scheduler, ContextQuery query)
        : runtime(rt), threads(scheduler), context_query(std::move(query)) {}

    bool backing() {
        if (runtime.memory().contains(address, bytes)) return true;
        runtime.stop("Volatile memory requires real contiguous guest backing at 0x08400000 for 4 MiB");
        return false;
    }
    Context context() const {
        if (context_query) return context_query();
        if (!threads || threads->can_wait()) return Context::Normal;
        return Context::UnknownCannotWait;
    }
    bool outputs(unsigned out_address,unsigned out_size) {
        for (const auto pointer : {out_address,out_size}) {
            const auto physical=psprecomp::GuestMemory::canonical(pointer);
            if (pointer && ((pointer & 0x80000003u) || physical<psprecomp::GuestMemory::kPhysicalBase ||
                std::uint64_t(physical)+4u>std::uint64_t(psprecomp::GuestMemory::kPhysicalBase)+runtime.memory().size() ||
                !runtime.memory().contains(pointer,4u))) {
                runtime.stop("Volatile memory invalid non-null output address: PSP fault semantics remain unsupported");
                return false;
            }
        }
        if (out_address && out_size && psprecomp::GuestMemory::canonical(out_address)==psprecomp::GuestMemory::canonical(out_size)) {
            runtime.stop("Volatile memory aliased output pointers require hardware validation"); return false;
        }
        return true;
    }
    void publish(unsigned out_address,unsigned out_size) {
        if (out_address) runtime.memory().store32(out_address,address);
        if (out_size) runtime.memory().store32(out_size,bytes);
    }
    void acquire_guest(std::int32_t uid) {
        holder=Holder::Guest; acquiring_uid=uid; native_token=0;
    }
    void lock(psprecomp::AllegrexContext &c) {
        const auto mode=c.gpr[4], out_address=c.gpr[5], out_size=c.gpr[6];
        const auto current_context=context();
        if (current_context != Context::Normal) {
            // Hardware vectors cover mode 0 with valid/null outputs in both
            // contexts. Other precedence combinations are not inferred.
            if (current_context==Context::UnknownCannotWait || mode!=0u) {
                runtime.stop("Volatile memory non-waitable context/validation precedence requires explicit native context semantics"); return;
            }
            if (!backing() || !outputs(out_address,out_size)) return;
            publish(out_address,out_size);
            c.set_gpr(2,current_context==Context::Interrupt ? illegal_context : cannot_wait);
            return;
        }
        if (mode!=0u) { c.set_gpr(2,invalid_mode); return; }
        if (threads && !threads->can_wait()) {
            runtime.stop("Volatile memory context query contradicts the native scheduler"); return;
        }
        if (!backing() || !outputs(out_address,out_size)) return;
        const auto uid=threads ? threads->current_uid() : 0;
        if (holder==Holder::None) {
            publish(out_address,out_size); acquire_guest(uid); c.set_gpr(2,0); return;
        }
        if (!threads) { runtime.stop("Contended volatile memory lock requires native scheduler"); return; }
        if (waits.contains(uid)) { runtime.stop("Nested volatile memory wait for the same thread requires lifecycle support"); return; }
        const auto ticket=next_ticket++;
        waits.emplace(uid,Wait{ticket,out_address,out_size,c}); fifo.emplace_back(uid,ticket);
        // A dedicated continuation consumes *any* real wake result. If a wait
        // is force-released, its ticket is removed before that thread can wait
        // on another service. Full CPU/FPU/VFPU/GP state is retained.
        c.gpr[31]=resume_pc;
        threads->wait(c,std::numeric_limits<std::uint64_t>::max(),{});
    }
    void grant_next() {
        while (holder==Holder::None && !fifo.empty()) {
            const auto [uid,ticket]=fifo.front();
            const auto found=waits.find(uid);
            if (found==waits.end() || found->second.ticket!=ticket) { fifo.pop_front(); continue; }
            auto &wait=found->second;
            if (!backing() || !outputs(wait.out_address,wait.out_size)) return;
            fifo.pop_front();
            // A cancellation can make the thread Ready before its dedicated
            // continuation runs. Preserve its saved context until that real
            // wake result is consumed; never wake a subsequent unrelated wait.
            if (!threads->wake(uid,0u)) continue;
            publish(wait.out_address,wait.out_size); acquire_guest(uid); wait.granted=true;
        }
    }
    void unlock(psprecomp::AllegrexContext &c) {
        if (c.gpr[4]!=0u) { c.set_gpr(2,invalid_mode); return; }
        if (context()!=Context::Normal) {
            runtime.stop("Volatile memory unlock in a non-waitable context requires hardware validation"); return;
        }
        if (holder==Holder::None) { c.set_gpr(2,not_locked); return; }
        if (holder==Holder::Native) {
            runtime.stop("Guest volatile unlock cannot release an active native utility claim"); return;
        }
        // Hardware/reference semantics use one global semaphore, not a
        // recursive mutex. The recorded UID is diagnostic, not an owner check.
        holder=Holder::None; acquiring_uid=0;
        grant_next();
        if (runtime.stopped()) return;
        c.set_gpr(2,0u);
        if (threads && threads->can_wait()) threads->dispatch_ready(c);
    }
    static void resume(psprecomp::Runtime &rt,psprecomp::AllegrexContext &c) {
        const auto instance=instances.find(&rt);
        auto self=instance==instances.end() ? std::shared_ptr<State>{} : instance->second.lock();
        if (!self || !self->threads) { rt.stop("Volatile resume has no live native service"); return; }
        const auto uid=self->threads->current_uid();
        const auto found=self->waits.find(uid);
        if (found==self->waits.end()) { rt.stop("Volatile resume has no saved thread context"); return; }
        auto wait=found->second;
        const auto result=c.gpr[2];
        if ((!wait.granted && result==0u) || (wait.granted && result!=0u)) {
            rt.stop("Volatile wait resumed with a result inconsistent with resource handoff"); return;
        }
        self->waits.erase(found);
        std::erase_if(self->fifo,[&](auto entry){return entry.first==uid && entry.second==wait.ticket;});
        c=wait.saved; c.set_gpr(2,result); c.pc=c.gpr[31];
    }
};

VolatileMemoryRuntime::VolatileMemoryRuntime(psprecomp::Runtime &rt,ThreadRuntime *threads,ContextQuery query)
    : state_(std::make_shared<State>(rt,threads,std::move(query))) {
    if (State::instances.contains(&rt) && !State::instances[&rt].expired())
        throw psprecomp::Error("Duplicate native volatile memory service for one Runtime");
    State::instances[&rt]=state_;
    rt.register_function(resume_pc,&State::resume,"chili_volatile_memory_resume");
    const std::weak_ptr<State> weak=state_;
    rt.register_hle("sceSuspendForUser",0x3E0271D3u,[weak](auto &r,auto &c){
        if(auto state=weak.lock()) state->lock(c); else r.stop("Volatile lock service has been destroyed");
    });
    rt.register_hle("sceSuspendForUser",0xA569E425u,[weak](auto &r,auto &c){
        if(auto state=weak.lock()) state->unlock(c); else r.stop("Volatile unlock service has been destroyed");
    });
}
VolatileMemoryRuntime::~VolatileMemoryRuntime() {
    if (!state_->runtime.stopped() && (!state_->waits.empty() || state_->holder!=State::Holder::None))
        state_->runtime.stop("Native volatile memory service destroyed with live resource ownership or guest continuations");
    State::instances.erase(&state_->runtime);
}
bool VolatileMemoryRuntime::locked() const { return state_->holder!=State::Holder::None; }
std::int32_t VolatileMemoryRuntime::last_acquiring_uid() const { return state_->acquiring_uid; }
std::size_t VolatileMemoryRuntime::waiting() const { return state_->waits.size(); }
bool VolatileMemoryRuntime::try_acquire_native(std::uint64_t token) {
    if (!token || !state_->backing() || state_->holder!=State::Holder::None) return false;
    state_->holder=State::Holder::Native; state_->native_token=token; state_->acquiring_uid=0; return true;
}
bool VolatileMemoryRuntime::release_native(std::uint64_t token) {
    if (state_->holder!=State::Holder::Native || token!=state_->native_token) return false;
    state_->holder=State::Holder::None; state_->native_token=0; state_->grant_next();
    return !state_->runtime.stopped();
}
}
