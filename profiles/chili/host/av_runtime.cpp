#include "av_runtime.hpp"
#include "sas_runtime.hpp"
#include <iostream>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
namespace chili {
AvRuntime::AvRuntime(psprecomp::Runtime &rt,SasRuntime *sas):sas_(sas),runtime_(rt){
    rt.register_hle("sceUtility",0xC629AF26u,[this](auto &,auto &c){module(c,true);});
    rt.register_hle("sceUtility",0xF7D8D092u,[this](auto &,auto &c){module(c,false);});
}
void AvRuntime::module(psprecomp::AllegrexContext &c,bool load){
    const auto id=c.gpr[4];std::cout<<"[av] "<<(load?"load":"unload")<<" module="<<id<<'\n';
    if(id>=loaded_.size()){c.set_gpr(2,0x80110F01u);return;}
    if(!load){
        if(!loaded_[id]){c.set_gpr(2,0x80110F03u);return;}
        if(unload_guards_[id] && !unload_guards_[id]()) {
            runtime_.stop("AV module unload while native service retains state requires lifecycle handling");return;
        }
        if(id==0u && (loaded_[2] || loaded_[3])){runtime_.stop("AV codec unload requires dependent modules to unload first");return;}
        if(id==1u && (!sas_ || !sas_->unload())){runtime_.stop("SAS module unload state mismatch");return;}
        loaded_[id]=false;c.set_gpr(2,0u);return;
    }
    if(loaded_[id]){c.set_gpr(2,0x80110F02u);return;}
    if(id==2u || id==3u){if(!loaded_[0]){c.set_gpr(2,0x8002013Cu);return;}}
    if(id==1u){
        if(!sas_ || !sas_->load()){runtime_.stop("SAS module requires available native mixer");return;}
        loaded_[id]=true;std::cout<<"[av] native SAS mixer acquired\n";c.set_gpr(2,0u);return;
    }
    if(id!=0u && id!=2u && id!=3u){runtime_.stop("AV module requires unimplemented native service: "+std::to_string(id));return;}
    if(!avcodec_find_decoder(AV_CODEC_ID_H264) || !avcodec_find_decoder(AV_CODEC_ID_ATRAC3P) || !av_find_input_format("mpeg")){
        runtime_.stop("Native AV backend lacks H264/ATRAC3+/MPEG-PS capability");return;
    }
    // Acquire the native backend. Bound codec services gate their APIs on this
    // module state and protect any retained state through unload guards.
    loaded_[id]=true;std::cout<<"[av] native decoder backend version="<<avcodec_version()<<'\n';c.set_gpr(2,0u);
}
}
