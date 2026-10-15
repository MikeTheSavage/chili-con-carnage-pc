#include "../host/av_runtime.hpp"
#include "../host/sas_runtime.hpp"
#include "../host/atrac_runtime.hpp"
#include "vcs_media_decoder.hpp"
#include <iostream>
#include <stdexcept>
int main(){try{
    psprecomp::Runtime rt;chili::SasRuntime sas(rt);chili::AvRuntime av(rt,&sas);chili::AtracRuntime atrac(rt,av);
    auto require=[](bool ok,const char *s){if(!ok)throw std::runtime_error(s);};auto &c=rt.cpu();
    auto module=[&](unsigned id,bool load){c.gpr[4]=id;rt.invoke_import("sceUtility",load?0xC629AF26u:0xF7D8D092u,c);};
    module(3,true);require(c.gpr[2]==0x8002013Cu && !av.loaded(3),"dependency check failed");
    module(1,true);require(c.gpr[2]==0 && av.loaded(1) && sas.loaded(),"SAS-before-codec load failed");
    module(0,true);require(c.gpr[2]==0 && av.loaded(0),"native codec backend unavailable");
    module(0,true);require(c.gpr[2]==0x80110F02u,"duplicate codec load accepted");
    module(2,true);require(c.gpr[2]==0 && av.loaded(2),"ATRAC backend load failed");
    auto acquire=[&](unsigned type){c.gpr[4]=type;rt.invoke_import("sceAtrac3plus",0x780F88D1u,c);return c.gpr[2];};
    require(acquire(123)==0x80630004u,"invalid ATRAC codec accepted");
    require(acquire(0x1000)==0 && acquire(0x1000)==1,"ATRAC IDs not distinct");
    const unsigned sentinel=0x08804000;rt.memory().store32(sentinel,0xDEADBEEF);
    c.gpr[4]=0;c.gpr[5]=sentinel;rt.invoke_import("sceAtrac3plus",0x9AE849A7u,c);
    require(c.gpr[2]==0x80630010u && rt.memory().load32(sentinel)==0xDEADBEEF,"no-data query changed output");
    c.gpr[4]=99;c.gpr[5]=0;rt.invoke_import("sceAtrac3plus",0x9AE849A7u,c);
    require(c.gpr[2]==0x80630005u,"remaining-frame invalid ID precedence failed");
    require(acquire(0x1000)==0x80630003u,"ATRAC exhaustion ignored");
    c.gpr[4]=0;rt.invoke_import("sceAtrac3plus",0x61EB33F5u,c);require(c.gpr[2]==0,"ATRAC release failed");
    require(acquire(0x1000)==0,"ATRAC freed ID not reusable");
    for(unsigned id: {0u,1u}){c.gpr[4]=id;rt.invoke_import("sceAtrac3plus",0x61EB33F5u,c);require(c.gpr[2]==0,"ATRAC cleanup failed");}
    c.gpr[4]=1;rt.invoke_import("sceAtrac3plus",0x61EB33F5u,c);require(c.gpr[2]==0x80630005u,"double ATRAC release accepted");
    module(3,true);require(c.gpr[2]==0 && av.loaded(3),"MPEG backend load failed");
    vcs::VideoStreamDecoder video;vcs::AudioStreamDecoder audio;vcs::PmfAudioDecoder pmf;
    require(!video.is_open() && !audio.is_open() && !pmf.is_open(),"fresh decoder has open stream");
    module(3,false);module(2,false);module(0,false);require(!av.loaded(0) && !av.loaded(2) && !av.loaded(3),"unload state failed");
    module(1,false);require(!av.loaded(1) && !sas.loaded(),"SAS unload failed");
    module(0,false);require(c.gpr[2]==0x80110F03u,"unloaded codec unload accepted");
    std::cout<<"PASS: native codec capability/dependency/duplicate/unload checks and decoder construction; no stream decoding verified\n";
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
