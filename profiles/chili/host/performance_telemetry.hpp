#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <ostream>
#include "parallel_live_observer.hpp"

namespace chili {

// Optional observer only. It neither reads guest memory nor changes guest time.
// Construct only for an explicit opt-in and pass nullptr in the default path.
class PerformanceTelemetry {
public:
    using Clock = std::chrono::steady_clock;
    struct Framebuffer { std::uint32_t address{}, stride{}, format{}; };
    enum Counter : std::size_t {
        Requests, ImmediateRequests, DeferredRequests, ReplacedPendingRequests,
        ImmediateApplies, DeferredApplies, AppliedTupleChanges,
        NormalPresents, ForcedPresents, PresentsWithoutNewApply, PresentFailures,
        GeListsStarted, GeListsCompleted, GeWordsRead,
        DrawAttempts, DrawSuccesses, DrawFailures,
        TriangleVertices, OtherVertices, TriangleCommittedFragments,
        DrawWallNs, GeListWallNs, PresentWallNs,
        ClearSpriteAttempts, UntexturedSpriteAttempts, TexturedSpriteAttempts, SpriteFeedbackAttempts,
        ClearSpritePreflightWallNs, UntexturedSpritePreflightWallNs, TexturedSpritePreflightWallNs,
        SpriteFeedbackWallNs, ClearSpriteRasterWallNs, UntexturedSpriteRasterWallNs,
        TexturedSpriteRasterWallNs,
        IdleDeadlineJumps, IdleDeadlineJumpUs, SchedulerTimerTicks, SchedulerTimerUs,
        SystemTimeLowReads, SystemTimeLowReadUs,
        AudioResumeRetries, AudioResumeCompletions, VblankWaits, VblankRequestedDelayUs, Count
    };
    using Counters = std::array<std::uint64_t, Count>;
    struct Sample {
        std::uint64_t wall_us{}, guest_us{}, delta_wall_us{}, delta_guest_us{};
        Counters delta{}, total{};
        std::uint64_t apply_generation{}, max_latch_observation_lateness_guest_us{};
        Framebuffer framebuffer{};
    };

    explicit PerformanceTelemetry(std::ostream &output,
        Clock::time_point started = Clock::now()) noexcept
        : output_(output), started_(started), reported_at_(started) {}

    // Setup/teardown and snapshots are confined to the existing runtime thread.
    // Null keeps the existing perf output verbatim. The observer reads no clock.
    void set_parallel_live_observer(const ParallelLiveObserver *observer) noexcept { parallel_observer_=observer; }

    void display_request(bool deferred, bool replaced_pending) noexcept {
        ++counters_[Requests];
        ++counters_[deferred ? DeferredRequests : ImmediateRequests];
        if (replaced_pending) ++counters_[ReplacedPendingRequests];
    }
    void display_apply(Framebuffer frame, bool deferred,
        std::uint64_t scheduled_guest_us, std::uint64_t observed_guest_us) noexcept {
        ++counters_[deferred ? DeferredApplies : ImmediateApplies];
        if (have_framebuffer_ && (frame.address != framebuffer_.address ||
            frame.stride != framebuffer_.stride || frame.format != framebuffer_.format))
            ++counters_[AppliedTupleChanges];
        framebuffer_ = frame; have_framebuffer_ = true; ++apply_generation_;
        if (deferred && observed_guest_us > scheduled_guest_us) {
            const auto late = observed_guest_us - scheduled_guest_us;
            if (late > max_latch_lateness_) max_latch_lateness_ = late;
        }
    }

    // Integer-only attribution of existing virtual-clock mutations and wait
    // outcomes. Call sites are nullable; these methods read no clocks, guest
    // state or host devices and never schedule, poll, or emit output.
    void idle_deadline_jump(std::uint64_t delta_us) noexcept {
        ++counters_[IdleDeadlineJumps]; counters_[IdleDeadlineJumpUs] += delta_us;
    }
    void scheduler_timer_tick() noexcept {
        ++counters_[SchedulerTimerTicks]; counters_[SchedulerTimerUs] += 100u;
    }
    void system_time_low_read() noexcept {
        ++counters_[SystemTimeLowReads]; ++counters_[SystemTimeLowReadUs];
    }
    void audio_resume(bool completed) noexcept {
        ++counters_[completed ? AudioResumeCompletions : AudioResumeRetries];
    }
    void vblank_wait(std::uint64_t requested_delay_us) noexcept {
        ++counters_[VblankWaits]; counters_[VblankRequestedDelayUs] += requested_delay_us;
    }

    // Returns a staged interval only when due. Raw elapsed units avoid hidden
    // division assumptions and keep presentation/submission rates distinct.
    bool sample(Clock::time_point now, std::uint64_t guest_us, Sample &out,
        bool force = false) noexcept {
        if (now < reported_at_ || now < started_ || guest_us < reported_guest_us_)
            return false;
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - reported_at_).count();
        if (!force && elapsed < 1'000'000) return false;
        Sample staged;
        staged.wall_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(now - started_).count());
        staged.guest_us = guest_us;
        staged.delta_wall_us = static_cast<std::uint64_t>(elapsed);
        staged.delta_guest_us = guest_us - reported_guest_us_;
        staged.total = counters_;
        for (std::size_t i = 0; i < Count; ++i) staged.delta[i] = counters_[i] - reported_[i];
        staged.apply_generation = apply_generation_;
        staged.max_latch_observation_lateness_guest_us = max_latch_lateness_;
        staged.framebuffer = framebuffer_;
        out = staged;
        reported_ = counters_; reported_at_ = now; reported_guest_us_ = guest_us;
        max_latch_lateness_ = 0;
        return true;
    }

    // Call only from the existing outer dispatch/Presentation host pump.
    // HLE event methods never emit output or call another service.
    void poll(Clock::time_point now, std::uint64_t guest_us, bool force = false) noexcept {
        Sample current;
        if (!sample(now, guest_us, current, force)) return;
        const auto saved_flags = output_.flags();
        const auto saved_width = output_.width();
        try {
            output_.width(0);
            output_ << std::dec << "[perf] wall_us=" << current.wall_us
                << " guest_us=" << current.guest_us
                << " d_wall_us=" << current.delta_wall_us
                << " d_guest_us=" << current.delta_guest_us;
            for (std::size_t i = 0; i < Count; ++i)
                output_ << ' ' << names_[i] << '=' << current.delta[i];
            output_ << " apply_generation=" << current.apply_generation
                << " latch_late_guest_us=" << current.max_latch_observation_lateness_guest_us
                << " framebuffer=" << current.framebuffer.address
                << " stride=" << current.framebuffer.stride
                << " format=" << current.framebuffer.format << '\n';
            if(parallel_observer_) {
                const auto totals=parallel_observer_->snapshot();
                output_ << "[parallel-shade] wall_us=" << current.wall_us
                    << " guest_us=" << current.guest_us
                    << " compiled=" << unsigned(kParallelShadeObservationCompiled)
                    << " cumulative=1 saturated=" << unsigned(totals.saturated)
                    << " raster_calls=" << totals.raster_calls
                    << " returned_successes=" << totals.draw_successes
                    << " returned_failures=" << totals.draw_failures
                    << " escaped_exceptions=" << totals.escaped_exceptions
                    << " prepared_triangles=" << totals.prepared_triangles
                    << " bbox_pixels=" << totals.bbox_pixels
                    << " attempts=" << totals.attempts
                    << " attempted_triangles=" << totals.attempted_triangles
                    << " pool_invocations=" << totals.pool_invocations
                    << " pool_completions=" << totals.pool_completions
                    << " failed_jobs=" << totals.failed_jobs
                    << " joined_job_failures=" << totals.joined_job_failures
                    << " setup_executor_exceptions=" << totals.setup_executor_exceptions
                    << " merge_failures=" << totals.merge_failures
                    << " known_commit_counts=" << totals.known_commit_counts
                    << " committed_fragments=" << totals.committed_fragments
                    << " caller_coincident_draws=" << totals.caller_coincident_draws;
                for(std::size_t i=0;i<totals.routes.size();++i)
                    output_ << ' ' << parallel_route_names_[i] << '=' << totals.routes[i];
                output_ << '\n';
            }
            output_.flags(saved_flags);
            output_.width(saved_width);
            if (!output_) output_failed_ = true;
        } catch (...) {
            output_failed_ = true;
            try { output_.flags(saved_flags); } catch (...) {}
            try { output_.width(saved_width); } catch (...) {}
        }
    }
    bool output_failed() const noexcept { return output_failed_; }
    const Counters &counters() const noexcept { return counters_; }

    class GeListScope {
    public:
        explicit GeListScope(PerformanceTelemetry *observer) noexcept
            : observer_(observer) {
            if (observer_) { started_ = Clock::now(); ++observer_->counters_[GeListsStarted]; }
        }
        GeListScope(const GeListScope &) = delete;
        GeListScope &operator=(const GeListScope &) = delete;
        ~GeListScope() {
            if (observer_) observer_->counters_[GeListWallNs] += ns_since(started_);
        }
        void word_read() noexcept { if (observer_) ++observer_->counters_[GeWordsRead]; }
        void completed() noexcept {
            if (observer_ && !completed_) { ++observer_->counters_[GeListsCompleted]; completed_ = true; }
        }
    private:
        PerformanceTelemetry *observer_{}; Clock::time_point started_{}; bool completed_{};
    };

    class DrawScope {
    public:
        explicit DrawScope(PerformanceTelemetry *observer, unsigned vertices, bool triangle) noexcept
            : observer_(observer), vertices_(vertices), triangle_(triangle) {
            if (observer_) { started_ = Clock::now(); ++observer_->counters_[DrawAttempts]; }
        }
        DrawScope(const DrawScope &) = delete;
        DrawScope &operator=(const DrawScope &) = delete;
        ~DrawScope() { finish(false, 0); }
        void finish(bool success, unsigned committed_fragments) noexcept {
            if (!observer_ || finished_) return;
            finished_ = true; observer_->counters_[DrawWallNs] += ns_since(started_);
            ++observer_->counters_[success ? DrawSuccesses : DrawFailures];
            if (success) {
                observer_->counters_[triangle_ ? TriangleVertices : OtherVertices] += vertices_;
                if (triangle_) observer_->counters_[TriangleCommittedFragments] += committed_fragments;
            }
        }
    private:
        PerformanceTelemetry *observer_{}; Clock::time_point started_{};
        unsigned vertices_{}; bool triangle_{}, finished_{};
    };

    enum class SpritePath { Clear, Untextured, Textured };
    // Coarse, disjoint phases inside the selected nontriangle helper. Preflight
    // includes state/storage validation and vertex staging; feedback is only
    // the existing dependency traversal; raster includes sampling/tests/writes
    // and ordinary helper cleanup. Failures charge their reached phase only.
    // A null observer performs no clock reads, counter updates or output.
    class SpriteScope {
    public:
        SpriteScope(PerformanceTelemetry *observer, SpritePath path) noexcept
            : observer_(observer) {
            if (!observer_) return;
            Counter attempts = TexturedSpriteAttempts;
            switch (path) {
            case SpritePath::Clear:
                attempts = ClearSpriteAttempts; phase_ = ClearSpritePreflightWallNs;
                raster_ = ClearSpriteRasterWallNs; break;
            case SpritePath::Untextured:
                attempts = UntexturedSpriteAttempts; phase_ = UntexturedSpritePreflightWallNs;
                raster_ = UntexturedSpriteRasterWallNs; break;
            case SpritePath::Textured:
                phase_ = TexturedSpritePreflightWallNs;
                raster_ = TexturedSpriteRasterWallNs; break;
            }
            started_ = Clock::now(); ++observer_->counters_[attempts];
        }
        SpriteScope(const SpriteScope &) = delete;
        SpriteScope &operator=(const SpriteScope &) = delete;
        ~SpriteScope() { if (observer_) account(Clock::now()); }
        void feedback() noexcept {
            if (!observer_) return;
            ++observer_->counters_[SpriteFeedbackAttempts];
            transition(SpriteFeedbackWallNs);
        }
        void raster() noexcept { if (observer_) transition(raster_); }
    private:
        void account(Clock::time_point now) noexcept {
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - started_).count();
            if (elapsed > 0) observer_->counters_[phase_] += static_cast<std::uint64_t>(elapsed);
        }
        void transition(Counter phase) noexcept {
            const auto now = Clock::now(); account(now); started_ = now; phase_ = phase;
        }
        PerformanceTelemetry *observer_{};
        Clock::time_point started_{};
        Counter phase_{TexturedSpritePreflightWallNs}, raster_{TexturedSpriteRasterWallNs};
    };

    class PresentScope {
    public:
        explicit PresentScope(PerformanceTelemetry *observer, bool forced) noexcept
            : observer_(observer), forced_(forced) { if (observer_) started_ = Clock::now(); }
        PresentScope(const PresentScope &) = delete;
        PresentScope &operator=(const PresentScope &) = delete;
        ~PresentScope() { finish(false); }
        void finish(bool success) noexcept {
            if (!observer_ || finished_) return;
            finished_ = true; observer_->counters_[PresentWallNs] += ns_since(started_);
            if (!success) { ++observer_->counters_[PresentFailures]; return; }
            ++observer_->counters_[forced_ ? ForcedPresents : NormalPresents];
            if (observer_->have_present_ && observer_->present_apply_generation_ == observer_->apply_generation_)
                ++observer_->counters_[PresentsWithoutNewApply];
            observer_->present_apply_generation_ = observer_->apply_generation_;
            observer_->have_present_ = true;
        }
    private:
        PerformanceTelemetry *observer_{}; Clock::time_point started_{};
        bool forced_{}, finished_{};
    };

private:
    static std::uint64_t ns_since(Clock::time_point start) noexcept {
        const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
        return value > 0 ? static_cast<std::uint64_t>(value) : 0;
    }
    inline static constexpr std::array<const char *, Count> names_{
        "requests", "immediate_requests", "deferred_requests", "pending_replaced",
        "immediate_applies", "deferred_applies", "applied_tuple_changes",
        "normal_presents", "forced_presents", "presents_without_new_apply", "present_failures",
        "ge_lists_started", "ge_lists_completed", "ge_words_read",
        "draw_attempts", "draw_successes", "draw_failures",
        "triangle_vertices", "other_vertices", "triangle_committed_fragments",
        "draw_wall_ns", "ge_list_wall_ns", "present_wall_ns",
        "sprite_clear_attempts", "sprite_untextured_attempts", "sprite_textured_attempts", "sprite_feedback_attempts",
        "sprite_clear_preflight_ns", "sprite_untextured_preflight_ns", "sprite_textured_preflight_ns",
        "sprite_feedback_ns", "sprite_clear_raster_ns", "sprite_untextured_raster_ns",
        "sprite_textured_raster_ns",
        "idle_jump_count", "idle_jump_us", "timer_count", "timer_us",
        "read_count", "read_us", "audio_resume_retry_count", "audio_resume_completion_count",
        "vblank_wait_count", "vblank_requested_delay_us"
    };
    inline static constexpr std::array<const char *,static_cast<std::size_t>(ParallelShadeRoute::Count)> parallel_route_names_{
        "frontend_not_reached", "environment_ineligible", "empty_prepared", "single_prepared",
        "too_many_prepared", "bbox_below_minimum", "bbox_above_maximum",
        "parallel_ready", "parallel_fallback", "observation_not_compiled"
    };
    const ParallelLiveObserver *parallel_observer_{};
    std::ostream &output_;
    Clock::time_point started_{}, reported_at_{};
    Counters counters_{}, reported_{};
    std::uint64_t reported_guest_us_{}, apply_generation_{}, present_apply_generation_{}, max_latch_lateness_{};
    Framebuffer framebuffer_{};
    bool have_framebuffer_{}, have_present_{}, output_failed_{};
};
}
