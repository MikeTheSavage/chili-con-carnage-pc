// PSPRecomp contributors, MIT license. Public ABI: PSPSDK pspatrac3.h.
#include "atrac_runtime.hpp"
#include "atrac_stream.hpp"
#include "audio_stats.hpp"
#include "sound_trace.hpp"
#include "av_runtime.hpp"
#include "thread_runtime.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
}
namespace chili {
struct AtracRuntime::Impl {
    psprecomp::Runtime &runtime;
    AvRuntime &av;
    ThreadRuntime *threads;
    std::array<unsigned,4> types{0x1000,0x1000,0x1001,0x1001};
    // Unopened contexts reserve IDs; AtracStream owns each attached decoder.
    std::array<AVCodecContext *,4> contexts{};
    std::array<std::unique_ptr<AtracStream>,4> streams{};
    std::map<int,std::vector<psprecomp::AllegrexContext>> delays;
    // PSPRECOMP_LOG=sound: a line per stream start and end. CHILI_ATRAC_TAP=<folder>: one WAV per
    // stream of the PCM the game is handed, read back from its buffer the way the game reads it.
    const bool trace{sound_log_enabled()};
    const SoundSolo solo{sound_solo()};
    std::filesystem::path tap_dir;
    std::array<std::unique_ptr<WavWriter>,4> taps{};
    std::array<unsigned,4> serials{},decoded_samples{};
    unsigned serial{};
    std::vector<short> tap_pcm;
    void started(unsigned id,unsigned read,unsigned status) {
        taps[id].reset();
        if(status){if(trace)std::cout<<"[sound] stream id="<<id<<" attach failed status=0x"<<std::hex<<status<<std::dec<<'\n';return;}
        const auto info=streams[id]->info();
        serials[id]=++serial;decoded_samples[id]=0;
        if(trace) {
            std::array<std::uint8_t,256> head{};
            const auto bytes=std::min<std::size_t>({head.size(),read-info.data_offset,info.data_size});
            runtime.memory().copy_out(info.buffer+info.data_offset,std::span<std::uint8_t>(head.data(),bytes));
            char text[400];
            std::snprintf(text,sizeof(text),"[sound] stream %u start id=%u out=%.3f guest_us=%lld buffer=0x%08X read=%u capacity=%u riff=%u data=%u channels=%u block=%u samples=%u resident=%d key=%08x\n",
                serials[id],id,sound_output_seconds(),threads?static_cast<long long>(threads->now_us()):0ll,info.buffer,read,info.capacity,info.file_size,info.data_size,
                info.channels,info.block,info.samples,info.resident?1:0,sound_key(std::span<const std::uint8_t>(head.data(),bytes)));
            std::cout<<text;
        }
        if(!tap_dir.empty()) {
            char name[96];std::snprintf(name,sizeof(name),"stream_%04u_id%u_%uch_%u.wav",serials[id],id,info.channels,info.samples);
            try{taps[id]=std::make_unique<WavWriter>(tap_dir/name);}catch(const std::exception &error){std::cerr<<"[sound] "<<error.what()<<'\n';}
        }
    }
    // The game takes a decoded block as stereo frames whatever the stream's channel count
    // (0x0883BC6C: bytes = samples << 2), so that is how the tap reads it.
    void tapped(unsigned id,unsigned pcm,unsigned count_address) {
        const auto samples=runtime.memory().load32(count_address);
        decoded_samples[id]+=samples;
        if(!taps[id] || !pcm || !samples || samples>2048u || !runtime.memory().contains(pcm,samples*4u))return;
        tap_pcm.resize(samples*2u);
        runtime.memory().copy_out(pcm,std::span<std::uint8_t>(reinterpret_cast<std::uint8_t *>(tap_pcm.data()),samples*4u));
        taps[id]->write(tap_pcm);
    }
    void finished(unsigned id) {
        if(trace && serials[id]) {
            char text[160];
            std::snprintf(text,sizeof(text),"[sound] stream %u end id=%u out=%.3f decoded=%u\n",serials[id],id,sound_output_seconds(),decoded_samples[id]);
            std::cout<<text;
        }
        taps[id].reset();serials[id]=0;
    }
    static std::map<psprecomp::Runtime *,Impl *> instances;
    Impl(psprecomp::Runtime &rt,AvRuntime &modules,ThreadRuntime *scheduler):runtime(rt),av(modules),threads(scheduler) {
        if(instances.contains(&rt))throw psprecomp::Error("Only one ATRAC service may bind a runtime");
        instances[&rt]=this;
        if(const char *folder=std::getenv("CHILI_ATRAC_TAP");folder && *folder){tap_dir=folder;std::error_code status;std::filesystem::create_directories(tap_dir,status);}
        av.set_unload_guard(2u,[this]{return std::none_of(contexts.begin(),contexts.end(),[](auto *c){return c!=nullptr;}) && delays.empty();});
        rt.register_function(AtracRuntime::decode_resume_trampoline,&decode_resumed,"chili_atrac_decode_resume");
        rt.register_hle("sceAtrac3plus",0x780F88D1u,[this](auto &r,auto &c){
            const auto type=c.gpr[4];
            if(type!=0x1000u && type!=0x1001u){c.set_gpr(2,0x80630004u);return;}
            if(!av.loaded(2)){r.stop("ATRAC ID allocation requires loaded ATRAC module");return;}
            for(unsigned i=0;i<types.size();++i)if(types[i]==type && !contexts[i]){
                const auto *codec=avcodec_find_decoder(type==0x1000u?AV_CODEC_ID_ATRAC3P:AV_CODEC_ID_ATRAC3);
                if(!codec){r.stop("Native backend lacks requested ATRAC decoder");return;}
                contexts[i]=avcodec_alloc_context3(codec);
                if(!contexts[i]){r.stop("Native ATRAC context allocation failed");return;}
                streams[i]=std::make_unique<AtracStream>(r.memory(),type);
                std::cout<<"[atrac] acquire id="<<i<<" codec="<<type<<'\n';c.set_gpr(2,i);return;
            }
            c.set_gpr(2,0x80630003u);
        });
        auto bind=[&](unsigned nid,auto handler){rt.register_hle("sceAtrac3plus",nid,[this,handler](auto &r,auto &c){
            const auto id=c.gpr[4];
            if(id>=contexts.size() || !contexts[id]){c.set_gpr(2,0x80630005u);return;}
            if(!av.loaded(2)){r.stop("ATRAC service requires loaded ATRAC module");return;}
            handler(r,c,id);
        });};
        bind(0x3F6E26B5u,[this](auto &,auto &c,unsigned id){
            if(trace || !tap_dir.empty())finished(id);
            const auto status=streams[id]->attach(c.gpr[5],c.gpr[6],c.gpr[7]);c.set_gpr(2,status);
            if(trace || !tap_dir.empty())started(id,c.gpr[6],status);
        });
        bind(0x5D268707u,[this](auto &,auto &c,unsigned id){c.set_gpr(2,streams[id]->stream_info(c.gpr[5],c.gpr[6],c.gpr[7]));});
        bind(0x7DB31251u,[this](auto &,auto &c,unsigned id){c.set_gpr(2,streams[id]->add_data(c.gpr[5]));});
        bind(0x9AE849A7u,[this](auto &,auto &c,unsigned id){c.set_gpr(2,streams[id]->remaining(c.gpr[5]));});
        bind(0xE23E3A35u,[this](auto &,auto &c,unsigned id){c.set_gpr(2,streams[id]->position(c.gpr[5]));});
        bind(0x2DD3E298u,[this](auto &,auto &c,unsigned id){c.set_gpr(2,streams[id]->reset_info(c.gpr[5],c.gpr[6]));});
        bind(0x644E5607u,[this](auto &,auto &c,unsigned id){
            const auto status=streams[id]->reset(c.gpr[5],c.gpr[6],c.gpr[7]);c.set_gpr(2,status);
            if(trace) {
                char text[160];
                std::snprintf(text,sizeof(text),"[sound] stream %u reset id=%u out=%.3f to sample=%u status=0x%08X decoded=%u\n",serials[id],id,sound_output_seconds(),c.gpr[5],status,decoded_samples[id]);
                std::cout<<text;
            }
        });
        bind(0xE88F759Bu,[this](auto &,auto &c,unsigned id){c.set_gpr(2,streams[id]->error_info(c.gpr[5]));});
        bind(0x6A8C3CD5u,[this](auto &,auto &c,unsigned id){
            if(threads && !threads->can_wait()){c.set_gpr(2,0x800201A7u);return;}
            const auto decoded=streams[id]->decode(c.gpr[5],c.gpr[6],c.gpr[7],c.gpr[8]);
            c.set_gpr(2,decoded);
            if(decoded==0 && c.gpr[5] && (solo==SoundSolo::Voices || (solo==SoundSolo::MonoStreams && streams[id]->channels()!=1u)))
                runtime.memory().zero(c.gpr[5],runtime.memory().load32(c.gpr[6])*4u);
            if(decoded==0 && (trace || taps[id]))tapped(id,c.gpr[5],c.gpr[6]);
            if(decoded==0 && threads){
                auto resume=c;resume.pc=c.gpr[31];delays[threads->current_uid()].push_back(resume);
                c.gpr[4]=2300;c.gpr[31]=AtracRuntime::decode_resume_trampoline;
                runtime.invoke_import("ThreadManForUser",0xCEADEB47u,c);
            }
        });
        bind(0x61EB33F5u,[this](auto &r,auto &c,unsigned id){
            const bool busy=std::any_of(delays.begin(),delays.end(),[id](const auto &entry){return std::any_of(entry.second.begin(),entry.second.end(),[id](const auto &saved){return saved.gpr[4]==id;});});
            if(busy){r.stop("ATRAC ID release while decode return is pending requires lifecycle handling");return;}
            finished(id);streams[id].reset();avcodec_free_context(&contexts[id]);c.set_gpr(2,0);
        });
    }
    static void decode_resumed(psprecomp::Runtime &rt,psprecomp::AllegrexContext &c){
        auto *self=instances.at(&rt);auto found=self->delays.find(self->threads->current_uid());
        if(found==self->delays.end() || found->second.empty()){rt.stop("ATRAC decode resume without saved thread context");return;}
        c=found->second.back();found->second.pop_back();if(found->second.empty())self->delays.erase(found);
    }
    ~Impl(){av.set_unload_guard(2u,{});instances.erase(&runtime);for(auto &s:streams)s.reset();for(auto &c:contexts)avcodec_free_context(&c);}
};
std::map<psprecomp::Runtime *,AtracRuntime::Impl *> AtracRuntime::Impl::instances;
AtracRuntime::AtracRuntime(psprecomp::Runtime &rt,AvRuntime &av,ThreadRuntime *threads):impl_(std::make_unique<Impl>(rt,av,threads)){}
AtracRuntime::~AtracRuntime()=default;
}
