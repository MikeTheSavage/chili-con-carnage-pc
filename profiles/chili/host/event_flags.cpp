#include "event_flags.hpp"
#include <algorithm>
#include <limits>
#include <iostream>
namespace chili {
namespace { constexpr auto unknown = 0x8002019Au, badarg = 0x800200D3u; }
EventFlags::EventFlags(psprecomp::Runtime &rt, ThreadRuntime &threads) : runtime_(rt), threads_(threads) {
    auto bind = [&](std::uint32_t nid, auto fn) { rt.register_hle("ThreadManForUser", nid, fn); };
    bind(0x55C20A00u, [this](auto &, auto &c) { create(c); });
    bind(0x1FB15A32u, [this](auto &, auto &c) { modify(c, true); });
    bind(0x812346E4u, [this](auto &, auto &c) { modify(c, false); });
    bind(0x402FCF22u, [this](auto &, auto &c) { wait(c, false); });
    bind(0x30FD48F0u, [this](auto &, auto &c) { wait(c, true); });
    bind(0xEF9E4C70u, [this](auto &, auto &c) { remove(c, false); });
    bind(0xCD203292u, [this](auto &, auto &c) { remove(c, true); });
    bind(0xA66B0120u, [this](auto &, auto &c) { status(c); });
    // Callback-aware waits require callback delivery and remain unbound.
}
void EventFlags::create(psprecomp::AllegrexContext &c) {
    const auto name=c.gpr[4], attr=c.gpr[5], options=c.gpr[7];
    if (!name || !runtime_.memory().contains(name, 1u) ||
        (options && !runtime_.memory().contains(options, 4u))) { c.set_gpr(2, badarg); return; }
    if ((attr & 0x100u) || attr >= 0x300u) { c.set_gpr(2, 0x80020191u); return; }
    const auto uid=next_uid_++;
    Flag flag{runtime_.memory().read_c_string(name,32u),attr,c.gpr[6],c.gpr[6],{}};
    std::cout << "[event] create uid=" << uid << " name=" << flag.name << " pattern=" << flag.pattern << '\n';
    flags_.emplace(uid,std::move(flag)); c.set_gpr(2,uid);
}
bool EventFlags::match(Flag &flag, const Waiter &w) {
    const bool matched = (w.mode & 1u) ? (flag.pattern & w.bits)!=0u : (flag.pattern & w.bits)==w.bits;
    if (!matched) return false;
    if (w.output) runtime_.memory().store32(w.output, flag.pattern);
    if (w.mode & 0x20u) flag.pattern &= ~w.bits;
    if (w.mode & 0x10u) flag.pattern = 0u;
    return true;
}
void EventFlags::finish(const Waiter &w, std::uint32_t result) {
    if (w.timeout) runtime_.memory().store32(w.timeout, static_cast<std::uint32_t>(
        w.deadline > threads_.now_us() ? w.deadline-threads_.now_us() : 0u));
    threads_.wake(w.thread,result);
}
void EventFlags::wait(psprecomp::AllegrexContext &c, bool poll) {
    const auto uid=c.gpr[4], bits=c.gpr[5], mode=c.gpr[6], output=c.gpr[7], timeout=poll?0u:c.gpr[8];
    if ((mode & ~0x31u) || (poll && (mode & 0x30u)==0x30u)) { c.set_gpr(2,0x80020195u); return; }
    if (!bits) { c.set_gpr(2,0x800201B1u); return; }
    if (!poll && !threads_.can_wait()) { c.set_gpr(2,0x800201A7u); return; }
    auto found=flags_.find(uid);
    if (found==flags_.end()) { c.set_gpr(2,unknown); return; }
    if ((output && ((output & 3u) || !runtime_.memory().contains(output,4u))) ||
        (timeout && ((timeout & 3u) || !runtime_.memory().contains(timeout,4u)))) { c.set_gpr(2,badarg); return; }
    auto &flag=found->second;
    const auto deadline=timeout ? threads_.now_us()+runtime_.memory().load32(timeout) : std::numeric_limits<std::uint64_t>::max();
    Waiter w{threads_.current_uid(),bits,mode,output,timeout,deadline};
    if (match(flag,w)) { c.set_gpr(2,0u); return; }
    if (poll && output) runtime_.memory().store32(output,flag.pattern);
    if (!flag.waiters.empty() && !(flag.attributes & 0x200u)) { c.set_gpr(2,0x800201B0u); return; }
    if (poll) { c.set_gpr(2,0x800201AFu); return; }
    if (timeout && deadline==threads_.now_us()) { c.set_gpr(2,0x800201A8u); return; }
    flag.waiters.push_back(w);
    threads_.wait(c,deadline,[this,uid,w] {
        auto found=flags_.find(uid);
        if (found!=flags_.end()) {
            if (w.output) runtime_.memory().store32(w.output,found->second.pattern);
            std::erase_if(found->second.waiters,[&](const auto &entry) { return entry.thread==w.thread; });
        }
        if (w.timeout) runtime_.memory().store32(w.timeout,0u);
    });
}
void EventFlags::modify(psprecomp::AllegrexContext &c, bool set) {
    auto found=flags_.find(c.gpr[4]);
    if (found==flags_.end()) { c.set_gpr(2,unknown); return; }
    auto &flag=found->second;
    if (set) flag.pattern |= c.gpr[5];
    else flag.pattern &= c.gpr[5]; // PSP clear argument is the mask to retain.
    if (set) {
        for (auto it=flag.waiters.begin(); it!=flag.waiters.end();) {
            if (match(flag,*it)) { const auto w=*it; it=flag.waiters.erase(it); finish(w,0u); }
            else ++it;
        }
    }
    c.set_gpr(2,0u);
    if (set) threads_.dispatch_ready(c);
}
void EventFlags::remove(psprecomp::AllegrexContext &c, bool cancel) {
    auto found=flags_.find(c.gpr[4]);
    if (found==flags_.end()) { c.set_gpr(2,unknown); return; }
    const auto output=cancel?c.gpr[6]:0u;
    if (output && !runtime_.memory().contains(output,4u)) { c.set_gpr(2,badarg); return; }
    auto &flag=found->second;
    if (output) runtime_.memory().store32(output,static_cast<std::uint32_t>(flag.waiters.size()));
    if (cancel) flag.pattern=c.gpr[5];
    for (const auto &w:flag.waiters) {
        if (w.output) runtime_.memory().store32(w.output,flag.pattern);
        finish(w,cancel?0x800201A9u:0x800201B5u);
    }
    flag.waiters.clear();
    if (!cancel) flags_.erase(found);
    c.set_gpr(2,0u); threads_.dispatch_ready(c);
}
void EventFlags::status(psprecomp::AllegrexContext &c) {
    auto found=flags_.find(c.gpr[4]); const auto output=c.gpr[5];
    if (found==flags_.end()) { c.set_gpr(2,unknown); return; }
    if (!output || !runtime_.memory().contains(output,4u)) { c.set_gpr(2,badarg); return; }
    const auto bytes=std::min(52u,runtime_.memory().load32(output));
    if (!runtime_.memory().contains(output,bytes)) { c.set_gpr(2,badarg); return; }
    std::vector<std::uint8_t> data(52u,0u);
    auto word=[&](unsigned offset,std::uint32_t value) { for(unsigned i=0;i<4;++i) data[offset+i]=static_cast<std::uint8_t>(value>>(8*i)); };
    const auto &flag=found->second; word(0,52u);
    for(std::size_t i=0;i<std::min<std::size_t>(31u,flag.name.size());++i) data[4+i]=flag.name[i];
    word(36,flag.attributes); word(40,flag.initial); word(44,flag.pattern); word(48,static_cast<std::uint32_t>(flag.waiters.size()));
    runtime_.memory().copy_in(output,std::span<const std::uint8_t>(data.data(),bytes)); c.set_gpr(2,0u);
}
}
