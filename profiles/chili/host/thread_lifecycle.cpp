#include "thread_lifecycle.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <limits>

namespace chili {
ThreadLifecycle::ThreadLifecycle(psprecomp::Runtime &rt, ThreadRuntime &threads,
    std::function<bool(std::uint32_t)> release_stack, const std::uint32_t *compiled_sdk_version)
    : runtime_(rt), threads_(threads), release_stack_(std::move(release_stack)),
      compiled_sdk_version_(compiled_sdk_version) {
    if (threads_.lifecycle_end_hook_) throw psprecomp::Error("Thread lifecycle already installed");
    if (!release_stack_) throw psprecomp::Error("Thread lifecycle requires a stack release callback");
    threads_.lifecycle_end_hook_ = [this](std::int32_t target,std::uint32_t status) { on_ended(target,status); };
    rt.register_hle("ThreadManForUser", 0x278C0DF5u, [this](auto &,auto &c) { join(c); });
    rt.register_hle("ThreadManForUser", 0xAA73C935u, [this](auto &,auto &c) { exit(c); });
    rt.register_hle("ThreadManForUser", 0x9FA03CD3u, [this](auto &,auto &c) { remove(c); });
    // ReferThreadRunStatus remains unbound: the current scheduler has no
    // resource wait tags or per-thread run/preemption/release accounting.
}
ThreadLifecycle::~ThreadLifecycle() { threads_.lifecycle_end_hook_ = {}; }

void ThreadLifecycle::join(psprecomp::AllegrexContext &c) {
    const auto target = static_cast<std::int32_t>(c.gpr[4]);
    const auto timeout = c.gpr[5];
    if (!target || target == threads_.current_) { c.set_gpr(2,0x80020197u); return; }
    if (threads_.in_interrupt_) { c.set_gpr(2,0x80020064u); return; }
    if (!threads_.can_wait()) { c.set_gpr(2,0x800201A7u); return; }
    const auto found = threads_.threads_.find(target);
    if (found == threads_.threads_.end()) { c.set_gpr(2,0x80020198u); return; }
    if (timeout && ((timeout & 3u) || !runtime_.memory().contains(timeout,4u))) {
        c.set_gpr(2,0x800200D3u); return;
    }
    const auto &thread = found->second;
    if (thread.state == ThreadRuntime::State::Dormant) { c.set_gpr(2,0x800201A2u); return; }
    if (thread.state == ThreadRuntime::State::Ended) { c.set_gpr(2,thread.exit_status); return; }
    const auto duration = timeout ? runtime_.memory().load32(timeout) : 0u;
    if (timeout && !duration) { c.set_gpr(2,0x800201A8u); return; }
    const auto deadline = timeout ? threads_.now_us() + duration : std::numeric_limits<std::uint64_t>::max();
    const auto waiter = threads_.current_;
    joins_[target].push_back(Join{waiter,timeout,deadline});
    threads_.wait(c,deadline,[this,target,waiter,timeout] {
        auto entry = joins_.find(target);
        if (entry != joins_.end()) {
            std::erase_if(entry->second,[waiter](const Join &j) { return j.waiter == waiter; });
            if (entry->second.empty()) joins_.erase(entry);
        }
        if (timeout) runtime_.memory().store32(timeout,0u);
    });
}

void ThreadLifecycle::on_ended(std::int32_t target, std::uint32_t status) {
    auto found = joins_.find(target);
    if (found == joins_.end()) return;
    const auto waiting = std::move(found->second);
    joins_.erase(found);
    for (const auto &j : waiting) {
        // This record is removed by timeout, so it cannot wake a later wait.
        if (!threads_.wake(j.waiter,status)) continue;
        if (j.timeout) runtime_.memory().store32(j.timeout,static_cast<std::uint32_t>(
            j.deadline > threads_.now_us() ? j.deadline - threads_.now_us() : 0u));
    }
}

void ThreadLifecycle::exit(psprecomp::AllegrexContext &c) {
    if (threads_.in_interrupt_) { c.set_gpr(2,0x80020064u); return; }
    if (!threads_.can_wait()) {
        if (compiled_sdk_version_ && *compiled_sdk_version_ < 0x03080000u) {
            runtime_.stop("ExitThread with interrupts disabled on an older SDK requires verified interrupt restoration");
        } else c.set_gpr(2,0x800201A7u);
        return;
    }
    // PSP user ExitThread normalizes negative arguments; the normal return
    // trampoline retains the raw signed return value instead.
    c.set_gpr(2,static_cast<std::int32_t>(c.gpr[4]) < 0 ? 0x800200D2u : c.gpr[4]);
    threads_.finish(c);
    // No writes to c after finish(): c may now belong to a different thread.
}

void ThreadLifecycle::remove(psprecomp::AllegrexContext &c) {
    const auto uid = static_cast<std::int32_t>(c.gpr[4]);
    if (!uid || uid == threads_.current_) { c.set_gpr(2,0x800201A4u); return; }
    auto found = threads_.threads_.find(uid);
    if (found == threads_.threads_.end()) { c.set_gpr(2,0x80020198u); return; }
    const auto &thread = found->second;
    if (thread.state != ThreadRuntime::State::Dormant && thread.state != ThreadRuntime::State::Ended) {
        c.set_gpr(2,0x800201A4u); return;
    }
    if (!thread.bottom || thread.top <= thread.bottom) {
        runtime_.stop("DeleteThread requires an owned stack allocation"); return;
    }
    if (thread.attributes & 0x00200000u) {
        const std::vector<std::uint8_t> zero(thread.top - thread.bottom,0u);
        runtime_.memory().copy_in(thread.bottom,zero);
    }
    if (!release_stack_(thread.bottom)) {
        runtime_.stop("DeleteThread stack release failed"); return;
    }
    for (auto it = threads_.callbacks_.begin(); it != threads_.callbacks_.end();) {
        if (it->second.owner != uid) { ++it; continue; }
        const auto callback = it->first;
        if (threads_.exit_callback_ == callback) threads_.exit_callback_ = 0u;
        for (auto &slot : threads_.power_callbacks_) if (slot == callback) slot = 0u;
        it = threads_.callbacks_.erase(it);
    }
    // Normally already drained on exit; retained as an invariant guard.
    on_ended(uid,0x800201ACu);
    threads_.threads_.erase(found);
    c.set_gpr(2,0u);
    threads_.dispatch_ready(c);
}
}
