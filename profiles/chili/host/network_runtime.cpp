#include "network_runtime.hpp"
#include <winsock2.h>
#include <array>
#include <iostream>
#include <random>
namespace chili {
namespace {constexpr auto not_loaded=0x80110803u,bad_module=0x80110804u,already_loaded=0x80110801u;}
NetworkRuntime::NetworkRuntime(psprecomp::Runtime &rt,PartitionMemory &memory):runtime_(rt),memory_(memory){
    std::random_device random;
    for(auto &byte:local_mac_)byte=static_cast<std::uint8_t>(random());
    local_mac_[0]=static_cast<std::uint8_t>((local_mac_[0]&0xFCu)|0x02u);
    rt.register_hle("sceWlanDrv",0x0C622081u,[this](auto &,auto &c){
        if(!c.gpr[4] || !runtime_.memory().contains(c.gpr[4],6u)){c.set_gpr(2,0x800200D3u);return;}
        runtime_.memory().copy_in(c.gpr[4],local_mac_);c.set_gpr(2,0u);
    });
    rt.register_hle("sceUtility",0x1579A159u,[this](auto &,auto &c){module(c,true);});
    rt.register_hle("sceUtility",0x64D50C56u,[this](auto &,auto &c){module(c,false);});
    rt.register_hle("sceNet",0x39AF39A6u,[this](auto &,auto &c){initialize(c);});
    rt.register_hle("sceNet",0x281928A9u,[this](auto &,auto &c){terminate();c.set_gpr(2,0u);});
    rt.register_hle("sceNet",0x89360950u,[this](auto &,auto &c){mac(c,true);});
    rt.register_hle("sceNet",0xD27961C9u,[this](auto &,auto &c){mac(c,false);});
    rt.register_hle("sceNetAdhoc",0xE1D621D7u,[this](auto &,auto &c){
        if(!adhoc_ || !pool_){c.set_gpr(2,not_loaded);return;}
        if(adhoc_initialized_){c.set_gpr(2,0x80410712u);return;}
        adhoc_initialized_=true;c.set_gpr(2,0u);
    });
    rt.register_hle("sceNetAdhoc",0xA62C6F57u,[this](auto &,auto &c){adhoc_initialized_=false;c.set_gpr(2,0u);});
    // Ad hoc discovery, control and PDP/PTP transport remain strict missing
    // imports until those services are implemented. Init does not imply peers.
}
NetworkRuntime::~NetworkRuntime(){terminate();}
void NetworkRuntime::module(psprecomp::AllegrexContext &c,bool load){
    const auto id=c.gpr[4];std::cout<<"[net] "<<(load?"load":"unload")<<" module="<<id<<'\n';
    if(id!=1u && id!=2u){runtime_.stop("Unsupported network module ID "+std::to_string(id));return;}
    auto &loaded=id==1u?common_:adhoc_;
    if(load){
        if(loaded){c.set_gpr(2,already_loaded);return;}
        if(id==2u && !common_){c.set_gpr(2,not_loaded);return;}
        loaded=true;c.set_gpr(2,0u);
    }else{
        if(!loaded){c.set_gpr(2,not_loaded);return;}
        if((id==1u && (pool_ || adhoc_)) || (id==2u && adhoc_initialized_)){
            runtime_.stop("Network module unload requires terminating dependent services first");return;
        }
        loaded=false;c.set_gpr(2,0u);
    }
}
void NetworkRuntime::initialize(psprecomp::AllegrexContext &c){
    if(!common_){c.set_gpr(2,not_loaded);return;}
    const auto bytes=c.gpr[4];
    if(!bytes || bytes>0x02000000u){c.set_gpr(2,0x800201B7u);return;}
    if(c.gpr[5]<8u || c.gpr[5]>119u || c.gpr[7]<8u || c.gpr[7]>119u){c.set_gpr(2,0x80020193u);return;}
    if(pool_){runtime_.stop("Repeated sceNetInit requires firmware lifecycle parity");return;}
    WSADATA info{};const auto result=WSAStartup(MAKEWORD(2,2),&info);
    if(result){runtime_.stop("Native Winsock initialization failed: "+std::to_string(result));return;}
    winsock_=true;
    pool_=memory_.reserve_region(bytes,256u,false);
    callout_stack_=memory_.reserve_region(4096u,256u,true);
    interrupt_stack_=memory_.reserve_region(4096u,256u,true);
    if(!pool_ || !callout_stack_ || !interrupt_stack_){terminate();c.set_gpr(2,0x80020190u);return;}
    std::cout<<"[net] initialized native Winsock, guest pool="<<bytes<<" bytes\n";
    c.set_gpr(2,0u);
}
void NetworkRuntime::terminate(){
    adhoc_initialized_=false;
    for(auto address:{pool_,callout_stack_,interrupt_stack_})if(address)memory_.release_region(address);
    pool_=callout_stack_=interrupt_stack_=0;
    if(winsock_){WSACleanup();winsock_=false;}
}
void NetworkRuntime::mac(psprecomp::AllegrexContext &c,bool to_string){
    if(!common_){runtime_.stop("MAC conversion requested before common network module load");return;}
    const auto input=c.gpr[4],output=c.gpr[5];
    if(!runtime_.memory().contains(input,to_string?6u:18u) || !runtime_.memory().contains(output,to_string?18u:6u)){
        runtime_.stop("Invalid guest MAC conversion buffer");return;
    }
    constexpr char hex[]="0123456789abcdef";
    if(to_string){
        for(unsigned i=0;i<6;++i){const auto b=runtime_.memory().load8(input+i);runtime_.memory().store8(output+i*3,hex[b>>4]);runtime_.memory().store8(output+i*3+1,hex[b&15]);if(i<5)runtime_.memory().store8(output+i*3+2,':');}
        runtime_.memory().store8(output+17,0u);
    }else{
        auto digit=[](unsigned char ch)->int{if(ch>='0' && ch<='9')return ch-'0';if(ch>='a' && ch<='f')return ch-'a'+10;if(ch>='A' && ch<='F')return ch-'A'+10;return -1;};
        std::array<std::uint8_t,6> bytes{};
        for(unsigned i=0;i<6;++i){auto high=digit(runtime_.memory().load8(input+i*3));auto low=digit(runtime_.memory().load8(input+i*3+1));if(high<0 || low<0 || (i<5 && runtime_.memory().load8(input+i*3+2)!=':')){runtime_.stop("Unsupported MAC string format");return;}bytes[i]=static_cast<std::uint8_t>((high<<4)|low);}
        if(runtime_.memory().load8(input+17)!=0u){runtime_.stop("MAC string is not terminated");return;}
        runtime_.memory().copy_in(output,bytes);
    }
}
}
