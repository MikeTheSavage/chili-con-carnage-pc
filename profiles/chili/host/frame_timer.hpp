#pragma once
#include "frame_costs.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <ostream>
#include <share.h>
#include <stdexcept>
#include <string>
#include <vector>
namespace chili {
// Wall and guest time between consecutive game frame boundaries. A boundary is
// the engine's frame-end vblank wait, so this counts game frames, not vblanks.
//
// The stall log: a frame whose work took longer than stall_us is kept with what happened in it
// (frame_costs.hpp: GE lists, textures decoded, pipeline states and surfaces created, file reads,
// present, sound hand-off). report() prints the totals and the longest ones as [stall] lines.
class FrameTimer {
public:
    using Clock=std::chrono::steady_clock;
    // Frames whose work took longer than this are stalls; long_stall_us is two pictures at 60 per second.
    static constexpr std::uint32_t stall_us=25000u,long_stall_us=33400u;
    explicit FrameTimer(const std::filesystem::path &csv={}){
        intervals_.reserve(1u<<16u);
        stalls_.reserve(256u);
        if(csv.empty())return;
        // Shared for reading, so a script can follow the log while the game runs.
        csv_=_wfsopen(csv.c_str(),L"wb",_SH_DENYWR);
        if(!csv_)throw std::runtime_error("Cannot create frame log: "+csv.string());
        rows_="frame,wall_ms,work_ms,guest_ms,wall_total_ms,guest_total_ms,ctrl_samples,game_dt_ms,ge_ms,submit_ms,file_ms,ge_wait_ms\n";
    }
    FrameTimer(const FrameTimer &)=delete;
    FrameTimer &operator=(const FrameTimer &)=delete;
    ~FrameTimer(){if(active_==this)active_=nullptr;flush();if(csv_)std::fclose(csv_);}
    // Lets a pacing layer report host time it spent waiting, so work time can be
    // told apart from the frame interval. Without a caller, work equals wall.
    void make_active() noexcept {active_=this;}
    static void add_idle(Clock::duration waited) noexcept {if(active_)active_->idle_+=waited;}

    // Returns the 1-based number of the boundary just seen. State hashes and frame images carry that number.
    // A CSV row carries the number of the interval that ended there, which is one less: row N is written at
    // boundary N+1, and its ctrl_samples are the samples taken before that boundary. The controller sample s
    // of a mission is read in the frame that ends at boundary s+47 (frame_lock 2), CSV row s+46.
    std::uint64_t boundary(std::uint64_t guest_us,std::uint64_t controller_samples,float game_dt_seconds){
        const auto now=Clock::now();
        if(boundaries_){
            const auto wall=now-last_wall_;
            const auto work=wall>idle_?wall-idle_:Clock::duration{};
            const Interval interval{micros(wall),micros(work),clamp(guest_us-last_guest_)};
            intervals_.push_back(interval);
            wall_total_+=wall;
            const FrameCosts spent=since(frame_costs,last_costs_);
            if(interval.work_us>stall_us){
                ++stall_count_;stall_work_us_+=interval.work_us;add(stall_costs_,spent);
                if(interval.work_us>long_stall_us)++long_stall_count_;
                if(stalls_.size()<stalls_kept)stalls_.push_back({intervals_.size(),interval.work_us,spent});
            }
            if(csv_){
                char row[256];
                const auto length=std::snprintf(row,sizeof(row),"%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%llu,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                    static_cast<unsigned long long>(intervals_.size()),interval.wall_us/1000.0,interval.work_us/1000.0,
                    interval.guest_us/1000.0,std::chrono::duration<double,std::milli>(wall_total_).count(),
                    (guest_us-first_guest_)/1000.0,static_cast<unsigned long long>(controller_samples),
                    std::isfinite(game_dt_seconds)?game_dt_seconds*1000.0:-1.0,spent.ge_ns/1e6,spent.submit_ns/1e6,spent.file_ns/1e6,spent.ge_wait_ns/1e6);
                if(length>0)rows_.append(row,static_cast<std::size_t>(length));
                if(now-last_flush_>=std::chrono::seconds(1) || rows_.size()>=(1u<<16u)){flush();last_flush_=now;}
            }
        }else{first_guest_=guest_us;last_flush_=now;}
        last_wall_=now;last_guest_=guest_us;idle_={};last_costs_=frame_costs;
        return ++boundaries_;
    }
    std::uint64_t frames() const noexcept {return intervals_.size();}
    void flush(){
        if(!csv_ || rows_.empty())return;
        std::fwrite(rows_.data(),1u,rows_.size(),csv_);std::fflush(csv_);rows_.clear();
    }
    // One line: frame count, wall milliseconds per frame (mean and nearest-rank
    // percentiles), guest frames per guest second and frames per wall second.
    // Then the stall log.
    void report(std::ostream &out) const {
        out<<"[frame-timer] frames="<<intervals_.size();
        if(intervals_.empty()){out<<'\n';return;}
        std::vector<std::uint32_t> wall,work;wall.reserve(intervals_.size());work.reserve(intervals_.size());
        std::uint64_t wall_sum{},work_sum{},guest_sum{};
        for(const auto &i:intervals_){wall.push_back(i.wall_us);work.push_back(i.work_us);wall_sum+=i.wall_us;work_sum+=i.work_us;guest_sum+=i.guest_us;}
        std::sort(wall.begin(),wall.end());std::sort(work.begin(),work.end());
        const auto count=static_cast<double>(intervals_.size());
        const auto rank=[&](const std::vector<std::uint32_t> &sorted,double fraction){
            const auto index=static_cast<std::size_t>(std::ceil(fraction*count));
            return sorted[std::min(sorted.size()-1u,index?index-1u:0u)]/1000.0;
        };
        char text[512];
        std::snprintf(text,sizeof(text)," wall_ms_mean=%.3f wall_ms_p50=%.3f wall_ms_p90=%.3f wall_ms_p99=%.3f wall_ms_max=%.3f"
            " work_ms_mean=%.3f work_ms_p50=%.3f work_ms_p99=%.3f guest_fps=%.3f wall_fps=%.3f",
            wall_sum/count/1000.0,rank(wall,0.50),rank(wall,0.90),rank(wall,0.99),wall.back()/1000.0,
            work_sum/count/1000.0,rank(work,0.50),rank(work,0.99),
            guest_sum?count*1e6/static_cast<double>(guest_sum):0.0,wall_sum?count*1e6/static_cast<double>(wall_sum):0.0);
        out<<text<<'\n';
        report_stalls(out);
    }
private:
    struct Interval{std::uint32_t wall_us,work_us,guest_us;};
    struct Stall{std::uint64_t frame;std::uint32_t work_us;FrameCosts costs;};
    static constexpr std::size_t stalls_kept=4096u,stalls_printed=40u;
    static std::uint32_t clamp(std::uint64_t value) noexcept {return static_cast<std::uint32_t>(std::min<std::uint64_t>(value,0xFFFFFFFFu));}
    static std::uint32_t micros(Clock::duration value) noexcept {
        return clamp(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(value).count()));
    }
    static FrameCosts since(const FrameCosts &now,const FrameCosts &then) noexcept {
        FrameCosts d;
        d.ge_ns=now.ge_ns-then.ge_ns;d.submit_ns=now.submit_ns-then.submit_ns;d.present_ns=now.present_ns-then.present_ns;
        d.gpu_wait_ns=now.gpu_wait_ns-then.gpu_wait_ns;d.pipeline_ns=now.pipeline_ns-then.pipeline_ns;
        d.surface_ns=now.surface_ns-then.surface_ns;d.texture_ns=now.texture_ns-then.texture_ns;
        d.file_ns=now.file_ns-then.file_ns;d.audio_ns=now.audio_ns-then.audio_ns;
        d.ge_wait_ns=now.ge_wait_ns-then.ge_wait_ns;d.ge_on_worker=now.ge_on_worker;
        d.texture_bytes=now.texture_bytes-then.texture_bytes;d.file_bytes=now.file_bytes-then.file_bytes;
        d.lists=now.lists-then.lists;d.draws=now.draws-then.draws;d.submits=now.submits-then.submits;
        d.pipelines=now.pipelines-then.pipelines;d.surfaces=now.surfaces-then.surfaces;d.textures=now.textures-then.textures;
        d.file_reads=now.file_reads-then.file_reads;d.audio_blocks=now.audio_blocks-then.audio_blocks;
        return d;
    }
    static void add(FrameCosts &sum,const FrameCosts &d) noexcept {
        sum.ge_ns+=d.ge_ns;sum.submit_ns+=d.submit_ns;sum.present_ns+=d.present_ns;sum.gpu_wait_ns+=d.gpu_wait_ns;
        sum.pipeline_ns+=d.pipeline_ns;sum.surface_ns+=d.surface_ns;sum.texture_ns+=d.texture_ns;sum.file_ns+=d.file_ns;
        sum.audio_ns+=d.audio_ns;sum.texture_bytes+=d.texture_bytes;sum.file_bytes+=d.file_bytes;
        sum.ge_wait_ns+=d.ge_wait_ns;sum.ge_on_worker=d.ge_on_worker;
        sum.lists+=d.lists;sum.draws+=d.draws;sum.submits+=d.submits;sum.pipelines+=d.pipelines;sum.surfaces+=d.surfaces;
        sum.textures+=d.textures;sum.file_reads+=d.file_reads;sum.audio_blocks+=d.audio_blocks;
    }
    // What the named parts leave of a frame's work: the recompiled game code, mostly. With the lists on
    // their own thread the renderer's time is not in the frame's work; the wait for it is.
    static double rest_ms(std::uint64_t work_us,const FrameCosts &c) noexcept {
        const auto renderer=c.ge_on_worker?c.ge_wait_ns:c.ge_ns+c.submit_ns;
        return std::max(0.0,work_us/1000.0-(renderer+c.file_ns+c.audio_ns)/1e6);
    }
    void report_stalls(std::ostream &out) const {
        char text[768];
        std::snprintf(text,sizeof(text),"[stall] frames with more than %.0f ms of work: %llu of %llu (more than %.1f ms: %llu), %.0f ms of work in them",
            stall_us/1000.0,static_cast<unsigned long long>(stall_count_),static_cast<unsigned long long>(intervals_.size()),
            long_stall_us/1000.0,static_cast<unsigned long long>(long_stall_count_),stall_work_us_/1000.0);
        out<<text<<'\n';
        if(!stall_count_)return;
        const auto &c=stall_costs_;
        const double all=std::max(1.0,stall_work_us_/1000.0);
        const auto share=[&](double ms){return 100.0*ms/all;};
        if(c.ge_on_worker)out<<"[stall] the GE lists ran on their own thread: ge and submit are that thread's time, ge_wait is the game thread's wait for it\n";
        std::snprintf(text,sizeof(text),"[stall] of that work: game code and the rest %.0f%%, waiting for the list thread %.0f%%, GE lists %.0f%% (%u textures decoded, %.1f MB, %.0f%%), "
            "submit %.0f%% (%u pipeline states %.0f%%, %u surfaces %.0f%%, present %.0f%%, waiting for the GPU %.0f%%), "
            "%u file reads %.1f MB %.0f%%, sound hand-off %.0f%%",
            share(rest_ms(stall_work_us_,c)),share(c.ge_wait_ns/1e6),share(c.ge_ns/1e6),c.textures,c.texture_bytes/1048576.0,share(c.texture_ns/1e6),
            share(c.submit_ns/1e6),c.pipelines,share(c.pipeline_ns/1e6),c.surfaces,share(c.surface_ns/1e6),share(c.present_ns/1e6),
            share(c.gpu_wait_ns/1e6),c.file_reads,c.file_bytes/1048576.0,share(c.file_ns/1e6),share(c.audio_ns/1e6));
        out<<text<<'\n';
        // The longest ones, in frame order.
        std::vector<const Stall *> longest;longest.reserve(stalls_.size());
        for(const auto &stall:stalls_)longest.push_back(&stall);
        if(longest.size()>stalls_printed){
            std::partial_sort(longest.begin(),longest.begin()+stalls_printed,longest.end(),[](const Stall *a,const Stall *b){return a->work_us>b->work_us;});
            longest.resize(stalls_printed);
            std::sort(longest.begin(),longest.end(),[](const Stall *a,const Stall *b){return a->frame<b->frame;});
        }
        for(const Stall *stall:longest){
            const auto &s=stall->costs;
            std::snprintf(text,sizeof(text),"[stall] frame=%llu work=%.1f ms: lists=%u draws=%u ge_wait=%.1f ge=%.1f (textures=%u %.2f MB %.1f) submit=%.1f "
                "(pipelines=%u %.1f surfaces=%u %.1f present=%.1f gpu_wait=%.1f) files=%u %.2f MB %.1f sound=%.1f rest=%.1f",
                static_cast<unsigned long long>(stall->frame),stall->work_us/1000.0,s.lists,s.draws,s.ge_wait_ns/1e6,s.ge_ns/1e6,s.textures,s.texture_bytes/1048576.0,
                s.texture_ns/1e6,s.submit_ns/1e6,s.pipelines,s.pipeline_ns/1e6,s.surfaces,s.surface_ns/1e6,s.present_ns/1e6,s.gpu_wait_ns/1e6,
                s.file_reads,s.file_bytes/1048576.0,s.file_ns/1e6,s.audio_ns/1e6,rest_ms(stall->work_us,s));
            out<<text<<'\n';
        }
        if(stall_count_>longest.size())out<<"[stall] "<<stall_count_-longest.size()<<" shorter ones not listed\n";
    }
    inline static FrameTimer *active_{};
    std::vector<Interval> intervals_;
    std::vector<Stall> stalls_;
    FrameCosts last_costs_{},stall_costs_{};
    std::uint64_t stall_count_{},long_stall_count_{},stall_work_us_{};
    std::FILE *csv_{};
    std::string rows_;
    Clock::time_point last_wall_{},last_flush_{};
    Clock::duration idle_{},wall_total_{};
    std::uint64_t boundaries_{},last_guest_{},first_guest_{};
};
}
