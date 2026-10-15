#include "../host/thread_runtime.hpp"
#include "psprecomp/common.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
std::vector<int> execution_order;
unsigned busy_progress;
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void import(psprecomp::Runtime &runtime, std::uint32_t nid) {
    runtime.invoke_import("ThreadManForUser", nid, runtime.cpu());
}
std::uint32_t create(psprecomp::Runtime &rt, std::uint32_t entry, std::uint32_t priority) {
    auto &ctx = rt.cpu();
    const std::string name = "test_child";
    for (std::size_t i = 0; i <= name.size(); ++i)
        rt.memory().store8(0x08801000u + static_cast<std::uint32_t>(i), i == name.size() ? 0u : name[i]);
    ctx.gpr[4] = 0x08801000u; ctx.gpr[5] = entry; ctx.gpr[6] = priority;
    ctx.gpr[7] = 0x1000u; ctx.gpr[8] = 0x80004000u;
    import(rt, 0x446D8DE6u);
    return ctx.gpr[2];
}
void start(psprecomp::Runtime &rt, std::uint32_t uid, std::uint32_t length = 0u) {
    rt.cpu().gpr[4] = uid; rt.cpu().gpr[5] = length; rt.cpu().gpr[6] = 0x08802000u;
    import(rt, 0xF475845Du);
}
void returning_child(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    require(ctx.gpr[29] >= 0x09000000u && (ctx.gpr[29] & 15u) == 0u, "Invalid child stack");
    if (ctx.gpr[4]) {
        require(ctx.gpr[4] == 4u && rt.memory().load32(ctx.gpr[5]) == 0x12345678u, "Arguments were not copied to child stack");
        require(ctx.gpr[5] != 0x08802000u, "Child arguments alias caller memory");
    }
    execution_order.push_back(2);
    ctx.gpr[2] = 7u;
    ctx.pc = ctx.gpr[31];
}
void parent_continuation(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    require(ctx.gpr[16] == 0xABCDEF01u, "Parent register state was lost");
    execution_order.push_back(1);
    ctx.gpr[2] = 0u;
    ctx.pc = chili::ThreadRuntime::return_trampoline;
}
void test_start(bool preempt) {
    execution_order.clear();
    psprecomp::Runtime rt;
    chili::ThreadRuntime threads(rt);
    rt.cpu().gpr[16] = 0xABCDEF01u;
    rt.cpu().gpr[31] = preempt ? 0x08804100u : chili::ThreadRuntime::return_trampoline;
    threads.initialize_bootstrap();
    rt.register_function(0x08804000u, returning_child, "test_child");
    rt.register_function(0x08804100u, parent_continuation, "parent_resume");
    rt.memory().store32(0x08802000u, 0x12345678u);
    const auto uid = create(rt, 0x08804000u, preempt ? 0x10u : 0x20u);
    require(uid == 2u, "Thread UID allocation failed");
    start(rt, uid, 4u);
    rt.memory().store32(0x08802000u, 0xDEADBEEFu);
    if (preempt) require(rt.cpu().pc == 0x08804000u, "Higher-priority child did not preempt");
    else require(rt.cpu().pc == chili::ThreadRuntime::return_trampoline, "Equal-priority child preempted parent");
    rt.run(rt.cpu().pc, 100u);
    require(rt.stop_reason() == "No runnable Chili threads", "Scheduler did not terminate after thread returns");
    require(execution_order == (preempt ? std::vector<int>{2, 1} : std::vector<int>{2}), "Thread execution order mismatch");
}
void test_invalid_calls() {
    psprecomp::Runtime rt;
    chili::ThreadRuntime threads(rt);
    threads.initialize_bootstrap();
    require(create(rt, 0x08804000u, 0u) == 0x80020193u, "Invalid priority was accepted");
    rt.cpu().gpr[4] = 999u;
    import(rt, 0xF475845Du);
    require(rt.cpu().gpr[2] == 0x80020198u, "Unknown thread UID was accepted");
}
void busy_child(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    busy_progress = ++ctx.gpr[16];
    if (busy_progress < 3000u) { ctx.pc = 0x08804200u; return; }
    execution_order.push_back(3);
    ctx.pc = ctx.gpr[31];
}
void peer_child(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    require(busy_progress > 0u && busy_progress < 3000u, "Equal-priority peer did not receive a time slice");
    execution_order.push_back(4);
    ctx.pc = ctx.gpr[31];
}
void test_timer_fairness() {
    execution_order.clear(); busy_progress = 0u;
    psprecomp::Runtime rt;
    chili::ThreadRuntime threads(rt);
    rt.cpu().gpr[31] = chili::ThreadRuntime::return_trampoline;
    threads.initialize_bootstrap();
    rt.register_function(0x08804200u, busy_child, "busy_child");
    rt.register_function(0x08804300u, peer_child, "peer_child");
    start(rt, create(rt, 0x08804200u, 0x30u));
    start(rt, create(rt, 0x08804300u, 0x30u));
    rt.run(rt.cpu().pc, 5000u);
    require(execution_order == std::vector<int>{4, 3}, "Timer preemption did not resume the busy thread");
}
void test_interrupt_preemption() {
    execution_order.clear();
    psprecomp::Runtime rt;
    chili::ThreadRuntime threads(rt);
    rt.cpu().gpr[31] = 0x08804100u;
    rt.cpu().gpr[16] = 0xABCDEF01u;
    threads.initialize_bootstrap();
    rt.register_function(0x08804000u, returning_child, "critical_child");
    rt.register_function(0x08804100u, parent_continuation, "parent_resume");
    rt.invoke_import("Kernel_Library", 0x092968F4u, rt.cpu());
    const auto outer_flags = rt.cpu().gpr[2];
    rt.invoke_import("Kernel_Library", 0x092968F4u, rt.cpu());
    require(outer_flags == 1u && rt.cpu().gpr[2] == 0u, "Nested suspend flags mismatch");
    start(rt, create(rt, 0x08804000u, 0x10u));
    require(rt.cpu().pc == 0x08804100u, "Preempted with interrupts disabled");
    rt.cpu().gpr[4] = 0u;
    rt.invoke_import("Kernel_Library", 0x5F10D406u, rt.cpu());
    require(rt.cpu().pc == 0x08804100u, "Inner resume enabled interrupts");
    rt.cpu().gpr[4] = outer_flags;
    rt.invoke_import("Kernel_Library", 0x5F10D406u, rt.cpu());
    require(rt.cpu().pc == 0x08804000u, "Outer resume did not schedule higher priority");
    rt.run(rt.cpu().pc, 100u);
    require(execution_order == std::vector<int>{2, 1}, "Interrupt handoff lost caller context");
}
void critical_continuation(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    execution_order.push_back(5);
    ctx.pc = chili::ThreadRuntime::return_trampoline;
}
void critical_busy(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    if (ctx.gpr[16] == 0u) {
        rt.invoke_import("Kernel_Library", 0x092968F4u, ctx);
        ctx.gpr[17] = ctx.gpr[2];
    }
    busy_progress = ++ctx.gpr[16];
    if (busy_progress < 3000u) { ctx.pc = 0x08804500u; return; }
    require(execution_order.empty(), "Timer preempted inside critical section");
    ctx.gpr[31] = 0x08804400u;
    ctx.gpr[4] = ctx.gpr[17];
    rt.invoke_import("Kernel_Library", 0x5F10D406u, ctx);
}
void critical_peer(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    require(busy_progress == 3000u, "Deferred timer ran before resume");
    execution_order.push_back(4);
    ctx.pc = ctx.gpr[31];
}
void test_deferred_timer() {
    execution_order.clear(); busy_progress = 0u;
    psprecomp::Runtime rt;
    chili::ThreadRuntime threads(rt);
    rt.cpu().gpr[31] = chili::ThreadRuntime::return_trampoline;
    threads.initialize_bootstrap();
    rt.register_function(0x08804500u, critical_busy, "critical_busy");
    rt.register_function(0x08804600u, critical_peer, "critical_peer");
    rt.register_function(0x08804400u, critical_continuation, "critical_continuation");
    start(rt, create(rt, 0x08804500u, 0x30u));
    start(rt, create(rt, 0x08804600u, 0x30u));
    rt.run(rt.cpu().pc, 5000u);
    require(execution_order == std::vector<int>{4, 5}, "Deferred timer did not hand off on resume");
}
}
int main() {
    try {
        test_start(false); test_start(true); test_invalid_calls(); test_timer_fairness(); test_interrupt_preemption(); test_deferred_timer();
        std::cout << "PASS: native thread handoff, priority/timer preemption, nested interrupts, deferred timer, parent context, copied arguments, invalid calls\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
