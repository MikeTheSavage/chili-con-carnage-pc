#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <ostream>
#include <share.h>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
namespace chili {
// 16-bit stereo 44.1 kHz WAV, written as the samples arrive. The sizes in the
// header are filled in on close.
class WavWriter {
    std::FILE *file_{};
    std::uint64_t frames_{};
    static void put32(unsigned char *p,std::uint32_t v){for(unsigned i=0;i<4u;++i)p[i]=static_cast<unsigned char>(v>>(i*8u));}
    void header(){
        unsigned char h[44]{'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,
            0,0,0,0,0,0,0,0,4,0,16,0,'d','a','t','a',0,0,0,0};
        const auto bytes=static_cast<std::uint32_t>(std::min<std::uint64_t>(frames_*4u,0xFFFFFFFFull-36u));
        put32(h+4,bytes+36u);put32(h+24,44100u);put32(h+28,176400u);put32(h+40,bytes);
        std::fseek(file_,0,SEEK_SET);std::fwrite(h,1u,sizeof(h),file_);
    }
public:
    explicit WavWriter(const std::filesystem::path &path){
        file_=_wfsopen(path.c_str(),L"wb",_SH_DENYWR);
        if(!file_)throw std::runtime_error("Cannot create audio capture: "+path.string());
        std::setvbuf(file_,nullptr,_IOFBF,1u<<20u);header();
    }
    WavWriter(const WavWriter &)=delete;
    WavWriter &operator=(const WavWriter &)=delete;
    ~WavWriter(){header();std::fclose(file_);}
    void write(std::span<const short> interleaved){
        std::fwrite(interleaved.data(),sizeof(short),interleaved.size(),file_);frames_+=interleaved.size()/2u;
    }
    void silence(std::uint64_t frames){
        static const short zero[1024]{};
        frames_+=frames;
        for(auto left=frames*2u;left;){const auto n=std::min<std::uint64_t>(left,1024u);std::fwrite(zero,sizeof(short),static_cast<std::size_t>(n),file_);left-=n;}
    }
    std::uint64_t frames() const noexcept {return frames_;}
};
// What the output side saw. Written by whichever thread feeds the device, read by the reporter.
struct AudioDeviceCounters {
    std::atomic<std::uint64_t> underruns{},dry_frames{},played_frames{};
    // Times the game took its output again while sound from before was still queued, and the frames skipped.
    std::atomic<std::uint64_t> restarts{},dropped_frames{};
    // Performance-counter time of the device's first and latest request, and the frames it took after the first.
    std::atomic<long long> first_request{},last_request{};
    std::atomic<std::uint64_t> frames_since_first{};
};
// Counters of the guest's audio output, for the exit line, the per-second line
// of the audio log channel and the optional per-call trace. A call here is one
// sceAudioOutput2OutputBlocking that carried samples.
class AudioStats {
public:
    using Clock=std::chrono::steady_clock;
    struct Call {std::uint64_t guest_call,wall_call,guest_return,wall_return;std::uint32_t queued_frames,retries;};
    AudioDeviceCounters device;
    explicit AudioStats(bool per_second,std::filesystem::path trace={}):per_second_(per_second),trace_(std::move(trace)){
        if(!trace_.empty())calls_.reserve(1u<<16u);
    }
    // Ticks per second of the counter AudioDeviceCounters is stamped with (audio_feed.cpp).
    static double performance_frequency();
    static std::uint64_t wall_us(Clock::time_point at,Clock::time_point since){
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(at-since).count());
    }
    // The call was accepted with `queued_frames` still to be played in front of it.
    void call(std::uint64_t guest_us,unsigned frames,unsigned queued_frames){
        const auto now=Clock::now();
        if(!total_.blocks){started_=now;window_started_=now;guest_started_=guest_us;window_guest_=guest_us;}
        else if(returned_){
            // From the previous return to this call: the Sound thread's own time.
            const auto gap_guest=guest_us-last_.guest_return,gap_wall=wall_us(now,started_)-last_.wall_return;
            for(auto *w:{&total_,&window_}){w->gap_guest_us+=gap_guest;w->gap_wall_us+=gap_wall;
                w->gap_guest_max=std::max(w->gap_guest_max,gap_guest);w->gap_wall_max=std::max(w->gap_wall_max,gap_wall);}
        }
        last_={guest_us,wall_us(now,started_),0u,0u,queued_frames,0u};returned_=false;
        for(auto *w:{&total_,&window_}){++w->blocks;w->frames+=frames;w->depth_sum+=queued_frames;
            w->depth_min=std::min<std::uint64_t>(w->depth_min,queued_frames);w->depth_max=std::max<std::uint64_t>(w->depth_max,queued_frames);}
    }
    // The calling guest thread goes on.
    void returned(std::uint64_t guest_us,unsigned retries){
        if(returned_ || !total_.blocks)return;
        const auto now=Clock::now();
        last_.guest_return=guest_us;last_.wall_return=wall_us(now,started_);last_.retries=retries;returned_=true;
        const auto wait_guest=guest_us-last_.guest_call,wait_wall=last_.wall_return-last_.wall_call;
        for(auto *w:{&total_,&window_}){w->wait_guest_us+=wait_guest;w->wait_wall_us+=wait_wall;
            w->wait_guest_max=std::max(w->wait_guest_max,wait_guest);w->wait_wall_max=std::max(w->wait_wall_max,wait_wall);}
        if(!trace_.empty())calls_.push_back(last_);
        last_guest_=guest_us;last_wall_=now;
    }
    // Once a second of host time, when the audio log channel is on.
    void tick(std::ostream &out,std::uint64_t guest_us){
        if(!per_second_ || !total_.blocks)return;
        const auto now=Clock::now();
        if(now-window_started_<std::chrono::seconds(1))return;
        line(out,"[audio-stats]",window_,wall_us(now,window_started_),guest_us-window_guest_,wall_us(now,started_),guest_us,window_device_);
        window_={};window_started_=now;window_guest_=guest_us;window_device_=snapshot();
    }
    void report(std::ostream &out){
        if(!total_.blocks){out<<"[audio] no output blocks\n";return;}
        line(out,"[audio] total",total_,wall_us(last_wall_,started_),last_guest_-guest_started_,wall_us(last_wall_,started_),last_guest_,{});
        if(const auto first=device.first_request.load(),last=device.last_request.load();last>first){
            // Frames the device took per second of host time: 44100 unless the device or the feeder is off.
            char text[160];
            std::snprintf(text,sizeof(text),"[audio] device took %.1f frames per second over %.1f s\n",
                device.frames_since_first.load()*performance_frequency()/static_cast<double>(last-first),(last-first)/performance_frequency());
            out<<text;
        }
        if(const auto restarts=device.restarts.load())
            out<<"[audio] output taken again with sound still queued: "<<restarts<<" times, "<<device.dropped_frames.load()*1000u/44100u<<" ms skipped\n";
        if(trace_.empty())return;
        if(auto *file=_wfsopen(trace_.c_str(),L"wb",_SH_DENYWR)){
            std::fputs("call,guest_call_us,wall_call_us,guest_return_us,wall_return_us,queued_frames,retries\n",file);
            for(std::size_t i=0;i<calls_.size();++i){const auto &c=calls_[i];
                std::fprintf(file,"%zu,%llu,%llu,%llu,%llu,%u,%u\n",i,static_cast<unsigned long long>(c.guest_call),
                    static_cast<unsigned long long>(c.wall_call),static_cast<unsigned long long>(c.guest_return),
                    static_cast<unsigned long long>(c.wall_return),c.queued_frames,c.retries);}
            std::fclose(file);
        }
    }
private:
    struct Window {
        std::uint64_t blocks{},frames{},wait_guest_us{},wait_guest_max{},wait_wall_us{},wait_wall_max{};
        std::uint64_t gap_guest_us{},gap_guest_max{},gap_wall_us{},gap_wall_max{};
        std::uint64_t depth_sum{},depth_min{~0ull},depth_max{};
    };
    struct Device {std::uint64_t underruns{},dry_frames{},played_frames{};};
    Device snapshot() const {return {device.underruns.load(std::memory_order_relaxed),device.dry_frames.load(std::memory_order_relaxed),
        device.played_frames.load(std::memory_order_relaxed)};}
    void line(std::ostream &out,const char *label,const Window &w,std::uint64_t wall,std::uint64_t guest,std::uint64_t wall_total,std::uint64_t guest_total,const Device &before) const {
        const auto now=snapshot();
        const auto blocks=static_cast<double>(std::max<std::uint64_t>(w.blocks,1u));
        const auto frames_to_ms=[](double frames){return frames*1000.0/44100.0;};
        char text[640];
        std::snprintf(text,sizeof(text),"%s wall_s=%.3f guest_s=%.3f span_wall_s=%.3f span_guest_s=%.3f blocks=%llu per_guest_s=%.2f per_wall_s=%.2f"
            " wait_guest_ms=%.2f/%.2f wait_wall_ms=%.2f/%.2f between_guest_ms=%.2f/%.2f between_wall_ms=%.2f/%.2f"
            " depth_ms=%.1f/%.1f/%.1f underruns=%llu dry_ms=%.1f played_s=%.3f\n",label,
            wall_total/1e6,guest_total/1e6,wall/1e6,guest/1e6,static_cast<unsigned long long>(w.blocks),
            guest?w.blocks*1e6/guest:0.0,wall?w.blocks*1e6/wall:0.0,
            w.wait_guest_us/blocks/1000.0,w.wait_guest_max/1000.0,w.wait_wall_us/blocks/1000.0,w.wait_wall_max/1000.0,
            w.gap_guest_us/blocks/1000.0,w.gap_guest_max/1000.0,w.gap_wall_us/blocks/1000.0,w.gap_wall_max/1000.0,
            frames_to_ms(w.blocks?static_cast<double>(w.depth_min):0.0),frames_to_ms(w.depth_sum/blocks),frames_to_ms(static_cast<double>(w.depth_max)),
            static_cast<unsigned long long>(now.underruns-before.underruns),frames_to_ms(static_cast<double>(now.dry_frames-before.dry_frames)),
            (now.played_frames-before.played_frames)/44100.0);
        out<<text;
    }
    bool per_second_,returned_{true};
    std::filesystem::path trace_;
    std::vector<Call> calls_;
    Window total_,window_;
    Device window_device_;
    Call last_{};
    Clock::time_point started_{},window_started_{},last_wall_{};
    std::uint64_t guest_started_{},window_guest_{},last_guest_{};
};
}
