#include "audio_runtime.hpp"
#include "audio_stats.hpp"
#include "pcm_queue.hpp"
#include "sound_trace.hpp"
#include "thread_runtime.hpp"
#include "performance_telemetry.hpp"
#include "host_log.hpp"
#include "frame_costs.hpp"
#include <map>
#include <vector>
#include <iostream>
namespace chili {
struct AudioRuntime::Impl {
    static Impl *active;
    psprecomp::Runtime &runtime;
    ThreadRuntime *threads;
    PerformanceTelemetry *pacing_observer{};
    std::unique_ptr<PcmSink> queue;
    bool completion{};
    unsigned long long observed{};
    // retry: the call found the queue full and starts over when the thread wakes.
    struct Wait {psprecomp::AllegrexContext context;unsigned long long serial;unsigned retries;bool retry;};
    std::map<int,Wait> waits;
    // Reserved is the guest-visible state; the host feed exists only with a device sink.
    bool reserved{};
    const AudioOptions options;
    const bool guest_clock_sink;
    const bool log{log_enabled(LogChannel::Audio)};
    unsigned frames{};
    std::vector<short> pcm;
    AudioStats stats;
    // Declared before the feed: the feeder thread writes the capture until it is joined.
    std::unique_ptr<WavWriter> capture;
    std::unique_ptr<HostPcmFeed> feed;
    std::uint64_t guest_now() const {return threads?static_cast<std::uint64_t>(threads->now_us()):0u;}
    explicit Impl(psprecomp::Runtime &rt,ThreadRuntime *scheduler,const AudioOptions &settings):runtime(rt),threads(scheduler),options(settings),
        guest_clock_sink(settings.guest_clock_sink),stats(log_enabled(LogChannel::Audio),settings.trace){
        if(guest_clock_sink && !threads)throw std::runtime_error("Guest-clocked audio sink requires the scheduler");
        // With a device the feeder writes the capture, silence included. Without one (a deterministic
        // run) the blocks are written back to back as the game hands them over: the same file every run.
        if(!options.capture.empty())capture=std::make_unique<WavWriter>(options.capture);
        if(threads){active=this;rt.register_function(0x0800000Cu,&resume,"chili_audio_resume");}
        rt.register_hle("sceAudio",0x2D53F36Eu,[this](auto &,auto &c){output(c);});
        rt.register_hle("sceAudio",0x647CEF33u,[this](auto &,auto &c){
            if(!queue){c.set_gpr(2,0x80260008u);return;}
            c.set_gpr(2,queue->pending()*frames);
        });
        rt.register_hle("sceAudio",0x01562BA3u,[this](auto &,auto &c){
            const auto count=c.gpr[4]&0x7FFFFFFFu;
            if(count<17u || count>4111u){c.set_gpr(2,0x80000104u);return;}
            if(reserved){c.set_gpr(2,0x80268002u);return;}
            const auto clock=[this]{return static_cast<unsigned long long>(guest_now());};
            if(guest_clock_sink)queue=std::make_unique<GuestClockPcmSink>(clock);
            else{
                const AudioPolicy policy{options.lead_frames,options.band_frames,options.catch_up};
                // The device stays open across a release. Sound the game queued before it gave the
                // output up plays on until it takes the output again; what is left then is dropped.
                if(feed)feed->restart();
                else{
                    // The device, not the frame's idle, decides when the Sound thread is needed.
                    if(threads)threads->set_tick_hook([this]{
                        if(!queue)return;
                        for(auto &[uid,wait]:waits)if(wait.retry?queue->pending()<2:queue->completed()>wait.serial)threads->expedite(uid);
                    });
                    feed=std::make_unique<HostPcmFeed>(policy.catch_up?policy.lead_frames-std::min(policy.lead_frames/2u,512u):policy.lead_frames,stats.device,capture.get());
                    std::cout<<"[audio] output: "<<feed->device_report()<<"; lead "<<policy.lead_frames*1000u/HostPcmFeed::rate<<" ms, band "
                             <<policy.band_frames*1000u/HostPcmFeed::rate<<" ms"<<(policy.catch_up?"":", guest clock only")<<'\n';
                }
                queue=std::make_unique<LivePcmSink>(clock,*feed,policy);
            }
            reserved=true;completion=false;observed=0;pcm.assign(count*2,0);frames=count;
            if(log)std::cout<<"[audio] native Output2 reserved frames="<<frames<<" rate=44100 stereo"<<(guest_clock_sink?" (guest-clocked null sink)":"")<<'\n';
            c.set_gpr(2,0);
        });
        rt.register_hle("sceAudio",0x63F2889Cu,[this](auto &,auto &c){
            const auto count=c.gpr[4];
            if(count-17u>=0xFFFu){c.set_gpr(2,0x80260006u);return;}
            if(!reserved){c.set_gpr(2,0x80260008u);return;}
            if(count!=frames){
                if(log)std::cout<<"[audio] Output2 length="<<count<<'\n';
                pcm.resize(count*2);frames=count;
            }
            c.set_gpr(2,0);
        });
        rt.register_hle("sceAudio",0x43196845u,[this](auto &,auto &c){
            if(!reserved){c.set_gpr(2,0x80260008u);return;}
            if(queue->pending()){c.set_gpr(2,0x80268002u);return;}
            queue.reset();
            reserved=false;frames=0;pcm.clear();c.set_gpr(2,0);
        });
    }
    // True when the calling thread was put to sleep (its context is then no longer `c`).
    bool output(psprecomp::AllegrexContext &c){
        const auto volume=c.gpr[4],buffer=c.gpr[5];
        if(volume>0xFFFFFu){c.set_gpr(2,0x8026000Bu);return false;}
        if(!queue){c.set_gpr(2,0x80260008u);return false;}
        const auto count=queue->pending();const auto serial=queue->completed();
        if(serial!=observed){completion=true;observed=serial;}
        if(count>=2){
            // A PSP blocks here. With a device the queue can be full on entry while
            // the device is behind; the game ignores the error and would lose the block.
            if(guest_clock_sink || !threads || !threads->can_wait()){c.set_gpr(2,0x80260002u);return false;}
            waits[threads->current_uid()]={c,serial,0u,true};delay(c);return true;
        }
        unsigned result{};
        if(buffer){
            if(!runtime.memory().contains(buffer,frames*4)){c.set_gpr(2,0x800200D3u);return false;}
            CostTimer cost(frame_costs.audio_ns);++frame_costs.audio_blocks;
            for(unsigned i=0;i<frames*2;++i)pcm[i]=static_cast<short>(runtime.memory().load16(buffer+i*2));
            const auto queued=queue->queued_frames();
            if(!queue->submit(pcm,volume)){c.set_gpr(2,0x80260002u);return false;}
            sound_output_frames()+=frames;
            if(guest_clock_sink && capture){
                if(volume!=0x8000u)for(auto &sample:pcm){const auto v=static_cast<long long>(sample)*volume/0x8000;sample=static_cast<short>(v<-32768?-32768:v>32767?32767:v);}
                capture->write(pcm);
            }
            stats.call(guest_now(),frames,queued);
            result=frames;if(count==0)completion=true;
        }else if(count==0){c.set_gpr(2,0);return false;}
        if(threads && !threads->can_wait()){c.set_gpr(2,0x800201A7u);return false;}
        c.set_gpr(2,result);
        if(completion){completion=false;stats.returned(guest_now(),0u);if(log)stats.tick(std::cout,guest_now());return false;}
        if(!threads){runtime.stop("Audio completion wait requires scheduler");return false;}
        waits[threads->current_uid()]={c,serial,0u,false};delay(c);return true;
    }
    void delay(psprecomp::AllegrexContext &c){c.gpr[4]=queue->retry_delay_us();c.gpr[31]=0x0800000Cu;runtime.invoke_import("ThreadManForUser",0xCEADEB47u,c);}
    static void resume(psprecomp::Runtime &,psprecomp::AllegrexContext &c){
        auto &s=*active;auto i=s.waits.find(s.threads->current_uid());
        if(i==s.waits.end()){s.runtime.stop("Audio resume without pending wait");return;}
        if(i->second.retry){
            if(s.queue && s.queue->pending()>=2){s.delay(c);return;}
            c=i->second.context;s.waits.erase(i);
            if(!s.output(c))c.pc=c.gpr[31];
            return;
        }
        if(!s.queue || s.queue->completed()>i->second.serial){
            if(s.pacing_observer)s.pacing_observer->audio_resume(true);
            s.stats.returned(s.guest_now(),i->second.retries);if(s.log)s.stats.tick(std::cout,s.guest_now());
            c=i->second.context;c.pc=c.gpr[31];if(s.queue)s.observed=s.queue->completed();s.waits.erase(i);return;
        }
        if(s.pacing_observer)s.pacing_observer->audio_resume(false);
        ++i->second.retries;
        s.delay(c);
    }
    ~Impl(){if(feed && threads)threads->set_tick_hook({});queue.reset();feed.reset();if(active==this)active=nullptr;}
};
AudioRuntime::Impl *AudioRuntime::Impl::active{};
AudioRuntime::AudioRuntime(psprecomp::Runtime &rt,ThreadRuntime *threads,bool guest_clock_sink):AudioRuntime(rt,threads,AudioOptions{guest_clock_sink}){}
AudioRuntime::AudioRuntime(psprecomp::Runtime &rt,ThreadRuntime *threads,const AudioOptions &options):impl_(std::make_unique<Impl>(rt,threads,options)){}
AudioRuntime::~AudioRuntime()=default;
void AudioRuntime::set_pacing_observer(PerformanceTelemetry *observer) noexcept {impl_->pacing_observer=observer;}
void AudioRuntime::report(std::ostream &out){impl_->stats.report(out);}
}
