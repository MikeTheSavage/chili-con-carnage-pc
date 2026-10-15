#pragma once
#include "file_runtime.hpp"
#include "thread_runtime.hpp"
#include <limits>
#include <iostream>
namespace chili {
class UmdRuntime {
public:
    UmdRuntime(psprecomp::Runtime &rt, FileRuntime &files, ThreadRuntime &threads)
        : runtime_(rt), files_(files), threads_(threads) {
        // The disc is a folder, or an .iso read in place (disc_image.hpp).
        if(disc_iso)medium_=disc_iso->file("UMD_DATA.BIN") && disc_iso->file("PSP_GAME/SYSDIR/BOOT.BIN");
        else medium_=std::filesystem::is_regular_file(rt.game_root()/"UMD_DATA.BIN") &&
            std::filesystem::is_regular_file(rt.game_root()/"PSP_GAME/SYSDIR/BOOT.BIN");
        files_.set_disc_mounted(false);
        rt.register_hle("sceUmdUser",0xC6183D47u,[this](auto &,auto &c) { activate(c); });
        rt.register_hle("sceUmdUser",0x8EF08FCEu,[this](auto &,auto &c) { wait(c); });
        rt.register_hle("sceUmdUser",0x20628E6Fu,[this](auto &,auto &c) { c.set_gpr(2,error_); });
        rt.register_hle("sceUmdUser",0x6B4A146Cu,[this](auto &,auto &c) { c.set_gpr(2,state()); });
        rt.register_hle("sceUmdUser",0x46EBB729u,[this](auto &,auto &c) { c.set_gpr(2,medium_?1u:0u); });
    }
    std::uint32_t state() const { return medium_ ? (mounted_?0x32u:0x12u) : 1u; }
private:
    void activate(psprecomp::AllegrexContext &c) {
        if(c.gpr[4]<1u || c.gpr[4]>2u || !c.gpr[5] || !runtime_.memory().contains(c.gpr[5],1u)) {c.set_gpr(2,0x80010016u);return;}
        const auto drive=runtime_.memory().read_c_string(c.gpr[5],32u);
        if(drive!="disc0:") {runtime_.stop("UMD activation requested unsupported mount: "+drive);return;}
        if(!medium_) {error_=0x80010013u;c.set_gpr(2,error_);return;}
        mounted_=true;error_=0;files_.set_disc_mounted(true);
        std::cout << "[umd] mounted disc0: state=" << state() << '\n';
        for(auto it=waiters_.begin();it!=waiters_.end();) {
            if(it->second & state()) {threads_.wake(it->first,0u);it=waiters_.erase(it);} else ++it;
        }
        c.set_gpr(2,0u);threads_.dispatch_ready(c);
    }
    void wait(psprecomp::AllegrexContext &c) {
        const auto bits=c.gpr[4];
        if(!(bits & 0x3Fu)) {c.set_gpr(2,0x80010016u);return;}
        if(!threads_.can_wait()) {c.set_gpr(2,0x800201A7u);return;}
        if(bits & state()) {c.set_gpr(2,0u);return;}
        const auto uid=threads_.current_uid();waiters_[uid]=bits;
        threads_.wait(c,std::numeric_limits<std::uint64_t>::max(),[this,uid] {waiters_.erase(uid);});
    }
    psprecomp::Runtime &runtime_;FileRuntime &files_;ThreadRuntime &threads_;
    bool medium_{},mounted_{};std::uint32_t error_{};
    std::map<std::int32_t,std::uint32_t> waiters_;
};
}
