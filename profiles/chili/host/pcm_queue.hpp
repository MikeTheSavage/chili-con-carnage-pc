#pragma once
#include "audio_feed.hpp"
#include <algorithm>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <vector>
#include <stdexcept>
namespace chili {
// Where sceAudio output blocks go: at most two queued, completed in order.
class PcmSink {
public:
    virtual ~PcmSink()=default;
    virtual unsigned pending()=0;
    virtual unsigned long long completed()=0;
    virtual bool submit(std::span<const short> samples,unsigned volume)=0;
    // Guest microseconds a blocked writer should sleep before it looks again.
    virtual unsigned retry_delay_us(){return 1000u;}
    // Frames accepted and not yet played, for the statistics.
    virtual unsigned queued_frames(){return 0u;}
};
// No device: a block completes once its playing time (44.1 kHz stereo) has
// passed on the supplied guest clock. Used by deterministic runs, where audio
// pacing must depend on guest time only.
class GuestClockPcmSink final:public PcmSink {
    std::function<unsigned long long()> now_us_;
    std::deque<unsigned long long> finish_;
    unsigned long long completed_{},anchor_us_{},anchor_frames_{};
public:
    explicit GuestClockPcmSink(std::function<unsigned long long()> now_us):now_us_(std::move(now_us)){}
    unsigned pending() override {
        const auto now=now_us_();
        while(!finish_.empty() && finish_.front()<=now){finish_.pop_front();++completed_;}
        return static_cast<unsigned>(finish_.size());
    }
    unsigned long long completed() override {pending();return completed_;}
    bool submit(std::span<const short> samples,unsigned) override {
        if(pending()>=2)return false;
        // An empty queue restarts the stream clock; otherwise blocks play back
        // to back, timed from the anchor so rounding never accumulates.
        if(finish_.empty()){anchor_us_=now_us_();anchor_frames_=0;}
        anchor_frames_+=samples.size()/2u;
        finish_.push_back(anchor_us_+anchor_frames_*1000000ull/44100ull);
        return true;
    }
    unsigned queued_frames() override {
        if(finish_.empty())return 0u;
        const auto now=now_us_();
        return finish_.back()>now?static_cast<unsigned>((finish_.back()-now)*44100ull/1000000ull):0u;
    }
    unsigned retry_delay_us() override {
        if(finish_.empty())return 1000u;
        const auto now=now_us_();
        return finish_.front()>now?static_cast<unsigned>(std::min<unsigned long long>(finish_.front()-now,1000000ull)):1u;
    }
    // For a sink that also answers to a device: the blocks still queued, without
    // looking at the clock, and all of them declared played.
    unsigned held() const noexcept {return static_cast<unsigned>(finish_.size());}
    unsigned long long completed_so_far() const noexcept {return completed_;}
    void complete_all() noexcept {completed_+=finish_.size();finish_.clear();}
};
// How the game's two-block queue is released while a device plays.
//
// The blocks go into the host ring (HostPcmFeed) and the queue the game sees
// is timed on the guest clock, exactly like the deterministic sink. The device
// overrides that clock at both ends:
//   - ring below low(): the game is behind the device (a long frame, a host
//     that is slower than real time). Every queued block counts as played, so
//     the Sound thread refills the ring at once instead of leaving a gap.
//   - ring above high(): the game is ahead of the device. Nothing completes
//     until the device has caught up.
// The mixer's 512-frame blocks come from a pure producer, so for them low() is
// the whole lead: the ring is topped up after every frame of work. The movie
// player sends 2048-frame blocks and checks them against the guest clock: when
// its sound is ahead of that clock it sends a block of silence instead. Releasing
// its queue early therefore puts holes in the movie's sound (measured: 103 holes
// in the two intro movies with the device as the only clock, none with the guest
// clock), so long blocks are left to the guest clock until half the lead is gone.
// band_frames 0 makes the device the only clock; catch_up false with a huge
// band leaves the guest clock alone.
struct AudioPolicy {
    unsigned lead_frames{3072u},band_frames{1024u};
    bool catch_up{true};
    static constexpr unsigned mixer_block=512u;
    unsigned low(unsigned block_frames) const noexcept {return block_frames>mixer_block?lead_frames/2u:lead_frames;}
    unsigned high(unsigned block_frames) const noexcept {return lead_frames+(band_frames?std::max(band_frames,block_frames*2u):0u);}
};
class LivePcmSink final:public PcmSink {
    GuestClockPcmSink timing_;
    HostPcmFeed &feed_;
    const AudioPolicy policy_;
    std::vector<short> scaled_;
    unsigned block_frames_{AudioPolicy::mixer_block};
public:
    LivePcmSink(std::function<unsigned long long()> now_us,HostPcmFeed &feed,const AudioPolicy &policy):timing_(std::move(now_us)),feed_(feed),policy_(policy){}
    unsigned pending() override {
        const auto lead=feed_.lead_frames();
        if(policy_.catch_up && lead<policy_.low(block_frames_)){timing_.complete_all();return 0u;}
        if(lead>policy_.high(block_frames_))return timing_.held();
        return timing_.pending();
    }
    unsigned long long completed() override {pending();return timing_.completed_so_far();}
    bool submit(std::span<const short> samples,unsigned volume) override {
        if(pending()>=2)return false;
        scaled_.resize(samples.size());
        for(std::size_t i=0;i<samples.size();++i){
            const auto v=static_cast<long long>(samples[i])*volume/0x8000;
            scaled_[i]=static_cast<short>(v<-32768?-32768:v>32767?32767:v);
        }
        if(!feed_.write(scaled_))return false;
        block_frames_=static_cast<unsigned>(samples.size()/2u);
        return timing_.submit(samples,volume);
    }
    unsigned queued_frames() override {return feed_.lead_frames();}
    unsigned retry_delay_us() override {
        const auto us=[](unsigned frames){return static_cast<unsigned>(frames*1000000ull/HostPcmFeed::rate);};
        const auto lead=feed_.lead_frames(),low=policy_.low(block_frames_),high=policy_.high(block_frames_);
        if(lead>high)return std::clamp(us(lead-high),200u,20000u);
        auto delay=timing_.retry_delay_us();
        if(policy_.catch_up && lead>=low)delay=std::min(delay,std::max(us(lead-low)+100u,200u));
        return delay;
    }
};
}
