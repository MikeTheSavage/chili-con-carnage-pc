#include "semaphores.hpp"
#include <algorithm>
#include <limits>
#include <iostream>
namespace chili {
namespace {constexpr auto unknown=0x80020199u,badarg=0x800200D3u,illegal_count=0x800201BDu;}
Semaphores::Semaphores(psprecomp::Runtime &rt,ThreadRuntime &threads):runtime_(rt),threads_(threads){
    auto bind=[&](std::uint32_t nid,auto fn){rt.register_hle("ThreadManForUser",nid,fn);};
    bind(0xD6DA4BA1u,[this](auto &,auto &c){create(c);});
    bind(0x4E3A1105u,[this](auto &,auto &c){wait(c,false);});
    bind(0x58B1F937u,[this](auto &,auto &c){wait(c,true);});
    bind(0x3F53E640u,[this](auto &,auto &c){signal(c);});
    bind(0x28B6489Cu,[this](auto &,auto &c){remove(c,false);});
    bind(0x8FFDF9A2u,[this](auto &,auto &c){remove(c,true);});
    bind(0xBC6FEBC5u,[this](auto &,auto &c){status(c);});
    // Resource-specific callback-aware waits remain unsupported.
}
void Semaphores::create(psprecomp::AllegrexContext &c){
    const auto name=c.gpr[4],attr=c.gpr[5],initial=c.gpr[6],maximum=c.gpr[7];
    if(!name || !runtime_.memory().contains(name,1u)){c.set_gpr(2,badarg);return;}
    if(attr>=0x200u){c.set_gpr(2,0x80020191u);return;}
    if(!maximum || maximum>0x7FFFFFFFu || initial>maximum){c.set_gpr(2,illegal_count);return;}
    const auto uid=next_uid_++;Sema s{runtime_.memory().read_c_string(name,32u),attr,initial,initial,maximum,{}};
    std::cout<<"[sema] create uid="<<uid<<" name="<<s.name<<" count="<<initial<<" maximum="<<maximum<<'\n';
    semaphores_.emplace(uid,std::move(s));c.set_gpr(2,uid);
}
void Semaphores::wait(psprecomp::AllegrexContext &c,bool poll){
    const auto uid=c.gpr[4],count=c.gpr[5],timeout=poll?0u:c.gpr[6];
    if(!count || count>0x7FFFFFFFu){c.set_gpr(2,illegal_count);return;}
    auto found=semaphores_.find(uid);if(found==semaphores_.end()){c.set_gpr(2,unknown);return;}
    auto &s=found->second;if(count>s.maximum){c.set_gpr(2,illegal_count);return;}
    if(!poll && !threads_.can_wait()){c.set_gpr(2,0x800201A7u);return;}
    if(timeout && !runtime_.memory().contains(timeout,4u)){c.set_gpr(2,badarg);return;}
    if(s.waiters.empty() && s.count>=count){s.count-=count;c.set_gpr(2,0u);return;}
    if(poll){c.set_gpr(2,0x800201ADu);return;}
    if(timeout && !runtime_.memory().load32(timeout)){c.set_gpr(2,0x800201A8u);return;}
    const auto thread=threads_.current_uid();const auto deadline=timeout?threads_.now_us()+runtime_.memory().load32(timeout):std::numeric_limits<std::uint64_t>::max();
    s.waiters.push_back({thread,count,timeout,deadline});
    threads_.wait(c,deadline,[this,uid,thread,timeout]{
        auto found=semaphores_.find(uid);
        if(found!=semaphores_.end()) {
            std::erase_if(found->second.waiters,[thread](const auto &w){return w.thread==thread;});
            if(!(found->second.attr & 0x100u))drain_after_timeout(found->second);
        }
        if(timeout)runtime_.memory().store32(timeout,0u);
    });
}
void Semaphores::finish(const Waiter &w,std::uint32_t result){
    if(w.timeout)runtime_.memory().store32(w.timeout,static_cast<std::uint32_t>(w.deadline>threads_.now_us()?w.deadline-threads_.now_us():0u));
    threads_.wake(w.thread,result);
}
void Semaphores::drain_after_timeout(Sema &s){
    while(!s.waiters.empty() && s.waiters.front().count<=s.count){
        const auto w=s.waiters.front();s.waiters.erase(s.waiters.begin());
        s.count-=w.count;finish(w,0u);
    }
}
void Semaphores::signal(psprecomp::AllegrexContext &c){
    auto found=semaphores_.find(c.gpr[4]);if(found==semaphores_.end()){c.set_gpr(2,unknown);return;}
    const auto amount=c.gpr[5];if(!amount || amount>0x7FFFFFFFu){c.set_gpr(2,illegal_count);return;}
    auto &s=found->second;const auto sum=static_cast<std::uint64_t>(s.count)+amount;
    if(sum>static_cast<std::uint64_t>(s.maximum)+s.waiters.size()){c.set_gpr(2,0x800201AEu);return;}
    auto available=sum;
    if(s.attr & 0x100u)std::stable_sort(s.waiters.begin(),s.waiters.end(),[this](const auto &a,const auto &b){return threads_.priority(a.thread)<threads_.priority(b.thread);});
    for(auto it=s.waiters.begin();it!=s.waiters.end();){
        if(it->count<=available){available-=it->count;const auto w=*it;it=s.waiters.erase(it);finish(w,0u);}else ++it;
    }
    s.count=static_cast<std::uint32_t>(available);c.set_gpr(2,0u);threads_.dispatch_ready(c);
}
void Semaphores::remove(psprecomp::AllegrexContext &c,bool cancel){
    auto found=semaphores_.find(c.gpr[4]);if(found==semaphores_.end()){c.set_gpr(2,unknown);return;}
    auto &s=found->second;const auto output=cancel?c.gpr[6]:0u;const auto value=static_cast<std::int32_t>(c.gpr[5]);
    if(cancel && value>=0 && static_cast<std::uint32_t>(value)>s.maximum){c.set_gpr(2,illegal_count);return;}
    if(output && !runtime_.memory().contains(output,4u)){c.set_gpr(2,badarg);return;}
    if(output)runtime_.memory().store32(output,static_cast<std::uint32_t>(s.waiters.size()));
    for(const auto &w:s.waiters)finish(w,cancel?0x800201A9u:0x800201B5u);
    s.waiters.clear();if(cancel)s.count=value<0?s.initial:static_cast<std::uint32_t>(value);else semaphores_.erase(found);
    c.set_gpr(2,0u);threads_.dispatch_ready(c);
}
void Semaphores::status(psprecomp::AllegrexContext &c){
    auto found=semaphores_.find(c.gpr[4]);const auto output=c.gpr[5];
    if(found==semaphores_.end()){c.set_gpr(2,unknown);return;}
    if(!output || !runtime_.memory().contains(output,4u)){c.set_gpr(2,badarg);return;}
    const auto bytes=std::min(56u,runtime_.memory().load32(output));if(!runtime_.memory().contains(output,bytes)){c.set_gpr(2,badarg);return;}
    std::vector<std::uint8_t> data(56u,0u);auto word=[&](unsigned offset,std::uint32_t value){for(unsigned i=0;i<4;++i)data[offset+i]=static_cast<std::uint8_t>(value>>(8*i));};
    const auto &s=found->second;word(0,56u);for(std::size_t i=0;i<std::min<std::size_t>(31u,s.name.size());++i)data[4+i]=s.name[i];
    word(36,s.attr);word(40,s.initial);word(44,s.count);word(48,s.maximum);word(52,static_cast<std::uint32_t>(s.waiters.size()));
    runtime_.memory().copy_in(output,std::span<const std::uint8_t>(data.data(),bytes));c.set_gpr(2,0u);
}
}
