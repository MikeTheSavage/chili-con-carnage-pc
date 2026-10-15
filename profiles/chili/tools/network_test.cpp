#include "../host/network_runtime.hpp"
#include <iostream>
#include <stdexcept>
int main(){try{
    psprecomp::Runtime rt;chili::PartitionMemory memory(rt,0x09000000u,0x09100000u);chili::NetworkRuntime net(rt,memory);
    auto require=[](bool ok,const char *s){if(!ok)throw std::runtime_error(s);};auto &c=rt.cpu();
    c.gpr[4]=0x08802100u;rt.invoke_import("sceWlanDrv",0x0C622081u,c);require(c.gpr[2]==0 && (rt.memory().load8(c.gpr[4])&3u)==2u,"virtual MAC is not local unicast");
    c.gpr[4]=0x08802110u;rt.invoke_import("sceWlanDrv",0x0C622081u,c);for(unsigned i=0;i<6;++i)require(rt.memory().load8(0x08802100u+i)==rt.memory().load8(0x08802110u+i),"virtual MAC changed within run");
    auto module=[&](unsigned id,bool load){c.gpr[4]=id;rt.invoke_import("sceUtility",load?0x1579A159u:0x64D50C56u,c);};
    module(2,true);require(c.gpr[2]==0x80110803u && !net.adhoc_loaded(),"dependency validation failed");
    module(1,true);require(c.gpr[2]==0 && net.common_loaded(),"common load failed");module(1,true);require(c.gpr[2]==0x80110801u,"duplicate load failed");module(2,true);require(c.gpr[2]==0 && net.adhoc_loaded(),"adhoc load failed");
    c.gpr[4]=0x10000;c.gpr[5]=7;c.gpr[6]=4096;c.gpr[7]=16;c.gpr[8]=4096;rt.invoke_import("sceNet",0x39AF39A6u,c);require(c.gpr[2]==0x80020193u && !net.initialized(),"bad priority initialized network");
    c.gpr[5]=16;rt.invoke_import("sceNet",0x39AF39A6u,c);require(c.gpr[2]==0 && net.initialized(),"Winsock/pool init failed");
    constexpr auto mac=0x08802000u,text=mac+32u,decoded=text+32u;
    const std::array<std::uint8_t,6> address{0x02,0xAB,0x00,0xFF,0x10,0x9C};rt.memory().copy_in(mac,address);
    c.gpr[4]=mac;c.gpr[5]=text;c.gpr[2]=0xDEADBEEFu;rt.invoke_import("sceNet",0x89360950u,c);require(rt.memory().read_c_string(text,18u)=="02:ab:00:ff:10:9c" && c.gpr[2]==0xDEADBEEFu,"binary MAC conversion/void ABI failed");
    c.gpr[4]=text;c.gpr[5]=decoded;rt.invoke_import("sceNet",0xD27961C9u,c);for(unsigned i=0;i<6;++i)require(rt.memory().load8(decoded+i)==address[i],"MAC round trip failed");
    rt.invoke_import("sceNetAdhoc",0xE1D621D7u,c);require(c.gpr[2]==0,"adhoc lifecycle init failed");rt.invoke_import("sceNetAdhoc",0xA62C6F57u,c);
    rt.invoke_import("sceNet",0x281928A9u,c);require(!net.initialized(),"network termination failed");module(2,false);module(1,false);require(!net.common_loaded() && !net.adhoc_loaded(),"module unload failed");
    require(memory.reserve_region(0x100000u,256u,false)==0x09000000u,"network pool/stacks not reclaimed");
    std::cout<<"PASS: module dependency/duplicates/unload, native Winsock/pool lifecycle, priority validation, MAC conversions and guest memory reclamation\n";
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
