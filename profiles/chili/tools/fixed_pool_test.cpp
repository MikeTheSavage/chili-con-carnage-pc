#include "../host/fixed_pool.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>
namespace {
constexpr auto name = 0x08802000u, output = name + 64u, child_output = output + 4u, timeout_ptr = output + 8u;
std::uint32_t pool_uid, held;
unsigned mode;
std::uint32_t second_thread;
std::vector<int> order;
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void call(psprecomp::Runtime &rt, std::uint32_t nid) { rt.invoke_import("ThreadManForUser", nid, rt.cpu()); }
std::uint32_t create_pool(psprecomp::Runtime &rt, std::uint32_t attributes = 0u) {
    rt.memory().store32(name, 0x006C7066u);
    auto &ctx = rt.cpu(); ctx.gpr[4]=name; ctx.gpr[5]=2u; ctx.gpr[6]=attributes;
    ctx.gpr[7]=17u; ctx.gpr[8]=1u; ctx.gpr[9]=0u;
    call(rt, 0xC07BB470u);
    require(static_cast<std::int32_t>(ctx.gpr[2]) > 0, "Pool creation failed");
    return ctx.gpr[2];
}
void alloc(psprecomp::Runtime &rt, bool poll, std::uint32_t out, std::uint32_t timeout = 0u) {
    rt.cpu().gpr[4]=pool_uid; rt.cpu().gpr[5]=out; rt.cpu().gpr[6]=timeout;
    call(rt, poll ? 0x623AE665u : 0xD979E9BFu);
}
void check_basic() {
    psprecomp::Runtime rt;
    chili::PartitionMemory memory(rt, 0x09000000u, 0x09100000u);
    chili::ThreadRuntime threads(rt); threads.initialize_bootstrap();
    chili::FixedPools pools(rt, memory, threads);
    pool_uid = create_pool(rt);
    alloc(rt, true, output); require(rt.cpu().gpr[2]==0u, "First allocation failed");
    held = rt.memory().load32(output);
    const auto info = output + 64u;
    rt.memory().store32(info, 56u); rt.cpu().gpr[4]=pool_uid; rt.cpu().gpr[5]=info;
    call(rt, 0xD8199E4Cu);
    require(rt.cpu().gpr[2]==0u && rt.memory().load32(info+40u)==17u &&
        rt.memory().load32(info+44u)==1u && rt.memory().load32(info+48u)==0u &&
        rt.memory().load32(info+52u)==0u, "Pool status counts mismatch");
    require(held==0x09000000u, "Block outside arena");
    rt.memory().store32(child_output, 0xDEADBEEFu);
    alloc(rt, true, child_output);
    require(rt.cpu().gpr[2]==0x80020190u && rt.memory().load32(child_output)==0xDEADBEEFu, "Exhausted poll modified output");
    rt.cpu().gpr[4]=pool_uid; rt.cpu().gpr[5]=held+1u; call(rt, 0xF6414A71u);
    require(rt.cpu().gpr[2]==0x800201B6u, "Interior pointer accepted");
    rt.cpu().gpr[4]=pool_uid; rt.cpu().gpr[5]=held; call(rt, 0xF6414A71u);
    require(rt.cpu().gpr[2]==0u, "Block free failed");
    call(rt, 0xF6414A71u); require(rt.cpu().gpr[2]==0x800201B6u, "Double free accepted");
    alloc(rt, true, child_output); require(rt.cpu().gpr[2]==0u && rt.memory().load32(child_output)==held, "Block not reusable");
    rt.memory().store32(timeout_ptr, 100u);
    rt.cpu().gpr[31]=0x08804000u;
    alloc(rt, false, output, timeout_ptr);
    require(rt.cpu().gpr[2]==0x800201A8u && rt.memory().load32(timeout_ptr)==0u && threads.now_us()==100u, "Wait timeout failed");
    rt.cpu().gpr[4]=pool_uid; call(rt, 0xED1410E0u); require(rt.cpu().gpr[2]==0u, "Delete failed");
    require(memory.reserve_region(0x100000u, 256u, false)==0x09000000u, "Backing arena was not reclaimed");
    alloc(rt, true, output); require(rt.cpu().gpr[2]==0x8002019Du, "Deleted pool remained valid");
}
void child_resume(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    const auto expected = mode == 0u ? 0u : mode == 1u ? 0x800201A9u : 0x800201B5u;
    require(ctx.gpr[2]==expected, "Wait result mismatch");
    if (!mode) require(rt.memory().load32(child_output)==held, "Freed block was not transferred to waiter");
    else require(rt.memory().load32(child_output)==0xDEADBEEFu, "Cancel/delete modified waiter output");
    order.push_back(2); ctx.pc=chili::ThreadRuntime::return_trampoline;
}
void child_entry(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    ctx.gpr[31]=0x08804200u;
    alloc(rt, false, child_output);
}
void parent_return(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    order.push_back(3); ctx.pc=chili::ThreadRuntime::return_trampoline;
}
void parent_release(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    order.push_back(1); ctx.gpr[31]=0x08804400u; ctx.gpr[4]=pool_uid;
    ctx.gpr[5]=mode == 0u ? held : 0u;
    call(rt, mode == 0u ? 0xF6414A71u : mode == 1u ? 0xA8AA591Fu : 0xED1410E0u);
}
void check_handoff(unsigned action) {
    mode=action; order.clear();
    psprecomp::Runtime rt;
    chili::PartitionMemory memory(rt, 0x09000000u, 0x09FF0000u);
    chili::ThreadRuntime threads(rt, [&memory](auto size) { return memory.reserve_stack(size); });
    chili::FixedPools pools(rt, memory, threads);
    rt.cpu().gpr[31]=0x08804300u; threads.initialize_bootstrap();
    rt.register_function(0x08804100u, child_entry, "fpl_child");
    rt.register_function(0x08804200u, child_resume, "fpl_child_resume");
    rt.register_function(0x08804300u, parent_release, "fpl_release");
    rt.register_function(0x08804400u, parent_return, "fpl_parent_return");
    pool_uid=create_pool(rt); alloc(rt, true, output); held=rt.memory().load32(output);
    rt.memory().store32(child_output, 0xDEADBEEFu);
    auto &ctx=rt.cpu(); ctx.gpr[4]=name; ctx.gpr[5]=0x08804100u; ctx.gpr[6]=0x10u; ctx.gpr[7]=0x1000u; ctx.gpr[8]=0u;
    call(rt, 0x446D8DE6u);
    const auto child=ctx.gpr[2]; ctx.gpr[4]=child; ctx.gpr[5]=0u; ctx.gpr[6]=0u; call(rt, 0xF475845Du);
    rt.run(ctx.pc, 100u);
    require(order==std::vector<int>{1,2,3}, "Wait did not preserve both continuations or priority wakeup");
}
void queue_resume(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    require(ctx.gpr[2]==0u, "Queued waiter allocation failed");
    order.push_back(static_cast<int>(ctx.gpr[16]));
    ctx.gpr[4]=pool_uid; ctx.gpr[5]=held; ctx.gpr[31]=chili::ThreadRuntime::return_trampoline;
    call(rt, 0xF6414A71u);
}
void queue_enter(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx, std::uint32_t marker) {
    ctx.gpr[16]=marker; ctx.gpr[31]=0x08804700u;
    alloc(rt, false, output + marker * 4u);
}
void queue_a(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { queue_enter(rt, ctx, 10u); }
void queue_b(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { queue_enter(rt, ctx, 8u); }
void start_second(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    ctx.gpr[31]=0x08804300u; ctx.gpr[4]=second_thread; ctx.gpr[5]=0u; ctx.gpr[6]=0u;
    call(rt, 0xF475845Du);
}
void check_order(bool priority) {
    mode=0u; order.clear();
    psprecomp::Runtime rt;
    chili::PartitionMemory memory(rt, 0x09000000u, 0x09FF0000u);
    chili::ThreadRuntime threads(rt, [&memory](auto size) { return memory.reserve_stack(size); });
    chili::FixedPools pools(rt, memory, threads);
    rt.cpu().gpr[31]=0x08804800u; threads.initialize_bootstrap();
    rt.register_function(0x08804500u, queue_a, "queue_a");
    rt.register_function(0x08804600u, queue_b, "queue_b");
    rt.register_function(0x08804700u, queue_resume, "queue_resume");
    rt.register_function(0x08804800u, start_second, "start_second");
    rt.register_function(0x08804300u, parent_release, "queue_release");
    rt.register_function(0x08804400u, parent_return, "queue_parent_return");
    pool_uid=create_pool(rt, priority ? 0x100u : 0u); alloc(rt, true, output); held=rt.memory().load32(output);
    auto make_thread = [&](std::uint32_t entry, std::uint32_t priority_value) {
        auto &ctx=rt.cpu(); ctx.gpr[4]=name; ctx.gpr[5]=entry; ctx.gpr[6]=priority_value;
        ctx.gpr[7]=0x1000u; ctx.gpr[8]=0u; call(rt, 0x446D8DE6u); return ctx.gpr[2];
    };
    const auto first=make_thread(0x08804500u, 0x10u);
    second_thread=make_thread(0x08804600u, 0x08u);
    auto &ctx=rt.cpu(); ctx.gpr[4]=first; ctx.gpr[5]=0u; ctx.gpr[6]=0u; call(rt, 0xF475845Du);
    rt.run(ctx.pc, 100u);
    require(order == (priority ? std::vector<int>{1,8,10,3} : std::vector<int>{1,10,8,3}),
        "FPL queue did not honor FIFO/priority ordering");
}
}
int main() {
    try { check_basic(); check_handoff(0u); check_handoff(1u); check_handoff(2u); check_order(false); check_order(true);
        std::cout << "PASS: FPL ownership, status, reuse, exhaustion, timeout, reclamation, FIFO/priority order, cancel/delete wakeup\n";
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
