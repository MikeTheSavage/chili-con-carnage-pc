#include "../host/power_runtime.hpp"
#include <cstring>
#include <stdexcept>
#include <vector>
#include <iostream>
namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void seed(psprecomp::AllegrexContext &c) {
    for (unsigned i=1; i<32; ++i) c.gpr[i]=0xCA000000u+i;
    for (unsigned i=0; i<32; ++i) c.fpr[i]=static_cast<float>(i)+0.25f;
    for (unsigned i=0; i<128; ++i) c.vfpu[i]=static_cast<float>(i)-63.5f;
    for (unsigned i=0; i<16; ++i) c.vfpu_ctrl[i]=0x0B000000u+i;
    c.hi=0x12345678u; c.lo=0x9ABCDEF0u; c.pc=0x0882D8FCu; c.fcr31=0xFEu;
}
void same_context(const psprecomp::AllegrexContext &a, const psprecomp::AllegrexContext &b) {
    require(a.gpr==b.gpr && a.hi==b.hi && a.lo==b.lo && a.pc==b.pc &&
        a.fcr31==b.fcr31 && a.vfpu_ctrl==b.vfpu_ctrl &&
        std::memcmp(a.fpr.data(),b.fpr.data(),sizeof(a.fpr))==0 &&
        std::memcmp(a.vfpu.data(),b.vfpu.data(),sizeof(a.vfpu))==0,
        "power call changed caller context");
}
}
int main() {
    psprecomp::Runtime rt; unsigned transitions=0; bool on=false;
    std::vector<unsigned> ticks;
    {
        chili::PowerRuntime power(rt,
            [&](bool requested) { ++transitions; on=requested; return true; },
            [&](bool system,bool display) { ticks.push_back((system?1u:0u)|(display?2u:0u)); return true; });
        auto &c=rt.cpu(); seed(c);
        auto call=[&](unsigned nid,unsigned mode) {
            c.gpr[4]=mode; auto expected=c; expected.gpr[2]=mode && nid!=0x090CCB3Fu ? 0x80000107u : 0u;
            rt.invoke_import("sceSuspendForUser",nid,c); same_context(c,expected); return c.gpr[2];
        };
        require(call(0xEADB1BD7u,1)==0x80000107u && transitions==0 && power.balance()==0,
            "invalid mode changed power state");
        require(call(0xEADB1BD7u,0)==0 && call(0xEADB1BD7u,0)==0 && on && transitions==1 && power.balance()==2,
            "nested power locks failed");
        for (const unsigned mode : {0u,1u,6u,0u}) call(0x090CCB3Fu,mode);
        require(ticks==std::vector<unsigned>({3u,1u,2u,3u}) && on && transitions==1 && power.balance()==2,
            "tick scopes or persistent lock preservation failed");
        require(call(0x3AEE7261u,0)==0 && on && call(0x3AEE7261u,0)==0 && !on && transitions==2,
            "power unlock nesting failed");
        call(0x090CCB3Fu,6u);
        require(!on && transitions==2 && power.balance()==0 && ticks.back()==2u,
            "tick acquired a persistent requirement");
        call(0xEADB1BD7u,0);
    }
    require(!on && transitions==4,"power destruction did not release host request");
    for (const unsigned mode : {2u,3u,4u,7u,0xFFFFFFFFu}) {
        psprecomp::Runtime invalid; unsigned invoked=0;
        chili::PowerRuntime power(invalid,[](bool) { return true; },
            [&](bool,bool) { ++invoked; return true; });
        auto &c=invalid.cpu(); seed(c); c.gpr[4]=mode; const auto expected=c;
        invalid.invoke_import("sceSuspendForUser",0x090CCB3Fu,c);
        require(invalid.stopped() && invoked==0 && power.balance()==0,"unsupported mode fabricated success");
        same_context(c,expected);
    }
    {
        psprecomp::Runtime failure; unsigned invoked=0;
        chili::PowerRuntime power(failure,[](bool) { return true; },
            [&](bool system,bool display) { ++invoked; require(system&&display,"wrong failed-provider scope"); return false; });
        auto &c=failure.cpu(); seed(c); c.gpr[4]=0; const auto expected=c;
        failure.invoke_import("sceSuspendForUser",0x090CCB3Fu,c);
        require(failure.stopped() && invoked==1 && failure.stop_reason()=="Windows idle-timer reset failed",
            "provider failure fabricated success"); same_context(c,expected);
    }
    {
        // Real Windows API smoke test: a one-shot tick creates no persistent
        // request and therefore needs no destructor cleanup.
        psprecomp::Runtime native; chili::PowerRuntime power(native);
        native.cpu().gpr[4]=0; native.cpu().gpr[2]=0xDEADBEEFu;
        native.invoke_import("sceSuspendForUser",0x090CCB3Fu,native.cpu());
        require(!native.stopped() && native.cpu().gpr[2]==0 && power.balance()==0,"native tick failed");
    }
    std::cout << "Power lock and idle-timer tests passed\n";
}
