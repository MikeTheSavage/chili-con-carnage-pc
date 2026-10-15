#pragma once
#include "display_runtime.hpp"
#include "pc_window.hpp"
#include <chrono>
#include <memory>
#include <functional>
#include <utility>
namespace chili {
// All guest framebuffer reads and window operations stay on the runtime thread.
class Presentation {
    inline static Presentation *active_{};
    psprecomp::Runtime &runtime_;DisplayRuntime &display_;ThreadRuntime &threads_;
    std::unique_ptr<PcWindow> window_;bool visible_,closing_{},gpu_{},paced_{},stale_{};
    std::chrono::steady_clock::time_point last_pump_{},last_frame_{},close_time_{};
    unsigned long long frames_{};
    std::function<void()> service_tick_,host_tick_;
    PerformanceTelemetry *telemetry_{};
    static void dispatched(psprecomp::Runtime &,psprecomp::AllegrexContext &,unsigned,int){active_->tick();}
    // Decodes the displayed guest framebuffer and paints it (GDI).
    void show(bool force){
        const auto frame=display_.snapshot();
        if(!frame.configured || !frame.address)return;
        if(!window_)window_=std::make_unique<PcWindow>(visible_);
        PerformanceTelemetry::PresentScope present_scope(telemetry_,force);
        if(!window_->present(runtime_.memory(),frame.address,frame.stride,frame.format)){
            runtime_.stop("Native framebuffer presentation failed");return;
        }
        present_scope.finish(true);
        last_frame_=std::chrono::steady_clock::now();stale_=false;++frames_;
    }
public:
    Presentation(psprecomp::Runtime &runtime,DisplayRuntime &display,ThreadRuntime &threads,bool visible=true,std::function<void()> service_tick={},PerformanceTelemetry *telemetry=nullptr):runtime_(runtime),display_(display),threads_(threads),visible_(visible),service_tick_(std::move(service_tick)),telemetry_(telemetry){
        if(active_)throw std::runtime_error("Only one Chili presentation service is supported");
        active_=this;psprecomp::set_runtime_post_dispatch_hook(&dispatched);
    }
    ~Presentation(){psprecomp::set_runtime_post_dispatch_hook(nullptr);active_=nullptr;}
    // Host-side chores that only need wall-clock granularity (control-file
    // polling, run deadlines). They run with the window pump, at most every 8 ms,
    // instead of after every guest dispatch.
    void set_host_tick(std::function<void()> host_tick){host_tick_=std::move(host_tick);}
    // Creates the window now rather than at the first presented frame: the
    // hardware renderer needs the handle before it draws anything.
    HWND ensure_window(){if(!window_)window_=std::make_unique<PcWindow>(visible_);return window_->handle();}
    // The hardware renderer puts the picture in the window itself; guest VRAM
    // is then never drawn to and must not be painted over it.
    void set_gpu_presents(bool on){gpu_=on;ensure_window();window_->set_external_paint(on);}
    void count_frame() noexcept {++frames_;}
    void set_fullscreen(bool on){ensure_window();window_->set_fullscreen(on);}
    // Whether the window is in borderless full screen now (the page's Screen row; F11 changes it too).
    bool fullscreen() const noexcept {return window_ && window_->fullscreen();}
    // Paced runs show every completed picture. Unpaced ones (deterministic
    // replays) run far ahead of real time and show at most one per vblank period.
    void set_paced(bool paced) noexcept {paced_=paced;}
    // Software renderer: the picture in the displayed framebuffer is complete
    // (a list ended on it). Shown at once, not at the next timer tick.
    void frame_complete(){
        if(gpu_)return;
        if(!paced_ && std::chrono::steady_clock::now()-last_frame_<std::chrono::microseconds(16683)){stale_=true;return;}
        show(false);
    }
    void tick(bool force=false){
        // Firmware-service deadlines use guest time and must progress even when
        // host window/frame presentation is throttled. This does not advance time.
        if(service_tick_)service_tick_();
        const auto now=std::chrono::steady_clock::now();
        if(!force && now-last_pump_<std::chrono::milliseconds(8))return;
        last_pump_=now;
        if(host_tick_)host_tick_();
        if(window_ && !window_->pump()){
            if(!closing_){closing_=true;close_time_=now;threads_.request_exit_callback();}
            if(now-close_time_>=std::chrono::seconds(2))runtime_.stop("Native window closed");
            return;
        }
        (void)display_.snapshot();
        if(telemetry_)telemetry_->poll(PerformanceTelemetry::Clock::now(),threads_.now_us());
        if(gpu_)return;
        // Pictures are shown when their list completes (frame_complete). The
        // timer only catches up a picture the unpaced gate held back, and
        // anything the CPU wrote into the framebuffer without a list.
        const auto waited=now-last_frame_;
        if(force || (stale_ && waited>=std::chrono::microseconds(16683)) || waited>=std::chrono::milliseconds(100))show(force);
    }
    // Window messages only, for a host wait that must keep the window responsive.
    // A close request is picked up by the next tick().
    void pump_window(){if(window_)window_->pump();}
    unsigned long long presented_frames() const{return frames_;}
    bool save_frame(const std::filesystem::path &path) const{return window_ && window_->save_frame(path);}
    HWND window_handle() const{return window_?window_->handle():nullptr;}
};
}
