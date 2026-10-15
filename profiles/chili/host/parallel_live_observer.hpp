#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace chili {
// Passive, runtime-thread-only counters. No clock, guest access, output,
// allocation, virtual callback or worker mutation occurs here.
enum class ParallelShadeRoute : std::uint32_t {
    FrontendNotReached, EnvironmentIneligible, EmptyPrepared, SinglePrepared,
    TooManyPrepared, BboxBelowMinimum, BboxAboveMaximum,
    ParallelReady, ParallelFallback, ObservationNotCompiled, Count
};
enum ParallelShadeFailure : std::uint32_t {
    JoinedJobFailed = 1, SetupOrExecutorException = 2, EnvironmentMergeFailed = 4
};
struct ParallelShadeEvent {
    ParallelShadeRoute route{ParallelShadeRoute::FrontendNotReached};
    std::size_t prepared_triangles{}, bbox_pixels{}, failed_jobs{};
    std::uint32_t failure_bits{};
    std::uint32_t committed_fragments{};
    bool attempted{}, pool_invoked{}, pool_completed{};
    bool returned{}, rendered{}, escaped_exception{}, committed_count_known{};
};
struct ParallelLiveCounts {
    std::array<std::uint64_t,static_cast<std::size_t>(ParallelShadeRoute::Count)> routes{};
    std::uint64_t raster_calls{}, draw_successes{}, draw_failures{}, escaped_exceptions{};
    std::uint64_t prepared_triangles{}, bbox_pixels{}, attempts{}, attempted_triangles{};
    std::uint64_t pool_invocations{}, pool_completions{}, failed_jobs{};
    std::uint64_t joined_job_failures{}, setup_executor_exceptions{}, merge_failures{};
    std::uint64_t committed_fragments{}, known_commit_counts{}, caller_coincident_draws{};
    bool saturated{};
    constexpr bool operator==(const ParallelLiveCounts&) const = default;
};
class ParallelLiveObserver {
public:
    void record(const ParallelShadeEvent &event) noexcept {
        add(counts_.raster_calls,1);
        const auto route=static_cast<std::size_t>(event.route);
        if(route<counts_.routes.size())add(counts_.routes[route],1);
        if(event.returned)add(event.rendered?counts_.draw_successes:counts_.draw_failures,1);
        if(event.escaped_exception)add(counts_.escaped_exceptions,1);
        add(counts_.prepared_triangles,event.prepared_triangles);
        add(counts_.bbox_pixels,event.bbox_pixels);
        if(event.attempted){add(counts_.attempts,1);add(counts_.attempted_triangles,event.prepared_triangles);}
        if(event.pool_invoked)add(counts_.pool_invocations,1);
        if(event.pool_completed)add(counts_.pool_completions,1);
        add(counts_.failed_jobs,event.failed_jobs);
        if(event.failure_bits&JoinedJobFailed)add(counts_.joined_job_failures,1);
        if(event.failure_bits&SetupOrExecutorException)add(counts_.setup_executor_exceptions,1);
        if(event.failure_bits&EnvironmentMergeFailed)add(counts_.merge_failures,1);
        if(event.committed_count_known){add(counts_.known_commit_counts,1);add(counts_.committed_fragments,event.committed_fragments);}
    }
    // Caller-only optional note. The coincident fast proof does not invoke the
    // raster and must not silently enter the raster participation denominator.
    void note_caller_coincident() noexcept { add(counts_.caller_coincident_draws,1); }
    ParallelLiveCounts snapshot() const noexcept { return counts_; }
private:
    void add(std::uint64_t &value,std::uint64_t amount) noexcept {
        constexpr auto max=std::numeric_limits<std::uint64_t>::max();
        if(amount>max-value){value=max;counts_.saturated=true;}else value+=amount;
    }
    ParallelLiveCounts counts_{};
};

inline constexpr bool kParallelShadeObservationCompiled =
#ifdef CHILI_PARALLEL_SHADE_OBSERVATION
    true;
#else
    false;
#endif

namespace parallel_live_detail {
#ifdef CHILI_PARALLEL_SHADE_OBSERVATION
inline thread_local ParallelShadeEvent *active_event{};
// Scoped only around one synchronous raster call. Saved outer state is restored
// on success and exception; this is not a global observer registration.
class EventBinding {
public:
    explicit EventBinding(ParallelShadeEvent &event) noexcept :previous_(active_event){active_event=&event;}
    ~EventBinding(){active_event=previous_;}
    EventBinding(const EventBinding&)=delete;
    EventBinding &operator=(const EventBinding&)=delete;
private: ParallelShadeEvent *previous_{};
};
// A null observed-wrapper call is an independent unobserved operation even
// inside another observed call. Direct original-renderer calls are unchanged.
class EventSuppression {
public:
    EventSuppression() noexcept :previous_(active_event){active_event=nullptr;}
    ~EventSuppression(){active_event=previous_;}
    EventSuppression(const EventSuppression&)=delete;
    EventSuppression &operator=(const EventSuppression&)=delete;
private: ParallelShadeEvent *previous_{};
};
#endif
inline void reached_stage(ParallelShadeEvent &event,std::size_t triangles,
    bool environment,std::size_t bbox) noexcept {
    event.prepared_triangles=triangles;event.bbox_pixels=bbox;
    if(!environment)event.route=ParallelShadeRoute::EnvironmentIneligible;
    else if(!triangles)event.route=ParallelShadeRoute::EmptyPrepared;
    else if(triangles==1)event.route=ParallelShadeRoute::SinglePrepared;
    else if(triangles>4096)event.route=ParallelShadeRoute::TooManyPrepared;
    else if(bbox<8192)event.route=ParallelShadeRoute::BboxBelowMinimum;
    else if(bbox>524288)event.route=ParallelShadeRoute::BboxAboveMaximum;
    else event.attempted=true;
}
}
}
