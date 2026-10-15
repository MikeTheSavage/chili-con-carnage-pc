#include "fixed_pool.hpp"
#include <algorithm>
#include <limits>
#include <iostream>
namespace chili {
namespace { constexpr auto unknown = 0x8002019Du, badarg = 0x800200D3u; }
FixedPools::FixedPools(psprecomp::Runtime &rt, PartitionMemory &memory, ThreadRuntime &threads)
    : runtime_(rt), memory_(memory), threads_(threads) {
    auto bind = [&](std::uint32_t nid, auto function) { rt.register_hle("ThreadManForUser", nid, function); };
    bind(0xC07BB470u, [this](auto &, auto &ctx) { create(ctx); });
    bind(0xD979E9BFu, [this](auto &, auto &ctx) { allocate(ctx, false); });
    bind(0x623AE665u, [this](auto &, auto &ctx) { allocate(ctx, true); });
    bind(0xF6414A71u, [this](auto &, auto &ctx) { free(ctx); });
    bind(0xED1410E0u, [this](auto &, auto &ctx) { clear(ctx, true); });
    bind(0xA8AA591Fu, [this](auto &, auto &ctx) { clear(ctx, false); });
    bind(0xD8199E4Cu, [this](auto &, auto &ctx) { status(ctx); });
    // Callback-aware allocation stays unbound until callback delivery exists.
}
void FixedPools::create(psprecomp::AllegrexContext &ctx) {
    const auto name = ctx.gpr[4], partition = ctx.gpr[5], attr = ctx.gpr[6];
    const auto size = ctx.gpr[7], count = ctx.gpr[8], options = ctx.gpr[9];
    if (!name || !runtime_.memory().contains(name, 1u) || partition != 2u) { ctx.set_gpr(2u, badarg); return; }
    if (attr & ~0x41FFu) { ctx.set_gpr(2u, 0x80020191u); return; }
    std::uint32_t alignment = 4u;
    if (options) {
        if (!runtime_.memory().contains(options, 4u)) { ctx.set_gpr(2u, badarg); return; }
        if (runtime_.memory().load32(options) >= 8u) {
            if (!runtime_.memory().contains(options, 8u)) { ctx.set_gpr(2u, badarg); return; }
            alignment = runtime_.memory().load32(options + 4u);
            if (alignment & (alignment - 1u)) { ctx.set_gpr(2u, badarg); return; }
            alignment = std::max(alignment, 4u);
        }
    }
    const auto stride64 = (static_cast<std::uint64_t>(size) + alignment - 1u) &
        ~static_cast<std::uint64_t>(alignment - 1u);
    if (!size || !count || stride64 > 0x02000000u || stride64 * count > 0x02000000u) {
        ctx.set_gpr(2u, 0x800201B7u); return;
    }
    const auto address = memory_.reserve_region(static_cast<std::uint32_t>(stride64 * count), alignment, (attr & 0x4000u) != 0u);
    if (!address) { ctx.set_gpr(2u, 0x80020190u); return; }
    const auto uid = next_uid_++;
    Pool pool{runtime_.memory().read_c_string(name, 32u), address, size,
        static_cast<std::uint32_t>(stride64), attr, std::vector<bool>(count, false), {}};
    std::cout << "[fpl] create uid=" << uid << " name=" << pool.name << " block=" << size << " count=" << count << '\n';
    pools_.emplace(uid, std::move(pool));
    ctx.set_gpr(2u, uid);
}
bool FixedPools::take(Pool &pool, std::uint32_t output) {
    for (std::size_t i = 0; i < pool.allocated.size(); ++i) {
        if (pool.allocated[i]) continue;
        runtime_.memory().store32(output, pool.address + static_cast<std::uint32_t>(i) * pool.stride);
        pool.allocated[i] = true;
        return true;
    }
    return false;
}
void FixedPools::allocate(psprecomp::AllegrexContext &ctx, bool poll) {
    const auto uid = ctx.gpr[4], output = ctx.gpr[5], timeout = poll ? 0u : ctx.gpr[6];
    auto found = pools_.find(uid);
    if (found == pools_.end()) { ctx.set_gpr(2u, unknown); return; }
    if (!output || !runtime_.memory().contains(output, 4u) ||
        (timeout && !runtime_.memory().contains(timeout, 4u))) { ctx.set_gpr(2u, badarg); return; }
    if (take(found->second, output)) { ctx.set_gpr(2u, 0u); return; }
    if (poll) { ctx.set_gpr(2u, 0x80020190u); return; }
    if (!threads_.can_wait()) { ctx.set_gpr(2u, 0x800201A7u); return; }
    if (timeout && !runtime_.memory().load32(timeout)) { ctx.set_gpr(2u, 0x800201A8u); return; }
    const auto thread = threads_.current_uid();
    const auto deadline = timeout ? threads_.now_us() + runtime_.memory().load32(timeout) : std::numeric_limits<std::uint64_t>::max();
    found->second.waiters.push_back(Waiter{thread, output, timeout, deadline});
    threads_.wait(ctx, deadline, [this, uid, thread, timeout] {
        auto pool = pools_.find(uid);
        if (pool != pools_.end()) std::erase_if(pool->second.waiters, [thread](const auto &w) { return w.thread == thread; });
        if (timeout) runtime_.memory().store32(timeout, 0u);
    });
}
void FixedPools::finish_wait(const Waiter &waiter, std::uint32_t result) {
    if (waiter.timeout) runtime_.memory().store32(waiter.timeout, static_cast<std::uint32_t>(
        waiter.deadline > threads_.now_us() ? waiter.deadline - threads_.now_us() : 0u));
    threads_.wake(waiter.thread, result);
}
void FixedPools::free(psprecomp::AllegrexContext &ctx) {
    auto found = pools_.find(ctx.gpr[4]);
    if (found == pools_.end()) { ctx.set_gpr(2u, unknown); return; }
    auto &pool = found->second;
    const auto address = psprecomp::GuestMemory::canonical(ctx.gpr[5]);
    if (address < pool.address || (address - pool.address) % pool.stride) { ctx.set_gpr(2u, 0x800201B6u); return; }
    const auto index = (address - pool.address) / pool.stride;
    if (index >= pool.allocated.size() || !pool.allocated[index]) { ctx.set_gpr(2u, 0x800201B6u); return; }
    pool.allocated[index] = false;
    if (!pool.waiters.empty()) {
        auto chosen = pool.waiters.begin();
        if (pool.attributes & 0x100u)
            chosen = std::min_element(pool.waiters.begin(), pool.waiters.end(), [this](const auto &a, const auto &b) {
                return threads_.priority(a.thread) < threads_.priority(b.thread);
            });
        const auto waiter = *chosen;
        pool.waiters.erase(chosen);
        take(pool, waiter.output);
        finish_wait(waiter, 0u);
    }
    ctx.set_gpr(2u, 0u);
    threads_.dispatch_ready(ctx);
}
void FixedPools::clear(psprecomp::AllegrexContext &ctx, bool erase) {
    auto found = pools_.find(ctx.gpr[4]);
    if (found == pools_.end()) { ctx.set_gpr(2u, unknown); return; }
    if (!erase && ctx.gpr[5]) {
        if (!runtime_.memory().contains(ctx.gpr[5], 4u)) { ctx.set_gpr(2u, badarg); return; }
        runtime_.memory().store32(ctx.gpr[5], static_cast<std::uint32_t>(found->second.waiters.size()));
    }
    for (const auto &waiter : found->second.waiters) finish_wait(waiter, erase ? 0x800201B5u : 0x800201A9u);
    found->second.waiters.clear();
    if (erase) { memory_.release_region(found->second.address); pools_.erase(found); }
    ctx.set_gpr(2u, 0u);
    threads_.dispatch_ready(ctx);
}
void FixedPools::status(psprecomp::AllegrexContext &ctx) {
    auto found = pools_.find(ctx.gpr[4]);
    const auto output = ctx.gpr[5];
    if (found == pools_.end()) { ctx.set_gpr(2u, unknown); return; }
    if (!output || !runtime_.memory().contains(output, 4u)) { ctx.set_gpr(2u, badarg); return; }
    const auto bytes = std::min(56u, runtime_.memory().load32(output));
    if (!runtime_.memory().contains(output, bytes)) { ctx.set_gpr(2u, badarg); return; }
    std::vector<std::uint8_t> data(56u, 0u);
    auto word = [&](unsigned offset, std::uint32_t value) { for (unsigned i = 0; i < 4u; ++i) data[offset+i] = static_cast<std::uint8_t>(value >> (8u*i)); };
    word(0u, 56u);
    const auto &pool = found->second;
    for (std::size_t i = 0; i < std::min<std::size_t>(31u, pool.name.size()); ++i) data[4u+i] = pool.name[i];
    word(36u, pool.attributes); word(40u, pool.block_size); word(44u, static_cast<std::uint32_t>(pool.allocated.size()));
    word(48u, static_cast<std::uint32_t>(std::count(pool.allocated.begin(), pool.allocated.end(), false)));
    word(52u, static_cast<std::uint32_t>(pool.waiters.size()));
    runtime_.memory().copy_in(output, std::span<const std::uint8_t>(data.data(), bytes));
    ctx.set_gpr(2u, 0u);
}
}
