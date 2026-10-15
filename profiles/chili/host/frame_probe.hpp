#pragma once
#include "display_runtime.hpp"
#include "frame_timer.hpp"
#include "pc_window.hpp"
#include "run_options.hpp"
#include "state_hash.hpp"
#include <bit>
#include <cstdio>
#include <functional>
#include <iostream>
#include <utility>
namespace chili {
// Everything measured at a game frame boundary: frame times, the optional
// state hash, the optional frame image and the optional frame limit. Runs once
// per game frame from the display runtime's frame-boundary hook.
class FrameProbe {
public:
    inline static const char frame_limit_reason[]="Frame limit reached";
    FrameProbe(psprecomp::Runtime &runtime,ThreadRuntime &threads,DisplayRuntime &display,const RunOptions &options,
        std::function<std::uint64_t()> controller_samples)
        :runtime_(runtime),threads_(threads),display_(display),timer_(options.frame_log),samples_(std::move(controller_samples)),
         hash_every_(options.state_hash_every),stop_at_(options.stop_at_frame),png_every_(options.frame_png_every),
         png_dir_(options.frame_png_dir),hash_exclude_(options.state_hash_exclude),hash_regions_(options.state_hash_regions){
        timer_.make_active();
        display.set_frame_boundary(frame_end_vblank_return,[this](psprecomp::AllegrexContext &ctx){boundary(ctx);});
    }
    ~FrameProbe(){display_.set_frame_boundary(0u,{});report();}
    FrameProbe(const FrameProbe &)=delete;
    FrameProbe &operator=(const FrameProbe &)=delete;
    // Game frames completed so far (1-based number of the last boundary).
    std::uint64_t frame() const noexcept {return frame_;}
    // Replaces the guest-VRAM frame image with the caller's (a hardware
    // renderer reads its own surface back).
    void set_frame_saver(std::function<bool(const std::filesystem::path &)> saver){frame_saver_=std::move(saver);}
    // Runs at every frame boundary after the measurements, before the engine's vblank wait: the place to
    // draw the scene the game is about to enqueue (GeRuntime::run_early). Its time belongs to the new frame.
    void set_frame_draw(std::function<void()> draw){frame_draw_=std::move(draw);}
    // Runs at every frame boundary before that, with the number of the frame that ended: what the new
    // frame is drawn with is decided here (the on-screen button prompts, prompt_style.hpp).
    void set_frame_start(std::function<void(std::uint64_t)> start){frame_start_=std::move(start);}
    void report(){if(!reported_){reported_=true;timer_.flush();timer_.report(std::cout);std::cout.flush();}}
private:
    void boundary(psprecomp::AllegrexContext &ctx){
        const auto guest_us=threads_.now_us();
        const auto dt=std::bit_cast<float>(runtime_.memory().load32(real_delta_time_address));
        frame_=timer_.boundary(guest_us,samples_?samples_():0u,dt);
        const bool last=stop_at_ && frame_>=stop_at_;
        if(hash_every_ && (frame_%hash_every_==0u || last)){
            const auto hash=hash_guest_state(runtime_.memory(),ctx,guest_us,hash_exclude_);
            char line[224];
            std::snprintf(line,sizeof(line),"[state-hash] frame=%llu hash=%016llx ram=%016llx cpu=%016llx clock_us=%llu samples=%llu",
                static_cast<unsigned long long>(frame_),static_cast<unsigned long long>(hash.combined),
                static_cast<unsigned long long>(hash.ram),static_cast<unsigned long long>(hash.cpu),
                static_cast<unsigned long long>(guest_us),static_cast<unsigned long long>(samples_?samples_():0u));
            std::cout<<line<<'\n';
            if(hash_regions_){
                std::cout<<"[state-hash-regions] frame="<<frame_;
                for(const auto region:hash_ram_regions(runtime_.memory(),hash_exclude_)){
                    std::snprintf(line,sizeof(line)," %08x",static_cast<unsigned>(region^(region>>32u)));std::cout<<line;
                }
                std::cout<<'\n';
            }
            std::cout.flush();
        }
        if(png_every_ && (frame_%png_every_==0u || last)){
            const auto snapshot=display_.snapshot();
            char name[40];std::snprintf(name,sizeof(name),"frame_%08llu.png",static_cast<unsigned long long>(frame_));
            const bool saved=frame_saver_?frame_saver_(png_dir_/name):save_display_frame(runtime_.memory(),snapshot.address,snapshot.stride,snapshot.format,png_dir_/name);
            if(!saved && !png_failed_){
                png_failed_=true;std::cerr<<"[frame-probe] cannot write "<<(png_dir_/name).string()<<'\n';
            }
        }
        if(last){runtime_.stop(frame_limit_reason);return;}
        if(frame_start_)frame_start_(frame_);
        if(frame_draw_)frame_draw_();
    }
    psprecomp::Runtime &runtime_;ThreadRuntime &threads_;DisplayRuntime &display_;
    FrameTimer timer_;
    std::function<std::uint64_t()> samples_;
    std::function<bool(const std::filesystem::path &)> frame_saver_;
    std::function<void()> frame_draw_;
    std::function<void(std::uint64_t)> frame_start_;
    std::uint64_t hash_every_,stop_at_,png_every_,frame_{};
    std::filesystem::path png_dir_;
    std::vector<HashExclusion> hash_exclude_;
    bool hash_regions_,reported_{},png_failed_{};
};
}
