#pragma once
#include "psprecomp/runtime.hpp"
#include "psmf_header.hpp"
#include "disc_image.hpp"
#include <map>
#include <fstream>
#include <iostream>
namespace chili {
class PsmfRuntime {
    bool loaded_{},started_{};
    std::map<unsigned,PsmfHeader> headers_;
    static constexpr unsigned uid=0x80000;
public:
    explicit PsmfRuntime(psprecomp::Runtime &rt){
        rt.register_hle("ModuleMgrForUser",0x977DE386u,[this](auto &r,auto &c){
            std::string path;bool end=false;
            for(unsigned i=0;i<1024;++i){if(!r.memory().contains(c.gpr[4]+i,1)){c.set_gpr(2,0x800200D3u);return;}auto ch=r.memory().load8(c.gpr[4]+i);if(!ch){end=true;break;}path+=static_cast<char>(ch);}
            std::cout<<"[module] load "<<path<<'\n';
            if(!end || path!="disc0:/PSP_GAME/USRDIR/module/psmf.prx" || c.gpr[5] || c.gpr[6]){r.stop("Module load requires unsupported path/flags/options semantics");return;}
            if(loaded_){r.stop("Duplicate native PSMF module load requires firmware parity");return;}
            // The disc is a folder, or an .iso read in place (disc_image.hpp).
            std::vector<unsigned char> bytes;
            if(disc_iso)bytes=disc_iso->bytes("PSP_GAME/USRDIR/module/psmf.prx",1u<<20u);
            else{std::ifstream file(r.game_root()/"PSP_GAME/USRDIR/module/psmf.prx",std::ios::binary);bytes.assign(std::istreambuf_iterator<char>(file),{});}
            const std::string name="scePsmf_library";
            if(bytes.size()!=6320 || !std::equal(bytes.begin(),bytes.begin()+4,"~SCE") || !std::equal(bytes.begin()+64,bytes.begin()+68,"~PSP") || !std::equal(name.begin(),name.end(),bytes.begin()+74)){r.stop("Unsupported PSMF PRX identity");return;}
            loaded_=true;started_=false;headers_.clear();c.set_gpr(2,uid);
        });
        rt.register_hle("ModuleMgrForUser",0x50F0C1ECu,[this](auto &r,auto &c){
            std::cout<<"[module] start uid="<<c.gpr[4]<<" args="<<c.gpr[5]<<" status="<<c.gpr[7]<<" options="<<c.gpr[8]<<'\n';
            if(!loaded_ || c.gpr[4]!=uid){c.set_gpr(2,0x8002012Eu);return;}
            if(started_ || c.gpr[5] || c.gpr[8]){r.stop("PSMF module start requires unsupported lifecycle/args/options semantics");return;}
            if(c.gpr[7] && !r.memory().contains(c.gpr[7],4)){c.set_gpr(2,0x800200D3u);return;}
            started_=true;if(c.gpr[7])r.memory().store32(c.gpr[7],0);c.set_gpr(2,uid);
        });
        rt.register_hle("scePsmf",0xC22C8327u,[this](auto &r,auto &c){
            if(!started_){r.stop("PSMF service requires started module");return;}
            const auto object=c.gpr[4],data=c.gpr[5];
            if(!r.memory().contains(object,32) || !r.memory().contains(data,2048)){c.set_gpr(2,0x800200D3u);return;}
            std::vector<std::uint8_t> bytes(2048);for(unsigned i=0;i<bytes.size();++i)bytes[i]=r.memory().load8(data+i);
            PsmfHeader header;
            try{header=parse_psmf_header(bytes);}catch(const std::runtime_error &e){const std::string what=e.what();c.set_gpr(2,what.find("version")!=std::string::npos?0x80615002u:what.find("offset")!=std::string::npos?0x806151FEu:0x80615501u);return;}
            r.memory().zero(object,32);r.memory().store32(object,header.version);r.memory().store32(object+4,2048);r.memory().store32(object+8,data);r.memory().store32(object+12,header.stream_size);r.memory().store32(object+20,header.stream_types.empty()?0xFFFFFFFFu:0u);
            headers_[data]=std::move(header);c.set_gpr(2,0);
        });
        rt.register_hle("scePsmf",0x68D42328u,[this](auto &r,auto &c){
            if(!started_){r.stop("PSMF service requires started module");return;}
            if(!r.memory().contains(c.gpr[4],32)){c.set_gpr(2,0x800200D3u);return;}
            auto i=headers_.find(r.memory().load32(c.gpr[4]+8));
            if(i==headers_.end()){c.set_gpr(2,0x80615001u);return;}
            c.set_gpr(2,i->second.count(c.gpr[5]));
        });
    }
};
}
